# MYP Deep-Learning Inference/Training Framework — Design Notes

> Scope: the `infer/` core library of `examples/deeplearning` plus the `dl` (Session)
> unified entry point — a general static-graph **inference + training** framework
> implemented in pure MYP.
> Roadmap & performance paradigm: `docs/gpu_paradigm.md`; feature/milestone timeline:
> `CHANGELOG.md`; user quick-start: `../README.md` (CN/EN), `docs/sli.md`, `docs/usage.md`.
> Doc version: **2026-09-03** (after the modularized Graph IR, declarative JSON source,
> Session training, and P10b GPU-backward work).

---

## 1. Positioning & design goals

- **Pure MYP**: zero Python / onnxruntime at runtime; the C layer is only GPU FFI
  (`runtime_gpu.c`) plus vendor calls such as cuBLAS.
- **One graph IR, two model sources**: a static graph (tensor table + op table + weight
  table) is decoupled from its origin — **ONNX** (parsed by `onnx_loader.myp`) and
  **declarative JSON** (built directly by `json_model.myp`) both fill the same `Graph`,
  go through the same pass pipeline, and lower to the same `InferenceRuntime`.
- **Inference + training in one**: the same runtime graph can `run()`/`runGpu()` forward,
  or train via `Session`'s static reverse graph (auto label + loss + backward + Update),
  on CPU and GPU backends.
- **FP32 precision**: activations/weights live in a `float[]` arena by default and are
  verified element-wise against onnxruntime; FP16/BF16 weights are converted to FP32 on
  load; training AMP uses an fp16-gradient-rounding simulation skeleton.
- **Graph optimization**: a pass pipeline runs at load time (constant folding / fusion /
  DCE / constant dedup / shape-value propagation / layout transform / topo sort / memory
  planning) to cut kernel launches and peak memory.
- **Correctness gates**: `MYP_IR_VERIFY=1` runs five verifiers; full end-to-end regression
  (`infer_tests/run_all.sh` → pass=135).

---

## 2. Directory layout (2026-09-03)

```
deeplearning/
├── dl/
│   └── dl.myp                # ★ SLI unified entry: `import dl;` → Session facade (thin)
├── infer/                    # ★ Core library (pure MYP)
│   ├── framework.myp         # SLI facade: class Session (load/run/train/dump/checkpoint)
│   ├── runtime.myp           # InferenceRuntime: tensor/op registry + run()/runGpu() + optimizer/accum
│   ├── ops.myp               # CPU op kernels (~82 opKinds, batch-aware, FP32)
│   ├── gpu_ops.myp           # GPU op kernels (@gpu for resident, FP32)
│   ├── op_iface.myp          # interface IOp {forward, backward} (phase-4e POC, relu sample)
│   ├── ops_iface_all.myp     # ★ full op registry: CPU/GPU class per op → fwd/bwd slots
│   ├── graph_defs.myp        # graph IR constants: Kind / NodeField / OpCode enums
│   ├── graph.myp             # Graph composition root (domain accessors + orchestration)
│   ├── graph_nodes.myp       #   node SoA table storage
│   ├── graph_weights.myp     #   weight/initializer SoA storage
│   ├── graph_shapes.myp      #   shape SoA (dims/rank5/kind/NHWC/dead)
│   ├── graph_node_attrs.myp  #   per-node scalar attribute slots (NodeField)
│   ├── graph_planner.myp     #   memory-plan mutable state
│   ├── graph_analysis.myp    #   analysis state (liveness, etc.)
│   ├── graph_optimizer.myp   # ★ pass-pipeline orchestration (GraphOptimizer)
│   ├── graph_compiler.myp    # ★ Graph IR → runtime lowering (buildRuntime)
│   ├── onnx_loader.myp       # ONNX parsing (protobuf wire) → Graph + training graph + error codes
│   ├── json_model.myp        # ★ declarative JSON model → Graph (bypasses ONNX)
│   ├── pb.myp                # protobuf wire-format reader (for .onnx)
│   ├── safetensors.myp       # safetensors weight reader (F32/BF16/F16, mmap)
│   ├── tensor.myp            # tensor index helpers
│   └── tools/                # Python helpers (make_*_onnx.py synth models + ORT refs / onnxvenv)
├── infer_tests/              # end-to-end regression entries (135 *_main.myp; run_all.sh)
├── json_tool/                # JSON-graph utility (standalone demo + CLI)
├── train/  llm/  diffusion/  # sibling sub-projects (3D training / Qwen2+distilgpt2 / SD1.5)
├── data/                     # models/inputs/datasets (git-ignored)
└── docs/                     # design.md (this doc) / usage.md / sli.md / gpu_paradigm.md (roadmap)
```

