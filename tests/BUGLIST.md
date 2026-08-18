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
| BUG-004 | � | `Option<struct>` 泛型实例化失败 | `tests/bugs/option_struct.myp` |
| BUG-005 | 🟥 | mapping 事件 action 在事件源线程执行（非 action 实例线程） | （待建 `tests/bugs/mapping_thread.myp`） |
| BUG-006 | 🟥 | `main()` 直调检查被运算符/管道语法绕过（`A + B`、`A |> Op` 不报错） | （待建，修复后转负测试 `tests/negative/main_not_wiring.myp`） |
| BUG-007 | 🟥 | `bitvector<N>` 宽度未校验——`bitvector<3>` 静默映射为 i32（应报错） | （待建，修复后转负测试 `tests/negative/bitvector_width.myp`） |
| BUG-008 | 🟥 | 接口 action 参数类型/个数不校验——粗粒度签名匹配（名称+返回类型 basic_type） | （待建，修复后转负测试 `tests/negative/interface_param_mismatch.myp`） |
| BUG-009 | 🟥 | 一个类内多个 `@startup` 行为不一致——`@thread` 入口取最后一个、`mypc run` 合成 main 取第一个 | （待建：运行时行为差异，手动双命令复现） |
| BUG-010 | 🟥 | 类引用字段的链式属性访问 `ref_.a` codegen 类型错误——生成 ptr 而非属性类型，LLVM verify 失败 | （`tests/bugs/ref_field_chain.myp`，修复后转正测试） |
| BUG-011 | 🟥 | 函数内 mapping 用实例变量名节点（`s.e -> t.a`）→ LLVM verify 失败；须用类名节点（`S.e -> T.a`） | （`tests/bugs/instance_mapping_verify.myp`，修复后转正测试） |
| BUG-012 | 🟥 | 直接跨线程调用（`@thread` 实例普通 action）编译器不检查——design.md §8.2 声称「不允许，必须通过 mapping()」，实测编译通过 | 行为测试 `tests/@test/cross_thread_call.myp`（修复后转负测试 `tests/negative/cross_thread_call.myp`） |
| BUG-013 | 🟥 | `Coro.resume` 返回值串值——同程序混用 `await expr`（值挂起）与顶层 `@coro` 内 `Coro.yield` 时，resume 返回错误值 | `tests/bugs/coro_resume_value_mix.myp`（@test，断言失败红） |
| BUG-014 | 🟥 | `Atomic.loadInt`/`storeInt` 编译成**普通非原子** `load`/`store`——仅命名带 Atomic，实际无原子性/内存序保证（只有 add/sub/xchg/addDouble 走 atomicrmw） | （待建：编译+`--emit-llvm` 断言 IR 为 `load`/`store` 而非 `atomicrmw`） |
| BUG-015 | 🟥 | `mypc --package-path` **不支持冒号分隔多路径**——`myp build` 把本地 `myp_packages/` 与 `MYP_PACKAGE_PATH` 合并为冒号串传入，mypc 不切分 → `cannot find import`（自举 `myp_self` 支持切分） | （待建：shell 断言 `mypc --package-path "a:b"` 包在 b 时编译成功） |
| BUG-016 | 🟥 | `main(int argc, string[] argv)` 参数传给函数并在体内使用 → **编译器段错误**（ASAN: stack-overflow in `llvm::DataLayout::getAlignment`） | `tests/bugs/main_argc_argv_crash.myp`（编译崩溃红） |
| BUG-017 | 🟥 | 关联类型接口方法返回 **string** 经接口分派 → codegen 生成 `i32` 非 `ptr`，LLVM verify 失败（`Item=int` 正常，`Item=string` 崩） | `tests/bugs/assoc_string_dispatch.myp`（编译失败红） |
| BUG-018 | 🟥 | `import collections` + 带关联类型约束的泛型类（`where T : I` + `T::Item`）→ 8 个伪错误 `expected numeric type, got 'I'`（去 collections 即过；`import option` 不触发） | `tests/bugs/assoc_constraint_import.myp`（编译失败红） |
| BUG-019 | 🟩 | `this.field = value` 写被拒——struct/class 的 `this.field` 分支误嵌套在 `if (!op)` 内，`this.x = v`（op 非空）整块跳过 → `not a valid assignment target` | 回归 `tests/@test/manual_ch7_struct.myp` t_this |
| BUG-020 | 🟩 | 文件级限定 struct 定义 `struct A::B { }` 被拒——顶层 dispatch `current_--` 回退使 parseStruct 限定分支看 `struct` 关键字而非名称 → `expected struct name`（自举支持） | 回归 `tests/@test/manual_ch7_struct.myp` t_nested_qualified |
| BUG-021 | 🟥 | class 含**泛型类属性**（`Option<int>`/`ArrayList<int>` 等）时，方法内 `this.prop` 在 sema 被解析为泛型实例类 → `class 'X_inst' has no member 'v'`（读+写都中；struct 泛型字段正常） | `tests/bugs/this_generic_prop.myp`（编译失败红） |
| BUG-022 | 🟥 | `@thread` 用于 **struct 实例**被静默接受（`S s @thread;` 编译+运行通过但无效果）——应拒绝却接受（与 BUG-006/007/008/012 同类） | （待建：修复后转负测试 `tests/negative/struct_thread.myp`） |
| BUG-023 | 🟥 | `@parallel for` / `@gpu for` 并行体**直接访问 class/static 属性数组** → LLVM verify 失败（`getelementptr i32, i64 0` GEP 基址为 0 非指针）/ `Atomic.addInt` 时运行段错误 139；须先拷到局部（manual BNCT 模式正确） | `tests/bugs/parallel_prop_access.myp`（编译失败红） |
| BUG-024 | 🟥 | 相对路径导入去重**不解析 `..`**——同一文件经不同相对路径（直导 `./helper.myp` + 子模块内 `../helper.myp`）规范化后仍不同 → 双重载入 → `duplicate class name`/`duplicate function name`（design §9 声称"规范化路径去重"未实现） | `tests/bugs/relimport_dedup.myp`（编译失败红） |
| BUG-025 | 🟩 | 多文件编译 `mypc a.myp b.myp` **只合并第一个文件的 imports**——合并循环漏了 imports/structs/bitfields/enums/ffis/macros/type_aliases（只合并 classes/interfaces/mappings/functions）→ 第二个文件的 `import env` 静默丢弃 → `Console` 未定义 | 回归 `tests/test_multifile.sh` |
| BUG-026 | 🟩 | `mypc --test` + 源码含用户 `int main()` → 用户 main 空块**无 terminator**（`LLVM verify failed: Basic Block in function 'main' does not have terminator!`），且残留占位使测试运行器 main 被改名为 `main.1` → 测试**静默不跑**（exit 0 全假过） | 回归 `tests/test_multifile.sh`（BUG-026 用例） |
| BUG-027 | � | `tools/codegen` 代码生成工具**未迁移到 BUG-001 属性私有规则**——模型类（`Expr`/`Field`/`TypeDecl`/`ServiceDecl`/`DslOp`/`Resource` 等 15 类）的 `property:` 被生成器跨类读取 → 301 个编译错误（`cannot access property ... from outside the class`），框架（serde/ffi/autodiff/idl/orm/embed/dsl/infer_ops）整体不可用 | 回归 `tools/codegen/run_tests.sh`（已接入 `tests/run_tests.sh`，全绿） |

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

