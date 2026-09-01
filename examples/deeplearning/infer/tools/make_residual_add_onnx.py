#!/usr/bin/env python3
"""生成残差融合（Conv+Add+Relu）测试夹具：
  data[1,4,8,8] → Conv(w[4,4,3,3], b[4], s1 p1) → conv_out
               → Add(residual[1,4,8,8]) → add_out → Relu → prob[1,4,8,8]
优化后 fuseConvAdd 应把整链融成 1 个 op（opKind 73 doRelu，fusedAddCount==1）；
prob 与 numpy 参考（conv+residual 再过 relu）逐位一致。
"""
import os
import numpy as np
import onnx
from onnx import TensorProto, helper

out = "deeplearning/data/onnx/residual_add.onnx"
os.makedirs(os.path.dirname(out), exist_ok=True)

N, C, H, W = 1, 4, 8, 8
KH = KW = 3
rng = np.random.RandomState(0)
data = (rng.rand(N, C, H, W) - 0.5).astype(np.float32)
residual = (rng.rand(N, C, H, W) - 0.5).astype(np.float32)
w = (rng.rand(C, C, KH, KW) - 0.5).astype(np.float32)
b = (rng.rand(C) - 0.5).astype(np.float32)

# conv2d（s=1, p=1，NCHW）
conv = np.zeros((N, C, H, W), dtype=np.float32)
for n in range(N):
    for oc in range(C):
        for oh in range(H):
            for ow in range(W):
                acc = float(b[oc])
                for ic in range(C):
                    for kh in range(KH):
                        for kw in range(KW):
                            ih = oh - 1 + kh
                            iw = ow - 1 + kw
                            if 0 <= ih < H and 0 <= iw < W:
                                acc += float(data[n, ic, ih, iw]) * float(w[oc, ic, kh, kw])
                conv[n, oc, oh, ow] = acc
prob = np.maximum(conv + residual, 0.0).astype(np.float32)

data.tofile("deeplearning/data/onnx/residual_data.f32")
residual.tofile("deeplearning/data/onnx/residual_resid.f32")
prob.tofile("deeplearning/data/onnx/residual_ref.f32")

nodes = [
    helper.make_node("Conv", ["data", "w", "b"], ["conv_out"], name="conv",
                     strides=[1, 1], pads=[1, 1, 1, 1]),
    helper.make_node("Add", ["conv_out", "residual"], ["add_out"], name="add_resid"),
    helper.make_node("Relu", ["add_out"], ["prob"], name="relu"),
]
graph = helper.make_graph(
    nodes, "residual_add",
    [helper.make_tensor_value_info("data", TensorProto.FLOAT, [N, C, H, W]),
     helper.make_tensor_value_info("residual", TensorProto.FLOAT, [N, C, H, W])],
    [helper.make_tensor_value_info("prob", TensorProto.FLOAT, [N, C, H, W])],
    [helper.make_tensor("w", TensorProto.FLOAT, [C, C, KH, KW], w.flatten().tolist()),
     helper.make_tensor("b", TensorProto.FLOAT, [C], b.tolist())],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
model.ir_version = 7
onnx.checker.check_model(model)
onnx.save(model, out)
print("wrote", out)