---

## 3. Overall architecture

```
  Model source                    Graph IR (format-agnostic)           Execution backend
┌──────────────┐   ┌────────────────────────────────────┐   ┌────────────────┐
│ ONNX (.onnx) │   │ Graph (composition root)           │   │ InferenceRuntime│
│ onnx_loader  │──▶│  storage: defs/nodes/weights/      │──▶│  ops.myp (CPU)  │
│ pb.myp parse │   │          shapes/node_attrs/        │   │  gpu_ops.myp    │
│ (mmap/external│  │          planner/analysis           │   │   @gpu resident │
│  /FP16/BF16)  │   │  algorithms: GraphOptimizer(passes)│   │  cuBLAS GEMM    │
├──────────────┤   │             GraphCompiler(lowering) │   │  run()/runGpu() │
│ JSON (.json) │   │  front: ONNX/JSON → optimize(infer)/│   │  runAuto() picks│
│ json_model   │──▶│          optimizeTrain(train)       │   └────────────────┘
└──────────────┘   └────────────────────────────────────┘          ▲
                                                                    Session (dl)
   Inference: load→optimize→run;  Training: loadTrain/loadJsonTrain→optimizeTrain→runTrain
```

- **Graph only understands the graph IR** (op name + input slots + attribute field codes
  `NodeField` + shapes); it does not care where the graph came from.
- **One builder API, two consumers**: both the ONNX parser and the JSON loader drive the
  same graph-building calls
  (`setFile/addWeight/addShapeD/addGraphOutput/beginNode/endNode/nodeType/nodeIn/nodeOut/…`).
- **MYP constraints**: methods in `function:` sections are class-private (not cross-class
  callable), so graph-building APIs live in `action:`; cross-class code may only call
  methods (not read fields directly), so Graph / storage classes expose accessors.

---

## 4. Graph IR & modularization (phase-1 refactor, frozen 2026-09-01)

The early single-file `graph.myp` (~1070 lines) was split (phase "graph.myp 拆分" + the
interface freeze) into:

- **Graph = composition root**: owns the storage/algorithm components plus domain
  accessors (`valueAt/nodeOutOf/…`); no longer implements passes/lowering directly.
- **Storage split (each its own SoA table; Graph never touches raw arrays)**:
  - `graph_defs.myp`: pure constants (Kind / NodeField / OpCode enums), no instance state.
  - `graph_nodes.myp`: node opcode + fixed in0..in4/out slots; attrs live in `GraphNodeAttrs`.
  - `graph_weights.myp`: weight/initializer metadata (name/index/get/set).
  - `graph_shapes.myp`: shapes (names/dims/rank5/kind/NHWC/dead).
  - `graph_node_attrs.myp`: per-node scalar attribute slots (NodeField → value), strided.
  - `graph_planner.myp` / `graph_analysis.myp`: algorithm mutable state.
- **Algorithms home**: `GraphOptimizer` owns every pass ("pass 迁入 GraphOptimizer");
  `GraphCompiler` owns full lowering (buildRuntime moved out + runtime tensor mapping).
- **Why (MYP engineering)**: one giant class with huge methods + many array fields made
  the file unwieldy and edits cascaded into large recompiles; decoupling SoA storage from
  algorithms lets passes evolve independently. **Note**: MYP imports do not re-export
  symbols — every module must import its dependencies explicitly (e.g. graph_defs is
  referenced by many modules).

---

## 5. Frontend 1: ONNX (`onnx_loader.myp` + `pb.myp`)

- Hand-written protobuf wire parsing of `ModelProto/GraphProto/NodeProto/AttrProto/
  TensorProto/ValueInfoProto`; weights support `float_data/raw_data/double_data`; dims from
  `repeated int64`.
- **Field numbers verified**: AttrProto `t`(TensorProto)=**5** (6 is g=GraphProto);
  BatchNormalization epsilon `f`=2 (wire type 5, `readU32()` 4-byte LE).
- **Robustness (phase 7)**: opset/version checks; Unsupported-op diagnostics (reason
  categories + output list); ONNX external-data weight reading; `loadMmap` zero-copy mmap;
  three-stage error-code separation.
