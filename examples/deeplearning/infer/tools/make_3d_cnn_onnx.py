#!/usr/bin/env python3
"""make_3d_cnn_onnx.py — 生成小型 3D CNN ONNX（8³ 合成 3D 训练夹具，阶段4）。

结构：Conv3D(1→4,k3,p1) → ReLU → MaxPool3D(k2,s2) → Flatten(256) → Gemm(256→2)
      → Softmax。输入 [1,1,8,8,8]（NCDHW），2 分类。
权重随机小初始化（训练入口从零训练，初始值无所谓）。

用法：
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_3d_cnn_onnx.py
"""
import os
import numpy as np
import onnx
from onnx import helper, TensorProto

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
OUT = os.path.join(ROOT, "deeplearning", "data", "onnx", "3d_cnn.onnx")
np.random.seed(11)


def w(name, shape, scale=0.1):
    return helper.make_tensor(name, TensorProto.FLOAT, list(shape),
                              (np.random.randn(*shape) * scale).astype(np.float32).flatten().tolist())


nodes = [
    helper.make_node("Conv", ["data", "conv1_w", "conv1_b"], ["c1"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="conv1"),
    helper.make_node("Relu", ["c1"], ["r1"], name="relu1"),
    helper.make_node("MaxPool", ["r1"], ["p1"], kernel_shape=[2, 2, 2], strides=[2, 2, 2], name="pool1"),
    helper.make_node("Flatten", ["p1"], ["f"], name="flatten"),
    helper.make_node("Gemm", ["f", "fc_w", "fc_b"], ["logits"], transB=1, name="fc"),
    helper.make_node("Softmax", ["logits"], ["prob"], axis=1, name="softmax"),
]
initializers = [
    w("conv1_w", [4, 1, 3, 3, 3], 0.2), w("conv1_b", [4], 0.1),
    w("fc_w", [2, 256], 0.1), w("fc_b", [2], 0.1),
]
inputs = [helper.make_tensor_value_info("data", TensorProto.FLOAT, [1, 1, 8, 8, 8])]
outputs = [helper.make_tensor_value_info("prob", TensorProto.FLOAT, [1, 2])]

graph = helper.make_graph(nodes, "3d_cnn", inputs, outputs, initializers)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 8
onnx.checker.check_model(model)
onnx.save(model, OUT)
print("saved:", OUT)
print("nodes:", [n.op_type for n in graph.node])
print("initializers:", [(i.name, list(i.dims)) for i in graph.initializer])
