#!/usr/bin/env python3
"""debug_offsets.py — 用 safetensors 原权重逐个核对 attention 块张量在 weights.bin 的偏移。"""
import numpy as np
from safetensors import safe_open

DL = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/unet"
SD = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/sd15"
w = np.fromfile(f"{DL}/weights.bin", dtype=np.float32)
base = 2266880  # base[2]

prefix = "down_blocks.0.attentions.0"
with safe_open(f"{SD}/unet/diffusion_pytorch_model.safetensors", framework="np") as f:
    keys = []
    for k in f.keys():
        if k.startswith(prefix):
            keys.append(k)
    keys.sort()
    off = 0
    print(f"{'tensor':<70}{'size':>10}{'offset':>10}  match")
    for k in keys:
        arr = f.get_tensor(k)
        n = arr.size
        wb = w[base+off:base+off+n]
        ok = np.array_equal(wb, arr.reshape(-1))
        print(f"{k:<70}{n:>10}{off:>10}  {'OK' if ok else 'MISMATCH'}")
        off += n
    print("total =", off)
