# DeepLearning Inference Framework (MYP)

A general-purpose, static-graph **inference + training** framework implemented in pure MYP,
driven by real ONNX models. No Python / onnxruntime at runtime.

> ⚡ **当前状态（2026-09-02，阶段一~九已全量落地）**：本文件后半部分是早期
> （阶段一~四 CNN/ResNet）逐阶段实现的详细历史记录；**现状与上手见下**。

## 现状总览（2026-09-02）

**算子覆盖（~80 opKind，CPU `ops.myp` + GPU `gpu_ops.myp`，全部 ORT 位精确对拍）**
CNN（Conv/Conv3D/1x1/Pool/GAP/BN/IN/LayerNorm/Resize/Pad…）、FC（Gemm/MatMul/BatchMatMul/
cuBLAS）、张量变换（Transpose/Slice/Concat/Split/Reshape/Expand/Where/Tile/Squeeze/Gather/
Reduce 族/LogSoftmax/Softmax/CE…）、3D（Conv3D/Pool3D/Pad3D/Resize3D）、激活全族、训练反向
算子、Dropout 推理/训练语义。真实模型：**ResNet18/ResNet50**（vs ORT 数值一致）、**3D U-Net**
（coarse/fine）、动态 batch（shape specialization 多 bucket）、多输入/多输出/可选输入、
FP16/BF16 权重、量化前全精度。

**图优化 pass 管线**（graph_optimizer）：常量折叠 → inferShapes → classifyShapes →
fuseConvBN → fuseConvRelu → GAP+Flatten → DCE → **常量去重 / 死权重裁剪 / 形状值传播 /
Conv1x1 lowering / 算子选择** → NHWC(opt-in) → topoSort → 内存规划 → buildRuntime；
`MYP_IR_VERIFY=1` 触发五重 verifier（verifyIR/verifyShapes/verifyDefUse/verifyTopo/
verifyRuntimeWiring）。

**模型工程（阶段七）**：opset/version 检查、unsupported op 诊断（If/Loop/Scan 明确拒绝）、
external-data 权重、模型 mmap 零拷贝加载、优化后 IR/计划缓存 `dumpPlan/loadPlan`、
shape specialization、加载/编译/执行三阶段错误码 + 统计。

**训练（阶段四，Session 统一）**：静态反向图（bwd 梯度 + Update）；优化器
SGD/动量/AdamW + weight decay；**梯度累积**（micro-batch）；**AMP 数值管线骨架**
（fp16 梯度舍入模拟）；**checkpoint**（dumpPlan 中途保存 / loadPlan 恢复续训）。

**SLI（阶段九，统一入口）**：`import dl;` 一个 import 即得全部能力 ——
```myp
import dl;                       // 编译加 --package-path examples/deeplearning
Session s = new Session();
s.loadTrain("model.onnx");       // 或 load/loadMmap/loadTrainMmap；setInputShape 可注入动态维
s.setInput("data", buf, n);      // 或 loadInputFromFile(name, f32)
s.runAuto();                     // MYP_GPU=1 → GPU，否则 CPU
double[] out = s.getOutput("y");
s.dumpGraph(); s.dumpIR(); s.dumpMem();   // 结构/计划/内存 dump
// 训练：setLr/setOptimizer(0|1|2)/setWeightDecay/setGradAccumEvery/setAmpSim
//       setTrainMode(1) → runTrain() → loss()；dumpPlan/loadPlan checkpoint
// 声明式 JSON 模型（不经过 ONNX）：s.loadJson("net.json") / s.loadJsonTrain
//   ——层式 JSON 描述网络，即 DAG：fan-out 靠名字引用、fan-in 用 in2/in3/in4 槽
//      op：Gemm/Conv/Pool/Relu/Sigmoid/Softmax/GlobalAveragePool/Flatten +
//          二元 Add/Sub/Div/Mul/MatMul(in+in2) + 多输入 Concat(in2/in3+axis)
//      权重 init 或 safetensors 源 W.safetensors{file,tensor} 直接读值
//   ——权重 B 可选（无 bias Gemm）；2D MatMul 已支持；Add/Sub/Mul/Div 训练反向已补
// 完整上手/陷阱/示例：docs/sli.md（本文件仅速览）
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
../infer_tests/        # 端到端回归（78 个 *_main.myp，见 README.md）
../train ../llm ../diffusion   # 相邻分项目（3D 训练 / Qwen2+distilgpt2 / SD1.5）
```

