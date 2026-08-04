# MYP 协程设计（Coroutines）

> 状态：**实施中 — C1/C2/C3/C4 已完成**（路线全部落地）
> 关联：语言规格 v1.0（`docs/grammar.md`）、现有实现（`src/runtime/runtime.c` +
> `src/codegen/codegen.cpp` + `stdlib/coro.myp`）
> 本文档完善 MYP 协程机制：**`@coro` 方法 + `await` 挂起/恢复 + 自动调度 + 事件集成**。

---

## 1. 背景与现状

### 1.1 现有实现

| 项 | 现状 |
|---|---|
| 运行时原语 | 完整 ucontext 用户态纤程：`__myp_coro_create` / `set_entry` / `yield` / `resume` / `is_active` / `destroy` / `current_handle` / `count` / `status` / `wait_event` / `wait_event_timeout` / `wait_any` / `request_cancel` / `cancel_requested` / `cancel_clear`；**动态槽位（无硬上限，按需扩容）**、**动态事件等待表（无 1024 硬上限）**、**栈池复用（上限 128，省 spawn/destroy 反复 malloc/free）**、128KB 栈（可配）、`swapcontext` 调度；**每协程独立返回上下文（`ret_ctx`，支持嵌套 resume）**；**`destroy` 自杀防护**；**协作式取消**（标记式，协程自行退出）；**协程状态线程本地（`__thread`）**——协程绑定创建线程，可与 `@thread` 线程并用（runtime.c §Coroutine）|
| 语法 | `@coro` 注解（`has_coro` 标记，parser 已解析，作用于**类 action 方法**和**顶层函数**）+ `await` 语句/表达式（`AwaitStmt` + `AwaitExpr`，支持 `await;` / `await expr;` / `int v = await expr;` / `await ClassName.eventName` / `await ClassName.eventName timeout N`）；`await` 仅限 `@coro` 上下文（sema `in_coro_method_`）|
| 标准库 | `stdlib/coro.myp`：静态类 `Coro`（`scheduler`/`resume`/`yield`/`isActive`/`destroy`/`result`/`waitEvent`/`current`/`count`/`status`/`waitEventTimeout`/`waitAny`/`requestCancel`/`cancelRequested`/`clearCancel`）；`Coro` 为编译器内建静态类（codegen 直接生成底层调用），`__myp_coro_*` 符号未注册、无 FFI 暴露 |

### 1.2 现状缺口（半成品 → 已解决项）

| 缺口 | 状态 |
|---|---|
| `@coro` 无 codegen 调度 | ✅ C1：`generateClass` 生成协程入口包装 + `generateCall` 生成 spawn |
| 无函数地址机制 | ✅ C1：codegen 直接 `ptrtoint(入口包装)` 传入 `set_entry` |
| `stdlib/coro.myp` FFI 签名错误（`set_func` vs `set_entry`）| ✅ C1：改为 `set_entry(handle, fn_ptr)`，fn_ptr 用 `long` 承载 64 位指针 |
| `await` 值传递 | ✅ C2：`yield(val)`/`resume(h,val)` 双向传递 + `int v = await expr;` |
| 协程返回值 | ✅ C2：`@coro` 方法 `return` 存结果槽 + `Coro.result(h)` |
| 自动调度 | ✅ C3：就绪队列 + `Coro.scheduler()` 每轮驱动所有就绪协程各一步 |
| 事件集成 | ✅ C4：`await ClassName.eventName` 阻塞等待，事件 fire 后重新就绪 |
| 用户 API 风格 | ✅ `Coro` 内建静态类（scheduler/resume/yield/isActive/destroy/result/waitEvent/current/count/status/waitEventTimeout/waitAny/requestCancel/cancelRequested/clearCancel）；`__myp_coro_*` 符号未注册，用户调用即 undefined |
| 事件等待表硬上限 | ✅ 动态化（原 1024 固定表满 → 静默死锁缺陷已修复）；`tests/coro_more/` 1100 并发等待验证 |
| `destroy` 正在运行协程 | ✅ 自杀防护：不释放正在执行的栈（协程继续运行到结束，栈延迟回收）|
| 嵌套协程 | ✅ 同类 `@coro` 方法相互 spawn 的 codegen 预填充修复 + 每协程独立 `ret_ctx` 嵌套 resume 上下文链；`tests/coro_nest/` |
| 超时等待 | ✅ `await event timeout N` + `Coro.waitEventTimeout`，超时返回 -1 vs 事件到达；`tests/coro_timeout/` |
| 多事件等待 | ✅ `Coro.waitAny(ids, count, timeoutMs, val)` 返回触发事件 id；`tests/coro_any/` |
| 协程诊断 | ✅ `Coro.status(h)`（-1 无效/0 结束/1 就绪运行/2 等待事件）+ `Coro.current()`/`Coro.count()`；`tests/coro_status/` |
| 协作式取消 | ✅ `requestCancel`/`cancelRequested`/`clearCancel`——协程在 await/yield 后自行检查退出（可清理），区别于 destroy 强杀；`tests/coro_cancel/` |
| spawn/destroy 性能 | ✅ 栈池复用（上限 128），避免反复 malloc/free |
| 无测试/示例 | ✅ `tests/coro/` + `tests/coro_auto/` + `tests/coro_event/` + `tests/coro_capacity/` + `tests/coro_stack/` + `tests/coro_thread/` + `tests/coro_top/` + `tests/coro_more/`（C1-C4 + 容量 + 栈可配置 + 线程并用 + 顶层 @coro + current/count/自杀防护/大容量事件等待）|
| 文档过时 | ✅ 本文档 + grammar/manual/design 已更新 |

