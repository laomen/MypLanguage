# MYP 协程设计（Coroutines）

> 状态：**设计提案 v0.1**（待评审后实施）
> 关联：语言规格 v1.0（`docs/grammar.md`）、现有实现（`src/runtime/runtime.c` +
> `src/codegen/codegen.cpp` + `stdlib/coro.myp`）
> 本文档完善 MYP 协程机制：**`@coro` 函数 + `await` 挂起/恢复 + 调度**。实施前请先评审。

---

## 1. 背景与现状

### 1.1 现有实现

| 项 | 现状 |
|---|---|
| 运行时原语 | 完整 ucontext 用户态纤程：`__myp_coro_create` / `set_entry` / `yield` / `resume` / `is_active` / `destroy`；每线程 1024 槽、256KB 栈、`swapcontext` 调度（`runtime.c` §Coroutine）|
| 语法 | `@coro` 注解（`has_coro` 标记，parser 已解析）+ `await` 语句（`AwaitStmt` → `__myp_coro_yield`）|
| 标准库 | `stdlib/coro.myp`：FFI 声明（`create`/`set_func`/`yield`/`resume`/`is_active`/`destroy`）|

### 1.2 现状缺口（半成品）

1. **`@coro` 无 codegen 调度**：`has_coro` 只标记，不生成协程启动/恢复代码
2. **无函数地址机制**：MYP 无法把 `@coro` 函数地址传给 `__myp_coro_set_entry`，无法从语言层启动真实协程
3. **`stdlib/coro.myp` FFI 签名错误**：声明 `__myp_coro_set_func(fn, arg)`，runtime 实际为 `__myp_coro_set_entry(handle, fn)` → 调用即链接失败
4. **`await` 无值传递**：`generateAwaitStmt` 仅调 `yield()`，不传/收值
5. **无测试/示例**：tests、examples 均无协程用例
6. **文档过时**：`docs/design.md` 标"规划中"

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
FuncDecl       ::= '@coro' Type Identifier '(' Params ')' Block   // 协程函数
AwaitStmt      ::= 'await' Expression ';'
                  // 挂起：Expression 值交给调度器；恢复时 await 表达式 = 传入值
CoroExpr       ::= Identifier '(' Arguments ')'                  // @coro 调用 = 启动协程
                  // 返回 int handle（协程句柄）
```

**示例**：

```myp
import coro;

@coro int worker(int n) {
    Console.writeString("w1\n");
    int v = await n * 2;        // 挂起，yield 值 = n*2；恢复时 v = 传入值
    Console.writeString("w2 ");
    Console.write(v);
    Console.writeString("\n");
    return v + 1;               // 协程结束，返回值存入协程槽
}

