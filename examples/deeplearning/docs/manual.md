# MYP 深度学习框架 — 用户手册（examples/deeplearning）

> 版本：**2026-09-03** · 语言：中文（英文版 `manual_EN.md`）
> 覆盖：`examples/deeplearning` 的**推理 + 训练**框架——纯 MYP 实现，运行时零
> Python / onnxruntime；**ONNX 与声明式 JSON** 两种模型源；CPU + GPU 双后端。
>
> **文档定位**：本手册 = 端到端「怎么用」（入门 → 建模 → 训练 → 验证 → 排障）。
> 深挖请配读：`docs/design.md`（架构设计，含 EN）、`docs/sli.md`（`import dl` +
> JSON op 全集参考）、`docs/usage.md`（infer 核心用法/交叉校验）、`../README.md`
> （现状速览）、`CHANGELOG.md`（里程碑）。

---

## 1. 框架能做什么（能力总览）

- **推理**：加载 ONNX 或 JSON 模型 → 图优化 pass 管线 → CPU `run()` / GPU `runGpu()`，
  逐元素与 onnxruntime 一致。真实模型已验证：ResNet18/50、3D U-Net（coarse/fine）。
- **训练**：同一张 runtime 图可静态反向训练——自动补 label + loss + 反向图 + 权重
  Update；CPU 与 GPU（`runTrainAuto`）双后端。
- **算子覆盖（~82 opKind）**：CNN（Conv/Conv3D/1x1/Pool/GAP/BN/IN/LayerNorm/…）、
  FC/MatMul（含 cuBLAS）、张量变换（Transpose/Slice/Concat/Split/Reshape/Expand/
  Where/Tile/Gather/Reduce 族…）、索引（ArgMax/TopK/OneHot/GatherElements/ScatterND）、
  3D（Conv3D/Pool3D/Pad3D/ConvTranspose3D/GAP3D）、激活全族、归一化（BN/IN/GroupNorm/
  LayerNorm/RmsNorm）、LLM（Rope 位置编码）、训练反向算子（激活/池化/归一化/数据
  重排/广播/规约/索引/batch-MatMul 反向全覆盖）。详见 §7 与 `docs/sli.md` §3。
- **GPU 训练**：`runTrainAuto()` 自动判定（图内每 op 有 GPU 分派 → GPU，否则 CPU
  回退）；fan-in 汇合 + BN/IN/batch-MatMul 等 GPU 反向已补（P10a/P10b）。

---

## 2. 环境与安装

- **编译器**：仓库根 `./build/mypc`（自举编译器；构建见根 `CMakeLists.txt`）。
- **标准库/包路径**：编译一律带 `--stdlib stdlib`；`import dl` 须再加
  `--package-path examples/deeplearning`。
- **GPU（可选）**：NVIDIA + CUDA 驱动；`MYP_GPU=1` 且可用时走 CUDA，否则自动回退 CPU。
- **数据**：模型/输入在 `examples/deeplearning/data/`（git 忽略；部分需自行下载）。
- **Python（可选，仅生成/校验，不在运行链路）**：`infer/tools/onnxvenv`
  （onnx + onnxruntime，用于合成模型与 ORT 参考对拍）。

---

## 3. 编译与运行约定

```bash
# 编译（仓库根 cwd）
./build/mypc your_app.myp -o /tmp/app --stdlib stdlib --package-path examples/deeplearning

# 运行：程序内数据路径相对 examples/（先 cd examples 再跑）
cd examples && /tmp/app                          # CPU
cd examples && MYP_GPU=1 /tmp/app                # GPU（无 GPU 自动回退 CPU）
cd examples && MYP_GPU=1 MYP_IR_VERIFY=1 /tmp/app  # + 图/运行时五重 verifier
```

### 环境变量

| 变量 | 默认 | 作用 |
|------|------|------|
| `MYP_GPU` | 空 | `1` 时 `runGpu()`/`runAuto` 走 CUDA（无 GPU 自动回退 CPU） |
| `MYP_LAYOUT_NHWC` | 空 | `1` 时启用 NCHW→NHWC 布局变换（G2 实验特性；GPU 默认 NCHW 更快） |
| `MYP_IR_VERIFY` | 空 | `1` 时触发五重 verifier（verifyIR/Shapes/DefUse/Topo/RuntimeWiring） |
| `MYP_NO_REUSE` | 空 | `1` 时禁用内存区域复用（调试：推理后中间张量仍可读） |
| `MYP_PROF_CPU` / `MYP_PROF_GPU` | 空 | `1` 时输出算子耗时剖析 |

