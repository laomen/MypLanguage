#!/usr/bin/env python3
"""阶段七：Unsupported operator 诊断合成模型（含控制流子图 If）。

用法（examples 目录）:
  ../examples/deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_unsup_onnx.py
生成:
  deeplearning/data/onnx/unsup_if.onnx     —— 含 If（控制流子图）→ 应诊断 "control-flow"
  deeplearning/data/onnx/unsup_unknown.onnx —— 含 FakeOp（普通未实现）→ 应诊断 "not implemented"

模型（opset13）：
  If 模型: x[1] → If(cond=x[0]>0) then y=2x else y=3x → y[1]
  FakeOp 模型: x[1] → FakeOp → y[1]（FakeOp 是 ONNX 未知算子，但 checker 允许）
"""
import numpy as np
import onnx
from onnx import helper, TensorProto

# ---- If 控制流模型 ----
then_g = helper.make_graph(
    [helper.make_node("Identity", ["x"], ["y_then"])], "then",
    [], [helper.make_tensor_value_info("y_then", TensorProto.FLOAT, [1])])
else_g = helper.make_graph(
    [helper.make_node("Identity", ["x"], ["y_else"])], "else",
    [], [helper.make_tensor_value_info("y_else", TensorProto.FLOAT, [1])])
if_node = helper.make_node("If", ["cond"], ["y"],
                           then_branch=then_g, else_branch=else_g)
graph_if = helper.make_graph(
    [helper.make_node("Greater", ["x", "zero"], ["cond"]),
     if_node],
    "unsup_if",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1])],
    initializer=[helper.make_tensor("zero", TensorProto.FLOAT, [1], [0.0])])
model_if = helper.make_model(graph_if, opset_imports=[helper.make_opsetid("", 13)])
onnx.checker.check_model(model_if)
onnx.save(model_if, "deeplearning/data/onnx/unsup_if.onnx")

# ---- 普通未知算子模型 ----
graph_fake = helper.make_graph(
    [helper.make_node("FakeOp", ["x"], ["y"])],
    "unsup_unknown",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1])],
    outputs=[helper.make_tensor_value_info("y", TensorProto.FLOAT, [1])])
model_fake = helper.make_model(graph_fake, opset_imports=[helper.make_opsetid("", 13)])
onnx.save(model_fake, "deeplearning/data/onnx/unsup_unknown.onnx")

print("wrote unsup_if.onnx (If control-flow) + unsup_unknown.onnx (FakeOp)")
