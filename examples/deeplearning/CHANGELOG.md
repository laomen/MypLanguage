# DeepLearning 训练/推理框架 变更日志（examples/deeplearning）

> 本文件记录 **deeplearning 分项目**（`examples/deeplearning/`）的独立变更，
> 与主仓库编译器 `docs/CHANGELOG.md` 分离（主 changelog 只记编译器/运行时/stdlib）。
> 分项目内的跨分项目运行时配合改动（如 GPU 运行时泄漏修复）也在此记录上下文。

---

## 2026-09-03 — GlobalAveragePool3D（5D 感知 gapoolD kernel）

- 根因：GAPoolOp 只读 tensorH/tensorW（忽略 tensorD）→ 5D 输入只对 D=0 片 H*W 求
  均值（探针 [4.5,45] 得 [2.5,6.5]）。
- 新增 gapoolD（CPU + GPU thread-per-(n,c)）：per-(n,c) 连续段 = D*H*W 全空间均值；
  GapoolOp/GpuGapoolOp 分支 tensorD>1 → gapoolD（4D 路径字节不变）。
- 测试 gap3d.json（x[1,2,2,2,2] c0=1..8/c1=10..80 → [4.5,45]）CPU+GPU。
  全量回归 pass=116→**117** fail=0。

---

## 2026-09-03 — ConvTranspose3D 图算子 + JSON（3D 转置卷积 fwd，CPU）

- kernel：转置卷积 gather 公式（od+pdt-k ≡0 mod sd → id=(od+pdt-k)/sd 界内），
  W[Cin,Cout,kd,kh,kw] 直接拷贝；group=1 无膨胀；outPad 并入输出形状（尾行=bias）。
- graph：OpCode CONVTRANSPOSE3D(114) + mapOpType + inferShapes(addShapeD5) +
  classifyShapes 权重 CNN_W/CNN_B + graph_compiler wiring（opKind 86，CPU）。
- JSON：W 5D 预注册 + ConvTranspose3D 分派（setKernel3DAttrs 3D）。
- 测试 convt3d.json（x[1,1,2,1,1]=[1,3] W=[2,5] stride1 → [2,11,15] 手算）+ convt3d_s2.json
  （stride2 → [2,5,6,15]——U-Net 解码 2x 上采样几何）。CPU+GPU runAuto 全过。
- **训练反向 BwdConvTranspose3D（CPU+GPU）**：OpCode BWD_CONVTRANSPOSE3D(115) +
  buildReverseGraph ConvTranspose3D→BwdConvTranspose3D + opKind 87 registerFwdBwd(86,87)；
  bwd kernel db[co]=Σdy / dW[ci,co,kz,ky,kx] dy·x 整除路由累加 / dX 反向散布（CPU 三重循环
  + GPU 三内核 thread-per-元素）。测试 convt3d_tr.json（→Flatten→Gemm→Softmax 分类
  400 步每类 loss 0.0009/0.0004）。顺修 fwd pad 槽映射（pdt/pt/pl=opP3/opP5/opP7）。
  全量回归 pass=115→**116** fail=0 —— ConvTranspose3D 推理+训练全链（fwd/bwd CPU+GPU）。

---

## 2026-09-03 — Pad 非 0 常量值（审计「Pad value」收口）

- graph.myp readF32Init 增加**内存 f32 分支**（GraphWeights memVal_ offset=-2，同
  readI64Init 内存分支）——Pad constant_value / Clip 边界等标量参数现可经推断路径读。
- json_model Pad 分派：`mode:"constant"` 且给 `value` → regF32Scalar 内存标量 wire
  nodeIn2 → inferShapes 折叠进 nPadCval_（kernel/Op 早已吃 cval）。
- 测试 pad_val.json（x[1,1,1,3]=[7,8,9] W 两侧 pad1 value=5 → [5,7,8,9,5]）CPU+GPU。
  全量回归 pass=113→**114** fail=0。
- 探测 GlobalAveragePool3D：5D 输入下现 GAP kernel 非 5D 感知（只按 H·W 均值）→
  留待独立 5D GAP kernel（不做"免费"项）。

---

## 2026-09-03 — GroupNorm + OneHot 图算子（审计行 1 收尾 + 索引族起步）

全量回归 pass=111→**113** fail=0（P2-4 条目见下）。

### P5：GroupNorm 图算子 + JSON（逐通道组归一化，CPU+GPU）
- 审计「LayerNorm/RMSNorm/GroupNorm」行最后一块（diffusion 只有私有 kernel 非图算子）。
  kernel = SD1.5 norm_num_groups 语义 + N 通用：每 (n,g) 组 mean/var over cpg*H*W；
  gamma/beta [C]，gOff/bOff<0 guard。CPU InferOps + GPU（K1 grid=N*groups 算
  mean/rstd→stats 暂存；K2 thread-per-element 应用），opKind 84。
- graph：OpCode GROUPNORM(112) + mapOpType + inferShapes copyShape + classifyShapes
  gamma/beta markFCBias（同 InstanceNormalization）+ graph_compiler wiring
  （groups=GROUP()、eps=EPS_INT；N/C/H/W 由运行时张量自带）+ JSON（gamma/beta [C]
  预注册、groups/epsilon）。
- 测试 groupnorm.json：x[1,2,2,2] 双输出覆盖 cpg=1（groups=2 每通道组）与 cpg>1
  （groups=1 全样本组）——手算 tol1e-3 CPU+GPU OK。

### P6a：OneHot 图算子 + JSON（逐元素索引 → 行优先 one-hot，CPU+GPU）
- 索引族（GatherElements/ScatterND/OneHot/Range）起步。kernel idx float[nIdx] →
  out[nIdx*depth] 行优先（每输入元素一行），idx 越界 clamp；CPU + GPU thread-per-output，
  opKind 85。graph OpCode ONEHOT(113) + inferShapes（输出 [depth,M]；FC runtime
  rows=M cols=depth）+ JSON depth。测试 onehot.json x[1,3]=[0,1,2] depth=3 → 单位阵
  展平 CPU+GPU OK。
- 该行另三留待：GatherElements/ScatterND（数据高级索引）、Range（无输入生成器，
  与 runtime 单/多输入 op 模型不适配——JSON 生成序列可先手写循环）。

---

## 2026-09-03 — 算子缺口优先级 2-4：Tanh / MSE·BCE loss / ArgMax·TopK

按「算子缺口审计」优先级收口（P1 LayerNorm/RMSNorm/GELU 见下条；全量回归
pass=106→**111** fail=0）：

### P2：Tanh 激活图算子（fwd/bwd，CPU+GPU + 训练）
- OpCode TANH(105)/BWD_TANH(106) + mapOpType + inferShapes 白名单 + opTraits。
- kernel y=2/(1+e^-2x)-1（大 |x| 双侧稳定）+ bwd dx=dy·(1-y²)（用 fwd 输出 y）；
  runtime addTanh(opKind 46)/addBwdTanh(108)；TanhOp/GpuTanhOp registerFwdBwd(46,108)；
  buildReverseGraph Tanh→BwdTanh。测试 tanh.json（手算 [0,0.7616,-0.7616,0.9640]）
  + tanh_train.json（x→Gemm→Tanh→Gemm→Softmax，每类 loss 0.014/0.011/0.012）。

### P3：MSE/BCE 逐元素损失（lossMode 2/3，图级训练——解锁 UNet 分割训练）
- mseLoss（mean(out-t)²，dp=2(out-t)/N）+ bceLoss（-mean[t·ln p̂+(1-t)ln(1-p̂)]，
  p̂=clamp(1e-7,1-1e-7)）；opKind 109/110 registerBwd（CPU-only 训练）。
- graph MSELOSS(107)/BCELOSS(108)；**buildReverseGraph 泛化**：lossMode 2/3 = 逐元素
  损失（模型输出本身是预测张量，无需 Softmax 分类头；label 同预测形状灌实数目标/
  掩码），0/1 保留 SoftmaxCE/Dice 原路径；Session.setLossMode + json_model 转发。
- 测试 mse_reg.json（回归 500 步 loss→0）+ bce_clf.json（600 步 loss→floor 0.325、
  方向正确）。

### P4：ArgMax/ArgMin/TopK 推理图算子 + JSON（行/特征轴 1D）
- argmax/argmin（单值 float 索引）+ topk（前 k 大 values+indices，并列按下标小者先）
  CPU+GPU；opKind 47/48/49；OpCode ARGMAX(109)/ARGMIN(110)/TOPK(111)；JSON TopK 用
  outs:[values,indices] + k。语义：沿输入行（分类/特征）轴 1D flat（单样本=整行
  logits，对应 runtime argmax1d/LLM 采样/CE 标签），输出 float 编码索引。
- **基建修复**：NodeField 新增标量槽须放宽 graph_node_attrs 存储——stride 65→66 +
  managed()/resetNode 注册（原槽仅 0..48 + CAST_TO(64)；新槽会被静默丢弃）；
  graph.myp nodeInt 接受集补 TOPK_K。测试 argmax.json（ArgMax=1 并列先遇/ArgMin=2/
  TopK tv=[0.9,0.9] ti=[1,3]）CPU+GPU。

---

## 2026-09-03 — LayerNorm/RmsNorm/GELU 图算子 + JSON（归一化/激活补全）

- **图算子接入（kernel/Op/CPU+GPU/register/runtime 均现成——LLM 手写 opKind 61/64/65
  只缺图接线）**：graph_defs OpCode LAYER_NORM(102)/RMS_NORM(103)/GELU(104) +
  mapOpType（LayerNorm/LayerNormalization、RmsNorm、GELU/Gelu）；inferShapes
  copyShape；graph_compiler wiring（D/S/n 传 0 → Op forward 运行时自省
  tensorRows/Cols——tensor 布局与 kernel [D 特征行, S 样本列] 天然一致）；eps attr。
- 归一化 kernel（layerNorm）加 gOff/bOff < 0 guard（无 gamma/beta 防越界）。
- JSON 分派：LayerNorm（gamma/beta [D] + epsilon）、RmsNorm（gamma [D] + epsilon）、
  GELU（单输入自动）。测试 json_norm_main（单样本 x[1,3] 手算：LN [-1.2247,0,
  3.674]、RMSN [0.4629,0.9258,1.3887]、GELU [-0.0455,-0.1587,0,0.8413,1.9545]）
  CPU+GPU JSON NORM OK。回归 pass=105→**106** fail=0。
- 布局注：JSON 2D 张量按框架 [特征行, 样本列]（dims=[样本,特征] 但数据特征主）——
  图归一化多接 Gemm 输出（布局自洽）；多样本直喂输入须特征主 flat（测试用单样本
  免歧义）。

---

- **JSON 3D U-Net 端到端跑通**：`unet3d.json`+`json_unet3d_main`（data[1,1,8,32,32]
  → Conv3D pad1 保尺寸 + MaxPool3D×2 → Resize(sizes 2x 上采样) + Concat(5D) 跳跃 ×2
  → Conv1x1 → Sigmoid，18 层）CPU+GPU `MYP_IR_VERIFY=1` JSON UNET3D OK。3D JSON
  分派（上轮）已足以表达完整 3D U-Net。
- **Split 多输出 JSON 结构**：层用 `outs`:[a,b,c] 数组 + axis + 可选 split 尺寸
  （int64 nodeIn1）；graph `nodeOut(i,name)` 自动填空槽 + outputCount（本为 Split 设计）；
  Skip 判断允许 Split 无单一 out。`split.json`+`json_split_main`：x[1,9] 均分 3 段
  a/b/c → [1,2,3]/[4,5,6]/[7,8,9] 三图输出 CPU+GPU OK。
- **3D 训练反向 🟥 撤销（假警报）**：c3d 训练 loss 波动实为三类样本初始随机 label 错配
  → 各自 loss 参差（cl0≈0.02 对 / cl1≈10.6 错）；判据只看首尾单样本误判。修判据
  （收敛后每类测 loss<0.5）→ `json_c3d_train_main` 300 步后三类 0.219/0.106/0.033，
  3D 反向全链（BwdConv3D/BwdMaxPool3D/BwdFlatten/BwdDense+CE）正常。教训：多类单
  样本训练判据须测每类收敛 loss。
- **并行回归**：`infer_tests/run_all.sh`（两阶段：编译 P4 控 LLVM 内存 + 运行 P6；
  mypc 并发偶发瞬时失败加编译重试兜底）——全量 105 测试 3m44s vs 串行 10min+。
  回归 pass=102→**105** fail=0。
- 仍留：Pad constant_value、ConvTranspose3D；LayerNorm/RMSNorm 图算子（kernel 已有
  RMSNorm opKind 61 未接图）、GELU/Tanh 激活、MSE/BCE loss 等见算子缺口审计。

---

- **JSON 补齐算子分派**（json_model.myp；ONNX/图级本已支持，只缺 JSON 接线）：
  `Conv3D`（5D W + B + 3D kernel/strides/pads[6]/dilations/group）、`MaxPool3D`/
  `AveragePool3D`（3D 几何 + count_include_pad）、`Resize`/`Upsample`（int64 sizes
  → nodeIn3，2D [1,1,outH,outW] / 3D [1,1,outD,outH,outW]）、`BatchNormalization`
  （[C] scale/bias/mean/var + eps）、`InstanceNormalization`（[C] scale/bias +
  eps）、`Clip`（内联 min/max → 内存 f32 标量常量）、`LeakyRelu` alpha 内联。
  权重预注册加 Conv3D/BN/IN；新增 `setKernel3DAttrs`（pads 6 值 3D 映射）+
  `regF32Scalar`（f32 标量常量）+ import pb（F32.toBits eps/alpha 位型）。
- 测试（全 CPU+GPU + MYP_IR_VERIFY）：`json_c3d`（Conv3D→Relu→MaxPool3D→
  Flatten→Gemm→Softmax 3D 推理全链）、`json_bn`（y=[2.0,3.125] 手算）、`json_in`
  （总体方差手算对拍）、`json_clip`（[0,0.5,1,1] 手算）、`json_resize`（nearest
  sizes 语义断言）。回归 pass=97→**102** fail=0。
- **3D 训练反向验证为正常**（撤销初判 🟥）：c3d 3D CNN 训练 loss 波动实为三类样本初始
  随机错配 → 各自 loss 参差（cl0≈0.02 对 / cl1≈10.6 错），判据只看首尾单样本误判。修正
  判据（收敛后每类测 loss）→ `json_c3d_train_main` 300 步后三类 loss 0.219/0.106/0.033
  全 <0.5，3D 反向全链（BwdConv3D/BwdMaxPool3D/BwdFlatten/BwdDense+CE）正常。教训：多类
  单样本训练判据须测每类收敛 loss。
- 仍留：Split 多输出（JSON out 单名结构限制）、Pad constant_value（graph_compiler
  未接）、ConvTranspose3D（无独立 3D deconv runtime）。

---

### 修复：训练图结构融合断链 → Conv 权重从不更新（隐蔽 bug）
- 现象：CNN 训练 loss 降但 `c_W` 10 步后 maxdelta=0（FC-only 假象——BwdConv 不在
  训练图）。根因：`runPipeline` 对训练图也跑 FUSE_CONV_RELU/FUSE_RELU/FUSE_GAP_FLATTEN；
  `fuseSingleUseOutput` 把 producer 线性输出标 dead + 有效输出改 fusedOut，而
  `buildReverseGraph` 反向遍历按 `compilerNodeOutput(ni,0)`（已 dead 的原输出名）判
  `grad(y)` 存在 → 融合 Conv 被跳过 → 无 BwdConv/无 Update。
- 修复：训练图跳过结构融合（保留独立 Relu/Flatten 节点使反向逐节点回传）。
  IGraphOptimizeHost 新增 `shouldFuseConvReluForOptimizer/shouldFuseReluForOptimizer/
  shouldFuseGapFlattenForOptimizer`（`trainingMode_==0?1:0`，同既有
  shouldFuseConvAddForOptimizer）；推理图仍融合（单内核加速）。FUSE_BN/FOLD_RELU 保留
  （代数等价/当前无 BN 训练模型）。验证：`c_W grad -1→21`、10 步 delta 0→0.086。
- **D 组数值 grad-check 固化**：`grad_cnn.json`（线性 CNN Conv→Flatten→Gemm→Softmax，
  无 relu/pool 全光滑）+ `json_cnn_gradcheck_main.myp`：lr=0 runTrain 反向写 grad 张量
  vs ±ε 有限差分逐元素对拍 239 权重（c_W/c_b/logits_W 216/logits_b）worstRel=0.123
  bad=0 → CNN GRAD OK（BwdConv dw/db + BwdReshape + BwdDense + CE 端到端精确）。
  教训：含 Relu（dead-zone）/MaxPool（argmax 突变）的网有限差分不可靠（ε 无关误差）；
  grad-check 须用无激活/无 argmax 的完全光滑网做权威对拍。

### 激活反向全覆盖（ReLU6/LeakyRelu/SiLU/HardSwish/LogSoftmax/Clip，CPU-only 训练）
- 此前仅 Relu/Sigmoid 有反向；补齐其余激活使 SwiGLU 等可端到端训练。全链路仿
  BwdSigmoid：OpCode BWD_RELU6(96)/BWD_LEAKY_RELU(97)/BWD_SILU(98)/BWD_HARDSWISH(99)/
  BWD_LOG_SOFTMAX(100)/BWD_CLIP(101) + opCode map + opTraits=2；ops.myp kernel
  （均需 fwd 输入 x 判导数：relu6 in(0,6)；leaky x>0?1:α；silu σ(x)(1+x(1-σ))；
  hardswish 三段；logsoftmax dx_i=dy_i-σ_i·Σdy；clip in[lo,hi]）；runtime addBwd*
  opKind 102-107；CPU 激活类 backward + registerFwdBwd（GPU 前向保留，训练反向
  CPU-only）；buildReverseGraph 分支 + graph_compiler wiring + bwdLike 豁免。
- **JSON op 名规范化**：`"Relu6"`（JSON 常见写法）≠ 框架规范 `"ReLU6"`（各 pass 按
  nodeTypeAt 原串比较）→ UNKNOWN + inferShapes fail。修：json_model 节点构建前
  `if(op=="Relu6") op="ReLU6"`。
- 测试：`bwd_activ_main.myp`（6 kernel dy·g(x) 手算对拍 bad=0）；`swiglu.json`+
  `json_swiglu_train_main`（fan-out 双 Gemm + SiLU + Mul SwiGLU，200 步 loss
  1.088→0.004）；`activ_chain.json`+`json_activ_chain_train_main`（LeakyRelu→ReLU6→
  HardSwish 链 300 步 loss 0.973→0.637）。Clip/LogSoftmax 训练主干罕见：kernel 直测 +
  同 register 机制（不强求训练 json）。

### JSON 2D U-Net + 文档双语化
- `unet2d.json`+`json_unet2d_main.myp`：2D U-Net（data[1,1,64,64]→编码 Conv3x3+
  MaxPool×2→瓶颈64→解码 ConvTranspose(2x)+Concat 跳跃 a3/a1→Conv1x1→Sigmoid），
  CPU+GPU `MYP_IR_VERIFY=1` JSON UNET2D OK。要点：3x3 Conv 必须 `pads:[1,1,1,1]`
  保尺寸否则跳跃 Concat 尺寸不匹配（初版输出 24²）；解码上采样用 ConvTranspose
  (k2,s2) 恰抵消 MaxPool 2×。
- `infer/README.md` 中英混杂 → **README.md（纯中文版）+ README_EN.md（新建英文完整
  版，英文历史迁入）**；`docs/sli.md` 补「连线=张量名」「选用/更换激活函数（含
  SwiGLU 组合）」「op 集补激活全族」「训练激活反向全覆盖」。
- 回归 pass=92→**97** fail=0。

---

## 2026-09 — B 组训练反向（ReduceMax/Min argmax + Pad constant）+ C 组 JSON（Where/Sqrt/Dropout）

- **B：ReduceMax/Min 反向（argmax 掩码，BwdReduceMM）**：归约 max/min 反向需把每组
  dy 赋给该组 argmax 位置（其余 0）——**需 x 值重算**最值位置（mode0 全组 / mode1
  per-(n,c) 段；并列取首个）。OpCode `BWD_REDUCEMM(94)` + runtime opKind 100
  （A=dy B=x C=dx）+ ops `bwdReduceMM` kernel + BwdReduceMMOp + registerBwd(100) +
  graph_compiler wiring + buildReverseGraph ReduceMax/Min 分支。
- **B：Pad(constant) 反向（去边取中心，BwdPad）**：constant pad 的 x 每元素唯一对应
  y 位置（offset 每轴=begin）→ dx = dy 中心区直接赋值（无需累加/清零）。reflect/edge
  需镜像累加未实现——buildReverseGraph Pad 仅 PADMODE==0 产 BwdPad。OpCode
  `BWD_PAD(95)` + runtime opKind 101 + ops `bwdPadConst` kernel + BwdPadOp +
  registerBwd(101) + graph_compiler wiring + buildReverseGraph Pad 分支（begin/end
  复制自 fwd 折叠 attr 组 3/4）。
- **C：Where JSON 分派**（3 输入 cond?x:y：in=cond in2=x in3=y）——之前 Where 只
  ONNX 支持；**Sqrt/Dropout 单输入无属性默认分派本就可用**（本轮测试固化；Dropout
  无 ratio 推理恒等）。
- 测试：`bwd_reduce_mm_main.myp`（max/min × mode0/1 四组 argmax 对拍）+ `bwd_pad_main.myp`
  （dx=dy 中心）+ `where.json/json_where_main` + `sqrt.json/json_sqrt_main` +
  `dropout.json/json_dropout_main`（CPU+GPU）。全量回归 pass=92 fail=0。
- **留待（低 ROI）**：Slice 反向（多轴负步长 scatter）、Split 多输出 JSON、
  BN/InstanceNorm/Resize JSON（JSON 少用中间 op）。

---

## 2026-09 — AvgPool2D 反向接通（2D 池化训练反向全覆盖：MaxPool/AvgPool/GAP）

- **背景**：2D AveragePool 无训练反向（此前 CNN 训练用 MaxPool/GAP 避过）。补全链路：
- **补全**：graph_defs OpCode `BWD_AVGPOOL(93)` + opCode map + opTraits(2)；runtime
  `addBwdAvgPool`（opKind 99，opP0-8=kh,kw,sh,sw,pt,pb,pl,pr,cip）；ops.myp
  `bwdAvgPool2D` kernel（同 fwd 窗口有效数 cnt 分母，cip=1→kh*kw；dy 均摊回 dx
  累加、清零）；ops_iface AvgpoolOp.backward + registerFwdBwd(36→99)（GPU 前向
  保留，训练反向 CPU-only）；graph_compiler BWD_AVGPOOL wiring + buildReverseGraph
  `AveragePool(2D)→BwdAvgPool` 分支（复制 KH/KW/SH/SW/PT/PB/PL/PR/CIP + dx guard）。
- 测试 `infer_tests/json_avgpool_cnn_train_main.myp` + `avgpool_cnn.json`：Conv(2ch
  3x3)→Relu→AvgPool(2x2/2)→Flatten[1,18]→Gemm→Softmax 200 步 SGD loss
  **1.037→0.431** 降（CPU+GPU，MYP_IR_VERIFY=1）。全量回归 pass=87 fail=0。
- **至此 2D 池化训练反向全覆盖**（MaxPool/AvgPool/GAP）——CNN 训练可配任意池化。
- **工具教训**：multi_replace_string_in_file 报告失败时**可能已部分/全部写入**（本次
  buildReverseGraph 分支实际写入却报失败，靠 git status/diff 复核补交）——失败后
  务必 git diff 检查残留，勿假定完全未应用。

---

## 2026-09 — GAP 反向（复用 BwdReduce mode1 mean）+ CNN 训练覆盖 GlobalAveragePool

- **背景**：CNN 训练固化（上条）用 MaxPool 避过 GAP——GAP（per-(n,c) 空间均值）
  无独立反向。GAP 反向 = **复用 BwdReduce mode1 mean**（前向均值 → 反向 dy[n*C+c]
  广播回 H*W 段 /S），零新 kernel。
- buildReverseGraph 加 `t=="GlobalAveragePool"` → BwdReduce 节点
  （`compilerSetNRedMode(bd,1)`+`SetNRedType(bd,0)`；graph_compiler BWD_REDUCE 已
  通用读回，N/C/S/total 从 x(nodeIn1) shape 推）。
- 测试 `infer_tests/json_gap_cnn_train_main.myp` + `gap_cnn.json`：Conv(2ch 3x3)
  →Relu→GAP([1,2,6,6]→[1,2,1,1])→Flatten[1,2]→Gemm→logits[1,3]→Softmax，200 步
  SGD loss **1.055→0.901** 降（MYP_IR_VERIFY=1）。全量回归 pass=86 fail=0。
- **数据可学性教训**：GAP 丢空间位置——class 只差块位置（内容全 1）时 GAP 特征
  无区分，loss 反升趋 ln3（**非 bug**，数学合理）；class 须内容可区分（块像素
  =cl+1）。训练回归输入设计须先确认结构理论上可学。
- **后续**：AvgPool2D 反向（池化族最后缺口）；多层真实 CNN demo。

---

## 2026-09 — JSON CNN 分类训练端到端固化（重要发现：CNN 训练反向全链本就可用）

- **背景**：此前所有 JSON 训练回归都是 FC/加性网（softmaxCE 2D logits 的误解——
  以为训练 loss 路径只能 2D，CNN 4D 无法训）。实证打破：**CNN 经 Flatten 回 2D 即可训**
  ——Conv/MaxPool2D 反向（BwdConv/BwdMaxPool）早已在框架，只是从未有端到端 CNN
  训练回归覆盖，缺失验证。
