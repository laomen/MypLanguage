# deeplearning LLM（阶段5）

在 MYP deeplearning 框架里跑 Transformer / LLM 的探索目录。

## 现状（阶段5，2026-08-21）

**核心算子已落地**（`../infer/ops.myp`，CPU）：
- `gatherRows`（embedding 查表）
- `rmsNorm`（RMSNorm）
- `attention`（缩放点积注意力 + 因果掩码，全序列、无 KV cache；**支持 GQA groups**）
- `attentionCached`（KV cache 增量注意力：单 token Q vs 历史 K/V 缓存，stride/len 分开）
- `layerNorm`（GPT-2 LayerNorm，gamma+beta）
- `gelu`（GPT-2 精确 erf 版，MYP 无 erf 内联 Abramowitz-Stegun 近似）
- `rope`（Llama 真实约定：**逐头**、head_dim 内半配对，cos/sin 表 [dh/2,S]）
- 已接入 `InferenceRuntime`（opKind 61-67 + run() 分发）

**验证**：`llm_ops_check.myp` vs numpy（`make_llm_ops_ref.py`）`LLM OPS CHECK OK`：
LayerNorm=0（精确）、GELU/RoPE/GQA-attn maxDiff≈2.4e-07；双编译器（mypc/myp_self）一致。

**distilgpt2 权重已下载**：`data/llm/distilgpt2/`（340MB）——`pytorch_model.bin` + config + BPE tokenizer。

**KV cache 增量 decode（5b）**：`gpt_generate.myp` —— 逐 token 增量前向（每层 QKV →
追加 K/V 到缓存 → `attentionCached` → 残差 → FFN），缓存满 W 左移（滑动窗口），argmax
采样继续生成。**填充阶段（step<W）增量与整窗重算精确一致（maxd=0）**，证明 KV cache
机制正确；滑窗阶段（step≥W）为标准滑动窗口近似（丢弃最旧 token 改变余下上下文，
Mistral 同款）。`GPT GENERATE OK`，双编译器一致。

**distilgpt2 真实权重前向（5c）**：`extract_distilgpt2.py` 把 ONNX 权重（wte/wpe/6 层/
**最终 ln_f**）转成 `data/llm/distilgpt2_weights.bin`（328MB）；`distilgpt2_forward.myp`
装配 6 层 GPT-2 前向 + 最终 ln_f + lm_head(wte tied) → **`DISTILGPT2 FORWARD OK`**
（argmax=464='The'，maxRelDiff=4.2e-4，与 transformers 权威输出一致）。

**distilgpt2 真实文本生成（5c）**：`prep_distilgpt2_gen.py`（GPT2Tokenizer 编码 prompt +
构建 id→字节 vocab 表 + transformers 贪心参考）→ `distilgpt2_generate.myp`
（KV-cache 增量 decode：LayerNorm+GELU、wte+wpe、最终 ln_f、argmax；MYP 内 BPE 字节级
解码）→ **`DISTILGPT2 GENERATE OK`，token mismatch=0**，生成的文本与 transformers
贪心逐 token 完全一致：

```
Once upon a time of war, the United States was the only country in the world to have
a military presence. The United States was the only country in the world to have a
```

**distilgpt2 GPU 前向（2026-08-21，验证框架不绑定 Qwen2 架构）**：`distilgpt2_gpu.myp`
把 GPT-2（LayerNorm/GELU/普通 MHA 因果注意力/非转置权重，与 Qwen2 的
RMSNorm/SiLU/RoPE/GQA 完全不同）全算子搬 GPU：新增通用 GPU 算子 `GpuInferOps.layernorm`
（mean/var 独立小核 + gamma/beta）、`GpuInferOps.gelu`（erf）、`GpuLLMOps.attention`
（满序列因果注意力，scOff 按 head 独立防竞争）→ 与参考 logits 对拍
**`DISTILGPT2 GPU OK`**（argmax=464='The'，maxRelDiff=4.2e-4，与 CPU/transformers 一致）。
```bash
./build/mypc examples/deeplearning/llm/distilgpt2_gpu.myp -o /tmp/dg2g --stdlib stdlib
cd examples && MYP_GPU=1 /tmp/dg2g
```

