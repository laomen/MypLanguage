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
# sample comparison
np.random.seed(0)
samp = np.random.choice(n, 100000, replace=False)
md = np.max(np.abs(a[samp] - b[samp]))
print("sample max diff (100k):", float(md))
if md < 1.0e-3:
    print("COARSE MATCH OK")
else:
    print("COARSE MISMATCH")