**编译 / 运行 / 回归**
```bash
./build/mypc examples/deeplearning/infer_tests/your_app.myp -o /tmp/app \
    --stdlib stdlib --package-path examples/deeplearning
cd examples && MYP_GPU=1 MYP_IR_VERIFY=1 /tmp/app     # 数据路径相对 examples/
# 全量回归（CPU+GPU，自动发现 infer_tests/*_main.myp）：
bash /tmp/run_infer_tests.sh    # → == pass=78 fail=0 ==
# 关键环境变量：MYP_GPU=1 GPU / MYP_IR_VERIFY=1 verifier / MYP_NO_REUSE=1 逐层对拍
#   / MYP_PROF_CPU|GPU=1 剖析 / MYP_LAYOUT_NHWC=1 / MYP_FAST_MATH=1
```

---

以下为早期（阶段一~四 CNN/ResNet）逐阶段实现记录与架构细节。

A general-purpose, static-graph inference framework implemented in MYP, driven by real ONNX models.

> 📚 相关文档（`deeplearning/docs/`）：
> - [`design.md`](../docs/design.md) — 架构设计说明（运行时 / opKind / 图 pass 管线 / 扩展指南）
> - [`usage.md`](../docs/usage.md) — 使用说明（构建运行 / 环境变量 / 回归测试 / 交叉校验）
> - [`gpu_paradigm.md`](../docs/gpu_paradigm.md) — GPU 范式库 + 推理框架路线图（M1-M4 / G1-G4）
>
> 📦 JSON 图小工具（XOR/MNIST 演示、通用 CLI、GPU 卸载验证）已移出，见
> [`../json_tool/`](../json_tool/README.md)。
> 🧪 端到端验证入口（r18/resnet/bn/act/const/onnx `*_main.myp`）已移出，见
> [`../infer_tests/`](../infer_tests/README.md)。

## Capabilities

- **CPU, static-graph execution** with a flat arena tensor memory
- **Batch-aware operators** (batch = tensor columns, one sample per column)
- **ONNX model loading** — pure-MYP protobuf reader + graph-pass optimizer pipeline
- **GPU offload** — `runGpu()` with device-resident kernels, automatic CPU fallback
- **Graph fusions** — Conv+BN / Conv+ReLU / GAP+Flatten / DCE / constant folding / NHWC layout
- **Memory planning** — first-fit region reuse for activations, persistent weights

## Layout

```
deeplearning/infer/
├── runtime.myp       # InferenceRuntime: tensor/op registry + run()/runGpu()
├── ops.myp           # CPU operator kernels (FP32)
├── gpu_ops.myp       # GPU operator kernels (@gpu for resident)
├── tensor.myp        # tensor indexing helpers
├── pb.myp            # protobuf wire-format reader (varint / len-delimited / skip)
├── onnx_loader.myp   # pure-MYP ONNX reader + graph optimizer (pass pipeline)
├── tools/
│   ├── make_mnist_mlp_onnx.py  # test-fixture generator: mnist_weights.bin -> mnist_mlp.onnx
│   ├── cross_check_onnx.py     # onnxruntime cross-validation (accuracy + sample0 probs)
│   └── prep_imagenet_input.py  # real photo -> 224x224 ImageNet-normalized f32 input
└── (纯核心：无验证入口；端到端验证在 ../infer_tests/，JSON 小工具在 ../json_tool/)
```

## Reading Real ONNX Models

`onnx_loader.myp` is a **pure-MYP protobuf wire-format reader** — no Python, no onnxruntime at
runtime. It opens a real `.onnx` file, parses the `ModelProto`/`GraphProto`/`NodeProto`/
`TensorProto`/`ValueInfoProto` messages directly, infers shapes, classifies weights/biases
(including `transB`), and builds the `InferenceRuntime` graph.