- **dtype normalization (phase 3)**: BF16→FP32 = bits<<16; FP16→FP32 via standard half
  decode; everything converges into the FP32 arena.
- **Training graph**: `loadTrain`/`loadTrainMmap` additionally register trainable weights
  during parsing (for backward Update).

---

## 6. Frontend 2: declarative JSON (`json_model.myp`, 2026-09-02)

- **Purpose**: a second graph source that bypasses ONNX — the user writes a layered JSON
  (`{op, in, ...}`) description; the loader fills the same `Graph` (nodes/shapes/in-memory
  weights) and reuses `optimize` (inference) / `optimizeTrain` (training: auto-adds
  label + loss + backward).
- **Wiring = tensor names**: `out` name → downstream `in`; fan-out reuses the name; fan-in
  uses `in2/in3/in4` slots; loader infers shapes/roles per op type.
- **Weight sources**: `init` (inline values) / `values` (explicit array) /
  `.safetensors` (in-memory weight channel offset=-2, value in memVal_, writeWeight writes
  the arena directly). JSON scalars reach node-constant inputs via `regF32Scalar`
  inference.
- **JSON-supported op families**: Gemm/MatMul/Conv/ConvTranspose/Pool/activation family
  (Relu↔SiLU↔LeakyRelu↔ReLU6… switch op name to switch activation)/Softmax/LogSoftmax/
  GlobalAveragePool/Flatten/normalization (BN/IN/GroupNorm/LayerNorm/RmsNorm) + binary
  Add/Sub/Div/Mul + multi-input Concat + data-layout (Reshape/Transpose/Squeeze/Expand/
  Tile/Gather) + reductions (Reduce family) + indexing (OneHot/ArgMax…) + Dropout + 3D
  (Conv3D/Pool3D/ConvTranspose3D).
- **Fixed sample**: `unet2d.json` (2D U-Net: encoder Conv+MaxPool / decoder ConvTranspose +
  skip Concat; end-to-end CPU+GPU).

---

## 7. Data model & memory

### 7.1 Runtime data model (`runtime.myp`)

- **Tensor table**: each tensor has `(name, rows, cols, 4D/5D metadata)`; CNN tensors are
  registered with `(N,C,H,W)` (NHWC layout is `(N,H,W,C)`); 5D tensors carry a rank5 flag
  and the D dimension.
- **Arena**: one `float[]` holds non-persistent tensors; persistent tensors (weights /
  graph inputs / graph outputs) are allocated separately and **never reused** (weights
  survive many runs).
- **Op table**: `opKind/opA/opB/opC/opP0..`: per-op integer kind + input/output/param
  tensor ids + scalar-parameter slots (spilling to extended slots opX/opD when opP is full).

### 7.2 Memory planning (`GraphPlanner` / planMemory)

- **First-fit region reuse**: advancing topologically, a tensor's region is reusable by
  later tensors after its last use (ResNet50 ~15M floats ≈ 60MB vs ~400MB without reuse).
- **Persistent marking**: weights / inputs / outputs never enter the reuse pool.
- **`MYP_NO_REUSE=1`**: disables reuse (reading a released tensor under reuse yields
  garbage — a common correctness-debugging trap).
- **Dynamic batch / shape specialization (phase 3)**: `setInputShape` injects dynamic dims →
  compile-time per-bucket specialization; shape-value propagation folds int64
  arithmetic/ReduceSum/Expand shape chains.
- **dumpPlan/loadPlan**: optimized-IR + plan cache (checkpointing / resume / skip
  re-optimization); `loadPlan` builds the runtime directly, skipping ONNX parsing+optimizing.

---

## 8. Graph-optimization pass pipeline (`GraphOptimizer`)

```
foldConstants → inferShapes → classifyShapes → fuseConvBN → fuseConvRelu
→ fuseGapFlatten → eliminateDeadNodes → [constant dedup / dead-weight prune /
shape-value propagation / Conv1x1 lowering / op selection] → layoutNHWC(opt-in)
→ topoSort → planMemory → buildRuntime
```

