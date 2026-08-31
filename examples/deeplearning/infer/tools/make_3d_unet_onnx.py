#!/usr/bin/env python3
"""make_3d_unet_onnx.py — 生成 3D U-Net 编码-解码 ONNX（阶段4d Dice 训练夹具）。

结构（U-Net 上采样路径，无跳跃连接；跳跃连接需 5D Concat 反向，留后续）：
  输入 data [1,1,8,8,8]（NCDHW）
  编码：
    e1: Conv3D(1→8,k3,p1) → ReLU
    e2: Conv3D(8→8,k3,p1)  → ReLU
    p1: MaxPool3D(k2,s2)    → [1,8,4,4,4]
    e3: Conv3D(8→16,k3,p1)  → ReLU
    e4: Conv3D(16→16,k3,p1) → ReLU
    p2: MaxPool3D(k2,s2)    → [1,16,2,2,2]
  解码：
    u1: Resize3D 2→4 (linear, asymmetric)
    d1: Conv3D(16→16,k3,p1) → ReLU
    u2: Resize3D 4→8 (linear, asymmetric)
    d2: Conv3D(16→8,k3,p1)  → ReLU
    d3: Conv3D(8→2,k3,p1)   → Softmax(prob) [1,2,8,8,8]

权重随机小初始化（训练入口从零训练；验证 Resize3D 反向端到端）。

用法（须在 examples/ 下）：
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_3d_unet_onnx.py
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

OUT = "deeplearning/data/onnx/3d_unet.onnx"
np.random.seed(77)


def w(name, shape, scale=0.1):
    return helper.make_tensor(name, TensorProto.FLOAT, list(shape),
                              (np.random.randn(*shape) * scale).astype(np.float32).flatten().tolist())


def b(name, n, scale=0.02):
    return helper.make_tensor(name, TensorProto.FLOAT, [n],
                              (np.random.randn(n) * scale).astype(np.float32).flatten().tolist())


def sz(name, vals):
    return helper.make_tensor(name, TensorProto.INT64, [len(vals)], [int(v) for v in vals])


nodes = [
    # 编码
    helper.make_node("Conv", ["data", "e1_w", "e1_b"], ["e1"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="enc_conv1"),
    helper.make_node("Relu", ["e1"], ["r1"], name="enc_relu1"),
    helper.make_node("Conv", ["r1", "e2_w", "e2_b"], ["e2"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="enc_conv2"),
    helper.make_node("Relu", ["e2"], ["r2"], name="enc_relu2"),
    helper.make_node("MaxPool", ["r2"], ["p1"],
                     kernel_shape=[2, 2, 2], strides=[2, 2, 2], name="enc_pool1"),
    helper.make_node("Conv", ["p1", "e3_w", "e3_b"], ["e3"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="enc_conv3"),
    helper.make_node("Relu", ["e3"], ["r3"], name="enc_relu3"),
    helper.make_node("Conv", ["r3", "e4_w", "e4_b"], ["e4"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="enc_conv4"),
    helper.make_node("Relu", ["e4"], ["r4"], name="enc_relu4"),
    helper.make_node("MaxPool", ["r4"], ["p2"],
                     kernel_shape=[2, 2, 2], strides=[2, 2, 2], name="enc_pool2"),
    # 解码（上采样）
    helper.make_node("Resize", ["p2", "", "", "u1_sz"], ["u1"],
                     mode="linear", coordinate_transformation_mode="asymmetric", name="dec_up1"),
    helper.make_node("Conv", ["u1", "d1_w", "d1_b"], ["d1"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="dec_conv1"),
    helper.make_node("Relu", ["d1"], ["rd1"], name="dec_relu1"),
    helper.make_node("Resize", ["rd1", "", "", "u2_sz"], ["u2"],
                     mode="linear", coordinate_transformation_mode="asymmetric", name="dec_up2"),
    helper.make_node("Conv", ["u2", "d2_w", "d2_b"], ["d2"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="dec_conv2"),
    helper.make_node("Relu", ["d2"], ["rd2"], name="dec_relu2"),
    helper.make_node("Conv", ["rd2", "d3_w", "d3_b"], ["d3"],
                     kernel_shape=[3, 3, 3], pads=[1, 1, 1, 1, 1, 1], strides=[1, 1, 1], name="dec_conv3"),
    helper.make_node("Softmax", ["d3"], ["prob"], axis=1, name="softmax1"),
]

inits = [
    w("e1_w", [8, 1, 3, 3, 3], 0.1), b("e1_b", 8, 0.02),
    w("e2_w", [8, 8, 3, 3, 3], 0.1), b("e2_b", 8, 0.02),
    w("e3_w", [16, 8, 3, 3, 3], 0.1), b("e3_b", 16, 0.02),
    w("e4_w", [16, 16, 3, 3, 3], 0.1), b("e4_b", 16, 0.02),
    w("d1_w", [16, 16, 3, 3, 3], 0.1), b("d1_b", 16, 0.02),
    w("d2_w", [8, 16, 3, 3, 3], 0.1), b("d2_b", 8, 0.02),
    w("d3_w", [2, 8, 3, 3, 3], 0.1), b("d3_b", 2, 0.02),
    sz("u1_sz", [1, 16, 4, 4, 4]),
    sz("u2_sz", [1, 16, 8, 8, 8]),
]

inputs = [helper.make_tensor_value_info("data", TensorProto.FLOAT, [1, 1, 8, 8, 8])]
outputs = [helper.make_tensor_value_info("prob", TensorProto.FLOAT, [1, 2, 8, 8, 8])]

graph = helper.make_graph(nodes, "3d_unet", inputs, outputs, initializer=inits)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 7
onnx.checker.check_model(model)
onnx.save(model, OUT)
print("wrote", OUT)
