# MYP Deep-Learning Framework — User Manual (examples/deeplearning)

> Version: **2026-09-03** · Language: English (中文版 `manual.md`)
> Covers the **inference + training** framework under `examples/deeplearning` — pure MYP,
> zero Python / onnxruntime at runtime; **ONNX and declarative JSON** model sources;
> CPU + GPU backends.
>
> **This manual is the single user document**: it has absorbed and replaces the former
> `docs/sli.md` (`import dl` + JSON op catalog) and `docs/usage.md` (infer usage/tools/
> cross-validation) — those two files are no longer kept.
> For architecture see `docs/design.md` (incl. `design_EN.md`); status overview
> `../README.md`; milestones `CHANGELOG.md`.

---

## Contents

1. What the framework can do (overview)
2. Environment & setup
3. Build / run / environment variables
4. Inference: quick start + generic runner
5. Training: quick start
6. ONNX model source (authoring / support / tools / cross-validation)
7. JSON model source (wiring / activations / weights / op catalog)
8. Session API reference (capability table + checkpoint + diagnostics)
9. Training in depth (losses / GPU training / convergence)
10. Validation & regression (end-to-end + op-level @tests + Python tools)
11. Data files
12. FAQ / gotchas
13. Reference docs & sample index

---

## 1. What the framework can do (overview)

- **Inference**: load an ONNX or JSON model → graph-optimization pass pipeline → CPU
  `run()` / GPU `runGpu()`, element-wise consistent with onnxruntime. Verified real
  models: ResNet18/50, 3D U-Net (coarse/fine).
- **Training**: the same runtime graph trains via a static reverse graph — label + loss +
  backward + weight Update are added automatically; CPU and GPU (`runTrainAuto`) backends.
- **Op coverage (~82 opKinds)**: CNN (Conv/Conv3D/1x1/Pool/GAP/BN/IN/LayerNorm/…),
  FC/MatMul (incl. cuBLAS), tensor transforms (Transpose/Slice/Concat/Split/Reshape/
  Expand/Where/Tile/Gather/Reduce family…), indexing (ArgMax/TopK/OneHot/GatherElements/
  ScatterND), 3D (Conv3D/Pool3D/Pad3D/ConvTranspose3D/GAP3D), the full activation family,
  normalization (BN/IN/GroupNorm/LayerNorm/RmsNorm), LLM (RoPE positional encoding),
  and training backward ops (activation/pooling/normalization/data-layout/broadcast/
  reduce/indexing/batch-MatMul backward, full coverage). See §7/§9.
- **GPU training**: `runTrainAuto()` decides automatically (GPU dispatch for every op →
  GPU, otherwise CPU fallback); GPU backward for fan-in merges + BN/IN/batch-MatMul etc.
  is complete (P10a/P10b).

---

## 2. Environment & setup

- **Compiler**: repo-root `./build/mypc` (self-hosted; see root `CMakeLists.txt`).
- **Stdlib / package path**: always compile with `--stdlib stdlib`; `import dl` also
  requires `--package-path examples/deeplearning` (`import dl` resolves `dl/dl.myp`).
- **GPU (optional)**: NVIDIA + CUDA driver; `MYP_GPU=1` uses CUDA when available,
  otherwise auto-falls back to CPU.
- **Data**: models/inputs under `examples/deeplearning/data/` (git-ignored, see §11).
- **Python (optional; generation/validation only, never in the runtime path)**:
  `infer/tools/onnxvenv` (onnx + onnxruntime) for synthesizing models and ORT references.

---

## 3. Build / run / environment variables

```bash
# Compile (repo root cwd): import dl requires --package-path
./build/mypc your_app.myp -o /tmp/app --stdlib stdlib --package-path examples/deeplearning

# Run: in-program data paths are relative to examples/ (cd examples first)
cd examples && /tmp/app                          # CPU
cd examples && MYP_GPU=1 /tmp/app                # GPU (auto CPU fallback without GPU)
cd examples && MYP_GPU=1 MYP_IR_VERIFY=1 /tmp/app  # + five-way graph/runtime verifier
```

