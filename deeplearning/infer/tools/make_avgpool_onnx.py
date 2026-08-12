#!/usr/bin/env python3
"""F8：生成合成 ONNX（AveragePool，count_include_pad 0/1）+ ORT 交叉校验。

用法（仓库根，venv 解释器）:
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_avgpool_onnx.py
  生成:
    deeplearning/data/onnx/avgpool_test.onnx
    deeplearning/data/onnx/avgpool_test.f32
    deeplearning/data/onnx/avgpool_test_ort.bin

模型（opset14）:
  x[1,2,6,6] → AveragePool(k=3,s=1,p=1,count_include_pad=0) → a[1,2,6,6]
             → AveragePool(k=2,s=2,p=0,count_include_pad=1) → y[1,2,3,3]
  两段各覆盖一种 cip 模式：第一段 cip=0（默认，除数排除 pad），第二段 cip=1（除数=kh*kw）。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/avgpool_test.onnx"
IN_F32 = "deeplearning/data/onnx/avgpool_test.f32"
ORT_BIN = "deeplearning/data/onnx/avgpool_test_ort.bin"

np.random.seed(41)
x = np.random.randn(1, 2, 6, 6).astype(np.float32)

nodes = [
    helper.make_node("AveragePool", ["x"], ["a"], kernel_shape=[3, 3], strides=[1, 1],
                     pads=[1, 1, 1, 1], count_include_pad=0, name="avg1"),
    helper.make_node("AveragePool", ["a"], ["y"], kernel_shape=[2, 2], strides=[2, 2],
                     pads=[0, 0, 0, 0], count_include_pad=1, name="avg2"),
]

graph = helper.make_graph(
    nodes, "avgpool_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 6, 6])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 2, 3, 3])],
    initializer=[],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 14)])
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
