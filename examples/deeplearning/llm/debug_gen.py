#!/usr/bin/env python3
"""debug_gen.py — 复刻 MYP 的 KV-cache 增量 decode（逐 token），每步与 transformers 贪心参考对拍。
定位 MYP 生成从哪一步、为什么开始分歧。
"""
import os
import numpy as np
import torch
from transformers import GPT2LMHeadModel

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
D = os.path.join(ROOT, "deeplearning", "data", "llm")
ONNXW = os.path.join(D, "distilgpt2_weights.bin")

V, Dm, L, H, POS = 50257, 768, 6, 12, 1024
perLayer = 7087872
W = 128
groups = 1
kvD = Dm

with open(ONNXW, "rb") as f:
    raw = f.read()

def rd(off, shape):
    n = int(np.prod(shape))
    return np.frombuffer(raw, dtype=np.float32, count=n, offset=off * 4).reshape(shape).copy()

base0 = V * Dm + POS * Dm
weightsEnd = base0 + L * perLayer + 1536
Wt = {"wte": rd(0, (V, Dm)), "wpe": rd(V * Dm, (POS, Dm)),
      "lnf_w": rd(weightsEnd - 1536, (Dm,)), "lnf_b": rd(weightsEnd - 768, (Dm,))}
ly = {"ln1w": 0, "ln1b": 768, "cattn_w": 1536, "cattn_b": 1771008, "cproj_w": 1773312,
      "cproj_b": 2363136, "ln2w": 2363904, "ln2b": 2364672, "cfc_w": 2365440, "cfc_b": 4724736,
      "mcproj_w": 4727808, "mcproj_b": 7087104}
layers = []
for li in range(L):
    b = base0 + li * perLayer
    p = {}
    for k, o in ly.items():
        shp = {"ln1w": (Dm,), "ln1b": (Dm,), "cattn_w": (3 * Dm, Dm), "cattn_b": (3 * Dm,),
               "cproj_w": (Dm, Dm), "cproj_b": (Dm,), "ln2w": (Dm,), "ln2b": (Dm,),
               "cfc_w": (4 * Dm, Dm), "cfc_b": (4 * Dm,), "mcproj_w": (Dm, 4 * Dm), "mcproj_b": (Dm,)}[k]
        p[k] = rd(b + o, shp)
    layers.append(p)

def gelu(x):
    return 0.5 * x * (1.0 + np.tanh(np.sqrt(2.0 / np.pi) * (x + 0.044715 * x**3)))

def layernorm(x, g, b, eps=1e-5):
    mean = x.mean(-1, keepdims=True)
    var = x.var(-1, keepdims=True)
    return (x - mean) / np.sqrt(var + eps) * g + b

def attention_cached(q, kc, vc, length):
    # q:[D], kc/vc:[kvD, W], length = 有效列数
    dh = Dm // H
    out = np.zeros(Dm)
    for b in range(H):
        sc = np.zeros(length)
        for i in range(length):
            sc[i] = np.dot(q[b*dh:(b+1)*dh], kc[b*dh:(b+1)*dh, i]) / np.sqrt(dh)
        e = np.exp(sc - sc.max())
        p = e / e.sum()
        for d in range(dh):
            out[b*dh+d] = np.dot(p, vc[b*dh+d, :length])
    return out

prompt = [7454, 2402, 257, 640]
GENERATE = 8

# ---- MYP 算法复刻（增量）----
kc = [np.zeros((kvD, W), np.float32) for _ in range(L)]
vc = [np.zeros((kvD, W), np.float32) for _ in range(L)]
cacheLen = 0
hist = []
myp_logits_by_step = []
for step in range(len(prompt) + GENERATE):
    tok = prompt[step] if step < len(prompt) else int(np.argmax(logits))
    hist.append(tok)
    if cacheLen >= W:
        kc = [np.concatenate([k[:, 1:], np.zeros((kvD, 1), np.float32)], 1) for k in kc]
        vc = [np.concatenate([v[:, 1:], np.zeros((kvD, 1), np.float32)], 1) for v in vc]
        cacheLen = W - 1
    x = Wt["wte"][tok].copy() + Wt["wpe"][cacheLen]
    for li in range(L):
        p = layers[li]
        xn = layernorm(x, p["ln1w"], p["ln1b"])
        qkv = p["cattn_w"] @ xn + p["cattn_b"]
        q, k, v = qkv[:Dm], qkv[Dm:2*Dm], qkv[2*Dm:]
        kc[li][:, cacheLen] = k
        vc[li][:, cacheLen] = v
        att = attention_cached(q, kc[li], vc[li], cacheLen + 1)
        x = x + p["cproj_w"] @ att + p["cproj_b"]
        xn = layernorm(x, p["ln2w"], p["ln2b"])
        x = x + p["mcproj_w"] @ gelu(p["cfc_w"] @ xn + p["cfc_b"]) + p["mcproj_b"]
    xl = layernorm(x, Wt["lnf_w"], Wt["lnf_b"])
    logits = Wt["wte"] @ xl
    myp_logits_by_step.append(logits)
    cacheLen += 1

print("MYP-replica generated:", hist[len(prompt):])

# ---- transformers 参考（逐 token）----
m = GPT2LMHeadModel.from_pretrained(os.path.join(D, "distilgpt2"))
m.eval()
g = list(prompt)
past = None
tf_logits_by_step = []
with torch.no_grad():
    for step in range(len(prompt) + GENERATE):
        inp = torch.tensor([g[-1:] if past is not None else g])
        out = m(inp, past_key_values=past, use_cache=True)
        past = out.past_key_values
        lg = out.logits[0, -1].float().numpy()
        tf_logits_by_step.append(lg)
        if step >= len(prompt):
            nxt = int(lg.argmax())
            g.append(nxt)
print("transformers generated:", g[len(prompt):])

# ---- 逐 logits 对拍 ----
print("\nstep | MYP argmax | TF argmax | maxdiff")
for i in range(len(myp_logits_by_step)):
    a = myp_logits_by_step[i]
    b = tf_logits_by_step[i]
    print(f"  {i:3d} | {int(a.argmax()):8d} | {int(b.argmax()):8d} | {np.abs(a-b).max():.4e}")

print("\nMYP-replica per-step logits[0..5] (for compare with MYP binary):")
for i, a in enumerate(myp_logits_by_step):
    print(f"  step {i:2d} tok={hist[i] if i < len(hist) else '?'} argmax={int(a.argmax())}  lg[0..5]={[round(float(x),4) for x in a[:6]]}")

