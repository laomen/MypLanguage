# MYP 深度学习推理/训练框架 — 设计说明

> 适用范围：`examples/deeplearning` 的 `infer/` 核心库 + `dl`（Session）统一入口
> （纯 MYP 实现的通用静态图**推理 + 训练**框架）。
> 路线图与性能范式见 `docs/gpu_paradigm.md`；功能/里程碑时间线见 `CHANGELOG.md`；
> 用户速览见 `../README.md`（中英）与 `docs/manual.md`/`docs/manual_EN.md`（已并入原 sli/usage）。
> 文档版本：**2026-09-03**（模块化 Graph IR + 声明式 JSON + Session 训练 + P10b GPU 反向之后）。

---

## 1. 定位与设计目标

- **纯 MYP 实现**：运行时零 Python / onnxruntime；C 层仅保留 GPU FFI（`runtime_gpu.c`）
  与 cuBLAS/cuDNN 类厂商库调用。
- **一套图 IR，两种模型源**：静态计算图（张量表 + 算子表 + 权重表）与具体来源解耦——
  **ONNX**（`onnx_loader.myp` 解析）与**声明式 JSON**（`json_model.myp` 直接构建）都填进
  同一 `Graph`，走同一 pass 管线 → 同一 `InferenceRuntime`。
- **推理 + 训练一体**：同一张 runtime 图既能 `run()`/`runGpu()` 前向推理，也能
  `Session` 静态反向图训练（自动补 label/loss/反向/Update）；CPU 与 GPU 双后端。
- **FP32 精度**：激活/权重默认 `float[]` arena 承载，与 onnxruntime 逐元素对拍；
  权重支持 FP16/BF16 输入（载入即转 FP32），训练 AMP 用 fp16 梯度舍入模拟骨架。
- **图优化**：load 时跑 pass 管线（常量折叠/融合/DCE/常量去重/形状值传播/布局变换/
  拓扑/内存规划），减少 kernel 启动数与峰值内存。
- **正确性门禁**：`MYP_IR_VERIFY=1` 五重 verifier + 全量端到端回归
  （`infer_tests/run_all.sh` → pass=135）。

---

## 2. 目录布局（2026-09-03）

```
deeplearning/
├── dl/
│   └── dl.myp                # ★ SLI 统一入口：`import dl;` → Session facade（薄转发）
├── infer/                    # ★ 核心库（纯 MYP）
│   ├── framework.myp         # SLI facade：class Session（load/run/train/dump/checkpoint）
│   ├── runtime.myp           # InferenceRuntime：tensor/op 注册 + run()/runGpu() + 优化器/累积
│   ├── ops.myp               # CPU 算子内核（~82 opKind，batch-aware，FP32）
│   ├── gpu_ops.myp           # GPU 算子内核（@gpu for resident，FP32）
│   ├── op_iface.myp          # interface IOp {forward,backward}（阶段4e POC，relu 示例）
│   ├── ops_iface_all.myp     # ★ 全量算子注册表：每 op 的 CPU/GPU 类 → fwd/bwd 槽
│   ├── graph_defs.myp        # 图 IR 常量：Kind / NodeField / OpCode 枚举
│   ├── graph.myp             # Graph 组合根（领域访问器 + 算法编排）
│   ├── graph_nodes.myp       #   节点表 SoA 存储（nodes 拥有）
│   ├── graph_weights.myp     #   权重/初始器 SoA 存储
│   ├── graph_shapes.myp      #   形状表 SoA（dims/rank5/kind/NHWC/dead）
│   ├── graph_node_attrs.myp  #   节点标量属性槽（NodeField）
│   ├── graph_planner.myp     #   内存规划可变状态
│   ├── graph_analysis.myp    #   分析状态（活性等）
│   ├── graph_optimizer.myp   # ★ pass 管线编排（GraphOptimizer）
│   ├── graph_compiler.myp    # ★ Graph IR → runtime 降级（buildRuntime/lowering）
│   ├── graph_shapes.myp      # （见上）
│   ├── onnx_loader.myp       # ONNX 解析（protobuf wire）→ Graph + 训练图构建 + 错误码
│   ├── json_model.myp        # ★ 声明式 JSON 模型 → Graph（不经过 ONNX）
│   ├── pb.myp                # protobuf wire-format 读取器（读 .onnx）
│   ├── safetensors.myp       # safetensors 权重读取（F32/BF16/F16，mmap）
│   ├── tensor.myp            # 张量索引辅助
│   └── tools/                # Python 辅助（make_*_onnx.py 合成模型 + ORT 参考 / onnxvenv）
├── infer_tests/              # 端到端回归入口（135 个 *_main.myp；run_all.sh）
├── json_tool/                # JSON 图小工具（独立演示 + CLI）
├── train/  llm/  diffusion/  # 相邻分项目（3D 训练 / Qwen2+distilgpt2 / SD1.5 文生图）
├── data/                     # 模型/输入/数据集（git 忽略）
└── docs/                     # design.md（本文档）/ manual.md + manual_EN.md / gpu_paradigm.md（路线图）
```

