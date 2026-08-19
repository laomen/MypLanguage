# 3D 扩展可行性评估（Conv3D / 5D 张量 / 动态形状）

> 日期：2026-08-12
> 目标模型：`coarse_model.onnx` + `fine_model_liver_vessel.onnx`（同一 3D 医学影像
> 管线的 coarse/fine 两阶段，3D U-Net，肝脏血管相关）
> 结论先行：**可行，但属架构级扩展**（非小批次算子补齐）。按「固定 5D NCDHW +
> 输入形状注入折叠」路线，估 **8-12 人天**，分 4 阶段落地。

---

## 1. 目标模型需求（两个模型共核）

| 项 | coarse | fine |
|---|---|---|
| 输入 | `[1,1,D,H,W]` 5D 动态 | 同 |
| 输出 | `[1,6,D',H',W']` 5D | 同（更细） |
| Conv | 38（Conv3D k=3³） | 56 |
| InstanceNormalization | 37 | 54 |
| Resize | 5（三线性 align_corners） | 11 |
| MaxPool/AveragePool | MaxPool 4 | AvgPool 5 |
| Pad | 0 | 5 |
| 动态形状算子 | Shape/Slice/Concat/ReduceProd/Cast | +Unsqueeze/Gather（18+14+7+7+6） |
| 全局归一化子图 | ReduceMean/Sub/Mul/Div/Sqrt/Clip | 同 |

**本质**：5D 张量（NCDHW）+ Conv3D + 逐通道 InstanceNorm + 3D 上下采样 +
运行时计算的 Resize 尺寸（依赖输入形状，非数据）。

---

## 2. 当前框架基线（2D/4D 静态）

- **runtime 张量**：`tRows_/tCols_`（扁平 2D 视图）+ `tN_/tC_/tH_/tW_`（4D NCHW
  视图），`int[512]`；`addTensor4/addTensorPlanned4`（4D）/`addTensorPlanned2`（FC）。
- **graph 形状表**：`shD0_..shD3_`（4D）+ `addShapeD4`；`addShapeD(name,dims,dimCount)`
  已支持任意 dimCount（当前截断到 4）。
- **算子**：Dense/Relu/Sigmoid/Softmax/Add/MatMul/Conv(2D)/MaxPool(2D)/GAPool/
  Flatten/ConvRelu/BN/激活族/Clip + F8 张量算子（Concat/Reshape/Transpose/Slice）。
  Conv 内核签名为 `(xN,xC,xH,xW, ... kh,kw, sh,sw, pt,pb,pl,pr, dh,dw, group)`——**2D 专用**。
- **静态形状**：图构建时所有形状已知；planMemory 静态分配；无运行时 int64 张量。
- **布局**：NCHW（默认）+ opt-in NHWC；NHWC 内存即行优先（图维度=内存维度）。

---

## 3. 三大工作块

### A. 5D 张量布局扩展（约 1.5-2 天）

- runtime 增加第 5 维：`tD_`（或 `tD0_..tD4_`），保留 `tRows_/tCols_` 作为扁平缓冲视图
  （NCDHW → rows=N*C, cols=D*H*W）。
- graph 形状表扩到 5 维：`shD0_..shD4_`；`addShapeD` 允许 dimCount=5。
- `onnx_loader`：`parseTensor/parseValueInfo` 已按 dimCount 读 dims，只需放开 5 维上限。
- **影响面**：inferShapes 各分支、planMemory、buildRuntime 登记、F8 张量算子
  （Concat/Slice/Transpose/Reshape 的坐标解码从 4D 推广到 5D——多为加一层 div/mod）。
- 权衡：**固定 5D NCDHW**（改动小，够跑 3D U-Net）vs **通用 N-D**（`int[] dims` 动态秩，
  改动大，收益低）。**选固定 5D**。

### B. 3D 算子内核（约 4.5-5.5 天）

