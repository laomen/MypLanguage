# bench/compiler — 编译器自身性能基准

测量 MYP 编译器 `mypc` 对规模源码的**完整编译时间**（外部高精度时钟）、**分阶段耗时**
（`MYP_TIMING=1`：load / lexer / parser / imports / sema / eval / codegen）与**峰值 RSS**
（GNU time），并按 N / 2N / 4N（P1 含 8N）规模检查复杂度斜率，用于防止编译器性能回退。

设计对应 `docs/testing_benchmark_roadmap.md` 第五节《需要新增的编译器性能基准》。

## 文件

- `gen.py` — 生成 P1..P7 规模 MYP 源码（输出到 stdout）
- `run.sh` — 基准运行器
- `README.md` — 本文档

## 用法

```bash
bash bench/compiler/run.sh              # 默认 P1..P7，每规模 3 轮取中位数
bash bench/compiler/run.sh P1 P4        # 只跑指定项
MYPCC=./build/mypc ITERS=5 bash bench/compiler/run.sh P2
SCALES="500 1000 2000" bash bench/compiler/run.sh P3   # 自定义规模档
JSON=bench/compiler/result.json bash bench/compiler/run.sh   # 输出 JSON
bash bench/compiler/run.sh --check            # 跑全量并与 baseline.json 比对
CHECK_TOL=1.5 bash bench/compiler/run.sh --check   # 自定义回退容差（默认 x1.3）
```

- 默认规模档：`1000 2000 4000`；P1 额外含 `8000`。
- 每规模先 warmup 编译一次，再测 `ITERS`（默认 3）次取中位数（ms），front(ms) 为
  parser+sema+eval+codegen 合计，RSS(KB) 为峰值常驻集中位数。
- 斜率列 `2N/N`：耗时比。> `SLOPE_TOL`（默认 3.0）判定为疑似超线性并打印警告（退出
  码仍为 0；可用 `SLOPE_TOL=…` 覆盖阈值）。
- 任一基准任一规模编译失败 → 退出码 1。
- `--check`：与 `baseline.json` 逐项比对 total_ms，任一项 > 基线 × `CHECK_TOL`（默认
  1.3）判定为回退 → 退出码 1（用于 CI / 性能回归门禁）。
- 源码与编译产物均在临时目录，运行结束自动清理。

## 基准项（对应 roadmap 5.2）

| 项 | 度量 | 说明 |
|----|------|------|
| P1 | 类数量 × 裸属性读取 | N-1 无关类 + 目标类方法内读裸属性 N 次 |
| P2 | 接口数量 × 接口方法调用 | N-1 无关接口 + 目标接口 + N 次接口方法调用 |
| P3 | 接口数量 × 接口变量声明 | N-1 无关接口 + main 内声明 N 个目标接口变量 |
| P4 | struct 数量 × 字段读取 | N-1 无关 struct + N 次目标 struct 字段读取 |
| P5 | enum 数量 × variant 构造 | N-1 无关 enum + N 次目标 enum variant 构造 |
| P6 | 类数量 × 方法调用 fallback | N-1 冲突类 + 已知 action/static/function:/new 链式调用 |
| P7 | 泛型实例数量 | 同一 `Box<T>` 模板 × N 个不同 struct 实参 + N 个泛型函数调用 |

## 当前基线（release `build/mypc`，P1-P6 于 commit `be087cd` 测得，P7 为修复后新设计
重测，ITERS=3，2026-08-11）

| Bench | N | total(ms) | front(ms) | RSS(KB) | 2N/N 斜率 |
|------:|---:|---:|---:|---:|---:|
| P1 | 1000 | 92 | 53 | 43 074 | — |
| P1 | 2000 | 187 | 139 | 51 896 | 2.03 |
| P1 | 4000 | 484 | 413 | 70 702 | 2.59 |
| P1 | 8000 | 1 486 | 1 385 | 107 172 | **3.07 超线性** |
| P2 | 1000 | 82 | 45 | 53 862 | — |
| P2 | 2000 | 136 | 89 | 71 144 | 1.66 |
| P2 | 4000 | 257 | 187 | 107 514 | 1.89 |
| P3 | 1000 | 163 | 118 | 76 400 | — |
| P3 | 2000 | 375 | 316 | 116 112 | 2.30 |
| P3 | 4000 | 1 014 | 969 | 196 542 | 2.70 |
| P4 | 1000 | 59 | 22 | 39 880 | — |
| P4 | 2000 | 86 | 42 | 45 916 | 1.46 |
| P4 | 4000 | 142 | 83 | 57 882 | 1.65 |
| P5 | 1000 | 98 | 58 | 45 260 | — |
| P5 | 2000 | 211 | 160 | 56 306 | 2.15 |
| P5 | 4000 | 569 | 496 | 76 354 | 2.70 |
| P6 | 1000 | 329 | 273 | 72 362 | — |
| P6 | 2000 | 925 | 842 | 111 620 | 2.81 |
| P6 | 4000 | 2 967 | 2 833 | 189 626 | **3.21 超线性** |
| P7 | 1000 | 820 | 752 | 108 674 | — |
| P7 | 2000 | 2 437 | 2 364 | 180 586 | 2.97 |
| P7 | 4000 | 8 673 | 8 510 | 325 198 | **3.56 超线性** |

