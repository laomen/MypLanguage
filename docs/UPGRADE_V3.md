# MYP V2.0 → V3.0 升级说明

> 适用范围：从 **V2.0**（`var` 推断/字符串插值/区间）升级到 **V3.0.0** 里程碑版本。
> 语言规格保持 **1.0**——V3.0 全部变更为 **additive**，**无破坏性语法变更**，
> V2.0 源码可直接在 V3.0 编译运行。

---

## 1. 版本路径（V2.0 → V3.0 经历了什么）

```
V2.0 ── V2.1 ── V2.2 ── V2.3 ── V2.4.0 ── V2.4.1 ── V2.4.2 ── V2.4.3 ── V3.0.0
 │       │       │       │        │         │         │         │        │
 │  泛型/枚举/ 测试框架 接口多态 语法冻结/ 算子@op  管道|>   协程C1-  协程C1-C10
 │  lambda/   /fmt    atomic/  GPU/标准库  (P1+P2)  (P3)     C4/异常  完整/Channel/
 │  FFI/包/          TUI/SDL/ 大扩充/内存                               协程Future/
 │  LSP/VSCode       协程初步  管理/LSP稳定                              事件队列/... 
 │
 V2.0 基线：var 类型推断、字符串插值、区间 a..b、myp_viz
```

- **V2.1**：泛型、枚举、lambda/闭包、FFI、包管理器、LSP、VS Code 扩展
- **V2.2**：内置测试框架（`--test`）、`myp fmt`
- **V2.3**：接口多态、mapping 增强、`atomic`、TUI、SDL2、`await`/`@coro` 初步、`const`、位运算、struct 方法
- **V2.4.0**：语法冻结（规格 1.0 + 版本策略）、GPU 计算、标准库大扩充（34+ 模块）、内存管理、LSP 稳定
- **V2.4.1/2**：算子系统（`@op`）、管道 `|>`、CI
- **V2.4.3**：协程 C1-C4、异常机制完善
- **V3.0.0**：协程 C1-C10 完整 + Channel + 协程 Future + 动态事件队列 + class const + property 默认值修复

---

## 2. 升级点：V2.0 没有的新能力

### 2.1 语言（语法/类型系统）

| 能力 | 引入 | 示例 |
|---|---|---|
| 泛型（monomorphization）| V2.1 | `ArrayList<int>`、`HashMap<string, double>` |
| 枚举 + 模式匹配 | V2.1 | `enum Color { Red, Green }` + `match` |
| Lambda/闭包 | V2.1 | `(int x) => { return x*2; }` |
| FFI | V2.1 | `ffi int myp_x();` |
| 接口多态（胖指针虚表）| V2.3 | `interface Shape` + `interface class Shape` |
| 运算符重载 | V2.4.1 | struct `operator:` 节 + 顶层 `@op("+")` |
| 管道 `\|>` | V2.4.2 | `A \|> Op1 \|> Op2`（左结合）|
| `const` 关键字 | V2.3 | `const int MAX = 100;` |
| **class 顶层 const** | V3.0 | `const double THERMAL = 0.0253;` |
| 位运算 `& \| ^ << >>` | V2.3 | `flags \| (1 << 3)` |
| 逗号多变量声明 | V2.3 | `int a=1, b=2;` |
| Range for | V2.0 区间已有，for-in 集成 | `for i in 0..5 { }` |
| 字符串插值 | V2.0 已有 | `"Hello, $name"` |

### 2.2 并发与协程（V2.0 无协程）