| pass | role |
|------|------|
| `foldConstants` | Constant (float) nodes → persistent tensors (`wRole_=4`, kind `FC_C`) |
| `inferShapes` | topo shape inference (unsupported op → return 0 → load fails) |
| `classifyShapes` | classify roles (weight/bias/activation/input/output) |
| `fuseConvBN` | fold Conv→BatchNormalization into conv weights/bias (G3) |
| `fuseConvRelu` | fuse Conv→Relu into one kernel (G1) |
| `fuseGapFlatten` | fuse GlobalAveragePool→Flatten (G2, batch==1) |
| `eliminateDeadNodes` | DCE (fixpoint; no live consumer & not a graph output) |
| `constant dedup` | merge identical initializers (ResNet18 regression guards BN-fold bias) |
| `dead-weight prune` | drop initializers unreferenced after DCE |
| `shape-value propagation` | fold int64 const shape chains (ReduceSum/Expand…) |
| `Conv1x1 lowering` | 1x1 conv → GEMM (opKind 83) |
| `op selection` | BatchMatMul batch=1 → 2D matmul / layout choice |
| `layoutNHWC` | NCHW→NHWC, opt-in via `MYP_LAYOUT_NHWC=1` (G2) |
| `topoSort` | Kahn topo sort of live nodes into `planOrder_` |
| `planMemory` | first-fit region reuse + persistent tensors |
| `buildRuntime` | wire `planOrder_` into `InferenceRuntime` |

> **verifier (phase 8)**: `MYP_IR_VERIFY=1` runs five checks — verifyIR / verifyShapes /
> verifyDefUse / verifyTopo / verifyRuntimeWiring (plus pass-equivalence tests).

### 8.1 Fusion details (correctness-critical; bug lessons)

- **Conv+BN fold (G3)**: `invStd[c]=scale[c]/sqrt(var[c]+eps)`,
  `W'[c]=W[c]*invStd[c]`, `B'[c]=(B_conv[c]-mean[c])*invStd[c]+B_bn[c]`; applied in
  `writeWeight` (commutes with NHWC weight transpose); BN node + 4 params are dead-ended;
  conv output rewritten to BN output. A bias-less conv folding BN synthesizes a
  `<conv>#bnb` weight (`wBNOnly_=1`).
- **`nFused_` vs `nRelu_` (G4 fix)**: each fusion type uses its **own semantic flag** —
  buildRuntime uses `nRelu_` (set only by fuseConvRelu) to choose addConvRelu vs addConv,
  so a BN-only fold followed by a residual Add never gets a spurious ReLU.
