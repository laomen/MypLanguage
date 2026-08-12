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

### v3.12.2（当前）— 类型系统增强（P0/P1/P2）+ 多态数学 intrinsic（§9.5）+ GPU `__nv_xf` 选型 + 共享 emitConversion
- **类型系统增强（type_system_design §3-§7/§9，P0/P1/P2 全部落地）**：
  - **单一转换权威（§7.1）**：`convertIntegerValue` 收敛 5+ 处内联转换（赋值/属性/数组元素/return/调用实参/变量初始化），无符号源统一 ZExt（修 D1：`long z; z = 0xFFFFFFFFu;` 不再 `-1`）。
  - **隐式转换格重写（§3.2，无损隐式/有损显式）**：移除 `Int/Long→UInt`、`Int/Long→Float` 隐式与 `char↔byte` 互换；`i64/u64→f64` 改显式；`ulong` 补全（小无符号→大无符号隐式 ZExt，跨符号/浮点显式）。
  - **bool 入转换链（D6）**：`int(b)`=b?1:0、`bool(n)`=n≠0、`bool(f)`=f≠0；隐式 bool↔整型仍禁。
  - **char=u8 语义定稿（D7）**：byte=有符号 i8、ubyte=无符号 i8、char=u8 语义别名（0xFF→255 非负）；char 字面量生成 i8；三处符号矛盾消除。
  - **string 转换统一（修 D2/D3/D4）**：`"x"+f32` 不再编译崩溃、无符号拼接成无符号十进制、char 拼接输出字符；runtime 新增 `myp_to_string_u32/u64/float`。
  - **string 能力**：比较操作符 `< <= > >=`（词法）、`s[i] : char`、`bytes(s)`/`str(bytes)`（string↔ubyte[]）。
  - **parse* 全族（§6.2）**：`parseInt/Long/Uint/Ulong/Float/Double(s)` 统一 strtol/strtoull/strtod 语义（`0x` 前缀，失败回 0）。
  - **位操作原语（§5.3）**：`popcount/clz/ctz/bitreverse/rotl/rotr`（LLVM ctpop/ctlz/cttz/bitreverse/fshl/fshr 直映，多态同宽返回）。
  - **bit + bitvector<N>（§5.1）**：`bit`=i1（`bit(x)`=x≠0）；`bitvector<8/16/32/64>`=iN——索引 `v[i]:bit`、`&|^<<>>`、`~` 取反、写索引 `v[i]=x`、`bitvector<N>(uint)`/`uintN(bv)` 互转、`bytesOf(bitvector<N>)`→ubyte[]。
  - **bitfield（§5.1）**：结构体位域打包（背衬整数 ≤8→i8/≤16→i16/≤32→i32/其余 i64）；读=位提取 bit/uint、写=读-改-写；支持类属性 `this.bf` 与数组元素 `arr[i].field`。
  - **bitcast<T,U>（§5.2）**：位保持重解释（同宽 8/16/32/64，跨宽显式错误）——`bitcast<uint>(1.0f)==0x3F800000`。
  - **泛型 where T : Trait（§9）**：内置数值 trait `Numeric/Integer/Float/Ordered` + 泛型函数/静态方法 `T f<T where T : Trait>`，实例化时约束校验（零运行时开销）。
  - **字面量增强（§4.3）**：下划线分隔 `1_000_000`/`0xFF_FF`/`1_000.5`/`1e1_0`/`1_000L`（lexer `scanNumber` 扫描剥离，parser 零改动）+ 二进制 `0b` / 显式八进制 `0o` 前缀（parser `parseIntegerLiteralValue` 统一解析；前导零 `0755` C 风格八进制保留）。
  - **checked 溢出变体（§4.2 P3）**：`checkedAdd/checkedMul(a,b)` 返回 `(value, overflow:bool)` 元组——`@llvm.sadd/smul.with.overflow.iN` 直映（有符号整型，公共类型提升），声明式解构与字段访问均可用；`tests/checked_overflow`。
  - **parseIntOpt（§6.2 P4）**：`parseIntOpt(s)` 返回 `(value:int, ok:bool)` 元组，用 `ok` 区分合法 `0` 与解析失败（`parseInt` 失败回 0 无法区分）；runtime `myp_str_parse_int_opt`；`tests/parse_opt`。
  - 测试：`tests/bitvector`、`tests/bitfield`、`tests/bitcast`、`tests/bit_ops`、`tests/parse_family`、`tests/generic_traits`、`tests/stringify_conv`、`tests/string_cmp`、`tests/string_subscript`、`tests/bool_convert`、`tests/char_semantics`、`tests/bytes_str`、`tests/unsigned_convert`、`tests/numeric_underscore` 等。
- **§9.5 多态数学 intrinsic + `Math` 库按 trait 重写**（§9.5 全部落地，CPU + GPU）：
  - `__myp_math_*` 一元实数/abs/trunc intrinsic 类型感知：sema 按实参类型定返回类型
    （f32→f32、f64→f64）；CPU codegen 按实参类型发 LLVM 标量 intrinsic
    （`llvm.sqrt.f32` 等；整型 `abs`→`llvm.abs.iN`、浮点→`llvm.fabs`；`trunc`→`llvm.trunc`）；
    GPU kernel 内按实参类型选 libdevice `__nv_xf`（float）`/__nv_x`（double），
    整型 `abs` 内联 `select(x<0,-x,x)`（返回同宽整型）。
  - `Math` 库泛型化：`T sqrt/exp/log/sin/cos/tan/asin/acos/atan/sinh/cosh/tanh/floor/
    ceil/trunc<T where T : Float>`、`abs<T where T : Numeric>`、`min/max/clamp<T where
    T : Ordered>`（int/double/string 通用）、`lerp<T where T : Float>`；`pow`/`atan2`
    保持 double。
  - **破坏性变更（标准库 API）**：`Math.trunc` 返回类型 `int`→`T`（T→T 向零取整）；
    删除 `Math.absInt`/`minLong`/`maxLong`/`clampDouble`（由泛型 `abs`/`min`/`max`/`clamp`
    取代）。迁移：`int(Math.trunc(x))`→`int(x)`；long 上下文 `Math.trunc(x)`→`long(x)`。
  - f32 数学不再需要 `float(Math.exp(...))` 样板（float 实参直接返回 float，精度/性能更好）。
  - 测试：`tests/math_traits`（CPU 泛型数学）、`tests/test_gpu_math_float`（GPU：float
    sqrt/exp-log 组合/double 回归/kernel 内 int abs/显式转换全 PASS）。
- **GPU/CPU 共享 `emitConversion`（§7.1 单一权威）**：GPU kernel 的 `ExprKind::Convert`
  分支改为调用共享自由函数 `convertIntegerValue`——消除 GPU 内重复转换逻辑（bool↔int/fp、
  char 无符号语义、fp↔int、float↔double、指针 bitcast 全覆盖，避免两套转换漂移）。

### v3.12.1 — 语言内建 @test 套件 + Man or Boy + lambda `nonlocal`
- **语言内建测试套件（`@test`）**：`mypc --test file.myp` 生成测试运行器（主循环经
  setjmp/longjmp 异常隔离），退出码反映失败；`tests/@test/` 目录自动发现 + 汇总
  `tests: N, assertions: X passed, Y failed`；断言 API 全系支持自定义 `msg`
  （assert/assertTrue/False/assertEq/Neq/assertLongEq/Neq/assertFloatEq/Neq/
  assertStrEq/Neq/assertNull/NotNull/fail/report）；`tests/test_myp_test.sh` 17 项检查。
- **Man or Boy 测试（Knuth）**：`tests/@test/man_or_boy.myp`——递归闭包把自身作为 thunk
  递归传递（M-FN-2 `__self`）+ 一等函数实参。以 Go 参考实现为准：`A(10,1,-1,-1,1,0) = -67`
  （k 按值每帧独立 + 按名 thunk）；含 `A(-1..10)` 全序列 12 断言。
- **lambda `nonlocal` 按引用捕获（M-FN-2 additive）**：lambda 内 `nonlocal k;` 显式按引用
  捕获外层函数参数/局部变量（共享可变）。codegen 在函数/action 序言把变量提升为堆 cell
  （隐藏类 `__cell_N` 单属性 `v:T`，ARC 管理），lambda 捕获 cell 对象、`__call` 开头注入
  属性 GEP 别名——读写与外层直达同一存储；函数退出释放本帧引用，闭包逃逸后 cell 仍存活。
  v1 边界：仅标量类型；嵌套 lambda / struct 方法内暂不支持（sema 报错）。Man or Boy 得以
  自然书写（不再需要 slice 盒/每帧副本）。`tests/@test/nonlocal`（5 测试 15 断言）。
- **修复：函数返回闭包 retain-at-return 缺失（M-FN-1 潜在 bug）**：`TypeKind::Function`
  未纳入返回 retain 集 → 作用域退出释放闭包 → 返回悬垂（单闭包场景内存未复用侥幸通过，
  多闭包暴露）。补：返回 fat pointer 的 closure（index 0）retain + `return <lambda>` /
  `return f()`（函数返回值）走 `arc_skip_retain_return_` 干净转移 rc=1。
- **修复：`visitFuncBody`/class action 体访问后 decl/action 引用悬垂（UAF）**：单态化重
  分配 `tu.functions`/`tu.classes` 使 `decl.nonlocal_captures` 赋值越界；改经成员暂存 +
  按索引重取赋值（ASAN 暴露）。

### v3.12.0 — 内存系列收尾（M5–M9）
- **M8 · 全量引用计数**：`string`（`myp_alloc_str`，`MYP_STR_TYPE_ID`）、动态数组
  `T[]` 与 `slice` backing（`myp_alloc_slice_backing`，`MYP_ARR_TYPE_ID`，24B 头
  `{rc, type_id, elem_size, count, cap}`）全部改为引用计数——作用域/覆盖自动释放，
  不再依赖 arena/进程退出回收。字符串拼接/数组字面量/切分均经计数。
- **in-place 字符串拼接**：`s = s + x` 在 `s` 为唯一计数串（rc==1）时 realloc 原位
  扩展（`myp_str_append`）——长串累积 O(n²)→O(n)（bench：808ms→52ms）。
