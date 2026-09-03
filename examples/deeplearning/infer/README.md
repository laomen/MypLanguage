# DeepLearning 推理/训练框架（MYP）

用纯 MYP 实现的**静态图推理 + 训练**框架：ONNX 与声明式 JSON 两种模型源，运行时零
Python / onnxruntime。

> 🌐 中文版（本文件）· [English — README_EN.md](README_EN.md)
>
> ⚡ **当前状态（2026-09-03）**：阶段一~九 + 训练激活反向全覆盖 + 2D U-Net 均已落地。
> 本文件为纯中文速览；早期（阶段一~四 CNN/ResNet/GPU）英文逐阶段记录见 README_EN.md。

## 现状总览（2026-09-03）

**算子覆盖（~80 opKind，CPU `ops.myp` + GPU `gpu_ops.myp`，全部 ORT 位精确对拍）**
CNN（Conv/Conv3D/1x1/Pool/GAP/BN/IN/LayerNorm/Resize/Pad…）、FC（Gemm/MatMul/BatchMatMul/
cuBLAS）、张量变换（Transpose/Slice/Concat/Split/Reshape/Expand/Where/Tile/Squeeze/Gather/
Reduce 族/LogSoftmax/Softmax/CE…）、3D（Conv3D/Pool3D/Pad3D/Resize3D）、**激活全族**
（Relu/Sigmoid/ReLU6/LeakyRelu/SiLU/HardSwish/Clip/Softmax/LogSoftmax）、训练反向算子、
Dropout 推理/训练语义。真实模型：**ResNet18/ResNet50**（vs ORT 数值一致）、**3D U-Net**
（coarse/fine）、动态 batch、多输入/多输出/可选输入、FP16/BF16 权重、量化前全精度。

**图优化 pass 管线**（graph_optimizer）：常量折叠 → inferShapes → classifyShapes →
fuseConvBN → fuseConvRelu → GAP+Flatten → DCE → **常量去重 / 死权重裁剪 / 形状值传播 /
Conv1x1 lowering / 算子选择** → NHWC(opt-in) → topoSort → 内存规划 → buildRuntime；
`MYP_IR_VERIFY=1` 触发五重 verifier（verifyIR/verifyShapes/verifyDefUse/verifyTopo/
verifyRuntimeWiring）。

**训练（Session 统一）**：静态反向图（bwd 梯度 + Update）；优化器 SGD/动量/AdamW +
weight decay；梯度累积（micro-batch）；AMP 数值管线骨架；checkpoint（dumpPlan/loadPlan）。
**激活反向全覆盖**：Relu/Sigmoid 早已支持；ReLU6/LeakyRelu/SiLU/HardSwish/LogSoftmax/Clip
已接通（CPU-only 训练，`bwd_activ_main` 数值对拍）。**SwiGLU 可端到端训练**
（`swiglu.json`：fan-out 双 Gemm + SiLU + Mul，200 步 loss 1.09→0.004）；**CNN 训练**固化
（cnn_train/gap_cnn/avgpool_cnn）。训练图自动跳过 Conv+Relu 等结构融合，反向逐节点回传
（修复了此前融合导致 Conv 权重不更新的隐蔽断链）。

**声明式 JSON 模型（不经过 ONNX）**：`loadJson`/`loadJsonTrain` 直接把层式 JSON 填进同一
套图管线。**连线 = 张量名**（`out`→`in`；fan-out 靠名字、fan-in 用 `in2/in3/in4`）；
权重源 init 内联 / `values` / `.safetensors`。示例固化：`unet2d.json`（2D U-Net——
编码 Conv+MaxPool / 解码 ConvTranspose+跳跃 Concat，CPU+GPU 端到端跑通）。

