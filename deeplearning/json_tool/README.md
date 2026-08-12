# JSON 图小工具（json_tool）

纯 MYP 的**轻量手搭图工具**：用 JSON 描述静态图（张量/算子/权重），构建到
`deeplearning/infer/runtime.myp` 的 `InferenceRuntime` 并推理。

> 定位：**教学 / 快速验证 / GPU 卸载对照**的小工具，不走 ONNX 加载与图优化
> （无融合、无内存复用，bump 分配）。实战模型请用 `deeplearning/infer/` 的 ONNX 路径。

## 文件

```
deeplearning/json_tool/
├── model_loader.myp     # JSON 模型加载器（核心：inline/二进制权重 + loadBatched）
├── models/              # 示例 JSON 模型
│   ├── xor_model.json        # XOR（2→4→1，inline 权重）
│   ├── sigmoid_model.json    # sigmoid + add 检查
│   └── mnist_model.json      # MNIST 784->64->10（结构 + 二进制权重）
├── json_main.myp        # XOR JSON 演示
├── run_model.myp        # 通用 CLI：任意 JSON 模型 + 命令行输入 → 输出张量
├── mnist_main.myp       # MNIST 单样本循环（JSON 图 + 二进制权重）
├── mnist_batch.myp      # MNIST 批量单趟推理
├── gpu_main.myp         # GPU 卸载验证（XOR + MNIST，GPU vs CPU 对比）
├── test_ops.myp         # sigmoid / add 算子正确性验证
├── mnist_model.myp      # 手写 MYP 版 MNIST 图（供参考，未用 JSON）
├── demo_model.myp       # 手写 MYP 版 XOR 图（供参考，未用 JSON）
└── main.myp             # XOR 手写图入口（引 demo_model）
```

## 构建与运行

仓库根执行（依赖 `../infer/runtime.myp`，编译时带 `--stdlib stdlib`）：

```bash
# XOR — JSON 驱动
./build/mypc deeplearning/json_tool/json_main.myp -o /tmp/json_main --stdlib stdlib && /tmp/json_main

# 通用 CLI（任意 JSON 模型）
./build/mypc deeplearning/json_tool/run_model.myp -o /tmp/run_model --stdlib stdlib
/tmp/run_model deeplearning/json_tool/models/xor_model.json input prob 2 1 0.0 1.0
# → output[prob]: 0.0179862 0.982014
/tmp/run_model deeplearning/json_tool/models/sigmoid_model.json x sum 2 1 0.0 1.0
# → output[sum]: 1 1.46212

# MNIST（单样本 / 批量，需 deeplearning/data/mnist_weights.bin）
./build/mypc deeplearning/json_tool/mnist_main.myp -o /tmp/mnist_main --stdlib stdlib && /tmp/mnist_main
./build/mypc deeplearning/json_tool/mnist_batch.myp -o /tmp/mnist_batch --stdlib stdlib && /tmp/mnist_batch

# GPU 卸载验证（MYP_GPU=1 走 CUDA，否则回退 CPU）
./build/mypc deeplearning/json_tool/gpu_main.myp -o /tmp/gpu_main --stdlib stdlib
MYP_GPU=1 /tmp/gpu_main

# 算子验证
./build/mypc deeplearning/json_tool/test_ops.myp -o /tmp/test_ops --stdlib stdlib && /tmp/test_ops
```

## JSON 模型格式

```json
{
  "name": "model",
  "weight_header": 3,
  "tensors": [
    { "name": "input", "rows": 784, "cols": 1 },
    { "name": "w1",    "rows": 64,  "cols": 784, "role": "weight" }
  ],
  "ops": [
    { "type": "dense",   "inputs": ["input", "w1", "b1"], "output": "h" },
    { "type": "relu",    "inputs": ["h"],                 "output": "h_relu" },
    { "type": "softmax", "inputs": ["logits"],            "output": "prob" }
  ],
  "inputs":  ["input"],
  "outputs": ["prob"],
  "weights": [
    { "name": "w1", "values": [1.0, -1.0, ...] },
    { "name": "w2", "file": "data/weights.bin", "offset": 50176, "count": 640 }
  ]
}
```

- `role: "weight"` 张量在 `loadBatched` 下保持固定形状；激活张量宽度 = batch。
- 权重可用 inline `values` 或二进制 `file`+`offset`/`count`（单位 double）；`weight_header` = 文件头跳过的 int32 个数。