## BUG-004（已修复）：`Option<struct>` 泛型实例化失败

- **状态**：🟩 已修复（2026-08-16）
- **根因**：`visitTranslationUnit` 中 `generic_classes_` 在 struct 字段校验
  （`visitStructDecl`）**之后**才注册。struct 字段类型 `Option<Node>` 解析时
  `generic_classes_.count("Option")` 为假 → 落回“未实例化模板名 `Option`”，
  后续 `a.next = new Option<Node>()` 类型比对 `Option_Node_inst` vs `Option` 失败，
  `set`/`get` 也按模板占位 T=int 解析。
- **修复**：
  1. `src/sema/sema.cpp`：把 generic class/function 模板注册**提前**到 struct
     字段校验之前；同时类声明循环跳过 `is_generic_inst`（实例在单态化时已
     `visitClassDecl`，二次访问报 duplicate class）。
  2. `src/codegen/codegen_expr.cpp`：`memberObjectClassName` 的 MemberAccess 分支
     兜底用 sema 记的 `resolved_object_class`（struct 名）+ struct 字段类型查找，
     `a.next.get()` 才能分发到 `Option_Node_inst_get` 而非模板 `Option_get`。
- **验证**：`tests/bugs/option_struct.myp` 编译通过、2/2 断言转绿；全量回归 274 通过。

---

## BUG-005（未修复）：mapping 事件 action 在事件源线程执行（非 action 实例线程）

- **状态**：🟥 未修复（2026-08-17 记录）
- **复现测试**：待建 `tests/bugs/mapping_thread.myp`（需能打印/比较当前线程 id 的
  手段；当前无线程 id 内建，需先加诊断或按行为断言）
- **现象**：`@thread` 实例 A（event）在线程 1、实例 B（action）在线程 2，
  `mapping() { a.event -> b.action; }` 触发后，`b.action` 在**线程 1** 上执行，
  而非 B 自己的线程 2。
- **根因**（`src/runtime/runtime.c`）：
  - `myp_event_fire`（~3301）用 `myp_thread_for_instance(sender)` 取**事件源
    （sender）的线程**，把事件投到 sender 的线程队列；
  - `myp_event_dispatch`（~3330）在当前队列处理线程上**直接调用 handler**
    （`myp_handlers[i].handler(instance, data)`），不切换/不重投到 handler 实例
    的线程。
  - 结论：action 跑在**事件源线程**，action 实例自己的 `@thread` 归属被忽略。
- **影响**：违反 manual.md「@thread 组件独立运行、无共享内存竞争」的隔离假设；
  B 的状态被 A 的线程（线程 1）读写，存在无锁并发/竞争风险。
- **期望语义**：`myp_event_fire`/dispatch 应按 **handler 实例**
  （`myp_handlers[i].instance`）的线程归属投递，把事件投到 B 所在线程 2 的队列，
  由 B 的线程执行 action。
- **备注**：与 `docs/next_improvements.md` §九-9 同一 bug（跨文件登记，修复时同步
  两处）。

---

## BUG-006（未修复）：`main()` 直调检查可被运算符/管道语法绕过

- **状态**：🟥 未修复（2026-08-18 记录；同日实测校正）
- **复现测试**：待建（修复后转负测试 `tests/negative/main_not_wiring.myp`）——
  当前无合适自动复现：`tests/bugs/` 框架对"应拒绝却接受"的诊断缺失类 bug 无法表达
  为红（修复前该文件编译绿，修复后应报错）。
- **已有校验**（`src/sema/sema.cpp` visitExprStmt ~1195）：main 内**顶层 ExprStmt 的
  CallExpr** 会被拒绝——直接方法调用 `sensor.readValue()` 报
  `direct function call not allowed in main() — use mapping() instead`；事件式调用
  `foo()` 同；但 **struct 方法调用被有意放行**（`obj_type.kind == Struct`）。
  外部 class property 读写另由 BUG-001 规则拒绝（`cannot assign to property...`）。
- **现象（实测 2026-08-18）**：同样的"逻辑"换用**运算符/管道语法**即可绕过——
  struct 算子 `v + u`、外部 `@op` 数组运算 `A + B`、管道 `A |> ScaleOp`、
  以及 `int x = v + u;` 这类带计算初始化的声明，均编译通过零告警
  （而等价的直接调用 `vecAdd(A,B)` / `scale.transform(A)` 会被拒）。
- **根因**：main 校验只覆盖 `ExprStmt && expression->kind == Call`；
  `BinaryOpExpr`（含 `@op` 解析）、`PipeExpr`、声明初始化表达式、循环体内的
  调用等路径不经过该检查 → 运算符/管道语法成为直调检查的旁路。
- **影响**：manual.md/design.md §4.4 声明"main 只做接线"，但 main 内计算逻辑
  仍可写成运算符/管道/声明初始化混入——规则可被语法糖绕过，前后不一致。
- **期望语义**：main 校验扩展到接线语义——允许 创建实例（声明/`new`）、
  `mapping()`、`return`；对解析为 `@op` 外部函数/class 方法调用的**表达式**
  （运算符、管道、带计算初始化的声明、循环体）同样报
  `direct ... not allowed in main()`；struct 方法调用保留放行。
- **备注**：manual.md 中 `sensor.readValue();`/`sensor.propertyName = 42;` 两个示例
  目前**已被正确拒绝**（分别由直调检查/BUG-001 规则），文档无需改；缺的是
  运算符/管道旁路这一条未覆盖路径，修复时补负测试。

---

## BUG-007（未修复）：`bitvector<N>` 宽度未校验，非 8/16/32/64 静默映射为 i32

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：待建（修复后转负测试 `tests/negative/bitvector_width.myp`）
- **现象**（实测）：`bitvector<3> bv;` 编译通过（exit 0），生成 `alloca i32, align 4`——
  codegen `getLLVMType(BitVector)` 只 switch 8/16/32/64，其余宽度落 `default: i32`；
  文档/注释声明 `bitvector<N>` 仅 8/16/32/64（AST.h/Type.h），但 sema 未校验宽度。
- **根因**：sema 对 `bitvector<N>` 的 N 无合法性检查；codegen default 分支静默用 i32。
- **影响**：`bitvector<3>` 等非法宽度静默变成 32 位，位语义错误（应为编译错误）；
  `bitvector<3>` 与 `bitvector<8>` 无法区分（同为 i32）。
- **期望语义**：sema 校验 N ∈ {8,16,32,64}，否则编译错误
  `bitvector width must be 8/16/32/64`；codegen default 分支改为 unreachable。
- **备注**：`bit`/`bool`（均映射 LLVM `i1`）内存实际占 1 字节（i1 存储粒度 1B，不按位
  打包）；亚字节按位压缩走 `bitfield`；design.md §5.1 已同步修正（bit/bool 大小
  标注为 1 字节而非 1 bit）。

---

