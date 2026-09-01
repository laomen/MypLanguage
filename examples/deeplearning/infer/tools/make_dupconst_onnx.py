#!/usr/bin/env python3
"""阶段五：常量去重（内容相同初始器合并）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_dupconst_onnx.py
生成:
  deeplearning/data/onnx/dupconst_test.onnx
  deeplearning/data/onnx/dupconst_in.f32
  deeplearning/data/onnx/dupconst_test_ort.bin

模型（opset13）:
  x[1,1,5,5]
    ├─ Conv(w1[1,1,3,3], b1[1]) → y1[1,1,3,3]
    └─ Conv(w2[1,1,3,3], b2[1]) → y2[1,1,3,3]     （w2==w1 字节相同，b2==b1）
  y = Add(y1, y2)

w2/b2 与 w1/b1 内容完全相同（但名字不同）→ 应被常量去重合并：
w2 的所有 use 改接 w1、标 dead；b2→b1。runtime 只注册一份 w1/b1，输出不变。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/dupconst_test.onnx"
IN_F32 = "deeplearning/data/onnx/dupconst_in.f32"
ORT_BIN = "deeplearning/data/onnx/dupconst_test_ort.bin"

np.random.seed(63)
x = np.random.randn(1, 1, 5, 5).astype(np.float32)
w1 = np.random.randn(1, 1, 3, 3).astype(np.float32)
w2 = np.array(w1, copy=True)               # 与 w1 字节相同
b1 = np.random.randn(1).astype(np.float32)
b2 = np.array(b1, copy=True)               # 与 b1 字节相同

nodes = [
    helper.make_node("Conv", ["x", "w1", "b1"], ["y1"], kernel_shape=[3, 3], pads=[0, 0, 0, 0], strides=[1, 1]),
    helper.make_node("Conv", ["x", "w2", "b2"], ["y2"], kernel_shape=[3, 3], pads=[0, 0, 0, 0], strides=[1, 1]),
    helper.make_node("Add", ["y1", "y2"], ["y"]),
]
graph = helper.make_graph(
    nodes, "dupconst_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1, 5, 5])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1, 3, 3])],
    initializer=[
        numpy_helper.from_array(w1, name="w1"),
        numpy_helper.from_array(b1, name="b1"),
        numpy_helper.from_array(w2, name="w2"),
        numpy_helper.from_array(b2, name="b2"),
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
print("wrote", OUT, "n_initializer", len(graph.initializer),
      "w2==w1", np.array_equal(w1, w2), "b2==b1", np.array_equal(b1, b2),
      "y[0,0,0,0]", y[0, 0, 0, 0])
