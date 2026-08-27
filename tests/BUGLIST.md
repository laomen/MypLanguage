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
| BUG-002 | 🟩 | @coro 主流程增量 spawn 卡死/帧损坏 | `tests/bugs/coro_incremental_spawn.myp` |
| BUG-003 | 🟩 | 泛型 T=string 的 `<`/`>` 按指针比较 | `tests/bugs/generic_string_cmp.myp` |
| BUG-004 | 🟩 | `Option<struct>` 泛型实例化失败 | `tests/bugs/option_struct.myp` |
| BUG-005 | 🟩 | mapping 事件 action 在事件源线程执行（非 action 实例线程） | （待建 `tests/bugs/mapping_thread.myp`） |
| BUG-006 | 🟩 | `main()` 直调检查被运算符/管道语法绕过（`A + B`、`A |> Op` 不报错） | （待建，修复后转负测试 `tests/negative/main_not_wiring.myp`） |
| BUG-007 | 🟩 | `bitvector<N>` 宽度未校验——`bitvector<3>` 静默映射为 i32（应报错） | （待建，修复后转负测试 `tests/negative/bitvector_width.myp`） |
| BUG-008 | 🟩 | 接口 action 参数类型/个数不校验——粗粒度签名匹配（名称+返回类型 basic_type） | （待建，修复后转负测试 `tests/negative/interface_param_mismatch.myp`） |
| BUG-009 | 🟩 | 一个类内多个 `@startup` 行为不一致——`@thread` 入口取最后一个、`mypc run` 合成 main 取第一个 | （待建：运行时行为差异，手动双命令复现） |
| BUG-010 | 🟩 | 类引用字段的链式属性访问 `ref_.a` codegen 类型错误——生成 ptr 而非属性类型，LLVM verify 失败 | （`tests/bugs/ref_field_chain.myp`，修复后转正测试） |
| BUG-011 | 🟩 | 函数内 mapping 用实例变量名节点（`s.e -> t.a`）→ LLVM verify 失败；须用类名节点（`S.e -> T.a`） | （`tests/bugs/instance_mapping_verify.myp`，修复后转正测试） |
| BUG-012 | 🟩 | 直接跨线程调用（`@thread` 实例普通 action）编译器不检查——design.md §8.2 声称「不允许，必须通过 mapping()」，实测编译通过 | 行为测试 `tests/@test/cross_thread_call.myp`（修复后转负测试 `tests/negative/cross_thread_call.myp`） |
| BUG-013 | 🟩 | `Coro.resume` 返回值串值——runtime.c 用线程本地共享槽存 yield/resume 值，多协程混用时后挂起者覆盖前者 → resume 返回错误值 | 回归 `tests/@test/coro_resume_value_mix.myp`（await 值挂起 + Coro.yield + Async.sleep 定时器三协程，3 断言） |
| BUG-014 | 🟩 | `Atomic.loadInt`/`storeInt` 编译成**普通非原子** `load`/`store`——仅命名带 Atomic，实际无原子性/内存序保证（只有 add/sub/xchg/addDouble 走 atomicrmw） | （待建：编译+`--emit-llvm` 断言 IR 为 `load`/`store` 而非 `atomicrmw`） |
| BUG-015 | 🟩 | `mypc --package-path` **不支持冒号分隔多路径**——`myp build` 把本地 `myp_packages/` 与 `MYP_PACKAGE_PATH` 合并为冒号串传入，mypc 不切分 → `cannot find import`（自举 `myp_self` 支持切分） | （待建：shell 断言 `mypc --package-path "a:b"` 包在 b 时编译成功） |
| BUG-016 | 🟩 | `var r = voidCall();` / `int x = voidCall();` **void 值赋给变量**被 sema 放行 → codegen 段错误（`llvm::DataLayout::getAlignment` 无限递归）；main(argc,argv) 传参本身无 bug | 负测试 `tests/negative/var_void_init.myp`、`tests/negative/void_value_init.myp`（编译拒绝） |
| BUG-017 | 🟩 | 关联类型接口方法返回 **string** 经接口分派 → 分派 stub 用 i32（关联类型占位符回落默认 int）→ `call i32 %iface_fn` 把 string 当 i32 → LLVM verify 失败/段错误 | 回归 `tests/@test/assoc_string_dispatch.myp`（string+int 双关联类型动态分派，4 断言） |
| BUG-018 | 🟩 | `import collections` + 带关联类型约束的泛型类（`where T : I` + `T::Item`）→ 8 个伪错误 `expected numeric type, got 'I'`（类通用类型参数在全局作用域声明泄漏，同名 T 被覆盖） | 回归 `tests/@test/assoc_constraint_import.myp`（collections + where 约束泛型类，1 断言） |
| BUG-019 | 🟩 | `this.field = value` 写被拒——struct/class 的 `this.field` 分支误嵌套在 `if (!op)` 内，`this.x = v`（op 非空）整块跳过 → `not a valid assignment target` | 回归 `tests/@test/manual_ch7_struct.myp` t_this |
| BUG-020 | 🟩 | 文件级限定 struct 定义 `struct A::B { }` 被拒——顶层 dispatch `current_--` 回退使 parseStruct 限定分支看 `struct` 关键字而非名称 → `expected struct name`（自举支持） | 回归 `tests/@test/manual_ch7_struct.myp` t_nested_qualified |
| BUG-021 | 🟩 | class 含**泛型类属性**（`Option<int>`/`ArrayList<int>` 等）时，方法内 `this.prop` 在 sema 被解析为泛型实例类 → `class 'X_inst' has no member 'v'`（sema 泛型实例化污染 current_class_name_ 不恢复） | 回归 `tests/@test/this_generic_prop.myp`（泛型属性 + this 读写 + 方法调用，4 断言） |
| BUG-028 | 🟩 | 类属性带 **ARC 初始化器**（`property: Foo f = new Foo();`）→ fresh new 的 rc=1 在语句末被释放 → 属性槽悬垂（use-after-free；setter 重赋值 → 双释放段错误 139） | 回归 `tests/@test/property_init_arc.myp`（初始化器对象存活 + 多次重赋值读取，3 断言） |
| BUG-029 | 🟩 | 类字段直接转 interface（`View v = <类字段>`）→ 坏胖指针（vtable 丢失）→ 段错误/内存损坏（codegen 只从 var_class_map_ 解析类名，字段查不到 → null vtable） | 回归 `tests/bugs/iface_field_conversion.myp`（裸名+this 形态，2 断言） |
| BUG-030 | 🟩 | mapping 事件在目标类构造器内触发 → 派发到未注册的自身实例（`__myp_inst_X` 构造后写入）→ 段错误 139 | 回归 `tests/bugs/mapping_ctor_self.myp`（ctor 内 2 次派发，1 断言） |
| BUG-031 | 🟩 | 跨线程多 @thread 目标事件无限重投（mapping handler 注册 instance=NULL → routed 副本在目标线程跑**所有**同 event 的 NULL-instance handler → 互相 route 乒乓） | 回归 `tests/bugs/cross_thread_multi_target.myp`（A/B 各收 1 次，2 断言） |
| BUG-032 | 🟩 | `this` 作为值（实参/赋值/返回，如 `Holder.set(this)`）被传 alloca **地址**而非实例值——`set` 把 &栈槽 存进属性，`get()` 当实例读 → 字段错位（49152 / this=0x100000000 → strcmp 段错误）；无 event 类时栈布局碰巧 0 未暴露，含 event 类必现 | 复现 `tests/bugs/b032_event_class_inst_store.myp`（@test 断言 get count==0，已绿）；C++ `generateThisExpr` 修复（load this 值）+ selfhost 本就正确（loadThis 已 load） || BUG-033 | 🟩 | **数组元素 → interface**（`View v = arr[i]`，arr 为**类属性数组**）→ 坏胖指针（vtable 丢失）→ RootView.onTouch 遍历 `call *0x8(vtable)` 段错误 139（BUG-029 只覆盖字段+局部变量数组，类属性数组元素 SubscriptExpr 走 else 分支只存 data） | 回归 `tests/bugs/b033_iface_array_elem.myp`（类属性数组元素 + 局部数组元素，3 断言，双编译器） || BUG-022 | 🟩 | `@thread` 用于 **struct 实例**被静默接受（`S s @thread;` 编译+运行通过但无效果）——应拒绝却接受（与 BUG-006/007/008/012 同类） | （待建：修复后转负测试 `tests/negative/struct_thread.myp`） |
| BUG-023 | 🟩 | `@parallel for` / `@gpu for` 并行体**直接访问 class/static 属性数组** → LLVM verify 失败（`getelementptr i32, i64 0` GEP 基址为 0 非指针）/ `Atomic.addInt` 时运行段错误 139 | 回归 `tests/@test/parallel_prop_access.myp`（静态属性数组写+读+Atomic 累加，4 断言） |
| BUG-024 | 🟩 | 相对路径导入去重**不解析 `..`**——同一文件经不同相对路径（直导 `./helper.myp` + 子模块内 `../helper.myp`）规范化后仍不同 → 双重载入 → `duplicate class name`/`duplicate function name`（design §9 声称"规范化路径去重"未实现） | 回归 `tests/@test/relimport_dedup.myp` |
| BUG-025 | 🟩 | 多文件编译 `mypc a.myp b.myp` **只合并第一个文件的 imports**——合并循环漏了 imports/structs/bitfields/enums/ffis/macros/type_aliases（只合并 classes/interfaces/mappings/functions）→ 第二个文件的 `import env` 静默丢弃 → `Console` 未定义 | 回归 `tests/test_multifile.sh` |
| BUG-026 | 🟩 | `mypc --test` + 源码含用户 `int main()` → 用户 main 空块**无 terminator**（`LLVM verify failed: Basic Block in function 'main' does not have terminator!`），且残留占位使测试运行器 main 被改名为 `main.1` → 测试**静默不跑**（exit 0 全假过） | 回归 `tests/test_multifile.sh`（BUG-026 用例） |
| BUG-027 | 🟩 | `tools/codegen` 代码生成工具**未迁移到 BUG-001 属性私有规则**——模型类（`Expr`/`Field`/`TypeDecl`/`ServiceDecl`/`DslOp`/`Resource` 等 15 类）的 `property:` 被生成器跨类读取 → 301 个编译错误（`cannot access property ... from outside the class`），框架（serde/ffi/autodiff/idl/orm/embed/dsl/infer_ops）整体不可用 | 回归 `tools/codegen/run_tests.sh`（已接入 `tests/run_tests.sh`，全绿） |
| BUG-046 | 🟩 | 类内同名 **static 方法**（签名不同）无重复检测 → codegen 用同一 LLVM 函数生成不同签名 body → `Function::getArg() out of range` 崩溃（static action 注册 `declare("Class.name")` 返回值未检查；properties/actions/functions/events 都有查重唯独 static 漏） | 负测试 `tests/negative/duplicate_static_action.myp`（编译拒绝） |

| BUG-047 | 🟩 | selfhost ARC 调用返回值**双重 retain 泄漏**——BUG-036 误删 Call/genIfaceCall 的 `addFreshTemp` → 方法调用返回每次泄漏 +1（parity arc/arc_m2/weak_cycle 根因） | 回归 parity arc/arc_m2/weak_cycle |
| BUG-048 | 🟩 | selfhost 闭源分发/链接缺口——无预编译库(.so/.a)发现 / 无 `--shared` 库模式 / 链接成功不打印 "Link OK"（parity closed-lib 根因） | 回归 `tests/test_closed_lib.sh`（closed-lib 12/12） |
| BUG-049 | 🟩 | selfhost codegen `&&`/`||` 结果槽栈泄漏——短路降级内联 `alloca i1` 循环体内每轮执行 RSP 不恢复 → 无限循环跌破栈底崩溃（rt_thread_test 自旋暴露） | 回归 `bench/freestanding/rt_thread_test.myp`（自旋 `||` 直写，8/8 稳定） |
| BUG-050 | 🟩 | 两个编译期解析缺口：(1) **裸 const 标识符不折叠**——顶层 `const int CAP=1024` 被两个编译器都解析成零参 const-decl **函数**类型，裸引用 `CAP`（非 `CAP()`）报 `expected numeric type, got 'function'/'() -> int'`；(2) **selfhost 对 `__myp_*` 盲目去前缀**——通用 Identifier callee 路径把真 `__myp_coro_*`/`__myp_destroy_*` 符号去前缀成 `myp_coro_resume`（未定义符号），且去前缀后函数类型解析失败 → 字面量实参 `0` 不提升成 i64 | 回归 裸 const（`CAP*8` 折叠 + `CAP()` 显式调用双编译）+ shadow `rt_coro_test`（IR `call i64 @__myp_coro_resume(i64, i64 0)`）+ 全量 323 |
| BUG-051 | 🟩 | **@static 类属性默认值不生效**——@static 实例全局恒 `zeroinitializer`（C++ codegen.cpp:1549 `ConstantAggregateZero` / selfhost codegen.myp:1509 `zeroinitializer`），属性默认值（`int x = 5;`/`= -1`）只在 `new` 路径应用（generateNewExpr:3560），@static 全局从不 `new` → 非零默认值静默变 0。手册 manual.md:167 写明「默认值在 new 时生效」→ 文档化但坑（手册又推荐 @static 类做类级共享常量）。**v3.15.69 selfhost codegen 已修复**（显式常量初始化器，含常量折叠）；oracle 冻结不改。**运行时 workaround 已回滚**（coroEnsureInit 显式置 current=-1、thread stackSize 显式置 1048576） | 回归 `tests/@test/manual_static_defaults.myp`（int/long 折叠/一元浮点/bool/字符串/int 除法，7 断言） |
| BUG-052 | 🟩 | 解析器**错误恢复死循环 OOM**（selfhost，v3.15.85）：`parseEnumDecl` 变体循环 / `parseMatchStmt` 臂循环 `consume`/`parseIdentifier` 失败都**不 advance** → 非法枚举（MYP 枚举须分号 `enum E { V0; V1; }`，逗号 `V0, V1` 即触发）或非法 match 臂（`E => {...}` 缺 `.`/变体名）卡死 → 每次迭代 `perr` 追加 Diag → 无界内存分配（实测 ~18GB，OOM 崩溃系统）。与 2026-08 LSP 爆炸同类缺陷的漏网点（当时只修 parseBlock/parseMapping） | 负测试 `tests/negative/enum_comma_separator.myp`、`tests/negative/match_missing_dot.myp`（7.5MB 有界报错） |
| BUG-053 | 🟩 | `exprLlvmType` 对 `+` Binary **指数爆炸**（selfhost codegen，v3.15.87）：`+` 分支为判字符串拼接调 `exprLlvmType(lhs/rhs)`，底部 `llt/rlt` 又算一次 → 左深链 `0+1+1+...` 每层 2 次递归、逐层翻倍 → 深度 24 编译 6.2s、深度 28 >20s（原 10,000 加号 >120s 超时不可编译）。内存低（7-24MB），纯时间爆炸 | torture `tests/torture/generated/execute/plus_bomb_0.myp`（10,000 加号 + 100 i++，现 10.8s） |
| BUG-054 | 🟩 | **不同 struct 类型赋值/初始化静默过 sema**（selfhost sema，v3.15.87）：`typesCompat` 只比 kind "struct"，`A a; B b; a = b;` 或链式字段 `root.inner...inner = root.inner`（L4=L1）被放行 → LLVM opt 报 `defined with type %B but expected %A` 崩溃（非干净诊断）。**附带隐患**：Member 表达式 `resolvedClass` 是**容器**类型名（`root.inner`→L0 而非 L1），直接比 resolvedClass 会误拒合法 `L2 y = a.b.c;` | 负测试 `tests/negative/struct_assign_mismatch.myp`（`A a; B b; a = b;` → `cannot assign value of type 'B' to variable of type 'A'`） |
| BUG-055 | 🟩 | **解析器表达式/语句级递归无守卫 → 深嵌套栈溢出**（selfhost parser，v3.15.88）：`recursionDepth_` 只数 `parsePrimary`（括号 300 守卫），而**三元（`1?2:1?2:...`）、`??` 合并、右结合赋值（`a=b=c`）、一元链（`!!!!x`）、嵌套块 `{{{...}}}`、if 链（`if(1) if(1)...`）**的右嵌套递归在 parseExpr/parseAssignment/parseCoalesce/parseUnary/parseBlock/parseStatement 层（parsePrimary 之上）——不被计数 → 50000 层 SIGSEGV 栈溢出 | 负测试 `tests/negative/expr_recursion_deep.myp`（500 层三元 → `expression nested too deeply` 干净拒绝）；torture `deep/*`（48 个，编译不崩溃） |
| BUG-056 | 🟩 | **嵌套泛型实例化 O(N³) 时间爆炸（DoS）**（selfhost sema，v3.15.89）：`Box<Box<...<int>...>>` 深度 N 时 `SymbolTable.lookup` 线性扫描（entries_ 已 N 项 × 长名比较）+ `instClassName` 递归拼名（每层 O(k²)）→ 总 O(N³)。实测 depth 800 >25s、depth 5000 不可完成；纯解析（不存在的泛型类）0.1s 平坦 → 爆炸全在 sema 实例化 | 守卫：`tryInstantiate` 顶层 `typeArgDepth` 测深，>64 单条 `generic type nested too deeply (N > 64)` 干净拒绝（合法嵌套 ≤4 层）；负测试 `tests/negative/generic_nested_deep.myp`；torture `deep/generic_*`（+8）；正向 `tests/@test/nested_generic.myp`（2/4/10 层正常编译） |
| BUG-057 | 🟩 | **顶层函数调用结果链式成员访问回落当前类**（selfhost sema，v3.15.90）：普通顶层函数调用 `rawStep(5).get()` 只设返回 kind 不设 CallExpr 的 valueClass/resolvedClass → 成员访问回落到当前类 `class 'X' has no member 'get'`。仅带显式类型实参的泛型调用（`resultOk<int,string>(7).get()`）、类 action（`c.m().get()`）、`new` 路径设了 → 不一致。类/struct/interface 返回均受影响 | 修复：`findFuncRet` 命中且 kind 为 class/struct/interface 时用 `findFuncRetType`+`gsRetValueClass` 设 `e.setValueClass`；回归 `tests/@test/call_result_chain.myp`（6 测试/12 断言，Result/具体类/struct/interface/两层链/lambda） |

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

## BUG-005（已修复）：mapping 事件 action 在事件源线程执行（非 action 实例线程）

- **状态**：� 已修复（2026-08-18）
- **复现测试**：`tests/bugs/mapping_thread.myp`（新增 `myp_thread_self()` 诊断 FFI：
  断言 mapping handler 在 handler 实例自己的线程执行；修复前红线）
- **修复**（C++ 与自举编译器同修）：
  - `src/runtime/runtime.c`：`myp_event_fire` 增加 `data_size` 参数；新增
    `myp_thread_is_current(instance)` / `myp_event_route_to_instance(...)`；
    `myp_event_t` 增 `data_size`/`data_owned`/`routed` 字段；`myp_event_dispatch`
    对归属其他线程的 handler 将事件深拷贝投到其线程队列，路由副本 `routed=1`
    不再重复路由，处理后 free 拷贝载荷。
  - `src/codegen/codegen_class.cpp generateMappingDecl`：handler 内对首个非静态
    目标实例做 `myp_thread_is_current` 检查——目标在其他线程 → 调用
    `myp_event_route_to_instance` 后返回，否则直接调用 action。
  - 自举镜像：`tools/selfhost/src/codegen.myp` genMappingChain 同检查；
    `genThreadVar` 补存 `@__myp_inst_<Cls>` 全局（原缺失 → handler 取到 null，
    路由失效）；`ir_emit.myp` 更新 `myp_event_fire` 4 参声明 + 新增两个运行时
    declare。
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

## BUG-006（已修复）：`main()` 直调检查可被运算符/管道语法绕过

- **状态**：� 已修复（2026-08-18）
- **复现测试**：`tests/negative/main_not_wiring.myp`（外部 `@op` + 管道 + 运算符
  绕过，编译拒绝）
- **修复**：`src/sema/sema_expr.cpp` visitBinaryOp（解析为外部 `@op` 的 `+`/`-`
  等）与 visitPipe（class transform）在 main() 内拒绝；自举 `tools/selfhost/src/
  sema.myp` 同镜像（visitBinaryOp ~3771、visitPipe ~4981）。struct 方法调用保留
  放行。
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

## BUG-007（已修复）：`bitvector<N>` 宽度未校验，非 8/16/32/64 静默映射为 i32

- **状态**：� 已修复（2026-08-18）
- **复现测试**：`tests/negative/bitvector_width.myp`（`bitvector<3>` 编译拒绝）
- **修复**：`src/sema/sema.cpp builtinTypeToTypeInfo` 校验 `bitvector<N>` 宽度
  ∈ {8,16,32,64} 否则报 `bitvector width must be 8/16/32/64`；自举
  `tools/selfhost/src/sema.myp` 同镜像。
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

## BUG-008（已修复）：接口 action 参数类型/个数不校验（粗粒度签名匹配）

- **状态**：� 已修复（2026-08-18）
- **复现测试**：`tests/negative/interface_param_mismatch.myp`（接口 `double area(int
  a, int b)` 对实现 `double area(int a)`，编译拒绝）
- **修复**：`src/sema/sema.cpp checkInterfaceImpl` 的 `matches` lambda 升级为
  **精确签名**——名称 + 参数类型 + 返回类型全部一致（`paramsMatch`）；事件按名称 +
  参数类型匹配；关联类型参与时保留仅名称匹配。自举 `tools/selfhost/src/sema.myp`
  同镜像。
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

## BUG-009（已修复）：一个类内多个 `@startup` 行为不一致

- **状态**：� 已修复（2026-08-18）
- **复现测试**：`tests/negative/multiple_startup.myp`（一个类多个 `@startup` 编译
  拒绝）
- **修复**：`src/sema/sema.cpp visitClassDecl` 报
  `at most one @startup per class`；自举 `tools/selfhost/src/sema.myp` 同镜像。
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

## BUG-010（已修复）：类引用字段的链式属性访问 `ref_.a` codegen 类型错误

- **状态**：� 已修复（2026-08-18）
- **复现测试**：`tests/@test/struct_prop_chain.myp`（裸/显式 this 的 struct 属性字段
  读写，9 断言；C++ 与自举编译器均绿）——本 bug 记录的是类引用字段链，实测同类修复
  覆盖 struct 属性字段链（`s.x` / `this.s.x`）
- **修复**：
  - 读：`src/codegen/codegen_expr.cpp generateMemberAccess` 加裸 struct 属性分支
    （GEP this→s 槽 → GEP 字段）；属性遍历 `continue`（非首属性不再 break）。
  - 写：`src/codegen/codegen_stmt.cpp generateAssignment` 在 `if(!op)` 内、错误
    兜底之前加裸属性分支（原 2222 块位于错误之后不可达——死代码，已移除）；
    `generateStructMemberAddress` ThisExpr 分支支持类 struct 属性（`this.s.x`）。
  - 自举镜像：`tools/selfhost/src/codegen.myp` memberAddr / memberFieldType /
    memberFieldAstType 加 `bareStructPropName` 分支；写路径复用 memberAddr 自动
    生效。
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

## BUG-011（已修复）：函数内 mapping 用实例变量名节点 → LLVM verify 失败

- **状态**：� 已修复（2026-08-18）
- **复现测试**：`tests/negative/instance_mapping.myp`（函数内 `mapping(){ s.e ->
  t.show; }` 编译拒绝，报 `mapping 节点 ... 是函数内局部变量名；实例级映射暂不支持
  ，请改用类名节点`）
- **修复**：`src/sema/sema.cpp` MappingStmt 访问器对非函数/非 lambda/非 transformer
  节点检查——非类名但符号表有（函数局部变量）→ 诊断；消息带真实类名提示
  （`Source.e`）。自举 `tools/selfhost/src/sema.myp analyzeMapping` 同镜像。
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

## BUG-012（已修复）：直接跨线程调用（`@thread` 实例普通 action）编译器不检查

- **状态**：� 已修复（2026-08-18）
- **复现测试**：`tests/negative/cross_thread_call.myp`（`W w = new W() @thread;
  w.work();` 编译拒绝，报 `cross-thread calls must go through mapping()`；原行为
  测试 `tests/@test/cross_thread_call.myp` 移除）
- **修复**：sema 对 `@thread` 实例的直接 action 调用一律拒绝（`@startup` 手动调用
  原有规则保留）；自举 `tools/selfhost/src/sema.myp` 同镜像（~4385）。
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

## BUG-013（已修复）：`Coro.resume` 返回值串值（混用 `await expr` + `Coro.yield`）

- **状态**：🟩 已修复（2026-08-18）
- **根因**：`src/runtime/runtime.c` 用 `__thread` **线程本地共享槽** `myp_coro_yield_val`
  / `myp_coro_resume_val` 存「上次挂起传出的值」与「上次 resume 传入的值」。同线程
  多协程混用时，后 spawn/挂起的协程覆盖前者：echo 挂起传出 10 → topLevel `Coro.yield(42)`
  覆盖 → `Coro.resume(echo_h, 100)` 返回 42；加 `Async.sleep` 定时器挂起覆盖为 0。
  协程内部值传递正确（每协程栈局部），只有 resume 返回值读共享槽串值。
- **修复**：yield/resume 值改为**每协程存储**——`myp_coro_t` 新增 `yield_val`/`resume_val`
  字段，`__myp_coro_yield` 存 `myp_coros[saved]->yield_val`、`__myp_coro_resume` 存
  `myp_coros[idx]->resume_val` 并返回 `myp_coros[idx]->yield_val`；`__myp_coro_create`
  槽复用/新建时清零。多协程、嵌套 resume 均按各自槽取回。
- **验证**：回归 `tests/@test/coro_resume_value_mix.myp`（echo await 值挂起 10 +
  topLevel Coro.yield 42 + timerCoro Async.sleep 挂起 0 三协程混用，3 断言，3 次运行
  稳定）；`tests/bugs/coro_resume_value_mix.myp` 移除。全量回归 300 通过 / 0 失败
  （含 tests/coro、tests/coro_top 等既有协程用例）。
- **备注**：design.md §8.6.1 示例的规避写法（不打印 resume 返回值）现可放开；
  顺带修的 `run_bugs.sh` 分类逻辑保留。

---

## BUG-014（已修复）：`Atomic.loadInt`/`storeInt` 编译成普通非原子 `load`/`store`

- **状态**：� 已修复（2026-08-18）
- **复现测试**：`tests/@test/atomic_load_store.myp`（loadInt/storeInt 功能正确 +
  @parallel 累加，4 断言；C++ 与自举编译器均绿）
- **修复**：`src/codegen/codegen_expr.cpp` atomic intrinsic 分支——`__myp_atomic_
  load_i32`/`store_i32` 改用原子 `LoadInst`/`StoreInst` 构造器（seq_cst；LLVM 21
  IRBuilder 无 CreateAtomicLoad/Store）；自举 `tools/selfhost/src/codegen.myp`
  同镜像（`load atomic`/`store atomic` 文本）。
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

## BUG-015（已修复）：`mypc --package-path` 不支持冒号分隔多路径

- **状态**：� 已修复（2026-08-18）
- **复现测试**：`tests/test_package_path.sh`（`--package-path "dirA:dirB"` 包在
  dirB 编译成功）
- **修复**：`src/main.cpp loadModule` 按 `:` 切分 `--package-path` 逐路径查找
  （与自举 `myp_self` 一致）；自举已支持，无需镜像。
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

## BUG-016（已修复）：void 值赋给变量（`var r = voidCall();` / `int x = voidCall();`）→ 编译器段错误

- **状态**：🟩 已修复（2026-08-18）
- **根因（纠错）**：2026-08-18 复核发现原「`main(int argc, string[] argv)` 传参导致
  `DataLayout::getAlignment` 无限递归」的诊断**不成立**——`int main(int argc,
  string[] argv) { return argc; }` 编译运行完全正常（argv 作为 `string[]` 参数传递无
  问题）。真正触发是复现里那行 `var r = report(argc, argv);`：`report` 返回 **void**，
  sema 对 `var r = <void调用>()` 的推断类型为 void 却未拒绝（显式 `int x =
  <void调用>()` 也被 `init_type.kind != Void` 级联守卫误跳过）→ codegen 用
  Int(i32) alloca 存 void 值 → `CreateStore(void, i32*)` → LLVM
  `getPrefTypeAlign(void)` 无限递归 → 编译器段错误（exit 139）。与 argc/argv 无关。
- **修复**（C++ `src/sema/sema.cpp` visitVarDecl + 自举 `tools/selfhost/src/sema.myp`）：
  1. 推断路径：`var r = <void调用>();` → `cannot infer type of 'var' from a void
     expression`；
  2. 显式路径：`int x = <void调用>();` → `cannot initialize variable 'x' of type
     'int' with value of type 'void'`。
  两级均用 `diag_.errorCount()` 快照区分「已知 void 调用」（补报）与「未解析表达式
  （已级联报错）」（跳过），避免级联误报。
- **验证**：负测试 `tests/negative/var_void_init.myp` + `tests/negative/void_value_init.myp`
  （编译拒绝）；原复现 `tests/bugs/main_argc_argv_crash.myp` 移除（标题误导）。
  全量回归 295 通过 / 0 失败；自举 sema 对拍 94/94 全绿（C++ 与自举消息逐字一致）。
- **备注**：manual.md §5 按设计意图描述（不标 bug，符合约定）；`import args` 的
  `Args.*` 读全局 argv 的用法仍是最佳实践。

---

## BUG-017（已修复）：关联类型接口方法返回 string 经接口分派 → 分派返回类型错为 i32

- **状态**：🟩 已修复（2026-08-18）
- **根因**：接口虚表动态分派处（`src/codegen/codegen_expr.cpp` 三处：广义分派 /
  标识符接口变量 / 接口数组元素），`ret_ty` 一律用
  `typeNodeToCodegenType(method->action->return_type)` 取**接口声明**的返回类型。
  关联类型方法（`Item getVal()`）的声明返回类型是关联类型占位符 `Item` →
  `typeNodeToCodegenType` 回落默认 **i32**；而具体类方法（`string getVal()`）返回
  **ptr** → `call i32 %iface_fn(ptr %4)` 把 string 当 i32 → 调用方（期望 ptr 的
  `Test.assertStrEq`）verify 失败（单方法接口）；含其他方法时运行段错误 139。
  `Item=int` 因默认类型恰为 i32 侥幸通过。
- **修复**：新增 `CodeGen::ifaceDispatchReturnType(ma, method)`——接口分派返回类型
  优先从对象已知具体类（`var_class_map_` 标识符 / `array_elem_class_map_` 数组元素）
  解析其**同名方法**的返回类型（与 vtable 指向的具体方法一致）；未知则回落接口声明
  类型。三处分派点统一改用。
