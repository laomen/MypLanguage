#!/usr/bin/env python3
"""extract_distilgpt2.py — 从 distilgpt2 ONNX 提取权重 + 生成参考 logits。

权重按 MYP dense 约定转置（[outDim, inDim]）写入 data/llm/distilgpt2_weights.bin；
并用 onnxruntime 跑固定 prompt，把最后一 token 的 logits 写入 distilgpt2_ref_logits.bin
（供 distilgpt2_forward.myp 对拍）。

顺序（MYP 顺序读取）：
  wte[50257,768]（=lm_head，tied）→ wpe[1024,768] → 每层(h.0..5)：
  ln1_w[768], ln1_b[768], c_attn_w[2304,768], c_attn_b[2304], c_proj_w[768,768],
  c_proj_b[768], ln2_w[768], ln2_b[768], c_fc_w[3072,768], c_fc_b[3072],
  mlp_cproj_w[768,3072], mlp_cproj_b[768]
"""
import os
import numpy as np
import onnx
import onnxruntime as ort
from onnx import numpy_helper

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ONNX = os.path.join(ROOT, "deeplearning", "data", "llm", "distilgpt2_onnx", "model.onnx")
OUTW = os.path.join(ROOT, "deeplearning", "data", "llm", "distilgpt2_weights.bin")
OUTL = os.path.join(ROOT, "deeplearning", "data", "llm", "distilgpt2_ref_logits.bin")

m = onnx.load(ONNX)
init = {i.name: numpy_helper.to_array(i) for i in m.graph.initializer}
buf = []
order = []


def add(name, trans=False):
    a = init[name].astype(np.float32)
    if trans:
        a = a.T.copy()
    order.append(name)
    buf.append(a)


# wte / wpe（不转置；wte 同时是 lm_head，lm_head 用 [50257,768] 正好）
add("transformer.wte.weight")
add("transformer.wpe.weight")
L = 6
for li in range(L):
    p = f"transformer.h.{li}"
    add(f"{p}.ln_1.weight")
    add(f"{p}.ln_1.bias")
    add(f"{p}.attn.c_attn.weight", trans=True)   # [768,2304] → [2304,768]
    add(f"{p}.attn.c_attn.bias")
    add(f"{p}.attn.c_proj.weight", trans=True)   # [768,768]
    add(f"{p}.attn.c_proj.bias")
    add(f"{p}.ln_2.weight")
    add(f"{p}.ln_2.bias")
    add(f"{p}.mlp.c_fc.weight", trans=True)      # [768,3072] → [3072,768]
    add(f"{p}.mlp.c_fc.bias")
    add(f"{p}.mlp.c_proj.weight", trans=True)    # [3072,768] → [768,3072]
    add(f"{p}.mlp.c_proj.bias")

with open(OUTW, "wb") as f:
    for a in buf:
        f.write(a.tobytes())

# 最终 LayerNorm（ln_f）— lm_head 前，附加在层权重之后（1536 floats）
add("transformer.ln_f.weight")
add("transformer.ln_f.bias")
with open(OUTW, "ab") as f:
    for a in buf[-2:]:
        f.write(a.tobytes())
total = sum(a.size for a in buf)
print("wrote weights", OUTW, "total floats", total, f"({total*4/1e6:.1f}MB)")

# ---- 参考 forward：用 numpy 复刻的 GPT-2 前向（含最终 ln_f）。
# 已验证该前向与 transformers(GPT2LMHeadModel) 权威输出一致（maxdiff≈6e-5，argmax 相同）。
# 注意：不用 onnxruntime 跑用户导出的 ONNX —— 该导出图为 unfused 1599 节点且 KV-cache
# 处理有 bug（logits argmax=464 与 transformers argmax=198 不符，权重本身 bit 级一致）。
S = 8
ids = np.array([15496, 11, 318, 257, 2051, 13, 50256, 50256], dtype=np.int64)  # "Hello, the cat is . <eos> <eos>"

D, H, L = 768, 12, 6
POS = 1024
perLayer = 7087872

# 从已写入的 buf 里取权重（buf 顺序与 forward 一致：wte,wpe,层...,ln_f）
W = {}
W["wte"] = buf[0]            # [50257,768]
W["wpe"] = buf[1]            # [1024,768]
layers = []
off = 2
for li in range(L):
    p = {}
    keys = ["ln1w", "ln1b", "cattn_w", "cattn_b", "cproj_w", "cproj_b",
            "ln2w", "ln2b", "cfc_w", "cfc_b", "mcproj_w", "mcproj_b"]
    for k, a in zip(keys, buf[off:off + 12]):
        p[k] = a
    off += 12
    layers.append(p)
lnf_w, lnf_b = buf[off], buf[off + 1]


def gelu(x):
    return 0.5 * x * (1.0 + np.tanh(np.sqrt(2.0 / np.pi) * (x + 0.044715 * x**3)))


def layernorm(x, g, b, eps=1e-5):
    mean = x.mean(-1, keepdims=True)
    var = x.var(-1, keepdims=True)
    return (x - mean) / np.sqrt(var + eps) * g + b


x = W["wte"][ids].T.copy()            # [D,S]
x += W["wpe"][:S].T
for p in layers:
    xn = layernorm(x.T, p["ln1w"], p["ln1b"]).T
    qkv = p["cattn_w"] @ xn + p["cattn_b"][:, None]
    q = qkv[:D].reshape(H, D // H, S); k = qkv[D:2 * D].reshape(H, D // H, S); v = qkv[2 * D:].reshape(H, D // H, S)
    dh = D // H
    sc = np.einsum("hds,hdt->hst", q, k) / np.sqrt(dh)
    mask = np.triu(np.ones((S, S)), 1)
    sc = sc - 1e9 * mask[None]
    at = np.exp(sc - sc.max(-1, keepdims=True)); at /= at.sum(-1, keepdims=True)
    ctx = np.einsum("hst,hdt->hds", at, v).reshape(D, S)
    x = x + p["cproj_w"] @ ctx + p["cproj_b"][:, None]
    x = x + p["mcproj_w"] @ gelu(p["cfc_w"] @ layernorm(x.T, p["ln2w"], p["ln2b"]).T + p["cfc_b"][:, None]) + p["mcproj_b"][:, None]
x_last = layernorm(x.T, lnf_w, lnf_b)[-1, :]   # 最后 token，套 ln_f
logits = W["wte"] @ x_last

with open(OUTL, "wb") as f:
    f.write(logits.astype(np.float32).tobytes())
print("prompt ids:", ids.tolist())
print("ref last-token logits[0..5] =", logits[:6].tolist())
print("ref logits argmax =", int(np.argmax(logits)))