## BUG-008（未修复）：接口 action 参数类型/个数不校验（粗粒度签名匹配）

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：待建（修复后转负测试 `tests/negative/interface_param_mismatch.myp`）——
  与 BUG-006/007 同型：「应拒绝却接受」类 bug，现有 `tests/bugs/` @test 框架无法
  表达为红（修复前该文件编译绿，修复后应报错）。手动复现：
  ```myp
  interface I { double area(int a, int b); }
  class C { interface class I; action: double area(int a){ return 1.0; } }
  int main(){ return 0; }
  ```
  `mypc` 编译通过（exit 0），参数个数不一致未被拒绝。
- **现象**（实测 2026-08-18）：接口 action 签名匹配只按 **名称 + 返回类型 basic_type**
  核对（`sema checkInterfaceImpl` 的 `matches` lambda：`ca.return_type.basic_type ==
  ia.return_type.basic_type`），**不校验参数类型/个数**——接口 `double area(int a,
  int b)` 对实现 `double area(int a)` 也能通过编译（`int area()` vs `double area()`
  因返回类型 basic_type 不同会被拒，见 BUG-008 对比）。事件仅按名称匹配（无参数校验）。
- **根因**（`src/sema/sema.cpp checkInterfaceImpl` ~374）：`matches` 条件
  `iface_uses_assoc ? true : ca.return_type.basic_type == ia.return_type.basic_type`；
  参数列表 `ia.params`/`ca.params` 未参与比较。自举 `tools/selfhost/src/sema.myp`
  ~1558 同逻辑（需同步修复）。
- **影响**：实现类可声明与接口签名不一致（参数不同）的 action 却通过 `interface
  class` 检查，编译期不报错；虚表调用按实现方法签名执行，接口调用点
  （interface 变量 `op.area(a, b)`）与实现签名不符时存在类型/参数错位风险。
- **期望语义**：action 匹配升级为**精确签名**——名称 + 参数类型 + 返回类型全部一致
  （参数默认值/关联类型场景需与 §三-5 关联类型绑定配合）；事件按名称 + 参数类型
  匹配；不一致 → 编译错误 `does not implement action 'a' from interface 'I'
  (signature mismatch)`。关联类型参与时保留现有仅名称匹配逻辑。
- **备注**：design.md §6.3 「接口与实现不一致」小节已如实记录该粗粒度行为并标注 ⚠️
  「当前已知限制」；修复后同步更新该处说明。

---

## BUG-009（未修复）：一个类内多个 `@startup` 行为不一致

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：待建（运行时行为差异，非编译错误——`tests/bugs/` @test 框架难以断言
  @thread 时序输出，先用手动双命令复现）。
- **现象**（实测 2026-08-18）：同一类声明两个 `@startup` 方法时，两种运行入口行为
  **不一致**：
  - 手写 `main()` + `@thread` 实例：启动后输出 **SECOND**（`@thread` 线程入口取
    **最后一个** `@startup`）；
  - `mypc run`（无 `main`，注入合成 main）：输出 **FIRST**（合成 main 调用**第一个**
    `@startup`）。
- **根因**：
  - `src/codegen/codegen_stmt.cpp` ~215：`@thread` 实例启动时遍历类 actions 找
    `has_startup`，循环**无 break**，每个 `@startup` 都覆盖 `startup_fn` → 取**最后一个**
    为线程入口。
  - `src/sema/sema.cpp injectAutoMainIfNeeded` ~551：合成 main 收集 @startup 类时
    内层 **break**，`startup_name` 取该类**第一个** `@startup`。
- **影响**：一个类多个 `@startup` 时语义不确定——同一代码在 `@thread` 与 `mypc run`
  两种入口下执行不同的启动方法；无编译期诊断，用户不易察觉。
- **期望语义**：一个类最多声明一个 `@startup`（sema 诊断报错），或两条路径统一（都取
  第一个/都取最后一个并文档化）。
- **备注**：design.md §6.5 `@startup` 小节已如实记录该不一致并建议「一个类只声明一个
  `@startup`」；修复后同步更新该处说明。

---

## BUG-010（未修复）：类引用字段的链式属性访问 `ref_.a` codegen 类型错误

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：待建 `tests/bugs/ref_field_chain.myp`（编译应通过 + `getRefA()==7` 断言；
  修复前 LLVM verify 失败编译都过不了，修复后转正测试）。
- **现象**（实测 2026-08-18）：同类 action 内对**类引用字段**做链式属性访问 `ref_.a`
  （`property: V ref_;` + `int getRefA(){ return ref_.a; }`）→
  `LLVM verify failed: Function return type does not match operand type of return inst!`
  （IR `ret ptr %3`——`ref_.a` 被生成为 `ptr` 而非 int 属性）。不 return 时（仅
  `Console.writeLine("ref.a=" + int(ref_.a))`）运行得到**空/垃圾值**而非 7。
  触发与实参来源无关：构造器参数存字段（`ref_ = other`）、普通 action `setRef(v1)`
  存字段后读 `ref_.a` 均复现；直接 `other.a`（参数/局部类引用）读取**正常**（`other.a=7`）。
- **根因**：codegen 成员访问对「类引用字段 → 其属性」路径解析错误——`ref_.a` 的
  类型被当成 `ptr`（实例指针）而非属性 `int`，GEP/load 索引取错 → 返回类型不匹配。
  与 BUG-001（外部链式访问，sema 已拒）相关但不同：这是**同类内**、经**类引用字段**
  的链式访问（sema 放行，codegen 错）。
- **影响**：把 class 实例存为字段后读取其属性的任何代码受影响（构造器拷贝存储、
  `setRef` 模式等）；普通直接读参数/局部实例正常。
- **期望语义**：`ref_.a` 解析为 `ref_` 指向实例的 int 属性（load 字段指针 → 正确 GEP +
  load），类型为属性类型而非 ptr。
- **备注**：与「右值传构造器」调查同源——`new V(makeV())` 传函数返回临时右值本身
  **工作正常**（构造器内 `other.a` 正确），仅「存字段后再读」触发本 bug。

---

## BUG-011（未修复）：函数内 mapping 用实例变量名节点 → LLVM verify 失败

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：待建 `tests/bugs/instance_mapping_verify.myp`（编译应通过 + 运行断言；
  修复前 verify 失败编译都过不了）。
- **现象**（实测 2026-08-18）：`main()`/`@constructor` 内实例级 mapping 若用**实例变量名**
  作节点——`mapping(){ s.e -> t.a; }`（`S s; T t;`）→
  `LLVM verify failed: Referring to an instruction in another function!`；
  换成**类名节点** `mapping(){ S.e -> T.a; }`（同一函数内）→ 编译通过。文件级
  `mapping(){ S.e -> T.a; }` 也正常。
- **根因**：实例级 mapping 的 codegen 把**函数内局部实例指针**的引用塞进全局注册
  （init/handler 表）代码，形成跨函数指令引用 → verify 失败。
- **影响**：文档 §7.2「实例级映射（函数内局部）」、§7.4.1 @scope、§7.5 完整语法块
  的示例全部用实例变量名（`s.valueRead -> d.showTemperature`），**实际无法编译**；
  现有真实代码（tests/ 的 lambda/mapping_chain/multi_event/scope_mapping/where_mapping）
  全部用类名节点规避；旧测试 `tests/test_cycle.myp`（实例名）同样 verify 失败。
