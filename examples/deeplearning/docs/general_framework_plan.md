# MYP 通用深度学习框架建设计划

> 日期：2026-09-01
> 范围：`examples/deeplearning/`
> 目标：从“可运行当前 ONNX/3D 模型的推理与训练框架”演进为可扩展、可验证、可接入多后端的通用静态图框架。

## 1. 当前基线

目前框架已经具备：

- 纯 MYP ONNX protobuf 解析器与通用 `Graph` IR。
- 2D/3D 静态图推理，支持 NCHW/NHWC（3D 默认 NCDHW）。
- CPU/GPU 执行、GPU arena 常驻、内存复用与基础内存规划。
- Conv/Conv3D、Dense、MatMul、Pool、Resize、Pad、Concat、Split、Transpose、Slice、Reduce、InstanceNorm、BN、激活等算子。
- 训练反向图、梯度检查、SoftmaxCE、Dice、Update，以及 CPU/GPU 接口化算子分派。
- 图优化：常量/形状折叠、DefUse、拓扑排序、DCE、Conv+BN、Conv+ReLU、Conv+残差 Add、GAP+Flatten、布局变换等。
- ONNX Runtime 交叉校验和真实 ResNet/3D U-Net 回归。

当前主要问题不是“缺少一个算子”，而是内部契约仍偏弱：

- `Graph` 仍把节点、shape、weight、分析、规划、runtime lowering 集中在一个类中。
- Value 主要以字符串和 `-1` 表示，容易产生悬空引用或 ID 混用。
- 当前动态 shape 实际是“加载时注入输入形状后静态化”。
- dtype、broadcast、layout 和 backend 能力尚未形成统一抽象。
- ONNX 支持范围、错误诊断、版本检查和模型缓存仍较有限。

## 2. 总体原则

1. **正确性优先于 pass 数量**：每个 pass 必须保持 IR、shape、DefUse、拓扑和 runtime wiring 一致。
2. **先稳定抽象，再扩展算子**：避免继续向 `Graph` 追加大量特殊字段和分支。
3. **按真实模型需求排序**：通过模型统计和 ORT 对拍决定优先级，不为没有目标的模式提前堆代码。
4. **CPU/GPU 语义一致**：同一 IR 和算子契约，后端只负责执行，不改变图语义。
5. **MYP 约束下采用组合**：使用公开的领域级 getter/setter 和独立组件，不依赖 partial class 或扩展方法。

## 3. 阶段一：Graph 数据域拆分与 IR 契约

### 目标

降低 `graph.myp` 的耦合，使优化器、分析器和 runtime lowering 可以独立演进。

### 推荐模块

```text
GraphFacade
├── GraphNodes       # NodeId、输入输出、属性、删除/替换
├── GraphShapes      # ValueId 的 shape、rank、dtype、layout
├── GraphWeights     # WeightId、initializer、常量、权重布局
├── GraphAnalysis    # DefUse、Topo、Liveness、Verifier
├── GraphOptimizer   # pass 实现与 pipeline
├── GraphPlanner     # 生命周期、arena 区域与持久化
└── GraphCompiler    # Graph IR -> InferenceRuntime
```

`GraphFacade` 是唯一对外暴露的组合对象。各子模块不直接访问其他模块的属性，
只通过 facade 的领域级 API 交换 `NodeId`、`ValueId`、`WeightId` 和计算结果。
因此 MYP 的 property 私有规则不会阻止拆分；getter/setter 是实现手段，但不应把
173 个并行数组原样暴露出去。

依赖方向固定为：

```text
GraphNodes ──────┐
GraphShapes ─────┼──> GraphAnalysis ──> GraphOptimizer
GraphWeights ────┘          │                 │
                            └──────────────> GraphPlanner
GraphOptimizer ───────────────────────────> GraphCompiler
GraphShapes + GraphWeights ────────────────> GraphCompiler
```

禁止反向依赖：`GraphNodes` 不依赖 optimizer，`GraphAnalysis` 不依赖 runtime，
`GraphCompiler` 不修改 IR。训练反向图属于 `GraphOptimizer` 的一个 graph-building
pass，不能继续把 Bwd*/Update 特殊逻辑塞进 runtime 或各个数据表。

### 各模块所有权

