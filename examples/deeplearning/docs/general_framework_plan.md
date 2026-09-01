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
- **已完成：G3 ReduceMean 负轴归一化**。inferShapes 负轴 +rank 后分类 mode，
  为阶段四 ReduceSum/Max/Min 铺路。
- **已验证**：28/28 infer_tests + grad_check（L0=1.92528）在 `MYP_GPU=1
  MYP_IR_VERIFY=1` 下全部通过；ResNet output sum `336.658`。关键提交：`8a38beb`
  （G2 基础）、`9508a10`（DATA role）、`88d0fd2`（G3 负轴）。
- **下一步**：G3 剩余（broadcast/ShapeExpr/dtype 转换规则）与阶段四算子
  （Gather 完整/ReduceSum/ReduceMax/ReduceMin/Expand/Where/Tile/Squeeze）。
  当前模型集为 4D/5D 静态 shape + 全 FLOAT + 同形状广播，broadcast 统一与
  ShapeExpr 无模型驱动——按「不为没有目标的模式提前堆代码」，优先推进有合成
  测试可验证的算子补全（ReduceSum/Max/Min 与已有 ReduceMean 同构）。

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

## 6. 阶段四：算子覆盖优先级

### 第一优先级：提高模型覆盖率

- `Gather`
- `Expand`
- `Where`
- `Tile`
- `Squeeze`
- `ReduceSum/ReduceMax/ReduceMin`
- 完整 broadcast 的 `MatMul/BatchMatMul`
- `LogSoftmax`
- `LayerNorm/RMSNorm`
- `Embedding`

### 第二优先级：训练和生成式模型

- Attention
- KV cache
- RoPE
- GELU 完整版本
- Dropout 的训练/推理语义
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

1. **常量去重**：相同内容的 Constant/initializer 合并，减少 IR 和显存占用。
2. **折叠后死权重裁剪**：融合和 DCE 后移除不再被 runtime 引用的权重。
3. **形状值传播**：扩展现有 int64 shape folding，覆盖 Gather/Expand/Reduce 等常见链。
4. **Conv 1x1 专用 lowering**：ResNet bottleneck 的实际高频路径，优先于 Conv+Sigmoid 等融合。
5. **算子选择**：根据 shape、dtype、layout 和 backend capability 选择 plain/tiled/library kernel。

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

1. GPU arena 常驻与增量同步继续完善；
2. cuBLAS GEMM；
3. cuDNN Conv/Pool（可用时）；
4. CPU 多线程、SIMD 和 cache blocking；
5. CUDA stream/event 与异步传输；
6. 根据问题规模自动选择 plain、tiled、GEMM 或厂商库实现；
7. 明确 unsupported op 的 CPU fallback 和性能提示。

性能优化必须以算子级 profile 为依据。当前经验表明，Conv/Conv3D 是主要瓶颈，逐元素融合不一定带来收益，可能因 arena 布局变化反而变慢。

## 9. 阶段七：模型工程能力

- ONNX opset/version 检查。
- Unsupported operator 诊断：节点名、op type、输入输出和原因。
- 多输入、多输出和可选输入完整处理。
- `If`、`Loop`、`Scan` 子图支持，或明确声明不支持。
- 外置 weight、模型 mmap/分块读取。
- 优化后 IR/计划缓存。
- shape specialization 和多个 shape bucket。
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
