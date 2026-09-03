# SLI — DeepLearning 框架统一入口指南（`import dl`）

> 定位：`examples/deeplearning` 的推理/训练框架对外唯一入口。一个 `import dl;`
> 即得**全部能力**——ONNX 与 JSON 两种模型源、推理/训练/checkpoint/诊断，不再
> 手动 import 一堆 `infer/*.myp`。
>
> 底层：`dl/dl.myp`（薄转发 `infer/framework.myp` 的 `class Session`）。用户程序
> 只面对一个 `Session` 对象。

## 0. 编译 / 运行 / 环境

```bash
# 编译：--package-path 指向 examples/deeplearning（import dl 按包规则命中 dl/dl.myp）
./build/mypc your_app.myp -o /tmp/app --stdlib stdlib --package-path examples/deeplearning

# 运行：数据路径相对 examples/ 目录（cd examples 再跑）
cd examples && /tmp/app                      # CPU
cd examples && MYP_GPU=1 /tmp/app            # GPU（无 GPU 自动回退 CPU）
cd examples && MYP_GPU=1 MYP_IR_VERIFY=1 /tmp/app   # + 图/运行时五重 verifier
```

关键环境变量：`MYP_GPU=1`（GPU）、`MYP_IR_VERIFY=1`（verifier）、`MYP_NO_REUSE=1`
（禁用内存复用，逐层对拍）、`MYP_PROF_CPU|GPU=1`（算子耗时剖析）。

## 1. Session 能力速览

| 域 | API |
|----|-----|
| 模型源 | `load`/`loadTrain`（ONNX）、`loadMmap`/`loadTrainMmap`（mmap 零拷贝）、`loadJson`/`loadJsonTrain`（声明式 JSON，不经过 ONNX） |
| 输入/输出 | `setInput(name, buf, n)`、`loadInputFromFile(name, f32)`、`setInputShape`（动态维注入）、`inputCount`/`inputName`、`outputCount`/`outputName`、`getOutput(name)`、`tensorId(name)`/`tensorSize(name)`/`setFlat`/`getFlat` |
| 执行 | `run()`/`runGpu()`/`runAuto()`（MYP_GPU 自动）；`runTrain()`（训练步） |
| 训练 | `setTrainMode(1)`、`setLr`、`setOptimizer(0=SGD/1=动量/2=AdamW)`、`setWeightDecay`、`setGradAccumEvery(K)`（梯度累积）、`setAmpSim(1)`（fp16 梯度舍入模拟）、`loss()`、`gradId(weightName)` |
| checkpoint | `dumpPlan(path)`/`loadPlan(path)`（op/tensor 表 + arena 权重序列化，跳过 ONNX 解析直接恢复） |
| 诊断 | `phase()`（0 未加载/1 加载中/2 编译中/3 就绪）、`loadError`/`compileError`/`runError`、`compileMs`/`lastRunMs`/`lastRunOps`、`opCount`、`dumpGraph`/`dumpIR`/`dumpMem` |
| 元数据 | `irVersion`/`opsetVersion`/`opsetSupported` |

## 2. ONNX 推理

```myp
import dl;
Session s = new Session();
int ok = s.loadTrain("./deeplearning/data/onnx/mnist_mlp.onnx");   // 推理用 load/loadMmap 亦可
s.setInput("data", buf, 784);         // 张量名可经 inputName() 枚举；动态维可 setInputShape 注入
int rk = s.runAuto();                 // MYP_GPU=1 → GPU
double[] prob = s.getOutput("prob");  // 图输出名可经 outputName() 枚举
// 多输入/多输出/可选输入：inputCount/outputCount 枚举 + getOutput(name) 各自取
```

真实模型验证：ResNet18/50、3D U-Net（coarse/fine）vs onnxruntime 数值一致；
ResNet18/50 用 `infer_tests/r18_main.myp`/`resnet_main.myp` 为参考（含 top-5）。

## 3. JSON 声明式模型（不经过 ONNX）

JSON 是**第二种模型源**：用户写层式 JSON，`loadJson` 直接填框架 Graph（复用同一套
优化/执行管线）。用户全程看不到 ONNX。

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