### 基线解读

- **P1** 与 roadmap 历史基线一致（8000 ≈ 1 486 ms vs 历史 1 430.6 ms），8000 处超线性，
  印证 CodeGen 对每次裸属性读取仍扫描全部类——建立 class-name 索引后应回落。
- **P2** 近线性（≤1.89），接口方法调用已受益于 Sema+CodeGen 精确方法索引优化。
- **P3** 仍偏超线性（2.70），热点为 CodeGen 接口类型判定逐变量扫全部接口。
- **P4** 线性（1.65），struct 字段读取缓存优化已生效（历史 5.4 倍提升已固化）。
- **P5** 趋势超线性（2.70），对应计划中的 enum-name/variant-name 缓存。
- **P6 / P7** 明显超线性（3.21 / 3.56），对应方法解析 fallback 全类扫描与泛型实例线性
  查找（O(N²)）——roadmap 预测的两处优化点。修复 struct 泛型后 P7 改用同一模板多实
  例设计，O(N²) 斜率更显著（4000 → 8.67 s）。

## 如何证明"编译器性能没有问题"

性能无法用单个数"证明"，但从四个维度可系统论证；任一项不合格即为问题：

### 1. 绝对速度可接受（用户可感知）
真实负载全部毫秒级：仓库最大真实源文件仅 25 KB（`stdlib/collections.myp`），编译
**0.03 s**；`examples/raytracer.myp` 0.05 s。即使用户工程达到 8,000 类（P1 极端规模）
也只 1.5 s。对比 C++/Go/Rust 编译器同量级输入，MYP 处同一量级。

### 2. 复杂度曲线符合预期（无隐藏 O(N²)）
- **应线性的路径线性**：P2 接口调用（2N/N≤1.89）、P4 struct 字段读（≤1.73）→ 无隐
  藏超线性。
- **超线性路径是"已知且已文档化"的**：P1/P3/P5/P6/P7 的超线性均为 roadmap 明确列
  出的优化点（class-name 索引 / 接口类型判定 / enum 缓存 / 方法 fallback / 泛型实例
  线性查找）。超线性 ≠ 意外，未出现在 roadmap 之外的路径即无问题。

### 3. 无回退（随时间可验证）
`baseline.json` 提交在仓库内；`bash bench/compiler/run.sh --check` 与基线逐项比对，
任一项中位数 > 基线 × 1.3 即退出码 1。本机实测同基线波动仅 **0.94–1.06**（warmup +
3 轮中位数 + 外部时钟），可复现、可区分真实回退与噪声。

### 4. 正确性未因性能牺牲 + 内存有界
- 全量回归 release/ASAN **233/233** 通过（含新增 generic_struct 用例）。
- 峰值 RSS 随规模近似线性（P1：43→52→70→107 MB @ 1000→8000），无失控内存增长。

### 5. 复现约束
性能测量受机器/负载/频率影响，`--check` 应在同一机器同一 CPU governor 下做相对比对；
`baseline.json` 记录 `commit` 与 `runs` 以便追溯。

## 已知发现（bench 触发，已修复）

**泛型类/泛型函数以 struct 类型做实参时链接失败**（bench P7 原始设计触发）：

```myp
struct T0 { int x; }
class Box<T> { action: T get() { return v; } property: T v; }
int main() { Box<T0> b = new Box<T0>(); T0 v = b.get(); return 0; }
```

- 现象：泛型类 `undefined reference to 'Box_get'`；泛型函数 `LLVM verify failed`。
- 根因：**`Sema::typeName` 缺少 `TypeKind::Struct` 分支** → 返回 "unknown"。sema 给
  单态化实例类命名 `Box_unknown_inst`（定义 `Box_unknown_inst_get`），而调用点 codegen
  的 `mangleConcreteTypeNode` 正确算出 `Box_T0_inst`（`Box_T0_inst_get`）→ 符号失配。
- 修复：`sema_expr.cpp` `typeName` 补 `case TypeKind::Struct: return type.class_name;`。
  泛型类/函数以 struct 实参即可正常编译链接与运行。
- 回归保护：`tests/@test/generic_struct.myp`（Box<Point> 属性存/取、id<Point> 传递、
  多 struct 实参独立实例，7 断言）。