**SLI（统一入口）**：`import dl;` 一个 import 即得全部能力 ——
```myp
import dl;                       // 编译加 --package-path examples/deeplearning
Session s = new Session();
s.loadTrain("model.onnx");       // 或 load/loadMmap/loadTrainMmap；setInputShape 注入动态维
s.setInput("data", buf, n);      // 或 loadInputFromFile(name, f32)
s.runAuto();                     // MYP_GPU=1 → GPU，否则 CPU
double[] out = s.getOutput("y");
s.dumpGraph(); s.dumpIR(); s.dumpMem();   // 结构/计划/内存 dump
// 训练：setLr/setOptimizer(0|1|2)/setWeightDecay/setGradAccumEvery/setAmpSim
//       setTrainMode(1) → runTrain() → loss()；gradId(权重名) 读梯度；dumpPlan/loadPlan
// 声明式 JSON 模型（不经过 ONNX）：s.loadJson("net.json") / s.loadJsonTrain
//   ——层式 JSON 描述网络，即 DAG：fan-out 靠名字引用、fan-in 用 in2/in3/in4 槽
//      op：Gemm/Conv/ConvTranspose/Pool/激活全族（Relu↔SiLU↔LeakyRelu↔ReLU6…换 op 名即换激活）/
//           Softmax/GlobalAveragePool/Flatten + 二元 Add/Sub/Div/Mul/MatMul + 多输入 Concat
//      权重 init/values 或 safetensors 源；训练反向覆盖激活+池化+数据重排/广播/归约族
// 完整上手/连线/激活选用/陷阱/示例：docs/sli.md（本文件仅速览）
```

**布局与测试**
```
infer/
├── framework.myp      # SLI facade：class Session（load/run/train/dump/checkpoint）
├── runtime.myp        # InferenceRuntime：tensor/op 注册 + run()/runGpu() + 优化器/累积状态
├── ops.myp / gpu_ops.myp / ops_iface_all.myp / op_iface.myp   # CPU/GPU 算子（接口分派）
├── graph*.myp         # Graph 域：analysis/compiler/optimizer/planner/shapes/weights/defs
├── onnx_loader.myp    # 纯 MYP protobuf ONNX 读取 + 训练图构建 + 错误码
├── pb.myp             # protobuf wire reader（含 F32/half 工具）
├── safetensors.myp    # safetensors 权重读取（mmap）
├── tensor.myp / graph_node_attrs.myp / graph_nodes.myp
├── tools/             # make_*_onnx.py 合成模型 + ORT 参考；onnxvenv
../dl/dl.myp           # import dl 包入口（薄转发 framework.myp 的 Session）
../infer_tests/        # 端到端回归（115 个 *_main.myp，见 infer_tests/README.md）
../train ../llm ../diffusion   # 相邻分项目（3D 训练 / Qwen2+distilgpt2 / SD1.5）
```

**编译 / 运行 / 回归**
```bash
./build/mypc examples/deeplearning/infer_tests/your_app.myp -o /tmp/app \
    --stdlib stdlib --package-path examples/deeplearning
cd examples && MYP_GPU=1 MYP_IR_VERIFY=1 /tmp/app     # 数据路径相对 examples/
# 全量回归（CPU+GPU，自动发现 infer_tests/*_main.myp）：
# 全量回归（CPU+GPU，自动发现 infer_tests/*_main.myp；并行 编译 P4 + 运行 P6）：
bash examples/deeplearning/infer_tests/run_all.sh    # → == pass=115 fail=0 ==（~4min）
# 旧串行等价：bash /tmp/run_infer_tests.sh（不再建议）
# 关键环境变量：MYP_GPU=1 GPU / MYP_IR_VERIFY=1 verifier / MYP_NO_REUSE=1 逐层对拍
#   / MYP_PROF_CPU|GPU=1 剖析 / MYP_LAYOUT_NHWC=1 / MYP_FAST_MATH=1
```

**相关文档**（`deeplearning/docs/`）：
- [`design.md`](../docs/design.md) — 架构设计说明（运行时 / opKind / 图 pass 管线 / 扩展指南）
- [`usage.md`](../docs/usage.md) — 使用说明（构建运行 / 环境变量 / 回归测试 / 交叉校验）
- [`sli.md`](../docs/sli.md) — SLI 上手（Session API / JSON 语法·连线 / 激活选用 / 训练 / 陷阱）
- [`gpu_paradigm.md`](../docs/gpu_paradigm.md) — GPU 范式库 + 推理框架路线图（M1-M4 / G1-G4）

---

## 早期实现记录（阶段一~四 CNN/ResNet/GPU，英文）

逐阶段的英文实现记录与架构细节（Capabilities / Layout / Reading Real ONNX Models /
G1-G4 图 pass / ResNet-50 / GPU Offload / Verified）已完整保留在英文版
[`README_EN.md`](README_EN.md)，本中文版不再重复。

