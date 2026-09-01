#!/usr/bin/env python3
"""make_opt_ref.py — 优化器（SGD/动量/AdamW）numpy 参考（阶段5e）
5 步已知梯度，float32 逐步计算（与 MYP 内核同公式）→ ref_sgd/ref_mom/ref_adamw.bin。
用法（须在 examples/ 下）：
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_opt_ref.py
"""
import os
import numpy as np

OUT = "deeplearning/data/opt"
os.makedirs(OUT, exist_ok=True)
N = 8
T = 5
lr = 0.01
wd = 0.01
decay = lr * wd
w0 = np.array([0.1, -0.2, 0.3, 0.4, -0.5, 0.6, 0.7, 0.8], dtype=np.float32)
grads = np.zeros((T, N), dtype=np.float32)
for t in range(T):
    for i in range(N):
        grads[t, i] = np.float32((t + 1) * 0.1 + i * 0.01)
w0.tofile(os.path.join(OUT, "w0.bin"))
grads.tofile(os.path.join(OUT, "grads.bin"))


def sgd():
    w = w0.copy()
    for t in range(T):
        g = grads[t]
        w = (w - np.float32(decay) * w - np.float32(lr) * g).astype(np.float32)
    return w


def mom():
    w = w0.copy()
    v = np.zeros(N, np.float32)
    for t in range(T):
        g = grads[t]
        v = (np.float32(0.9) * v + g).astype(np.float32)
        w = (w - np.float32(decay) * w - np.float32(lr) * v).astype(np.float32)
    return w


def adamw():
    w = w0.copy()
    m = np.zeros(N, np.float32)
    v = np.zeros(N, np.float32)
    eps = 1e-8
    for t in range(1, T + 1):
        g = grads[t - 1]
        m = (np.float32(0.9) * m + np.float32(0.1) * g).astype(np.float32)
        v = (np.float32(0.999) * v + np.float32(0.001) * g * g).astype(np.float32)
        bc1 = 1.0 - 0.9 ** t
        bc2 = 1.0 - 0.999 ** t
        mhat = (m / np.float32(bc1)).astype(np.float32)
        vhat = (v / np.float32(bc2)).astype(np.float32)
        w = (w - np.float32(decay) * w - np.float32(lr) * (mhat / (np.sqrt(vhat) + eps))).astype(np.float32)
    return w


sgd().tofile(os.path.join(OUT, "ref_sgd.bin"))
mom().tofile(os.path.join(OUT, "ref_mom.bin"))
adamw().tofile(os.path.join(OUT, "ref_adamw.bin"))
print("wrote", OUT)