```bash
# MNIST MLP from a real .onnx — prints sample0 probs + accuracy
./build/mypc deeplearning/infer/onnx_main.myp -o /tmp/infer_onnx --stdlib stdlib && /tmp/infer_onnx
# → ONNX MLP accuracy: 78% (78/100), sample0 prob matches onnxruntime
```

Supported ops: FC subset — `Gemm` (with `transB`, optional bias `C`), `MatMul`, `Add`,
`Relu`, `Sigmoid`, `Softmax` (with `axis`); **CNN subset — `Conv` (strides/pads/dilations/
group), `MaxPool`, `GlobalAveragePool`, `Flatten`**. Weights may come from `float_data`/
`raw_data`/`double_data`; dims are read from the non-packed `repeated int64 dims` field.

**G1 — graph-level Conv+ReLU fusion**: after shape inference, `OnnxLoader.fuseConvRelu()`
replaces each `Conv → Relu` pair (relu input == conv output, conv output consumed only by that
relu, conv output is not a graph output) with a single fused op (`opKind=11`, `addConvRelu`;
CPU `InferOps.convRelu` + GPU `GpuInferOps.convRelu` compute the convolution and apply ReLU
in one kernel, never writing the intermediate tensor). The dead relu nodes are skipped and
`planMemory` is fusion-aware (the fused conv's output becomes the relu output; the vacated
intermediate region is immediately reusable). For ResNet50 this cuts GPU kernel launches
**122 → 89**; top-5 stays bit-identical to CPU/onnxruntime (runtime ~66ms on GPU — flat vs
baseline, since conv compute is the bottleneck and ReLU is cheap; the win is fewer launches
+ less intermediate traffic, and it paves the way for G2 layout transforms).

**G2 — graph-level advances**: the loader now runs a small graph-pass pipeline
(`foldConstants → inferShapes → classifyShapes → fuseConvRelu → fuseGapFlatten →
eliminateDeadNodes → layoutNHWC(opt-in) → topoSort → planMemory → buildRuntime`):
- **GAP+Flatten fusion** (`fuseGapFlatten`): a `GlobalAveragePool → Flatten` pair becomes a
  single GAP that writes straight into the flatten output tensor (batch==1 guard), removing one
  copy + the `[N,C,1,1]` intermediate. ResNet50 kernel launches **89 → 88**.
- **DCE** (`eliminateDeadNodes`): fixpoint removal of nodes whose output has no live consumer
  and is not a graph output; fused-away intermediates are marked `shDead_` so `planMemory`/
  `buildRuntime` never allocate or register them.
- **Constant folding** (`foldConstants`): `Constant` nodes whose `value` is a float tensor are
  folded into persistent tensors (new kind `FC_C`, direct-copy role) and the node is marked dead
  — so models containing `Constant` nodes load even though no runtime op executes them.
- **Layout transform NCHW→NHWC** (`layoutNHWC`, opt-in via `MYP_LAYOUT_NHWC=1`): the CNN
  backbone is switched to NHWC by inserting one `NCHW2NHWC` transpose at the graph input,
  transposing conv weights at load time `[Cout,Cin,KH,KW]→[Cout,KH,KW,Cin]`, registering NHWC
  tensors with `(N,H,W,C)` metadata, and using dedicated NHWC kernels (opKinds 12-16, CPU+GPU).
  GAP is the layout-agnostic boundary (reads NHWC, emits `[N,C]`). A topological sort
  (`topoSort`, Kahn) reorders the node list so the appended transpose runs before its consumers.
  Results: numerically identical output (sum 336.658, same top-5) in both layouts; **CPU gets
  ~39% faster (14.4s → 8.8s)** thanks to contiguous channel reduction, while the naive
  per-thread GPU kernel is slower under NHWC (66ms → 208ms, non-coalesced weight reads without
  shared-memory tiling) — so the pass stays opt-in and NCHW remains the GPU default.

Constant folding is end-to-end validated by `const_main.myp` against
`tools/…/const_fold_test.onnx` (Constant→Add→Relu, output matches onnxruntime exactly); the
NHWC kernels are unit-tested in `tests/@test/graph_opt.myp` (4 cases / 214 assertions) for
numerical equivalence against their NCHW counterparts.