### Environment variables

| var | default | effect |
|-----|---------|--------|
| `MYP_GPU` | unset | `1` → `runGpu()`/`runAuto` uses CUDA (auto CPU fallback) |
| `MYP_LAYOUT_NHWC` | unset | `1` → loader enables NCHW→NHWC layout transform (G2 experimental; GPU NCHW faster by default) |
| `MYP_IR_VERIFY` | unset | `1` → five-way verifier (verifyIR/Shapes/DefUse/Topo/RuntimeWiring) |
| `MYP_NO_REUSE` | unset | `1` → disable memory-region reuse (debug: intermediates readable after inference) |
| `MYP_PROF_CPU` / `MYP_PROF_GPU` | unset | `1` → per-op timing profile |

### End-to-end verification entries (`deeplearning/infer_tests/`)

Each `*_main.myp` is a minimal runnable sample of one capability (loads a real/synthetic
model and prints OK/FAIL):

| entry | verifies |
|-------|----------|
| `r18_main.myp` | ResNet18 (20 BN fused) top-5/output-sum vs ORT (CPU ~5s / GPU ~51ms) |
| `resnet_main.myp` | ResNet50 inference top-5 + timing (GPU 66ms) |
| `bn_main.myp` | BN end-to-end 3 cases (fold / standalone / bn_norelu) |
| `act_main.myp` | activations end-to-end (Clip/LeakyRelu/HardSwish vs ORT) |
| `const_main.myp` | constant folding end-to-end |
| `onnx_main.myp` | MNIST MLP inference + accuracy (78/100) |
| `tensorops_main.myp` / `slice_main.myp` / `ops2d_main.myp` | F8 tensor ops / Slice / 2D generic ops vs ORT |
| **`run_onnx.myp`** | **generic ONNX runner**: any model, zero boilerplate → top-k/output/.bin (auto IO detection) |

ResNet18 example:

```bash
./build/mypc deeplearning/infer_tests/r18_main.myp -o /tmp/r18 --stdlib stdlib
/tmp/r18                      # CPU
MYP_GPU=1 /tmp/r18            # GPU, output sum 0.101238, matches CPU/ORT
```

---

## 4. Inference: quick start

One `import dl;` + one `Session`:

```myp
import dl;
class App {
    action:
        @constructor App() {
            Session s = new Session();
            int ok = s.load("./deeplearning/data/onnx/mnist_mlp.onnx");  // or loadTrain/loadMmap
            if (ok != 1) { Console.writeString("load failed: " + int(s.loadError())); return; }
            double[] buf = new double[784];   // fill input
            s.setInput("data", buf, 784);     // name via inputName(); dynamic dims via setInputShape
            int rk = s.runAuto();             // MYP_GPU=1 → GPU
            double[] prob = s.getOutput("prob");  // output name via outputName()
            // use prob ...
        }
}
int main() { App a = new App(); return 0; }
```

- **Any ONNX model, zero boilerplate**: `infer_tests/run_onnx.myp` (auto-detects
  input/output tensors → top-k).
- **Multi-input / multi-output / optional input**: enumerate `inputCount/inputName`,
  `outputCount/outputName` and read each `getOutput(name)`.
- Real-model references: `infer_tests/r18_main.myp` (ResNet18) / `resnet_main.myp`
  (ResNet50, incl. top-5); 3D U-Net (coarse/fine).

---

## 5. Training: quick start

```myp
import dl;
class TrainApp {
    action:
        @constructor TrainApp() {
            Session t = new Session();
            int ok = t.loadTrain("./deeplearning/infer_tests/batch_matmul_train.json"); // ONNX too
            t.setLossMode(2);        // 0=SoftmaxCE 1=Dice 2=MSE 3=BCE (set before load)
            t.setLr(0.05); t.setTrainMode(1);
            int labT = t.tensorId("label");
            int step = 0;
            while (step < 800) {
                t.setInput("x", bx, 8);            // rebuild inputs every step (arena reuse)
                int i = 0; while (i < 12) { t.setFlat(labT, i, 0.0); i = i + 1; }  // CE/Dice one-hot
                t.runTrain();                       // or runTrainAuto() (GPU training)
                step = step + 1;
            }
            double lf = t.loss();
            double[] o = t.getOutput("o");
        }
}
int main() { TrainApp a = new TrainApp(); return 0; }
```

