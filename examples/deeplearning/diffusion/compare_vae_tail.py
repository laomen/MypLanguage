#!/usr/bin/env python3
"""compare_vae_tail.py — 对照 MYP dump（myp_post_silu.f32 / myp_final.f32）定位 VAE 尾部 bug。
- numpy silu(conv_norm_out_ref) vs myp_post_silu   → 检查 MYP silu
- numpy conv_out(myp_post_silu) vs myp_final        → 检查 MYP conv_out
"""
import os
import numpy as np

DL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(DL, "data", "diffusion", "vae")

W = np.fromfile(os.path.join(OUT, "vae_weights.bin"), dtype=np.float32)
B = np.fromfile(os.path.join(OUT, "bases.bin"), dtype=np.int32)
cw = B[21]
cb = B[21] + 3 * 128 * 9

gn_ref = np.fromfile(os.path.join(OUT, "stage_conv_norm_out.f32"), dtype=np.float32).reshape(1, 128, 512, 512)
silu_ref = gn_ref * (1.0 / (1.0 + np.exp(-gn_ref)))
ref_final = np.fromfile(os.path.join(OUT, "ref_out.f32"), dtype=np.float32).reshape(1, 3, 512, 512)

myp_silu = np.fromfile(os.path.join(OUT, "myp_post_silu.f32"), dtype=np.float32).reshape(1, 128, 512, 512)
myp_final = np.fromfile(os.path.join(OUT, "myp_final.f32"), dtype=np.float32).reshape(1, 3, 512, 512)

print("=== MYP silu vs numpy silu(conv_norm_out_ref) ===")
print(f"maxAbsDiff = {np.abs(myp_silu - silu_ref).max():.6f}")

# MYP 的 silu 输入是否就是正确的 groupNorm 输出？
def conv3x3(x, w, b):
    xp = np.pad(x, ((0, 0), (0, 0), (1, 1), (1, 1)), mode="constant")
    o = np.zeros((1, w.shape[0], x.shape[2], x.shape[3]), dtype=np.float32)
    for c in range(w.shape[0]):
        acc = np.zeros((x.shape[2], x.shape[3]), dtype=np.float32)
        for ic in range(x.shape[1]):
            for ky in range(3):
                for kx in range(3):
                    acc += w[c, ic, ky, kx] * xp[0, ic, ky:ky + x.shape[2], kx:kx + x.shape[3]]
        o[0, c] = acc + b[c]
    return o

w = W[cw:cb].reshape(3, 128, 3, 3).astype(np.float32)
b = W[cb:cb + 3].astype(np.float32)

print("=== MYP conv_out(myp_silu) vs myp_final (MYP 内部一致性) ===")
# 用 numpy 对 MYP 的 silu 输出做 conv_out，看是否等于 MYP 的 final —— 若不等则 MYP conv_out 有 bug
calc = conv3x3(myp_silu.astype(np.float32), w, b)
print(f"maxAbsDiff = {np.abs(calc - myp_final).max():.6f}")

print("=== MYP final vs ref_out ===")
print(f"maxAbsDiff = {np.abs(myp_final - ref_final).max():.6f}")

print("=== 通道独立性检查（若某通道全 0/常数 → 布局/索引 bug）===")
for c in range(3):
    print(f"  myp_final ch{c}: min={myp_final[0,c].min():.4f} max={myp_final[0,c].max():.4f} std={myp_final[0,c].std():.4f}")
    print(f"  ref_final ch{c}: min={ref_final[0,c].min():.4f} max={ref_final[0,c].max():.4f} std={ref_final[0,c].std():.4f}")
