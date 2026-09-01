# 优化 IR 层设计（Optimization IR Layer for deeplearning/infer）

> 日期：2026-09-02
> 范围：`examples/deeplearning/infer/`（graph.myp 为核心的图优化层）
> 目标：为「优化」设计一套一等公民的 IR——清晰的 Node/Value/Attribute 抽象 +
> pass 管理器 + pattern-match/rewrite + 分析（def-use/shape/liveness），
> 同时**不重写**已验证的 graph.myp 管线（增量演进，逐步把内部实现隐藏到访问器后）。

---

## 0. 现状：graph.myp 的 IR 是什么

`graph.myp` 当前是一套 **结构体数组（SoA）并行数组 IR**：

- **节点**（`nCount_` ≤512）：`nType_[512]`（**字符串**算子名）+ `nIn0_..nIn4_`/`nOut0_..nOut3_`
  （**字符串**张量名）+ **~50 个属性并行数组**（`nAxis_/nSH_/nSW_/nPT_/nP0_..nP3_/nMode_/nTransform_/
  nSlAx_/nPadB_/nSD3_/nCastTo_/nFused_/nNHWC_`…）。
- **形状**（`sCount_` ≤512）：`shName_/shD0_..shD4_/shR5_/shKind_/shNHWC_/shDead_`。
- **权重**（`wCount_` ≤256）：`wName_/wRows_/wCols_/wType_/wKind_/wOff_/wTrans_/wRole_/wBN_*`。
- **运行时张量映射**：`tName_/tId_`（graph 张量名 → runtime tensor id）。
- **内存规划**：`planOff_/planProducer_/planLastUse_/planOrder_/planR*`（区域复用）。

**pass 管线**（`runPassPipeline`，推理/训练共用）：
`foldShapeChains → foldConstants → inferShapes → foldShapeChains → classifyShapes →
fuseConvBN → fuseConvRelu → fuseReluOp → fuseGapFlatten → eliminateDeadNodes →
(可选 layoutNHWC) → topoSort → planMemory → buildRuntime`；训练再加 `buildReverseGraph`
（追加 Bwd*/Update 节点）→ `topoSort → planMemory → buildRuntime`。

**这套 IR 已经充分验证**（ResNet50/MNIST/ResNet18/coarse+fine 3D U-Net/distilgpt2/
Qwen2/SD1.5 全套 vs onnxruntime/transformers/diffusers；训练 CPU/GPU 100%）。

## 1. 为什么需要新的 IR 层（现状的痛点）

| # | 痛点 | 后果 |
|---|---|---|
| 1 | **属性分散在 ~50 个并行数组**，且 `int/string/F32 位型` 混存 | 加新算子/属性要改 ~10 处（parser+inferShapes+classify+buildRuntime+新数组），极易漏 |
| 2 | **算子类型是字符串**（`nType_[i] == "Conv3D"` 全图到处比） | 慢、易错（拼写）；无类型安全 |
| 3 | **def-use 是 O(n) 线性扫**（`producerIdx(name)` 逐节点比对输出名） | topoSort/inferShapes/planMemory 大量重复线性扫；无消费者列表 |
| 4 | **pass 是硬编码方法**，无 pass 管理器/pattern-match/rewrite 基建 | 新融合（如 Add+Relu、激活合并、Mul 折叠）只能手写全遍历循环 |
| 5 | **无统一 value/SSA 语义**；反向图节点与前向混在同一数组 | 训练/推理两套逻辑纠缠；GradAcc 等多消费者场景要特殊 hack |
| 6 | **属性访问不统一**（浮点用 `F32.toBits` 位型、数组用 `[node*4+k]` 手算偏移） | 调用方易越界/错位（历史多个 bug 源于此） |
| 7 | 固定容量 512/256 | 大图（deep LLM/高分辨率扩散）可能触顶 |

## 2. 设计目标

1. **一等 Node/Value/Attribute 抽象**：节点有 op 枚举 + 输入/输出 value id 列表 + 类型化属性表；
   value 有 shape/kind/producer/consumers。**优化 pass 只通过这些抽象写**。
2. **def-use 索引内建**：producer 映射 + consumers 列表，改写时增量维护，**消除线性扫**。
3. **pass 管理器**：Pass 对象统一接口 + 管线 + 迭代到不动点 + 分析缓存。
4. **pattern-match/rewrite 基建**：把融合写成「模式 + 替换」，而非手写遍历。
5. **增量演进**：P0-P4 每步行为不变（回归全绿），最终把 graph.myp 内部实现藏到访问器后。

