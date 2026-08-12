# MYP 深度学习推理框架 — 使用说明

> 适用范围：`deeplearning/infer/`。设计细节见同目录 `design.md`，路线图见 `gpu_paradigm.md`。

---

## 1. 环境要求

- **编译器**：仓库根 `./build/mypc`（构建见仓库根 `CMakeLists.txt`，或直接使用已构建产物）。
- **标准库**：编译时必须带 `--stdlib stdlib`。
- **GPU（可选）**：NVIDIA + CUDA 驱动；`MYP_GPU=1` 且可用时走 CUDA，否则自动回退 CPU。
- **Python 工具（可选，仅生成/校验用，不在推理链路）**：
  `deeplearning/infer/tools/onnxvenv/bin/python`（onnx 1.22 + onnxruntime 1.28，git 已忽略）。

---

## 2. 快速开始

所有入口统一编译方式（在仓库根）：

```bash
./build/mypc deeplearning/infer/<entry>.myp -o /tmp/<out> --stdlib stdlib && /tmp/<out>
```

CPU / GPU 切换用环境变量：

```bash
# CPU
/tmp/<out>
# GPU（无 GPU 自动回退 CPU）
MYP_GPU=1 /tmp/<out>
# GPU + NHWC 布局（G2 实验特性，GPU 默认 NCHW 更快）
MYP_GPU=1 MYP_LAYOUT_NHWC=1 /tmp/<out>
```

### 端到端验证入口（`deeplearning/infer_tests/`）

| 入口 | 用途 | 数据 |
|------|------|------|
| `r18_main.myp` | **G4：真实 ResNet18**（resnet18_v1_7.onnx，20 个 BN 全融合）top-5 与 ORT 一致 | `data/onnx/resnet18_v1_7.onnx` + `resnet_input.f32` |
| `resnet_main.myp` | ResNet50 推理 top-5 + 耗时（GPU 66ms） | `data/onnx/gluon_resnet50_v1b_Opset16.onnx` + `resnet_input.f32` |
| `bn_main.myp` | **G3：BN 端到端**（fold / standalone / bn_norelu 3 用例） | `data/onnx/bn_*_test.onnx` |
| `act_main.myp` | **G4：激活端到端**（Clip/LeakyRelu/HardSwish vs ORT） | `data/onnx/act_test.onnx` |
| `const_main.myp` | 常量折叠端到端 | `data/onnx/const_fold_test.onnx` |
| `onnx_main.myp` | MNIST MLP 推理 + 准确率（78/100） | `data/onnx/mnist_mlp.onnx` |
| `tensorops_main.myp` | **F8：张量操作端到端**（Concat/Reshape/Transpose vs ORT，位精确） | `data/onnx/tensorops_test.onnx`（`tools/make_tensorops_onnx.py` 生成） |
| `slice_main.myp` | **F8：Slice 端到端**（正区间/负索引/负 step/INT64_MAX vs ORT，位精确） | `data/onnx/slice_test.onnx`（`tools/make_slice_onnx.py` 生成） |

示例（ResNet18）：

```bash
./build/mypc deeplearning/infer_tests/r18_main.myp -o /tmp/r18 --stdlib stdlib
/tmp/r18                      # CPU，~5s
MYP_GPU=1 /tmp/r18            # GPU，~51ms，output sum 0.101238 与 CPU/ORT 一致
```

---

## 3. 环境变量

| 变量 | 默认 | 作用 |
|------|------|------|
| `MYP_GPU` | 空 | `1` 时 `runGpu()` 走 CUDA（无 GPU 自动回退 CPU） |
| `MYP_LAYOUT_NHWC` | 空 | `1` 时 loader 启用 NCHW→NHWC 布局变换（G2 实验特性） |
| `MYP_NO_REUSE` | 空 | `1` 时禁用内存区域复用（调试：推理后中间张量仍可读） |

---

## 4. 回归与算子级测试

框架的算子级测试走仓库通用 @test 运行器（会自动扫描 `tests/@test/*.myp`）：