```bash
# 冒烟 + 算子对拍 + 生成（须在 examples/ 下）
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/make_gpt_smoke_ref.py
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/make_llm_ops_ref.py
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/extract_distilgpt2.py   # 权重+参考
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/prep_distilgpt2_gen.py  # prompt+vocab+参考
./build/mypc examples/deeplearning/llm/transformer_smoke.myp -o /tmp/tr_smoke --stdlib stdlib && /tmp/tr_smoke
./build/mypc examples/deeplearning/llm/llm_ops_check.myp -o /tmp/llm_ops --stdlib stdlib && /tmp/llm_ops
./build/mypc examples/deeplearning/llm/gpt_generate.myp -o /tmp/gpt_gen --stdlib stdlib && /tmp/gpt_gen
./build/mypc examples/deeplearning/llm/distilgpt2_forward.myp -o /tmp/dgpt_fwd --stdlib stdlib && /tmp/dgpt_fwd
./build/mypc examples/deeplearning/llm/distilgpt2_generate.myp -o /tmp/dgpt_gen --stdlib stdlib && /tmp/dgpt_gen
```

## CPU 推理一键工具（全链路 MYP，含 BPE 编码）

`run_distilgpt2_chat.py` —— 任意 prompt **文本**在 CPU 上跑 distilgpt2 生成（MYP 读
`prompt.txt` → **MYP 内 BPE 编码** → KV-cache 推理 → **MYP 内 BPE 解码** → 文本输出，
Python 只做数据准备/参考生成）：

```bash
# 在 examples/ 下；默认生成 transformers 贪心参考并逐 token 对拍（BPE ENCODE OK + DISTILGPT2 GENERATE OK）
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_distilgpt2_chat.py "The future of AI" 64
# --no-ref：快速模式（不生成参考、删旧参考避免误报，只输出文本）
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_distilgpt2_chat.py "Hello, how are you?" 48 --no-ref
# --talk：交互式多轮对话（全链路 MYP；输入 exit/quit 退出；n_tokens 为每轮生成上限）
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_distilgpt2_chat.py --talk 40
```

- **交互式对话**（`distilgpt2_talk.myp`）：`You:` 读 stdin 一行 → 拼 `User: <输入>\nAssistant:`
  到对话历史（字节缓冲）→ MYP BPE 编码 → KV-cache 生成（遇 EOS=50256 停）→ 解码打印回复
  → 回复追加回历史 → 下一轮。上下文跨轮累积。注意 distilgpt2 是 **base 模型**（非指令微调），
  贪心采样会重复/复读，属正常现象（下一步可加温度/top-k 采样改善）。

- `distilgpt2_chat.myp`：读 `distilgpt2_prompt.txt` → `Bpe.encode`（MYP）→ 生成 →
  解码输出；BPE 编码 vs `prompt_ids.bin`（GPT2Tokenizer）对拍 + 生成 vs `ref_gen_ids.bin`
  对拍。
- `bpe.myp`：**纯 MYP GPT-2 ByteLevel BPE 编码器**（字节编码 → 预分词 → BPE 合并 →
  vocab 查表），表由 `make_bpe_tables.py` 生成（开放寻址哈希，FNV-1a）。
- `bpe_encode_test.myp`：7 个测试文本 vs GPT2Tokenizer 逐 token 对拍 → `BPE ENCODE OK`。
- `prep_distilgpt2_gen.py`：CLI `["prompt"] [n_tokens] [--no-ref]`；写 prompt.txt /
  prompt_ids.bin / gen_cfg.bin / vocab 表（已存在跳过）/ ref_gen_ids.bin（--no-ref 删旧）。
- `distilgpt2_generate.myp`（旧，token-id 路径）：生成数从 gen_cfg.bin 读；参考存在才
  对拍；prompt/generated ids 打印在 `MYP_DG2_DBG=1` 下。

```bash
# BPE 编码器独立对拍
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/make_bpe_tables.py
./build/mypc examples/deeplearning/llm/bpe_encode_test.myp -o /tmp/bpe_test --stdlib stdlib && /tmp/bpe_test
```

## Qwen2-0.5B-Instruct 真实对话（5c5，2026-08-21）—— 指令微调模型

distilgpt2 是 **base 模型**（贪心复读，无法对话）；换 **Qwen2-0.5B-Instruct**（0.5B
指令微调，RoPE+GQA+RMSNorm+SiLU 架构，1.98GB fp32 权重）后 **MYP CPU 上真实多轮对话**：

