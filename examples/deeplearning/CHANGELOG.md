# DeepLearning 训练/推理框架 变更日志（examples/deeplearning）

> 本文件记录 **deeplearning 分项目**（`examples/deeplearning/`）的独立变更，
> 与主仓库编译器 `docs/CHANGELOG.md` 分离（主 changelog 只记编译器/运行时/stdlib）。
> 分项目内的跨分项目运行时配合改动（如 GPU 运行时泄漏修复）也在此记录上下文。

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
