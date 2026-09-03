# MYP Deep-Learning Framework — User Manual (examples/deeplearning)

> Version: **2026-09-03** · Language: English (中文版 `manual.md`)
> Covers the **inference + training** framework under `examples/deeplearning` — pure MYP,
> zero Python / onnxruntime at runtime; **ONNX and declarative JSON** model sources;
> CPU + GPU backends.
>
> **Doc roles**: this manual = end-to-end “how to use” (getting started → authoring →
> training → validating → troubleshooting). Go deeper with: `docs/design.md` (architecture,
> incl. `design_EN.md`), `docs/sli.md` (`import dl` + JSON op reference),
> `docs/usage.md` (infer-core usage / cross-validation), `../README.md` (status overview),
> `CHANGELOG.md` (milestones).

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
  reduce/indexing/batch-MatMul backward). See §7 and `docs/sli.md` §3.
- **GPU training**: `runTrainAuto()` decides automatically (GPU dispatch for every op →
  GPU, otherwise CPU fallback); GPU backward for fan-in merges + BN/IN/batch-MatMul etc.
  is complete (P10a/P10b).

---

## 2. Environment & setup

- **Compiler**: repo-root `./build/mypc` (self-hosted; see root `CMakeLists.txt`).
- **Stdlib / package path**: always compile with `--stdlib stdlib`; `import dl` also
  requires `--package-path examples/deeplearning`.
- **GPU (optional)**: NVIDIA + CUDA driver; `MYP_GPU=1` uses CUDA when available,
  otherwise auto-falls back to CPU.
- **Data**: models/inputs under `examples/deeplearning/data/` (git-ignored; some need
  downloading).
- **Python (optional; generation/validation only, never in the runtime path)**:
  `infer/tools/onnxvenv` (onnx + onnxruntime) for synthesizing models and ORT references.

---

## 3. Build & run conventions

