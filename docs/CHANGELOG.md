# MYP 变更日志 (CHANGELOG)

> 语言规格版本：**1.0**（自 `docs/grammar.md` 发布起语法冻结）
> 编译器版本与语言规格版本分离：
> - **编译器版本**（`mypc --version`）跟踪实现进度，如 `v2.4.0`。
> - **语言规格版本**（`Language Spec`）只在**破坏性语法/语义变更**时递增。

## 版本策略（冻结约定）

自语言规格 v1.0 起：

- **破坏性变更**（删除/重命名语法、改变语义、改关键字）**仅允许在规格主版本
  递增时引入**（如 1.0 → 2.0）。此类变更必须在此文件显式记录"破坏性变更"条目。
- **非破坏性变更**（新增语法糖、新增标准库、新增注解、新增内置函数）可在任意
  编译器版本引入，不改变规格主版本。
- 每次提交必须保持 `docs/grammar.md` 与实现一致；新增/修改语法后需同步更新规格。

---

## 语言规格

| 规格版本 | 发布日期 | 说明 |
|---------|---------|------|
| **1.0** | 当前 | 语法冻结基准。正式发布 `docs/grammar.md` EBNF。语法自此受变更策略约束。 |

---

## 编译器版本历史

### v3.9.0（当前）
- **类构造器（M1-M4，additive）**：`@constructor` 注解 + **函数名==类名隐式构造器**。
  - M1 语法 + AST：`parseActionDecl`/`parseFunction` 识别 `@constructor`；类/struct 中
    方法名==类名 → 隐式构造器（可省略注解，C++/Java 风格）；struct 新增 `action:` 节
    （方法/属性前瞻区分）。
  - M2 class 构造器绑定：`new C(args)` 优先绑定构造器（重载解析 + 数字提升
    `int → long → double`，歧义/无匹配编译报错）；泛型 `new Box<double>(1.5)` 绑定
    单态化实例类的构造器（根治 `@startup` 泛型分发 bug 的根因）；构造器重载
    mangling（`Class_Action_<paramtypes>`）；legacy `@startup init` 回退保留。
  - M3 struct 构造器：函数式构造 `Struct(args)`（栈临时 + 构造器；声明初始化/赋值/
    `return Struct(...)`/与 `operator:` 共存）。
  - M4 `copy()` 深拷贝约定（纯约定方法，无新语法）；manual/design/grammar/CHANGELOG 同步。
  - 修复既有 bug：`generateMemberAccess` 对类实例局部变量的 `.property` 访问错误返回
    实例指针（`c.data_[i]` 写入实例内存）；`checkStructMethods` 排除构造器为兄弟方法
    （其名==struct 名遮蔽 struct 类型名）。
  - 设计见 `docs/constructor.md`；测试 `tests/constructor/` + `tests/copy/`。
- **@startup → 构造器迁移（不留 legacy）**：
  - stdlib 8 文件（`json`/`fs.Path`/`logger`/`net.TcpServer`/`net.TcpClient`/`regex`/
    `text.StringBuilder`/`time.Timer`）+ `stream`×3 与 `layers`×2 空占位删除。
  - 41 个测试 + examples + docs/examples + deeplearning + BNCT 的 legacy `@startup`
    （非 @thread）→ `@constructor`；`tests/startup` 重写为新语义（构造器 + @thread 线程入口）。
  - **移除** codegen 中 `new C(args)` 自动调 `@startup` 的 legacy 绑定；
    `@startup` 严格只作启动信号（@thread 线程入口）。
  - 验证：`-O0`/ASAN 全套 121/121。

### v3.8.0
- **集合动态扩容**（`stdlib/collections.myp`）：`ArrayList`/`HashMap`/`Set`/`Deque`/`Queue`/
  `Stack`/`PriorityQueue`/`LinkedList`/`StrHashMap` 全部突破固定 1024 上限。
  - 惰性分配（首操作时 `new T[cap_]`）+ 容量翻倍扩容；哈希类 75% 负载因子翻倍重建；
    环形缓冲 grow 重排；LinkedList 节点池翻倍。
  - 规避 `@startup` 泛型分发 bug（`new Box<double>()` 曾误调模板 init 致堆损坏）。
