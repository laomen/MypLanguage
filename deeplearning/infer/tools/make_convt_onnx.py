#!/usr/bin/env python3
"""F8：生成合成 ONNX（ConvTranspose，strides=2/pads/output_padding/bias）+ ORT 交叉校验。

用法（仓库根，venv 解释器）:
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_convt_onnx.py
  生成:
    deeplearning/data/onnx/convt_test.onnx
    deeplearning/data/onnx/convt_test.f32
    deeplearning/data/onnx/convt_test_ort.bin

模型（opset13）:
  x[1,2,4,4] → ConvTranspose(W[2,3,3,3], b[3], strides=[2,2], pads=[1,1,1,1],
               output_padding=[1,1]) → y[1,3,8,8]
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/convt_test.onnx"
IN_F32 = "deeplearning/data/onnx/convt_test.f32"
ORT_BIN = "deeplearning/data/onnx/convt_test_ort.bin"

np.random.seed(61)
x = np.random.randn(1, 2, 4, 4).astype(np.float32)
w = np.random.randn(2, 3, 3, 3).astype(np.float32)   # [Cin, Cout, kh, kw]
b = np.random.randn(3).astype(np.float32)

nodes = [
    helper.make_node("ConvTranspose", ["x", "w", "b"], ["y"],
                     kernel_shape=[3, 3], strides=[2, 2], pads=[1, 1, 1, 1],
                     output_padding=[1, 1], name="convt"),
]

graph = helper.make_graph(
    nodes, "convt_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 4, 4])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 3, 8, 8])],
    initializer=[
        numpy_helper.from_array(w, name="w"),
        numpy_helper.from_array(b, name="b"),
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
print("ORT y shape:", y.shape, "sum:", float(y.sum()))

with open(IN_F32, "wb") as f:
    f.write(x.tobytes())
with open(ORT_BIN, "wb") as f:
    f.write(y.astype(np.float32).tobytes())
print("wrote", IN_F32, "and", ORT_BIN)