**G3 — BatchNormalization** (the most common op missing from the original CNN subset):
- **Conv+BN fusion** (`fuseConvBN`, runs before G1): a `Conv → BatchNormalization` pair where
  the conv output is consumed only by that BN is folded — per channel,
  `invStd[c]=scale[c]/sqrt(var[c]+eps)`, `W'[c]=W[c]*invStd[c]`,
  `B'[c]=(B_conv[c]-mean[c])*invStd[c]+B_bn[c]`. The fold is applied in `writeWeight` (it
  commutes with the NHWC weight transpose), the BN node + its 4 parameter tensors are marked
  dead, and the conv's effective output becomes the BN output — so the following G1 pass fuses
  `Conv+Relu` too, collapsing `Conv→BN→Relu` into a **single op**.
- **Standalone BN op** (for non-fusable BN, e.g. after `Add`): opKind 17 (NCHW) / 18 (NHWC),
  kernel computes `y=(x-mean[c])*invStd[c]+bias[c]` per channel; scale/bias/mean/var are `[C]`
  initializers. `epsilon` is captured from the node attribute (float, wire-type-5 4 bytes) and
  resolved to a `double` in the runtime dispatch.
- The loader now reads BN inputs 3/4 (`nIn3_/nIn4_`), and `fuseConvRelu` matches on the conv's
  effective output (so BN-folded convs still fuse with Relu).
- Verified end-to-end by `bn_main.myp` against `bn_fold_test.onnx` (Conv→BN→Relu → 1 op) and
  `bn_standalone_test.onnx` (x→BN→Relu → 2 ops): outputs match onnxruntime element-wise
  (max diff < 1e-6) on CPU and GPU, in both NCHW and NHWC layouts. Kernel-level tests in
  `tests/@test/bn_opt.myp` (2 cases / 84 assertions). Regression **236/236**.

