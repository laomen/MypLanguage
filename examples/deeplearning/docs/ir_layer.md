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
  dtype         : enum F32 / I64 / BOOL / F16 / BF16
  storageClass  : enum ACT / WEIGHT / CONST / GRAD / SHAPE   // 复用 shKind_ 语义
  layout        : enum NCHW / NHWC
  producer      : int node id（图输入 = -1）
  uses          : Use[]（def-use 索引，增量维护）
  dead          : bool   // 复用 shDead_
  runtimeTid    : int    // buildRuntime 后填；复用 tId_
```
- 图输入/输出 = 一组 value id（复用 `giName_/goName_`）。
- 常量折叠产出的 int64/float 常量也是 value（复用 `i64v_/cF32_` 语义）。
- `Use = { userNode: NodeId, operandSlot: int }`，而不是只有 `consumerNode`：同一 value
  可在同一节点出现多个 operand（如 `Mul(x,x)`），改写、计数和 topoSort 都须精确到槽位。
- 当前后端的物理上限仍是 5D；IR 的 shape API 用 `rank + dims[]` 表达，lowering 到现有
  runtime 时才限制为 2D/4D/5D。这样新前端或优化不被当前执行后端的 rank 限制绑死。

### 3.2 Node（算子，替代字符串类型 + 并行属性数组）
```
Node:
  op            : enum（前向/反向统一编号，替代 nType_ 字符串）
  operands      : value id 列表（≤5，替代 nIn0_..nIn4_）
  results       : value id 列表（≤4，替代 nOut0_..nOut3_）
  attrs         : 类型化属性表（key→int/float/string/int[]/float[]）
  traits        : pure / stateful / trainOnly / mayAliasOperand
  layout/fused/dead : 复用 nNHWC_/nFused_/nType_=="" 语义
```
- **op 枚举**：把现有 `opKindName` 的 ~72 个算子统一编号（`enum Op { Conv, Conv3D, Relu, ... }`），
  forward/backward 用同一枚举（`BwdConv` 等）+ 一个 `isBwd` 标记，或反向独立编号段。
- **属性表**：`attrInt(node, AttrAxis)`、`attrFloat(node, AttrEps)`、`attrIntArr(node, AttrPads, k)`
  统一访问——取代 F32 位型与 `[node*4+k]` 手算。内部仍可用并行数组，但**访问器统一**。
- 每个 op 同时有 **schema**：合法 operand/result 数、dtype/layout 约束、必需属性、默认值、
  shape-inference 回调和 traits。通用属性表只解决存储；schema 才能阻止漏属性、类型错配和
  对 Update 等 stateful 节点做非法 DCE/重排。

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

### 3.4 IR 不变量与 mutation API（所有改写唯一入口）

稳定的 `NodeId`/`ValueId` 仅在 Graph 生命周期内有效；删除采用 tombstone（`dead=1`），
compaction 只能在所有 pass 完成、lowering 前执行。每次改写只能调用下列 API，禁止直接写
SoA 内部数组：

```
addValue(spec) -> ValueId                 addNode(op, operands, results, attrs) -> NodeId
replaceOperand(user, slot, newValue)      replaceAllUses(oldValue, newValue)
replaceResult(node, slot, newValue)       eraseNode(node) / eraseValue(value)
setAttrInt(node, key, value)              setAttrIntArr(node, key, index, value)
rebuildDefUse()                           invalidate(analysisMask)
verifyIR(stageName) -> VerifyResult
```

- `replaceOperand` 必须先从旧 value 删除精确 `Use{user,slot}`，再登记新 value 的 use；
  `replaceAllUses` 必须遍历 use 的**快照**，防止遍历期间被自身改写。
- 每个 result 只允许一个 producer；图输入、常量和权重 producer 为 `-1`。同名 ONNX tensor
  在导入时映射到同一个 ValueId；不得由名称匹配来决定 producer。
- `verifyIR()` 在 debug/测试模式下验证：Node/Value id 范围、operand/result 数符合 schema、
  每个 use 与 user operand 双向一致、单 producer、无活节点读取 dead value、图输出仍活、
  stateful 节点未被 DCE。每个会改变图的 pass 后运行。

### 3.5 与 MYP 的映射（两种方案，评估取舍）
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
PassResult:
  ok: bool, changed: bool, preservedAnalyses: bitset

interface IRPass { PassResult run(GraphIR g, AnalysisManager am); }
PassManager:
  add(name, pass, requiredAnalyses, maxIter)
  run()                                         // 请求分析、执行、失效/保留分析
```
- `changed` 是迭代到不动点的唯一依据；`maxIter` 是防御上限，达到仍 changed 即报错并输出
  触发 rewrite 的节点。
