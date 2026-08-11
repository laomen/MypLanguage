# MYP 测试与 Benchmark 下一步优化项目

> 状态：待实施
> 审计日期：2026-08-11
> 适用范围：编译器前端、Sema、LLVM CodeGen、运行时、LSP、并发与协程
> 当前基线：普通/O2 回归各 206 项通过；LSP 14 项通过；主 benchmark 为
> MYP 39 个源文件、C++ 35 个源文件、Go 25 个源文件。

本文记录当前测试与 benchmark 体系中仍可能出现的问题、尚未覆盖的行为，以及下一步
应实施的优化项目。目标不是单纯增加测试数量，而是确保以下三件事：

1. 正确性失败一定使测试命令返回非零，不能出现“输出看起来异常但 CI 仍为绿色”。
2. 已发现并修复的非线性编译路径有可重复、可比较的规模基准，防止性能回退。
3. 高风险 CodeGen/运行时分支都有能区分“正确实现”和“碰巧通过”的定向回归。

---

## 一、当前体系与已确认基线

### 1.1 功能测试

当前快速测试入口为：

```bash
bash tests/run_tests.sh
bash tests/run_tests_O2.sh
```

覆盖内容包括：

- `tests/*/test.myp` 编译、运行和 expected 字节级输出比较。
- `tests/negative/*.myp` 负测试。
- `@test` 测试框架。
- 编译器 no-crash 回归。
- 自举包管理器、格式化器、可视化器。
- `mypc run`。
- LSP hover/completion/documentSymbol 缓存和失效。

2026-08-11 基线为普通/O2 各 206 项通过，LSP 独立测试为 14/14。

### 1.2 专项测试

- `tests/run_tests_asan.sh`：编译器和生成程序的 ASan/UBSan 验证。
- `tests/run_tests_tsan.sh`：并发相关生成程序的 TSan 验证。
- `tests/stress/run_stress.sh`：协程、Channel、并行和异步 I/O 压力测试。
- `tests/fuzz_test.py`、`tools/fuzz_myp.py`：固定语法生成和变异模糊测试。
- GPU/CUDA 测试：依赖本机 NVIDIA 环境，未进入普通 CI。

### 1.3 Benchmark

- `bench/run_compare.sh`：MYP `-O2` 与 C++ `-O3` 的运行时性能对比。
- `bench/run_compare_go.sh`：MYP 与 Go 主负载、协程、Channel、I/O 对比。
- 每个运行时 benchmark 输出 `verify <value>` 与 `ms <time>`。
- 当前脚本默认运行 3 次并取最小值。

现有 benchmark 对生成程序的运行时性能覆盖较广，但尚未形成正式的编译器自身性能
基准。此前类、struct、interface 和 LSP 查找优化主要依赖 `/tmp` 下临时生成的压力
源码，无法自动防止以后重新引入 O(N^2) 扫描。

---

## 二、测试框架可能出现的问题

本节项目优先级高于新增普通测试，因为框架若不能可靠报告失败，更多测试也可能只是
增加“假绿”数量。

### T1：缺失 expected 会被自动接受

**现状**

`tests/run_tests.sh` 在找不到 `tests/expected/<name>.expected` 时，会复制本次输出作为
baseline，并将该项计为通过。

**可能问题**

- 新测试忘记提交 expected 时，CI 会把错误输出当成新基线并通过。
- 测试程序崩溃前若仍以 0 退出，错误文本也可能被保存为 expected。
- 开发者无法区分“测试通过”和“测试资产不完整”。

**优化方案**

- 默认模式下缺失 expected 必须失败。
- 仅 `--update` 或新增的 `--bless` 模式允许创建 expected。
- 汇总中单独显示 `MISSING BASELINE`，不能计入 PASS。

**需要补充的框架自测**

1. 临时创建只有 `test.myp`、没有 expected 的测试目录。
2. 普通模式应返回非零且不能创建文件。
3. `--update` 模式应创建 expected 并返回零。

**验收标准**

- CI 中漏提交 expected 必然失败。
- `git status` 不会因为普通测试自动出现新的 expected 文件。

### T2：负测试只检查“失败”，不检查“为什么失败”