- `infer_tests/cnn_train.json` + `json_cnn_train_main.myp`：data[1,1,8,8] →
  Conv(2ch 3x3)→Relu→MaxPool(2x2)→Flatten[1,18]→Gemm→logits[1,3]→Softmax。
  loadJsonTrain + MYP_IR_VERIFY=1（ops=11）通过；200 步 SGD（8x8 class 特征块
  输入 + one-hot[3]）loss **1.01059→0.142419** 显著降（CPU+GPU 同值；runTrain CPU
  计算）——梯度经 BwdConv/BwdMaxPool2D/BwdRelu/BwdFlatten(copy)/BwdDense 全链回传
  更新 Conv+Gemm 权重。全量回归 pass=85 fail=0。
- **后续可扩**：AvgPool2D/GlobalAveragePool 反向（当前 CNN 用 MaxPool 避过）、更
  真实（多层）CNN 训练 demo、CNN 推理/训练数值对拍。

---

## 2026-09 — JSON Pad 分派（pads int64 + mode）

- **背景**：Pad 框架链路本就完整（inferShapes Pad 分支 readI64Init 读 nodeIn1 折叠进
  nPadB_/nPadE_ int 数组 + graph_compiler PAD 读回，ONNX 位精确），JSON 只缺分派。
- json_model 预注册 `pads` → `out_"_pads"` int64（**ONNX opset11 8 值
  [N,C,H,W] begin + [N,C,H,W] end**）；节点分派接 nodeIn1 + `mode` 字符串 →
  NodeField.PADMODE（constant=0/reflect=1/edge=2，默认 constant）。constant_value
  非零未支持（f32 标量常量内存通道未做，默认 0）。
- 测试 `infer_tests/json_pad_main.myp` + `pad.json`：x[1,1,3,3] 1..9 → Pad
  pads=[0,0,1,1,0,0,1,1]（H/W 四边各 1）→ out[1,1,5,5] constant 0 边界 + 中心
  数据手算，CPU+GPU JSON PAD OK。全量回归 pass=84 fail=0。
- **剩余 A 组**：Split 多输出（需多输出槽）、Where/Dropout/BN/InstanceNorm/Resize
  JSON 分派（低 ROI 留需）。

---

## 2026-09 — JSON Slice 分派（starts/ends/axes/steps int64 内存常量）

- **背景**：Slice 是框架最成熟算子之一（ONNX slice_main 位精确，inferShapes 用
  readI64Init 读 nodeIn1-4 + compilerAttrSetLongArray 填 attr + sliceAxis 算维度），
  JSON 只缺分派接线。
- json_model 预注册 `starts/ends/axes/steps` → `out_"_s0.._s3"` 内存 int64 常量；
  节点分派接 nodeIn1-4（**axes/steps 可缺省不接** → inferShapes 默认 axes=0..、
  step=1）。数据输入 float → inferShapes 顶部 Slice 拦截守卫（i64Count(in0)）不触发。
- 测试 `infer_tests/json_slice_main.myp` + `slice.json`：x[1,2,3,4] 值 1..24 → Slice
  starts[1,0] ends[3,4] axes[2,3]（H 取 1..2、W 全取，ends 开区间，steps 缺省全 1）
  → out[1,2,2,4] 16 值手算 [5..12,17..24]，CPU+GPU JSON SLICE OK。全量回归
  pass=83 fail=0。
- **剩余 A 组**：Pad JSON（pads attr 数组 + mode，需核对 compilerAttrIntArray 组号）、
  Split 多输出（需多输出槽 out2/out3）。

---

## 2026-09 — JSON registerWeight 显式 values + Embedding（查表）分派

- **registerWeight 支持 `values` 数组**：JSON 权重 W/B 除 `init`(LCG)/`safetensors` 外
  可内联 `"values":[...]` 按 dims row-major 手写权重（float 数组，`j.getDouble` 逐
  元素读）——替代随机/文件权重源，**值可精确复现**（单 op 手算验证、Embedding 表等）。
- **JSON Embedding 分派**：inferShapes/classifyShapes（markFCWeight transB=1 + ids
  dead）/graph_compiler（readI64Init → 临时 idxT + addEmbedding）本就支持 Embedding——
  补齐 json_model 分派：预注册 W 权重 + ids int64；buildGraph 第 3 步放宽 in 条件
  （Embedding 无数据输入，W=nodeIn0 `out_W`、ids int64=nodeIn1 `out_idx`）。
- 测试：`infer_tests/json_value_main.myp`+`value_gemm.json`（x[1,2]=[1,2] → Gemm
  W values [3,4,5,6,7,8]=[[3,4],[5,6],[7,8]] B values [1,2,3] → y=[12,19,26] 手算
  精确，CPU+GPU JSON VALUE OK）+ `json_embedding_main.myp`+`embedding.json`（W
  values [10,11,20,21,30,31] + ids=[2,0,1] → out=[30,31,10,11,20,21] 查表，CPU+GPU
  JSON EMBEDDING OK）。全量回归 pass=82 fail=0。

---

## 2026-09 — B 训练反向补：Gather（scatter-add）

- **背景**：B 系列后 loss 路径仍缺 Gather（沿 axis 收集）反向。收集反向 =
  **scatter-add**：out 每元素来自 data 的 idx[o_axis] 行，indices 可重复 →
  dx[src] 累加 dy（先清零）。
  - ops.myp `bwdGather` kernel：清零 dx（data dims dn/dc/dh/dw）后遍历 out 累加；
    源索引 clamp/负索引同前向；out dims on/oc/oh/ow 从 dy tensor tN_/C_/H_/W_ 读
  - graph_defs OpCode `BWD_GATHER(92)` + opCode map + opTraits(2)；runtime
    `addBwdGather`（opKind 98，A=dy B=idxT C=dx P0=axis P1=lenIdx P2-5=data dims）
  - graph_compiler BWD_GATHER：**indices=nodeIn2 → readI64Init 建临时 f32 idxT
    （同 fwd GATHER）**；axis 从 bwd node attr；data dims 从 nodeIn1/x shape。
    buildReverseGraph Gather 分支复制 AXIS + 传 indices 名；dx guard（Gather data
    常是图输入 → dx 清空，backward dx<0 return）
- 验证 `infer_tests/bwd_gather_main.myp` runtime 数值对拍：data[1,3,1,1] axis1
  idx[0,2] dy=[5,7] → dx=[5,0,7]；**idx[0,0] 重复 → dx=[7,0,0]（累加）**。
  CPU 全 PASS BWD GATHER OK。全量回归 pass=80 fail=0。
- **剩余（记录，后续）**：ReduceMax/Min（argmax 掩码）、Slice/Pad（切割 scatter；
  Slice 反向需 attr 数组，JSON 也未分派 Slice/Pad——两者在框架里常只做形状/中间
  操作，训练价值低，留待需要时）。

---

## 2026-09 — B 训练反向补：ReduceSum/ReduceMean（broadcast 回 x）

- **背景**：B1/B2 后 loss 路径仍缺归约类（ReduceSum/ReduceMean）反向。归约反向 =
  **梯度广播回 x**（与 B2 的 scatter 相反方向）。
  - ops.myp `bwdReduce` kernel：mode0 全规约 → dy 标量广播到每 x 元素（mean
    /total）；mode1 空间 per-(n,c) → dy[n*C+c] 广播到该 (n,c) 空间段（mean /S）；
    redType 0=mean(除法)/1=sum(复制)。**ReduceMax/Min（2/3）需 argmax 掩码未实现**
    → buildReverseGraph 跳过（梯度 0 语义，同 Resize 2D）
  - graph_defs OpCode `BWD_REDUCE(91)` + opCode map + opTraits(2)；runtime
    `addBwdReduce`（opKind 97，A=dy B=dx P0=mode P1=N P2=C P3=S P4=total P5=redType）
  - **mode/redType 存 graph 的 `nRedMode_`/`nRedType_` 数组（按 node index）非
    NodeField attr** → buildReverseGraph 用 `compilerSetNRedMode/Type` 复制到 bwd
    节点；graph_compiler 用 `compilerReduceMode`/`compilerNRedType` 读回
  - graph_compiler BWD_REDUCE wiring（N/C/S 从 nodeIn1/x shape、total=N*C*S）；
    ReduceMeanOp.backward + registerFwdBwd(30→97)（GPU 前向保留）
- 验证 `infer_tests/bwd_reduce_main.myp` runtime 数值对拍（x[1,2,2,2] N=1,C=2,S=4）：
  ReduceSum mode1 dy=[10,20] → dx=[10×4,20×4]；ReduceMean mode1 dy=[4,8] →
  dx=[1×4,2×4]；ReduceSum mode0 dy=[5] → dx=[5×8]；ReduceMean mode0 dy=[3] →
  dx=[0.375×8]。CPU 全 PASS BWD REDUCE OK。全量回归 pass=79 fail=0。
- **剩余（记录，后续）**：ReduceMax/Min（argmax 掩码）、Gather（scatter，重复
  indices 需累加）、Slice/Pad（切割 scatter，Slice 反向需 attr 数组）。

---

## 2026-09 — B2 训练反向补：Expand/Tile（广播 scatter-add）

- **背景**：B1 后 loss 路径仍缺 Expand/Tile（广播/平铺复制）反向。广播/平铺是
  多对一映射 → 反向须 **scatter-add**（dx 清零 + 逐 y 累加到源）。
  - **关键洞察**：expand 与 tile 的前向源坐标规则统一——`c_k = id_k==1 ? 0 :
    y_k % id_k`（expand 时 id_k==od_k>1 故 % 等价直接映射；tile 时任意倍数）→
    共用一个 `bwdBroadcastCopy` scatter kernel（先清零 dx 再遍历 y 累加）
  - graph_defs OpCode `BWD_EXPAND(89)/BWD_TILE(90)` + opCode map + opTraits(2)；
    runtime `addBwdExpand/addBwdTile`（opKind 95/96，A=dy B=dx P0-3=id P4-7=od）；
    ops_iface ExpandOp/TileOp.backward（trainMode guard + dx<0 return）+
    registerFwdBwd(74→95)/(76→96)（GPU 前向保留，训练反向 CPU-only）
  - graph_compiler buildRuntime BWD_EXPAND/TILE：id=nodeIn1(x) shape、od=nodeIn0
    (dy=前向输出 grad) shape；buildReverseGraph Expand/Tile 分支 + dx 图输入 guard
- 验证 `infer_tests/bwd_bc_main.myp` runtime 数值对拍：Expand x[1,2,1,3]→y[1,2,2,3]
  dy=0..11 → **dx=[3,5,7,15,17,19]**；Tile x[1,1,2,3]→y[1,1,4,3] →
  **dx=[6,8,10,12,14,16]**（手算精确，kernel 清零验证）。CPU 全 PASS BWD BC OK。
  全量回归 pass=78 fail=0。
- **剩余 B2（记录，后续）**：Gather（scatter；axis 重复 indices 需累加）、Reduce 族
  （广播回 x：mean /S、sum 复制、max/min 需 argmax 掩码）、Slice/Pad（切割 scatter，
  Slice 反向需 attr 数组 + Pad 反向去边）。

---

## 2026-09 — B1 训练反向补：Reshape/Flatten/Squeeze/Transpose（纯数据重排反向）

- **背景**：buildReverseGraph 此前只覆盖 Gemm/MatMul/Conv/Relu/Sigmoid/SoftmaxCE/
  Add/Sub/Mul/Div/Concat 反向——loss 路径含 Reshape/Flatten/Squeeze/Transpose（纯
  数据重排，无参）时梯度链断。本轮补全链路（**训练反向只 CPU**：runTrain CPU-only，
  run() 对无 fwd 表的 op 落 bwdCpu_ 表调 backward，GPU 表无需注册）。
  - graph_defs OpCode `BWD_RESHAPE(87)/BWD_TRANSPOSE(88)`（紧跟 A2 84-86）+
    opCode map + opTraits(2)（bwdLike/豁免 out 空在 graph_compiler）
  - runtime `addBwdReshape`(opKind 93, A=dy B=dx)/`addBwdTranspose`(opKind 94,
    opP0-3=perm opP4=pc opP5-8=x dims)
  - ops.myp `bwdTranspose` kernel：遍历 x(id dims) 每元素，dy 坐标 = (i_{perm0..3})
    （transpose 双射 → 无需累加）；dy dims od = perm(id) 内部推。
  - **Reshape/Flatten/Squeeze 反向 = copyFlat**（框架前向即连续拷贝不重排）→
    buildReverseGraph 对三者统一产 BwdReshape 节点；Transpose 产 BwdTranspose
    （复制 perm 属性）。dx 仅当 x 是节点输出才产（图输入多节点共享同名 grad 冲突，
    同 BwdDense guard，须 endNode 后 replaceResult）
  - ops_iface_all ReshapeOp/TransposeOp.backward（trainMode==1 guard + dx<0 return）
    + registerFwdBwd(24→93)/(26→94)（GPU 前向 registerFwd 保留）
- 验证 `infer_tests/bwd_rtr_main.myp` runtime 数值对拍：Reshape fwd y==x、bwd
  dx==dy；Transpose x[1,1,2,2]=[1,2,3,4] perm[0,1,3,2]（H/W 换）→ y=[1,3,2,4]，
  dy=[10,20,30,40] → **dx=[10,30,20,40]**（手算精确）。CPU 全 PASS BWD RTR OK。
  全量回归 pass=77 fail=0。
- **范围与后续**：softmaxCE 是 2D rows/cols（FC logits）→ 训练 loss 路径末端须 2D；
  Reshape/Transpose 是 4D op 且 classifyShapes 激活 kind 沿输入（2D↔4D reshape
  kind 转换受限）→ 端到端可训网构造受限，本轮以 runtime 数值对拍做权威判据。
  剩余（B2 待办）：Expand/Tile（反向需广播归约 scatter-add）、Gather（scatter）、
  Reduce 族（广播回 x shape）、Slice/Pad。

---

## 2026-09 — A3 扩展：JSON 属性/参数 op 批量分派（Expand/Tile/Reduce 族/Squeeze/Transpose/LogSoftmax）

- **int64 参数类**（沿 A3 Reshape/Gather 模式，nodeIn1 接内存 int64 常量 +
  readI64Init 内存分支）：
  - `Expand(shape)`：沿 size-1 维 broadcast 复制到目标 shape（inferShapes 读 nodeIn1
    shape 常量；classifyShapes 已 markInt64Param）
  - `Tile(repeats)`：沿各维按倍数复制（输出 = 输入维 × repeats 维，逐元素乘）
- **属性类**（JSON int 数组内联，onnx_loader 同款 nodeInt 连续字段）：
  - `Squeeze(axes)`：去 size-1 维（axes 可选；缺省全 size-1），数据连续拷贝
  - `Transpose(perm)`：perm 属性（缺省反转维度）
  - `ReduceSum/ReduceMean/ReduceMax/ReduceMin(axes[,keepdims])`：axes → AXES0-3+
    AXES_N（含 N/C → 全规约 mode0；[2,3] → 空间规约 mode1；redType 0-3 由 op 名决定）；
    keepdims → NodeField.KEEP
  - `LogSoftmax(axis)`：并入 Softmax 的 axis 属性分支（LogSoftmax 须显式 axis）
- json_model 新增 `setIntArr` helper（int 数组 → 连续 NodeField 字段 base+k）。
- 测试 `infer_tests/json_ops2_main.myp` + `ops2.json`（**多输入 5 个** xe/xt/xr/xs/xl +
  8 层 8 输出）：Expand ch0[[1,2,3]×2]ch1[[4,5,6]×2] / Transpose [1,3,2,4]（H/W 交换）/
  ReduceSum axes[2,3]=[10,20] / ReduceMean axes[0,1]=[3.75] / ReduceMax=[8] /
  ReduceMin axes[2,3]=[1,2] / Squeeze axes[1] 连续拷贝 / Tile repeats[1,1,2,1]=
  [1..6,1..6]，全手算断言 CPU+GPU `JSON OPS2 OK`。全量回归 pass=76 fail=0。
- **MYP 坑**：MYP 无二维动态数组（`double[][]` 不存在，`new int[2][]` 编译错）——
  测试期望表用每输出独立 `double[]` + `cmpOut` 辅助比较。

---

## 2026-09 — A3 JSON int64 参数化 op（Reshape/Gather）+ C7 SLI 端到端文档

- **A3：int64 内存常量机制（参数化 op 解锁）**：此前 JSON/ONNX 权重型参数只走
  float 通道（Gemm 权重等），Reshape 的 shape / Gather 的 indices 这类 **int64
  参数**无源。新增 GraphWeights 内存 int64 常量通道：`addMemI64(name, rows, cols,
  long[] vals, n)`（dtype=7 INT64、kind=1、offset_=-3 标记 + memI64Off_/memI64Val_/
  memI64Cap_/memI64Count_ 数组，long[] 池初始 1024）+ `memI64Base(weight)` /
  `memI64ValAt(i)` 读取器；graph `readI64Init` 加内存分支（offset<0 时从
  `weights_.memI64Base(wi)` 读 long 至 i64TmpL_，kind1/3 file 分支保留）+
  `addMemI64Weight(nm, rows, cols, long[] vals, n)` 包装；json_model
  `regI64Arr(Json j, string p, string nm)`——读 JSON int64 数组 →
  addMemI64Weight + addShapeD(nm,[n]) + role SHAPE + dtype INT64。reset 重置
  memI64Count_=0。
- **A3：JSON 分派 Reshape/Gather**：buildGraph 第 2 步预注册 Reshape 的
  `out+"_shape"` 与 Gather 的 `out+"_idx"` 内联 int64 常量；第 3 步分派 Reshape
  （nodeIn slot1 接 shape 常量）+ Gather（nodeIn slot1 接 indices 常量 +
  NodeField.AXIS 属性）。
- **修复 inferShapes 顶部形状链拦截误伤数据 Gather**：此前 `t=="Gather"` 无条件
  当形状链拦截（i++;continue，out 无 shape）→ JSON 数据 Gather out noShape
  （verifyShapes fail）。改为仅当输入含 int64 参数（`i64Count(in0)>0` 或
  `i64ChainAlive(in0)==1`）才拦截。gather_main（数据 Gather）回归无损。
- **修复 buildRuntime 临时张量 off0 覆盖 bug**：readI64Init 在 buildRuntime 期间
  建临时 addTensor（Gather indices / Slice 参数等）从 arenaTop_=0 bump（plan 路径
  不 bump arenaTop_）→ **off 0 覆盖 data** → JSON Gather 输出全首通道（idx 第二
  个失效）。gather_main ONNX 此前侥幸通过（data 首两元素 -1,0 经 clamp 恰 encode
  期望 idx）。修复：runtime 加 `setBumpBase(base){ arenaTop_=base; }` +
  graph_optimizer optimize/optimizeTrain 在 buildRuntime 前 `resizeArena(peak+4096)`
  + `setBumpBase(peak)`（peak 处预留临时张量区，不再覆盖数据）。
- **C7：SLI 端到端文档（docs/sli.md）**：import dl 用法 / 编译运行 / 能力速览 /
  ONNX 推理 / JSON 模型（schema+DAG+权重源）/ 训练（Sub/Mul/Div/Add 反向）/
  checkpoint / 诊断（MYP_PROF_CPU 等）/ 语言陷阱 / 参考实现；infer/README 挂载
  指针。
- 测试：`infer_tests/json_reshape_main.myp`+`reshape.json`（[2,6]→[3,4] reshape，
  JSON RESHAPE OK）+ `infer_tests/json_gather_main.myp`+`gather.json`（4D data
  [1,3,2,2]，Gather axis=1 indices=[0,2]，期望 o=[0.1*4, 0.3*4] 段，JSON GATHER
  OK CPU+GPU）。全量回归 pass=75 fail=0（含新两个 *_main）。

---

## 2026-09-02 — A1 2D MatMul 修复 + A2 Sub/Mul/Div 训练反向（json_train_submul/bwd_rt）

- **A1：修复 2D MatMul**（此前 JSON 2D MatMul [1,4]×[4,3] 布局错乱 sz=16）。根因：
  inferShapes/graph_compiler 的 MatMul 恒判 rank≥3（`ar=4`）→ 一律走 4D batch
  广播。修复：双方均为 2D 视图（非 5D 且 d2/d3≤1）→ 普通 2D matmul（`addMatmul`/
  `[d0(a),d1(b)]`），任一方含 batch/空间维 → BatchMatMul。branch.json 恢复
  `m=a2·W([4,3])` 断言 [40,40,40]。bmm/opselect/multiio 4D batch 回归无损。
- **A2：Sub/Mul/Div 训练反向补齐**（此前仅推理）。全链路：
  - graph_defs OpCode `BWD_SUB(84)/BWD_MUL(85)/BWD_DIV(86)` + opCode map + opTraits
    保护（trainOnly）；runtime `addBwdSub/Mul/Div`（bwd opKind 90/91/92，
    opA=dy,opB=x0,opC=x1,opD=dx0,opP0=dx1）；ops.myp `bwdSub/bwdMul/bwdDiv`
    kernel（同尺寸线性：Sub dx0=dy/dx1=-dy；Mul dx0=dy*x1/dx1=dy*x0；Div
    dx0=dy/x1/dx1=-dy*x0/(x1²)）；SubOp/DivOp/MulOp.backward 填充 + 注册改
    registerFwdBwd(31→90/32→92/33→91)；buildReverseGraph 加 Sub/Mul/Div →
    Bwd 节点生成（带前向 x0/x1 作操作数）。
  - 测试 `bwd_rt_main.myp`（runtime 直接对拍三反向解析梯度，BWD RT OK）+
    `json_train_submul_main.myp`（train_sub.json Sub 链 + train_mul.json Mul 链
    各 SGD 200 步 loss 显著降：sub 2.16→0.47 / mul 1.28→0.158，证明梯度经
    BwdRelu/BwdDense 回传更新权重）。测试数 71→73。
- **修复 BwdDense 共享图输入 fan-out def-use 冲突（MYP_IR_VERIFY 训练图回归阻塞）**：
  图输入 x 被多 Gemm 消费时，各 BwdDense 都产出 grad(x) → 多节点输出同名 value →
  `verifyDefUse`/`topoSort(reverse)` fail（json_train_submul RUNTIME FAIL）。
  修复：① buildReverseGraph Gemm/MatMul 的 BwdDense dx（slot0）当输入非节点输出
  （=图输入，无上游需回传）时清空——**须在 endNode() 后**（count 在 end 才 +1，
  replaceResult 的 guard `node>=count` 会静默丢弃，曾导致修复无效）；② ops.myp
  `bwdDense` dx 段加 `dxOff>=0` guard（dx=-1 不写）；③ graph_compiler wire 检查
  豁免反向节点 out（slot0 可合法为空）。修复后 json_train_submul MYP_IR_VERIFY
  通过（Sub/Mul 链训降不变）。
- **修复 Add forward 2D FC dims bug（Add 网 loss 恒 ln4 根因，MYP_PROF_CPU 定位）**：
  loss 路径含 `Add` 汇合（logits=Add(…)）训练 loss 恒 ln4——排查确认 BwdAdd/
  反向/Update 全执行 + 权重/logits 变化但 loss() 恒 → 再用 `prob`（图输出）dump
  发现 **前向 logits 恒全等（softmax uniform）**，且 loadJson 推理同现（非训练
  问题）。根因：`AddOp.forward` 输出 4D dims 从 `tensorN/C/H/W(oc)` 读——FC 2D
  输出 tensor 这些为 **0** → add kernel 同尺寸快路径条件（阶段三防广播误判收紧
  的 `an==on && ac==oc ...`）失败 → 掉 4D 通用 fallback → 0 维解码恒出首元素和
  → 输出全等。branch.json 的 Add "对" 是假象（const 全等输入下首元素和=真值）。
  **修复**：AddOp.forward 输出 dims 改从 opP（a/b dims）逐维 max（不读 tensorN）。
  修后 Add 网训练 loss 1.139→0.417（prob 非 uniform），推理 prob 正确。**Add
  训练反向本就可工作——阻塞在 forward dims**。多分支 JSON 训练（Add 汇合）现可用。

---

## 2026-09-02 — JSON 多分支/更多算子（fan-in·fan-out DAG + op 扩展，json_branch_main.myp）

- **多分支语义（不引入新语法）**：层式 JSON 即 **DAG**——fan-out 靠名字引用
  （一个 out 名被多个后续层 in），fan-in 汇合用多输入槽 `in2/in3/in4`
  （nodeIn slot1..3，沿用 ONNX 输入槽序）。原有单输入 `in`/层链格式完全兼容。
- **更多算子（json_model buildGraph 分派扩展，全部零 int64 常量依赖）**：
  - 单输入：`Sigmoid` / `GlobalAveragePool` / `Flatten`(axis)
  - 二元 fan-in：`Sub`/`Div`/`Mul`（与 Add 同款 numpy 广播，in+in2）
  - 多元 fan-in：`Concat`（in+in2/in3 + axis 属性；框架 Concat axis=NodeField.AXIS）
  - `MatMul`（in+in2 或可选 `W`{dims=[K,N]} 权重作第二输入）
  - 顺带修复 V1 缺口：JSON `MaxPool`/`AveragePool` 此前未设 kernel/strides/pads
    属性 → 现 setKernelAttrs。
- 测试 `infer_tests/json_branch_main.myp` + `branch.json`：双分支分叉
  （x→Gemm const2/const3→Relu 得 a2=[10]*4、b2=[15]*4）→ fan-in 汇合
  Sub/Mul/Div/Add/Concat + 4D 图算子 GAP/Sigmoid/Flatten（img[1,2,2,2]），
  12 op / 11 输出全手算断言（含 flat=[1,2,3,4,2,4,6,8]、gap=[2.5,5]）。
  CPU+GPU JSON BRANCH OK。测试数 70→71。