| 模块 | 拥有的数据 | 允许提供的核心 API |
|---|---|---|
| `GraphNodes` | `nType_`、`nIn*`、`nOut*`、节点属性和融合标志 | `addNode`、`nodeOp`、`nodeInput`、`nodeOutput`、`attr*`、`replaceOperand`、`eraseNode` |
| `GraphShapes` | `shName_`、`shD*`、`shR5_`、`shKind_`、`shNHWC_`、`shDead_` | `addShape`、`shapeOf`、`shapeDim`、`shapeElementCount`、`setShapeKind`、`markDead` |
| `GraphWeights` | `w*`、`file_`、int64/f32 常量池 | `addWeight`、`weightInfo`、`readF32`、`readI64`、`registerConstant`、`writeWeight` |
| `GraphAnalysis` | `du*`、`planOrder_`、analysis generation/cache | `ensureDefUse`、`valueProducer`、`valueUses`、`topoOrder`、`verifyIR` |
| `GraphOptimizer` | pass 状态与 pipeline 顺序 | `runPass`、`runToFixpoint`、`optimize`、`buildReverseGraph` |
| `GraphPlanner` | `plan*`、区域复用和 persist 标志 | `planMemory`、`plannedOffset`、`peakMemory` |
| `GraphCompiler` | IR 到 runtime 的临时映射 | `buildRuntime`、`runtimeTensorId` |

### 接口层规则

所有跨模块写操作必须经过 mutation API：

```myp
int addNode(int op, int[] operands, int[] results);
void replaceOperand(int node, int slot, int value);
void replaceAllUses(int oldValue, int newValue);
void replaceResult(int node, int slot, int value);
void eraseNode(int node);
void setShape(int value, int rank, int[] dims);
void setAttrInt(int node, int key, int value);
```

接口必须返回明确的失败值，并在 debug 模式提供 node/value 上下文。不得让外部
模块直接写 `nIn0_`、`shD0_`、`wOff_` 或 `planOff_`。

### 实施方式

不要为每一个私有数组机械生成 getter，而是提供领域级 API：

```myp
int nodeInputValue(int node, int slot);
int nodeOutputValue(int node, int slot);
int nodeShapeDim(int value, int axis);
int weightElementCount(int weight);
void replaceAllUses(int oldValue, int newValue);
void eraseNode(int node);
```

### 完整迁移顺序

虽然最终七个模块都要抽取，但不能按文件从上到下机械搬运。推荐四个可回滚切片：

1. **接口冻结**：保留现有 `Graph` 对外 API；把 `node*`、`shape*`、`weight*`、
  `attr*`、mutation 和 analysis API 补齐，旧实现仍在 Graph 内。
2. **数据域搬迁**：先搬 `GraphShapes` 和 `GraphWeights`，再搬 `GraphNodes`；
  facade 转发旧 API，所有现有 pass 仍能运行。
3. **分析与执行搬迁**：搬 `GraphAnalysis`、`GraphPlanner`、`GraphCompiler`；
  分别验证 DefUse/Topo、内存规划、runtime wiring。
4. **策略搬迁**：最后搬 `GraphOptimizer` 和 pipeline；GraphFacade 只保留组合、
  配置和生命周期，不再包含 pass 实现。

每一步都允许短期保留兼容转发，但禁止新代码继续直接访问旧数组。全部迁移完成后，
再决定是否把 SoA 换成对象式 Node/Value；这不是阶段一的前置条件。

### 当前进度（2026-09-01）

- **已完成：`GraphShapes`**。`infer/graph_shapes.myp` 独立拥有 shape 名称、维度、
  rank、kind、layout、dead 标记和计数；`Graph` 通过 `shapes_` facade 转发，旧的
  `shName_`/`shD*`/`shR5_`/`shKind_`/`shNHWC_`/`shDead_`/`sCount_` 已从 Graph
  property 移除。
- **已完成：`GraphWeights`**。独立拥有权重元数据与常量池；`Graph` 通过 `weights_`
  facade 转发，旧 `w*` 字段移除。
- **已完成：`GraphNodes` + `GraphNodeAttrs`**。节点记录（type/input/output/count）
  与全部属性、数组属性、融合/布局元数据均迁出；`Graph` 无节点 SoA。
- **已完成：`GraphAnalysis` / `GraphPlanner` / `GraphCompiler`（mapping + lowering）**。
  DefUse/topo/pass-result 状态、内存规划区域、runtime tensor 映射及完整
  `buildRuntime` lowering 算法均迁出。`GraphCompiler.lower(Graph, rt)` 经 `compiler*`
  公共访问器跨类查询，`Graph` 只保留组合与 host 转发。
- **已完成：`GraphOptimizer` orchestration**。推理/训练 pipeline 编排（runPipeline/
  optimize/optimizeTrain）移入 `GraphOptimizer`，经唯一 `IGraphOptimizeHost` 回调
  Graph 执行具体 pass。
- **已完成：全部具体 pass 算法迁移**。`GraphOptimizer` 现拥有全部 pass 实现：
  `eliminateDeadNodes`/`fuseGapFlatten`/`foldIdentityOps`/`foldDoubleRelu`/
  `fuseConvRelu`/`fuseReluOp`/`fuseConvAdd`/`topoSort`/`planMemory`/`layoutNHWC`/
  `classifyShapes`/`fuseConvBN`/`buildReverseGraph`/`inferShapes`/`foldShapeChains`/
  `foldConstants`（+ `normalize3DNode`/`sliceAxis` helper）。每个 pass 以
  「具体 `Graph` 参数 + `compiler*` 公共访问器」跨类访问 Graph 状态；Graph 的
  `runIRPass` 只做分发委托。Graph 从 ~3670 行降至 ~1120 行，仅保留组合、解析器
  入口 API、i64/protobuf 字节解释器（经桥暴露）、runIRPass 骨架与 `compiler*`
  窄桥——达成阶段一「GraphFacade 只保留组合、配置和生命周期」目标。