**现状**

`tests/negative/*.myp` 只要 `mypc` 返回非零即视为通过。文件中的 `EXPECT ERROR` 注释
目前不是机器断言。

**可能问题**

- 本应验证接口缺失的方法，却可能因语法错误、导入失败或链接错误而通过。
- 诊断文案和源位置严重退化时无法发现。
- 编译器在更早阶段错误拒绝合法前缀，也会掩盖真正待测路径。

**优化方案**

- 解析首行或前几行的 `// EXPECT ERROR: <substring>`。
- 要求 stderr 至少包含指定稳定子串。
- 可选支持 `// EXPECT STAGE: parser|sema|codegen|link`。
- 不应对整条诊断做字节级匹配，以免正常措辞调整造成大量维护成本。

**需要补充的框架自测**

- 返回非零但诊断不含 EXPECT ERROR 时必须失败。
- 返回非零且包含目标子串时通过。
- 意外 SIGSEGV/SIGABRT/ASan 报告必须归类为 CRASH，而不是负测试通过。

### T3：LSP 依赖缺失时静默跳过

**现状**

当 Node.js 或 `test_lsp.js` 不可用时，主套件打印跳过，但不计失败。

**可能问题**

- CI 镜像改变后 LSP 可能长期未运行，而总结果仍为绿色。
- `myp_lsp` 未构建或路径错误可能被误判为环境可选项。

**优化方案**

- CI/严格模式中，Node、脚本或 `myp_lsp` 缺失必须失败。
- 本地普通模式可保留 skip，但汇总应显示 SKIP 数量。
- 增加 `MYP_TEST_STRICT=1`，CI 固定启用。

### T4：ASan 脚本包含不可达旧副本

**现状**

`tests/run_tests_asan.sh` 在调用 `run_tests.sh` 后立即 `exit $?`，其后仍保留一份旧测试
框架副本。

**可能问题**

- 阅读和维护时容易误改不可达版本。
- 搜索测试行为时会得到两套矛盾逻辑。
- 将来移动 `exit` 或合并冲突可能意外恢复旧实现。

**优化方案**

- 删除 `exit $?` 后全部不可达内容。
- ASan 脚本只负责设置环境并调用唯一的 `run_tests.sh`。
- 保留 `detect_leaks=0` 的原因说明；LLVM 未插桩全局导致的噪声与 MYP 对象泄漏需分开
  处理。

### T5：测试产物写入源码目录

**现状**

每个回归测试会生成 `test.out` 和 `test.output` 到 `tests/<name>/`。

**可能问题**

- 中断测试后留下旧产物，人工查看时可能误认为本次结果。
- 并行运行 O0/O2/ASan 会争用相同文件并相互覆盖。
- 工作区容易出现大量生成文件，影响状态检查和增量工具。

**优化方案**

- 输出统一写入 `build/test-results/<mode>/<name>/` 或 `mktemp -d`。
- O0、O2、ASan 使用独立目录。
- 测试结束后保留失败项日志，成功项可清理。

**验收标准**

- O0 与 O2 可并行运行而不互相污染。
- 测试前后源码目录无新增生成文件。

### T6：固定端口和时序导致偶发失败

**现状**

部分 socket/异步测试使用固定端口，例如 `coro_wait_compact` 使用 24783。并发测试中也
存在输出顺序受调度影响的场景。

**可能问题**

- 端口被占用、TIME_WAIT 或两个测试进程并发执行时偶发失败。
- 线程输出顺序改变会造成 expected mismatch，即使语义正确。
- 重跑通过会掩盖真实竞态，也可能把纯时序波动误判为运行时 bug。

**优化方案**

- TCP server 支持绑定端口 0，并向测试返回实际端口；若语言 API 暂不支持，脚本为每个
  测试分配互斥端口范围。
- 并发测试输出最终聚合结果，不依赖线程打印先后顺序。
- 禁止测试框架自动重跑后吞掉第一次失败；重跑仅作为诊断信息。

### T7：CTest/IDE 无法发现项目测试

**现状**

CMake 未注册主套件、O2、LSP 或专项测试，因此 `ctest` 和 VS Code CMake Tools 无法
列出测试。

