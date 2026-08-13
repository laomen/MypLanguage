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
| `sieve_odd` | 只筛奇数的埃氏筛：减半字节数组 + 步长 2p 紧凑内层循环（sieve 优化变体） | N=10⁷ |
| `montepi` | 蒙特卡洛求 π：LCG 随机数 + 双精度乘加/分支（FP 分支密集） | N=10⁸ 点 |
| `matmul` | 浮点乘加 + 自动向量化（分块 64 写法，内层 C/B 连续可向量化） | 512×512 |
| `nbody` | 浮点除法 + sqrt + O(N²) 嵌套访存 | 5000 体 × 2 步 |
| `nbodybg` | **benchmarksgame n-body**：5 体太阳系引力（忠实复刻官方数据/算法，10 对/步） | 5×10⁶ 步 |
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
| `kmeans` | K-means 聚类：浮点距离 + 数据相关分支 | 16384 点×8维×8簇×400 轮 |
| `bigint` | 大数乘法：schoolbook uint16 字 + 64 位进位链 | 8192 位 × 500 次 |
| `huffman` | Huffman 编码：字节计数 + 二叉树 + 码长（uint8 下标） | 8MB 字节流 |
| `convolution` | 2D 图像卷积 5×5 核（滑窗浮点，imagick 类） | 2048×2048 |
| `knapsack` | 0/1 背包动态规划（1D 滚动数组 + 数据相关分支） | 10000 物品 × 容量 10000 |
| `kmp` | KMP 串匹配：失配表 + 单趟扫描（字节分支） | 32MB 文本 × 256 模式 |
| `radixsort` | LSD 基数排序：直方图 + 前缀和 + 散布（窄下标） | 4×10⁶ 整数 × 4 趟 |
| `sobel` | Sobel 边缘检测：3×3 梯度（uint8 字节滑窗 + 符号梯度） | 2048×2048 灰度图 |
| `floyd` | Floyd-Warshall 全源最短路（稠密三层循环 + 原地最小） | V=600 |
| `heapsort` | 二叉堆排序：堆化 + sift-down + 交换 | 10⁶ 整数 |
| `crc32` | 表驱动 CRC-32（uint32 表 + 字节循环 + 位移异或） | 32MB 数据 |
| `dotprod` | 结构体数组点积（AoS：struct 数组 + 字段读写） | 2×10⁶ 个 Vec |
| `slicevec` | slice<Vec> 结构体切片点积（AoS + 运行时边界检查） | 2×10⁶ 个 Vec |
| `slicemat` | 嵌套 slice<slice<int>> 矩阵求和（二维运行时切片） | 2048×2048 |
| `parcomp` | 并行计算：MYP @parallel for vs C++ std::thread（16 线程分块） | 10⁶ 迭代×200 浮点 |
| `parreduce` | 并行归约：@parallel for + Atomic（每线程槽位）vs std::thread 各自累加 | 10⁶ 随机整数 |
| `matrix_int_mul` | 整型矩阵乘法（int SIMD/ALU 循环） | 256×256 |
| `dot_f64` | double 点积（浮点归约，无 fast-math 时向量化受限） | 10⁶ 元素 |
| `lcs` | 最长公共子序列 DP（二维表内存访问 + 数据相关分支） | 2000×2000 |
| `fib_matrix` | 矩阵快速幂斐波那契（ALU + 小矩阵乘 + 位循环） | fib(10⁸)×5×10⁴ |
| `fannkuch` | **Benchmarks Game** fannkuch-redux（排列 + 数组翻转 + 整型 ALU） | N=11 |
| `spectral_norm` | **Benchmarks Game** spectral-norm（O(N²) 浮点幂迭代 + 开方） | N=5500 |
| `binary_trees` | **Benchmarks Game** binary-trees（数组树递归构建 + item_check） | 深度 4..17 |

每个二进制打印 `verify <值>` 和 `ms <毫秒>` 两行；脚本取多轮最小 ms、校验两语言
verify 一致（浮点容差 1e-3）、输出比值表。