---

## 3. 整体架构分层

```
  模型源                        Graph IR（格式无关）                执行后端
┌──────────────┐   ┌────────────────────────────────────┐   ┌────────────────┐
│ ONNX (.onnx) │   │ Graph（组合根）                     │   │ InferenceRuntime│
│ onnx_loader  │──▶│  存储：defs/nodes/weights/shapes/  │──▶│  ops.myp(CPU)   │
│ pb.myp 解析   │   │        node_attrs/planner/analysis │   │  gpu_ops.myp    │
│ (mmap/外部    │   │  算法：GraphOptimizer(管线)         │   │   @gpu resident │
│  external/    │   │        GraphCompiler(lowering)     │   │  cuBLAS GEMM    │
│  FP16/BF16)   │   │  前端：ONNX/JSON → optimize(推理)/ │   │  run()/runGpu() │
├──────────────┤   │        optimizeTrain(训练自动补)    │   │  runAuto() 选择 │
│ JSON (.json)  │   │        inferShapes/fold/fuse/...   │   └────────────────┘
│ json_model    │──▶└────────────────────────────────────┘        ▲
│ (声明式层式)   │                                              Session (dl)
└──────────────┘   推理：load→optimize→run；训练：loadTrain/loadJsonTrain→optimizeTrain→runTrain
```

- **Graph 只认图 IR**（op 名 + 输入槽 + 属性字段码 `NodeField` + 形状），不关心图从哪来。
- **两个 lowering 消费者一致**：ONNX 解析器与 JSON loader 都调同一套图构建 API
  （setFile/addWeight/addShapeD/addGraphOutput/beginNode/endNode/nodeType/nodeIn/nodeOut/…）。
- **MYP 约束**：`function:` 段方法类私有（跨类不可调）→ 图构建 API 放 `action:`；
  跨类只能调方法、不能直读字段 → Graph/各存储类用公开方法暴露访问。

---

## 4. 图 IR 与模块化（阶段一重构，2026-09-01 冻结）

早期 `graph.myp` 单文件（~1070 行）经阶段一拆分（`graph.myp 拆分` 系列 + 接口冻结）为：

- **Graph = 组合根**：持有各存储/算法组件 + 领域级访问器（`valueAt/nodeOutOf/…`），
  不再直接实现 pass/lowering。
- **存储拆分（各自 SoA 表，Graph 不触内部数组）**：
  - `graph_defs.myp`：纯常量（Kind/NodeField/OpCode 枚举），无实例状态。
  - `graph_nodes.myp`：节点 opcode + 固定 in0..in4/out 槽；属性归 `GraphNodeAttrs` 独有。
  - `graph_weights.myp`：权重/初始器元数据（name/index/get/set）。
  - `graph_shapes.myp`：形状（names/dims/rank5/kind/NHWC/dead）。
  - `graph_node_attrs.myp`：节点标量属性槽（NodeField 码 → 值），stride 布局。
  - `graph_planner.myp` / `graph_analysis.myp`：算法可变状态。
