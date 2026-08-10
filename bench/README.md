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
| `sha256` | 密码哈希：uint32 位运算 + 64 轮压缩（原生 rol） | 4MB 消息 |
| `quicksort` | 递归快排（Lomuto 分区）+ 随机数组比较/交换 | 80 万元素 |
| `nqueens` | N 皇后回溯（对角数组 O(1) 冲突检查，类方法递归） | N=14（365596 解） |
| `bst` | 二叉搜索树：数组节点插入 + 中序遍历（指针追逐） | 26 万随机键 |
| `dijkstra` | 稠密图单源最短路 O(V²)（无堆，邻接矩阵走查） | 4096×4096 |
| `base64` | Base64 编码：字符映射 + 位打包（perlbench 类字符串处理） | 8MB 字节流 |
| `alphabeta` | 极小极大 + α-β 剪枝：递归游戏树搜索（deepsjeng 类） | B=6, D=14 |
| `spmv` | 稀疏矩阵×稠密向量：CSR 随机 gather（cactuBSSN 类） | 65536 行 × 64 非零 |

每个二进制打印 `verify <值>` 和 `ms <毫秒>` 两行；脚本取多轮最小 ms、校验两语言
verify 一致（浮点容差 1e-3）、输出比值表。

## 解读指南

- 纯计算项（sieve/matmul/nbody/mandelbrot）用**原始类型数组**，不产生 ARC 开销，
  比值主要反映代码生成质量（循环、数组、内联、向量化）。
- `hashmap` 一项 MYP 的 `HashMap<K,V>` 是**纯 MYP 泛型类**（线性探测），含类实例
  ARC 成本，不能和 `std::unordered_map` 直接比 CPU 速度——它衡量的是"MYP 里自己写
  泛型容器"的代价。
- **`gol` 的教训**：MYP `-O2` = LLVM O2 直接做循环展开/向量化；g++ `-O2` 不展开
  gol 的邻居求和，慢 ~2.7x，`-O3` 才持平。所以 C++ 端用 `-O3` 是对齐同一优化强
  度、避免"假赢"。（下标窄整数零扩展修复后 MYP gol 165→~120ms，反超 C++ ~165ms，
  见下文 bug 修复。）
- **`astar` 曾是 0.57，修复后 1.00+**：根因是 MYP 版在**每个扩展节点的热点循环里
  `new int[4]`×2 分配方向数组**（C++ 是函数级栈数组）。把方向表提出主循环后
  21ms→11ms，反超 C++。教训：MYP 没有栈上小数组，热点循环里别分配数组。
- **`sha256` 曾 0.56，加 `uint32` 类型后 0.91**：旧版用 `long` 模拟 uint32，LLVM
  认不出 i64 上的 32 位旋转（移位量之和 32≠64），6 个 rotate 各 3 条 + 状态溢出到
  栈。MYP 新增 `uint32`/`uint`（含 `u` 字面量后缀、逻辑右移、无符号除/比较、
  uint→long ZExt 拓宽）后，`(x>>n)|(x<<(32-n))` 被识别为原生旋转（`rol`），
  32ms→22ms，比值 0.56→0.91。剩余差距是 LLVM 贪心分配器把 8 个状态变量溢出到栈
  （gcc 全寄存器），属分配器差异。
- **递归/数据结构批（quicksort/nqueens/bst/dijkstra）**：quicksort（递归调用+
  比较交换）、bst（数组节点指针追逐+中序遍历）、dijkstra（稠密 O(V²) 最短路）均
  与 C++ 持平（0.95~1.08）；nqueens（类方法递归+回溯）C++ 快 ~10%（0.91）。说明
  MYP 的递归调用/数组索引/循环在这些负载上已与 g++ -O3 相当。
- **SPEC 风格批（base64/alphabeta/spmv）**：spmv（随机 gather 稀疏乘）MYP 反超
  （1.12）；alphabeta（递归搜索+剪枝）C++ 快 16%（0.86）；base64（字符/字节处理）
  曾 0.58（MYP 无强转填不了 `uint8[]`，只能 `long[]` 8 倍内存）——加显式转换
  `uint8(x)` 后改用 `uint8[]`，0.58→0.81。
- 想让 C++ 更强可加 `-march=native`（脚本里可取消注释）。
- 若某项 `verify` 不一致，说明两语言算法/数据布局有差异，**该行比值无效**。