**影响**

- IDE、标准 CMake CI 和外部贡献者不知道正确测试入口。
- 测试依赖关系和标签无法统一管理。

**优化方案**

- 至少注册 `myp-regression-o0`、`myp-regression-o2`、`myp-lsp`。
- 给 ASan、TSan、stress、GPU 添加标签和条件开关，不强制每次本机构建执行。
- 设置合理 timeout，并让 CTest 保存失败输出。

---

## 三、正确性回归需要补充的项目

### C1：接口 vtable 槽位的所有对象形态

**已覆盖**

`tests/interface_method_index` 已覆盖：两个接口存在同名 `shared()`，目标接口将该方法
放在不同 vtable 槽位，通过接口局部变量调用并验证返回 22。该测试可发现“按方法名
扫描所有接口并取第一个槽位”的错误。

**仍缺少**

CodeGen 的 fat-pointer 分发不止局部标识符一条路径，还包括成员属性、下标表达式、
函数返回值和接口值复制。需要补充：

1. `this.iface.shared()`：接口类型类属性。
2. `interfaces[i].shared()`：动态接口数组或 slice 下标。
3. `makeInterface().shared()`：函数返回接口值后的链式调用。
4. `ISecond b = a; b.shared()`：接口 fat pointer 整体复制。
5. 接口作为参数传递后调用。
6. 上述每项都必须保留“另一个先声明接口的同名方法在不同槽位”条件，否则错误实现也
   可能碰巧返回正确结果。

**风险**

- 取错 vtable 下标会调用同一对象的另一个方法，属于静默 wrong-code。
- 若错误槽位函数签名不同，可能产生无效 IR、参数错位或运行时崩溃。
- 只测试方法名唯一的接口不能发现该类问题。

**验收标准**

- O0/O2/ASan 全部通过。
- 每种对象形态至少包含一个同名不同槽位测试。

### C2：接口方法签名差异

增加两个接口使用同名方法、但返回类型或参数数量不同的回归。目标接口调用必须使用
自身声明构造 LLVM `FunctionType`，不能从其他接口同名方法推断。

建议覆盖：

- `int value()` 与 `double value()`。
- `int map(int)` 与 `int map(int, int)`。
- 返回 class、string、struct、interface fat pointer 的接口方法。

该组测试用于发现“槽位正确但返回类型仍来自全局首次匹配接口”的问题。

### C3：类方法和属性同名冲突

过去已出现按方法名 fallback 选中第一个类的错误。下一轮 CodeGen 类索引优化会修改
大量 fallback 路径，应建立矩阵回归：

- 两个类有同名 action，不同返回值。
- action 参数数量相同但参数类型不同。
- action、static action、`function:` 段使用同名方法。
- 类属性持有另一类，调用 `this.child.run()` 与裸属性 `child.run()`。
- `new C().run()`、`arr[i].run()`、`factory().run()`。
- 泛型实例类与模板具有同名方法，必须选择具体实例。

每项应使错误候选也能成功编译但返回不同值，避免测试只证明“没有崩溃”。

### C4：枚举名称和 variant 冲突

下一步计划建立枚举及 variant 索引。需补：

- 多个 enum 共享 `None`、`Some`、`Value` 等 variant 名。
- 同一 enum 中无数据和带数据 variant。
- 带多个不同宽度字段的 payload，验证字段偏移。
- enum 定义顺序改变不影响构造和 match。
- 导入模块 enum 与本模块 enum 同 variant 名。

负测试还应断言未知 enum、未知 variant、参数数量错误和参数类型错误的准确诊断。

### C5：struct 成员索引边界

现有测试覆盖 struct 字段、方法、数组和 slice，但针对索引缓存仍应补充：

- 两个 struct 共享字段和方法名，但字段顺序不同。
- 顶层 struct 与嵌套 `Class::Struct` 同短名。
- 字段与方法同名时验证语言规定的优先级。
- struct 方法返回另一 struct 后链式访问。
- struct 数组、slice 和类属性中的嵌套链式访问组合。

### C6：缓存生命周期与 AST vector 扩容

