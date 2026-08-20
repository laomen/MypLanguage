# DeepLearning 训练（train/）

MYP 语言的深度学习**训练**模块（推理框架在 `../infer/`）。路线图：XOR 冒烟 →
MNIST MLP → Graph IR 反向图 → CNN/GPU。

## 文件

```
deeplearning/train/
├── mnist_reader.myp   # 纯 MYP 的 MNIST idx 读取器（train/test 共用，像素 /255）
├── mnist_train.myp    # 阶段1：MNIST MLP 手写反向训练（784→64→10 + 动量 SGD）
├── xor_train.myp      # 阶段0：XOR 手写反向冒烟（MLP 2-4-1 sigmoid）
├── graph_train.myp    # 阶段2：图级反向训练（Graph.buildReverseGraph，任意 ONNX 图）
└── grad_check.myp     # 阶段2：数值梯度对拍（验证图级 backward 正确性）
```

## 阶段0：XOR 冒烟

`examples/deeplearning/train/xor_train.myp`：MLP 2-4-1（sigmoid）+ MSE +
逐样本 SGD，手写前向/反向/更新。验证「MYP 里前向 + 链式反向 + SGD」闭环可行。
双编译器（mypc / myp_self）都打印 `XOR TRAIN OK`（loss 0.134→0.0007，4/4 全对）。

```bash
./build/mypc examples/deeplearning/train/xor_train.myp -o /tmp/xor_train --stdlib stdlib && /tmp/xor_train
```

## 阶段1：MNIST MLP（闭环验证）

`mnist_train.myp`：784→64→10（ReLU 隐层 + SoftmaxCrossEntropy）+ 动量 SGD
（lr 0.1→0.03→0.01，momentum 0.9，batch 128，15 epochs，30000 训练样本）。

```bash
# 训练（须在 examples/ 下，数据路径相对 examples/）
./build/mypc examples/deeplearning/train/mnist_train.myp -o /tmp/mnist_train --stdlib stdlib
cd examples && /tmp/mnist_train
# → 每 epoch 打印 acc，最终 MNIST TRAIN OK（~97%）
```

**闭环验证**（训练 → ONNX → 纯 MYP 推理，精度应一致 ~97-99%，远超旧 78%）：
```bash
cd examples
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_mnist_mlp_onnx.py
# → 用新 mnist_weights.bin 重建 mnist_mlp.onnx（W1[64,784]/B1[64]/W2[10,64]/B2[10]）
cd .. && ./build/mypc examples/deeplearning/infer_tests/onnx_main.myp -o /tmp/onnx_main --stdlib stdlib
cd examples && /tmp/onnx_main
# → ONNX MLP accuracy: 99% (100/100)
```

### 权重导出格式（对接 make_mnist_mlp_onnx.py）
```
头 3×int32 大端： (NIN*NH, NH*NOUT, NH+NOUT)
wh[NH*NIN]  double 小端  = W1（[64,784]，Gemm transB=1 直用）
wo[NOUT*NH] double 小端  = W2（[10,64]）
bh[NH+NOUT] double 小端  = [b1(64) | b2(10)]
```
> 架构固定 784→64→10 以复用导出器；换更大隐层需同步改
> `make_mnist_mlp_onnx.py` 的 `N_HID`。

## 路线图

| 阶段 | 内容 | 状态 |
|---|---|---|
| 0 | XOR 手写反向冒烟 | ✅ |
| 1 | MNIST MLP + 动量 SGD + 闭环 ONNX 验证 | ✅（97%→推理 99%） |
| 2 | Graph IR 反向 pass（buildReverseGraph） | ✅（随机初始化 3%→97%） |
| 3 | conv/maxpool 反向（CPU）→ 小 CNN | ✅（13%→96%） |
| 3d | `@gpu for` backward 内核（GPU）| ✅（泄漏修复 + 持久化 arena，GPU 13.4s / CPU 23.2s / 旧 GPU 120s） |
| 4 | 3D backward（conv3d/maxpool3d/avgpool3d）+ Dice loss | ✅（CPU 内核 + 数值对拍 + 3D CNN 100% + Dice 对拍） |
| 4d | bwdConvTranspose + 3D U-Net（Dice） | ⬜ |
| 4b | GPU 3D backward 内核 | ⬜ |

