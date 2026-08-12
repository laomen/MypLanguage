# ONNX 推理框架验证入口（infer_tests）

`deeplearning/infer/` 纯核心库的**端到端验证入口**。每个 `*_main.myp` 加载一个真实或合成的
`.onnx` 模型，与 onnxruntime 参考逐元素对比，输出结论。

> 定位：验证 `infer/`（ONNX 解析 + 图优化 + CPU/GPU 执行）的正确性，非框架本体。
> 算子级 @test 在仓库 `tests/@test/`（act_opt / bn_opt / graph_opt 等）。

## 入口一览

| 入口 | 里程碑 | 验证内容 | 数据 |
|------|:------:|----------|------|
| `r18_main.myp` | G4 | 真实 ResNet18（20 BN 全融合），top-5/output sum 与 ORT 一致 | `data/onnx/resnet18_v1_7.onnx` + `resnet_input.f32` |
| `resnet_main.myp` | G1-G3 | ResNet50 推理 top-5 + 耗时（GPU 66ms） | `data/onnx/gluon_resnet50_v1b_Opset16.onnx` + `resnet_input.f32` |
| `bn_main.myp` | G3 | BN 端到端 3 用例（fold / standalone / bn_norelu 回归） | `data/onnx/bn_*_test.onnx` + f32 输入 |
| `act_main.myp` | G4 | 激活端到端（Clip / LeakyRelu / HardSwish vs ORT） | `data/onnx/act_test.onnx` |
| `const_main.myp` | G2 | 常量折叠端到端 | `data/onnx/const_fold_test.onnx` |
| `onnx_main.myp` | 早期 | MNIST MLP 推理 + 准确率（78/100） | `data/onnx/mnist_mlp.onnx` |

## 构建与运行

仓库根执行（依赖 `../infer/`，编译带 `--stdlib stdlib`）：

```bash
./build/mypc deeplearning/infer_tests/<entry>.myp -o /tmp/<out> --stdlib stdlib
/tmp/<out>                          # CPU
MYP_GPU=1 /tmp/<out>                # GPU（无 GPU 自动回退 CPU）
MYP_GPU=1 MYP_LAYOUT_NHWC=1 /tmp/<out>   # GPU + NHWC 布局
```

示例：

```bash
# ResNet18（G4 标志性验证）
./build/mypc deeplearning/infer_tests/r18_main.myp -o /tmp/r18 --stdlib stdlib
MYP_GPU=1 /tmp/r18    # → output sum 0.101238，top-5 与 ORT 一致，GPU ~51ms

# BN 三用例
./build/mypc deeplearning/infer_tests/bn_main.myp -o /tmp/bn --stdlib stdlib
/tmp/bn               # → BN FOLD / STANDALONE / NORELU 全 OK
```

## 环境变量

| 变量 | 默认 | 作用 |
|------|------|------|
| `MYP_GPU` | 空 | `1` 走 CUDA（无 GPU 自动回退 CPU） |
| `MYP_LAYOUT_NHWC` | 空 | `1` 启用 NCHW→NHWC 布局变换 |
| `MYP_NO_REUSE` | 空 | `1` 禁用内存区域复用（调试中间张量） |

> 新增验证时：先在 `data/onnx/` 放 `xxx_test.onnx`（opset 用 ORT 支持的版本），用
> `../infer/tools/cross_check_onnx.py` 出参考，再在此目录写 `<name>_main.myp` 逐元素对比。
