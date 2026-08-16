# MYP 编译器 Bug 跟踪清单（BUGLIST）

> 本文件跟踪 mypc（C++ oracle）的已知 bug。每个 bug 有唯一 ID、根因位置、
> 复现测试、当前状态。
>
> **状态图例**：🟥 未修复（复现红） · 🟨 已定位待修 · 🟩 已修复
>
> 复现测试放 `tests/bugs/`（用内建 `@test` 框架），运行：
> `MYPCC=./build/mypc bash tests/bugs/run_bugs.sh`（未修复时退出码非 0）。

## 状态总览

| ID | 状态 | 标题 | 复现测试 |
|----|------|------|----------|
| BUG-001 | 🟩 | 链式类字段访问产生垃圾值/崩溃 | `tests/negative/external_property_{read,write,chain}.myp`（编译拒绝） |
| BUG-002 | � | @coro 主流程增量 spawn 卡死/帧损坏 | `tests/bugs/coro_incremental_spawn.myp` |
| BUG-003 | � | 泛型 T=string 的 `<`/`>` 按指针比较 | `tests/bugs/generic_string_cmp.myp` |
| BUG-004 | 🟥 | `Option<struct>` 泛型实例化失败 | `tests/bugs/option_struct.myp` |

---

## BUG-001（已修复）：链式类字段访问产生垃圾值 / 崩溃

- **状态**：🟩 已修复（2026-08-16）
- **根因**：sema 此前允许外部读 class `property:`（"Properties — accessible
  from anywhere"），但 codegen 只正确支持 `this.prop` 与单级读；链式
  `o.mid.inner.val` 走 struct 字段链/名字兜底 → 垃圾值/段错误；链式写
  `o.mid.inner.val = 99` → codegen 用 null 基址 GEP 崩溃。
- **修复**：
  1. sema 收紧（C++ `src/sema/sema_expr.cpp` + 自举 `tools/selfhost/src/sema.myp`）：
     外部实例访问 class property（读+写）→ 编译错误
     `cannot access property 'X' of 'Y' from outside the class`。
     允许：`this.prop`、**同类另一实例**（C++ 私有成员语义，如 GpuBuffer 内
     `src.host_`）、`@static class` 的 `Class.prop`。
  2. 自举 AST（`ast.myp` 等 29 个纯数据 class）迁移为 getter 访问：
     跨实例直接读 `e.lhs_` → `e.lhs()`，新增 ~360 个 getter、改写 ~2900 处
     访问（含关键字冲突字段 `ref_→isRef()`、`const_→isConst()` 等 8 个特例，
     与无下划线字段 `AstPair.k/v/typeStr`、`AstNonlocalSlot.slot/cell` 改名）。
  3. 受影响测试改造成合规访问（getter/struct/setter），新增 3 个负测试。
- **验证**：自举 94/94；全量回归 270 通过、仅剩 BUG-003 导致的 `generic_traits`。
- **遗留**：自举源码中"链式字段访问产生垃圾 → 先用局部变量"的规避注释现可逐步清理。

---

## BUG-002（已修复）：@coro 主流程增量 spawn 卡死 / 帧损坏

- **状态**：🟩 已修复（2026-08-16）
- **根因**：@coro 方法/函数的**类引用参数（及 `this`）被借用、不 retain**，但协程
  比调用方作用域长寿。主流程每轮 `new Channel()` → spawn 过滤器 → `ch = nx` 释放
  旧 channel 后，旧 Channel 对象的唯一强引用就是已 park 协程的借用参数 → 对象被
  释放并被下一轮 `new Channel()` 复用 → 协程唤醒时 `in.handle_` 读到新对象的句柄
  （如 0 变 6）→ 过滤链错位、复合数漏过。全预 spawn 无此问题是因为所有 channel
  仍被主流程局部变量持有。
- **修复**（`src/codegen/codegen_class.cpp`）：新增 `registerCoroParam`——@coro 方法/
  函数入口对 `this` 与每个 ARC 参数（class/interface/function/slice/dyn-array/string/
  含 ARC 字段的 struct）**retain** + 注册为 ARC 作用域槽（协程正常完成时释放）+ 镜像
  进协程帧注册表（Coro.destroy/异常时释放）。普通函数参数保持借用不变。
- **验证**：`tests/bugs/coro_incremental_spawn.myp` 转绿（8/8 断言）；全量回归 273
  通过（`threadpool`/`coro_thread`/`coro_stack` 为既有 @thread/深递归时序 flaky，
  与本修复无关）；自举 94/94、两级自举 15/15。

---

## BUG-003（已修复）：泛型 T=string 的 `<`/`>` 按指针比较

- **状态**：🟩 已修复（2026-08-16）
- **根因**：codegen 的 string 比较判定用 `e.lhs->resolved_kind == String`。
  泛型模板体（共享 AST）中类型参数的 `resolved_kind` 是占位符默认值（如 Int），
  单态化时不会更新 → `T=string` 实例的比较退化为指针地址比较。
- **修复**（`src/codegen/codegen_expr.cpp`）：新增 `exprResolvedString(e)`：
  `e.resolved_kind == String || exprIsString(e)`——前者覆盖 sema 已解析的成员访问/
  下标/内建调用（如 `str(bytes(x))`）；后者按 alloca 指针类型兜底泛型类型参数。
  同时 `exprIsString` 排除动态数组（`array_elem_types_`），避免 `T[]` 被误判为 string。
- **验证**：`tests/bugs/generic_string_cmp.myp` 转绿（6/6 断言），`tests/generic_traits`
  回归同时转绿（`s=a` 实得 `s=a`）；全量回归 273 通过、仅剩 `coro_stack` 一处
  **既有 flaky**（深度递归 3000 层恰在 2048KB 栈边界，非本 bug 引入）。

---

## BUG-004（未修复）：`Option<struct>` 泛型实例化失败

- **状态**：🟥 未修复
- **严重度**：中（递归数据结构无法用纯 struct 表达时，`Option<struct>` 是唯一出路）
- **复现**：`tests/bugs/option_struct.myp`（编译失败）

```myp
struct Node {
    int val;
    Option<Node> next;   // 递归引用用 Option 包装
}
```

  错误：`cannot assign value of type 'Option_Node_inst' to variable of type
  'Option'`、`argument 1: expected 'int', got 'Node'`（`set(b)` 解析错）、
  `cannot access member of non-class type 'int'`（`get().val`）。

- **影响**：递归 AST/图结构不能用 `struct`（值类型无限递归），`Option<struct>`
  修好后才能把递归纯数据也迁到 struct。当前自举 AST 保留 class + getter 的
  折中方案即因本 bug 未修。