- **已验证**：**27/27 个 infer_tests + grad_check（L0=1.92528）在
  `MYP_GPU=1 MYP_IR_VERIFY=1` 下全部通过**；ResNet output sum `336.658`。
  关键提交：`f6f969a`（Shapes）、`1e2589d`（Weights）、`493d31b`/`87b8665`/
  `83220b8`（Nodes）、`8cce71d`（Analysis）、`9e14439`（Planner）、`9a2fb26`/
  `732f30e`（Compiler）、`824c376`/`f59337f`/`3eeef70`/`cd640cf`/`095157a`/
  `b6963f9`/`2a26977`/`23d0a52`/`807c1d8`/`c992df5`/`82fd763`/`8859fa4`/
  `ffc5dbf`（Optimizer 与 pass 迁移）。
- **阶段一完成**：Graph 已拆分为 `GraphShapes`/`GraphWeights`/`GraphNodes`/
  `GraphNodeAttrs`/`GraphAnalysis`/`GraphPlanner`/`GraphCompiler`/`GraphOptimizer`
  八个独立组件，`Graph` 为组合根（facade），无 pass 算法残留。

`GraphWeights` 的迁移边界进一步固定为：

```text
GraphWeights
├── WeightId -> name/shape/dtype/role/layout/storage
├── ByteSource -> 原始 ONNX 文件的只读字节访问
├── Int64Pool -> Shape/Slice/Concat/Gather 等形状常量
├── F32Pool -> Cast/计算得到的运行时常量
└── FoldMetadata -> BN/布局/转置等写权重前元数据
```

`GraphWeights` 不依赖 `InferenceRuntime`，也不负责决定节点是否使用某个权重；
节点到权重的语义仍由 `GraphNodes`/`GraphOptimizer` 决定。`GraphCompiler` 只能通过
`weightRead`、`weightInfo` 和 `writeWeight` 读取它，不能访问 weight 数组。

### MYP 实现决策（已通过探针验证）

MYP `struct` 的字段可以被其他类直接读写；定长数组字段也可正常修改。因此
`GraphWeights` 采用 `struct` 而不是带几十个 setter 的 class：

```myp
struct GraphWeights {
  string[256] name;
  int[256] rows;
  int[256] cols;
  int count;
}

class Graph {
  action:
    void setWeightRow(int id, int value) { weights_.rows[id] = value; }
  property:
    GraphWeights weights_;
}
```

迁移规则：先把完整字段组放入 `GraphWeights`，再按字段组把 Graph 内的直接访问改为
`weights_.field`；全部引用迁完后删除 Graph 中旧字段。迁移期间允许编译器逐字段报错，
但不允许最终保留两份可写权重状态。此次探针提交前缀为 `array_owner_probe`，验证了
class setter 和 struct array direct access 两种路径均可编译运行。

### 验收标准

- 现有 `Graph` 公共 API 不变。
- `infer_tests` 和训练回归不改行为。
- `MYP_IR_VERIFY=1` 下所有模型通过。
- 每个活节点的输入、输出、shape、DefUse、producer 和 use edge 可单独验证。
- `Graph` 不再直接依赖另一组件的私有数组。
- 七个模块可以独立编译/测试；GraphFacade 的公共 API 和 `OnnxLoader` API 不变。
- `GraphCompiler` 是唯一允许依赖 `InferenceRuntime` 的 Graph 子模块。
- 所有 pass 通过 mutation API 改图，任何直接数组访问都只允许暂时存在于尚未迁移的旧实现。

## 4. 阶段二：强类型 Value 与统一张量描述

### 目标

减少字符串和哨兵值带来的错误。

### 建设内容

引入统一概念：

```text
NodeId
ValueId
WeightId
ShapeId
DType
Layout
OpKind
```

优先级：

1. 在现有字符串 API 外包一层 `ValueId` 访问器；
2. 禁止新 pass 直接使用 `shapeIdx(name)` 和 `-1` 判断；
3. 统一 `shape / dtype / layout / storage role`；
4. 区分 data tensor、shape tensor、initializer 和 graph input；
5. 将参数输入（pads、sizes、axes 等）与计算输入明确建模。

这一步可以从根源上避免 Resize/Pad 已发现的“折叠生产者后留下悬空输入”问题。

### 当前进度（2026-09-01）

