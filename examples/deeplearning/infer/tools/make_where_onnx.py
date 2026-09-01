#!/usr/bin/env python3
"""G5：Where（逐元素选择 cond ? x : y）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_where_onnx.py
生成:
  deeplearning/data/onnx/where_test.onnx
  deeplearning/data/onnx/where_cond.f32 / where_x.f32 / where_y.f32
  deeplearning/data/onnx/where_test_ort.bin

模型（opset13）: cond[1,2,1,1] ? x[1,2,3,4] : y[1,2,3,4] → out[1,2,3,4]
  cond 的 H/W 维=1 → 广播到整个 [1,2,3,4]（同时验证选择 + 广播）。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/where_test.onnx"
ORT_BIN = "deeplearning/data/onnx/where_test_ort.bin"

np.random.seed(52)
cond = (np.random.rand(1, 2, 1, 1) > 0.5).astype(np.float32)   # 0/1
x = np.random.randn(1, 2, 3, 4).astype(np.float32)
y = np.random.randn(1, 2, 3, 4).astype(np.float32)

nodes = [
    helper.make_node("Where", ["cond", "x", "y"], ["out"], name="w1"),
]
graph = helper.make_graph(
    nodes, "where_test",
    inputs=[
        helper.make_tensor_value_info("cond", TensorProto.BOOL, [1, 2, 1, 1]),
        helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 3, 4]),
        helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 2, 3, 4]),
    ],
    outputs=[
        helper.make_tensor_value_info("out", TensorProto.FLOAT, [1, 2, 3, 4]),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
cond.tofile("deeplearning/data/onnx/where_cond.f32")
x.tofile("deeplearning/data/onnx/where_x.f32")
y.tofile("deeplearning/data/onnx/where_y.f32")

sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
out = sess.run(None, {"cond": cond.astype(np.bool_), "x": x, "y": y})[0]
out.tofile(ORT_BIN)
print("wrote", OUT, "out.shape", out.shape, "out[0,0,0,0]", out[0, 0, 0, 0],
      "n_pos_expected", int((cond[0, 0, 0, 0] > 0) * 12))