| 能力 | 引入 | 说明 |
|---|---|---|
| **协程完整体系** | V2.4.3 → V3.0 | `@coro` 方法/顶层函数 + `await` 挂起/恢复（C1-C10）|
| 协程值传递 / 返回值 | C2 | `int v = await expr;` + `Coro.result(h)` |
| 自动调度器 | C3 | `Coro.scheduler()` round-robin |
| 事件等待 / 超时 / 多事件 | C4/C10 | `await event timeout N`、`Coro.waitAny` |
| 嵌套协程 / 诊断 | C10 | `Coro.status/current/count` |
| 协作式取消 / 异常边界 | C10 | `requestCancel`、未捕获异常安全结束 |
| **Channel 缓冲通道** | V3.0 | `stdlib/channel.myp`：协程阻塞 send/recv |
| **协程 await Future** | V3.0 | 协程内 `Future.get()` 非阻塞等待 |
| `@thread` 线程（Actor 隔离）| V2.3+ | `new Worker() @thread` |
| `@parallel for` + 线程池 | V2.4 | 数据并行 + work-stealing |
| Barrier / Future / Promise | V2.3/V2.4 | pthread 封装 + MYP 封装 |
| `atomic` | V2.3 | LLVM atomicrmw |

### 2.3 标准库（V2.0 → 34+ 模块）

| 类别 | 模块 |
|---|---|
| 数据结构 | `collections`（ArrayList/HashMap/Set/Queue/Stack/Deque/PriorityQueue/LinkedList/Sort）|
| 网络/进程 | `net`（TCP）、`process`、`args`、`env` |
| 数据 | `json`、`regex`、`base64`、`fs`、`date` |
| 并发 | `coro`、`channel`、`atomic`、`barrier`、`future`、`pool` |
| 图形 | `sdl`（SDL2）、`ui`（TUI）、`cuda`（GPU）|
| 工具 | `logger`、`test`、`stream`、`time`、`timeline`、`random`、`math` |

### 2.4 工程与工具链

| 能力 | 引入 |
|---|---|
| 语法冻结 + 版本策略（规格 1.0）| V2.4.0 |
| 包管理器 `myp`（init/build/install/run）| V2.1 |
| LSP + VS Code 扩展 | V2.1 |
| `myp fmt` / `myp_viz` | V2.2 / V2.0 |
| 内置测试框架（`--test` + `@test`）| V2.2 |
| GPU 计算（`@gpu for` + CUDA）| V2.4.0 |
| 编译"永不崩溃"（LLVM 错误处理器）| V2.4.0 |
| 异常机制完善（finally/重抛/对象异常）| V2.4.3 |
| CI（GitHub Actions）+ ASAN/fuzz 回归 | V2.4.2 |

---

## 3. 破坏性变更

**无破坏性语法变更。** 语言规格自 V2.4.0 冻结为 1.0，此后仅允许 additive 变更；
V2.0 → V3.0 期间未做规格主版本递增（无删除/重命名语法、无改变语义）。

### 注意事项（罕见情况，非破坏）：

1. **内部 `__myp_*` 调用**（V2.2）：非标准库文件禁止直接调用 `__myp_*` 内部函数。
   V2.0 代码若直接调用内部函数，需改用标准库公开 API（标准用法不受影响）。
2. **新增关键字**：`const`/`operator`/`await` 等成为保留字。若 V2.0 代码把它们当作
   标识符使用，需重命名（极罕见）。
3. **property 默认值修复**（V3.0）：`int x = 5;` 声明默认值现在在 `new` 时**生效**
   （V3.0 之前只 memset 为 0）。依赖旧行为的代码输出会变化（更符合声明语义）。

---

## 4. 迁移建议

- **V2.0 源码可直接用 V3.0 编译**（全部 additive，无迁移改写）。
- 检查项：
  - 若代码直接调用了 `__myp_*` 内部函数 → 改为标准库 API。
  - 若代码依赖"property 默认值为 0"（声明了非零默认值却期望 0）→ 删除多余默认值。
- 新项目可充分利用 V3.0 特性（协程、Channel、算子、管道、GPU 等）。

---

## 5. 验证

```bash
./build/mypc --version     # MYP Compiler v3.0.0 (Language Spec 1.0)
bash tests/run_tests.sh    # 普通 + ASAN 全套 109/109
```

---

## 6. 参考

- 语法：`docs/grammar.md`（规格 1.0 EBNF）
- 编程手册：`docs/manual.md` / `docs/manual_en.md`
- 协程设计：`docs/coro.md`；算子：`docs/operators.md`；异常：`docs/exceptions.md`
- 变更历史：`docs/CHANGELOG.md`
