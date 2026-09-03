# DeepLearning Inference & Training Framework (MYP)

A general-purpose, static-graph **inference + training** framework implemented in pure MYP,
driven by real ONNX models *and* declarative JSON graphs. No Python / onnxruntime at runtime.

> 📖 This is the English README. 中文版见 [`README.md`](README.md).

## Current Status (2026-09-03, stages 1–9 + activations/UNet)

**Operator coverage (~82 opKind, CPU `ops.myp` + GPU `gpu_ops.myp`, all bit-verified vs onnxruntime)**
CNN (Conv/Conv3D/1x1/Pool/GAP/BN/IN/LayerNorm/Resize/Pad…), FC (Gemm/MatMul/BatchMatMul/cuBLAS),
tensor ops (Transpose/Slice/Concat/Split/Reshape/Expand/Where/Tile/Squeeze/Gather/Reduce family/
LogSoftmax/Softmax/CE…), 3D (Conv3D/Pool3D/Pad3D/Resize3D/ConvTranspose3D), **indexing / data advanced
indexing (ArgMax/ArgMin/TopK/OneHot/GatherElements/ScatterND — GatherElements/ScatterND with P8b
backward gradients for training)**, **the full activation family**, LLM (RmsNorm/LayerNorm/GELU/
**Rope** rotary position embeddings), training backward ops, Dropout infer/train semantics. Real models:
**ResNet18/ResNet50** (bit-identical
vs ORT), **3D U-Net** (coarse/fine), dynamic batch, multi-input/output/optional-input, FP16/BF16
weights.

**Graph-pass pipeline** (`graph_optimizer`): constant folding → inferShapes → classifyShapes →
fuseConvBN → fuseConvRelu → GAP+Flatten → DCE → dedup/dead-weight prune/shape propagation/
Conv1x1 lowering/op selection → NHWC (opt-in) → topoSort → memory plan → buildRuntime;
`MYP_IR_VERIFY=1` runs a five-fold verifier (IR/shapes/def-use/topo/runtime wiring).

**Training (Session unified)**: static reverse graph (bwd gradients + Update); SGD/Momentum/AdamW
+ weight decay; gradient accumulation; AMP simulation; checkpoint via `dumpPlan`/`loadPlan`.
**Activation backward is now complete**: `Relu`/`Sigmoid` (long supported) plus `ReLU6`/
`LeakyRelu`/`SiLU`/`HardSwish`/`LogSoftmax`/`Clip` (CPU-only training; `bwd_activ_main` numeric
checks). **SwiGLU is trainable** (`swiglu.json`: fan-out double Gemm + SiLU + Mul, 200 steps
loss 1.09→0.004) and so are CNN chains (`cnn_train`/`gap_cnn`/`avgpool_cnn` JSONs). Training graphs
skip Conv+Relu-style fusions so reverse-mode AD walks every node.

**Declarative JSON models (no ONNX)**: `loadJson`/`loadJsonTrain` — a layer-style JSON is filled
straight into the same Graph pipeline. Edges are expressed by tensor names (`out` → `in`; fan-out
by name, fan-in via `in2/in3/in4`). Weights via deterministic init, inline `values`, or
`.safetensors`. A **2D U-Net** (`unet2d.json`, encoder Conv+MaxPool / decoder ConvTranspose +
skip Concat) runs end-to-end on CPU+GPU.

**SLI (unified entry, stage 9)**: `import dl;` gives everything —
```myp
import dl;                       // compile with --package-path examples/deeplearning
Session s = new Session();
s.loadTrain("model.onnx");       // or load/loadMmap/loadTrainMmap; setInputShape injects dynamic dims
s.setInput("data", buf, n);      // or loadInputFromFile(name, f32)
s.runAuto();                     // MYP_GPU=1 → GPU, else CPU
double[] out = s.getOutput("y");
s.dumpGraph(); s.dumpIR(); s.dumpMem();   // structure / plan / memory dumps
// Training: setLr/setOptimizer(0|1|2)/setWeightDecay/setGradAccumEvery/setAmpSim
//           setTrainMode(1) → runTrain() → loss(); dumpPlan/loadPlan checkpoint
// Declarative JSON models: s.loadJson("net.json") / s.loadJsonTrain — layer-style DAG
//   op set + full walkthrough/traps: docs/sli.md
```

**Layout & tests**
```
infer/
├── framework.myp      # SLI facade: class Session (load/run/train/dump/checkpoint)
├── runtime.myp        # InferenceRuntime: tensor/op registry + run()/runGpu() + optimizer/accum state
├── ops.myp / gpu_ops.myp / ops_iface_all.myp / op_iface.myp   # CPU/GPU kernels (interface dispatch)
├── graph*.myp         # Graph domain: analysis/compiler/optimizer/planner/shapes/weights/defs
├── onnx_loader.myp    # pure-MYP protobuf ONNX reader + training graph build + error codes
├── pb.myp             # protobuf wire reader (incl. F32/half helpers)
├── safetensors.myp    # safetensors weight reader (mmap)
├── tensor.myp / graph_node_attrs.myp / graph_nodes.myp
├── tools/             # make_*_onnx.py fixtures + ORT reference; onnxvenv
../dl/dl.myp           # import dl package entry (thin forward of framework.myp Session)
../infer_tests/        # end-to-end regression (124 *_main.myp, see infer_tests/README.md)
../train ../llm ../diffusion   # sibling projects (3D training / Qwen2+distilgpt2 / SD1.5)
```

**Build / run / regression**
```bash
./build/mypc examples/deeplearning/infer_tests/your_app.myp -o /tmp/app \
    --stdlib stdlib --package-path examples/deeplearning
cd examples && MYP_GPU=1 MYP_IR_VERIFY=1 /tmp/app     # data paths are relative to examples/
# full regression (CPU+GPU, auto-discovers infer_tests/*_main.myp):
# full regression (CPU+GPU; parallel: compile P4 + run P6):
bash examples/deeplearning/infer_tests/run_all.sh    # → == pass=124 fail=0 == (~4min)
# env: MYP_GPU=1 GPU / MYP_IR_VERIFY=1 verifier / MYP_NO_REUSE=1 per-op check
#      / MYP_PROF_CPU|GPU=1 profiling / MYP_LAYOUT_NHWC=1 / MYP_FAST_MATH=1
```

**Related docs** (`deeplearning/docs/`):
- [`design.md`](../docs/design.md) — architecture (runtime / opKind / pass pipeline / extending)
- [`usage.md`](../docs/usage.md) — usage (build-run / env vars / regression / cross-validation)
- [`sli.md`](../docs/sli.md) — the SLI quickstart (Session API, JSON syntax, training, traps)
- [`gpu_paradigm.md`](../docs/gpu_paradigm.md) — GPU paradigm library + roadmap (M1-M4 / G1-G4)

---

# Early implementation notes (stages 1–4: CNN / ResNet / GPU)

The following sections record the original stage-by-stage work (English). The Chinese
quick-start above supersedes them for current usage.

## Capabilities

- **CPU, static-graph execution** with a flat arena tensor memory
- **Batch-aware operators** (batch = tensor columns, one sample per column)
- **ONNX model loading** — pure-MYP protobuf reader + graph-pass optimizer pipeline
- **GPU offload** — `runGpu()` with device-resident kernels, automatic CPU fallback
- **Graph fusions** — Conv+BN / Conv+ReLU / GAP+Flatten / DCE / constant folding / NHWC layout
- **Memory planning** — first-fit region reuse for activations, persistent weights

## Layout (early)

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
└── (pure core only: end-to-end verification lives in ../infer_tests/)
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