- **已知限制（已注明文档）**：2D `MatMul`（[1,4]×[4,3]）在框架走 4D batch 路径
  （inferShapes MatMul 无条件 rank≥3 → batch 广播），值/形状错——2D MatMul 须以
  4D batch 形状声明（与 ONNX BatchMatMul 同）；本次 demo 未含。Sub/Div/Mul 等
  新算子**训练反向未补**（buildReverseGraph 仅 Gemm/MatMul/Relu/Sigmoid/Add/Conv/
  Pool/Concat 有 Bwd，Sub/Div/Mul 输入梯度不产出 → 仅推理）——训练反向为后续项。

---

## 2026-09-02 — JSON 权重 safetensors 源 + 无 bias Gemm 框架修复（json_safe_main.myp）

- JSON 层权重 `W`/`B` 除 `init` 外支持 **`safetensors` 权重源**：`"W": {"dims":[...],
  "safetensors": {"file":"<.safetensors 路径>", "tensor":"<张量名>"}}`——JsonModelLoader
  registerWeight 按 JSON 张量名自动从 .safetensors 读值装配进内存权重通道（filled=1
  跳过 init 块），**替代手写 readF32Into/手动布局**（safetensors 本有名字段，JSON
  声明即得权重，无手写装配代码）。
- 测试 `infer_tests/json_safe_main.myp` + `safe_gemm.json`（单 Gemm data[1,4]→out[1,4]，
  W=deeplearning/data/onnx/test_w.safetensors 张量 w1=1..16，**无 bias**）：
  out=w1·[0.5,1,1.5,2]=[15,35,55,75] vs numpy（test_w_ort.bin）max diff=0，CPU+GPU
  JSON SAFE OK。测试数 69→70。
- **修复框架无 bias Gemm（隔离出的老缺口，非 safetensors 问题）**：JSON/ONNX 合法无
  bias Gemm（B 可选槽省略）此前**必段错误 139**（init/xavier 权重源同样崩 → 隔离到
  "无 bias Gemm 图本身 run 崩"）。根因：graph_compiler GEMM 分支无 B 时
  `addDense(a, w, -1, out)`（bias tid=-1），DenseOp.forward `tensorOff(-1)` 无 guard
  越界读 tOff_ 数组 → bOff 垃圾 → dense `arena[bOff+i]` 越界读（gdb: DenseOp_forward
  SIGSEGV；对照 +bias zeros 即不崩、数值精确）。**修复**：① runtime `tensorOff`
  guard `tid<0 → -1`（全局防御）；② CPU `InferOps.dense` + GPU `GpuInferOps.dense`
  @gpu 内核改 `sum=0.0; if (bOff>=0) sum=arena[bOff+i]`（同 conv 既有的 bias-free
  guard）；③ 反向 `bwdDense`（CPU+GPU）db 归约 `dbOff>=0` 才写（无 bias Gemm 训练）。
  denseTiled/cuBLAS 路径本就 guard 过 bOff=-1。
- MYP 坑：bwdDense db guard 若用 `if (dbOff >= 0) { int i = 0; ... }` 引入块作用域，
  后续 dW 段复用 `i = 0`（无新声明）报 `undefined symbol 'i'`——guard 须放循环内
  写处（`if (dbOff >= 0) arena[...] = s;`），保持循环变量函数级。

---

## 2026-09-02 — 声明式 JSON 模型（json_model.myp）—— 不经过 ONNX 的图构建新入口

- 用户写层式 JSON（op/输入输出/权重 init）→ `Session.loadJson/loadJsonTrain`
  直接填框架 Graph（beginNode/addMemWeight/addShapeD…）→ 复用 optimize/
  optimizeTrain（训练自动补 label/loss + 反向）——ONNX 只是"填 Graph 的一种源"，
  JSON 是第二种源，**用户全程看不到 ONNX**。
- **GraphWeights 内存权重通道**：值存 `memVal_`（offset=-2 标记），`writeWeight`
  内存分支直写 arena（跳过 file_/BN/NHWC——JSON 干净权重）；去重等 file_ 依赖 pass
  对内存权重走 offset<0 跳过逻辑。
- `infer/json_model.myp` JsonModelLoader：JSON 导航（stdlib json）+ 层→节点构建
  （V1 op：Gemm/Relu/Add/Conv/ConvTranspose/MaxPool/AveragePool/Softmax + kernel/
  strides/pads/dilations/group/transB/axis 属性）+ 权重 init（xavier/zeros/ones/
  const/gauss，确定性 LCG seed）+ rows/cols 视图 + 图输入/输出登记。
- `Session` 加 `loadJson/loadJsonTrain`（jsonMode 分派 tensorId/tensorSize/输入输出
  枚举/run/loss/gradId/错误码/dump——JsonModelLoader 提供同批 getter 镜像）。
- 测试 `infer_tests/json_model_main.myp` + `mlp.json`：JSON 定义 MLP（Gemm/Relu/
  Softmax + xavier）——推理 prob softmax 归一 sum=1（CPU+GPU）+ 训练 loss
  2.43→0.12（自动补 label/loss 收敛）。JSON MODEL OK。测试数 68→69。
- 边界：V1 op 集 + 层式单输入层链；复杂拓扑/控制流/动态形状后续扩展。

---

## 2026-09-02 — 阶段四：AMP 数值管线骨架（fp16 梯度舍入模拟 + 训练鲁棒性验证）

- `Session.setAmpSim(1)`（runtime ampSim_）——更新前梯度经 fp16 舍入（模拟混合
  精度训练：主权重 fp32 累积、梯度 fp16 传输/计算）。ops.myp 新增纯位运算
  `f2hRNE`（f32→fp16 round-to-nearest-even，含亚正规/溢出/NaN，与 numpy float16
  逐位对拍 14 值全等）+ `halfToF32BitsL` + `halfQuantizeGrad`（梯度就地
  f32→f16→f32）；UpdateOp.backward 在 ampSim 时对每步梯度量化（累积模式在累积前）。
- 新增 `infer_tests/sli_amp_main.myp`：fp32 vs fp16 梯度模拟同一序列 200 步都显著
  收敛（loss 0.027）；首步 W1 权重相对差 2.6e-7（mnist 梯度对 fp16 高度鲁棒）。
  CPU+GPU `SLI AMP OK`。测试数 67→68。
- **MYP 教训**：类内 static 方法互调（`halfQuantizeGrad` 内裸名 `f2hRNE`）codegen
  成**顶层裸符号 @f2hRNE**（无类前缀）→ opt "undefined value"——须用全名
  `InferOps.f2hRNE`。方法名避免 `f32*` 前缀（非根因，但 f2hRNE 命名更稳）。
- **标注**：这是 AMP 数值管线骨架（CPU fp32 上模拟 fp16 舍入）。真计算级 AMP
  （fp16 kernel + 图内 loss-scale + 动态 scale + 溢出回退）为后续大工程。

---

## 2026-09-02 — 阶段四：梯度累积（Session.setGradAccumEvery，micro-batch 累积）

- runtime 加梯度累积：`setGradAccumEvery(K)/gradAccumEvery()/gradAccumStep()/`
  `gradAccOff_[]`——K>0 时每 K 次 runTrain（micro-step）才应用一次权重更新。
  启用时遍历 Update(55) 权重在 arena 尾（optBase_ 增长）预分配 gAcc 缓冲；
  run()/runGpu() 训练入口 micro-step 计数（gradAccStep_++）；UpdateOp.backward
  累积 `gAcc += g`，达 K 应用 `gAcc/K × lr`（等效 batch×K 且 loss 平均）+ 清空。
  注：GPU 训练（GpuUpdateOp）不累积（每步照常 update），梯度累积限 CPU runTrain。
- Session 暴露 `setGradAccumEvery(k)/gradAccumEvery()/gradAccumStep()`。
- 新增 `infer_tests/sli_acc_main.myp` 三重验证：A) K=1 vs 不累积同一序列 loss
  逐位 maxdiff=0（退化正确）；B) K=4 更新节奏——micro 1..3 后 W1 权重不变
  （只累积），第 4 micro 才应用变化；C) K=4 收敛——100 micro（25 应用）loss
  2.62→显著降。CPU+GPU（runTrain）均 `SLI ACC OK`。测试数 66→67。

---

## 2026-09-02 — 阶段九 SLI：优化器暴露 + 收敛验证（SGD/动量/AdamW + weight decay）

- `Session` 补 `setOptimizer(m)/optimizer()`（0=SGD 默认，1=动量，2=AdamW）与
  `setWeightDecay(wd)/weightDecay()`——runtime 阶段 5e 优化器机制（buildTrain 每
  权重 prepOptState 预留 2n 状态区；UpdateOp 按 optMode 走 updateSGD/updateMom/
  updateAdamW）此前从未经 facade 暴露/测试。
- 新增 `infer_tests/sli_opt_main.myp`：mnist_mlp 训练模式同一确定性 class 数据
  序列分别 SGD/动量/AdamW 各 250 步——三者 loss 均显著收敛（SGD 3.92→0.021、
  动量 3.92→0.0008、AdamW 3.92→0.0028）；AdamW ±wd=0.01 对拍权重范数受约束
  （559.9→533.1）。CPU+GPU 均 `SLI OPT OK`。测试数 65→66。
- 陷阱：Console.writeFloat 逐 write 换行 + 精度低，收敛量级差异用 int(L*1000)
  量化；动量可收敛到 loss~0.0008（量化后 0）——判据不得要求 loss 量化值 >0。

---

## 2026-09-02 — 阶段七：多输入/多输出/可选输入完整处理（系统验证固化）

- 新增 `tools/make_multiio_onnx.py`（opset13）：2 输入不同形状（x1[1,3,8,8]
  Conv + x2[1,4] Gemm）+ 2 输出不同大小（y1=256、y2=3）+ Conv 无 bias
  （B 可选输入槽省略）——多 IO 组合场景 + ORT 参考。
- 新增 `infer_tests/multiio_main.myp`（Session/import dl 路径）：inputCount/
  outputCount/inputName/outputName 枚举（x1/x2→y1/y2）、多输入按名注入
  （loadInputFromFile）、多输出各自 getOutput vs ORT（y1 max diff 3.6e-7、
  y2 2.4e-7），CPU+GPU 均 `MULTIIO ALL OK`。
- **结论**：框架多输入/多输出/可选输入（无 bias Conv 空槽）此前经 bmm（4 输入
  2 输出）隐式覆盖，本轮以不同形状/大小 + 无 bias Conv 组合系统固化；无框架
  改动（直接通过）。测试数 64→65。阶段七全部待办清零。

---

## 2026-09-02 — 阶段九 SLI：训练完整循环 demo（真实收敛 + checkpoint 续训）

- 新增 `infer_tests/sli_fit_main.myp`——Session 训练路径端到端演示：带 class 特征
  信号数据（784 维中 10 类特征位，确定性伪随机）经 SGD（lr=0.002）250 步 runTrain，
  loss 单调显著下降（882→205→116，即 0.88→0.21→0.12，@50/150/250）——证明 bwd
  梯度 + Update 权重更新都真实生效；@250 dumpPlan → 新 Session loadPlan 续训 100
  步 loss 继续降（82<116，同 lr 轨迹一致）。CPU+GPU 均 `SLI FIT OK`。测试数 63→64。
- **修正认知（非 bug）**：早期 demo 误判 "loss 骤 0" 为回归——实测恒等/单类数据 +
  足够大 lr 时 mnist_mlp 数步内即过拟合（loss 5.68→4e-5，B/C probe），**"连续
  runTrain 第 2 次起 loss=0" 是真实快速收敛而非训练 bug**。loss 是否下降的正确判据
  需数据带不可完全拟合的信号（多 class 轮换 + 适当 lr），否则 loss 早停于 ~0 无法
  展示下降。demo 采样点须避开已收敛区。
- **训练数据约定再确认**：mnist_mlp label 是 one-hot[10]（softmaxCE 依赖
  label>0.5），非标量 class id；每次 run 前重建输入（arena 复用覆盖）。

---

## 2026-09-02 — 阶段九 SLI：`import dl` 单模块入口（统一 Standard Library Interface 包）

- 新增 `dl/dl.myp` 薄转发入口模块——用户程序只需 `import dl;` 即得统一
  Session facade（推理/训练/checkpoint/dump 全能力），不再手动 import
  pb/runtime/graph/onnx_loader 拼装样板。MYP import 是 AST 合并，framework.myp
  顶层 `Session` 经包入口直接可见。
- **机制**：`--package-path examples/deeplearning` 下 `import dl` 按包解析规则命中
  `<package-path>/dl/dl.myp`（MYP import 无路径搜索：stdlib → source 相对 → 
  package-path `<seg>/<module>/<module>.myp`）。回归脚本全量统一加
  `--package-path examples/deeplearning`（对现有相对/stdlib import 无副作用——
  package-path 仅在该规则命中时生效）。
- `Session` 补 `tensorCount()`（转发 rt.tensorCountDebug，dump/调试用）。
- 测试 `infer_tests/sli_dl_main.myp`：`import dl;` 端到端——deadweight
  load/runAuto/getOutput vs ORT max diff 2.4e-7 + tensorCount>0，CPU+GPU
  `SLI DL OK`。测试数 62→63。

---

## 2026-09-02 — 阶段九 SLI：训练 checkpoint（Session dumpPlan/loadPlan）+ 确定性回归

- `Session` 暴露 `dumpPlan(path)`/`loadPlan(path)`（复用阶段七计划缓存）——训练
  中途 dump 当前 runtime（op 表 + arena 权重），loadPlan 恢复跳过 ONNX 解析直接续跑。
  **注意**：loadPlan 恢复的 Session 无 OnnxLoader/graph → 按名 `tensorId()`/`loss()`
  返回 -1/0（此前空 loader 会崩，见下健壮性修复），训练续跑须用 dump 前记录的
  tid 经 `getFlat`/`setFlat` 访问。
- 测试 `infer_tests/sli_ckpt_main.myp`（确定性）：mnist_mlp 训练 lr=0（loss 照算、
  权重不变）→ run1 loss 稳定 → dumpPlan → 新 Session loadPlan → 权重逐位一致
  （W1/W2 a=b）→ a/b 各续跑 loss 一致（11.1062）——SLI CKPT OK，CPU+GPU。
- **健壮性修复**：`OnnxLoader` @constructor 即建 `g_`（Graph）——此前 g_ 在
  loadCommon 才 new，loadPlan 恢复会话未走 loadCommon → g_=null，按名
  `tensorId()`/`loss()` 解引用崩溃；修复后未 load 时 tensorId 安全返回 -1。
- **教训（训练输入约定）**：mnist 训练 label 是 one-hot[10]（grad_check 写 10 位置
  class=1，非标量）——softmaxCE 依赖 `label>0.5` 才累计 loss，写错 label 会导致
  loss 异常/0；训练 run 间 arena 会复用覆盖输入区，**每次 run 前须重建 data+label**。
- 阶段九待办推进：Checkpoint（训练持久化/续训）✅（复用 dumpPlan/loadPlan）；
  测试数 61→62。

---

## 2026-09-02 — 修复 ResNet18 数值回归（常量去重误合并 BN 折叠 bias）

- **现象**：ResNet18 推理 sum=1331.47（或注入 batch 后 0.008）vs ORT 0.101261，
  top-1 matchstick(644) vs ORT lakeside(975)。ResNet50 336.658 正常。
- **根因**：无 bias Conv 经 `fuseConvBN` 生成 `#bnb` 折叠 bias（G4，"无源字节仅用于
  count"——内容在 writeWeight 时按各自 BN_NODE 的 scale/bias/mean/var 合成）。阶段五
  常量去重在 writeWeight **之前**比较字节——`#bnb` 全 0 相同 → 全部合并到第一个 conv
  的 `#bnb` → 每 stage 所有 conv 共享同一 bias（dump opC 全指 conv0_fwd#bnb）→ 输出错。
  ResNet50 conv 自带 bias（折叠写回原张量，不生成 #bnb）故不受影响。
- **修复**：`eliminateDeadNodes` 的 dedup 跳过 `BN()==1`/`BN_ONLY()==1` 张量（内容由
  BN 参数合成，源字节不代表最终折叠内容，不参与内容去重）。
- **配套**：r18_main/passverify 加载 ResNet18 前 `setInputShape("data",1,3,224,224,1,0)`
  注入动态 batch（data 是 dim_param 'N'，不注入 d0=0 → runtime batch 0）。
- **排查工具教训**：run 全跑后读中间层会被内存复用覆盖（relu0 区域被 stage1_relu0
  复用）→ 逐层对拍须 `MYP_NO_REUSE=1`；ORT 提取中间层用
  `del m.graph.output[:]` + `make_tensor_value_info` 替换 graph 输出。
- **结果**：ResNet18 sum=0.101238（= ORT 0.101261，2e-5 浮点差），top-1 lakeside
  (15.1839) 与 ORT 一致，CPU+GPU。

---

## 2026-09-02 — 阶段九：统一 Standard Library Interface（SLI）facade 起步

- 新增 `infer/framework.myp`：`Session` 单一入口类——用户程序从「手动 import
  pb/runtime/onnx_loader + 20 行样板」降为「import framework + Session」：
  `load/loadTrain/loadMmap`（三阶段封装）→ `setInput(name,buf,n)` /
  `loadInputFromFile(name,f32)` → `run()/runGpu()/runAuto()`（MYP_GPU=1 自动 GPU）→
  `getOutput(name)`；统一错误码与统计（phase/loadError/compileError/runError/
  compileMs/lastRunMs/lastRunOps）与输入/输出名枚举（inputName/outputName）。
- **教训 1（MYP 类成员）**：成员必须在 class 尾部 `property:` 区声明，写在
  `action:` 前会报 "cannot access member of non-class type 'void'"。
- **教训 2（tensorSize 怪癖）**：`rt.tensorSize(tid)` 对 CNN 张量返回 0——resnet18
  `data` 的 `tRows_=d0*d1` 中 `d1` 被记为 0（`shapeElementCount` 动态 0 按 1 处理
  故正确）。Session 用 `loader_.shapeElementCount(name)` 计算张量长度，勿用
  `rt.tensorSize`。
- 测试 `infer_tests/sli_main.myp`：deadweight vs ORT max diff 2.4e-7（数值对拍）
  + 真实 ResNet18 编译成功（phase=3 ops=34）——SLI ALL OK，CPU+GPU。
- **dump 命令**：`Session.dumpGraph()`（图输入/输出 + 每活节点 op/输入/关键属性——
  Conv/Pool 的 k/s/p/g/d、Gemm transB、BN eps、Resize scale、Reduce axes、Split/
  Pad/Softmax axis 等按 op 类型分派）+ `Session.dumpIR()`（IR 节点表 + 张量表
  [dims/kind/dtype/role/NHWC/r5/DEAD] + planOrder[带节点类型] + runtime op 完整
  展开[opKind 名 + A/B/C/D 张量名及运行时 2D 维度 + P0..P7 参数 + relu 标记]）
  + `Session.dumpMem()`（内存规划：每非 dead 张量 arena 偏移/元素数/producer/
  lastUse + 峰值——ResNet18 全部激活共享首块 802816 元素区，复用清晰可见）。
  Graph/OnnxLoader 加 `tensorName(tid)` bridge（runtime tid → 张量名）。ResNet18
  dump：69 原始节点 → 34 runtime 算子，BN 参数 DEAD、`#bnb` 折叠权重、Conv+残差
  Add、GAP+Flatten 融合在 dump 中一目了然。
- **训练接口（Session 统一）**：`loadTrain` 后 `setLr/lr/setTrainMode/trainMode/
  runTrain`（前向+反向+更新）+ `loss()`（读 "loss" 标量）+ `gradId(w)`（读 "W#g"
  解析梯度张量 id）。`infer_tests/sli_train_main.myp` 回归：mnist_mlp 训练模式
  12 ops，loss=11.1062，SLI TRAIN OK（CPU+GPU MYP_IR_VERIFY=1，训练反向图 wiring
  校验覆盖）。测试数 60→61。
- ResNet18 数值回归已修复（常量去重误合并 BN 折叠 bias，见顶部条目）；SLI 测试对
  r18 只验编译，r18_main 现输出 0.101238 vs ORT 0.101261。

---

## 2026-09-02 — 阶段八：IR 完整性 verifier 收尾（verifyShapes/verifyRuntimeWiring）

- `Graph` 新增 `verifyShapes`（topoSort 后）：每个活节点输入不得引用已死的**数据**
  张量、有效输出必有已建立 shape 且非 dead；`verifyRuntimeWiring`（buildRuntime 后）：
  runtime op>=1、每个非 dead 张量都被注册、活节点引用的非 dead 输入无悬空、
  tensorCount>=IR 非 dead 张量数。挂接 `MYP_IR_VERIFY=1`（topoSort + optimize/
  optimizeTrain）。
- **教训 1（verifyShapes 误报）**：Resize `roi/scales`、Gather `indices`、Slice
  `starts` 等形状/参数初始器被 `markParamDead`/`markInt64Param` 标 dead 是合法的
  （从 file_ 直读，不占 runtime 张量）——verifyShapes 对输入 dead 检查须按
  role（SHAPE/PARAM）或 dtype（INT64）豁免，否则 10 个测试误报 fail。
- **教训 2（verifyRuntimeWiring 误报）**：Gather/Squeeze 的 `readI64Init` 会用
  `rt.addTensor` 建 int64→float 临时索引张量（不在 IR shape 表）——tensorCount
  计数检查用 `>=` 而非 `==`。
- 阶段八五个校验项（verifyIR/verifyDefUse/verifyTopo/verifyShapes/
  verifyRuntimeWiring）全部显式化；regression 59/59。

---

## 2026-09-02 — 阶段八：IR 完整性 verifier（verifyDefUse/verifyTopo）+ pass 等价性测试

- `Graph` 新增 `verifyDefUse`（每个活节点输出 value 的 duProducer 恰为该节点——
  用 `nodeOutputValue` 与 rebuildDefUse 同口径，避免 fused 节点 effectiveOut 歧义）
  与 `verifyTopo`（planOrder 中 producer 必须先于所有消费者，无逆边）。
- 挂接：`topoSort` 前在 `MYP_IR_VERIFY=1` 下依次 verifyIR → verifyDefUse；topoSort
  后 verifyTopo。新增 bridge `compilerVerifyDefUse`/`compilerVerifyTopo`。
- **教训**：verifyDefUse 遍历输出槽须用 `nodeOutputValue(node,slot)`（同 rebuildDefUse
  口径），不能用 `nodes_.outputAt(node,slot)`——fused 节点原始输出被 tombstone、
  effectiveOut 指向融合目标，两者 value id 不同导致误报 prod=-1。
- 回归：`infer_tests/passverify_main.myp`（Conv + 真实 ResNet18 + 动态 batch 三模型
  在 MYP_IR_VERIFY=1 下全管线通过，输出 vs ORT 2.4e-7，PASSVERIFY ALL OK，CPU+GPU）。
- 阶段八推进：pass 等价性（优化前后数值不变 + 管线 IR 完整）与四层测试中的
  IR 单元 / CPU-GPU 对拍 / 真实 ONNX 层已由既有 58 测试 + passverify 覆盖。

---

## 2026-09-02 — 阶段七：三阶段错误码与统计信息分离

- `OnnxLoader` 增加三阶段错误码与统计：`phase()`（0=未加载 1=加载中 2=编译中
  3=已就绪）+ `loadError()`/`compileError()`/`runError()` 各阶段独立错误码——
  加载：1=文件读失败 2=文件空 3=ONNX 解析失败 4=unsupported op；编译：1=pass
  管线失败 2=buildRuntime 无算子；执行：0=无错。
- 统计 getter：`compileMs()`（parseModel+optimize 耗时）/ `lastRunMs()`/
  `lastRunOps()`；新增 `runRuntime`/`runGpuRuntime` 执行包装（记录耗时/算子数
  + 成功返回 1）。
- `loadCommon` 各失败点设置阶段错误码（替代原只有 load() 返回 0/1 的模糊语义）。
- 回归：`infer_tests/phase_main.myp`——PHASE ALL OK，CPU+GPU：不存在文件
  loadErr=1、FakeOp 模型 loadErr=4、known-good phase=3 且 loadErr/compileErr=0、
  runRuntime 统计正确（ops=1 runErr=0，输出 vs ORT 2.4e-7）。
- 阶段七全部完成：opset/version、unsupported 诊断、外置 weight、模型 mmap、
  计划缓存、shape specialization、三阶段错误码分离。

---

## 2026-09-02 — 阶段七：shape specialization 多个 shape bucket 验证

- 验证编译期 shape specialization 对**多个 shape bucket** 的正确性：同一动态
  batch 模型（`dynbatch_test.onnx`，x[N,1,5,5]→Conv→y[N,1,3,3]）分别注入
  batch=2/4/8 各 build 一个 InferenceRuntime，输出各自 vs ORT 参考（batch
  维广播到注入值，inferShapes/arena 规划按 bucket 独立）。
- 回归：`infer_tests/shapebucket_main.myp`——SHAPEBUCKET ALL OK，batch
  2/4/8 三 bucket CPU+GPU max diff <9.6e-7 vs ORT（输入/参考由 python 按
  bucket 生成 `dynbucket_in_<B>.f32`/`dynbucket_ort_<B>.bin`）。
- 阶段七已完：opset/version、unsupported 诊断、外置 weight、模型 mmap、
  计划缓存、shape specialization（多 bucket）。剩余：三阶段错误码分离、
  If/Loop/Scan 子图（已声明不支持）、多输入/输出/可选输入（已覆盖）。

---

## 2026-09-02 — 阶段七：优化后 IR/计划缓存（dumpPlan/loadPlan）

- `InferenceRuntime` 新增 `dumpPlan(path)` / `loadPlan(path)`：把 buildRuntime
  后的执行计划序列化到磁盘——op 表（kind/A-D/P0-8/X0-3/Relu/Cval）+ tensor 表
  （rows/cols/N/C/D/H/W/off）+ Slice 参数表（sCnt/sAx/sSt/sSp/sOd/sId）+
  arena 权重区。重复加载同一模型 loadPlan 直接恢复，跳过 ONNX 解析 + 优化
  pass + buildRuntime（大模型从秒级降到毫秒级）。