| 算子 | 量 | 说明 | 估时 |
|---|---|---|---|
| `Conv3D` | 38/56 | 仿 Conv 2D 加一层 kd 循环（CPU+GPU）；权重 `[Cout,Cin,D,H,W]`，pads 6、strides 3、dilations 3 | 2 天 |
| `InstanceNormalization` | 37/54 | 逐通道在 D×H×W 上算 mean/var（biased）+ affine（scale/bias 常量）+ eps；**2D 版可顺带实现（对 YOLO/2D U-Net 通用）** | 1 天 |
| `MaxPool3D` / `AvgPool3D` | 4/5 | 3D 池化（含 pad/stride） | 1 天 |
| `Pad`（常量/边缘） | 5 | 3D padding，模式常量即可（`mode='constant'` 大概率） | 0.5 天 |
| `Resize3D`（三线性 align_corners） | 5/11 | 坐标映射 `align_corners` + trilinear 插值；**scales/sizes 折叠后为常量**（见 C） | 1-1.5 天 |
| `ReduceMean`/`Sub`/`Div`/`Mul`/`Sqrt` | ~12 | 全规约 + 逐元素广播（易） | 0.5 天 |

> GPU 注意：`@gpu for` 内核 5D 化后索引链更长；此前「内核内 while 导致 GPU codegen
> 失败」——3D 内核需沿用**展开/朴素嵌套**风格，避免复杂控制流。

### C. 动态形状 → 输入形状注入折叠（约 2-3 天，最关键）

**核查结论**：两个模型的 Resize 尺寸/全局归一化子图均**只依赖输入形状 + 常量，
无数据依赖**（`Shape(input)→Slice→Concat→Resize`；`Shape→ReduceProd→Cast`）。
因此无需完整运行时动态形状——**在加载时注入具体输入形状，即可把这些子图常量折叠**，
与当前静态框架完全一致。

工作项：
- loader 支持注入输入形状（run_onnx 读 .f32 知元素数；输入 ValueInfo 静态维
  batch=1、C=1 已知，需从外部给 H/W/D——由 run_onnx 的输入参数或约定传入）。
- **Shape 常量折叠**：输入形状已知 → `Shape` 节点输出折叠为 int64 常量。
- **int64 常量折叠扩展**：`Slice`/`Concat`/`Unsqueeze`/`Gather`/`Cast`/`ReduceProd`
  作用于 int64 常量时折叠（现 `foldConstants` 主要处理 f32 常量，需扩 int64 路径）。
- 折叠后 Resize sizes/scales 变常量 → Resize 内核静态化。

> 风险：若某模型 Resize 尺寸数据依赖中间激活（非形状），折叠路线失效——需运行时
> 形状传播（回退方案，成本 ×3）。**本目标模型已核查无此情况**。

---

## 3.5 图优化器影响评估（无需重做，只需扩展）

G5 解耦后的 `graph.myp`（`Graph`：格式无关 IR + pass）按 `nType_` 字符串分派，
**绝大多数 pass 与维度无关**，3D 扩展是「针对性扩展」而非重写：

| pass | 对 3D 的影响 | 动作 |
|---|---|---|
| `inferShapes` | Conv/MaxPool/Resize 输出需多算 D 维（`shD4_`） | **扩展**：Conv3D/Pool3D/Resize3D 各加 ~3-5 行 D 计算 |
| `classifyShapes` | `Conv`/`MaxPool` 已按 type → `CNN_ACT`（type 判定即可覆盖 5D）；仅 `consumerKind` 的 4D 检查（`d2>1||d3>1`）需顺带认 5D | 微调（几行） |
| `fuseConvBN` | 目标模型用 `InstanceNormalization`（mean/var 数据依赖），**非 BatchNormalization** → 该 pass 不会触发，逻辑无需改 | 无（语义上正确跳过 IN） |
| `fuseConvRelu` | 3D U-Net 模式是 `Conv→IN→Relu`（IN 隔开）→ 不触发 | 无；后续可加 `IN+Relu` 融合作优化 |
| `fuseGapFlatten` | 3D 模型用 MaxPool/AvgPool，不触发 | 无 |
| `eliminateDeadNodes` / `topoSort` | 纯图结构，维度无关 | 无 |
| `layoutNHWC` | 3D 走 NCHW（不启用 NHWC） | 无（3D 不开 NHWC） |
| `planMemory` | `size = d0*d1*d2*d3`（1 行） | **扩展**：乘 `*d4` |
| `buildRuntime` | 张量登记 `addTensorPlanned4` + 新算子分发 | **扩展**：`addTensorPlanned5` + Conv3D/IN/Resize3D 分发 |
| `foldConstants` | 现主处理 f32 常量 | **新增**：P3 扩 int64 常量折叠（Shape/Slice/Concat/ReduceProd/Cast on int64） |