- **期望语义**：实例级 mapping 正确捕获函数内局部实例（注册到实例级 handler），
  或编译期诊断「mapping 节点须用类名」并说明实例级映射当前不支持。
- **备注**：design.md §7.2/§7.4.1/§7.5 已改为类名节点并标注本限制（2026-08-18）。

---

## BUG-012（未修复）：直接跨线程调用（`@thread` 实例普通 action）编译器不检查

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：行为测试 `tests/@test/cross_thread_call.myp`（`W w = new W() @thread; w.work();`
  当前编译+运行通过，`@test` 1/1 断言；修复后本测试转红，改转负测试
  `tests/negative/cross_thread_call.myp`——「应拒绝却接受」型，与 BUG-006/007/008 同类）。
- **现象**（实测 2026-08-18）：对 `@thread` 实例直接调用**普通 action**（非 @startup）→
  编译通过（exit 0），action 在**调用线程**执行（不经 mapping、不切到 @thread 目标线程）；
  仅 `@startup` 手动调用被拒（sema_expr ~2129，专门规则）。
- **根因**：sema/codegen 对「调用 @thread 实例的 action」无跨线程检查；§8.2 的
  「直接跨线程调用 ❌ 不允许」只是设计语义/程序员自律（§8.4 已注明「编译器检查
  （未来版本）或程序员自律（当前版本）」）。
- **影响**：design.md §8.2 表格「直接跨线程调用 ❌ 不允许，必须通过 mapping()」与实际
  不符——直接调用编译通过；与 BUG-005（mapping handler 在事件源线程执行）同源，
  线程归属语义整体未在编译期落实。
- **期望语义**：sema 对 `@thread` 实例的跨线程直接调用报错（提示 `use mapping() instead`），
  或明确降级为「程序员自律」并在文档如实标注（当前 §8.2 表述过强）。
- **备注**：design.md 不标本 bug（用户要求文档不标记 bug）；行为测试
  `tests/@test/cross_thread_call.myp` 已记录现状。

---

## BUG-013（未修复）：`Coro.resume` 返回值串值（混用 `await expr` + `Coro.yield`）

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：`tests/bugs/coro_resume_value_mix.myp`（@test：`echo` 协程 `int v = await n*2;`
  + 顶层 `@coro long topLevel(long n) { long x = Coro.yield(n*2); return x+100; }`；断言
  `Coro.resume(echo_h, 100) == 10`。`run_bugs.sh` 实测：0 passed / 1 failed，
  `ASSERTION FAILED: echo resume 返回值应 = n*2 = 10`）。
- **现象**（实测 2026-08-18，/tmp 探针）：
  - **正确基线**：`tests/coro`（仅 `await expr`）→ `resume(echo_h, 100)` 返回 10 ✓；
    `tests/coro_top`（仅顶层 `Coro.yield`）→ `resume(top_h, 7)` 返回 42 ✓。
  - **混用 A**（echo + topLevel + run 仅 `await;`）：`resume(echo_h, 100)` 应返回
    echo 挂起时传出的 10，实测返回 **42**（= topLevel 的 `Coro.yield(42)` 值）；
    `resume(top_h, 7)` 返回 42（正确）。
  - **混用 B**（再加 run() 内 `await Async.sleep(20)` 定时器挂起）：
    `resume(echo_h, 100)` 返回 **0**（应 10）；`Coro.result(top_h)`=107 仍正确。
  - **不变量**：协程**内部**值传递正确（`v=100`、`r=0`、`result=107` 均对）——只有
    `Coro.resume` 的**返回值**（上次挂起 yield 的值）串；不同混用组合返回值
    不同（42 vs 0），说明存在被覆盖的共享值槽。
- **根因（推测）**：协程的「上次 yield 值」槽未按协程隔离——后续 spawn（顶层
  `Coro.yield`）或定时器挂起（`Async.sleep`）写入覆盖了该共享槽，`resume` 读的是
  全局/最后写入值而非目标协程自己的值。需查 runtime.c 协程 yield/返回值存取路径
  （`__myp_coro_*` 的 yield 值存哪里、resume 返回值从哪里取）。
- **影响**：`await expr`（值挂起）与 `Coro.yield` 混用、或与定时器混用时，
  `Coro.resume(h, val)` 的返回值不可靠；协程内部 `int v = await expr;` 不受影响。
  design.md §8.6.1 示例已规避（不打印 resume 返回值，只打印协程内 `v=100`/`result=42`）。
- **期望语义**：`Coro.resume(h, val)` 返回**该协程**上次挂起时传出的值，与其他协程
  的 spawn/yield、定时器挂起互不影响。
- **备注**：design.md 不标本 bug（用户要求文档不标记 bug）；文档示例已用规避写法。
  顺带修复 `tests/bugs/run_bugs.sh` 分类：@test 断言失败进程退出码非 0，原逻辑误判为
  RUNTIME CRASH——现改为先捕获输出+退出码，再按输出（`ASSERTION FAILED`/汇总行
  `passed, N failed`）分类为 RED (assertion)。

---

## BUG-014（未修复）：`Atomic.loadInt`/`storeInt` 编译成普通非原子 `load`/`store`

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：待建（建议：编译含 `Atomic.loadInt(arr,i)`/`Atomic.storeInt(arr,i,v)`
  的代码 → `mypc --emit-llvm`，断言 IR 是普通 `load`/`store` 而非 `atomicrmw`/原子
  load——与 addInt/subInt/xchgInt/addDouble 的 `atomicrmw` 对照；修复后本断言应变为
  「IR 是原子 load/store」）。
- **现象**（实测 2026-08-18，`src/codegen/codegen_expr.cpp` atomic intrinsic 分支）：
  - `__myp_atomic_add_i32`/`sub_i32`/`xchg_i32`/`add_f64` → `CreateAtomicRMW`
    （`atomicrmw` add/sub/xchg/fadd，SequentiallyConsistent）✓ 真原子。
  - `__myp_atomic_load_i32` → **`CreateLoad`（普通非原子 load）**；
    `__myp_atomic_store_i32` → **`CreateStore`（普通非原子 store）**——两者
    均无 AtomicOrdering、无 volatile。`stdlib/atomic.myp` 的
    `Atomic.loadInt`/`Atomic.storeInt` 直接委托这两个 intrinsic。
- **根因**：codegen 对 load/store 两个 atomic intrinsic 走普通内存访问路径，未生成
  原子 load/store 指令（LLVM `load atomic`/`store atomic` 或 `atomicrmw`），命名
  与实现不符。
- **影响**：跨线程共享数组用 `Atomic.loadInt`/`storeInt` 读写在 LLVM 语义下
  **不具原子性/内存序保证**（可被重排/缓存合并；x86-64 对齐 i32 实际原子只是
  CPU 特性，非语言保证）；`@parallel for` 只读共享数组若依赖「原子」语义则不成立；
  命名有误导性（design.md §8.9 原写 `atomicrmw add 0`/`atomicrmw xchg`，实测非原子，
  已修正文档）。