- **关键坑**：ONNX 路径用 `addTensorPlanned*`（内存规划，不 bump `arenaTop_`），
  dump 时 `arenaTop_` 为 0 → 权重区未写入 → 恢复全 0。修复：序列化整个
  `arenaCap_ - statsSize()` 数据区（= 规划峰值，权重/常量全在其中），
  loadPlan `resizeArena(dataLen)` 恢复。
- 二进制格式：magic "MYPP" + version + tensorCount/opCount/dataLen + 各表 +
  arena float（小端）。
- 回归：`infer_tests/plan_main.myp`（Conv 单 op：normal vs plan max diff 0，
  PLAN CACHE ALL OK，CPU+GPU）+ `infer_tests/planr18_main.myp`（真实 ResNet18
  47MB 69-node 图：34 op/61 tensor 一致，sum diff 0，PLANR18 ALL OK，CPU+GPU）。
- 阶段七已完：opset/version、unsupported 诊断、外置 weight、模型 mmap、计划缓存。
  剩余：shape specialization、三阶段错误码分离、If/Loop/Scan 子图（已声明不支持）、
  多输入/输出/可选输入（已覆盖）。

---

## 2026-09-02 — 阶段七：模型 mmap 零拷贝加载

- **字节源双模式抽象**：`PbReader` 加 `initMmap(MmapFile)` + `byteAt(p)`（mmap/
  int[] 双源，readVarint/readString/readU32/readU64 统一走 byteAt）；`Graph` 加
  `setFileMmap(MmapFile)`/`setFileAppend(int[] f, int extLen)`/`byteAt(k)`，
  全部 `file_[k]` 直接字节访问替换为 `byteAt(k)`（writeWeight/f32At/
  readF32Init/readI64Init/i64RegConstant/compilerWeightBytesEqual/
  compilerFileByte）——mmap 主文件 + external data 追加区双段，逻辑偏移
  = mmapLen_+本地。
- `OnnxLoader.loadMmap`/`loadTrainMmap`：mmap 打开主模型（零拷贝，避免
  readFile 的 int[] 全量拷贝 4x 内存膨胀）；external data 追加区仍走 file_，
  appendExternalBytes 返回逻辑偏移（mmap 基）+ 刷新 Graph 追加区引用。
- 验证：`infer_tests/mmap_main.myp`（deadweight + extdata 模型经 loadMmap
  CPU+GPU max diff <4.8e-7 vs ORT，MMAP ALL OK）；**真实 ResNet50 102MB 经
  loadMmap 输出 sum 336.658 精确保持**（resnet_main 加 `MYP_LOAD_MMAP=1`
  opt-in 开关，默认仍走 load()）。
- 阶段七已完：opset/version、unsupported 诊断、外置 weight、模型 mmap。
  剩余：IR/计划缓存、shape specialization、三阶段错误码分离、If/Loop/Scan
  子图（已明确声明不支持）、多输入/输出/可选输入（已覆盖）。

---

## 2026-09-02 — 阶段七：ONNX external data 外置权重读取

- onnx_loader 支持 `data_location=EXTERNAL` 权重：parseTensor 解析
  `external_data`（field 13，StringStringEntryProto location/offset/length）+
  `data_location`（field 14）；外部文件字节追加到 file_ 扩展区，payload 按
  external offset/length 定位（单文件多张量正确区分）。
- 新增 `appendExternalBytes`（读外部文件追加、file_ 增长缓冲，`cap_` 提升为
  property 与 readFile 共享）+ `parseExternalData`（key/value 跨迭代保留——
  修复 StringStringEntryProto key=1/value=2 分次读到的覆盖 bug）+ `atoi10`
  （ASCII 十进制 string→int）+ `modelDir_`（Path.dirname 计算相对路径基）。
- `g_.setFile` 在 append 后刷新（数组重分配时 Graph 的 file_ 引用需同步）。
- 回归：`infer_tests/extdata_main.myp`（Conv w[1,1,3,3]/b[1] 外置到
  `extdata_test.bin`，w=offset0,len36 / b=offset36,len4）——EXTDATA ALL OK，
  CPU+GPU max diff 4.8e-7 vs ORT。生成器 `tools/make_extdata_onnx.py`（手动
  set_external_data + SerializeToString 落盘——onnx 1.22 `save_as_external_data`
  会把 external 写回内联，`convert_model_to_external_data` 的
  `write_external_data_tensors` 有路径解析问题，故完全手动写 bin + proto）。
- 阶段七已完：opset/version 检查、unsupported op 诊断、外置 weight。剩余：
  多输入/多输出/可选输入完整处理（多输入多输出已覆盖）、If/Loop/Scan 子图
  （已明确声明不支持）、模型 mmap/分块读取、IR/计划缓存、shape specialization、
  三阶段错误码分离。

---

## 2026-09-02 — 阶段七：Unsupported op 诊断完善（原因分类 + 输出列表）

- onnx_loader unsupported 诊断输出补全：节点名/op/输入/输出 + 原因分类——
  If/Loop/Scan 控制流子图 → `control-flow subgraph op not supported`（明确
  声明不支持）；其他 → `operator not implemented`。
- 回归：`infer_tests/unsup_main.myp`（If 控制流模型 + FakeOp 普通未知模型）——
  UNSUP ALL OK，If/FakeOp load=0（badOp_ 中止）+ 诊断含原因/输入/输出，
  known-good（opsetcheck v13）load=1 不误伤。
- 阶段七已完：opset/version 检查、unsupported op 诊断。剩余：多输入/多输出/
  可选输入完整处理（多输入多输出已覆盖）、If/Loop/Scan 子图（已明确声明不支持）、
  外置 weight/mmap、IR/计划缓存、shape specialization、三阶段错误码分离。

---

## 2026-09-02 — 阶段七：ONNX opset/version 检查

- onnx_loader `parseModel` 新增解析 `ir_version`（ModelProto field 1）与
  `opset_import`（field 8，OperatorSetIdProto domain=1/version=2）；标准 opset
  （domain=""）优先记录，仅无标准时记自定义 domain。
- getter：`irVersion()` / `opsetVersion()` / `opsetDomain()` /
  `opsetSupported()`（opset ∈ [9,22] 返回 1，提示性）。
- 回归：`infer_tests/opsetcheck_main.myp`（同 Conv 模型导出 opset 13 与 21）——
  OPSETCHECK ALL OK，v13/v21 opsetVersion 各为 13/21、irVersion>0、
  supported=1、输出 vs ORT max diff 4.8e-7（CPU+GPU）。
- 阶段七其余项：unsupported op 诊断（badOp_ 已有）、多输入/多输出/可选输入
  （多输入多输出已覆盖）、If/Loop/Scan 子图、外置 weight/mmap、IR/计划缓存、
  shape specialization、加载/编译/执行阶段错误码分离。

---

## 2026-09-02 — 阶段六：cuBLAS GEMM（GPU dense/matmul 厂商库加速）

- gpu_ops.myp `dense`/`matmul`：规模达标（outDim≥32 && batch≥32 && outDim*batch
  ≥4096）且 `GpuBackend.cublasAvailable()==1` 时优先 cuBLAS SGEMM（列主序映射：
  行主序 y=W·x → y^T[batch,outDim]=x^T[batch,xRows]·W^T[xRows,outDim]，即
  m=batch,n=outDim,k=xRows，设备指针 = dev+off*4）；失败回退 denseTiled。
- dense 走 cuBLAS 后若 bOff≥0 用 `addBiasRows` kernel 加 bias；matmul 无 bias
  直返。`MYP_CUBLAS_LOG=1` 打印触发（实测 m=96 n=64 k=128 r=1）。
- 回归：`infer_tests/cublas_main.myp`（4D BatchMatMul：A[1,1,64,128]@
  B[1,1,128,96] 大→cuBLAS + C[1,1,96,64]@D[1,1,64,32] 小→thread-per-output，
  双输出）——CUBLAS ALL OK，CPU+GPU max diff 5.7e-6。
- **阶段六优先级 2（cuBLAS GEMM）完成**。剩余：cuDNN Conv/Pool（可用时）、
  CPU 多线程/SIMD/cache blocking、CUDA stream/event 异步传输。

---

## 2026-09-02 — 阶段六：GPU arena 持久化增量同步回归（P5b 多帧正确性）

- runtime 加 `gpuH2DUploads()` / `gpuH2DDownloads()`：持久化模式最近一次
  runGpu 增量 H2D 上传 / D2H 下载的张量数（非持久化整块往返为 0），供
  多帧增量同步正确性断言。
- 回归：`infer_tests/gpupersist_main.myp`（双输入 Sub 模型，gpuInferStart
  持久化 + 两帧）——帧1 上传 2 输入、帧2 仅改 x1 只上传 1 个脏张量（
  `dirtyTid_` 置脏 + runGpu 只增量 H2D），两帧输出 vs ORT max diff 0；
  无 GPU 时 `gpuInferStart` 返回 0 → 回退 CPU 普通路径，仍正确。
- **阶段六优先级 1（GPU arena 常驻与增量同步）P5b 机制已就绪 + 多帧回归
  固化**。下一步：cuBLAS GEMM（优先级 2）。

---

## 2026-09-02 — 阶段五：算子选择（BatchMatMul batch=1 降级 2D matmul）

- ops.myp `matmulBcast`：`ob0==ob1==1`（batch 维全 1，等价普通 2D matmul
  [M,K]×[K,N]）→ 降级 `InferOps.matmul`（@parallel GEMM），否则原串行
  thread-per-output。
- gpu_ops.myp 同条件 → `GpuInferOps.matmul`（内部按 outDim/batch 规模选择
  denseTiled 分块 GEMM vs thread-per-output）——**大矩阵 batch=1 的 BatchMatMul
  获得分块 GEMM 收益**。
- 回归：`infer_tests/opselect_main.myp`（两个 batch 全 1 MatMul：
  [1,1,16,24]@[1,1,24,8] 大 + [1,1,8,8]@[1,1,8,8] 小，双输出）——OPSELECT
  ALL OK，CPU+GPU max diff <2e-6，op 表仍 2× opKind 82（kernel 内分派）。
- 阶段五「先做且 ROI 明确」5 项全部完成（常量去重/死权重裁剪/形状值传播/
  Conv 1x1/算子选择）。

---

## 2026-09-02 — 阶段五：Conv 1x1 专用 lowering（opKind 83，GEMM 化）

- graph_compiler CONV wiring：`kh==kw==1 && stride==1 && 无 padding && dil==1 &&
  group==1` → `rt.addConv1x1`（opKind 83）替代通用 conv/convRelu。**ResNet
  bottleneck 高频路径**（1x1 降维/升维）。
- runtime `addConv1x1`：opA/B/C/D=in/w/b/out，doRelu 存 opRelu_；维度从 tensor
  元数据读（无需额外参数槽）。
- ops.myp `conv1x1` CPU kernel + gpu_ops.myp GPU kernel：纯 GEMM——
  thread-per-output-pixel × 通道归约，**无 kh*kw 窗口循环**，比通用 conv 更简单。
- `Conv1x1Op` + `GpuConv1x1Op` 注册 opKind 83（CPU+GPU）。
- 回归：`infer_tests/conv1x1_main.myp`（两个 1x1 Conv + ReLU 融合 + 3x3 对照，
  Concat 输出）——CONV1X1 ALL OK，CPU+GPU max diff 2.9e-6，has opKind 83=1
  （lowering 生效，测试断言 op 表含 83）。

---

## 2026-09-02 — 阶段五：形状值传播（int64 四则/ReduceSum/Expand 形状链折叠）

- `foldShapeChains` 新增 int64 形状链算子：**Add/Sub/Mul/Div 四则**（Resize sizes
  链的 dim*2 / dim/2，标量广播，Div 除零→0）、**ReduceSum**（与 ReduceProd 并列，
  全归约求和）、**Expand**（shape 输入[1]给出目标长度，数据长度=1 沿维复制）。
- `inferShapes` 顶部形状链节点识别扩展：四则（两输入均 int64 链）/ReduceSum/
  Expand 仅当数据输入是 int64 链时拦截折叠（float 数据四则/ReduceSum 是运行时
  算子，走下方正常分支，不受影响）。
- `OpCode` 注册 `Shape(82)` + `opCode()` 映射——Shape 本是受支持形状链节点，
  但未注册导致 onnx_loader unsupported 诊断误报。
- 回归：`infer_tests/shapeprop_main.myp`（Shape→Slice→Mul(dim,2)→Concat 链驱动
  Resize sizes）——CPU+GPU max diff 4.8e-7 vs ORT，输出 [1,1,8,8] 正确。
- **Resize 测试坑**：ORT 默认 `coordinate_transformation_mode=half_pixel`（loader
  TRANSFORM 默认 0=align_corners）且 nearest 默认 `round_prefer_floor`（内核用
  int()=floor）——测试须显式指定 asymmetric+floor 才能与 ORT 对齐。

---

## 2026-09-02 — 阶段五：常量去重（内容相同初始器合并）

- graph_optimizer `eliminateDeadNodes` 内 DCE + 死权重裁剪后新增常量去重：两两
  比较权重（rows/cols/dtype/transposed/role 相同 + 文件字节逐字节相同，
  `compilerWeightBytesEqual` bridge），内容相同则把后者所有 use 改接前者
  （`replaceAllUses` 覆盖节点 input + graph output）并标 dead → runtime 只
  注册一份，减少 IR/显存占用。
- **去重判据安全设计**：要求 role/transposed 也相同（避免 Conv 权重与 Gemm
  权重等 layout 语义不同的权重被错误合并）；计算常量（cF32_ 存储，无 file_
  字节）不参与去重。
- 回归：`infer_tests/dupconst_main.myp`（两路 Conv，w2==w1/b2==b1 字节相同）——
  dedup count=2、w2/b2 dead、w1/b1 保留、输出 vs ORT max diff 9.5e-7（CPU+GPU）。

---

## 2026-09-02 — 阶段五：死权重裁剪（DCE 后移除未引用初始器）

- graph_optimizer `eliminateDeadNodes` 末尾新增 `pruneDeadWeights`：遍历全部权重，
  检查每个初始器是否被任一活节点（input slot 0-7）引用；未被引用即通过
  `graph.compilerMarkDeadTensor(nm)` 标记 dead，后续不注册 runtime 张量、不搬运。
- 效果：融合/DCE 折叠掉的节点对应权重在 IR/显存中一并移除（resnet 等模型
  折叠链留下的孤儿权重会被清理）。
- 回归：`infer_tests/deadweight_main.myp`（Conv 模型 + 未引用 `junk[3,3]` 初始器）；
  验证输出 vs ORT max diff 2.4e-7 且 `compilerShapeDead(junk)==1`（CPU+GPU）。

---

## 2026-09-02 — 阶段四：BatchMatMul（4D batch matmul + batch 广播）

- inferShapes MatMul：A/B 任一 rank≥3 → batch 维逐维 max 广播 + 最后两维
  matmul（`[max(ab0,bb0), max(ab1,bb1), M, N]`）；2D → 保持 `[d0(A), d1(B)]`。
- **修复 classifyShapes MatMul 输出 kind**：从强制 FC_ACT 改为沿用主输入 kind
  ——4D 输入 → CNN_ACT（4D 张量注册）。原 FC_ACT 把 batch 输出 [1,2,3,5]
  注册成 2D size=2 → 输出全错。
- graph_compiler MATMUL wiring：4D 输入 → `rt.addBatchMatmul`（opKind 82），
  2D → `rt.addMatmul`（opKind 6）不变。runtime addBatchMatmul（opP0=aM,
  opP1=aK, opP2=bN, opP3-8=batch dims）。
- ops/gpu_ops `matmulBcast` kernel：thread-per-output-element，batch 维
  `% 输入 batch 维`（维=1 → 0），K 归约。
- 新增 `tools/make_bmm_onnx.py`（A[1,2,3,4]@B[1,2,4,5] + A2[1,1,3,4]@B2[1,2,4,5]
  batch 广播）+ `infer_tests/bmm_main.myp`（BMM ALL OK，CPU+GPU max diff
  2.4e-7 vs ORT）。
- 回归：43/43 infer_tests + grad_check（GRAD CHECK OK）+ ResNet output sum
  `336.658`（2D MatMul 无破坏）。

## 2026-09-02 — 阶段三：动态 batch 输入 shape 注入 + 编译期 specialization

- **修复 `parseDim` 动态维解析 bug**：默认 `v=1` → `v=0`。ONNX 动态维用"空
  dim"（未设置 dim_value，protobuf 省略默认 0）表示，被解析为 1 → 输入/输出
  登记 d0=1 → inferShapes 的 `addShapeD4` 覆盖条件（任一维==0）永不触发 →
  注入 batch 后输出 batch 仍为 1。修复后空 dim / dim_param / dim_value=0 全部
  → 0，`setInputShape`/inferShapes 覆盖正常。
- 新增 `tools/make_dynbatch_onnx.py`（opset13 Conv：x[N,1,5,5] → y[N,1,3,3]，
  batch 动态 dim_param）+ `infer_tests/dynbatch_main.myp`（`setInputShape`
  注入 batch=2 → DYNBATCH ALL OK，CPU+GPU max diff 4.8e-7 vs ORT，out size=18
  非 9）。验证"加载时输入 shape 注入 + 编译期 specialization"。
- 回归：42/42 infer_tests + grad_check（GRAD CHECK OK）+ ResNet output sum
  `336.658`。

## 2026-09-01 — 阶段三：BF16 权重 dtype 转换（BFLOAT16 → float32）

- `graph.myp` writeWeight 加 dtype==16 分支：每 2 字节 = F32 高 16 位，
  `bits = v << 16` → float32（BF16 位型语义）。
- `onnx_loader` parseTensor 支持 `bfloat16_data`（字段 16，packed 2 字节/元素）；
  raw_data 存 BF16 时由 writeWeight 按 dtype==16 读 2 字节（已覆盖）。
- **ORT CPU 不支持 BF16 Conv** → `make_bf16_onnx.py` 用 numpy 手算参考（BF16
  权重转 f32 后卷积）；`infer_tests/bf16_main.myp`（BF16 ALL OK，CPU+GPU
  max diff 6e-8 = float 精度）。
- 回归：41/41 infer_tests + grad_check（GRAD CHECK OK）+ ResNet output sum
  `336.658`。阶段三 dtype 转换（FP16/BF16）完成，剩 INT8（需量化 scale/zp）。

## 2026-09-01 — 阶段三：FP16 权重 dtype 转换（FLOAT16 → float32）

- `graph.myp` 加 `halfToF32Bits`（IEEE half → F32 位型，含亚正规归一化）；
  `writeWeight` 加 dtype==10 分支：每 2 字节 half → float，直接 wire 顺序拷贝
  （不转置；FP16 权重为常见直读场景）。
- `onnx_loader` parseTensor 支持 `float16_data`（字段 6，packed 2 字节/元素）；
  raw_data（f==9）存 FP16 时由 writeWeight 按 dtype==10 读 2 字节（已覆盖）。
- 新增 `tools/make_fp16_onnx.py`（opset13：x[1,1,5,5] → Conv(W/B FLOAT16) →
  y[1,1,3,3]）+ `infer_tests/fp16_main.myp`（FP16 ALL OK，CPU+GPU max diff 0
  vs ORT）。
- 回归：40/40 infer_tests + grad_check（GRAD CHECK OK）+ ResNet output sum
  `336.658`。

## 2026-09-01 — 阶段三：Add 统一 numpy 广播（4D）

- inferShapes Add 从 element-wise copyShape 拆出 → a/b 逐维 max（广播合并）。
- runtime `addAdd` 传 a/b 4D dims（opP0-3=a、opP4-7=b），doRelu 保留 opRelu_；
  graph_compiler ADD + GRAD_ACC wiring 传 dims。
- ops/gpu_ops add kernel：标量 / 同尺寸（a/b/out 三维全同）/ 逐通道 [1,C,1,1] /
  通用 4D 广播 fallback；doRelu 融合贯穿所有路径。
- bcast 测试扩展 out7（Add 同形状）+ out8（Add 逐通道 bias）——8 输出
  BCAST ALL OK，CPU+GPU max diff 0 vs ORT。
- 回归：39/39 infer_tests + grad_check（GRAD CHECK OK）+ ResNet output sum
  `336.658`。

## 2026-09-01 — 阶段三：统一 numpy 广播（Sub/Div/Mul 4D）

- inferShapes Sub/Div/Mul 输出 = a/b 逐维 max（广播合并），替代 `copyShape(a)`
  （修正 b 比 a 大时输出 shape 过小的潜在 bug）。
- runtime `addSub/addDiv/addMul` 传 a/b 4D dims（opP2-5=a、opP6-8+bw=opX0=b）；
  forward 从 out tid 的 `tN_/tC_/tH_/tW_` 读输出 dims（免额外参数槽）。
- ops/gpu_ops sub/div/mul kernel：保留标量/同尺寸/逐通道快路径 + 新增通用 4D
  numpy 广播 fallback（输出 index % 各输入维，维=1 → 0，复用 where/tile decode）。
- **修复**：同尺寸快路径从 `bSize==n` 收紧为 `an==on..aw==ow && bn==on..bw==ow`
  （b 元素数=输出但 a 更小、a 广播时 bSize==n 误判同尺寸 → a 越界读）。
- 新增 `tools/make_bcast_onnx.py`（opset13，12 输入 6 输出：同形状/标量/逐通道
  [1,C,1,1]/W 广播/HW 广播/b 放大输出）+ `infer_tests/bcast_main.myp`
  （BCAST ALL OK，CPU+GPU max diff 0 vs ORT）。
- 回归：39/39 infer_tests + grad_check（GRAD CHECK OK）+ ResNet output sum
  `336.658`。

## 2026-09-01 — 阶段四 G5：Dropout 算子（推理恒等 / 训练随机 mask）

- OpCode `DROPOUT(81)` + `opCode` 映射；`consumerKindOf` Dropout → CNN_ACT；
  `classifyShapes` ratio（f32 初始器，opset12 输入[1]）+ training_mode
  （int64/bool，若给出）标 dead；inferShapes copyShape（同 element-wise）。
- 语义由 `rt.trainMode()` 分派（forward 内）：推理 = 恒等（copyFlat，CPU+GPU）；
  训练 = 确定性 LCG 序列 `y[i] = (u<ratio) ? 0 : x[i]`（CPU）。GPU 训练路径
  退化恒等（无模型驱动的 GPU 随机，记录）。
- `ops_iface_all` 补 `DropoutOp/GpuDropoutOp` + `registerFwd(81)`。
- 新增 `tools/make_dropout_onnx.py`（opset12：x[1,2,3,4] ratio=0.5，training_mode
  省略 → 推理恒等）+ `dropout_main.myp`（DROPOUT INFER ALL OK，CPU+GPU max diff
  0）；`dropout_train_rt_main.myp` 训练单测（zero rate 0.497 ≈ 0.5，
  DROPOUT TRAIN OK）。
- 回归：38/38 infer_tests + grad_check（GRAD CHECK OK）+ ResNet output sum
  `336.658`。

## 2026-09-01 — 阶段四 G5：LogSoftmax + Embedding 算子

- **LogSoftmax（opKind 79）**：沿 axis 稳定 log-softmax。`consumerKindOf`
  LogSoftmax → CNN_ACT；inferShapes copyShape（同 element-wise）；`axis` 属性
  （默认 -1，负轴 +rank）；graph_compiler 按图维度算 `outer/axisDim/inner`
  （张量视作 [outer, axisDim, inner] 行优先）；CPU/GPU kernel 每 (outer,inner)
  列 max + Σexp + lse（稳定版）。
- **Embedding（opKind 80）**：row-major 查表 `out[s*D+d] = w[ids[s]*D+d]`
  （ONNX 无标准 Embedding op，常规用 Gather(axis=0) 表达——本 op 作为框架层
  查表算子）。`classifyShapes` 权重表 FC_W transB=1（直接拷贝）+ ids 标
  SHAPE/INT64 dead；inferShapes out[S,D,1,1]；graph_compiler `readI64Init` 读
  ids → 临时 f32 张量；kernel clamp ids 越界。
- `ops_iface_all` 补 `LogSoftmaxOp/GpuLogSoftmaxOp/EmbeddingOp/GpuEmbeddingOp` +
  `registerFwd(79/80)`。
- 新增 `tools/make_logsoftmax_onnx.py`（opset13：x[1,2,3,4] axis=1，x*2 拉开
  范围测稳定性）+ `logsoftmax_main.myp`（LOGSOFTMAX ALL OK，CPU+GPU max diff
  4.8e-7 = float 精度）；`embedding_rt_main.myp` 直接 runtime 单测（w[5,4]
  线性 + ids=[1,3,0]，EMBEDDING ALL OK，CPU+GPU max diff 0）。
- 回归：36/36 infer_tests + grad_check（GRAD CHECK OK）+ ResNet output sum
  `336.658`。

## 2026-09-01 — 阶段四 G5：Squeeze + Gather 算子

- **Squeeze（opKind 77）**：去 size-1 维。`consumerKindOf` Squeeze → CNN_ACT；
  `inferShapes` 输出 = 去维后保持轴顺序 + 左对齐补 1 到 4D（axes 属性，无 axes
  时去掉全部 size-1 维）；数据不变 → `copyFlat` 连续拷贝（CPU+GPU，复用 reshape
  kernel 范式）。
- **Gather（opKind 78）**：沿 axis 按 indices 收集（数据 Gather）。`axis` 属性
  （opset11）；indices int64 初始器 `classifyShapes` 标 SHAPE/INT64 dead（不登记
  f32 张量）；`inferShapes` 输出 = data dims 替换 axis 维为 len(indices)；
  `graph_compiler` 用 `readI64Init` 读 indices → 临时 f32 张量（int→float，
  <2^24 精确），kernel 按 gatherRows 模式 `int(a[iOff+k])` 读；负索引 +dim、越界
  clamp；CPU/GPU thread-per-output kernel。
