#!/usr/bin/env python3
"""3D：生成合成 Resize3D ONNX（trilinear, align_corners）+ ORT 交叉校验。

用法（仓库根，venv 解释器）:
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_resize3d_onnx.py
  生成:
    deeplearning/data/onnx/resize3d_test.onnx
    deeplearning/data/onnx/resize3d_test.f32
    deeplearning/data/onnx/resize3d_test_ort.bin

模型（opset13）:
  x[1,2,4,4,4] → Resize(sizes=[1,2,8,8,8], linear, align_corners) → y[1,2,8,8,8]
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/resize3d_test.onnx"
IN_F32 = "deeplearning/data/onnx/resize3d_test.f32"
ORT_BIN = "deeplearning/data/onnx/resize3d_test_ort.bin"

np.random.seed(7)
x = np.random.randn(1, 2, 4, 4, 4).astype(np.float32)
sizes = np.array([1, 2, 8, 8, 8], dtype=np.int64)
empty = np.array([], dtype=np.float32)

nodes = [
    helper.make_node("Resize", ["x", "roi", "scales", "sz"], ["y"],
                     mode="linear", coordinate_transformation_mode="align_corners", name="rs3d"),
]

graph = helper.make_graph(
    nodes, "resize3d_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 4, 4, 4])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 2, 8, 8, 8])],
    initializer=[
        numpy_helper.from_array(empty, name="roi"),
        numpy_helper.from_array(empty, name="scales"),
        numpy_helper.from_array(sizes, name="sz"),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 8
onnx.checker.check_model(model)
onnx.save(model, OUT)
print("wrote", OUT)

import onnxruntime as ort
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(["y"], {"x": x})[0]
print("ORT y shape:", y.shape, "sum:", float(y.sum()))

with open(IN_F32, "wb") as f:
    f.write(x.tobytes())
with open(ORT_BIN, "wb") as f:
    f.write(y.astype(np.float32).tobytes())
print("wrote", IN_F32, "and", ORT_BIN)
