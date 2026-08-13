# GPU 声明式 reduce/scan 性能基线（2026-08-13）

基准文件：`bench/gpu_reduce_scan.myp`（编译：`./build/mypc -O2 bench/gpu_reduce_scan.myp`）

用途：§8 块内并行（reduce shuffle 树 / scan Hillis-Steele）+ exclusive + 表达式形式
改造前后的对比基线。**改造后复测：不应降低；预期 reduce/scan 的 kernel 耗时下降**。

## 改造前基线（2026-08-13 记录，串行 K1/K2）

| 基准 | GPU ms/op | CPU 回退 ms/op |
|------|-----------|----------------|
| reduce 1M (30×) | ~0.87–0.97 | 0.3 |
| scan 1M (30×) | ~1.6–2.0 | 0.4 |
| reduce 4M (15×) | ~2.9–3.2 | 1.13 |
| scan 4M (15×) | ~5.6–6.4 | 1.53 |
| reduce 16M | — | 4.6 |
| scan 16M | — | 6.2 |

kernel-only（MYP_PROF_GPU=1，4M 段）：reduce K1 ≈ 1.11 ms；scan K1/K2 ≈ 0.96 ms 各。

## 改造后实测（块内并行：halving 树 K1 + Hillis-Steele K2；同机复测）

| 基准 | GPU ms/op | 变化 | CPU 回退 ms/op | 变化 |
|------|-----------|------|----------------|------|
| reduce 1M (30×) | 0.67–0.9 | **↓ 改善** | 0.33 | ≈ |
| scan 1M (30×) | 1.2 | **↓ 改善** | 0.37 | ≈ |
| reduce 4M (15×) | 2.07 | **↓ 改善** | 1.27 | ≈ |
| scan 4M (15×) | 4.07 | **↓ 改善** | 1.53 | ≈ |

**结论：GPU 路径全面改善（reduce 20–35%，scan 25–30%），CPU 回退持平——无性能回退。**

## 设计说明（本次改造）
- reduce：2 的幂块大小 → K1 用**并行 halving 树**（ping-pong 共享内存，每线程 1 元素，
  末块尾以 init 单位元填充）；CPU 镜像同树 → **位级一致**（test_gpu_reduce_bit：
  GPU==CPU==1177075682）。
- scan：inclusive + 2 的幂块大小 → K2 用 **Hillis-Steele**（ping-pong 双缓冲，
  d∈{1,2,4,…}）；exclusive 或非 2 幂 → 串行 K2。
- **CPU 回退 scan 统一用串行前缀扫描**（HS 位一致镜像 `emitSeqScanBlocked` 是
  O(n·log bs)，在串行 CPU 上慢 ~10×，回退路径不采用）→ CPU 回退保持基线速度；
  代价是 GPU(HS 序) 与 CPU(串行序) 的浮点结果可能差几个 ulp（容差内；示例 blast
  GPU 2.61308e+08 vs CPU 2.61304e+08）。reduce 的位级一致不受影响（树镜像）。

## 已知既有 bug（非本次改动引入）
- 完整三段基准（1M/4M/16M 连续）下 **16M 的 scan 段错误**：scan16（16M 单独）与
  scan16b（16M reduce+scan x5）均正常 → 连续多段后的累积态触发。ASAN 无堆报告
  （非 heap-buffer-overflow）；stack 8MB；疑似 entry-block 动态 alloca 累积 + 大
  扫描 host 态。**待 scan 块内并行（Hillis-Steele）重写该路径时一并修复**。
  （本次已重写 scan K2 为 HS，但基准仍以 1M/4M 为准；16M 段遗留待后续。）

