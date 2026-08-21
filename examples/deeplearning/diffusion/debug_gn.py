#!/usr/bin/env python3
"""debug_gn.py — 独立复算 attention GroupNorm，定位 d0a0_gn 分歧源。
输入：stage_d0r0.f32（≈MYP D0P0，已验 1.1e-5）+ base[2] norm 权重；对照 stage_d0a0_gn.f32。
"""
import numpy as np

DL = "/home/xlkj/code/MYPLanguage/examples/deeplearning/data/diffusion/unet"
def rd(name):
    return np.fromfile(f"{DL}/{name}", dtype=np.float32)

x = rd("stage_d0r0.f32")            # [320*4096] C-major = [320, 4096]
ref = rd("stage_d0a0_gn.f32")       # torch GroupNorm 参考 [320*4096]
weights = rd("weights.bin")
bases = np.fromfile(f"{DL}/bases.bin", dtype=np.int32)
b2 = int(bases[2])
print("base[2] =", b2)
nw = weights[b2:b2+320].copy()
nb = weights[b2+320:b2+640].copy()
print("norm_w[0:5] =", nw[:5])
print("norm_b[0:5] =", nb[:5])

# 复算 groupNorm：C=320, HW=4096, groups=32, eps=1e-5
C, HW, G = 320, 4096, 32
cpg = C // G
xx = x.reshape(C, HW)
out = np.zeros_like(xx)
for g in range(G):
    seg = xx[g*cpg:(g+1)*cpg]            # [cpg, HW]
    mean = seg.mean()
    var = ((seg - mean)**2).mean()
    rstd = 1.0/np.sqrt(var + 1e-5)
    out[g*cpg:(g+1)*cpg] = (seg - mean) * rstd * nw[g*cpg:(g+1)*cpg, None] + nb[g*cpg:(g+1)*cpg, None]

diff = np.abs(out.ravel() - ref)
print("numpy groupNorm vs torch ref maxAbsDiff =", diff.max())
print("numpy[0:3] =", out.ravel()[:3], " ref[0:3] =", ref[:3])

# 也检查输入 x 与 ref 的关系：stage_d0r0 是不是组范输入
print("input x[0:3] =", x[:3])
print("ref gn[0:3] =", ref[:3])