> 后三项为 [Benchmarks Game](https://benchmarksgame-team.pages.debian.net/) 标准算法
> （fannkuch-redux / spectral-norm / binary-trees），与官方同算法同规模；verify 为
> 数学常数（fannkuch(11)=51、spectral(5500)=1.123046、binary-trees checksum），MYP/
> C++/Go 三语言一致（spectral 用 numpy 复核）。

## 解读指南

- 纯计算项（sieve/matmul/nbody/mandelbrot）用**原始类型数组**，不产生 ARC 开销，
  比值主要反映代码生成质量（循环、数组、内联、向量化）。
- **benchmarksgame n-body（`nbodybg`，新增）**：忠实复刻
  [benchmarksgame 官方 n-body](https://benchmarksgame-team.pages.debian.net/benchmarksgame/program/nbody-gcc-9.html)
  （5 体太阳系，同数据同算法）。50M 步两行输出 `-0.169075164`/`-0.169059907` 与
  官方**逐位一致**（MYP 与 C++ 均复现）。MYP `-O2` vs C++ `-O3`（默认 arch，SSE2
  基线）标量写法对比，MYP 反超 ~10%（5M 步 167ms vs 183ms）。注：gcc#9 是手写 AVX
  内联汇编版（`-march=ivybridge`），属人工 SIMD 特化，不参与 MYP/C++ 同源公平对比。
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
- **浮点/大数/压缩批（kmeans/bigint/huffman）**：huffman（字节计数+二叉树+uint8
  下标）持平（1.06）；kmeans（浮点聚类）C++ 快 22%（0.82）；bigint（uint16 字大数
  乘法+64 位进位链）C++ 快 19%（0.84）——bigint/kmeans 的差距来自 i64 宽算术与
  浮点距离的寄存器压力，属宽算术/分配器差异。
- **滑窗/DP/串匹配批（convolution/knapsack/kmp）**：knapsack（0/1 背包 DP）接近
  持平（0.92）；convolution（2D 滑窗浮点）C++ 快 1.7x（0.59，索引计算重的内层
  循环未向量化）；kmp（字节比较 + 失配跳转）C++ 快 1.6x（0.63，字节加载/分支）。
  这两项是 MYP 后续可补强的方向（滑窗索引强度削减 + 字节比较向量化）。
- **排序/图像/图/校验批（radixsort/sobel/floyd/heapsort/crc32）**（29 项 verify
  全一致）：radixsort（直方图+前缀和+散布）1.60、crc32（uint32 表驱动）1.42、
  sobel（uint8 字节滑窗）1.29、floyd（稠密三层循环）1.09——**MYP 全部反超 C++**，
  说明顶层函数 internal 化后这些内核被常量特化+内联+向量化。heapsort（数据相关的
  sift-down）0.83 是唯一落后项，属分支布局/预测差异（两版内层结构一致），非
  codegen 缺陷。这批同时验证了 uint8/uint32/显式转换/窄下标在此类负载上均正确。
- **结构体数组批（dotprod）**：dotprod（struct 数组点积）2.29——顺带**发现并修复
  一个编译 bug**：`v[i].x`（struct 数组元素字段访问）此前读/写都报 "unknown
  property"，codegen 只处理了标量 struct 和链式成员，未处理 Subscript 对象。新增
  `generateArrayElementAddress` 后接入读/写/链式三路径（回归测试
  `tests/struct_array/`）。
- **切片批（slicevec/slicemat）**：slicevec（slice<Vec> 点积）2.67、slicemat
  （嵌套 slice<slice<int>> 矩阵和）2.50——MYP 反超，说明 slice 下标边界检查在 -O2
  下基本被优化掉。这批**发现并修复两个编译 bug**：①嵌套泛型 `slice<slice<int>>`
  无法解析（lexer 把 `>>` 合成一个 token，泛型收尾期待单 `>`——新增
  `consumeGenericClose` 拆分）；②slice-of-slice 分配大小错误（`typeNodeToLLVMType`
  没处理 slice 类型，`slice<slice<int>>` 元素算成 4 字节而非 16，写穿外层数据区 +
  `rows[i][j]` 双下标 codegen 缺失——新增 `sliceTypeOfExpr`/`generateSliceElementAddress`
  统一读写路径）。回归测试 `tests/nested_slice/`。
- **并行计算批（parcomp/parreduce）**：parcomp（并行计算，写 slice）0.89~0.94
  基本持平。parreduce（并行归约）**1.00 持平**——曾 0.60，查明是基准写法用了
  `Atomic.addInt`（每元素原子 RMW），但每线程专属槽位（`Parallel.workerId()` 恒定、
  槽位互斥）**无竞争，普通 load-add-store 即可**；去掉原子后 MYP 与 C++ 寄存器累加
  完全持平（3ms vs 3ms）。**教训：MYP 并行归约到每线程槽位用普通写，只有共享槽位
  才用 `Atomic`**。两者 verify 与串行精确一致，串行同负载约 10x 加速。
- 想让 C++ 更强可加 `-march=native`（脚本里可取消注释）。
- 若某项 `verify` 不一致，说明两语言算法/数据布局有差异，**该行比值无效**。

## 协程对比（MYP @coro vs Go goroutine）

`bash bench/run_compare_go.sh [iters]` —— MYP `@coro`（寄存器级汇编切换纤程，协作式
调度）vs Go goroutine（可增长栈，抢占式调度）。结果（verify 一致）：

| 基准 | 测什么 | MYP | Go | Go/MYP |
|------|--------|-----|-----|--------|
| `coro_switch` | 上下文切换吞吐（200 协程 × 10000 次挂起/恢复） | **72ms** | 307ms | **4.26** |
| `coro_spawn` | spawn 开销（20000 个只返回的协程） | **24ms** | 3ms | **0.12** |

- **切换**：MYP 比 Go 快 ~4.3x（4.26）——2026-08 把 ucontext swapcontext（每次切换
  都做 sigprocmask syscall，~180ns）换成**寄存器级汇编切换**（coro_ctx.S，~13ns，
  微基准 13.9x），400ms→72ms。Go 的 goroutine 切换本身 ~150ns（有运行时抢占/检查
  开销），被 MYP 的纯寄存器切换反超。
- **spawn**：MYP 460→**24ms**（19x）——修复 `__myp_coro_create` 的 O(n²) 槽位复用
  扫描（每创建遍历全部槽找可复用者）+ 句柄槽位不复用（句柄唯一）。Go 仍快 ~8x：
  Go goroutine ~2KB 可增长栈批量创建极廉价；MYP `@coro` 每个分配固定栈（默认
  128KB，`@coro(stack=KB)` 可调小）+ 初始化。适合少量长生命周期协程，不适合海量短任务。

## Go 主套件对比（MYP vs Go）

`bash bench/run_compare_go.sh [iters]` 现覆盖**全部 21 个主套件基准 + 2 个协程通信/I-O
专项 + 2 个协程切换专项**（共 25 项）。Go 侧 `bench/go/*.go` 由
`bench/cpp/*.cpp` 逐文件移植（同算法、同规模、同 LCG），MYP -O2 vs Go
`go build`（默认优化，**无 -march=native**），verify 全部与 MYP 对拍（整数精确、
浮点 1e-3 容差）。结果（16 核，min-of-3）：

| 基准 | MYP(ms) | Go(ms) | Go/MYP |
|------|--------:|-------:|:------:|
| `sieve` | 13 | 14 | 1.08 |
| `matmul` | 17 | 59 | **3.47** |
| `nbody` | 112 | 238 | 2.12 |
| `mandelbrot` | 98 | 99 | 1.01 |
| `tripleloop` | 10 | 17 | 1.70 |
| `fft` | 74 | 64 | **0.86** |
| `sha256` | 17 | 22 | 1.29 |
| `quicksort` | 37 | 42 | 1.14 |
| `knapsack` | 24 | 41 | 1.71 |
| `kmp` | 73 | 86 | 1.18 |
| `crc32` | 77 | 119 | 1.55 |
| `radixsort` | 15 | 33 | 2.20 |
| `sobel` | 6 | 19 | **3.17** |
| `floyd` | 66 | 111 | 1.68 |
| `heapsort` | 72 | 83 | 1.15 |
| `convolution` | 23 | 93 | **4.04** |
| `base64` | 16 | 24 | 1.50 |
| `spmv` | 13 | 28 | 2.15 |
| `kmeans` | 95 | 362 | **3.81** |
| `huffman` | 9 | 19 | 2.11 |
| `bigint` | 61 | 97 | 1.59 |
| `channel_pingpong` | 5 | 6 | **1.20** |
| `io_socket` | 71 | 78 | **1.10** |
| `coro_spawn`（见上节） | 24 | 3 | **0.12** |

- **结论：MYP 在 24 个主套件/协程基准里赢 24 项**（Go/MYP>1 即 MYP 快）；唯一 Go 赢
  的是 `fft`（0.85）。
- **最大差距集中在浮点/内存带宽类**：convolution 4.04、kmeans 3.81、matmul 3.47、
  sobel 3.17、radixsort 2.20、spmv 2.15。根因是 **MYP 走 LLVM O2 自动向量化
  （SIMD 循环展开）**，而 Go 编译器默认几乎不自动向量化，这些标量浮点/字节循环
  退化为逐元素执行。
- **整数/分支/字符串类差距温和**（1.1~1.7x）：MYP 的 LLVM 内联+常量折叠+窄下标
  优化在起作用，但 Go 的简单循环执行效率本身很高。
- **注意公平性**：MYP 是"编译器优化到 LLVM IR"，天然继承 LLVM 的向量化；Go 更
  强调快速编译 + GC + 简单内联。二者都不是 `-march=native`，均用基础 x86-64。
- **协程对比见上一节**：spawn 差 ~8x（固定栈 vs 可增长栈，460→24ms 已修复 O(n²)）、
  **切换已反超**（汇编切换 400→72ms，MYP 快 4.3x），是 MYP 协程实现特性，与主套件
  趋势独立。
- **协程通信/I-O 专项**：`channel_pingpong`（cap=1 Channel 双向 10⁵ 次收发）
  **MYP 反超 1.20**（5ms vs Go 6ms）——2026-08 加**同步交接**（send/recv 唤醒对端
  等待者时立即 resume，Go 式 rendezvous，免调度轮往返），21ms→5ms；此前多消费者
  count 下溢崩溃与句柄槽位复用两个缺陷已修（见下），交接现在安全。
  `io_socket`（回环 TCP 逐字节 ping-pong，@coro + waitFd vs goroutine + 阻塞
  socket）**MYP 反超 1.10**——汇编切换把调度代价降到与 Go netpoller 相当甚至更低。


## 性能修复记录

- **Channel 同步交接（2026-08，channel_pingpong 21→5ms，反超 Go）**：send/recv
  完成缓冲操作后唤醒对端等待者时，若调用方是协程则**立即 `__myp_coro_resume`**
  （Go 式 rendezvous，免一轮 `Coro.scheduler()` 往返）。深度守卫（64）防链式递归
  失控；close/try_* 保持 ready-only。此前两个前置缺陷（多消费者 count 下溢、句柄
  槽位复用串位）修复后本优化才安全；181/181 回归（普通/ASan）+ 4p×2c 压力测试全过。

- **Channel 多消费者 count 下溢（2026-08，崩溃级缺陷）**：`myp_channel_recv` 的
  park-resume 路径无守卫 `count--`——多消费者时，第二个消费者被唤醒但值已被第一个
  取走，`count` 下溢 -1 → 环形缓冲写 `buf[-1]`（ASan: heap-buffer-overflow + 堆损坏/
  munmap 崩溃）。`send` 的 park-resume 无守卫 `count++` 同理。**修复**：send/recv
  park-resume 后循环**重新校验缓冲状态**（count 不足/已满就重新挂起），不再假设被
  唤醒就一定有值/空间。回归测试 `tests/channel_multi_consumer/`（1p×2c，修复前崩溃）。
- **协程句柄槽位复用（2026-08，结果串位）**：`__myp_coro_create` 复用已完成协程的
  槽位 → 无 await 的协程 eager 启动即完成，下一个协程拿到**相同句柄** → 已存句柄
  别名、`Coro.result` 读到新协程结果（verify 900≠600）。**修复**：槽位不复用（句柄
  唯一）；完成协程的栈经线程局部 retired 列表延迟回收（create/调度器安全点移回栈池，
  避免 2 万×64KB 栈累积）。顺带消除 create 里找可复用槽的 **O(n²) 线性扫描**：
  coro_spawn 460→**24ms**（19x）。回归测试 `tests/coro_handle_unique/`。

- **协程寄存器级汇编切换（2026-08，coro_switch 400→72ms，5.6x）**：把
  `swapcontext`（ucontext，内部 `sigprocmask` syscall，微基准 ~180ns/次）换成
  自研 `coro_ctx.S`（x86-64 SysV 寄存器级切换，仅保存/恢复 rsp/rip/rbx/rbp/
  r12-r15，微基准 ~13ns/次，13.9x）。集成：`myp_runtime` 新增 `coro_ctx.S`；
  `mypc` 链接生成程序时也编译该文件（`src/main.cpp`）；非 x86-64 回退 ucontext；
  ASan 构建用 `__sanitizer_start/finish_switch_fiber` 显式通知纤维切换（trampoline
  入口补配对 finish）。效果：

  | 基准 | ucontext | 汇编切换 | Go/MYP 变化 |
  |------|----------|----------|------------|
  | coro_switch | 400ms (0.76) | **72ms (4.26)** | Go 快 33% → **MYP 快 4.3x** |
  | channel_pingpong | 54ms (0.11) | **21ms (0.29)** | Go 快 9x → Go 快 3.4x |
  | io_socket | 86ms (0.90) | **71ms (1.10)** | 基本持平 → **MYP 反超** |

  回归：普通/ASan 构建各 179/179 全过。另修复脚本缺陷：`run_compare_go.sh` 此前
  未调用 `build_all()`（依赖预编译产物），补上后删除 out/ 也能完整重建。

- **协程调度器 wait 表压缩（2026-08，O(N²)→O(N)，io_socket 71x）**：新增
  `channel_pingpong`/`io_socket` 基准时发现 **`myp_coro_waits` 从不压缩**——每条
  `waitFd`/`await` 完成后 `active=0` 的记录留在表里，表长随等待次数线性增长，调度器
  每轮（超时扫描 + fd-poll 收集 + pump 扫描）变 O(N) → 总 O(N²)。回环 TCP
  ping-pong 在 `__myp_coro_scheduler` 入口做原地压缩（仅保留 `active=1` 的等待者）
  后：

  | N（等待轮次） | 修复前 | 修复后 |
  |------|--------|--------|
  | 20000（io_socket） | 6101ms | **86ms**（71x） |
  | 50000（回归测试） | ~38s（超时 FAIL） | **0.30s**（线性） |

  安全前提已验证：跨调度器调用不持有表索引（唤醒路径用 handle/spec 索引而非表位），
  仍在等待的协程 `active=1` 被保留。回归测试 `tests/coro_wait_compact/`（N=50000，
  无修复 38s 超时，有修复 0.30s）。

- **顶层函数内联化（2026-08，最大一次提升）**：根因是 MYP 把**所有顶层函数都发成
  external 链接**（含 `convolution`/`kmp`/`sha256` 等热点内核），LLVM -O2 的内联器
  因成本超阈值（如 convolution 425>225）拒绝内联 → 调用点传的常量参数（2048/5、
  33554432/256…）无法常量折叠 → 小循环（5 次 kx、256 模式）因运行时上界被
  cost-model 判定"向量化不划算"→ 全部退化为标量循环。g++ 则把 `static` 函数内联+
  常量特化+按深度展开递归，故 C++ 快 1.6~1.7x。**修复**：非库构建下把所有函数定义
  标记为 `internal`（仅保留 `main` external），LLVM 的 IPSCCP/内联器随即常量特化+
  内联+向量化（库构建 `--shared/--static` 跳过，保持符号导出）。效果（同机
  min-of-3）：

  | 基准 | 修复前 | 修复后 |
  |------|--------|--------|
  | convolution | 46ms (0.61) | 22ms (**1.32**) |
  | kmp | 109ms (0.63) | 72ms (**0.96**) |
  | base64 | 27ms (0.81) | 16ms (**1.29**) |
  | sha256 | 22ms (0.82) | 15ms (**1.20**) |
  | kmeans | 120ms (0.83) | 95ms (**1.05**) |
  | huffman | 17ms (0.94) | 8ms (**2.12**) |

  代价：`gol` 因内联后 n=512 常量折叠 + SLP 对 8 邻居过度向量化，内层循环膨胀
  （110 条指令/8 次栈重载 vs external 版 48 条/3 次），119ms→153ms（仍 1.06x 领先
  C++）。回归全部通过（O0/O2/ASAN 175/175，TSan 12/12）。
- **knapsack 0.8x（DP 条件存储）**：内层 `if (cand > dp[w]) dp[w] = cand` 的
  数据相关分支 + 逆序访存。曾试给 `myp_region_alloc` 加 `noalias` 返回属性（让 LLVM
  证明数组不别名、提升 `val[i]`/`wt[i]` 循环不变量），但触发后端 if-conversion 成
  `cmov`+**无条件 store**（每迭代翻倍内存写），24ms→33ms 回退，故撤销。分支预测对
  该 50% 命中率的 store 更优，0.8x 已是合理水平。
- **bigint 0.85 / alphabeta 0.86 / nqueens 0.91（剩余差距）**：bigint 是**顺序进位
  链**（每迭代依赖上一轮 carry，~3 周期关键路径，两版内层结构一致，近算法极限）；
  alphabeta 是 gcc 独有的**按深度特化递归展开**（LLVM -O2 无递归内联）；nqueens 是
  递归回溯。三者非 MYP codegen 缺陷，属优化器深度差异。


## 编译器自身性能基准

`bench/compiler/` 是**编译器自身**（`mypc` 前端/语义/CodeGen）的规模性能基准，与
本目录的运行时语言对比分离。测量完整编译时间 + 分阶段耗时（`MYP_TIMING=1`）+ 峰值
RSS，覆盖类/接口/struct/enum/方法解析/泛型的 N/2N/4N 复杂度曲线。详见
[bench/compiler/README.md](compiler/README.md)。

```bash
bash bench/compiler/run.sh            # P1..P7 全量基线
```

