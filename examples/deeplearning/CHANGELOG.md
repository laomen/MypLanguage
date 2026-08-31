# DeepLearning 训练/推理框架 变更日志（examples/deeplearning）

> 本文件记录 **deeplearning 分项目**（`examples/deeplearning/`）的独立变更，
> 与主仓库编译器 `docs/CHANGELOG.md` 分离（主 changelog 只记编译器/运行时/stdlib）。
> 分项目内的跨分项目运行时配合改动（如 GPU 运行时泄漏修复）也在此记录上下文。

---

## 2026-09-01 — 算子拆分 POC：interface 多态分派（阶段4e）

### 背景
`InferOps`（67 个 CPU static 内核）+ `GpuInferOps`（66 个 GPU 内核）由
`InferenceRuntime.run()/runGpu()` 的 **127 个 if/else 分支**按 opKind 分发。用户
提议把算子拆开、用 MYP interface 实现（MYP interface = fat-pointer vtable，手册
§Interface 明言「适合算子模式」，见 `examples/ad.myp`）。

### 接口分派机制（`infer/runtime.myp`）
- 新增 `interface IOp { void run(InferenceRuntime rt, int opIdx); }` + 两张分派表
  `IOp[128] opsCpu_ / opsGpu_`（property，接口数组元素支持）+
  `registerOp(k, op, isGpu)` + 公共访问器（`arenaRef/devRef/trainMode/lrRef/
  tensorRows..W/opAAt..opP8At/opXAt/opReluAt`）。
- `run()`/`runGpu()` 循环开头：`kk=opKind_[i]`，`opsCpu_[kk]/opsGpu_[kk] != null`
  则 `ops_[kk].run(this, i)`，否则 `else if` 回退原 if/else——**增量、零行为变化**。
- `curDev_` 字段：runGpu 每轮存当前设备指针，接口 GPU 算子经 `rt.devRef()` 取 dev
  （原 `dev` 是 runGpu 局部变量，接口方法拿不到，必须存字段）。

### 拆分实现（`infer/op_iface.myp`）
- relu(2)/bwdRelu(51) 四个算子拆成独立类：`CpuReluOp/CpuBwdReluOp`（调
  `InferOps`）+ `GpuReluOp/GpuBwdReluOp`（调 `GpuInferOps` `@gpu for` 内核）。
  每个类 `interface class IOp;` + `void run(rt, i)` 里写原 if/else 分支体（经
  rt 访问器读参数/张量）。`registerIfaceOps(rt)` 注册 4 个算子。
- **验证** `train/op_iface_check.myp`：同一 2 层小图（dense→relu→dense→
  softmaxCE+反向+update）两份 runtime——A 不注册（if/else）、B 注册接口；每步 loss
  + 最终 w1/w2/b1/b2 **逐位 diff=0**（CPU 与 GPU 均 `OP IFACE CHECK OK`），且
  loss 正常下降（0.82→0.58）证明训练闭环仍工作。
- **迁移模式**：其余 ~63 个 opKind 照此搬——每 opKind 一个类，run() 里复制原分支
  体；搬完可整体删 if/else。GPU 内核在接口方法内经 vtable 分派验证可行。
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