- Label shape follows the loss: CE/Dice one-hot (softmaxCE needs label>0.5); MSE/BCE take
  real targets / 0-1 masks shaped like the output (no Softmax head needed). See §9.

---

## 6. ONNX model source (`.onnx`)

- **Loading**: `load` (inference) / `loadTrain` (training; registers trainable weights) /
  `loadMmap`/`loadTrainMmap` (zero-copy mmap).
- **Support**: hand-written protobuf wire parsing; weights `float_data/raw_data/
  double_data`; **FP16/BF16 weights auto-converted to FP32**; ONNX external data; opset/
  version checks; clean Unsupported-op diagnostics (reason categories + output list, no
  crash).
- **Verified real models**: ResNet18 (20 BN fused), ResNet50 (top-5 matches ORT),
  3D U-Net (coarse/fine), dynamic batch, multi-input/multi-output/optional input.

### Python helper tools (`deeplearning/infer/tools/`, never in the runtime path)

```bash
PY=deeplearning/infer/tools/onnxvenv/bin/python
```

| tool | purpose |
|------|---------|
| `make_mnist_mlp_onnx.py` | generate `mnist_mlp.onnx` from `data/mnist_weights.bin` (fixture) |
| `prep_imagenet_input.py` | real image → 224×224 ImageNet-normalized f32 input (`resnet_input.f32`) |
| `cross_check_onnx.py` | run a .onnx with onnxruntime, dump reference tensors (.bin) for MYP-side comparison |

**Cross-validation flow (when adding validation)**: ① produce the ORT reference for
`data/onnx/xxx_test.onnx` with `cross_check_onnx.py`; ② load it in a `*_main.myp`, feed
the input, run inference, compare element-wise; ③ expect folded/fused ops still
bit/near-identical to ORT (max diff < 1e-6, FP32).

---

## 7. JSON model source (`.json`, bypasses ONNX)

A layered JSON describes the net; `loadJson`/`loadJsonTrain` fill the framework Graph
directly (reusing the same optimization/execution pipeline; the user never sees ONNX).

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

### 7.1 Wiring = tensor names (implicit named graph; no explicit edge array)

- Each `layers[]` element is one node: `out:"name"` starts an edge; consumers reference it
  with `in:"name"` — the name is the only “wire”.
  - **fan-out (one output, many consumers)**: several layers use the same `out` name as
    their `in` (SwiGLU `gate`/`up` both `in:"h"`); the tensor auto-forks.
  - **fan-in (many inputs meet)**: one layer takes several upstreams via `in2/in3/in4`
    (`Mul in:"gact" in2:"up"`; binary `Add/Sub/Div/Mul/MatMul`, multi-input `Concat`,
    ternary `Where`).
  - Two producers must not define the same `out` name (name clash = redefinition).

### 7.2 Switching / choosing activations

An activation is a normal single-input layer — swap the `op` name to switch
(`Relu`→`SiLU`/`LeakyRelu`/`ReLU6`/`HardSwish`/`Sigmoid`…), keeping `in`/`out` names
(`LeakyRelu` alpha defaults to 0.01). A Relu after a fused `Conv`/`Add` fuses into a
single kernel at inference (automatic); training graphs split it back into an independent
Relu backward. **SwiGLU/GLU gating** = fan-out dual Gemm + `SiLU` + `Mul` (not a single op):
```json
{ "op":"Gemm","in":"h","out":"gate","transB":1,"W":{"dims":[FF,HD],"init":"xavier"}},
{ "op":"Gemm","in":"h","out":"up",  "transB":1,"W":{"dims":[FF,HD],"init":"xavier"}},
{ "op":"SiLU","in":"gate","out":"gact" },
{ "op":"Mul", "in":"gact","out":"m","in2":"up" }
```

### 7.3 Weight sources

