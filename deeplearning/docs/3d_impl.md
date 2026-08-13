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

## 8. GPU 正确性修复：InstanceNorm 公式 bug（coarse_model 端到端）

**现象**：真实 coarse_model（3D U-Net，160³）GPU 输出 vs ORT max diff 0.767
（确定性、同一索引），而 CPU 匹配（6.1e-3，float32 漂移）。小尺寸测试（8³/32³）
全过 → 是**尺度相关**的核 bug。

**定位方法**（逐 op dump）：在 runGpu 的 op 循环末尾按 op 索引 dump 输出张量
（D2H 该张量范围），与「正确 16³ 输入的 ORT 参考」逐 op 对比，找到第一个分叉
算子。**注意**：run 结束后 dump 会因 arena 内存复用而显示垃圾——必须**每个 op
执行完立即 dump**。

**根因**：GPU `instancenorm` 核用 `invStd = scale / (Math.sqrt(variance) + eps)`，
而正确公式是 `scale / Math.sqrt(variance + eps)`（eps 在根号内，CPU 核一直正确）。
小方差通道（var~1.2e-4）两者差 4-8%；经 143 层 U-Net 累积 → 输出 0.767 误差。

**另一发现**：float 累加方差经 20+ 层 InstanceNorm 也会累积 ~0.4 误差 → GPU 核
需用 **double 累加**（与 CPU/ORT 一致）才能匹配到 float32 漂移水平。

**修复**（`gpu_ops.myp` instancenorm）：
- 正确公式 `Math.sqrt(variance + eps)`
- sum/ss/variance/invStd 用 double 累加

**验证**：coarse_model GPU vs ORT max diff 0.767 → **0.0065**（与 CPU 同级）；
237/237 编译器测试、全部 3D/2D infer 测试 CPU+GPU 通过。

## 9. GPU 性能：conv3d 寄存器 tiling（2026-08-13，§2.3 ①）

**基线**（fine_model_liver_vessel 96³，RTX 2070 SUPER，MYP_PROF_GPU=1）：
总 3090ms，其中 **conv3d 2253ms（73%）** 为唯一瓶颈；instancenorm 245ms、
H2D/D2H ~450ms（整 arena 3.2GB）。

**根因排查**：16³ tile 版 `conv3dTiled` 把全部输入通道 patch 一次装共享，真实模型
**56 个 conv 中 54 个（Cin≥16）共享超限回退 thread-per-output** → tiled 实际未生效
（≈原版 2.2s 是回退的假象）。

**重写（v3）**：`conv3dTiled` = **输出通道分组（OC_GRP=4）+ 输入通道分块（CC，
任意 Cin 进 tiled）+ W 寄存器滑窗（RW=4）**。块 = (nn, oc组, 4×8×32 空间 tile)，
256 线程各算 4oc×4W=16 输出；每 (ic,kz,ky) 载 6 输入 + 4oc×3 权重 → 16 输出
（每输出共享读 ~1.1，原 thread-per-output 全局读+L1）。要求 group=1、dil=1、
stride=1、k≤3（fine 模型全满足；否则回退 conv3d）。

**通用化（2026-08-14）**：v3 曾把 `stride=1、k≤3` 写死（针对 fine 模型特调）。
已重写为**通用算子**：任意 kd/kh/kw、stride、dilation 都走同一条 tiled 路径
（仅 group≠1 或共享超限回退 thread-per-output）。权重共享布局
`((ocIdx*CC+jic)*kd+jz)*kh*kw+jy*kw+jx`，计算侧 `wbase=(ic2*kd+kz)*kh*kw+ky*kw`
+ `ocStride=CC*kd*kh*kw` 逐 oc 偏移。**坑**：初版 compute 多加 `ic2*kd*kh*kw`
→ diff 344 错（cooperative 载入与 compute 读取的共享布局必须严格一致）；
权重读应写 `smem[patchTot + ocStride*ocIdx + wbase + kx]`（ocIdx=0..3）。
**验证**：bench/conv3d_gen_main.myp —— k=5/stride=2 GPU vs CPU diff=0 ✅、
k=3/dilation=2 diff=0 ✅；fine 模型（全 s=1/d=1）逐位一致。

**实测**：conv3d **2253 → 990 ms（2.3×）**；总推理 **3090 → 1686 ms（1.8×）**。
avgpool3d double→float（FP64=1/32 吞吐）15 → 0 ms。coarse_model GPU 591ms
（16³→160³）。**正确性**：conv3d 与旧验证输出逐位一致（diff=1e-5 仅 avgpool
float 漂移）；coarse vs ORT 6.1e-3（float32 漂移）✅；conv3d_main GPU vs ORT OK；
通用参数测试 ✅；回归 266/266。