```
You: Hello, who are you?
Assistant: I am a large language model created by Alibaba Cloud. I'm named Qwen
and I'm available 24/7 to assist you with your queries! How may I help you today?
You: What is the capital of France?
Assistant: The capital of France is Paris.
You: Tell me a joke.
Assistant: Why don't scientists trust atoms anymore? Because they make up everything.
```

一键运行（在 examples/ 下；权重 1.98GB 加载 ~19s，生成 ~0.1s/token CPU）：

```bash
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_qwen2_chat.py "What is the capital of France?"
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_qwen2_chat.py --talk   # 交互式多轮对话
```

**文件**：
- `extract_qwen2.py`：权重提取（1.98GB → `data/llm/qwen2_weights.bin`，含 **q/k/v bias**）
  + chat 模板 prompt ids + transformers 贪心参考（`qwen2_ref_logits.npy` /
  `qwen2_ref_gen_ids.bin`）。参考输出：**"The capital of France is Paris."**。
- `verify_qwen2.py`：numpy 复刻 Qwen2 前向 vs transformers 参考 logits → `RESULT: OK`
  （argmax=785='The'，diff 0.26 = bf16 量化噪声）。
- `qwen2_forward.myp`：24 层 Qwen2 全量前向（RMSNorm/RoPE/GQA/SwiGLU + bias）vs 参考
  logits → **`QWEN2 FORWARD OK`**（argmax=785，maxAbsDiff=0.26 与 numpy 一致）。
- `qwen2_generate.myp`：KV-cache 增量 decode（GQA + RoPE 逐位置 + SwiGLU）→
  **`QWEN2 GENERATE OK`，token mismatch=0**（与 transformers 贪心逐 token 一致）。
- `make_qwen2_tiktoken_tables.py` + `qwen2_tokenizer.myp`：**MYP 内 tiktoken 分词器**
  （Qwen2 预分词正则 + 3 个特殊 token 先切分 + BPE + id→UTF-8 解码），
  `qwen2_tokenize_test.myp` → **`QWEN2 TOKENIZE OK`**（10 prompt + chat 模板特殊 token
  20/20 + decode 往返全过）。
- `qwen2_chat.myp`：端到端（文本 → MYP 组装 chat 模板 → 分词 → 贪心生成 → 解码），
  TOKENIZE/GENERATE 均 mismatch=0 → `QWEN2 CHAT OK`。
- `qwen2_talk.myp`：交互式多轮对话（温度 0.8 + top-k 40 采样，历史跨轮累积）。
- `run_qwen2_chat.py`：一键驱动（`--talk` 交互 / 传 prompt 单轮）。
- `data/llm/qwen2_cossin.bin`：预计算 RoPE cos/sin 表（[32, 32768]，MYP 不直接调
  Math.cos/sin）。

**Qwen2 架构关键点**：
- H=896, L=24, 14Q/2KV 头（GQA groups=7, dh=64）, FFN=4864, V=151936, rope_theta=1e6,
  rms eps=1e-6, tie_word_embeddings。
- **q/k/v 投影带 bias**（q:896, k:128, v:128；o/norm/mlp 无 bias）——早期 numpy 前向漏
  bias 导致 argmax 错（13='.' vs 785='The'），这是调试最久的根因。
- 权重布局 `perLayer=14,912,384`：`ln1,q,qb,k,kb,v,vb,o,ln2,gate,up,down`。
- RoPE 逐头（dh 内 0↔32 配对），cos/sin 预计算表；无位置嵌入（RoPE 处理位置）。
- 分词：Qwen2 tiktoken 正则 + 3 特殊 token（`<|endoftext|>`=151643 / `<|im_start|>`=151644
  / `<|im_end|>`=151645，长度 13/12/10 字节）；chat 模板
  `<|im_start|>system\n...<|im_start|>user\n{in}<|im_end|>\n<|im_start|>assistant\n`；
  EOS=151645。

**Qwen2 踩坑**：
- **q/k/v bias**：模型有 72 个 bias（每层 q/k/v），提取/numpy/MYP 都要带；漏了 argmax 就错。
- **特殊 token 长度**：`<|im_start|>` 是 **12 字节**（含尾部 `|`，非 11）——按 11 写死偏移
  导致切分全失败（BPE 把它当普通文本）。三串长度 13/12/10。