`W:{"dims":[…], "init":"xavier|zeros|ones|const|gauss"}` (deterministic LCG) /
`W:{"dims":[…], "values":[… ]}` (row-major exact array) /
`W:{"dims":[…], "safetensors":{"file":"….safetensors","tensor":"name"}}`
(auto-reads from a .safetensors by JSON tensor name). `B` is optional (bias-less Gemm/Conv
supported). Also **input-less `Embedding`**:
`{"op":"Embedding","W":{"dims":[3,2],"values":[…]}, "ids":[2,0,1],"out":"e"}` (W[vocab,D]
+ int64 ids → lookup).

### 7.4 op catalog (per-op syntax)

- **Single-input activations**: `Relu`/`Sigmoid`/`ReLU6` (note `LU6` capitalized)/
  `LeakyRelu`/`SiLU` (=Swish β=1)/`HardSwish`/`Clip` (inline `min`/`max` bounds)/`Tanh`/
  `Softmax(axis)`/`LogSoftmax(axis)`/`GlobalAveragePool`/`Flatten(axis)`; single-input
  `Sqrt`/`Dropout` (identity at inference / random mask at training).
- **Binary**: `Add`/`Sub`/`Div`/`Mul`/`MatMul` (`in2` = right operand); **multi-input**
  `Concat` (`in/in2/in3, axis`); **ternary** `Where` (`in/in2/in3`).
- **Weighted**: `Gemm`/`Conv`/`ConvTranspose`/`MatMul` (optional W); pooling `MaxPool`/
  `AveragePool` (kernel/strides/pads).
- **Normalization (all with scale·bias training grads)**: `BatchNormalization`
  (scale/bias/mean/var + epsilon; fixed-mean/var backward dx=dy·scale·rstd)/
  `InstanceNormalization` (per-(n,c) recomputed stats backward)/`LayerNorm` (gamma/beta)/
  `RmsNorm` (gamma)/`GroupNorm` (gamma/beta [C], groups, epsilon; NCHW [N,C,H,W], a group
  spans cpg·H·W per (n,g); SD1.5 norm_num_groups).
- **3D**: `Conv3D` (W 5D [Cout,Cin,kd,kh,kw] + kernel/strides/pads6)/`MaxPool3D`/
  `AveragePool3D`/`Resize` (sizes [1,1,(outD,)outH,outW])/`ConvTranspose3D` (W 5D
  [Cin,Cout,kd,kh,kw] + strides/pads6; transposed conv, U-Net decode upsample) — 3D JSON
  inputs/weights use 5-D dims.
- **LLM positional encoding**: `Rope(in, in2:cos, in3:sin, heads)` (x[D,S] feature-rows ×
  position-cols; cos/sin[dh/2,S] position table fed via setInput; out is an independent
  tensor = copy + per-head in-place rotation).
- **Parameterized ops (inline int64 consts)**: `Reshape(shape)`, `Gather(indices,axis)`,
  `Expand(shape)`, `Tile(repeats)`, `Slice(starts,ends[,axes][,steps])`,
  `Pad(pads[,mode][,value])` (pads 8 values [N,C,H,W] begin+end; constant fill value
  inline float, default 0). E.g. `{"op":"Reshape","in":"data","out":"out","shape":[2,3]}`.
- **Attribute ops (inline int arrays)**: `Squeeze(axes)`, `Transpose(perm)`,
  `ReduceSum`/`ReduceMean`/`ReduceMax`/`ReduceMin(axes[,keepdims])` (Reduce family with
  N/C → full reduce; `[2,3]` → spatial per-(n,c)).
- **Indexing (row/feature-axis 1D flat; single sample = whole logits row)**:
  `ArgMax`/`ArgMin` (single scalar index output), `TopK(k, outs:[values,indices])`
  (top-k, indices output as float-encoded ints — for CE labels/LLM sampling),
  `OneHot(depth)` (float idx element-wise → row-major one-hot [nIdx,depth]).