- `ops_iface_all` 补 `SqueezeOp/GpuSqueezeOp/GatherOp/GpuGatherOp` +
  `registerFwd(77/78)`。
- 新增 `tools/make_squeeze_onnx.py`（opset11：x[1,2,3,4] axes=[0] → [2,3,4]）+
  `squeeze_main.myp`；`tools/make_gather_onnx.py`（opset11：x[1,2,3,4] axis=1
  indices=[1,0] → 翻转通道）+ `gather_main.myp`。两者 `SQUEEZE/GATHER ALL OK`，
  CPU+GPU max diff 0 vs ORT。
- 回归：34/34 infer_tests + grad_check（GRAD CHECK OK）+ ResNet output sum
  `336.658`。

## 2026-09-01 — 阶段四 G5：Tile 算子（沿各维按倍数复制）

- OpCode `TILE(76)` + `opCode` 映射；`consumerKindOf` Tile → CNN_ACT（4D）；
  `classifyShapes` Tile repeats 初始器标 SHAPE/INT64 dead；`inferShapes` Tile
  分支：输出 = 输入维 × repeats 维（逐元素乘，repeats 左对齐 rank 不足补 1）。
- runtime `addTile(in,out,id0-3,od0-3)` opKind 76（opP0-3=id, opP4-7=od）；
  ops/gpu_ops `tile` kernel：输出 index % 输入维（维=1 → 0），与 where1/expand
  同构的 4D broadcast decode（输入维可为任意倍数，非仅 1）。
- `ops_iface_all` 补 `TileOp`/`GpuTileOp` + `registerFwd(76)`。
- 新增 `tools/make_tile_onnx.py`（opset13：in[1,2,3,4] tile [1,2,2,3] →
  out[1,4,6,12]）+ `infer_tests/tile_main.myp`：`TILE ALL OK`，CPU+GPU
  max diff 0 vs ORT。
- 回归：32/32 infer_tests + grad_check（GRAD CHECK OK）+ ResNet output sum
  `336.658`。

## 2026-09-01 — 阶段四 G5：Where 算子（逐元素选择）

- OpCode `WHERE(75)` + `opCode` 映射；`consumerKindOf` Where → CNN_ACT（4D 数据）；
  `inferShapes` Where 分支：输出 = cond/x/y 逐维 max 广播（无 shape 的输入按 1）。
- runtime `addWhere(cond,x,y,out)` opKind 75（opA/B/C/D 4 槽）；各输入 4D 维度由
  forward 从各自 tid 的 `tN_/tC_/tH_/tW_` 读取（无需额外参数槽）。
- ops/gpu_ops `where1` kernel：逐元素 `out = cond ? x : y`，三输入各自 4D 广播
  （维=1 → 0，其余维 % 输入维）。**注意**：`where` 是 MYP 保留字（mapping 过滤
  关键字），用作函数名导致整个文件解析错乱（后续方法全部误报 duplicate
  function name）——函数名用 `where1`。
- `ops_iface_all` 补 `WhereOp`/`GpuWhereOp` + `registerFwd(75)`。
- 新增 `tools/make_where_onnx.py`（opset13：cond[1,2,1,1] ? x[1,2,3,4] :
  y[1,2,3,4] → out[1,2,3,4]，cond 广播）+ `infer_tests/where_main.myp`：
  `WHERE ALL OK`，CPU+GPU max diff 0 vs ORT。
- 回归：31/31 infer_tests + grad_check（GRAD CHECK OK）+ ResNet output sum
  `336.658`。

## 2026-09-01 — 阶段四 G4：Expand 算子（broadcast 复制）

- OpCode `EXPAND(58)` + `opCode` 映射；`inferShapes` Expand 分支：输出维度取
  int64 shape 输入[1]（4D），记录到 shapes；`classifyShapes` 把 Expand 的
  shape 张量标为 SHAPE/INT64（dead，不注册数据张量）。
- `graph_compiler` EXPAND wiring（shape 输入 dead 时仍读维度传给
  `rt.addExpand(id0-3, od0-3)`）；runtime `addExpand` opKind=74（**注意**：
  初始误用 73，与 ConvResidual 前向冲突 + ExpandOp 未注册 → dispatch 到
  ConvResidual 内核触发 SIGFPE；改 74 并补 `registerFwd(74)` 修复）。
- ops `expand` CPU kernel + gpu_ops GPU kernel：4D 解码，输入维=1 沿该维
  broadcast 复制，逐元素 `dst=src[c]`。
- `ops_iface_all` 补 `ExpandOp`/`GpuExpandOp` + 注册。
- 新增 `tools/make_expand_onnx.py`（opset13：x[1,2,1,1] →
  Expand(shape=[1,2,3,4])→y[1,2,3,4]，ORT 参考）+ `infer_tests/expand_main.myp`：
  `EXPAND ALL OK`，CPU+GPU max diff 0 vs ORT。
- 回归：30/30 infer_tests + grad_check（GRAD CHECK OK）+ ResNet output sum
  `336.658`。

## 2026-09-01 — 阶段七：Unsupported ONNX op 诊断

- `onnx_loader` 解析时检测未知 op：打印 op 类型、节点索引、输入名，设
  `badOp_` 标记，`load()` 返回 0（显式失败而非静默错误/段错误）。
- `OpCode` 补 `CONSTANT(57)` 映射（防 Constant 误报）。
- 验证：Where 模型 → `Unsupported ONNX op: 'Where' node=0` + `in[0..2]` +
  `load aborted`。回归 29/29 infer_tests + grad_check（L0=1.92528）在
  `MYP_GPU=1 MYP_IR_VERIFY=1` 下全过；ResNet output sum `336.658`。

## 2026-09-01 — 阶段四 G4：ReduceSum/ReduceMax/ReduceMin 算子

- OpCode `REDUCE_SUM/MAX/MIN`(54-56) + `opCode` 映射；Graph 加 `nRedType_`
  （0=mean,1=sum,2=max,3=min）+ `compilerNRedType` 桥。
- `inferShapes` Reduce 族共用 axes 归一化（负轴）+ mode 判定，记录 redType；
  `graph_compiler` REDUCE_* wiring 传 redType；runtime `addReduceMean` 加
  redType（opP6）；ops `reduceMean` kernel 支持 mean/sum/max/min 聚合；
  gpu_ops `reduceMean` 支持 mean/sum（max/min GPU 未实现，打印警告）。
- 新增 `tools/make_reduce_onnx.py`（opset12 合成模型：x[1,2,3,4] → 空间规约
  Reduce{Mean,Sum,Max,Min}→y1[4,2,1,1] + 全规约→y0[4,1,1,1]，ORT 参考）
  + `infer_tests/reduce_main.myp`：`REDUCE ALL OK`，CPU+GPU max diff 0 vs ORT。
  GPU max/min 并行归约补全（`62b8b21`：初值 ±1e30，按 redType 分派，移除
  CPU-fallback 警告）。
- 回归：29/29 infer_tests + grad_check（L0=1.92528）在 `MYP_GPU=1
  MYP_IR_VERIFY=1` 下全过；ResNet output sum `336.658`。

## 2026-09-01 — 阶段二 G2/G3：ValueId、dtype/role 建模 + Reduce 负轴

- **G2 dtype/role 建模**（commit `8a38beb`/`9508a10`）：
  - `graph_defs.myp` 新增 `DType`（ONNX elem_type）与 `TensorRole`
    （UNKNOWN/DATA/SHAPE/WEIGHT/PARAM/GRAPH_IN/GRAPH_OUT）常量类。
  - `GraphShapes` 新增 `shDType_`/`shRole_` 数组与访问器，`addShapeD4/D5` 初始化。
  - `Graph` 新增 ValueId 语义访问器 `valueExists`/`valueDType`/`valueRole`（+ id
    变体），`GraphOptimizer` 3 处 `shapeIdx(x)<0/>=0` 哨兵替换为 `compilerValueExists`。
  - `onnx_loader` 加载时登记 dtype+role：initializer→WEIGHT、value_info
    input→GRAPH_IN / output→GRAPH_OUT（解析 TypeProto.elem_type）、int64 参数→
    SHAPE/INT64、float 参数→PARAM；`classifyShapes` 激活传播设 DATA role +
    传播 dtype（含 Split 多输出）。新增 `Graph graph()` 访问器。
  - 效果：ResNet50 全 231 张量分类（DATA=121/WEIGHT=108/GRAPH_IN=1/GRAPH_OUT=1、
    FLOAT=231、UNKNOWN=0）；slice 模型 int64 参数 13 个 SHAPE/INT64。
- **G3 ReduceMean 负轴归一化**（commit `88d0fd2`）：inferShapes 负轴 +rank 后
  分类 mode，为 ReduceSum/Max/Min 铺路。
- **新回归** `infer_tests/g2_probe_main.myp`：加载真实 ResNet50 断言 WEIGHT≥50/
  GRAPH_IN=1/GRAPH_OUT=1/FLOAT≥50 → "G2 DTYPE ROLE OK"。
- **回归**：28/28 infer_tests + grad_check（L0=1.92528）在 `MYP_GPU=1
  MYP_IR_VERIFY=1` 下全过；ResNet output sum `336.658`。

## 2026-09-01 — 阶段一完成：全部 pass 迁入 GraphOptimizer（Graph 降为组合根）

- **具体 pass 算法全部迁出 `graph.myp`**：`eliminateDeadNodes`/`fuseGapFlatten`/
  `foldIdentityOps`/`foldDoubleRelu`/`fuseConvRelu`/`fuseReluOp`/`fuseConvAdd`/
  `topoSort`/`planMemory`/`layoutNHWC`/`classifyShapes`/`fuseConvBN`/
  `buildReverseGraph`/`inferShapes`/`foldShapeChains`/`foldConstants`
  （+ `normalize3DNode`/`sliceAxis`）移入 `graph_optimizer.myp`，各以「具体
  `Graph` 参数 + `compiler*` 公共访问器」跨类访问 Graph 状态；`Graph.runIRPass`
  只做分发委托。
- **`graph.myp` 从 ~3670 行降至 ~1120 行**：仅保留组合、解析器入口 API
  （`setShape`/`graphInput*`/`graphOutput*`/`isWeightName`）、i64/protobuf 字节
  解释器（经桥暴露给 optimizer）、runIRPass 骨架与 `compiler*` 窄桥。阶段一
  「GraphFacade 只保留组合、配置和生命周期」目标达成。
- **新增 `compiler*` 桥**（Graph `action:` 段）：`WeightAdd/WeightRows/
  LiveConsumers/BnCount/IncBn/LossMode/GradName/EnsureGrad/ReplaceResult/
  IsNodeOutput/ShapeKindOfName/I64*/F32At/Pb*/AttrSetIntArray/LongArray/
  SliceCount/OutputCount/Resize*/NRedMode/NPadCval/ReadF32Init/RegComputedF32` 等，
  覆盖 shape 推断与融合 pass 的全部跨类查询/写操作。
- **回归**：**27/27 infer_tests + grad_check（L0=1.92528）在 `MYP_GPU=1
  MYP_IR_VERIFY=1` 下全部通过**；ResNet output sum `336.658`；bn/conv3d/
  residual_add/split 等专项均 OK。关键提交：`f59337f`（DCE）→ `3eeef70`（GAP）→
  `cd640cf`（identity/double-relu）→ `095157a`（conv-relu/in-relu）→ `b6963f9`
  （conv+add）→ `2a26977`（topo）→ `23d0a52`（planMemory）→ `807c1d8`（NHWC）→
  `c992df5`（classify）→ `82fd763`（BN）→ `8859fa4`（reverse graph）→ `ffc5dbf`
  （inferShapes/fold*）。

## 2026-09-01 — 阶段一：GraphCompiler 拥有完整 lowering（buildRuntime 迁出）

### 变更
- `infer/graph_compiler.myp`：新增 `lower(Graph, InferenceRuntime)`，拥有完整
  IR→runtime lowering——张量登记、op 接线、权重写入（原 Graph.buildRuntime）。
- `infer/graph.myp`：新增 `compiler*` 公共访问器（shape/plan/node/attr/weight/
  padCval/reduceMode/diceWMode），供 GraphCompiler 跨 MYP property 边界查询；
  删除 buildRuntime 方法，`lowerRuntime` 委托 `compiler_.lower(this, rt)`。
- 关键：MYP 一个类只能实现一个 `interface class`，故 lowering 不用第二个 host
  interface，而用循环 import + 具体 `Graph` 参数（探针验证可用）。
- 桥接必须置于 `action:` 段（此前误落 `function:` → 跨类不可调用）。

### 验证（全绿）
- resnet output sum **336.658**、bn 三用例、coarse（591ms 写出输出）、
  grad_check GRAD CHECK OK、split/residual_add/conv3d/const/pad/slice 全部在
  `MYP_GPU=1 MYP_IR_VERIFY=1` 下通过。

---

## 2026-09-01 — 阶段一接口冻结：Graph 领域级访问器

### 变更（`infer/graph.myp`）
- 在不搬迁 SoA 字段、不改变现有 pass 行为的前提下，补齐后续 Graph 组件的稳定接口：
  `nodeOperand/nodeResult/nodeIsAlive`、`attrInt/setAttrInt`、`attrIntArr/setAttrIntArr`、
  `replaceOperand/replaceResultValue`、`valueRank/valueDim/valueKind/valueElementCount`、
  `weightName/weightDType/weightOffset/weightLength` 等。
- 统一规则：后续模块只通过领域级 API 读写节点、属性、shape、weight 和 mutation；不得直接
  访问 `nIn*`、`shD*`、`wOff_`、`planOff_` 等私有 SoA 数组。
- 为后续 `GraphNodes/GraphShapes/GraphWeights/GraphAnalysis/GraphOptimizer/GraphPlanner/
  GraphCompiler` 抽取冻结兼容边界；现有 `Graph`、`OnnxLoader` 公共 API 保持不变。

### 验证
- `resnet_main` 在 `MYP_GPU=1 MYP_IR_VERIFY=1` 下通过，output sum **336.658**。
- `train/grad_check.myp` 编译通过；`git diff --check` 通过。

### 后续迁移进度
- `GraphShapes` 已独立迁移至 `infer/graph_shapes.myp`：Graph 不再持有旧 shape SoA，
  所有 shape 访问通过 `shapes_` facade；16 个 infer_tests 在 `MYP_GPU=1
  MYP_IR_VERIFY=1` 下通过。提交：`f6f969a`。
- 下一块为 `GraphWeights`，包含 weight metadata、原始字节源、int64/f32 常量池和
  BN fold metadata；完成标准是单一所有权，不接受 getter 外壳 + 双份状态。

---

## 2026-09-01 — 修复 MYP_IR_VERIFY 在 Resize/Pad 上的悬空输入（隐藏 IR 一致性 bug）

### 背景
`MYP_IR_VERIFY=1` 下 coarse/fine/seg_liver 报 `DBG fail verifyIR/topoSort`（既有的、
被调试路径掩盖的结构性 bug）：`verifyIR` 检查每个活节点的 5 个输入槽都必须在
shape 表登记且 DefUse 边一致，但折叠类 pass 会把参数链折叠后留下悬空输入引用。

### 根因（两类，同一模式）
- **Resize**：`foldShapeChains`（管线第 1 步）折叠 sizes 链并把生产者节点
  `nType_=""` erase，但 `inferShapes` 尚未为这些中间张量登记 shape →
  Resize 的 `nIn3_`（sizes）仍指向已折叠张量，`shapeIdx` 返回 -1。
- **Pad**：pads/constant_value 参数同样被折叠链 erase，Pad 的 `nIn1_`/`nIn2_`
  悬空。
- `buildRuntime` 的 Resize/Pad 分支只读折叠后的 `nRsz*_` / `nPadB_`/`nPadE_`/
  `nPadCval_`，**从不读悬空输入槽** → 运行期无影响，但违反 verifyIR 不变量。

### 修复（`infer/graph.myp`，纯悬空清理，零计算变化）
- `foldShapeChains` Resize/Upsample 分支：成功折叠出 `nRszD_/H_/W_` 后清空
  `nIn3_[i]`。
- `inferShapes` Pad 分支（3D/4D 两路）：折叠出 `nPadB5_/nPadE5_`（或
  `nPadB_/nPadE_`）与 `nPadCval_` 后清空 `nIn1_[i]`/`nIn2_[i]`。
- `verifyIR`：失败时打印具体 node/type/slot/输入名（仅 `MYP_IR_VERIFY=1` 下），
  便于定位后续同类问题。

### 验证
- `MYP_IR_VERIFY=1` 下 coarse/fine/seg_liver/coarselike32 + 16 个 infer_tests
  全绿（此前 coarse/fine/seg_liver 均 fail verifyIR）。
- 训练路径：grad_check「GRAD CHECK OK」+ 3d_liver_grad_check/cnn_train 编译通过。
- 输出位一致：coarse 98MB 输出与修复前 **byte-identical**；resnet sum 336.658、
  r18 1331.47、residual_add/conv3d/coarselike32/bn/ops2d/const/act 全 OK。

---

## 2026-09-XX — graph.myp 拆分：常量类移入 graph_defs.myp（纯重构，零行为变化）

### 背景
`infer/graph.myp` 达 3670 行/236 方法，主要是单个 `Graph` 类（约 3435 行，
state-of-the-art SoA 数组 + pass 管线）。MYP 无 partial class / 扩展方法 / 继承，
且类属性私有（BUG-001），`Graph` 类本体无法跨文件拆分；但其文件头部 5 个**纯静态
常量类**（无实例状态、自包含）可安全独立成文件。

### 变更
- 新增 `infer/graph_defs.myp`：`Kind` / `NodeField` / `IRAnalysis` / `IRPassKind` /
  `OpCode` 5 个常量类（含 `OpCode.opCode()`/`opTraits()` 静态方法），逐字节从
  graph.myp 迁移（241 行）。
- `infer/graph.myp`：移除上述常量类，仅保留 `Graph` 类 + 头部注释；新增
  `import "./graph_defs.myp";`（3670 → 3437 行）。
- `infer/onnx_loader.myp`：使用 `NodeField.` 67 处，MYP import **不传递导出符号**，
  故显式补 `import "./graph_defs.myp";`。

### 验证（全绿）
- 编译：resnet/r18/coarse/fine/coarselike32/residual_add/conv3d/bn/ops2d/const/
  act/identity_fold/seg_liver 全部 Link OK；训练路径 grad_check/cnn_train 亦编译通过。
- 数值/输出位一致：resnet sum **336.658**、r18 sum **1331.47**（与基线一致）；
  coarse 98MB 输出、fine 输出与拆分前 **byte-identical**；coarselike32/residual_add/
  conv3d/bn/ops2d/const/act/identity_fold 全部 OK。
- 注：`MYP_IR_VERIFY=1` 下 coarse/coarselike32 的 `DBG fail verifyIR/topoSort`
  为**拆分前既有**的调试路径打印（基线与拆分一致），非本次引入。

---

## 2026-09-XX — 阶段 P5d：3D 卷积 tiled im2col-GEMM（大通道 fine 模型）

### 变更（`infer/gpu_ops.myp`）
- 新增 `conv3dTiledGEMM`：把 3D 卷积写成 GEMM `Y[M,N]=W[M,K]·Xim2col[K,N]`
  （M=yC、N=xN*yD*yH*yW、K=cpg*kd*kh*kw），块=32×32 输出 tile + K 分块 BK=32，
  256 线程各算 2×2，W/im2col X tile 协作载入共享内存；im2col 即时展开（任意
  stride/pad/dilation，越界填 0）。权重经 32 oc 复用（原 patch 版仅 OC_GRP=4）。
- 原 patch 版改名 `conv3dTiledPatch`；`conv3dTiled` 改入口：group=1 且
  M≥32/N≥32/M*N≥4096 走 GEMM，否则回退 patch。
- 输出按 NCDHW 布局写回（GEMM N 维=空间位置 gj，分解回 (nn,oz,oy,ox) 加 oc 平面偏移）。

### 验证
- fine（Cin=256/yC=128 大权重 conv，docs 标注的 GEMM 目标）：conv3d **1277→1040ms
  （18.6%）**、ops-loop 1542→1306ms、单帧 1931→1716ms；GEMM vs patch **bit-identical**。
- fine 持久化（run_onnx MYP_GPU_PERSIST）：1729→1296ms（1.33×）；累计单帧
  1931→1296ms（1.49×）。
- coarse（小通道 ≤128，GEMM 门槛多数不满足）：无回退（597-614 vs 607-621 持平，
  与 P5c 已记录的负结果一致）。
- 全回归绿：resnet（sum 336.658）、r18、residual_add、MLP 99%、BN、ops2d、const、
  identity_fold、conv3d、coarselike32 GPU OK、3D U-Net skip grad check OK、
  conv2d_gen diff=0。

---

## 2026-09-XX — 阶段 P5c：推理持久化接入通用运行器 + 3D 大模型（coarse）

### 变更
- `run_onnx.myp`（通用 ONNX 运行器）：`MYP_GPU_PERSIST=1` 时走 `gpuInferStart(outTid)`
  + warmup + 10 帧稳态平均（默认无 env 保持单帧整块传输语义）。
- `coarse_main.myp`（3D U-Net 基准）：同样接入持久化推理（warmup + 10 帧平均）。

### 验证
- coarse（3D U-Net，输出 160³×6=98MB，整块 arena 含全部中间激活）GPU
  **617 → 385ms（1.6×）**（默认 H2D 118ms + D2H 116ms 整块往返 → 持久化仅 D2H 输出
  张量）；输出 vs 修复前 **bit-identical**、vs ORT 0.006119（既有 float32 漂移）。
- run_onnx resnet50：37 → 17ms（稳态），输出 bit-identical；run_onnx coarse：618→382ms。
- 全回归绿：resnet（output sum 336.658）、r18、residual_add、MLP 99%、BN、ops2d、
  const、identity_fold、conv3d、coarselike32 GPU OK、3D U-Net skip grad check OK。

### 备注（已验证的负结果）
- 试过把 `conv2dTiled`（implicit GEMM）移植到 3D（`conv3dTiledGEMM`）：coarse 上
  **位一致但无提速（597-614 vs 607-621ms），已回退**。原因：2D 的 4.2× 来自把
  「无共享内存 thread-per-output」换成「32-oc 权重复用 GEMM」；而 3D patch 版
  `conv3dTiledPatch` 本已是共享内存 tiled，且 3D im2col 即时展开（K=27×、多维索引
  分解）成本抵消权重复用收益。docs 建议的「更大 OC_GRP」同属寄存器压力权衡，收益
  边际——3D 卷积当前 patch 版已近最优，不再投。
- 结论：3D 大模型（coarse）余下大头是整块 arena 传输（H2D+D2H ~234ms），由推理
  持久化（本阶段）解决；算子侧 conv3d 已是 tiled，无廉价再优化空间。

---

## 2026-09-XX — 修复：coarse 3D 模型加载段错误（isConstValue/readF32Init 越界）

### 背景
`coarse_main`（真实 3D U-Net，coarse_model.onnx）GPU 运行段错误 139、零输出——
回溯定位 `Graph_isConstValue`（foldIdentityOps 内）读 `file_[wOff_[wi]+j*4]` 越界。
git bisect 定位 **IR-P1c/P2a（90104b0）引入**（DefUse/恒等折叠），此前回归集不含
coarse 未暴露。

### 根因
`isConstValue`（P4.1 扩展为全元素校验）与 `readF32Init` 对**计算常量**（`wKind_==5`，
Cast int64→float 折叠结果，`wOff_==-1`、值存 `cF32_`）无守卫，直接按 `wOff_` 读
`file_` 越界（`file_[-1]`）；也未检查 `wOff_ + n*4 > fileLen_`。

### 修复（`infer/graph.myp`）
- `isConstValue`：`wKind_==5` 改读 `cF32_[wi*32+j]`（最多 32 元素）；`wOff_<0`
  或越界 → 不折叠返回 0；file 路径保留逐元素校验。
- `readF32Init`：`wOff_<0`（计算常量无 file_ 字节）→ 返回 0。

### 验证
- coarse_main GPU 恢复运行（671ms），输出 **vs 修复前工作二进制 bit-identical**；
  vs ORT maxDiff 0.006119（既有 float32 漂移水平）。
- coarselike32 GPU vs ORT maxDiff 1.67e-6 OK、3D U-Net skip grad check OK。
- 全回归绿：resnet（37ms / persist 16ms、output sum 336.658）、r18、residual_add、
  MLP 99%、BN、ops2d、const、identity_fold、conv3d、conv2d_gen diff=0。

---

## 2026-09-XX — 阶段 P5b：推理持久化（权重驻留 + 输出-only D2H）

### 变更（`infer/runtime.myp`）
- 新增 `gpuInferStart(outTid)` / `gpuInferEnd()`：复用训练持久化机制，使**推理**
  也能把权重一次性驻留显存。此后每次 `runGpu` 只增量 H2D 置脏输入 + 只 D2H 输出
  张量（`markGpuSync`），不再整块 arena 往返（原单帧推理每次 ~100MB H2D + 整块
  D2H 含全部权重）。
- `resnet_main.myp`/`r18_main.myp`：`MYP_GPU_PERSIST=1` 时启用持久化推理 +
  warmup + 20 帧稳态平均（默认无 env 保持原单帧整块传输语义，回归不变）。

### 验证
- ResNet50 GPU 单帧 **38 → 15ms**（稳态，2.5×；profile H2D/D2H=0ms、ops-loop ~15ms）。
  自 P5 前 95ms 累计 **6.3×**；output sum 336.658 逐位一致。
- ResNet18 GPU 9 → 0ms（<1ms 稳态），output sum 1331.47 逐位一致。
- 回归全绿：residual_add/MLP 99%/BN/ops2d/const/identity/conv3d + conv2d_gen
  diff=0 + MNIST train 97%。