- **M5 · struct 引用字段值语义 ARC**：struct 槽按字段计数（拷贝逐字段 retain / 释放
  逐字段 release，kind-5），struct 不再"引用字段不参与计数"。
- **M6 · 跨线程原子 ARC**：`rc` 为 `_Atomic uint32_t`；`myp_retain`=relaxed
  fetch_add，`myp_release`=release fetch_sub（返回旧值），末次释放 acquire fence 后
  析构；分配/释放列表由进程级自旋锁 `myp_alloc_lock` 保护。class/string/数组可安全
  跨线程传递。验证：移除原子后 TSan 报竞争、恢复后无竞争。
- **M7 · `@weak` 弱引用**：字段注解 `@weak`（仅 class/interface 引用字段，struct 字段
  编译期拒绝）；弱表 64 槽链式哈希 + 自旋锁；目标销毁 `myp_weak_notify_death` 持锁置空
  全部弱槽并重查 rc（防并发升级竞争）。读取 = 弱→强一次性升级。测试 `tests/weak_cycle`、
  `tests/weak_non_ref`（负）。
- **M9 · 内存诊断与严格校验**：`Memory.*` 存活/按类型计数、arena/region 字节、协程槽/
  栈池统计；分配失败注入（`MYP_FAIL_ALLOC=n` / `failAllocEnable`）；strict 头校验
  （rc 下溢、重复释放、非法 `type_id` abort，ASAN 默认开）。`tests/mem_diag`、
  `stress/oom_sweep`。
- **M1/M2 · 协程资源上限**：句柄 `{generation<<32|slot}` 世代化（槽复用安全、旧句柄判
  无效）；栈池字节预算 `MYP_CORO_STACK_POOL_MAX_BYTES=16MiB`、大栈
  `MYP_CORO_STACK_BIG=1MiB` 旁路池。`tests/coro_slot_reuse`、`tests/coro_stack_pool_cap`。
- **if/while/for 条件临时泄漏修复**：条件求值产生的 class 临时（调用/弱升级）被分支体
  语句末 flush 抢占 → 分支后 `arcReleaseConditionTemps` 释放，修另一路径泄漏。
- **`string + 非 string` 拼接泄漏修复**：`stringifyForConcat` 转换临时在
  `myp_strcat` 后显式 release（修 `"s"+i` 每拼接漏 1）。
- **mapping 数据传递测试集（`tests/map_data_*`）**：系统覆盖 mapping 传递的数据特性
  矩阵——标量家族（int/long/float/double/bool）、string、多参数事件、事件链返回值
  （double/string/bool）、where 过滤、lambda 变换、`slice<T>`、tuple（含解构/嵌套/
  lambda）、struct（含 class 引用字段/string/嵌套/链返回）、class 引用、interface 胖
  指针（虚表分派）。随测试修复 7 个真实 bug：
  - slice `.length`/`.size` 字段形式：codegen 生成 `ptrtoint(&s)` 当长度 → extractvalue；
    sema 标成 `()->int` → 字段返回 int、调用形式经 visitCall 拦截。
  - action 参数 slice 登记缺失（`generateClassAction` 漏 `var_slice_types_`）→ `a[i]`
    LLVM verify 崩溃。
  - interface 事件参数 upcast：`paramIfaceName` 不识别 `fire_<Class>_<Event>` → fire
    调用处具体类实参未提升为 `{data,vtable}` fat pointer。
  - struct 内 class 引用字段 i32 占位：`buildStructTypes` 早于 `buildClassStructTypes`，
    `typeNodeToLLVMType` 经 `getClassStruct` 落回 i32 → `h.p.get()` 生成 `Payload_get(i32)`；
    加 TU class 名 fallback 恒返回 ptr。
  - struct 字段 store 缺 interface upcast（单层 `h.s=c` + 链式 `w.h.s=c`）：Circle* 直接
    存进 `{ptr,ptr}` 字段、vtable 未初始化 → 虚表分派 SIGSEGV；两处均补 buildInterfaceFat。
- **ARC 覆盖补强**：`map_data_struct_iface`（struct 含 interface 字段：拷贝共享 retain
  平衡/覆盖释放/嵌套链式/级联零泄漏）、`weak_multi_sub`（多个 `@weak` 槽共享同一目标、
  销毁全置空、先后销毁顺序无泄漏）。
- **fuzz 回归（变异模糊测试 `tools/fuzz_myp.py`）**：3000 迭代 ×2 全 CLEAN；修复 void
  函数 `return <void-expr>;` 生成非法 IR（`emitFunctionReturn` 归零 void 值走 `ret void`）；
  HANG 确认重跑 timeout 4s→20s（排除大 stdlib 种子并行负载假阳性）。
- **回归基线**：release 227/227、ASAN 227/227、ASAN stress 6/6、TSan 6/6、OOM 注入
  13/13、fuzz 3000 迭代 0 崩溃。

### v3.11.20
- **内存生命周期 P0 加固**：
  - `@region` 新增函数级保守逃逸分析：slice/数组经 return、property/global store、
    subscript store、throw 或调用参数逃逸时，不建立 arena mark，防止函数返回后持久引用
    指向已回滚 backing；局部-only 函数继续使用 region 快速回滚。
  - `slice<class>` backing 改用引用计数类数组布局，并由 runtime 建立唯一清理登记；
    region 退出或线程/进程清理时逐元素 release，修复元素写入 retain 后无对应 release 的
    长跑泄漏。slice 的 16 字节 ABI 和浅拷贝语义不变。
  - 新增 `tests/region_escape`、`tests/region_slice_class_arc`。
- **@parallel for / @gpu for 体不支持构造的静默垃圾值 → 编译期干净报错**：
  - 症状：并行体内 `new Node()`（→ 常量 0/null）、类实例字段读写（写被丢弃、读
    恒 0——实测 `n.val=7` 后 1000 次读全错）、字符串拼接（`"iter "+i` → 垃圾指针
    运算 → LLVM verify "Call parameter type does not match function signature!"）。
  - 修复（codegen_gpu.cpp emitKernelExpr）：`new`/`new[]`、字符串字面量、类实例字段
    访问（`var_class_map_` 检测）在 kernel 路径现报清晰错误，提示"在循环外分配/只做
    数值运算"。数值数组读写、slice、struct 元素字段、Atomic 等正常路径不受影响。
  - 新增负测试 `tests/negative/parallel_new.myp`、`tests/negative/parallel_string.myp`；
    195/195 回归通过。
  - 注：`tests/stress/parallel_stress.myp` 的 `workers >= 2` 断言间歇性失败（16 核下
    偶发 workers=1）——4096 迭代太快、首 worker 抢完全部导致的时序竞态，与本次改动
    无关（改动只在编译期加错误分支）。
- **编译期拒绝 @coro 方法递归自调用（把静默垃圾值变成清晰错误）**：
  - 背景：`@coro` 调用 = spawn 新协程返回 handle（long），不是返回值。因此
    `@coro long deep(n) { return deep(n-1) + 1; }` 是对 handle 做运算——实测无论
    n 多大恒返回 2（静默错误）。
  - 修复：sema 跟踪当前方法名（`current_method_name_`），在 `@coro` 体内检测到
    自调用（裸名 / `this.` 形式）且**结果被当值使用**（return/算术/参数/赋值）时
    报错：
    `recursive call to '@coro' method 'X' is not supported: an '@coro' call spawns
    a new coroutine and returns a handle... Move the recursion into a plain function`。
  - **语句丢弃形式**（`deep(n-1);`）仍允许——那是 spawn 链（tests/coro_stack 依赖）。
  - 验证：值使用自调用报错、语句丢弃放行、普通函数递归不受影响、合法嵌套协程
    （coro_nest 模式）不受影响。新增负测试 `tests/negative/coro_self_recursion.myp`；
    193/193 回归通过。
- **修复 slice<类> 元素 / `new Foo().x` 上的链式字段访问（LLVM verify 崩溃）**：
  - 症状：`s[0].val`（slice<类> 元素，s[i] 返回类引用）、`new Node(7).val` 报
    `LLVM verify failed: Call parameter type does not match function signature!`
    或读出垃圾值（链式访问在 codegen fallback 丢字段）。
  - 修复：`generateMemberAccess` 的 sema-记录类分支（`resolved_object_class`）从
    仅 Call 对象扩展到 **Subscript / NewExpr** 对象——对 slice/数组类元素下标结果
    和新鲜 `new` 结果 GEP 属性字段。
  - 验证：`s[i].field`、`new Foo().field`、方法调用结果链式全部正确；新增
    `tests/slice_class_chain/`；192/192 回归 + ASAN 干净。
  - 已知限制（未改）：slice 数据用 `myp_region_alloc` 竞技场分配、无析构——slice<类>
    元素不会 ARC 释放（区域/进程退出才回收）。`slice<Node>` 循环创建会累积泄漏。
- **修复 catch/finally 体内 throw 无限循环（异常 handler 未及时 pop）**：
  - 症状：`try { throw "a"; } finally { throw "b"; }`、catch 体内 `throw`、嵌套
    finally 内抛 → 无限循环（运行时几秒吐出千万行）。根因：本 try 的 handler
    （jmp_buf）直到 `merge_bb`/`rethrow_bb` 才 pop，而 catch/finally **体**执行时
    它仍在栈顶 → 体内 throw 经 `__myp_throw` 长跳到**同一 try** → 反复触发。
  - 修复（codegen_stmt.cpp）：handler 只在 try 体执行期间保持激活——
    - 有 finally：在 `finally_bb` 起点 pop（覆盖 try 结束/catch 结束/propagate/
      return/break/continue 转发全部入口）；
    - 无 finally（仅 catch）：try 体正常结束、catch 体起点、`rethrow_bb` 各 pop；
    - `emitExceptionRethrow`（裸 `throw;`）去掉 pop——`throw;` 只在 catch 内，
      handler 已在 catch 起点 pop 过，再 pop 会弹掉外层 handler（表现为未捕获）。
  - 验证：finally 抛替换原异常传外层、catch 体抛传外层、裸重抛保消息、嵌套 finally
    全正确；新增 `tests/exception_throwin/`；191/191 回归 + ASAN 压力全过。
