#!/usr/bin/env python3
"""3D：生成合成 AvgPool3D + Pad3D ONNX + ORT 交叉校验。

模型（opset12）:
  x[1,2,8,8,8] → Pad(constant, pads=[0,0,1,1,1, 0,0,1,1,1]) → [1,2,10,10,10]
               → AveragePool(kernel=[2,2,2], stride=[2,2,2]) → y[1,2,5,5,5]
  另加各向异性核分支：
  x2[1,2,8,8,8] → AveragePool(kernel=[1,2,2], stride=[1,2,2]) → y2[1,2,8,4,4]
"""
import numpy as np, onnx
from onnx import helper, TensorProto, numpy_helper
import onnxruntime as ort

np.random.seed(17)
x = np.random.randn(1, 2, 8, 8, 8).astype(np.float32)
pads = np.array([0,0,1,1,1, 0,0,1,1,1], dtype=np.int64)
nodes = [
    helper.make_node("Pad", ["x", "pads"], ["p"], mode="constant"),
    helper.make_node("AveragePool", ["p"], ["y"], kernel_shape=[2,2,2], strides=[2,2,2]),
    helper.make_node("AveragePool", ["x"], ["y2"], kernel_shape=[1,2,2], strides=[1,2,2]),
]
graph = helper.make_graph(
    nodes, "pad3d_avgpool3d",
    inputs=[helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2, 8, 8, 8])],
    outputs=[
        helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 2, 5, 5, 5]),
        helper.make_tensor_value_info("y2", TensorProto.FLOAT, [1, 2, 8, 4, 4]),
    ],
    initializer=[numpy_helper.from_array(pads, name="pads")],
)
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 12)])
model.ir_version = 8
onnx.checker.check_model(model)
onnx.save(model, "deeplearning/data/onnx/pad3d_avgpool3d.onnx")
sess = ort.InferenceSession("deeplearning/data/onnx/pad3d_avgpool3d.onnx", providers=["CPUExecutionProvider"])
y, y2 = sess.run(["y", "y2"], {"x": x})
print("y shape:", y.shape, "sum:", float(y.sum()))
print("y2 shape:", y2.shape, "sum:", float(y2.sum()))
with open("deeplearning/data/onnx/pad3d_avgpool3d_in.f32","wb") as f: f.write(x.tobytes())
with open("deeplearning/data/onnx/pad3d_avgpool3d_y.bin","wb") as f: f.write(y.astype(np.float32).tobytes())
with open("deeplearning/data/onnx/pad3d_avgpool3d_y2.bin","wb") as f: f.write(y2.astype(np.float32).tobytes())
print("wrote refs")