- **算法归位**：`GraphOptimizer` 拥有全部 pass（阶段一「pass 迁入 GraphOptimizer」）；
  `GraphCompiler` 拥有完整 lowering（buildRuntime 迁出 + runtime 张量映射）。
- **拆分原因（MYP 工程）**：单类巨型方法 + 多数组字段使单文件过大、改一处级联大编译；
  SoA 存储与算法解耦后 pass 可独立演进。**注意**：MYP import 不传递导出符号——
  每个模块须显式 import 依赖（graph_defs 等被多模块引用）。

---

## 5. 前端一：ONNX（`onnx_loader.myp` + `pb.myp`）

- 手写 protobuf wire 解析 `ModelProto/GraphProto/NodeProto/AttrProto/TensorProto/ValueInfoProto`；
  权重支持 `float_data/raw_data/double_data`；dims 从 `repeated int64` 读。
- **字段号实测**：AttrProto `t`(TensorProto)=**5**（6 是 g=GraphProto）；BN epsilon `f`=2
  （wire type 5，`readU32()` 4B 小端）。
- **鲁棒性（阶段七）**：opset/version 检查；Unsupported op 诊断（原因分类 + 输出列表）；
  ONNX external data 外置权重读取；`loadMmap` 零拷贝 mmap 加载；错误码三阶段分离。
- **dtype 归一化（阶段三）**：BF16→FP32 = bits<<16；FP16 → FP32 用标准 half 解码；
  后续统一 FP32 arena。
- **训练图构建**：`loadTrain`/`loadTrainMmap` 在解析期额外把「可训权重」登记（供反向 Update）。

---

## 6. 前端二：声明式 JSON（`json_model.myp`，2026-09-02）

- **定位**：不经过 ONNX 的第二图源——用户写层式 JSON（`{op, in, ...}`）描述网络，
  loader 填同一 `Graph`（节点/形状/内存权重），复用 `optimize`（推理）/
  `optimizeTrain`（训练自动补 label+loss+反向）。
- **连线 = 张量名**：`out` 名 → 下游 `in`；fan-out 靠名字多次引用；fan-in 用
  `in2/in3/in4` 槽；张量形状/角色由 loader 按 op 类型推断登记。
- **权重源**：`init`（内联值）/ `values`（显式数组）/ `.safetensors`（内存权重通道
  offset=-2，值存 memVal_，writeWeight 直写 arena）。JSON 标量经 `regF32Scalar` 推断路径
  wire 到节点常量输入。
- **JSON 支持的算子族**：Gemm/MatMul/Conv/ConvTranspose/Pool/激活全族（Relu↔SiLU↔
  LeakyRelu↔ReLU6…换 op 名即换激活）/Softmax/LogSoftmax/GlobalAveragePool/Flatten/
  归一化（BN/IN/GroupNorm/LayerNorm/RmsNorm）+ 二元 Add/Sub/Div/Mul + 多输入 Concat +
  数据重排（Reshape/Transpose/Squeeze/Expand/Tile/Gather）+ 规约（Reduce 族）+ 索引
  （OneHot/ArgMax…）+ Dropout + 3D（Conv3D/Pool3D/ConvTranspose3D）。
- **固化示例**：`unet2d.json`（2D U-Net：编码 Conv+MaxPool / 解码 ConvTranspose+跳跃
  Concat，CPU+GPU 端到端）。

---

## 7. 数据模型与内存

### 7.1 运行时数据模型（`runtime.myp`）

- **张量表**：每张量 `(name, rows, cols, 4D/5D 元数据)`；CNN 张量以 `(N,C,H,W)` 登记
  （NHWC 布局为 `(N,H,W,C)`）；5D 张量另带 rank5 标记与 D 维。