```bash
./tests/run_tests.sh        # 全量回归，当前基线 237/237
```

深度学习相关 @test（会 `import` infer 模块）：

- `tests/@test/gpu_paradigm.myp` — GPU 范式库（CPU 回退 + 真 GPU 双跑）
- `tests/@test/graph_opt.myp` — NHWC 内核与 NCHW 等价（G2）
- `tests/@test/bn_opt.myp` — BN 算子与手工参考一致（G3）
- `tests/@test/act_opt.myp` — ReLU6/LeakyRelu/SiLU/HardSwish/Clip（G4，含 SiLU 手工参考）

---

## 5. Python 辅助工具（`deeplearning/infer/tools/`）

> 均不参与运行时推理，仅用于生成 fixture / 交叉校验。使用 venv 解释器：

```bash
PY=deeplearning/infer/tools/onnxvenv/bin/python
```

| 工具 | 用途 |
|------|------|
| `make_mnist_mlp_onnx.py` | 由 `data/mnist_weights.bin` 生成 `mnist_mlp.onnx`（fixture） |
| `prep_imagenet_input.py` | 真实图片 → 224×224 ImageNet 归一化 f32 输入（`resnet_input.f32`） |
| `cross_check_onnx.py` | 用 onnxruntime 跑 .onnx，输出参考张量（.bin）供 MYP 端对比 |

### 交叉校验流程（新增验证时）

1. 用 `cross_check_onnx.py` 对 `data/onnx/xxx_test.onnx` 出 onnxruntime 参考输出。
2. 在对应 `*_main.myp` 里加载模型、喂输入、跑推理，与参考逐元素对比（max diff）。
3. 预期：折叠/融合后算子仍与 ORT 逐位/近一致（max diff < 1e-6 量级，FP32）。

---

## 6. 数据文件（`deeplearning/data/`，git 忽略）

```
data/
├── onnx/
│   ├── gluon_resnet50_v1b_Opset16.onnx   # 98MB ResNet50（ImageNet CNN）
│   ├── resnet18_v1_7.onnx                # 45MB ResNet18（G4 真实带 BN 模型）
│   ├── mnist_mlp.onnx                    # MNIST MLP
│   ├── resnet_input.f32                  # 预处理好的 ImageNet 输入（NCHW 1*3*224*224）
│   ├── bn_fold_test.onnx / bn_standalone_test.onnx / bn_norelu_test.onnx  # G3 用例
│   ├── act_test.onnx                     # G4 激活用例（opset 14）
│   └── const_fold_test.onnx              # G2 常量折叠用例
├── imagenet/                             # classes.txt（1000 类标签）+ 测试图
├── mnist_weights.bin                     # 训练产物（早期 train_mnist 导出）
└── *.idx3-ubyte                          # MNIST 原始四件套
```

> `.onnx` 属于大文件，`deeplearning/data/` 整体被 `.gitignore` 忽略；换机器/克隆后需重新下载
> （ResNet18 来源：ONNX Model Zoo `vision/classification/resnet/model/resnet18-v1-7.onnx`）。

---

## 7. 常见问题

- **加载失败 / 形状为 0**：模型含不支持的算子 → 先在 `onnx_loader.myp` 的 `inferShapes`
  里确认 op_type 是否被识别；或先用 `tools/cross_check_onnx.py` 确认 .onnx 本身可跑。
- **推理后读中间张量得到垃圾值**：区域复用导致 → `MYP_NO_REUSE=1` 重跑（或持久化该张量）。
- **GPU 与 CPU 结果不一致**：确认内核数值路径一致（`@gpu for` 内用 `float`，
  标量参数经分发解析成 `double` 传入）；先 CPU 定位再上 GPU。
- **top-5 全乱**：多为图接线/拓扑问题 → 检查 `topoSort` 是否生效、`planOrder_` 是否被使用。
- **string + float 拼接乱码**：MYP 既有显示 bug，改用 `Console.writeFloat(double)` 中转。