---

## 2. 目标特性

1. **`@coro` 函数**：声明为协程入口，调用即创建并启动协程，返回 handle
2. **`await` 挂起/恢复（带值）**：`int v = await expr;` —— 挂起时把 `expr` 值交给调度器，恢复时 `await` 表达式 = 调度器传入值
3. **协程调度**：手动 `resume(handle, val)` + 自动调度器（协程就绪队列）
4. **生命周期**：`destroy` / 自动回收
5. **与事件循环集成**（可选）：`await event` 在事件到达时恢复
6. **线程模型**：协程绑定创建它的线程（线程本地调度器）

---

## 3. 语法设计（全部 additive）

```ebnf
ActionDecl     ::= '@coro' '(' 'stack' '=' Integer ')'? Type Identifier '(' Params ')' Block
                  // 协程方法（类 action 段）；@coro(stack=N)：栈大小 KB（默认 128）
TopFuncDecl    ::= '@coro' '(' 'stack' '=' Integer ')'? Type Identifier '(' Params ')' Block
                  // 顶层协程函数（无需类封装）；返回 handle，参数经入口槽传递
AwaitStmt      ::= 'await' ';'                                   // 简单挂起（C1）
                 | 'await' Expression ';'                        // 带值挂起（C2）
                 | 'await' ClassName '.' EventName ';'           // 等待事件（C4）
                 | 'await' ClassName '.' EventName 'timeout' Integer ';'  // 等待事件带超时（C10，毫秒）
AwaitExpr      ::= 'await' Expression                            // 表达式：int v = await expr;（C2）
                 | 'await' ClassName '.' EventName               // 表达式：事件等待（C4）
                 | 'await' ClassName '.' EventName 'timeout' Integer  // 表达式：事件等待带超时（C10）
CoroCall       ::= Object '.' Identifier '(' Arguments ')'       // @coro 方法调用 = 启动协程
                 | Identifier '(' Arguments ')'                  // 顶层 @coro 函数调用 = 启动协程
                  // 返回 long handle（协程句柄）
```

> **`await` 上下文检查**：`await` 只能在 `@coro` 方法或顶层 `@coro` 函数内使用。
> 普通 action / `function:` / `static:` 段或普通顶层函数中的 `await` 会报错
> `'await' is only allowed inside an '@coro' method`（sema `in_coro_method_` 标志）。

**C1 已实现的用法**（`@coro` 是**类 action 方法**或**顶层函数**，`await;` 简单挂起，参数通过入口槽传递）：

