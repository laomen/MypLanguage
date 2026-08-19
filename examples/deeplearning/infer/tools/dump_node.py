#!/usr/bin/env python3
"""dump_node.py — 运行 ORT 并导出指定 ONNX 节点/张量的输出到 .bin（float32 原始字节）。
用法: dump_node.py <node_index_or_tensor_name> [out.bin]
  若参数是整数，则取该节点第一个输出；若是字符串则取同名张量。
"""
import sys, numpy as np, onnxruntime as ort, onnx, tempfile, os

M = "deeplearning/data/onnx/fine_model_liver_vessel.onnx"
IN = "deeplearning/data/onnx/fine_in.f32"
# 支持逗号分隔的多个索引/名字
SPECS = [s for s in sys.argv[1].split(",") if s]

model = onnx.load(M)
g = model.graph
NAMES = []
for s in SPECS:
    if s.isdigit():
        NAMES.append(g.node[int(s)].output[0])
    else:
        NAMES.append(s)
for nm in NAMES:
    model.graph.output.append(onnx.helper.make_empty_tensor_value_info(nm))
TMP = tempfile.mktemp(suffix=".onnx")
onnx.save(model, TMP)

sess = ort.InferenceSession(TMP, providers=["CPUExecutionProvider"])
inp = sess.get_inputs()[0]
raw = np.fromfile(IN, dtype=np.float32)
if raw.size < 4096:
    padded = np.zeros(4096, dtype=np.float32)
    padded[:raw.size] = raw
    raw = padded
x = raw.reshape([1, 1, 16, 16, 16])
res = sess.run(NAMES, {inp.name: x})
for nm, data in zip(NAMES, res):
    out = "/tmp/ort_" + nm.replace("::", "_") + ".bin"
    np.asarray(data, dtype=np.float32).tofile(out)
    print(f"{nm} shape={np.asarray(data).shape} size={np.asarray(data).size} -> {out}")
os.remove(TMP)