- **连线（边）= 张量名（隐式命名图，无显式 edge 数组）**：每个 `layers[]` 元素是
  一个节点。节点用 `out:"名"` 定义**边起点**，消费者层用 `in:"名"` 引用即连成
  有向边——名字是唯一"导线"：
  ```json
  { "op":"Gemm","in":"data","out":"h" },      // data ──Gemm──▶ h
  { "op":"Relu","in":"h","out":"h"  },        // h    ──Relu──▶ h（原地改值）
  { "op":"Gemm","in":"h","out":"logits" }     // h    ──Gemm──▶ logits
  ```
  即 mlp 全链：`data→Gemm→h→Relu→h→Gemm→logits→Softmax→prob`。
  - **fan-out（一线接多下游）**：多个层把同一 `out` 名当 `in`（SwiGLU 的 `gate`/
    `up` 都 `in:"h"`），该张量自动分叉给多个消费者；
  - **fan-in（多线汇合）**：一层要接多个上游用 `in2/in3/in4` 槽
    （`Mul in:"gact" in2:"up"`；`Add`/`Sub`/`Div`/`Mul`/`MatMul` 二元、`Concat`
    多输入、`Where` 三输入）；
  - 不允许把两个不同生产者写成同一 `out` 名（名字冲突=重定义）。
- **选用 / 更换激活函数**：激活是普通单输入层——把 `op` 名换成目标激活即可
  （`Relu`→`SiLU`/`LeakyRelu`/`ReLU6`/`HardSwish`/`Sigmoid`…），`in`/`out` 名字照旧接前层输出：
  ```json
  { "op":"Gemm","in":"h","out":"h1","transB":1,
    "W":{"dims":[64,784],"init":"xavier"},"B":{"dims":[64],"init":"zeros"}},
  { "op":"SiLU","in":"h1","out":"a" },    // 原 Relu → 换 SiLU
  { "op":"Gemm","in":"a","out":"logits","transB":1,
    "W":{"dims":[10,64],"init":"xavier"},"B":{"dims":[10],"init":"zeros"}}
  ```
  `LeakyRelu` alpha 默认 0.01（ONNX 源可带 alpha 属性）。融合写法 `Conv`/`Add` 后的
  Relu 推理会单内核融合（自动）；训练图自动拆成独立 Relu 反向（见 §4）。
  **SwiGLU/GLU 型门控**用 fan-out 双 Gemm + `SiLU` + `Mul` 组合（非独立算子）：
  ```json
  { "op":"Gemm","in":"h","out":"gate","transB":1,"W":{"dims":[FF,HD],"init":"xavier"},...},
  { "op":"Gemm","in":"h","out":"up",  "transB":1,"W":{"dims":[FF,HD],"init":"xavier"},...},
  { "op":"SiLU","in":"gate","out":"gact" },
  { "op":"Mul", "in":"gact","out":"m","in2":"up" }
  ```