```myp
import env;    // Console
import coro;   // 协程 FFI

// 顶层 @coro 函数：无需类封装，调用即启动协程，返回 handle
@coro long worker(long n) {
    long x = Coro.yield(n * 2);     // 挂起并传出 n*2；恢复时 x = resume 传入值
    return x + 100;                 // 存结果槽，Coro.result(h) 读取
}

class Worker {
    property:
        string label_;
    action:
        void setLabel(string s) { label_ = s; }
        @coro void run() {              // 协程方法（可带参数）
            Console.writeString(label_); Console.writeString(":1\n");
            await;                      // 挂起
            Console.writeString(label_); Console.writeString(":2\n");
        }
}

class Main {
    action:
        @startup void run() {
            Worker a = new Worker();  a.setLabel("A");
            long ha = a.run();          // spawn：创建 + 首启到第一个 await，返回 handle
            Console.writeString("main\n");
            Coro.resume(ha, 0);         // 恢复：从 await 处继续到协程结束
            Console.writeString("done\n");
        }
}

int main() { Main m = new Main(); return 0; }
```

**输出**（手动调度）：
```
A:1
main
A:2
done
```

> **C1 说明**：`@coro` 方法调用 `obj.meth(args)` 编译为 spawn（`create` + 入口参数槽 +
> `set_entry` + 首启 `resume`），返回 `long` handle。入口包装 `__myp_coro_entry_<类>_<方法>`
> 从线程本地入口参数槽（`this`=槽 0，参数=槽 1..N）读出并调用真实方法。`await;` 简单挂起，
> 返回后由 `Coro.resume(h, 0)` 恢复。协程自然结束自动回收槽；`Coro.destroy(h)` 提前取消；
> 进程退出 `atexit` 统一释放栈。

**C2 值传递 + 返回值**：

```myp
class Worker {
    action:
        // 值传递：挂起传出 n*2；恢复后 v = resume 传入值
        @coro void echo(int n) {
            int v = await n * 2;              // await 绑定完整表达式 = await (n*2)
            Console.writeString("v="); Console.write(v); Console.writeString("\n");
        }
        // 返回值：return 存入结果槽，__myp_coro_result(h) 读取
        @coro int compute() {
            await;
            return 42;
        }
}

class Main {
    action:
        @startup void run() {
            Worker w = new Worker();
            long h = w.echo(5);                       // spawn，yield 传出 10
            long out = Coro.resume(h, 100);           // 传 100 → v=100；out = 10
            long hc = w.compute();                    // spawn 到 await 挂起
            Coro.resume(hc, 0);                       // 恢复：return 42
            Console.write(Coro.result(hc));           // → 42
        }
}
```

> **C2 说明**：`await expr` 是**表达式**（`int v = await expr;`），绑定完整操作数
> （`await n * 2` == `await (n * 2)`）。编译为 `__myp_coro_yield(expr)`，恢复后
> 表达式值 = `resume` 传入值。`@coro` 方法 `return val` 自动调用 `__myp_coro_set_result`
> 存入 per-协程结果槽，`Coro.result(h)` 读取。`Coro.resume(h, val)` 返回协程
> 挂起时传出的值（结束则 0）。

**C3 自动调度**（就绪队列 + `Coro.scheduler()`）：

```myp
class Worker {
    action:
        @coro void run() {
            Console.writeString("a\n"); await;
            Console.writeString("b\n"); await;
            Console.writeString("c\n");
        }
}

// spawn 多个协程（都自动入就绪队列）
long h1 = a.run();  // → a, yield
long h2 = b.run();  // → a, yield
Coro.scheduler();   // 每轮驱动所有就绪协程各一步：b, b
Coro.scheduler();   // c, c（结束）
```

> **C3 说明**：spawn 的协程自动加入**就绪队列**；普通 `await` 挂起后仍就绪。
> `Coro.scheduler()` 先处理已排队事件，再对就绪协程**各 resume 一步**
> （跑到下一个 `await` 或结束），一轮后返回——空转无害。手动 `Coro.resume(h, val)`
> 仍可用，与调度器并存。