- **arena**：单一 `float[]` 承载非持久张量；持久张量（权重/图输入/图输出）独立分配，
  **永不复用**（多轮不损坏权重）。
- **算子表**：`opKind/opA/opB/opC/opP0..`：每条算子 = 整数 kind + 输入/输出/参数张量 id
  + 标量参数槽（占满后溢出到 opX/opD 等扩展槽）。

### 7.2 内存规划（`GraphPlanner`/planMemory）

- **first-fit 区域复用**：按拓扑推进，张量 lastUse 后区域可被后续张量复用
  （ResNet50 ~15M floats≈60MB vs 不复用 ~400MB）。
- **持久张量标记**：权重/输入/输出不入复用池。
- **`MYP_NO_REUSE=1`**：禁用复用（区域复用导致读已释放张量得垃圾值是调正确性常见陷阱）。
- **动态 batch / shape specialization（阶段三）**：`setInputShape` 注入动态维 →
  编译期按 bucket specialization；形状值传播（int64 四则/ReduceSum/Expand 形状链折叠）。
- **dumpPlan/loadPlan**：优化后 IR + 计划缓存（checkpoint/续训/免重复优化）；
  数据源 `loadPlan` 跳过 ONNX 解析+优化直接建 runtime。

---

## 8. 图优化 pass 管线（`GraphOptimizer`）

```
foldConstants → inferShapes → classifyShapes → fuseConvBN → fuseConvRelu
→ fuseGapFlatten → eliminateDeadNodes → [常量去重/死权重裁剪/形状值传播/Conv1x1
lowering/算子选择] → layoutNHWC(opt-in) → topoSort → planMemory → buildRuntime
```

| pass | 作用 |
|------|------|
| `foldConstants` | Constant 节点（float）→ 持久张量（`wRole_=4`，kind `FC_C`） |
| `inferShapes` | 拓扑推断形状（遇不支持算子返回 0 → 加载失败） |
| `classifyShapes` | 按角色分类（权重/偏置/激活/输入/输出） |
| `fuseConvBN` | Conv→BatchNormalization 折叠进卷积权重/偏置（G3） |
| `fuseConvRelu` | Conv→Relu 融合单内核（G1） |
| `fuseGapFlatten` | GlobalAveragePool→Flatten 融合（G2，batch==1） |
| `eliminateDeadNodes` | DCE（fixpoint 删无活消费者且非图输出） |
| `常量去重` | 内容相同初始器合并（防误合并 BN 折叠 bias 已在 ResNet18 回归修复） |
| `死权重裁剪` | DCE 后移除未引用初始器 |
| `形状值传播` | int64 常量形状链折叠（ReduceSum/Expand 等） |
| `Conv1x1 lowering` | 1x1 卷积 GEMM 化（opKind 83） |
| `算子选择` | BatchMatMul batch=1 降级 2D matmul / 布局选择 |
| `layoutNHWC` | NCHW→NHWC，`MYP_LAYOUT_NHWC=1` opt-in（G2） |
| `topoSort` | Kahn 拓扑排序重排活节点到 `planOrder_` |
| `planMemory` | first-fit 区域复用 + 持久张量 |
| `buildRuntime` | 按 `planOrder_` 接线到 `InferenceRuntime` |

> **verifier（阶段八）**：`MYP_IR_VERIFY=1` 触发五重校验 verifyIR / verifyShapes /
> verifyDefUse / verifyTopo / verifyRuntimeWiring（含 pass 等价性测试）。

### 8.1 图融合细节（正确性关键，历次 bug 教训）

- **Conv+BN 折叠（G3）**：`invStd[c]=scale[c]/sqrt(var[c]+eps)`，
  `W'[c]=W[c]*invStd[c]`，`B'[c]=(B_conv[c]-mean[c])*invStd[c]+B_bn[c]`；在 `writeWeight`
  应用（与 NHWC 权重转置可交换）；BN 节点+4 参数标死；conv 输出改写为 BN 输出。
  无 bias conv 折 BN 时生成 `<conv>#bnb` 合成权重（`wBNOnly_=1`）。
