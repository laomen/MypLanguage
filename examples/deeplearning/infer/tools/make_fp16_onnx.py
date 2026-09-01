#!/usr/bin/env python3
"""阶段三：FP16 权重 dtype 转换（ONNX FLOAT16 → MYP float32）合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_fp16_onnx.py
生成:
  deeplearning/data/onnx/fp16_test.onnx
  deeplearning/data/onnx/fp16_in.f32
  deeplearning/data/onnx/fp16_test_ort.bin

模型（opset13）: x[1,1,5,5] → Conv(W[1,1,3,3] FLOAT16, B[1] FLOAT16) → y[1,1,3,3]
  （权重/偏置存 FP16；MYP loader 读 float16_data 转 float32）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/fp16_test.onnx"
IN_F32 = "deeplearning/data/onnx/fp16_in.f32"
ORT_BIN = "deeplearning/data/onnx/fp16_test_ort.bin"

np.random.seed(59)
x = np.random.randn(1, 1, 5, 5).astype(np.float32)
w = np.random.randn(1, 1, 3, 3).astype(np.float32)
b = np.random.randn(1).astype(np.float32)

nodes = [
    helper.make_node("Conv", ["x", "w", "b"], ["y"], kernel_shape=[3, 3], pads=[0, 0, 0, 0], strides=[1, 1]),
]
graph = helper.make_graph(
    nodes, "fp16_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1, 5, 5])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 1, 3, 3])],
    initializer=[
        numpy_helper.from_array(w.astype(np.float16), name="w"),   # FP16 权重
        numpy_helper.from_array(b.astype(np.float16), name="b"),   # FP16 偏置
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

# 确认 initializer 用了 float16_data(6) 还是 raw_data(9)
g = onnx.load(OUT).graph
print("w field check:", "float16_data" if g.initializer[0].float16_data else "raw_data/other")

import onnxruntime as ort
x.tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
y = sess.run(None, {"x": x})[0]
y.tofile(ORT_BIN)
print("wrote", OUT, "y.shape", y.shape, "y[0,0,0,0]", y[0, 0, 0, 0], "y[0,0,2,2]", y[0, 0, 2, 2])