**C4 事件集成**（`await ClassName.eventName`）：

```myp
class Signal {
    action:
        void send() { go(); }        // 类 action 内裸名 fire 事件
    event:
        go();
}

class Waiter {
    action:
        @coro void run() {
            Console.writeString("waiting\n");
            await Signal.go;          // 阻塞直到 go 事件
            Console.writeString("got go\n");
        }
}

long h = w.run();        // spawn → waiting，事件等待（移出就绪队列）
Signal s = new Signal();
s.send();                // fire go → 派发 → waiter 重新就绪
Coro.scheduler();        // 跑就绪 waiter → got go
```

> **C4 说明**：`await ClassName.eventName` 把当前协程**移出就绪队列**（阻塞）并注册到
> 事件等待表；事件 fire → 派发（`myp_event_dispatch`）→ 通知匹配等待者**重新就绪**；
> 之后 `Coro.scheduler()`（或手动 resume）驱动其继续。事件引用只在 `await`
> 上下文中识别（`ClassName.eventName` 不改变普通成员访问语义）。

---

## 4. 实现方案

### 4.1 方案 A（推荐，C1/C2 已采用）：复用现有 ucontext 原语

协程方法保持普通方法（编译为普通 LLVM 函数），通过现有 ucontext 纤程运行：

- **启动**：`@coro fn(args)` 调用 → codegen 生成
  `__myp_coro_create()` + `__myp_coro_set_entry(h, ptrtoint(fn))` + 参数槽 + `resume(h)`（首启）✅ C1
- **挂起**：`await expr` → 求值 `expr` 存入**线程本地值槽**，调 `__myp_coro_yield(val)`，恢复后从槽读回 ✅ C2
- **恢复**：`__myp_coro_resume(h, val)` → 存 `val` 到槽，`swapcontext` 进协程；协程 `await` 处读槽得值 ✅ C2
- **参数**：协程方法参数在启动时存入**线程本地入口参数槽**（`this`=槽 0，参数=槽 1..N）✅ C1
- **返回值**：协程 `return` 前把值存入协程**结果槽**（`__myp_coro_set_result`）；`__myp_coro_result(h)` 读取 ✅ C2
- **自动调度**：协程带 `ready` 标记，spawn 入就绪队列；`__myp_coro_scheduler()` 先处理事件再对就绪协程各 resume 一步（round-robin 快照，避免一轮内重入）✅ C3
- **事件等待**：`await ClassName.eventName` → `__myp_coro_wait_event(event_id)` 注册等待、移出就绪队列并挂起；`myp_event_dispatch` 派发后 `__myp_coro_event_notify` 重新就绪匹配等待者 ✅ C4

**优点**：runtime 已就绪，改动集中在 codegen 的启动/await 值传递 + stdlib 修复；无栈转换。
**缺点**：每协程 256KB 栈（内存开销）；参数/返回值走共享槽（非寄存器级）。

### 4.2 方案 B（备选）：状态机转换

编译器把 `@coro` 函数拆成状态机：每个 `await` 一个状态，局部变量存堆/协程帧；恢复 = 跳转对应状态。

**优点**：无栈切换、内存小、可嵌套自然。
**缺点**：实现复杂（跨 `await` 的局部变量保存、IR 拆分、与现有 codegen 集成难）；相当于实现 C++ 协程等价物。

> **推荐 v1 用方案 A**（复用现有原语，快速打通）；方案 B 作为 v2 优化方向。

---

## 5. 运行时扩展（方案 A）