---

## 2026-09-XX — 阶段 P5：2D 卷积 tiled（implicit GEMM + 共享内存）

### 变更（`infer/gpu_ops.myp`）
- 新增 `conv2dTiled`：把 2D 卷积写成 GEMM `Y[M,N]=W[M,K]·Xim2col[K,N]`
  （M=yC、N=xN*yH*yW、K=cpg*kh*kw），块=32×32 输出 tile + K 分块 BK=32，
  256 线程各算 2×2，W/im2col X tile 协作载入共享内存；im2col 即时展开
  （stride/pad/dilation 全支持，越界填 0）。
- `conv`/`convRelu`/`convResidual` 改为入口：满足 group=1 且 M≥32/N≥32/M*N≥4096
  走 tiled，否则回退 thread-per-output（`convPlain`/`convReluPlain`/
  `convResidualPlain`，原逻辑保留）；convResidual 的 rOff/doRelu 在 tiled 内处理。
- 输出按 NCHW 布局写回（GEMM N 维=空间位置 gj，需分解回 (nn,oy,ox) 加 oc 平面偏移）。

### 验证
- 新 `bench/conv2d_gen_main.myp`：k5/s2、k3/dilation2（tiled）+ group2、yC8（回退）
  + convRelu、convResidual+doRelu，GPU vs CPU **diff=0** 全过。
- ResNet50 GPU **95 → 37ms（2.6×）**，ops-loop 76→18ms（convRelu 50→13ms、conv
  7→1ms）；top-1 lakeside(10.328)、output sum 336.658 **逐位一致**（CPU 同）。
- ResNet18 GPU 输出逐位一致；residual_add CPU/GPU OK（小卷积回退路径）；MLP 99% / BN
  / ops2d / const / identity_fold / conv3d 全 OK；conv grad check OK、MNIST 97%、
  CNN 96%、3D U-Net skip grad check OK。

---

## 2026-09-XX — 阶段 IR-P4.3b：残差融合专用回归

### 新增
- `infer/tools/make_residual_add_onnx.py` + `infer_tests/residual_add_main.myp`：
  Conv→Add(residual)→Relu 合成模型，断言 `fusedAddCount==1`，CPU+GPU vs numpy
  max diff 3.58e-7（opKind 73 doRelu 独立回归，此前仅 ResNet 端到端覆盖）。

---

## 2026-09-XX — 阶段 IR-P4.3：残差融合折叠 Add 后 Relu + 训练门控

### 变更
- `convResidual` 内核加 doRelu（后置：conv+residual 再过 relu）；runtime `addConvResidual`
  增加 doRelu 参数（存 opRelu_），iface 透传。
- `fuseConvAdd` 融合 Conv→Add 后折叠后续 Relu（`Conv→Add→Relu` → 单 op）。
- `FUSE_CONV_ADD` 仅推理运行（`trainingMode_` 门控；convResidual 无 backward）。

### 验证
- ResNet50 GPU ops 88→72→56、`residual_fused=16`、top-1 lakeside(10.328) 逐位一致、稳态
  ~80ms；CPU 输出一致。
- 全回归绿：identity/BN/ops2d/const/ONNX MLP/conv3d、skip U-Net GPU 训练 acc 100% +
  grad check、MNIST train 97%（`MYP_IR_VERIFY=1`）。

---

## 2026-09-XX — 阶段 IR-P4.2：残差 Conv+Add 融合

### 变更
- 图 pass `FUSE_CONV_ADD`（graph.myp）：Conv/BN 折叠输出唯一 Use=Add(slot0) → 卷积有效输出
  改接 Add 输出、残差（slot1）登记 `nIn3_`、tombstone Add；不融合带 Relu 的卷积。
- 后端（runtime/ops/gpu_ops/ops_iface_all）：新 opKind 73 `addConvResidual` + CPU/GPU
  `convResidual` 内核（输出直接加残差）+ 接口类注册。
- `planMemory` lastUse 覆盖 `nIn3_`/`nIn4_`（残差消费者 + BwdConcat 第三输入）。

### 验证
- ResNet50 GPU：`residual_fused=16`、ops 88→72、top-1 lakeside(10.328) 逐位一致、稳态
  ~80ms（原 96ms）；ResNet50 CPU / resnet18 top-5 参考一致。
- 回归 `MYP_IR_VERIFY=1`：identity/BN/ops2d/const/ONNX MLP/conv3d 全 OK、skip U-Net GPU
  训练 acc 100% + grad check OK。

---

## 2026-09-XX — 阶段 IR-P4.1：恒等折叠支持任意形状/广播常量张量

### 变更（`infer/graph.myp`）
- `isScalarInitValue` → `isConstValue`：初始化器所有元素等于恒等值（0/1）即折叠，不再要求
  标量；标量、任意形状、broadcast 常量张量均满足，逐元素读文件数据校验（不落 f32Tmp_）。
- `foldIdentityOps` 的 Add/Mul/Sub/Div 四个恒等模式全部受益；数值语义严格（x+0/x-0/x*1/x÷1
  对任意 broadcast 逐位保持）。

### 验证
- identity_fold 夹具 one/zero2/zero4/one4 改为全形状常量张量（zero 保持标量）；runtime 仍
  2 ops、图输出 rename/读数一致，`IDENTITY FOLD OK`。
- 回归：BN maxDiff 0、ops2d 4.77e-7、CONST FOLD OK、ONNX MLP 99%、skip U-Net GPU 训练
  acc 100%（`MYP_IR_VERIFY=1`）。

---

## 2026-09-XX — 阶段 IR-P3d：属性统一访问器层收口（P3 完成）

### 变更（`infer/graph.myp`）
- `attrFloat` 扩展 UPSH/UPSW（Resize scales F32 位型）；`inferShapes` 的
  `F32.toDouble(nUpSH_/nUpSW_)` 改走 `attrFloat`（pass 代码内不再有节点属性位型直读）。
- 新 `attrIntArr(node,key,k)` 统一数组访问（SlAx/SlSt/SlSp/PadB/PadE/PadB5/SplitSt/SplitLn，
  取代 `[node*4+k]`/`[node*5+k]` 手算）；`buildRuntime` 的 Slice/Split 分支改走它。
- 3D raw 数组已有 rawSt/rawPd/rawDl/rawKk；节点 int 属性保持直读（热点）。P3 至此完成。

### 验证
- 全量回归 `MYP_IR_VERIFY=1`：identity_fold 2 ops、BN maxDiff 0、ops2d 4.77e-7、
  CONST FOLD OK、ONNX MLP 99%、slice/split 全 OK、resize maxDiff 1.19e-7、
  skip U-Net GPU 训练 acc 100%。

---

## 2026-09-XX — 阶段 IR-P3c：补全 canonical mutation API（replaceResult/eraseNode）

### 变更（`infer/graph.myp`）
- 新增 `replaceResult(node, slot, newName)`（结果槽改写）与 `eraseNode(node)`
  （tombstone + 标死有效输出 + 失效 DefUse）；§3.4 的四个改写原语
  `replaceNodeInput`/`replaceAllUses`/`replaceResult`/`eraseNode` 全部落地。
- `foldIdentityOps`/`foldDoubleRelu`/`eliminateDeadNodes` 的删除路径改用 `eraseNode`；
  `buildReverseGraph` 的 GradAcc 多消费者梯度重命名改用 `replaceResult`（不再直写 SoA）。
- 行为零变化。

### 验证
- 全量回归 `MYP_IR_VERIFY=1`：identity_fold 2 ops、BN maxDiff 0、ops2d 4.77e-7、
  CONST FOLD OK、ONNX MLP 99%、skip U-Net GPU 训练 acc 100% + grad check OK。

---

## 2026-09-XX — 阶段 IR-P3b：buildRuntime 分派迁移到 op 枚举

### 变更（`infer/graph.myp`）
- `OpCode` 扩至 53 算子（补全前向 lowering 算子 Gemm/MatMul/Conv3D/MaxPool/AveragePool/
  Pad/ReLU6/LeakyRelu/SiLU/HardSwish/Clip/Split/NCHW2NHWC/Reshape/Transpose/Slice/Concat/
  Sqrt/ReduceMean/Resize/Upsample 等），成为全图 string→enum 唯一映射。
- `buildRuntime` 接线循环改 `nodeOp(ni)` 分派，全部 ~50 分支 `t == "X"` →
  `code == OpCode.X()`；调试信息改用 `nType_[ni]`；属性读取保持直读（热点）。行为零变化。

### 验证
- 全量回归 `MYP_IR_VERIFY=1`：identity_fold 2 ops、BN maxDiff 0、ops2d 4.77e-7、
  CONST FOLD OK、ONNX MLP 99%、conv3d/pad/slice/split/tensorops/avgpool 全 OK、
  ResNet GPU top-1 lakeside(10.328)、skip U-Net GPU 训练 acc 100% + grad check OK。

---

## 2026-09-XX — 阶段 IR-P3a：op 枚举 + 保护 traits + 统一属性访问器

### 变更（`infer/graph.myp`）
- 新 `OpCode` 类：枚举 pass 逻辑与训练/状态算子，`opCode(string)` 为唯一 string→enum
  映射；`opTraits(code)` 标记 stateful（Update/GradAcc）与 trainOnly（DiceLoss/SoftmaxCE/
  Bwd*），DCE 对受保护节点一律跳过（防御：P4 在训练图重跑 DCE 时不会误删状态节点）。
- 新 `nodeOp(i)`/`attrFloat(node,key)` 访问器；P2 恒等/Relu pass、Conv/IN/GAP→Relu/Flatten
  融合 guard、buildReverseGraph 的 Softmax 定位与 BwdConcat 检查迁移到枚举；BN 折叠 eps
  改用 attrFloat。buildRuntime lowering 保持字符串直读（P3b 迁移）。
- MYP 踩坑修复：static 类方法内未限定调用同类静态方法会生成未定义全局 `@ADD`；须全限定。

### 验证
- identity_fold 2 ops、BN maxDiff 0、ops2d 4.77e-7、CONST FOLD OK、ONNX MLP 99%、
  ResNet GPU top-1 lakeside(10.328) 逐位一致、skip U-Net GPU 训练 acc 100%、grad check OK
  （均 `MYP_IR_VERIFY=1`）。

---

## 2026-09-XX — 阶段 IR-P2e：恒等简化族扩展 + 连续激活合并

### 变更（`infer/graph.myp`）
- `foldIdentityOps` 扩展到 Sub/Div：`Sub(x,0)`/`Div(x,1)` 按严格标量初始器折叠；恒等
  操作数仅允许 slot1（`Sub(0,x)=-x`、`Div(1,x)=1/x` 非恒等不折叠），Add/Mul 仍可交换。
- 新 `FOLD_RELU` pass（IRPassKind 12，声明 DefUse）：`Relu(Relu(x))→Relu(x)`，内层
  producer 可为独立 Relu 或已融合（`nFused_+nRelu_`，如 ConvRelu）的 producer——有效
  输出已过 ReLU 则外层 Relu 冗余；外层输出改接内层有效输出（含图输出 rename）。

### 验证
- `make_identity_fold_onnx.py` 新增 `data3→Relu→Relu→out3`（双 Relu 合并）与
  `data4→Sub(0)→Div(1)→out4`（Sub/Div 链折叠）；优化后 runtime **2 ops**（Softmax+
  Relu），图输出 `out3→relu3a`、`out4→data4`，读数一致，`IDENTITY FOLD OK`。
- 回归：BN maxDiff 0、ops2d 4.77e-7、CONST FOLD OK、ONNX MLP 99%、ResNet GPU top-1
  lakeside(10.328) 逐位一致、skip U-Net GPU 训练 acc 100%。

---

## 2026-09-XX — 阶段 IR-P2d：图输出 rewrite（replaceAllUses 覆盖隐式消费者）

### 变更（`infer/graph.myp`）
- `replaceAllUses(oldValue,newValue)` 把**图输出也视为值的隐式消费者**：`goName_` 指向
  oldValue 的条目一并改接为 newValue，与既有精确 `(userNode,operandSlot)` edge 重连共用
  同一条 mutation API。后续所有 value-replacement rewrite 自动获得图输出支持。
- `analysisFingerprint` 计入 `goName_`（图输出名是 planMemory persist / Liveness 依赖），
  纯图输出 rename 不再被指纹误判为 changed=0。
- `foldIdentityOps` 解除「跳过图输出」限制：`Add(x,0)`/`Mul(x,1)` 的输出即使直接是图输出
  也能折叠，图输出名改指保留值（如 `Add(data2,0)→data2`），原输出张量 tombstone 后不再登记。

### 验证
- `make_identity_fold_onnx.py` 扩展第二输出 `out2 = Add(data2, zero2)`；优化后 runtime 仍
  **1 op**，`graphOutputName(1) == "data2"`、`tensorId("out2") < 0`、`out2` 读数 == data2
  缓冲 `[10,20]`，`IDENTITY FOLD OK`（`MYP_IR_VERIFY=1`）。
- 回归：BN fold/standalone/norelu max diff **0**；ops2d vs ORT **4.76837e-07**；`const_main`
  `CONST FOLD OK`；ONNX MLP **99%**；带跳跃连接 3D U-Net GPU 训练 **acc 100%**（训练路径
  `buildReverseGraph` 与图输出 rename 无冲突）。

---

## 2026-09-XX — 阶段 IR-P2b：Conv→Relu 融合迁移到 DefUse Pattern/Rewrite

### 变更（`infer/graph.myp`）
- `FUSE_CONV_RELU` 声明 DefUse 依赖，并由 `matchSingleUseOp(value,"Relu",0)` 匹配：
  输入 Value 必须仅有一条精确 use，且 user 为 Relu 的 slot0。替换旧版“找 Relu + 全图扫
  consumers”的字符串循环；重复 operand/残差多消费者不会误融合。
- `fuseReluIntoProducer` 作为统一 rewrite primitive，执行 effective-output 重定向、
  nFused/nRelu lowering 标记、Relu tombstone 和中间 Value dead 标记；完全沿用现有
  CPU/GPU ConvRelu/Conv3D doRelu backend 协议。

### 验证
- `MYP_IR_VERIFY=1`：带跳跃连接 3D U-Net GPU 训练 **acc 100%**。
- ResNet GPU 推理正常（真实 Conv→Relu 与残差多消费者路径）。
- 后续同一 matcher 已迁移 InstanceNorm→Relu 与 GAP→Flatten：`ops2d` GPU vs ORT
  max diff **4.76837e-07**（`OPS2D ALL OK`）；ResNet GPU 推理仍正常。
- `Conv→BatchNormalization` 也已迁移到 `matchSingleUseOp`（仅替换安全匹配，保留既有
  权重/偏置折叠逻辑）：BN 专项 `fold/standalone/norelu` 均 max diff **0**。
- `DCE` 声明 DefUse 依赖，改为每轮重建后按 `valueUseCount` 删除死节点，取代每候选节点
  全图 `liveConsumers` 扫描；identity 夹具仍 **3 ops→1**，`const_main` 为 `CONST FOLD OK`，
  skip U-Net GPU 训练仍 acc 100%。

---

## 2026-09-XX — 阶段 IR-P1c/P2a：按需分析 + 首条 DefUse 驱动恒等 Rewrite

### 变更（`infer/graph.myp`）
- P1c：`ensureDefUse/ensureTopo/ensurePassAnalyses` 按需获取分析；`runIRPassToFixpoint`
  以 `lastPassChanged` 最多迭代指定次数，未收敛即失败。Liveness 继续只由 lowering 前
  `planMemory` 显式构建，防止优化 pass 隐式改写内存规划。
- P2a：新增 `IDENTITY_FOLD`（声明 DefUse 依赖），严格匹配标量初始器 `Add(x,0)` 与
  `Mul(x,1)`，通过 `replaceAllUses` 的精确 `(userNode,operandSlot)` edge 重连，再 tombstone
  节点/中间 Value；跳过图输出、广播、动态或近似常量。
- 修复 DefUse analysis validity：`replaceAllUses`/`replaceNodeInput` 除置 `duValid_=0` 外也
  清 `analysisValid_` 的 DefUse 位；`ensureDefUse` 检查两者。此前链式 rewrite 第二轮会错用
  旧 edge，导致消费者未重连、buildRuntime wire fail。

### 验证
- 新 `infer/tools/make_identity_fold_onnx.py` + `infer_tests/identity_fold_main.myp`：
  `data→Add(0)→Mul(1)→Softmax` 优化后 runtime **3 ops→1 Softmax**，输出匹配 numpy，
  `IDENTITY FOLD OK`（含 `MYP_IR_VERIFY=1`）。
- 回归：带跳跃连接 3D U-Net GPU 训练 **acc 100%**（`MYP_IR_VERIFY=1`）。

---

## 2026-09-XX — 阶段 IR-P1b：PassResult changed 契约 + 指纹驱动分析保留

### 变更（`infer/graph.myp`）
- `runIRPass` 新增 Graph 内 PassResult 等价状态：`lastPassOk/Changed/PreservedAnalyses`；
  保持既有 MYP 方法签名和固定 pipeline 顺序。
- 新 `analysisFingerprint` 覆盖 node op/inputs/outputs/fused/layout 与 value shape/dead/layout，
  不覆盖仅影响数值的属性（epsilon/alpha），因此只反映 DefUse/Topo/Liveness 所依赖的结构。
- pass 前后指纹相同：`changed=0` 且保留全部分析、不递增 generation；不同：`changed=1`
  且保守失效全部分析。新增 `passRequiredAnalyses` 稳定入口，现有 legacy pass=0；P2 rewrite
  将首个声明 DefUse 依赖。

### 验证
- `MYP_IR_VERIFY=1`：ONNX MLP 推理 **99%**；带跳跃连接 3D U-Net GPU 训练 **acc 100%**。
- 尚未实现逐分析 preserved/按需 analysis 重建或不动点迭代；这些留为 P1c，在 P2 pattern
  rewrite 需要时再落地。

---

## 2026-09-XX — 阶段 IR-P1a：AnalysisManager 有效性管理 + 固定 Pass Dispatcher

### 变更（`infer/graph.myp`）
- 新 `IRAnalysis` 位集（DefUse/Topo/Liveness）与 Graph 的 `analysisValid_`、全局及各分析
  generation；`invalidateAnalyses(mask)`/`markIRMutation()` 统一管理缓存失效。
- 新 `IRPassKind` + `runIRPass`：把既有 fold/infer/classify/fuse/DCE/layout 的固定调用顺序
  纳入 dispatcher。旧 pass 保持原实现和顺序；每个成功 pass 后保守使全部分析失效。
- `rebuildDefUse`、`topoSort`、`planMemory` 分别标记当前 generation 的 DefUse/Topo/Liveness
  有效；反向图追加 Bwd*/Update 后显式失效，避免训练第二阶段读取前向图的分析缓存。

### 验证
- `MYP_IR_VERIFY=1`：ONNX MLP 推理 **99%**；带跳跃连接 3D U-Net GPU 训练 **acc 100%**。
- P1a 仅实现保守失效；`PassResult.changed/preservedAnalyses` 与不动点迭代留在 P1b，避免
  在现有 SoA pass 尚未迁移时做不可靠的精确缓存保留。

---

## 2026-09-XX — 阶段 IR-P0：优化 IR 访问器 + 精确 DefUseAnalysis（零行为变化）

### 变更（`infer/graph.myp`）
- 新建最小 **IR 访问器层**：Shape table index 作为稳定 `ValueId`，提供
  `valueId/valueName`、`nodeAlive`、`nodeInputValue/nodeOutputValue`、
  `valueProducer/valueUseCount/valueUseNode/valueUseSlot`；名称仍仅在 ONNX 边界保留。
- 新 `rebuildDefUse()`：由现有 SoA 节点/形状表重建 O(1) producer 与精确
  `Use{userNode, operandSlot}` 边。采用紧凑链表（最多 5×512=2560 条边），不为每个 Value
  预留 512 条 use；同一值在同节点多次使用（如 `Mul(x,x)`）保留为独立边。
- 新最小 mutation API `replaceNodeInput()`（失效 `duValid_`）和 `verifyIR()`；
  `MYP_IR_VERIFY=1` 时 topoSort 前验证活节点输入存在对应 Value 与精确反向 use edge。
- `topoSort()` 改从 DefUseAnalysis 取 producer 初始化入度，并按输出 use-edge 递减，删除
  逐轮扫描全部节点/字符串输入的 O(N²) 依赖更新；多输出与重复 operand 语义不变。

### 验证
- `MYP_IR_VERIFY=1`：ONNX MLP 推理 **99%**；带跳跃连接 3D U-Net GPU 训练
  **acc 100%**；验证器未报错。
- 设计文档 `docs/ir_layer.md` 更新 P0 实施状态、真实存储布局与 P1-P3 边界。

---

## 2026-09-XX — 阶段4h：训练优化器基建（SGD / 动量 / AdamW + weight decay）

### 目标
把训练优化从「裸 SGD」升级为可配置优化器，向通用训练框架迈出关键一步（现代训练
基本离不开 Adam/AdamW + weight decay）。

### 变更
- **`infer/ops.myp` + `infer/gpu_ops.myp`**：`update` 拆为 3 个内核（CPU+GPU 同公式）：
  - `updateSGD`：`W -= lr·g + decay·W`（decay=lr·wd；wd=0 即纯 SGD，与旧版逐位一致）
  - `updateMom`：经典动量（beta1=0.9 固定）`v=0.9v+g; W -= lr·v + decay·W`
  - `updateAdamW`：`m=0.9m+0.1g; v=0.999v+0.001g²; mhat=m/bc1; vhat=v/bc2;
    W -= decay·W + lr·mhat/(√vhat+eps)`，eps=1e-8；**bc1/bc2=1-β^t 由 host 预计算**
    （Math.pow，避免 GPU libdevice pow 依赖）；m 在 mOff、v 在 mOff+n。
- **`infer/runtime.myp`**：优化器状态管理
  - 字段 `optMode_`（0=SGD 默认/1=动量/2=AdamW）、`optWd_`、`optStep_`、`optOff_[512]`、
    `optBase_`；`setOptMode`/`setOptWd`/`optStep`/`optOffAt` 访问器
  - `prepOptState(tid)`：为权重张量在 **arena 尾部**预留 2·n 状态区（动量 v / Adam 的
    m+v），`growOptArena` 复制保真扩容；buildRuntime 的 Update 分支调用 → 先于任何
    run（含 gpuPersistentStart 的 H2D）分配好，持久化 GPU 状态区有效
  - `run()/runGpu()` 训练模式每步 `optStep_++`（Adam 偏差修正 t，1-based）
- **`infer/ops_iface_all.myp`**：`UpdateOp`/`GpuUpdateOp` 按 `rt.optMode()` 分派三内核，
  传状态偏移 + `decay=lr·wd` + 预计算 bc1/bc2；`import math`。
- **`infer/graph.myp`**：Update 分支加 `rt.prepOptState(w)`。

### 验证
- **`train/opt_check.myp` + `infer/tools/make_opt_ref.py`（新）**：5 步已知梯度，CPU 三内核
  最终权重 vs numpy float32 参考 **maxDiff=0**（`OPT CHECK OK`）——SGD/动量/AdamW 数学逐位正确。
- `3d_liver_train` 加 `MYP_OPT_MODE`（0/1/2）：SGD/动量/AdamW 均 fg-DSC=1.0（GPU）——
  GPU 优化器收敛验证。
- 回归：`3d_seg` 100%、`3d_unet_skip` 100%（默认 SGD 逐位兼容，无回归）。
- 说明：beta1/beta2 固定 0.9/0.999（Adam 标准默认，MYP 方法参数上限 10 所致），eps=1e-8；
  梯度裁剪（全局 norm 归约）留待后续。

---

### 目标
真实肝脏 CT 中前景（肝脏）仅占 ~5% 体素（类不平衡）——等权 Dice 会塌缩到全背景。
为框架加**类不平衡 Dice loss**，并用肝脏形态合成数据验证小前景可学，为真实 seg_liver 铺路。

### 变更
- **`infer/ops.myp` + `infer/gpu_ops.myp`**：`diceLoss` 加 `wMode` 参数：
  - `wMode=0` 等权（逐位兼容旧版）；`wMode=1` **归一化逆频率加权**
    `w_c = 1/(freq_c+0.001)`、`wc = w_c/Σw` → `loss = 1 - Σ_c wc_c·Dice_c` **有界**。
  - **关键教训**：未归一化逆频率（wc 直接用 1/freq）会让随机初始 loss ≈ -5.8（Σw 大 →
    loss 大负 → 大梯度 → 第一步就把 softmax 推饱和 → 训练卡死 0.489）。归一化后
    随机初始 loss≈0.34、全背景 loss≈0.95（强推学前景）。
  - **GPU 跨类归约**：wMode=1 用 runtime stats 区（`rt.statsOffRef`）分 4 核
    （K1 每类 w_c→stOff+c；K2 单线程 W=Σ→stOff+rows；K3 每类梯度+写 loss 项；
    K4 单线程 loss=1-Σ）。**去掉 eps 参数**（固定 1e-5）以容纳 stOff（GPU 10 参上限）。
- **`runtime.myp`**：`addDiceLoss` 去 eps，`opP1=wMode`；`onnx_loader`/`graph` 加
  `setDiceWMode(1)` 透传（DiceLoss 节点接线）。
- **新**：`make_3d_liver_onnx.py`（16³ 带跳跃 U-Net）、`3d_liver_train.myp`（肝脏形态
  合成：前景 ~5% 模糊边界椭球 + 噪声；`MYP_DICE_WMODE`/`MYP_LIVER_FIXED`/`MYP_LIVER_SMALL`
  控制）、`3d_liver_grad_check.myp`（16³ 全图梯度对拍）、`dice_weighted_grad_check.myp`
  （加权梯度有限差分 + GPU 对拍）。