- **已完成：G2 ValueId/dtype/role 建模**。`graph_defs.myp` 加 `DType`/`TensorRole`
  常量类；`GraphShapes` 加 `shDType_`/`shRole_` + 访问器；`Graph` 加 `valueExists`/
  `valueDType`/`valueRole` 语义访问器（替代 `shapeIdx(x)>=0`/`-1` 裸判断）；`GraphOptimizer`
  3 处 `-1` 哨兵替换为 `compilerValueExists`。`onnx_loader` 在加载时登记 dtype+role
  （initializer→WEIGHT、input→GRAPH_IN、output→GRAPH_OUT、int64 参数→SHAPE/INT64、
  float 参数→PARAM）；`classifyShapes` 激活传播设 DATA role + 传播 dtype。ResNet50
  全 231 张量分类完成（DATA=121/WEIGHT=108/GRAPH_IN=1/GRAPH_OUT=1，FLOAT=231，
  UNKNOWN=0）。新增 `infer_tests/g2_probe_main.myp` 加载级回归。
- **已完成：阶段二收尾（2026-09-01）**。`graph_optimizer.myp` 全部 37 处
  `compilerShapeIdx(x) < 0` 裸 `-1` 哨兵存在性判断替换为
  `compilerValueExists(x) == 0` 语义调用——覆盖 inferShapes 全部 35 个分支 +
  `buildReverseGraph`（logits 存在）+ `foldShapeChains`（Shape 节点输入存在）。
  剩余 `< 0` 判断均为节点/计划/权重 index（`matchSingleUseOp`/`lastUse`/
  `picked`/`initIdx` 等），非 shape 存在性语义，保留。阶段二第 1/2 条（ValueId
  外层 + 全面禁裸哨兵）闭环：新 pass 一律用 `compilerValueExists` 语义判断，
  后续 ValueId 重构只改 `valueExists` 一处。
- **已完成：G3 ReduceMean 负轴归一化**。inferShapes 负轴 +rank 后分类 mode，
  为阶段四 ReduceSum/Max/Min 铺路。
- **已完成：阶段四第一优先级 ReduceSum/Max/Min**（commit `0440a18`）。OpCode
  54-56 + `nRedType_`（0=mean,1=sum,2=max,3=min）；inferShapes Reduce 族共用
  axes 归一化 + mode 判定；runtime `addReduceMean` 加 redType（opP6）；ops kernel
  支持 mean/sum/max/min 聚合；gpu_ops 支持 mean/sum（max/min GPU 未实现，打印
  警告）。新增 `tools/make_reduce_onnx.py`（opset12 合成模型，空间+全规约）+
  `infer_tests/reduce_main.myp`（REDUCE ALL OK，CPU+GPU max diff 0 vs ORT）。
  GPU max/min 并行归约已补全（commit `62b8b21`，初值 ±1e30，按 redType 分派）。
- **已完成：阶段四 G4 Expand 算子**（commit `bad6242`）。OpCode `EXPAND(58)` +
  `opCode` 映射；`inferShapes` Expand 输出维度取 int64 shape 输入[1]（4D）；
  `classifyShapes` 把 shape 张量标 SHAPE/INT64（dead）；`graph_compiler`
  EXPAND wiring → `rt.addExpand(id0-3, od0-3)` opKind=74（**教训**：初版误用 73
  与 ConvResidual 冲突且 ExpandOp 未注册 → dispatch 到 ConvResidual 内核触发
  SIGFPE；改 74 + 补 `registerFwd(74)` 修复）。CPU/GPU broadcast 复制 kernel +
  `ExpandOp`/`GpuExpandOp`。新增 `tools/make_expand_onnx.py`（opset13：
  x[1,2,1,1]→Expand(shape=[1,2,3,4])→y[1,2,3,4]）+ `infer_tests/expand_main.myp`
  （EXPAND ALL OK，CPU+GPU max diff 0 vs ORT）。
- **已完成：阶段四 G5 Where 算子**（commit `6552144`）。OpCode `WHERE(75)` +
  `opCode` 映射；`inferShapes` 输出 = cond/x/y 逐维 max 广播（缺省 1）；
  runtime `addWhere(cond,x,y,out)` opKind 75，4D 维度由 forward 从各 tid
  `tN_/tC_/tH_/tW_` 读（免额外参数槽）；CPU/GPU kernel 三输入各自 4D 广播
  （维=1→0）。**教训**：`where` 是 MYP 保留字（mapping 过滤），函数名用
  `where1`（保留字做函数名 → 整文件解析错乱）。新增 `tools/make_where_onnx.py`
  （opset13：cond[1,2,1,1]?x[1,2,3,4]:y[1,2,3,4]→out，cond 广播）+
  `infer_tests/where_main.myp`（WHERE ALL OK，CPU+GPU max diff 0 vs ORT）。