| 函数 | 说明 | 状态 |
|---|---|---|
| `__myp_coro_yield(int64 val)` → `int64` | 挂起并传出值；恢复时返回传入值 | ✅ C2 |
| `__myp_coro_resume(int64 handle, int64 val)` → `int64` | 恢复协程并传入值；返回协程挂起时传出的值（结束则 0，无效 -1）| ✅ C2 |
| `__myp_coro_set_entry_arg(int64 idx, int64 val)` / `get_entry_arg` | 入口参数槽（`this`=0，参数=1..N）| ✅ C1 |
| `__myp_coro_set_result(int64 val)` / `__myp_coro_result(int64 handle)` → `int64` | 协程返回值槽（per-协程）| ✅ C2 |
| `__myp_coro_scheduler()` → `void` | 自动调度：处理事件 + 对就绪协程各 resume 一步（round-robin）| ✅ C3 |
| `__myp_coro_wait_event(int64 event_id, int64 val)` → `int64` | 等待事件（阻塞，移出就绪队列）；`myp_event_dispatch` 通知重新就绪；**动态等待表（无 1024 硬上限）** | ✅ C4 |
| `__myp_coro_current_handle()` → `int64` | 当前正在执行的协程 handle（不在协程内 -1）| ✅ C9 |
| `__myp_coro_count()` → `int64` | 当前线程活跃（未结束）协程数 | ✅ C9 |
| `__myp_coro_status(int64 handle)` → `int64` | 协程状态：-1 无效 / 0 结束 / 1 就绪运行 / 2 等待事件 | ✅ C10 |
| `__myp_coro_wait_event_timeout(int64 id, int64 ms, int64 val)` → `int64` | 带超时事件等待：事件到达返回 resume 传入值，超时返回 -1 | ✅ C10 |
| `__myp_coro_wait_any(const int64* ids, int64 count, int64 ms, int64 val)` → `int64` | 多事件等待：任一触发返回事件 id，超时返回 -1 | ✅ C10 |
| `__myp_coro_request_cancel(int64 h)` / `cancel_requested()` / `cancel_clear()` | 协作式取消：设置/读取/清除取消标记（协程自行退出）| ✅ C10 |
| `__myp_coro_destroy(int64 handle)` | 取消协程；**自杀防护**：不释放正在执行的协程栈（延迟回收）；栈入池复用 | ✅ C9 |
| `__myp_coro_spawn(fn_ptr, arg_slot)` → `int` | 便捷：create + set_entry + 首启（编译器 spawn 已覆盖，非独立 FFI）| — |
| `__myp_coro_set_args(int handle, ...)` | 参数按签名设置（已由入口槽实现）| ✅ C1 |

- 值槽：线程本地（每线程 `yield_val` / `resume_val`）+ 每协程 `result` 槽
- 调度：每协程 `ready` 标记（就绪队列）；`wait_event` 置 0（阻塞），事件通知置 1
- 类型：v1 支持整数/句柄/指针（int64 承载）；浮点经 `bitcast`（C2 已验证 double 参数）

---

## 6. 实现面评估

| 层 | 改动 |
|---|---|
| **Parser** | `@coro` 注解（支持 `@coro(stack=N)` KB，作用于类 action 方法与顶层函数）+ `await` 语句/表达式（`AwaitStmt`/`AwaitExpr`，支持 `await;` / `await expr;` / `await ClassName.eventName` 事件等待 / `await ClassName.eventName timeout N`）|
| **Sema** | `@coro` 方法/顶层函数调用返回 handle（long）；`await` 表达式类型（long）；`await` 仅限 `@coro` 上下文（`in_coro_method_` 标志）；`await ClassName.eventName` 事件引用识别；`__myp_coro_*` 不注册符号（对用户隐藏）|
| **Codegen** | `@coro` 调用 → spawn（create(stack_bytes)/入口参数槽/set_entry/首启）；顶层 `@coro` 函数入口包装 `generateCoroFuncEntry`（无 this 槽）+ 预扫描解决定义顺序与同类互 spawn；`await` 值传递（yield 带值 + 恢复取回）；`return` 存结果槽；`Coro` 内建静态类 → 直接生成 runtime 调用；`await event` → wait_event；`await event timeout N` → wait_event_timeout；`waitAny` slice 解包 |
| **Runtime** | ucontext 纤程原语；`create(stack_bytes)` 支持每协程自定义栈；**协程状态线程本地（TLS）**——每线程独立槽数组/调度上下文/事件等待表，线程退出自动清理；线程本地值槽（yield/resume）；per-协程 result 槽；入口参数槽；动态槽位（无硬上限）；**动态事件等待表（无硬上限）+ 超时过期检查**；**每协程独立返回上下文 `ret_ctx`（嵌套 resume 上下文链）**；**栈池复用（上限 128）**；就绪队列 + `__myp_coro_scheduler()`；事件等待表 + 派发通知；`destroy` 自杀防护 + wait 记录清理；`current`/`count`/`status`/`wait_any`/协作式取消 |
| **Stdlib** | `stdlib/coro.myp`：`Coro` 内建静态类（scheduler/resume/yield/isActive/destroy/result/waitEvent/current/count/status/waitEventTimeout/waitAny/requestCancel/cancelRequested/clearCancel），无 FFI 声明、无内部符号暴露 |
| **测试** | `tests/coro/`（C1+C2）+ `tests/coro_auto/`（C3）+ `tests/coro_event/`（C4）+ `tests/coro_capacity/` + `tests/coro_stack/` + `tests/coro_thread/` + `tests/coro_top/` + `tests/coro_more/` + `tests/coro_nest/` + `tests/coro_status/` + `tests/coro_timeout/` + `tests/coro_any/` + `tests/coro_cancel/`；普通 + ASAN 全套 105/105 |