- **泛型 `new T[n]` 端到端支持**（additive，新语言能力）：
  - parser：`new Ident[n]` → 动态数组；局部声明歧义消除补 `Ident [] name`。
  - codegen：单态化类型参数映射（`current_type_params_`），`new T[n]` 用真实元素类型分配。
  - 支持 `new Foo[n]`（类数组）与 `new T[n]`（泛型参数）。
- **`function:` 段跨方法调用修复**：sema 声明类作用域符号表 + codegen 预声明函数符号，
  方法可调用段中任意位置的方法（此前只能调用靠前的）。
- **LSP 解析错误恢复死循环修复**（内存爆炸根因）：
  - `parseBlock`/`parseMapping` 的 body 循环加"必须前进"保证（`current_` 未变则强制 `advance`），
    畸形输入（如函数体内孤立 `class`、`mapping =>`）不再无限循环耗尽内存（曾致 `myp_lsp`
    膨胀至 35GB）。
  - 模糊测试 5 seed × 600 例随机畸形输入 = 3000 例 0 挂起；全 stdlib 31 文件 LSP 实测 4MB。
- **`stdlib/memory.myp` 修复**（预存坏文件）：`ffi void*` 语法不支持 → 改用 MYP 惯例
  **指针以 `long` 承载**（同 json/regex 的 handle）；类名 `Mem` → `Memory`（对齐 manual）；
  移除基于不存在的 `__myp_ptr_write/read` 的 `DynamicArray`（动态数组统一走
  `collections.ArrayList<T>`）。`Memory` 提供 alloc/free/realloc/release（FFI 裸内存 +
  确定性释放）。新增 `tests/memory/`。
- 验证：`tests/collections_grow/`（全集合超 1024）；`-O0`/`-O2` 全套 115/115；ASAN 115/115。

### v3.7.0
- **DAP 调试支持（M7）**：`src/dap/dap_server.cpp` → `myp_debug`（DAP ↔ gdb MI2 桥）。
  - 支持：initialize/launch/setBreakpoints/configurationDone/continue/next/stepIn/
    stepOut/threads/stackTrace/scopes/variables/evaluate/pause/disconnect。
  - 复用 `-g` DWARF：VS Code 设断点/单步/查局部变量。
  - VS Code 扩展 `vscode-myp` 注册 `myp` 调试器（`myp.debuggerPath` 配置 + 自动探测）。
  - 验证：`tests/test_dap.py`（launch→断点→stopped→栈/局部变量/evaluate，15 断言）；
    `-O0`/`-O2` 全套 114/114。
- **文档手册完善**：`docs/manual.md` 第 12 章补完整命令行选项表、优化（`-O`/`--passes`）、
  调试（`-g`/gdb）、元编程（`@eval`/`macro`/`@macro`+`quote`）章节。

### v3.6.0（当前）
- **元编程 M4：过程宏 `@macro` + `quote`**（`src/eval/eval.cpp` 解释器扩展 + `src/macro/macro_expand.cpp` 集成）。
  - `@macro` 注解修饰函数（sema 跳过 body / codegen 不生成）；`quote { ... }` 上下文关键字
    （仅 `quote {` 识别为 AST 模板，`char quote = ...` 变量不受影响）。
  - 解释器 `EvalValue` 加 AST 值类型：`StmtList + StmtList` 拼接、`quote` 求值、`$x` 插值
    （数值→字面量、字符串→标识符（变量名 `int $name = ...` / 赋值目标 `$x = ...`）、AST→内联）。
  - 展开 pass：`@macro` 调用编译期执行（`evalProcMacro`）→ AST 替换；`main.cpp` Phase 3b
    对 `@macro` 也触发展开（修复：`ast.macros` 空但含 `@macro` 时未展开的 bug）。
  - 验证：`tests/proc_macro/`（`genAssign("x",42)` → `int x = 42`；`makeCalls(3)` → 3 条
    `Console.write(0/1/2)`）；`-O0`/`-O2`/ASAN 全套 114/114。
  - 设计见 `docs/metaprogramming.md` §5。

