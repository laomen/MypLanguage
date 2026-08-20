#!/usr/bin/env python3
"""ground_truth_distilgpt2.py — 用 transformers 加载原始 distilgpt2 得到权威 logits，
并校验 ONNX 提取权重与原始权重是否一致（判断导出是否引入权重损坏）。

用法（onnxvenv 内，需 torch+transformers）：
  onnxvenv/bin/python ground_truth_distilgpt2.py
"""
import os
import numpy as np
import torch
from transformers import GPT2LMHeadModel, GPT2Tokenizer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MODEL_DIR = os.path.join(ROOT, "deeplearning", "data", "llm", "distilgpt2")
ONNXW = os.path.join(ROOT, "deeplearning", "data", "llm", "distilgpt2_weights.bin")
ONNX = os.path.join(ROOT, "deeplearning", "data", "llm", "distilgpt2_onnx", "model.onnx")

PROMPT = "Hello, is a miss ."
IDS = [15496, 11, 318, 257, 2051, 13]

print("=== loading model from", MODEL_DIR)
m = GPT2LMHeadModel.from_pretrained(MODEL_DIR)
m.eval()
tok = GPT2Tokenizer.from_pretrained(MODEL_DIR)

ids = torch.tensor([IDS])
with torch.no_grad():
    out = m(ids)
logits = out.logits[0, -1].float().numpy()

top = np.argsort(logits)[::-1][:10]
print("\n=== transformers logits (last token) ===")
print("argmax =", int(top[0]), "->", repr(tok.decode([int(top[0])])))
for t in top:
    print(f"  {int(t):6d} {repr(tok.decode([int(t)])):12s} {logits[t]:10.4f}")
np.save("/tmp/gt_logits.npy", logits)

# ---- 校验 ONNX 提取权重 vs 原始权重（抽查关键层，含转置方向）----
print("\n=== weight cross-check (ONNX-extracted vs pytorch) ===")
sd = m.state_dict()
checks = [
    ("transformer.wte.weight", None),
    ("transformer.wpe.weight", None),
    ("transformer.h.0.ln_1.weight", None),
    ("transformer.h.0.ln_1.bias", None),
    ("transformer.h.0.attn.c_attn.weight", "T"),   # [768,2304] -> [2304,768]
    ("transformer.h.0.attn.c_attn.bias", None),
    ("transformer.h.0.attn.c_proj.weight", "T"),
    ("transformer.h.0.mlp.c_fc.weight", "T"),
    ("transformer.h.0.mlp.c_proj.weight", "T"),
    ("transformer.h.5.attn.c_attn.weight", "T"),
    ("transformer.ln_f.weight", None),
]
offs = {
    "transformer.wte.weight": 0,
    "transformer.wpe.weight": 50257 * 768,
}
L, D = 6, 768
POS, perLayer = 1024, 7087872
base0 = 50257 * 768 + POS * D
ly = {
    "ln_1.weight": 0, "ln_1.bias": 768, "attn.c_attn.weight": 1536,
    "attn.c_attn.bias": 1771008, "attn.c_proj.weight": 1773312,
    "attn.c_proj.bias": 2363136, "ln_2.weight": 2363904, "ln_2.bias": 2364672,
    "mlp.c_fc.weight": 2365440, "mlp.c_fc.bias": 4724736,
    "mlp.c_proj.weight": 4727808, "mlp.c_proj.bias": 7087104,
}
with open(ONNXW, "rb") as f:
    raw = f.read()

def read_off(off, shape):
    n = int(np.prod(shape))
    a = np.frombuffer(raw, dtype=np.float32, count=n, offset=off * 4).reshape(shape)
    return a.copy()

worst = 0.0
for name, tr in checks:
    if name.startswith("transformer.h."):  # has layer part
        parts = name[len("transformer.h."):]
        li, rest = parts.split(".", 1)
        li = int(li)
        off = base0 + li * perLayer + ly[rest]
        shape = {"wte.weight": (50257, 768), "wpe.weight": (1024, 768)}.get(name, None)
        if shape is None:
            if rest.startswith("attn.c_attn.weight"):
                shape = (2304, 768)
            elif rest.startswith("attn.c_proj.weight"):
                shape = (768, 768)
            elif rest.startswith("mlp.c_fc.weight"):
                shape = (3072, 768)
            elif rest.startswith("mlp.c_proj.weight"):
                shape = (768, 3072)
            else:
                shape = (768,)
    else:
        off = offs[name]
        shape = (50257, 768) if name == "transformer.wte.weight" else (1024, 768)
    onnx_a = read_off(off, shape)
    ref = sd[name].float().numpy()
    if tr == "T":
        ref = ref.T
    d = np.max(np.abs(onnx_a - ref))
    worst = max(worst, d)
    print(f"  {name:48s} maxdiff={d:.3e}  shape={shape}")
print(f"\nWORST weight diff = {worst:.3e}")
if worst < 1e-4:
    print("WEIGHTS MATCH: ONNX-extracted weights == original distilgpt2 weights.")
else:
    print("WEIGHTS MISMATCH: export corrupted weights!")
