#!/usr/bin/env python3
# verify_unet_ops.py — D3a：UnetOps 四算子（groupNorm/attention2/geglu/nearestUpsample2x）
# vs numpy/torch 参考，编译运行 unet_ops_test.myp 对拍。
#
# 布局与 unet_ops_test.myp 一致；参考用 float64 镜像 MYP 的 double 累积 → 期望近字节精确。

import math
import os
import subprocess
import sys

import numpy as np

DL = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EX = os.path.dirname(DL)
REPO = os.path.dirname(EX)
OUT = os.path.join(DL, "data", "diffusion", "opstest")
os.makedirs(OUT, exist_ok=True)

rng = np.random.default_rng(42)

# ============ 生成输入（布局与 MYP 一致） ============
C, H, W = 8, 8, 8
x_gn = rng.standard_normal(C * H * W).astype(np.float32)
g_gn = rng.standard_normal(C).astype(np.float32)
b_gn = rng.standard_normal(C).astype(np.float32)

D, heads, dh = 16, 4, 4
S, kvS = 9, 5
q_cr = rng.standard_normal(D * S).astype(np.float32)
k_cr = rng.standard_normal(D * kvS).astype(np.float32)
v_cr = rng.standard_normal(D * kvS).astype(np.float32)
S2 = 7
q_sf = rng.standard_normal(D * S2).astype(np.float32)
k_sf = rng.standard_normal(D * S2).astype(np.float32)
v_sf = rng.standard_normal(D * S2).astype(np.float32)

gn = 64
gg_in = rng.standard_normal(2 * gn).astype(np.float32)

CH, HW, WW = 4, 4, 4
up_x = rng.standard_normal(CH * HW * WW).astype(np.float32)

inp = np.concatenate([x_gn, g_gn, b_gn, q_cr, k_cr, v_cr, q_sf, k_sf, v_sf, gg_in, up_x])
inp.astype(np.float32).tofile(os.path.join(OUT, "input.bin"))

# ============ numpy float64 参考 ============
def group_norm(x, g, b, C, H, W, groups, eps):
    x = x.reshape(C, H * W).astype(np.float64)
    out = np.zeros_like(x)
    cpg = C // groups
    for gg in range(groups):
        sl = slice(gg * cpg, (gg + 1) * cpg)
        seg = x[sl]
        mean = seg.mean()
        var = seg.var()
        rstd = 1.0 / math.sqrt(var + eps)
        out[sl] = (seg - mean) * rstd * g[sl, None] + b[sl, None]
    return out.reshape(-1).astype(np.float32)

def attention2(q, k, v, D, S, kvS, heads):
    q = q.astype(np.float64).reshape(D, S)
    k = k.astype(np.float64).reshape(D, kvS)
    v = v.astype(np.float64).reshape(D, kvS)
    dh = D // heads
    scale = 1.0 / math.sqrt(dh)
    out = np.zeros((D, S))
    for b in range(heads):
        qb = q[b * dh:(b + 1) * dh, :]      # [dh,S]
        kb = k[b * dh:(b + 1) * dh, :]      # [dh,kvS]
        vb = v[b * dh:(b + 1) * dh, :]      # [dh,kvS]
        sc = (qb.T @ kb) * scale            # [S,kvS]
        p = np.exp(sc - sc.max(axis=1, keepdims=True))
        p = p / p.sum(axis=1, keepdims=True)
        out[b * dh:(b + 1) * dh, :] = vb @ p.T
    return out.reshape(-1).astype(np.float32)

def geglu(a, b):
    # 与 InferOps.gelu 相同的 Abramowitz-Stegun 近似（erf）
    a = a.astype(np.float64); b = b.astype(np.float64)
    z = b / 1.4142135623730951
    az = np.abs(z)
    t = 1.0 / (1.0 + 0.3275911 * az)
    y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * np.exp(-az * az)
    erf = np.where(z < 0.0, -y, y)
    g = 0.5 * b * (1.0 + erf)
    return (a * g).astype(np.float32)

def up2x(x, C, H, W):
    x = x.reshape(C, H, W).astype(np.float64)
    out = np.zeros((C, 2 * H, 2 * W))
    for h in range(H):
        for w in range(W):
            out[:, 2 * h, 2 * w] = x[:, h, w]
            out[:, 2 * h, 2 * w + 1] = x[:, h, w]
            out[:, 2 * h + 1, 2 * w] = x[:, h, w]
            out[:, 2 * h + 1, 2 * w + 1] = x[:, h, w]
    return out.reshape(-1).astype(np.float32)

y_gn = group_norm(x_gn, g_gn, b_gn, C, H, W, 4, 1e-5)
ocr = attention2(q_cr, k_cr, v_cr, D, S, kvS, heads)
osf = attention2(q_sf, k_sf, v_sf, D, S2, S2, heads)
gg_out = geglu(gg_in[:gn], gg_in[gn:])
up_y = up2x(up_x, CH, HW, WW)

ref = np.concatenate([y_gn, ocr, osf, gg_out, up_y]).astype(np.float32)
ref.tofile(os.path.join(OUT, "ref_out.f32"))

# ============ 编译 + 运行 MYP ============
mypc = os.path.join(REPO, "build", "mypc")
stdlib = os.path.join(REPO, "stdlib")
src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "unet_ops_test.myp")
binp = "/tmp/unetops"
subprocess.run([mypc, src, "-o", binp, "--stdlib", stdlib], check=True, cwd=REPO, capture_output=True)
subprocess.run([binp], check=True, cwd=EX, capture_output=True)

# ============ 对拍 ============
myp_out = np.fromfile(os.path.join(OUT, "output.bin"), dtype=np.float32)
assert len(myp_out) == len(ref), f"长度 {len(myp_out)} vs {len(ref)}"
names = [("groupNorm", 512), ("attn2-cross", 144), ("attn2-self", 112), ("geglu", 64), ("upsample2x", 256)]
off = 0
all_ok = True
print("=" * 60)
for name, n in names:
    a = myp_out[off:off + n].astype(np.float64)
    r = ref[off:off + n].astype(np.float64)
    md = float(np.max(np.abs(a - r)))
    ident = np.array_equal(myp_out[off:off + n].view(np.uint8), ref[off:off + n].view(np.uint8))
    print(f"  {name:12s} maxAbsDiff={md:.3e}  字节级一致={ident}")
    all_ok = all_ok and (md < 1e-5)
    off += n
print("=" * 60)
if all_ok:
    print("  UNET OPS OK  (groupNorm / attention2 / geglu / upsample2x)")
    sys.exit(0)
else:
    print("  UNET OPS FAIL")
    sys.exit(1)