- `requiredAnalyses` 由 PassManager 在执行前计算；图结构或属性变更默认失效 DefUse、Shape、
  Topo、Liveness。pass 只有在明确证明未影响某分析时，才能在 `preservedAnalyses` 保留它。
- 把现有 `runPassPipeline` 的硬编码调用序列改为「pass 注册表 + 顺序执行」：fold /
  inferShapes / classify / fuse* / DCE / layout / topoSort 各包一个 Pass 对象，**行为不变**
  （回归为准）。初期可保留固定顺序，暂不实现 pass 依赖图拓扑，避免工程化超过收益。

### 4.2 Pattern-match / Rewrite
```
patternMatch(g, rootOp, {subgraph 约束}, out vars)   // 找匹配子图
rewrite(g, matched, fusedOp, attrs, {consumers 重连}) // 替换并维护 def-use
```
- Pattern 约束必须可表达：operand slot、单 use（而非 consumer 数）、常量值、dtype/layout、
  op traits、结果是否图输出。匹配返回 NodeId/ValueId，不返回名称或数组位置。
- Rewrite 必须使用 3.4 的 mutation API，并在一次 rewrite 结束后才失效分析；失败则保持图不变。
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
- **AnalysisManager**：每个分析带 valid 位与 generation。图 mutation 增加 generation；
  `get(DefUse)` 等在无效时重建。不得由调用方猜测索引是否仍可用。

### 4.4 与 buildRuntime 的接缝
- buildRuntime 目前直接读并行数组（`tensorId(nIn0_[ni])` 等）。**保留直读**（热点），
  但接入点收敛：buildRuntime 只按 `nodeOp(i)` 分派 + `attrInt/attrIntArr` 取参，
  使「新增算子 = op schema + inferShapes + lowering 三分支」。
- 训练图不是特殊字符串命名约定：Bwd*/Update/GradAcc 也是 Node，GradAcc 的多路梯度由
  普通 Value + Use 表达。反向 pass 通过 `addNode/replaceAllUses` 构图，禁止绕过 def-use。

## 5. 迁移路径（P0-P4，每步回归全绿）

| 阶段 | 内容 | 行为 | 验证 |
|---|---|---|---|
| **P0** | 稳定 NodeId/ValueId、Use(user,slot)、访问器、mutation API、`rebuildDefUse`、`verifyIR`；先只在导入后/每个既有 graph mutation pass 后重建，新增只读示例 pass | **已实施**：Shape table index=ValueId；压缩 use 链表（最大 5×512 边）；topoSort 改用索引 | ONNX MLP 99%、skip U-Net GPU 100%，均以 `MYP_IR_VERIFY=1` 运行 |
| **P1a** | AnalysisManager 的保守有效性管理 + 固定 pass dispatcher | **已实施**：`IRAnalysis` valid 位/generation；每个旧 pass 后 ALL 失效；DefUse/Topo/Liveness 在重建点标记有效；`runIRPass` 驱动原固定顺序 | ONNX MLP 99%、skip U-Net GPU 100%，均以 `MYP_IR_VERIFY=1` 运行 |
| **P1b** | `PassResult{ok,changed,preservedAnalyses}` + required analysis 声明；把保守失效收紧为按 pass 精确保留 | **已实施基础契约**：Graph `lastPassOk/Changed/Preserved`；结构指纹判断 changed；未改图保留 ALL，改图失效 ALL | ONNX MLP 99%、skip U-Net GPU 100%，均以 `MYP_IR_VERIFY=1` 运行 |
| **P1c** | 按需 DefUse/Topo 获取 + 有上限的不动点执行器 | **已实施**：`ensureDefUse/ensureTopo/ensurePassAnalyses`；`runIRPassToFixpoint`；Liveness 保持 lowering 前显式构建 | identity-fold + ONNX MLP + skip U-Net |
| **P2a** | pattern-match/rewrite 基建 + 迁移一个低风险融合（Conv→Relu）；新增 Add(x,0)、Mul(x,1) 等恒等简化 | **已实施首条 rewrite**：严格标量初始器的 Add(x,0)/Mul(x,1)，DefUse 重连后 tombstone | identity_fold.onnx：3 ops→1 Softmax，结果匹配 numpy |
| **P2b** | 迁移 Conv→Relu 等既有融合为 pattern 描述 | 输出不变（重构） | 与旧 pass 逐位对比 |
| **P3** | op 枚举化 + schema + 属性统一访问 + 反向图在 IR 上构建（buildReverseGraph 走 mutation API） | 零变化（重构） | infer_tests + grad_check + opt_check |
| **P4** | DefUse 增量维护、分析缓存、激活/残差/布局优化；容量动态化或受控扩容 | 增量优化 | 性能、图大小、峰值内存对比 |