- **验证**：`tests/@test/assoc_string_dispatch.myp`（回归，string+int 双关联类型 +
  多方法接口动态分派，4 断言）；原 `tests/bugs/assoc_string_dispatch.myp` 移除。
  `Processor<T where T:Container>` 泛型单态化路径（静态直接调用）本就不受影响。
- **备注**：manual.md §6 关联类型按设计意图描述（不标 bug，符合约定）。

---

## BUG-018（已修复）：`import collections` + 带关联类型约束的泛型类 → 伪错误

- **状态**：🟩 已修复（2026-08-18）
- **根因**：`src/sema/sema.cpp` `visitClassDecl` 把类**通用类型参数**在
  `symbol_table_.enterScope()` **之前**（全局作用域）声明——类作用域弹出后 T 仍残留
  全局符号表。随后另一个用同名类型参数的泛型类把全局 T 覆盖成自己的绑定：collections
  的 `Set<T>`（无约束，T→Int）先注册，用户 `Processor<T where T:Container>` 把全局 T
  覆盖成 `Container`（接口）→ 之后检查 Set<T> 模板体（`val % cap_`、`data_[i] < x`）
  时 T 解析为 Container → `expected numeric type, got 'Container'`（8 个伪错误，行号
  落在 stdlib）。去掉 collections（无 Set<T>）或去掉约束（不覆盖 T）都不触发。
- **修复**：类型参数注册移到 `enterScope()` 之后（类作用域内），弹出即清除——同名
  类型参数不再跨类泄漏/覆盖。与 BUG-021（current_class_name_ 污染）同类——都是
  visitClassDecl 类上下文不隔离。
- **验证**：回归 `tests/@test/assoc_constraint_import.myp`（collections + `where
  T:Container` + `T::Item` + `Processor<IntBox>` 实例化，1 断言）；`tests/bugs/
  assoc_constraint_import.myp` 移除。全量回归 299 通过 / 0 失败。自举编译器天然无此
  bug（Pass A/B 作用域隔离），无需镜像。
- **备注**：manual.md §6 关联类型按设计意图描述（不标 bug）；`tests/@test/
  manual_ch6_class.myp` 与 collections 分开放的历史拆分可保留（无害）。

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

## BUG-021（已修复）：class 含泛型类属性时方法内 `this.prop` sema 解析污染

- **状态**：🟩 已修复（2026-08-18）
- **根因**：`src/sema/sema.cpp` `visitClassDecl`（泛型实例化入口）在函数内把
  `current_class_name_` 设为实例类名（如 `Option_int_inst`）**且退出时不恢复**。
  class H 含 `Option<int> o` 属性 → Pass 2 `buildCurrentClassMemberTypes(H)` 解析
  属性类型触发 `Option<int>` 实例化 → 进入 `visitClassDecl` → 退出后
  `current_class_name_` 残留 `Option_int_inst` → 之后 H 方法体 `this.v` 经
  `visitThisExpr` 返回 `Option_int_inst` → `class 'Option_int_inst' has no member 'v'`。
- **修复**：`visitClassDecl` 开头保存 `saved_current_class`、末尾恢复
  `current_class_name_`（类上下文不污染）。早期 return（duplicate class）在赋值
  之前，无需处理。
- **验证**：回归 `tests/@test/this_generic_prop.myp`（`Option<int>` + `ArrayList<int>`
  泛型属性 + `this.v` 读写 + 泛型属性方法调用，4 断言）；`tests/bugs/this_generic_prop.myp`
  移除。验证时顺带暴露 BUG-028（属性初始化器 ARC，见下）。
- **备注**：BUG-019 修复验证中发现；manual.md §7 `this` 按设计意图描述，不标 bug。

---

## BUG-028（已修复）：类属性带 ARC 初始化器 → 悬垂/双释放

- **状态**：🟩 已修复（2026-08-18）
- **复现**：`property: Foo f = new Foo();`（class/interface/string/slice/数组类型属性
  带初始化器）+ 构造后读取 `this.f.v()` 或 setter 重赋值 `setF(new Foo())` →
  读取 use-after-free（内存未重用时碰巧通过）、setter 重赋值双释放 → 运行段错误 139
  （确定性）。
- **根因**：`src/codegen/codegen_expr.cpp` 属性默认值发射（`new` 语句内对每个带
  `init_expr` 的属性 `generateExpr` + 直接 `CreateStore(gep)`）。fresh `new Foo()`
  的 rc=1 经 `arcPushTemp` 进语句末临时释放列表，但 store 后**未 `arcConsumeTemp`**
  → 语句末 `myp_release` 把对象释放（rc 1→0）→ 属性槽悬垂。对比：`this.prop =
  value` 赋值路径正确做 `arcStoreRef + arcConsumeTemp`。
- **修复**：属性初始化器发射与赋值路径同语义——ARC 引用属性（class/interface）
  `arcStoreRef(gep, v, iface, fresh)`、string `arcStoreRef(...,false,fresh)`、slice
  `arcStoreSlice`、counted-array `arcStoreRef`，均 + `arcConsumeTemp(v)`；alias 值
  retain、fresh 值 consume（`isFreshArcExpr`）。
- **自举镜像**：`tools/selfhost/src/codegen.myp` 属性默认值发射同样直接 store、未
  consumeTemp（IR 复核：fresh new 的 rc=1 在语句末 `myp_release` → 属性槽悬垂）→
  同样修复：`ft=="ptr" && isArcType` 时走 `storeRef(gep, pv, isFreshTemp(pv))`
  （内部 retain/consume），否则保持 plain store。IR 复核：属性对象不再语句末释放。
- **验证**：回归 `tests/@test/property_init_arc.myp`（初始化器对象存活 + 多次重赋值
  读取，3 断言）；`tests/@test/this_generic_prop.myp` 泛型属性带初始化器场景同覆盖。
  自举 `test_myp_self.sh` 94/94 全绿。全量回归 296 通过 / 0 失败。
- **备注**：BUG-021 修复验证时暴露（属性初始化器此前未在编译通过的用例中出现）。

---

## BUG-022（已修复）：`@thread` 用于 struct 实例被静默接受（应拒绝却接受）

- **状态**：� 已修复（2026-08-18）
- **复现测试**：`tests/negative/struct_thread.myp`（`S s @thread;` 编译拒绝）
- **修复**：`src/sema/sema.cpp visitVarDecl` 在类型完整解析后校验——`@thread` 仅可
  用于 class 实例，struct 报 `'@thread' can only be applied to a class instance
  variable`；自举 `tools/selfhost/src/sema.myp` 同镜像。
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

## BUG-023（已修复）：`@parallel for` / `@gpu for` 并行体直接访问 class/static 属性数组

- **状态**：🟩 已修复（2026-08-18）
- **根因**：`src/codegen/codegen_gpu.cpp` `emitKernelExpr` 的 MemberAccess 静态属性
  分支要求类名在 `kernel_vars`（并行体只捕获外层局部变量）→ `@static class X` 的
  `X` 不在其中 → 落到 `i64 0` 占位 → 下标 `X.arr[i]` GEP 基址为整数 0
  （`getelementptr i32, i64 0, %0`）→ LLVM verify 失败；`Atomic.addInt(X.sum,...)`
  传 0 占位当数组指针 → 运行段错误 139。
- **修复**：MemberAccess 静态属性分支直接以模块全局 `__myp_static_<Class>` 为基址
  GEP 进属性槽（CPU `@parallel` 同模块可直取全局）；`@gpu` 核函数（独立 PTX 模块）
  仍走捕获的 kernel arg（`kernel_vars` 命中时优先用 arg）。
- **验证**：回归 `tests/@test/parallel_prop_access.myp`（静态属性数组写 + 读 +
  `Atomic.addInt` 原子累加，4 断言，3 次运行稳定）；`tests/bugs/parallel_prop_access.myp`
  移除。全量回归 298 通过 / 0 失败。
- **备注**：manual §9 @parallel/@gpu 限制说明保持（并行体捕获局部变量 + 属性访问
  先拷局部的模式仍是最佳实践）；此修复让直接访问静态属性数组成为可能。

---

## BUG-024（已修复）：相对路径导入去重不解析 `..` → 同文件不同相对路径重复载入

- **状态**：🟩 已修复（2026-08-18 修复）
- **复现测试**：回归 `tests/@test/relimport_dedup.myp`（+ helpers/b24_helper.myp +
  relimport_sub/sub.myp；直导 `./helpers/b24_helper.myp` + 子模块 `../helpers/b24_helper.myp`
  同一文件 → 只加载一次、2 断言通过）。原 `tests/bugs/relimport_dedup.myp` 复现已移除。
- **现象**（实测 2026-08-18）：同一文件经**不同相对路径**导入不去重：
  - main 直导 `./helper.myp`（→ `/mod/helper.myp`）+ 递归导入 `./sub/sub.myp`（内部
    `../helper.myp` → `/mod/sub/../helper.myp`）→ `duplicate class name` /
    `duplicate function name`。
  - **同串** `import "./helper.myp";` 两次 → 正常去重（仅 1 次载入）。
- **根因**（`src/main.cpp` normalizePath ~59）：去重键 `dedup_key = is_path ?
  normalizePath(path) : module_name`；`normalizePath` 只移除开头 `./`、`/./`、`//`，
  **不解析 `..`**——`/mod/helper.myp` 与 `/mod/sub/../helper.myp` 规范化后仍不同 →
  双双载入 → duplicate。
- **修复**：重写 `normalizePath` 为**词法组件解析**——按 `/` 分段，`.`/空段跳过，`..`
  弹栈折叠（相对路径保留前导 `..`；绝对路径根 `..` 丢弃），`//` 自然合并。同一文件
  无论经哪条相对路径都归一到同一规范键。
- **验证**：`tests/@test/relimport_dedup.myp` 2 断言通过；全量回归 293 通过 / 0 失败。
- **备注**：design §9「规范化路径去重」修复后成立；manual §10 去重注记已同步（2026-08-18）。

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

---

## BUG-029（已修复）：类字段直接转 interface → 坏胖指针（vtable 丢失）→ 段错误

- **状态**：🟩 已修复（2026-08-18）
- **复现/回归**：`tests/bugs/iface_field_conversion.myp`（裸名 `c` + `this.c` 两形态，
  2 断言）。修复前 RED（`free(): invalid pointer` / 段错误 139），修复后 GREEN。
- **现象**：`View v = <类字段>`（如 `Shape s = c;`，`c` 是 `Circle` 属性）编译通过，
  但运行时接口分派/ARC 释放崩溃。对比：`View v = <局部类变量>`、`View v = new Circle()`
  均正常。
- **根因**：`src/codegen/codegen_stmt.cpp` 接口变量分支（`View v = init`）中
  "从既有具体变量赋值（`IFoo f = impl;`）"路径的 `cls_name` 解析只处理
  `d.init_expr->kind == ExprKind::Identifier` 且仅查 `var_class_map_`（codegen 只登记
  **局部变量**）→ 类字段（`this.c` / 裸 `c`）查不到 → `cls_name` 空 → 走 else 分支只
  `CreateStore(inst, data)`（vtable 槽零初始化）→ 坏胖指针 → 派发段错误。
- **修复**：else 分支后新增 cls_name 兜底解析——（a）按当前类属性表（`curClass` 的
  properties）解析字段具体类名（覆盖裸名 / `this.field`），（b）兜底用 sema 解析的
  `expr->type` 类名；均含泛型 mangling（`Box<int>` → `Box_int_inst`，与局部变量分支
  一致）。解析成功即构建 vtable（IR 复核：`store ptr @__myp_vtable_Shape_Circle`）。
- **自举镜像**：`tools/selfhost/src/codegen.myp` 的 `upcastIface` + 接口变量分支同样
  只从 `varAstType`（局部/参数）解析——已补 `propAstType(curClass_, name)` 字段兜底
  （见下）。
- **验证**：`tests/bugs/run_bugs.sh` 6/6 全绿；全量回归 311 通过 / 0 失败；MOS uikit
  原绕法（`Label local = <字段>; View v = local;`）现可逐步简化为直接字段→接口。
- **备注**：uikit 的"接口数组元素 + 接口形参"崩溃经验证**非独立 bug**（最小复现
  exit 0），实为 BUG-029 污染的坏胖指针所致。

---

## BUG-030（已修复）：mapping 事件在目标类构造器内触发 → 段错误

- **状态**：🟩 已修复（2026-08-18）
- **复现/回归**：`tests/bugs/mapping_ctor_self.myp`（ctor 内 2 次触发，1 断言）。
  修复前 RED（Segmentation fault 139），修复后 GREEN。
- **现象**：`mapping() { Button.Clicked -> App.onOk; }`，`App` 构造器内 `b.press()`
  触发 `Clicked` → 运行段错误 139。构造完成后触发正常。
- **根因**：mapping 分发依赖 `__myp_inst_<类名>` 全局找目标实例；该全局由调用侧
  `generateVarDecl` 在 `new App()` 返回后写入，**目标类构造器执行期间尚未注册** →
  分发读空/旧指针 → 段错误。
- **修复**：`src/codegen/codegen_class.cpp generateClassAction` 构造器入口
  （`action.has_constructor`）把 `this`（`func->getArg(0)`）写入
  `class_instance_globals_[cls.name]`（`__myp_inst_<Class>`，mapping 预填保证存在）→
  构造器内触发的 mapping 事件派发到当前实例。
- **自举镜像**：`tools/selfhost/src/codegen.myp` 构造器生成入口同样注册 `this`
  （见下）。
- **验证**：`tests/bugs/run_bugs.sh` 6/6 全绿；全量回归 311 通过 / 0 失败。

---

## BUG-031（已修复 ✅）：跨线程多 @thread 目标事件无限重投

- **状态**：🟩 已修复（2026-08-18 发现，应用壳阶段 3 触发；同日修复）
- **复现**：`tests/bugs/cross_thread_multi_target.myp`（@test 断言：A/B 各收 1 次，
  修复前各 5 万+ 次 → 断言失败；已接入 `tests/bugs/run_bugs.sh` 门禁）。
- **现象**：mapping 把同一事件路由到**多个 @thread 目标**（`Svc.Life ->
  AppA.onLife; Svc.Life -> AppB.onLife`，AppA/AppB 各 @thread）时，事件被无限
  重复投递。对照组：
  - 非 @thread 多目标（同线程）：各收 1 次 ✅
  - 单 @thread 目标：正常 ✅（chain_demo logd→notify）
  - 同类多 @thread 实例 + 单 mapping：只派发到其中一个实例（非广播，但不重复）
- **根因**（TRACE_ENABLED 诊断确认）：handler 注册 `instance=NULL` →
  `myp_event_dispatch` 第一遍按 handler 归属 route 副本（routed=1）后，第二遍
  跑**所有**同 event 的 NULL-instance handler（无归属 → 当前线程跑）——副本在
  目标线程 dispatch 时把**其他目标**的 handler 也跑了，每个 handler 内部
  BUG-005 的 `myp_thread_is_current(inst)==0 → myp_event_route_to_instance`
  检查又把事件 route 回其他目标线程 → 无限乒乓（route(instance) 8.7 万+ 次）。
- **修复**：
  1. `src/codegen/codegen_class.cpp`：mapping handler 注册 `instance` 从 NULL 改
     为**目标实例全局地址** `&__myp_inst_X`（传地址非值——注册发生在
     `__myp_init`，早于实例 new）。仅单目标普通目标链生效，lambda/transformer/
     函数目标或无类级实例全局回落 NULL（原行为）。
  2. `src/runtime/runtime.c`：`myp_event_dispatch` 用 `myp_handler_target()`
     （若注册 instance 为全局地址则解引用得实例）按线程归属路由——routed
     副本只跑归属本线程的 handler，跨线程多目标各收 1 次。
  3. `tools/selfhost/src/codegen.myp`：镜像注册 instance 逻辑（`regInst`）。
- **验证**：`tests/bugs/run_bugs.sh` 7/7 全绿（含新 `cross_thread_multi_target`）；
  全量回归 311 通过 / 0 失败；selfhost `test_myp_self.sh` 94/94 + bootstrap 16/16
  （不动点 myp_self2 == myp_self3 字节相同）。
- **影响解除**：MYP 跨线程事件广播（1 事件 → 多 @thread 目标）可用；MOS 应用壳
  （`app_lifecycle_demo`）不再需要非 @thread 广播规避（可改用 @thread 应用）。

---

## BUG-032（已修复 ✅）：`this` 作为值被传 alloca 地址（含 event 类共存时暴露）

- **状态**：🟩 已修复（2026-08-18 发现，2026-08-18 修复；MOS 跨进程应用壳暴露）
- **复现**：`tests/bugs/b032_event_class_inst_store.myp`（@test 断言
  `Holder.get().getCount()==0`；修复前断言失败读到垃圾 49152 或运行崩溃，
  run_bugs.sh RED → 修复后 GREEN）
- **现象**：同编译单元含 `event:` 声明的类（如 `AppManager`）时，**无 event 类
  实例经 `@static` 类属性 / 类实例属性存储 this** 后，`get()`/属性读取**字段
  错位**——`AppManagerBus.set(this)` 后 `Holder.get()` 返回实例的 `count_`
  读到 `0xC000`(49152)（正常 0）；事件循环中回调 `onControl` 的 `this` 变
  `0x100000000` → `find()` 里 strcmp 野指针段错误。
- **根因（最终定位）**：不是 event 类布局污染。MYP codegen `generateThisExpr`
  返回 `getNamedValue("this")`（**alloca 地址**）而未 load 实例值。当 `this`
  作为**值**使用（实参/赋值/返回，如 `Holder.set(this)`、`@static` 属性存储）
  时，把 **&栈槽** 传过去——`set` 把地址存进属性，`get()` 把地址当实例 →
  字段错位。证据：`--emit-llvm` 显示含/不含 event 两个版本 IR 完全相同
  （`call void @Holder_set(ptr %this)` 均传 alloca 地址）；-O0 objdump 见
  `lea 0x20(%rsp), %rdi`（取地址）后 `call Holder_set`。无 event 版仅因栈布局
  碰巧 &栈槽+16 为 0 未暴露，是**同一潜在 bug**（`/tmp/me_no.myp` 同错）。
- **修复**：C++ `src/codegen/codegen_expr.cpp` 的 `generateThisExpr` 改为：
  `this` 槽为 alloca 时 `CreateLoad` 返回实例值（`this` 作为值场景用）；
  `this.field` / `this.method()` 的地址用途仍经 `getNamedValue` 直取（不受
  影响）。selfhost `tools/selfhost/src/codegen.myp` 的 `loadThis()` **本就**
  `load ptr, ptr %this.addr`（load 值）——C++ 修复后与 selfhost 行为一致，
  无需镜像。
- **验证**：b032 复现 GREEN（mypc + myp_self 均 `this=0 h=0`）；run_bugs.sh
  8/8；父仓库 311/311；MOS ctest 13/13（含 app_bus_demo）；test_myp_self 94/94。
- **规避（MOS 曾用，可保留）**：AppManagerBus self-contained——事件循环+广播
  内联，不把 this 存进 @static/实例属性。修复后此规避不再必要，可回退为
  直接 `set(this)` 存储。
- **教训**：`this` 在 C++ codegen 中既是地址用途（成员访问/方法接收者）又是值
  用途（实参/赋值/返回），generateThisExpr 必须返回**值**；地址用途由
  generateMemberAccess 等经 getNamedValue 直取。

---

## BUG-033（已修复 ✅）：数组元素 → interface → 坏胖指针（vtable 丢失）

- **状态**：🟩 已修复（2026-08-18 发现+修复；MOS 桌面壳 launcher_demo 暴露）
- **复现**：`tests/bugs/b033_iface_array_elem.myp`（@test 断言，3 断言：类属性
  数组元素 0/1 + 局部数组元素；双编译器 mypc + myp_self）。修复前 RED
  （Segmentation fault 139），修复后 GREEN。
- **现象**：`View v = arr[i]`（`arr` 为**类属性数组**，如 `Button[] appBtns`
  的 `appBtns[i]`）编译通过，但运行时接口分派读 null vtable → 段错误 139。
  MOS 桌面壳 `launcher_demo` 里 `View vb = appBtns[i]; root.add(vb)` 后
  `root.onTouch` 遍历 children 调 `child.hit()` → `call *0x8(vtable)` 崩
  （gdb：`rbp=0x0`，第 9 个 child 的 vtable 槽零）。局部变量数组
  `Circle[] arr` 的 `arr[i]` 正常。
- **根因**：BUG-029 只修了接口变量分支的类名解析——(a) `Identifier`（局部
  var_class_map_）+ (a) `MemberAccess`（this.field）+ 局部变量数组
  （array_elem_class_map_）。**类属性数组元素**（`appBtns[i]`，SubscriptExpr
  的 array 是类属性名）不在覆盖内 → `cls_name` 空 → else 分支只
  `CreateStore(inst, data)`、vtable 槽零初始化 → 坏胖指针。
- **修复**：
  - C++ `src/codegen/codegen_stmt.cpp` 接口变量分支兜底加 (a2) `SubscriptExpr`：
    数组名 → ①局部变量数组查 `array_elem_class_map_`（含泛型 mangling），
    ②**类属性数组**从当前类属性表解析 `p.type.element_type` 类名（含泛型
    mangling，`Box<int>[]` → `Box_int_inst`）。解析成功即构建 vtable
    （IR 复核：`store ptr @__myp_vtable_View_Button`）。
  - selfhost `tools/selfhost/src/codegen.myp` 镜像：`upcastClsName` 加
    `Subscript` 分支——数组名（Identifier / this.Member）→ 局部变量
    `varAstType` element / 类属性 `propAstType` element → `classInstName`
    （含泛型 mangling）。
- **验证**：b033 GREEN（mypc + myp_self）；单文件/多文件最小复现 O0+O2 均
  `onApp hit=1`；父仓库 311/311；run_bugs.sh 8/8（+b033 后 9 项预期）；
  test_myp_self 94/94；MOS launcher_demo 重建后逻辑全通（`launch app=Notes
  idx=0` / `settings theme=1`）。
- **教训**：接口 fat pointer 的类名解析覆盖了 new / 局部变量 / 字段 / 局部数组
  / 关联类型，但**类属性数组元素**（SubscriptExpr + 属性名）是 BUG-029 家族
  的又一条路径——凡是 `View v = <表达式>` 需具体类名的，都要按表达式形态完整
  解析（new / Identifier / MemberAccess / Subscript 的局部与属性两种数组）。

## BUG-034（已修复 ✅）：接口 fat pointer 构造在「函数返回 / 接口字段写 / 接口方法调用参数」三处缺失

- **状态**：🟩 已修复（2026-08-19 发现+修复；MOS UIX 声明式框架暴露）
- **复现**：`tests/bugs/b034_iface_fat_upcast.myp`（@test 断言，2 测试 4 断言：
  返回接口 / 接口字段写 + 接口方法调用参数；双编译器 mypc + myp_self）。
  修复前：mypc 编译 p1/p2/p3 段错误 139 或 LLVM verify "Function return type
  does not match operand type"；myp_self 编译 opt failed。
- **现象**（接口类型 = fat pointer `{ptr data, ptr vtable}`，具体类 = 裸 ptr）：
  - p1 `View f() { return new Label(); }`——返回 Label*(ptr) 与返回类型（接口
    fat {ptr,ptr}）不匹配 → 落到 struct-from-pointer 分支把 Label 对象内存当
    fat load → verify 失败/段错误。
  - p2 `last_ = l`（last_ 为**接口属性**）——arcStoreRef 只按 data 做
    retain/release，store 只存 data、vtable 槽留旧值 → 接口分派读垃圾 vtable。
  - p3 `parent.addChild(l)`（**接口方法调用**，addChild(View)）——interface
    dispatch 实参直接传裸 ptr，被调方按 {ptr,ptr} 解释 → 参数错位 → 段错误。
- **根因**：接口类型作为「函数返回值 / 类属性字段 / 接口方法调用实参」时，缺少
  「具体类 ptr → 接口 fat {data, vtable}」的上转构造。类方法调用参数 upcast 与
  接口变量/字段/数组元素转接口早已覆盖（BUG-033 的 buildInterfaceFat /
  upcastIface + 类名解析），但这三条路径漏接入。
- **修复**：
  - C++（`src/codegen/codegen_stmt.cpp` + `codegen_expr.cpp`）：
    - p1：`emitFunctionReturn` 在返回类型转换前，若 `current_ret_ti_.kind ==
      Interface` 且 ret_val 是 ptr，用 `buildInterfaceFat`（接口名取
      current_ret_ti_.class_name，具体类经 `resolveArgClassName(*src)`）包装成 fat。
    - p2：三处属性赋值（this.prop / static / obj.prop 的 `arcStoreRef(iface_prop)`
      分支）补 vtable——`buildInterfaceFat` 后 store 完整 fat（ARC 仍按 data）。
    - p3：新增 `upcastIfaceCallArgs(call_args, e, method)`——接口方法（vtable
      动态分派）的接口形参实参若为具体 ptr → buildInterfaceFat；接入 interface
      dispatch 三处（广义 / 接口变量 / Subscript 数组元素），param_types 同步用
      转换后类型。
  - selfhost（`tools/selfhost/src/codegen.myp`）镜像：
    - p1：函数头记录 `curRetIface_`（返回类型是接口时），`genReturnValue` 对
      vt=="ptr" 时 `upcastIface` 成 fat（fresh new/call 的原始 temp consume 转移）；
      返回语句 `{ptr,ptr}` 分支接口返回时按 New/Call/Lambda 判定 skip retain。
    - p3：`genIfaceCall` 参数循环加 `ifaceParamIfaceName` 判定接口形参 →
      upcastIface。
    - 附带修复（同一家族）：**本类接口属性字段方法调用**（`last_.width()`，裸名
      Identifier）`varAstType` 只查局部变量查不到属性 → 曾生成 `@<Iface>_<method>`
      直接调用；加 `propAstType` 兜底走 vtable（genIfaceCall）。`exprLlvmType`
      对接口字段未给 {ptr,ptr} 时用 `memberFieldAstType` 判定强制 vtable。
- **验证**：b034 GREEN（mypc + myp_self，4 断言）；run_bugs.sh 10/10；
  父仓库 311/311；selfhost 自举一致；最小复现 p1/p2/p3 均 w=6 正确。
- **教训**：接口 fat pointer 的上转构造（buildInterfaceFat / upcastIface）是
  统一模式，必须覆盖全部「具体 ptr 进入接口上下文」的路径——局部/字段/数组元素
  转换（BUG-029/033 已修）、**函数返回 / 属性字段写 / 接口方法调用实参**（本 bug）。
  C++ 与 selfhost 需同步镜像；selfhost 的接口对象方法调用（局部/属性/数组元素）
  都要能识别「接口值」并走 vtable 分派。

## BUG-035（已修复 ✅）：字符串拼接结果作为函数调用实参 → 每调用泄漏 1 个计数字符串

- **状态**：🟩 已修复（2026-08-19 发现+修复；mypview player 长时间播放 RSS 线性增长暴露）
- **复现**：`tests/bugs/b035_concat_arg_leak.myp`（@test 断言，3 测试 3 断言：拼接实参→
  字段赋值 / 拼接→局部 / 返回拼接；双编译器 mypc + myp_self 均通过）。
  泄漏量级 headless 复现（/usr/bin/time -v，200 万次循环）：
  `h.set(Fmt.i(i % 100) + "%")` → 修复前 Max RSS ≈ **95,852 KB**，修复后 ≈ **2,036 KB**。
- **现象**：字符串拼接 `myp_strcat` 返回 rc=1 的新字符串，作为**函数调用实参**传给
  形参后，调用方从未 release 它（Fmt.i 的中间结果被 release 了，唯独拼接结果漏掉）。
  每次调用泄漏 1 个计数字符串引用 → 60fps UI 播放循环（每帧 `label.setText(Fmt.i(x)+"%")`）
  实测约 2.2KB/s 线性增长，长时间运行内存膨胀。局部变量拼接/字段赋值不泄漏
  （store 路径 arcConsumeTemp 正确）；唯独「拼接 temp 作实参」路径缺失。
- **根因**：oracle `src/codegen/codegen_expr.cpp` 字符串拼接分支生成 `myp_strcat`
  调用后返回 `cat`，但**未把 cat 纳入 ARC pending temp**（未 `arcPushTemp(cat)`）。
  函数调用返回的 fresh 字符串（Fmt.i 等）会进 pending temp、语句末 flush release；
  拼接是内联调用，结果被漏掉。selfhost（`tools/selfhost/src/codegen.myp`）已有
  `addFreshTemp(t)` 正确处理——**oracle 落后于 selfhost**。
- **修复**（oracle，与 selfhost 对齐）：
  - `codegen_expr.cpp` 拼接分支 `return cat` 前 `arcPushTemp(cat)`——拼接结果进
    pending temp：被 store 时 arcConsumeTemp（不双释放），作实参/中间值时语句末
    flush release。
  - `codegen_stmt.cpp` `generateReturnStmt`（finally 分支 + 普通分支）的
    `arc_skip_retain_return_` 条件加 `|| isStringConcatExpr(*s.value)`——`return a+b`
    的拼接结果由调用方直接拥有（rc 转移），不再额外 retain（否则 pending flush 与
    retain 叠加成新泄漏）。
  - selfhost 无需改动（已有 addFreshTemp + return 拼接判定）。
- **验证**：b035 GREEN（mypc + myp_self，3 断言）；run_bugs.sh 11/11；父仓库 311/311；
  leak_play 复现 200 万次 95,852 KB → 2,036 KB（≈-98%）；selfhost 自举一致。
- **教训**：MYP 的 fresh 临时（new/call/concat 产生的 rc=1 对象）必须统一纳入
  ARC pending temp 管理：store 时 consume、语句末 flush。oracle 与 selfhost 任何
  一侧新增「产生 fresh 值」的路径（本 bug 的 concat）都必须同步注册 temp，否则
  作函数实参/中间表达式时泄漏。排查方向：内存随时间线性增长的 UI/循环，先二分
  「局部 vs 字段 vs 函数实参」路径，再对比 oracle 与 selfhost 的 temp 注册差异。

## BUG-036（已修复 🟩）：selfhost 接口/字符串借用引用被当 fresh 释放 → mypview 自举运行段错误

- **状态**：� 已修复（2026-08-19）。编译期 5 处 + 运行期借用释放 3 处 +
  接口数组元素 ARC 2 处（BUG-037/038）+ UTF-8 双重编码（BUG-039）全部修复。
  mypview 全集 myp_self 编译运行成功（69 行全绿）；bootstrap 16/16 +
  父级 312/312 + bugs 11/11 + mypview UIX/PIPE PASS 全绿。
