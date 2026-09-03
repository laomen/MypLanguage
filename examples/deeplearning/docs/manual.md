# MYP 深度学习框架 — 用户手册（examples/deeplearning）

> 版本：**2026-09-03** · 语言：中文（英文版 `manual_EN.md`）
> 覆盖：`examples/deeplearning` 的**推理 + 训练**框架——纯 MYP 实现，运行时零
> Python / onnxruntime；**ONNX 与声明式 JSON** 两种模型源；CPU + GPU 双后端。
>
> **本手册为唯一用户文档**：已内联并取代原 `docs/sli.md`（`import dl` + JSON op 全集）
> 与 `docs/usage.md`（infer 用法/工具/交叉校验）——两者不再单独保存。
> 深挖架构见 `docs/design.md`（含 EN）；现状速览 `../README.md`；里程碑 `CHANGELOG.md`。

---

## 目录

1. 框架能做什么（能力总览）
2. 环境与安装
3. 编译 / 运行 / 环境变量
4. 推理：快速上手 + 通用运行器
5. 训练：快速上手
6. ONNX 模型源（建模 / 支持 / 工具 / 交叉校验）
7. JSON 模型源（连线 / 激活 / 权重 / op 全集）
8. Session API 参考（能力速览表 + checkpoint + 诊断）
9. 训练进阶（损失 / GPU 训练 / 收敛调试）
10. 验证与回归（端到端 + 算子级 @test + Python 工具）
11. 数据文件
12. 常见问题（FAQ / 陷阱）
13. 参考文档与示例索引

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
  重排/广播/规约/索引/batch-MatMul 反向全覆盖）。op 清单见 §7/§9。
- **GPU 训练**：`runTrainAuto()` 自动判定（图内每 op 有 GPU 分派 → GPU，否则 CPU
  回退）；fan-in 汇合 + BN/IN/batch-MatMul 等 GPU 反向已补（P10a/P10b）。

---

## 2. 环境与安装

- **编译器**：仓库根 `./build/mypc`（自举编译器；构建见根 `CMakeLists.txt`）。
- **标准库/包路径**：编译一律带 `--stdlib stdlib`；`import dl` 须再加
  `--package-path examples/deeplearning`（`import dl` 按包规则命中 `dl/dl.myp`）。
- **GPU（可选）**：NVIDIA + CUDA 驱动；`MYP_GPU=1` 且可用时走 CUDA，否则自动回退 CPU。
- **数据**：模型/输入在 `examples/deeplearning/data/`（git 忽略，见 §11）。
- **Python（可选，仅生成/校验，不在运行链路）**：`infer/tools/onnxvenv`
  （onnx + onnxruntime，用于合成模型与 ORT 参考对拍）。

---

## 3. 编译 / 运行 / 环境变量