## 阶段4：3D 反向（CPU）+ Dice loss

`ops.myp` 新增 3 个 3D 反向内核：`bwdConv3D`（9 重循环一次累加 dW/dX + db，
非对称 padding pdt/pdb/pt/pb/pl/pr + 膨胀 dd/dh/dw，dW/dX 先清零）、
`bwdMaxPool3D`（重算 argmax 路由梯度）、`bwdAvgPool3D`（按 cip 分母均摊梯度）。
`runtime.myp` 新增 opKind 58/59/60（BwdConv3D/BwdMaxPool3D/BwdAvgPool3D，参数存
opP*/opX*，Conv3D 膨胀/group 假定 1，kd/kh/kw 取 w 张量形状）；
`graph.myp` `buildReverseGraph` 支持 `Conv3D`/`MaxPool3D`/`AveragePool3D` 节点
（复制 3D 参数），`buildRuntime` 接线 3 个反向节点。

**数值对拍**（`conv3d_grad_check.myp`，`CONV3D GRAD CHECK OK`）：
- `bwdConv3D`：非对称 padding + 有限差分，dw/dx 全匹配、db=27；
- `bwdMaxPool3D`：重建性质 Σx·dX=Σy 精确 + one-hot（non01=0）；
- `bwdAvgPool3D`：有限差分匹配。

**端到端 3D 训练**（`3d_cnn_train.myp`，`3D CNN TRAIN OK`，best acc 100%）：
合成 8³ 体积 2 分类（类0=中心高亮块 / 类1=角落块），模型
`data/onnx/3d_cnn.onnx`（`tools/make_3d_cnn_onnx.py` 生成）：
Conv3D(1→4)→ReLU→MaxPool3D→Flatten→Gemm→Softmax。loss 0.159→0.003 单调下降，
证明 bwdConv3D/bwdMaxPool3D 经图反向链端到端正确。

**BUG（framework）：Flatten 5D 形状推断漏深度维**——`graph.myp` 的 Flatten 只乘
`shD1*shD2*shD3`，5D 输入（NCDHW）拍平漏掉 `shD4` → 256 元素被拍成 64 → 下游
Gemm 读越界、label/loss 区被覆盖（loss=0）。修复：`if (shR5_[a]==1) f = f*shD4_[a]`。

**Dice loss**（`ops.myp` `diceLoss`）：分割损失 `L = 1 - (1/C)Σ_c 2Σ(p·y)/(Σp+Σy+eps)`，
反向 `dp = -(2/rows)·(y·B-A)/B²`。数值对拍 `dice_grad_check.myp` →
`DICE GRAD CHECK OK`（dp 解析 vs 中心差分 ~1e-5 吻合）。

```bash
# 运行（须在 examples/ 下）
./build/mypc examples/deeplearning/train/conv3d_grad_check.myp -o /tmp/conv3d_gc --stdlib stdlib && cd examples && /tmp/conv3d_gc
./build/mypc examples/deeplearning/train/dice_grad_check.myp -o /tmp/dice_gc --stdlib stdlib && /tmp/dice_gc
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/infer/tools/make_3d_cnn_onnx.py
./build/mypc examples/deeplearning/train/3d_cnn_train.myp -o /tmp/3d_train --stdlib stdlib && /tmp/3d_train
```

### 阶段4 要点 / 坑
- 3D 反向的 dW/dX 是**累加**（多输出位置映射到同一输入位置），必须先清零。
- Conv3D 反向参数：膨胀 dd/dh/dw、group 假定 1（U-Net 常用；需扩槽再支持）。
  kd/kh/kw 从 w 张量形状取（w=[yC,C/group,kd,kh,kw] → tD_/tH_/tW_）。
- 3D 反向节点参数复制：nSD3_/nPDT_/nPDB_/nKD3_/nCip_ 等（beginNode 会复位）。
- **5D Flatten 必须连深度维拍平**（见上 BUG），否则下游 Gemm 读越界、损失区被覆盖。

