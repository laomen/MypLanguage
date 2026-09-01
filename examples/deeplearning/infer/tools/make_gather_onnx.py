#!/usr/bin/env python3
"""G5：Gather（沿 axis 按 indices 收集）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_gather_onnx.py
生成:
  deeplearning/data/onnx/gather_test.onnx
  deeplearning/data/onnx/gather_in.f32
  deeplearning/data/onnx/gather_test_ort.bin

模型（opset11，axis 属性 + int64 indices 初始器）:
  x[1,2,3,4] → Gather(axis=1, indices=[1,0]) → y[1,2,3,4]
  （沿 C 维取 [1,0] → 翻转通道；indices 是 int64 初始器）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/gather_test.onnx"
IN_F32 = "deeplearning/data/onnx/gather_in.f32"
ORT_BIN = "deeplearning/data/onnx/gather_test_ort.bin"

np.random.seed(55)
x = np.random.randn(1, 2, 3, 4).astype(np.float32)
indices = np.array([1, 0], dtype=np.int64)

nodes = [
    helper.make_node("Gather", ["x", "indices"], ["y"], axis=1, name="g1"),
]
graph = helper.make_graph(
    nodes, "gather_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 3, 4])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 2, 3, 4])],
    initializer=[numpy_helper.from_array(indices, name="indices")],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 11)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
x.tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(None, {"x": x})[0]
y.tofile(ORT_BIN)
print("wrote", OUT, "y.shape", y.shape, "y[0,0,0,0]", y[0, 0, 0, 0], "y[0,1,0,0]", y[0, 1, 0, 0])