### 验证
- **等权（wMode=0）fg-DSC=0（塌缩全背景）** vs **归一化加权（wMode=1）fg-DSC=1.0**
  （GPU）/ **0.949**（CPU 小规模）——类不平衡问题解决，`LIVER 3D TRAIN OK`。
- `dice_weighted_grad_check`：CPU 有限差分匹配 + GPU maxDiff=0/lossDiff=0。
- `3d_liver_grad_check`：16³ 全图梯度（含 concat 路径）GRAD CHECK OK。
- 回归：dice_grad_check / dice_softmax_grad_check OK（wMode=0 等权逐位兼容）；
  `3d_unet_skip_train`（等权）acc 100% 无回归。

---

### 目标
U-Net 编码器-解码器加入**跳跃连接**（skip connection，channel 维 Concat）并端到端训练：
完整补上此前 3d_unet_train 注释中的待办「无跳跃连接——需 5D Concat 反向留后续」。

### 变更
- **`infer/ops.myp` + `infer/gpu_ops.myp`**：新 `bwdConcat` 反向内核（opKind 72，CPU+GPU）。
  concat 是双射（每输出元素唯一对应一输入元素）→ 直接散射赋值；布局同前向 concat
  （4D 视图 + axis + 各输入轴长 ax0/1/2，5D 时 od3 折叠 H*W）。
- **`infer/runtime.myp`**：`addBwdConcat`（opKind 72，opA=dy, opB/C/D=dx0/1/2, opP0-8=axis/nIn/od0-3/ax0-2）。
- **`infer/ops_iface_all.myp`**：`BwdConcatOp` + `GpuBwdConcatOp`（registerBwd(72) CPU+GPU）。
- **`infer/graph.myp`**：
  - `buildReverseGraph` 加 **Concat → BwdConcat** 分支（反向图集成；nAxis 复制）。
  - **多消费者梯度累加（关键）**：跳跃张量（r1/r3）同时被编码器 Conv 与 Concat 消费 →
    其梯度名被**多个反向节点重复产出** → topoSort 按输出名 over-decrement → 死锁；
    且 BwdConcat 与主 producer 存在依赖环（编码器反向依赖 concat 上游 u#g），不能靠
    执行顺序累加。方案：主 producer 输出改 `g_a`、BwdConcat 输出改 `g_b`、加纯前向
    **GradAcc 节点** `Add(g_a,g_b) → g`（buildRuntime 接 `rt.addAdd`）；topoSort 支持
    in3 入度。
  - **planMemory 图输出缓冲修复（训练失败根因）**：`prob`（图输出）被 DiceLoss 消费后
    lastUse≠-1 → 被判可复用 → 缓冲被 GradAcc 输出复用 → GradAcc（softmax 之后执行）
    覆盖 prob → softmax 概率变 logits → Dice loss 塌缩 → 训练 acc 不升。修复：图输出
    （`isGraphOutput`）强制 persist=1，缓冲不参与复用。
- **`infer/tools/make_3d_unet_skip_onnx.py`（新）**：生成带跳跃连接 3D U-Net
  （`data/onnx/3d_unet_skip.onnx`）：编码 Conv3D×2→MaxPool×2 + 解码 Resize→Concat(skip)→Conv3D→
  Resize→Concat(skip)→Conv3D×2→Softmax；8³ 输入、2 类分割、axis=1（C 维）跳跃。
- **`train/3d_unet_skip_train.myp`（新）**：跳跃 U-Net 端到端训练（Dice loss，SGD）。
- **`train/3d_unet_skip_grad_check.myp`（新）**：全图梯度对拍（e1_w/e4_w/d1_w/d3_w 采样，
  解析梯度 vs 中心差分；e1_w 含 concat 路径、e4_w 含 r3→cat1 路径）。

### 验证
- `3d_unet_skip_train`：**CPU 与 GPU（MYP_GPU=1）均训练至 acc 100%**（epoch 0 即 100%，
  数据为 d<4 纯块 50/50）。
- `3d_unet_skip_grad_check`：**CPU 与 GPU 均 GRAD CHECK OK**（e1_w 全链含 concat 路径梯度匹配）。
- `infer/tools/concat_check.myp`（新，bwdConcat 内核对拍）：CONCAT-FWD/BWDCONCAT OK。
- 回归：3d_unet（无跳跃）100%、conv3d/resize3d/dice grad_check OK（planMemory 改动无副作用）。

---

### 变更（`infer/gpu_ops.myp` + `ops_iface_all.myp`）
- **4 个新通用 GpuInferOps 内核**（补 61-67 GPU 缺口；rmsnorm/layernorm/gelu 已有）：
  `gatherRows`（thread-per-output-element）、`rope`（thread-per-(h,a,s) 对，写旋转两行）、
  `attention`（thread=(head,i)，scOff 须 [heads*S*S]）、`attentionCached`（thread=head，
  scOff 须 [heads*len]）——对齐 ops.myp InferOps 语义 + 既有「resident 单数组 + @gpu for」
  模式（无原子、无 stream，与 rmsnorm/layernorm/gelu 一致）。
- **7 个 Gpu* 接口类**（GpuRmsNormOp/GpuAttentionOp/GpuGatherRowsOp/GpuLayerNormOp/
  GpuGeluOp/GpuRopeOp/GpuAttentionCachedOp）——rmsNorm/layerNorm 复用通用内核 + arena
  尾部 stOff（rt.statsOffRef()）。
- **注册**：`registerAllIfaceOps` 61-67 改为 CPU+GPU 双注册 → `runGpu()` 现可统一分派
  **全部 opKind 1-71**（接口框架 CPU+GPU 100% 覆盖，无 if/else 回退）。
- 删 registerAllIfaceOps 头部"LLM 61-67 runGpu 无分支只注册 CPU"过时注释。

### 验证（`infer_tests/llm_runtime_gpu_main.myp` 新回归）
- 静态图含 gatherRows→rmsNorm→attention(因果,GQA)→rope→gelu→layerNorm + 独立
  attentionCached decode，A=CPU run()、B=GPU runGpu()，同种子填充：
  **各输出 maxDiff=0 → `LLM RUNTIME GPU CHECK OK`**（CPU/GPU 均）。
- 回归：3d_unet/3d_seg 训练 CPU+GPU、run_onnx 推理、distilgpt2_forward 全 OK。

---

## 2026-09-01 — 阶段4e4：run()/runGpu() 删除 if/else，只保留全接口分派

### 变更（`infer/runtime.myp` + `ops_iface_all.myp`）
- `run()`/`runGpu()` 的 **127 个 if/else 分支全部删除**，只保留接口分派：
  `fwdCpu_/bwdCpu_`（CPU）与 `fwdGpu_/bwdGpu_`（GPU）查表 → `forward/backward`
  虚表调用。opKind 全覆盖（1-45/50-60/61-71，relu 2/51 经 registerIfaceOps）。
- **首调自动注册**：`run()/runGpu()` 开头 `if (ifaceInit_ == 0) { ifaceInit_ = 1;
  registerAllIfaceOps(this); }`——runtime.myp **循环导入** ops_iface_all.myp
  （`import "./ops_iface_all.myp"`；MYP LoadedSet 去重安全处理循环），全入口
  零改动，无需逐文件显式注册。
- 删除已过时的对拍测试 `train/op_iface_check.myp`/`op_iface_full_check.myp`
  （原为接口 vs if/else 逐位对拍，if/else 已不存在 → 失去意义；正确性由训练/
  推理回归 + 前期 bit-exact 对拍保证）。
- 依赖自举编译器 v3.15.199 的接口数组类型修复（`[128 x {ptr,ptr}]`，slot≥64 不再
  别名）。runtime.myp 末尾保留 `registerFwd/Bwd/FwdBwd` 公共 API（仍可用，但
  run() 会自动全量注册）。

### 验证
- 推理：`run_onnx`（cnn_mnist top-3）CPU/GPU 正常；bn_main/onnx_main/conv3d_main
  推理测试 OK（onnx_main MLP 99%）。
- 训练（全接口路径）：3d_unet_train / 3d_seg_train **CPU/GPU 均 `TRAIN OK`**
  （acc 100%、dice→0）。
- deeplearning 全量入口编译检查通过（无真错误，库文件 no-main 正常）。

---

## 2026-09-01 — 算子全量接口化迁移（阶段4e3，含自举编译器接口数组 bug 修复）

### 全量迁移已写（`infer/ops_iface_all.myp`）
- 把全部 ~50 个算子（opKind 1-45, 50-60, 61-71）按 interface 双方法
  `IOp{forward,backward}` 拆成独立类：`DenseOp/SoftmaxOp/SigmoidOp/AddOp/
  MatmulOp/ConvOp/MaxpoolOp/.../Conv3dOp/Maxpool3dOp/Avgpool3dOp/Resize3dOp/
  ConvTransposeOp`（含对应反向方法）+ GPU 版 `Gpu*Op` + 训练专用
  `SoftmaxCeOp/UpdateOp/DiceLossOp`（bwd 表，backward 内自检 trainMode）。
- `registerAllIfaceOps(rt)` 全量注册（registerFwd/registerBwd/registerFwdBwd；
  仅单向算子用 registerFwd/registerBwd；LLM 61-67 CPU-only 不注册 GPU）。
- 需要的 runtime 访问器补齐：`sCntAt/sIdAt/sOdAt/sAxAt/sStAt/sSpAt`（Slice）、
  `pCvalAt`（Pad）、`statsOffRef`（GPU 归约暂存区）。
- 2 层小图 `op_iface_check.myp`：A(if/else) vs C(registerAllIfaceOps)
  **逐位 diff=0**（CPU/GPU，loss 0.82→0.58 下降）——已注册的 dense/relu/
  softmaxCE/update 走接口正确。

### **自举编译器接口数组类型 bug —— 已修复（tools/selfhost/src/codegen.myp）**
- **症状**：全接口路径（C=registerAllIfaceOps）3D U-Net 从 **bwdSoftmax（opKind 70）
  发散**（loss 一致、prob 被覆盖 → 权重全错 → 训练卡死）。GPU 通过、CPU 失败。
- **根因（执行时逐 op checksum 追踪定位）**：自举编译器 `IrEmit.llvmType` 对定长
  数组用简单元素类型递归——接口元素返回 `ptr` → `IOp[128]` 错成 `[128 x ptr]`
  （8B/元素，共 1024B），而 store/load 用 `{ptr,ptr}` GEP（16B/元素，2048B）→
  **slot≥64 溢出写穿数组边界、别名到后续字段**：`bwdCpu_[70]`（field 26 字节 1120）
  与 `fwdGpu_[6]`（field 27 偏移 96）同址 → registerAllIfaceOps 中 SoftmaxOp
  注册（`bwdCpu_[70]`）后被 GpuMatmulOp 注册（`fwdGpu_[6]`）覆盖 → `bwdCpu_[70]
  .backward()` 调 GpuMatmulOp.backward（写垃圾、SoftmaxOp.backward 打印不触发）。
  C++ oracle 正确处理（`typeNodeToLLVMType` 接口→`{ptr,ptr}`），仅 selfhost 缺口。
- **修复**：`codegen.myp` 的 `llvmType()` 增加定长数组分支——元素类型用 codegen 自身
  `llvmType`（接口/struct/枚举/bitfield 感知），`IOp[128]` → `[128 x {ptr,ptr}]`
  （2048B），与 store/load GEP 一致、不再越界别名。
- **验证**：
  - `train/op_iface_full_check.myp`（新回归）：3D U-Net A(if/else) vs
    C(registerAllIfaceOps) 同数据跑前向+反向，**loss 逐位一致、prob maxDiff=0
    bad=0 → `FULL IFACE CHECK OK`**（修复前 prob 分歧 1016/1024）。
  - `op_iface_check`（2 层 POC）CPU/GPU 仍 OK；3d_seg/3d_unet 训练 CPU/GPU 回归
    全 OK；自举 bootstrap 95/95 + 主套件 466/466 全绿。
  - 双编译器对照（执行时追踪 + 结构体 dump `[128 x {ptr,ptr}]`）确认修复生效。
- **现状**：`ops_iface_all.myp` 全部算子类 + `registerAllIfaceOps` 可直接启用——
  训练入口加一行 `registerAllIfaceOps(rt)` 即可一键切换全接口分派（增量零行为变化）。

---

## 2026-09-01 — 算子拆分 POC：interface 多态分派（阶段4e）

### 背景
`InferOps`（67 个 CPU static 内核）+ `GpuInferOps`（66 个 GPU 内核）由
`InferenceRuntime.run()/runGpu()` 的 **127 个 if/else 分支**按 opKind 分发。用户
提议把算子拆开、用 MYP interface 实现（MYP interface = fat-pointer vtable，手册
§Interface 明言「适合算子模式」，见 `examples/ad.myp`）。

### 接口分派机制（`infer/runtime.myp`）
- 新增 `interface IOp { void forward(rt, i); void backward(rt, i); }`——**接口同时
  声明前向与后向两个方法**（对齐 ad.myp 的算子模式；单 `run()` 会失去「同一算子
  前向+反向」的语义，且无法在算子对象里保存前向中间量供反向复用）。
- 四张分派表 `IOp[128] fwdCpu_/bwdCpu_/fwdGpu_/bwdGpu_`（property，接口数组元素
  支持）+ `registerFwd(k,op,isGpu)` / `registerBwd(k,op,isGpu)` /
  `registerFwdBwd(fwdKind, bwdKind, op, isGpu)`（同一实例进前向/反向两个槽）+ 公共
  访问器（`arenaRef/devRef/trainMode/lrRef/tensorRows..W/opAAt..opP8At/opXAt/
  opReluAt`）。
- `run()`/`runGpu()` 循环开头：`kk=opKind_[i]`，前向表命中 → `forward()`，否则反向
  表命中 → `backward()`，否则 `else if` 回退原 if/else——**增量、零行为变化**。
- `curDev_` 字段：runGpu 每轮存当前设备指针，接口 GPU 算子经 `rt.devRef()` 取 dev
  （原 `dev` 是 runGpu 局部变量，接口方法拿不到，必须存字段）。

### 拆分实现（`infer/op_iface.myp`）
- relu 合并为一个算子类，**forward(前向 y=relu(x)) + backward(后向 dX=dY·(x>0)) 双
  方法**：`ReluOp`（CPU，调 `InferOps`）+ `GpuReluOp`（GPU，调 `GpuInferOps`
  `@gpu for` 内核）。backward 实现内自检 `rt.trainMode()==1`（与 if/else 分支一致）。
  `registerIfaceOps(rt)` 用 `registerFwdBwd(2, 51, op, isGpu)` 把同一实例注册到
  relu 前向槽(2)与反向槽(51)。
- **验证** `train/op_iface_check.myp`：同一 2 层小图（dense→relu→dense→
  softmaxCE+反向+update）两份 runtime——A 不注册（if/else）、B 注册接口；每步 loss
  + 最终 w1/w2/b1/b2 **逐位 diff=0**（CPU 与 GPU 均 `OP IFACE CHECK OK`），且
  loss 正常下降（0.82→0.58）证明训练闭环仍工作。
- **迁移模式**：其余算子照此搬——每算子一个类，forward()/backward() 里分别写原
  if/else 前向/反向分支体（只有单向的算子用 registerFwd/registerBwd 单独注册）；
  搬完可整体删 if/else。GPU 内核在接口方法内经 vtable 分派验证可行。
- 回归：3d_seg/convt/3d_unet 训练 + resize3d/dice_softmax/3d_unet 梯度对拍全 OK
  （接口表默认全 null → 未注册路径与改动前完全一致）。

---

## 2026-09-01 — 3D U-Net 编码-解码：bwdResize3d + Resize 反向图集成 + 编解码训练

### bwdResize3d（`infer/ops.myp` + `gpu_ops.myp`，opKind 71）
- Resize3D 上采样的反向（U-Net 解码上采样路径）。坐标变换同前向 resize3d
  （transform 0=align_corners / 1=half_pixel / 2=asymmetric；mode 0=nearest
  1=trilinear）。CPU：thread-per-output 散射累加（单线程无竞争）。GPU：
  **每输入一个线程**——各线程遍历全部输出按插值权重求和，避免散射 `+=`
  的并发累加竞争（首版 thread-per-output 散射在 GPU 上丢更新，maxDiff 1.9→0）。
  trilinear 边界与前向一致：`izf>=inD-1` 时 clamp `fz=0`（前向 iz2 同样 clamp
  到 inD-1），保证权重守恒。
- `runtime.myp` opKind 71 `addBwdResize3d(dOut,dIn,inD..outW,mode,transform)`
  + run/runGpu 分发（N/C 取 dOut 张量）+ opKindName。
- **对拍** `train/resize3d_grad_check.myp`：nearest/trilinear × align_corners/
  asymmetric 四组合，`L=Σy·dy` 对 x 有限差分 + GPU vs CPU 逐元素 maxDiff=0。

### Resize 反向图集成（`infer/graph.myp`）
- buildReverseGraph 新增 `Resize/Upsample` 反向分支 → `BwdResize(dy,x)→dx`
  （仅 3D；2D 反向未实现跳过），复制 mode/transform。buildRuntime 新增
  `BwdResize` 接线（inD/inH/inW/outD/outH/outW 从输入/输出张量形状推出）。
- **框架 BUG（Update 空梯度）**：反向图对每个节点**所有输入**都 ensureGrad
  （含 Resize 的 sizes 常量 `u1_sz`）→ `u1_sz#g` 有形状 → 第 5 步给 role=0 的
  常量造 Update 节点 → buildRuntime 因该常量非运行时张量（tensorId=-1）失败
  `buildRuntime wire fail type=Update in0=u1_sz(-1)`。**修复**：Update 条件从
  `shapeIdx(gradName)>=0` 改为 `isNodeOutput(gradName)==1`（仅梯度确实由反向
  算子产出的可训权重）。
- **框架 BUG（Resize3D 输出 D=1）**：foldShapeChains 在输入秩确定前跑
  （shapeRank5(p2) 误判 0）→ 只填 nRszH/nRszW，nRszD_=0 → inferShapes 条件
  `nRszH>0` 成立 → outD=nRszD_=0 → clamp 成 1 → 下游全部塌成 D=1
  （u2 应为 [1,16,8,8,8]=8192 却 1024）。**修复**：foldShapeChains 用 sizes
  元素个数判 3D/2D（5=[N,C,D,H,W]，4=[N,C,H,W]，不依赖秩时序）；inferShapes
  is5=1 时要求 nRszD/H/W 三者齐备才用折叠值，否则回退 readI64Init 读全 sizes。

### 3D U-Net 编码-解码训练（`train/3d_unet_train.myp` + `tools/make_3d_unet_onnx.py`）
- 模型：Conv3D×2→MaxPool3D→Conv3D×2→MaxPool3D→**Resize3D**→Conv3D→**Resize3D**
  →Conv3D×2→Softmax（编码下采样 + Resize3D 上采样解码；无跳跃连接——跳跃连接需
  5D Concat 反向，留后续）。输入 [1,1,8,8,8]，2 类分割（中心块=前景）。
  Resize 用 `sizes` int64 初始器 [N,C,outD,outH,outW] + mode=linear +
  coordinate_transformation_mode=asymmetric。
- `train/3d_unet_grad_check.myp`：全图 dL/dw 对 e1_w/e3_w/d1_w 中心差分对拍
  （lr=0 + train 模式：前向算 loss、Update 无副作用）→ `3D UNET GRAD CHECK OK`。
- **端到端**：CPU/GPU 均 **acc 100%、dice 0.15→1.95e-8** → `3D UNET TRAIN OK`
  （lr=0.4→0.15→0.05；lr=1.0 时 GPU float32 累积在分割边界震荡，acc 卡 91%）。
- 经验：GPU 训练用更小 lr（float32 累积精度低于 CPU double 归约）。

---

## 2026-08-31 — 阶段4d：bwdConvTranspose + Dice loss 图集成 + 3D 分割训练

### bwdConvTranspose（`infer/ops.myp` + `gpu_ops.myp`，opKind 68）
- 权重 [Cin,Cout,kh,kw] 同前向 ONNX 布局；dW 用 gather 公式（ih=(oh+pt-ky)/sh 整除
  界内）、dX 用 scatter 公式（oh=ih*sh-pt+ky 界内）、db=Σdy。GPU 三内核
  thread-per-元素全和赋值（无原子、无需清零）。`runtime.myp` opKind 68 + run/runGpu
  分发；`graph.myp` buildReverseGraph/buildRuntime 支持 ConvTranspose 反向节点。
- **对拍** `train/convtranspose_grad_check.myp`：dW/dX 有限差分 + db 直接对拍，
  GPU vs CPU 逐元素 maxDiff=0。**端到端** `train/convt_train.myp`
  （ConvTranspose 分类 ONNX）CPU/GPU acc 100%、loss 0.667→0.35 → `CONVT TRAIN OK`。

### Dice loss 图集成（opKind 69 DiceLoss + 70 BwdSoftmax）
- 分离式集成：`diceLoss`（已有 CPU，新增 GPU thread-per-class 串行归约，O(C·N)）
  + `bwdSoftmax`（dlogit=p·(dp-Σp·dp)，CPU/GPU，thread-per-column）。
  `OnnxLoader.setLossMode(1)` → `Graph.setLossMode` → buildReverseGraph 用
  `DiceLoss(prob,label)→(dprob,loss)` + `BwdSoftmax(dprob,prob)→dlogits` 替代
  SoftmaxCE。prob 即 Softmax 图输出（[1,C,D,H,W] → 运行时 [C,D*H*W] 视图，
  softmax 自然沿 C 归一化）。
- **对拍** `train/dice_softmax_grad_check.myp`：dlogit 全链有限差分 + GPU vs CPU
  maxDiff=0 → `DICE SOFTMAX GRAD CHECK OK`。
- **框架 BUG（label 5D 形状）**：buildReverseGraph 的 label 用 `addShapeD4` 硬编码
  → 5D 分割 logits [1,C,D,H,W] 丢深度维（label 注册成 [C,1] 2 元素）→ diceLoss 读
  越界 label → 训练不收敛（loss 0.5 卡死 / 先增后降）。**修复**：label 按 logits
  秩 `addShapeD5` 且 `setShapeKind(label, shKind_[logits])`（否则 buildRuntime 的
  FC_ACT 分支优先于 5D 分支，仍注册成 [C,1]）。**症状识别**：dump 张量表 label
  size=2（应为 C·D·H·W）→ 查 label 注册秩/kind。

### 3D 分割训练（`train/3d_seg_train.myp` + `tools/make_3d_seg_onnx.py`）
- 模型：Conv3D(1→4,k3,p1) → ReLU → Conv3D(4→2,k3,p1) → Softmax；数据合成 8³ 体素
  2 类（上半体=前景）。`setLossMode(1)` + Dice loss → **dice 0.5→8.6e-8、acc 100%**
  → `3D SEG TRAIN OK`（CPU/GPU）。U-Net 编解码上采样（bwdResize3d 或
  ConvTranspose3D）留作后续扩展。

---

## 2026-08-31 — 阶段4b：GPU 3D 反向（bwdConv3D/bwdMaxPool3D/bwdAvgPool3D）

### GPU 3D 反向内核（`infer/gpu_ops.myp`）
- 新增 `bwdConv3D`（db/dW/dX 三内核，thread-per-元素全和赋值，无原子、无需清零，
  非对称 padding pdt/pdb/pt/pb/pl/pr + 膨胀 dd/dh/dw + group，镜像 2D bwdConv
  模式）、`bwdMaxPool3D`（清 dX + thread-per-output 重算 argmax 路由 dY，
  stride≥kernel 无重叠假设）、`bwdAvgPool3D`（清 dX + thread-per-output 算 div
  散射 dY/div，cip=1 用整窗口分母）。
- `infer/runtime.myp` `runGpu()` 新增 opKind 58/59/60 GPU 分发，参数映射与 CPU
  `run()` 逐位一致（Conv3D 假定膨胀=1、group=1，kd/kh/kw 取权重形状）。
- `train/3d_cnn_train.myp` 接入 GPU 训练：`gpuPersistentStart()` + `markGpuSync`
  + `step()` 走 `runGpu()`（MYP_GPU=1），否则 CPU `run()`。
- **数值对拍** `train/conv3d_grad_check_gpu.myp` → bwdConv3D/bwdMaxPool3D/
  bwdAvgPool3D 三内核与 CPU `InferOps` **逐元素 bit-exact（maxDiff=0）**；
  `3d_cnn_train.myp` MYP_GPU=1 → **3D CNN TRAIN OK**（loss/acc 与 CPU 逐轮一致）。
- **依赖的编译器修复**：selfhost 编译器 `@gpu for` 内 `Math.exp` 只发射
  `declare @__nv_expf` 未链接 libdevice → 真 GPU 返回 0（softmax 输出全 0 →
  loss=0）。修复见主 changelog v3.15.198（kernel .ll 先 llvm-link libdevice.10.bc
  + opt internalize/globaldce 再 llc）。至此 GPU 3D 训练闭环。

---

## 2026-08-31 — stdlib/mmap.myp + safetensors mmap 零拷贝读（加载 6.3s→2.7s）

### 新增内存映射文件库（分两层：runtime 提供符号，stdlib 薄包装）
- `runtime_myp/mmap.myp`：MYP runtime 符号 `myp_file_size` / `myp_mmap_file` /
  `myp_munmap_file`（open(2)/lseek(8)/mmap(9)/munmap(11)/close(3) 纯 raw syscall，
  与其他 runtime_myp 模块一致——raw syscall 属 syscall 边界，不进 stdlib）。
- `stdlib/mmap.myp`：`ffi` 声明 + `MmapFile` 薄包装（`u8/i32/i64/f64` 按文件相对
  偏移 O(1) 零拷贝读，`__myp_mem_load_*`）。可移植：Windows/ARM 由对应 runtime
  提供同名符号，stdlib 不碰裸 syscall。
