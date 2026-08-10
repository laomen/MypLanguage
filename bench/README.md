# bench — MYP vs C++ 计算效率对比

MYP 走 LLVM 后端，与 C++（clang）同源。这里的每对 `myp/xxx.myp ↔ cpp/xxx.cpp`
用**同算法、同数据、同规模**，唯一差异是前端/语义/代码生成，用来观察 MYP 把多少
优化机会保留给了 LLVM。

> **优化级别契约**：MYP 侧固定 `-O2`（精确映射 LLVM `OptimizationLevel::O2`，即
> clang++ `-O2` 的同一条管道，含循环展开/向量化）。C++ 侧默认 `-O3` 以对齐 LLVM
> O2 的激进程度——因为 g++ 的 `-O2` 明显更保守（`gol` 项在 g++ `-O2` 下要慢
> ~2.7x，`-O3` 才与 MYP 持平，见下）。装好 clang++ 后可用 `CXX=clang++` 直接得到
> 与 MYP 完全同源的后端对比。

## 运行

```bash
bash bench/run_compare.sh [iterations]    # 默认 3 轮取最小 ms
MYPCC=/path/to/mypc bash bench/run_compare.sh
```

## 基准清单

| 基准 | 测什么 | 规模 |
|------|--------|------|
| `sieve` | 字节数组内存带宽 + 分支 + 紧致循环 | N=10⁷ |
| `matmul` | 浮点乘加 + 自动向量化（分块 64 写法，内层 C/B 连续可向量化） | 512×512 |
| `nbody` | 浮点除法 + sqrt + O(N²) 嵌套访存 | 5000 体 × 2 步 |
| `mandelbrot` | 双精度分支密集 + 提前跳出 | 1000×1000, 256 迭代 |
| `hashmap` | 泛型 + 类实例 ARC 成本（vs std::unordered_map） | 10⁶ put/get |
| `tripleloop` | 三层嵌套循环控制 + 整型 ALU（无内存访问） | 300³ 迭代 |
| `raytracer` | 光线追踪：软阴影/玻璃折射/反射 + 2×2 AA | 800×600, depth 3 |
| `gol` | Game of Life：字节网格状态机 + 邻域计数（空间填充走查） | 1024×1024, 60 代 |
| `fft` | 基-2 FFT：位反转 + 蝶形运算（复数、随机访问） | 4096 点, 800 轮 |
| `astar` | A* 寻路：二叉最小堆 + 曼哈顿启发 + 结构化地图 | 512×512 公路网格 |
| `sha256` | 密码哈希：32 位位运算 + 64 轮压缩（long 模拟 uint32） | 4MB 消息 |

每个二进制打印 `verify <值>` 和 `ms <毫秒>` 两行；脚本取多轮最小 ms、校验两语言
verify 一致（浮点容差 1e-3）、输出比值表。

## 解读指南

- 纯计算项（sieve/matmul/nbody/mandelbrot）用**原始类型数组**，不产生 ARC 开销，
  比值主要反映代码生成质量（循环、数组、内联、向量化）。
- `hashmap` 一项 MYP 的 `HashMap<K,V>` 是**纯 MYP 泛型类**（线性探测），含类实例
  ARC 成本，不能和 `std::unordered_map` 直接比 CPU 速度——它衡量的是"MYP 里自己写
  泛型容器"的代价。
- **`gol` 的教训**：MYP `-O2` = LLVM O2 直接做循环展开/向量化；g++ `-O2` 不展开
  gol 的邻居求和，慢 ~2.7x，`-O3` 才持平（≈163ms vs MYP 172ms）。所以 C++ 端用
  `-O3` 是对齐同一优化强度、避免"假赢"。
- `astar`/`sha256` 是 C++ 领先的两项：astar 是二叉堆 + 大量小结构体搬移（MYP 的
  堆/类开销），sha256 是纯 32 位位运算（MYP 用 i64 模拟 uint32，每轮多几次
  `&0xFFFFFFFFL` mask；C++ 原生 uint32 自然环绕）。这是 MYP 该补强的两类场景。
- 想让 C++ 更强可加 `-march=native`（脚本里可取消注释）。
- 若某项 `verify` 不一致，说明两语言算法/数据布局有差异，**该行比值无效**。
