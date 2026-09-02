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

- **结构即 DAG**：fan-out 靠名字引用（一个 `out` 名被多个层当 `in`）；fan-in 汇合
  用多输入槽 `in2/in3/in4`（`Add`/`Sub`/`Div`/`Mul`/`MatMul` 二元、`Concat` 多输入）。
- **op 集**：单输入 `Relu`/`Sigmoid`/`Softmax(axis)`/`LogSoftmax(axis)`/
  `GlobalAveragePool`/`Flatten(axis)`；二元 `Add`/`Sub`/`Div`/`Mul`/`MatMul`；多输入
  `Concat(in/in2/in3, axis)`；权重型 `Gemm`/`Conv`/`ConvTranspose`/`MatMul(可选 W)`；
  池化 `MaxPool`/`AveragePool`(kernel/strides/pads)；**参数化（int64 常量）**
  `Reshape(shape)`、`Gather(indices,axis)`、`Expand(shape)`、`Tile(repeats)`、
  `Slice(starts,ends[,axes][,steps])`、`Pad(pads[,mode])`（pads 8 值
  [N,C,H,W] begin+end）；
  **属性类** `Squeeze(axes)`、`Transpose(perm)`、`ReduceSum`/`ReduceMean`/`ReduceMax`/
  `ReduceMin(axes[,keepdims])`。示例：`infer_tests/branch.json`（多分支 DAG）、
  `mlp.json`、`safe_gemm.json`、`reshape.json`、`gather.json`、`ops2.json`。
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
  收集 op 现可出现在 loss 路径）。

## 4. 训练

```myp
Session t = new Session();
int ok = t.loadTrain("model.onnx");      // 或 loadJsonTrain("net.json")
t.setLr(0.01); t.setOptimizer(2); t.setWeightDecay(1e-4);   // AdamW
t.setTrainMode(1);
int labT = t.tensorId("label");          // 训练自动补的 label（one-hot）
// 每步：喂输入 + label（one-hot；softmaxCE 依赖 label>0.5）→ runTrain → loss
while (step < N) {
    t.setInput("data", bx, 784);
    for (i...) { t.setFlat(labT, i, 0.0); }
    t.setFlat(labT, cl, 1.0);
    t.runTrain();
    double l = t.loss();
}
```

- **label 是 one-hot**（softmaxCE 依赖 label>0.5），非标量。
- **每次 run 前重建输入**：训练 arena 复用会覆盖输入区。
- **可训结构**：链式 + fan-in 汇合（`Add`/`Sub`/`Mul`/`Concat` 在 loss 路径）均可训
  降（回归 `json_train_submul_main`：sub/mul/add 三网 200 步 loss 显著降）。fan-out
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