### v3.5.0（当前）
- **元编程 M3：声明式宏 `macro`**（`src/macro/macro_expand.cpp` + `include/mylang/Macro.h`）。
  - `macro name($a, $b) { ... }` 顶层声明；`$param` 为模板占位（新 token `Dollar` + `Keyword_macro`）。
  - 宏体是普通 MYP 块；展开为 AST 深拷贝 + `$param` → 实参 AST 替换（parse 后、sema 前）。
  - 支持表达式参数（`log($a)`）、语句参数（`repeat($n, $body)`）、赋值参数（`$x = ...`）、
    嵌套宏（`twice(addN(v,10))`，迭代展开 + 深度上限）。
  - `--macro-expand`：展开后 AST dump 调试。
  - 验证：`tests/macro/`（repeat+addN+twice+log → v=37）；`-O0`/`-O2` 全套 113/113。
  - 设计见 `docs/metaprogramming.md`。

### v3.4.0（当前）
- **元编程 M1：`@eval` 编译期求值**：`src/eval/eval.cpp` 轻量 MYP 解释器。
  - `@eval` 标记纯函数（标量/递归/条件/循环/`@eval` 互调/const 引用），编译期执行。
  - 顶层 `const int X = <expr>` 折叠：`const int FIB10 = fib(10)` 生成 `ret i32 55`。
  - 求值器在 sema 后、codegen 前运行（`main.cpp` Phase 4b）；非折叠初始化保持运行时行为。
  - 验证：`tests/eval/`（FIB10=55/FIB20=6765/HALF=2.5/BIG=true/T5=165/BIGL=1000000）。
- **元编程 M2：泛型约束 `where T : Interface`**：
  - 语法 `<T where T : Shape>`；sema monomorphization 时检查类型参数实现接口。
  - `DrawList<Circle>` 通过、`DrawList<int>` 编译期报错；`tests/generic_constraint/` + negative。
  - 验证：`-O0`/`-O2`/ASAN 全套 112/112。
  - 设计见 `docs/metaprogramming.md`。

### v3.3.0（当前）
- **自定义 LLVM pass（M6）**：`src/codegen/myp_passes.cpp` + `include/mylang/MypPasses.h`。
  - `MypRedundantStorePass`（FunctionPass）：消除同基本块内相邻同址死 store
    （如 `int x = 0;` 生成的双 store，实测 -O0 IR 每个变量多一次重复 store）。
  - `mypc --passes myp-pass` 可调用（`runMypPasses` 直接分发）；未知 pass 名报错。
  - `registerMypPasses`：追加到 -O 管线末尾 + 注册 `-passes` 解析回调（opt 风格嵌套）。
  - LLVM 21 备注：`parsePassPipeline` 顶层不解析自定义 pass 名（需嵌套 `module(...)`），
    故 `mypc --passes` 走自建分发。
  - 验证：`tests/test_myp_pass.sh`（6 项断言：可调用/语义/死 store 10→7/-O2 组合/未知拒绝）；
    `-O0`/`-O2` 全套 109/109。
  - 设计见 `docs/optimization_debugging.md` §3.5/M6。

### v3.2.0
- **DWARF 调试信息（M3-M5）**：`-g/--debug` 生成 gdb 可用调试信息。
  - M3：编译单元/文件/函数 DISubprogram + 逐语句行号（`break foo.myp:N` 命中）；
    `main.cpp` 全链路 `-g` 传参（单文件/多文件）。
  - M4：参数（`createParameterVariable`）+ 局部变量（`popScope` 集中 `dbg.declare`）
    变量映射（gdb `print a/b/sum/x/y` 正确）。
  - M5：类型映射（int/long/double/float/bool/char/指针/struct 成员/数组子范围）。
  - 验证：`tests/test_debug.sh`（gdb 批处理 6 项断言）+ `-g -O0`/`-g -O2` gdb 实测；
    `-O0`/`-O2` 全套 109/109、ASAN 109/109。
  - 说明：LLVM 21 已移除 `Type::getPointerElementType`（opaque pointer），
    指针类型统一映射为 `void*`；类实例显示为指针。
  - 设计见 `docs/optimization_debugging.md`。

