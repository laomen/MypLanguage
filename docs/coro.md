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
| 运行时原语 | 完整 ucontext 用户态纤程：`__myp_coro_create` / `set_entry` / `yield` / `resume` / `is_active` / `destroy`；**动态槽位（无硬上限，按需扩容）**、128KB 栈、`swapcontext` 调度（`runtime.c` §Coroutine）|
| 语法 | `@coro` 注解（`has_coro` 标记，parser 已解析，作用于**类 action 方法**）+ `await` 语句/表达式（`AwaitStmt` + `AwaitExpr`，支持 `await;` / `await expr;` / `int v = await expr;` / `await ClassName.eventName`）|
| 标准库 | `stdlib/coro.myp`：静态类 `Coro`（`scheduler`/`resume`/`yield`/`isActive`/`destroy`/`result`/`waitEvent`）；`Coro` 为编译器内建静态类（codegen 直接生成底层调用），`__myp_coro_*` 符号未注册、无 FFI 暴露 |

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
| 用户 API 风格 | ✅ `Coro` 内建静态类（scheduler/resume/yield/isActive/destroy/result/waitEvent）；`__myp_coro_*` 符号未注册，用户调用即 undefined |
| 无测试/示例 | ✅ `tests/coro/` + `tests/coro_auto/` + `tests/coro_event/` + `tests/coro_capacity/`（C1-C4 + 容量突破 1024 全覆盖）|
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
ActionDecl     ::= '@coro' Type Identifier '(' Params ')' Block   // 协程方法（类 action 段）
AwaitStmt      ::= 'await' ';'                                   // 简单挂起（C1）
                 | 'await' Expression ';'                        // 带值挂起（C2）
                 | 'await' ClassName '.' EventName ';'           // 等待事件（C4）
AwaitExpr      ::= 'await' Expression                            // 表达式：int v = await expr;（C2）
                 | 'await' ClassName '.' EventName               // 表达式：事件等待（C4）
CoroCall       ::= Object '.' Identifier '(' Arguments ')'       // @coro 方法调用 = 启动协程
                  // 返回 long handle（协程句柄）
```

**C1 已实现的用法**（`@coro` 是**类 action 方法**，`await;` 简单挂起，参数通过入口槽传递）：

```myp
import env;    // Console
import coro;   // 协程 FFI

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
| `__myp_coro_wait_event(int64 event_id, int64 val)` → `int64` | 等待事件（阻塞，移出就绪队列）；`myp_event_dispatch` 通知重新就绪 | ✅ C4 |
| `__myp_coro_spawn(fn_ptr, arg_slot)` → `int` | 便捷：create + set_entry + 首启（编译器 spawn 已覆盖，非独立 FFI）| — |
| `__myp_coro_set_args(int handle, ...)` | 参数按签名设置（已由入口槽实现）| ✅ C1 |

- 值槽：线程本地（每线程 `yield_val` / `resume_val`）+ 每协程 `result` 槽
- 调度：每协程 `ready` 标记（就绪队列）；`wait_event` 置 0（阻塞），事件通知置 1
- 类型：v1 支持整数/句柄/指针（int64 承载）；浮点经 `bitcast`（C2 已验证 double 参数）

---

## 6. 实现面评估

| 层 | 改动 |
|---|---|
| **Parser** | `@coro` 注解（类 action 方法）+ `await` 语句/表达式（`AwaitStmt`/`AwaitExpr`，支持 `await;` / `await expr;` / `await ClassName.eventName` 事件等待）|
| **Sema** | `@coro` 方法调用返回 handle；`await` 表达式类型（long）；`await ClassName.eventName` 事件引用识别；`__myp_coro_*` 不注册符号（对用户隐藏）|
| **Codegen** | `@coro` 调用 → spawn（create/入口参数槽/set_entry/首启）；`await` 值传递（yield 带值 + 恢复取回）；`return` 存结果槽；`Coro` 内建静态类 → 直接生成 runtime 调用；`await event` → wait_event |
| **Runtime** | ucontext 纤程原语；线程本地值槽（yield/resume）；per-协程 result 槽；入口参数槽；就绪队列（ready 标记）+ `__myp_coro_scheduler()`；事件等待表 + 派发通知 |
| **Stdlib** | `stdlib/coro.myp`：`Coro` 内建静态类（scheduler/resume/yield/isActive/destroy/result/waitEvent），无 FFI 声明、无内部符号暴露 |
| **测试** | `tests/coro/`（C1+C2）+ `tests/coro_auto/`（C3）+ `tests/coro_event/`（C4）+ `tests/coro_capacity/`（动态槽位突破 1024）；普通 + ASAN 全套 95/95 |

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
| 7 | 栈大小 | 128KB 固定（v1，安全默认）；`@coro(stack=...)` 可配置留作扩展 |
| 8 | 生命周期 | 自然结束自动回收槽 + `Coro.destroy` 显式取消 + `atexit` 统一释放（C1 实现）|
| 9 | 线程模型 | 协程绑定创建线程（线程本地调度器/值槽）（实现遵循）|
| 10 | 用户 API 风格 | 静态类 `Coro` 封装（`Coro.*`），`__myp_*` 仅编译器内部使用 |

---

## 8. 实施路线

| 阶段 | 内容 | 状态 |
|---|---|---|
| C1 | 修复 `stdlib/coro.myp` FFI（`set_entry`）+ `@coro` 启动 codegen（create/set_entry/首启）+ 手动 `resume` | ✅ 已完成（含入口参数槽：`this`+参数）|
| C2 | `await` 值传递（yield 带值 + 恢复取回）+ 协程返回值槽 | ✅ 已完成（`int v = await expr;` + `Coro.result`）|
| C3 | 自动调度器（就绪队列 + resume 循环）| ✅ 已完成（`Coro.scheduler()` round-robin）|
| C4 | 事件集成（`await event`）+ 扩展 `tests/coro/` + grammar.md/manual.md/design.md 收尾 | ✅ 已完成（`await ClassName.eventName` + 全套测试 + 文档）|

每阶段独立可验证：构建（正常 + ASAN）+ 全套测试（95/95）+ no-crash 回归。

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