- **背景**：目标「用 self 自举编译器编译 mypview」。编译期暴露 5 处接口 fat
  pointer 代码生成缺口（全部已修，bootstrap 16/16 + 父级 312/312 不破）：
  ① 接口 `==/!=` 生成 `icmp eq {ptr,ptr}` 非法 → 逐字段 extractvalue 比较
  （genStructEq，对齐 C++ oracle eqRec）；② 接口 `== null` 对 null extractvalue
  非法 → 只比较 data 指针；③ exprLlvmType 函数值调用返回类型缺分支 →
  varAstType(callee).funcReturn()；④ exprLlvmType 接口方法调用返回类型 →
  isInterfaceName(resolvedKind)→{ptr,ptr} 兜底 + ifaceAbstractRetAstType
  （接口抽象方法返回类型，如 ViewBuilder.create→View）；⑤ 发现 mypc 数组
  substring 缓冲覆盖 bug（`a[i]=Str.substring(...)` 后新字符串覆盖数组元素，
  mypc 编译的程序实测元素变 newTmp 名；myp_self 编译正常）→ genStructEq 规避
  （字段类型不存数组，每次 structFieldAt 重解析）。
- **运行期根因**：selfhost 对**方法/接口方法调用返回 ptr**（字符串属性、成员/
  数组元素引用 = 借用，调用方不拥有）无条件 `addFreshTemp` → 语句末
  `myp_release` 释放借用引用 → 悬垂/双释放段错误（myp_release 收到已释放对象
  或字符串内容 0x65646f6e20786975="uix node"）。C++ oracle 用 isFreshArcExpr
  （方法调用返回=false，借用不释放）。
- **已修复**（tools/selfhost/src/codegen.myp，3 处）：
  - genExpr Call 分支普通函数/方法调用返回 ptr：删 `addFreshTemp(t)`（labelAt
    返回 labels_[i] 数组成员被释放的根因）。
  - genIfaceCall direct/vtable 分支返回 ptr：删 `addFreshTemp(t2/t)`（vm.getProp
    返回 bag 属性字符串被释放）。
- **待续**：（已由 BUG-037/038 解决）UixLoader_sync 内 `nodes_[ni].setAttr
  (bindProps_[i], val)` 接口方法调用后 release val 崩——根因是接口数组元素
  store 不 retain 借用 fat（BUG-038）→ 对象提前释放 → 内存被 myp_strcat 复用。
- **复现**：`myp_self` 编译 mypview 全集（uix_logic）→ 运行段错误 139；
  最小复现 /tmp/icmp_test.myp（接口==）、/tmp/arr_test*.myp（mypc substring 污染）、
  /tmp/utf8_min.myp（接口数组元素赋值，复现 BUG-037 编译期类型错误 + BUG-038 悬垂）。
- **教训**：MYP 借用引用（方法返回成员/属性/数组元素）调用方不拥有，不得
  addFreshTemp 释放；fresh 仅限 new/concat 等确实返回新对象的路径。selfhost 与
  oracle 的「fresh 判定」必须一致（isFreshArcExpr vs addFreshTemp）。
  接口数组元素 store：具体类 new → fat 上转 + fresh 转移；借用 fat → retain data
  （数组槽持有，否则调用方局部释放 → 悬垂）。

## BUG-037（已修复 🟩）：selfhost 接口数组元素赋值缺 fat 上转 + fresh 转移

- **症状**：`nodes_[i] = new Label(...)`（接口数组元素 = 具体类 new）时，LLVM
  报 `'%t55' defined with type 'ptr' but expected '{ ptr, ptr }'`（编译期）；
  修类型错误后运行期对象提前释放（缺 fresh 转移）。
- **根因**（tools/selfhost/src/codegen.myp Subscript 左值分支）：`elemLt ==
  "{ ptr, ptr }"`（接口数组元素）时直接 `store {ptr,ptr} rv`，未像 Member 分支
  那样调用 upcastIface/buildIfaceFat 把具体类裸 ptr 上转为 {data, vtable}；且
  上转后未消费 new 的 fresh temp → 语句末 flushTemps 双重释放。
- **修复**：新增 subscriptElemIfaceName(arr)（数组元素接口名解析，镜像
  subscriptElemLt 形态）；Subscript 分支 `elemLt=="{ptr,ptr}" && rt=="ptr"`
  → buildIfaceFat + `isFreshTemp(rawV)!=0 → consumeTemp(rawV)`（对齐局部接口
  变量 6627 的 arcConsumeTemp）。
- **验证**：最小复现 /tmp/utf8_min.myp 编译通过 + 运行正常；mypview 全集
  myp_self 编译通过。

## BUG-038（已修复 🟩）：selfhost 接口数组元素 store 借用 fat 不 retain → 对象悬垂

- **症状**：myp_self 编译的 mypview 运行段错误 139。gdb：sync 的 setAttr 参数
  全部正确（this/name/val），但 this 对象 text_ 槽 = `0x65646f6e20786975`
  （"uix node" 字符串内容）；watchpoint 捕获对象被 `myp_strcat`（walkParents
  拼接 "children"）写入 → **对象内存被字符串拼接复用**。
- **根因**：build 里 `Label l = new Label(...)`（rc=1 局部），registerNode 的
  `nodes_[nodeCount_] = v`（v 为接口 fat 借用）→ Subscript 分支对
  `rt=="{ptr,ptr}"` 直接 store 不 retain；build 返回时 l 释放 → rc 0 → 对象
  释放 → nodes_ 数组悬垂 → 内存被 myp_alloc/myp_strcat 复用。
- **修复**（codegen.myp Subscript 分支新增 else-if）：`elemLt=="{ptr,ptr}" &&
  rt=="{ptr,ptr}"`（接口 fat 借用）→ extractvalue 0 + `myp_retain(data)`
  （数组槽持有借用 fat，对齐 C++ 接口数组 store 的 retain 语义）。
- **验证**：mypview 全集运行全绿（uix style ok=7aff、sync status=ok-login
  等 69 行）；bootstrap 16/16 + 父级 312/312 + bugs 11/11。

## BUG-039（已修复 🟩）：selfhost 词法器 UTF-8 双重编码 → 中文乱码

- **症状**：myp_self 编译含中文字符串字面量的程序，输出乱码
  （`你好` → `ä½ å¥½`），`Str.len` 返回字节数翻倍（6 → 12）。mypc 正常。
- **根因**（tools/selfhost/src/lexer.myp scanString）：非转义字符
  `val.append(__myp_chr(advance()))` —— `advance()` 返回源码**原始字节**
  （UTF-8 多字节的一部分，如 0xE4），`__myp_chr` 把字节当 **Unicode 码点**
  再 UTF-8 编码（0xE4 → C3 A4）→ 双重编码。IR 里 `你好` 变
  `c"\C3\A4\C2\BD\C2\A0..."`（9 字节）。
- **修复**：scanString 非 ASCII 字节（>=128）改用 `Str.substring(source_,
  pos_-1, pos_)` 原样保留源码字节；ASCII 仍 `__myp_chr`。
- **验证**：最小测试 `你好 len=6`；mypview 全集中文正常（`t=你好 n=6`）；
  bootstrap 16/16 固定点不破（selfhost 源码字符串多为 ASCII）。
- **教训**：源码按字节读，`__myp_chr` 按码点编码——两者只对 ASCII 等价；
  字节级 round-trip 须用 substring/memcpy 原样保留（同 stdlib/io.myp readAll
  注释）。

## BUG-040（已修复 🟩）：selfhost 接口局部变量初始化借用 fat 不 retain → draw 崩溃

- **症状**：myp_self 编译的 mypview **真实 SDL 绘制**示例 `examples/player.myp`
  运行段错误 139——`LinearLayout_draw` 里 `View kid = kids_[i]` 后 `kid.draw(r)`
  崩溃（`call *0x30(%rbx)` 中 rbx=0，或 this 对象 kids_ 字段 = 垃圾）。
  此前 uix_logic headless 测试 `.draw(` 调用计数为 **0**，从未覆盖接口数组
  元素 → 局部接口变量 → 方法调用这条路径。
- **根因**（tools/selfhost/src/codegen.myp 局部接口变量初始化 6602-6638）：
  `View kid = kids_[i]`（接口数组元素**借用 fat**）走 `it=="{ptr,ptr}"` 分支
  直接 `store {ptr,ptr} iv, va` **不 retain**；而局部接口变量是 arcSlot，
  作用域末 `releaseArcSlots` 对接口槽执行 `load data + myp_release` → **释放
  借用** → kids_ 悬垂/对象被释放 → draw 崩溃。`buildIfaceFat` 分支（借用具体类
  实例，如局部对象变量）同样缺 retain。
- **修复**：局部接口变量初始化两个分支对**借用**（`isFreshTemp(iv)==0`）在
  store 前 `extractvalue 0 + myp_retain(data)`（局部持有，作用域末释放配对）；
  fresh（new）保持 `consumeTemp` 转移所有权不变。
- **附带修复**：`mypview/examples/build.sh` 固定文件列表过时（缺
  sortable_list/long_press_button/gesture/theme/dialog 等，player 的
  SortableList/LongPressButton/GestureDetector 报 unknown type）→ 改用目录
  通配符（`$SRC/core/*.myp` 等），新增文件自动包含；mypc 与 myp_self 均验证。
- **验证**：player 120 帧全绿（`frame=120 list=8 q=低 vol=5`、ttf-cache
  hits=6310）；mypc + myp_self 编译运行均 OK；bootstrap 16/16、父级 312/312、
  bugs 11/11、mypview UIX/PIPE PASS。
- **教训**：接口 fat 的「借用 vs fresh」判定必须贯穿所有路径——局部接口变量、
  接口数组元素 store、接口方法调用参数/返回、赋值 RHS。凡「借用 fat 被某个
  ARC 槽持有」，作用域末 release 就必须先 retain（配对）；只有 new/fresh 才
  转移。测试应覆盖真实绘制（headless 只测布局不测 draw 会漏这类 ARC 缺口）。

## BUG-041（已修复 🟩）：多文件编译对文件顺序敏感（mypc 依赖顺序 / myp_self 混合路径丢 main）

- **症状**：同一源码集合仅**文件顺序不同**，mypc 编译结果一个正常一个崩溃：
  - run.sh 手工顺序（55 文件）→ 运行 69 行全绿
  - 字母序 `sort`（同样 55 文件）→ 运行段错误 139，0 行输出
  - 崩溃点：`ConstraintLayout_layout`（被 UixLoader_buildInto 调用）；LLVM verify
    偶报 `renderer.myp: Call parameter type does not match function signature`。
  - 通配符 `src/core/*.myp ...` 按字母序展开，故 mypc 用通配符编译 mypview 全集
    会崩溃；myp_self 用同样字母序通配符**运行正常**（顺序无关，更健壮）。
- **根本原因**（src/codegen/codegen_expr.cpp callee 选择）：`cols_[i].layout()`
  的对象是**类属性数组元素**，codegen 用 `memberObjectClassName`（查
  `array_elem_class_map_`，但该 map **只记录局部变量数组**，类属性数组缺失）
  → 返回空 → **fallback 按类注册顺序遍历找第一个同名方法** `layout` → 字母序下
  ConstraintLayout 先注册 → 错调 `ConstraintLayout_layout`（参数错 → 对象
  ARC/字段错乱 → 崩溃）。sema 已解析的 `resolved_object_class`（静态元素类型
  `LinearLayout`，**与文件顺序无关**）未用于 callee 选择。
- **修复**（codegen_expr.cpp 两处 callee 选择 + fallback）：优先用
  `ma.resolved_object_class`（sema 静态类型），为空才 fallback 到
  `memberObjectClassName`。`cols_[i].layout()` 现在稳定解析到 LinearLayout。
- **验证**：字母序 mypc 编译运行 uix_logic 69 行（原崩溃）；字母序全量 69 行；
  run.sh UIX/PIPE PASS；父级 312/312、bugs 11/11、bootstrap 16/16。
- **备注**：build.sh 仍保留双分支（mypc 依赖顺序列表可撤销但无害 / myp_self
  通配符+相对路径）；myp_self 混合路径丢 main 是独立问题，见 BUG-041b。

## BUG-041b（已修复 🟩）：myp_self 对「绝对路径源码 + 相对 target」混合路径丢 main

- **症状**：build.sh 用 `$DIR/../src`（绝对路径）编译时，myp_self 报
  `undefined reference to 'main'`；全相对路径正常。
- **根因**：myp_self 对绝对/相对混合路径的 target（main 所在文件）处理错，
  未生成/链接 main。
- **修复**（build.sh）：源码/标准库一律用相对路径（`cd $DIR` 后稳定）。
- **验证**：myp_self 编译 player/counter 全绿。

## BUG-042（已修复 🟩）：myp_self 把内部析构/协程入口生成为 global 符号

- **症状**：nm 对比，myp_self 二进制 130 个全局函数 T（含 85 个 `__myp_destroy_*`
  + 4 个 `__myp_coro_*`）；mypc 对应为 **local（t）**（74 个 destroy + 2 个 coro）。
- **根因**：selfhost codegen 生成 `__myp_destroy_<Class>` 析构和 `__myp_coro_*`
  协程入口时未标 internal（`define internal`）。
- **实际影响**：小。`.dynsym`/`.dynstr` 与 mypc 相同（58 个动态符号，不导出）；
  strip 后无影响。仅理论风险：未来多编译单元/静态库链接时同名符号冲突、以及
  链接器 `--gc-sections` 无法裁剪未用函数。
- **修复方向**：codegen.myp 生成这两类函数时加 `internal`（与 mypc 对齐）。

## BUG-043（待优化 ⏳）：mypc 接口上转每次生成新 vtable 副本（149 vs 15 个）

- **症状**：对比 mypc 与 myp_self 编译同一 mypview 源码集的二进制，mypc
  大 ~79KB。段级定位：`.text` 主代码几乎相同（138KB vs 137KB），真正差异在
  **`.data.rel.ro`（vtable 表）**——mypc 21.6KB vs myp_self 2.6KB（+19KB）。
- **根因**：mypc 每次做「接口上转」（具体类实例 → 接口 fat，如 `new Button()`
  传给 `View` 参数 / 存进接口数组）时**生成一个新的 vtable 常量**（带递增后缀
  `__myp_vtable_View_Button.1010/.1011/.1012...` 去重），共 **149 个**（15 基础
  + 134 副本，约 17KB）；myp_self 复用每个接口实现类 **1 个** vtable（15 个）。
  副本内容完全相同、各被一个 upcast 点引用（非死代码）→ 冗余分配。
- **实际影响**：仅二进制体积（~17KB/示例）；功能完全等价（both 模式输出一致）。
- **修复方向（未做，暂缓）**：mypc 复用/合并 vtable（按「接口名_类名」去重，
  对齐 myp_self），可省 ~17KB。需改 src/codegen 的 vtable 生成/引用逻辑。
- **验证**：`nm -S` 统计 vtable；`objdump -h` 看 .data.rel.ro。

## BUG-044（已修复 🟩）：generateClassDefaultAction 漏设 current_ret_ti_ → 接口
默认实现 stub 用「残留返回类型」生成 myp_retain(i32) → LLVM verify 失败

- **症状**：把 mypview 作为包 `import mypview;`（聚合主模块相对路径递归加载
  控件）编译时，LLVM verify 失败 `Call parameter type does not match ... call
  void @myp_retain(i32 1)`。同一批文件**直接命令行多文件编译正常**。
- **根因**：`CodeGen::generateClassDefaultAction`（生成接口默认实现
  `__ifdef_View_<method>_<Class>` stub）**未设置 `current_ret_ti_`**（正常路径
  generateClassAction/generateClassFunction 均设置）。stub 内有 `return` 语句
  （如 `int enabled() { return 1; }`）时，emitFunctionReturn 用上一函数残留的
  current_ret_ti_ 判断是否需 ARC retain——残留类型为 string/Interface 时错误
  生成 `myp_retain(i32 1)`。之所以「直接编译正常 / import 聚合崩」：合并后类
  的 codegen 顺序不同，恰好让 stub 前一个函数是对象返回类型。
- **修复**：`generateClassDefaultAction` 补设
  `current_ret_ti_ = typeNodeToCodegenType(action.return_type)`，并与
  generateClassAction 对齐 `arc_skip_retain_return_ = false`、
  `arc_pending_temps_.clear()`。
- **验证**：mypc + myp_self 对 `import mypview;` 包消费者编译运行输出一致
  （`pkg button=Login label=hello num=50 slider=60`）；parent 312/312、
  bugs 11/11、mypview UIX/PIPE PASS。

## BUG-045（已修复 🟩）：selfhost `@parallel for` 把 float[] 参数当 i32[] 处理 →
3D Conv3D/MaxPool3D/AveragePool3D 输出全 0（deeplearning 框架自举编译数值偏差）

- **症状**：自举编译器 `myp_self` 编译 `examples/deeplearning/infer_tests/` 的
  `conv3d_main`/`coarselike`/`coarselike32`/`pad3d_avgpool3d`，产物 MISMATCH：
  Conv3D 输出（`cv`）全 0 → InstanceNorm/ReLU/MaxPool 塌缩成常量（全 0.05）。
  oracle `mypc` 全 OK。输入 x / 权重 w3d / 偏置 b3d 与 oracle 逐元素一致（张量
  布局 LAYOUTS IDENTICAL），唯 conv 输出全 0。
- **根因**：`tools/selfhost/src/codegen.myp` `genParallelFor` 收集捕获时，**参数
  一律传 `elem=""`**（只有局部变量带元素类型）→ 并行 body 内 `float[] arena`
  参数被当 `i32[]`：GEP 用 i32 元素类型 + `store i32`（fptosi float→i32）。
  i32 1 = 0x00000001 当 float ≈ 1.4e-45（denormal）→ 浮点显示 0。3D 算子
  `InferOps.conv3d`/`maxpool3d`/`avgpool3d`（ops.myp）都用
  `@parallel for` 写 arena（float[] 参数）→ 全 0。（2D 算子 dense/relu 等
  不用 @parallel → 不受影响，故 2D/F8/BN/r18 自举结果正常。）
- **修复**：`genParallelFor` 参数捕获从 `paramAstTypes_.get(i)` 算元素类型：
  slice → `sliceElemType` + slice 标记；数组 → `pt.element() → llvmType`，
  与 `varElemType` / `isSliceVar` 对齐。
- **验证**：`tests/bugs/b045_parallel_float_array.myp`（4 断言）mypc + myp_self
  均 PASS；deeplearning infer_tests 全 18 入口（含 3D 四用例）自举产物与 oracle
  一致（conv3d/coarselike/coarselike32/pad3d_avgpool3d 全 OK）。

## BUG-046（已修复 🟩）：类内同名 static 方法（签名不同）无重复检测 → CodeGen 崩溃