Sema 和 CodeGen 逐步增加 name-to-index/name-to-declaration 缓存。若缓存保存 vector 元素
裸指针，泛型单态化向 `tu.classes` 或 `tu.functions` 追加元素后可能失效。

需要专门测试：

- 在分析函数体时触发多个泛型类实例化，迫使 `tu.classes` 扩容。
- 实例化后继续解析先前声明类、接口、struct 和 enum 成员。
- 泛型函数和泛型静态方法产生大量实例，迫使 `tu.functions` 扩容。
- ASan 下重复执行，确保无 use-after-free。

缓存设计应优先保存稳定索引或拥有稳定存储的数据；不得默认 `std::vector` 元素地址在
单态化期间保持不变。

### C7：LSP 缓存协议覆盖

当前 LSP 测试覆盖 hover、completion、documentSymbol 及 didChange 失效。仍应补充：

- 两个 URI 各自缓存，修改 A 不得清除或污染 B。
- didOpen/didClose 后缓存生命周期。
- 相同版本重复 didChange。
- UTF-8 多字节字符前后的 position 与 Content-Length。
- 空文件、语法错误文件和超大文件。
- 高频交替请求 hover/completion/documentSymbol，验证响应 id 和结果不串位。
- 非法/缺字段 JSON-RPC 请求应返回协议错误而不是终止进程。

### C8：运行时内存和并发边界

近期优化涉及 intrusive ARC、chunked arena、文件 I/O 锁、协程槽位、wait 表和 Channel。
除现有功能测试外，还应补：

- ARC：接口复制、接口数组覆盖、异常展开、协程 frame、闭包捕获组合后的 live count 回零。
- Arena：跨 chunk 边界、超大单次分配、零大小分配、嵌套 region。
- 文件 I/O：同一 handle 多线程读写、不同 handle 并行、close 与正在进行操作的竞态。
- Channel：多生产者/多消费者、close 时仍有等待者、同步交接递归深度边界。
- 协程：创建/完成/销毁交错，等待表压缩与新等待记录同时发生，多线程 TLS 隔离。

并发测试应分别在普通、TSan 和长时间 stress 三种模式下运行；单次确定性回归不能替代
竞态检测。

---

## 四、Benchmark 框架本身的问题

### B1：verify 不一致不会使脚本失败

**现状**

`bench/run_compare.sh` 会显示 `a != b`，但脚本最终仍返回 0。
`bench/run_compare_go.sh` 也不会因 verify 不一致退出非零。

**更严重的问题**

Go 脚本使用 `${same:+ (OK)}`。Shell 的 `:+` 判断变量是否非空，而字符串 `0` 仍是
非空，因此 `same=0` 时也会显示 `(OK)`。

**优化方案**

- 维护 `FAIL` 计数，任一编译、运行、解析或 verify 失败时最终退出 1。
- 仅在 `[ "$same" -eq 1 ]` 时显示 `OK`。
- verify 不一致时打印 MYP/对照值、绝对误差和相对误差。
- CI smoke 模式至少运行一组小规模 benchmark 来验证 harness。

### B2：运行失败被转换成合法的 `0 0`

**现状**

`run_once()` 在二进制失败时输出 `0 0`。若两边都失败或真实 verify 也是 0，脚本可能
把失败当成一致结果。

**优化方案**

- `run_once()` 必须传播非零状态。
- 缺少 `verify` 或 `ms` 行必须标记 `MALFORMED OUTPUT`。
- `ms <= 0` 对当前毫秒级 benchmark 应视为无效；未来若支持亚毫秒，应改用 ns/us，
  不能用 0 代表成功。
- 保存失败程序的 stdout/stderr，不能统一丢弃 stderr。

### B3：只取最小值无法识别抖动

**现状**

默认 3 次取最小值适合观察最佳 codegen 性能，但不能显示调度、频率、GC 或系统噪声。

**优化方案**

- 至少预热 1 次，再采样 5 次。
- 报告 min、median、max，建议增加 MAD 或变异系数。
- 对并行/协程/I/O benchmark 标记高抖动，不用单次最小值做回归阈值。
- 输出机器可读 JSON，记录 CPU、核心数、内核、编译器版本、git commit 和参数。

