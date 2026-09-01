#!/usr/bin/env python3
"""阶段五：Conv 1x1 专用 lowering（opKind 83）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_conv1x1_onnx.py
生成:
  deeplearning/data/onnx/conv1x1_test.onnx
  deeplearning/data/onnx/conv1x1_in.f32
  deeplearning/data/onnx/conv1x1_test_ort.bin

模型（opset13）:
  x[1,3,8,8]
    ├─ Conv(w1[5,3,1,1], b1[5]) → y1[1,5,8,8]    （1x1 无 ReLU）
    ├─ Conv(w2[4,5,1,1], b2[4]) → y2[1,4,8,8]    （1x1 + ReLU）
    └─ Conv(w3[2,3,3,3], b3[2]) → y3[1,2,6,6]    （3x3 对照，走普通 conv）
  y = Concat(y1, y2, y3, axis=1) → [1,11,8,8]

验证：y1/y2 走 Conv1x1Op（opKind 83），y3 仍走普通 conv（opKind 7）；
输出 vs ORT 一致。运行时 op 表中应出现 opKind 83。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/conv1x1_test.onnx"
IN_F32 = "deeplearning/data/onnx/conv1x1_in.f32"
ORT_BIN = "deeplearning/data/onnx/conv1x1_test_ort.bin"

np.random.seed(65)
x = np.random.randn(1, 3, 8, 8).astype(np.float32)
w1 = np.random.randn(5, 3, 1, 1).astype(np.float32)
b1 = np.random.randn(5).astype(np.float32)
w2 = np.random.randn(4, 5, 1, 1).astype(np.float32)
b2 = np.random.randn(4).astype(np.float32)
w3 = np.random.randn(2, 3, 3, 3).astype(np.float32)
b3 = np.random.randn(2).astype(np.float32)

nodes = [
    helper.make_node("Conv", ["x", "w1", "b1"], ["y1"], kernel_shape=[1, 1], pads=[0, 0, 0, 0], strides=[1, 1]),
    helper.make_node("Conv", ["y1", "w2", "b2"], ["y2"], kernel_shape=[1, 1], pads=[0, 0, 0, 0], strides=[1, 1]),
    helper.make_node("Relu", ["y2"], ["y2r"]),
    helper.make_node("Conv", ["x", "w3", "b3"], ["y3"], kernel_shape=[3, 3], pads=[1, 1, 1, 1], strides=[1, 1]),
    helper.make_node("Concat", ["y1", "y2r", "y3"], ["y"], axis=1),
]
graph = helper.make_graph(
    nodes, "conv1x1_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 3, 8, 8])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 11, 8, 8])],
    initializer=[
        numpy_helper.from_array(w1, name="w1"),
        numpy_helper.from_array(b1, name="b1"),
        numpy_helper.from_array(w2, name="w2"),
        numpy_helper.from_array(b2, name="b2"),
        numpy_helper.from_array(w3, name="w3"),
        numpy_helper.from_array(b3, name="b3"),
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
print("wrote", OUT, "y.shape", y.shape, "y[0,0,0,0]", y[0, 0, 0, 0])