## 3. 核心抽象

### 3.1 Value（值 = 张量，替代字符串张量名）
```
Value:
  id            : int   // opaque handle（不再是字符串名；名字仅作调试/ONNX 名保留）
  shape         : d0..d4, rank5   // 复用 shD0_..shD4_/shR5_
  kind          : enum ACT / WEIGHT / CONST / GRAD   // 复用 shKind_ 语义
  layout        : enum NCHW / NHWC
  producer      : int node id（图输入 = -1）
  consumers     : int[] node id 列表（def-use 索引，增量维护）
  dead          : bool   // 复用 shDead_
  runtimeTid    : int    // buildRuntime 后填；复用 tId_
```
- 图输入/输出 = 一组 value id（复用 `giName_/goName_`）。
- 常量折叠产出的 int64/float 常量也是 value（复用 `i64v_/cF32_` 语义）。

### 3.2 Node（算子，替代字符串类型 + 并行属性数组）
```
Node:
  op            : enum（前向/反向统一编号，替代 nType_ 字符串）
  operands      : value id 列表（≤5，替代 nIn0_..nIn4_）
  results       : value id 列表（≤4，替代 nOut0_..nOut3_）
  attrs         : 类型化属性表（key→int/float/string/int[]/float[]）
  layout/fused/dead : 复用 nNHWC_/nFused_/nType_=="" 语义
```
- **op 枚举**：把现有 `opKindName` 的 ~72 个算子统一编号（`enum Op { Conv, Conv3D, Relu, ... }`），
  forward/backward 用同一枚举（`BwdConv` 等）+ 一个 `isBwd` 标记，或反向独立编号段。
- **属性表**：`attrInt(node, AttrAxis)`、`attrFloat(node, AttrEps)`、`attrIntArr(node, AttrPads, k)`
  统一访问——取代 F32 位型与 `[node*4+k]` 手算。内部仍可用并行数组，但**访问器统一**。

### 3.3 Graph（图）
```
GraphIR:
  values        : Value[]（形状表演进）
  nodes         : Node[]（节点表演进）
  inputs/outputs: value id[]
  weights       : 权重表（wName_/wOff_/wRole_…演进）
  defUse        : 索引（producer 查表 O(1) + consumers 列表）
  analyses      : 缓存（shape/liveness/topo order，pass 声明依赖）
```

### 3.4 与 MYP 的映射（两种方案，评估取舍）
- **方案 A：SoA 并行数组演进（推荐起步）**。保留现有数组，但**新增访问器层**：
  `nodeOp(i)`（枚举）、`nodeIn(i,k)`/`nodeOut(i,k)`（value id）、`attrInt(i,key)`、
  `valueShape(v)`、`valueConsumers(v)`、`buildDefUse()` 等。**新 pass 只走访问器**；
  既有 pass 逐步迁移。改动最小、行为零变化、无 GC/引用风险。
- **方案 B：对象图**（`Node`/`Value` 用 class/struct 记录，动态数组持有）。表达力最强，
  但 MYP 的对象语义（引用 vs 值拷贝、GC、数组嵌套性能）需先探针验证；风险高。
- **决策**：**方案 A 起步**，把「新代码只允许通过 IR 访问器操作内部数组」作为纪律；
  若未来算子/图规模持续扩大，再评估 B（或混合：热点 buildRuntime 直读数组）。

## 4. Pass 基础设施

### 4.1 Pass 管理器
```
interface IRPass { void run(GraphIR g); }     // 与 IOp 同风格
PassManager:
  add(name, pass, deps[], maxIter)            // maxIter>1 = 迭代到不动点
  run()                                        // 按依赖拓扑执行
```
- 把现有 `runPassPipeline` 的硬编码调用序列改为「pass 注册表 + 顺序执行」：
  fold / inferShapes / classify / fuse* / DCE / layout / topoSort 各包一个 Pass 对象，
  **行为不变**（回归为准）。

### 4.2 Pattern-match / Rewrite
```
patternMatch(g, rootOp, {subgraph 约束}, out vars)   // 找匹配子图
rewrite(g, matched, fusedOp, attrs, {consumers 重连}) // 替换并维护 def-use
```
- 迁移现有融合为 pattern 描述：
  - `Conv -> Relu`（现 `fuseConvRelu`）、`Conv -> BN -> …`（`fuseConvBN`）、
    `GAP -> Flatten`（`fuseGapFlatten`）、`op -> Relu`（`fuseReluOp`）。
