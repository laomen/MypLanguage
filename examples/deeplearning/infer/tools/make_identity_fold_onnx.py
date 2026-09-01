#!/usr/bin/env python3
"""生成 P2 identity-fold + 图输出 rewrite 夹具：
  data  -> Add(0) -> Mul(1) -> Softmax -> prob   （P2a 链式恒等折叠，3 ops→1）
  data2 -> Add(zero2) -> out2                    （P2d 图输出 rewrite：out2 被折叠，
                                                   图输出名重命名为 data2，tensorId("out2") 消失）
优化后运行时只保留 Softmax 一个 op；两个输出均须与输入逐位一致。
"""
import os
import numpy as np
import onnx
from onnx import TensorProto, helper

out = "deeplearning/data/onnx/identity_fold.onnx"
os.makedirs(os.path.dirname(out), exist_ok=True)
zero = helper.make_tensor("zero", TensorProto.FLOAT, [], [0.0])
one = helper.make_tensor("one", TensorProto.FLOAT, [], [1.0])
zero2 = helper.make_tensor("zero2", TensorProto.FLOAT, [], [0.0])
nodes = [
    helper.make_node("Add", ["data", "zero"], ["add0"], name="add_zero"),
    helper.make_node("Mul", ["add0", "one"], ["mul1"], name="mul_one"),
    helper.make_node("Softmax", ["mul1"], ["prob"], axis=1, name="softmax"),
    helper.make_node("Add", ["data2", "zero2"], ["out2"], name="add_out2"),
]
graph = helper.make_graph(
    nodes,
    "identity_fold",
    [
        helper.make_tensor_value_info("data", TensorProto.FLOAT, [1, 3]),
        helper.make_tensor_value_info("data2", TensorProto.FLOAT, [1, 2]),
    ],
    [
        helper.make_tensor_value_info("prob", TensorProto.FLOAT, [1, 3]),
        helper.make_tensor_value_info("out2", TensorProto.FLOAT, [1, 2]),
    ],
    [zero, one, zero2],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 7
onnx.checker.check_model(model)
onnx.save(model, out)
print("wrote", out)