- **分层教训**：首版把 raw `__myp_syscall` 直接写进 stdlib——`__myp_syscall` 的
  syscall 号按架构硬编码（x86-64 open=2/mmap=9，aarch64 openat=56/mmap=222，
  Windows 无稳定 syscall ABI），且是绕过运行时的逃生舱。改正：syscall 下沉 runtime，
  stdlib 用 ffi。

### safetensors 接入 mmap（`infer/safetensors.myp`）
- `load()` 里 `mm_.open(path)` 整文件 mmap；`readF32RangeAt` 走 mmap 快路径（免
  fopen/fseek/fread），mmap 不可用时回退原 fread 逐字节。
- **坑**：① `mm_` 是 class 属性默认 null，构造器须 `mm_ = new MmapFile()`，否则
  null 解引用段错误；② `MmapFile.u8(off)` 的 off 是**文件相对偏移**（内部检查
  `off >= size_`），首版传了 `mm_.base() + off`（绝对地址）→ 越界恒返 0。
- **验证**：safetensors_test 整张量 bit-exact（maxAbsDiff=0）；qwen2 直连前向
  权重加载 **6.3s→2.7s（2.3x）**，argmax=785 == transformers。

---

## 2026-08-28 — safetensors 直连 Qwen2 推理（免 extract_qwen2.py）

### `llm/qwen2_safetensors_forward.myp`：模型直接从 model.safetensors 跑起来
- **背景**：safetensors 只有权重、无网络结构（结构在 config.json + 架构代码）——
  不能像 ONNX 那样由 onnx_loader 自动建图；装配层必须按架构手写（或按 config
  参数化），graph.myp 帮不上这个场景。
- **实现**：`infer/safetensors.myp` 新增 `readF32Into(i, dst, dstOff)`（整张量读入
  调用方数组指定偏移，免中间数组拷贝）；`readF32Range` 重构为 `readF32RangeAt`
  （核心带 outOff）+ 薄包装（签名不变）。
- **前向**：与 qwen2_forward.myp 相同的 24 层装配，唯一区别是权重逐张量从
  `model.safetensors`（290 张量 BF16）按名读入 arena（`loadT` 逐张量
  findTensor + readF32Into，按 HF state_dict 命名映射到 .bin 同款布局）。
  BF16→F32 = bits<<16，与 .bin 的 fp32 值 bit-exact。
- **验证**：权重加载 **6.3s**（BF16 272MB，vs .bin fp32 1.98GB 的 ~18s）；前向
  logits maxAbsDiff=0.26（bf16 量化噪声），**argmax=785 == transformers 参考**，
  `QWEN2 SAFETENSORS FORWARD OK`。全程无需 extract_qwen2.py。
- 结论：Qwen2 已可从 HuggingFace safetensors 直接推理；下一步可把生成/对话入口
  （qwen2_generate/qwen2_talk）同样切到 safetensors 直读。

---

## 2026-08-28 — safetensors 整张量读取（readF32All / readF32Range）

### 完善 `infer/safetensors.myp`：解除「只能读 4096」的限制
- 原 `readF32(i, out, maxN)` 须调用方预分配 `out` 且传 `maxN` 上限，验证只抽查
  前 4096 元素 → 整张量读取不直观、易越界。
- 新增 API：
  - `readF32All(i)` / `readF32AllByName(name)`：按张量元素数 `tensorElemCount(i)`
    内部分配 `new float[n]`，一次性读完整张量并返回 `float[]`（MYP 方法返回动态
    数组已用最小探针验证可行；失败返回空数组）。
  - `readF32Range(i, startElem, out, count)`：读 `[startElem, startElem+count)`
    段（seek 到 `byteOff + startElem*elemSize`），支撑 LLM 分块加载 embed/权重。
  - `readF32(i, out, maxN)` 重构为 `readF32Range(i, 0, out, maxN)` 的薄包装（
    签名不变，无破坏）。
- 验证：`infer_tests/safetensors_test.myp` 改为整张量读取
  `model.embed_tokens.weight`（151936×896 = **136,134,656** 元素），与
  qwen2_weights.bin（fp32）采样对拍（首 4096 + 末 4096 + 均匀 16 点 = 8207 点）
  **maxAbsDiff=0（bit-exact）**；整次运行 1.9s（MYP 缓冲 io 逐字节读 272MB BF16
  无压力）。

---

## 2026-08-26 — json_tool 同步 runtime.addAdd 新签名（修复 7 个入口编译失败）

### 修复：`json_tool/model_loader.myp` 的 `addAdd` 缺 doRelu 参数
- `infer/runtime.myp` 的 `addAdd(aTid, bTid, outputTid, doRelu)` 已升级为 4 参数
  （图优化 Add+Relu 融合，`infer/graph.myp:2091` 已传 `nRelu_[ni]`）；但 json_tool
  的旧 loader 仍传 3 参数 → **7 个 json_tool 入口编译失败**
  （`missing required argument(s) 'doRelu' — expected 4 arguments, got 3`）。
- 修复：`rt.addAdd(a, b, out)` → `rt.addAdd(a, b, out, 0)`（doRelu=0 纯 Add，
  与 json_tool 旧语义一致，不融合 Relu）。
- 验证：deeplearning 全量编译 **70 入口成功 / 11 库文件（无 main，正常）/ 0 真错误**
  （infer_tests 26 + train + json_tool + llm + diffusion）。

---

## 2026-08-20 — GPU 批量推理（qwen2 batch=4 250 tok/s）+ 通用算子库 + distilgpt2 GPU 前向

### 多序列批量推理（`llm/qwen2_gpu_batch.myp`）
- N 路同步批量（权重共享、pos 同步）：BN=1 11ms/90 tok/s → BN=2 12ms/166 →
  **BN=4 16ms/250 tok/s**（mismatch=0，33 步 EOS）。CUDA Graph 默认 ON。
- **关键修正（mismatch=384 根因）**：matmul 写 `logOff[o*BN+s]`（vocab-major、
  batch 内序），argmax 原先按 seq-major 读 → 批量错位；**BN=1 掩盖布局 bug**。
  fix：argmaxBatch 按 stride=BN 读。
- **rstdBatch 独立小 kernel**（`gpu_llm_ops_batch.myp`）：原来每个 chunk 线程
  冗余重算 896-sum²（同 seq ~9728 线程）→ 拆成 grid=N 独立 kernel → gateup
  **2.4x**（0.416→0.170ms/layer）。
- **amortized 内核否决**：seq-in-thread（4 标量累加器）→ 2x 更慢（并行度损失 >
  带宽节省）。GPU batch GEMM 是**延迟受限**非 DRAM 受限。

### 通用 GPU 算子库（`infer/gpu_ops.myp`）
- 新增：`rmsnorm`（独立小 kernel 算 rstd + thread-per-element）、`layernorm`
  （mean/invStd 独立 kernel，g/b 可选）、`gelu`（erf inline）、`denseTr`
  （转置权重 [xRows,outDim]，thread=输出行，bOff<0 无 bias）。既有 bwdDense/
  bwdConv/bwdConv3D/bwdMaxPool/bwdAvgPool/bwdRelu/bwdSigmoid/bwdAdd/softmaxCE/
  update(SGD) + instancenorm（stats 独立 kernel 模式）。
- 测试：`infer_tests/rmsnorm_main.myp`（N=4/N=1 多形状 host 对拍）、
  `infer_tests/dense_tr_main.myp`（dense vs denseTr **bit-identical**，maxDiff=0）。
- `llm/gpu_llm_ops.myp` 新增 `attention`（全序列因果：thread=(head,i)，scOff
  [heads*S*S]，j>i → -1e30）。

### distilgpt2 GPU 前向（`llm/distilgpt2_gpu.myp`）
- 82M GPT-2 全 GPU：LayerNorm/GELU/MHA + **非转置权重**（与 Qwen2 转置布局不同）
  → 证明 GPU 框架不绑定 Qwen2 架构。结果 argmax=464='The'，maxRelDiff=4.2e-4。
- 复用 `GpuInferOps.layernorm/dense/gelu/add` + `GpuLLMOps.attention` +
  `GpuLLMOpsBatch.gatherRows`。坑：GPU dense 10 参（a,work,xOff,xRows,wOff,outDim,
  batch,bOff,yOff,dev）vs CPU 8 参。

### 通用优化模式（GPU 算子设计可复用）
1. **stats 独立小 kernel**（rstd/mean 用 N 线程网格单独算）——避免每个数据线程
   冗余重算（rstdBatch 2.4x）。
2. **thread 映射按输出布局选最快维度**（denseTr thread=输出行对 batch=1 GEMV
   最优；qkvC `r=o*N+ss` seq-fastest）。
3. **输出布局与 host 读一致**（vocab-major vs seq-major）——BN=1 会掩盖。
4. **带宽 vs 并行度权衡**：LLM decode 延迟受限，amortized/向量化收益为负。
5. **transposed vs 非转置权重**：两种都要支持（GPT-2 vs Qwen2），denseTr 新增
   转置变体而非改原 dense。

---

## 2026-08-21 — 交互式多轮对话（5c4，--talk）

- **`llm/distilgpt2_talk.myp`**：交互式 REPL，全链路 MYP。`You:` 读 stdin 一行 →
  拼 `User: <输入>\nAssistant:` 到对话历史（int[] 字节缓冲）→ MYP BPE 编码 → KV-cache
  生成（遇 EOS=50256 停）→ 解码打印回复 → 回复文本追加回历史 → 下一轮。输入 exit/quit
  退出。上下文跨轮累积（实测 11→62 tokens）。
- 驱动 `run_distilgpt2_chat.py` 加 `--talk [n_tokens]` 模式（编译+运行 talk 程序）。
- **说明**：distilgpt2 是 base 模型（非指令微调），贪心 argmax 会重复/复读（"I'm a
  freelance writer..."），属正常现象。对话机制（多轮+上下文记忆）完整可用；要自然对话
  需指令微调模型 + 温度/top-k 采样（下一阶段）。

---

## 2026-08-21 — BPE 编码搬进 MYP + 端到端 chat（5c2/5c3）

### 纯 MYP GPT-2 ByteLevel BPE 编码器（`llm/bpe.myp`）
- 三阶段全在 MYP：字节编码（bytes_to_unicode，`b2c_` 表运行时生成）→ 预分词（ASCII 等价
  扫描器复现 GPT-2 正则 `'s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+`）
  → BPE 合并（merges 开放寻址哈希，pair 元素可多字符）→ vocab 查表（开放寻址哈希）。
- 表由 `llm/make_bpe_tables.py` 从 tokenizer.json 生成（FNV-1a，T=2^17，key=int16 LE
  每码点，merge pair 用 0xFFFF 分隔）：`bpe_merges_*.bin` + `bpe_vocab_*.bin`。
- **验证**：`llm/bpe_encode_test.myp` vs GPT2Tokenizer，7 个测试文本逐 token 一致 →
  `BPE ENCODE OK`（含撇号缩写、标点、重复词等）。

### 端到端 chat（`llm/distilgpt2_chat.myp`）
- 全链路 MYP：读 `distilgpt2_prompt.txt`（UTF-8）→ **MYP BPE 编码** → KV-cache 推理 →
  **MYP 字节级解码** → 文本输出。Python 只做数据准备 + transformers 参考生成。
- 驱动 `run_distilgpt2_chat.py` 改跑 chat 程序：`prompt.txt` → 编译 → 运行；输出
  `BPE ENCODE OK`（vs prompt_ids.bin）+ `DISTILGPT2 GENERATE OK`（vs ref_gen_ids.bin）。
- **实测**：`Once upon a time`/32 → BPE 4 tokens 一致、生成 mismatch=0；`The secret of
  happiness is`/40（--no-ref）正常输出。
- **坑**：MYP 类属性必须在 `property:` 段（类顶部放属性是解析错误，且错误行号错位到
  导入方）；预分词是 re 交替"首个匹配"非最长（`'s!` → `'s`+`!`）；FNV-1a 用 long 防
  int32 溢出。

---

## 2026-08-21 — 阶段5c：distilgpt2 真实权重导入 + 前向装配 + KV-cache 真实文本生成

### 权重提取与参考（`llm/extract_distilgpt2.py`）
- 从用户导出的 ONNX（`data/llm/distilgpt2_onnx/model.onnx`，unfused 1599 节点）提取
  权重 → `data/llm/distilgpt2_weights.bin`（328MB，f32 LE）：wte[50257,768](=lm_head tied)
  + wpe[1024,768] + 6 层各 12 数组 + **最终 `transformer.ln_f`（w+b，1536 floats）**。
- 参考 logits 改用 **numpy 复刻前向（含 ln_f）** 生成（已与 transformers GPT2LMHeadModel
  权威输出验证一致），不再信任 unfused ONNX 图的计算结果（其 KV-cache 处理有 bug：
  onnxruntime argmax=464 ≠ 真实；权重本身 bit 级一致）。

### 前向装配（`llm/distilgpt2_forward.myp`）
- 6 层 GPT-2 前向：LayerNorm → c_attn(dense) → attention(12头,因果) → c_proj → 残差 →
  LayerNorm → c_fc → gelu(erf) → mlp_c_proj → 残差；最后 **ln_f → lm_head(wte tied)**。
- **`DISTILGPT2 FORWARD OK`**：argmax MYP=464 == 参考=464（'The'），maxRelDiff=4.2e-4，
  与 transformers 一致。
- **根因（本次核心 bug）**：MYP forward 与旧 numpy 参考都**漏了 lm_head 前的最终
  LayerNorm ln_f** → logits 量级爆炸（+134~+277，argmax=262）≠ 真值（argmax=464）。
  加 ln_f 后 MYP=numpy=onnxruntime=transformers 全一致。权重文件同步补上 ln_f（1536
  floats，原缺）。

### 真实文本生成（`llm/prep_distilgpt2_gen.py` + `llm/distilgpt2_generate.myp`）
- `prep_distilgpt2_gen.py`（onnxvenv，torch+transformers）：GPT2Tokenizer 编码 prompt →
  `distilgpt2_prompt_ids.bin`；构建 id→字节 查表（`vocab_offs.bin` + `vocab_data.bin`，
  字节级解码后的 UTF-8）；transformers 贪心生成 → `distilgpt2_ref_gen_ids.bin`。
- `distilgpt2_generate.myp`：KV-cache 增量 decode（GPT-2：LayerNorm+GELU、wte+wpe、
  最终 ln_f、MHA 12 头、argmax），W=128 滑窗；MYP 内 **BPE 字节级解码**（ubyte[] →
  `str()`），直接输出可读文本。
- **`DISTILGPT2 GENERATE OK`，token mismatch=0**：与 transformers 贪心逐 token 完全一致。
  生成文本：`Once upon a time of war, the United States was the only country in the
  world to have a military presence. ...`（贪心退化重复是 GPT-2 已知现象）。
- **CPU 推理一键工具**（通用化）：`run_distilgpt2_chat.py "任意 prompt" [n_tokens] [--no-ref]`
  一键在 CPU 上跑 distilgpt2（编码 → 编译 → KV-cache 推理 → 解码文本）。`prep` 脚本支持
  CLI 参数 + 写 gen_cfg.bin（生成数）；MYP 生成数从 cfg 读、参考文件存在才对拍、prompt/
  generated ids 打印移到 `MYP_DG2_DBG=1`。验证：`The quick brown fox jumps` 32 token
  mismatch=0 / GENERATE OK；`In a world where machines think`（--no-ref）输出正常。

### 踩坑（写入 llm/README.md）
- **对拍必须用同一 prompt**：S=6（无 EOS）transformers argmax=198、S=8（含 2 EOS）
  argmax=464——prompt 不同会得假"分歧"。
- **`__myp_io_write_byte` 写当前文件句柄，不是 stdout**（无打开文件返回 -1）。stdout
  输出原始字节：收集进 `ubyte[]`（`out[i] = ubyte(v)`）→ `Console.writeString(str(b))`。
- MYP `ubyte(int)` 显式强转可用（numeric→numeric）；`b[i]=<int>` 无隐式转换会报错。

---

## 2026-08-20 — 阶段5b：KV cache 增量 decode（滑动窗口生成）

- `infer/ops.myp` 新增 `attentionCached`（KV cache 增量注意力：单 token Q [D] vs 历史
  K/V 缓存 [kvD,stride]，softmax 后 Σp·v；**stride（缓存容量/行步长）与 len（参与注意力
  列数）分开传**）。runtime opKind 67 + run() 分发。
- `llm/gpt_generate.myp`：逐 token 增量前向（每层 QKV → 追加 K/V 到缓存 →
  attentionCached → 残差 → FFN → LM head → argmax），缓存满 W 左移（滑动窗口）。
  每一步与整窗重算（forwardFull，全序列 attention）对拍。
- **验证**：`GPT GENERATE OK`（双编译器）——**填充阶段（step<W）增量与整窗重算精确
  一致（maxd=0）**，证明 KV cache 机制正确；滑窗阶段（step≥W）为标准滑动窗口近似
  （丢弃最旧 token 改变余下 token 上下文，Mistral 同款，只统计不判失败）。
- **两个坑**：
  1. `attentionCached` 初版用 `W`（当前长度）当缓存行步长 → 与固定容量 Wmax 布局冲突
     → 改为 stride/len 分开（隔离测试 W=1 时 stride==len 不暴露，填充多 token 才出错）；
  2. 滑动窗口 shift 后增量 ≠ 整窗重算是**固有近似**（上下文变化），不是 bug。
- 待续：5c distilgpt2 转 ONNX + 图路径 + tokenizer → 5e GPU attention。

---

## 2026-08-20 — 阶段5a2：LayerNorm/GELU/RoPE/GQA 算子 + distilgpt2 权重下载

### 算子补齐（`infer/ops.myp` + `infer/runtime.myp`，CPU）
- `layerNorm`（GPT-2：y=(x-mean)/√(var+eps)·γ+β，按特征维/每 token）；
- `gelu`（GPT-2 精确 erf 版 0.5x(1+erf(x/√2))，MYP 无 erf → 内联 Abramowitz-Stegun 近似）；
- `rope`（Llama 真实约定：**逐头**、head_dim 内半配对，cos/sin 表 [dh/2,S] 共享）；
- `attention` 增加 **GQA groups** 参数（Q 头 b 共享 KV 头 b/groups，MHA=1 不变）。
- runtime opKind 64/65/66（LayerNorm/GELU/RoPE）+ attention opP5=groups。

### 验证
- `llm/make_llm_ops_ref.py`（numpy）+ `llm/llm_ops_check.myp` → `LLM OPS CHECK OK`：
  LayerNorm maxDiff=0（精确）、GELU/RoPE/GQA-attn ≈2.4e-07；双编译器一致。
- RoPE 坑：初版用「全特征半旋转」，与真实 Llama「逐头 head_dim 内配对」不符 → 修正为逐头约定。

### distilgpt2 权重下载
- `data/llm/distilgpt2/`（340MB）：`pytorch_model.bin`（352,833,716B）+ config +
  BPE tokenizer（vocab.json/merges.txt/tokenizer.json）。
- **下载坑**：huggingface.co 连不通；`hf-mirror.com` 镜像可用，但 `snapshot_download` 的
  大 LFS 文件走 xethub CAS 报 401 → 改用 `/resolve/main/<file>` 直连镜像下载。

---

## 2026-08-20 — 阶段5a：Transformer 推理算子 + 小 GPT 前向对拍

### 阶段5a：Transformer 核心算子（CPU）
- `infer/ops.myp` 新增 3 个 Transformer 算子：
  - `gatherRows`（embedding 查表，token id → 词向量行）；
  - `rmsNorm`（RMSNorm，`y = x/sqrt(mean(x²)+eps)·γ`，按特征维/序列列）；
  - `attention`（缩放点积注意力：`score=Σ q·k/√dh` + 因果掩码 → softmax(j) →
    `out=Σ p·v`；独立 `scOff` 暂存 [S,S] 分数；全序列、无 KV cache）。
- `infer/runtime.myp` 新增 opKind 61/62/63（RMSNorm/Attention/GatherRows）
  + `add*` 方法 + `run()` 分发（eps 以 F32 位型存 opP2_）。
- 新目录 `llm/`：`llm/make_gpt_smoke_ref.py`（numpy 参考）+ `llm/transformer_smoke.myp`。

### 验证：小 GPT 前向与 numpy 参考精确对拍
- 2 层小 GPT（V=32, D=64, H=4, dh=16, S=16, ffn=128，随机权重）：
  Embedding → [RMSNorm → QKV → attention(因果) → 残差 → RMSNorm → FFN(silu)
  → 残差]×2 → LM head → logits[V,S]。
- `transformer_smoke.myp` 用 `InferOps` 直连内核拼装（不经图），与
  `make_gpt_smoke_ref.py` 导出的参考 logits 逐元素对拍：
  **max diff = 1.49e-07（float32 级精确）→ `TRANSFORMER SMOKE OK`**，
  双编译器（mypc/myp_self）一致。
- 布局约定：激活 [特征, 序列]（行=特征，列=token），dense/attention/rmsNorm 一致；
  QKV 用单个 [3D,D] Gemm，q/k/v 按行偏移取 [0,D)/[D,2D)/[2D,3D)。

### 后续（待续）
- 5b：KV cache 增量 decode（滑动窗口）→ 真生成文本；
- 5c：真实小模型权重（GPT-2 等转 ONNX）+ tokenizer；
- 5d：ONNX 图路径接入（inferShapes/buildRuntime 的 RMSNorm/Attention/Gather 节点）；
- 5e：GPU attention 内核。

---

## 2026-08-20 — 阶段3d + 阶段4a/4c：GPU 训练闭环 + 3D 反向 + Dice loss

### 阶段3d：GPU 训练（backward 内核 + 显存泄漏修复 + 持久化 arena）
- `infer/gpu_ops.myp` 新增 8 个 GPU backward 内核：`bwdDense`(3 内核)/`bwdRelu`/
  `bwdSigmoid`/`bwdAdd`/`softmaxCE`/`update`/`bwdConv`(db/dW/dX 3 内核，dX 逆映射
  公式)/`bwdMaxPool`(重算 argmax)。线程逐元素 + 串行归约、**无原子**。
  `runtime.myp` `runGpu()` 反向分发（opKind 50-57，trainMode 门控）。
- **显存泄漏根因（跨分项目，`src/runtime/runtime_gpu.c`）**：`myp_gpu_destroy_kernel`
  只 free 宿主结构、**从不 `cuModuleUnload`**，每次 launch 的模块显存永不回收 →
  训练显存线性暴涨。修复：按 (PTX,name) 缓存 kernel 模块（进程生命周期只加载一次）
  + 未缓存路径 `cuModuleUnload` + load 入口 `cuCtxSetCurrent`。复现/回归
  `train/gpu_leak_test.myp`（9000 launch 显存平台化）。
- **持久化设备 arena**（`infer/runtime.myp` + `train/cnn_train.myp`）：
  `gpuPersistentStart()/gpuPersistentEnd()/markGpuSync(tid)`——arena 一次驻留显存，
  每步只增量上传 `setFlat` 置脏输入（data/label）+ 下载标记输出（loss/prob）。
  旧 runGpu 每样本整块 16MB H2D/D2H → GPU 训练比 CPU 慢 ~5 倍；修复后
  **GPU 120s→13.4s**，反超 CPU（23.2s）~1.7×，loss 轨迹与 CPU 完全一致，
  acc 88%（1000 样本子集）/ 96%（全量 30000）。
- 详细：`train/README.md` 阶段3d 章节。

### 阶段4a：3D 反向（CPU）
- `infer/ops.myp` 新增 `bwdConv3D`（9 重循环一次累加 dW/dX + db，非对称 padding
  pdt/pdb/pt/pb/pl/pr + 膨胀 dd/dh/dw）、`bwdMaxPool3D`（重算 argmax 路由）、
  `bwdAvgPool3D`（cip 分母均摊）。
- `infer/runtime.myp` opKind 58/59/60 + `run()` CPU 分发（Conv3D 假定膨胀=1、
  group=1，kd/kh/kw 取 w 张量形状）；`infer/graph.myp` `buildReverseGraph`/
  `buildRuntime` 支持 Conv3D/MaxPool3D/AveragePool3D 反向节点（复制 3D 参数）。
- **数值对拍** `train/conv3d_grad_check.myp` → `CONV3D GRAD CHECK OK`
  （bwdConv3D 有限差分 + bwdMaxPool3D 重建 Σx·dX=Σy + bwdAvgPool3D 差分）。
- **端到端** `train/3d_cnn_train.myp` + `tools/make_3d_cnn_onnx.py`（合成 8³ 两分类）
  → loss 0.159→0.003、**acc 100%**（双编译器 mypc/myp_self 均 `3D CNN TRAIN OK`）。
- **框架 BUG 修复**：`graph.myp` Flatten 5D 形状推断漏乘深度维 `shD4`（5D 输入被
  拍成 3 维 → 下游 Gemm 读越界、label/loss 区被覆盖 → loss 恒 0）。修复按 `shR5_`
  连乘 `shD4`。

### 阶段4c：Dice loss（分割）
- `infer/ops.myp` `diceLoss`：`L = 1-(1/C)Σ_c 2Σ(p·y)/(Σp+Σy+eps)`，反向
  `dp = -(2/rows)(y·B-A)/B²`。
- **数值对拍** `train/dice_grad_check.myp` → `DICE GRAD CHECK OK`（~1e-5 吻合）。

### 回归
- 2D CNN 训练 88% 不变；GPU 推理 `bn_main`（MYP_GPU=1）BN OK 无回归；
  自举编译 3D 训练跑通（acc 100%）。

---

## 历史阶段（0–3）
- 阶段0/1/2/3 的变更记录见 `train/README.md`（XOR → MNIST MLP → Graph IR 反向
  → CNN 反向）与 `docs/`（设计/GPU 范式）。
