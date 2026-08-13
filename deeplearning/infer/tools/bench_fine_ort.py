#!/usr/bin/env python3
"""对比 fine_model_liver_vessel：MYP CPU vs onnxruntime CPU。
用法：
  python3 deeplearning/infer/tools/bench_fine_ort.py
先跑 bench/fine_cpu_bench.myp 生成 /tmp/seg_liver_cpu.f32 再对比。
"""
import numpy as np, time, sys
import onnxruntime as ort

MODEL = "deeplearning/data/onnx/fine_model_liver_vessel.onnx"
INP = "/tmp/seg_liver_in.f32"
MYP_OUT = "/tmp/seg_liver_cpu.f32"

inp = np.fromfile(INP, dtype=np.float32)
print(f"input: {inp.size} floats = {inp.reshape(-1)[:6].tolist()}...")

sess = ort.InferenceSession(MODEL, providers=["CPUExecutionProvider"])
iname = sess.get_inputs()[0].name
print("input name:", iname, "shape:", sess.get_inputs()[0].shape)
print("outputs:", [(o.name, o.shape) for o in sess.get_outputs()])

# ONNX 输入声明为动态维 [1,1,0,0,0]（形状注入）→ 显式喂 96³（与 MYP runtime 一致）
assert inp.size == 96 ** 3, f"expect 96^3={96**3} floats, got {inp.size}"
x = inp.reshape(1, 1, 96, 96, 96)
print("feed shape:", x.shape)

def bench(provider_opts, label, iters=5):
    s = ort.InferenceSession(MODEL, providers=[("CPUExecutionProvider", provider_opts)])
    # warmup
    for _ in range(2):
        s.run(None, {iname: x})
    ts = []
    for _ in range(iters):
        t0 = time.perf_counter()
        s.run(None, {iname: x})
        ts.append((time.perf_counter() - t0) * 1000)
    ts.sort()
    print(f"ORT {label}: best {ts[0]:.1f} ms  avg {np.mean(ts):.1f} ms  (runs {[f'{t:.1f}' for t in ts]})")
    return ts[0], s.run(None, {iname: x})[0]

best_mt, out_mt = bench({}, "CPU multi-thread", iters=3)
best_st, out_st = bench({"intra_op_num_threads": 1, "inter_op_num_threads": 1}, "CPU 1-thread", iters=1)

# 对比 MYP CPU 输出
try:
    a = np.fromfile(MYP_OUT, dtype=np.float32)
except Exception as e:
    print("no myp output:", e); sys.exit(0)
b = out_mt.reshape(-1).astype(np.float32)
print("myp size:", a.size, " ort size:", b.size)
if a.size == b.size:
    diff = np.abs(a - b)
    idx = int(np.argmax(diff))
    print("MYP CPU vs ORT max diff:", float(diff[idx]), "at", idx,
          " rel:", float(diff[idx] / max(1.0, abs(float(b[idx])))))
    print("FINE MATCH OK (float32 accumulation drift)" if diff[idx] < 0.05 else "FINE MISMATCH")