- **特殊 token 先切分再编码**：tiktoken 先用特殊串正则把文本切成段，再对每段独立预分词+
  BPE（不能内联匹配——否则预分词会跨过特殊串，如 `assistant.<|im_end|>` 匹配成
  `.<|_`+... 而 transformers 是 `.` + `<|im_end|>`）。
- **RoPE 表预计算**：直接调 `Math.cos`/`Math.sin` 泛型 + `import ops.myp` 触发编译器 bug
  （LLVM `getArg out of range`）；且表要按真实 POS=32768 生成（曾只生成 4096 导致加载越界
  cos[0,1]=0.9995）。规避：Python 预生成 `qwen2_cossin.bin`。
- **向 ops.myp 加 `silu`/`mul` 方法触发编译器 bug**（整个 ops.myp 编译崩，任何 import 都
  挂）——规避：把 silu/mul 定义在各 .myp 类内私有方法（不动 ops.myp）。见 `tests/BUGLIST.md`
  BUG-046。
- **KV cache 先 RoPE 再存**：增量 decode 必须先把 k 旋转再入缓存（曾先存未旋转 k 再旋转
  本地副本 → 注意力用错 → 生成全偏）。

## CPU 并行加速（@parallel，2026-08-20）— 10 tok/s → ~18 tok/s（1.8x）

- **`par_ops.myp`**：`ParOps.dense/matmul`（@parallel 于输出行）——decode 每 token 瓶颈是
  矩阵乘（24 层投影 + lm_head，~5 亿 MACs），输出行天然并行。零回归：不动共享 ops.myp
  （也避开 BUG-046 上下文）。qwen2_talk/chat/generate 已接入（q/k/v/o/gate/up/down/lm_head
  全走 ParOps；rmsNorm/rope/attentionCached/silu/mul/add 量小保持串行）。
- **实测**（16 线程，Ryzen 7 9700X）：lm_head matmul 151936×896 **47ms→14ms（3.4x）**，
  并行==串行（maxdiff=0）；每 token 总 ~100ms→~54ms（层 41ms + lm_head 14ms）。
- **为什么只 1.8x（内存带宽瓶颈）**：decode 每 token 要把 ~1.95GB 权重全部读一遍
  （层 24×~60MB + lm_head 545MB）。CPU ~30GB/s → ~60ms 内存地板，并行计算填不满带宽。
  lm_head 已贴近内存地板（545MB→14ms）。进一步提速方向：
  - **fp16/bf16 权重**（2B/元素，减半流量 → 约再 1.8x，~33 tok/s）；
  - **GPU**（RTX 2070 SUPER 448GB/s → 权重流 ~4.5ms + 计算，可望 100+ tok/s；但
    rmsNorm/rope/attentionCached/mul 缺 GPU 内核，需补）。
- 计时调试：`MYP_Q2_TIMING=1 /tmp/q2talk` 逐 step 打印 layers/lm_head/total ms。

## GPU 推理（@gpu resident + 异步流，2026-08-21）— 20ms/token（2.7x CPU）

Qwen2-0.5B-Instruct 全链路搬到 GPU（RTX 2070 SUPER 8GB）：文本 → MYP 分词 → 24 层 +
lm_head 全在 CUDA 上跑 → D2H logits → host 采样 → 解码。**`mismatch=0` 与 CPU/transformers
完全一致**。

```bash
# 在 examples/ 下；权重 1.98GB 加载 ~19s + H2D ~0.4s；需 MYP_GPU=1
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_qwen2_gpu.py "What is the capital of France?"
deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/run_qwen2_gpu.py --talk   # GPU 交互式多轮对话
```

- **`gpu_llm_ops.myp`**：Qwen2 GPU 算子（`@gpu for ... resident(a=dev) stream(s)` 异步流）。
  补齐 gpu_ops.myp 缺的 rmsNorm/rope/attentionCached/gatherRows/mul，并给
  dense/matmul/add/silu 提供**流版 + K 分块版**（`qkvC`/`gateupC`/`matmulC`/`matmulCAdd`，
  chunk+reduce 2 核提并行度）；`ropeFused` 把 q/k 的 rope 合一核并顺带写 KV cache
  （K 须 rope 后才缓存）；`swiglu` 融合 silu+mul。