class Main {
    action:
        @startup void run() {
            int h = worker(5);              // @coro 调用 → 创建 + 首启到第一个 await
            Console.writeString("back1\n");
            int active = __myp_coro_resume(h, 100);   // 恢复，传 100；返回是否仍活动
            Console.writeString("back2\n");
            int result = __myp_coro_result(h);        // 协程结束后取返回值
            __myp_coro_destroy(h);
        }
}
```

**输出**（手动调度）：
```
w1
back1
w2 100
back2
```

---

## 4. 实现方案

### 4.1 方案 A（推荐）：复用现有 ucontext 原语

协程函数保持普通函数（编译为普通 LLVM 函数），通过现有 ucontext 纤程运行：

- **启动**：`@coro fn(args)` 调用 → codegen 生成
  `__myp_coro_create()` + `__myp_coro_set_entry(h, ptrtoint(fn))` + 参数槽 + `resume(h)`（首启）
- **挂起**：`await expr` → 求值 `expr` 存入**线程本地值槽**，调 `__myp_coro_yield()`，恢复后从槽读回
- **恢复**：`__myp_coro_resume(h, val)` → 存 `val` 到槽，`swapcontext` 进协程；协程 `await` 处读槽得值
- **参数**：协程函数参数在启动时存入**协程参数槽**（固定 N 个，或按 handle 的全局参数区）
- **返回值**：协程 `return` 前把值存入协程返回值槽；`__myp_coro_result(h)` 读取

**优点**：runtime 已就绪，改动集中在 codegen 的启动/await 值传递 + stdlib 修复；无栈转换。
**缺点**：每协程 256KB 栈（内存开销）；参数/返回值走共享槽（非寄存器级）。

### 4.2 方案 B（备选）：状态机转换

编译器把 `@coro` 函数拆成状态机：每个 `await` 一个状态，局部变量存堆/协程帧；恢复 = 跳转对应状态。

**优点**：无栈切换、内存小、可嵌套自然。
**缺点**：实现复杂（跨 `await` 的局部变量保存、IR 拆分、与现有 codegen 集成难）；相当于实现 C++ 协程等价物。

> **推荐 v1 用方案 A**（复用现有原语，快速打通）；方案 B 作为 v2 优化方向。

---

## 5. 运行时扩展（方案 A）

| 函数 | 说明 |
|---|---|
| `__myp_coro_yield(int64 val)` → `int64` | 挂起并传出值；恢复时返回传入值（替代现有无参 `yield()`）|
| `__myp_coro_resume(int handle, int64 val)` → `int` | 恢复协程并传入值；返回 `is_active` 结果 |
| `__myp_coro_set_args(int handle, ...)` | 设置协程函数参数（按签名）|
| `__myp_coro_result(int handle)` → `int64` | 协程结束后取返回值 |
| `__myp_coro_spawn(fn_ptr, arg_slot)` → `int` | 便捷：create + set_entry + 首启 |

- 值槽：线程本地（每线程一个 `yield_val` / `resume_val` / 协程参数区 / 结果区）
- 类型：v1 仅支持整数/句柄/指针（int64 承载）；浮点/复杂类型后续

---

## 6. 实现面评估

| 层 | 改动 |
|---|---|
| **Parser** | 已支持 `@coro`/`await`（无需改）；如需 `await` 带类型标注可扩展 |
| **Sema** | `@coro` 函数约束（无 `@region` 冲突、返回 int/handle 语义）；`await` 类型检查（expr 类型 → 值槽）|
| **Codegen** | `@coro` 调用 → spawn 代码（create/set_entry/参数/resume）；`await` 值传递（yield 带值 + 恢复取回）；`return` 存结果槽 |
| **Runtime** | `yield`/`resume` 带值；参数/结果槽；`spawn` 便捷函数 |
| **Stdlib** | 修复 `stdlib/coro.myp` FFI（`set_entry` 替换 `set_func`）；加 `spawn`/`result` 包装 |
| **测试** | `tests/coro/`：启动/挂起/恢复/值传递/返回值/销毁/多协程/自动调度 |

**风险**：ucontext 栈切换正确性（挂起点恢复、值槽线程本地）是核心；改动集中在协程路径，不碰正常执行路径，符合 v1.0 非破坏约束。

---

## 7. 待决问题（评审清单）

1. **启动方式**：`@coro fn(args)` 直接调用返回 handle（推荐），还是显式 `__myp_coro_spawn`？（§3）
2. **await 值传递**：`int v = await expr;` 的 `expr` 值 + 恢复传值是否都做？类型限制（int64/句柄 v1）？（§3/§5）
3. **参数传递**：协程函数参数怎么进协程？（参数槽 v1 vs 首启时栈传递）
4. **返回值**：`__myp_coro_result(h)` 读取？（§5）
5. **调度**：手动 `resume` 为主（v1），还是加自动调度器（就绪队列 + resume 循环）？（§2/§4）
6. **事件集成**：`await event`（事件到达恢复）是否纳入 v1？（§2 #5）
7. **栈大小**：固定 256KB 是否够？是否可配（`@coro(stack=...)`）？
8. **生命周期**：`destroy` 显式 vs 协程结束自动回收；handle 复用？
9. **线程模型**：协程绑定创建线程（线程本地调度器）确认？（§2 #6）

---

## 8. 实施路线（评审通过后）

| 阶段 | 内容 |
|---|---|
| C1 | 修复 `stdlib/coro.myp` FFI（`set_entry`）+ `@coro` 启动 codegen（create/set_entry/首启）+ 手动 `resume` |
| C2 | `await` 值传递（yield 带值 + 恢复取回）+ 协程参数/返回值槽 |
| C3 | 自动调度器（就绪队列）+ 多协程调度 |
| C4 | 事件集成（`await event`，可选）+ `tests/coro/` 测试 + grammar.md/manual.md/design.md 更新 |

每阶段独立可验证：构建（正常 + ASAN）+ 全套测试 + no-crash 回归。