- **`nFused_` vs `nRelu_`（G4 修 bug）**：各融合类型用**独立语义标志**——buildRuntime
  用 `nRelu_`（仅 fuseConvRelu 置位）决定 addConvRelu vs addConv，避免 BN-only+残差 Add
  误加 ReLU。
- **DCE 活性**：用 `effectiveOut`（融合节点 `nFusedOut_`）判活，否则被消节点误删。
- **planMemory 融合感知**：被消节点的输出 producer 覆写为 fused 节点，否则 lastUse 窗口
  允许 fused 输出复用其输入区域 → 写目标与输入别名 → 静默错。
- **训练跳过结构融合**：训练图自动跳过 Conv+Relu 等结构融合，反向逐节点回传
  （修复此前融合导致 Conv 权重不更新的隐蔽断链）。

---

## 9. 运行时与算子（`runtime.myp` + `ops_iface_all.myp`）

### 9.1 opKind 与 OpCode 两套号

- **opKind**：runtime 层内核编号（算子/反算子的执行码），随算子族增长（现 ~82+）。
- **OpCode**（`graph_defs.myp`）：图 IR 层算子类型码（含反向 BWD_*、loss 等，120+）。
- ONNX op_type / JSON op 名 → loader 映射为 OpCode/kind → buildRuntime 登记 opKind 算子。

### 9.2 接口化算子注册（阶段4e 起，run/runGpu 无 if/else）

- `interface IOp { void forward(rt, opIdx); void backward(rt, opIdx); }`
- 每算子两个类（CPU `XxxOp` 与 GPU `GpuXxxOp`）各自实现 IOp；注册
  `registerFwdBwd(fwdKind, bwdKind, op, isGpu)` 把同一实例放前向/反向两槽（CPU 与 GPU
  各一套表）；`backward()` 只在 `trainMode()==1` 时执行。
- 首调自动 `registerAllIfaceOps`；全量注册在 `ops_iface_all.myp`（权威清单）。

### 9.3 算子族一览（CPU `ops.myp` + GPU `gpu_ops.myp`，与 ORT 位精确对拍）

| 族 | 代表算子 |
|----|----------|
| 卷积/池化/填充 | Conv（NCHW/NHWC/1x1-GEMM）、Conv3D、ConvTranspose3D、MaxPool/AvgPool/GlobalAveragePool/GAP3D、Pad（constant/edge/reflect + 非 0 value） |
| 全连接/矩阵 | Gemm/Dense、MatMul、BatchMatMul（4D + batch 广播，GPU cuBLAS） |
| 归一化 | BatchNorm（NCHW/NHWC）、InstanceNorm、GroupNorm、LayerNorm、RMSNorm |
| 激活 | Relu/Sigmoid/ReLU6/LeakyRelu/SiLU/HardSwish/Clip/Tanh/GELU |
| 概率/损失 | Softmax/LogSoftmax/CE/Dice/MSE/BCE（loss 训练节点） |
| 张量变换 | Transpose/Reshape/Flatten/Squeeze/Concat/Split/Slice/Expand/Tile/Where/Embedding |
| 数据索引 | Gather/GatherElements/ScatterND/ArgMax/ArgMin/TopK/OneHot/Range |
| 规约/逐元 | ReduceSum/Mean/Max/Min（mode 泛化）、Add/Sub/Mul/Div/Sqrt、Resize（NCHW/NHWC） |
| LLM | RmsNorm/LayerNorm/GELU/RoPE 位置编码 |
| 3D | Conv3D/Pool3D/Pad3D/Resize3D/ConvTranspose3D（5D tensorD 感知） |
| Dropout | 推理恒等 / 训练随机 mask |

---