## 6. 收益（预期）

- **新 pass**：从「手写 ~100 行全遍历循环 + 多处数组改」降到「~30 行 pattern 描述」。
- **新算子**：接入点收敛到三处（op 枚举 + inferShapes + buildRuntime）。
- **def-use**：`producerIdx`/`isNodeOutput`/`liveConsumers` 的 O(n) 扫 → O(1) producer 与
  精确 Use-edge 索引，
  topoSort/planMemory/buildReverseGraph 直接受益。
- **训练/推理统一**：反向图用同一 IR 语义，GradAcc 等多消费者处理不再特判。
- **容量**：512/256 固定上限可动态化（P4）。

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 重写破坏已验证管线 | P0-P4 严格增量；每阶段跑 infer_tests 全量 + 3d 训练 + grad_check/opt_check |
| 改写后 def-use / 分析过期 | mutation API 是唯一写入口；默认失效所有分析；每个变换 pass 后 verifyIR |
| 同值多次作 operand 被错误合并 | consumers 采用 `Use{userNode,operandSlot}`，不只存 NodeId |
| Update 等状态节点被优化掉或跨步重排 | schema traits 标注 `stateful/trainOnly/mayAliasOperand`；DCE/重排必须查询 traits |
| 访问器 vs 直读数组性能 | 访问器是薄封装（MYP 可内联）；buildRuntime 等热点保留直读 |
| MYP 对象/GC/引用语义 | 起步用方案 A（SoA+访问器），避开对象图风险 |
| 字符串→枚举改动面大 | P3 单独成阶段，字符串仅在调试/ONNX 名保留 |
| 优化 pass 改变数值结果 | 代数简化只删恒等冗余（+0/*1）；融合保持逐位一致（现有融合已验证） |

## 8. 建议的落地顺序

1. **P0**：先落地稳定 id、Use edge、mutation API、验证器与可重建 def-use；这是所有 rewrite
  的正确性地基。
2. **P1a**：先落地保守 analysis invalidation 和固定 dispatcher（已完成）。
3. **P1b**：再加 `PassResult` 与精确 preserved 契约；没有 changed/preserved 不能安全做
  不动点 rewrite。
4. **P2**：在上述基建稳定后才做 pattern-match/rewrite，先迁移一个既有融合，再增加恒等简化。
5. P3 枚举/schema 与 P4 性能优化后置，避免重构、语义改动、性能调优在同一个阶段叠加。

## 9. 实施状态：P0（2026-09-02）

`graph.myp` 已落地不改变 IR 写入方式的最小实现：

- `ValueId` 是稳定的 shape-table index；新只读 API：`valueId/valueName`、`nodeAlive`、
  `nodeInputValue/nodeOutputValue`、`valueProducer/valueUseCount/valueUseNode/valueUseSlot`。
- `rebuildDefUse()` 从现有 SoA 重建 producer 与精确 `(userNode, operandSlot)` use edge。
  使用紧凑链表 `duFirstUse/duNextUse`，全图边数上限为 `5 * nCount_ = 2560`，避免为每个
  Value 预留 512 条 use 的无谓内存。重复 operand（`Mul(x,x)`）存为两条独立 edge。
- `replaceNodeInput()` 是第一条 mutation API；P0 将 `duValid_` 失效，下一次消费前重建。
  既有 pass 仍直接写 SoA，因此 `topoSort()` 在所有已有 mutation pass 完成后无条件重建。
- `verifyIR()` 验证每个活节点的非空输入都有 Shape Value，且每个 operand 都存在精确反向
  use edge。`MYP_IR_VERIFY=1` 打开验证；失败使 topoSort 返回失败。
- `topoSort()` 的入度初始化改用 `valueProducer()`，并按每个输出的 use-edge 递减，取代原先
  对所有节点扫描字符串输入的 O(N²) 逻辑。多输出和重复 operand 仍保持原有拓扑语义。

**P0 边界**：尚未把所有旧 pass 改为 mutation API，也没有增量 use-edge 维护、PassManager、
analysis invalidation 或 op enum；这些严格留给 P1-P3，避免在已验证图优化路径中混入重构。

## 10. 实施状态：P1a（2026-09-02）

P1a 先实现可验证的最小 AnalysisManager，而不改变任一既有 pass 的优化语义：

- `IRAnalysis` 定义 `DEF_USE/TOPO/LIVENESS` 位；Graph 持有 `analysisValid_`、全局
  `analysisGeneration_`，以及各分析的生成 generation。
- `invalidateAnalyses(mask)` 是唯一失效入口；`markIRMutation()` 对旧 SoA 直写 pass
  使用保守 `ALL` 失效。每次 mutation generation 增加，`duValid_` 和 topo order 同步清除。
- `rebuildDefUse()`、`topoSort()`、`planMemory()` 分别记录 DefUse、Topo、Liveness 为当前
  generation 有效。对外暴露 `analysisValid` 与 generation getter，供后续 pass/测试断言。
- `IRPassKind + runIRPass` 以固定顺序分派现有 fold/infer/classify/fuse/DCE/layout pass；
  仅在 pass 成功后标记 mutation。`runPassPipeline` 的顺序和失败处理保持旧行为。
- `buildReverseGraph()` 追加 Bwd*/Update 节点后显式 `markIRMutation()`，保证训练图第二次
  topoSort 绝不消费前向图的 def-use/topo/liveness 缓存。

