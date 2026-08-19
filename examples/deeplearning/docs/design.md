# MYP 深度学习推理框架 — 设计说明

> 适用范围：`deeplearning/infer/`（纯 MYP 实现的通用静态图推理框架）
> 里程碑路线图见同目录 `gpu_paradigm.md` §14（M1-M4 / G1-G5 / F1-F6）。
> 文档版本：2026-08-12（G5 重构之后）。

---

## 1. 设计目标

- **纯 MYP 实现**：不依赖 Python / onnxruntime 做运行时推理；C 层仅保留 GPU FFI（`runtime_gpu.c`）。
- **通用静态图**：一张图 = 张量表 + 算子表 + 权重表，与具体模型解耦。
- **FP32 精度**：与 onnxruntime 一致；arena 以 `float[]` 承载全部激活/权重。
- **CPU + GPU 双后端**：同一张 runtime 图，`run()` 走 CPU，`runGpu()` 走 CUDA（无 GPU 自动回退）。
- **图优化**：load 时跑一条图 pass 管线（常量折叠 / 融合 / DCE / 布局变换 / 拓扑排序 / 内存规划），
  减少 kernel 启动数与中间张量，正确性经 onnxruntime 逐元素校验。

---

## 2. 目录布局

```
deeplearning/
├── infer/
│   ├── pb.myp             # protobuf wire-format 读取器（读 .onnx）
│   ├── graph.myp          # ★ G5：通用图优化器组件（格式无关）：图 IR + 8 个 pass +
│   │                      #   planMemory + buildRuntime + 图构建 API（~1070 行）
│   ├── onnx_loader.myp    # ONNX 解析器（薄）：protobuf 解析 → 填充 Graph（~340 行）
│   ├── runtime.myp        # InferenceRuntime：张量/算子注册表 + run()/runGpu() 分发
│   ├── ops.myp            # CPU 算子内核（batch-aware，FP32）
│   ├── gpu_ops.myp        # GPU 算子内核（@gpu for resident，FP32）
│   ├── tensor.myp         # 张量索引辅助
│   └── tools/             # Python 辅助（onnxruntime 交叉校验 / fixture 生成）
├── infer_tests/           # ONNX 端到端验证入口（r18/resnet/bn/act/const/onnx *_main.myp）
├── json_tool/             # JSON 图小工具（独立）：model_loader + XOR/MNIST 演示 + CLI
├── data/                  # 模型 / 输入 / 数据集（git 忽略）
└── docs/                  # 本文档 + usage.md + gpu_paradigm.md（路线图）
```

---

## 3. 运行时架构（`runtime.myp`）

### 3.1 数据模型

- **张量表** `t_/tMeta_/tOff_`：每个张量有 `(name, rows, cols, 元数据)`；CNN 张量以 4D 元数据
  `(N, C, H, W)` 登记（NHWC 布局为 `(N, H, W, C)`）。
- **arena**：单一 `float[]`，承载所有非持久张量；持久张量（权重 / 图输入 / 图输出）单独分配，**永不复用**。
- **算子表** `opKind_/opA_/opB_/opP0_/opP1_`：每条算子一个整数 kind + 输入/输出/参数张量 id。

### 3.2 opKind 一览

| kind | 算子 | 说明 | 里程碑 |
|-----:|------|------|--------|
| 1 | Dense | Y = W·X + b | 早期 |
| 2 | Relu | max(x, 0) | 早期 |
| 3 | Softmax | per-sample 归一化 | 早期 |
| 4 | Sigmoid | 1/(1+e^-x) | 早期 |
| 5 | Add | 逐元素加（残差） | 早期 |
| 6 | MatMul | 矩阵乘 | 早期 |
| 7 | Conv | NCHW 卷积（strides/pads/dilations/group） | G1 前 |
| 8 | MaxPool | NCHW 最大池化 | G1 前 |
| 9 | GAPool | 全局平均池化 | G1 前 |
| 10 | Flatten | 展平 | G1 前 |
| 11 | ConvRelu | **融合** Conv+ReLU 单内核 | G1 |
| 12 | NCHW2NHWC | 布局转置节点 | G2 |
| 13-16 | Conv/MaxPool/GAPool 的 NHWC 变体 | | G2 |
| 17/18 | BatchNorm（NCHW / NHWC） | 独立 BN 算子 | G3 |
| 19 | ReLU6 | min(max(x,0),6) | G4 |
| 20 | LeakyRelu | alpha 经 opP0 存位型，分发时解析 | G4 |
| 21 | SiLU | x·sigmoid(x) | G4 |
| 22 | HardSwish | x·ReLU6(x+3)/6 | G4 |
| 23 | Clip | min/max 标量（minTid/maxTid=-1 表示不限） | G4 |

> 注意：OpKind 11 之后是融合/变体算子，**不是** ONNX 原始算子；ONNX 算子经 loader 映射到这套 kind。

### 3.3 执行