> 编译为纯 MYP 源码（`import dl;`），数据路径相对 `examples/`（`./deeplearning/...`）。

---

## 4. 五分钟上手：推理

统一入口只需 `import dl;` + 一个 `Session`（`dl/dl.myp` 薄转发
`infer/framework.myp` 的 `class Session`）：

```myp
import dl;
class App {
    action:
        @constructor App() {
            Session s = new Session();
            int ok = s.load("./deeplearning/data/onnx/mnist_mlp.onnx");  // 或 loadTrain
            if (ok != 1) { Console.writeString("load failed: " + int(s.loadError())); return; }
            double[] buf = new double[784];   // 填输入
            s.setInput("data", buf, 784);     // 张量名可 inputName() 枚举；动态维 setInputShape
            int rk = s.runAuto();             // MYP_GPU=1 → GPU
            double[] prob = s.getOutput("prob");
            // 用 prob ...
        }
}
int main() { App a = new App(); return 0; }
```

- **任意 ONNX 模型零样板**：用 `infer_tests/run_onnx.myp`（通用 ONNX 运行器，自动
  探测输入/输出张量 → top-k / 输出 / .bin）。
- 多输入/多输出/可选输入：`inputCount/inputName`、`outputCount/outputName` 枚举 +
  `getOutput(name)` 各自取。

---

## 5. 五分钟上手：训练

```myp
import dl;
class TrainApp {
    action:
        @constructor TrainApp() {
            Session t = new Session();
            int ok = t.loadTrain("./deeplearning/infer_tests/batch_matmul_train.json"); // ONNX 亦同
            t.setLossMode(2);        // 0=SoftmaxCE 1=Dice 2=MSE 3=BCE（load 前设）
            t.setLr(0.05); t.setTrainMode(1);
            int labT = t.tensorId("label");
            int step = 0;
            while (step < 800) {
                t.setInput("x", bx, 8);            // 每次 run 前重建输入（arena 复用会覆盖）
                int i = 0; while (i < 12) { t.setFlat(labT, i, 0.0); i = i + 1; }  // CE/Dice one-hot
                t.runTrain();                       // 或 runTrainAuto()（GPU 训练）
                step = step + 1;
            }
            double lf = t.loss();
            double[] o = t.getOutput("o");
        }
}
int main() { TrainApp a = new TrainApp(); return 0; }
```

- label 形状随损失：CE/Dice one-hot（softmaxCE 依赖 label>0.5）；MSE/BCE 同输出形状
  灌实数目标/0-1 掩码（无需 Softmax 头）。
- 优化器/累积/AMP：`setOptimizer(0=SGD/1=动量/2=AdamW)`、`setWeightDecay`、
  `setGradAccumEvery(K)`、`setAmpSim(1)`；详见 §9。

---

## 6. 模型源一：ONNX（`.onnx`）

- 加载：`load`（推理）/ `loadTrain`（训练，登记可训权重）/ `loadMmap`/`loadTrainMmap`
  （mmap 零拷贝）。
- 支持：手写 protobuf wire 解析；权重 `float_data/raw_data/double_data`；**FP16/BF16
  权重自动转 FP32**；ONNX external data 外置权重；opset/version 检查；Unsupported op
  干净诊断（原因分类 + 输出列表，不崩）。
- 已验证真实模型：ResNet18/20-BN 融合、ResNet50（top-5 与 ORT 一致，GPU ~66ms）、
  3D U-Net（coarse/fine）、动态 batch、多输入/多输出/可选输入。
- 新模型快速验证：`infer_tests/run_onnx.myp` 零样板跑任意模型。

---

## 7. 模型源二：声明式 JSON（`.json`，不经过 ONNX）

层式 JSON 描述网络，`loadJson`/`loadJsonTrain` 直接填同一套图管线。