**P1a 边界**：旧 pass 不报告 `changed`，因此每个成功 pass 都保守失效；尚未实现 PassResult、
required/preserved analysis 声明或不动点迭代。它们是 P1b 的目标，也是开始 P2 rewrite 前的门槛。

## 11. 实施状态：P1b（2026-09-02）

P1b 将 existing pass 的返回 `ok` 扩展为不改变 MYP 方法签名的 Graph 内 `PassResult` 等价物：

- `runIRPass()` 记录 `lastPassOk_`、`lastPassChanged_`、`lastPassPreserved_`，并公开 getter。
  MYP 目前无适合作为热路径返回值的小型 record，因此先以 Graph 状态槽承载；P2 的 pattern
  rewrite 可直接消费该契约。
- `analysisFingerprint()` 覆盖 node op/operands/results/fused/layout，以及 value name/shape/
  dead/layout。它**刻意不覆盖数值属性**（alpha/epsilon 等），因为它们不影响 DefUse、Topo 或
  Liveness。`hashIRText` 保留字符串长度，避免简单串连接碰撞。
- pass 前后 fingerprint 相等：`changed=0`、`preserved=ALL`，不递增 generation；不相等：
  `changed=1`、保守失效 ALL。现阶段不声称单分析精确保留，避免 schema 与 mutation API 尚未
  完备时出现过期缓存。
- `passRequiredAnalyses()` 已提供稳定查询入口，现有 legacy pass 返回 0（它们直接扫描 SoA）；
  P2 的第一条 rewrite pass 必须声明 `DEF_USE`，并在进入前检查 `analysisValid`。

**P1b 后续边界**：仍未实现 pass 的逐分析 preserved 掩码、PassManager 按需调用
`rebuildDefUse/topoSort/planMemory`，也尚未实现不动点迭代。这些是 P1c，且只在 P2 需要
迭代 pattern rewrite 时落地；当前 P1b 已提供正确的 changed 基础。

## 12. 实施状态：P1c + P2a（2026-09-02）

- `ensureDefUse/ensureTopo/ensurePassAnalyses` 使 pass 能声明并在执行前获取所需分析；
  Liveness 不提供隐式 ensure，因为 `planMemory` 写入 lowering 规划，仍只能在 lowering 前
  显式执行。`runIRPassToFixpoint(pass,maxIter)` 以 `lastPassChanged` 收敛，到上限仍变化即失败。
- 第一条声明 `DEF_USE` 的 P2 pass 是 `IDENTITY_FOLD`。它只匹配精确 F32 标量初始化器：
  `Add(x, 0)` 或 `Mul(x, 1)`，跳过图输出、动态标量、广播和近似值；通过
  `replaceAllUses(out, keep)` 重连精确 `(user,slot)` edge，然后 tombstone 原节点/结果。
  既有 DCE 随后回收失去用户的常量。
- 专用 `identity_fold.onnx` 夹具为 `data → Add(0) → Mul(1) → Softmax`。P2a 后 runtime
  op 数由 3 降为 1，Softmax 输出与 numpy 匹配，`MYP_IR_VERIFY=1` 无错误。
- **DefUse 一致性修复**：首版 `replaceAllUses` 只置 `duValid_=0`，却没有清
  `analysisValid_` 的 DEF_USE 位；链式 rewrite 的第二轮 `ensureDefUse` 因 generation 未变
  错用旧 edge，留下 `Mul` 输出的悬空消费者。现 `ensureDefUse` 同时检查 `duValid_`，
  `replaceNodeInput/replaceAllUses` 同步清 DEF_USE valid 位。这是 mutation API 必须同时维护
  数据与 analysis validity 的直接证明。

**下一步**：P2b 将先把现有 `Conv→Relu` 融合迁移到同一 match/rewrite API；此时需扩展
`replaceResult/eraseNode` 与图输出 rewrite，不能把现有 fusion 循环直接复制为“pattern”。

---
