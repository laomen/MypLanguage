#!/usr/bin/env python3
"""阶段五：死权重裁剪（DCE 后移除未引用初始器）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_deadweight_onnx.py
生成:
  deeplearning/data/onnx/deadweight_test.onnx
  deeplearning/data/onnx/deadweight_in.f32
  deeplearning/data/onnx/deadweight_test_ort.bin

模型（opset13）:
  x[1,1,5,5] → Conv(W[1,1,3,3], B[1]) → y[1,1,3,3]
  + 未引用初始器 junk[3,3]（不被任何节点消费 → 应被死权重裁剪）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/deadweight_test.onnx"
IN_F32 = "deeplearning/data/onnx/deadweight_in.f32"
ORT_BIN = "deeplearning/data/onnx/deadweight_test_ort.bin"

np.random.seed(62)
x = np.random.randn(1, 1, 5, 5).astype(np.float32)
w = np.random.randn(1, 1, 3, 3).astype(np.float32)
b = np.random.randn(1).astype(np.float32)
junk = np.random.randn(3, 3).astype(np.float32)   # 未引用

nodes = [
    helper.make_node("Conv", ["x", "w", "b"], ["y"], kernel_shape=[3, 3], pads=[0, 0, 0, 0], strides=[1, 1]),
]
graph = helper.make_graph(
    nodes, "deadweight_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1, 5, 5])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1, 3, 3])],
    initializer=[
        numpy_helper.from_array(w, name="w"),
        numpy_helper.from_array(b, name="b"),
        numpy_helper.from_array(junk, name="junk"),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
x.tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(None, {"x": x})[0]
y.tofile(ORT_BIN)
print("wrote", OUT, "n_initializer", len(graph.initializer), "y[0,0,0,0]", y[0, 0, 0, 0])