- **症状**：类内两个同名 static 方法（不同签名）→ mypc 编译在 CodeGen 阶段崩溃：
  ```
  mypc: /usr/lib/llvm-21/include/llvm/IR/Function.h:885:
  llvm::Argument* llvm::Function::getArg(unsigned int) const:
  Assertion `i < NumArgs && "getArg() out of range!"' failed.
  ```
- **发现（2026-08-20，Qwen2-0.5B 导入时误判为 `mul` 特殊名）**：给
  `examples/deeplearning/infer/ops.myp` 加一个 5 参 `mul` 方法即崩溃——但那是**误导**：
  ops.myp **本来就有一个 8 参 `mul`**（F8 广播算子，`(arena,aOff,bOff,outOff,n,bSize,C,S)`），
  加 5 参的造成同名重复。最小复现（任意名字都崩）：
  ```
  class Dup { static:
      void foo(float[] a, int x) { }
      void foo(float[] a, int x, int y, int z) { }   // 同名不同签名
  }
  ```
- **根因（gdb 定位 + 读码确认）**：`src/sema/sema.cpp` visitClassDecl 的 static action
  注册：
  ```
  std::string static_name = cls_name + "." + action.name;
  symbol_table_.declare(static_name, func_type);   // 返回值未检查！
  ```
  **唯一漏查重复的方法类别**——properties（"duplicate member"）、actions（"duplicate
  action"）、functions（"duplicate function"）、events（"duplicate event"）都检查了
  `declare`/`lookup` 返回值，唯独 static actions 没查。同名 static 方法静默注册两次 →
  codegen `module_->getFunction("Class_foo")` 返回第一个定义创建的 LLVM 函数（5 参），
  `generateStaticAction` 却按第二个 action 的 8 参数迭代 `func->getArg(i)` → 越界。
- **修复**：`src/sema/sema.cpp` static action 注册处检查 `declare` 返回值，重复时报
  `duplicate static action '<name>' in class '<cls>'`（与其它方法类一致）。
- **验证**：负测试 `tests/negative/duplicate_static_action.myp`（编译拒绝）；
  干净 ops.myp（单一 8 参 mul）编译无回归。
- **备注**：曾经的「import ops.myp + 调 Math.cos 崩溃」也是本 bug 的下游症状
  （当时 ops.myp 里带着重复 mul）。Qwen2 已采用的规避（silu/mul 定义在各 .myp 类内
  私有方法）仍成立，且现在若真往 ops.myp 加同名方法会得到清晰报错。

---

## BUG-047（已修复 🟩）：selfhost ARC 调用返回值双重 retain 泄漏

- **状态**：🟩 已修复（2026-08-24）
- **复现/回归**：parity arc / arc_m2 / weak_cycle（`tests/parity_matrix.sh`），0 差距
- **现象**：selfhost 编译器产物每次方法/接口方法调用返回 ptr 泄漏 +1 引用，parity
  arc 系列计数不符；循环场景内存线性增长。
- **根因**（tools/selfhost/src/codegen.myp）：被调方 retain-at-return 已转移 +1，但
  BUG-036 曾误删 genExpr Call / genIfaceCall 的 `addFreshTemp(t)` → 调用方 store 点
  `isFreshTemp(v)==0` 再 retain 一次 → 每次调用返回泄漏 +1。
- **修复**：三处（genExpr Call、genIfaceCall direct/vtable）对 `retLt=="ptr"` 恢复
  addFreshTemp（对齐 C++ generateCall 的 arcPushTemp + isFreshArcExpr(Call)=fresh）。
- **验证**：arc/arc_m2/weak_cycle 全绿 + parity 0 差距 + bootstrap 16/16。
- **教训**：BUG-036「方法返回借用」的判断错误——MYP 所有 ptr 返回都 retain-at-return
  （+1），调用方必拥有；接口 fat 返回同理。

## BUG-048（已修复 🟩）：selfhost 闭源分发/链接缺口

- **状态**：🟩 已修复（2026-08-24）
- **复现/回归**：`tests/test_closed_lib.sh`（closed-lib 12/12）
- **现象**：selfhost 链接器无法消费预编译闭源库（.so/.a）：`cannot find import` /
  undefined symbol；无 `--shared` 库模式；链接成功不打印 "Link OK"（测试 grep 判据）。
- **根因**（tools/selfhost/src/link.myp + codegen.myp）：①link.myp 无预编译库发现；
  ②codegen 无库模式导出符号；③无成功打印。
- **修复**：三处——①link.myp 增 listLibFiles + nmDynSymbols（nm -D，strip 过的 .so
  导出符号在 .dynsym）+ 固定点符号匹配直接链接；②codegen 库模式把 `define internal`
  →`define`（Str.replaceAll 后处理）导出符号、无 body 签名方法发 declare 而非空 stub
  （genStaticAction/genInstanceActionNamed 的 `a.body()==null` 分支 + genDeclare 助手）、
  link 用 `-shared -fPIC`；③链接成功补打印 "Link OK"。
- **验证**：closed-lib 12/12。
- **教训**：闭源分发 = 签名 .myp（无 body 发 declare）+ .so（库模式导出符号）+
  MYP_BRIDGES 自动链。

## BUG-049（已修复 🟩）：selfhost codegen `&&`/`||` 结果槽栈泄漏 → 无限循环 RSP 崩溃

- **状态**：🟩 已修复（2026-08-26）
- **复现/回归**：`bench/freestanding/rt_thread_test.myp`（3 并发子线程自旋 `while
  (f0==0 || f1==0 || f2==0)` 用 `||` 直写，8/8 稳定；修复前 `||`/`&&` 自旋必崩 139）
- **现象**：`while(1){ if (a && b && c) break; ... }` 类**无限循环**段错误 139——
  崩溃点 PC 落在循环体内 `call`（压栈时）/ `movb`（栈写时），gdb 反汇编见循环体内
  `mov %rsp,%rdx; lea -0x10(%rdx),%rax; mov %rax,%rsp` 每轮执行且无配对恢复 → 主栈
  8MB 约 26 万轮 RSP 跌破栈底。单条件自旋（`while (x != 0)`）无此问题（探针验证）。
- **根因**（tools/selfhost/src/codegen.myp `&&`/`||` 短路降级）：结果槽每次求值内联
  发射 `alloca i1`（唯一 tmp 名 `.res`）——在循环体内每轮执行，LLVM 对非 entry 块的
  alloca 生成动态栈增长指令不恢复。C++ oracle 的 `generateShortCircuitLogic` 用
  **PHI** 无 alloca，本就正确。
- **修复**：结果槽改 `entryAlloca("i1")`（提升到 entry 块、零初始化；`||` 默认 true
  显式 store 保留）。IR 复核：alloca 在 entry 块、循环体内仅普通 store。
- **附带发现（同一里程碑 #34）**：clone 子线程分支不能声明局部——子 RSP=新栈顶无
  prologue 帧，codegen 的 `0x28(%rsp)` 正偏移在栈顶之上越界 → 子分支只把 entry 经
  实参（rdi）传进 helper `myp_thread_child_entry` 建帧 + capture 后置 `done_read=1`
  确定性握手。
- **验证**：shadow 24/24（thread 8/8 稳定）；bootstrap 16/16（新 fixpoint `1e6d4f7`，
  编译器改）；全量 323/323。
- **教训**：自举 codegen 任何「每次求值分配的临时槽」都必须进 entryAllocas_（entry
  块 alloca），否则在循环体内泄漏 RSP；oracle 与 selfhost 的短路降级实现（PHI vs
  alloca）需保持行为等价。

## BUG-050（已修复 🟩）：裸 const 标识符不折叠 + selfhost `__myp_*` 盲目去前缀误伤

- **状态**：🟩 已修复（2026-08-26，runtime myp 化 #36 coro core 暴露）
- **复现/回归**：
  - 裸 const：`const int CAP = 1024; int x = CAP * 8;`（`CAP` 裸引用折叠为 `call i32
    @CAP()`）+ `const string A = "hi x"; ... A()`（显式调用仍按可调用函数）
  - `__myp_*` 去前缀：shadow `rt_coro_test`（`__myp_coro_resume` 直调，IR 须为
    `call i64 @__myp_coro_resume(i64, i64 0)` 全名 + i64 字面量）
- **现象**：
  1. 顶层 `const int CAP = 1024;` 被**两个编译器**解析为零参 const-decl **函数**
     （body `return N;`）——只有 `CAP()` 显式调用能折叠；裸引用 `CAP` 报
     selfhost `expected numeric type, got 'function'` / mypc `'() -> int'`。
  2. selfhost codegen.myp 通用 Identifier callee 路径有**一刀切**
     `Str.startsWith(fn,"__myp_")→substring(fn,2)` 去前缀——把**真** `__myp_coro_*`/
     `__myp_destroy_*` 符号去成 `myp_coro_resume`（undefined symbol），且去前缀后函数
     类型解析失败 → 字面量实参 `0` 不提升为 i64（`call @myp_coro_resume(i32 0)`）。
- **根因**：
  1. C++ oracle：`visitFuncDecl` 把所有顶层函数（含 const-decl）以 Function 类型注册进
     `symbol_table_`，`visitIdentifier` 查表返回函数类型 → 裸引用得不到值类型。selfhost
     同理（`isFuncName` → "function"）。
  2. selfhost 用**盲目去前缀**模拟 C++ oracle 的显式 `intrinsic_map_`（`__myp_charcode`
     → `myp_charcode`、print_*/math_*/io_*/strlen 等别名）——正确做法是**除真
     `__myp_*` 符号族外**才去前缀。
- **修复**：
  1. **裸 const 折叠（双编译器）**：sema 的 Identifier 分支——查到 const-decl 零参函数
     时把类型改判为其返回类型（`findConstDeclFunc` + `constRetValueClass`）；codegen 对
     const-decl 标识符发射**隐式零参调用** `call <retty> @CAP()`。C++ 侧 `visitIdentifier`
     + `generateIdentifier` 镜像（用 `is_const_decl` + 空参判断；codegen 遍历
     `current_tu_->functions` 找 const-decl）。**关键守卫**：`CAP()` 的 callee 不折叠——
     `visitCall` 解析 callee 时置 `in_call_callee_`，否则 `const string A; A()` 被误折叠成
     值类型 → `'A' is not callable`（const_string/eval 两测试曾编译失败）。
  2. **去前缀豁免**：codegen.myp de-prefix 排除 `__myp_coro_`/`__myp_destroy_` 前缀
     （`mapGpuIntrinsic` 已全覆盖 GPU 内建故不受影响）。
- **验证**：裸 const `/tmp/const_bare5.myp` 双编译 exit=0（IR `call i32 @CAP()` 两处）；
  shadow 25/25（rt_coro_test 直调 `__myp_coro_resume` exit=0）；bootstrap 16/16（新
  fixpoint `091d2204`，编译器改）；全量 323/323（含 const_string/eval 回归）。
- **教训**：C++ 用显式 `intrinsic_map_` 做 `__myp_*`→`myp_*` 别名、selfhost 用一刀切去
  前缀——中间路线是**按真符号族豁免**（coro/destroy），且编译器内建调用的字面量实参
  提升依赖「函数名能解析」这一前提，被去前缀破坏。

## BUG-051（已修复 🟩，v3.15.69）：@static 类属性默认值不生效（全局恒 zeroinitializer）

- **状态**：🟨 已定位待修（2026-08-25，runtime myp化 #38 协程 Phase C 暴露；
  编译器未修，运行时已规避）→ **🟩 已修复（2026-08-26，v3.15.69 selfhost
  codegen 显式常量初始化器）**。
- **复现**：`/tmp/static_default.myp`——`@static class S { property: int x = 5;
  int y = -1; int z = 0; }`；`int main(){ return (S.x-5)+(S.y+1); }`。
  双编译器、**非 shared 与 --shared 均**：IR `@__myp_static_S = global %S
  zeroinitializer`，运行读 S.x=0/S.y=0 → exit=252（= -4）。
- **现象**：@static 类属性带**非零默认值**（`= 5`/`= -1`）时静默变 0；`= 0` 恰好匹配
  zeroinit 不受影响。手写 `CoroT.current = -1`（协程表当前槽）从 0 起 → 恰等于首个
  协程槽号 → main（非协程）在 spawn 后被误判为「在协程 0」→ channel/wait 走协程
  分支（内联 resume 改变输出时序 / 误 park）。thread.myp `Thr.stackSize = 1048576`
  读出 0 被钳到 65536（栈过小）。
- **根因（双编译器一致）**：
  1. @static 实例全局创建处**无条件零初始化**：C++ `src/codegen/codegen.cpp:1549`
     `ConstantAggregateZero::get(st)`；selfhost `tools/selfhost/src/codegen.myp`
     `" = global %" + c.name() + " zeroinitializer\n"`。
  2. 属性默认值（`PropertyDecl.init_expr`）**只在 `new` 路径应用**：C++
     `src/codegen/codegen_expr.cpp:3560-3588`（generateNewExpr 逐属性 store）。
     @static 类 = 全局实例、从不 `new` → 默认值永不参与。
  3. 手册 `docs/manual.md:167` 明写「属性默认值在 new 时生效」→ 技术上文档化，但
     与手册同段推荐的「@static 类做类级共享常量」自相矛盾（`@static class C {
     int K=1024; }` 实际 K=0）。
- **修复（v3.15.69，仅 selfhost codegen，oracle 冻结不改）**：
  `emitStaticClassGlobals` 存在**可折叠非零默认值** → 发显式结构体常量初始化器
  `global %S { i32 5, i32 -1, ... }`；否则保持 zeroinitializer（零改动保护既有模块）。
  支持：字面量（int/float/bool/null/string）+ 一元负号 + Convert 透传 + 二元整数/
  浮点常量折叠（`constIntEval` 递归处理 `2*3+1` 嵌套）；数组/结构体/向量元素 →
  `ft zeroinitializer`；字符串默认 → `@str` 常量 GEP；double/float 十六进制位型。
- **运行时 workaround 已回滚（2026-08-26）**：coroEnsureInit 不再显式置
  `CoroT.current = -1`（靠初始值）；thread myp_thread_spawn 不再显式置
  `Thr.stackSize = 1048576`（靠初始值）。sync.myp 的 arena **表内容**清零保留
  （myp_arena_alloc 非零初始化，与默认值无关）。
- **回归**：`tests/@test/manual_static_defaults.myp`（7 断言）；全量 324/324、
  自举 16/16 不动点。
- **教训**：`--shared` 模式背锅了——实际是**任何模式**下 @static 全局都不应用默认值
  （之前记的「static-init 不适用于 --shared」是误判）；@static 类非零默认值必须
  运行时显式初始化或避免依赖。

## BUG-052（已修复 🟩，v3.15.85）：解析器错误恢复死循环 OOM（enum 变体 / match 臂不推进）

- **状态**：🟩 已修复（2026-08-26，v3.15.85，selfhost `tools/selfhost/src/parser.myp`）
- **复现**：任何含**非法枚举**或**非法 match** 的文件即触发。最小复现：
  - `enum E { V0, V1 }` —— MYP 枚举变体须 `;` 分隔（含末尾）：`enum E { V0; V1; }`；
    逗号是非法语法 → mypc 内存无限增长（实测 ~18GB，OOM 崩溃系统，被迫手动 kill）。
  - `match (e) { E => { ... } }` —— match 臂缺 `.` 与变体名（须 `E.V0 => { ... }`）。
- **根因**（selfhost parser.myp，错误恢复不保证推进——与 2026-08 LSP 内存爆炸同类，
  但当时只修了 `parseBlock`/`parseMapping`，`parseEnumDecl` 变体循环和
  `parseMatchStmt` 臂循环漏网）：
  1. `parseEnumDecl` 变体循环：`consume(";", ...)` / `parseIdentifier` 失败都
     **不 advance**。非法分隔符（逗号/漏分号）→ 循环卡在同一 token 无限转，每次
     迭代 `perr` 追加 Diag 对象 → 无界分配。
  2. `parseMatchStmt` 臂循环同缺陷：`parseIdentifier` + `consume("."/"=>"/"{")`
     失败不推进 → 非法模式（`E => {...}`）同样死转。
- **修复**：两循环加**推进保护** `int before = current_; ... if (current_ == before)
  advance();`（与顶层/mapping/class/block 循环同款）。已核查自举 parser 全部
  `while (curKind() != "}" ...)` 循环：顶层/class/mapping/block 有 guard；bitfield
  出错 break；struct 外层 else break——仅 enum/match 两处曾缺。
- **回归**：负测试 `tests/negative/enum_comma_separator.myp`、`tests/negative/
  match_missing_dot.myp`（现 7.5MB 有界报错，原 ~18GB OOM）；全量 329/0。
- **教训**：① MYP 枚举变体须 `;` 分隔（含最后一个）：`enum E { V0; V1; }`，
  `V0, V1` 是非法语法；② 枚举构造用变体字面量 `E.V0`（**无 `new E(index)`**）；
  ③ 编译压测必须 `ulimit -v`（`tests/torture/run_torture.sh` 已内置默认 8GB、
  负测试循环 1GB）——病态输入曾 18GB OOM 崩溃系统。

## BUG-053（已修复 🟩，v3.15.87）：`exprLlvmType` 对 `+` Binary 指数爆炸（巨表达式不可编译）

- **状态**：🟩 已修复（2026-08-26，v3.15.87，selfhost `tools/selfhost/src/codegen.myp`）
- **复现**：左深 `+` 链表达式（torture plus_bomb 暴露）：
  `long r = 0 + 1 + 1 + ... + 1;`（N 个加号）。gdb 热栈：`CodeGen_genExpr` →
  `CodeGen_exprLlvmType`（23 层递归）→ `IrEmit_intWidth` → `myp_str_eq`。
- **现象**：纯**时间**爆炸（内存 7-24MB 不变）：深度 20=0.5s、22=1.6s、24=6.2s、
  26>20s（指数 ~1.9x/+1）。10,000 加号 >120s 超时不可编译（用户要的"变态测试"
  直接打爆编译器）。
- **根因**（selfhost codegen `exprLlvmType` Binary `+` 分支）：
  ```myp
  if (op == "+") {
      ... if (exprLlvmType(lhs) == "ptr" || exprLlvmType(rhs) == "ptr") return "ptr";
  }
  ...（底部）...
  string llt = exprLlvmType(lhs);   // 又算一次！
  string rlt = exprLlvmType(rhs);   // 又算一次！
  ```
  `+` 分支为判字符串拼接（sema 未解析场景 LLVM 类型为 ptr）调用 `exprLlvmType(lhs/
  rhs)`，底部 `llt/rlt` **再算一次** → 左深链每层 2 次递归、逐层翻倍 = 2^N。
- **修复**：`+` 分支去掉递归调用（resolvedKind=="string" 快路径保留，不递归）；
  把「LLVM 类型为 ptr → 拼接结果 ptr」检测移到底部，**复用已算的 `llt/rlt`**
  （`if (op=="+" && opCall==0 && (llt=="ptr"||rlt=="ptr")) return "ptr";`）。
  操作数类型每节点只算一次 → O(n²)（genExpr 每节点一次 exprLlvmType(lhs)），
  深度 24 6.2s → 0.10s；10,000 加号 >120s → 10.8s。
- **回归**：torture `plus_bomb_*`（10000..17000 加号 + 100..240 `i++`，execute 8 全过）；
  字符串拼接/数值 `+` 语义对拍（strcon ok=4）；全量 333/0。
- **教训**：① 递归类型函数（exprLlvmType 等）内**同一操作数多次递归调用**是
  指数爆炸温床——左深链无共享子树，但"每层算两次子类型"照样 2^N；② 深表达式
  编译压测既能测编译器健壮性，也能暴露此类纯时间复杂度 bug（内存限制抓不到）。

## BUG-054（已修复 🟩，v3.15.87）：不同 struct 类型赋值/初始化静默过 sema → LLVM opt 崩溃

- **状态**：🟩 已修复（2026-08-26，v3.15.87，selfhost `tools/selfhost/src/sema.myp`）
- **复现**（torture struct_nest 生成器踩中）：`A a; B b; a = b;`（A/B 不同 struct）
  或链式字段 `root.inner...inner = root.inner`（L4=L1）编译过 sema → opt-21 报
  `error: '%t7' defined with type '%B' but expected '%A'`（store 类型错）崩溃，
  而非干净诊断。`A a = b;` 初始化同样。
- **根因**：`typesCompat(a,b)` 只比较 kind 字符串——两个 struct 都是 "struct" 恒等
  → Assign（sema.myp visitExpr `k=="Assign"`）与 VarDecl init（`typesCompat(vt,it)`）
  都放行不同 struct 互赋。
- **附带隐患**（修消息时发现）：Member 表达式（`root.inner`）的 `resolvedClass`
  是**容器** struct 名（L0）而非字段类型（L1）——若直接比 resolvedClass 会**误拒**
  合法同型初始化 `L1 x = root.inner;` / `L2 y = a.b.c;`。
- **修复**：
  1. 新增 `exprStructName(e)` + `structFieldTypeName(structName, fieldName)`：
     Identifier 查 SymbolEntry.className；**Member 递归解析对象 struct 名 + 查字段
     声明类型**（tu_.structs() 找字段 type className/basicName）。
  2. Assign：`l=="struct" && r=="struct"` 且具体名不同 → 报错
     `cannot assign value of type 'B' to variable of type 'A'`。
  3. VarDecl init：`vt=="struct" && it=="struct"` 且具体名不同 → 报错
     `cannot initialize variable 'a' of type 'A' with value of type 'B'`。
- **回归**：负测试 `tests/negative/struct_assign_mismatch.myp`（A=B 干净拒绝）；
  合法嵌套 struct 链 `L1 x=root.inner; L2 y=root.inner.inner; ...`（struval2 ok=4）
  + 同型整体拷贝（struok ok=2）；torture struct_nest_*（6..20 层）全过；全量 334/0。
- **教训**：① 类型兼容只比 kind 不够——struct/class 须比具体类型名；② 修类型检查
  务必验证**合法同型**路径不被误拒（resolvedClass 对 Member 是容器名这个坑）；③
  生成器踩中的 LLVM 崩溃（非干净诊断）本身就是编译器 bug 信号。

## BUG-055（已修复 🟩，v3.15.88）：解析器表达式/语句级递归无守卫 → 深嵌套栈溢出

- **状态**：🟩 已修复（2026-08-26，v3.15.88，selfhost `tools/selfhost/src/parser.myp`）
- **复现**（"上千层嵌套括号/嵌套条件表达式/嵌套宏展开，专门搞栈溢出"压测）：
  - 括号 `(((1)))` ×300 → parsePrimary 300 守卫**生效**（干净报错）——已有保护。
  - 但**无括号右嵌套**：三元 `1?2:1?2:...9`、`??` 合并 `a??b??c`、右结合赋值
    `a=b=c=...`、一元链 `!!!!x`、嵌套块 `{{{...}}}`、if 链 `if(1) if(1)...`——
    **50000 层 SIGSEGV 栈溢出**（gdb 确认 parsePrimary 300 计数器数不到这些
    parsePrimary 之上的递归）。
- **根因**：`recursionDepth_`（300 守卫）只在 `parsePrimary` 计数。而三元假分支 /
  `??` RHS / 赋值 RHS / 一元操作数 / 嵌套块 / if 体的递归都在
  parseExpr/parseAssignment/parseCoalesce/parseUnary/parseBlock/parseStatement 层——
  parsePrimary 之上，**不经 parsePrimary** → 计数器不增长 → 无上限 → 栈溢出。
- **修复**（三处独立计数器，均 300 守卫、跳过平衡括号/到 `;`/`}` 恢复）：
  1. `exprDepth_`：`parseExpr`（三元/右结合赋值 RHS 改走 parseExpr）+ `parseCoalesce`
     （`??` 自递归）+ `parseUnary`（拆 wrapper 守卫 + inner，自递归走 wrapper）。
  2. `stmtDepth_`：`parseBlock`（嵌套块，跳到匹配 `}`）+ `parseStatement`（if/while
     链，跳到 `;`/`}`）。
  3. 右结合赋值 `=`/复合赋值 RHS 从 `parseAssignment()` 改 `parseExpr()`（语义等价，
     获 parseExpr 守卫）。
- **回归**：负测试 `tests/negative/expr_recursion_deep.myp`（500 层三元干净拒绝）；
  torture 新增 `deep/*`（48 个：paren/ternary/blocks/ifchain/coalesce/unary，
  4000..32000 层，编译不崩溃）；全量 334/0。
- **教训**：① 递归下降解析器的深度守卫必须放在**每个递归入口**（parsePrimary 不够）——
  右结合操作符（三元/`??`/赋值）和无花括号 if 链是最容易漏的；② 50000 层这类
  "专门搞栈溢出"压测正好暴露守卫覆盖不全；③ 守卫恢复策略（跳平衡括号/到 `;`）保证
  报错后还能继续解析不卡死。

## BUG-056（已修复 🟩，v3.15.89）：嵌套泛型实例化 O(N³) 时间爆炸（DoS）

- **状态**：🟩 已修复（2026-08-26，v3.15.89，selfhost `tools/selfhost/src/sema.myp`）
- **背景**：审查"多层嵌套是否有守护"审计发现——表达式/语句/括号/宏都有守卫，但
  **类型级嵌套泛型** `Box<Box<...<int>...>>` 的 sema **实例化路径无守卫**。
- **复现**（`typeArgDepth` 前手工生成 `Box<Box<...int>> x;`，`--emit-llvm` 计时）：
  - depth 100 → 0.10s；300 → 1.4s；500 → 7.5s；800 → **>25s（超时）**；5000 → 不可完成。
  - **纯解析路径（不存在的泛型类 `Undef<Undef<...>>`）0.104s 平坦（100→5000 全同）**——
    解析器对嵌套泛型**无栈溢出**（50000 层 rc=1 干净语义错误），爆炸全在 sema 实例化。
- **根因**（O(N³)）：
  1. `SymbolTable.lookup` 是**线性扫描** `entries_`（已 N 项）× 长 mangled 名比较
     → 每次实例化查重 O(N²)；
  2. `instClassName` **递归拼名**（每层拼接整串 O(k)）→ 单层实例化名构建 O(N²)；
  3. `tryInstantiate` 对每个内层 typeArgs 递归实例化 → N 层 × 上面两项 = O(N³)。
- **修复**（sema.myp）：
  - `typeArgDepth(AstType)`：O(N) 单遍测最大泛型实参嵌套深度（不递归拼名）。
  - `tryInstantiate` 入口顶层测深：`>64` → 单条 `generic type nested too deeply
    (N > 64) while instantiating 'X'` 干净拒绝并整体返回（不做任何实例化/名构建）。
  - 保留 `instDepth_` 递归计数器作纵深防御（`typeArgDepth` 未覆盖的路径也不得超 64）。
  - 合法泛型嵌套 ≤4 层（`Map<string, ArrayList<Option<byte>>>`），64 极其宽松。
- **验证**：depth 10/200 正常编译（rc=0）；200/500/5000 → 单条守卫错误、0.10~0.30s
  （原 7.5s/25s+/小时级）。
- **回归**：负测试 `tests/negative/generic_nested_deep.myp`（70 层 → 1 条干净拒绝）；
  torture 新增 `deep/generic_*`（+8，4000..32000 层编译不崩溃，共 168 全过）；正向
  `tests/@test/nested_generic.myp`（2/4/10 层声明+构造正常编译运行）；全量 339/0。
- **viz 对拍**：torture/generated 病态文件（deep/generic_*）从 `test_myp_viz.sh`
  全语料对拍排除——自举带守卫拒绝、C++ oracle 无守卫，属预期分歧（同 negative/）。
- **教训**：① 栈溢出守卫 ≠ 全部健壮性——**时间爆炸**（O(N³) 病态输入挂死编译器）
  同属 DoS，审计时要测"会不会慢到不可完成"而不只看"会不会崩"；② 守卫要放在
  **顶层一次测深**（单条干净错误），放递归内部会每层各报一次（65 条重复）+ 底下
  64 层仍做 O(N²) 名构建；③ 区分阶段：纯解析平坦 → 问题在 sema，别误修 parser。


## BUG-057（已修复 🟩，v3.15.90）：顶层函数调用结果链式成员访问回落当前类

- **状态**：🟩 已修复（2026-08-26，v3.15.90，selfhost `tools/selfhost/src/sema.myp`）
- **背景**：排查"泛型链式访问"时发现——`rawStep(5).get()`（顶层函数返回
  `Result<int,string>` 后直接取成员）报 `class 'T2' has no member 'get'`，成员查找
  **回落到当前类**。进一步隔离发现**不止泛型**：`makeErr().message()`（返回具体类
  ParseError）、`strVal().len()`（返回 string）全失败。
- **复现对比**（同样返回类/struct，唯独这条路径失败）：
  - ✅ `resultOk<int,string>(7).get()`（泛型显式类型实参）——泛型路径设了 valueClass
  - ✅ `c.step3(-1).getOr(99)`（类 action）——方法路径设了 resolvedClass
  - ✅ `new Result<int,string>(9).get()`（new）——new 路径设了
  - ❌ `rawStep(5).get()`（普通顶层函数）——**只设返回 kind，不设 valueClass**
- **根因**（sema.myp Call 解析 ~4694）：普通顶层函数调用 `findFuncRet(fn)` 只返回
  kind（"class"），没把返回类名记到 CallExpr。后续 Member 访问（对象为 Call 时）
  `cn = arr.valueClass(); if empty cn = arr.resolvedClass();` → 都空 → 回落到当前类
  "class 'T2' has no member"。
- **修复**：`findFuncRet` 命中且 kind 为 class/struct/interface 时，用
  `findFuncRetType(fn, nargs)`（返回 AstType）+ `gsRetValueClass`（取类名/泛型实例名）
  → `e.setValueClass(fvc)`。对齐泛型显式实参/类 action/new 路径。
- **验证**：`rawStep(5).get()=6`、`.isErr()/.getErr()/.getOr()`、`makeErr().message()`、
  `makeVec(3).x`（struct）、`makeShape().area()`（interface 分发）、两层链
  `doubleGet(5)=12`、lambda 内 `rawStep(x).get()*3` 全通。
- **回归**：`tests/@test/call_result_chain.myp`（6 测试/12 断言：Result/具体类/
  struct/interface/两层链/lambda）；全量 342/0。
- **边界**：`strVal().len()` 仍失败是**设计使然**——MYP 字符串值本无 `.len()` 成员
  （`"hello".len()`/`s.len()` 均不支持，须 `Str.len(s)`），非本 bug 缺口。
- **教训**：① 同一"取调用返回类型"语义在 sema 有四条路径（泛型显式实参/类 action/
  new/普通顶层函数），只三条设了返回类名——排查链式访问问题时先枚举所有调用形态；
  ② 报错信息 `class '<当前类>' has no member` 是"回落当前类"的典型信号，别被误导成
  当前类真的缺成员。

## BUG-058（已修复 🟩，v3.15.91）：调用结果下标 f()[i] 元素类型丢失（BUG-057 姊妹）

- **状态**：🟩 已修复（2026-08-26，v3.15.91，selfhost sema.myp + codegen.myp）
- **背景**：测试缺口审计发现 `f()[i]`（函数返回 T[] 后直接下标）@test 零覆盖；实测
  `makeArr()[1]` 报 `expected numeric type, got 'array'`——返回数组的元素类型没传到
  Subscript。与 BUG-057 同根（调用结果的类型信息没记录）。
- **复现**：
  - ❌ `int a = makeArr()[1];`（顶层函数返回 int[]）→ sema 报 "expected numeric type, got 'array'"
  - ❌ `string s = makeStr()[1];`（string[]）→ 同样
  - ❌ `int m = b.get()[0];`（方法返回 int[]）→ 同样
  - ❌ `double d = makeD()[1];`（double[]）→ 同样
- **根因（两处）**：
  1. **sema**：Subscript 解析里 `sa.kind() == "Call"` 分支只处理 `bytesOf`→ubyte，
     其它返回数组的调用不取元素类型 → `et` 停留 "array"。
  2. **codegen**：`subscriptElemLt(arr)` 对 Call 返回默认 "i32" → `string[]`/对象[]
     `load i32` 后 `myp_retain(ptr)` 收到 i32 → LLVM 类型不匹配（opt 崩）。
- **修复**：
  - sema：Call callee 为 Identifier → `findFuncRetType` 取 element；为 Member →
    `cal.resolvedClass()`（sema 解析成员时已设具体类名）+ `findMethodRetAst` 取 element。
    元素是类/struct 时 `e.setResolvedClass(el.className())`。
  - codegen：`subscriptElemLt` 补 Call 分支——Identifier callee →
    `findFuncRetAstType`，Member callee → `methodRetAstType`，用 `llvmType(element())`。
- **验证**：int/double/string 元素、方法链 `b.get()[i]`、`f()[i]` 参与算术、写路径
  （临时数组）全通。
- **回归**：`tests/@test/call_subscript_chain.myp`（5 测试/12 断言：int/double/string/
  方法链/写路径）；全量 343/0。
- **教训**：① 测试缺口审计先枚举语法形态（`f().member()` 修了、`f()[i]` 还坏——同一
  类问题的姊妹缺口），再枚举返回类型（int/double/string/对象/struct）；② 修 sema 后
  必须验证 codegen——sema 报语义错掩盖了 codegen 的默认 i32 类型不匹配，两层都要改。

## BUG-059（已修复 🟩，v3.15.92）：调用结果 slice 链式访问（下标/size）未解析

- **状态**：🟩 已修复（2026-08-26，v3.15.92，selfhost sema.myp + codegen.myp）
- **背景**：续 BUG-058 排查姊妹缺口，`makeSlice()` 返回 `slice<int>` 后：
  - ❌ `makeSlice()[0]` → "cannot initialize int with array"（元素类型未解析）
  - ❌ `makeSlice().size()` → "int with void"（.size() 未解析）
  - ❌ `makeStrSlice()[0]`（slice<string>）→ 同上
  - ❌ `b.get()[0]`（方法返回 slice）→ 同上
- **根因（slice 用 typeArgs 存元素，非 element()；且多处只处理 Identifier 数组）**：
  1. **sema Subscript**：Call 分支从返回 AstType 取 element——但 `slice<T>` 元素在
     `typeArgs`，`element()` 为 null → 只修了数组没修 slice。Member-callee 分支同理。
  2. **sema Call-Member-callee**：slice 内建 `.size()/.length()/.data()` 只处理
     `mo.kind()=="Identifier"`（slice 变量），`mo.kind()=="Call"`（slice 返回调用）
     漏 → 回落方法查找 → void。
  3. **codegen**：`subscriptElemLt` Call 分支补 slice(typeArgs) 元素 LLVM 类型；
     `genCall` slice `.size()/.data()` 只处理 Identifier，Call 返回 slice 漏 →
     `Object_size(ptr {ptr,i64})` 类型不匹配。
- **修复**：sema 两处 Call 分支（Identifier/Member callee）补 slice(typeArgs) 元素 +
  Call-Member-callee 补 slice 返回调用的 size/length/data；codegen subscriptElemLt
  补 slice 元素 LLVM 类型 + genCall 补 slice 返回调用的 size/data extractvalue
  （`exprLlvmType(obj)=="{ ptr, i64 }"` 判定）。
- **验证**：slice<int>/<string> 返回下标、size/length、方法返回 slice 下标+size、
  slice 下标参与算术全通。
- **回归**：`tests/@test/slice_call_chain.myp`（4 测试/11 断言）；全量 344/0。
- **教训**：① slice 与数组的类型表示不同（slice 元素在 typeArgs、数组在 element），
  凡"取元素类型"处两条都要覆盖；② 链式访问族（BUG-057/058/059）根因相同——调用
  结果的类型信息（成员类/数组元素/slice 元素）没在 sema/codegen 各路径完整传递，
  审计要按"语法形态 × 返回类型"矩阵枚举。

## BUG-060（已修复 🟩，v3.15.93）：表达式 try 类型检查缺失 + catch 值转换块位错

- **状态**：🟩 已修复（2026-08-26，v3.15.93，selfhost sema.myp + codegen.myp）
- **背景**：补表达式 try（`var n = try expr catch (e) default;`）@test 时暴露两处：
  - **060a**：`int a = try risky(5) catch (e) "oops";`（int vs string）自举静默过
    sema（C++ visitTryExpr 有 typesCompatible 检查）→ codegen PHI i32/ptr 类型
    不匹配 → opt 崩（非干净诊断）。
  - **060b**：`long v = try risky(5) catch (e) -1L;`（int try / long catch，数字提升
    合法）→ opt verify "input module is broken"——genTryExpr 把 catch 值转换
    （convertValue/trunc）放在 `openBlock(merge)` **之后**，trunc 落进 merge 块、
    phi 前 → 违反「phi 必须在块首」+「指令支配」两条规则。
- **根因**：060a——自举 TryExpr 分支只有 `visitExpr(tryExpr)/visitExpr(catchExpr)`，
  无 `typesCompat` 检查；060b——转换指令发射位置错（须在 catch 块、`emitBr(merge)` 前）。
- **修复**：
  - sema：TryExpr 加 `typesCompat(t,f)==0` → 报 `try/catch expressions have
    incompatible types: 'X' and 'Y'` 干净拒绝。
  - codegen：genTryExpr 把 `if (t2 != t1) v2 = convertValue(v2,t2,t1);` 移到
    `openBlock(mergeB)` 之前（catch 块内）。
- **验证**：int vs string → 干净报错；int/long 提升 → v=10 正常。
- **回归**：正 `tests/@test/expr_try.myp`（5 测试/9 断言：成功/失败默认值/算术/
  提升/实参/嵌套）；负 `tests/negative/expr_try_type_mismatch.myp`；全量 346/0。
- **教训**：① 表达式 try 的 catch 变量是**占位符**，不在 catch 表达式中绑定（C++
  oracle 与自举一致）——`catch (e) Str.len(e)` 报 undefined 是设计，非 bug；
  ② phi 前**不允许任何非 phi 指令**（LLVM 块首规则），凡 merge 块需先算好在各
  前驱块内完成的转换值。

## BUG-061（已修复 🟩，v3.15.94）：调用结果嵌套 slice 双下标 make2d()[i][j] 未解析

- **状态**：🟩 已修复（2026-08-26，v3.15.94，selfhost sema.myp）
- **背景**：用户追问"之前测试的 2D 数据访问"——BUG-058 时 `make2d()` 定义（`new
  int[2][]`）报错搁置。MYP 的 2D 是**嵌套 slice**（`slice<slice<int>>`），非
  `int[][]`。探针暴露 `make2d(4)[2][3]`（函数返回 2D + 链式双下标）报
  `expected numeric type, got 'array'`。
- **根因**：sema Subscript 的 Call 分支处理 `slice<slice<int>>` 返回时，元素类型
  （typeArgs[0]=`slice<int>`）解析成 "slice"，但**没记 `setSliceElem`（深层元素 int）**
  → 二级下标 `(make2d(4)[2])[3]` 走 `sa.kind()=="Subscript"` 分支取 `sa.sliceElem()`
  为空 → `et` 停留 "array"。变量版（nested_slice）走了 `en.elementElem()` 路径正常，
  调用结果版漏。
- **修复**：sema Call 分支（Identifier + Member callee 两处）在元素为 slice 且
  `el.typeArgs().size()>0` 时 `e.setSliceElem(typeToKind(el.typeArgs().get(0)))`
  ——记深层元素供二级下标。
- **验证**：`make2d(4)[2][3]=11`、算术、单下标取行、方法返回 2D、变量对照全通。
- **回归**：`tests/@test/slice_2d_chain.myp`（5 测试/12 断言）；全量 351/0。
- **教训**：① MYP 2D = 嵌套 slice，不是 `int[][]`（`new int[2][]` 不存在）；②
  调用结果链式访问族（BUG-057/058/059/061）同一根因——类型信息（成员类/数组元素/
  slice 元素/深层 slice 元素）在各路径未完整传递；③ 二级下标靠 `setSliceElem`
  记录深层元素，凡返回 `slice<slice<T>>` 的调用路径都要补。

## BUG-062（已修复 🟩，v3.15.95）：泛型集合方法返回链式访问（元素/实例类型未传播）

- **状态**：🟩 已修复（2026-08-26，v3.15.95，selfhost sema.myp）
- **背景**：用户问"collections 有没有此问题"（链式访问族 BUG-057~061）。探针验证
  collections 链式访问：
  - ✅ `ArrayList<struct>.get(i).field`（原已好）
  - ❌ `ArrayList<int[]>.get(i)[j]` → "with value of type 'array'"（数组元素未传播）
  - ❌ `ArrayList<slice<int>>.get(i)[j]` → 同上（slice 元素未传播）
  - ❌ `ArrayList<ArrayList<int>>.get(0).get(1)` → 分派到**未实例化模板** `@ArrayList_get`
    （返回 ptr 而非 int）→ opt 崩
- **根因（泛型方法返回 T 的替换不完整）**：
  1. 下标 Call-Member 分支用 `findMethodRetAst` 取原始返回 `T`（无 element/typeArgs）
     → 元素取不到。
  2. Call 泛型方法返回替换用 `substRet`（`SymbolEntry.instArgs` **拍平字符串**，
     只存 className 丢 typeArgs）→ `ArrayList<ArrayList<int>>.get()` 返回替换成
     "ArrayList" 而非 "ArrayList_int_inst" → 链式 `.get()` 分派错。
- **修复（sema.myp 两处）**：
  1. 下标 Call-Member 分支：`mret` 无 element/typeArgs 时用
     `findInstTypeArgs(mcls)` + `findClassTypeParams` + `substituteType` 替换
     类型参数取元素。
  2. Call 泛型方法返回替换路径 1：优先用 `findInstTypeArgs(实例类名)` +
     `substRetAst`（完整 AstType 实参），拍平字符串仅作兜底。
- **设计确认**：`HashMap<K,V>` 是**整型键**（`%` 哈希）；字符串键用
  `StrHashMap<string,V>`（DJB2）——`HashMap<string,...>` 不支持（`%` 对字符串
  指针 srem 崩，opt 报错不清）。map.myp 注释已说明。
- **验证**：A~E 全通（struct/map/数组元素/slice 元素/嵌套集合链）。
- **回归**：`tests/@test/collections_chain.myp`（5 测试/14 断言）；全量 352/0。
- **教训**：① 泛型方法返回替换要**保留完整 typeArgs**（AstType 级），拍平字符串
  会丢实例类型；② 链式访问族根因统一——调用结果的类型/元素信息各路径未完整传递，
  collections 泛型方法返回 T 是最典型的漏网点。

## BUG-063（已修复 🟩，v3.15.96）：调用结果成员后下标 get().field[j] 未解析

- **状态**：🟩 已修复（2026-08-26，v3.15.96，selfhost sema.myp）
- **背景**：续链式访问族排查（BUG-057~062 之后），系统探测更多形态：
  - ✅ 多级方法链 c.makeB().makeA().val() / 元素类方法 arr[i].method() /
    三级集合链 cube.get(0).get(0).get(1) / 构造链 new().method()——原已好。
  - ❌ `ps.get(0).data[1]`（调用结果 .struct数组字段 再下标）→ "with value of
    type 'array'"；`bs.get(0).arr()[1]`（调用结果 .方法返回数组 再下标）同坏。
- **根因**：sema Subscript 的 Member 分支只处理对象是 `Identifier`（`mo2.kind()==
  "Identifier"` → sym_.lookupClass），对象是 `Call`（`ps.get(0).data`）时不解析
  字段元素类型 → `et` 停留 "array"。slice 字段同样漏。
- **修复**：Member 分支对象加 `Call/Subscript/Member/New` 情况——用
  `mo2.valueClass()/resolvedClass()` 取类名，再查 class 属性/struct 字段元素
  （数组 element + slice typeArgs + 嵌套 slice 深层元素，对齐既有分支）。
- **验证**：`ps.get(0).data[1]=4`、`bs.get(0).arr()[1]=6`、写路径、算术全通。
- **回归**：`tests/@test/collections_chain.myp` 增补 test_member_subscript /
  test_method_subscript（7 测试/20 断言）；全量 352/0。
- **教训**：① 链式访问族的"对象是 Identifier vs 链式结果（Call 等）"分支不对称
  是持续漏网点——凡按对象形态分派的解析路径，都要枚举 Call/Subscript/Member/New；
  ② 测试先枚举"对象形态 × 成员形态 × 返回类型"矩阵（Identifier/Call × field/
  method × 数组/slice/类）。

## BUG-064（已修复 🟩，v3.15.97）：interface 转换从链式结果/struct 字段漏具体类名 → null vtable 段错误

- **状态**：🟩 已修复（2026-08-26，v3.15.97，selfhost codegen.myp）
- **背景**：review BUGLIST 找"相似未修复"——发现表里 4 个非 🟩（029/032/046/051）
  其实详情都是已修复（表标记过时 U+FFFD，已修）；但进一步按 interface 转换族
  （BUG-029/033/034）探针验证发现真缺口：
  - ✅ `Shape s = new Circle()` / 局部变量 / `this.field` / `arr[i]`（已修）
  - ❌ `Shape s = factory();`（顶层函数返回）→ 段错误
  - ❌ `Shape s = b.make();`（方法返回）→ 段错误
  - ❌ `Shape s = cs.get(0);`（集合 get 泛型方法返回）→ 段错误
  - ❌ `Shape s = h.c;`（struct 变量 .字段）→ SIGTRAP
- **根因**：codegen `upcastClsName`（接口变量分支解析具体类名建 vtable）只处理
  New/Identifier/this.field/Subscript，漏 Call（函数/方法/集合 get 返回）与
  struct 变量 .字段 → `clsName` 空 → 只存实例不存 vtable → 派发 `call *0x(vtable)`
  段错误。
- **修复**（upcastClsName 加两分支）：
  1. `Call`：用 `e.valueClass()/resolvedClass()`（sema BUG-057/062 已设具体类名）。
  2. `Member` + 对象为 Identifier 且是 struct 变量：`structFieldAstType` 解析字段
     声明类型。
- **验证**：S1~S6（new/局部/函数返回/方法返回/集合 get/struct 字段）全通。
- **回归**：`tests/@test/iface_upcast_chain.myp`（5 测试/6 断言）；全量 353/0。
- **教训**：① BUGLIST 表状态与详情必须同步（4 个过时标记 U+FFFD 误导排查）；
  ② interface 转换族与链式访问族同根——具体类名须按源表达式形态传播，upcastClsName
  的形态枚举（New/Identifier/This.field/Subscript/Call/struct 字段）同样要补全。

## BUG-065（已修复 🟩，v3.15.98）：interface 转换从链式结果 .字段 漏具体类名（BUG-064 姊妹）

- **状态**：🟩 已修复（2026-08-26，v3.15.98，selfhost codegen.myp）
- **背景**：修完 BUG-064（直接 Call 返回 → interface）后继续探 interface 转换族
  更多上下文：
  - ✅ 接口参数从 call 结果（`takeShape(factory())`）/ 返回 call 结果作 interface
    ——原已好（走接口值拷贝路径）。
  - ❌ `Shape s = hs.get(0).c;`（集合 get 结果的 struct 字段 → interface）→ 段错误。
- **根因**：codegen `upcastClsName` 的 Member 分支只处理对象是 Identifier（struct
  变量）与 this.field，对象是 Call（`hs.get(0).c`）时不解析字段具体类 → vtable 槽
  null → 派发段错误。
- **修复**：Member 分支对象加 Call/Subscript/Member/New——用 valueClass/
  resolvedClass 取返回类型，再按 struct 字段（structFieldAstType）/class 属性
  （propAstType）解析字段具体类。
- **验证**：`hs.get(0).c` → interface =4；接口参数/返回 call 结果全通。
- **回归**：`tests/@test/iface_upcast_chain.myp` 增补 test_coll_get_field /
  test_iface_param / test_iface_ret（8 测试/10 断言）；全量 353/0。
- **教训**：interface 转换族与链式访问族同根且同形态枚举问题——upcastClsName 的
  Member 分支与 Subscript 分支一样，都要覆盖"对象是 Identifier vs 链式结果"
  全形态；连续探针（BUG-064→065）逐步暴露。

## BUG-066（已修复 🟩，v3.15.100）：类内未限定方法调用缺实参类型校验 → opt 崩

- **状态**：🟩 已修复（2026-08-26，v3.15.100，selfhost sema.myp）
- **背景**：「缺失编译期校验 → codegen 崩」族（BUG-007/008/016/046/050/054 模式）
  续查。探测类型不匹配时发现：基本赋值 / 返回 / 数组元素写/读校验全面，但
  **类内未限定方法调用实参**漏检。
  - ❌ `takeInt("hello")`（string 实参 vs int 形参）静默过 sema → codegen 发射
    `i32 getelementptr(ptr @str, ...)`（ptr 当 i32）→ **opt 崩**
    （`constant expression type mismatch: got type 'ptr' but expected 'i32'`，
    非干净诊断）。C++ oracle 干净拒绝 `argument 1: expected 'int', got 'string'`。
- **根因**：Identifier-callee 的 `inClass_ != 0` 未限定方法调用路径（sema.myp
  ~4724）只设 ret/resolvedClass + fillDefaultArgs，**漏 `normalizeCallArgs` +
  `setCallParamTypes`** → 后续实参类型检查（`callParamTypes()` 非空才跑）被跳过。
  而 Member 分支（`obj.method()`，~4793）早已有该检查 → 仅类内裸名调用漏网。
  Member-callee 的 `else if inClass_` 回退路径（~4896）同样漏。
- **修复**：两条路径镜像 Member 分支补 `normalizeCallArgs` + `setCallParamTypes`。
  **事件守卫**：事件（`event:` 段）在 `methods_` 里以 **0 参数**注册（mapping 触发
  用裸名 `emit(v)`，实参另处处理）——若不加 `isEvent(cls, name)` 守卫会误报
  "call takes no arguments, but 1 given"（C++ oracle 接受裸名事件触发）。
- **验证**：`takeInt("hello")` / `takeStr(7)` 均干净拒绝，诊断文本/位置与 oracle
  逐字节一致（test_myp_self 对拍 95/0）；`where_mapping`（裸名 `emit(5)` 事件
  触发）不受影响。
- **回归**：`tests/negative/arg_type_mismatch.myp`（负测试，EXPECT ERROR
  argument 1: expected 'int', got 'string'）；全量 131 @test PASS / 0 FAIL。
- **教训**：这类「oracle 有校验、自举漏」的缺口集中在**调用路径**（方法/函数实参）
  与**赋值路径**（变量/返回/元素写）——赋值族已全，调用族此次补齐类内未限定
  路径；后续审查应重点比对 oracle `typesCompatible` 出现的每个调用点。

## BUG-067（已修复 🟩，v3.15.101）：泛型实例方法缺实参类型校验 → opt 崩

- **状态**：🟩 已修复（2026-08-26，v3.15.101，selfhost sema.myp）
- **背景**：修完 BUG-066（类内未限定方法调用）后继续探「oracle 有校验、自举漏」
  的调用路径缺口——泛型实例方法实参：
  - ❌ `ai.add("str")`（`ArrayList<int>` 的 add，string 实参 vs int 形参）静默过
    sema → codegen 发射 `i32 getelementptr(ptr @str, ...)`（ptr 当 i32）→ **opt
    崩**（`constant expression type mismatch: got type 'ptr' but expected 'i32'`，
    非干净诊断）。C++ oracle 干净拒绝 `argument 1: expected 'int', got 'string'`。
- **根因**：泛型实例方法经 `resolveBase(instCls)` 回落到**模板**类取形参——模板
  形参占位符 `T` 不在 curGeneric_ 时 `typeToKind(T)` → `"void"` → 实参类型检查的
  `anyVoidParam` 守卫**整段跳过**。Member 分支虽早设了 `setCallParamTypes`，但
  kind 是模板占位符（void），检查不生效。类内未限定路径（BUG-066 修的）同理。
- **修复**：
  1. 新增 `substParamKinds(base, params, targs)`（镜像 `substRetAst` 替换：形参
     `T` → `typeToKind(targs[j])`，int/string/class/struct/interface）。
  2. 新增 `instTypeArgsOfObject(mo)`（镜像 BUG-062 三形态：Identifier 变量 /
     裸属性 `currentClassMemberAstType` / 链式结果 valueClass·resolvedClass →
     `findInstTypeArgs`）。
  3. Member 分支 `setCallParamTypes` 处：若对象是泛型实例，用替换后的 kind。
  4. **关联类型守卫**：`check(T c, T::Item v)` 的参数 2 替换后仍是 `assoc`
     （T::Item 未解析到具体实例的关联类型）——若整段检查跑起来会误报
     `expected 'assoc', got 'byte'`（manual_ch6_class/assoc_types 回归暴露）。
     把 `"assoc"` 也纳入 `anyVoidParam` 守卫跳过（修复前参数 1 的 T→void 本就
     整段跳过，行为不回归）。
- **验证**：`ai.add("str")` / 链式 `makeList().add("str")` / 属性 `plist.add("str")`
  / 类元素 `ac.add(42)` 均干净拒绝（诊断文本/位置与 oracle 逐字节一致）；
  合法调用 `ai.add(5)`、`as.add("hi")`、`aai.add(row)`（int[] 元素）不受影响。
  注：类形参诊断文本 selfhost 用 "class"、oracle 用类名 "Circle"——**既有分歧**
  （非泛型 `takeCircle(42)` 同样），非本次引入。
- **回归**：负测试 `tests/negative/generic_arg_type_mismatch.myp`（EXPECT ERROR
  argument 1: expected 'int', got 'string'）；正测试 `tests/@test/generic_arg_ok.myp`
  （ArrayList<int>/string/int[] 元素 + 链式返回 + 属性泛型，1 测试/7 断言，
  双编译器 7/7）；全量 132 @test PASS / 0 FAIL；oracle 对拍 95/0。
- **教训**：泛型实例方法（`ArrayList<int>` 等）的实参校验必须用**替换后的形参
  kind**——模板占位符 kind 是 void/assoc，触发守卫跳过形同虚设。凡泛型方法返回
  替换（BUG-062 `substRetAst`）存在的地方，实参校验同样需要替换，二者须同步。

## BUG-068（已修复 🟩，v3.15.102）：接口变量方法缺实参类型校验（静默错参）

- **状态**：🟩 已修复（2026-08-26，v3.15.102，selfhost sema.myp）
- **背景**：续 BUG-066/067 的「oracle 有校验、自举漏」调用路径审查——接口变量：
  - ❌ `IBox ib = new IntBox(); ib.put("str");`（`put(int)` 的 string 实参）自举
    编译+运行通过（静默错参，ptr 截断成 i32）。C++ oracle 干净拒绝
    `argument 1: expected 'int', got 'string'`。
- **根因**：接口方法注册在 `interfaceMethods_`（**不在** `methods_`/`methodSigIdx_`）
  → Member 分支 `findMethodParams(base3, member)` 返回 null → `callParamTypes` 不设
  → 实参检查（非空才跑）被跳过。
- **修复**：新增 `findIfaceMethodParams(ifc, meth)`（镜像 `findIfaceMethodRet`）；
  Member 分支 `anyMps` 解析后 `if (anyMps == null) anyMps = findIfaceMethodParams(
  base3, memberName)`——接口签名即规范形参，走同一 normalizeCallArgs +
  setCallParamTypes 校验路径。
- **验证**：`ib.put("str")` 干净拒绝（文本/位置与 oracle 逐字节一致）；合法
  `ib.put(7)`、多实现类（IntBox/Circle）分派不受影响。
- **回归**：负测试 `tests/negative/iface_arg_type_mismatch.myp`（EXPECT ERROR
  argument 1: expected 'int', got 'string'）；全量 132 @test PASS / 0 FAIL；
  oracle 对拍 95/0。
- **教训**：凡「方法签名存在但不在 methodSigIdx_ 主索引」的调用路径（接口方法、
  事件已用 isEvent 守卫）都要补 callParamTypes——校验依赖 callParamTypes 非空。

## BUG-069（已修复 🟩，v3.15.103）：泛型 static 调用缺实参校验 → opt 崩

- **状态**：🟩 已修复（2026-08-26，v3.15.103，selfhost sema.myp）
- **背景**：续探调用路径实参校验——泛型 static（`@static class` 的泛型 static 方法）：
  - ❌ `List.foldInt<int>(arr, 0)`（漏 fn 实参）静默过 sema → 运行时行为错；
  - ❌ `List.foldInt<int>(arr, "str", ...)`（string 实参 vs int 形参）静默过 sema
    → codegen 实参类型错（ptr 当 i32）→ **opt 崩**（`constant expression type
    mismatch: got type 'ptr' but expected 'i32'`，非干净诊断）。
  - C++ oracle 三处全拒（`missing required argument 'f'` / `expected 3 arguments,
    got 2` / `argument 2: expected 'int', got 'string'`）。
- **根因**：`resolveGenericStaticCall`（自举）只校验**类型实参**个数/推导，完全不
  校验**值实参**数量与类型；C++ 同名函数对替换后的实例签名逐参 typesCompatible。
- **修复**：`resolveGenericStaticCall` 末尾加实参校验——`normalizeCallArgs` 管
  数量/默认/命名（失败 ret=void）；随后逐参用 `substituteType(param.type, tps,
  concrete)` 得替换后 kind，与实参 resolvedKind 比（lambda 实参 → "function"，
  void/assoc 跳过）。
- **验证**：缺参/错参均干净拒绝；合法 `foldInt<int>(arr,0,(int a,int b)=>a+b)`
  （=6）与 `map<int,string>`（=v5）不受影响。
- **回归**：负测试 `tests/negative/generic_static_arg_mismatch.myp`（EXPECT
  ERROR argument 2: expected 'int', got 'string'）；全量 132 @test PASS / 0 FAIL；
  oracle 对拍 95/0。
- **已知限制（非本次）**：函数类型形参的**签名**比较（`(int) -> int` vs
  `(string) -> int`）自举在**所有路径**都不做（kind 都是 "function"，typesCompat
  恒放行）——oracle 报 `expected '(int) -> int', got '(string) -> int'`。非崩溃
  缺口（静默错参），涉及函数签名结构化比较，另立特性待办。
- **教训**：泛型 static 调用是 `resolveGenericStaticCall` 独立路径，容易漏「值
  实参校验」——与实例方法（Member 分支）不同，须在 resolve 内补齐。凡泛型替换
  （substituteType/substRetAst）出现处，实参校验都要用替换后签名。

## BUG-070（已修复 🟩，v3.15.104）：bitcast 非数字操作数/源类型不匹配漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-26，v3.15.104，selfhost sema.myp）
- **背景**：续「缺失编译期校验 → codegen 崩」族审查，覆盖内建 `bitcast<T,U>`：
  - ❌ `bitcast<int>(s)`（string 操作数，非数字）静默过 sema → codegen 发射
    `bitcast ptr to i32`（ptr 64 位 vs i32 32 位，LLVM 非法 cast）→ **opt 崩**
    （`invalid cast opcode for cast from 'ptr' to 'i32'`，非干净诊断）。
  - ❌ `bitcast<float,int>(x)`（显式源 float 与 int 操作数不匹配）静默过。
  - C++ `visitBitcast` 三段检查齐全（源类型匹配 / numeric / 同宽）。
- **根因**：自举 bitcast 只查宽度 `if (sw != 0 && tw != 0 && sw != tw)`——非数字
  源（string/class）`bitcastWidth→0` 时条件为假 → 不报错直接放行；也缺显式源
  类型与操作数类型的兼容检查。
- **修复**：镜像 C++ visitBitcast 三段——①显式源（2 个类型实参）与操作数
  resolvedKind 比 `typesCompat`，不匹配报 `bitcast source type 'X' does not match
  operand type 'Y'`；②`sw==0 || tw==0` 报 `bitcast requires numeric source and
  target types (integer/float/char)`；③同宽检查保留（原逻辑）。
- **验证**：`bitcast<int>(string)` / `bitcast<float,int>(x)` / 宽度不符均干净拒绝；
  合法 `bitcast<int>(int)`、`bitcast<double>(long)`、`bitcast<int>(float)` 位保持
  正确（1.5f → 1069547520）。
- **回归**：负测试 `tests/negative/bitcast_numeric.myp`（EXPECT ERROR bitcast
  requires numeric source and target types）；全量 132 @test PASS / 0 FAIL；
  oracle 对拍 95/0。
- **教训**：内建（bitcast 等）的编译期校验也是「oracle 有、自举漏」高发区——凡
  oracle 有 error 分支的自举内建都要逐段比对，尤其「宽度 0 = 非数字」这类
  `!= 0 &&` 守卫会让非法输入绕过整段校验。

## BUG-071（已修复 🟩，v3.15.105）：显式转换 int(x) 非数字操作数漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-26，v3.15.105，selfhost sema.myp + parser.myp）
- **背景**：续「缺失编译期校验 → codegen 崩」族，覆盖显式转换 `int(x)`/`long(x)`
  等（内置类型名当函数调用，manual §三-3）：
  - ❌ `int(f)`（class 操作数）/ `int(s)`（string 操作数）静默过 sema → codegen
    把对象/字符串指针当 i32 用 → **opt 崩**（`%t9 defined with type 'ptr' but
    expected 'i32'`，非干净诊断）。
  - C++ `visitConvert` 校验源/目标须数字或 bool（含 bit/bitvector/char）。
- **根因**：自举 Convert 处理（sema.myp `k=="Convert"`）完全不校验，只
  setResolvedKind(target) 直接返回；string 不入 T(x)（那是 parseInt §6.2）。
- **修复**：
  1. sema：Convert 校验——源/目标 kind 须数字或 bool/bit/bitvector/char，否则报
     `cannot convert 'X' to 'Y' (conversion operand and target must be numeric
     or bool)`（文本与当前 src/ 一致；build-cpu 冻结种子是旧文本）。位保持的
     bitvector 互转（uint(bv)/int(bv)/byte(bv)）合法。
  2. parser：Convert 节点（通用 + bitvector<N> 两分支）补 `setPos`（此前
     line/col=0，报错位置落到 0:0）。
- **验证**：`int(Foo)` / `int(string)` 干净拒绝，位置与 oracle 逐字节一致
  （7:21）；合法转换 `double(int)`/`int(double)`/`float(int)`/`byte(long)`/
  `int(bitfield)`/`long(char)`/`bool(int)`/`int(bool)` 全过；bitvector 目录测试
  不受影响。
- **回归**：负测试 `tests/negative/convert_nonnumeric.myp`（EXPECT ERROR cannot
  convert 'string' to 'int'）；全量 132 @test PASS / 0 FAIL；oracle 对拍 95/0。
- **教训**：显式类型转换（ConvertExpr）是另一个「oracle 有 error 分支、自举裸
  return」的缺口——与 bitcast（BUG-070）同族。含 bit/bitvector 的转换规则要照
  C++ visitConvert 逐分支镜像，不能只做「数字 vs 非数字」二分（会误伤 bitvector）。

## BUG-072（已修复 🟩，v3.15.106）：数组/字符串下标类型漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-26，v3.15.106，selfhost sema.myp）
- **背景**：续「缺失编译期校验 → codegen 崩」族：`a["str"]`（string 下标）静默
  过 sema → codegen 把 string 指针当 i64 索引 → **opt 崩**（`invalid cast opcode
  for cast from 'ptr' to 'i64'`，非干净诊断）。C++ `expectNumeric` 镜像（允许
  byte/short/int/long/ubyte/ushort/uint/ulong/char/float/double）。
- **根因**：自举 Subscript 处理不校验 index 类型（只解析元素类型）。
- **修复**：Subscript 分支访问 index 后校验 `isNumKind(indexKind)`，非数字报
  `expected numeric type, got 'X'`（文本/位置与 oracle 逐字节一致）。
- **验证**：`a["str"]` 干净拒绝（4:41 与 oracle 一致）；合法 `a[n]`/`a[1L]`/
  `s[1]`（string 下标 char）不受影响。
- **回归**：负测试 `tests/negative/subscript_type.myp`；全量 132 @test PASS /
  0 FAIL；oracle 对拍 95/0。
- **教训**：`isNumKind`（与 oracle expectNumeric 同集）可用于下标/算术/位运算
  多处；凡「oracle 有 expectNumeric/expectBool」的位置都要逐一比对自举。

## BUG-073（已修复 🟩，v3.15.107）：new T[n] 数组大小漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-26，v3.15.107，selfhost sema.myp）
- **背景**：`new int["hi"]`（数组大小非整数）静默过 sema → codegen 把 string
  指针当 i64 大小 → **opt 崩**（`constant expression type mismatch: got type
  'ptr' but expected 'i64'`，非干净诊断）。C++ `visitNewArrayExpr` 镜像（仅
  int/long/short/byte）。
- **根因**：自举 NewArray 处理不校验维度类型。
- **修复**：NewArray 分支逐维度校验 `dk ∈ {int,long,short,byte}`，否则报
  `array size must be an integer expression`（文本/位置与 oracle 逐字节一致）。
- **验证**：`new int["hi"]` 干净拒绝（4:27 与 oracle 一致）；合法 `new int[5]`/
  `new int[5L]` 不受影响。
- **回归**：负测试 `tests/negative/array_size_type.myp`；全量 132 @test PASS /
  0 FAIL；oracle 对拍 95/0。
- **教训**：数组大小是整数表达式专有（不含 uint/float）——与下标（expectNumeric
  含 float）不同，别用 isNumKind 一刀切，须逐字段镜像 oracle 的精确集。

## BUG-074（已修复 🟩，v3.15.108）：布尔上下文漏 expectBool 校验（应拒绝却接受）

- **状态**：🟩 已修复（2026-08-26，v3.15.108，selfhost sema.myp）
- **背景**：续审查发现「应拒绝却接受」族——自举对布尔上下文完全不做
  `expectBool` 校验，`p && q`（int 操作数）/ `!p`（int）/ `p ? a : b`（int
  条件）/ `if (intExpr)` / `while (intExpr)` 全部静默当 bool（非零→true）。
  C++ oracle 四处 expectBool（`&&`/`||` 操作数、`!` 操作数、三元条件、
  if/while 条件）只允许 bool/bit，其余报 `expected boolean expression, got
  'X'`。手册 §三 明确要求整数判断写 `!= 0`——本应是编译错误。
- **根因**：自举 `&&`/`||`（comparison 分支）、Unary `!`、Ternary、If/While 的
  条件处理都不校验操作数 kind。
- **修复**：四处补 expectBool（仅 bool/bit）——①`&&`/`||` 操作数；②`!` 操作数；
  ③三元条件；④if/while 条件。消息用 `exprTypeName`（class/struct 解析类名）。
- **验证**：`p && q` / `!p` / `p ? 1 : 2` / `if(p)` / `while(p)` / `for(;int;)`
  均干净拒绝；合法 `b1 && !b2`（bit）、`if (boolVar)`、`while (b)`、标准
  `for (int i=0; i<n; i++)` 不受影响；自举编译器自身只用 `while(true)`，
  stdlib/examples/mypview 无 truthy-int 依赖（全量回归证明）。
- **回归**：负测试 `tests/negative/bool_context.myp`（EXPECT ERROR expected
  boolean expression, got 'int'）；全量 362 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：expectNumeric（下标/算术）与 expectBool（&&/! /?: /if/while）是
  oracle 两个高频校验族；自举此前只镜像了 expectNumeric（binary-op），漏了
  expectBool 全家。凡「oracle 有 expectBool」的位置逐一比对补齐。

## BUG-075（已修复 🟩，v3.15.109）：非数组基址下标漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-27，v3.15.109，selfhost sema.myp）
- **背景**：`5[0]` / `d[0]`（double 变量）/ `factory()[0]`（顶层函数返回 class）
  等非数组基址此前被自举 Subscript **静默当数组解析**（缺 C++ visitSubscript 的
  "cannot index non-array type" 检查）→ 若赋值类型恰好匹配（`int[] z = 5[0]` /
  `int[] z = factory()[0]`），codegen 对非指针做 GEP → **opt 崩**（"integer
  constant must have integer type" / "'%t16' defined with type 'i32' but expected
  'ptr'"）。仅 array/slice/string/bitvector 可下标（string→char、bitvector→bit）。
- **根因**：自举 Subscript 只查 index 类型（BUG-072），不查基址类型；基址 kind
  非数组时落到 `et = "array"` 默认继续。
- **修复**：按基址形态检查——①Call 且 callee 是 Identifier（顶层函数/内建）：
  用 `findFuncRetType` 判返回 array/slice/string（镜像元素解析 BUG-058 的取法）；
  ②其余（Identifier/Member/字面量/转换/New）按 resolvedKind 严格判；③方法调用
  基址（`arrs.get(0)[0]`）的 resolvedKind 是**元素 kind**（自举元素传播模型怪癖，
  ArrayList<int[]> 的 get → "int"）——保守放行，其元素类型由 BUG-058/062 机制
  决定。报 `cannot index non-array type 'X'`。
- **验证**：`5[0]`（byte）/`d[0]`（double）/`factory()[0]`（Circle）均干净拒绝，
  `5[0]` 文本与 oracle 逐字节一致（连 'byte' 都对）；合法 `arrs.get(0)[0]`、
  `makeInt()[i]`（顶层函数返 int[]）、`makeS()[i]`（返 slice）、`makeStr()[i]`
  （返 string）不受影响。
- **回归**：负测试 `tests/negative/subscript_nonarray.myp`（EXPECT ERROR cannot
  index non-array type 'byte'）；全量 363 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：下标基址校验不能只按 resolvedKind 一刀切——自举「Call/Subscript 基址
  resolvedKind = 元素 kind」的传播模型（BUG-058/062 族）会让 `arrs.get(0)` 的
  基址 kind 是 "int" 而非 "array"。顶层函数基址用 findFuncRetType 判返回类型，
  方法调用基址保守放行（其返回类型机制自带校验）。

## BUG-076（已修复 🟩，v3.15.110）：await timeout 类型漏校验（应拒绝却接受）

- **状态**：🟩 已修复（2026-08-27，v3.15.110，selfhost sema.myp）
- **背景**：`await T2.go timeout "x"`（string 当毫秒数）此前静默过 sema。C++
  visitAwaitExpr 对 timeout 双重校验（expectNumeric + `await timeout must be
  numeric (ms)`）。
- **根因**：自举 Await 有两处处理——**语句级**（sema.myp ~3975，@coro 方法体里
  的 `await ...` 语句走这条，timeout 在 s.timeout()）与**表达式级**（~5741，
  e.timeout()）——都只 visit timeout 不校验类型。只改表达式级不生效（语句级
  才是实际路径）。
- **修复**：两处都加——visit timeout 后 `isNumKind` 校验，非数字报
  `expected numeric type, got 'X'` + `await timeout must be numeric (ms)`（与
  oracle 逐字节一致，位置 7:33 对齐）。
- **验证**：`await T2.go timeout "x"` 干净拒绝（双消息）；合法
  `await T2.go timeout 30` 不受影响。
- **回归**：负测试 `tests/negative/await_timeout_type.myp`；全量 364 通过 /
  0 失败；oracle 对拍 95/0。
- **教训**：同一语法（await）在语句级与表达式级有两套 visit 处理，校验须同步
  两处——只改一处会「看似改了却不生效」（表达式级从未被语句 await 触发）。

## BUG-077（已修复 🟩，v3.15.111）：nonlocal 目标非外层变量漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-27，v3.15.111，selfhost sema.myp）
- **背景**：`nonlocal d`（d 是 lambda 参数）此前静默建 cell → codegen 取外层 cell
  得 ptr 当 i32 → **opt 崩**（"%t54 defined with type 'ptr' but expected 'i32'"）。
  C++ capture 解析（sema_expr.cpp:3586）报 `nonlocal 'd' does not resolve to an
  outer variable`。
- **根因**：自举 lambda 捕获收集（sema.myp ~5837）对 `nonlocal name;` 只收集不
  校验——lambda 参数/内部局部不是外层变量，仍建 cell。
- **修复**：nonlocalNames 收集循环加校验——目标在 lambda 的 params/locals（绑定名
  集合）→ 报 `nonlocal 'X' does not resolve to an outer variable`；否则若
  `sym_.lookup` 未声明 → 报 `nonlocal: undeclared variable 'X'`（镜像
  visitNonlocalStmt）。
- **验证**：`nonlocal d`（参数）干净拒绝；合法 `nonlocal k`（外层变量，
  manual_ch5 counter()）不受影响。
- **回归**：负测试 `tests/negative/nonlocal_param.myp`；全量 365 通过 / 0 失败；
  oracle 对拍 95/0。
- **教训**：lambda 捕获/闭包机制（captures/cells）也是「缺失校验 → codegen 崩」
  高发区——nonlocal 目标须是外层变量，自举 `locals` 集合（参数+内部局部+lambda
  名）正好是「非外层」判定集。

## BUG-078（已修复 🟩，v3.15.112）：void 参数漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-27，v3.15.112，selfhost sema.myp）
- **背景**：`void take(void v)` 的 void 参数此前静默过 sema → codegen 发
  `define internal void @T2_take(ptr %this, void %v)`（LLVM 非法）→ **opt 崩**
  （"void type only allowed for function results"，非干净诊断）。C++ sema.cpp
  （方法参数 + 结构体方法）报 `cannot declare parameter of type 'void'`。
- **根因**：自举 `declareParam`（参数声明）不校验 void 类型；字面 void 的
  AstType basicName = "void"（parser 2667），typeToKind → "void"。
- **修复**：`declareParam` 加校验——`typeToKind(pt)=="void" && basicName=="void"
  && className 空` → 报 `cannot declare parameter of type 'void'`（className 非空
  的未知类型不误伤）。
- **验证**：`void v` 参数干净拒绝；合法方法参数不受影响（全量回归证明）。
- **回归**：负测试 `tests/negative/void_param.myp`；全量 366 通过 / 0 失败；
  oracle 对拍 95/0。
- **教训**：parser 对字面 void 的表示是 basicName="void"（非空）——判定字面
  void 要看 basicName 而非「空」。内建/类型关键字做参数/字段时的校验（sema.cpp
  的 cannot declare … of type 'void' 系列）逐一比对。

## BUG-079（已修复 🟩，v3.15.113）：bitvector 移位量漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-27，v3.15.113，selfhost sema.myp）
- **背景**：`bitvector<8> v8; v8 << "x"`（string 移位量）此前静默过 sema → codegen
  把 ptr 当移位量 → **opt 崩**（"constant expression type mismatch: got type
  'ptr' but expected 'i8'"，非干净诊断）。C++ visitBinaryOp 要求 bitvector 移位
  量是整数或 bitvector。
- **根因**：自举 bitvector 移位特殊分支（sema.myp `(op=="<<"||">>") &&
  l=="bitvector"`）只设标志返回 "bitvector"，不校验右操作数类型（比通用数字
  移位路径早 return，跳过 "expected numeric type" 检查）。
- **修复**：该分支加 `isNumKind(r)==0 && r!="bitvector"` → 报 `bitvector shift
  requires a bitvector left operand and an integer or bitvector shift amount`
  （与 oracle 文本逐字节一致）。
- **验证**：`v8 << "x"` 干净拒绝；合法 `v8 << 2` / `v8 << 1L` 不受影响。
- **回归**：负测试 `tests/negative/bitvector_shift_type.myp`；全量 367 通过 /
  0 失败；oracle 对拍 95/0。
- **教训**：特殊类型（bitvector）的运算符路径常提前 return 绕过通用校验——凡
  oracle 对某类型组合有专有 error 分支（bitvector 比较/移位/位运算），自举对应
  分支都要补。

## BUG-080（已修复 🟩，v3.15.114）：解构目标类型不匹配漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-27，v3.15.114，selfhost sema.myp）
- **背景**：`(int a, string b) = t`（t 是 (int,int) 元组变量）此前静默过 sema →
  codegen string 槽存 int → **opt 崩**（"'%t11' defined with type 'i32' but
  expected 'ptr'" 在 myp_retain 处，非干净诊断）。C++ destructure walk 报
  `destructure: variable 'b' declared as 'string' but element is 'int'`。
- **根因**：自举解构只在「元组字面量赋值形态」校验类型（dv.kind()==Tuple &&
  !isDecl）；标识符元组变量（dv.kind()==Identifier）与调用返回元组、声明式解构
  都不校验。
- **修复**：新增 `destructureTupleElems(dv)`（元组字面量/元组变量/Call 返回元组
  三形态取元素 kind）+ `checkDestructureTypes`（递归 walk，声明式报 "declared as
  ... but element is ..."，赋值式报 "cannot assign ..."）+ `hasNestedDestructure`
  守卫（嵌套解构的扁平 kind 无法表示子 tuple，保持旧行为——tuple.myp 嵌套
  解构回归）。
- **验证**：`(int a,string b) = t` 干净拒绝（消息与 oracle 逐字节一致）；合法
  声明式 `(int v,bool ov) = checked(5)` / 赋值式 `(a,b) = (10,20)` 不受影响；
  嵌套解构 `((int p,int q),int z) = getNested()` 不受影响。
- **回归**：负测试 `tests/negative/destructure_type.myp`；全量 368 通过 / 0 失败；
  oracle 对拍 95/0。
- **教训**：解构校验要覆盖 rhs 全形态（字面量/变量/调用返回），不能只查字面量
  分支；嵌套解构需结构化 kind 表示，扁平列表会误判——保守守卫跳过嵌套。

## BUG-081（已修复 🟩，v3.15.115）：catch 类型漏校验（应拒绝却接受）

- **状态**：🟩 已修复（2026-08-27，v3.15.115，selfhost sema.myp）
- **背景**：`catch (int e)`（int 非类/接口）此前自举静默接受（catch 变量未用
  不崩，但「应拒绝却接受」须对齐 oracle）。C++ visitTryStmt 报 `catch type 'X'
  is not a class or interface`。
- **根因**：自举 Try 处理声明 catch 变量时不校验类型（empty/string 走兜底，
  其余按类型声明）。
- **修复**：Try 处理加校验——catch 类型非空且非 string/类/接口/struct（自举
  超集保留）→ 报 `catch type 'X' is not a class or interface`。
- **验证**：`catch (int e)` 干净拒绝；合法 `catch (e)` 兜底 / `catch (string s)`
  / `catch (ParseError e)` / `catch (Error e)` 接口不受影响。
- **回归**：负测试 `tests/negative/catch_type.myp`；全量 369 通过 / 0 失败；
  oracle 对拍 95/0。
- **教训**：异常机制（catch 变量类型）也是「应拒绝却接受」高发区——catch 类型
  只能 string/类/接口，其余是错误。

## BUG-082（已修复 🟩，v3.15.116）：const 属性赋值漏校验（应拒绝却接受）

- **状态**：🟩 已修复（2026-08-27，v3.15.116，selfhost sema.myp）
- **背景**：`const int cap = 100; cap = 200;`（const 属性赋值）此前自举静默改写
  （常量可变，语义错误）。C++ 报 `cannot assign to const property 'X'`。
- **根因**：自举 Assign 处理不检查赋值目标是否 const 属性（属性 const 标志
  prop.isConst 由 parser 353 行设置，但 sema 从不查询）。
- **修复**：新增 `classPropIsConst`（查 classProps_ 的 prop.isConst）+ `assignTargetConst`（裸名 / this.prop / 同类实例.prop 三形态）+ Assign 处理在类型检查前报
  `cannot assign to const property 'X'`。
- **防回归（重要）**：首版 assignTargetConst 的 obj.prop 分支未守卫
  `findClass(currentClass_)>=0`——struct 方法内 currentClass_ 是 struct 名（不在
  classIdx_）→ `classProps_.get(-1)` 越界 → **编译器段错误**（operators.myp 的
  r.x_ = ... 触发，exit 139）。重构：提前算 ci 并全局守卫，struct 字段赋值不受
  影响。
- **验证**：裸名/this.prop 的 const 属性赋值干净拒绝；合法非 const 属性赋值、
  struct 字段赋值（r.x_ = ...）不受影响。
- **回归**：负测试 `tests/negative/const_prop_assign.myp`；全量 370 通过 /
  0 失败；oracle 对拍 95/0。
- **教训**：新增校验引用类索引（classProps_/classIdx_）时必须守卫索引有效性——
  struct 方法里 currentClass_ 不是类，findClass 返回 -1 会导致 OOB 段错误；
  这在「自举编译器自身崩溃」上尤其隐蔽（stderr 段错误消息不进管道）。

## BUG-083（已修复 🟩，v3.15.117）：@async 非 @coro 上下文调用漏校验（应拒绝却接受）

- **状态**：🟩 已修复（2026-08-27，v3.15.117，selfhost sema.myp）
- **背景**：`@async` 方法/函数在非 @coro 上下文直接调用（`slowCalc(5)`）此前
  自举静默当普通调用（运行时阻塞回退）→「应拒绝却接受」。C++ visitCall 报
  `'@async' function can only be awaited inside an '@coro' method`。
- **根因**：自举完全不跟踪 @async 注解（AstAction/AstFunction 有 async()，parser
  257/512 设置，但 sema 从不查询）。
- **修复**：①新增 `asyncFuncNames_`（顶层 @async 函数）+ `asyncMethodKeys_`
  （cls.method，action/static/function）注册（4 处 addMethod/函数注册点）；②助手
  `isAsyncFunc`/`isAsyncMethod` + `checkAsyncCall(e, isAsync)`（非 @coro 上下文
  报错）；③调用点接线——顶层函数路径（findFuncRet 命中）、Member 方法路径、
  类内未限定方法路径（Path A，裸 `slowCalc(5)` 走这条）。
- **验证**：裸/this/obj 的 @async 调用在非 @coro 干净拒绝（消息与 oracle 逐字节
  一致）；`@coro` 内 `await slowCalc(5)` 不受影响；async_file/async_socket 等
  async 测试全过。
- **回归**：负测试 `tests/negative/async_method_outside_coro.myp`；全量 371 通过
  / 0 失败；oracle 对拍 95/0。
- **教训**：@async 是「注解有、sema 不查」类缺口——凡 C++ 有 isAsyncCallee/
  has_async 查询、自举 AstAction 有 async() 但从不使用的，都要镜像（跟
  @coro 的 coroNames_ 同构）。类内裸调用（未限定）也要覆盖三条调用路径。

## BUG-084（已修复 🟩，v3.15.118）：checkedAdd/checkedMul 实参类型漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-27，v3.15.118，selfhost sema.myp）
- **背景**：`checkedAdd(1.5, 2.5)`（浮点实参）此前静默过 sema → codegen 发非法
  内在签名 → **opt 崩**（"invalid intrinsic signature"，非干净诊断）。C++
  visitCheckedOp 校验实参为有符号整数（byte/short/int/long）。
- **根因**：自举 isNoVisitIntr 分支把 checkedAdd/checkedMul 直接设 ret="tuple"
  且**不访问实参**（拦截名单内建默认不 visit）——实参类型无从校验。
- **修复**：checkedAdd/checkedMul 分支改为访问 2 个实参并校验
  byte/short/int/long，否则报 `checkedAdd expects signed integer arguments
  (int/long/byte/short)`（C++ 文本镜像）；实参数 !=2 报 `takes exactly two
  signed integer arguments`。setArgsVisited(1) 防尾部二次访问。
- **验证**：`checkedAdd(1.5,2.5)` 双实参均干净拒绝；合法
  `checkedAdd(2147483647,1)`（溢出 ov=true）、`checkedMul(100000L,200000L)`
  不受影响（2/2 断言）。
- **回归**：负测试 `tests/negative/checked_op_type.myp`；全量 372 通过 /
  0 失败；oracle 对拍 95/0。
- **教训**：isNoVisitIntr（拦截名单内建不访问实参）是校验盲区——凡内建需要
  校验实参类型的，不能走「不 visit」捷径；checked 溢出族（visitCheckedOp）
  是典型。

## BUG-085（已修复 🟩，v3.15.119）：重复变量声明漏校验（应拒绝却接受）

- **状态**：🟩 已修复（2026-08-27，v3.15.119，selfhost sema.myp）
- **背景**：`int x = 5; int x = 7;`（同作用域重复声明）此前自举静默 last-wins
  shadow（编译运行 x=7）。C++ visitVarDecl 的 lookup 沿作用域链判重复——MYP 无
  shadow 规则（连嵌套 shadow 也拒）。
- **根因**：自举 VarDecl 不查重复；且**循环变量不弹作用域**（符号表 last-wins），
  顺序 `for (int i){} for (int i){}` 与循环后 `int i` 复用全靠 shadow。
- **修复**：①VarDecl 加重复检查——`sym_.lookup(name)` 非空 → 报 `duplicate
  variable 'X'`（`_` 忽略符可重复，tuple_ignore）；②For 处理包
  enterScope/leaveScope（镜像 oracle 循环变量作用域弹出）——修复后顺序循环/
  循环后复用不误报，且嵌套 shadow 正确拒。
- **防回归（重要）**：首版无 for 作用域，自举源码本身有顺序同名循环变量
  （codegen.myp hasVar 两个 `for (int i)`）→ bootstrap 崩（duplicate variable
  'i'）；曾用 inForInit_ 豁免（partial），后改为**正确 for 作用域**根治。
- **验证**：同作用域重复 / 嵌套 shadow 干净拒绝（文本与 oracle 逐字节一致）；
  顺序 for 循环变量、循环后复用、`_` 忽略符、@parallel for 循环后复用均不受
  影响。
- **回归**：负测试 `tests/negative/duplicate_var.myp`；全量 373 通过 / 0 失败；
  oracle 对拍 95/0。
- **教训**：重复变量检查与「循环变量作用域」强耦合——必须先让 for 弹作用域
  再查重复，否则自举源码自身的顺序同名循环变量会误报。用豁免（inForInit_）
  是 partial 且漏 `for (int i){} int i` 复用；正确解是作用域。

## BUG-086（已修复 🟩，v3.15.120）：for-in 迭代不可迭代对象漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-27，v3.15.120，selfhost sema.myp）
- **背景**：`for (int x in 5)` / `for (char c in strVar)`（不可迭代对象）此前
  自举静默过 → 循环变量未声明 → codegen 找不到 → **opt 崩**（"expected value
  token"，非干净诊断）。C++ 镜像：不可迭代 → `cannot iterate over type 'X'`；
  动态数组 → `cannot iterate a dynamic array 'X' (no runtime length); use
  slice<T> or a collection class`。
- **根因**：自举 ForIn 处理对不可迭代对象只 fallback 访问 body（elemType 保持
  void），不报错、不声明循环变量。
- **修复**：ForIn 末尾补校验——①动态数组标识符（arraySize()<=0）→ 专门消息
  （for-in 只支持定长数组/slice/集合）；②其余不可迭代（int/string/无 get 的
  class）→ `cannot iterate over type 'X'`（resolvedKind 为 void 时跳过防级联）。
- **验证**：`for (int x in 5)`（byte）/`for (char c in "hi")`（string）/
  动态数组均干净拒绝（文本与 oracle 逐字节一致）；定长数组/slice<T>/范围 for-in
  不受影响。
- **回归**：负测试 `tests/negative/forin_iterable.myp`；全量 374 通过 /
  0 失败；oracle 对拍 95/0。
- **教训**：ForIn 是独立语句处理，容易漏「不可迭代」校验（fallback 直接访问
  body 而不报错）——镜像 C++ 的 iterKind 判定（定长数组/slice/集合/其余）。
  动态数组是专门消息，别混进 "cannot iterate over type"。

## BUG-087（已修复 🟩，v3.15.121）：throw 类型/裸重抛上下文漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-27，v3.15.121，selfhost sema.myp）
- **背景**：①`throw 5`（非 string/类/接口表达式）此前自举静默接受 → codegen
  生成 `myp_throw_object(integer)` 非法 IR → **opt-21 崩**（"integer constant
  must have integer type"）；②catch 外裸 `throw;`（重抛）静默接受 → 运行时
  行为未定义。C++ visitThrowStmt 镜像：非 string/类 → `throw requires a
  string or class instance, got 'X'`；`throw;` 在 in_catch_depth_==0 时 →
  `'throw;' rethrow is only valid inside a catch block`；void 表达式仅在已报错
  时静默（级联恢复）。
- **根因**：自举 Throw 处理只映射 throw_type（rethrow/string/类名）、**从不报错**；
  且无 catch 深度计数（oracle 有 in_catch_depth_，catch 块前后 ++/--）。
- **修复**：①新增 `inCatchDepth_` 字段（构造器初始化 0），Try 处理在访问每个
  catch 块前 +1、后 -1；②Throw 处理补两类校验——`throw;` 且 inCatchDepth_==0
  → 报错；非 string/class/interface 表达式 → 报错（void 用 errorCount() 判
  级联恢复，与 oracle diag_.hasErrors() 一致）。
- **验证**：`throw 5`（'byte'，文本与 oracle 逐字节一致）/catch 外 `throw;`
  干净拒绝；`throw "msg"`/`throw e`（类实例）/catch 内裸重抛（嵌套 try 传播
  "outer caught: inner"）均不受影响。
- **回归**：负测试 `tests/negative/throw_type.myp` + `tests/negative/throw_rethrow_outside.myp`；
  全量 377 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：语句处理若「只设标志不报错」就是校验盲点——Throw 之前只填
  throw_type 给 codegen，从未做 oracle 有的类型/上下文校验。裸重抛的 catch
  上下文用深度计数（++/--）而非布尔，支持嵌套 catch。

## BUG-088（已修复 🟩，v3.15.122）：non-void 函数缺 return 漏校验（应拒绝却接受）

- **状态**：🟩 已修复（2026-08-27，v3.15.122，selfhost sema.myp）
- **背景**：`int ret(){ int x=1; }`（non-void 函数体不保证终止）与
  `int bare(){ return; }`（裸 return 无值）此前自举静默接受 → codegen 发
  `ret void` 于 i32 函数 → 运行时未定义（返回垃圾值）。C++ 镜像：①
  checkMissingReturn + stmtGuaranteesTermination → `missing return statement
  (function expects 'X')`（空体 FFI 桩豁免）；②visitReturnStmt 裸 return →
  `missing return value (function expects 'X')`。
- **根因**：自举 Return 处理只查有值分支的类型兼容，无值分支（裸 return）不
  查；函数/方法体访问完后从不做「是否可能落空到末尾」分析（oracle 在 action/
  function/static action/top-level/struct 方法 5 处体末尾调 checkMissingReturn）。
- **修复**：①移植 stmtGuaranteesTermination（Return/Throw 终止；Block 看末
  语句；If 须 else 且双分支终止；Match 须全臂终止；Try 的 try 或 finally 终止；
  while(true)/while(非0字面量)/for(;;) 恒终止）；②新增 checkMissingReturn
  （non-void + 不保证终止 + 非空体 → 报错），接到 5 处方法体访问点（顶层函数/
  类 action/类 func/static action/struct 方法，仅非泛型 visitStmt 路径）；
  ③Return 处理补无值分支校验（裸 return 于 non-void → missing return value）。
- **验证**：缺 return/裸 return 干净拒绝（文本与 oracle 一致）；末尾 return/
  if+else 双 return/while(true)/for(;;)/空体 FFI 桩均不受影响；bootstrap
  自举成立（自举源码无 false positive）。
- **回归**：负测试 `tests/negative/missing_return.myp` +
  `tests/negative/bare_return.myp`；全量 377 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：函数体访问完是校验末尾 return 的天然钩子——oracle 在 5 处体末尾
  调 checkMissingReturn，自举此前完全没有「终止性分析」概念。空体 FFI 桩豁免
  是关键（extern 声明函数无 body return 合法）。

## BUG-089（已修复 🟩，v3.15.123）：for-in 变量类型不匹配漏校验（应拒绝却接受）

- **状态**：🟩 已修复（2026-08-27，v3.15.123，selfhost sema.myp）
- **背景**：`for (string s in int[2] arr)`（显式循环变量类型与元素类型不匹配）
  此前自举静默接受 → 循环变量按 string 声明、迭代给 int → codegen 存 int 入
  string 槽 → 垃圾/opt 崩。C++ visitForInStmt 镜像：`for-in variable type 'X'
  does not match element type 'Y'`（typesCompatible 校验：同 kind/class 同名/
  Int↔Long/接口←类）。
- **根因**：自举 ForIn 声明循环变量时用显式类型（loopHasType → typeToKind），
  但从不校验其与 elemType 兼容。
- **修复**：ForIn 声明前补校验——class 元素（elemType 带 "class:" 前缀）比类名
  （同名 或 接口变量←元素类，inInterface 保守放行）；非 class 元素比 kind
  （同 kind 或 Int↔Long 或 bit↔bool，镜像 oracle typesCompatible）。位置在
  sym_.enterScope() 之后、declare 之前。
- **验证**：`for (string s in int[2] arr)` 干净拒绝（文本与 oracle 一致）；
  匹配/Int↔Long/范围 for-in/同名类/接口变量均不受影响；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/forin_var_type.myp`；全量 379 通过 /
  0 失败；oracle 对拍 95/0。
- **教训**：for-in 循环变量显式类型是独立校验点（has_type 分支）；class 元素
  的 elemType 带 "class:" 前缀须剥前缀再比类名，接口←类须保守放行防误伤
  （inInterface 泛判，不逐类查 implements）。

## BUG-090（已修复 🟩，v3.15.124）：slice 构造/成员调用参数漏校验（应拒绝却接受）

- **状态**：🟩 已修复（2026-08-27，v3.15.124，selfhost sema.myp）
- **背景**：①`slice<int,int>`（两个类型参数）②`new slice<int>(4,5)`（两个
  size）③`new slice<string>("abc")`（size 非整数）④`s.data(2)`（slice 成员带
  参数）此前自举静默接受 → codegen 按单元素/size 生成 → 垃圾/错配。C++
  visitNewExpr 镜像：`slice requires exactly one type argument: slice<T>` /
  `slice requires exactly one size argument: new slice<T>(n)` / `slice size
  must be an integer expression`；visitCall 镜像 `slice size()/length() takes
  no arguments` / `slice data() takes no arguments`。
- **根因**：自举 New slice 分支只 visit 全部实参设 slice 返回，不查类型/实参
  个数与 size 整型性；slice 成员 .size()/.length()/.data() 分支只设返回类型不
  查实参数。
- **修复**：①New slice 加 3 项校验（typeArgs==1 / args==1 / size 为
  int/long/short/byte，镜像 oracle visitNewExpr）；②slice 成员 .size/.length/
  .data 两处分支（Identifier 基 + Call 返回 slice 基 BUG-059）加实参数为 0
  校验。
- **验证**：四种非法形态干净拒绝（文本与 oracle 一致）；`new slice<int>(3)` /
  `s.size()` / `s.data()` / `s.length()` 均不受影响；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/slice_type_args.myp` +
  `tests/negative/slice_size_args.myp`；全量 380 通过 / 0 失败；oracle 对拍
  95/0。
- **教训**：slice 是内建「伪类」——其构造与成员调用都是内建拦截（不落类方法
  表），校验须在拦截点显式写；实参/类型实参个数 + 参数类型是常漏三项。

## BUG-091（已修复 🟩，v3.15.125）：内建实参校验缺失 → opt 崩（parse/位操作/bytes 族）

- **状态**：🟩 已修复（2026-08-27，v3.15.125，selfhost sema.myp）
- **背景**：isNoVisitIntr 拦截名单内建（不 visit callee/实参）中 parse 族 /
  位操作族 / bytes 族 / load4-store4 无实参校验 → 错参静默过 → codegen 发非法
  内在签名 → **opt 崩**：
  - `parseInt(123)` → "integer constant must have integer type"
  - `popcount(1.5)` → "invalid intrinsic signature"
  - `rotl(5,"x")` → "got type 'ptr' but expected 'i32'"
  - `bytesOf("hi",2)` / `bytesOf("hi")` → "got type 'ptr' but expected 'i64'"
  C++ 镜像：visitParse/visitParseOpt（string 实参）、visitBitOps（整型实参 +
  移位量整型）、visitBytesStr/visitBytesOf（恰一实参 + 类型匹配回落普通解析）、
  visitVec4Access（load4/store4）。
- **根因**：isNoVisitIntr 分支对 parse/bytes 族只设返回类型不 visit 实参（与
  BUG-084 checkedAdd 同构的校验盲区）；位操作族 visit 实参但不校验类型。
- **修复**：①parse 族加「恰一实参 + string 类型」校验（parseIntOpt 特
  "takes exactly one string argument"）；②位操作族加实参数与整型校验（一元
  popcount/clz/ctz/bitreverse=1、二元 rotl/rotr=2；非整型 → expects an
  integer argument / shift amount must be an integer）；③bytes/bytesOf 加
  「恰一实参 + 类型匹配（bytes←string，bytesOf←bitvector）否则回落普通函数
  解析」；str 加「恰一实参 + 收数组否则回落」；④load4/store4 加
  float[] 首参/索引整型/float4 三参校验。
- **验证**：各错参形态干净拒绝；parseInt("123")/popcount(255)/rotl(1,3)/
  bytes("s") 合法用编译+运行正确；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/parse_type.myp` + `bitop_type.myp` +
  `bytesof_args.myp`；全量 382 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：isNoVisitIntr（拦截名单内建不 visit 实参）是校验盲区——凡内建收
  特定类型实参，必须在该分支显式 visit+校验；类型不匹配的「回落普通函数解析」
  （非崩溃 undefined）也要镜像，否则自举会把非法参数当内建 → codegen 崩。

## BUG-092（已修复 🟩，v3.15.126）：pipe 目标无 transform / lhs 类型不兼容漏校验

- **状态**：🟩 已修复（2026-08-27，v3.15.126，selfhost sema.myp）
- **背景**：`5 |> Foo`（目标类无 transform action）与 `5 |> Foo.helper`
  （MemberAccess 非算子）此前自举静默透传 lhs → 语义错（运行 a=5 而非报错）；
  `5 |> ScaleOp`（ScaleOp.transform(double[]) 与 int lhs 不兼容）同样静默透传。
  C++ visitPipe 镜像：`pipe '|>' requires an operator component with a
  single-argument 'transform' method` / `pipe: cannot apply 'X.transform' to
  operand of type 'Y'`。
- **根因**：自举 pipe 处理只解析目标类名/实例、查 findMethodRet（返回类型），
  transform 未找到或类型不兼容时静默保留 lhs 类型继续——没有「无 transform 即
  非法算子」的拒绝，也没有 lhs/形参兼容校验。
- **修复**：①目标类/实例但无 1 参 transform action → 报 requires ... single-
  argument 'transform' method；②目标非类名/类实例（MemberAccess 等）→ 同一报
  错；③transform 找到时校验 lhs 与形参类型兼容（typesCompat），不兼容 →
  pipe: cannot apply。
- **验证**：无 transform/MemberAccess/类型不兼容三形态干净拒绝；`double[] A |>
  ScaleOp` / 实例管道 / 链式均不受影响（pipe 回归 PASS）；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/pipe_no_transform.myp` +
  `tests/negative/pipe_type_mismatch.myp`；全量 385 通过 / 0 失败；oracle 对拍
  95/0。
- **教训**：pipe 是表达式级运算符——目标非法时须显式报错而非「保留 lhs 静默
  透传」；transform 的 1 参限定 + lhs/形参兼容是 C++ findTransform 的两道校验，
  自举此前全漏。

## BUG-093（已修复 🟩，v3.15.127）：var 推断元组 / 直接调用元组成员访问缺失

- **状态**：🟩 已修复（2026-08-27，v3.15.127，selfhost sema.myp）
- **背景**：`var r = pair()`（pair 返回 (int,bool)）后 `r.0`，以及 `pair().0`
  直接成员访问——oracle 均正确支持（v=42 v2=42），自举此前**拒绝合法代码**：
  var 推断元组无 tupleTypes → `r.0` 解析 void → "cannot initialize ... with
  value of type 'void'"；元组成员访问只处理 Identifier 基 → `pair().0` 同样
  void。tuple.myp 用显式类型 `(int,int) t` 所以从未暴露。
- **根因**：①VarDecl 的 var 推断分支对元组只 `sym_.declare(name,"tuple")`（不带
  tupleTypes；显式类型路径 declareTuple 正常）；②元组成员访问 `.N` 的
  isIntegerStr 分支只查 `arr.kind()=="Identifier"` 的符号表 tupleTypes，Call
  基不处理。
- **修复**：①var 推断元组分支：init 为 Call（Identifier callee）→
  findFuncRetType 取返回 tuple 的 funcParamTypes → declareTuple（带元素类型）；
  其他形态回落原 declare（sema 干净拒）；②元组成员访问加 Call 基分支（callee
  Identifier → findFuncRetType → isTuple → 取元素 kind；越界报
  "tuple index N out of range"）。
- **防回归**：tuple 字面量 var（`var tl = (7,false)`）codegen 布局未支持（opt
  崩 i32/i8）→ 保持回落（sema 拒，不声明 tupleTypes），仅 Call 形态修复；
  显式类型/`var r = pair()`/`pair().0` 全部正常。
- **验证**：`var r = pair(); r.0/r.1`、`pair().0/.1`、`tri().1`、显式类型均编译
  运行正确；越界 `pair().5` 干净拒；bootstrap 自举成立。
- **回归**：正测试 `tests/@test/tuple_var_infer.myp`（1 test / 6 断言）；全量
  388 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：与「应拒却接受」相反——这是「oracle 接受、自举拒绝」的功能缺口，
  同属 parity 审计范围。var 推断与显式类型两条声明路径必须一致（declareTuple）；
  成员访问的基表达式形态（Identifier/Call）要全覆盖。

## BUG-094（已修复 🟩，v3.15.128）：bitfield 重复名/字段漏校验（应拒绝却接受）

- **状态**：🟩 已修复（2026-08-27，v3.15.128，selfhost sema.myp）
- **背景**：`bitfield Flags { bit a; bit a; }`（bitfield 内重复字段）与两次
  `bitfield Flags { ... }`（重复声明名）此前自举静默 last-wins → 字段访问取
  最后定义（位偏移错）→ 语义错。C++ 镜像：declareBitfieldName →
  `duplicate bitfield name 'X'`；visitBitfieldDecl → `duplicate bitfield field
  'X' in 'Y'`。
- **根因**：自举 bitfield 注册（tu.bitfields 收集循环）只收集名/字段/宽度，
  从不查重复（对比 struct/class/function 的 duplicate 检查均已有）。
- **修复**：收集循环加两道校验——①bitfield 名与已收集的 bitfieldNames_ 比对
  → duplicate bitfield name；②字段名与当前 bitfield 已收集的 names 比对 →
  duplicate bitfield field。位置：无位置字段（AstBitfield/AstBitfieldField
  不存 line/col）→ 诊断 0,0（同 pipe 处理）。
- **验证**：重复名/重复字段干净拒绝；合法 bitfield（bitfield.myp 回归）不受
  影响；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/bitfield_dup_field.myp` +
  `tests/negative/bitfield_dup_name.myp`；全量 390 通过 / 0 失败；oracle 对拍
  95/0。
- **教训**：声明收集循环（bitfield/struct/class/function）是重复校验的高频
  遗漏点——凡 oracle 有 duplicate 分支、自举收集处无比对，即「应拒却接受」；
  AstBitfield 无位置字段，诊断用 0,0（位置精度低但拒绝语义正确）。

## BUG-095（已修复 🟩，v3.15.129）：类内重复 action/event/function/struct 方法漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-27，v3.15.129，selfhost sema.myp）
- **背景**：`int go(){...} int go(){...}`（类内同名 action）此前自举静默接受 →
  codegen 重定义同一 LLVM 函数 → **opt-21 崩**（"invalid redefinition of
  function 'T2_go'"）。同模式的类内 function/event/struct 方法重复也静默
  last-wins。C++ 镜像：`duplicate action 'X' in class 'Y'` /
  `duplicate function 'X' in class 'Y'` / `duplicate event 'X' in class 'Y'` /
  `duplicate method 'X' in struct 'Y'`。顶层函数重复已有（duplicate function
  name）；static action 由 BUG-046 单独规则（签名不同才报）。
- **根因**：自举 addMethod/方法注册只 `methodSigIdx_.put`（key 已存在则跳过），
  从不报重复——对比顶层函数、struct/class 名重复检查均有，类方法族全漏。
- **修复**：四处注册点加重复检查（methodSigIdx_ 已含同名即重）：①类 action（
  ctor 豁免——同名构造器重载合法）；②类 function；③类 event（AstEvent 无位置
  字段 → 0,0）；④struct 方法（ctor 豁免——与 struct 同名构造器）。
- **防回归**：struct 构造器方法（与 struct 同名）首版误伤 constructor.myp →
  ctor 豁免修复。自举源码无重复 → bootstrap 成立。
- **验证**：重复 action/event/struct 方法干净拒绝（文本与 oracle 一致）；构造器
  重载（Box()/Box(int)）/合法 action 不受影响；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/duplicate_action.myp` +
  `tests/negative/duplicate_event.myp`；全量 390 通过 / 0 失败；oracle 对拍
  95/0。
- **教训**：类方法注册（actions/functions/events/struct methods）是 duplicate
  校验重灾区——oracle 用符号表 declare 返回值判重（类作用域单空间，同名即重），
  自举方法注册全用「key 存在则跳过」的静默 put。构造器（类+struct）是豁免点，
  同名重载合法。

## BUG-096（已修复 🟩，v3.15.130）：重复 interface 名漏校验（应拒绝却接受）

- **状态**：🟩 已修复（2026-08-27，v3.15.130，selfhost sema.myp）
- **背景**：两次 `interface I1 { ... }`（重复声明名）此前自举静默 last-wins →
  接口二义。C++ visitInterfaceDecl 镜像：`duplicate interface name 'X'`。
  对比：重复 class/enum 自举已拒（duplicate class name / duplicate enum name）；
  重复 struct oracle 用 declared_struct_names_ 静默跳过（非硬错）→ 自举保持
  一致不报。
- **根因**：自举 interface 收集（tu.interfaces 循环）无条件 interfaceNames_.add，
  从不查重复——class/enum 有 duplicate 检查、interface 漏。
- **修复**：interface 收集循环加重复检查（与已收集的 interfaceNames_ 比对 →
  duplicate interface name）。AstInterface 无位置字段 → 诊断 0,0。
- **验证**：重复 interface 干净拒绝；合法 interface（manual_ch6_class 回归）不
  受影响；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/duplicate_interface.myp`；全量 392 通过 /
  0 失败；oracle 对拍 95/0。
- **教训**：声明收集循环的 duplicate 校验覆盖不均——class/enum 有、interface/
  bitfield 漏（bitfield 已 BUG-094）。凡 oracle 有 duplicate 分支、自举收集处
  无比对，即「应拒却接受」。

## BUG-097（已修复 🟩，v3.15.131）：集合类缺 get/size 或元素是数组漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-27，v3.15.131，selfhost sema.myp）
- **背景**：`for (int x in ng)`（ng 是只有 size() 无 get(int) 的类）此前自举
  静默 → codegen 发 undefined @NoGet_get → **opt-21 崩**（"use of undefined
  value '@NoGet_get'"）。集合 get(int) 返回数组（`int[] get(int)`）时也静默 →
  循环变量按数组类型声明/迭代错配。C++ visitForInStmt 镜像：`'X' is not
  iterable: requires size() and get(int) methods` / `cannot iterate a
  collection whose element is an array 'X'; wrap it in a class or use slice<T>`。
- **根因**：自举 ForIn class 路径只查 `findMethodRetAst("get")`（无 get 时
  getType 空、elemType 保持 void）但**不报错**——靠下游 generic "cannot
  iterate over type" 分支兜底，而该分支因 iterable.resolvedKind() 解析时序未
  触发 → 无 sema 错 → codegen 崩。
- **修复**：ForIn class 路径改显式校验——findMethodParams 查 get(int)（1 参）
  + size()（0 参），缺任一 → "'X' is not iterable: requires size() and get(int)
  methods"；get 返回 array → "cannot iterate a collection whose element is an
  array"（置 elemType=void 防声明循环变量）。新增 iterReported 标志抑制
  generic "cannot iterate over type" 级联。
- **验证**：缺 get/元素数组干净拒绝（单错）；ArrayList<int> 等合法集合 for-in
  不受影响（for_in/collections_chain 回归 PASS）；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/forin_no_get.myp` +
  `tests/negative/forin_array_elem.myp`；全量 393 通过 / 0 失败；oracle 对拍
  95/0。
- **教训**：ForIn 集合类路径的「缺 get/size」此前靠 generic 兜底（BUG-086），
  但兜底有解析时序盲区（resolvedKind 未及时设）→ 静默 → codegen 崩。凡 oracle
  有专门消息（is not iterable + 原因 / element is an array），自举须显式发而非
  依赖 generic。

## BUG-098（已修复 🟩，v3.15.132）：`var x;` 无初始化器漏校验（应拒绝却接受）

- **状态**：🟩 已修复（2026-08-27，v3.15.132，selfhost sema.myp）
- **背景**：`var x;`（无初始化器）此前自举静默当 `int x = 0`（isInferred 但无
  init → it 保持 void、按默认 int 声明）→ 语义错。C++ visitVarDecl 镜像：
  `'var' declaration requires an initializer`。
- **根因**：自举 VarDecl 处理对 isInferred 但 init==null 不设防——BUG-016 注释
  明言「无 init 由 C++ 拒绝」但自举未实现。
- **修复**：VarDecl 处理在推断前加校验——`v.isInferred()!=0 && v.init()==null`
  → "'var' declaration requires an initializer"（continue 不声明变量，后续引用
  报 undefined symbol，与 oracle 级联一致）。
- **验证**：`var x;` 干净拒绝（含 undefined symbol 级联，与 oracle 一致）；
  `var x = 5;` / `var s = "hi";` 不受影响；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/var_no_init.myp`；全量 395 通过 / 0 失败；
  oracle 对拍 95/0。
- **教训**：注释标注的「oracle 行为自举未实现」就是待办缺口——凡 C++ 有
  is_inferred && !init 分支、自举有 isInferred() 但不用，即「应拒却接受」。

## BUG-099（已修复 🟩，v3.15.133）：嵌套解构 arity 漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-27，v3.15.133，selfhost sema.myp）
- **背景**：`((int a,int b,int d),int c) = getNested()`（getNested 返回
  ((int,int),int)，内层值 2 元素绑 3 个）此前自举静默过 → codegen
  extractvalue 越界 → **opt-21 崩**（"invalid indices for extractvalue"）。
  C++ destructure walk 镜像：`destructure: nested tuple arity mismatch` /
  `destructure: expected a nested tuple here`。
- **根因**：BUG-080 用 hasNestedDestructure 豁免嵌套解构（扁平 kind 列表无法
  表示子 tuple）→ 嵌套层 arity/结构完全不校验；顶层 arity 检查只比
  target.elements().size() vs 值外层元素数，内层不查。
- **修复**：新增 checkNestedDestructureArity（递归 walk：嵌套节点值须 tuple 且
  arity 与 target 元素数一致；reported 防跨层重复报），接入 Destructure 处理
  ——Call rhs（用 findFuncRetType 的 frt 嵌套 AstType）与元组字面量 rhs（用
  dv.elements() 嵌套 expr）两形态，且仅在顶层 arity 匹配后才走（避免与顶层
  "tuple has N" 重复）。
- **验证**：嵌套 arity 错（Call + 字面量）干净拒；合法嵌套解构
  （tuple.myp 25 行 ((int p,int q),int z)）不受影响；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/destructure_nested_arity.myp`；全量 395 通过
  / 0 失败；oracle 对拍 95/0。
- **教训**：BUG-080 的「嵌套豁免」是权宜——只豁免了扁平类型检查，但**结构
  arity**（每层元素数）仍须镜像 oracle 的递归 walk；豁免导致嵌套 arity 错 →
  codegen 越界 → opt 崩。凡 C++ 有递归 walk（嵌套 arity/结构校验），自举不能
  整体豁免，须分层实现。

## BUG-100（已修复 🟩，v3.15.134）：函数类型实参签名/裸函数名漏校验 → opt 崩

- **状态**：🟩 已修复（2026-08-27，v3.15.134，selfhost sema.myp + ast.myp）
- **背景**：`apply(strFn, 5)`（apply 形参 (int)->int，strFn 是 (string)->int）与
  `apply(dbl, 5)`（dbl 是匹配的裸函数名）此前自举 sema 放行 → codegen 生成
  非法闭包/函数指针 IR → **opt-21 崩**（"defined with type 'i32' but expected
  '{ ptr, ptr }'"）。C++ 镜像：typesCompatible 的 Function 结构性签名比较 →
  `argument 1: expected '(int) -> int', got '(string) -> int'`。
- **根因**：自举 typesCompat 对 kind 字符串 "function"=="function" 恒放行
  （无签名比较）；函数类型实参路径（lambda 除外）从不校验签名。且自举 codegen
  不支持「裸注册函数名 → 闭包」转换（仅 lambda / 函数类型变量受支持）→ 即使
  签名匹配也崩。
- **修复**：①AstExpr 增 callParamFuncSig_（每函数类型形参 [ret, p1...]），4 处
  AstParam 型 setCallParamTypes 同步设置；②arg-check 处当 pt=="function" &&
  at=="function" 时：裸注册函数名（funcIdx_ 有、无 declareFunc 符号）→ 拒
  "cannot use function name 'X' as a value; wrap it in a lambda"（防 codegen
  崩）；函数类型变量/lambda → argFuncSigOf + sigsMatch 签名比较（expected '(T)
  -> R', got ...）。
- **验证**：裸函数名（匹配/不匹配）/函数类型变量不匹配干净拒绝；lambda/匹配
  函数类型变量编译+运行正确（A r=10 / B r2=13）；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/fntype_bare_fn.myp` +
  `tests/negative/fntype_var_mismatch.myp`；全量 397 通过 / 0 失败；oracle 对拍
  95/0。
- **教训**：函数类型是一等值但自举 codegen 只支持 lambda / 函数类型变量作值，
  不支持裸函数名→闭包（seed 也拒）——「oracle 有 Function 结构性签名比较、自举
  kind 二分」是漏检；同时 codegen 能力边界（裸函数名）须在 sema 干净拒绝而非
  opt 崩。

## BUG-101（已修复 🟩，v3.15.135）：long→double / int↔long 构造器提升漏（拒合法代码）

- **状态**：🟩 已修复（2026-08-27，v3.15.135，selfhost sema.myp）
- **背景**：`double a = 1L;`（long→double 变量/实参）与 `new BoxD(1L)`（long→
  double 构造器）、`new BoxI(1L)`（int↔long 构造器）——oracle 均接受，自举此前
  **拒绝合法代码**（"cannot initialize ... / argument ... expected 'double' /
  no matching constructor for 'Box(long)'"）。
- **根因**：①`promotesTo` 的 long 分支恒 0——long→double 未视为合法宽化提升
  （byte/short/int→double 都有，long 漏）；②`ctorArgCompat` 只走
  `promotesTo(actual→parameter)`，无 int↔long 双向对称（typesCompat 有
  Int↔Long 特例）→ `new Box(int 构造)(1L)` 拒。
- **修复**：①`promotesTo(long→double)`=1（long 是 64 位整数宽化到 double）；
  ②`ctorArgCompat` promotesTo 失败时回落 typesCompat（覆盖 int↔long 双向）。
  歧义构造器（Box(int)+Box(double) 对 1L/byte 双可行）仍拒（自举 "no matching"
  / oracle "ambiguous"，消息不同但双端拒）。
- **验证**：`double a=1L` / `f(1L)` / `new BoxD(1L)` / `new BoxI(1L)` / byte→
  int/double 提升全部编译+运行正确；歧义双构造器仍拒；bootstrap 自举成立。
- **回归**：正测试 `tests/@test/ctor_promotion.myp`（2 tests / 8 断言）；全量
  400 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：promotesTo 数值提升表要逐源类型核对（byte/short/int/float 都有→
  double，long 漏）；ctorArgCompat 与 typesCompat 的提升规则须一致（int↔long
  双向在 typesCompat 有特例、ctor 路径漏）——「oracle 接受、自举拒绝」同属
  parity 审计。

## BUG-102（已修复 🟩，v3.15.136）：bitvector 比较/位运算/移位量缺同宽校验
（应拒却接受）

**非破坏性**（selfhost sema）。`bitvector<8> == bitvector<16>`、`bitvector<8> &
bitvector<16>`、`bitvector<8> << bitvector<16>` 自举均接受并运行（非崩溃），但
oracle 当前源（sema_expr.cpp visitBinaryOp）全部拒绝——「应拒却接受」的语义严格性
差距（非 opt 崩）。

- **根因**：比较分支直接返回 "bool" 不查 bitvector 宽度；`&`/`|`/`^` 分支
  "bitvector" 无同宽校验；`<<`/`>>` 只有 BUG-079 的移位量类型检查、无同宽检查。
  此前无「bitvector 变量宽度」记录，无法比对。
- **修复**：①类字段 `StrHashMap<int> bitvectorWidths_`（名→N）+ 构造器初始化；
  ②var 声明处（BUG-007 宽度合法性校验通过后）`bitvectorWidths_.put(v.name(), bw)`；
  ③helper `bitvectorWidthOf(AstExpr)`：Convert toKind=="bitvector" 取节点 `bw()`
  （`bitvector<N>(x)` 语法走 Convert，parser 已 setBw），Identifier 查
  bitvectorWidths_，否则 0；④比较分支：任一侧 bitvector 则双端必须同宽；
  ⑤`&`/`|`/`^`：双端同宽；⑥`<<`/`>>`：移位量为 bitvector 时与左操作数同宽。
- **验证**：异宽比较/位运算/移位三形态全部 `sev=error`；同宽
  `bitvector<8>==bitvector<8>` / `&` / `<<` 编译+运行正确；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/bitvector_comp_width.myp` /
  `bitvector_bitwise_width.myp` / `bitvector_shift_width.myp`（各 EXPECT ERROR 子串）；
  全量 403 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：bitvector 运算（比较/位运算/移位量）同宽校验依赖「变量宽度追踪」——
  不能只靠 Convert 节点 bw；类型名只到 "bitvector"，宽度信息要另建映射表。

## BUG-103（已修复 🟩，v3.15.137）：函数类型实参数量漏校验 → opt 崩 / 应拒却接受

**非破坏性**（selfhost sema）。两类函数值调用实参数漏校验：
①`(int x)=>{...}(5,6)`（lambda 直调多参）静默过 → codegen `call void @(i32 5,
i32 6)` → **opt-21 崩**（expected value token）；②`f(5,6)`（f:(int)->int 函数
类型变量多参）静默过 → 多余实参被忽略、**语义错**（应拒却接受）。oracle 两处都
拒 "expected 1 arguments, got 2"。

- **根因**：自举 Call 处理函数值 callee 有两条路径——①函数类型变量（Identifier
  callee 的 fve.funcRet() 分支）只 setCallParamTypes 不校验数量；②lambda 直调
  （非 Identifier/Member 的 fallback `else { visitExpr(callee); }`）不设
  callParamTypes、不校验数量。oracle visitCall 对 Function TypeInfo callee 调
  normalizeCallArgs（无命名/默认元数据 → 严格按数量匹配）。
- **修复**：①函数类型变量分支——`fve.functionParamTypes()`（ArrayList<string>
  kinds）数量比对，不匹配报错并 callFailed=1；②fallback 分支——callee 是
  Lambda 时用 `callee.params()`（ArrayList<AstParam>）数量比对，不匹配报错；
  匹配时 setCallParamTypes(paramKinds(lamParams)) + setCallParamFuncSig，复用
  现有逐参类型校验。
- **验证**：lambda 直调多参/少参 + 函数类型变量多参三形态全部 `sev=error`
  "expected N arguments, got M"；正确调用（f(5)、语句级 lambda 直调）仍编译+运行；
  lambda 直调作值表达式双端同拒（oracle "cannot call expression"）；bootstrap 成立。
- **回归**：负测试 `tests/negative/lambda_call_argc.myp` / `fntype_var_call_argc.myp`；
  全量 406 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：函数值是「有参数量约束」的类型——凡函数类型 callee（变量/lambda
  直调）都须按数量严格匹配（oracle 函数值路径无命名/默认元数据 → 严格计数）；
  自举两处函数值调用路径都漏计数校验。

## BUG-104（已修复 🟩，v3.15.138）：三元分支类型不兼容漏校验 → opt 崩

**非破坏性**（selfhost sema）。`p ? 5 : "str"` 静默过 → codegen 把 string 当
byte 存 → **opt-21 崩**（constant expression type mismatch: got type 'ptr' but
expected 'i8'）。oracle 拒 "ternary branches have incompatible types: 'byte'
and 'string'"。

- **根因**：visitTernary 只在「双数值」分支用 ternaryCommon 提升；非数值双分支
  直接返回 true 分支类型 t，从不校验 t/f 兼容。oracle visitTernary 数值路径
  外还有 `typesCompatible(true_type, false_type)` 检查。
- **修复**：非数值路径加 `typesCompat(t, f)` 检查（镜像 oracle typesCompatible）；
  不兼容报错 "ternary branches have incompatible types: 'X' and 'Y'"（exprTypeName
  取显示名）。null 分支（p ? "a" : null）也拒——与 oracle 同文（oracle 的
  typesCompatible 只放行 null↔class，string 与 null 不放行）。
- **验证**：不匹配（int/string、string/null）拒；匹配（int/int、string/string、
  数值混合提升 1 与 int）编译+运行正确；bootstrap 成立。
- **回归**：负测试 `tests/negative/ternary_branch_type.myp`；全量 406 通过 / 0
  失败；oracle 对拍 95/0。
- **教训**：三元是「表达式级类型合并点」——双分支须数值可提升或类型兼容；自举
  只镜像了数值提升路径、漏了非数值兼容检查（与 BUG-074 的三元条件 bool 检查
  互补）。null 与 string 不兼容是 oracle 既有语义（非自举新加）。

## BUG-105（已修复 🟩，v3.15.139）：一元 - / ~ 操作数类型漏校验 → opt 崩

**非破坏性**（selfhost sema）。`-"x"`（一元负号 string）与 `~d`（位取反
double/float）此前静默过 sema → codegen 生成 neg/not 非整型 → **opt-21 崩**
（integer constant must have integer type）。oracle 拒 "expected numeric type,
got 'string'"（Negate expectNumeric）与 "'~' requires an integer or bitvector
operand（got 'double'）"（BitNot）。

- **根因**：自举 Unary 处理只镜像了 `!`（BUG-074 expectBool）；`-`/`~` 直接
  `setResolvedKind(o); return o;` 从不校验操作数类型。
- **修复**：Unary 分支补两道校验——①`-`（Negate）：`isNumKind(o)==0` →
  "expected numeric type, got 'X'"（exprTypeName 取显示名），错误返回 Int（镜像
  oracle）；②`~`（BitNot）：`bitvector/bit/整型（排除 float/double）` 合法，
  否则 "'~' requires an integer or bitvector operand (got 'X')"。
- **验证**：`~double`/`~float`/`-"x"` 三形态全部 `sev=error`；`~int`（-6）/
  `-int`/`-double`（-5,-1.5）仍编译+运行；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/unary_tilda_type.myp` / `unary_minus_type.myp`；
  全量 408 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：一元操作符校验不是只有 `!` 一处——`-`（expectNumeric）与 `~`
  （BitNot 专属整型/bitvector/bit）各有独立条件；自举 Unary 处理器此前是
  「只设 resolvedKind 不校验」的盲点（与 BUG-074 的 `!` 成对补齐）。

## BUG-106（已修复 🟩，v3.15.140）：重复形参名漏校验 → opt 崩

**非破坏性**（selfhost sema）。`int f(int a, int a)` 重复形参名静默过 sema →
codegen 发 `define internal i32 @f(i32 %a, i32 %a)`（LLVM 参数重名）→ **opt-21
崩**（redefinition of argument '%a'）。oracle 容忍（last-wins 运行，输出正常），
但重复形参名是用户错误——自举干净拒绝。

- **根因**：自举 declareParam 逐参声明、从不检查同形参表内重名（BUG-085 的
  duplicate variable 只覆盖函数体局部变量，不覆盖形参）。
- **修复**：新增 `checkDupParamNames(ArrayList<AstParam>, fline, fcol)` helper
  （StrHashMap 记录已见名，重名报 "duplicate parameter name 'X'"），接入 5 处
  形参声明循环——顶层函数/类 action/类 function/类 static action/struct 方法
  （调用处用函数/方法行号作诊断位置；AstParam 无 nameLine/nameCol 成员）。
- **验证**：顶层/类 action/struct 方法三形态全部 `sev=error`；正常形参
  `f(3,4)`=7 仍编译+运行；bootstrap 自举成立（自举源码无重复形参）。
- **回归**：负测试 `tests/negative/dup_param_name.myp` / `dup_param_action.myp`；
  全量 410 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：重复校验要覆盖「函数/方法形参表」——oracle 靠 LLVM 参数按索引引用
  容忍重名，自举 codegen 按名引用即崩；同类盲区（BUG-085 局部变量、BUG-095
  方法、BUG-094 bitfield 字段）逐个补齐。方向相反（oracle 接受、自举崩）——
  编译期干净拒绝优于 codegen 崩。

## BUG-107（已修复 🟩，v3.15.141）：@gpu reduce/scan 声明式校验漏 → opt 崩

**非破坏性**（selfhost sema）。`@gpu reduce ... init "str"`（init 与元素类型
不匹配）自举此前静默过 → codegen 把 string 当 float 常量 → **opt-21 崩**
（constant expression type mismatch: got 'ptr' but expected 'float'）。oracle
当前源 visitGpuReduceStmt/visitGpuScanStmt 有全套校验（数组/元素类型/输出/
init/range/op return）。

- **根因**：自举 GpuReduce/GpuScan 语句处理只「标记/访问表达式 + 提取 op
  return」，从不校验（GpuScatter 已有校验、reduce/scan 漏——不对称）。
- **修复**：镜像 oracle 六道校验——①输入数组须 T[] 动态数组（reduce 用 arr()、
  scan 用 inName()）；②元素类型 float/double/int；③输出（reduce 标量 / scan
  数组）类型匹配；④init 类型 == 元素类型（先 visit 存 kind）；⑤begin/end
  isNumKind；⑥op 体须含 return 表达式（提取后 opExpr==null 报错）。校验失败
  时 markAll 各子树并 return；通过后按元素类型声明 acc/x 再访问 op 体。
- **验证**：reduce init 错型/数组不存在/op 无 return/输出错型 + scan init 错型/
  输出错型 六形态全部 `sev=error`；正确 reduce/scan 仍编译；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/gpu_reduce_init_type.myp` /
  `gpu_reduce_op_noreturn.myp` / `gpu_scan_init_type.myp`；全量 413 通过 / 0
  失败；oracle 对拍 95/0。
- **教训**：GPU 声明式语句（reduce/scan/scatter）是校验盲区——GpuScatter 已镜
  像、reduce/scan 漏（不对称）；「声明式语法 + 匿名 lambda op」的校验须在访问
  op 体前完成（数组/init/range），op return 提取后校验。

## BUG-108（已修复 🟩，v3.15.142）：@gpu tile grid(nb) 块数校验漏 → opt 崩

**非破坏性**（selfhost sema）。`@gpu tile ... grid("x")`（grid 非整数）自举
此前静默过 → codegen 把 string 当 i64 → **opt-21 崩**（constant expression
type mismatch: got 'ptr' but expected 'i64'）；`grid(0)` 静默接受（应拒却接受）。
oracle 当前源 visitGpuTileStmt 检查 "grid must be an integer (block count)" /
"grid must be a positive block count"。

- **根因**：自举 GpuTile grid 处理只 visit gridExpr 并设 gridVal，不校验类型/
  正数（原逻辑 `kind=="Integer" || gridLiteral>0` 非正字面量落到 -1 分支静默）。
- **修复**：镜像 oracle——①gridExpr 非数字 → "grid must be an integer (block
  count)"，gridVal=-1；②Integer 字面量 ≤0 → "grid must be a positive block
  count"；③非字面量（运行时表达式如 conv3d nTiles）→ gridVal=-1 标记 host
  求值（保持原行为）。
- **验证**：grid("x") / grid(0) 双形态 `sev=error`；grid(4) 编译正常；bootstrap
  自举成立。
- **回归**：负测试 `tests/negative/gpu_tile_grid_type.myp` /
  `gpu_tile_grid_positive.myp`；全量 415 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：@gpu tile 子句（grid/block/shared/stream）逐一核对 oracle——grid
  有类型+正数两道校验；自举原逻辑「非正字面量→-1」掩盖了校验缺失。stream(s)
  须 GpuStream 实例校验为非崩缺口（自举超前，seed 无法解析，低优先待办）。

## BUG-109（已修复 🟩，v3.15.143）：@gpu for/tile resident 子句校验漏

**非破坏性**（selfhost sema）。`@gpu for ... resident(a = da)`（da 非 long
device 指针）与 `resident(b = db)`（b 未声明数组）此前自举静默接受 → GPU 运行
时错误（应拒却接受，非 opt 崩）。oracle 当前源 visitGpuForStmt/visitGpuTileStmt
校验 "resident array 'X' not found in scope" / "resident 'X' is not an array" /
"resident device-pointer variable 'X' must be 'long'"。

- **根因**：自举 GpuFor（for 循环形）/GpuTile 语句处理只 visit stream、不校验
  resident 子句（resident() 返回 ArrayList<AstPair>，k=数组名/v=device 名）。
- **修复**：镜像 oracle——两处（GpuFor 循环形 + GpuTile）遍历 resident 子句：
  ①数组名须在作用域（lookupEntry 非空）且 type=="array"；②device 名须存在且
  type=="long"。诊断用语句行号。
- **验证**：dev 非 long / arr 未声明 双形态 `sev=error`（for 与 tile 两处）；
  正确 resident(a=da, b=db)（long）编译正常；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/gpu_resident_dev_type.myp` /
  `gpu_resident_arr_missing.myp`；全量 417 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：GPU 子句族（grid/block/shared/stream/resident）是校验盲区——逐一
  核对 oracle visitGpuForStmt/visitGpuTileStmt 的每个子句分支；resident 的
  device 指针须 long（GPU 运行时按 long 传）在编译期即可验。

## BUG-110（已修复 🟩，v3.15.144）：@gpu scatter 区间界类型漏校验 → opt 崩

**非破坏性**（selfhost sema）。`@gpu scatter(unique) a["x"..10) ...`（a 区间
下界为 string）自举此前静默过 → codegen 把 ptr 当 i64 索引 → **opt-21 崩**
（constant expression type mismatch: got 'ptr' but expected 'i64'）。oracle
当前源 visitGpuScatterStmt 校验 "range bound must be an integer" / "index
range bound must be an integer"。

- **根因**：自举 GpuScatter 校验 input/output/index 数组，但区间界（aBegin/
  aEnd/idxBegin/idxEnd）只 visit 不校验类型。冲突模式 parser 已限定（scatter
  (foo) 解析拒），无需 sema。
- **修复**：GpuScatter 分支在访问区间界前加 isNumKind 校验——aBegin/aEnd →
  "range bound must be an integer"；idxBegin/idxEnd → "index range bound must
  be an integer"；任一失败 scatterOK=0 → body markAll（避免半访问）。
- **验证**：a 区间界 string / idx 区间界 string 双形态 `sev=error`；正确
  scatter 编译正常；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/gpu_scatter_bound_type.myp` /
  `gpu_scatter_idx_bound_type.myp`；全量 420 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：GPU 声明式语句的「区间界」是独立校验点（reduce/scan 的 begin/end
  已修、scatter 的 a/idx 界漏）——所有 `[lo..hi)` 界都须整型。

## BUG-111（已修复 🟩，v3.15.145）：@gpu tile shared 须数组类型漏校验

**非破坏性**（selfhost sema）。`@gpu tile (float sm) ...`（标量 shared 内存
声明）自举此前静默当数组处理 → 语义错（应拒却接受，非 opt 崩）。oracle 当前源
visitGpuTileStmt 校验 "requires an array type (e.g. float[32][32])"。

- **根因**：自举 GpuTile 共享内存处理把 sharedType 当数组遍历（element() 循环），
  非数组类型不报错、静默当 int 元素。
- **修复**：GpuTile 分支开头校验 `sharedType().element()==null` → "@gpu tile
  requires an array type (e.g. float[32][32])"，body/stream markAll 后 return。
- **验证**：`float sm`（标量）`sev=error`；正确 `float[64] sm` 编译正常；
  bootstrap 自举成立。
- **回归**：负测试 `tests/negative/gpu_tile_shared_nonarray.myp`；全量 420
  通过 / 0 失败；oracle 对拍 95/0。
- **教训**：@gpu tile 共享内存声明是「类型形状」校验点——须数组类型、维度编译
  期常量、48KB 上限三道；自举有 48KB、漏形状（非数组）；维度常量由 parser
  `float[n]` 拒（不可达）。

## BUG-112（已修复 🟩，v3.15.146）：stream/resident 仅限 @gpu for 归属漏校验

**非破坏性**（selfhost sema）。普通 `for (...) stream(s) {...}` 与
`for (...) resident(...) {...}`（非 @gpu）此前自举静默忽略子句（应拒却接受）。
oracle 当前源 "stream(...) is only valid on '@gpu for'" / "resident(...) is
only valid on '@gpu for'"。

- **根因**：自举 for 语句分支只校验 block() 的归属（"block only valid on
  @gpu for"），stream/resident 子句归属漏（parser 允许普通 for 带子句）。
- **修复**：for 分支补两道——`s.gpu()==0 && s.stream()!=null` →
  "'stream(...)' is only valid on '@gpu for'"；`s.gpu()==0 && resident 非空` →
  "'resident(...)' is only valid on '@gpu for'"。
- **验证**：普通 for 带 stream / resident 双形态 `sev=error`；@gpu for 带
  stream/resident 编译正常；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/stream_not_gpufor.myp` /
  `resident_not_gpufor.myp`；全量 423 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：GPU 子句「归属」校验（仅 @gpu for/tile）与「类型」校验（grid/resident/
  stream）是两组独立检查——block 已有归属检查、stream/resident 漏（不对称补齐）。

## BUG-113（已修复 🟩，v3.15.147）：@gpu stream(s) 参数须 GpuStream 漏校验

**非破坏性**（selfhost sema）。`@gpu for ... stream(s)`（s 为 int）此前自举
静默接受（应拒却接受，非 opt 崩）→ GPU 运行时错。oracle 当前源 "stream(...)
argument must be a 'GpuStream' (got '...')"。

- **根因**：自举 GpuFor 循环形 + GpuTile 两处只 visit stream 表达式、不校验
  其类型。GpuStream 是 stdlib/gpu/graph.myp 类。
- **修复**：两处 stream 访问加校验——stream 表达式须 Identifier 且
  `sym_.lookupClass(name)=="GpuStream"`，或 New 且 className=="GpuStream"；
  否则报 "'stream(...)' argument must be a 'GpuStream' (got 'X')"。
- **验证**：stream(int) `sev=error`；stream(GpuStream)（test_gpu_graph）编译
  正常；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/gpu_stream_arg_type.myp`；全量 423 通过 /
  0 失败；oracle 对拍 95/0。
- **教训**：@gpu 子句参数类型校验须覆盖「类实例」形态（Identifier 查
  lookupClass / New 查 className）——此前只 visit 不判型；GpuStream 是自举
  超前（seed 无法解析）但 oracle 当前源有校验，须镜像。

## BUG-114（已修复 🟩，v3.15.148）：@gpu tile shared 数组名冲突 + block<dim 警告

**非破坏性**（selfhost sema）。`@gpu tile (float[64] sm)` 与外层变量 `sm`
同名——自举此前 sym_.declareElem 静默覆盖外变量（应拒却接受）。oracle 当前源
visitGpuTileStmt 校验 "shared array name 'X' already declared" + block 小于
共享数组最大维度时 warning。

- **根因**：自举 GpuTile 声明共享数组不查外层同名；block 与维度关系不检查。
- **修复**：①声明前 `sym_.lookupEntry(s.name()) != null` → "shared array name
  'X' already declared"；②`block() > 0` 且小于最大维度（遍历 sharedType
  arraySize 取 max）→ diag_.warn "'block(...)' size (N) is smaller than shared
  array dimension (M); threads may not cover the array"（warning 记录到 dump，
  非阻塞，与 oracle 警告性质一致）。
- **验证**：外层同名 shared → `sev=error`；正确 tile 编译正常；bootstrap 成立。
- **回归**：负测试 `tests/negative/gpu_tile_shared_dup.myp`；全量 424 通过 /
  0 失败；oracle 对拍 95/0。
- **教训**：@gpu tile 共享内存声明是最后一个未镜像点（形状 BUG-111 + 名字冲突
  + block<dim 警告）——至此 oracle GPU 校验全镜像：reduce/scan/scatter/
  tile/for/resident/stream/grid/block/shared 全覆盖。

## BUG-115（已修复 🟩，v3.15.149）：枚举变体数据实参数漏校验

**非破坏性**（selfhost sema）。`Opt.Some(1, 2)`（多参）与 `Opt.Some()`（少参）
此前自举静默接受 → 多余/缺失数据被忽略（应拒却接受，非 opt 崩）。oracle 拒
"expected 1 arguments, got 2" / "expected 1 arguments, got 0"。

- **根因**：自举枚举变体构造（Enum.Variant(args)）分支只 visit 实参、不校验
  数量。oracle 把带数据变体视为 Function 类型（param_types），normalizeCallArgs
  严格计数。
- **修复**：变体构造分支加实参数校验——按变体名取 params().size() 与
  e.args().size() 比对，不匹配报 "expected N arguments, got M"。无数据变体带
  实参（Opt.None(5)）报 "expected 0 arguments, got 1"（oracle 报 "not
  callable"，消息不同但双端拒）。
- **验证**：多参/少参/无数据带参三形态 `sev=error`；Opt.Some(5) 正确编译+运行；
  bootstrap 自举成立。
- **回归**：负测试 `tests/negative/enum_variant_argc.myp` /
  `enum_variant_argc_few.myp`；全量 427 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：枚举带数据变体是「函数式构造」——实参数校验与普通调用同（oracle
  统一走 Function normalizeCallArgs）；自举变体分支只 visit 不计数是盲点。

## BUG-116（已修复 🟩，v3.15.150）：tuple 变量初始化类型不匹配漏校验 → opt 崩

**非破坏性**（selfhost sema）。`(int, int) u = t`（t 为 `(int, string)` 元组
变量）此前自举静默 → codegen store 类型不匹配 → **opt-21 崩**（'%t9' defined
with type '{ i32, ptr }' but expected '{ i32, i32 }'）。oracle 拒 "cannot
initialize variable 'u' of type '(int, int)' with value of type '(int,
string)'"。

- **根因**：自举 tuple arity/元素检查只处理 `tie.kind()=="Tuple"`（字面量）
  形态；tuple 变量/调用返回元组 形态漏（init 解析为 "tuple" 无元素比较）。
  赋值语句（`u = t`）同样只做 typesCompat("tuple","tuple")=1 静默过。
- **修复**：①变量初始化——复用 `destructureTupleElems`（Tuple 字面量/
  Identifier/Call 三形态取元素 kind 列表）与 `v.type().funcParamTypes()` 比对
  arity + 逐元素 typesCompat；②赋值语句——`l=="tuple"&&r=="tuple"` 时同样用
  destructureTupleElems 双端比较；新增 `tupleKindListName`（kind 列表 →
  "(int, string)" 显示名）。消息与 oracle 逐字一致。
- **验证**：tuple 变量初始化元素不匹配/arity 错 + 赋值语句不匹配 三形态
  `sev=error`（消息与 oracle 一致）；匹配形态编译+运行正确；bootstrap 成立。
- **回归**：负测试 `tests/negative/tuple_var_type_mismatch.myp` /
  `tuple_assign_type_mismatch.myp`；全量 428 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：tuple 初始化/赋值校验「字面量有、变量/调用无」是不对称盲区（BUG-093
  的 destructureTupleElems 已覆盖三形态可直接复用）；tuple 是结构化类型——
  typesCompat 按 kind 只到 "tuple"，变量声明与赋值语句两处都要逐元素比较。

## BUG-117（已修复 🟩，v3.15.151）：tuple 返回类型不匹配漏校验 → opt 崩

**非破坏性**（selfhost sema）。`(int, int) ret() { return (1, "a"); }` 自举
此前静默 → codegen store 类型不匹配 → **opt-21 崩**（'%t2' defined with type
'{ i32, ptr }' but expected '{ i32, i32 }'）。oracle 拒 "cannot return value
of type '(byte, string)' from function returning '(int, int)'"。

- **根因**：Return 检查用 typesCompat(currentRet_, rt)——tuple 只到 "tuple"，
  元素类型不比较；且无当前函数返回 AstType 可查元素类型（只有 kind 字符串）。
- **修复**：①新增字段 `AstType currentRetAst_`（5 处函数/方法声明点随
  currentRet_ 同步设置）；②Return 检查加 tuple 分支——currentRet_==rt=="tuple"
  时 `destructureTupleElems(s.value())` 与 `currentRetAst_.funcParamTypes()`
  比对 arity + 逐元素 typesCompat；消息与 oracle 逐字一致。
- **验证**：tuple 返回元素不匹配 `sev=error`（消息与 oracle 一致）；匹配形态
  编译+运行正确；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/tuple_ret_type_mismatch.myp`；全量 429
  通过 / 0 失败；oracle 对拍 95/0。
- **教训**：tuple 结构性校验须「当前函数返回 AstType」可查——只存 kind 字符串
  不够，须另存 AstType（与 BUG-116 的声明/赋值路径补齐成 tuple 三路径：声明/
  赋值/返回）；destructureTupleElems 三形态复用是统一手法。

## BUG-118（已修复 🟩，v3.15.152）：关系比较 < > <= >= 左操作数类型漏校验 → opt 崩

**非破坏性**（selfhost sema）。`a < 5`（a 为 struct，@op("<") 类型不匹配时
resolveOp 返回空）此前自举静默当 bool → codegen 比较异构类型 → **opt-21 崩**
（integer constant must have integer type）。oracle 拒 "expected numeric type,
got 'Vec2'"（visitBinaryOp Lt/Gt/Le/Ge 的 expectNumeric(lhs)）。

- **根因**：自举比较分支 resolveOp 不匹配后直接 setResolvedKind("bool")
  返回，无 expectNumeric 检查（`+`/`-` 等算术分支有、比较分支漏——不对称）。
- **修复**：比较分支加 expectNumeric(lhs)——`<`/`>`/`<=`/`>=` 且 lhs 非
  string/bool/bit/bitvector/null/数字 → "expected numeric type, got 'X'"
  （exprTypeName 取显示名）。@op 匹配路径（resolveOp 返回非空）不受影响。
- **验证**：struct<int 关系比较 `sev=error`（消息与 oracle 一致）；@op 匹配
  （Vec2<Vec2）/ 数字比较编译+运行正确；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/relop_nonnumeric_lhs.myp`；全量 430 通过 /
  0 失败；oracle 对拍 95/0。
- **教训**：比较运算符（< > <= >=）与算术运算符同样需要 expectNumeric——自举
  算术分支有、比较分支漏（不对称）；`==`/`!=` 双端 sema 均宽松（struct==int
  oracle 也接受、struct==struct 双端后端崩）不属本类缺口。

## BUG-119（已修复 🟩，v3.15.153）：泛型实例函数实参类型不匹配漏校验

**非破坏性**（selfhost sema）。`id<string>(5)`（显式 type-arg string、实参
byte）此前自举静默接受 → 实参类型与替换后形参不匹配 → 运行时垃圾（应拒却接受，
非 opt 崩）。oracle 拒 "argument 1: expected 'string', got 'byte'"。

- **根因**：泛型函数调用路径 `normalizeCallArgs(e, inst.params())` 只校验实参
  数量，不校验类型——形参经 instantiateGenericFunction 已替换具体类型，但
  实参类型未逐参比对。
- **修复**：normalizeCallArgs 成功后加逐参校验——`inst.params().get(i).type()`
  的 typeToKind 与 `e.args().get(i).resolvedKind()` typesCompat；不匹配报
  "argument N: expected 'X', got 'Y'"（guard gpk!="void"/"assoc" 防泛型占位）。
- **验证**：`id<string>(5)` `sev=error`（消息与 oracle 一致）；推导 `id("a")` /
  正确显式 `id<string>("hi")` 编译+运行正确；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/generic_fn_arg_type.myp`；全量 431 通过 /
  0 失败；oracle 对拍 95/0。
- **教训**：泛型函数「显式 type-arg」路径的实参类型校验独立于推导路径——推导
  路径 inferConcrete 天然匹配、显式路径须显式逐参比较；BUG-067 修了泛型方法、
  BUG-069 修了泛型 static、本 bug 补泛型函数显式实参（三处调用路径逐一核对）。

## BUG-120（已修复 🟩，v3.15.154）：lambda 直调（正确计数）自举 codegen 无法
发射 → opt 崩

**非破坏性**（selfhost sema）。`(int x) => { return x + 1; } (5)`（lambda 直调
作语句）自举此前静默过 sema → codegen 生成 `call void @(...)`（空函数名）→
**opt-21 崩**（expected value token）。oracle codegen 拒 "cannot call
expression"（codegen_expr.cpp:2510，干净拒绝不崩）。@parallel 体内同理崩。

- **根因**：BUG-103 只校验 lambda 直调实参数（正确计数放行、设 callParamTypes），
  但自举 codegen 根本无法发射「直接 lambda 调用」（lambda 仅可作实参/赋给函数
  类型变量后经变量调用）——正确计数直调落到 `call void @(...)` 空名 → opt 崩。
- **修复**：fallback 分支 lambda callee 处理——计数不匹配仍报 BUG-103 消息
  （保留负测试）；计数匹配改为拒绝 "cannot call expression"（镜像 oracle
  codegen 消息），callFailed=1 避免 codegen。@parallel 体同路径覆盖。
- **验证**：普通语句直调 / @parallel 体内直调 双形态 `sev=error` "cannot call
  expression"（与 oracle 消息一致）；lambda 作实参 / 函数类型变量调用仍编译+
  运行正确；BUG-103 多参负测试保留；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/lambda_direct_call.myp` /
  `lambda_direct_call_parallel.myp`；全量 433 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：codegen 能力边界（自举无法发射直接 lambda 调用）须在 sema 干净拒绝
  ——BUG-103 只修了「多参」形态、漏「正确计数」直调（codegen 崩）；oracle 的
  "cannot call expression" 在 codegen 层（不崩）→ 自举应在 sema 层提前拒。

## BUG-121（已修复 🟩，v3.15.155）：@static 方法内使用 this → opt 崩

**非破坏性**（selfhost sema）。`@static class S1 { static: int getK() {
return this.k; } }`——`this` 在 @static 方法内自举此前静默过 sema → codegen
生成访问 `%this.addr`（无实例）→ **opt-21 崩**（use of undefined value
'%this.addr'）。oracle 接受但返回垃圾 0（语义无意义）；@static 类无实例，静态
状态应经 `Class.property` 访问。

- **根因**：自举 ThisExpr 处理无条件返回 "class"，不区分 @static 方法上下文
  （无 inStatic_ 标志）；@static 方法体 visit 时 codegen 找不到 this 槽。
- **修复**：①新增 `int inStatic_` 字段；②@static action 循环 body visit 前置 1/
  后置 0；③ThisExpr 处理 `inStatic_!=0` → "cannot use 'this' inside a @static
  method (no instance; access static state via Class.property)" 干净拒绝。
- **验证**：@static 方法内 this `sev=error`（不再 opt 崩）；实例方法 this /
  @static 类属性（Class.prop）编译+运行正确；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/this_in_static.myp`；全量 434 通过 / 0
  失败；oracle 对拍 95/0。
- **教训**：反向缺口（oracle 接受-垃圾、自举崩）也须修——@static 方法无实例，
  `this` 语义无意义，干净拒绝优于 opt 崩；@static 上下文需 inStatic_ 标志
  （与 inClass_/inStruct_/inCoro_ 同族）在 ThisExpr 前置检查。

## BUG-122（已修复 🟩，v3.15.156）：`const` 局部变量 parser 误拒（拒合法代码）

**非破坏性**（selfhost parser）。`const int x = 5;`（函数体/类 action/for-init
的 const 局部变量）自举此前 **parse 误拒**（"expected type"——parseType 看到
`const`），oracle 接受（const 局部当普通变量，可重赋值）。这是「拒合法代码」
的反向缺口。

- **根因**：语句分发 line 841 `if (k == "const") return parseVarDeclStmt();`
  未 advance `const` 就调 parseVarDeclStmt → parseType 看到 "const"（非类型
  token）→ "expected type"。for-init（line 1183 `curKind()=="const"`）同理。
- **修复**：parseVarDeclStmt 开头处理 `const` 前缀——`isConst` 检测 + 显式
  advance + v.setConst(1)（仅记录不强制，与 oracle 一致：const 局部可重赋值）。
  **关键**：只对 `const` advance、不对 `var` advance——`var` 由 parseType 消费
  （isTypeTokenStr 含 var，作 int 推断占位）；双消费 var 会误拒 `var x = 5`
  （首版即犯此错，5 个测试回归失败后修正）。
- **验证**：const 局部（函数体/action/for-init）编译+运行正确；const 局部可
  重赋值（与 oracle 一致）；`var x = 5` 等 var 声明不受影响；顶层 const-decl
  （const int CAP）不受影响；bootstrap 自举成立。
- **回归**：正测试 `tests/@test/const_local_var.myp`（4 断言）；全量 435 通过
  / 0 失败；oracle 对拍 95/0。
- **教训**：parser 关键字修饰符（var/const）的消费点要分清——`var` 走
  parseType（类型 token 占位）、`const` 走显式 advance；改动前先确认该 token
  的既有消费路径（防双消费/漏消费）。

## BUG-123（已修复 🟩，v3.15.157）：struct == 比较无 @op("==") → opt 崩

**非破坏性**（selfhost sema）。`Vec2 == 5`（struct 与 int 比较）与 `Vec2 ==
Vec2`（同 struct 无 @op）此前自举静默过 sema → codegen icmp 异构类型 →
**opt-21 崩**（icmp requires integer operands / integer constant must have
integer type）。oracle struct==int 接受-垃圾（语义无意义）、struct==struct
codegen verify 崩；manual §operators 要求 struct 比较走 @op("==")。

- **根因**：`==`/`!=` 比较分支对 struct 操作数不做任何检查——resolveOp 无 @op
  匹配后直接 setResolvedKind("bool")，codegen icmp struct → 崩。
- **修复**：`==`/`!=` 分支加 struct 检查——任一侧 struct 且（无 @op 匹配时）
  → "struct comparison requires an '@op(\"==\")' operator (structs are not
  comparable by value)"。@op 匹配路径在 resolveOp 已提前返回不受影响；
  class/interface ==（引用比较）不受影响。
- **验证**：struct==int / struct==struct 双形态 `sev=error`（不再 opt 崩）；
  @op("==") struct 比较 / class==class 引用比较编译+运行正确；bootstrap 成立。
- **回归**：负测试 `tests/negative/struct_eq_no_op.myp`；全量 436 通过 / 0
  失败；oracle 对拍 95/0。注意：诊断消息带 `\"` 转义，EXPECT ERROR 行须用
  转义形式匹配。
- **教训**：`==`/`!=` 对非数字非字符串类型的操作数（struct）是 codegen 崩盲区
  ——oracle 宽松接受（垃圾/后端崩），自举须干净拒绝（manual 要求 @op("==")）；
  类/接口引用比较是合法语义（指针相等），struct 值比较须显式 @op。

## BUG-124（已修复 🟩，v3.15.158）：实例化接口 new IC() 漏校验

**非破坏性**（selfhost sema）。`IC ic = new IC()`（实例化接口）自举此前静默
接受 → 无实现类、无 vtable 的接口实例 → 运行时错（应拒却接受，非 opt 崩）。
oracle 拒 "unknown class 'IC'" + "cannot initialize 'IC' with 'IC'"。

- **根因**：自举 New 处理按类名查构造器——接口名不在 tu_.classes()（接口独立
  存储），无构造器 → 落 legacy 默认 → 静默接受。
- **修复**：New 处理 slice 分支后加接口检查——`inInterface(e.className())!=0`
  → "cannot instantiate interface 'X' (create a class implementing it
  instead)"。接口实例须 `new 实现类` 再转接口（View v = new Label()）。
- **验证**：`new IC()`（含/不含实现类）`sev=error`；接口正常用法（new MyC →
  接口，ic.go()=42）编译+运行正确；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/new_interface.myp`；全量 437 通过 / 0
  失败；oracle 对拍 95/0。
- **教训**：`new` 目标类型须核对「可实例化」——接口/抽象不可 new（无实现）；
  自举 New 处理按「查构造器」间接判断，接口无构造器落默认 → 静默；须显式检查
  接口名（inInterface）。

## BUG-125（已修复 🟩，v3.15.159）：match 枚举变体绑定 arity 漏校验

**非破坏性**（selfhost sema）。`match (o) { Opt.Some(x, y) => ... }`（变体 1
个数据字段、绑 2 个）与 `Opt.Some(x)`（变体 2 个字段、绑 1 个）自举此前静默
接受并运行（绑定被截断/多余绑定未声明）→ 应拒却接受。oracle 拒 "variant
'Some' expects 1 data fields, got 2"（few 方向 "expects 2 data fields, got
1"）。

- **根因**：自举 Match 处理找变体后只按 `bi < bindings.size() && bi <
  params.size()` 截断声明绑定、从不比对数量（oracle visitMatchStmt 有
  `arm.bindings.size() != variant.params.size()` 报错分支）。
- **修复**：arm.setIndex + 取 variant 后加 arity 校验——绑定数 != 数据字段数
  → "variant 'X' expects N data fields, got M"。oracle 报错后仍按 min 声明
  绑定并访问 body（此处同，不 continue，防级联）。
- **验证**：many/few 双方向 `sev=error`（消息与 oracle 逐字一致）；正确 arity
  （Opt.Some(x) → got=5）编译+运行正确双端；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/match_bind_arity.myp`；全量 438 通过 / 0
  失败；oracle 对拍 95/0。
- **教训**：match 枚举臂的绑定数量是独立校验点（oracle 有 arity 报错分支、
  自举只截断声明）——凡「绑定/实参数量 vs 声明数量」不一致须显式报错；MYP
  int→string 拼接用 `"..." + n` 直接可行。

## BUG-126（已修复 🟩，v3.15.160）：nonlocal 仅 lambda body 内合法漏校验

**非破坏性**（selfhost sema）。普通函数/类方法里 `nonlocal x;`（非 lambda
上下文）自举此前静默忽略并运行（应拒却接受，语义无意义），oracle 拒
"'nonlocal' is only allowed inside a lambda body"。

- **根因**：自举 Nonlocal 语句处理是空壳（`// 无表达式`），从不校验上下文；
  oracle visitNonlocalStmt 有 `if (in_lambda_body_ == 0)` 报错分支（named
  lambda __call body 访问时置位）。
- **修复**：①新增 `int inLambda_` 字段；②Pass B 类 action 循环的 named lambda
  `__call`（`__lambda_` 前缀 + c.lambda() 非空 + 方法名 __call，与既有 nonlocal
  槽声明条件一致）body visit 前置 1/后置 0；③Nonlocal 语句处理加
  `inLambda_==0` → "'nonlocal' is only allowed inside a lambda body"（变量解析
  校验 lambda 创建时已做 BUG-077，此处只查上下文）。
- **验证**：普通函数 nonlocal `sev=error`（消息与 oracle 逐字一致）；named
  lambda 内 nonlocal（tests/@test/nonlocal.myp 5 断言）编译+运行全 PASS；
  anonymous lambda 内 nonlocal 双端仍拒（消息不同非崩）；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/nonlocal_outside_lambda.myp`；全量 439
  通过 / 0 失败；oracle 对拍 95/0。
- **教训**：上下文限制类校验（nonlocal 仅 lambda、await 仅 @coro、throw 仅
  catch）靠「访问体时置位/复位」的上下文标志（inLambda_/inCoro_/
  inCatchDepth_ 同族）——自举语句处理空壳处是盲点；nonlocal 语句处理此前完全
  空、oracle 有 in_lambda_body_ 兜底检查。

## BUG-127（已修复 🟩，v3.15.161）：`this` 仅类 action 内合法漏校验 → opt 崩

**非破坏性**（selfhost sema）。顶层函数里 `this.x`（非类/struct/lambda 上下
文）自举此前静默当 class → codegen 对 %Object 值 GEP → **opt-21 崩**（base
element of getelementptr must be sized），oracle 干净拒 "'this' can only be
used inside a class action"。

- **根因**：自举 ThisExpr 处理只查 inStatic_（BUG-121），不查 inClass_/
  inStruct_——顶层函数（inClass_=0）落默认 class → codegen 崩。
- **修复**：ThisExpr 处理 inStatic_ 检查后加 `inClass_==0 && inStruct_==0` →
  "'this' can only be used inside a class action"（镜像 C++ visitThisExpr
  `!in_class_method_ && !in_struct_method_`）；struct 方法（inStruct_=1 且
  inClass_=1）/ 类 action / lambda __call 放行。
- **验证**：顶层函数 this `sev=error`（消息与 oracle 逐字一致）；struct 方法
  this（p.x=42）与类 action this（k=7）编译+运行正确；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/this_in_top_function.myp`；全量 440 通过 /
  0 失败；oracle 对拍 95/0。
- **教训**：`this` 的「上下文合法性」是 codegen 崩盲区——oracle visitThisExpr
  有 in_class_method_/in_struct_method_ 双检查；自举 ThisExpr 曾只设 resolved
  Kind 不校验上下文（与 BUG-121 inStatic_ 同族、比其更宽）；@static/顶层函数
  两条路径都在 ThisExpr 前置检查。

## BUG-128（已修复 🟩，v3.15.162）：嵌套 @parallel for 数据竞争（内层并行化 → 串行化）

**非破坏性**（selfhost codegen）。`@parallel for` 嵌套在 `@parallel for` 体内
自举此前对内层也发 `myp_pool_parallel_for` → 共享全局 pool（myp_global_pool）
的 work_fn/work_arg/barrier/deques 被并发外层 worker 覆写 → 数据竞争、
**非确定错误结果**（total 应为 100 却得 11 / sum[0]=7 / 各 run 不同）。seed
（build-cpu/mypc）的 generateParallelFor 用 emitKernelStmt 生成嵌套 for
（只有最外层并行化），结果确定正确 100。

- **根因**：自举 codegen.myp 的 For 处理 `parallel()` + canParallelizeFor →
  无条件 genParallelFor；genParallelBody 生成外层 body 时对内层 @parallel for
  再 genParallelFor → 嵌套 myp_pool_parallel_for 调用（外层 4 个 worker 并发
  各自调内层 → 竞争）。
- **修复**：新增 `int parallelDepth_` 守卫（genParallelBody 生成 body 期间
  +1/-1）；For 处理 `st.parallel()!=0 && canParallelizeFor(st)!=0 &&
  parallelDepth_==0` 才 genParallelFor，否则（嵌套）走 genSerialFor 串行化。
- **验证**：双层嵌套 total=100、三层混合嵌套各元素 16，3 次运行确定一致；
  内层普通 for 不受影响；现有 @parallel 测试全 PASS；bootstrap 自举成立。
- **回归**：正测试 `tests/@test/parallel_nested.myp`（双层 + 三层混合，5
  断言，3 次确定）；全量 441 通过 / 0 失败；oracle 对拍 95/0。
- **嵌套 @gpu for / 混合嵌套**（@gpu in @parallel / @parallel in @gpu）：GPU
  kernel 生成失败（llc-21 报错 stderr）→ CPU 串行回退，结果正确；llc 噪音非
  致命（exit 0）。
- **教训**：@parallel for 的「嵌套」语义 = 只有最外层并行化、内层串行（数据
  并行嵌套无池安全支持——共享全局 pool 非可重入）；自举 codegen 对嵌套缺守卫
  发重入 pool 调用 → 竞争非确定；与 seed 行为对齐（emitKernelStmt 生成嵌套
  for）。

## BUG-129（已修复 🟩，v3.15.163）：带数据枚举变体裸引用漏校验 → 垃圾数据

**非破坏性**（selfhost sema）。`Opt o = Opt.Some;`（带数据变体 `Some(int v)`
裸引用、无 `(data)`）自举此前静默接受 → 枚举值只有 tag、无 payload → 后续
match `Opt.Some(x)` 提取**垃圾数据**（曾得 x=1730215984）。oracle 把
`Opt.Some` 当 Function 类型 `(int)->unknown` 拒（"cannot initialize variable
'o' of type 'unknown' with value of type '(int) -> unknown'"）。

- **根因**：自举 Member 处理枚举变体分支 `enumHasVariant` → 一律 `t="enum"`
  （不区分数据/无数据变体）；codegen genEnumVariant 按 `call.args().size()`
  决定是否插 payload——裸引用 args 空 → 只插 tag、payload 留 poison →
  match 提取垃圾。
- **修复**：Member 枚举变体分支加数据 arity 检查——`enumVariantParamCount`
  > 0（带数据变体）且裸引用 → "enum variant 'X.Y' requires N data
  argument(s) (use 'X.Y(...)')"，resolvedKind=void。Call 形式 `Opt.Some(5)`
  在 Call 分支（BUG-115 处）提前处理、不经此 Member 检查 → 不受影响；无数据
  变体 `Opt.None` 裸引用仍合法。
- **验证**：裸引用 `Opt o = Opt.Some;` / `take(Opt.Some)` `sev=error`；
  `Opt.Some(42)` match 得 42、`Opt.None` 正常；bootstrap 自举成立。
- **回归**：负测试 `tests/negative/enum_variant_bare_data.myp`；全量 442
  通过 / 0 失败；oracle 对拍 95/0。
- **教训**：枚举带数据变体「裸引用 vs Call」是两个 AST 形态（Member vs
  Call(Member)）——自举 Call 分支处理带实参构造（BUG-115）、Member 分支漏
  数据变体裸引用；凡带数据变体须有实参（oracle 视裸引用为 Function 值）；
  枚举值「tag-only 无 payload」是隐式垃圾数据的来源。

## BUG-130（已修复 🟩，v3.15.164）：mapping 源事件/目标 action 存在性漏校验

**非破坏性**（selfhost sema）。`mapping() { Src.nonexist -> Dst.onOut; }`（源
事件不存在）自举此前**静默接受** → codegen findEventClass 空 → handler 不生成
→ no-op（语义错；oracle 在 LLVM verify 失败）；`Src.output -> Dst.nonexist`
（目标 action 不存在）→ codegen 发 `@Dst_nonexist` 未定义 → **opt-21 崩**
（use of undefined value；oracle 静默容忍 no-op）。

- **根因**：自举 analyzeMapping 只做 BUG-011（实例名节点）+ where/lambda 访问，
  从不校验源事件/目标 action 存在——源不存在 codegen 提前 return 静默；目标
  不存在 codegen 发未定义函数引用 → opt 崩。
- **修复**：analyzeMapping 加存在性校验——节点 0（源）须为 src 类的事件
  （"mapping source 'X.Y' is not an event on class 'X'"）；节点 1+（目标）
  isFunction → 顶层函数须存在（"mapping target function 'X' not found"）、否则
  须为 src 类的 action（"mapping target 'X.Y' is not an action on class 'X'"）。
  类不在 tu_.classes()（导入/泛型实例）跳过（保守不误报）；lambda/transformer
  节点跳过。
- **验证**：源事件不存在 / 目标 action 不存在 `sev=error`（不再静默 no-op /
  opt-21 崩）；有效 mapping 编译+运行正确；bootstrap 自举成立。
- **回归**：负测试 `mapping_missing_source.myp` + `mapping_missing_target.myp`；
  全量 444 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：mapping 是「类名节点 = 源事件 + 目标 action」的声明式语句——校验
  须按节点位置区分（节点 0=事件、1+=action）；oracle 自身无 sema 校验（源在
  verify 崩、目标容忍），自举干净拒绝优于两端；`AstMappingNode` 无位置 →
  诊断 0,0。

## BUG-131（已修复 🟩，v3.15.165）：事件 fire(...) 实参校验漏 → opt 崩/静默缺参

**非破坏性**（selfhost sema）。`fire("x")`（事件 `fire(int v)` 实参类型不匹配）
自举此前静默过 sema → codegen `fire_T2_fire(ptr, i32 <string ptr>)` → **opt-21
崩**（constant expression type mismatch）；`fire()`（缺参）静默接受 → 事件参数
缺失。oracle 拒 "argument 1: expected 'int', got 'string'" / "expected 1
arguments, got 0"。

- **根因**：事件（event: 段）在 methods_ 以 **0 参数**注册（mapping 裸名触发
  emit(v) 用，避免误报 "takes no arguments"），类内未限定调用路径的 isEvent
  守卫跳过 normalizeCallArgs → `fire(...)` 实参从不校验。
- **修复**：①新增 `eventParamsOf(cls, name)`——查 events() 真实参数（非 methods_
  0 参注册）；②类内未限定调用 isEvent 分支加 fire 实参校验——数量比对
  （"expected N arguments, got M"）+ 逐参 `typesCompat(pk, ak)`（**注意序**：
  typesCompat(a,b)=promotesTo(b,a)，须 (形参, 实参) 否则 byte 字面量误报）。
- **验证**：`fire("x")` / `fire()` `sev=error`（消息与 oracle 逐字一致）；
  `fire(42)`（byte 字面量→int 提升）编译+运行正确；bootstrap 自举成立。
- **回归**：负测试 `event_fire_arg_type.myp` + `event_fire_arg_count.myp`；
  全量 446 通过 / 0 失败；oracle 对拍 95/0。
- **教训**：事件是「声明 0 参注册（mapping 用）+ 真 fire 实参另查」的双轨——
  isEvent 守卫防 mapping 误报、但连 fire(...) 也跳过 → 须按事件真实参数
  （events()）单独校验；typesCompat 参数序（形参, 实参）易倒置导致 byte 字面量
  误报。