**风险**：ucontext 栈切换正确性（挂起点恢复、值槽线程本地）是核心；改动集中在协程路径，不碰正常执行路径，符合 v1.0 非破坏约束。C1-C4 全部落地，无未决设计项（见 §7）。

---

## 7. 决策记录（原评审清单 → 已决策）

| # | 问题 | 决策 |
|---|---|---|
| 1 | 启动方式 | `@coro fn(args)` 直接调用返回 handle（C1 实现）|
| 2 | await 值传递 | `int v = await expr;` 双向传递（C2 实现）|
| 3 | 参数传递 | 入口参数槽（`this`=槽0，参数=槽1..N）（C1 实现）|
| 4 | 返回值 | `Coro.result(h)`（C2 实现）|
| 5 | 调度 | 手动 `Coro.resume` + 自动调度器（`Coro.scheduler()` round-robin）（C3 实现）|
| 6 | 事件集成 | `await ClassName.eventName` 阻塞等待 + 事件派发通知（C4 实现）|
| 7 | 栈大小 | `@coro(stack=N)`（KB，默认 128）可配置；已实现 ✅ |
| 8 | 生命周期 | 自然结束自动回收槽 + `Coro.destroy` 显式取消 + `atexit` 统一释放（C1 实现）|
| 9 | 线程模型 | 协程绑定创建线程：协程状态 `__thread`（TLS）线程本地，每线程独立槽/调度上下文/等待表，线程退出自动清理；可与 `@thread` 线程并用 ✅ 已实现 |
| 10 | 用户 API 风格 | 静态类 `Coro` 封装（`Coro.*`），`__myp_*` 仅编译器内部使用 |
| 11 | 栈溢出防护 | ucontext 固定栈溢出会破坏相邻内存；当前：默认 128KB + `@coro(stack=N)` 可配置 + 极小栈（<16KB）编译警告（C8 已实现）；完整 guard-page（mmap + SIGSEGV handler）或 canary 检测为平台相关改动，暂缓评估 |
| 12 | 取消语义 | `destroy` = 强杀（不执行 finally/清理，栈入池复用）；`requestCancel` = 协作式取消（协程在 await/yield 后检查 `cancelRequested()` 自行退出，可清理）。取 Go context/C# token 的协作式路径 ✅ C10 |
| 13 | 嵌套调度 | 每协程独立返回上下文 `ret_ctx`：协程内可 resume 另一协程（子协程返回父协程，父返回其调用者）；替代共享 `sched_ctx`（无法表达嵌套返回链）✅ C10 |

---

## 8. 实施路线

