#!/usr/bin/env python3
"""阶段七：多输入 / 多输出 / 可选输入完整处理 合成 ONNX + ORT 交叉校验。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_multiio_onnx.py
生成:
  deeplearning/data/onnx/multiio_test.onnx
  deeplearning/data/onnx/multiio_in_x1.f32 / multiio_in_x2.f32
  deeplearning/data/onnx/multiio_test_ort_y1.bin / multiio_test_ort_y2.bin

模型（opset13，多输入多输出不同形状 + 可选输入空槽）:
  x1[1,3,8,8] → Conv(k=3,pad=1, 无 bias——B 槽缺省) → c1 → Relu → y1[1,4,8,8]
  x2[1,4]     → Gemm(W2[3,4] transB=1, B2[3]) → y2[1,3]
其中 Conv 的 B（bias）为可选输入省略；Relu 无参数。两输出形状/大小不同
（y1=256 元素，y2=3 元素），验证框架按名对多输入注入、多输出各自读取。
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/multiio_test.onnx"

np.random.seed(72)
x1 = np.random.randn(1, 3, 8, 8).astype(np.float32)
W1 = np.random.randn(4, 3, 3, 3).astype(np.float32) * 0.3
x2 = np.random.randn(1, 4).astype(np.float32)
W2 = np.random.randn(3, 4).astype(np.float32)
B2 = np.random.randn(3).astype(np.float32) * 0.1

nodes = [
    # Conv 无 bias：B（第 2 输入槽）可选省略
    helper.make_node("Conv", ["x1", "W1"], ["c1"],
                     name="conv1", kernel_shape=[3, 3], pads=[1, 1, 1, 1]),
    helper.make_node("Relu", ["c1"], ["y1"], name="relu1"),
    helper.make_node("Gemm", ["x2", "W2", "B2"], ["y2"],
                     name="gemm1", transB=1, alpha=1.0, beta=1.0),
]
graph = helper.make_graph(
    nodes, "multiio_test",
    inputs=[
        helper.make_tensor_value_info("x1", TensorProto.FLOAT, [1, 3, 8, 8]),
        helper.make_tensor_value_info("x2", TensorProto.FLOAT, [1, 4]),
    ],
    outputs=[
        helper.make_tensor_value_info("y1", TensorProto.FLOAT, [1, 4, 8, 8]),
        helper.make_tensor_value_info("y2", TensorProto.FLOAT, [1, 3]),
    ],
    initializer=[
        helper.make_tensor("W1", TensorProto.FLOAT, [4, 3, 3, 3], W1.flatten()),
        helper.make_tensor("W2", TensorProto.FLOAT, [3, 4], W2.flatten()),
        helper.make_tensor("B2", TensorProto.FLOAT, [3], B2.flatten()),
    ],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

import onnxruntime as ort
feed = {"x1": x1, "x2": x2}
x1.tofile("deeplearning/data/onnx/multiio_in_x1.f32")
x2.tofile("deeplearning/data/onnx/multiio_in_x2.f32")
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
outs = sess.run(None, feed)
y1, y2 = outs[0], outs[1]
y1.flatten().astype(np.float32).tofile("deeplearning/data/onnx/multiio_test_ort_y1.bin")
y2.flatten().astype(np.float32).tofile("deeplearning/data/onnx/multiio_test_ort_y2.bin")
print("wrote", OUT)
print("y1.shape", y1.shape, "y2.shape", y2.shape)
print("y1[0,0,0,0]", y1[0, 0, 0, 0], "y2[0,0]", y2[0, 0])