- **`qwen2_gpu.myp`**：单发推理（读 `qwen2_prompt.txt` → chat 模板 → 生成 → 对拍 + 解码）。
- **`qwen2_talk_gpu.myp`**：GPU 交互式多轮对话（温度 0.8 + top-k 40 采样，历史跨轮累积）。
- **`run_qwen2_gpu.py`**：一键驱动（`--talk` 交互 / 传 prompt 单轮）。

**提速关键**（按收益排序）：
1. **权重转置 `[xRows,outDim]`**（加载时一次转置）：GEMV 线程=输出、warp 连续读 →
   合并访存。lm_head 545MB **12ms→1.6ms（7.5x）**；CPU 版权重布局是 `[outDim,xRows]`
   非合并 → ~30x 慢。lm_head 另存转置 wte（`lmOff`，+545MB 显存）。
2. **异步流**：全部内核排队到单一 `GpuStream`，步末 `copyToHostAsync(logits) + gs.sync()`
   一次；默认流会每 kernel `cuCtxSynchronize`（~0.25ms/核 → 216 核/步 = host 瓶颈）。
3. **rmsNorm 内联融合（2026-08-21，收益最大）**：两个 rmsNorm 是 grid=1 单线程核
   （每层 ~145µs），48 个/步 → 融进 qkvC/gateupC 的 chunk 核（每线程 float 重算 rstd，
   消除独立核）→ **20ms→13ms**。float rmsNorm 精度足够（mismatch 仍 0）。
4. **内核融合**：qkv 三投影一核、gateup 两投影一核、reduce 顺带 cachePut V/残差加、
   gateup reduce 顺带 swiglu（同线程归约 gate+up 无竞争）、ropeFused（q/k rope 合一 +
   顺带写 K cache）。每层 15→10 核。
5. **CUDA Graph（默认开）**：逐 step 标量改读设备参数槽 → step 0 捕获整段前向，后续每步
   一次 `exec.launch`。融合后核数减少，graph 11ms < per-kernel 12ms；`MYP_Q2G_GRAPH=0` 回退。

**实测（RTX 2070 SUPER，fp32，mismatch=0）**：GPU **11ms/step（≈91 tok/s）** vs CPU
@parallel **54ms/step**（18.5 tok/s）→ **4.9x**；对照官方 transformers 引擎 fp32 同卡
**120 tok/s**（MYP 已达其 76%）。理论地板 fp32 226 tok/s。

**批量并发（N 路共享权重，2026-08-21）— BN=4 聚合 250 tok/s（单路 2.75x）**

`qwen2_gpu_batch.myp` + `gpu_llm_ops_batch.myp`：每 step 读一遍权重同时推进 N 条序列
（同步 batch：共享 pos，激活/输出按 `[D, N]` feature×seq，KV cache `[L][N][2*kvD*W]`）。
N 份相同 prompt 验证 N 路逐 token 一致（`mismatch=0`）+ 报聚合 tok/s。

```bash
./build/mypc examples/deeplearning/llm/qwen2_gpu_batch.myp -o /tmp/q2gb --stdlib stdlib
cd examples && MYP_GPU=1 MYP_Q2G_BATCH=4 /tmp/q2gb     # MYP_Q2G_GRAPH=0 关图
```

**实测**：BN=1 **11ms/step / 90 tok/s**、BN=2 **12ms/step / 166 tok/s**、BN=4 **16ms/step /
250 tok/s**（33 步 EOS，输出 "The capital of France is Paris."，mismatch=0）。
聚合 = 单路 2.75x，未达理想 4x——batch GEMM 受小核延迟/占用率制约，非纯 DRAM 带宽。

**官方引擎 batch 对照**（`bench_gpu_torch.py --batch N`，同卡同 prompt，真 KV-cache decode）：

| 配置 | MYP | 官方 transformers |
|---|---|---|
| 单路 fp32 | 90 tok/s (11ms) | 120 tok/s (8.3ms) |
| batch=4 fp32 | 250 tok/s (16ms) | **361 tok/s (11.1ms)** |
| batch=4 bf16 | — | 166 tok/s（2070S 无 bf16 硬件，软件模拟反更慢） |

结论：**MYP 批量（250）压过官方单路（120），但官方一开 batch=4（361）就反超我们 44%**——
官方 cuBLAS/tensor core/融合内核更优；MYP 批量赢在权重共享摊薄读取，赢点不是引擎内核质量。
bf16 在 2070S 上是负优化（sm_75 无 bf16 硬件，transformers 软件模拟）。