- **op 集**：单输入**激活** `Relu`/`Sigmoid`/`ReLU6`(注意 `LU6` 大写)/`LeakyRelu`/
  `SiLU`(=Swish β=1)/`HardSwish`/`Clip`(内联 `min`/`max` 边界)/`Tanh`/`Softmax(axis)`/
  `LogSoftmax(axis)`/
  `GlobalAveragePool`/`Flatten(axis)`；二元 `Add`/`Sub`/`Div`/`Mul`/`MatMul`；多输入
  多输入 `Concat(in/in2/in3, axis)`；三输入 `Where(in/in2/in3)`；单输入 `Sqrt`/`Dropout`
  （推理恒等）；权重型 `Gemm`/`Conv`/`ConvTranspose`/`MatMul(可选 W)`；
  池化 `MaxPool`/`AveragePool`(kernel/strides/pads)；**3D** `Conv3D`(W 5D
  [Cout,Cin,kd,kh,kw] + kernel/strides/pads6)/`MaxPool3D`/`AveragePool3D`/`Resize`
  (sizes [1,1,(outD,)outH,outW])/`ConvTranspose3D`(W 5D [Cin,Cout,kd,kh,kw] + strides/
  pads6；3D 转置卷积，U-Net 解码上采样)——3D JSON 输入/权重用 5 维 dims；**归一化**
  `BatchNormalization`(scale/bias/mean/var + epsilon)/`InstanceNormalization`
  (scale/bias + epsilon)（二者均含 **scale·bias 训练梯度 P8c**：BN 固定 mean/var 反向
  dx=dy·scale·rstd、IN per-(n,c) 重估统计反向；scale/bias 自动入 Update，见
  bn_train.json/in_train.json）/**`LayerNorm`(gamma/beta) / `RmsNorm`(gamma) / `GELU`**
  （图归一化/激活，复用 LLM kernel；多接 Gemm 输出，tensor 按 [特征行, 样本列] 布局，
  gamma/beta [D=D 特征]）/**`GroupNorm`(gamma/beta [C], groups, epsilon)**（逐通道组
  归一化；NCHW [N,C,H,W]，组=g 覆盖每 (n,g) 的 cpg·H·W；SD1.5 norm_num_groups）；**LLM 位置编码**
  `Rope(in, in2:cos, in3:sin, heads)`（RoPE 图算子：x[D,S] 特征行×位置列，cos/sin[dh/2,S]
  位置表运行时 setInput；out 独立张量 = copy+原地逐头旋转，不破坏 x）；**参数化（int64 常量）**
  `Reshape(shape)`、`Gather(indices,axis)`、`Expand(shape)`、`Tile(repeats)`、
  `Slice(starts,ends[,axes][,steps])`、`Pad(pads[,mode][,value])`（pads 8 值
  [N,C,H,W] begin+end；constant 填充值 value 内联浮点，默认 0）；
  **属性类** `Squeeze(axes)`、`Transpose(perm)`、`ReduceSum`/`ReduceMean`/`ReduceMax`/
  `ReduceMin(axes[,keepdims])`；**索引类（行/特征轴 1D flat，单样本=整行 logits）**
  `ArgMax`/`ArgMin`（单输出标量索引）、`TopK(k, outs:[values,indices])`（前 k 大，
  输出 float 编码索引——对应 CE 标签/LLM 采样）、`OneHot(depth)`（idx float 逐元素
  → 行优先 one-hot [nIdx,depth]）；**数据高级索引（P8/P8b）** `GatherElements(in,in2:idx,
  axis)`（data/indices 同形逐元素 gather，out shape=indices；axis 0..rank-1，索引 float
  张量运行时 setInput）、`ScatterND(in,in2:idx[q,k],in3:upd)`（data 副本 + indices 前缀
  scatter；k ≤ data 秩，块长=data[k:]，upd=[q]+data[k:]）。两 op 均含**训练反向**（P8b）：
  GE 反向 dy scatter-add 回 data（重复 idx 累加）；ScatterND 反向 dx=dy 且写位置清零、
  du=写位置 gather dy（updates 图输入时无 du）。示例：`infer_tests/branch.json`
  （多分支 DAG）、`mlp.json`、`safe_gemm.json`、`reshape.json`、`gather.json`、
  `ops2.json`、`argmax.json`（ArgMax/ArgMin/TopK）、`gather_elements.json`、
  `scatter_nd.json`（P8 数据高级索引）、`gather_elem_train.json`/`scatter_nd_train.json`
  （P8b 反向梯度训练：Conv→索引→MSE 收敛）。两索引 op 的 **GPU 反向**已接通（P9b：GE
  反向 thread-per-dx-slot 扫描累加、无 float 原子、重复 idx 确定；ScatterND 反向 dx=dy 拷贝
  + 后置零 + du gather；`gpu_bwd_gather_elements_main`/`gpu_bwd_scatter_nd_main`）；
  **自然 2D**（2D 图输入喂 GE/ScatterND）已验证并固化 `json_gather_elements_2d` /
  `json_scatter_nd_2d`（FC/Gemm 转置产物 2D 属布局错配边界）。**BN NHWC 反向**（P9c，
  opKind 117，复用 BWD_BATCH_NORM）：NHWC flat=sp*C+c，graph_compiler 按 fwd 节点
  compilerAttrNhwc 路由，`bwd_bn_nhwc_main` 手算对拍（IN 未入 NHWC 布局表，故仅 BN）。
- **参数化 op（int64 内联常量）**：`shape`/`indices`/`repeats` 等 int64 数组直接内联在层里，
  框架登记为内存 int64 常量（无需 ONNX 初始器）：
  ```json
  {"op":"Reshape","in":"data","out":"out","shape":[2,3]},
  {"op":"Gather","in":"data","out":"out","indices":[0,2],"axis":1},
  {"op":"Tile","in":"x","out":"out","repeats":[1,1,2,1]}
  ```
  （前者把 `data[1,6]` 变 `[2,3]`；中者沿 axis 收集 `data` 的指定行/通道；后者沿各维
  按倍数复制。`Expand` 沿 size-1 维 broadcast 复制到目标 shape。）
- **属性类 op（int 数组内联）**：`axes`/`perm` 直接内联（Reduce 族含 N/C → 全规约，
  `[2,3]` → 空间规约 per-(n,c)）：
  ```json
  {"op":"Squeeze","in":"x","out":"out","axes":[1]},
  {"op":"ReduceSum","in":"x","out":"out","axes":[2,3]}
  ```
- **权重源**：`W:{"dims":[…], "init":"xavier|zeros|ones|const|gauss"}`（确定性 LCG），
  `W:{"dims":[…], "values":[… ]}`（row-major 手写权重数组，值精确可复现），
  或 `W:{"dims":[…], "safetensors":{"file":"….safetensors","tensor":"名"}}`（按 JSON
  张量名自动从 .safetensors 读值，替代手写装配）。`B` 可选（无 bias Gemm/Conv 支持）。
  另有**无数据输入的 `Embedding`**（`W`[vocab,D] + `ids` int64 数组 → 查表）：
  `{"op":"Embedding","W":{"dims":[3,2],"values":[… ]},"ids":[2,0,1],"out":"e"}`。
- **训练**：`loadJsonTrain` 自动补 label/loss + 反向图（Gemm/MatMul/Conv/Relu/
  Sigmoid/SoftmaxCE/Add/Sub/Mul/Div/Pool/Concat + Reshape/Flatten/Squeeze/
  Transpose/Expand/Tile/ReduceSum/ReduceMean/Gather 反向——纯数据重排/广播/归约/
  收集 op 现可出现在 loss 路径；**4D/batch MatMul 反向 P9**：2D MatMul 走 BwdDense、
  4D batch（广播）走 BwdBatchMatmul——`batch_matmul_train.json` Conv→batch MatMul→MSE
  收敛验证）。

## 4. 训练

```myp
Session t = new Session();
int ok = t.loadTrain("model.onnx");      // 或 loadJsonTrain("net.json")
t.setLr(0.01); t.setOptimizer(2); t.setWeightDecay(1e-4);   // AdamW
t.setTrainMode(1);
// 损失模式（load 前设）：0=SoftmaxCE(默认) 1=Dice 2=MSE(回归) 3=BCE(二值/掩码)
// t.setLossMode(2);   // MSE：label 同输出形状灌实数目标
// t.setLossMode(3);   // BCE：输出须先 Sigmoid，label 0-1 同形状
int labT = t.tensorId("label");          // 训练自动补的 label
// 每步：喂输入 + label（CE/Dice one-hot；MSE/BCE 实数/掩码）→ runTrain → loss
while (step < N) {
    t.setInput("data", bx, 784);
    for (i...) { t.setFlat(labT, i, 0.0); }
    t.setFlat(labT, cl, 1.0);
    t.runTrain();
    double l = t.loss();
}
```

- **label 形状随损失**：CE/Dice one-hot（softmaxCE 依赖 label>0.5，非标量）；
  **MSE/BCE**（lossMode 2/3）label **同输出形状**灌实数目标/0-1 掩码——无需
  Softmax 分类头，模型输出本身即预测张量（回归 `mse_reg.json`/`json_mse_train_main`
  500 步 loss→0；二值 `bce_clf.json`/`json_bce_train_main` 600 步→floor 0.325）。
- **每次 run 前重建输入**：训练 arena 复用会覆盖输入区。
- **Session GPU 训练统一（P10a）**：`runTrainAuto()` 训练步——MYP_GPU=1 且
  `gpuTrainReady()`（图内全部 opKind 有 GPU 分派槽；梯度累积/AMP 模拟除外）→ 首步
  `gpuPersistentStart` + 每步 `runGpu`（增量上传置脏输入 + 全量 D2H），否则回退 CPU
  `runTrain`（绝不静默错）。结束 `trainGpuEnd()`。GPU 反向已补：ReLU6/LeakyRelu/SiLU/
  HardSwish/Clip/LogSoftmax/Reshape bwd + MSE/BCE loss。**已知 GPU 局限（P10b）**：
  fan-in（同输入多分支）训练网 GPU 不收敛（Sub/Mul/Add 汇合、swiglu；CPU 正常）——
  相关 main 仍 CPU；BN/IN/BN-NHWC/batch-MatMul/Reduce/Transpose/Expand/Tile/Gather/Pad
  的 bwd 仍 CPU-only（`gpuTrainReady` 自动回退）。
- **可训结构**：链式 + fan-in 汇合（`Add`/`Sub`/`Mul`/`Concat` 在 loss 路径）均可训
  降（回归 `json_train_submul_main`：sub/mul/add 三网 200 步 loss 显著降）。**激活反向**
  全覆盖：`Relu`/`Sigmoid`/`Tanh` 早已支持；`ReLU6`/`LeakyRelu`/`SiLU`/`HardSwish`/
  `LogSoftmax`/`Clip` 亦已接通（CPU-only 训练，`bwd_activ_main` 数值对拍；`SiLU`+
  `Mul` SwiGLU 可训——`swiglu.json`/`json_swiglu_train_main` 200 步 loss 1.09→0.004；
  激活链 `activ_chain.json`/`json_activ_chain_train_main` LeakyRelu→ReLU6→HardSwish
  300 步 loss 0.97→0.64）。**CNN 也**
  可训**（`cnn_train.json`/`json_cnn_train_main`：Conv→Relu→MaxPool→Flatten→FC→
  Softmax，200 步 loss 1.01→0.14；`gap_cnn.json`/`json_gap_cnn_train_main`：
  Conv→Relu→GlobalAveragePool→FC，200 步 loss 1.06→0.90；`avgpool_cnn.json`/
  `json_avgpool_cnn_train_main`：Conv→Relu→AveragePool→FC，200 步 loss 1.04→0.43——
  Conv/MaxPool/AvgPool/GAP/Flatten 反向全链可用，4D 经 Flatten 回 2D 接 FC+softmaxCE）。fan-out
  到多个训练反向 op 的中间层梯度累加仍在 5c(BwdConcat) 覆盖内（见 CHANGELOG）。
- **优化器/累积/AMP**：`setGradAccumEvery(K)` micro-batch 累积；`setAmpSim(1)` fp16
  梯度舍入模拟。GPU 训练当前 CPU-only（runTrain）。

## 5. checkpoint

```myp
t.dumpPlan("/tmp/ckpt.bin");     // 训练中途：op/tensor 表 + arena 权重序列化
Session t2 = new Session();
t2.loadPlan("/tmp/ckpt.bin");    // 新 Session 跳过 ONNX/JSON 解析直接恢复，续训
// 约定：loadPlan 会话无 loader，按名 tensorId 返回 -1——续训用 dump 前记录的 tid
//      经 setFlat/getFlat（feedInputByTid 语义）；label 同 one-hot。
```

## 6. 诊断

```myp
s.phase(); s.loadError(); s.compileError(); s.runError();   // 各阶段错误码
s.compileMs(); s.lastRunMs(); s.lastRunOps();               // 统计
s.dumpGraph(); s.dumpIR(); s.dumpMem();                     // 结构/计划/内存 dump
```

加载失败先看 `phase`/错误码，别猜段错误。诊断栈：加载/编译/运行三阶段错误分开。

## 7. 常见陷阱速查

- `import dl` 编译必须 `--package-path examples/deeplearning`。
- 程序数据路径相对 `examples/`（`cd examples` 后跑；`./deeplearning/...`）。
- `Console.writeFloat` 每次 write 自带换行且精度低 → 打印 loss/数值用
  `int(L * 1000)` 量化或 `Fmt.i`。
- 2D `MatMul` 框架走 4D batch 路径——JSON 请以 4D batch 形状声明或直接用 `Gemm`。
- `ref` 是 MYP 保留字（变量名用 `refv`）。
- 图输出若同时是多分支/多输出的中间，`MYP_NO_REUSE=1` 可逐层对拍。

## 8. 参考实现

`infer_tests/*_main.myp` 是各能力的最小可跑示例：
`json_model_main`（JSON 推理+训练）、`json_branch_main`（JSON 多分支推理）、
`json_train_submul_main`（JSON 训练含 Add/Sub/Mul 汇合）、`json_safe_main`
（JSON safetensors 权重源）、`sli_fit_main`（训练完整循环 + checkpoint 续训）、
`sli_opt_main`（优化器）、`sli_acc_main`（梯度累积）、`sli_amp_main`（AMP 骨架）、
`multiio_main`（多输入/输出/可选输入）。
