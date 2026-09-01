#!/usr/bin/env python3
"""生成 P2 identity-fold 夹具：data -> Add(0) -> Mul(1) -> Softmax。
优化后 Add/Mul 均被删除，只保留 Softmax；输出须与 numpy 一致。
"""
import os
import numpy as np
import onnx
from onnx import TensorProto, helper

out = "deeplearning/data/onnx/identity_fold.onnx"
os.makedirs(os.path.dirname(out), exist_ok=True)
zero = helper.make_tensor("zero", TensorProto.FLOAT, [], [0.0])
one = helper.make_tensor("one", TensorProto.FLOAT, [], [1.0])
nodes = [
    helper.make_node("Add", ["data", "zero"], ["add0"], name="add_zero"),
    helper.make_node("Mul", ["add0", "one"], ["mul1"], name="mul_one"),
    helper.make_node("Softmax", ["mul1"], ["prob"], axis=1, name="softmax"),
]
graph = helper.make_graph(
    nodes,
    "identity_fold",
    [helper.make_tensor_value_info("data", TensorProto.FLOAT, [1, 3])],
    [helper.make_tensor_value_info("prob", TensorProto.FLOAT, [1, 3])],
    [zero, one],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 7
onnx.checker.check_model(model)
onnx.save(model, out)
print("wrote", out)