```json
{ "name":"mlp",
  "inputs":[{"name":"data","dims":[1,784]}],
  "outputs":["prob"],
  "layers":[
    {"op":"Gemm","in":"data","out":"h","transB":1,
     "W":{"dims":[64,784],"init":"xavier"},"B":{"dims":[64],"init":"zeros"}},
    {"op":"Relu","in":"h","out":"h"},
    {"op":"Gemm","in":"h","out":"logits","transB":1,
     "W":{"dims":[10,64],"init":"xavier"},"B":{"dims":[10],"init":"zeros"}},
    {"op":"Softmax","in":"logits","out":"prob","axis":1}
  ]}
```

- **连线 = 张量名**：节点 `out:"名"` 定边起点，消费者 `in:"名"` 引用即连成边。
  - fan-out：多下游同引用一个 `out` 名（自动分叉）；
  - fan-in：一层接多上游用 `in2/in3/in4`（二元 `Add/Sub/Div/Mul/MatMul`、`Concat`
    多输入、`Where` 三输入）；
  - 同一 `out` 名不能由两个生产者定义（名字冲突）。
- **换激活 = 换 op 名**：`Relu`→`SiLU`/`LeakyRelu`/`ReLU6`/`HardSwish`/`Sigmoid`…
  （`LeakyRelu` alpha 默认 0.01）。SwiGLU/GLU 门控 = fan-out 双 Gemm + `SiLU` + `Mul`。
- **权重源**：`init:"xavier|zeros|ones|const|gauss"`（确定性 LCG）/
  `values:[…]`（row-major 精确值）/ `safetensors:{"file":…,"tensor":…}`；
  `B` 可选（无 bias Gemm/Conv 支持）。`Embedding` 无数据输入（`W[vocab,D]`+`ids`）。
- **参数化/属性/索引 op**：`Reshape/Gather/Expand/Tile/Slice/Pad`（int64 内联常量）、
  `Squeeze/Transpose/ReduceSum|Mean|Max|Min`（axes 内联）、`ArgMax/ArgMin/TopK/OneHot`、
  `GatherElements/ScatterND`（P8 数据高级索引，含反向 P8b）；3D（`Conv3D/MaxPool3D/
  AveragePool3D/ConvTranspose3D/Resize`）；归一化（`BatchNormalization/
  InstanceNormalization/LayerNorm/RmsNorm/GroupNorm`）；LLM `Rope`。
- **op 全集与逐 op 语法**：见 `docs/sli.md` §3（此处不重复）。
- 固化示例（`infer_tests/*.json` + `*_main.myp`）：`mlp.json`、`branch.json`（多分支
  DAG）、`unet2d.json`（2D U-Net）、`swiglu.json`、`cnn_train.json`、`bn_train.json`、
  `gather_elements.json`/`scatter_nd.json`（+ 训练版）等。

---

## 8. Session API 速查

| 域 | API |
|----|-----|
| 模型源 | `load`/`loadTrain`/`loadMmap`/`loadTrainMmap`（ONNX）；`loadJson`/`loadJsonTrain`（JSON） |
| 输入/输出 | `setInput(name,buf,n)`、`loadInputFromFile(name,f32)`、`setInputShape`（动态维）、`inputCount/inputName`、`outputCount/outputName`、`getOutput(name)`、`tensorId/tensorSize/setFlat/getFlat` |
| 执行 | `run()`/`runGpu()`/`runAuto()`（推理）；`runTrain()`/`runTrainAuto()`（训练步，GPU 判定） |
| 训练 | `setTrainMode(1)`、`setLossMode(0..3)`、`setLr`、`setOptimizer(0|1|2)`、`setWeightDecay`、`setGradAccumEvery`、`setAmpSim`、`loss()`、`gradId(权重名)`、`trainGpuEnd()` |
| checkpoint | `dumpPlan(path)` / `loadPlan(path)`（op/tensor 表 + arena 权重序列化，跳过解析直接恢复/续训） |
| 诊断 | `phase()`（0..3）、`loadError/compileError/runError`、`compileMs/lastRunMs/lastRunOps`、`opCount`、`dumpGraph/dumpIR/dumpMem` |
| 元数据 | `irVersion/opsetVersion/opsetSupported` |

---

## 9. 训练进阶

