#!/usr/bin/env python3
"""生成 P2 identity-fold + 图输出 rewrite + 连续 Relu 合并夹具：
  data  -> Add(0) -> Mul(1) -> Softmax -> prob      （P2a 链式恒等折叠，3 ops→1）
  data2 -> Add(zero2) -> out2                        （P2d 图输出 rewrite → data2）
  data3 -> Relu -> relu3a -> Relu -> out3            （P2e 连续 Relu 合并 → 1 Relu，out3→relu3a）
  data4 -> Sub(zero4) -> sub4 -> Div(one4) -> out4   （P2e Sub(x,0)/Div(x,1) 折叠 → data4）
优化后运行时只保留 Softmax + 1 个 Relu（共 2 ops）；图输出 prob/data2/relu3a/data4。
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
zero4 = helper.make_tensor("zero4", TensorProto.FLOAT, [], [0.0])
one4 = helper.make_tensor("one4", TensorProto.FLOAT, [], [1.0])
nodes = [
    helper.make_node("Add", ["data", "zero"], ["add0"], name="add_zero"),
    helper.make_node("Mul", ["add0", "one"], ["mul1"], name="mul_one"),
    helper.make_node("Softmax", ["mul1"], ["prob"], axis=1, name="softmax"),
    helper.make_node("Add", ["data2", "zero2"], ["out2"], name="add_out2"),
    helper.make_node("Relu", ["data3"], ["relu3a"], name="relu_inner"),
    helper.make_node("Relu", ["relu3a"], ["out3"], name="relu_outer"),
    helper.make_node("Sub", ["data4", "zero4"], ["sub4"], name="sub_zero"),
    helper.make_node("Div", ["sub4", "one4"], ["out4"], name="div_one"),
]
graph = helper.make_graph(
    nodes,
    "identity_fold",
    [
        helper.make_tensor_value_info("data", TensorProto.FLOAT, [1, 3]),
        helper.make_tensor_value_info("data2", TensorProto.FLOAT, [1, 2]),
        helper.make_tensor_value_info("data3", TensorProto.FLOAT, [1, 4]),
        helper.make_tensor_value_info("data4", TensorProto.FLOAT, [1, 2]),
    ],
    [
        helper.make_tensor_value_info("prob", TensorProto.FLOAT, [1, 3]),
        helper.make_tensor_value_info("out2", TensorProto.FLOAT, [1, 2]),
        helper.make_tensor_value_info("out3", TensorProto.FLOAT, [1, 4]),
        helper.make_tensor_value_info("out4", TensorProto.FLOAT, [1, 2]),
    ],
    [zero, one, zero2, zero4, one4],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 7
onnx.checker.check_model(model)
onnx.save(model, out)
print("wrote", out)
