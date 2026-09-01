#!/usr/bin/env python3
"""阶段三：动态 batch 输入 shape 注入 + 编译期 specialization 合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_dynbatch_onnx.py
生成:
  deeplearning/data/onnx/dynbatch_test.onnx
  deeplearning/data/onnx/dynbatch_in.f32
  deeplearning/data/onnx/dynbatch_test_ort.bin

模型（opset13，batch 动态 dim_param）:
  x[N,1,5,5] → Conv(W[1,1,3,3], B[1]) → y[N,1,3,3]
  MYP 在加载前 setInputShape 注入 batch=2 → 编译期 specialization → 输出 [2,1,3,3]。
  ORT 参考用 batch=2 输入生成。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/dynbatch_test.onnx"
IN_F32 = "deeplearning/data/onnx/dynbatch_in.f32"
ORT_BIN = "deeplearning/data/onnx/dynbatch_test_ort.bin"

np.random.seed(60)
B = 2
x = np.random.randn(B, 1, 5, 5).astype(np.float32)
w = np.random.randn(1, 1, 3, 3).astype(np.float32)
b = np.random.randn(1).astype(np.float32)

# batch 动态（dim_param "N"）
x_info = helper.make_tensor_value_info("x", TensorProto.FLOAT, [None, 1, 5, 5])
y_info = helper.make_tensor_value_info("y", TensorProto.FLOAT, [None, 1, 3, 3])

nodes = [
    helper.make_node("Conv", ["x", "w", "b"], ["y"], kernel_shape=[3, 3], pads=[0, 0, 0, 0], strides=[1, 1]),
]
graph = helper.make_graph(
    nodes, "dynbatch_test",
    inputs=[x_info],
    outputs=[y_info],
    initializer=[
        helper.make_tensor("w", TensorProto.FLOAT, [1, 1, 3, 3], w.flatten().tolist()),
        helper.make_tensor("b", TensorProto.FLOAT, [1], b.flatten().tolist()),
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
print("wrote", OUT, "batch=", B, "y.shape", y.shape, "y[0,0,0,0]", y[0, 0, 0, 0], "y[1,0,2,2]", y[1, 0, 2, 2])
