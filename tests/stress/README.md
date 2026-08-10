# tests/stress — 协程 / 并发压力测试

针对 MYP 运行时（ucontext 协程、通道、异步 I/O、线程池）的**压力测试**：用远超常规
测试的负载压垮运行时，寻找**崩溃、内存泄漏、死锁、数据竞争、协程/句柄泄漏**。

> 与 `bench/` 的区别：bench 测量**性能**（同算法对拍 C++/Go，取最小 ms）；
> stress 关注**稳定性**——只求在高压下不崩、不漏、不死锁、结果正确。
>
> 与 `tests/` 常规回归的区别：压测负载重、含时序数据（ms/worker 数），
> 故**独立于 `run_tests.sh`**，按需手动运行。

## 运行

```bash
bash tests/stress/run_stress.sh                # 全部（-O2）
bash tests/stress/run_stress.sh coro_flood     # 只跑指定项
TSAN=1 bash tests/stress/run_stress.sh         # ThreadSanitizer 查数据竞争
ASAN=1 bash tests/stress/run_stress.sh         # AddressSanitizer 查内存错误
```

退出码 0=全部通过，1=有失败。

## 测试清单

| 测试 | 压什么 | 负载 | 验证 |
|------|--------|------|------|
| `coro_flood` | 协程**海量创建/销毁**（自然回收 + 强杀 `destroy` 两条路径） | 6 波 × 3000 × 2 ≈ 3.6 万个 @coro（stack=64KB） | `Coro.count()` 每波归零、无泄漏 |
| `coro_switch_storm` | **上下文切换吞吐/稳定性**（ucontext swapcontext） | 200 协程 × 2 万次 await = 400 万次挂起/恢复 | 全部结束、累加和精确、count 归零 |
| `channel_stress` | **通道多生产者/多消费者**（含容量 1 强制 ping-pong） | 4 产 × 4 消 × 5000 值，cap=1 / cap=8 两轮 | 无死锁、Σrecv == 理论值、count 归零 |
| `async_io_stress` | **异步 socket**（@thread 服务器 + 协程 `await recvAsync`） | 32 个 loopback TCP 客户端并发 | 全部收到正确 payload、无挂起 |
| `parallel_stress` | **@parallel for + Atomic 高压竞争**（精确整型累加） | 8 轮 × 20 万迭代，含 worker 分布检测 | Σ 精确 == n、≥2 worker 真正并行 |

## 建议流程

```bash
# 常规
bash stress/run_stress.sh

# 数据竞争专项（并发/通道/并行最该跑）
TSAN=1 bash stress/run_stress.sh channel_stress parallel_stress coro_flood

# 内存错误专项
ASAN=1 bash stress/run_stress.sh coro_flood coro_switch_storm

# 多次重复以暴露偶发问题（配合 timeout 外的循环）
for i in 1 2 3 4 5; do bash stress/run_stress.sh || break; done
```

> TSan 对 ucontext 协程会打印已知警告（makecontext/swapcontext 限制），
> 属于误报，关注真正的 `WARNING: ThreadSanitizer: data race` 行。