**批量踩坑 / 优化**（都是实测定位的）：
- **logits 布局**：`matmul` 写 `logOff[o*BN+s]`（vocab-major, batch 内层），argmax 必须按
  stride=BN 读。BN=1 时两种布局重合所以单路正常——这是 BN=4 mismatch=384 的**根因**
  （Layer0 q 四路一致但最终 logits 分歧 → 逐层 bisect 定位到 lm_head 输出布局）。
- **rstdBatch**：batch 下 qkvC/gateupC 每 chunk 线程冗余 896 次 sum2（同 seq ~9728 线程重算
  同一 rstd）→ 单独 `rstdBatch` 小核（grid=N）预计算 rstdOff[si]，chunk 核只读一次。
  实测 gateup **0.416→0.170ms/layer（2.4x）**，全步 23→18ms。这是批量版与单路的本质差异
  （单路 rmsNorm grid=1 慢所以走内联融合；batch 内联反而变冗余重算）。
- **thread 映射 seq 最内层**（`r = o*N+ss`）：同 output 的 N 个 seq 线程相邻，warp 内同地址
  权重读被合并。
- **探索后否决「摊销版」**：把 4 个 seq 展开进线程（4 标量累加器、权重读一次）→ 线程数 ÷4，
  并行度损失抵消带宽节省，实测 2x 更慢（gateup 0.845 vs 0.416）。此 GPU batch GEMM 受延迟
  制约，保持高线程数 + 降冗余计算（rstdBatch）才是正确方向。

**剩余瓶颈**：GPU 侧每 kernel 执行 ~40µs（小 grid 延迟受限，非 launch 开销）。纯
matmul-only 只 6ms/step，小核（rmsNorm/rope/attn/swiglu/reduce ~168 个/步）是主要耗时。

**CUDA Graph 探索（2026-08-21，结论：无提速）**：把逐 step 标量（tok/pos）改为读设备
内存参数槽（`paramTokOff`/`paramPosOff`，graph 内 kernel 结构固定仅参数变），step 0
`captureBegin/captureEnd` 捕获整段前向（315 核），后续每步一次 `exec.launch` 重放。
实测 **graph 19ms/step ≈ per-kernel 20ms/step**——**瓶颈是 GPU 侧执行时间而非 host 派发
开销**，graph 消除 launch 无效。`MYP_Q2G_GRAPH=1` 启用（默认 per-kernel）；两种模式均
mismatch=0。进一步提速只剩「加大小核 grid 并行度」或「单层巨型核 + 原子栅格同步」，均
受 MYP @gpu for 无共享内存/网格同步限制，收益不确定。

**踩坑**：
- q/k/v 投影必须用 **rmsNorm 输出 xn**（不是原始 embed x）——早期写错导致 mismatch=7。
- **K 的 cache 必须 rope 后写**（ropeFused 里写）；V 不 rope 可在 qkv 归约里写。
- cachePut 融合进 qkvC 时局部 `W`（qkv 输出数）会遮蔽 cache 步长 → cache 错位 → 全错；
  cache 步长须单独传参 `stride`。
- graph 模式须读设备参数槽：内核 `int tok = int(a[paramTokOff])`（float 位存 int，<2^24 精确）；
  捕获段内只排内核（无 H2D/D2H），参量 H2D 放捕获前。
- `gpu_rope_test`/`gpu_rms_test` 等隔离测试验证过单个核正确后，端到端 `mismatch=0` 是
  最终回归判据（`QWEN2 GPU OK`）。

## 布局约定（重要）

所有激活张量 **`[特征, 序列]`**（行=特征，列=token，序列沿 cols）：
- `dense`：x[inDim, S] × W[outDim, inDim] + b[outDim] → y[outDim, S]
- QKV 用单个 [3D, D] Gemm，q/k/v 按行偏移取 [0,D) / [D,2D) / [2D,3D)
- `attention` 需要 `scOff` 暂存 [S,S] 分数
- token id 以 float 存 arena（`int(arena[...])` 取回）
- KV cache：`kc/vc[kvD, W]`（行=kv 维，列=位置），每层独立；groups=1 时 kvD=D

## 踩坑记录（distilgpt2 导入）

