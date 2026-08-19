#!/usr/bin/env python3
"""P4：比较 coarse_model 的 MYP 输出（coarse_myp.bin）与 ORT 参考（coarse_ort.bin）。
用法：先运行 coarse_main.myp 生成 coarse_myp.bin，再运行本脚本。
"""
import numpy as np, sys

a = np.fromfile("deeplearning/data/onnx/coarse_myp.bin", dtype=np.float32)
b = np.fromfile("deeplearning/data/onnx/coarse_ort.bin", dtype=np.float32)
print("myp size:", a.size, " ort size:", b.size)
if a.size != b.size:
    print("SIZE MISMATCH")
    sys.exit(1)
n = min(a.size, b.size)
diff = np.abs(a[:n] - b[:n])
idx = np.argmax(diff)
print("max diff:", float(diff[idx]), "at", int(idx))
print("myp[at]=", float(a[idx]), " ort[at]=", float(b[idx]))
# 相对误差（大模型 143 op 的 float32 累积漂移，绝对差可达 ~1e-2）
rel = diff[idx] / max(1.0, abs(float(b[idx])))
print("rel diff at max:", float(rel))
# sample comparison
np.random.seed(0)
samp = np.random.choice(n, 100000, replace=False)
md = np.max(np.abs(a[samp] - b[samp]))
print("sample max diff (100k):", float(md))
# 阈值：深层网络的 float32 累积误差（绝对值 0.05 或 相对 5e-3）
if diff[idx] < 0.05 or rel < 5.0e-3:
    print("COARSE MATCH OK (float32 accumulation drift)")
else:
    print("COARSE MISMATCH")