- **期望语义**：`Atomic.loadInt`/`storeInt` 应编译为原子 `load`/`store`（seq_cst 或
  acquire/release），与 add/sub/xchg/addDouble 一致；或至少在文档/API 明示非原子。
- **备注**：design.md §8.9 已如实标注（普通 load/store、非 atomicrmw）；design.md 不标
  bug（用户要求文档不标记 bug）。

## BUG-015（未修复）：`mypc --package-path` 不支持冒号分隔多路径

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：待建（建议 shell：包放 `dirB/foo/src/foo.myp`，`mypc main.myp
  --package-path "dirA:dirB"` 按设计意图应**编译成功**，实测报 `cannot find import
  'foo'`；修复后断言成功）。
- **现象**（实测 2026-08-18，`src/main.cpp` `loadModule`）：
  - `mypc --package-path "dirA:dirB"`（包在 dirB）→ `cannot find import 'foo'`
    ——**不按冒号切分**，把整串当单一目录。
  - 单路径 `--package-path "dirB"` → 编译通过 ✓。
  - `myp build`（`tools/pm/build.myp` `resolvePackagePath()`）把本地 `myp_packages/`
    与 `MYP_PACKAGE_PATH`（冒号分隔）**合并为冒号串**传给 `--package-path`：
    - 仅设 `MYP_PACKAGE_PATH`（无本地 `myp_packages/`）→ 编译通过 ✓；
    - **本地 `myp_packages/` 存在 + `MYP_PACKAGE_PATH` 同时设置** → 传
      `--package-path "myp_packages:<path>"` → mypc 不切分 → `cannot find import`
      （实测失败）。
  - 自举编译器 `myp_self`（`tools/selfhost/src/main.myp` `loadModule`）**支持**冒号
    切分（`Str.splitCount(packagePath, ":")` 逐路径查找）——设计意图如此，仅 C++
    mypc 未实现。
- **根因**：`src/main.cpp` `loadModule` 对 `package_path` 直接拼接
  `<package_path>/<module>/src/<module>.myp`，未按 `:` 切分多路径。
- **影响**：`docs/manual.md` 文档化用法 `export MYP_PACKAGE_PATH=/a:/b` 端到端失效
  （多路径，或与本地 `myp_packages/` 并存时）；design.md §9 声称「支持冒号分隔多路径」
  对 mypc 不成立（对 `myp_self` 成立）。
- **期望语义**：`mypc --package-path` 与 `myp_self` 一致，按 `:` 切分逐路径查找
  （`<pkg_i>/<module>/src/<module>.myp` 或 `<pkg_i>/<module>/<module>.myp`）。
- **备注**：design.md §9 按设计意图描述（不标 bug，符合约定）；bug 仅记录于本清单。

---

## BUG-016（未修复）：`main(int argc, string[] argv)` 参数传给函数并使用 → 编译器段错误

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：`tests/bugs/main_argc_argv_crash.myp`（编译即崩；`run_bugs.sh` 显示
  COMPILE CRASH）
- **现象**（实测 2026-08-18）：
  - `int main(int argc, string[] argv)` 将 `argc`/`argv` 传给顶层函数并在函数体内
    使用（如 `Console.write(argc)`、`argv[1]`）→ mypc 在 **codegen 阶段段错误**
    （exit 139）。
  - ASAN 构建：`AddressSanitizer: stack-overflow ... in llvm::DataLayout::getAlignment`
    ——`string[] argv` 传参时 LLVM 类型布局计算**无限递归**。
  - 简单用法不崩：`int main(int argc, string[] argv) { return argc; }`、空体、
    或 main 不把参数传下去。`mypc run file.myp args...` 同崩（编译阶段即段错误）。
  - `--test` 模式：**BUG-026 修复后（2026-08-18）已不再报错**——test 模式跳过并擦除
    用户 main，argv 传参 codegen 路径不执行 → 编译+运行通过；但**普通编译路径仍段
    错误（exit 139 确定性复现，2026-08-18 复核）**。⚠️ `run_bugs.sh` 用 `--test`
    编译 → `main_argc_argv_crash.myp` 现显示 GREEN，**非 test 编译仍崩**，该复现
    不再能检出 BUG-016（如需检出须改非 --test 编译断言）。
- **根因**：codegen 对 `string[]`（动态数组）作为参数从 `main` 传入函数时的类型
  布局/对齐计算触发 LLVM `DataLayout::getAlignment` 无限递归。
- **影响**：`docs/manual.md` §5 `main(argc, argv)`（配合 `import args`）文档化的实际
  用法（把 argc/argv 传给顶层函数）会崩编译器；目前只能改用 `import args` 的
  `Args.*`（内部读全局 argv）规避。
- **期望语义**：`main(argc, argv)` 参数可正常传给任意函数使用；修复后
  `tests/bugs/main_argc_argv_crash.myp` 编译通过并打印 argc 与 argv[1]。
- **备注**：manual.md §5 按设计意图描述（不标 bug，符合约定）；bug 仅记录于本清单。

---

## BUG-017（未修复）：关联类型接口方法返回 string 经接口分派 → codegen 类型错误

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：`tests/bugs/assoc_string_dispatch.myp`（`Container sb = new StrBox();
  Test.assertStrEq(sb.getVal(), "hi");` —— 编译阶段 LLVM verify 失败红）
- **现象**（实测 2026-08-18）：
  - `interface Container { type Item; Item getVal(); }` + `class StrBox { interface
    class Container; type Item = string; string getVal() {...} }`，经接口变量
    `Container sb = new StrBox(); sb.getVal()` → codegen 报
    `LLVM verify failed: Call parameter type does not match function signature!`
    `%6 = call i32 %iface_fn(ptr %4)`——**分派 stub 返回类型生成为 `i32` 而非
    string `ptr`**（interface fat-pointer 虚表调用返回类型错）。
  - **对照**：`Item = int`（IntBox）经接口分派 `c.getVal()` **正常**（int i32 匹配）；
    仅 `Item = string` 触发。
  - **范围（2026-08-18 复核）**：仅「接口变量（胖指针）直接分派返回 string 的关联
    类型方法」受影响——单方法接口 `Container{ Item getVal(); }` → 编译 LLVM verify
    失败；接口含其他方法（如 `bool contains(Item)`）时编译过但**运行时段错误**
    （exit 139）。**`Processor<T where T:Container>` 泛型单态化路径正常**
    （`T::Item` 静态解析到具体类直接调用，不走 vtable）——`Processor<StrBox> ps;
    string sv = ps.peek(sb);` 实测返回 "hi" ✓。
- **根因**：codegen 生成接口虚表分派 stub 时，对**关联类型绑定为 string** 的方法
  返回类型未按绑定类型解析为 string（ptr），沿用默认标量类型（i32）。
- **影响**：manual.md §6 关联类型示例中 `StrBox`（`Item = string`）经接口变量
  `sb.getVal()` **无法编译**；int/数值关联类型正常。关联类型 string 成员的实际
  用法（接口变量上取 string 值）不可用。
- **期望语义**：`Item = string` 的接口方法经虚表分派返回 string（ptr）；修复后
  `tests/bugs/assoc_string_dispatch.myp` 编译通过、getVal() 返回 "hi"。
