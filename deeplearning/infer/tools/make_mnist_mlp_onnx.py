#!/usr/bin/env python3
"""make_mnist_mlp_onnx.py — 用现有 MNIST 权重导出真实 MLP ONNX 模型（测试夹具生成器，开发期工具）。

从 deeplearning/data/mnist_weights.bin（train_mnist.myp 导出的权重）构建
标准 MLP ONNX：输入 [1,784] -> Gemm -> Relu -> Gemm -> Softmax(axis=1) -> [1,10]。
生成 deeplearning/data/onnx/mnist_mlp.onnx，供纯 MYP ONNX 读取器（Path B）验证。

用法：
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_mnist_mlp_onnx.py
"""
import os
import struct
import numpy as np
import onnx
from onnx import helper, TensorProto

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
BIN = os.path.join(ROOT, "deeplearning", "data", "mnist_weights.bin")
OUT = os.path.join(ROOT, "deeplearning", "data", "onnx", "mnist_mlp.onnx")

N_IN, N_HID, N_OUT = 784, 64, 10

# ---- 读取权重：头 3 个 int32 大端，随后 wh/wo/bh 为原生(小端) double ----
with open(BIN, "rb") as f:
    c1, c2, c3 = struct.unpack(">iii", f.read(12))
    assert (c1, c2, c3) == (N_IN * N_HID, N_HID * N_OUT, N_HID + N_OUT), (c1, c2, c3)
    wh = np.array(struct.unpack("<%dd" % (N_IN * N_HID), f.read(8 * N_IN * N_HID)), dtype=np.float64)
    wo = np.array(struct.unpack("<%dd" % (N_HID * N_OUT), f.read(8 * N_HID * N_OUT)), dtype=np.float64)
    bh = np.array(struct.unpack("<%dd" % (N_HID + N_OUT), f.read(8 * (N_HID + N_OUT))), dtype=np.float64)

# wh: [64, 784]（hidden×input）；Gemm transB=1 时 B 按 [N,K] 存储（PyTorch Linear 约定），
# 所以直接用 wh 原样（不转置）：wire W1 = [64, 784]
W1 = wh.reshape(N_HID, N_IN).astype(np.float32)             # [64, 784]
B1 = bh[:N_HID].astype(np.float32)                          # [64]
# wo: [10, 64]；同样按 [N,K] 存储：wire W2 = [10, 64]
W2 = wo.reshape(N_OUT, N_HID).astype(np.float32)            # [10, 64]
B2 = bh[N_HID:].astype(np.float32)                          # [10]

nodes = [
    helper.make_node("Gemm", ["data", "W1", "B1"], ["h"], transB=1, name="gemm1"),
    helper.make_node("Relu", ["h"], ["h_relu"], name="relu1"),
    helper.make_node("Gemm", ["h_relu", "W2", "B2"], ["logits"], transB=1, name="gemm2"),
    helper.make_node("Softmax", ["logits"], ["prob"], axis=1, name="softmax"),
]
initializers = [
    helper.make_tensor("W1", TensorProto.FLOAT, [N_HID, N_IN], W1.flatten().tolist()),
    helper.make_tensor("B1", TensorProto.FLOAT, [N_HID], B1.flatten().tolist()),
    helper.make_tensor("W2", TensorProto.FLOAT, [N_OUT, N_HID], W2.flatten().tolist()),
    helper.make_tensor("B2", TensorProto.FLOAT, [N_OUT], B2.flatten().tolist()),
]
inputs = [helper.make_tensor_value_info("data", TensorProto.FLOAT, [1, N_IN])]
outputs = [helper.make_tensor_value_info("prob", TensorProto.FLOAT, [1, N_OUT])]

graph = helper.make_graph(nodes, "mnist_mlp", inputs, outputs, initializers)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 8
onnx.checker.check_model(model)
onnx.save(model, OUT)
print("saved:", OUT)
print("ir_version:", model.ir_version, "opset:", model.opset_import[0].version)
print("nodes:", [n.op_type for n in graph.node])
print("initializers:", [(i.name, list(i.dims)) for i in graph.initializer])