- **修复条件表达式（`&&`/`||`/ternary）分支内类临时对象 ARC 释放违反支配（LLVM verify 崩溃）**：
  - 症状：`w.get().x == 3 && w2.get().x == 5`（方法调用返回类的链式字段访问）、
    `true ? w.get().x : w2.get().x`、`c ? new Point(1) : new Point(2)` 报
    `LLVM verify failed: Instruction does not dominate all uses!`。链式 `get().x`
    还会丢字段（`Console_write(ptr)` 参数类型不匹配）。
  - 根因一（丢字段）：`generateMemberAccess` 对 `Call` 对象无分支，fallback 直接
    返回调用结果。修复：sema 在 `MemberAccessExpr` 记录对象解析 class，codegen
    用它对调用结果 GEP 字段（新增 `resolved_object_class`）。
  - 根因二（不支配）：`arc_pending_temps_` 扁平列表在**语句末**统一释放；短路 `&&`
    的 merge 块可从 entry 直达（跳过 rhs_bb），ternary 的 merge 可从另一分支直达
    —— 分支块不支配 merge，分支内创建的临时对象在 merge 释放违反支配。
  - 修复：新增 `arcEndBranch(before, result)`——在分支块内释放分支创建的中间临时
    对象；若分支结果是新类引用临时对象则转移所有权给 merge phi（两臂都是新临时时
    推 phi 由语句末释放一次；单臂新临时时消费并泄漏，避免对借用分支双重释放）。
    应用于 `generateShortCircuitLogic`（rhs_bb）与 `generateTernary`（true/false_bb）。
  - 验证：`&&`/`||`/ternary × 链式字段访问/类结果全部通过；新增 `tests/member_chain/`；
    190/190 回归 + ASAN + TSAN 压力测试全过。
- **修复 @macro StmtList 累加拼接 O(n²)（`out = out + quote{...}` 惯用法）**：
  - 症状：文档推荐的 `makeCalls(n)` 风格循环里每次 `+` 都深克隆整个已累加列表 →
    二次方。实测 gen(1000)→0.5s、gen(4000)→7.6s、gen(10000)→**46s**（每翻倍 n
    耗时×4）。
  - 根因：`evalBinary` 的 `Ast + Ast` 无条件 `cloneStmtI` 双侧；`EvalValue::Ast`
    是 `shared_ptr`，累加变量与求值临时量共享同一 vector（refcount=2），无法安全
    move 走左侧。
  - 修复（两处）：
    - `evalExpr` 赋值分支增加快路径：识别 `acc = acc + X`（acc 为 StmtList）时
      **原地 append** X 的语句到 acc 现有列表——摊销 O(n)。用 `use_count()==1`
      守卫：若别的变量别名共享 acc 的列表，则回退到通用克隆路径（保证别名不被污染）。
    - `evalBinary` 的 `Ast + Ast` 在左操作数 `use_count()==1`（如新鲜 quote）时
      移动而非克隆。
  - 效果：gen(10000) 46s → **0.2s**，gen(40000) <1s，线性缩放。输出与文档惯用法
    逐字节一致；别名安全用例（`alias = out; out = out + X;` → alias 不受污染）通过。
  - 新增正测试 `tests/macro_concat/`；189/189 回归通过，ASAN 干净。
- **修复编译期 `const string` 拼接导致 LLVM verify 崩溃（元编程测试暴露）**：
  - 症状：`const string G = "a" + "b";`（或 @eval 函数返回拼接串、`a() + b()`）
    报 `LLVM verify failed: Function return type does not match operand type of
    return inst!`。根因：`evalBinary` 缺字符串 `+` 分支 → 两个 Str 值落入 int 分支，
    读 Str 的 `.i` 字段得垃圾整数 → const 被替换成类型不符的 int 字面量 → codegen
    给返回 `ptr` 的函数生成 `ret i32`。
  - 修复：`evalBinary` 增加 `Str + Str → ofStr(a.s + c.s)`；`eq`/`lt` 增加 Str
    分支（此前字符串相等/比较同样读 `.i` 算错）。编译期字符串拼接/相等现在正确。
  - 新增正测试 `tests/const_string/`（拼接/链式拼接/@eval 拼接/编译期 ==）；
    188/188 回归通过，ASAN 干净。
- **修复前缀 `++`/`--` 嵌套反解的指数级 AST 膨胀（内存耗尽挂死，fuzz 暴露）**：
  - 症状：`-`×50（偶数个 → 25 个 `--` token）使 mypc 100% CPU 自旋 + 内存暴涨挂死
    （`内存占用太大挂了`）；`----1` 等深层前缀链触发。原实现逐运算符反解 `x = x ± 1`
    并 `cloneExpr` 递归副本，每层把整棵子树翻倍 → 节点数 2^n：25 层 ≈ 6700 万节点。
  - 修复：`parseUnary` 先收集**连续**的 `++`/`--` 求和（净增量 N），一次构建
    `x = x ± N`——AST 线性。对纯链语义等价（每层在基址上叠加/对消）：`----x` =
    `x = x - 2`、`++--x` 抵消。
  - 验证：`-`×50/200/5000 全部瞬间完成并干净报错（对字面量赋值）；`--x`/`++x`/
    `----x`/`++--x` 语义正确（新正测试 `tests/prefix_chain/`）；ASAN 干净；
    187/187 回归通过。负测试 `tests/negative/unary_chain_oom.myp`。
- **编译期拒绝在 `@thread` 实例上手动调用 `@startup` 方法（压测暴露的误用 → 编译期诊断）**：
  - 症状：`Worker w = new Worker() @thread; w.run(...)`（`run` 为 `@startup`）在运行时
    SIGSEGV —— `@startup` 已由运行时在 worker 线程自动执行，手动再调一遍 = 双重执行。
  - 修复：sema 记录 `@thread` 注解变量（`VarDecl.has_thread_annotation`），在成员访问
    解析到 `@startup` 方法（`ActionDecl.has_startup`）时报错：
    `cannot manually call '@startup' method 'run' on a @thread instance (auto-invoked in the worker thread)`。
  - 作用域：仅限 `@thread` 实例；普通实例上手动调 `@startup` 仍合法（`mypc run` 依赖此
    路径），回归含 `mypc run` 用例全过。
  - 新增负测试 `tests/negative/thread_startup_call.myp`；185/185 回归通过。
- **协程切换路径剔除 sanitizer fiber 钩子（perf 定位：非 ASan `cpp_long` 的
  `__sanitizer_finish_switch_fiber` NULL 检查占 `__myp_coro_resume`/`__myp_coro_yield`
  自样本 64~80%）**：
  - `__myp_coro_resume`/`__myp_coro_yield`/trampoline 每次上下文切换都做两次
    `if (__sanitizer_start_switch_fiber)` 运行时检查——weak 符号经 GOT 加载 + 分支，
    普通/TSan 构建里恒为 NULL，纯浪费（perf 样本大量聚在切换返回后的检查指令上）。
  - 修复：`src/runtime/runtime.c` 用 `#if defined(__SANITIZE_ADDRESS__)` 守卫——
    只在 ASan 编译（`-fsanitize=address` → gcc 定义该宏）下启用 fiber 钩子；
    普通/TSan 构建编译期整体剔除（行为与 weak-NULL no-op 一致，少 GOT 加载+分支）。
    顺带把 `myp_asan_fake_stack` 从非 ASan 的 TLS 布局里移出（TLS 块小 8 字节）。
  - 效果：`cpp_long`（channel 乒乓 N=10⁷）454→441ms（~3%，交错 A/B 一致）；
    coro_switch 66→62ms。ASan 路径不变：`build-asan` 全局 `-fsanitize=address` →
    `__SANITIZE_ADDRESS__` 定义 → 钩子保留，ASan 冒烟 + 181/181 回归全过。
  - 181/181 正常 + ASan、25/25 Go、33/33 C++ 基准全部通过，无回归。
  - 注：perf 显示剩余热点是 channel send/recv/wake + 2 次上下文切换/消息的固有
    机器开销（切换返回边界的样本涂抹），无单一可安全下刀的大项。

- **修复 `@parallel for` 偶发永久挂起（计数器重置竞态，压测暴露）**：
  - 症状：间歇性 ~1/3 概率 worker 全核空转 + 主线程 barrier 永等；也是此前回归
    `parallel_for` 偶发超时的根因。
  - 根因：`myp_pool_parallel_for` 先推送分块、后重置 `done_count=0`。上次调用
    残留的自旋 worker 在「推送后、重置前」窗口抢到新分块并 `++done_count`，随后
    重置抹掉该增量 → 该分块永久不计 → `done_count` 到不了 `total_chunks` → 挂起。
  - 修复：发布任何新分块前先重置计数器（`runtime.c`）。

- **新增协程/并发压力测试套件 `tests/stress/`**：
  - `run_stress.sh`（-O2 / TSAN=1 / ASAN=1）+ 5 项：`coro_flood`（3.6 万协程创建/
    销毁）、`coro_switch_storm`（400 万次切换）、`channel_stress`（多产多消）、
    `async_io_stress`（loopback TCP）、`parallel_stress`（@parallel for + Atomic）。
  - 独立于 `run_tests.sh`（负载重、含时序数据，按需运行）。

- **可读性重构：拆分 10807 行 codegen.cpp 与 4785 行 sema.cpp / 2952 行 parser.cpp**
  （纯重构，零行为变化，181/181 + 压测 5/5 全过）：
  - `codegen/` → codegen.cpp(核心) / codegen_class / codegen_stmt / codegen_expr /
    codegen_gpu；`sema/` → sema.cpp + sema_expr.cpp；`parser/` → parser.cpp +
    parser_expr.cpp + parser_stmt.cpp。最大文件 10807 → ~2900 行。
  - `convertIntegerValue`/`zextIndexValue` 去 static 并在 `CodeGen.h` 声明（跨 TU）。

