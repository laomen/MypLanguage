#!/usr/bin/env python3
"""G3：生成 Reduce 族（Mean/Sum/Max/Min）合成 ONNX + ORT 交叉校验。

用法（仓库根）:
  python3 deeplearning/infer/tools/make_reduce_onnx.py
生成:
  deeplearning/data/onnx/reduce_test.onnx
  deeplearning/data/onnx/reduce_test.f32
  deeplearning/data/onnx/reduce_test_ort.bin

模型（opset14）: x[1,2,3,4] → 8 路 Reduce（Mean/Sum/Max/Min × 全规约/空间规约）→ Concat
  ReduceMean axes=[2,3] keepdims=1 → [1,2,1,1]
  ReduceSum  axes=[2,3] keepdims=1 → [1,2,1,1]
  ReduceMax  axes=[2,3] keepdims=1 → [1,2,1,1]
  ReduceMin  axes=[2,3] keepdims=1 → [1,2,1,1]
  ReduceMean axes=[] (all) keepdims=1 → [1,1,1,1]
  ReduceSum  axes=[] (all) keepdims=1 → [1,1,1,1]
  ReduceMax  axes=[] (all) keepdims=1 → [1,1,1,1]
  ReduceMin  axes=[] (all) keepdims=1 → [1,1,1,1]
  → Concat(axis=0) → y[8,1,2,1,1] 4D → 实际 Concat 后 [8,2,1,1]
"""
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper

OUT = "deeplearning/data/onnx/reduce_test.onnx"
IN_F32 = "deeplearning/data/onnx/reduce_test.f32"
ORT_BIN = "deeplearning/data/onnx/reduce_test_ort.bin"

np.random.seed(41)
x = np.random.randn(1, 2, 3, 4).astype(np.float32)

def make_reduce(op, inp, out, axes=None, keepdims=1):
    kwargs = {"axes": axes, "keepdims": keepdims}
    return helper.make_node(op, [inp], [out], name=out, **kwargs)

nodes = []
# 空间规约（per-(n,c)）
nodes.append(helper.make_node("ReduceMean", ["x"], ["rm"], axes=[2, 3], keepdims=1, name="rm"))
nodes.append(helper.make_node("ReduceSum", ["x"], ["rs"], axes=[2, 3], keepdims=1, name="rs"))
nodes.append(helper.make_node("ReduceMax", ["x"], ["rx"], axes=[2, 3], keepdims=1, name="rx"))
nodes.append(helper.make_node("ReduceMin", ["x"], ["rn"], axes=[2, 3], keepdims=1, name="rn"))
# 全规约（标量，keepdims=1 → [1,1,1,1]）
nodes.append(helper.make_node("ReduceMean", ["x"], ["rm0"], keepdims=1, name="rm0"))
nodes.append(helper.make_node("ReduceSum", ["x"], ["rs0"], keepdims=1, name="rs0"))
nodes.append(helper.make_node("ReduceMax", ["x"], ["rx0"], keepdims=1, name="rx0"))
nodes.append(helper.make_node("ReduceMin", ["x"], ["rn0"], keepdims=1, name="rn0"))
# 同维度分别 Concat
nodes.append(helper.make_node("Concat", ["rm", "rs", "rx", "rn"], ["y1"], axis=0, name="cat1"))
nodes.append(helper.make_node("Concat", ["rm0", "rs0", "rx0", "rn0"], ["y0"], axis=0, name="cat0"))

graph = helper.make_graph(
    nodes, "reduce_test",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 3, 4])],
    outputs=[helper.make_tensor_value_info("y1", TensorProto.FLOAT, [4, 2, 1, 1]),
             helper.make_tensor_value_info("y0", TensorProto.FLOAT, [4, 1, 1, 1])],
    initializer=[],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 12)])
onnx.checker.check_model(model)
onnx.save(model, OUT)

# 输入 + ORT 参考（多输出 y1/y0 拼成一个文件：y1 后接 y0）
import onnxruntime as ort
x.tofile(IN_F32)
sess = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
res = sess.run(None, {"x": x})
y1 = res[0]
y0 = res[1]
np.concatenate([y1.ravel(), y0.ravel()]).astype(np.float32).tofile(ORT_BIN)
print("wrote", OUT, "y1.shape", y1.shape, "y0.shape", y0.shape)
print("spatial mean[0,0]=", y1.ravel()[0], " sum[0,0]=", y1.ravel()[2], " max[0,0]=", y1.ravel()[4], " min[0,0]=", y1.ravel()[6])