**判退建议**

- 本机开发：展示结果，不设置硬阈值。
- 固定性能 runner：以 median 对历史基线，连续多次超过阈值才失败。
- 编译器规模测试：主要判断复杂度斜率，而不是绝对毫秒值。

### B4：MYP/C++/Go 清单和文档漂移

2026-08-11 文件清单：

- MYP：39。
- C++：35。
- Go：25。
- MYP 比 C++ 多出的 4 个是 `channel_pingpong`、`coro_spawn`、`coro_switch`、
  `io_socket`，属于 Go 专项，无 C++ 对照是合理的。
- Go 尚缺 14 个已有 MYP workload：`alphabeta`、`astar`、`bst`、`dijkstra`、
  `dotprod`、`gol`、`hashmap`、`nbodybg`、`nqueens`、`parcomp`、`parreduce`、
  `raytracer`、`slicemat`、`slicevec`。

**可能问题**

- README 中“21/24/25 项”等文字与脚本实际数组可能不一致。
- 新增源文件但忘记加入 `names` 数组时不会运行。
- 缺少对照实现时，读者可能误解为完整同算法比较。

**优化方案**

- 由目录清单或 manifest 驱动，不在多个脚本和 README 手工维护数组。
- manifest 声明语言实现、分类、规模、verify 类型、容差和是否允许缺少对照。
- 增加 `--list` 与 `--check-manifest`，检查孤立源文件和缺失实现。
- README 表格可由 manifest 生成或至少由检查脚本验证。

### B5：公平性元数据不足

当前 README 已解释 MYP LLVM O2 与 C++ O3 的选择，但结果文件没有固定记录：

- 实际选择的是 clang++ 还是 g++。
- 编译器具体版本和目标架构。
- 是否启用 `-march=native`、fast-math、sanitizer。
- Go 版本、GOMAXPROCS。
- CPU governor、核心数、并行 benchmark 的线程数。

这些差异足以改变结果。脚本应在表格头和 JSON 中记录环境，并拒绝把 sanitizer 构建
当作正常性能结果。

---

## 五、需要新增的编译器性能基准

新增 `bench/compiler/`，与运行时语言对比 benchmark 分离。建议使用 Node.js 或独立
脚本生成 MYP 源码，并通过外部高精度时钟测量 `mypc` 完整编译时间。

### 5.1 基准原则

每项至少测试 N、2N、4N，必要时测试 8N。判断重点：

- 输入翻倍后耗时是否接近 2 倍，而不是 4 倍。
- 优化前后使用相同生成器和相同源码。
- 每个规模预热后取 median。
- 编译输出到临时目录并清理。
- 分开记录 parse+Sema+CodeGen+LLVM/链接时间；若当前编译器没有阶段计时，先测完整
  编译，后续增加 `--time-phases`。
- 同时记录峰值 RSS，防止用大量缓存换取不可控内存增长。

### 5.2 必做规模基准

#### P1：类数量 × 裸属性读取

生成 N-1 个无关类，将目标类放在最后，并在其方法内读取目标裸属性 N 次。

已测基线：

| N | 当前耗时 |
|---:|---:|
| 1,000 | 84.2 ms |
| 2,000 | 173.7 ms |
| 4,000 | 453.3 ms |
| 8,000 | 1,430.6 ms |

该曲线表明 CodeGen 仍可能为每次裸属性读取扫描全部类。下一步建立 CodeGen
class-name 索引后，应显著降低 4N/8N 的增长率。

#### P2：接口数量 × 接口方法调用

生成 N-1 个无关接口、一个目标接口和 N 次目标接口调用。

优化前基线：

| N | 优化前 |
|---:|---:|
| 4,000 | 642.6 ms |
| 8,000 | 2,215.6 ms |

Sema interface member cache 后 8,000 为 1,702.4 ms；Sema + CodeGen 精确方法索引后为
421.1 ms，总体约 5.3 倍提升。该项必须固化，用于防止重新引入全接口扫描。

#### P3：接口数量 × 接口变量声明

生成 N-1 个无关接口，在函数中声明 N 个目标接口变量。

