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
Graph
├── GraphNodes       # 节点、输入输出、属性、删除/替换
├── GraphShapes      # shape、rank、dtype、layout
├── GraphWeights     # initializer、常量、权重布局
├── GraphAnalysis    # DefUse、Topo、Liveness、Verifier
├── GraphOptimizer   # pass 实现与 pipeline
├── GraphPlanner     # 生命周期与 arena 规划
└── GraphCompiler    # Graph -> InferenceRuntime
```

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

第一批先抽取 `GraphAnalysis` 和 `GraphShapes`，原因是：

- DefUse/Topo/Verifier 边界清晰；
- shape 访问已经存在多个统一入口；
- 能直接验证“组合 + getter/setter”是否适合 MYP；
- 对后续 CSE、动态 shape 和内存规划都有基础收益。

### 验收标准

- 现有 `Graph` 公共 API 不变。
- `infer_tests` 和训练回归不改行为。
- `MYP_IR_VERIFY=1` 下所有模型通过。
- 每个活节点的输入、输出、shape、DefUse、producer 和 use edge 可单独验证。
- `Graph` 不再直接依赖另一组件的私有数组。

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