### v3.1.0
- **IR 优化管线（M1）**：`-O1/-O2/-O3` 真正生效——`writeObjectFile` 用 PassBuilder NewPM
  运行 `buildPerModuleDefaultPipeline`（此前 `-O` 只影响后端指令选择，IR 无优化）。
  修复优化暴露的两个真实 bug：`setjmp` 缺 `returns_twice` 属性、`myp_throw` 误标 `noreturn`
  （后者导致 try/catch 在 `-O1/-O2` 下失效）。
  验证：`-O0` 与 `-O2` 双跑全套 109/109（`tests/run_tests_O2.sh`）+ ASAN 109/109。
  设计见 `docs/optimization_debugging.md`。

### v3.0.0
**里程碑版本**：整合 v2.4 系列全部累积，语言与运行时能力大幅扩展。所有变更 additive，语言规格保持 1.0。

- **协程完整体系（C1-C10）**：`@coro` 方法/顶层函数 + `await` 挂起/恢复 + 值传递 + 返回值槽 +
  自动调度器（`Coro.scheduler`）+ 事件等待（`await ClassName.eventName`）+ 超时（`await event timeout N`）+
  多事件等待（`Coro.waitAny`）+ 嵌套协程（`ret_ctx` 上下文链）+ 诊断（`Coro.status/current/count`）+
  协作式取消（`requestCancel`）+ 协程内异常边界（未捕获异常安全结束协程，不崩进程）+
  栈池复用 + `@coro(stack=N)` 栈可配置 + TLS 线程并用
- **协程间通信**：`stdlib/channel.myp` 缓冲通道（协程阻塞 send/recv + 唤醒）；协程 await Future（非阻塞等待）
- **并发/运行时**：动态事件等待表与动态事件队列（无 1024 硬上限）、工作窃取线程池（条件变量唤醒）、
  Barrier / Future / Promise
- **语言**：class 顶层 const、property 默认值修复（`int x = 5` 在 `new` 时生效）、Range for、
  类型推断 `var`、`long` 字面量后缀
- **语法**：算子系统（struct `operator:` 节 + 顶层 `@op("...")`）、管道 `|>`
- **工程**：异常机制完善（finally 全路径/`throw;` 重抛/对象异常/接口匹配）、LSP 稳定性修复、
  测试基础设施（普通 + ASAN 全套 109/109）

> **V2.0 → V3.0 升级说明**：见 [docs/UPGRADE_V3.md](UPGRADE_V3.md)。

### v2.4.3
- **协程（C1-C4 完整落地，additive）**：`@coro` 类 action 方法 + `await` 挂起/恢复
  - C1：spawn（create/set_entry/入口参数槽/首启）+ 手动 `resume`；`await;` 简单挂起
  - C2：`await` 值传递（`int v = await expr;`）+ `@coro` 返回值槽（`Coro.result`）
  - C3：自动调度器（就绪队列 + `Coro.scheduler()` round-robin）
  - C4：事件集成（`await ClassName.eventName` 阻塞等待 + 事件派发通知）
  - 用户 API 为编译器内建静态类 `Coro`（scheduler/resume/yield/isActive/destroy/result/waitEvent），
    `__myp_coro_*` 符号未注册（用户调用即 undefined）；`stdlib/coro.myp` 无 FFI 声明
  - 回归测试 `tests/coro/` + `tests/coro_auto/` + `tests/coro_event/`（普通 + ASAN 全套 94/94 通过）
  - 设计规范见 `docs/coro.md`
- **异常机制完善（additive）**：`finally` 全路径传播（return/break/continue）、`throw;` 重抛、
  未处理异常消息+abort、标准异常类（FileError/JsonError/NetError 等库接入）、`catch (Error e)` 接口匹配
- **LSP 稳定性**：修复 `didOpen`/`didChange` JSON 解码 bug（`\n`/`\t` 转义处理），消除 35GB 内存爆炸
- **测试基础设施**：`run_tests.sh` diff 改用临时文件（规避 process-substitution FIFO 不可靠）；
  `time`/`timeline` 测试改为确定性断言（消除 sleep 精度 flaky）

### v2.4.2
- **管道 `|>`（P3，additive）**：算子组件流水线
  - `A |> Op`：Op 为算子类名（自动实例化）或实例（复用），调用其 `transform`
  - 左结合链式 `A |> Op1 |> Op2`（= `Op2.transform(Op1.transform(A))`）
  - 新增 token `|>`；低优先级（高于赋值）；配合 SetOp 契约
  - 回归测试 `tests/pipe/`