已测基线：

| N | 当前耗时 |
|---:|---:|
| 1,000 | 111.8 ms |
| 2,000 | 246.5 ms |
| 4,000 | 674.9 ms |

热点位于 CodeGen 接口类型判定：每个变量声明扫描全部接口。计划增加 interface-name
集合或 declaration index，并复用到类型映射、ARC 类型判定和 vtable 创建。

#### P4：struct 数量 × 字段读取

生成 N 个 struct 和 N 次目标 struct 字段读取。此前 struct member TypeInfo 缓存已将
4,000 struct/读取从 657.6 ms 降至 121.9 ms，约 5.4 倍。该项用于保护已完成优化。

#### P5：enum 数量 × variant 构造

生成 N-1 个无关 enum 和 N 次目标 enum variant 构造。

已测基线：

| N | 当前耗时 |
|---:|---:|
| 1,000 | 76.0 ms |
| 2,000 | 164.4 ms |
| 4,000 | 471.3 ms |

计划建立 enum-name 到 declaration、variant-name 到 index 的缓存。Sema 与 CodeGen 均应
使用同一声明语义，但不能共享生命周期不稳定的 vector 元素裸指针。

#### P6：类数量 × 方法调用 fallback

分别生成：

- 已知对象类的 action 调用。
- static action 调用。
- `function:` 调用。
- `new C().method()`、`arr[i].method()`、`factory().method()`。
- 方法名在多个类中冲突。

该组同时测性能和正确性。优化目标是在 Sema 已提供 `resolved_object_class` 时直接定位，
只在旧 AST 或无法解析的特殊表达式上使用兼容 fallback。

#### P7：泛型实例数量

生成大量泛型函数、泛型静态方法和泛型类实例，测量：

- 已存在实例查找。
- 新实例追加后继续查找。
- `tu.functions`/`tu.classes` 扩容。
- 峰值 RSS。

用于判断 generic instance 的线性扫描是否形成 O(N^2)，并验证稳定索引设计。

### 5.3 LSP 性能基准

将目前临时压力测试固化到 `bench/lsp/`：

- hover：5,000 类、2,000 请求，历史结果 431.2 ms 降至 89.7 ms。
- completion：1,000 类、500 请求，约 52.9 MiB 输出；应区分计算时间和 stdout 写出
  时间。
- documentSymbol：5,000 类、100 请求，历史结果 1,747.3 ms 降至 88.6 ms。
- didChange 后第一次请求与缓存命中请求分别计时。
- 多文档交替请求，测缓存隔离和总内存。

LSP 基准必须校验每个 JSON-RPC response id 和结果数量，不能只测进程耗时。

---

## 六、需要新增的运行时性能与资源基准

### R1：ARC

现有 `hashmap` 混合了算法、容器实现和 ARC，不足以单独解释引用计数成本。新增微基准：

- retain/release 配对吞吐。
- 大量短命 class 创建销毁。
- 接口 fat pointer 复制与释放。
- class 动态数组覆盖元素。
- 闭包捕获 class 引用。

输出时间、峰值 RSS、最终 `Memory.liveObjectCount()`。verify 必须确认 live count 回到预期
基线。

### R2：Arena/region

- 小对象连续分配吞吐。
- 跨 chunk 边界。
- 大对象旁路或独立 chunk。
- region 重置/释放时间。
- 与逐对象 malloc 的 C/C++ 对照。

记录总分配字节、chunk 数、峰值 RSS，避免只看分配速度。

### R3：协程 spawn 的时间和内存

现有 `coro_spawn` 只报告时间。MYP 固定栈与 Go 可增长栈的关键差异是内存，应增加：

- 1k、5k、10k、20k 协程的时间曲线。
- 峰值 RSS。
- 不同 `@coro(stack=KB)` 的对比。
- 完成后 retired stack pool 的稳定内存。

该项可直接验证未来栈池、mmap、guard page 或更小默认栈优化。

### R4：文件 I/O 并发

新增不同 handle 并行读写、同 handle 加锁竞争、顺序小块读和大块读。除吞吐外校验文件
hash，避免并发性能提升以数据错乱为代价。