## 10. 执行后端

### 10.1 CPU（`ops.myp`）

按算子表顺序调 `InferOps` 内核（batch-aware，FP32）；训练/规约型内核可用 `@parallel for`
（小 n 微并行正确性已由 runtime_myp 线程池丢唤醒修复保障，见主仓 BUGLIST BUG-135）。

### 10.2 GPU（`gpu_ops.myp` + cuBLAS）

- 内核以 `@gpu for (long p...) resident(a=dev)` 设备驻留模式编写：数组参数直接用设备
  指针，跳过逐次 H2D/D2H。
- **arena 持久化增量同步（阶段六）**：图/权重一次 H2D 建立设备 arena；`setFlat` 置脏 →
  runGpu 增量上传脏区 → 训练步 `gpuMarkSyncAll` 全量 D2H（正确性优先）。
- **cuBLAS GEMM（阶段六）**：dense/matmul/Conv1x1 lowering 走厂商库（GPU 加速主要来源）。
- **内核约束（MYP）**：体内不能声明 `double` 局部、不能调宿主函数——标量参数
  （eps/alpha/min/max）在分发时先解析成 `double` 再传参；核内数学用 `float` +
  `__nv_*` 内建。
- **回退**：无 GPU / `MYP_GPU` 未设 / 图含无 GPU 分派算子 → 自动回退 CPU（结果一致）。
  `Session.runAuto()`/`runTrainAuto()` 按此自动选择。

### 10.3 一致性门禁

每新算子 CPU/GPU 数值对拍（探针 GPU==CPU 逐位一致）；端到端 vs ORT 逐元素；
`run_all.sh` 全量 pass=135。

---

## 11. 训练（Session / 静态反向图）

### 11.1 反向图自动构建

- `optimizeTrain`（JSON）/ `loadTrain`（ONNX）自动补 **label + loss + 反向**：
  从输出张量回溯建静态反向图（Bwd 算子族 + Update 节点），每前向 opKind 对应
  Bwd opKind（`registerFwdBwd`）。
- **反向覆盖**：激活（Relu/Sigmoid/ReLU6/Leaky/SiLU/HardSwish/Clip/LogSoftmax/Tanh/GELU）、
  池化（MaxPool/AvgPool/GAP/GAP3D）、归一化（BN/IN scale·bias 梯度、GroupNorm）、
  数据重排/广播（Reshape/Flatten/Squeeze/Transpose/Expand/Tile/Gather scatter-add）、
  规约（ReduceSum/Mean broadcast 回 x、ReduceMax/Min argmax）、
  索引（GatherElements/ScatterND）、4D/batch MatMul、Pad、Concat、Conv（含 1x1）等。

### 11.2 Session 统一（阶段九 SLI + P10a/P10b）

- 入口：`load/loadTrain/loadMmap/loadTrainMmap/loadJson/loadJsonTrain` → 推理或训练图。
- 推理：`runAuto()`（MYP_GPU=1 → GPU，否则 CPU）；`getOutput/setInput/loadInputFromFile`。
- 训练：`setLr/setOptimizer(0|1|2)/setWeightDecay/setGradAccumEvery/setAmpSim/`
  `setTrainMode(1) → runTrain()/runTrainAuto() → loss()`；`gradId(权重名)` 读梯度。
- 优化器：SGD / 动量 / **AdamW + weight decay**；梯度累积（micro-batch）；
  AMP 数值管线骨架（fp16 梯度舍入模拟）。
- loss 族：CE/Dice（分类）与 MSE/BCE（逐元素，`setLossMode`）。
- checkpoint：`dumpPlan/loadPlan`（优化后 IR+计划，续训跳过解析优化，确定性）。
- **GPU 训练（P10a）**：`runTrainAuto` 在 MYP_GPU=1 且图内每 op 有 GPU fwd/bwd 槽时走
  持久化 GPU 训练步（每步全量 D2H 正确性优先），否则自动 CPU 回退；
  **P10b** 补 fan-in 汇合（Sub/Mul/Div/Add）与 BN/IN/BN-NHWC/batch-MatMul/Reduce/
  Transpose/Expand/Tile/Gather/ReduceMM/Pad 的 GPU bwd → BN/IN/batch-MatMul 训练网
  现走 runTrainAuto GPU。
