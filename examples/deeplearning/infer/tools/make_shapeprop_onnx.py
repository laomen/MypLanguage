#!/usr/bin/env python3
"""阶段五：形状值传播（int64 四则 Mul）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_shapeprop_onnx.py
生成:
  deeplearning/data/onnx/shapeprop_test.onnx
  deeplearning/data/onnx/shapeprop_in.f32
  deeplearning/data/onnx/shapeprop_test_ort.bin

模型（opset13）:
  x[1,1,4,4] → Conv(w[1,1,3,3], b[1]) → y[1,1,2,2]
  s  = Shape(x) = [1,1,4,4]
  s0 = Slice(s, [0],[2])   = [1,1]     （前两维不变）
  s1 = Slice(s, [2],[4])   = [4,4]     （后两维）
  s2 = Mul(s1, 2)          = [8,8]     （int64 四则形状值传播）
  sizes = Concat(s0, s2)   = [1,1,8,8]
  y2 = Resize(y, sizes=sizes) → [1,1,8,8]

验证：sizes 链（Shape→Slice→Mul→Concat）被 foldShapeChains 折叠，Resize
输出 [1,1,8,8] 与 ORT 一致。Mul(int64) 折叠是本次新增。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/shapeprop_test.onnx"
IN_F32 = "deeplearning/data/onnx/shapeprop_in.f32"
ORT_BIN = "deeplearning/data/onnx/shapeprop_test_ort.bin"

np.random.seed(64)
x = np.random.randn(1, 1, 4, 4).astype(np.float32)
w = np.random.randn(1, 1, 3, 3).astype(np.float32)
b = np.random.randn(1).astype(np.float32)
two = np.array([2], dtype=np.int64)

nodes = [
    helper.make_node("Conv", ["x", "w", "b"], ["y"], kernel_shape=[3, 3], pads=[0, 0, 0, 0], strides=[1, 1]),
    helper.make_node("Shape", ["x"], ["s"]),
    helper.make_node("Slice", ["s", "st0", "en0", "ax0"], ["s0"]),
    helper.make_node("Slice", ["s", "st1", "en1", "ax1"], ["s1"]),
    helper.make_node("Mul", ["s1", "two"], ["s2"]),
    helper.make_node("Concat", ["s0", "s2"], ["sizes"], axis=0),
    helper.make_node("Resize", ["y", "", "", "sizes"], ["y2"], mode="nearest", coordinate_transformation_mode="asymmetric", nearest_mode="floor"),
]
graph = helper.make_graph(
    nodes, "shapeprop_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1, 4, 4])],
    outputs=[helper.make_tensor_value_info("y2", TensorProto.FLOAT, [1, 1, 8, 8])],
    initializer=[
        numpy_helper.from_array(w, name="w"),
        numpy_helper.from_array(b, name="b"),
        numpy_helper.from_array(np.array([0], dtype=np.int64), name="st0"),
        numpy_helper.from_array(np.array([2], dtype=np.int64), name="en0"),
        numpy_helper.from_array(np.array([0], dtype=np.int64), name="ax0"),
        numpy_helper.from_array(np.array([2], dtype=np.int64), name="st1"),
        numpy_helper.from_array(np.array([4], dtype=np.int64), name="en1"),
        numpy_helper.from_array(np.array([0], dtype=np.int64), name="ax1"),
        numpy_helper.from_array(two, name="two"),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
x.tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y2 = sess.run(None, {"x": x})[0]
y2.tofile(ORT_BIN)
print("wrote", OUT, "y2.shape", y2.shape, "y2[0,0,0,0]", y2[0, 0, 0, 0])