- **修复词法层 3 处缓冲区越界读取（fuzz 暴露，畸形输入崩溃 SIGABRT）**：
  - `scanNumber`：`5.`（数字后点 + EOF）时 `source_[offset_ + 1]` 越界（`..` 范围
    符判断未做边界检查）。
  - `scanString` / 字符字面量：`"\` / `'\`（反斜杠是最后一个字符）时转义处理后
    无条件 `advance()` 越过缓冲末尾。
  - 修复：`source_[offset_+1]` 加 `offset_+1 < source_.size()` 守卫；转义处理后
    `if (!isAtEnd()) advance()`。修复前 3036 个边角料输入 3 崩溃 → 修复后 0。

### v3.11.19
- **C 运行时以 -O2 编译（perf 定位：`cpp_long` channel 乒乓 N=10⁷ 的
  `myp_channel_recv` 31.6% / `myp_channel_wake_one` 15.0%）**：
  - `mypc` 链接生成程序时**每次用 gcc 现编 `runtime.c`**，但编译命令没有任何 `-O`
    参数——整个 C 运行时（channel/coroutine/string/ARC/thread pool）一直以 gcc
    默认的 **-O0** 编译：无内联、所有局部变量上栈，连 `myp_channel_get` 这种 3 行
    函数都是独立 call。`mypc -O2` 只优化 MYP 生成的 LLVM 代码，从未作用到 C 运行时。
  - 修复：`src/main.cpp` 链接阶段以 `-O2` 编译 `runtime.c`/`sdl_bridge.c`/
    `runtime_gpu.c`，并把 `-O2` 折进缓存哈希（旧的 -O0 缓存对象不会复用）。
  - 效果（对所有生成程序生效）：
    - channel 乒乓 N=10⁷：**582→453ms（22%）**；`channel_pingpong` bench 6→4ms，
      反超 Go（Go/MYP 1.50）。
    - ARC 压力（3M 对象+字符串拼接）：~515→~450ms（15%）。
    - coro_switch 72→66ms、io_socket 71→69ms；parreduce 反超 C++ 从 2.00→**3.00**
      （1ms vs 3ms），parcomp 也反超 C++（0.88）。
  - 顺手把 channel 环形缓冲的 `% capacity`（每次消息 2 次 idiv）改为有界条件回绕
    （head/count 均 < capacity，单次比较回绕即精确）。
  - 181/181 回归（含 ASan）、25/25 Go 基准、33/33 C++ 基准全部通过，无回归。

### v3.11.18
- **`@parallel for` body 改为 chunk 循环（perf 定位：并行归约 N=10⁸ 的
  `parallel_body_j` 62% / `myp_pool_worker_id` 8.9%）**：
  - 原实现把 body 生成为**每迭代一个函数** `void(i, arg)`，worker 每轮 `call` 一次
    ——每次迭代付 call/ret + 栈帧开销，且 body 是"单条语句"，LLVM 既无法提升
    `Parallel.workerId()` 也无法展开/向量化累加。
  - 改为生成 **chunk 循环体** `void(start, end, step, arg)`（body 内部 `for(i=start;
    i<end; i+=step)`），worker 每 chunk 只调用一次。运行时 `work_fn` 签名由
    `void(int,void*)` 改为 `void(int,int,int,void*)`；`myp_pool_parallel_for` 外部
    签名不变（LLVM IR 中 fn 为不透明 `ptr`），缓存 `.myp.ll` 无需重编。
  - 将 `myp_pool_worker_id` 标记 `readnone nounwind willreturn`（TLS 值在 chunk 循环
    内恒定），让 LICM 把 `workerId()` 提出循环——由此 `perThread[wid]` 地址成为循环
    不变式，LLVM 可把累加器提升到寄存器（打破内存往返依赖链），甚至向量化。
  - 效果：并行归约 N=10⁸：**320→122ms（2.6x）**；N=10⁶ bench 3→1ms；parreduce 反超
    C++ std::thread（1ms vs 2ms，C++/MYP 2.00）。181/181 回归（含 ASan）、25/25
    Go 基准、33/33 C++ 基准全部通过，无回归。

### v3.11.17
- **整数→字符串快速路径（perf 定位：ARC 压力 31% 在 `myp_to_string_i64`）**：
  原实现 `snprintf("%ld")` + `myp_strcat`（glibc 通用格式化 + 内部 malloc + 二次
  strlen/拷贝）。改为手写 2 位查表 itoa：每步 `%100` 反向写两数字（int64 至多 10
  次除法），直接精确分配结果串，无中间缓冲/二次拷贝。
  - 修复过程中发现并修正 i32 路径 bug（负 i32 符号扩展成巨正数 → `-5` 输出错误）；
    边界 ALL PASS（INT64_MAX/INT32_MIN/10^18 等）+ 181/181 回归。
  - 效果：ARC 压力（3M 次对象+字符串拼接）589→~518ms（12%）；主/Go 基准无回归。

### v3.11.16
- **Channel 同步交接（channel_pingpong 21→5ms，反超 Go 1.20）**：`send`/`recv`
  完成缓冲操作后唤醒对端等待者时，若调用方是协程则**立即 `__myp_coro_resume`**
  （Go 式 rendezvous，免一轮 `Coro.scheduler()` 往返）。深度守卫（64）防链式递归
  失控；`close`/`try_*` 保持 ready-only。
  - 前置：v3.11.15 修复的多消费者 count 下溢与句柄槽位复用两个缺陷是本优化的
    安全前提——修复后同步交接 181/181 回归（普通/ASan）+ 4p×2c 压力测试全过。
  - 效果（Go/MYP）：channel_pingpong **0.29→1.20**（5ms vs Go 6ms，MYP 反超）；
    io_socket 1.08、coro_switch 4.22、coro_spawn 0.16 无回归；全 25 基准 verify 全对。

### v3.11.15
- **修复两个协程/通道崩溃级缺陷**：
  - **Channel 多消费者 count 下溢**：`myp_channel_recv` park-resume 路径无守卫
    `count--`——多消费者时第二个消费者被唤醒但值已被第一个取走，count 下溢 -1 →
    环形缓冲越界写 `buf[-1]`（ASan heap-buffer-overflow + munmap 崩溃）。send 的
    park-resume 无守卫 `count++` 同理。修复：send/recv park-resume 后循环重新校验
    缓冲状态（count 不足/已满重新挂起）。回归测试 `tests/channel_multi_consumer/`
    （修复前崩溃）。
  - **协程句柄槽位复用（结果串位）**：`__myp_coro_create` 复用已完成协程的槽位，
    无 await 协程 eager 启动即完成 → 新协程拿到相同句柄 → 已存句柄别名、
    `Coro.result` 读到新协程结果（verify 900≠600）。修复：槽位不复用（句柄唯一）；
    完成协程栈经线程局部 retired 列表延迟回收（create/调度器安全点移回栈池）。
    回归测试 `tests/coro_handle_unique/`。
- **性能**：顺带消除 `__myp_coro_create` 找可复用槽的 O(n²) 线性扫描，
  **coro_spawn 460→24ms（19x）**；channel_pingpong 21ms 无回归；coro_switch 72ms。
  普通/ASan 构建各 **181/181** 回归全过。

### v3.11.14
- **协程上下文切换改为寄存器级汇编（coro_ctx.S，x86-64 SysV）**：
  - 替代 `swapcontext`（ucontext 内部每次 `sigprocmask` syscall，微基准 ~180ns/次）
    为自研寄存器级切换（仅保存/恢复 rsp/rip/rbx/rbp/r12-r15，~13ns/次，13.9x）。
  - 集成：`myp_runtime` 新增 `src/runtime/coro_ctx.S`；`mypc` 链接生成程序时同样
    编译该文件（`src/main.cpp`）；非 x86-64 回退 ucontext；ASan 构建用
    `__sanitizer_start/finish_switch_fiber` 显式通知纤维切换（trampoline 入口补配对）。
  - 效果（16 核 min-of-3，Go/MYP）：coro_switch 400→**72ms**（0.76→**4.26**，
    MYP 反超 Go 4.3x）；channel_pingpong 54→**21ms**（0.11→0.29，Go 快 9x 收窄到
    3.4x）；io_socket 86→**71ms**（0.90→**1.10**，MYP 反超 Go）。
  - 修复过程中的 bug：初始帧定位（`myp_ctx_init` 用 `top-8` 导致保存块越界 48 字节
    堆损坏，改为 `top-64`）；ASan fiber 配对（trampoline 入口补 finish_switch_fiber）。
  - 回归：普通/ASan 构建各 179/179 全过。修复 `run_compare_go.sh` 未调用
    `build_all()` 的缺陷（依赖预编译产物）。

### v3.11.13
- **协程通信/I-O 基准 + 调度器 O(N²) 修复**：新增 `channel_pingpong`（cap=1
  Channel 双向 10⁵ 次收发）与 `io_socket`（回环 TCP 逐字节 ping-pong，@coro +
  `waitFd` vs goroutine + 阻塞 socket）两个 MYP/Go 对比基准，`run_compare_go.sh`
  扩至 25 项。
  - channel_pingpong：MYP 54ms vs Go 6ms（0.11，Go 快 ~9x）——Go channel 双方阻塞
    时直接交接（无栈切换/syscall），MYP 每次 park/resume 走 ucontext 栈切换 +
    调度器驱动（~270ns vs ~30ns）。
  - io_socket：MYP 86ms vs Go 79ms（0.92，基本持平）——都受限于 syscall + 唤醒，
    MYP waitFd 轮询调度代价已与 Go netpoller 相当。
  - **顺带修复真实缺陷**：`myp_coro_waits` 从不压缩，`active=0` 的 wait 记录随等待
    次数线性累积 → 调度器每轮 O(N) 扫描 → 总 O(N²)。io_socket 修复前 6101ms、
    修复后 86ms（71x）；N=50000 回归测试由 ~38s（超时）→ 0.30s（线性）。
    修复：`__myp_coro_scheduler` 入口原地压缩（仅保留 active=1），跨调度器不持有
    表索引，安全。回归测试 `tests/coro_wait_compact/`。