## 阶段3：CNN 反向（CPU）

`ops.myp` 新增 `bwdConv`（7 重循环，一次同时累加 dW/dX + db）+ `bwdMaxPool`
（重算 argmax 路由梯度）；`runtime.myp` 新增 opKind 56/57（BwdConv/BwdMaxPool）；
`graph.myp` `buildReverseGraph` 支持 `Conv`/`MaxPool` 节点（复制卷积/池化参数到
反向节点）；`tools/make_cnn_onnx.py` 生成小 CNN ONNX
（Conv→ReLU→MaxPool→Conv→ReLU→MaxPool→Flatten→Gemm→Softmax，28×28 输入）。

```bash
# 数值对拍（bwdConv 正确性）：L=Σy → dY=全1，中心差分 vs 解析
./build/mypc examples/deeplearning/train/conv_grad_check.myp -o /tmp/conv_gc --stdlib stdlib
cd examples && /tmp/conv_gc   # → CONV GRAD CHECK OK（dw/dx/db 全匹配）

# 小 CNN 端到端训练（28×28 MNIST，随机初始化）
./build/mypc examples/deeplearning/train/cnn_train.myp -o /tmp/cnn_train --stdlib stdlib
cd examples && /tmp/cnn_train  # → 13% → 96%，CNN TRAIN OK
```

**结果**：小 CNN（2×Conv+2×MaxPool+Dense）从随机初始化 **13%→96%**，证明
bwdConv/bwdMaxPool 端到端正确。

### 阶段3 要点 / 坑
- `bwdConv` 的 dW/dX 是**累加**（多个输出位置映射到同一输入位置），必须先清零。
- `bwdConv` 无偏置卷积 `dbOff=-1` 时跳过 db 计算（runtime 传 dbOff=-1）。
- `bwdConv` 里 `oc` 声明不能放在 `if(dbOff>=0)` 块内（后面 dW/dX 循环要用）。
- 卷积/池化参数（sh/sw/pt/pl/dh/dw/group/kh/kw）要**复制到反向节点**（beginNode
  会复位），buildRuntime 才能拿到。
- CNN batch=1 纯 SGD 的 lr 要比 MLP 更小（0.01 起，衰减 0.003）。

## 阶段3d：GPU 训练（backward 内核 + 持久化 arena）

`gpu_ops.myp` 新增 8 个 GPU backward 内核（`bwdDense`/`bwdRelu`/`bwdSigmoid`/
`bwdAdd`/`softmaxCE`/`update`/`bwdConv`/`bwdMaxPool`，线程逐元素 + 串行归约，无
原子）；`runtime.myp` `runGpu()` 增加反向算子分发（opKind 50-57，trainMode 门控）。

**显存泄漏修复（`runtime_gpu.c`）**：旧 `myp_gpu_destroy_kernel` 只释放宿主结构、
从不 `cuModuleUnload`，每次 launch 的模块显存永不回收 → 训练显存持续暴涨直至
OOM。修复：按 (PTX,name) 缓存 kernel 模块（进程生命周期内只加载一次）+ 未缓存
路径 `cuModuleUnload`。泄漏复现 `train/gpu_leak_test.myp`（3000×3 launch 显存平台化）。

**持久化设备 arena（阶段3d 提速）**：旧 `runGpu` 每样本整块 16MB arena H2D→算子
→整块 D2H→释放（batch=1 训练 12k 样本 = 384GB PCIe 往返），GPU 训练反而比 CPU
慢 ~5 倍。新增 `gpuPersistentStart()/gpuPersistentEnd()/markGpuSync(tid)`：
arena 一次驻留显存，每次 `runGpu` 只增量上传 `setFlat` 置脏的输入张量
（data/label）+ 下载 `markGpuSync` 标记的输出（loss/prob）。

```bash
# GPU 训练（须在 examples/ 下；MYP_GPU=1 启用 GPU，MYP_SAMPLES 可缩小训练集）
./build/mypc examples/deeplearning/train/cnn_train.myp -o /tmp/cnn_gpu --stdlib stdlib
cd examples && MYP_GPU=1 MYP_SAMPLES=1000 /tmp/cnn_gpu
# → 88%（1000 样本子集）；全量 30000 样本可达 96%
```