**坑（本次）**：
1. patch 载入 D/H 维漏 `-pdt/-pt` 偏移（x 有 -pl）→ 卷积窗口整体偏移 → 输出错
   （vs 正确 diff 22.7）；加回偏移后逐位一致。
2. v1（per-oc 块）反而比回退慢：同一空间 tile 的 patch 被 yC 个块重复载入
   （yC× 冗余全局读）→ 必须 OC_GRP 分组摊薄。
3. `@gpu tile` 内 `float wbase = ...` 应为 `long`（索引）→ 编译错。
4. 通用化时权重共享索引：cooperative 载入按 `((ocIdx*CC+jic)*kd+jz)*kh*kw+jy*kw+jx`，
   compute 读取必须一致（wbase + ocStride*ocIdx），多加 `ic2*kd*kh*kw` → diff 344。

**⏳ 剩余**：conv3d 仍 ~1.0s（未达 <0.5s）——进一步需更大 OC_GRP / 共享 tile 摊薄
patch 重载、L2 感知调度；`gapool` 块归约未动（不在本模型）。CPU 核性能
（~1000× vs ORT）仍留待（§7 TODO）。

## 10. CPU 并行化 + SIMD（2026-08-14，@parallel + 4-wide）

**根因**：CPU 推理之前极慢，两个主因：(1) **mypc 默认 `-O0`**（`main.cpp` 默认 `opt_level=0`），编译 CPU 推理时没带 `-O3` → 单层 conv3d 32×16×64³ O0 12682ms vs O3 1150ms（11×）；(2) 纯单线程 + 标量循环。

**落地**（`ops.myp` + `runtime.myp`）：
1. **conv3d 输出通道维 `@parallel`**（各 oc 写独立区域，无竞争）。
2. **maxpool3d / avgpool3d 通道维 `@parallel`**（无害小收益）。
3. **instancenorm / reduceMean 并行化 → 负优化已回退**：coarse 16s→32s。原因：double 累加内存带宽饱和 + 通道数小（coarse 输出 6 类）负载不均衡。→ **经验：`@parallel` 只对计算密集算子（conv）有效，带宽受限算子保持串行**。
4. **conv3d 4-wide ox 展开（SIMD，核心）**：中间段 4 个连续 ox 共享输入读、去 padding 边界 if，LLVM **SLP 向量化**打包 4 个独立累加 → 单层 conv3d（32×16×64³，O3+fastmath）396→129ms（3.1×）。边界 ox（左/右）+ 中间尾部用带/不带 if 标量兜底。**完全通用**：任意 kd/kh/kw/stride/dilation/pad（边界公式 oxL=ceil(pl/sw)、oxR=ceil((xW+pl-(kw-1)*dw)/sw)），sw=1 时输入连续 SIMD 收益最大。
5. **instancenorm 3 趟 → 2 趟**：一趟同时算 Σx 与 Σx²（double 累加保精度；内存读同 float 带宽），省 1/3 内存读。
6. **MYP_PROF_CPU=1 剖析**（runtime.run() 按 opKind 累计，复用 GPU 版 opKindName）——定位 **conv3d 占 95%**（fine 56 conv = 42.4s / ops-loop 44.5s）。
7. SIMD 需 `MYP_FAST_MATH=1`（FP 重排/收缩 FMA）。

**实测（fine_model_liver_vessel 96³，16 核 CPU，O3+fastmath）**：fine CPU **212s→19.2s（11×）**（conv3d 42.4s→19.2s）；coarse **56.5s→7.23s（7.8×）**。ORT CPU 16 核 2.14s / 单线程 2.11s（图优化+融合后计算量小）→ MYP CPU 仍慢 **~9×**（conv 未 im2col；resize/relu/add 等未融合，占 ~2s）。

**正确性**：fine vs ORT rel 2.1e-3（FINE MATCH OK，4-wide 未引入误差）；coarse COARSE MATCH OK；conv3d 通用性（k5/s2、k3/d2）GPU vs CPU diff=0；回归 266/266。

**脚本**：`bench/fine_cpu_bench.myp`（CPU fine，同 GPU 输入，O3 编译，写 /tmp/seg_liver_cpu.f32）；`deeplearning/infer/tools/bench_fine_ort.py`（ORT CPU 对比，显式喂 [1,1,96,96,96]）；`bench/cpu_conv_simd.myp`（conv SIMD 可行性：去 if vs 4-wide）。

**⏳ 剩余**：fine 仍慢 ~9×（2.14s vs 19.2s）——resize3d/relu/add 等 element-wise 算子融合（省内存往返）+ conv+relu/bn 融合；conv 用 8-wide 展开进一步 SIMD；instancenorm float 累加（带宽再减半）。
