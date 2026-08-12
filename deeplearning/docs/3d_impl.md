# 3D 扩展实现总结（Conv3D / 5D 张量 / int64 形状折叠）

> 日期：2026-08-12
> 对应提交：852eb92（P1）、795fb2a（P2/P3）、bd9485b（P4a）、…（P4b fine）
> 目标：`deeplearning/infer` 纯 MYP 推理框架支持 5D（NCDHW）3D 医学影像网络
> （coarse/fine 两阶段 3D U-Net），并在 GPU（`@gpu for`）上运行。

## 1. 目标模型算子需求

| 算子 | coarse_model | fine_model_liver_vessel |
|---|---|---|
| Conv (→Conv3D) | 38 | 56 |
| InstanceNormalization | 37 | 54 |
| Relu | 28 | 41 |
| MaxPool (→MaxPool3D) | 4 | — |
| AveragePool (→AveragePool3D) | — | 5（含各向异性核 [1,12,12] 等）|
| Resize (→Resize3D) | 5 | 11 |
| Pad (→Pad3D) | — | 5 |
| Shape/Slice/Concat/ReduceProd/Cast/Unsqueeze/Gather/Constant(int64) | 90+ | 200+（Resize sizes 动态链）|
| Sub/Div/Mul/Sqrt/Add/Clip/ReduceMean | 归一化 | 归一化 |

两模型均：group=1、dil=(1,1,1)、opset 12/13、输入 `[1,1,D,H,W]` 动态维。

## 2. 运行时扩展（runtime.myp）

- `tD_` 属性 + `addTensor5(d0..d4)` / `addTensorPlanned5(d0..d4, off)`：
  5D 张量 `rows=N*C, cols=D*H*W, tN_=N, tC_=C, tD_=D, tH_=H, tW_=W`。
- `opX_[2048]` 属性：3D 算子额外参数 `[id*4+k]`（opP0..8 只有 9 个 int）。
- 3D 算子（opKind + CPU/GPU 分发 + 核）：
  - 41 `addConv3D`：权重 `[Cout,Cin,kd,kh,kw]`；opP=sd,sh,sw,6 pads；opX=dd,dh,dw,group
  - 42 `addMaxPool3D`：opP=kd,kh,kw,sd,sh,sw,pdt,pdb,pt；opX=pb,pl,pr
  - 43 `addAvgPool3D`：同 MaxPool3D + opX[3]=cip（count_include_pad）
  - 44 `addPad3D`：10 pads（N,C,D,H,W 各 begin/end）；opX=pwE,mode,cvalBits
  - 45 `addResize3d`：inD/H/W + outD/H/W + mode + transform

核（ops.myp / gpu_ops.myp）：
- `conv3d` / `maxpool3d` / `avgpool3d` / `pad3d` / `resize3d`
- 全部支持 5D NCDHW 行优先索引、各向异性核（pool）、6 pads、group、膨胀。
- GPU 端均为 `@gpu for (thread-per-output)`。

## 3. 图优化器扩展（graph.myp）

- **5D 形状表**：`shD4_` + `shR5_` 标志；`addShapeD5`；`copyShape`（秩无关传播）；
  `planMemory` 乘 `shD4_`；`buildRuntime` 走 `addTensorPlanned5`。
- **动态维覆盖**：`addShapeD4/D5` 遇已存在含 0 维条目（图输出动态维）→ 覆盖。
- **normalize3DNode**：按输入秩（5D）把 `Conv/MaxPool/AveragePool/ConvTranspose`
  改名为 `xxx3D`，并从通用原始 attr 重映射 3D 字段（SD3/DD3/KD3/PDT/PDB + 6 pads）。
  在 `inferShapes` 循环内调用（生产者输出形状已登记，下游 3D 池化输入秩已知）。
- **int64 形状链折叠 `foldShapeChains`**（幂等，inferShapes 前后各一次 + 循环内）：
  - 常量池 `i64v_` + `i64Reg`/`i64Get`/`i64Count`；初始器播种（`wType_==7`）。
  - 折叠 `Shape / Slice / Concat / Cast / Unsqueeze / Gather / ReduceProd / Constant(int64)`。
  - `Cast(int64→float)` → 注册运行时 float 常量（`wRole_=5`，值存 `cF32_`）。
  - `Resize` sizes 折叠 → `nRszD_/H/W_`（供 inferShapes 用）。