### v3.11.12
- **MYP vs Go 主套件全量对比**：把 21 个主套件基准（sieve..bigint）逐个移植为
  `bench/go/*.go`（由 `bench/cpp/*.cpp` 逐文件翻译，同算法/同规模/同 LCG），
  `bench/run_compare_go.sh` 从 2 个协程专项扩展为 **21 主套件 + 2 协程**共 23 项，
  verify 与 MYP 全部对拍（整数精确、浮点 1e-3 容差）。
  - 结果：MYP 赢 20/21 项，几何平均 ~1.8x；仅 `fft` Go 略快（0.86）。
  - 最大差距在浮点/带宽类：convolution 4.04、kmeans 3.81、matmul 3.47、sobel
    3.17、radixsort 2.20、spmv 2.15——根因是 MYP 走 LLVM O2 **自动向量化**，Go
    默认不向量化。
  - 整数/分支/字符串类 1.1~1.7x：quicksort 1.14、kmp 1.18、heapsort 1.15、
    sieve 1.08、mandelbrot 1.01 等。
  - 移植中修正：sha256 初版用了旧的 64KB 消息长度，与 C++/MYP 的 4MB 不一致导致
    verify 不符（实现本身经 Go 标准库 crypto/sha256 与 sha256sum 双重验证正确）。

### v3.11.11
- **MYP `@coro` vs Go goroutine 协程对比**：新增 `bench/go/`（Go 侧）+ `bench/myp/`
  的 coro_switch/coro_spawn + `bench/run_compare_go.sh`（MYP -O2 vs `go build`）。
  - coro_switch（200 协程 × 10000 次挂起/恢复）：MYP 406ms vs Go 306ms（0.75，
    Go 快 ~33%）——ucontext 交换 vs Go 抢占调度，差距不大。
  - coro_spawn（20000 个只返回协程）：MYP 527ms vs Go 3ms（0.01，Go 快 ~175x）
    ——Go goroutine ~2KB 可增长栈极廉价；MYP @coro 每个分配固定栈（默认 128KB，
    `@coro(stack=KB)` 可调小）+ ucontext 初始化。
  - 结论：MYP 协程适合少量长生命周期任务（I/O/事件），不适合海量短任务。

### v3.11.10
- **`Parallel.setThreads(n)` 超过 CPU 数时打警告**：`myp_pool_set_threads` 在
  `n > sysconf(_SC_NPROCESSORS_ONLN)` 时向 stderr 打一次警告（过订阅通常损害
  吞吐），但仍尊重用户显式指定、不静默 cap。实测 `setThreads(64)` 于 16 核 →
  警告 + 仍 64 worker；`setThreads(8)` 无警告。回归 O0/O2/ASAN 178/178 全过。

### v3.11.9
- **parreduce 0.60→1.00**：并行归约基准曾用 `Atomic.addInt`（每元素原子 RMW），
  但每线程专属槽位（`Parallel.workerId()` 恒定、槽位互斥）**无竞争，普通
  load-add-store 即可**——去掉原子后 MYP 3ms vs C++ std::thread 3ms 完全持平
  （verify 仍精确一致 496532956）。**MYP 并行归约最佳实践：每线程槽位用普通写，
  只有共享槽位才用 `Atomic`**。

### v3.11.8
- **并行归约基准 parreduce**：`@parallel for` + `Atomic.addInt`（每线程
  `Parallel.workerId()` 槽位低竞争）vs C++ `std::thread` 各自累加——verify 精确
  一致（496532956），0.60（C++ 快，因 MYP 每元素原子 RMW vs C++ 线程内普通加）。
  与 parcomp 构成并行基准对（计算/归约）。34 项基准 verify 全一致。

### v3.11.7
- **并行基准 parcomp**：MYP `@parallel for`（16 线程池，写入 slice）vs C++
  `std::thread`（16 线程分块）——0.94 基本持平（MYP 17ms vs C++ 16ms，串行同负载
  180ms+）。首个并行基准，验证 `@parallel for` + slice 的并行效率与手写线程相当。
  33 项基准 verify 全一致。

### v3.11.6
- **`@parallel for` 与 slice 联用**：此前并行体（emitKernelExpr 路径）只支持普通
  数组——`slice<T>[i]` 读写直接 GEP 捕获的 `{data,len}` 结构体值 → LLVM verify 失败；
  `slice<struct>` 字段 `v[i].x` 读/写静默返回 0（verify 错乱）。修复：
  - Subscript 读/写分支识别 slice 变量，解包 `{data,len}` + 边界检查 + GEP
    （`data` 指针线程共享，各线程写不同索引无竞争）。
  - 新增 `emitKernelElementAddr`（slice 解包 / 普通数组直 GEP 统一元素地址）。
  - MemberAccess 读 + Assignment 写处理 Subscript 对象（slice<struct> 与 struct
    数组的字段访问）。
  - 实测：串行 37ms → `@parallel for` 3ms（~12x，16 线程池）；verify 与串行一致。
  - 回归测试 `tests/parallel_slice/`；回归 O0/O2/ASAN 178/178 全过。

### v3.11.5
- **嵌套泛型解析**：`slice<slice<int>>`、`Box<Vec<int>>`、`foo<Bar<int>>(...)`、
  `new Box<int>[]` 此前无法解析——lexer 把 `>>` 合成一个 `GreaterGreater` token，
  泛型收尾期待单 `>`。新增 `consumeGenericClose`：遇 `>>` 消费后压入合成 `>`
  （`pending_` 栈，`peek`/`advance` 优先消费），6 处泛型收尾点统一替换。
- **slice-of-slice 分配大小错误**：`new slice<slice<int>>(n)` 元素大小算成 4 字节
  而非 16（`typeNodeToLLVMType` 无 `slice<T>` 分支，落入内置默认 i32），写穿外层
  slice 数据区 → verify 错乱 + 退出段错误。修复：`typeNodeToLLVMType` 加 slice 分支。
- **嵌套 slice 双下标 `rows[i][j]` 读写 + slice<struct> 字段访问 `v[i].x`**：
  codegen 只处理 Identifier slice / 标量 struct。新增 `sliceTypeOfExpr`（递归解析
  slice 值类型）+ `generateSliceElementAddress`（解包+边界检查+GEP）统一读写路径；
  `generateArrayElementAddress` 扩展 slice 分支。
- 由 slice 类基准（slicevec/slicemat）暴露；回归测试 `tests/nested_slice/`。
- 回归：O0/O2/ASAN 177/177 全过；32 项基准 verify 全一致，slicevec 2.67 /
  slicemat 2.50 反超。

### v3.11.4
- **支持 struct 数组元素字段访问 `arr[i].field`**：此前 `v[i].x` 读/写都报
  "unknown property 'x'"——codegen 只处理标量 struct（Identifier 对象）和链式成员
  （MemberAccess 对象），未处理 Subscript 对象（sema 能解析、codegen 挂）。新增
  `generateArrayElementAddress` 计算 struct 数组元素地址，接入读/写/链式三路径，
  并支持 struct 字段数组（`obj.arr[i]`）。
  - 由新基准 dotprod（struct 数组点积）暴露；回归测试 `tests/struct_array/`
    （字段写/读/循环/整体拷贝/链式嵌套 `o[i].in.a`）。
  - 回归：O0/O2/ASAN 176/176 全过；30 项基准 verify 全一致，dotprod 2.29 反超。

### v3.11.3
- **顶层函数 internal 化（最大一次性能提升）**：此前所有顶层函数发成 external 链接，
  LLVM -O2 内联器因内联成本超阈值拒绝内联（如 convolution 425 > 225）→ 调用点传的
  常量参数无法常量折叠 → 小循环因运行时上界被 cost-model 判"向量化不划算"→ 热点
  内核（convolution/kmp/sha256 等）全部退化为标量执行。
  - 修复：非库构建下把所有函数定义标记 `internal`（仅保留 `main` external），LLVM
    的 IPSCCP/内联器随即常量特化 + 内联 + 向量化；库构建 `--shared/--static` 跳过
    以保持符号导出（顺带修复单文件库构建的 `library_mode` 未传递问题）。
  - 基准效果（MYP -O2 vs C++ -O3，24 项 verify 全一致）：convolution 0.61→1.32、
    kmp 0.63→0.96、base64 0.81→1.29、sha256 0.82→1.20、kmeans 0.83→1.05、
    huffman 0.94→2.12；gol 因常量折叠后 SLP 过度向量化 119→153ms（仍 1.06x 领先
    C++）。
  - 回归：O0/O2/ASAN 175/175、TSan 12/12 全过。

### v3.11.2
- **显式类型转换 `uint8(x)` / `byte(x)` / `long(x)` / `double(x)`**：内置类型名当
  函数调用即转换。宽→窄截断、窄→宽按源符号扩展、double↔int 转换。解决了
  "无法从 `long` 计算填充 `uint8[]`"（base64 基准 0.58→0.81，`uint8[]` 替代
  `long[]` 省 8 倍内存）。
  - 新增回归测试 `tests/convert_expr/`；自举 viz lexer 同步 intN/uintN 关键字。
  - 顺带修复既有缺口：float→double 调用实参（`Console.writeFloat(float 变量)`
    之前 LLVM verify 失败）。
  - 回归：O0/O2/ASAN 175/175 全过。

### v3.11.1
- **修复数组下标窄整数符号扩展 bug**：`cnt[msg[i]]` 里 `msg[i]` 是 `uint8`(i8)，
  作为数组下标被 LLVM GEP **符号扩展**——字节值 >=128（如 190=0xBE 即 i8 -66）变成
  负下标，计数丢失 + 越界写（段错误）。huffman 类基准暴露。
  - 修复：`generateSubscript`/下标赋值/GPU kernel 的 GEP 索引统一**零扩展**
    （`zextIndexValue`：i8/i16/i32 → i64），与 slice 路径的 zext 约定一致。
  - 新增回归测试 `tests/subscript_narrow/`（uint8/uint16 作读+写下标 + int 控制组）。
  - 顺带收益：gol 基准 MYP 165→~120ms（索引变 i64 后 LLVM 对邻居循环优化更好）。
  - 回归：O0/O2/ASAN 174/174 全过。