- **备注**：manual.md §6 关联类型按设计意图描述（不标 bug，符合约定）；bug 仅记录于
  本清单。

---

## BUG-018（未修复）：`import collections` + 带关联类型约束的泛型类 → 伪错误

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：`tests/bugs/assoc_constraint_import.myp`（`import collections` +
  `class Processor<T where T : Container>` + `T::Item peek(T c){ return c.getVal(); }`
  + `Processor<IntBox>` 实例化 —— 编译失败红）
- **现象**（实测 2026-08-18）：
  - 文件含 `import collections` 时，任何「带关联类型约束的泛型类」
    （`where T : <接口>` + 方法内引用 `T::Item`）→ sema 报 **8 个**
    `expected numeric type, got '<接口名>'`（如 'Container'/'Bag'）。错误行号落在
    合并模块内 collections.myp 区域（固定 205/223/235/246/285/482/498/499），
    与用户文件内容无关。
  - **对照**：去掉 `import collections` → 编译通过（exit 0）；`import option` 不触发；
    仅接口声明 / 仅实现类 / 仅 `T::Item` 单独用都不触发——必须「collections +
    where 关联类型约束泛型类」组合。
- **根因（推测）**：sema 在 collections.myp 大量泛型类（Set/Deque/Queue 等，均用
  `T` 作类型参数）与用户「带关联类型约束的泛型类」共存时，`T::Item` 的关联类型
  解析状态被污染——`T::Item` 回落到接口类型本身（而非绑定类型），导致
  collections 泛型类语义分析时把接口类型当数值用报错。
- **影响**：manual.md §6 关联类型「泛型约束」示例
  `class Processor<T where T : Container> { T::Item peek(T c){...} }` 一旦与常见
  `import collections` 并存（如同时用 ArrayList）即无法编译——**文档化模式实际
  受限于 import 顺序**。
- **期望语义**：`where T : <关联类型接口>` 的泛型类可与任意 stdlib 导入共存；
  `T::Item` 始终单态化为绑定类型。
- **备注**：manual.md §6 关联类型按设计意图描述（不标 bug，符合约定）；bug 仅记录于
  本清单。回归测试 `tests/@test/manual_ch6_class.myp` 因此拆分：关联类型 Processor
  用例与 collections 分开放。

---

## BUG-019（已修复）：`this.field = value` 写被拒——struct/class 分支误嵌套在 `if (!op)` 内

- **状态**：🟩 已修复（2026-08-18 审计 §7 发现并修复）
- **复现测试**：修复前 `this.x = v`（struct/class 方法）→ 编译错误
  `not a valid assignment target`。修复后回归 `tests/@test/manual_ch7_struct.myp`
  t_this（`this.x = v` 写 + `this.x` 读均过）；`tests/test_smart_building.myp`
  （大量 `this.count = s`）转绿。
- **现象**（实测 2026-08-18）：struct 方法 `void set(double v){ this.x = v; }` 与
  class 方法 `void init(int s){ this.count = s; }` 都报 `not a valid assignment
  target`；`this.x` **读**正常；无 `this` 的裸写 `x = v` 正常；`v.x = 3.0`（外部
  struct 字段写）、`o.inner.a = 7`（链式）、`arr[i].field = v`（数组元素）均正常。
  自举编译器 `myp_self` 支持 `this.x = v` → C++ 专属 bug。
- **根因**（`src/codegen/codegen_stmt.cpp` generateAssignment）：`if (!op)` 块（~1988）
  本应只含「对象非 `this`」的分支（Identifier struct 字段路径 / static / 外部拒绝），
  但**闭合花括号错位**——`if (!op)` 一直延伸到 ~2271，把「struct 方法 this.field 分支」
  与「class this.prop 循环」（都用**非空** `op` 作 GEP 基址）也吞进块内。于是
  `this.x = v`（`op` = this 指针**非空**）→ `if (!op)` 为假 → 整块跳过 → struct/class
  分支永不执行 → 落到 `not a valid assignment target`；而 `op` 为空的链式/数组元素/
  外部拒绝路径反而被块内嵌套逻辑覆盖。
- **修复**：把 `if (!op)` 的闭合移到链式 + 数组元素分支之后，将「struct 方法
  this.field」与「class this.prop」两个分支移到 `if (!op)` **之外**（`this` 非空
  时执行）。括号平衡用逐字符计数验证（忽略字符串/注释）。
- **验证**：`manual_ch7_struct.myp` 7 tests / 12 断言；`test_smart_building.myp`
  编译通过；全量回归 281 通过（仅 myp_fmt/myp_viz/myp_lsp 3 个自举工具因
  build/ 缺二进制 exit 127 的既有环境失败，与本修复无关）。
- **遗留**：调试中发现「class 含泛型类属性时 `this.prop` sema 解析污染」→ 另立
  BUG-021。

---

## BUG-020（已修复）：文件级限定 struct 定义 `struct A::B { }` 被 parser 拒绝

- **状态**：🟩 已修复（2026-08-18 审计 §7 发现并修复）
- **复现测试**：修复前 `struct Device::Mode { int code; }`（文件级，类内无声明）→
  `expected struct name` / `expected '{' after struct name` / `expected '}'`。
  修复后回归 `tests/@test/manual_ch7_struct.myp` t_nested_qualified（`Device::Mode`
  编译+运行，2 断言）。
- **现象**（实测 2026-08-18）：manual.md §7「外部定义（类外部展开）」
  `struct Sensor::Config { ... }` 无法编译；EBNF（design.md §13）声明该形态合法；
  自举 parser 支持 → C++ 专属 bug。类内嵌套 `struct Config` + 外部 `Sensor::Config`
  **类型引用**（nest1）正常。
- **根因**（`src/parser/parser.cpp` parseProgram 顶层 struct 分发 ~31-43）：
  顶层 dispatch 在 `match(Keyword_struct)` 后 `current_--` **回退到 `struct` 关键字**，
  再调 `parseStruct()`——而 `parseStruct` 内部限定检查是
  `check(Identifier) && tokens_[current_+1]==DoubleColon`，回退后 `check` 看到的是
  `struct` 关键字而非名称 → 限定分支永不命中 → 走 `parseIdentifier("expected
  struct name")` 在 `struct` 处报错。`current_--` 与 `parseStruct` 内部限定检查
  同源于初始提交，两者叠加自始就使顶层限定定义不可达。
- **修复**：删除顶层 `current_--` 回退与重复的限定检查分支，直接 `parseStruct()`
  （`match` 已消费 `struct`，`current_` 指向名称，`parseStruct` 内部限定检查生效）。
  与自举 parser（无回退、直接 `parseStruct()`）一致。
- **验证**：`struct Device::Mode` / `struct Sensor::Config`（仅限定定义）编译+运行；
  全量回归 281 通过（3 个既有环境失败同上）。
- **备注**：类内 `struct Config` + 文件级 `struct S::C` 并存为**重复定义**（misuse），
  当前 sema 给出 BUG-001 式属性私有错误（`c.v = 1` → "cannot assign to property"），
  语义欠清晰但非主路径，未单独登记。

---

## BUG-021（未修复）：class 含泛型类属性时方法内 `this.prop` sema 解析污染

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：`tests/bugs/this_generic_prop.myp`（`import option` + `class H {
  int v; Option<int> o; void setV(int s){ this.v = s; } }` → 编译失败红）