**关键点**：
- 融合 pass（Conv+BN / Conv+Relu / GAP+Flatten）对 3D U-Net **基本不触发**（IN 隔在
  Conv 与 Relu 之间），所以这些 pass 无需为 3D 改动——它们的价值主要在 2D CNN。
- 真正的新机制是 **Shape/int64 常量折叠**（P3）：输入形状注入后把 `Shape→Slice→
  Concat→Resize`、`Shape→ReduceProd→Cast` 折叠为常量。这属于 `foldConstants` 的
  **扩展**（加 int64 常量路径 + Shape 折叠），不是重做。
- 形状表 `shD0_..shD4_` 与 `addShapeD(name,dims,dimCount)` 已按 dimCount 读 dims，
  放开 5 维即可；`addShapeD4` 平移为 `addShapeD5`。

**结论**：图优化器整体框架复用，3D 扩展的图侧工作量集中在 inferShapes（D 维计算）
与 foldConstants（int64 折叠），约占总估时 1.5-2.5 天，无重构风险。

---

## 4. 两条路线对比

| | 路线 1：通用 N-D 张量 | 路线 2：固定 5D NCDHW（**推荐**） |
|---|---|---|
| 张量 | `int[] dims` 动态秩 + 通用解码 | 加第 5 维，4D 逻辑平移 |
| 内核 | 循环全参数化（每算子重写） | 2D 内核加一层循环 |
| 动态形状 | 完整运行时 shape 传播 | 输入注入 + 常量折叠 |
| 改动量 | 大（≈2-3×路线 2） | 中 |
| 适用 | 任意秩 + 任意动态模型 | 3D U-Net（目标模型） |
| 估值 | 20-30 人天 | **8-12 人天** |

---

## 5. 分阶段计划（路线 2）

- **P1（2-3 天）**：5D 张量布局 + `Conv3D` + `InstanceNormalization`（2D/3D）
  ——跑通 U-Net **编码器**（Conv+IN+Relu+MaxPool3D）。合成 3D ONNX vs ORT 验证。
- **P2（2-3 天）**：`Pad` + `AvgPool3D` + `Resize3D` + `Concat3D` 解码
  ——跑通 U-Net **解码器**（含 skip concat）。
- **P3（2-3 天）**：输入形状注入 + Shape/int64 常量折叠 + 全局归一化子图
  （ReduceMean/Sub/Div/Mul/Sqrt/Cast/ReduceProd）——跑通端到端。
- **P4（1-2 天）**：GPU 内核 5D 化 + 真实模型验证（coarse/fine，topk/逐元素 vs ORT）
  + 回归 237/237 + 文档/记忆 + 提交。

---

## 6. 风险与权衡

1. **GPU codegen**：5D 内核索引更长、控制流更复杂，可能触发 codegen 失败 → 朴素
   嵌套风格 + 展开；必要时 CPU 回退。
2. **内存峰值**：3D 模型中间张量大（如 `256³×64×4B ≈ 1GB` 级）→ 依赖 planMemory
   复用；需实测峰值，必要时分块。
3. **InstanceNorm 精度**：方差用 biased（÷N）与 ORT 对齐；eps 位型传递。
4. **形状折叠边界**：仅对「尺寸=输入形状+常量」的模型有效；数据依赖尺寸的模型
   需运行时形状（回退方案）。
5. **NHWC 布局**：3D 路线默认 NCHW；若启用 NHWC 需 3D 布局传播（低优先级）。

---

## 7. 结论

- **可行**。目标模型是固定架构的 3D U-Net，无数据依赖动态尺寸 → 用「固定 5D +
  输入形状注入折叠」能跑通，且与现有静态框架一致。
- **工作量 8-12 人天**（P1-P4），大头在 Conv3D/Resize3D 内核与动态形状折叠。
- **先行价值**：P1 的 `InstanceNormalization`（2D 版）和 3D 内核设计同时惠及 2D
  模型（YOLO/2D U-Net），不是沉没成本。
- **建议**：若确定做 3D，按 P1→P4 推进；若暂缓，可先摘出 `InstanceNorm`（2D）+
  `Resize`（2D）进 F8 2D 批次（对 2D 模型立即可用），3D 单独立项。
