#!/usr/bin/env python3
"""verify_vae_tail.py — 验证 VAE 解码尾部（conv_norm_out→silu→conv_out）与 ref_out 的一致性。
如果 numpy 从 stage_conv_norm_out 出发经 silu+conv_out 能还原 ref_out，则 MYP 的 bug 在 silu/conv_out；
否则 ref_out 本身与 stage 链不一致（参考/提取问题）。
"""
import os
import numpy as np

DL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(DL, "data", "diffusion", "vae")

W = np.fromfile(os.path.join(OUT, "vae_weights.bin"), dtype=np.float32)
B = np.fromfile(os.path.join(OUT, "bases.bin"), dtype=np.int32)

# conv_norm_out 块（bases[20]）：group_norm w,b；conv_out 块（bases[21]）：[3,128,3,3] + bias[3]
gnw = B[20]
gwb = B[20] + 128
cw = B[21]
cb = B[21] + 3 * 128 * 9

gn = np.fromfile(os.path.join(OUT, "stage_conv_norm_out.f32"), dtype=np.float32).reshape(1, 128, 512, 512)

# silu
s = gn * (1.0 / (1.0 + np.exp(-gn)))

# conv_out 3x3 pad1 stride1（numpy：张量化，[3,128,3,3] 权重，im2col）
w = W[cw:cb].reshape(3, 128, 3, 3).astype(np.float32)
b = W[cb:cb + 3].astype(np.float32)
x = np.pad(s, ((0, 0), (0, 0), (1, 1), (1, 1)), mode="constant")
out = np.zeros((1, 3, 512, 512), dtype=np.float32)
# 滑动窗口求和：把 3x3 邻域乘权重累加
for c in range(3):
    acc = np.zeros((512, 512), dtype=np.float32)
    for ic in range(128):
        for ky in range(3):
            for kx in range(3):
                acc += w[c, ic, ky, kx] * x[0, ic, ky:ky + 512, kx:kx + 512]
    out[0, c] = acc + b[c]

ref = np.fromfile(os.path.join(OUT, "ref_out.f32"), dtype=np.float32).reshape(1, 3, 512, 512)
d = np.abs(out - ref)
print(f"numpy tail maxAbsDiff = {d.max():.6f}")
print("numpy out ch0 row0 px0-3 =", out[0, 0, 0, :4].tolist())
print("ref   out ch0 row0 px0-3 =", ref[0, 0, 0, :4].tolist())
if d.max() < 1e-3:
    print("CONSISTENT: ref_out == silu(conv_norm_out) conv_out  → MYP bug 在 silu/conv_out")
else:
    print("INCONSISTENT: ref_out 与 stage 链不一致 → 参考/提取问题（非 MYP 尾部）")