### R5：Channel 与调度器扩展曲线

- 生产者/消费者数量矩阵：1x1、1xN、Nx1、NxN。
- channel capacity：0/1/小/大。
- ready queue 和 wait table 数量 N、2N、4N。
- 同步交接命中率与最大递归深度。
- 除吞吐外记录 p50/p95 延迟。

---

## 七、CI 与执行分层

所有测试都塞进每次提交会导致反馈过慢和时序噪声。建议分四层。

### Level 1：每次提交，目标 10 分钟内

- Release 全目标构建。
- O0 主回归。
- O2 主回归。
- LSP 测试。
- 固定 seed 小规模 fuzz。
- benchmark harness 自测和 compiler scaling 小规模 smoke，不做严格绝对时间判退。

### Level 2：每个 PR 或每日

- ASan/UBSan 主回归。
- TSan 专项。
- stress 普通模式。
- 多 seed fuzz。
- compiler scaling 完整 N/2N/4N 曲线。

### Level 3：每日或每周固定性能机

- MYP/C++ 全 benchmark。
- MYP/Go 全 benchmark。
- LSP 完整吞吐。
- ARC/arena/coroutine RSS。
- 与历史 JSON 基线比较 median 和复杂度斜率。

### Level 4：专用硬件

- CUDA/GPU 正确性与性能。
- 不同 CPU 架构和非 x86-64 协程回退路径。
- 长时间 soak、端口和文件系统高并发。

CI 当前已跑 Release、普通主套件、固定 seed fuzz 和 ASan，但尚未跑 O2、TSan、stress、
compiler scaling 或 benchmark smoke。CI 顶部“84 个测试”的注释也已过时，应改成动态描述，
避免每次测试数量变化都维护硬编码数字。

---

## 八、实施顺序

### 阶段 A：先保证失败可见（P0）

1. 修复两个 benchmark 脚本的退出码、`0 0` 和 Go `(OK)` 判断。
2. 普通测试禁止自动创建 expected。
3. 负测试校验 EXPECT ERROR。
4. 删除 ASan 脚本不可达副本。
5. CI 启用严格依赖模式。

**完成定义**：人为制造编译失败、运行崩溃、verify 不一致、缺 expected 和错误诊断时，
对应命令均返回非零。

### 阶段 B：固化近期优化（P0）

1. 新增 `bench/compiler` 生成器和 JSON 输出。
2. 固化 class/interface/struct/enum 规模曲线。
3. 补齐接口属性、下标、返回值、复制和参数分发回归。
4. 补类方法冲突、enum variant 冲突和缓存扩容回归。

**完成定义**：已完成的 interface/struct/class lookup 优化有持续性能保护；错误 vtable
槽位和错误类 fallback 均能被测试确定性捕获。

### 阶段 C：扩大持续验证（P1）

1. CI 加 O2。
2. TSan/stress 转为每日任务。
3. CTest 注册快速套件和标签。
4. 测试产物移出源码树。
5. 固定端口和并发输出顺序治理。

### 阶段 D：资源与长期性能（P1/P2）

1. ARC、arena、协程、I/O、Channel 增加时间和 RSS 基准。
2. benchmark manifest 与环境元数据。
3. 固定性能 runner 和历史趋势。
4. 逐步补齐 Go 对照；对不适合跨语言直接比较的项目明确标注。

---

## 九、每个优化提交的统一验收清单

涉及编译器查找、缓存或 CodeGen 的提交至少执行：

```bash
bash tests/run_tests.sh
bash tests/run_tests_O2.sh
node tests/test_lsp.js
```

涉及内存生命周期时追加 ASan；涉及线程、Channel、文件 I/O 或事件时追加 TSan 和相关
stress。涉及性能时还必须：

1. 保存优化前后相同输入、相同命令、相同机器的 median。
2. 校验程序输出或 JSON-RPC 结果，不允许只看耗时。
3. 报告规模翻倍曲线，说明复杂度是否变化。
4. 运行 `git diff --check`。
5. 不把无关 warning、已有波动或失败测试静默归入“通过”。

只有同时具备定向正确性回归、完整回归和可重复性能证据，优化项目才视为完成。