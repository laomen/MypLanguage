#!/usr/bin/env python3
"""F8：生成合成 ONNX（Split 多输出：axis=1，split=[1,3]）+ ORT 交叉校验。

用法（仓库根，venv 解释器）:
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_split_onnx.py
  生成:
    deeplearning/data/onnx/split_test.onnx
    deeplearning/data/onnx/split_test.f32
    deeplearning/data/onnx/split_test_ort0.bin / split_test_ort1.bin

模型（opset13）:
  x[1,4,8,8] → Split(axis=1, split=[1,3]) → y0[1,1,8,8], y1[1,3,8,8]
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/split_test.onnx"
IN_F32 = "deeplearning/data/onnx/split_test.f32"
ORT0 = "deeplearning/data/onnx/split_test_ort0.bin"
ORT1 = "deeplearning/data/onnx/split_test_ort1.bin"

np.random.seed(71)
x = np.random.randn(1, 4, 8, 8).astype(np.float32)

nodes = [
    # opset13+：split 为输入[1]（int64 初始器）；axis 为属性
    helper.make_node("Split", ["x", "split"], ["y0", "y1"], axis=1, name="sp"),
]

graph = helper.make_graph(
    nodes, "split_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 4, 8, 8])],
    outputs=[
        helper.make_tensor_value_info("y0", TensorProto.FLOAT, [1, 1, 8, 8]),
        helper.make_tensor_value_info("y1", TensorProto.FLOAT, [1, 3, 8, 8]),
    ],
    initializer=[numpy_helper.from_array(np.array([1, 3], dtype=np.int64), name="split")],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 8
onnx.checker.check_model(model)
onnx.save(model, OUT)
print("wrote", OUT)

import onnxruntime as ort
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
res = sess.run(["y0", "y1"], {"x": x})
print("ORT y0:", res[0].shape, " y1:", res[1].shape)

with open(IN_F32, "wb") as f:
    f.write(x.tobytes())
with open(ORT0, "wb") as f:
    f.write(res[0].astype(np.float32).tobytes())
with open(ORT1, "wb") as f:
    f.write(res[1].astype(np.float32).tobytes())
print("wrote", IN_F32, ORT0, ORT1)