- **DCE liveness**: decide with `effectiveOut` (a fused node's `nFusedOut_`); otherwise
  nodes consumed by a fusion get deleted as dead.
- **planMemory fusion-aware**: the consumed node's output producer is overwritten to the
  fused node; otherwise last-use windows let the fused output reuse its input region →
  output aliases input → silent corruption.
- **Training skips structural fusion**: training graphs auto-skip Conv+Relu-style
  structural fusions and backpropagate node-by-node (this fixed a hidden disconnect where
  fused Conv weights never updated).

---

## 9. Runtime & ops (`runtime.myp` + `ops_iface_all.myp`)

### 9.1 Two numbering spaces: opKind & OpCode

- **opKind**: runtime kernel codes (fwd/bwd execution codes), growing with op families
  (~82+ today).
- **OpCode** (`graph_defs.myp`): graph-IR op-type codes (including BWD_*, loss; 120+).
- ONNX op_type / JSON op names are mapped by loaders to OpCode/kind; buildRuntime registers
  the opKind op.

### 9.2 Interface-based op registry (phase 4e onward; run/runGpu have no if/else)

- `interface IOp { void forward(rt, opIdx); void backward(rt, opIdx); }`
- Each op has two classes (CPU `XxxOp`, GPU `GpuXxxOp`), both implementing IOp; registering
  with `registerFwdBwd(fwdKind, bwdKind, op, isGpu)` places one instance into forward and
  backward slots (CPU and GPU tables); `backward()` only runs when `trainMode()==1`.
- First dispatch auto-calls `registerAllIfaceOps`; the full registry lives in
  `ops_iface_all.myp` (authoritative list).

### 9.3 Op-family overview (CPU `ops.myp` + GPU `gpu_ops.myp`, ORT-bit-exact)

| family | representative ops |
|--------|--------------------|
| conv/pool/pad | Conv (NCHW/NHWC/1x1-GEMM), Conv3D, ConvTranspose3D, MaxPool/AvgPool/GlobalAveragePool/GAP3D, Pad (constant/edge/reflect + non-zero value) |
| FC/matmul | Gemm/Dense, MatMul, BatchMatMul (4D + batch broadcast, cuBLAS on GPU) |
| normalization | BatchNorm (NCHW/NHWC), InstanceNorm, GroupNorm, LayerNorm, RMSNorm |
| activation | Relu/Sigmoid/ReLU6/LeakyRelu/SiLU/HardSwish/Clip/Tanh/GELU |
| probability/loss | Softmax/LogSoftmax/CE/Dice/MSE/BCE (training loss nodes) |
| tensor layout | Transpose/Reshape/Flatten/Squeeze/Concat/Split/Slice/Expand/Tile/Where/Embedding |
| indexing | Gather/GatherElements/ScatterND/ArgMax/ArgMin/TopK/OneHot/Range |
| reduce/elementwise | ReduceSum/Mean/Max/Min (mode-generalized), Add/Sub/Mul/Div/Sqrt, Resize (NCHW/NHWC) |
| LLM | RmsNorm/LayerNorm/GELU/RoPE positional encoding |
| 3D | Conv3D/Pool3D/Pad3D/Resize3D/ConvTranspose3D (5D tensorD-aware) |
| Dropout | identity at inference / random mask at training |

---

## 10. Execution backends

### 10.1 CPU (`ops.myp`)

Kernels run in op-table order via `InferOps` (batch-aware, FP32). Training/reduction
kernels may use `@parallel for` (small-n micro-parallel correctness is guaranteed after
the runtime_myp pool lost-wakeup fix — see main-repo BUGLIST BUG-135).

### 10.2 GPU (`gpu_ops.myp` + cuBLAS)

- Kernels are written as `@gpu for (long p...) resident(a=dev)` device-resident: array
  arguments use device pointers directly, skipping per-op H2D/D2H.
- **Persistent incremental arena sync (phase 6)**: graph/weights do one H2D to build a
  device arena; `setFlat` dirties regions → runGpu incrementally uploads dirty regions →
  training steps call `gpuMarkSyncAll` for a full D2H (correctness first).
- **cuBLAS GEMM (phase 6)**: dense/matmul/Conv1x1-lowering use the vendor library (main
  GPU speedup source).
- **Kernel constraints (MYP)**: no `double` locals, no host-function calls inside a
  kernel — scalar params (eps/alpha/min/max) are resolved to `double` at dispatch and
  passed in; in-kernel math uses `float` + `__nv_*` builtins.
- **Fallback**: no GPU / `MYP_GPU` unset / graph has an op without a GPU dispatch → auto
  CPU fallback (identical results). `Session.runAuto()`/`runTrainAuto()` choose accordingly.

### 10.3 Consistency gate

Each new op is numerically paired CPU vs GPU (probe GPU==CPU bit-wise); end-to-end checks
are element-wise vs ORT; `run_all.sh` is all-green at pass=135.

---

## 11. Training (Session / static reverse graph)

### 11.1 Automatic reverse-graph construction

- `optimizeTrain` (JSON) / `loadTrain` (ONNX) auto-add **label + loss + backward**: a
  static reverse graph (Bwd op family + Update nodes) is traced back from the output
  tensor; each forward opKind has a matching Bwd opKind (`registerFwdBwd`).
- **Backward coverage**: activations (Relu/Sigmoid/ReLU6/Leaky/SiLU/HardSwish/Clip/
  LogSoftmax/Tanh/GELU), pooling (MaxPool/AvgPool/GAP/GAP3D), normalization (BN/IN
  scale·bias grads, GroupNorm), layout/broadcast (Reshape/Flatten/Squeeze/Transpose/
  Expand/Tile/Gather scatter-add), reductions (ReduceSum/Mean broadcast back to x,
  ReduceMax/Min argmax), indexing (GatherElements/ScatterND), 4D/batch MatMul, Pad,
  Concat, Conv (incl. 1x1), etc.

### 11.2 Session unified (phase-9 SLI + P10a/P10b)

- Entry: `load/loadTrain/loadMmap/loadTrainMmap/loadJson/loadJsonTrain` → inference or
  training graph.
- Inference: `runAuto()` (MYP_GPU=1 → GPU, else CPU); `getOutput/setInput/
  loadInputFromFile`.
- Training: `setLr/setOptimizer(0|1|2)/setWeightDecay/setGradAccumEvery/setAmpSim/`
  `setTrainMode(1) → runTrain()/runTrainAuto() → loss()`; `gradId(weightName)` reads grads.
- Optimizers: SGD / momentum / **AdamW + weight decay**; gradient accumulation
  (micro-batch); AMP numeric-pipeline skeleton (fp16 gradient-rounding simulation).
- Loss family: CE/Dice (classification) and MSE/BCE (element-wise; `setLossMode`).
- Checkpoint: `dumpPlan/loadPlan` (optimized IR + plan; resume skips parse/optimize;
  deterministic).
- **GPU training (P10a)**: `runTrainAuto` takes the persistent GPU training step when
  MYP_GPU=1 and every op in the graph has a GPU fwd/bwd slot (per-step full D2H,
  correctness first); otherwise auto CPU fallback. **P10b** adds GPU backward for
  fan-in merges (Sub/Mul/Div/Add) and BN/IN/BN-NHWC/batch-MatMul/Reduce/Transpose/
  Expand/Tile/Gather/ReduceMM/Pad — BN/IN/batch-MatMul training nets now run on GPU via
  `runTrainAuto`.
- Fixed end-to-end: CNN training, 2D U-Net (json), SwiGLU (fan-out dual Gemm + SiLU + Mul,
  200 steps loss 1.09→0.004), 3D U-Net, ResNet inference, etc.

---

## 12. Extending: adding an op / a model

New op (forward; backward when needed):
1. Add the CPU kernel to `ops.myp` (batch-aware, FP32); GPU version to `gpu_ops.myp`
   (`@gpu for`+resident, numerically identical).
2. Add op classes implementing `interface IOp{forward,backward}` (CPU `XxxOp` + GPU
   `GpuXxxOp`); register in `ops_iface_all.myp` (fwd/bwd slots; loss/special ops use
   backward-only registration).
3. Add an OpCode in `graph_defs.myp` if graph-level recognition is needed; map the ONNX
   op_type in `onnx_loader.myp` → shape inference `inferShapes` → `buildRuntime` wiring
   (add a pass if fusable); for JSON add the op-name dispatch in `json_model.myp` /
   `graph_compiler`.
4. GPU backward: make sure `GpuXxxOp.backward()` is implemented and registered into the GPU
   bwd slot (convention: GPU op classes often have an empty backward() stub —
   registerFwdBwd only places the slot; check the class body when adding GPU bwd).
5. Verify: synthetic `.onnx`/`.json` + ORT element-wise reference (`*_main.myp`);
   op-level `tests/@test/*_opt.myp`; CPU/GPU paths; training cases with reverse numeric
   pairing (`bwd_*_main.myp` / finite differences).

New model, zero boilerplate: use `infer_tests/run_onnx.myp` (generic ONNX runner,
auto-detects input/output), or `import dl` + `runAuto`.

---

## 13. Known constraints / MYP gotchas

- `var`, `ref`, `data`, `fact` are MYP reserved words; avoid as local names.
- `float − double` mixed arithmetic does not auto-promote; LLVM verify rejects implicit
  float→double assignment (declare an initialized double first).
- No float↔bits conversion in MYP → eps/alpha are stored as bit-typed ints and parsed with
  `F32.toDouble`.
- @parallel/@gpu bodies **capture only outer locals**: class/static property arrays must be
  copied to locals first; no arena allocation / host calls inside the body; loop vars may
  be int or long.
- @gpu kernel scalars must be read from the device arena inside the kernel (host copies go
  stale under persistence) — e.g. bwdClip min/max.
- Interface conversion / fat pointers: every expression shape of class→interface needs the
  concrete class name (historical BUG-029/033/064 lessons); new data-input ops in the
  JSON/graph path must be role-registered so they are not treated as 2D-transposed layout.
- Numeric diagnostics: under large-magnitude MSE targets the BN scale gradient is large
  (dscale≈Σdy·x) → use lr 0.005 to avoid NaN.
- No statement-level named calls inside `main()` → put output/training logic in a class
  `@constructor` (see the *_main.myp convention).

---

## Appendix: related docs & validation

- `../README.md` / `README_EN.md`: current-state quick start (CN/EN).
- `docs/usage.md`: infer usage; `docs/sli.md`: `import dl` unified-entry guide.
- `docs/gpu_paradigm.md`: GPU paradigm library + inference/training roadmap.
- `CHANGELOG.md`: this sub-project's milestone timeline (phases 1–9 + JSON P2..P10b).
- Regression: `bash examples/deeplearning/infer_tests/run_all.sh` → `pass=135 fail=0`.