- **损失模式**（load 前 `setLossMode`）：`0=SoftmaxCE`（默认，分类）`1=Dice`、
  `2=MSE`（回归，label 实数同形）、`3=BCE`（二值/掩码，输出先 Sigmoid）。
- **反向覆盖**：激活全族、池化、归一化（BN/IN scale·bias 梯度）、数据重排/广播
  （Reshape/Flatten/Transpose/Expand/Tile/Gather）、规约（ReduceSum/Mean、ReduceMax/Min
  argmax）、索引（GatherElements/ScatterND）、4D/batch MatMul、Pad、Concat、Conv。
- **GPU 训练**：`runTrainAuto()`——`MYP_GPU=1` 且图内每 op 有 GPU 分派 → 持久化 GPU
  训练步（每步全量 D2H，正确性优先），否则自动 CPU 回退（绝不静默错）。fan-in 汇合
  与 BN/IN/batch-MatMul 网已走 GPU（P10b）。
- **checkpoint/续训**：`dumpPlan` 训练中途落盘 → 新 `Session.loadPlan` 跳过解析直接
  恢复权重续训（loadPlan 会话无 loader，续训用 dump 前记的 tid + `setFlat/getFlat`）。
- **收敛调试提示**：loss 不降先确认 label/损失模式匹配、学习率（大数值 MSE 目标下
  BN scale 梯度大 → 用 lr≈0.005）、每次 run 前重建输入；GPU==CPU loss 逐位一致是
  隔离「新核错误 vs 网络层 bug」的利器。

---

## 10. 验证与回归

```bash
# 全量端到端回归（自动发现 infer_tests/*_main.myp；编译 P4 + 运行 P6）
bash examples/deeplearning/infer_tests/run_all.sh    # → pass=135 fail=0
```

- 每条 `*_main.myp` 是一个能力的最小可跑示例（加载真/合成模型 + 判据输出 OK/FAIL）；
  新算子/新模型建议照此固化一条 + `tests/@test/*_opt.myp` 算子级断言。
- CPU/GPU 一致性：探针 GPU==CPU 逐位一致；端到端 vs ORT 逐元素对拍（`infer/tools/`
  Python 生成合成 `.onnx` 与参考）。
- `MYP_IR_VERIFY=1` 跑图/运行时五重 verifier；`MYP_NO_REUSE=1` 供逐层调试。

---

## 11. 常见问题（FAQ / 陷阱）

- **`import dl` 编译报找不到**：漏 `--package-path examples/deeplearning`。
- **数据打不开**：程序数据路径相对 `examples/`——先 `cd examples` 再运行
  （`./deeplearning/...`）。
- **推理后读中间张量得垃圾值**：区域复用导致 → `MYP_NO_REUSE=1` 重跑，或持久化该张量。
- **`Console.writeFloat` 自带换行且精度低**：打印 loss/数值用 `int(L*1000)` 量化或
  `Fmt.i`。
- **2D MatMul 结果不对**：框架走 4D batch 路径——JSON 用 4D batch 形状声明，或直接用
  `Gemm`。
- **MYP 保留字**：`var`/`ref`/`data`/`fact` 等不能作变量名（如 `refv`）。
- **`main()` 内不能语句级具名调用**：把逻辑放类 `@constructor`，`main` 只 `new`。
- **GPU 训练不收敛而 CPU 正常**：确认 `runTrainAuto` GPU 判定；用 GPU==CPU loss 轨迹
  逐位一致隔离；fan-in 汇合网已修复（P10b），若仍异常看 `phase/错误码`。

---

## 12. 参考文档与示例索引

- **本手册**：`manual.md`（中）/ `manual_EN.md`（英）。
- 入门/状态：`../README.md`、`../README_EN.md`、`../infer_tests/README.md`。
- 架构：`docs/design.md` / `docs/design_EN.md`。
- 参考：`docs/sli.md`（`import dl` + JSON op 全集）、`docs/usage.md`（infer 用法 +
  交叉校验流程）、`docs/gpu_paradigm.md`（GPU 范式 + 路线图）。
- 里程碑：`CHANGELOG.md`。
- 最小可跑示例：`infer_tests/*_main.myp`（推理 `json_model_main`、训练 `sli_fit_main`、
  通用运行器 `run_onnx.myp` 等）。
