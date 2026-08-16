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
| BUG-002 | 🟥 | @coro 主流程增量 spawn 卡死/帧损坏 | `tests/bugs/coro_incremental_spawn.myp` |
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

## BUG-002（未修复）：@coro 主流程增量 spawn 卡死 / 帧损坏

- **状态**：🟥 未修复
- **复现**：`tests/bugs/coro_incremental_spawn.myp`（移植自 go/test/sieve.go 并发素数筛）
- **现象**：主流程（非协程）边读边逐个 spawn @coro 过滤器，已 park 协程间歇性
  不再 resume / 帧损坏（类引用局部被改写）→ 复合数漏过筛网（got 15 漏过）。
  对照：全部预 spawn 无此问题。
- **验证**：修复后 `tests/bugs/coro_incremental_spawn.myp` 断言转绿。

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
