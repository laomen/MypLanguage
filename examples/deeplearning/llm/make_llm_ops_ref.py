#!/usr/bin/env python3
"""make_llm_ops_ref.py — 阶段5：LayerNorm/GELU/RoPE/GQA-attention 的 numpy 参考 + 导出。

导出 data/llm_ops_ref.bin（float32 LE 顺序）：
  [x, gamma, beta, ln_out, xg, gelu_out, xr, cosT, sinT, rope_out, q, k, v, gqa_out]
D=64, S=16, H=4, groups=2（num_kv=2，kvD=32）。
"""
import os
import numpy as np
from math import erf

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "deeplearning", "data", "llm_ops_ref.bin")
np.random.seed(5)

D, S, H = 64, 16, 4
DH = D // H
groups = 2
num_kv = H // groups
kvD = num_kv * DH


def f32(a):
    return np.asarray(a, dtype=np.float32)


buf = []

# ---- 1) LayerNorm（按特征维、每 token；biased var /D）----
x = f32(np.random.randn(D, S))
g = f32(np.random.randn(D))
b = f32(np.random.randn(D))
mean = x.astype(np.float64).mean(0)
var = x.astype(np.float64).var(0)  # biased (/D)
ln = f32((x - mean) / np.sqrt(var + 1e-5) * g[:, None] + b[:, None])
buf += [x, g, b, ln]

# ---- 2) GELU（erf 精确版）----
xg = f32(np.random.randn(256))
gel = f32(0.5 * xg * (1 + np.vectorize(erf)(xg / 2 ** 0.5)))
buf += [xg, gel]

# ---- 3) RoPE（Llama 真实约定：逐头、head_dim 内半配对；cos/sin 表 [dh/2,S]）----
HALF = DH // 2
xr = f32(np.random.randn(D, S))
freq = 1.0 / (10000.0 ** (2 * np.arange(HALF) / DH))   # [HALF]
ang = np.arange(S)[:, None] * freq[None, :]            # [S,HALF]
cosT = f32(np.cos(ang).T)                              # [HALF,S]
sinT = f32(np.sin(ang).T)
xm = xr.reshape(H, DH, S)                              # [H,dh,S]
x0 = xm[:, :HALF, :].copy()
x1 = xm[:, HALF:, :].copy()
new0 = x0 * cosT[None, :, :] - x1 * sinT[None, :, :]
new1 = x0 * sinT[None, :, :] + x1 * cosT[None, :, :]
rope_out = xm.copy()
rope_out[:, :HALF, :] = new0
rope_out[:, HALF:, :] = new1
rope_out = f32(rope_out.reshape(D, S))
buf += [xr, cosT, sinT, rope_out]

# ---- 4) GQA attention（H=4, num_kv=2, groups=2；因果掩码）----
q = f32(np.random.randn(D, S))
k = f32(np.random.randn(kvD, S))
v = f32(np.random.randn(kvD, S))
qv = q.astype(np.float64).reshape(H, DH, S)
kexp = np.repeat(k.astype(np.float64).reshape(num_kv, DH, S), groups, axis=0)  # [H,dh,S]
vexp = np.repeat(v.astype(np.float64).reshape(num_kv, DH, S), groups, axis=0)
sc = np.einsum("hdi,hdj->hij", qv, kexp) / np.sqrt(DH)
mask = np.triu(np.ones((S, S)), 1).astype(bool)
sc[:, mask] = -1e30
e = np.exp(sc - np.max(sc, axis=-1, keepdims=True))
p = e / e.sum(axis=-1, keepdims=True)
gqa_out = f32(np.einsum("hij,hdj->hdi", p, vexp).reshape(D, S))
buf += [q, k, v, gqa_out]

with open(OUT, "wb") as f:
    for arr in buf:
        f.write(arr.flatten().tobytes())
print("wrote", OUT, "blocks:", [a.shape for a in buf])
