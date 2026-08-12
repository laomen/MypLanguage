#!/usr/bin/env python3
"""F8：生成合成 ONNX（Pad：constant/reflect/edge + constant_value）+ ORT 交叉校验。

用法（仓库根，venv 解释器）:
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_pad_onnx.py
  生成:
    deeplearning/data/onnx/pad_test.onnx
    deeplearning/data/onnx/pad_test.f32
    deeplearning/data/onnx/pad_test_ort.bin

模型（opset13）:
  x[1,1,5,5] → Pad(pads=[0,0,1,1,0,0,1,1], mode=constant, value=0.5) → [1,1,7,7]
             → Pad(pads=[0,0,1,2,0,0,1,2], mode=edge)                  → [1,1,9,10]
             → Pad(pads=[0,0,1,1,0,0,1,1], mode=reflect)               → [1,1,11,13]
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/pad_test.onnx"
IN_F32 = "deeplearning/data/onnx/pad_test.f32"
ORT_BIN = "deeplearning/data/onnx/pad_test_ort.bin"

np.random.seed(51)
x = np.random.randn(1, 1, 5, 5).astype(np.float32)

def i64(name, arr):
    return numpy_helper.from_array(np.array(arr, dtype=np.int64), name=name)

nodes = [
    helper.make_node("Pad", ["x", "p1_pads", "p1_val"], ["a"], mode="constant", name="pad1"),
    helper.make_node("Pad", ["a", "p2_pads"], ["b"], mode="edge", name="pad2"),
    helper.make_node("Pad", ["b", "p3_pads"], ["y"], mode="reflect", name="pad3"),
]

graph = helper.make_graph(
    nodes, "pad_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1, 5, 5])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1, 11, 13])],
    initializer=[
        i64("p1_pads", [0, 0, 1, 1, 0, 0, 1, 1]),
        numpy_helper.from_array(np.array(0.5, dtype=np.float32), name="p1_val"),
        i64("p2_pads", [0, 0, 1, 2, 0, 0, 1, 2]),
        i64("p3_pads", [0, 0, 1, 1, 0, 0, 1, 1]),
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
