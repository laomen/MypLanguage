#!/usr/bin/env python3
"""extract_sd15.py — 阶段 D2：提取 SD1.5 CLIP 文本编码器权重 → MYP .bin + transformers 参考。

CLIP 文本编码器（= openai/clip-vit-large-patch14）：
  wte[49408,768] → wpe[77,768] → 12 层：
    layer_norm1 → self_attn(q/k/v/out，各 [768,768]+bias，因果掩码) → 残差
    → layer_norm2 → mlp.fc1[3072,768](gelu) → mlp.fc2[768,3072] → 残差
  → final_layer_norm → last_hidden_state[77,768]

权重布局（GPT-2 兼容，MYP dense [outDim,xRows] 行主序，无需转置）：
  wte → wpe → 每层(ln1w,ln1b, qkv_w[2304,768],qkv_b[2304]（q/k/v 拼接）,
       out_w,out_b, ln2w,ln2b, fc1_w,fc1_b, fc2_w,fc2_b) → final_lnw,final_lnb
  其中 qkv 拼接顺序 = 行主序写 q 全行、k 全行、v 全行（MYP dense 直接读）。
输出：
  data/diffusion/clip/weights.bin        文本编码器权重（fp32 LE）
  data/diffusion/clip/config.txt         H/L/FFN/MAX 等参数
  data/diffusion/clip/prompt_ids.bin    测试 prompt 的 77 个 token id（i32 LE）
  data/diffusion/clip/ref_emb.bin       transformers last_hidden_state[77,768]（fp32 LE）
用法（onnxvenv 内）：
  infer/tools/onnxvenv/bin/python deeplearning/diffusion/extract_sd15.py
"""
import os
import numpy as np
import torch
from transformers import CLIPTextModel, CLIPTokenizer

DL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # examples/deeplearning
OUT = os.path.join(DL, "data", "diffusion", "clip")
MODEL_DIR = os.path.join(DL, "data", "diffusion", "clip-vit-large-patch14")
os.makedirs(OUT, exist_ok=True)

D = 768
L = 12
FFN = 3072
MAX = 77
V = 49408
H = 12          # attention heads
DH = D // H     # 64

print("loading CLIP text encoder (fp32)...")
tok = CLIPTokenizer.from_pretrained(MODEL_DIR)
m = CLIPTextModel.from_pretrained(MODEL_DIR, torch_dtype=torch.float32)
m.eval()
sd = m.state_dict()

# ---------------- 权重提取 ----------------
def W(k):  # [out,in] fp32
    return sd[k].detach().cpu().numpy().astype(np.float32)

# 每层布局（q/k/v 拼接 [2304,768]）
per_ln = 2 * D
per_qkv_w = (3 * D) * D
per_qkv_b = 3 * D
per_out_w = D * D
per_out_b = D
per_fc1_w = FFN * D
per_fc1_b = FFN
per_fc2_w = D * FFN
per_fc2_b = D
per = (per_ln * 2 + per_qkv_w + per_qkv_b + per_out_w + per_out_b
       + per_fc1_w + per_fc1_b + per_fc2_w + per_fc2_b)

total = V * D + MAX * D + L * per + 2 * D
print(f"weights floats = {total} ({total*4/1e6:.1f} MB)")
with open(os.path.join(OUT, "weights.bin"), "wb") as f:
    # wte（CLIPTextModel 的 state_dict 无 text_model. 前缀）
    f.write(W("embeddings.token_embedding.weight").tobytes())
    # wpe
    f.write(W("embeddings.position_embedding.weight").tobytes())
    for i in range(L):
        p = f"encoder.layers.{i}"
        ln1w = W(f"{p}.layer_norm1.weight")
        ln1b = W(f"{p}.layer_norm1.bias")
        q = W(f"{p}.self_attn.q_proj.weight")
        k = W(f"{p}.self_attn.k_proj.weight")
        v = W(f"{p}.self_attn.v_proj.weight")
        qb = W(f"{p}.self_attn.q_proj.bias")
        kb = W(f"{p}.self_attn.k_proj.bias")
        vb = W(f"{p}.self_attn.v_proj.bias")
        ow = W(f"{p}.self_attn.out_proj.weight")
        ob = W(f"{p}.self_attn.out_proj.bias")
        ln2w = W(f"{p}.layer_norm2.weight")
        ln2b = W(f"{p}.layer_norm2.bias")
        f1w = W(f"{p}.mlp.fc1.weight")
        f1b = W(f"{p}.mlp.fc1.bias")
        f2w = W(f"{p}.mlp.fc2.weight")
        f2b = W(f"{p}.mlp.fc2.bias")
        f.write(ln1w.tobytes()); f.write(ln1b.tobytes())
        f.write(np.concatenate([q, k, v], axis=0).tobytes())   # [2304,768]
        f.write(np.concatenate([qb, kb, vb], axis=0).tobytes()) # [2304]
        f.write(ow.tobytes()); f.write(ob.tobytes())
        f.write(ln2w.tobytes()); f.write(ln2b.tobytes())
        f.write(f1w.tobytes()); f.write(f1b.tobytes())
        f.write(f2w.tobytes()); f.write(f2b.tobytes())
    f.write(W("final_layer_norm.weight").tobytes())
    f.write(W("final_layer_norm.bias").tobytes())
print("weights.bin written")

with open(os.path.join(OUT, "config.txt"), "w") as f:
    f.write(f"H={D} L={L} FFN={FFN} MAX={MAX} V={V} HEADS={H} DH={DH}\n")

# ---------------- 参考 embedding ----------------
prompt = "a photo of a cat sitting on a table"
ids = tok(prompt, padding="max_length", max_length=MAX, truncation=True, return_tensors="pt")
input_ids = ids["input_ids"]
attention_mask = ids["attention_mask"]
print("prompt tokens:", input_ids[0].tolist())
print("real seq_len =", int(attention_mask.sum()))
with open(os.path.join(OUT, "prompt_ids.bin"), "wb") as f:
    f.write(input_ids[0].numpy().astype(np.int32).tobytes())
with open(os.path.join(OUT, "prompt_mask.bin"), "wb") as f:
    f.write(attention_mask[0].numpy().astype(np.int32).tobytes())

with torch.no_grad():
    emb = m(input_ids=input_ids, attention_mask=attention_mask).last_hidden_state
emb = emb[0].numpy().astype(np.float32)          # [77,768]
emb.tofile(os.path.join(OUT, "ref_emb.bin"))
print("ref_emb.bin written", emb.shape)
# 打印前几个有效位置的头部数值，供 MYP 对拍冒烟
print("ref_emb[0][0:4] =", emb[0][0:4])
