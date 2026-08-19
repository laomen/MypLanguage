#!/usr/bin/env python3
"""3D：生成合成 3D ONNX（Conv3D → InstanceNorm → ReLU → MaxPool3D）+ ORT 交叉校验。

用法（仓库根，venv 解释器）:
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_conv3d_onnx.py
  生成:
    deeplearning/data/onnx/conv3d_test.onnx
    deeplearning/data/onnx/conv3d_test.f32
    deeplearning/data/onnx/conv3d_test_ort.bin

模型（opset13）:
  x[1,2,8,8,8] → Conv3D(weight[3,2,3,3,3], stride=1, pad=1) → [1,3,8,8,8]
              → InstanceNorm(scale[3],bias[3]) → [1,3,8,8,8]
              → ReLU → [1,3,8,8,8]
              → MaxPool3D(kernel=2, stride=2) → y[1,3,4,4,4]
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/conv3d_test.onnx"
IN_F32 = "deeplearning/data/onnx/conv3d_test.f32"
ORT_BIN = "deeplearning/data/onnx/conv3d_test_ort.bin"

np.random.seed(42)
x = np.random.randn(1, 2, 8, 8, 8).astype(np.float32)
w = (np.random.randn(3, 2, 3, 3, 3) * 0.2).astype(np.float32)
b = np.array([0.1, -0.2, 0.05], dtype=np.float32)
scale = np.array([1.4, 0.9, 1.1], dtype=np.float32)
bias = np.array([0.2, -0.1, 0.15], dtype=np.float32)

nodes = [
    helper.make_node("Conv", ["x", "w3d", "b3d"], ["c"], strides=[1, 1, 1], pads=[1, 1, 1, 1, 1, 1], name="conv3d"),
    helper.make_node("InstanceNormalization", ["c", "in_scale", "in_bias"], ["n"], epsilon=1e-5, name="in3d"),
    helper.make_node("Relu", ["n"], ["r"], name="relu3d"),
    helper.make_node("MaxPool", ["r"], ["y"], kernel_shape=[2, 2, 2], strides=[2, 2, 2], name="pool3d"),
]

graph = helper.make_graph(
    nodes, "conv3d_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 8, 8, 8])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 3, 4, 4, 4])],
    initializer=[
        numpy_helper.from_array(w, name="w3d"),
        numpy_helper.from_array(b, name="b3d"),
        numpy_helper.from_array(scale, name="in_scale"),
        numpy_helper.from_array(bias, name="in_bias"),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 8
onnx.checker.check_model(model)
onnx.save(model, OUT)
print("wrote", OUT)

import onnxruntime as ort
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(["y"], {"x": x})[0]
y = sess.run(["y"], {"x": x})[0]
print("ORT y shape:", y.shape, "sum:", float(y.sum()))

with open(IN_F32, "wb") as f:
    f.write(x.tobytes())
with open(ORT_BIN, "wb") as f:
    f.write(y.astype(np.float32).tobytes())
print("wrote", IN_F32, "and", ORT_BIN)
