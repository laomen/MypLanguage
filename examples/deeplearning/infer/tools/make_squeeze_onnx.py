#!/usr/bin/env python3
"""G5：Squeeze（去 size-1 维）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_squeeze_onnx.py
生成:
  deeplearning/data/onnx/squeeze_test.onnx
  deeplearning/data/onnx/squeeze_in.f32
  deeplearning/data/onnx/squeeze_test_ort.bin

模型（opset11，axes 属性）: x[1,2,3,4] → Squeeze(axes=[0]) → y[2,3,4]
  （去掉 batch 维=1；数据不变）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/squeeze_test.onnx"
IN_F32 = "deeplearning/data/onnx/squeeze_in.f32"
ORT_BIN = "deeplearning/data/onnx/squeeze_test_ort.bin"

np.random.seed(54)
x = np.random.randn(1, 2, 3, 4).astype(np.float32)

nodes = [
    helper.make_node("Squeeze", ["x"], ["y"], axes=[0], name="sq1"),
]
graph = helper.make_graph(
    nodes, "squeeze_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 3, 4])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [2, 3, 4])],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 11)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
x.tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(None, {"x": x})[0]
y.tofile(ORT_BIN)
print("wrote", OUT, "y.shape", y.shape, "y[0,0,0]", y[0, 0, 0], "y[1,2,3]", y[1, 2, 3])