- `run()`（CPU）：按算子表顺序调用 `InferOps`（`ops.myp`）内核。
- `runGpu()`（GPU）：**整块 arena 一次 H2D**（`GpuBufferF`）→ 各算子以设备指针调用
  `GpuInferOps`（`gpu_ops.myp`，`@gpu for ... resident(a=dev)` 设备驻留）→ 一次 D2H。
  无 GPU / `MYP_GPU` 未设时自动回退 CPU（结果一致）。

### 3.4 内存规划（`planMemory`）

- **first-fit 区域复用**：按算子拓扑序推进，张量 lastUse 之后其区域可被后续张量复用，
  大幅降低峰值内存（ResNet50：~15M floats ≈ 60MB vs 不复用 ~400MB）。
- **持久张量标记**：权重 / 图输入 / 图输出永不进入复用池（多轮推理不损坏权重）。
- **调试开关 `MYP_NO_REUSE=1`**：禁用区域复用，保证推理后所有中间张量仍可读
  （区域复用导致读取已释放张量得到垃圾值，是调正确性时的常见陷阱）。

---

## 4. 图优化器（`graph.myp`）与 ONNX 解析器（`onnx_loader.myp`）

> **G5 重构（2026-08-12）**：把图优化器从 ONNX loader 分离为**通用组件** `Graph`
> （`graph.myp`）。架构分层：
>
> ```
> onnx_loader.myp（薄解析器，ONNX 专属）           graph.myp（Graph，格式无关）
> ──────────────────────────────                  ─────────────────────────────
> readFile / pb_ 字节读取                         图 IR（权重/节点/形状/规划/张量表）
> parseModel/Graph/Node/Attr/Tensor/ValueInfo     foldConstants / inferShapes /
>   → 经图构建 API 填充 Graph                       classifyShapes / fuseConvBN /
>   （setFile/addWeight/addShapeD/addGraphOutput/  fuseConvRelu / fuseGapFlatten /
>    beginNode/endNode/nodeType/nodeIn/nodeOut/    eliminateDeadNodes / layoutNHWC /
>    nodeInt→NodeField）                            topoSort / planMemory / buildRuntime
>                                                 optimize(rt) = 管线编排
> ```
>
> 关键点：
> - **Graph 只认图 IR**（op_type 字符串 + 输入 + 属性字段码 `NodeField` + 形状），
>   不关心图从哪来；ONNX 解析器把 ONNX 属性名映射为 `NodeField` 字段码。
> - **MYP 约束**：`function:` 区方法是类私有（跨类不可调）→ 图构建 API 放 `action:`（公共）；
>   跨类只能调方法、不能直接读字段（`a.x_[0]` 编译报错），所以 Graph 用公开方法暴露构建接口。
> - `OnnxLoader` 公共接口（`load/tensorId/各统计访问器`）保持不变 → infer_tests 零改动。
> - `foldConstants`/`writeWeight` 仍从原始文件字节（`file_`）读权重 → Graph 持有 `file_` + 自己的 `pb_`。

### 4.1 解析（onnx_loader.myp）

- 用 `pb.myp` 手写 protobuf wire 解析 `ModelProto/GraphProto/NodeProto/TensorProto/ValueInfoProto`。
- 权重支持 `float_data / raw_data / double_data`；dims 从 `repeated int64 dims` 读取。
- 关键字段号（实测验证）：AttributeProto `t`(TensorProto) = **field 5**（非 6，6 是 g=GraphProto）；
  BatchNormalization epsilon = AttributeProto `f` = field 2（wire type 5，`readU32()` 4 字节小端）。

### 4.2 Pass 管线（`Graph.optimize()` 内顺序）

```
foldConstants → inferShapes → classifyShapes → fuseConvBN → fuseConvRelu
→ fuseGapFlatten → eliminateDeadNodes → layoutNHWC(opt-in) → topoSort
→ planMemory → buildRuntime
```

| pass | 作用 |
|------|------|
| `foldConstants` | Constant 节点（float 张量）→ 持久张量（`wRole_=4`，kind `FC_C`），节点标死 |
| `inferShapes` | 拓扑推断各张量形状（遇不支持算子返回 0 → 加载失败） |
| `classifyShapes` | 按角色分类张量（权重/偏置/激活/输入/输出） |
| `fuseConvBN` | Conv→BatchNormalization 折叠进卷积权重/偏置（G3） |
| `fuseConvRelu` | Conv→Relu 融合为单内核（G1，标记 `nRelu_`） |
| `fuseGapFlatten` | GlobalAveragePool→Flatten 融合（G2） |
| `eliminateDeadNodes` | DCE：fixpoint 删无活消费者且非图输出的节点（G2） |
| `layoutNHWC` | NCHW→NHWC 布局变换，`MYP_LAYOUT_NHWC=1` opt-in（G2） |
| `topoSort` | Kahn 拓扑排序重排活节点到 `planOrder_`（转置节点后追加必须重排） |
| `planMemory` | first-fit 区域复用 + 持久张量 |
| `buildRuntime` | 按 `planOrder_` 接线到 `InferenceRuntime` |

### 4.3 图融合细节（正确性关键）