- **已完成：阶段四 G5 Tile 算子**（commit `0b4eeb2`）。OpCode `TILE(76)` +
  `opCode` 映射；`classifyShapes` repeats 初始器标 SHAPE/INT64 dead；
  `inferShapes` 输出 = 输入维 × repeats 维；runtime `addTile(in,out,id0-3,od0-3)`
  opKind 76；CPU/GPU kernel 输出 index % 输入维（与 where1/expand 同构的 4D
  broadcast decode，输入维可为任意倍数）。新增 `tools/make_tile_onnx.py`
  （opset13：in[1,2,3,4] tile [1,2,2,3] → out[1,4,6,12]）+
  `infer_tests/tile_main.myp`（TILE ALL OK，CPU+GPU max diff 0 vs ORT）。
- **已完成：阶段四 G5 Squeeze + Gather**（commit `9f5dfee`）。**Squeeze**（opKind
  77）：去 size-1 维，inferShapes 输出保持轴顺序左对齐补 1，数据不变复用
  copyFlat（CPU+GPU）。**Gather**（opKind 78）：沿 axis 收集，axis 属性（opset11），
  int64 indices 初始器标 dead，graph_compiler `readI64Init` 读入临时 f32 张量
  （int→float），kernel 按 gatherRows 模式读 `int(a[iOff+k])`（负索引+dim、clamp）。
  新增 `tools/make_squeeze_onnx.py`/`make_gather_onnx.py` +
  `squeeze_main.myp`/`gather_main.myp`（SQUEEZE/GATHER ALL OK，CPU+GPU max diff
  0 vs ORT）。
- **已完成：LogSoftmax + Embedding**（commit `d3bd44f`）。**LogSoftmax**（opKind
  79）：沿 axis 稳定 log-softmax（outer/axisDim/inner，负轴 +rank，每列
  max+Σexp+lse），CPU+GPU max diff 4.8e-7（float 精度）。**Embedding**（opKind
  80）：row-major 查表 out[s*D+d]=w[ids[s]*D+d]（ONNX 无标准 op，用 Gather
  axis=0 表达；本 op 作框架层查表），ids int64 初始器标 dead + readI64Init 读
  临时 f32 张量，CPU+GPU max diff 0（直接 runtime 单测）。
- **已完成：Dropout**（commit `fbcad0f`）。opKind 81：opset12（ratio f32 初始器
  输入，training_mode 省略 → 推理）；`rt.trainMode()` 分派——推理恒等
  （copyFlat，CPU+GPU max diff 0 vs ORT）；训练确定性 LCG 序列随机 mask
  （zero rate 0.497 ≈ 0.5）。GPU 训练路径退化恒等（无模型驱动的 GPU 随机）。
- **已完成：阶段七 Unsupported op 诊断**（commit `627a58c`）。loader 解析时检测
  未知 ONNX op → 打印 op 类型/节点索引/输入名 + `badOp_` 标记 → `load()` 返回 0
  （显式失败而非静默错误/段错误）。OpCode 补 `CONSTANT(57)` 防误报。
- **已验证**：38/38 infer_tests + grad_check（L0=1.92528，GRAD CHECK OK）在
  `MYP_GPU=1 MYP_IR_VERIFY=1` 下全部通过；ResNet output sum `336.658`。关键
  提交：`8a38beb`（G2 基础）、`9508a10`（DATA role）、`88d0fd2`（G3 负轴）、
  `0440a18`（Reduce 族）、`627a58c`（op 诊断）、`bad6242`（Expand）、
  `6552144`（Where）、`0b4eeb2`（Tile）、`9f5dfee`（Squeeze+Gather）、
  `d3bd44f`（LogSoftmax+Embedding）、`fbcad0f`（Dropout）。
- **下一步**：G3 剩余（broadcast/ShapeExpr/dtype 转换规则）、第二优先级训练/
  生成式模型专项剩余（Checkpoint、梯度累积、混合精度）与第一优先级
  MatMul 广播（BatchMatMul）。当前模型集为 4D/5D 静态 shape + 全 FLOAT +
  同形状广播，broadcast 统一与 ShapeExpr 无模型驱动——按「不为没有目标的模式
  提前堆代码」，优先推进有合成测试可验证的算子补全。

## 5. 阶段三：Shape、DType 与 Broadcast 基础层

### 目标

从固定 4D/5D 特化框架，提升为支持常见模型的静态 shape 推导框架。

### 内容

- `ShapeExpr`：常量维度、输入维度、乘法、加法和除法等表达式。
- 动态 batch、动态 H/W/D，以及输入 shape specialization。
- 运行时 shape 与编译期 shape 的明确边界。
- FP32、FP16、BF16、INT8 的 dtype 描述和转换规则。
- 统一 NumPy/ONNX broadcast 规则。
- Reduce 的 axes、keepdims、负轴和空 axes 语义统一。
- shape tensor 与 data tensor 的静态类型检查。