### v3.11.0
- **无符号整数类型补全（`uint32`/`uint8`/`uint16`/`uint64` 固定宽度别名）**：
  - 新增 `u`/`U` 字面量后缀（`0xFFFFFFFFu`），按值定宽（≤0xFF→`ubyte`、≤0xFFFF→
    `ushort`、≤0xFFFFFFFF→`uint`、更大→`ulong`），可直接初始化无符号变量/数组。
  - 无符号语义：`uint` 的 `>>` 是逻辑右移（`lshr`）、`/`→`udiv`、`%`→`urem`、
    比较用无符号谓词、加减自动回绕；uint→long 拓宽用 ZExt（`0xFFFFFFFFu`→
    `4294967295L`）。`(x>>n)|(x<<(32-n))` 被 LLVM 识别为单条 `rol`/`rorl`。
  - sema：`visitBinaryOp` 无符号类型推断；`typesCompatible` 拓宽表加无符号族；
    codegen：二元运算按 `result_unsigned` 选 UDiv/URem/LShr/无符号比较，新增
    `convertIntegerValue` 助手使调用实参/变量初始化对无符号源做 ZExt。
  - 自举格式化/可视化器同步 `u` 后缀与新类型关键字（对拍通过）。
  - 新增回归测试 `tests/unsigned_types/`（逻辑右移/无符号除/比较/回绕/旋转/ZExt）。
  - 效果：bench sha256 用 uint32 后 32ms→22ms（比值 0.56→0.91），verify 不变。
- **有符号定宽别名 `int8`/`int16`/`int32`/`int64`**（→`byte`/`short`/`int`/`long`，
  与 uint 家族对称补齐）。

### v3.10.2
- **变异模糊测试驱动的 7 项修复**（`tools/fuzz_myp.py`：对 tests/examples/stdlib 种子做
  行级/表达式级变异，用 ASAN 编译的 `mypc` 编译 + 超时分类输出 ——
  CLEAN / CRASH(ASAN) / VERIFY(LLVM verify failed) / INTERNAL / HANG；本次跑 12000
  次迭代收敛为 0 发现）：
  - **未知类型导致 sema/codegen 签名分歧**（`propertyvoid`/`UnknownType`）：sema
    `typeNodeToTypeInfo` 之前对未注册类名静默回退 Void，而 codegen 把同名解析成 i32 →
    函数签名不一致 → LLVM verify "return type does not match"。Fix：未找到类时报
    `unknown type 'X'`（@macro 的 AST 类型 StmtList/Stmt/Expr 保留 Void 兜底）。
  - **非 void 函数缺失 return 落到 `ret void`**（verify 崩溃）：sema 新增保守的
    `stmtGuaranteesTermination`（Return/Throw/Block 末句/If+else 双支/Match 全臂/
    Try+finally/while(true)/for(;;)）+ `checkMissingReturn`，在 `analyze` 与
    `visitFuncBody` 的 action/function/struct 方法/顶层函数各调用点报
    `missing return statement`（跳过空体 FFI 桩与 @coro/@async 的 Void 保护）。
  - **空体非 void 函数/action/static 返回 `ret void`**（`int helper() {}`、
    `@async long sleep() {}`）：codegen 三处 fall-through（generateClassFunction /
    generateClassAction / generateStaticAction）按返回类型补零值 `CreateRet`。
  - **编译器 use-after-free（ASAN）**：`visitFuncBody`/action 的
    `visitStmt` 触发的单态化会 realloc `tu.functions`/`tu.classes`，读
    `decl.range`/`decl.body` 悬垂。Fix：visitStmt **前**捕获 `SourceRange` 与
    `shared_ptr` body。
  - **`throw <void 表达式>`**（`throw Console.writeString(...)`）：codegen 会发
    `myp_throw_object(<badref>)` → verify。Fix：visitThrowStmt 对真 void 报
    `throw requires a string or class instance, got 'void'`（仅在有既有错误时静默恢复）。
  - **泛型模板体 use-before-decl 的 codegen 晚期错误**（`map<T,R>` 体内 `r` 未定义，
    sema 跳过模板体）：codegen 之前用 i32 0 占位 → LLVM verify 崩溃。Fix：
    `CodeGen::generate` 在 finalizeDebugInfo 后 `diag_.hasErrors()` 即干净中止
    （不再把类型不兼容占位送入 verify）。
  - **枚举体错误恢复死循环（HANG）**（`enum { a.b; }` / `enum { 1; }` / `enum { "s"; }`）：
    `parseEnumDecl` 体内 token 既不能被 parseIdentifier 也不能被 consume(Semicolon)
    消费时（两者失败都不前进）→ 无限循环。Fix：循环内记录 `before = current_`，
    迭代无前进则 `advance()` 一次保证前向进度。
  - **嵌套 struct 成员赋值静默失效（顺带发现的 wrong-code bug）**：
    `o.inner.val = 3.14` 之前落到 codegen `unknown property` 兜底（写 i32 0），
    且 mypc 退出码为 0 → 错误 expected 文件掩盖（`inner.val=0`）。Fix：新增
    `generateStructMemberAddress`（递归解析 `v.a.b` 链地址），赋值处理器在
    `if(!op)` 层级处理 `MemberAccess` 目标并 GEP/store；`tests/nested_struct`
    expected 更新为真值 `inner.val=3.14`。
  - 验证矩阵：**-O0/-O2/ASAN 全套 173/173、TSan 12/12**；12000 次 fuzz 收敛为 0
    CRASH/VERIFY/INTERNAL/HANG。

### v3.10.1
- **系统探测驱动的 8 项修复**（`tests/probe.sh`：编译+运行每个 `.myp` 的 -O0/-O2
  输出与崩溃对比；覆盖枚举/类数组/泛型/命名 lambda/字符串/slice/异常/协程/接口/静态/
  数值运算符，~20 个探测用例全部 PASS）：
  - **`new Generic<Arg>[n]` 解析失败**（`new Box<int>[2]`）：parser 解析完泛型实参后
    遇 `[` 未走数组分支。Fix：type_args 后检查 `[` → NewArrayExpr（元素带 type_args）。
  - **泛型类数组元素类型解析成 i32**：`typeNodeToLLVMType`/`typeNodeToCodegenType`/
    `isArcClassType` 未对 class_name+type_args 做实例 mangling（`Box<int>`→`Box_int_inst`）。
  - **数组元素方法分派到模板**（`boxes[0].take()` → 模板签名未定义）：新增
    `array_elem_class_map_`（数组变量→元素类名），`best_class` 的 obj_cls 对 Subscript 用它。
  - **struct 字段数组下标元素类型解析失败**（`bg.nodes[0]` 元素 i32 错读）：generateSubscript/
    赋值下标的 MemberAccess 分支只查类、漏 struct 字段；加 `findStruct` 字段类型解析。
  - **`new Box<Node>().make2()` 方法解析到模板**：`memberObjectClassName` 辅助（Identifier/
    this/Subscript/NewExpr/Call → 类名，泛型 mangling）。
  - **内联调用类返回方法泄漏 retain-at-return 的 +1**（`obj.take().get()` 每次泄漏 1）：
    `generateCall` 包装器对 `callReturnsArcRef`（callee 返回类/类数组）的结果 push 语句末
    临时；`return f()`（f 返类）作 fresh 转移（consume+skip retain）。
  - **三元表达式数值字面量分支类型不统一**（`x>0 ? 1 : x` byte vs int 报错）：sema
    `isNumericKind`/`commonNumericKind` 统一到较宽数值类型；codegen `generateTernary`
    两分支 SExt/FP 拓宽到共同类型再建 phi。
  - **链式泛型方法调用**（`pp.first().first()` 外层对象是 CallExpr）回退到模板：
    `callReturnClassName` 解析调用返回类型类名；`memberObjectClassName` 加 Call 分支；
    两处 obj_cls + 名字兜底统一用它。
  - 验证矩阵：**-O0/-O2/ASAN 全套 173/173、TSan 12/12**；showcase O0==O2==ASAN 逐字节一致。

### v3.10.0
- **showcase 差分测试驱动的 4 项修复**（`examples/showcase.myp` 作为语言能力展示 +
  差异测试工件，暴露并回归了以下缺陷）：
  - **枚举带数据变体载荷恒为 0**：`Shape.Circle(2.5)` 之前只存判别值、match 绑定硬编码 0
    （`data(v)=0`/`radius=0`）。修复：枚举 LLVM 类型从 `i32` 改为结构体
    `{ i32 disc, [N x i8] payload }`（N = 最大变体载荷字节）；构造打包载荷
    （`buildEnumVariant`，与 `getEnumStructType` 同字节偏移）、`generateMatchStmt`
    提取判别 + 按偏移解包载荷绑定、等值比较按判别（Eq/Ne/Lt..）；`typeNodeToLLVMType`/
    `typeNodeToCodegenType`/局部变量分配均识别枚举。`tests/enum_match` 期望更新为真值。
  - **`ArrayList<T>`（T 为类）销毁时元素不释放** + **动态/固定类数组元素泄漏**：
    `new T[n]`（T 为类）之前走裸 `myp_region_alloc`，无长度、无释放。修复：**引用计数类数组**
    ——运行时 `myp_alloc_class_array` 分配 24 字节头
    `{ count:u64, elem_size:u32, pad:u32, rc:u32, type_id=MYP_ARR_TYPE_ID }`
    （rc/type_id 与类对象头同偏移，`myp_retain`/`myp_release` 统一可用；
    `myp_release` 见 magic 即逐元素释放再 `myp_free_class_array`）；
    codegen `isArcRefType` 对动态类数组返回 true（销毁桩/字段存储/作用域退出/临时释放
    自动接管）、`generateNewArrayExpr` 类元素走 `myp_alloc_class_array` + 语句末临时、
    局部动态类数组注册 ARC 槽、固定 `[N x T]` 栈数组注册 kind-3 槽（`myp_release_fixed_class_array`
    按 count 释放元素不 free 栈缓冲）、`return` 转移/retain-at-return 覆盖类数组、
    `heapCopyArrayReturn` 对固定类数组做引用计数深拷贝。
    `tests/arc_m2` 期望更新（`after=2` 不再编码数组泄漏）。
  - **泛型模板体 for-in 崩溃**（sema 跳过模板体 → ForInStmt 注解未计算 → codegen 读默认值
    → LLVM 对齐栈溢出）：模板体递归注解（`annotateForInsInStmt`）+ codegen 兜底报清晰错误
    （不再崩溃；迭代泛型集合仍建议索引循环）。
  - **命名 lambda 自引用失败**（`fn fact(n) => ...` 递归本名解析不到）：AST 加
    `LambdaExpr::name`/`ClassDecl::lambda_name`；sema 在 `__call` 作用域声明自名
    （函数类型、不捕获），调用解析 `resolved_call_name = <cls>__self`；codegen 识别
    `__self` 后缀走 `this` tramp 递归。`/tmp/nl2.myp`（含捕获 + 自递归）O0/O2 通过。
  - 验证矩阵：**-O0/-O2/ASAN 全套 173/173、TSan 12/12**；`arclist.myp`（d1=0）、
    `arrleak.myp`（fixedarr=0/dynarr=0）、`arrfull.myp`（传参/返回/覆盖/字段级联，leak=0）。