- **Conv+BN 折叠（G3）**：`invStd[c]=scale[c]/sqrt(var[c]+eps)`，
  `W'[c]=W[c]*invStd[c]`，`B'[c]=(B_conv[c]-mean[c])*invStd[c]+B_bn[c]`。
  在 `writeWeight` 应用（与 NHWC 权重转置可交换）；BN 节点 + 4 参数张量标死；
  conv 有效输出改写为 BN 输出。随后 G1 继续融合 Conv+ReLU → **Conv→BN→Relu 坍缩为单算子**。
- **无 bias 卷积的 bias 合成（G4）**：真实模型（如 Caffe2 导出）的 conv 常无 bias 输入，
  此时折 BN 没有 conv-bias 张量可折 → `fuseConvBN` 生成 `<conv>#bnb` 合成权重
  （`wBNOnly_=1`），`B'[oc]=(0-mean[oc])*invStd[oc]+B_bn[oc]`。
- **`nFused_` 与 `nRelu_` 的区分（G4 修 bug）**：`nFused_` 同时被 fuseConvBN 与 fuseConvRelu
  置位，若 buildRuntime 只查 `nFused_` 会把"BN-only 融合、后接残差 Add"的 conv 误加 ReLU。
  **修复**：新增 `nRelu_[512]` 标志**仅**由 fuseConvRelu 置位，buildRuntime 用 `nRelu_`
  决定 `addConvRelu` vs `addConv`。经验：每个融合类型应使用**独立语义标志**。
- **GAP+Flatten（G2）**：batch==1 才融合（保证 GAP 顺序写 n*C+c 与 FC_ACT 转置登记一致）。
- **DCE 活性判定（G2）**：用 `effectiveOut`（融合节点的 `nFusedOut_`）判活性，
  否则被融合消掉的节点会被误判为死而误删。
- **planMemory 融合感知（G1）**：被消节点的输出 producer 覆写为 fused 节点，
  否则 lastUse 窗口允许 fused 输出复用其输入区域 → 写目标与输入别名 → 静默错误。

### 4.4 Clip / ReLU6 的 ONNX 映射（G4）

- **ReLU6 不是标准 ONNX 算子**，真实模型用 `Clip(x, 0, 6)`；opset≥13 时 min/max 是
  **输入张量**（opset 11 前是属性）。loader 从初始器把 min/max 解析为标量张量 id
  （`-1` = 不限），buildRuntime 映射到 `addClip`（opKind 23）。
- SiLU / HardSwish 需 opset≥14；本环境 onnx 1.22 / onnxruntime 1.28 **未注册 SiLU schema**
  → SiLU 的 ORT 交叉校验不可用，改由 `tests/@test/act_opt.myp` 手工参考验证。

---

## 5. GPU 卸载（`gpu_ops.myp`）

- 内核全部以 **`@gpu for (long p...) resident(a=dev)`** 设备驻留模式编写：
  数组参数直接用设备指针，跳过 H2D/D2H/释放。
- **MYP 约束**：内核体内不能声明 `double` 局部、不能调用宿主函数（如 `F32.toDouble`）
  ——标量参数（eps/alpha/min/max）在 runGpu 分发时先解析成 `double` 再传参；
  内核内数学用 `float` + `__nv_sqrt` 等内建。
- 每算子一个内核；runGpu 统一"整块 arena 一次 H2D → 逐算子 resident → 一次 D2H"。
- 性能参考（RTX 2070 SUPER）：ResNet50 66ms（内核 O2，M3.5）；ResNet18 51ms。

---

## 6. 扩展一个新算子（步骤）

1. `runtime.myp`：`addXxx()` 分配新 opKind（或复用现有 kind 变体）。
2. `ops.myp`：CPU 内核 `InferOps.xxx`（batch-aware，FP32）。
3. `gpu_ops.myp`：GPU 内核 `GpuInferOps.xxx`（`@gpu for` + `resident`），与 CPU 数值一致。
4. `run()` / `runGpu()`：分发分支（含 opKind 比较）。
5. `onnx_loader.myp`：`parseNode` 识别 ONNX op_type → 形状推断 `inferShapes` →
   `buildRuntime` 接线（若可融合则加 pass）。
6. 验证：`data/onnx` 放一个合成 `xxx_test.onnx`（opset 用 ORT 支持的版本），
   用 `tools/cross_check_onnx.py` 出参考，`*_main.myp` 逐元素对比；再补
   `tests/@test/xxx_opt.myp` 算子级断言。

---

## 7. 已知约束 / MYP 踩坑备忘

- `var`、`ref`、`data` 是 MYP 保留字，不能用作局部变量名。
- `float - double` 混合算术不会自动提升（需显式 `double xv = arena[...]`）。
- LLVM verify 不接受 `lo = arena[...]` 的隐式 float→double 赋值（先声明 double 初值变量）。
- 字符串 + float 拼接有既有显示 bug（数值对、排版乱）→ 调试时用 `double d=f; Console.writeFloat(d)`。
- MYP 无 float↔bits 转换（eps/alpha 用位型 int 存 + `F32.toDouble` 解析）。