**G4 — general activations + a real BN-bearing model**:
- **5 new activation ops** (opKind 19-23, CPU `ops.myp` + GPU `gpu_ops.myp`):
  `ReLU6`, `LeakyRelu` (alpha passed via op params, resolved to double in dispatch),
  `SiLU` (x·sigmoid(x)), `HardSwish`, and `Clip` (min/max scalar params). Kernel-level tests in
  `tests/@test/act_opt.myp` (5 cases / 48 assertions, incl. a manual SiLU reference since
  onnxruntime 1.28 doesn't register SiLU).
- **Clip is the real-model primitive, not ReLU6**: ReLU6 isn't a standard ONNX op — real models
  express it as `Clip(x,0,6)`. Since opset 13, Clip's min/max come in as **input tensors**; the
  loader resolves them from initializers to scalar params. `act_main.myp` validates
  `act_test.onnx` (opset 14, parallel Clip/LeakyRelu/HardSwish branches) against onnxruntime on
  CPU/GPU/NHWC (max diff < 3e-8).
- **Real ResNet18** (`resnet18_v1_7.onnx`, ONNX Model Zoo, 45MB): 69 nodes (20 Conv + 20 BN +
  17 Relu + 8 Add + MaxPool + GAP + Flatten + Gemm) with **all 20 convs bias-free** (Caffe2
  export style — the bias is only present via the following BN fold). `r18_main.myp` loads it
  (bn_fused=20, ops=39) and matches onnxruntime exactly: **output sum 0.101238, top-5
  [975,976,978,977,449]**, CPU ~5s / GPU **51ms**.
- **Three bugs this surfaced** (all fixed):
  1. Bias-free conv segfault: dispatch did `tOff_[-1]` when `bc=-1`. All 8 conv dispatch sites
     + all 4 conv kernels now guard `bOff >= 0`.
  2. Lost bias after BN fold: with no conv-bias tensor there was nothing to fold into, so
     `fuseConvBN` now **synthesizes** a bias tensor (`<conv>#bnb`) computing
     `B'[oc]=(0-mean[oc])*invStd[oc]+B_bn[oc]`.
  3. **Root bug — `nFused_` flag overload**: `buildRuntime`'s Conv branch checked `nFused_==1`
     to emit the fused Conv+ReLU, but `nFused_` is set by **both** `fuseConvBN` and
     `fuseConvRelu`, so a BN-only fused conv (feeding a residual Add, not a ReLU) wrongly got
     ReLU applied. Fixed by adding a dedicated `nRelu_` flag set only by `fuseConvRelu`; a
     regression case `bn_norelu_test.onnx` (Conv→BN→Add→Relu, 1 fused conv, 3 ops) now passes
     on CPU/GPU/NHWC (max diff 3.8e-6).
- **Debugging aid**: intermediate activations are region-reused, so reading them post-run gives
  garbage — `MYP_NO_REUSE=1` disables region reuse (`noReuse_`) to get valid intermediate
  values; a `tensorOff(tid)` accessor reports a tensor's arena offset.
- Regression **237/237**; ResNet50 (336.658 / 88 ops) and MNIST 78/100 unchanged.

CNN runtime model: tensors are 4D NCHW row-major (`rows = N*C`, `cols = H*W`), the arena is a
**dynamically sized `float[]` (FP32)** sized by a first-fit memory planner (`planMemory`) that
reuses dead activation regions to cut peak arena memory (~15M floats ≈ 60MB for ResNet50 vs
~400MB if kept all at once at FP64). Weights / graph inputs / graph outputs are marked
**persistent** and never reused (required for multi-pass inference loops — reusing a weight
region would corrupt weights on the 2nd sample).

Precision: the whole runtime is **FP32** (like onnxruntime). `setFlat`/`getFlat` take/return
`double` for caller convenience but round to `float` at the arena boundary; all kernels compute
in `float`. MYP gotchas found while doing this: no `f` literal suffix (use `float(1.5)` cast),
no implicit float↔double in `return` position (assign to a `double` temp first), and float must
not be compared against a `double` literal (`v > float(0.0)`).

Key ONNX field numbers (verified against the spec + real files):

| Message      | Fields |
|--------------|--------|
| `ModelProto` | graph = **7**, opset_import = **8** |
| `GraphProto` | node = 1, name = 2, initializer = 5, input = 11, output = 12 |
| `NodeProto`  | input = 1 (repeated), output = 2, name = 3, op_type = 4, attribute = 5 |
| `TensorProto`| dims = 1 (varints, **non-packed**, one per field), data_type = 2, float_data = **4**, name = **8**, raw_data = **9**, double_data = **10** |
| `ValueInfoProto` | name = 1, type = 2 → TypeProto.tensor_type = 1 → elem_type = 1, shape = 2 → dim = 1 → dim_value = 1 / dim_param = 2 |

`transB` semantics (matters for real models): with `transB=1` the stored weight is `[N, K]`
(the PyTorch `Linear` convention) and is copied straight into the runtime; with `transB=0`
it is `[K, N]` and is transposed to `[N, K]` while writing.

`tools/make_mnist_mlp_onnx.py` regenerates the test fixture from `mnist_weights.bin`;
`tools/cross_check_onnx.py` verifies MYP output against onnxruntime (needs `tools/onnxvenv`).

## ResNet-50 (CNN) — full ImageNet classification in pure MYP

Reads the real 98MB `gluon_resnet50_v1b_Opset16.onnx` (122 nodes: 53×Conv, 49×Relu, 16×Add,
MaxPool, GlobalAveragePool, Flatten, Gemm) with no Python at runtime:

```bash
# 1) preprocess a real photo → 224x224 ImageNet-normalized f32 input
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/prep_imagenet_input.py \
    deeplearning/data/imagenet/picsum.jpg

# 2) run the whole thing in pure MYP
./build/mypc deeplearning/infer/resnet_main.myp -o /tmp/infer_resnet --stdlib stdlib && /tmp/infer_resnet
# → ResNet50 inference: ~14500 ms
#   top-5: [975] lakeside (10.328) / [978] seashore ...  ← matches onnxruntime exactly
```

Verified: MYP top-5 classes and scores are **numerically identical to onnxruntime** on both a
synthetic input and a real photo (scores agree to 5+ significant digits), after the same
resize→center-crop→normalize preprocessing. Conv attr wire format: `ints` is **field 8,
non-packed** (one varint per field); `group/transB/axis` are field 3 (single int);
`pads = [top, left, bottom, right]`.

## GPU Offload

All operators — FC (`dense`/`matmul`/`relu`/`sigmoid`/`softmax`/`add`) **and CNN**
(`conv`/`maxpool`/`gapool`/`flatten`) — have GPU kernels (`gpu_ops.myp`, via `@gpu for`).
The runtime exposes `runGpu()` as a drop-in for `run()`; `resnet_main` auto-selects GPU when
`MYP_GPU=1` and CUDA is available:

```bash
./build/mypc deeplearning/infer/gpu_main.myp -o /tmp/infer_gpu --stdlib stdlib
MYP_GPU=1 /tmp/infer_gpu
# → XOR gpu=4/4 ; MNIST gpu=78/100 (FP32 kernels)

# ResNet-50 on GPU (~2.9 s vs ~14.5 s CPU on an RTX 2070 SUPER — 5x)
./build/mypc deeplearning/infer/resnet_main.myp -o /tmp/infer_resnet --stdlib stdlib
MYP_GPU=1 /tmp/infer_resnet
# → device: NVIDIA GeForce RTX 2070 SUPER (GPU)  ResNet50 inference: ~2900 ms
#   top-5 identical to CPU/onnxruntime

# Without MYP_GPU, @gpu for automatically falls back to CPU (same results)
/tmp/infer_resnet
```

How it works:
- Kernels keep the exact same flat-arena layout as the CPU ops, but the `@gpu for` loop bound is the **whole arena length** (`n`) so the shared arena is fully transferred H2D/D2H; an internal `if (p < work)` guard restricts work to the op's actual output range. (Plain `@gpu for` transfers only `loop_bound × elem_size` bytes of each captured array, which is wrong for offset-based shared-arena access.)
- GPU math (`Math.exp` for sigmoid/softmax) maps to CUDA libdevice automatically.
- Verify with `gpu_main.myp`: XOR 4/4 and MNIST 78/100 on GPU exactly match CPU.

Caveat: kernels run **FP32** (arena is `float[]`), so GPU conv/dense run at full consumer-GPU FP32
rate. `@gpu for` transfers each captured array at its **real byte size** (read from the ref-counted
array header), so parameter/property arrays transfer correctly; the shared arena is transferred per
op (H2D+D2H). The ResNet 5x speedup is limited by per-op transfer + launch overhead — keeping the
arena resident on the GPU across ops (single H2D/D2H) would give a much larger speedup.

### GPU compiler fixes made for FP32 (src/codegen/codegen_gpu.cpp)
- float arrays now transfer at **4 bytes/elem** (was 8 → read/wrote past host float[]).
- transfer byte size = **real array size from header** (`count × elem_size` at obj-24), not
  `loop_bound × elem_size` (fixed over/under-transfer for parameter/property arrays).
- kernel `VarDeclStmt` now allocates the **declared type** and inserts fpext/fptrunc/sitofp/fptosi
  (was: alloca of the init-expression type → `double x = <float expr>` became float and the value
  was bit-reinterpreted as double → garbage).

### cuda.myp GPU compute library (stdlib)
`Matrix.matmul`/`matmulF` (GEMM), `Vectors.dot`/`dotF` (GPU atomic), `Vectors.max`/`min`/`argmax`
(+ `*F` float variants, GPU block reduction) — all verified on GPU vs CPU reference. Fixed the
existing reductions' `double[1]` scratch → `new double[1]` (heap arrays have a header; fixed stack
arrays must not be captured by `@gpu for`).

## Verified

- XOR: code-defined, JSON-driven, and CLI all produce identical probabilities; accuracy 4/4.
- MNIST: single-sample loop and batched single-pass both 78% (78/100) — identical.
- sigmoid/add: operator test PASS.
- GPU (MYP_GPU=1): XOR 4/4, MNIST 78/100 — identical to CPU; CPU fallback identical.
- ONNX: pure-MYP reader loads `mnist_mlp.onnx` → 78/100, sample0 probability vector matches
  onnxruntime to 5+ significant digits (verified by `tools/cross_check_onnx.py`).
- **ResNet-50 CNN**: pure-MYP reads the 98MB real ONNX and classifies a real photo; top-5
  classes + scores are numerically identical to onnxruntime (~14.5 s/forward on CPU).
