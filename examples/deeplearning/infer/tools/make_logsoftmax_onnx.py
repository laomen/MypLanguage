#!/usr/bin/env python3
"""G5：LogSoftmax（沿 axis 稳定 log-softmax）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_logsoftmax_onnx.py
生成:
  deeplearning/data/onnx/logsoftmax_test.onnx
  deeplearning/data/onnx/logsoftmax_in.f32
  deeplearning/data/onnx/logsoftmax_test_ort.bin

模型（opset13，axis 属性）: x[1,2,3,4] → LogSoftmax(axis=1) → y[1,2,3,4]
  （沿通道维 log-softmax）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/logsoftmax_test.onnx"
IN_F32 = "deeplearning/data/onnx/logsoftmax_in.f32"
ORT_BIN = "deeplearning/data/onnx/logsoftmax_test_ort.bin"

np.random.seed(56)
x = np.random.randn(1, 2, 3, 4).astype(np.float32) * 2.0   # 拉开数值范围测稳定性

nodes = [
    helper.make_node("LogSoftmax", ["x"], ["y"], axis=1, name="ls1"),
]
graph = helper.make_graph(
    nodes, "logsoftmax_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 3, 4])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 2, 3, 4])],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
x.tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(None, {"x": x})[0]
y.tofile(ORT_BIN)
print("wrote", OUT, "y.shape", y.shape, "y[0,0,0,0]", y[0, 0, 0, 0], "y[0,1,2,3]", y[0, 1, 2, 3])