- **现象**（实测 2026-08-18）：class 的 `property:` 含**泛型类实例**字段
  （`Option<int>` / `ArrayList<int>` 等，`import option` / `import collections`）
  时，方法内 `this.prop`（读**和**写）被 sema 解析到**泛型实例类**本身：
  `class 'Option_int_inst' has no member 'v'`——`this` 的类型被绑成 `Option_int_inst`
  而非当前 class。**struct** 含泛型字段（`Option<int> o`）+ `this.v = s` **正常**。
  无泛型字段的 class `this.v = s` 正常（BUG-019 修复后）。
- **根因（推测）**：sema 在解析 class 方法内 `this` 成员访问时，遍历/定位属性列表
  的上下文被「泛型类属性」污染（疑似与关联类型/泛型实例的 `var_class_map_` 或
  `resolved_object_class` 记录冲突），把 `this` 的类解析成最后一个泛型实例类。
  需查 `src/sema/sema_expr.cpp` visitMemberAccess 对 ThisExpr + 泛型属性共存路径。
- **影响**：任何「class 带 `Option<T>`/`ArrayList<T>` 等泛型属性 + 方法内用
  `this.prop`」的代码无法编译（如本会话 ARC 测试 `Holder { string msg;
  ArrayList<int> items; setMsg(){ this.msg = s; } }`）。含泛型属性的 class 是常见
  模式（缓冲/可选字段），影响面较大。
- **期望语义**：`this.prop` 始终解析为**当前 class** 的属性，与类内泛型属性共存
  无关。
- **备注**：BUG-019 修复验证中发现（ARC 字段 this 写测试触出）；manual.md §7
  `this` 关键字按设计意图描述，不标 bug。

---

## BUG-022（未修复）：`@thread` 用于 struct 实例被静默接受（应拒绝却接受）

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：待建（修复后转负测试 `tests/negative/struct_thread.myp`）——
  「应拒绝却接受」型（修复前编译绿，修复后应报错），与 BUG-006/007/008/012 同类。
- **现象**（实测 2026-08-18）：`struct S { int v; }` + `@test void t(){ S s @thread; }`
  → 编译通过（exit 0）+ 运行通过（0 断言，无效果）——`@thread` 注解被**静默忽略**，
  不建线程、不报错。manual.md §7 struct vs class 表标 `@thread` ❌（struct 不支持），
  编译器却接受。
- **根因**：sema/codegen 对 `@thread` 注解未校验目标必须是 class 实例；struct 局部
  变量声明上的 `@thread` 被当作普通注解吞掉。
- **影响**：文档声称 struct 不可 `@thread`，实际静默接受无效果——用户可能误以为
  建了线程；应编译报错 `@thread` can only be applied to class instances。
- **期望语义**：`@thread` 仅可用于 class 实例声明；struct 实例加 `@thread` →
  编译错误。
- **备注**：manual.md §7 表格按设计意图描述（不标 bug）；同族「应拒绝却接受」bug
  见 BUG-006/007/008/012。

---

## BUG-023（未修复）：`@parallel for` / `@gpu for` 并行体直接访问 class/static 属性数组

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：`tests/bugs/parallel_prop_access.myp`（`@static class X { property:
  int[] arr; }` + `@parallel for (int i...) { X.arr[i] = i; }` → 编译失败红）
- **现象**（实测 2026-08-18）：
  - `@parallel for` 体内**直接写** class/static 属性数组（`X.arr[i] = i` /
    `this.arr[i] = i`）→ `LLVM verify failed: GEP base pointer is not a vector or
    a vector of pointers`（IR `getelementptr i32, i64 0, %0`——GEP 基址是整数 0
    而非指针，属性访问未解析为数组指针）。
  - 经 `Atomic.addInt(X.arr, i, i)`（把静态数组当实参）→ 编译通过但**运行段错误
    139**（3/3 复现，只打印 start 后崩）。
  - `@gpu for` 体内同样：class 属性数组（`this.data[i]`）→ LLVM verify 失败。
  - **对照**：并行体用**外层局部数组**（`int[] tally = ...` / manual 示例
    `double[] data = new double[n]`）→ 正常（含 Atomic 累加、@gpu CPU 回退）；
    manual BNCT 模式（静态数组先拷局部 `double[] depthDose = TallyData.depthDose`）
    → 正常。
- **根因（推测）**：`@parallel for`/`@gpu for` 的变量捕获机制只收集**外层局部变量**
  （`generateParallelFor` 遍历作用域栈收集 named values 构建捕获 struct）；class/
  static 属性访问是全局路径，未被捕获进捕获结构体，codegen 生成 GEP 时基址解析
  失败（落到 i64 0）。应：捕获时也处理属性访问（或报清晰编译错误「并行体不能
  访问属性，请先拷到局部」）。
- **影响**：想直接在并行循环里读写 class/static 数组属性的代码无法编译/运行崩溃；
  常见规避是拷到局部（manual §9 BNCT 示例已用该模式）。
- **期望语义**：并行体支持属性访问（捕获 this/static 指针），或编译期清晰报错
  提示「先拷到局部」。
- **备注**：manual §9 @parallel for 限制与 @gpu for 限制已加说明（并行体只捕获
  局部变量，属性访问先拷局部）；bug 仅记录于本清单。

---

## BUG-024（未修复）：相对路径导入去重不解析 `..` → 同文件不同相对路径重复载入

- **状态**：🟥 未修复（2026-08-18 记录）
- **复现测试**：`tests/bugs/relimport_dedup.myp`（`import "./helper.myp";` +
  `import "./sub/sub.myp";`，sub 内部 `import "../helper.myp";` 同一文件 → 编译失败红）
- **现象**（实测 2026-08-18）：同一文件经**不同相对路径**导入不去重：
  - main 直导 `./helper.myp`（→ `/tmp/mod/helper.myp`）+ 递归导入
    `./sub/sub.myp`（内部 `../helper.myp` → `/tmp/mod/sub/../helper.myp`）
    → `duplicate class name 'Helper'` / `duplicate function name 'helperTop'`
    （Semantic analysis failed）。
  - **同串** `import "./helper.myp";` 两次 → 正常去重（仅 1 次载入）。
  - manual/design 声称「自动去重（基于规范化路径/模块名）」——实际只按**路径字符串**
    去重，`..` 未规范化 → 两个指向同一文件的路径串被视为不同。
- **根因**（`src/main.cpp` loadModule ~164-169）：去重键 `dedup_key = is_path ?
  normalizePath(path) : module_name`；`normalizePath`（~59）只移除开头 `./`、`/./`、
  `//`，**不解析 `..` 父目录组件**——`/tmp/mod/helper.myp` 与
  `/tmp/mod/sub/../helper.myp` 规范化后仍不同 → 双双载入 → duplicate。
- **影响**：分层模块项目里共享 helper 被「直导 + 递归（经 `..`）」同时引用即编译失败
  （manual §10 项目组织建议的分层模式受限于此）；标准库子模块内部相对导入
  （`import "./gpu/backend.myp"`）与用户直导同名文件也可能撞车。
