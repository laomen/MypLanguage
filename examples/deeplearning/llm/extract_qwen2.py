#!/usr/bin/env python3
"""extract_qwen2.py — 提取 Qwen2-0.5B-Instruct 权重 → MYP .bin + numpy 前向验证 + transformers 参考。

架构（Qwen2ForCausalLM）：
  wte[151936,896]（lm_head tied）→ 24 层：
    input_layernorm(RMSNorm,896) → q_proj[896,896]/k_proj[128,896]/v_proj[128,896]
    (RoPE 逐头, dh=64) → GQA attention(14Q/2KV, groups=7) → o_proj[896,896] → 残差
    → post_attention_layernorm(RMSNorm,896) → SwiGLU MLP: gate[4864,896]/up[4864,896]
      /down[896,4864]（silu(gate·x)·(up·x)）→ 残差
  → final_norm(RMSNorm,896) → lm_head(=wte tied)。
  **q/k/v 投影带 bias**（q:896, k:128, v:128；o/norm/mlp 无 bias）。
  rope_theta=1e6；rms eps=1e-6；head_dim=64。

权重布局（MYP 顺序，fp32 LE）：
  wte → 每层(ln1,q,qb,k,kb,v,vb,o,ln2,gate,up,down) → final_norm
  其中 qb/kb/vb 为 q/k/v 的 bias。
输出：
  data/llm/qwen2_weights.bin（~2GB fp32）
  data/llm/qwen2_ref_logits.bin（transformers 参考 logits，chat 模板 prompt）
  data/llm/qwen2_ref_gen_ids.bin（transformers 贪心生成参考）
用法（onnxvenv 内）：
  onnxvenv/bin/python extract_qwen2.py
"""
import os
import numpy as np
import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
D = os.path.join(ROOT, "deeplearning", "data", "llm")
MODEL_DIR = os.path.join(D, "qwen2-0.5b-instruct")

print("loading model (bf16)...")
tok = AutoTokenizer.from_pretrained(MODEL_DIR)
m = AutoModelForCausalLM.from_pretrained(MODEL_DIR, torch_dtype=torch.bfloat16)
m.eval()
sd = m.state_dict()

# 架构参数直接从 config.json 读（避免 transformers 版本字段改名）
import json
with open(os.path.join(MODEL_DIR, "config.json")) as f:
    cfg = json.load(f)
H = cfg["hidden_size"]            # 896
L = cfg["num_hidden_layers"]      # 24
QH = cfg["num_attention_heads"]   # 14
KVH = cfg["num_key_value_heads"]  # 2
HD = H // QH                      # 64
FFN = cfg["intermediate_size"]    # 4864
V = cfg["vocab_size"]             # 151936
POS = cfg["max_position_embeddings"]
THETA = cfg["rope_theta"]         # 1e6
EPS = cfg["rms_norm_eps"]         # 1e-6
print(f"H={H} L={L} QH={QH} KVH={KVH} HD={HD} FFN={FFN} V={V} theta={THETA} eps={EPS}")

# ---------------- 提取权重 ----------------
# 布局: ln1,q,qb,k,kb,v,vb,o,ln2,gate,up,down （qb/kb/vb = q/k/v bias）
per = (H + H * H + H + (KVH * HD * H) + (KVH * HD) + (KVH * HD * H) + (KVH * HD)
       + H * H + H + 3 * (FFN * H))
print("per-layer floats =", per)

def f32(name):
    return sd[name].float().numpy()  # bf16 → fp32

buf = []
def add(name):
    a = f32(name)
    buf.append(a)
    return a

wte = add("model.embed_tokens.weight")          # [V, H]
ly_off = []
for li in range(L):
    p = f"model.layers.{li}"
    ln1 = add(f"{p}.input_layernorm.weight")            # [H]
    q = add(f"{p}.self_attn.q_proj.weight")             # [H, H]
    qb = add(f"{p}.self_attn.q_proj.bias")              # [H]
    k = add(f"{p}.self_attn.k_proj.weight")             # [KVH*HD, H]
    kb = add(f"{p}.self_attn.k_proj.bias")              # [KVH*HD]
    v = add(f"{p}.self_attn.v_proj.weight")             # [KVH*HD, H]
    vb = add(f"{p}.self_attn.v_proj.bias")              # [KVH*HD]
    o = add(f"{p}.self_attn.o_proj.weight")             # [H, H]
    ln2 = add(f"{p}.post_attention_layernorm.weight")   # [H]
    gate = add(f"{p}.mlp.gate_proj.weight")             # [FFN, H]
    up = add(f"{p}.mlp.up_proj.weight")                 # [FFN, H]
    down = add(f"{p}.mlp.down_proj.weight")             # [H, FFN]
    off = sum(a.size for a in buf) - per
    ly_off.append(off)
    print(f"  layer {li} off={off}")
final_norm = add("model.norm.weight")           # [H]

with open(os.path.join(D, "qwen2_weights.bin"), "wb") as f:
    for a in buf:
        f.write(a.astype(np.float32).tobytes())
total = sum(a.size for a in buf)
print(f"wrote qwen2_weights.bin: {total} floats ({total*4/1e9:.2f} GB)")

# ---------------- 参考：transformers（chat 模板）----------------
CHAT = [{"role": "user", "content": "What is the capital of France?"}]
ids = tok.apply_chat_template(CHAT, tokenize=True, add_generation_prompt=True, return_tensors="pt")
if hasattr(ids, "input_ids") and not hasattr(ids, "shape"):
    ids = ids.input_ids   # transformers 5.x 返回 BatchEncoding(dict)
ids_np = np.asarray(ids)
if ids_np.ndim == 1:
    ids_np = ids_np[None, :]
ids_list = ids_np.reshape(-1).tolist()
print("prompt ids:", ids_list, "n=", ids_np.shape[1])

with torch.no_grad():
    out = m(torch.tensor(ids_np))
    lg = out.logits[0, -1].float().numpy()
np.save(os.path.join(D, "qwen2_ref_logits.npy"), lg)
top = np.argsort(lg)[::-1][:8]
print("ref logits argmax =", int(top[0]), tok.decode([int(top[0])]))
print("top8:", [(int(t), tok.decode([int(t)])) for t in top])

# 存 chat 模板 prompt ids（供 MYP 端对拍；须在贪心生成前写，避免列表被污染）
with open(os.path.join(D, "qwen2_prompt_ids.bin"), "wb") as f:
    f.write(np.array([ids_np.shape[1]] + list(ids_list), np.int32).tobytes())

# 贪心生成参考（KV cache）
g = list(ids_list)
past = None
with torch.no_grad():
    for _ in range(24):
        inp = torch.tensor([g[-1:] if past is not None else g])
        o = m(inp, past_key_values=past, use_cache=True)
        past = o.past_key_values
        nxt = int(o.logits[0, -1].argmax())
        if nxt == 151645:  # <|im_end|>
            g.append(nxt)
            break
        g.append(nxt)
with open(os.path.join(D, "qwen2_ref_gen_ids.bin"), "wb") as f:
    f.write(np.array([len(g)] + g, np.int32).tobytes())
print("greedy ids:", g)
print("greedy text:", tok.decode(g, skip_special_tokens=False))

print("wrote qwen2_prompt_ids.bin / ref_logits.npy / ref_gen_ids.bin")