- **Advanced data indexing (P8/P8b, both with training backward)**:
  - `GatherElements(in, in2:idx, axis)`: data/indices same-shape element-wise gather,
    out shape = indices; axis 0..rank-1; the float index tensor is fed via setInput at
    runtime. Backward scatter-adds dy back into data (duplicate idx accumulate).
  - `ScatterND(in, in2:idx[q,k], in3:upd)`: copy of data + indices-prefix scatter;
    k ≤ data rank, block = data[k:], upd=[q]+data[k:]. Backward dx=dy with the written
    positions zeroed; du = gather of dy at the written positions (no du when updates is a
    graph input).
  - Both have **GPU backward** (GE: thread-per-dx-slot scan-accumulate, no float atomics;
    ScatterND: dx=dy copy + zero-fill + du gather). Natural-2D cases frozen in
    `json_gather_elements_2d`/`json_scatter_nd_2d` (FC/Gemm transposed 2D remains a
    layout-mismatch boundary).
  - **BN NHWC backward** (P9c, opKind 117): NHWC flat=sp*C+c; IN is not in the NHWC
    layout table, so BN only.
- Auto-added training backward (see §9): Gemm/MatMul/Conv/Relu/Sigmoid/SoftmaxCE/Add/Sub/
  Mul/Div/Pool/Concat + Reshape/Flatten/Squeeze/Transpose/Expand/Tile/ReduceSum/
  ReduceMean/Gather backward — pure data-layout/broadcast/reduce/gather ops may appear in
  the loss path; 4D/batch MatMul uses BwdBatchMatmul (P9).

Frozen JSON samples (`infer_tests/`): `mlp.json`, `branch.json` (multi-branch DAG),
`unet2d.json` (2D U-Net), `swiglu.json`, `cnn_train.json`, `bn_train.json`,
`gather_elements.json`/`scatter_nd.json` (+ training variants), `safe_gemm.json`,
`reshape.json`, `argmax.json`, `mse_reg.json`/`bce_clf.json`, etc.

---

## 8. Session API reference

### 8.1 Capability table

| domain | API |
|--------|-----|
| model source | `load`/`loadTrain` (ONNX), `loadMmap`/`loadTrainMmap` (zero-copy mmap), `loadJson`/`loadJsonTrain` (declarative JSON, bypasses ONNX) |
| I/O | `setInput(name, buf, n)`, `loadInputFromFile(name, f32)`, `setInputShape` (dynamic-dims injection), `inputCount`/`inputName`, `outputCount`/`outputName`, `getOutput(name)`, `tensorId(name)`/`tensorSize(name)`/`setFlat`/`getFlat` |
| execution | `run()`/`runGpu()`/`runAuto()` (inference, MYP_GPU-aware); `runTrain()`/`runTrainAuto()` (training step, GPU-aware) |
| training | `setTrainMode(1)`, `setLr`, `setOptimizer(0=SGD/1=momentum/2=AdamW)`, `setWeightDecay`, `setGradAccumEvery(K)`, `setAmpSim(1)`, `loss()`, `gradId(weightName)`, `trainGpuEnd()` |
| checkpoint | `dumpPlan(path)`/`loadPlan(path)` (op/tensor tables + serialized arena weights; skips ONNX/JSON parsing, resume directly) |
| diagnostics | `phase()` (0 unloaded/1 loading/2 compiling/3 ready), `loadError`/`compileError`/`runError`, `compileMs`/`lastRunMs`/`lastRunOps`, `opCount`, `dumpGraph`/`dumpIR`/`dumpMem` |
| metadata | `irVersion`/`opsetVersion`/`opsetSupported` |

### 8.2 Checkpoint (dumpPlan / loadPlan)

```myp
t.dumpPlan("/tmp/ckpt.bin");     // mid-training: op/tensor tables + arena weights
Session t2 = new Session();
t2.loadPlan("/tmp/ckpt.bin");    // new Session skips ONNX/JSON parsing, resumes directly
// Convention: a loadPlan session has no loader; tensorId(name) returns -1 — resume with
//   tids recorded before dump + setFlat/getFlat (feedInputByTid semantics); label one-hot.
```

### 8.3 Diagnostics