```bash
# Compile (repo root cwd)
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
| `MYP_LAYOUT_NHWC` | unset | `1` → enable NCHW→NHWC layout transform (G2 experimental; GPU NCHW is faster by default) |
| `MYP_IR_VERIFY` | unset | `1` → five-way verifier (verifyIR/Shapes/DefUse/Topo/RuntimeWiring) |
| `MYP_NO_REUSE` | unset | `1` → disable memory-region reuse (debug: intermediates still readable after inference) |
| `MYP_PROF_CPU` / `MYP_PROF_GPU` | unset | `1` → per-op timing profile |

---

## 4. Inference in five minutes

One `import dl;` + one `Session` (`dl/dl.myp` is a thin forwarder of
`infer/framework.myp`’s `class Session`):

```myp
import dl;
class App {
    action:
        @constructor App() {
            Session s = new Session();
            int ok = s.load("./deeplearning/data/onnx/mnist_mlp.onnx");  // or loadTrain
            if (ok != 1) { Console.writeString("load failed: " + int(s.loadError())); return; }
            double[] buf = new double[784];   // fill input
            s.setInput("data", buf, 784);     // name via inputName(); dynamic dims via setInputShape
            int rk = s.runAuto();             // MYP_GPU=1 → GPU
            double[] prob = s.getOutput("prob");
            // use prob ...
        }
}
int main() { App a = new App(); return 0; }
```

- **Any ONNX model, zero boilerplate**: use `infer_tests/run_onnx.myp` (generic ONNX
  runner that auto-detects input/output tensors → top-k / output / .bin).
- Multi-input / multi-output / optional input: enumerate `inputCount/inputName`,
  `outputCount/outputName` and read each `getOutput(name)`.

---

## 5. Training in five minutes

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

- Label shape follows the loss: CE/Dice use one-hot (softmaxCE needs label>0.5);
  MSE/BCE take real targets / 0-1 masks shaped like the output (no Softmax head needed).
- Optimizers/accumulation/AMP: `setOptimizer(0=SGD/1=momentum/2=AdamW)`,
  `setWeightDecay`, `setGradAccumEvery(K)`, `setAmpSim(1)`; see §9.

---

## 6. Model source 1: ONNX (`.onnx`)

- Loading: `load` (inference) / `loadTrain` (training; registers trainable weights) /
  `loadMmap`/`loadTrainMmap` (zero-copy mmap).
- Support: hand-written protobuf wire parsing; weights `float_data/raw_data/double_data`;
  **FP16/BF16 weights auto-converted to FP32**; ONNX external data; opset/version checks;
  clean Unsupported-op diagnostics (reason categories + output list, no crash).
- Verified real models: ResNet18 (20 BN fused), ResNet50 (top-5 matches ORT, GPU ~66ms),
  3D U-Net (coarse/fine), dynamic batch, multi-input/multi-output/optional input.
- Quick check for a new model: `infer_tests/run_onnx.myp` runs any model with no boilerplate.

---

## 7. Model source 2: declarative JSON (`.json`, bypasses ONNX)

A layered JSON describes the net; `loadJson`/`loadJsonTrain` feed the same graph pipeline.

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

- **Wiring = tensor names**: a node’s `out:"name"` starts an edge; consumers reference it
  with `in:"name"`.
  - fan-out: several downstream nodes share one `out` name (auto fork);
  - fan-in: one node takes several upstreams via `in2/in3/in4` (binary `Add/Sub/Div/Mul/
    MatMul`, multi-input `Concat`, ternary `Where`);
  - two producers must not define the same `out` name (name clash).
- **Switch activation = switch op name**: `Relu`→`SiLU`/`LeakyRelu`/`ReLU6`/`HardSwish`/
  `Sigmoid`… (`LeakyRelu` alpha defaults to 0.01). SwiGLU/GLU gating = fan-out dual Gemm +
  `SiLU` + `Mul`.
- **Weight sources**: `init:"xavier|zeros|ones|const|gauss"` (deterministic LCG) /
  `values:[…]` (row-major exact) / `safetensors:{"file":…,"tensor":…}`;
  `B` is optional (bias-less Gemm/Conv supported). `Embedding` has no data input
  (`W[vocab,D]` + `ids`).
- **Parameterized/attribute/indexing ops**: `Reshape/Gather/Expand/Tile/Slice/Pad`
  (inline int64 consts), `Squeeze/Transpose/ReduceSum|Mean|Max|Min` (inline axes),
  `ArgMax/ArgMin/TopK/OneHot`, `GatherElements/ScatterND` (P8 advanced indexing, incl.
  P8b backward); 3D (`Conv3D/MaxPool3D/AveragePool3D/ConvTranspose3D/Resize`);
  normalization (`BatchNormalization/InstanceNormalization/LayerNorm/RmsNorm/GroupNorm`);
  LLM `Rope`.
- **Full op list and per-op syntax**: `docs/sli.md` §3 (not duplicated here).
- Fixed examples (`infer_tests/*.json` + `*_main.myp`): `mlp.json`, `branch.json`
  (multi-branch DAG), `unet2d.json` (2D U-Net), `swiglu.json`, `cnn_train.json`,
  `bn_train.json`, `gather_elements.json`/`scatter_nd.json` (+ training variants), etc.

---

## 8. Session API quick reference

| domain | API |
|--------|-----|
| model source | `load`/`loadTrain`/`loadMmap`/`loadTrainMmap` (ONNX); `loadJson`/`loadJsonTrain` (JSON) |
| I/O | `setInput(name,buf,n)`, `loadInputFromFile(name,f32)`, `setInputShape` (dynamic dims), `inputCount/inputName`, `outputCount/outputName`, `getOutput(name)`, `tensorId/tensorSize/setFlat/getFlat` |
| execution | `run()`/`runGpu()`/`runAuto()` (inference); `runTrain()`/`runTrainAuto()` (training step, GPU-aware) |
| training | `setTrainMode(1)`, `setLossMode(0..3)`, `setLr`, `setOptimizer(0|1|2)`, `setWeightDecay`, `setGradAccumEvery`, `setAmpSim`, `loss()`, `gradId(weightName)`, `trainGpuEnd()` |
| checkpoint | `dumpPlan(path)` / `loadPlan(path)` (op/tensor tables + serialized arena weights; skip parsing, resume directly) |
| diagnostics | `phase()` (0..3), `loadError/compileError/runError`, `compileMs/lastRunMs/lastRunOps`, `opCount`, `dumpGraph/dumpIR/dumpMem` |
| metadata | `irVersion/opsetVersion/opsetSupported` |

---

## 9. Training in depth

- **Loss modes** (`setLossMode` before load): `0=SoftmaxCE` (default, classification)
  `1=Dice`, `2=MSE` (regression; real-shaped label), `3=BCE` (binary/mask; apply Sigmoid
  first).
- **Backward coverage**: full activation family, pooling, normalization (BN/IN scale·bias
  grads), data layout/broadcast (Reshape/Flatten/Transpose/Expand/Tile/Gather), reductions
  (ReduceSum/Mean, ReduceMax/Min argmax), indexing (GatherElements/ScatterND), 4D/batch
  MatMul, Pad, Concat, Conv.
- **GPU training**: `runTrainAuto()` — with `MYP_GPU=1` and a GPU dispatch for every op in
  the graph it runs persistent GPU training steps (per-step full D2H, correctness first),
  otherwise it falls back to CPU (never silently wrong). Fan-in merges and
  BN/IN/batch-MatMul nets already run on GPU (P10b).
- **Checkpoint/resume**: `dumpPlan` mid-training → a new `Session.loadPlan` skips parsing
  and restores weights to continue (loadPlan sessions have no loader; resume with tids
  recorded before dump + `setFlat/getFlat`).
- **Convergence tips**: if loss does not drop, check label/loss-mode match and learning
  rate (large-magnitude MSE targets make BN scale grads large → use lr≈0.005); rebuild
  inputs every step; GPU==CPU bit-identical loss traces isolate “wrong new kernel vs
  network-layer bug”.

---

## 10. Validation & regression

```bash
# Full end-to-end regression (auto-discovers infer_tests/*_main.myp; compile P4 + run P6)
bash examples/deeplearning/infer_tests/run_all.sh    # → pass=135 fail=0
```

- Each `*_main.myp` is a minimal runnable sample of one capability (loads a real/synthetic
  model and prints OK/FAIL). When adding an op/model, freeze a `*_main.myp` + an op-level
  `tests/@test/*_opt.myp`.
- CPU/GPU consistency: probes are GPU==CPU bit-identical; end-to-end element-wise vs ORT
  (Python in `infer/tools/` synthesizes `.onnx` models and references).
- `MYP_IR_VERIFY=1` runs the five-way graph/runtime verifier; `MYP_NO_REUSE=1` for
  layer-by-layer debugging.

---

## 11. FAQ / gotchas

- **`import dl` fails to compile**: missing `--package-path examples/deeplearning`.
- **Data not found**: program data paths are relative to `examples/` — `cd examples` first
  (`./deeplearning/...`).
- **Reading an intermediate tensor after inference yields garbage**: region reuse → rerun
  with `MYP_NO_REUSE=1`, or persist that tensor.
- **`Console.writeFloat` adds a newline and has low precision**: print loss/values as
  `int(L*1000)` or via `Fmt.i`.
- **2D MatMul seems wrong**: the framework routes through the 4D batch path — declare the
  JSON tensor as 4D batch, or use `Gemm`.
- **MYP reserved words**: `var`/`ref`/`data`/`fact` etc. cannot be variable names
  (e.g. use `refv`).
- **No statement-level named calls in `main()`**: put logic in a class `@constructor` and
  let `main` only `new`.
- **GPU training diverges while CPU is fine**: check the `runTrainAuto` GPU decision;
  isolate with GPU==CPU bit-identical loss traces; fan-in merges are fixed (P10b); still
  off → inspect `phase`/error codes.

---

## 12. Reference docs & sample index

- **This manual**: `manual.md` (CN) / `manual_EN.md` (EN).
- Getting started/status: `../README.md`, `../README_EN.md`, `../infer_tests/README.md`.
- Architecture: `docs/design.md` / `docs/design_EN.md`.
- References: `docs/sli.md` (`import dl` + JSON op catalog), `docs/usage.md` (infer usage +
  cross-validation flow), `docs/gpu_paradigm.md` (GPU paradigm + roadmap).
- Milestones: `CHANGELOG.md`.
- Minimal runnable samples: `infer_tests/*_main.myp` (inference `json_model_main`,
  training `sli_fit_main`, generic runner `run_onnx.myp`, etc.).