### v3.9.0
- **异常 × -O2 修复（§五-3 × 优化管线）**：`-O2` 全套复验（套件涨到 173 后首次）暴露
  `result` **段错误** + `arc_throw` **泄漏**——异常 dispatch/propagate 读 try 内 ARC 槽，
  其唯一 def 在 try_block（不支配 longjmp 路径），LLVM 把 load 折叠成 `undef`
  （`MYPC_DUMP_OPT_IR=1` 可见 `call myp_release(ptr undef)`）。修复：运行时
  `myp_release_slot(槽地址, kind)` 读**物理槽位**再释放（对 LLVM 不透明）+ `registerArcSlot`
  内 `myp_try_escape` 让槽逃逸保住 try_block 的 store（协程帧镜像仍先 `emitCoroFrameClear`）。
  **附带**：`mypc run` 支持子命令前 flag（`-O2 run file.myp`，提取 -O 级传给编译）；
  5 个自举子脚本（pm/gitee/fmt/viz/run）`"$MYPCC"` 引号在 `MYPCC` 带参数时失效→去引号 +
  `MYP_ABS` 取首个词。验证矩阵：**-O0/-O2/ASAN 全套 173/173、TSan 12/12**。
- **反射 / RTTI（§五-4，additive）**：class 对象头 `{rc, type_id}` 自带运行时类型 id。
  新增 `stdlib/rtti.myp` 静态类 `Rtti`——`typeOf<T>(obj)`（运行时类名）、
  `typeId<T>(obj)`（运行时类型 id）、`sameType<T,U>(a,b)`（同类型判定；null → id 0 / 空名）。
  实现：codegen 并列生成 `__myp_type_name_table`（type_id → 类名，ExternalLinkage 常量表，
  无类 TU 退化为 `[null]`）；新内建 `__myp_type_id`/`__myp_type_name`（sema 注册 +
  `intrinsic_map_` 解析）→ runtime `myp_obj_type_id`/`myp_obj_type_name`。`tests/rtti`。
  - **§三 类型系统增强（additive）**：
  - **类型别名 `type X = ...`**：上下文关键字（仅顶层 `type <Id> = <Type> ;` 形态），
    别名可在任何类型位置使用、支持别名套别名、递归别名编译报错。`tests/typealias` + 负测试。
  - **`Option<T>` 可空容器 + `T?` 语法糖**：stdlib `option.myp`（`Option()`=none /
    `Option(T v)`=some + `isSome/isNone/get/getOr/set/clear`）；`Type?` ≡ `Option<Type>`
    （类型位置）。`tests/option`。
  - **泛型函数 `T foo<T>(T x)`**：显式类型实参或实参推断（含 `T[]` 推元素类型）；
    按类型实参单态化（`foo_int_inst`），模板不生成运行时代码。`tests/generic_func`。
  - **一等函数与闭包（M-FN-1/2/3）**：函数类型 `(A,B)->R`（胖指针 `{closure, call_fn}` +
    统一 tramp）；lambda 按值捕获（标量/字符串深拷贝、class 引用浅拷贝、嵌套、上下文类型
    推断）；命名 lambda `fn name(...) =>` 递归；泛型高阶函数 `mapOpt`/`foldInt` + `Option.map`。
    设计见 `docs/function.md`；`tests/function`。
  - **元组 + 解构（TUP-1/2）**：元组类型 `(int, string)` + 字面量 + **多值返回** +
    声明式/赋值式/嵌套解构 + 字段访问 `t.N`；与函数类型/lambda/元组变量声明四路消歧。
    设计见 `docs/tuple.md`；`tests/tuple` + 3 负测试。
  - **泛型 `@static` 类方法（M-FN-3 stdlib 落位）**：`static:` 段方法名后带类型参数
    （`List.map<T,R>`），跨模块可见；`resolveGenericStaticCall` 单态化到 `tu.functions`
    （`__gs_<Class>_<method>_<types>_inst`）。`tests/generic_static`。
  - 全库回归：141/141（`-O0` + ASAN）。
  - **trait 默认实现（§三-5，additive）**：
  - 接口方法**带默认体** → 实现类可省略该方法，虚表回退默认函数；类覆盖则用覆盖。
  - 实现：按类特化默认函数 `__ifdef_<Iface>_<method>_<Class>`（预声明 + generateClass 生成），
    `this` 绑定具体类 → 默认体内 `this.method()`/裸方法调用**静态解析到类方法**（含默认调默认）；
    sema `checkInterfaceImpl` 放行带默认体的接口方法，纯签名方法仍强制实现（负测试）。
  - `tests/interface_default`（默认/覆盖/this 分派/默认调默认）+ 负测试。
  - 全库回归：144/144（`-O0` + ASAN）。
  - **关联类型（§三-5，additive）**：接口 `type Item;` 声明抽象关联类型，实现类
    `type Item = int;` 绑定（**必须绑定**，负测试 `assoc_unbound`）；接口方法参数/返回
    引用关联类型；绑定经 `X::Item` 直接引用（局部变量/参数/返回类型）；泛型
    `where T:I` 内 `T::Item` 实例化后单态化为具体绑定（约束类型参数注册为接口类型、
    `T::Item` 替换、Assoc 通配；codegen `::` 拦截 + `resolveAssocType` + 类参数
    `var_class_map_` 注册使 `c.method()` 精确解析到具体实例类）。
  - `tests/assoc_types`（基本引用 + 泛型 T::Item + 关联类型参数/返回）+ 负测试。
  - 全库回归：146/146（`-O0` + ASAN）。
  - **class 实例 ARC（§五-1，M-ARC-1，additive 无新语法）**：自动引用计数回收中寿命对象。
    - 对象头 `{rc:u32, type_id:u32}`（数据指针前 8 字节）；`myp_alloc_object/retain/release/
      free_object`；每类销毁桩 `__myp_destroy_<Class>`（级联释放类/接口引用字段）+ 按
      type_id 分派的 `__myp_release_table`（ExternalLinkage，运行时按需读取）。
    - 插桩：作用域退出释放局部类/接口引用槽（参数/`this` 借用不释放）；函数返回
      **retain-at-return**（借用返回/新对象均覆盖，调用方转移接管）；赋值 retain-new
      （fresh new/call 转移）release-old（自赋值安全）；属性存储（`this.prop`/裸 `prop`/
      静态属性/映射全局）retain；`slice<T>` 类元素 retain/release；`var x = new` 归槽。
    - 修复潜在 bug：类结构体**两遍构建**（自/交叉类属性引用解析为 ptr，原为 i32 致
      指针字段损坏，ARC 测试暴露）；`myp_release` free 前缓存 rc（原 free 后读头 → UAF）。
    - 诊断：`Memory.liveObjectCount()`（当前线程存活实例数）。
    - `tests/arc`（生命周期/级联/自赋值/借用返回/循环不累积）；全库回归 147/147（-O0+ASAN）。
    - **M-ARC-2（2026-08-06）**：`T[]` 数组元素 retain/release（局部/this.arr/obj.arr，
      slice 同）；语句末临时释放（`new` 作实参/丢弃不累积，强槽 store 消费）；`return new T()`
      转移（跳过 retain-at-return，修 M-ARC-1 fresh-return 泄漏）；函数 epilogue release
      （修 return 结尾局部泄漏）；`@thread`/`@threadpool` 实例在 `myp_thread_destroy` 释放
      startup_arg（线程池改 `myp_alloc_object` 带头）。修复：lambda 闭包/`@thread` 实例
      临时消费（ASAN 捕获的过早释放）；emitFunctionReturn 顺序（retain 先于 release、
      main 的 release 先于 `myp_free_all`）。`tests/arc_m2`；全库回归 148/148（-O0+ASAN）。
    - **M-ARC-3（2026-08-06）**：**闭包释放**——函数值局部注册为 ARC 槽（fat pointer
      index 0 = 闭包），作用域退出释放；`LambdaExpr` 视为 fresh（闭包是新分配的 class
      实例）；别名赋值 retain 闭包；捕获的 class 引用在 `generateLambda` **retain**（闭包
      拥有自己的引用，销毁桩级联释放平衡——修外层局部释放后闭包持悬垂借用的 UAF）。
      `tests/arc_fn`；全库回归 149/149（-O0+ASAN）。
    - **剩余**：异常/throw-catch 展开释放（v1 泄漏安全，`myp_free_all` 兜底）、协程帧
      释放、`@region` 逃逸精修。设计见 `docs/arc.md`。
    - **同步原语 stdlib（§五-2，additive）**：`sync.myp`——`Mutex`（普通+可重入
      `PTHREAD_MUTEX_RECURSIVE`，tryLock）、`RWLock`（pthread_rwlock，try rd/wr）、
      `CondVar`（wait 关联 Mutex handle + signal/broadcast）、`Semaphore`（POSIX sem_t，
      tryWait）、`Once`（enter/done call-once 惯用法）。全部 handle 模式（同 `Barrier`，
      固定数组+分配表+槽位互斥，每类 64 槽）。`tests/sync`（4 `@thread` worker 用 Mutex
      保护 `@static` 共享计数 400 确定性 + CondVar 生产者/消费者 + tryLock/递归/RWLock/
      Semaphore/Once API 检查）；全库回归 150/150（-O0 + ASAN）。
  - **for-in / 集合迭代（§四-2，additive）**：`for (x in coll)` / `for (T x in coll)` /
    无括号 `for x in coll`。四种可迭代源：**固定数组** `T[N]`（编译期长度）、**slice**
    `slice<T>`（`.size()`）、**集合类**（需 `size()`+`get(int)`，de-facto 迭代器协议，
    如 `ArrayList<T>`）、**range** `for (i in a..b)`（右开 `i<b`，等价索引式 for）。
    循环变量每次迭代声明（作用域级），`break`/`continue`/嵌套支持；迭代源只求值一次。
    ARC：类迭代集合适用临时持有 + 循环末释放；类元素（数组/slice 下标借用→retain、
    类 get 结果 fresh 转移）循环变量作用域末释放，实测零泄漏。负例：动态数组 `T[]`/
    非可迭代类/非集合类型/集合元素为数组均编译期报错。`tests/for_in`；全库回归
    151/151（-O0 + ASAN）。
  - **默认参数 / 命名实参（§四-1，additive）**：`Param ::= Type Identifier ('=' Expression)?`
    默认参数——顶层函数/类方法（action:/function:）/静态方法/构造器/struct 构造；事件与
    枚举数据字段不允许默认值。调用点 `f(name = value)` 按赋值表达式解析，sema 按「目标
    标识符匹配形参名」重解释为命名实参（乱序可用；与宏的赋值实参 `$n/$body` 无歧义——
    宏参数名永不匹配普通标识符）。位置实参按序填前 N 形参、命名按名填入、缺失且有默认值
    者克隆默认表达式在调用点求值；声明期校验默认值类型兼容。泛型函数/泛型静态方法亦支持
    （实例参数克隆保留默认值 + 实参规范化）。负例：未知/重复命名、位置+命名重叠、必填
    缺失、实参过多、默认值类型不匹配。`tests/defarg` + 6 负测试；全库回归 158/158（-O0+ASAN）。
  - **值式错误传播 Result<T,E>（§五-3，additive）**：`stdlib/result.myp`——`Result<T,E>`
    二态容器（`Result()`=err / `Result(T v)`=ok + `isOk/isErr/get/getErr/getOr/setOk/setErr`）；
    顶层泛型工厂 `resultOk/resultErr`（部分类型实参推断）；组合子 `resultMap/resultAndThen/
    resultMapErr`（无异常错误传播）；异常桥 `resultTry<T>((() -> T) f) -> Result<T,string>`
    （`catch (string s)` 优先 + `catch (Error e)` 用 `e.message()`）。`error.myp` 补
    `StringError.setMsg`。`tests/result`；全库回归 159/159（-O0+ASAN）。
    **附带修复 5 个既有 bug**：①lambda 捕获分析误捕全局函数/类名（`collectExprCaptures`
    加 `isGlobalName` 过滤，lambda 内可直接调用顶层函数）；②`string + bool` 拼接把 i1 传
    给 `myp_to_string_i32`（改 `myp_to_string_bool` + sext）；③泛型函数内 `new G<T>(args)`
    带参构造不调用构造器（codegen 按具体实例类名 + 实参个数重建 ctor）；④接口值来自函数
    返回值赋给接口变量时把胖指针当实例指针（interface passthrough）；⑤`catch (string)`
    绑定共享 `myp_error_msg` 缓冲、后续 throw 覆写导致存下的错误消息漂移（绑定前
    `myp_strdup` 拷贝——`resultTry` 多连调用即踩中）。
  - **stdlib 缺口补强（§六-4，additive）**：
    - **crypto/hash**：`stdlib/crypto.myp`——`Crc32.crc32/crc32Hex`（IEEE 802.3，
      原始 32 位值 + 8 位无符号十六进制显示）+ `Hash.md5/sha1/sha256`（小写十六进制）。
      核心在 C 运行时（`myp_crc32`/`myp_hash_md5`/`myp_hash_sha1`/`myp_hash_sha256`，
      MYP 侧静态类封装）。已知测试向量回归（含 56 字节跨块消息）。
    - **sprintf 格式化**：`stdlib/fmt.myp`——`Fmt` 静态类（默认参数）：`i`（十进制）、
      `u`（无符号十进制）、`x/X`（无符号十六进制大小写）、`o`（八进制）、`b`（二进制）、
      `f/e/g`（定点/科学/最短，精度）、`s/sR`（字符串左右对齐），全部支持宽度 + 填充字符。
      运行时补 `myp_fmt_u64_base`/`myp_fmt_double_f/e/g`（snprintf）。
    - **随机分布补强**：`random.myp` 加 `range(lo,hi)`（[lo,hi) 均匀）、`exponential(lambda)`
      （逆变换采样）、`poisson(lambda)`（Knuth 算法）。
    - **HTTP 客户端**：`stdlib/http.myp`——基于 `net.myp` TCP 的 HTTP/1.1 客户端
      （仅 `http://`，无 TLS）：`Http.get/post/request` + `HttpResult`（status/body/
      header 大小写不敏感/isOk）。URL 解析（scheme/host/port/path/query）；响应状态行/
      头解析；`Content-Length` 定长体、`Transfer-Encoding: chunked` 分块体、关闭定界体。
      连接失败抛 NetError、非法 URL/非 http scheme 抛 string。自包含测试：本地
      `TcpServer` 响应线程（@thread）验证 GET/404/chunked/POST/scheme 校验。
    - `tests/crypto` + `tests/fmt` + `tests/random_dist` + `tests/http`；全库回归
      **166/166**（-O0+ASAN）。
  - **`mypc run`（仿 `go run`）+ 单类文件自动 `main`（additive）**：
  - `mypc run file.myp [args]`：编译到临时产物 → 链接 → 直接运行 → 清理；退出码=程序
    退出码；args 透传（`main(argc,argv)`/构造器）。
  - **单类文件无 `main` 也可 run**：sema Pass 1 后注入合成 `main()`（实例化类并触发其
    `@startup` 入口）；`FuncDecl.is_auto_main` 豁免 main() 直接调用限制；无 `@startup` /
    多 `@startup` 类编译报错。正常编译（非 run）仍要求显式 main。
  - `tests/test_myp_run.sh` 8 断言；全库回归 142/142（`-O0` + ASAN）。
