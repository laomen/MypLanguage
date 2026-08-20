#!/usr/bin/env python3
"""bench_gpu_torch.py — 官方 transformers 引擎在 RTX 2070 SUPER 上跑 Qwen2-0.5B-Instruct 的基准。

与 MYP GPU 版（~50 tok/s decode）对照的"官方引擎"参考。逐 token 手动前向计时
（每步 1 次前向 + sync，与 MYP KV-cache decode 同口径）：
  1) prefill 一次前向 26 tok
  2) decode：逐 token 前向（KV cache 累计），取稳态均值

用法：
  deeplearning/infer/tools/onnxvenv/bin/python deeplearning/llm/bench_gpu_torch.py
"""
import os
import sys
import time
import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

torch.set_grad_enabled(False)
BASE = os.path.dirname(os.path.abspath(__file__))
MODEL = os.path.normpath(os.path.join(BASE, "..", "data", "llm", "qwen2-0.5b-instruct"))

# 精度：默认 fp32（与 MYP 同口径）；--bf16 可跑 bf16 对照
DT = torch.float32
if "--bf16" in sys.argv:
    DT = torch.bfloat16

# 批量：N 份相同 prompt（与 MYP qwen2_gpu_batch 同口径，测聚合吞吐）
BATCH = 1
if "--batch" in sys.argv:
    BATCH = int(sys.argv[sys.argv.index("--batch") + 1])

def run():
    print(f"torch {torch.__version__} cuda={torch.cuda.is_available()} dev={torch.cuda.get_device_name(0)}")
    tok = AutoTokenizer.from_pretrained(MODEL)
    model = AutoModelForCausalLM.from_pretrained(MODEL, dtype=DT).to("cuda")
    gb = model.num_parameters() * (2 if DT == torch.bfloat16 else 4) / 1e9
    print(f"loaded {model.num_parameters()/1e6:.0f}M params {'bf16' if DT==torch.bfloat16 else 'fp32'} ({gb:.2f}GB weights on GPU)")

    msgs = [{"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "What is the capital of France?"}]
    ids = tok.apply_chat_template(msgs, tokenize=True, add_generation_prompt=True,
                                  return_tensors="pt")["input_ids"].to("cuda")
    n_ctx = ids.shape[-1]
    print(f"prompt: {n_ctx} tokens, batch={BATCH}")

    # 批量：复制 N 份相同序列
    if BATCH > 1:
        ids = ids.expand(BATCH, n_ctx)

    # ---- prefill：一次前向 ----
    with torch.inference_mode():
        torch.cuda.synchronize(); t0 = time.perf_counter()
        model(ids)
        torch.cuda.synchronize(); dt = time.perf_counter() - t0
    print(f"prefill: {n_ctx} tok in {dt*1000:.1f}ms -> {BATCH*n_ctx/dt:.1f} tok/s(agg)")

    # ---- decode：带 KV cache 逐 token 前向（真 decode，与 MYP 同口径）----
    n = 120
    times = []
    with torch.inference_mode():
        # prefill：一次前向建 KV cache
        out0 = model(ids, use_cache=True)
        past = out0.past_key_values
        cur = ids[:, -1:]  # [BATCH, 1]
        # warmup 5
        for _ in range(5):
            lg = model(cur, past_key_values=past, use_cache=True)
            past = lg.past_key_values
            nxt = lg.logits[:, -1].argmax(dim=-1).unsqueeze(1)  # [BATCH, 1]
            cur = nxt
        torch.cuda.synchronize()
        # measure 逐 token decode
        for i in range(n):
            torch.cuda.synchronize(); t0 = time.perf_counter()
            lg = model(cur, past_key_values=past, use_cache=True)
            torch.cuda.synchronize(); dt = time.perf_counter() - t0
            times.append(dt)
            past = lg.past_key_values
            nxt = lg.logits[:, -1].argmax(dim=-1).unsqueeze(1)
            cur = nxt
    steady = times[5:]
    avg = sum(steady) / len(steady)
    agg = BATCH / avg
    print(f"decode(KV-cache): 稳态 {len(steady)} step, avg {avg*1000:.2f}ms/step -> {agg:.1f} tok/s (aggregate, batch={BATCH})")
    print(f"  per-seq equiv: {1.0/avg:.1f} tok/s  (first 5: {[f'{t*1000:.1f}' for t in times[:5]]} ms/step)")

if __name__ == "__main__":
    run()
