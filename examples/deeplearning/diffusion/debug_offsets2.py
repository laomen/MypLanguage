#!/usr/bin/env python3
"""debug_offsets2.py — 按抽取脚本 attention_block 顺序核对 down0 attn0 各张量偏移。"""
import numpy as np
from safetensors import safe_open

DL = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/unet"
SD = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/sd15"
w = np.fromfile(f"{DL}/weights.bin", dtype=np.float32)
base = 2266880  # base[2]

keys = [
 "down_blocks.0.attentions.0.norm.weight", "down_blocks.0.attentions.0.norm.bias",
 "down_blocks.0.attentions.0.proj_in.weight", "down_blocks.0.attentions.0.proj_in.bias",
 "down_blocks.0.attentions.0.transformer_blocks.0.norm1.weight", "down_blocks.0.attentions.0.transformer_blocks.0.norm1.bias",
 "down_blocks.0.attentions.0.transformer_blocks.0.attn1.to_q.weight", "down_blocks.0.attentions.0.transformer_blocks.0.attn1.to_k.weight",
 "down_blocks.0.attentions.0.transformer_blocks.0.attn1.to_v.weight", "down_blocks.0.attentions.0.transformer_blocks.0.attn1.to_out.0.weight",
 "down_blocks.0.attentions.0.transformer_blocks.0.attn1.to_out.0.bias",
 "down_blocks.0.attentions.0.transformer_blocks.0.attn2.to_q.weight", "down_blocks.0.attentions.0.transformer_blocks.0.attn2.to_k.weight",
 "down_blocks.0.attentions.0.transformer_blocks.0.attn2.to_v.weight", "down_blocks.0.attentions.0.transformer_blocks.0.attn2.to_out.0.weight",
 "down_blocks.0.attentions.0.transformer_blocks.0.attn2.to_out.0.bias",
 "down_blocks.0.attentions.0.transformer_blocks.0.norm3.weight", "down_blocks.0.attentions.0.transformer_blocks.0.norm3.bias",
 "down_blocks.0.attentions.0.transformer_blocks.0.ff.net.0.proj.weight", "down_blocks.0.attentions.0.transformer_blocks.0.ff.net.0.proj.bias",
 "down_blocks.0.attentions.0.transformer_blocks.0.ff.net.2.weight", "down_blocks.0.attentions.0.transformer_blocks.0.ff.net.2.bias",
 "down_blocks.0.attentions.0.proj_out.weight", "down_blocks.0.attentions.0.proj_out.bias",
]
with safe_open(f"{SD}/unet/diffusion_pytorch_model.safetensors", framework="np") as f:
    off = 0
    for k in keys:
        arr = f.get_tensor(k)
        n = arr.size
        wb = w[base+off:base+off+n]
        ok = np.array_equal(wb, arr.reshape(-1))
        print(f"{'.'.join(k.split('.')[-2:]):<28}{n:>10}{off:>10}  {'OK' if ok else 'MISMATCH'}")
        off += n
    print("total =", off)