- **类构造器（M1-M4，additive）**：`@constructor` 注解 + **函数名==类名隐式构造器**。
  - M1 语法 + AST：`parseActionDecl`/`parseFunction` 识别 `@constructor`；`@constructor`
    构造器**无返回类型**（`@constructor Window(...)`，不写 `void`）且名称**必须==类名**
    （编译校验）；类/struct 中方法名==类名 → 隐式构造器（可省略注解，C++/Java 风格）；
    struct 新增 `action:` 节（方法/属性前瞻区分）。
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
- **`@parallel for` 多线程检测 + 线程池 API 扩展**：
  - 运行时新增 TLS `myp_pool_worker_id()`（当前 worker 索引，`@parallel for` body 内
    返回 0..N-1，非池线程 -1）；`emitKernelExpr` 支持其直接调用；
    `tests/parallel_for/` 用 `Parallel.workerId()` 检测多线程真正启动。
  - 池运行时新增 `myp_pool_worker_count()`（实际 worker 数）、`myp_pool_is_active()`
    （是否初始化）、`myp_pool_set_threads(n)`（池大小，首次创建前生效，0=自动）；
    `myp_pool_init_global` 遵循设定的线程数。
  - `stdlib/pool.myp` FFI 集中于此文件，`Parallel` 静态类扩展为完整查询/配置 API：
    `threadCount()`/`workerCount()`/`workerId()`/`isActive()`/`setThreads(n)`。
  - 新增 `tests/pool/`（`setThreads(2)` 确定性断言池大小、worker 索引区间、池内外 workerId）。
  - 验证：`-O0`/ASAN 全套 123/123。

### v3.8.0
- **切片类型 `slice<T>` + `@region` 两级 arena（P4，additive）**：
  - P4a `slice<T>` 值类型：`{ T* data; int64 len }` fat pointer；`new slice<T>(n)`、
    `s[i]`（运行时边界检查）、`.size()`/`.length()`、值传递/返回（逃逸→进程级）、
    浅拷贝共享 data、空 slice；grammar v1.0 列为内置类型。
  - P4b 两级 arena + `@region` 注解：region-local arena（`emitRegionEnter` + TLS 追踪），
    `@region` 函数退出时回收本 region 分配，返回引用类型时逃逸到上层。
  - P4c slice 集合二元：`@op("+")`/`@op("*")` 元素级运算 + 标量广播（A+B / A*k / k*A）。
  - P4d grammar 增量（`slice<T>`/`@region` 入规格）+ region 动态作用域（嵌套/逃逸规则）。
  - 测试：`tests/slice/`、`tests/slice_binop/`、`tests/slice_more/`、`tests/region/`、
    `tests/region_chain/`。设计见 `docs/slice.md`；算子见 `docs/operators.md` P4。
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

### v3.6.0
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

### v3.5.0
- **元编程 M3：声明式宏 `macro`**（`src/macro/macro_expand.cpp` + `include/mylang/Macro.h`）。
  - `macro name($a, $b) { ... }` 顶层声明；`$param` 为模板占位（新 token `Dollar` + `Keyword_macro`）。
  - 宏体是普通 MYP 块；展开为 AST 深拷贝 + `$param` → 实参 AST 替换（parse 后、sema 前）。
  - 支持表达式参数（`log($a)`）、语句参数（`repeat($n, $body)`）、赋值参数（`$x = ...`）、
    嵌套宏（`twice(addN(v,10))`，迭代展开 + 深度上限）。
  - `--macro-expand`：展开后 AST dump 调试。
  - 验证：`tests/macro/`（repeat+addN+twice+log → v=37）；`-O0`/`-O2` 全套 113/113。
  - 设计见 `docs/metaprogramming.md`。

### v3.4.0
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

### v3.3.0
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
