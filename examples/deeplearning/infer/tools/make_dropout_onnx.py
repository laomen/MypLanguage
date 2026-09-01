#!/usr/bin/env python3
"""G5：Dropout（推理恒等）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_dropout_onnx.py
生成:
  deeplearning/data/onnx/dropout_test.onnx
  deeplearning/data/onnx/dropout_in.f32
  deeplearning/data/onnx/dropout_test_ort.bin

模型（opset12，ratio 输入初始器，training_mode 省略 → 推理恒等）:
  x[1,2,3,4] → Dropout(ratio=0.5) → y[1,2,3,4] = x（推理模式下 Dropout 是 identity）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/dropout_test.onnx"
IN_F32 = "deeplearning/data/onnx/dropout_in.f32"
ORT_BIN = "deeplearning/data/onnx/dropout_test_ort.bin"

np.random.seed(57)
x = np.random.randn(1, 2, 3, 4).astype(np.float32)
ratio = np.array(0.5, dtype=np.float32)

nodes = [
    helper.make_node("Dropout", ["x", "ratio"], ["y"], name="dp1"),
]
graph = helper.make_graph(
    nodes, "dropout_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 3, 4])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 2, 3, 4])],
    initializer=[numpy_helper.from_array(ratio, name="ratio")],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 12)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
x.tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(None, {"x": x})[0]
y.tofile(ORT_BIN)
print("wrote", OUT, "y.shape", y.shape, "y[0,0,0,0]", y[0, 0, 0, 0], "y[0,1,2,3]", y[0, 1, 2, 3])
