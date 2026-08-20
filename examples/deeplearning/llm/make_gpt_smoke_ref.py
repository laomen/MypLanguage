#!/usr/bin/env python3
"""make_gpt_smoke_ref.py — 阶段5：小 GPT 前向 numpy 参考 + 权重/期望 logits 导出。

模型：2 层小 GPT（V=32, D=64, H=4, dh=16, S=16, ffn=128, 无 tokenizer，纯随机权重）。
  Embedding → [RMSNorm → QKV(3D) → 分裂 q/k/v → 缩放点积注意力(因果) → 残差
             → RMSNorm → FFN(silu) → 残差]×2 → LM head(Gemm) → logits [V,S]。
布局：所有激活 [特征, 序列]（行=特征，列=token），与 ops.myp 的 dense/attention/rmsNorm 一致。
导出：gpt_smoke_weights.bin（各权重 float32 LE 顺序拼接）+ gpt_smoke_logits.bin（[V,S]）。

用法：
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/make_gpt_smoke_ref.py
"""
import os
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "deeplearning", "data")
np.random.seed(42)

V, D, H, S, FFN, L = 32, 64, 4, 16, 128, 2
DH = D // H
EPS = 1e-6


def w(shape, scale=0.1):
    return (np.random.randn(*shape) * scale).astype(np.float32)


def rmsnorm(x, g):
    m = (x.astype(np.float64) ** 2).mean(axis=0)
    r = 1.0 / np.sqrt(m + EPS)
    return (x * (r[None, :] * g[:, None])).astype(np.float32)


def silu(x):
    return (x * (1.0 / (1.0 + np.exp(-x.astype(np.float64))))).astype(np.float32)


def attn(q, k, v):  # each [D,S]
    qd, kd, vd = q.astype(np.float64), k.astype(np.float64), v.astype(np.float64)
    qm, km, vm = qd.reshape(H, DH, S), kd.reshape(H, DH, S), vd.reshape(H, DH, S)
    sc = np.einsum("hdi,hdj->hij", qm, km) / np.sqrt(DH)
    mask = np.triu(np.ones((S, S)), 1).astype(bool)
    sc[:, mask] = -1e30
    e = np.exp(sc - np.max(sc, axis=-1, keepdims=True))
    p = e / e.sum(axis=-1, keepdims=True)
    out = np.einsum("hij,hdj->hdi", p, vm)
    return out.reshape(D, S).astype(np.float32)


E = w((V, D), 0.15)
layers = []
for _ in range(L):
    layers.append((w((D,)), w((3 * D, D)), w((3 * D,), 0.05), w((D,)),
                   w((FFN, D)), w((FFN,), 0.05), w((D, FFN)), w((D,), 0.05)))
Wout = w((V, D), 0.1)
bout = w((V,), 0.05)

ids = np.array([1, 3, 5, 7, 2, 4, 6, 8, 0, 9, 11, 13, 15, 17, 19, 21], dtype=np.int64)
x = E[ids].T  # [D,S]
for (g1, Wqkv, bqkv, g2, W1, b1, W2, b2) in layers:
    xn = rmsnorm(x, g1)
    qkv = (Wqkv @ xn + bqkv[:, None]).astype(np.float32)
    q, k, v = qkv[0:D, :], qkv[D:2 * D, :], qkv[2 * D:3 * D, :]
    a = attn(q, k, v)
    x = (x + a).astype(np.float32)
    xn2 = rmsnorm(x, g2)
    h = silu(W1 @ xn2 + b1[:, None])
    out = (W2 @ h + b2[:, None]).astype(np.float32)
    x = (x + out).astype(np.float32)
logits = (Wout @ x + bout[:, None]).astype(np.float32)  # [V,S]

with open(os.path.join(DATA, "gpt_smoke_weights.bin"), "wb") as f:
    for arr in [E] + [p for layer in layers for p in layer] + [Wout, bout]:
        f.write(arr.flatten().tobytes())
with open(os.path.join(DATA, "gpt_smoke_logits.bin"), "wb") as f:
    f.write(logits.flatten().tobytes())

print("wrote gpt_smoke_weights.bin + gpt_smoke_logits.bin")
print("ids=", ids.tolist())
print("logits[0,:4]=", logits[0, :4].tolist())
print("logits shape", logits.shape)