**结果**（1000 样本 × 12 epoch，同一二进制）：

| 后端 | 耗时 | acc |
|---|---|---|
| CPU | 23.2s | 88% |
| GPU 旧（整块往返） | ~120s | 88% |
| GPU 持久化 arena | **13.4s** | 88% |

GPU 持久化后比 CPU 快 ~1.7 倍，且显存全程平台化（无泄漏）。loss 轨迹三端一致
（1.48→0.29）。

## 阶段2：图级反向训练（buildReverseGraph）

`graph.myp` 新增 `buildReverseGraph()` + `optimizeTrain()`；`onnx_loader.myp`
新增 `loadTrain()`。原理：前向图跑完 pass 管线后，反向拓扑遍历为每个可训算子
追加 backward 节点 + 梯度张量（`X#g`），末尾 Softmax 输出处接
`SoftmaxCE(logits, label)→(dlogits, loss)`，权重追加 `Update(W, W#g)`。
运行时 `InferenceRuntime` 新增 opKind 50-55（BwdDense/BwdRelu/BwdSigmoid/
BwdAdd/SoftmaxCE/Update）+ `setTrain()/setLr()`；`ops.myp` 新增 6 个反向内核。
`run()` 一次跑完 前向→反向→更新（trainMode=1）；eval 时 setTrain(0) 跳过反向。

```bash
# 图级训练 mnist_mlp.onnx（默认微调阶段1权重 97%；MYP_RANDOM_INIT=1 从零训练）
./build/mypc examples/deeplearning/train/graph_train.myp -o /tmp/graph_train --stdlib stdlib
cd examples && /tmp/graph_train                    # 微调：97% 保持
cd examples && MYP_RANDOM_INIT=1 /tmp/graph_train  # 从零：3% → 97%
```

**数值梯度对拍**（`grad_check.myp`，证明 backward 正确）：随机初始化 + 中心差分
`(L(+eps)-L(-eps))/2eps` vs 解析梯度 `W#g`，W2 元素 0.008751 vs 0.008821 一致；
Update 验证 `W -= lr·grad` 精确命中。双编译器（mypc/myp_self）都跑通。

### 阶段2 实现要点 / 坑
- **梯度张量必须显式登记形状**（`ensureGrad`）：反向节点 `nodeOut` 只写名字，
  不创建形状 → buildRuntime 注册不到（`buildRuntime wire fail`）。每个用到的
  梯度张量要 `addShapeD` + `setShapeKind`。
- **权重梯度布局镜像基底**（`Kind.FC_G`=7）：buildRuntime 按 `wTrans_[基底]`
  登记，保证 `Update(W, W#g)` 元素对齐（transB=1 权重 [d0,d1]）。
- **Update 输出不能用权重名**：会让 `producerIdx(权重)=Update` → 前向消费权重 +
  Update 依赖梯度 → 环。用独立 dummy 标量张量。
- **topoSort 必须支持多输出**：原 `producerIdx`/递减只认 `effectiveOut`（单输出），
  多输出节点（BwdDense 出 dx/dw/db）的次级输出对 producerIdx 不可见 → Update 入度
  被低估 → 梯度算完前就提前执行（loss 卡 2.433 均匀）。修复：producerIdx 匹配
  `nOut1_-nOut3_` + 递减覆盖全部输出 + 空名守卫。
- **batch=1 纯 SGD 的 lr 必须小**：lr=0.5 会被单个难样本的大梯度（~13）摧毁模型
  （loss 0.001→94→inf）；lr=0.01 稳定收敛。这不是 backward bug，是训练动力学。

## 已知约束 / 坑
- 大整数字面量勿超 int64（`9223372036854775808L`=2⁶³ 会让编译器 `stoll` 崩）。
- 数据路径相对 `examples/`；`mnist_weights.bin` 会被训练覆盖（精度 78%→97%，
  json_tool 的 mnist_main 因 `addAdd(doRelu)` 签名未同步为**预先存在**编译失败，
  与训练无关）。