### 推荐范围

先支持“加载时输入 shape 注入 + 编译期 shape specialization”，再考虑完整运行时动态 shape。对于当前 3D U-Net，完整动态 shape 不是优先需求。

### 当前进度（2026-09-01）

- **已完成：统一 numpy 广播（Sub/Div/Mul 4D）**（commit `eec0b50`）。inferShapes
  Sub/Div/Mul 输出 = a/b 逐维 max（广播合并）；kernel 保留标量/同尺寸/逐通道
  快路径 + 新增通用 4D 广播 fallback（输出 index % 各输入维，维=1 → 0，复用
  where/tile decode 模式）；forward 从 out tid 读输出 dims（免额外参数槽）。
  **修复**：同尺寸快路径收紧为 a/b/out 三维全同（`bSize==n` 在 a 广播时误判 →
  a 越界读）。新增 `tools/make_bcast_onnx.py`（opset13，12 输入 6 输出覆盖
  同形状/标量/逐通道/W/HW/b 放大）+ `infer_tests/bcast_main.myp`（BCAST ALL OK，
  CPU+GPU max diff 0 vs ORT）。
- **已完成：Add 统一 numpy 广播（4D）**（commit `9cc8841`）。Add 从 element-wise
  copyShape 拆出 → 逐维 max；runtime `addAdd` 传 a/b dims（doRelu 保留）；kernel
  标量/同尺寸/逐通道 [1,C,1,1]/通用 4D 广播 fallback（doRelu 融合贯穿）；
  graph_compiler ADD + GRAD_ACC wiring 传 dims。bcast 测试扩展 out7/out8（Add
  同形状 + 逐通道 bias），8 输出 BCAST ALL OK，CPU+GPU max diff 0 vs ORT。
  至此 **Add/Sub/Mul/Div 四元逐元素 op 全部支持统一 4D numpy 广播**。
- **已完成：FP16 权重 dtype 转换**（commit `859b876`）。`graph.myp` 加
  `halfToF32Bits`（IEEE half → F32，含亚正规）；`writeWeight` dtype==10 分支每
  2 字节 half → float（直接 wire 拷贝）；`onnx_loader` parseTensor 支持
  `float16_data`（字段 6）。`make_fp16_onnx.py`（opset13 Conv W/B FLOAT16）+
  `fp16_main.myp`：FP16 ALL OK，CPU+GPU max diff 0 vs ORT。
- **已完成：BF16 权重 dtype 转换**（commit `235df45`）。`writeWeight` dtype==16
  分支每 2 字节 → `bits = v << 16`（BF16 = F32 高 16 位）；loader 支持
  `bfloat16_data`（字段 16）。ORT CPU 不支持 BF16 → `make_bf16_onnx.py` 用
  numpy 手算参考；`bf16_main.myp`：BF16 ALL OK，CPU+GPU max diff 6e-8。
  阶段三 dtype 转换（FP16/BF16）完成，剩 INT8（需量化 scale/zero_point 语义）。
- **已完成：动态 batch 输入 shape 注入 + specialization**（commit `d48904b`）。
  **修复 `parseDim`**：默认 v=0（原来 1）——ONNX 动态维是"空 dim"（未设置
  dim_value，protobuf 省略默认 0），解析为 1 → 登记 d0=1 → inferShapes
  `addShapeD4` 覆盖条件（任一维==0）不触发 → 注入 batch 后输出 batch 卡 1；
  修复后空/dim_param/dim_value=0 全 → 0，`setInputShape`/覆盖正常。
  `make_dynbatch_onnx.py`（x[N,1,5,5] Conv → y[N,1,3,3]）+ `dynbatch_main.myp`
  （注入 batch=2，DYNBATCH ALL OK，out size=18 非 9）。
- **G3 ReduceMean 负轴归一化**已完成（见阶段二进度区）；Reduce axes/keepdims/
  空 axes 语义统一仍在 Reduce 族共用归一化内。
- **待办**：ShapeExpr（常量/输入维度表达式）、INT8 dtype 转换（需量化
  scale/zero_point 语义，非量化模型场景少）。

## 6. 阶段四：算子覆盖优先级

### 第一优先级：提高模型覆盖率

- `Gather`（✅ 完成，commit `9f5dfee`）
- `Expand`（✅ 完成，commit `bad6242`）
- `Where`（✅ 完成，commit `6552144`）
- `Tile`（✅ 完成，commit `0b4eeb2`）
- `Squeeze`（✅ 完成，commit `9f5dfee`）
- `ReduceSum/ReduceMax/ReduceMin`（✅ 完成，commit `0440a18` + `62b8b21`）
- 完整 broadcast 的 `MatMul/BatchMatMul`（✅ 完成，commit `a0646dc`：4D batch
  matmul + batch 维逐维 max 广播；2D 路径不变）