- **CI + 测试确定性**：
  - 新增 GitHub Actions（`.github/workflows/ci.yml`）：构建 + 全套测试 + fuzz + ASAN
  - `date`/`process` 测试改为确定性（移除墙钟日期与原始 PID）——套件从"84 通过 2 抖动"到"84 通过 0 失败"

### v2.4.1
- **算子系统（P1+P2，additive）**：`运算符 = 算子` 统一模型实施
  - struct `operator:` 节 + `@op("+")` 注解 → 值类型数学算子（`v + u`、`v * s`）
  - 顶层 `@op("+")` 函数 → 外部算子（内置类型 `A + B`、对称二元 `s * v`、跨模块）
  - 重载解析顺序：内建 → struct 内 → 外部（标量热路径不受影响）
  - 新增关键字 `operator`；设计规范见 `docs/operators.md`
  - 回归测试 `tests/operators/`（正常 + ASAN 套件通过）

### v2.4.0
- **语法冻结**：正式发布 `docs/grammar.md`（EBNF 语言规格 v1.0），建立版本策略
  与变更日志；`mypc --version` 同时输出编译器版本与语言规格版本。
- **稳定性**：安装 LLVM 致命错误处理器，把 `LLVM ERROR: ...` 崩溃转为干净的
  内部错误诊断（编译器"永不崩溃"保证的一部分）。
- **先前累积（自 v2.3 起）**：
  - GPU 计算：`@gpu for` 支持数学函数（编译时链接 CUDA libdevice）、`stdlib/cuda.myp`
    （设备信息 / GPU 数学 / Vectors 归约 / Matrix）。
  - 稳定性：ASAN/UBSAN 构建、修复 3 个真实内存 bug、fuzz 崩溃判定、no-crash 回归。
  - 标准库：`process`、`args`、`env`、`json`、`regex`、`base64`、`net`、
    `collections`（PriorityQueue / LinkedList）、字符串增强、`date`、`logger`。
  - 内存管理：字符串/JSON/文件系统函数改用 arena 追踪分配，修复泄漏。
  - LSP：惰性解析 + 循环提前退出，修复服务器死机。
  - BNCT：原生 MYP 引擎 + 蒙特卡洛示例 + 并行计算支持。
  - 模块化：新增模块测试体系。

### v2.3
- 接口多态 + mapping 增强（文件级函数作为链节点）+ 标准库流类型。
- `stdlib/atomic.myp`：原子操作（LLVM atomicrmw）。
- `stdlib/ui.myp`：终端 TUI 框架。
- SDL2 GUI 支持（FFI + C 桥接层 + `stdlib/sdl.myp`）。
- 逗号分隔多变量声明 `int a=1, b=2;`。
- Barrier / Future/Promise / `await` / `@coro` 初步实现。
- `const` 关键字（语法层面）。
- 位运算符 `& | ^ << >>` 完整支持。
- struct 方法完整支持 + `@static` 类属性访问。

### v2.2
- 内置测试框架（`--test` + `stdlib/test.myp` 的 `Test` 类）。
- `myp fmt` 格式化工具。
- 标准库扩充；非标准库文件移除 `__myp_*` 内部调用。

### v2.1
- 泛型、枚举、lambda、FFI、包管理器（`--package-path`）、LSP、VS Code 扩展。

### v2.0
- `var` 类型推断、字符串插值、区间 `a..b`、`myp_viz`。

### v1.x
- 事件驱动组件语言原型：`class/action/event/property/mapping` 核心。

---

## 破坏性变更记录

（规格 v1.0 前无记录义务。此节自 v1.0 起必须维护。）

- 无（尚无破坏性变更）。

---

## 待定 / 路线图

- 编译器"永不崩溃"保证的其余部分：fuzz 持续集成、断言覆盖。
- 自举（用 MYP 实现编译器前端）所需的最小语法子集审计。
- 算子系统（`运算符 = 算子` 统一模型）：struct `operator:` 节 + 外部 `@op` 函数 +
  `|>` 管道。设计提案见 `docs/operators.md`，实施为 additive 变更。