- **5D 兼容算子**：
  - `Concat`（axis=1 通道拼接）：4D 视图 `od3=H*W`（od0..2=N,C,D）。
  - `Sub/Div/Mul/ReduceMean/InstanceNorm`：空间大小 `S=D*H*W`（5D）。
  - `Resize` inferShapes：4D 用 `W=shD3_`、5D 用 `D=shD2_,H=shD3_,W=shD4_`。

## 4. 加载器扩展（onnx_loader.myp）

- **NodeField 44-48**：SD3/DD3/KD3/PDT/PDB（3D 第 1 个 stride/dil/kernel + D-pads）。
- **NodeField 49-63**：原始 strides/pads/dilations/kernel_shape 通用捕获
  （`nSt3_/nPd3_/nDl3_/nKk3_`），normalize3DNode 按秩映射。
- `Cast` 的 `to` 属性（NodeField 64）。
- **输入形状注入 `setInputShape`**：动态维模型（`[1,1,0,0,0]`）注入具体输入形状。
- `parseDim`：dim_param / dim_value=0 → 返回 0（动态维标记，供 inferShapes 覆盖）。

## 5. 测试与验证

| 测试 | 验证点 | vs ORT |
|---|---|---|
| `conv3d_main` | Conv3D+InstanceNorm+Relu+MaxPool3D（x[1,2,8,8,8]）| 7.15e-07 |
| `resize3d_main` | Resize3D trilinear align_corners | 2.38e-07 |
| `coarselike_main` | 复刻 coarse 预处理（Shape→Slice→Concat→sizes + 归一化 + 小卷积块）| 9.5e-07 |
| `pad3d_avgpool3d_main` | Pad3D + AvgPool3D（含各向异性核）| 5.96e-08 / 1.19e-07 |
| `coarse_main` + `check_coarse.py` | 真实 coarse_model（234 节点/143 op/86 fold）| max abs 6.1e-3（rel 2.1e-4）|
| `fine_main` + `check_fine.py` | 真实 fine_model（435 节点/209 op/161 fold）| … |

真实模型与 ORT 的差异 ~1e-3 相对量级属深层网络 float32 累积漂移
（合成小模型均 <1e-6）。

## 6. 关键设计取舍

- **ONNX 单算子多秩**：Conv/MaxPool/AveragePool 的 2D/3D 由输入秩判定
  （normalize3DNode），避免在加载器按 op_type 硬编码。
- **Resize sizes 动态链**：Shape 依赖输入形状 → 输入形状注入 + int64 常量解释器
  在 inferShapes 过程中逐步折叠（生产者形状随拓扑序就绪）。
- **5D Concat 用 4D 视图**：通道拼接（axis=1）下，把 `[N,C,D,H,W]` 视作
  `[N,C,D,H*W]`，复用 4D concat 核（od3=H*W）。
- **f32/f64 混合**：核内用 float 累加（`float` / `0.0f` 字面量），
  避免 double 降低 GPU kernel 兼容性；大模型累积误差 <1e-3。

## 7. ⚠️ 性能问题（已标记，后续优化）

**现状**：CPU 朴素核 vs onnxruntime 差距 ~1000×（coarse_model：ORT 0.9s，MYP ~20min）。
**这不是语言解释开销**（MYP 编译为 LLVM 原生码），而是「朴素标量单线程核 vs 工业级优化」。

差距来源（相乘）：
- 单线程 vs OpenMP 多线程（8-16×）
- 无 SIMD 向量化（AVX2/AVX-512）vs 一次 8/16 float（4-8×）
- 无 Winograd（3×3 核 ~2.25× 乘法减少）/ im2col+GEMM
- 内层 padding 边界分支 + 乘除索引预计算缺失（2-3×）
- 无 cache 分块/布局重排（2-4×）
- 算子融合较少（Conv+BN+ReLU 折叠有做，但中间张量读写仍多）

**后续优化路线（按收益排序，独立任务）**：
1. 多线程（最简单，8-16×）
2. 内层边界检查外提 / 索引预计算（2-3×）
3. im2col + 复用现有 dense GEMM（2-5×）
4. Winograd 3×3（~2×）
5. 向量化（依赖编译器支持）
6. **GPU 加速**（每输出一线程，天然并行；当前已铺好 `@gpu for` 核）
   → 这是当前优先方向，见 gpu_paradigm.md / 3D GPU 核。

> TODO(perf)：待 GPU 方向完成后，再回头优化 CPU 核性能。