```myp
s.phase(); s.loadError(); s.compileError(); s.runError();   // per-stage error codes
s.compileMs(); s.lastRunMs(); s.lastRunOps();               // stats
s.dumpGraph(); s.dumpIR(); s.dumpMem();                     // structure/plan/memory dumps
```

On a load failure check `phase`/error codes before guessing at segfaults. Diagnostics are
split into load / compile / run stages.

---

## 9. Training in depth

- **Loss modes** (`setLossMode` before load): `0=SoftmaxCE` (default, classification)
  `1=Dice`, `2=MSE` (regression; real-shaped label), `3=BCE` (binary/mask; apply Sigmoid
  first). Without a Softmax head the model output itself is the prediction tensor
  (`mse_reg.json` 500 steps loss→0; `bce_clf.json` 600 steps → floor 0.325).
- **Rebuild inputs every step**: training arena reuse overwrites the input region.
- **GPU training (P10a)**: `runTrainAuto()` — with `MYP_GPU=1` and `gpuTrainReady()`
  (every opKind has a GPU dispatch slot; excluding grad-accumulation/AMP simulation) it
  runs `gpuPersistentStart` on the first step and per-step `runGpu` (incremental upload of
  dirty inputs + full D2H); otherwise it falls back to CPU `runTrain` (never silently
  wrong); call `trainGpuEnd()` at the end. From P10b the GPU backward set is complete for
  fan-in merges (Sub/Mul/Div/Add) and BN/BN-NHWC/IN/batch-MatMul/Reduce/Transpose/
  Expand/Tile/Gather/ReduceMM/Pad — BN/IN/batch-MatMul training nets now run on GPU
  (loss matches CPU).
- **Trainable structures**: chains and fan-in merges (Add/Sub/Mul/Concat in the loss path)
  train; full activation backward (Relu/Sigmoid/Tanh + ReLU6/LeakyRelu/SiLU/HardSwish/
  LogSoftmax/Clip); SwiGLU (`swiglu.json` 200 steps loss 1.09→0.004), activation chain
  (`activ_chain.json` 300 steps 0.97→0.64), CNN (`cnn_train.json` 200 steps 1.01→0.14;
  `gap_cnn.json` 1.06→0.90; `avgpool_cnn.json` 1.04→0.43 — Conv/MaxPool/AvgPool/GAP/
  Flatten backward chain fully usable).
- **Optimizers / accumulation / AMP**: `setGradAccumEvery(K)` micro-batch accumulation;
  `setAmpSim(1)` fp16 gradient-rounding simulation (numeric-pipeline skeleton).
- **Convergence tips**: if loss does not drop, check label/loss-mode match and learning
  rate (large-magnitude MSE targets make BN scale grads large → use lr≈0.005); rebuild
  inputs every step; GPU==CPU bit-identical loss traces isolate “wrong new kernel vs
  network-layer bug”.
- **Label shape**: CE/Dice one-hot (softmaxCE needs label>0.5, not scalar); MSE/BCE
  same-shaped as the output.

---

## 10. Validation & regression

```bash
# Full end-to-end regression (auto-discovers infer_tests/*_main.myp; compile P4 + run P6)
bash examples/deeplearning/infer_tests/run_all.sh    # → pass=135 fail=0 (~4 min)
```

- Each `*_main.myp` is a minimal runnable sample; freeze one + an op-level @test when
  adding an op/model.
- **Op-level @tests** (repo generic runner, auto-scans `tests/@test/*.myp`):
  `./tests/run_tests.sh` (full baseline 237/237). Deep-learning @tests:
  `tests/@test/gpu_paradigm.myp` (GPU paradigm library CPU-fallback + real-GPU),
  `graph_opt.myp` (NHWC vs NCHW equivalence), `bn_opt.myp` (BN vs hand reference),
  `act_opt.myp` (ReLU6/LeakyRelu/SiLU/HardSwish/Clip).
- **CPU/GPU consistency**: probes are GPU==CPU bit-identical; end-to-end element-wise vs
  ORT (Python `infer/tools/` synthesizes `.onnx` + ORT references).
