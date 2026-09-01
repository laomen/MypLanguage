#!/usr/bin/env python3
"""G4：Expand（broadcast 复制）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_expand_onnx.py
生成:
  deeplearning/data/onnx/expand_test.onnx
  deeplearning/data/onnx/expand_test.f32
  deeplearning/data/onnx/expand_test_ort.bin

模型（opset13）: x[1,2,1,1] → Expand(shape=[1,2,3,4]) → y[1,2,3,4]
  （H/W 维从 1 广播复制到 3/4）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/expand_test.onnx"
IN_F32 = "deeplearning/data/onnx/expand_test.f32"
ORT_BIN = "deeplearning/data/onnx/expand_test_ort.bin"

np.random.seed(51)
x = np.random.randn(1, 2, 1, 1).astype(np.float32)
shape = np.array([1, 2, 3, 4], dtype=np.int64)

nodes = [
    helper.make_node("Expand", ["x", "shape"], ["y"], name="exp"),
]
graph = helper.make_graph(
    nodes, "expand_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 1, 1])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 2, 3, 4])],
    initializer=[numpy_helper.from_array(shape, name="shape")],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
x.tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(None, {"x": x})[0]
y.tofile(ORT_BIN)
print("wrote", OUT, "y.shape", y.shape, "y[0,0,0,0]", y[0, 0, 0, 0], "y[0,0,2,3]", y[0, 0, 2, 3])
