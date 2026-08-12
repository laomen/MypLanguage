#!/usr/bin/env python3
"""P4：比较 fine_model_liver_vessel 的 MYP 输出（fine_myp.bin）与 ORT 参考（fine_ort.bin）。
用法：先运行 fine_main.myp 生成 fine_myp.bin，再运行本脚本。
"""
import numpy as np, sys

a = np.fromfile("deeplearning/data/onnx/fine_myp.bin", dtype=np.float32)
b = np.fromfile("deeplearning/data/onnx/fine_ort.bin", dtype=np.float32)
print("myp size:", a.size, " ort size:", b.size)
if a.size != b.size:
    print("SIZE MISMATCH")
    sys.exit(1)
n = min(a.size, b.size)
diff = np.abs(a[:n] - b[:n])
idx = np.argmax(diff)
print("max diff:", float(diff[idx]), "at", int(idx))
print("myp[at]=", float(a[idx]), " ort[at]=", float(b[idx]))
rel = diff[idx] / max(1.0, abs(float(b[idx])))
print("rel diff at max:", float(rel))
if diff[idx] < 0.05 or rel < 5.0e-3:
    print("FINE MATCH OK (float32 accumulation drift)")
else:
    print("FINE MISMATCH")