- `MYP_IR_VERIFY=1` five-way verifier; `MYP_NO_REUSE=1` for layer-by-layer debugging.

---

## 11. Data files (`deeplearning/data/`, git-ignored)

```
data/
├── onnx/
│   ├── gluon_resnet50_v1b_Opset16.onnx   # 98MB ResNet50 (ImageNet CNN)
│   ├── resnet18_v1_7.onnx                # 45MB ResNet18 (G4 real BN model)
│   ├── mnist_mlp.onnx                    # MNIST MLP
│   ├── resnet_input.f32                  # preprocessed ImageNet input (NCHW 1*3*224*224)
│   ├── bn_fold_test.onnx / bn_standalone_test.onnx / bn_norelu_test.onnx  # G3 cases
│   ├── act_test.onnx                     # G4 activation cases (opset 14)
│   └── const_fold_test.onnx              # G2 constant-folding case
├── imagenet/                             # classes.txt (1000 labels) + test images
├── mnist_weights.bin                     # early train_mnist export
└── *.idx3-ubyte                          # raw MNIST quartet
```

> `.onnx` files are large; `deeplearning/data/` is wholly git-ignored — re-download after
> cloning (ResNet18 source: ONNX Model Zoo
> `vision/classification/resnet/model/resnet18-v1-7.onnx`).

---

## 12. FAQ / gotchas

- **`import dl` fails to compile**: missing `--package-path examples/deeplearning`.
- **Data not found**: in-program data paths are relative to `examples/` — `cd examples`
  first (`./deeplearning/...`).
- **Load failure / shape 0**: the model has an unsupported op — check whether the op_type
  is recognized in `onnx_loader.myp`’s `inferShapes`; or confirm the .onnx itself runs
  with `tools/cross_check_onnx.py`.
- **Reading an intermediate tensor after inference yields garbage**: region reuse → rerun
  with `MYP_NO_REUSE=1`, or persist that tensor.
- **GPU and CPU disagree**: confirm the numeric paths match (`@gpu for` bodies use
  `float`; scalar params resolved to `double` at dispatch); debug on CPU first.
- **top-5 totally wrong**: usually a graph-wiring/topology issue → check `topoSort` ran
  and `planOrder_` is used.
- **`Console.writeFloat` adds a newline and has low precision**: print loss/values as
  `int(L*1000)` or via `Fmt.i`.
- **2D MatMul seems wrong**: the framework routes through the 4D batch path — declare the
  JSON tensor as 4D batch, or use `Gemm`.
- **MYP reserved words**: `var`/`ref`/`data`/`fact` etc. cannot be variable names
  (e.g. use `refv`).
- **No statement-level named calls in `main()`**: put logic in a class `@constructor` and
  let `main` only `new`.
- **string + float concatenation garbage**: an existing MYP display bug — go through
  `Console.writeFloat(double)`.
- **GPU training diverges while CPU is fine**: check the `runTrainAuto` GPU decision;
  isolate with GPU==CPU bit-identical loss traces; fan-in merges are fixed (P10b); still
  off → inspect `phase`/error codes.

---

## 13. Reference docs & sample index

- **This manual**: `manual.md` (CN) / `manual_EN.md` (EN) — it now contains everything the
  former `sli.md` and `usage.md` had.
- Getting started/status: `../README.md`, `../README_EN.md`, `../infer_tests/README.md`.
- Architecture: `docs/design.md` / `docs/design_EN.md`; GPU paradigm/roadmap:
  `docs/gpu_paradigm.md`.
- Milestones: `CHANGELOG.md`.
- Minimal runnable samples: `infer_tests/*_main.myp` — inference `json_model_main`,
  JSON inference+training, `json_branch_main` (multi-branch), `json_train_submul_main`
  (training with merges), `json_safe_main` (safetensors weights), `sli_fit_main` (full
  training loop + checkpoint resume), `sli_opt_main` (optimizers), `sli_acc_main`
  (grad-accumulation), `sli_amp_main` (AMP skeleton), `multiio_main` (multi-I/O/optional
  input), generic runner `run_onnx.myp`.