```bash
# 编译（仓库根 cwd）：import dl 必须带 --package-path
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
| `MYP_LAYOUT_NHWC` | 空 | `1` 时 loader 启用 NCHW→NHWC 布局变换（G2 实验特性；GPU 默认 NCHW 更快） |
| `MYP_IR_VERIFY` | 空 | `1` 时触发五重 verifier（verifyIR/Shapes/DefUse/Topo/RuntimeWiring） |
| `MYP_NO_REUSE` | 空 | `1` 时禁用内存区域复用（调试：推理后中间张量仍可读） |
| `MYP_PROF_CPU` / `MYP_PROF_GPU` | 空 | `1` 时输出算子耗时剖析 |

### 端到端验证入口（`deeplearning/infer_tests/`）

每条 `*_main.myp` 是一个能力的最小可跑示例（加载真/合成模型 + 判据输出 OK/FAIL）：

| 入口 | 验证内容 |
|------|----------|
| `r18_main.myp` | ResNet18（20 BN 融合）top-5/output sum 与 ORT 一致（CPU ~5s / GPU ~51ms） |
| `resnet_main.myp` | ResNet50 推理 top-5 + 耗时（GPU 66ms） |
| `bn_main.myp` | BN 端到端 3 用例（fold / standalone / bn_norelu） |
| `act_main.myp` | 激活端到端（Clip/LeakyRelu/HardSwish vs ORT） |
| `const_main.myp` | 常量折叠端到端 |
| `onnx_main.myp` | MNIST MLP 推理 + 准确率（78/100） |
| `tensorops_main.myp` / `slice_main.myp` / `ops2d_main.myp` | F8 张量操作/Slice/2D 通用算子 vs ORT |
| **`run_onnx.myp`** | **通用 ONNX 运行器**：任意模型零样板推理 → top-k/输出/.bin（自动探测 IO） |

示例（ResNet18）：

```bash
./build/mypc deeplearning/infer_tests/r18_main.myp -o /tmp/r18 --stdlib stdlib
/tmp/r18                      # CPU
MYP_GPU=1 /tmp/r18            # GPU，output sum 0.101238 与 CPU/ORT 一致
```

---

## 4. 推理：快速上手

统一入口只需 `import dl;` + 一个 `Session`：

```myp
import dl;
class App {
    action:
        @constructor App() {
            Session s = new Session();
            int ok = s.load("./deeplearning/data/onnx/mnist_mlp.onnx");  // 或 loadTrain/loadMmap
            if (ok != 1) { Console.writeString("load failed: " + int(s.loadError())); return; }
            double[] buf = new double[784];   // 填输入
            s.setInput("data", buf, 784);     // 名可 inputName() 枚举；动态维 setInputShape 注入
            int rk = s.runAuto();             // MYP_GPU=1 → GPU
            double[] prob = s.getOutput("prob");  // 图输出名 outputName() 枚举
            // 用 prob ...
        }
}
int main() { App a = new App(); return 0; }
```

- **任意 ONNX 模型零样板**：`infer_tests/run_onnx.myp`（自动探测输入/输出张量 → top-k）。
- **多输入/多输出/可选输入**：`inputCount/inputName`、`outputCount/outputName` 枚举 +
  `getOutput(name)` 各自取。
- 真实模型验证参考：`infer_tests/r18_main.myp`（ResNet18）/ `resnet_main.myp`
  （ResNet50，含 top-5）；3D U-Net（coarse/fine）。

---

## 5. 训练：快速上手

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
  灌实数目标/0-1 掩码（无需 Softmax 头）。详见 §9。

---

## 6. ONNX 模型源（`.onnx`）

- **加载**：`load`（推理）/ `loadTrain`（训练，登记可训权重）/ `loadMmap`/`loadTrainMmap`
  （mmap 零拷贝）。
- **支持**：手写 protobuf wire 解析；权重 `float_data/raw_data/double_data`；
  **FP16/BF16 权重自动转 FP32**；ONNX external data 外置权重；opset/version 检查；
  Unsupported op 干净诊断（原因分类 + 输出列表，不崩）。
- **已验证真实模型**：ResNet18（20 BN 融合）、ResNet50（top-5 与 ORT 一致）、3D U-Net
  （coarse/fine）、动态 batch、多输入/输出/可选输入。

### Python 辅助工具（`deeplearning/infer/tools/`，均不参与运行时推理）

```bash
PY=deeplearning/infer/tools/onnxvenv/bin/python
```

| 工具 | 用途 |
|------|------|
| `make_mnist_mlp_onnx.py` | 由 `data/mnist_weights.bin` 生成 `mnist_mlp.onnx`（fixture） |
| `prep_imagenet_input.py` | 真实图片 → 224×224 ImageNet 归一化 f32 输入（`resnet_input.f32`） |
| `cross_check_onnx.py` | 用 onnxruntime 跑 .onnx，输出参考张量（.bin）供 MYP 端对比 |

**交叉校验流程（新增验证时）**：① `cross_check_onnx.py` 对 `data/onnx/xxx_test.onnx`
出 ORT 参考；② 在对应 `*_main.myp` 里加载、喂输入、跑推理，与参考逐元素对比；
③ 预期折叠/融合后仍与 ORT 逐位/近一致（max diff < 1e-6 量级，FP32）。

---

## 7. JSON 模型源（`.json`，不经过 ONNX）

层式 JSON 描述网络，`loadJson`/`loadJsonTrain` 直接填框架 Graph（复用同一套优化/执行
管线，用户全程看不到 ONNX）。

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

### 7.1 连线 = 张量名（隐式命名图，无显式 edge 数组）

- 每个 `layers[]` 元素是一个节点：`out:"名"` 定义**边起点**，消费者层 `in:"名"` 引用
  即连成有向边——名字是唯一「导线」。
  - **fan-out（一线接多下游）**：多层把同一 `out` 名当 `in`（SwiGLU 的 `gate`/`up`
    都 `in:"h"`），张量自动分叉；
  - **fan-in（多线汇合）**：一层接多上游用 `in2/in3/in4` 槽（`Mul in:"gact" in2:"up"`；
    `Add/Sub/Div/Mul/MatMul` 二元、`Concat` 多输入、`Where` 三输入）；
  - 不允许两个不同生产者写同一 `out` 名（名字冲突=重定义）。

### 7.2 选用 / 更换激活

激活是普通单输入层——把 `op` 名换成目标激活即可（`Relu`→`SiLU`/`LeakyRelu`/`ReLU6`/
`HardSwish`/`Sigmoid`…），`in`/`out` 名照旧（`LeakyRelu` alpha 默认 0.01）。
融合写法 `Conv`/`Add` 后接 Relu 推理会单内核融合（自动）；训练图自动拆成独立 Relu
反向。**SwiGLU/GLU 型门控** = fan-out 双 Gemm + `SiLU` + `Mul`（非独立算子）：
```json
{ "op":"Gemm","in":"h","out":"gate","transB":1,"W":{"dims":[FF,HD],"init":"xavier"}},
{ "op":"Gemm","in":"h","out":"up",  "transB":1,"W":{"dims":[FF,HD],"init":"xavier"}},
{ "op":"SiLU","in":"gate","out":"gact" },
{ "op":"Mul", "in":"gact","out":"m","in2":"up" }
```

### 7.3 权重源

`W:{"dims":[…], "init":"xavier|zeros|ones|const|gauss"}`（确定性 LCG）/
`W:{"dims":[…], "values":[… ]}`（row-major 手写数组，值精确可复现）/
`W:{"dims":[…], "safetensors":{"file":"….safetensors","tensor":"名"}}`
（按 JSON 张量名自动从 .safetensors 读值）。`B` 可选（无 bias Gemm/Conv 支持）。
另有**无数据输入 `Embedding`**：`{"op":"Embedding","W":{"dims":[3,2],"values":[…]},
"ids":[2,0,1],"out":"e"}`（W[vocab,D] + ids int64 → 查表）。

### 7.4 op 全集（逐 op 语法）

- **单输入激活**：`Relu`/`Sigmoid`/`ReLU6`（注意 `LU6` 大写）/`LeakyRelu`/`SiLU`
  （=Swish β=1）/`HardSwish`/`Clip`（内联 `min`/`max` 边界）/`Tanh`/`Softmax(axis)`/
  `LogSoftmax(axis)`/`GlobalAveragePool`/`Flatten(axis)`；单输入 `Sqrt`/`Dropout`
  （推理恒等 / 训练随机 mask）。
- **二元**：`Add`/`Sub`/`Div`/`Mul`/`MatMul`（`in2` 右操作数）；**多输入** `Concat`
  （`in/in2/in3, axis`）；**三输入** `Where`（`in/in2/in3`）。
- **权重型**：`Gemm`/`Conv`/`ConvTranspose`/`MatMul`（可选 W）；池化 `MaxPool`/
  `AveragePool`（kernel/strides/pads）。
- **归一化（均含 scale·bias 训练梯度）**：`BatchNormalization`（scale/bias/mean/var +
  epsilon；BN 固定 mean/var 反向 dx=dy·scale·rstd）/`InstanceNormalization`（per-(n,c)
  重估统计反向）/`LayerNorm`（gamma/beta）/`RmsNorm`（gamma）/`GroupNorm`（gamma/beta
  [C], groups, epsilon；NCHW [N,C,H,W]，组=g 覆盖每 (n,g) 的 cpg·H·W；SD1.5
  norm_num_groups）。
- **3D**：`Conv3D`（W 5D [Cout,Cin,kd,kh,kw] + kernel/strides/pads6）/`MaxPool3D`/
  `AveragePool3D`/`Resize`（sizes [1,1,(outD,)outH,outW]）/`ConvTranspose3D`（W 5D
  [Cin,Cout,kd,kh,kw] + strides/pads6；转置卷积，U-Net 解码上采样）——3D JSON
  输入/权重用 5 维 dims。
- **LLM 位置编码**：`Rope(in, in2:cos, in3:sin, heads)`（x[D,S] 特征行×位置列；
  cos/sin[dh/2,S] 位置表运行时 setInput；out 独立张量=copy+原地逐头旋转）。
- **参数化 op（int64 内联常量）**：`Reshape(shape)`、`Gather(indices,axis)`、
  `Expand(shape)`、`Tile(repeats)`、`Slice(starts,ends[,axes][,steps])`、
  `Pad(pads[,mode][,value])`（pads 8 值 [N,C,H,W] begin+end；constant 填充 value 内联
  浮点默认 0）。例：`{"op":"Reshape","in":"data","out":"out","shape":[2,3]}`。
- **属性类 op（int 数组内联）**：`Squeeze(axes)`、`Transpose(perm)`、
  `ReduceSum`/`ReduceMean`/`ReduceMax`/`ReduceMin(axes[,keepdims])`（Reduce 族含
  N/C → 全规约；`[2,3]` → 空间规约 per-(n,c)）。
- **索引类（行/特征轴 1D flat，单样本=整行 logits）**：`ArgMax`/`ArgMin`（单输出标量
  索引）、`TopK(k, outs:[values,indices])`（前 k 大，输出 float 编码索引——对应 CE
  标签/LLM 采样）、`OneHot(depth)`（idx float 逐元素 → 行优先 one-hot [nIdx,depth]）。
- **数据高级索引（P8/P8b，均含训练反向）**：
  - `GatherElements(in, in2:idx, axis)`：data/indices 同形逐元素 gather，out shape=
    indices；axis 0..rank-1；索引 float 张量运行时 setInput。反向 dy scatter-add 回
    data（重复 idx 累加）。
  - `ScatterND(in, in2:idx[q,k], in3:upd)`：data 副本 + indices 前缀 scatter；k ≤ data
    秩，块长=data[k:]、upd=[q]+data[k:]。反向 dx=dy 且写位置清零、du=写位置 gather dy
    （updates 图输入时无 du）。
  - 两 op 的 **GPU 反向**已接通（GE 反向 thread-per-dx-slot 扫描累加、无 float 原子；
    ScatterND 反向 dx=dy 拷贝+后置零+du gather）。自然 2D 用例固化
    `json_gather_elements_2d`/`json_scatter_nd_2d`（FC/Gemm 转置产物 2D 属布局错配边界）。
  - **BN NHWC 反向**（P9c，opKind 117）：NHWC flat=sp*C+c；IN 未入 NHWC 布局表故仅 BN。
- 训练自动补的反向（见 §9）：Gemm/MatMul/Conv/Relu/Sigmoid/SoftmaxCE/Add/Sub/Mul/Div/
  Pool/Concat + Reshape/Flatten/Squeeze/Transpose/Expand/Tile/ReduceSum/ReduceMean/
  Gather 反向——纯数据重排/广播/归约/收集 op 可出现在 loss 路径；4D/batch MatMul 走
  BwdBatchMatmul（P9）。

固化 JSON 示例（`infer_tests/`）：`mlp.json`、`branch.json`（多分支 DAG）、
`unet2d.json`（2D U-Net）、`swiglu.json`、`cnn_train.json`、`bn_train.json`、
`gather_elements.json`/`scatter_nd.json`（+ 训练版）、`safe_gemm.json`、`reshape.json`、
`argmax.json`、`mse_reg.json`/`bce_clf.json` 等。

---

## 8. Session API 参考

### 8.1 能力速览表

| 域 | API |
|----|-----|
| 模型源 | `load`/`loadTrain`（ONNX）、`loadMmap`/`loadTrainMmap`（mmap 零拷贝）、`loadJson`/`loadJsonTrain`（声明式 JSON，不经过 ONNX） |
| 输入/输出 | `setInput(name, buf, n)`、`loadInputFromFile(name, f32)`、`setInputShape`（动态维注入）、`inputCount`/`inputName`、`outputCount`/`outputName`、`getOutput(name)`、`tensorId(name)`/`tensorSize(name)`/`setFlat`/`getFlat` |
| 执行 | `run()`/`runGpu()`/`runAuto()`（推理，MYP_GPU 自动）；`runTrain()`/`runTrainAuto()`（训练步，GPU 判定） |
| 训练 | `setTrainMode(1)`、`setLr`、`setOptimizer(0=SGD/1=动量/2=AdamW)`、`setWeightDecay`、`setGradAccumEvery(K)`（梯度累积）、`setAmpSim(1)`（fp16 梯度舍入模拟）、`loss()`、`gradId(weightName)`、`trainGpuEnd()` |
| checkpoint | `dumpPlan(path)`/`loadPlan(path)`（op/tensor 表 + arena 权重序列化，跳过 ONNX/JSON 解析直接恢复续训） |
| 诊断 | `phase()`（0 未加载/1 加载中/2 编译中/3 就绪）、`loadError`/`compileError`/`runError`、`compileMs`/`lastRunMs`/`lastRunOps`、`opCount`、`dumpGraph`/`dumpIR`/`dumpMem` |
| 元数据 | `irVersion`/`opsetVersion`/`opsetSupported` |

### 8.2 checkpoint（dumpPlan / loadPlan）

```myp
t.dumpPlan("/tmp/ckpt.bin");     // 训练中途：op/tensor 表 + arena 权重序列化
Session t2 = new Session();
t2.loadPlan("/tmp/ckpt.bin");    // 新 Session 跳过 ONNX/JSON 解析直接恢复，续训
// 约定：loadPlan 会话无 loader，按名 tensorId 返回 -1——续训用 dump 前记录的 tid
//       经 setFlat/getFlat（feedInputByTid 语义）；label 同 one-hot。
```

### 8.3 诊断

```myp
s.phase(); s.loadError(); s.compileError(); s.runError();   // 各阶段错误码
s.compileMs(); s.lastRunMs(); s.lastRunOps();               // 统计
s.dumpGraph(); s.dumpIR(); s.dumpMem();                     // 结构/计划/内存 dump
```

加载失败先看 `phase`/错误码，别猜段错误。诊断栈：加载/编译/运行三阶段错误分开。

---

## 9. 训练进阶

- **损失模式**（load 前 `setLossMode`）：`0=SoftmaxCE`（默认，分类）`1=Dice`、
  `2=MSE`（回归，label 同输出形状实数目标）/`3=BCE`（二值/掩码，输出先 Sigmoid）。
  无需 Softmax 分类头时，模型输出本身即预测张量（回归 `mse_reg.json` 500 步 loss→0；
  二值 `bce_clf.json` 600 步→floor 0.325）。
- **每次 run 前重建输入**：训练 arena 复用会覆盖输入区。
- **GPU 训练（P10a）**：`runTrainAuto()`——`MYP_GPU=1` 且 `gpuTrainReady()`（图内全部
  opKind 有 GPU 分派槽；梯度累积/AMP 模拟除外）→ 首步 `gpuPersistentStart` + 每步
  `runGpu`（增量上传置脏输入 + 全量 D2H），否则回退 CPU `runTrain`（绝不静默错）；
  结束 `trainGpuEnd()`。P10b 起 GPU 反向补全 fan-in 汇合（Sub/Mul/Div/Add）与 BN/
  BN-NHWC/IN/batch-MatMul/Reduce/Transpose/Expand/Tile/Gather/ReduceMM/Pad → BN/IN/
  batch-MatMul 训练网转 GPU（loss 同 CPU）。
- **可训结构**：链式 + fan-in 汇合（Add/Sub/Mul/Concat 在 loss 路径）均可训练；
  激活反向全覆盖（Relu/Sigmoid/Tanh + ReLU6/LeakyRelu/SiLU/HardSwish/LogSoftmax/Clip）；
  SwiGLU（`swiglu.json` 200 步 loss 1.09→0.004）、激活链（`activ_chain.json` 300 步
  0.97→0.64）、CNN（`cnn_train.json` 200 步 1.01→0.14；`gap_cnn.json` 1.06→0.90；
  `avgpool_cnn.json` 1.04→0.43——Conv/MaxPool/AvgPool/GAP/Flatten 反向全链可用）。
- **优化器/累积/AMP**：`setGradAccumEvery(K)` micro-batch 累积；`setAmpSim(1)` fp16
  梯度舍入模拟（数值管线骨架）。
- **收敛调试提示**：loss 不降先查 label/损失模式匹配、学习率（大数值 MSE 目标下 BN
  scale 梯度大 → 用 lr≈0.005）、每次 run 前重建输入；GPU==CPU loss 逐位一致是隔离
  「新核错误 vs 网络层 bug」的利器。
- **label 形状**：CE/Dice one-hot（softmaxCE 依赖 label>0.5，非标量）；MSE/BCE 同输出形状。

---

## 10. 验证与回归

```bash
# 全量端到端回归（自动发现 infer_tests/*_main.myp；编译 P4 + 运行 P6）
bash examples/deeplearning/infer_tests/run_all.sh    # → pass=135 fail=0（~4min）
```

- 每条 `*_main.myp` 是一个能力的最小可跑示例；新算子/新模型照此固化一条 +
  算子级 @test。
- **算子级 @test**（仓库通用运行器，自动扫描 `tests/@test/*.myp`）：
  `./tests/run_tests.sh`（全量基线 237/237）。深度学习相关 @test：
  `tests/@test/gpu_paradigm.myp`（GPU 范式库 CPU 回退+真 GPU 双跑）、`graph_opt.myp`
  （NHWC 与 NCHW 等价）、`bn_opt.myp`（BN 与手工参考一致）、`act_opt.myp`
  （ReLU6/LeakyRelu/SiLU/HardSwish/Clip）。
- **CPU/GPU 一致性**：探针 GPU==CPU 逐位一致；端到端 vs ORT 逐元素对拍（Python
  `infer/tools/` 合成 `.onnx` + ORT 参考）。
- `MYP_IR_VERIFY=1` 五重 verifier；`MYP_NO_REUSE=1` 供逐层调试。

---

## 11. 数据文件（`deeplearning/data/`，git 忽略）

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

> `.onnx` 属大文件，`deeplearning/data/` 整体被 `.gitignore` 忽略；换机/克隆后需重新下载
> （ResNet18 来源：ONNX Model Zoo `vision/classification/resnet/model/resnet18-v1-7.onnx`）。

---

## 12. 常见问题（FAQ / 陷阱）

- **`import dl` 编译报找不到**：漏 `--package-path examples/deeplearning`。
- **数据打不开**：程序数据路径相对 `examples/`——先 `cd examples` 再运行
  （`./deeplearning/...`）。
- **加载失败 / 形状为 0**：模型含不支持的算子 → 先在 `onnx_loader.myp` 的
  `inferShapes` 确认 op_type 是否被识别；或先用 `tools/cross_check_onnx.py` 确认
  .onnx 本身可跑。
- **推理后读中间张量得垃圾值**：区域复用导致 → `MYP_NO_REUSE=1` 重跑，或持久化该张量。
- **GPU 与 CPU 结果不一致**：确认内核数值路径一致（`@gpu for` 内用 `float`，标量参数
  经分发解析成 `double` 传入）；先 CPU 定位再上 GPU。
- **top-5 全乱**：多为图接线/拓扑问题 → 检查 `topoSort` 是否生效、`planOrder_` 是否被用。
- **`Console.writeFloat` 自带换行且精度低**：打印 loss/数值用 `int(L*1000)` 量化或
  `Fmt.i`。
- **2D MatMul 结果不对**：框架走 4D batch 路径——JSON 用 4D batch 形状声明，或直接用
  `Gemm`。
- **MYP 保留字**：`var`/`ref`/`data`/`fact` 等不能作变量名（如 `refv`）。
- **`main()` 内不能语句级具名调用**：把逻辑放类 `@constructor`，`main` 只 `new`。
- **string + float 拼接乱码**：MYP 既有显示 bug，改用 `Console.writeFloat(double)` 中转。
- **GPU 训练不收敛而 CPU 正常**：确认 `runTrainAuto` GPU 判定；GPU==CPU loss 逐位一致
  隔离；fan-in 汇合已修复（P10b）；仍异常看 `phase`/错误码。

---

## 13. 参考文档与示例索引

- **本手册**：`manual.md`（中）/ `manual_EN.md`（英）——已并入原 `sli.md` 与
  `usage.md` 的全部内容。
- 入门/状态：`../README.md`、`../README_EN.md`、`../infer_tests/README.md`。
- 架构：`docs/design.md` / `docs/design_EN.md`；GPU 范式/路线图：`docs/gpu_paradigm.md`。
- 里程碑：`CHANGELOG.md`。
- 最小可跑示例：`infer_tests/*_main.myp`——推理 `json_model_main`、JSON 推理+训练、
  `json_branch_main`（多分支）、`json_train_submul_main`（训练含汇合）、`json_safe_main`
  （safetensors 权重源）、`sli_fit_main`（训练完整循环 + checkpoint 续训）、
  `sli_opt_main`（优化器）、`sli_acc_main`（梯度累积）、`sli_amp_main`（AMP 骨架）、
  `multiio_main`（多输入/输出/可选输入）、通用运行器 `run_onnx.myp`。