| 阶段 | 内容 | 状态 |
|---|---|---|
| C1 | 修复 `stdlib/coro.myp` FFI（`set_entry`）+ `@coro` 启动 codegen（create/set_entry/首启）+ 手动 `resume` | ✅ 已完成（含入口参数槽：`this`+参数）|
| C2 | `await` 值传递（yield 带值 + 恢复取回）+ 协程返回值槽 | ✅ 已完成（`int v = await expr;` + `Coro.result`）|
| C3 | 自动调度器（就绪队列 + resume 循环）| ✅ 已完成（`Coro.scheduler()` round-robin）|
| C4 | 事件集成（`await event`）+ 扩展 `tests/coro/` + grammar.md/manual.md/design.md 收尾 | ✅ 已完成（`await ClassName.eventName` + 全套测试 + 文档）|
| C5 | `@coro(stack=N)` 栈大小可配置（KB，默认 128） | ✅ 已完成（深递归验证栈生效）|
| C6 | 协程与线程并用：协程状态线程本地化（TLS），多 `@thread` 线程各自独立跑协程 | ✅ 已完成（`tests/coro_thread/`）|
| C7 | 顶层 `@coro` 函数（无需类封装）+ `await` 上下文检查（仅 `@coro` 内可用）| ✅ 已完成（`tests/coro_top/`；普通方法中 `await` 报错）|
| C8 | 栈溢出防护（诊断层）：`@coro(stack=N)` 栈 < 16KB 编译警告，防止静默内存损坏 | ✅ 已完成（极小栈警告；完整 guard-page/canary 保护待评估）|
| C9 | 加固：动态事件等待表（修复 1024 上限静默死锁）+ `destroy` 自杀防护 + `Coro.current()`/`Coro.count()` | ✅ 已完成（`tests/coro_more/`：1100 并发等待 + 自杀 + current/count）|
| C10 | 进阶：嵌套协程（同类互 spawn 修复 + `ret_ctx` 上下文链）+ `Coro.status()` + 超时等待（`await event timeout N`/`waitEventTimeout`）+ 多事件等待 `waitAny` + 栈池复用 + 协作式取消 | ✅ 已完成（`tests/coro_nest/status/timeout/any/cancel`；普通 + ASAN 105/105）|

每阶段独立可验证：构建（正常 + ASAN）+ 全套测试（105/105）+ no-crash 回归。

---

## 9. 性能测试

基准脚本：`examples/coro_bench.myp`（`./build/mypc examples/coro_bench.myp && ./examples/coro_bench.out`），
用 `Time.nowMs()`（毫秒精度）+ 大量迭代放大测量：

| 指标 | 方法 | 实测（参考机器）|
|---|---|---|
| **切换开销** | 1 个协程循环 `await` N 次，主流程 `Coro.resume(h,0)` N 次；总耗时 ÷ (2N) | **~98 ns/切换**（单次 `swapcontext`）|
| **函数调用基线** | 普通函数调用 N 次（对比）| ~1 ns/次 |
| **自动调度** | M 个协程 × R 轮 `Coro.scheduler()`；总耗时 ÷ (R×M×2) | **~99 ns/切换**（含事件处理，与手动 resume 相当）|
| **spawn+destroy** | 循环 `Coro.create`+`destroy` S 次；总耗时 ÷ S | **~1000 ns/次**（含 256KB 栈分配）|

要点：
- 每次 `yield`/`resume` 各是一次 `swapcontext`（保存/恢复全部寄存器 + 切换栈），
  故 1 轮 resume+yield = 2 次切换。
- 协程切换比普通函数调用约慢 2 个数量级，属用户态上下文切换的正常水平；
  与 `Coro.scheduler()` 自动调度相比，手动 resume 无额外开销差异（事件处理近似 0）。
- 协程内存：每协程 **128KB 栈**，M 个协程峰值 ≈ M×128KB（benchmark M=8 → 1MB；
  `tests/coro_capacity/` 用 1500 个 ≈ 192MB）。
- **协程数量上限**：动态槽位（初始 64，按需翻倍扩容），**无硬上限**，受内存约束；
  槽对象（含 ucontext）独立分配、地址稳定，扩容只移动指针数组。
- 栈大小注意：单协程运行至少需要 >64KB（MYP 方法栈帧 + ucontext），128KB 为安全默认。