- **GPT-2 lm_head 前有最终 LayerNorm `transformer.ln_f`（1536 floats）**——MYP forward 和
  旧 numpy 参考都漏了它 → logits 量级爆炸（+134~+277，argmax=262）≠ 真值（argmax=464，
  logits [-52,-45,...]）。加 ln_f 后 MYP=numpy=onnxruntime=transformers 全部一致。
  用户导出的 ONNX **权重 bit 级一致**，但 unfused 1599 节点图 + KV-cache 有 bug（不信任
  其计算结果，只当权重容器）。
- **对拍必须用同一 prompt**：S=6（无 EOS）transformers argmax=198，S=8（含 2 个 EOS）
  argmax=464——prompt 逐 token 不一致会得出假"分歧"。
- **`__myp_io_write_byte` 写当前文件句柄，不是 stdout**（无打开文件时返回 -1）。MYP 里
  向 stdout 输出原始字节的正确姿势：收集进 `ubyte[]`（`out[i] = ubyte(v)`）→
  `Console.writeString(str(b))`（`myp_bytes_to_str` 按字节往返）。
- MYP 无 `byte()` 强转用法，但 **`ubyte(int)` 显式强转**可用（numeric→numeric）。
- **BPE 编码坑（bpe.myp）**：
  - **类属性必须在 `property:` 段**（`int[512] tRows_;`），放类顶部（`action:` 之前）是
    解析错误"expected 'action:'..."——且错误行号会错位到导入方文件（误导排查）。
  - 预分词是 **Python re 交替「第一个匹配的替代」获胜，不是最长匹配**——`'s!` 应切成
    `'s` + `!`（`'s` 在前），不是 `'s!`。MYP 用 if/elif 链复现。
  - merge pair 元素可多字符（如 `ĠColl ider`）→ pair key 用共享字节池 + 开放寻址哈希
    （int16 LE 每码点，pair 间 0xFFFF 分隔），不能用单码点 rank 数组。
  - FNV-1a 用 `long` 算（`& 4294967295L`）避免 int32 溢出语义差异；`0x811C9DC5L`/
    `16777619L` 十六进制字面量可用。
  - 对拍必须同 prompt：MYP 读 `prompt.txt` 自己编码 vs Python 的 `prompt_ids.bin`。

## 路线

| 子步 | 内容 | 状态 |
|---|---|---|
| 5a | Transformer 算子（embedding/RMSNorm/attention）+ 小 GPT 前向对拍 | ✅（1.49e-07） |
| 5a2 | LayerNorm/GELU/RoPE/GQA 算子 + 对拍 | ✅（LLM OPS CHECK OK） |
| 5b | KV cache 增量 decode（滑动窗口生成） | ✅（填充阶段精确一致，GPT GENERATE OK） |
| 5c | distilgpt2 权重导入 + 前向装配 + 真实文本生成 | ✅（FORWARD OK + GENERATE OK，token 级一致） |
| 5c2 | **BPE 编码搬进 MYP**（字节编码+预分词+合并+查表） | ✅（BPE ENCODE OK，7/7 vs GPT2Tokenizer） |
| 5c3 | **端到端 chat**：文本→MYP编码→推理→MYP解码→文本 | ✅（DISTILGPT2 GENERATE OK，全链路 MYP） |
| 5c4 | **交互式多轮对话**（--talk，历史累积） | ✅（全链路 MYP；base 模型贪心会重复） |
| 5c5 | **Qwen2-0.5B-Instruct 导入**：bias 修复 + numpy 验证 + MYP 前向 + KV 生成 + tiktoken 分词 + 交互对话 | ✅（FORWARD/GENERATE/TOKENIZE/CHAT OK + 真实多轮对话） |
| 5e | GPU attention 内核 | ⬜ |

## 现状：为什么不能直接跑现成 LLM

MYP 推理引擎是静态图 + CNN 式算子，跑现成 LLM 需：attention/KV cache/RoPE/RMSNorm/
GQA + 分词器 + 权重格式转换。5a-5c3 已补齐 attention/KV cache/embedding/LayerNorm/
GELU + **BPE 编码/解码（全 MYP）** + distilgpt2 端到端：**任意文本输入 → MYP 编码 →
KV-cache 生成 → MYP 解码 → 文本输出**，全程不依赖 Python 分词（Python 仅数据准备/
参考生成）。8GB Turing（sm_75）可用量级：7B@Q4（llama.cpp/Ollama 现成可跑，与本框架无关）。