- **期望语义**：`normalizePath` 解析 `..`（折叠 `/a/../b` → `/b`），使同一文件无论经
  哪条相对路径都去重到同一规范键。
- **备注**：manual §10 导入规则已加注（去重按路径字符串，`..` 未规范化；见 tests/bugs/）；
  bug 仅记录于本清单。

---

## BUG-025（已修复）：多文件编译只合并第一个文件的 imports / structs / enums / ffis 等

- **状态**：🟩 已修复（2026-08-18 审计 §12 发现并修复）
- **复现测试**：回归 `tests/test_multifile.sh`（已接入 `tests/run_tests.sh`；
  BUG-025 第二文件 import 合并 + struct/enum 第二文件可见两用例）
- **现象**（实测 2026-08-18）：
  - `mypc a.myp b.myp`，`b.myp` 含 `import env;` + 类内 `Console.writeString(...)` →
    `undefined symbol 'Console'`（错误行号错位到 a.myp 合并区）；`import test` 同理
    `undefined symbol 'Test'`。
  - **文件顺序相关**：import 在**第一个**文件 → 正常；在第二个 → 丢弃。
  - **无 import** 的多文件（纯函数跨文件调用）→ 正常。
  - 第二个文件里的文件级 `struct` / `enum` 变体同样不可见（`unknown class 'VecE'` /
    `undefined symbol 'GREEN'`）——合并循环只搬了 classes/interfaces/mappings/
    functions。
- **根因**（`src/main.cpp` 多文件分支 ~1068-1076）：合并循环
  `auto merged = std::move(units[0]);` 后只对 `units[1..]` 搬运 classes/interfaces/
  mappings/functions 四个字段，**漏了 imports/structs/bitfields/enums/ffis/macros/
  type_aliases**。`merged->imports` 只含第一个文件的 import，故 `loadModule`
  （对 imports 递归加载）只处理第一个文件的导入。
- **修复**：合并循环补齐全部 11 个字段（imports/structs/bitfields/classes/
  interfaces/mappings/functions/enums/ffis/macros/type_aliases）。`loadModule` 按
  模块名/规范化路径去重，跨文件重复 import 只加载一次，无重复定义风险。
- **验证**：`tests/test_multifile.sh` 4 用例全过（跨文件函数 / 第二文件 import env /
  第二文件 struct+enum+@test / 多文件 @test + 用户 main）；全量回归 288 通过。
- **备注**：manual §12 多文件编译「合并为单模块」按设计意图描述（修复后成立）；
  bug 仅记录于本清单。

---

## BUG-026（已修复）：`--test` + 用户 `int main()` → 空块无 terminator + 运行器静默不跑

- **状态**：🟩 已修复（2026-08-18 审计 §12 发现并修复）
- **复现测试**：回归 `tests/test_multifile.sh`（BUG-026 --test + 用户 main 运行器
  执行用例）
- **现象**（实测 2026-08-18，BUG-025 修复后暴露）：
  - `mypc --test x.myp`，源码含 `int main() { return 0; }` + `@test` 函数 →
    `LLVM verify failed: Basic Block in function 'main' does not have terminator!`
    （编译失败）。
  - 修复空块 terminator 后若**保留**用户 main 占位，则 `generateTestRunner()` 新建的
    `main` 因名字冲突被 LLVM 自动改名 `main.1`，链接入口仍是用户空 `main` →
    **测试静默不跑**（exit 0、tests: 0 假过）——比崩溃更危险。
  - 现有 `tests/@test/*.myp` 均无用户 main（依赖 `--test` 生成运行器 main），故该
    路径此前从未被覆盖。
- **根因**（`src/codegen/codegen_class.cpp` generateFunctionDecl ~1655）：
  `decl.name == "main"` + `test_mode_` 分支**提前 return** 跳过用户 main 主体，
  但函数/入口块已创建且空、无 terminator → LLVM verify 失败；占位函数残留在模块
  内使运行器 main 改名。
- **修复**：test 模式跳过用户 main 时 `func->eraseFromParent()` 擦除空占位函数，
  使 `generateTestRunner()` 的 `main` 保持名字并成为真正入口（空块随之消失，
  无 terminator 问题）。
- **验证**：`mypc --test` + 用户 `int main()` + `@test` → 运行器执行
  （`RUN: t_cross_file`、`tests: 1, assertions: 1 passed, 0 failed`、exit 0）；
  全量回归 288 通过。
- **备注**：manual §12 测试框架按设计意图描述；bug 仅记录于本清单。

---

## BUG-027（已修复）：`tools/codegen` 未迁移到 BUG-001 属性私有规则

- **状态**：🟩 已修复（2026-08-18 修复，迁移到 getter）
- **复现测试**：回归 `tools/codegen/run_tests.sh`（serde/ffi/resources/autodiff/idl/idl_socket/
  orm/embed/--verify/dsl/infer_ops 生成 → 编译 → round-trip 全绿）；已接入 `tests/run_tests.sh`。
- **背景**：`tools/codegen/` 是 schema 驱动的代码生成框架（torchgen 式，纯 MYP，
  P0–P8 已实施，2026-08-12）：`main.myp` CLI + `schema.myp`（JSON schema 解析）+
  `model.myp`（模型类）+ `emit.myp`（发射器）+ 各 `gen_*.myp`。§13 编译与工具曾未文档化。
- **现象**（实测 2026-08-18）：`mypc tools/codegen/main.myp` 301 个编译错误，全为
  `cannot access property '<p>' of '<Class>' from outside the class (properties are
  private; use a getter action or a struct)`。约 40 组（类,属性）对、涉及 15 个模型类
  （Expr/Field/TypeDecl/ServiceDecl/DslOp/DslDecl/TableDecl/MethodDecl/FfiParam/
  Resource/ExprDecl/EmbedDecl/OpsOp/FfiFunc/Schema）。跨类访问**全部是读、无写**。
- **根因**：BUG-001 修复（2026-08-16，`414c5dd`）后 `tools/codegen` 未迁移；自举编译器
  （tools/selfhost）当时已迁 getter，本工具遗漏。
- **修复**：①模型类加 getter（`get<Prop>()`，model.myp 各 class + gen_autodiff 的 Expr，
  统一命名使 `x.prop → x.getProp()` 与变量类型无关）；②Python 脚本迁移跨类读
  （含 `).prop`/`].prop` 链式形态，跳过字符串/注释，224+6 处替换）；③gen_dsl 生成模板
  也犯 BUG-001——生成的 `CalcExpr` 私有属性 + 生成的 `_eval` 跨类读，已给生成类加
  getter 并让模板发出 getter 调用；④修复 run_tests.sh 相对 MYPCC 路径解析 + 接入主套件。
  **判断：全部加 getter，无 struct 转换**——Expr 是递归树（struct 无限大小）；其余类都
  是 `new`+`ArrayList` 堆对象（值语义会破坏共享/引用）；selfhost AST 先例即 getter。
- **验证**：`tools/codegen/run_tests.sh` 全绿（11 个生成器 round-trip）；全量回归
  292 通过 / 0 失败。
- **备注**：修复后 §13 已补 `代码生成工具（tools/codegen）` 文档节。
