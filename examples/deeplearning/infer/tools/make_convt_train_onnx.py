#!/usr/bin/env python3
"""make_convt_train_onnx.py — 生成小型 ConvTranspose 分类 ONNX（阶段4d 训练夹具）。

结构：ConvTranspose(1→2,k2,s2) → ReLU → Flatten(128) → Gemm(128→2) → Softmax。
输入 [1,1,4,4]（NCHW），2 分类。权重随机小初始化（训练入口从零训练）。

用法（须在 examples/ 下）：
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_convt_train_onnx.py
"""
import os
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/convt_train.onnx"
np.random.seed(21)


def w(name, shape, scale=0.1):
    return helper.make_tensor(name, TensorProto.FLOAT, list(shape),
                              (np.random.randn(*shape) * scale).astype(np.float32).flatten().tolist())


# ConvTranspose 权重 [Cin, Cout, kh, kw]（ONNX 布局）
nodes = [
    helper.make_node("ConvTranspose", ["data", "ct_w", "ct_b"], ["c1"],
                     kernel_shape=[2, 2], strides=[2, 2], pads=[0, 0, 0, 0], name="ct1"),
    helper.make_node("Relu", ["c1"], ["r1"], name="relu1"),
    helper.make_node("Flatten", ["r1"], ["f1"], name="flat1"),
    helper.make_node("Gemm", ["f1", "gemm_w", "gemm_b"], ["logits"],
                     transB=1, name="gemm1"),
    helper.make_node("Softmax", ["logits"], ["prob"], axis=1, name="softmax1"),
]

inits = [
    w("ct_w", [1, 2, 2, 2], 0.1), w("ct_b", [2], 0.02),
    w("gemm_w", [2, 128], 0.1), w("gemm_b", [2], 0.02),
]

inputs = [helper.make_tensor_value_info("data", TensorProto.FLOAT, [1, 1, 4, 4])]
outputs = [helper.make_tensor_value_info("prob", TensorProto.FLOAT, [1, 2])]

graph = helper.make_graph(nodes, "convt_train", inputs, outputs, initializer=inits)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 7
onnx.checker.check_model(model)
onnx.save(model, OUT)
print("wrote", OUT)