- `LogSoftmax`（✅ 完成，commit `d3bd44f`）
- `LayerNorm/RMSNorm`（✅ 已有 opKind 64/61）
- `Embedding`（✅ 完成，commit `d3bd44f`）

### 第二优先级：训练和生成式模型

- Attention（✅ 已有 opKind 62）
- KV cache（✅ 已有）
- RoPE（✅ 已有 opKind 66）
- GELU 完整版本（✅ 已有 opKind 65）
- `LogSoftmax`（✅ 完成，commit `d3bd44f`）
- `Embedding`（✅ 完成，commit `d3bd44f`，框架层 row-major 查表；ONNX 常规用 Gather axis=0）
- `Dropout` 的训练/推理语义（✅ 完成，commit `fbcad0f`）
- Checkpoint、梯度累积和混合精度

### 暂不作为主线的 pass

当前提出的 #14–#25 中，以下属于模型驱动的专项功能，暂不应优先实现：

- Conv+Sigmoid、Conv+Swish：当前模型没有实际目标。
- No-op Transpose：当前模型目标极少。
- NCHW↔NCHW 变换：需要真实 layout 不一致模型后再做。
- Mul/Add 链式代数化简：需要先定义严格的广播、浮点重排和精度语义。
- CSE：应在 ValueId、side-effect 和属性等价规则稳定后实现，不能只按字符串比较。

## 7. 阶段五：图优化 pass 路线

### 先做且 ROI 明确

1. **常量去重**：✅ 已完成（2026-09-02，DCE+死权重裁剪后 `compilerWeightBytesEqual`
   两两比较；内容+role/transposed 相同才合并，后者改接+标 dead；
   `infer_tests/dupconst_main.myp` 回归 CPU+GPU 通过）。
2. **折叠后死权重裁剪**：✅ 已完成（2026-09-02，`pruneDeadWeights` 挂在
   `eliminateDeadNodes` 末尾，遍历权重标记未被活节点引用的初始器为 dead；
   `infer_tests/deadweight_main.myp` 回归 CPU+GPU 通过）。
3. **形状值传播**：✅ 已完成（2026-09-02，foldShapeChains 新增 int64 四则
   Add/Sub/Mul/Div + ReduceSum + Expand；`infer_tests/shapeprop_main.myp`
   Shape→Slice→Mul→Concat 驱动 Resize sizes 回归 CPU+GPU 通过）。
4. **Conv 1x1 专用 lowering**：✅ 已完成（2026-09-02，opKind 83 `conv1x1`
   GEMM 化——thread-per-output-pixel × 通道归约，无窗口循环；1x1+ReLU 也走它；
   `infer_tests/conv1x1_main.myp` 回归 CPU+GPU 通过，断言 op 表含 83）。
5. **算子选择**：✅ 已完成（2026-09-02，BatchMatMul batch 维全 1 降级 2D
   matmul——CPU @parallel GEMM / GPU denseTiled 分块 GEMM；`infer_tests/
   opselect_main.myp` 回归 CPU+GPU 通过）。**阶段五「先做且 ROI 明确」5 项
   全部完成**。

### 后续实现

- CSE：需要纯函数 op 白名单、输入顺序/交换律规则、属性完整比较、side-effect 保护和 fixpoint 上限。
- Bias/Scale/Shift 融合：只允许明确的常量广播形状，必须有 CPU/GPU 对拍。
- No-op/连续 Transpose 消除：必须根据 permutation 组合证明等价，不能只看节点名称。
- Output pruning：与现有 DCE 合并设计，避免两套死节点逻辑。

## 8. 阶段六：Backend 抽象与性能

### 目标架构

```text
Graph
  -> BackendPlanner
  -> BackendProgram
  -> CPU / CUDA kernels / cuBLAS / cuDNN
```

### 优先级

1. GPU arena 常驻与增量同步继续完善；✅ P5b 机制已就绪（gpuInferStart/
   gpuPersistentStart 增量脏输入 H2D + markGpuSync 输出 D2H），2026-09-02 补
   多帧增量同步正确性回归 `infer_tests/gpupersist_main.myp`（帧2 只上传 1 个
   脏输入，输出 vs ORT 一致，CPU 回退同样正确）。
2. cuBLAS GEMM；✅ 已完成（2026-09-02，GPU dense/matmul 规模达标时优先 cuBLAS
   SGEMM——列主序映射 y^T=x^T·W^T，m=batch,n=outDim,k=xRows，dev+off*4 指针；
   失败回退 denseTiled；`infer_tests/cublas_main.myp` 回归 CPU+GPU 通过，
   MYP_CUBLAS_LOG 确认触发）。
3. cuDNN Conv/Pool（可用时）；
4. CPU 多线程、SIMD 和 cache blocking；
5. CUDA stream/event 与异步传输；
6. 根据问题规模自动选择 plain、tiled、GEMM 或厂商库实现；
7. 明确 unsupported op 的 CPU fallback 和性能提示。