- 端到端固化：CNN 训练、2D U-Net（json）、SwiGLU（fan-out 双 Gemm+SiLU+Mul，
  200 步 loss 1.09→0.004）、3D U-Net、ResNet 推理等。

---

## 12. 扩展：新算子/新模型流程

新算子（fwd，必要时 bwd）：
1. `ops.myp` 加 CPU 内核（batch-aware，FP32）；GPU 版加 `gpu_ops.myp`（`@gpu for`+resident，数值一致）。
2. 建算子类实现 `interface IOp{forward,backward}`（CPU `XxxOp` + GPU `GpuXxxOp`）；
   在 `ops_iface_all.myp` 注册（fwd/bwd 槽；loss/特殊用 backward-only 注册）。
3. `graph_defs.myp` 加 OpCode（如需图层识别）；`onnx_loader.myp` 映射 ONNX op_type →
   形状推断 `inferShapes` → `buildRuntime` 接线（可融合则加 pass）；JSON 侧在
   `json_model.myp`/`graph_compiler` 补 op 名分派。
4. GPU bwd：确认 `GpuXxxOp.backward()` 已实现 + 注册到 GPU bwd 槽（惯例：GPU 算子类的
   backward() 常是空 stub，registerFwdBwd 只放槽不实现体——补 GPU bwd 先查该类实现）。
5. 验证：合成 `.onnx`/`.json` + ORT 参考逐元素对比（`*_main.myp`）；算子级
   `tests/@test/*_opt.myp`；CPU/GPU 双路径；训练用例加反向数值对拍（bwd_*_main.myp /
   有限差分）。

新模型：零样板用 `infer_tests/run_onnx.myp`（通用 ONNX 运行器，自动探测输入输出）；或
`import dl` + `runAuto`。

---

## 13. 已知约束 / MYP 踩坑备忘

- `var`、`ref`、`data`、`fact` 是 MYP 保留字，不能作局部变量名。
- `float - double` 混合算术不自动提升；LLVM verify 不接受隐式 float→double 赋值
  （先声明 double 初值变量）。
- MYP 无 float↔bits 转换 → eps/alpha 用位型 int 存 + `F32.toDouble` 解析。
- @parallel/@gpu 并行体**只捕获外层局部**：class/static 属性数组须先拷到局部；体不得
  分配 arena/调宿主函数；循环变量 int/long 均可。
- @gpu 内核标量须从设备 arena 核内读（持久化下宿主副本陈旧）——bwdClip min/max 类即此。
- 接口转换/胖指针：类转 interface 各表达式形态都要具体类名（历史 BUG-029/033/064 教训）；
  JSON/图路径的新数据输入算子须登记角色，避免被当 2D 转置布局。
- 数值诊断：大数值 MSE 目标下 BN scale 梯度大（dscale≈Σdy·x）→ 用 lr 0.005 防 NaN。
- `main()` 内禁语句级具名调用 → 输出/训练逻辑放类 @constructor（见各 *_main.myp 惯例）。

---

## 附录：相关文档与验证

- `../README.md` / `README_EN.md`：现状速览（中/英）。
- `docs/manual.md` / `docs/manual_EN.md`：用户手册（已并入原 `sli.md`/`usage.md` 全部内容）。
- `docs/gpu_paradigm.md`：GPU 范式库 + 推理/训练路线图。
- `CHANGELOG.md`：本分项目里程碑时间线（阶段一~九 + JSON P2..P10b）。
- 回归：`bash examples/deeplearning/infer_tests/run_all.sh` → `pass=135 fail=0`。