- **新增融合（现在难写，有了基建才划算）**：`Add(x,0)→x`、`Mul(x,1)→x`、`Conv+Add(残差)`
  融合、连续激活合并、`1×1 Conv→MatMul` 等代数简化。

### 4.3 分析
- **DefUseAnalysis**：producer O(1) + consumers 列表；改写时增量更新。
  - 取代现有 `producerIdx()` 线性扫、`isNodeOutput()`、`liveConsumers()`。
- **ShapeAnalysis**：`inferShapes` 迁移；缓存输出 value 形状。
- **LivenessAnalysis**：`planMemory` 的 `planLastUse_` 迁移；为内存复用/图输出持久化服务。
- **TopoAnalysis**：`topoSort` 迁移；缓存 `planOrder_`，pass 声明「需要有序视图」。

### 4.4 与 buildRuntime 的接缝
- buildRuntime 目前直接读并行数组（`tensorId(nIn0_[ni])` 等）。**保留直读**（热点），
  但接入点收敛：buildRuntime 只按 `nodeOp(i)` 分派 + `attrInt/attrIntArr` 取参，
  使「新增算子 = op 枚举 + inferShapes + buildRuntime 三分支」。

## 5. 迁移路径（P0-P4，每步回归全绿）

| 阶段 | 内容 | 行为 | 验证 |
|---|---|---|---|
| **P0** | 访问器层 + `buildDefUse()`：`nodeOp/nodeIn/nodeOut/attrInt/attrFloat/valueShape/valueConsumers`；在 optimize/optimizeTrain 入口建 def-use 索引；新增一个"走访问器"的示例 pass（如简单的 dead-const 预检） | 零变化 | infer_tests 全量 + 训练 CPU/GPU |
| **P1** | Pass 管理器：现有 pass 逐个包装为 `IRPass`（fold/inferShapes/classify/fuse*/DCE/layout/topoSort），`runPassPipeline` 由管理器驱动 | 零变化 | infer_tests 全量 |
| **P2** | pattern-match/rewrite 基建 + 迁移融合 passes 为 pattern 描述；新增 1-2 个代数简化（Add(x,0)、Mul(x,1)）| 输出不变（优化只删冗余） | 与旧版逐位对比（开关 `MYP_OLD_IR=1`） |
| **P3** | op 枚举化（string→enum）+ 属性统一访问 + 反向图在 IR 上构建（buildReverseGraph 走访问器） | 零变化（重构） | infer_tests + grad_check + opt_check |
| **P4** | 分析缓存 + 新优化 pass（激活合并、残差融合、布局选择）；容量放开（动态数组或扩容量） | 增量优化 | 性能/图大小对比 |

## 6. 收益（预期）

- **新 pass**：从「手写 ~100 行全遍历循环 + 多处数组改」降到「~30 行 pattern 描述」。
- **新算子**：接入点收敛到三处（op 枚举 + inferShapes + buildRuntime）。
- **def-use**：`producerIdx`/`isNodeOutput`/`liveConsumers` 的 O(n) 扫 → O(1) 索引，
  topoSort/planMemory/buildReverseGraph 直接受益。
- **训练/推理统一**：反向图用同一 IR 语义，GradAcc 等多消费者处理不再特判。
- **容量**：512/256 固定上限可动态化（P4）。

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 重写破坏已验证管线 | P0-P4 严格增量；每阶段跑 infer_tests 全量 + 3d 训练 + grad_check/opt_check |
| 访问器 vs 直读数组性能 | 访问器是薄封装（MYP 可内联）；buildRuntime 等热点保留直读 |
| MYP 对象/GC/引用语义 | 起步用方案 A（SoA+访问器），避开对象图风险 |
| 字符串→枚举改动面大 | P3 单独成阶段，字符串仅在调试/ONNX 名保留 |
| 优化 pass 改变数值结果 | 代数简化只删恒等冗余（+0/*1）；融合保持逐位一致（现有融合已验证） |

## 8. 建议的落地顺序

1. **P0**：访问器层 + def-use 索引（低风险，立即收益：新代码能走干净接口）。
2. **P2 先于 P1** 也可：pattern-match 基建对「新增优化」收益最大；P1（pass 管理器）
   是工程整洁，可后置。
3. P3 枚举化在有多个新 pass 稳定后再做（避免重构+新功能叠加）。

---