性能优化必须以算子级 profile 为依据。当前经验表明，Conv/Conv3D 是主要瓶颈，逐元素融合不一定带来收益，可能因 arena 布局变化反而变慢。

## 9. 阶段七：模型工程能力

- ONNX opset/version 检查。✅ 已完成（2026-09-02，parseModel 解析 ir_version +
  opset_import；irVersion()/opsetVersion()/opsetDomain()/opsetSupported()；
  `infer_tests/opsetcheck_main.myp` 回归 opset 13/21 CPU+GPU 通过）。
- Unsupported operator 诊断：节点名、op type、输入输出和原因。✅ 已完成
  （2026-09-02，诊断含节点名/op/输入/输出 + 原因分类——If/Loop/Scan 控制流子图
  明确声明不支持，其他 operator not implemented；`infer_tests/unsup_main.myp`
  回归 If/FakeOp load=0 + 诊断完整，known-good 不误伤）。
- Unsupported operator 诊断：节点名、op type、输入输出和原因。
- 多输入、多输出和可选输入完整处理。
- `If`、`Loop`、`Scan` 子图支持，或明确声明不支持。
- 外置 weight、模型 mmap/分块读取。✅ 已完成（2026-09-02，ONNX
  `data_location=EXTERNAL` external data 权重读取 + 模型 mmap 零拷贝加载：
  PbReader/Graph 字节源双模式抽象（mmap 主文件 + int[] 追加区），
  `loadMmap` 大模型免 int[] 4x 拷贝；`infer_tests/mmap_main.myp` 回归
  CPU+GPU 通过，真实 ResNet50 102MB loadMmap 输出 336.658 精确保持）。
- 优化后 IR/计划缓存。✅ 已完成（2026-09-02，`InferenceRuntime` 新增
  `dumpPlan`/`loadPlan`——op 表 + tensor 表 + Slice 参数 + arena 权重序列化到
  磁盘，重复加载 loadPlan 直接恢复跳过 ONNX 解析+优化+buildRuntime；注意 ONNX
  路径 arenaTop_ 不 bump 需序列化 arenaCap_-statsSize 全数据区；
  `infer_tests/plan_main.myp` + `planr18_main.myp`（真实 ResNet18 47MB 34 op）
  回归 CPU+GPU 通过）。
- shape specialization 和多个 shape bucket。✅ 已完成（2026-09-02，动态
  batch 输入 shape 注入 + 编译期 specialization 对多 bucket 验证：同一模型
  注入 batch 2/4/8 各 build 一个 runtime 输出各自 vs ORT 正确；
  `infer_tests/shapebucket_main.myp` 回归 CPU+GPU 通过）。
- 加载、编译、执行三个阶段的错误码与统计信息分离。
- 保持 `onnx_loader.myp` 只负责格式解析，通用逻辑留在 `graph.myp` 及其组件。

## 10. 阶段八：测试与发布质量

每个算子和 pass 都应覆盖四层测试：

1. **IR 单元测试**：构造小图，验证节点、边、shape 和属性。
2. **CPU/GPU 对拍**：固定输入，逐元素比较。
3. **Pass 等价性测试**：优化前后输出、graph output、dtype、shape 一致。
4. **真实 ONNX/ORT 测试**：覆盖代表性模型和多个输入 shape。

每次 pass 变更至少执行：

```text
verifyIR
verifyShapes
verifyDefUse
verifyTopo
verifyRuntimeWiring
```

新增回归模型必须放入 `infer_tests/`，正向语言特性测试放入 `tests/@test/`，bug 复现放入 `tests/bugs/`。

## 11. 推荐执行顺序

```text
G1  GraphAnalysis + GraphShapes 拆分，完善 verifier
G2  ValueId/NodeId/ShapeId 外层契约
G3  dtype/layout/broadcast/ShapeExpr 基础层
G4  Gather/Expand/Where/Reduce/LayerNorm 等覆盖率算子
G5  常量去重、死权重裁剪、形状值传播
G6  Conv 1x1 + backend kernel selection
G7  cuBLAS/cuDNN、异步执行、多后端能力查询
G8  动态 shape specialization、量化、控制流和 KV cache
```

## 12. 阶段完成定义

“通用框架”不以支持的 pass 数量定义，而以以下条件定义：

- 新增一个算子只需实现统一的 shape、CPU、GPU/backend 和测试契约。
- 新增一个 pass 不需要直接访问其他模块的私有数组。
- 任意 pass 后都能验证 IR 和 runtime wiring。
- 不支持的模型会给出具体诊断，而不是 segfault、空指针或静默错误。
- 同一个优化图可以选择 CPU、CUDA 或厂商库 backend，结果语义一致。
- 动态输入 shape 在支持范围内可以通过 specialization 复用编译结果。
