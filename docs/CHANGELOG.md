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

### v3.15.244 — M-1 表达式/值宏：声明式宏表达式位展开（additive，selfhost）

**M-1 缺口**（docs/review/metaprogramming_gaps.md §2）：声明式宏只能**语句位**展开，
无法产出内联表达式/值（表达式位调用宏 → `undefined symbol`）；Rust `macro_rules!`
有 expr 位置。规格根源：docs/metaprogramming.md §5.6 明列「表达式位置的过程宏
（V2）」未实现。已实现声明式宏表达式位（路径 A）。

**语义（additive，语法零新增，语言规格 1.0 不变）**：宏体为**恰一条 ExprStmt**
（单表达式宏）时，可出现在任意表达式位置，展开为深克隆表达式（$param → 实参
AST 片段）：

```myp
macro twice($e) { ($e) + ($e); }
int r = twice(21);        // 展开为 (21) + (21) → 42（此前 undefined symbol）
int a = sel(c > 1, x, y); // 多 $param；实参可为含宏的表达式
int q = quad(2);          // 宏内嵌宏：dbl(dbl($e))
```

- 位置：VarDecl 初值 / 赋值 RHS / return / 实参 / 三元分支 / 嵌套（二元/下标/调用
  实参内部）……泛型函数体（显式类型实参实例化）亦可用。
- 单表达式宏天然两位置共用（语句位 `twice(x);` 照旧展开为语句）；多语句/空体宏
  只能语句位，表达式位 clean reject（MYP 无 block-expr；语句级控制流仍走语句位宏）。
- 与语句位宏同一套 $param 片段捕获；M-3 宏卫生经 MacroParam 替换区 mhRn_=0 保持
  （调用方标识符不改名）；单 ExprStmt 体无自声明局部 → 无需 gensym。

**实现**（sema.myp，expandMacros pass2，parser/ast 不动）：所有语句位展开完成后整树
再遍历——expandStmtExprs 按语句 kind 提取表达式字段（Block/ExprStmt/VarDecl init/
Return/If/While/For/Throw/Await）→ expandExprMacros 递归表达式树，命中
`Call(Identifier 宏名)`：singleExprMacroBody（体=恰一条 ExprStmt）取表达式深克隆注入
并继续展开嵌套宏；非单表达式体 → clean 诊断。

**验证**：`tests/@test/macro_expr_value.myp`（11 断言：review 核心/三元+嵌套/多
$param/实参/return/宏嵌宏/赋值 RHS/泛型显式实例）；负测试
`tests/negative/macro_expr_multistmt.myp`（多语句体表达式位 reject）。探针确认：
嵌套 `twice(2)*3`/`1+twice(4)`/实参位/泛型体；双编译器 seed 冻结不 backfill
（selfhost-only additive，同 M-2/M-3）。全量 547/547 + bugs 20/20 + 自举门通过。

### v3.15.243 — M-2 编译期常量表：模块级 @eval 只读常量数组（additive，selfhost）

**M-2 缺口**（docs/review/metaprogramming_gaps.md §2）：编译期表/数组常量不存在——
`docs/metaprogramming.md §3.1` 宣称的模块级 `@eval <类型> <name> = {...}` 表语法此前
不可编译（解析为 @eval 函数 → `expected '(' after function name`）。已实现。

**新语法（additive，语言规格 1.0 不变）**：模块级只读常量数组，编译期固化为 LLVM
`private constant [N x T]`；运行时 `name[i]`（i 可运行时变量/表达式）编译为对常量数组
的 GEP+load（无运行时分配/ARC，.rodata 只读）：

```myp
@eval int[8] pow2 = [1, 2, 4, 8, 16, 32, 64, 128];   // 定长 N
@eval int[]  pow3 = [1, 3, 9, 27];                    // N 由元素数定
const int BASE = 10;
@eval int[4] mix = [BASE, 5, 100, 200];               // 顶层 const 标量引用元素
int v = pow2[i];                                      // 运行时变量下标
```

元素支持：数字/布尔字面量、纯常量算术（`2*3+1`）、顶层 const 标量引用（`[BASE, 5]`，
算术内 const 引用暂不支持）。元素类型限标量（byte..double/bool；string/嵌套拒）。
表只能下标读（`name[i]`）；裸表名引用 clean reject。

**实现**（自举编译器，mirror 设计定稿 m2-const-table-design）：
- parser：`@eval` 后类型+名后遇 `=`（非 `(`）→ 表声明（AstFunction.evalTable_；
  body=Return(数组字面量)），原 MYP 数组字面量 `[..]`（文档 `{..}` 为笔误）。
- sema：findEvalTable（表不注册 funcIdx_/不跑普通函数体分析——数据壳）；
  validateEvalTables（数组类型/标量元素/定长 count 匹配/常量元素）；Identifier 裸表名
  clean reject；Subscript 特判（元素 kind/resolvedClass，数值下标检查）。
- codegen：emitEvalTables 发 `@__myp_table_<name> = private constant [N x T] [...]`
  （staticPropConst 折叠，const 引用递归展开 init）；genExpr Subscript 特判
  GEP+load；subscriptElemLt 表元素类型（bool→i1，对齐 exprLlvmType/toI1）。
- 表不发射运行时函数；顶层函数循环/constDecl 并列 skip evalTable。

**验证**：`tests/@test/m2_eval_table.myp`（10 断言：int 求和/运行时+表达式下标/动态
形态/const 引用/常量算术/bool/double/long）；负测试 `tests/negative/m2_eval_table_*
.myp`×3（裸表名/非标量元素/定长 mismatch）；`--emit-llvm` 检查 `@__myp_table_*`
private constant + GEP 发射。全量 545/545 + bugs 20/20 + 自举门（2 级 MD5 一致）通过。
已知限制（记录 additive）：@eval 函数调用直接作表元素暂不支持（常量初值无运行时
执行；需 const 中转，const init 为 @eval 调用的展开折叠仍未接——后续轮）。

### v3.15.242 — BUG-153 修复：coro 反应堆非阻塞 accept（stdlib net acceptNb）（stdlib/运行时）

**BUGLIST BUG-153（运行时，非编译器）**。stdlib `net.myp` `TcpServer.acceptNb`。

- **根因**：coro 反应堆 acceptor（`Coro.waitFd(监听fd)` → 阻塞 `accept()`）在真并发突发下，
  poll 就绪与 accept 间竞态 → accept 时队列已空 → **阻塞 accept 永久卡死** → acceptor 不再
  进 scheduler → 事件循环停摆（进程存活、acceptor 不再被 poll、请求全超时）。strace 证据：
  3 次成功 accept 后第 4 次 `accept(3)` 无返回、进程被 kill 时仍阻塞在 accept syscall。纯
  stdlib 自包含复现（无 fork）稳定触发（round1 即楔）。
- **修复**：`TcpServer.acceptNb` = 监听 fd 置 `O_NONBLOCK` 后 accept，无待接连接立即返 -1
  （EAGAIN），调用方回 `waitFd` 继续等，绝不在事件循环内阻塞（标准 reactor 语义）。同步
  serve（阻塞 accept 等连接）继续用原 `accept()` 不变。coro 反应堆（myp_http/复现 srv.myp）
  改用 acceptNb。
- **验证**：`tests/bugs/b153_reactor_wedge/repro.sh`（自包含纯 coro 反应堆，40 轮 2 路并发 +
  跟进单发）修复前 round1 楔 → 修复后 **40 轮全 200 NO-WEDGE**；全量 541/541 + bugs 20/20
  （async_socket 等 net/coro 目录无回归）。mypagent myp_http coro serve 侧同步改用 acceptNb
  （跨项目跟进）。

### v3.15.241 — M-3 声明式宏卫生：宏体自声明局部 gensym（不撞调用方作用域）（selfhost additive）

**非破坏性硬化（metaprogramming_gaps M-3，声明式宏展开器）**。sema `sema.myp`。

- `macro setV($v,$n){ int tmp = $n; $v = tmp; }` + 调用方 `int tmp = 7; setV(tmp,1);`
  此前宏体克隆把自声明 `tmp` 原样插入调用点块 → `duplicate variable 'tmp'`（M-3 无
  卫生性；文档建议宏内用独特前缀 `_i/_k` 规避）。
- 修复：宏展开克隆时对宏体**自声明局部**做 gensym——展开前 `mhCollectDecls` 递归收集
  宏体 VarDecl 名，登记 `原名 → 原名_m<seq>`（`macroSeq_` 全局递增，同块多次展开不冲突）；
  克隆期间 `mhRn_=1` 使宏体内**声明 + 引用一致改名**（deepCloneStmt VarDecl 声明名 /
  deepCloneExpr Identifier 变量引用同查映射）；`MacroParam` 实参替换区临时 `mhRn_=0`
  （调用方标识符保持原名，不被误改名）。宏无自声明 → 映射空 → 展开行为不变（现有宏
  用例零回归）。
- 正例 tests/@test/macro_hygiene.myp（5 断言：二次调用/多局部+if 嵌套/调用方 lo-hi 实参/
  调用方同名 tmp）；全量 540/540 + bugs 20/20 + bootstrap MD5 一致。

### v3.15.240 — MO-4 路径 import 找不到文件 → 明确诊断（漏 `.myp` 不再级联误导）（selfhost additive）

**非破坏性硬化（module_gaps MO-4，诊断改进）**。`main.myp Frontend.loadModule`。

- 相对/绝对路径 import（`import "./sub/lib.myp";`）找不到文件时，此前镜像 C++ **静默
  return 0**（只打 stderr 不加诊断）→ import 文件未并入 TU → 引用其符号报**级联误导**
  （`undefined symbol 'x'` / `'x' is not callable` / `argument 1: expected 'int',
  got 'void'`）——漏 `.myp` 扩展名（manual §10 规定相对/绝对导入须带）是最典型触发。
- 修复：路径 import 找不到即发一条明确诊断
  `cannot find import file '<path>' (relative/absolute imports need the '.myp'
  extension; no such file: '<resolved>')`。名字 import（stdlib/点分）找不到仍报原
  `cannot find import '<name>'` 不变。
- 负测试 tests/negative/import_missing_ext.myp（EXPECT ERROR 匹配新诊断子串）；
  全量 540/540 + bugs 20/20 + bootstrap MD5 一致。

### v3.15.239 — A5 容器型形参推导（`ArrayList<T>` 实参 → T）（selfhost additive 超前 oracle）

**非破坏性硬化（generic_gaps A5，selfhost 新功能先落地）**。sema `sema.myp`。

- 顶层泛型函数 `T first<T>(ArrayList<T> l)` 调 `first(li)`（li: ArrayList<int>）现推导
  T=int（此前两编译器都不推导，须显式 `first<int>(l)`）。
- **根因修正**：形参 `ArrayList<T>` 的 typeArgs 含 T → 递归 `typeMatchesGenericParam`
  让容器形参误走「裸 T」形态 → `argToAstType` 把实参**整体** `ArrayList_int_inst`
  绑给 T（推断错值）→ 调用点报含混 `type 'class'`。
- 修复（`inferConcreteTypeForGenericParam` 加**容器分支**）：形参 typeArgs 某位==类型
  参数、实参为**同类泛型实例**（Identifier → `entryTemplateType` 重建「模板+typeArgs」）
  → 取实参对应 typeArg（T=int）。非容器实参不匹配 → 干净 `cannot infer type parameter
  'T' ... (pass explicit args)`（与 oracle 诊断一致，不再错值）。
- 泛型 static / 实例方法的容器推导未做（内联推导循环仍只认裸 T / T[] → 干净 cannot
  infer，非错值）；函数类型作泛型 typeArg（函数值容器）为独立缺口（见 semantic §6.2，
  横向大改，另轮）。
- 正例 tests/@test/generic_a5_container_infer.myp（int/string 容器推导 + size/pick，
  5 断言）；全量 539/539 + bugs 20/20 + bootstrap MD5 一致。

### v3.15.238 — `??` 空合并对齐 oracle（值类型判零）+ `ref` 参数干净拒绝（selfhost additive）

**非破坏性硬化（semantic_gaps §1/§2 P1）。** codegen `codegen.myp` + parser `parser.myp`。

- **`??` 空合并对齐 oracle（semantic §2 记录纠偏）**：oracle（seed）本就支持
  `a ?? b`（`int b = a ?? 5`：非零取左否则右；string/类判非 null 取左）。selfhost
  parser 已把 `??` 展开成 `(a != null ? a : b)`（镜像 C++ parseCoalesce），但对**值
  类型**（int）生成 `icmp ne i32 %a, null` → opt-21 `null must be a pointer type`
  崩（语义文档曾误记「语法层无 ??」——实为 selfhost 值类型路径落后）。修复（codegen
  二元比较）：残留 null 常量在非 ptr 类型按 opLt 转同类型零（int→0 / fp→0.0），
  ptr 不变。正例 tests/@test/coalesce_op.myp（int 零/非零、string null/空串、类
  null/非 null，6 断言）。
- **`ref` 参数干净拒绝（semantic §2 P1）**：`ref` 是保留字但 Param 无引用语义实现——
  双编译器都把它当值传递静默接受（`void bump(ref int x)` 调 `bump(y)` 后 y 不变 =
  静默误导）。选择干净拒绝：parser parseParam 遇 `ref` 报 `'ref' parameters are
  not supported (passed by value silently); remove 'ref'`（顶层函数/类方法/function
  段同拒）。**不删保留字**（删除属破坏性语法变更，撞 v1.0 规格冻结）；真能力若做走
  additive **标量 out 窄形态**（int/double 传地址改写，避开 ARC/别名/跨帧雷；禁
  @coro/闭包捕获），引用类型仍用返回交棒/对象字段惯用法。负测试
  tests/negative/ref_param.myp。
- 全量 + bugs 20/20 + bootstrap MD5 一致。

### v3.15.237 — 泛型 parity 收尾：B5 属性收者委托 / B3 assoc 局部 / B4 泛型类 static 共存（selfhost additive）

**非破坏性硬化（generic_gaps B 系剩余三缺口，全勾销）**。codegen `codegen.myp`。

- **B5 泛型类方法 + 属性"另一泛型套自身 T"**（`Wrap<T>` 属性 `ArrayList<T> list_`，方法
  委托 `list_.add(v)` 等）：模板体内收者为**类属性**（非局部）时，sema 在共享模板体把
  callee.resolvedClass 落成占位实例名 `ArrayList_T_inst` → codegen 直拼
  `@ArrayList_T_inst_add` → opt 未定义（gs27）。修复：obj-Identifier 实例方法分支里，
  `varAstType` 查不到局部时用 `propAstType(curClass_, 属性名)` 取属性声明类型
  `ArrayList<T>`，经 `resolveType`（curTypeArgs_ 生效，实例发射期 T→int）→ 实例类
  `ArrayList_int_inst` 覆盖 cls2 → 真实实例方法（add/get/size/set 委托全通；gs19 返回
  替换错随 A 系列已闭环）。@test/generic_b5_prop_outer（Wrap<int>/Wrap<string> 双实例）。
- **B3 关联类型 `T::Item` 于泛型顶层函数**（`T::Item v = c.getVal();` 局部）：codegen
  `resolveType` 的 assoc 解析只按**具体类名** owner（`"T::Item"` 找不到名为 "T" 的类 →
  空）→ 局部类型解析不出 → 按引用槽处理（alloca ptr + myp_retain(i32)）→ opt
  `i32 but expected 'ptr'`。修复：owner 是当前类型参数（curTypeParams_/curTypeArgs_）
  时先映射成实例实参（T→IntBox）再解析 assoc（IntBox::Item=int），镜像 sema
  `associatedTypeKind` 的 tp→instArgs 映射；泛型类方法内 assoc 局部同修复。
  @test/generic_b3_assoc_tfn（int/string Item + 顶层/类内对照）。
- **B4 泛型类模板 + 泛型 static 共存**（`class Holder<T>{...static: same<R>...}`）：
  parser 对任何含 `static:` 分区的类置 isStatic=1（含泛型模板）→
  `emitStaticClassGlobals` 为模板生成 `@__myp_static_Holder = global %Holder
  zeroinitializer`，而 %Holder 模板类型不被 emitClassTypes 发射 → opt `invalid type
  for null constant`。修复：emitStaticClassGlobals 与 emitClassTypes 同规则跳过泛型
  模板（泛型模板无静态容器；纯泛型 static 走 `__gs_` 实例化路径）。
  @test/generic_b4_static_tpl（same<R>/cat<S> int/string + 实例共存）。
- 记录边界（两缺，seed 同缺非 parity）：泛型模板上**非泛型** static 方法
  （`Holder.pick`，不依赖类 T）oracle 亦 `undefined Holder_pick` → 未支持，非本次范围。
- 正例 tests/@test/generic_b5_prop_outer / generic_b3_assoc_tfn / generic_b4_static_tpl
  （6+3+5 断言）；全量 536/536 + bugs 20/20 + bootstrap MD5 一致。

### v3.15.236 — B6 new 形态：泛型模板体内 `new Box<T>` 逐实例化（含局部析构槽）（selfhost additive）

**非破坏性硬化（generic_gaps B5/B6 占位 family 的 new 形态；v3.15.235 记录缺口收尾）**。
泛型模板体内以**外层类类型参数作显式实参经 `new`** 构造另一泛型类（`Wrap<T>.innerBox`
中 `new Box<T>(t_)`、顶层泛型函数 `mkBox<T>` 中 `new Box<T>(x)`）此前 codegen 发模板名
实体的未定义引用 → opt-21 崩溃。codegen `codegen.myp`。

- **根因**：sema 在共享模板体上把 New 的 `resolvedClass` 落成**模板名**（Box）、ctor 落成
  带未绑定 T 的模板构造名（`@Box_Box_T`）、alloc 用模板 size/typeid → codegen 直接采信
  → `call void @Box_Box_T` 未定义（实例 `@Box_int_inst_Box_int` 其实已物化但未被引用）。
  既有重建分支只覆盖 resolvedClass 为**空**的情况。
- **修复（codegen New 发射）**：新助手 `cgClsIsTpl(name)`（类表里 typeParams>0 且未实例
  化的模板）与 `cgNewInstName(className, ne)`（className + `resolveType(typeArgs)` 各实参
  + `_inst`，curTypeArgs_ 生效即实例发射期 T→具体）。凡 resolvedClass 是**泛型模板**且带
  typeArgs → 与 rc 为空同路重建实例类名（alloc size/typeid、属性默认初值全走实例），并
  置空 baked ctor 走既有「按实例类动作 + resolveType 形参」重建实例 ctor。
- **同根因·局部析构槽**：`Box<T> b = new Box<T>(x)` 栈逃逸分类用 `resolvedClass`（模板名）
  → `classHasArcProps(模板)` 因 T 字段 ARC 未知误判需析构 → bake `@__myp_destroy_Box`
  （模板桩不发射，未定义）。重建实例名后：`Box_int_inst`（int 字段）无 ARC → 直接栈上
  分配、不注册 `__stackdrop` 槽；`Box_string_inst` 若需析构亦走正确实例桩名。
- **记录独立小缺口（非本次引入）**：泛型返回实例上**链式**成员调用（`ws.make().get()`）
  中间结果丢失 typeArgs → `.get()` 解析回未绑定 T 报类型错；局部中转即绕（回归测试按
  此写）。
- 正例 `tests/@test/generic_new_typearg.myp`（8 断言：类方法局部+返回 / 类方法直接返回
  new / 顶层泛型函数返回 new / 顶层泛型函数局部+取内部值（触发析构路径），Wrap<int> 与
  Wrap<string> 双实例并存互不污染）；全量 533/533 + bugs 20/20 + bootstrap MD5 一致。

### v3.15.235 — A4 构造器类型实参推导（new 泛型类 目标类型推断）（selfhost additive）

**非破坏性硬化（generic_gaps A4）**。`new 泛型类(...)` 现可从**目标类型**推断缺省的
类型实参：`Box<int> b = new Box(5);`（此前 → "no matching constructor for
'Box(byte)'"，数值字面量默认 byte 且无泛型 ctor 推断；两编译器皆缺，须显式 `<...>`）。
sema `sema.myp`。

- **目标类型注入（三处上下文）**：①VarDecl 初值（`Box<int> b = new Box(5)`，v.type()）；
  ②Return（`return new Box(7)`，currentRetAst_）；③赋值（`c = new Box(11)`，c 是泛型
  实例变量——符号只存扁平实例名 `className_="Box_int_inst"`+`instArgs_` →
  `entryTemplateType` 重建「模板+typeArgs」AstType）。注入后 New 走既有显式路径
  （instance 类 + 构造器匹配）。
- **具体性守卫 `targetTypeArgConcrete`**：目标 typeArgs 必须**具体**（基本类型 / 已知
  类·枚举·接口·struct 递归，**活动类型参数不算**）→ 泛型模板体内 `Box<T> b = new
  Box(...)`（T=外层类类型参数）不注入（留给实例化期，须显式）——不产生幻影 `_inst`。
  多类型参数 Pair<K,V> 同样推导；显式 `<...>` 不变。
- **记录既有缺口（非本次，修复前即坏）**：泛型模板方法内 `new Box<T>(t_)`（外层类类型
  参数作另一泛型类的显式实参，经 new 构造）codegen 发 `@Box_Box_T` 未定义（opt）——
  baseline/修复后同坏，属 B5/B6 占位 family 的 new 形态，待续（应干净拒/延迟物化）。
- 正例 `tests/@test/generic_ctor_infer.myp`（9 断言：decl int/string/显式/赋值 incl
  double、双类型参数 Pair、return、方法内 decl+return）；全量 532/532 + bugs 20/20 +
  bootstrap MD5 一致。

### v3.15.234 — BUG-151 同族·JSON 主路径：共享单表 JsonTab/JsonCtx 加锁串行化（runtime_myp/json.myp）

**非破坏性硬化（runtime_myp，@parallel 并发 JSON 正确性）**。审计「设计级改造项」落地：
stdlib `Json` 是**薄句柄壳**（`handle_ = myp_json_parse(...)` 返回共享表 slot），全进程
所有 `new Json`/parse 落**同一张全局 JsonTab/JsonCtx**（runtime_myp/json.myp；json_bridge.c
已 MYP 化被跳过 → 这是 JSON 主路径，非 C 旧路径）。@parallel 并发 `myp_json_parse` 互踩
`JsonCtx`（src/len/pos）+ 表追加/liveParses → **探针（8 worker 并发 parse）修复前段错
误 6/6**（stdlib Json 经同一层 → fan/mypagent 类并发场景同样可达）。

- **修复（runtime_myp/json.myp）**：加全局自旋锁 `JsonLockT.lockS`（mmap 锁字，不依赖
  arena）把 14 个 `myp_json_*` 入口串行化（解析/查询/释放互斥即安全——表本就进程单例）。
  实现：14 个函数体改名 `json*Impl` + 追加同名加锁 wrapper（`jsonLock(); r=json*Impl(...);
  jsonUnlock(); return r;`；内部无互调 public → 无死锁）。
- **边界/后续**：探针另暴露「首个 jsonEnsure 大数组分配发生在 pool worker 线程 + 后续
  ARC 复用」会段错误（myp_release ← jsonParseObject）——主线程预热建表/锁即消失（真实用
  法 main 先 parse，如 fan/mypagent），疑为对象堆跨线程大数组复用潜在缺陷，独立记录跟进
  （非本锁范畴）。另 runtime JsonTab 节点 child 定容 64/节点（>64 子节点解析截断，设计限
  制，非本次）。
- 正例 `tests/@test/parallel_json_threadsafe.myp`（主线程预热 + 8 worker 并发 parse +
  逐值抽查；5 轮全绿）；全量 531/531 + bugs 20/20 + bootstrap MD5 一致。

### v3.15.233 — BUG-151 同族审计：复用型 per-thread scratch 逐线程化（float/date 运行时）

**非破坏性硬化（runtime_myp，@parallel 并发正确性）**。v3.15.232 修异常状态后，系统
排查「C runtime `__thread` vs MYP @static」同族：审计结论——①异常运行时（BUG-151）
已逐线程；②`myp_io_cur` 早已 gettid 表化（io.myp IoCur）；③coro 各表（CoroT/
CoroCtl/CoroStackPool/CoroExec…）已 @static @thread class；④alloc 诊断计数
（Live/TLive/CC 标志/FailA）C 为 __thread、MYP 为全局——仅诊断/收集门控单线程，
竞态无害（无内存破坏）。**仍漏的两处可复用跨调用 scratch**（worker 并发触达 → 错值
串扰）：`float.myp ExDig/ExSig`（%.f |v|>=2^53 / P>15 精确大整数十进制展开在多次
helper 间读写 intLen/fracLen/X）、`date.myp DateBuf`（localtime_r/strftime 共享
struct tm 缓冲——一线程 strftime 读到另一半写 → 错日期）。

- **修复（runtime_myp/float.myp + date.myp）**：ExDig/ExSig/DateBuf 改 `@static
  @thread class` → LLVM thread_local 逐线程（惰性 arena 分配各自缓冲；单线程行为不
  变）。@parallel worker 并发格式化大 double/日期各用各的 scratch → 输出确定。
- 正例 `tests/@test/parallel_scratch_threadsafe.myp`（8 worker 并发大 double 定点+
  日期格式化逐 worker 一致性 + 精确展开抽查；6 轮全绿）；全量 530/530 + bugs 20/20
  + bootstrap MD5 一致。

### v3.15.232 — BUG-151 修复：@parallel 并发 worker 异常状态逐线程化（runtime_myp/exception.myp）

**BUG-151（🟩）**。@parallel 并发 worker 内 try/catch/throw（mypagent 真并发子 agent
各 tick 内 try/catch + Json/memory 操作）间歇崩：`uncaught exception (object, type
21)` exit 134（假 uncaught）或段错误 exit 139/总线错误 135（隔离 ~5-7%）。type 21 =
JsonError——子 tick 内 Json 解析失败**本应被同线程 catch**，却报未捕获。

- **根因（MYP runtime 落后 C runtime）**：异常运行时状态 `Exc`（depth/handlerBufs/
  curType/curObj/errBuf）是 `@static class` → **进程级全局、非线程安全**；C runtime
  （runtime.c）同函数为 `static __thread`（真 TLS）→ C 版无此 bug。@parallel worker
  是 pthread（v3.15.77 起 myp_thread_spawn 用 pthread_create 建真 TLS）——各自
  try/catch push/pop **交叉读写共享 handler 栈**：①worker throw → get_jmpbuf 读全局
  depth 顶部槽 = 别线程刚 push 的 jmp_buf → longjmp 到别线程栈帧 → 段错误/总线；
  ②并发 push/pop 非原子 → depth 丢失到 0 → 同线程该被 catch 的异常报 "uncaught" →
  exit(134) 杀全进程。
- **最小纯 MYP 复现（本版建立）**：`@parallel` 8 worker × 40000 项 × 16 次内层
  try/throw/catch——修复前 ~7/8 段错误/总线错误；no-throw 变体全稳定（隔离到 throw/
  longjmp 路径）；arena 预暖不变（排除分配首触）。mypagent fan_shared_mem_check
  修复前隔离 2/25 崩。
- **修复（runtime_myp/exception.myp 一处）**：`Exc` 改 `@static @thread class` → LLVM
  `thread_local`（@thread 静态类 + pthread TLS 真逐线程）——异常状态逐线程，各 worker
  try/catch/throw 线程内一致（同线程 catch 生效、无跨线程 longjmp、无假 uncaught），
  对齐 C runtime `__thread` 语义。
- 正例 `tests/@test/parallel_exc_threadsafe.myp`（8×40000×16 并发 try+throw，修复前
  ~全崩 → 全绿）；全量 528/528 + bugs 20/20 + bootstrap MD5 一致；mypagent
  fan_shared_mem_check 隔离 30/30（修复前 2/25）。

### v3.15.231 — A1 两条边界：跨实例类泛型（instance-of-instance 类型实参）+ 泛型类接口默认方法（selfhost）

**非破坏性硬化（generic_gaps A1 收尾后两条边界，parser + sema + codegen）**。A1 仅剩的
两条边界落地：
①**跨实例类泛型**（泛型类型实参 = 泛型类【实例】`Box<int>`/`Box<Box<int>>`）的显式
（`dup<Box<int>>`）与推导（`dup(b)`）调用，含泛型体内调泛型 T 依赖 + 实例类实参、两
层嵌套实例类穿透两层泛型链、方法级/static 泛型以实例类为实参；
②**泛型类模板实现接口 + 接口默认方法**（trait 默认实现）——泛型类实例省略默认方法时
的 `__ifdef_*_Box_int_inst` stub 生成。

- **parser `scanGenericCall` 不识词法单 token `>>`**（parser.myp）：嵌套泛型类型实参闭合
  `dup<Box<int>>(b)` 的 `>>` 词法合成单 token → 前瞻扫描按深度不闭合 → 误判为二元
  `<` → RHS `int` 类型 token → "unexpected token 'int'" 解析错。加 `>>` 按两个 `'>'`
  计深（同 consumeGenericClose / 类型跳过）。
- **sema `typeToKind` 不识泛型实例类名**（sema.myp）：实例类（Box_int_inst）不在
  classIdx_（classNames_ 不含 _inst 克隆，除非 registerInstClass 延迟注册）→ 推导路径
  （argToAstType 把 SymbolEntry.className_=实例名扁平化）把 concrete 建成裸实例名 →
  typeToKind 返 void → "cannot initialize ... with value of type 'void'"（`Box<int> d =
  dup(b)`）。加 `isGenericInstClassByName` 兜底（扫 tu_.classes() 的 isGenericInst 真
  类）→ 返 class（valueClass=实例名，与显式路径同一实例名 `dup_Box_int_inst_inst`，
  去重一致）。
- **codegen trait 默认 stub 未给泛型类【实例】生成**（codegen.myp）：trait 默认生成守
  `Str.len(c.iface())!=0 && c.isGenericInst()==0`——模板类（isGenericInst==0）本就被类
  发射循环跳过，实例类又被该守卫排除 → `@__myp_vtable_<Iface>_Box_int_inst` 引用
  `@__ifdef_<Iface>_<m>_Box_int_inst` 未定义（opt undefined）。去 isGenericInst 排除，
  实例类按名生成 stub（默认体内 this.method() 经 findIfaceDefault 解析到
  `<实例类>_<m>` / 兄弟 __ifdef stub）。
- 正例：`tests/@test/generic_instance_typearg.myp`（10 断言）、`tests/@test/
  generic_iface_default.myp`（6 断言）；全量 528/528 + bugs 20/20；bootstrap MD5 一致。

### v3.15.230 — A1 余量：泛型体内调泛型【实例】方法（this.m<U> 内嵌 + 227 物化回归）（selfhost）

**非破坏性硬化（generic_gaps A1 收尾，selfhost codegen 逐实例映射 + sema 物化修复）**。
A1 仅剩项落地：泛型体（方法级泛型方法 `echoU<U>` / 顶层泛型函数 `viaInst<T>`）内调用
另一方法级泛型**实例**方法（`this.id<U>` / 裸 `id<U>` / 变量收者 `c.id<T>`），内层类型
实参引用外层类型参数。此前方法级泛型体不经逐体解析（模板体 markAllStmt）→ 内层
callTypeArgs 未解析 → codegen 落裸 `@Class_id`（unbound T 形参 ptr）→ opt 类型错 /
undefined。现支持（codegen `codegen.myp` + sema `sema.myp`）。

- **codegen 实例方法逐实例映射**（三种收者形态）：实例（`id_int_inst`/`id_string_inst`
  等）已由克隆体逐体分析（v3.15.224，echoU_int_inst 深克隆体里 id<int> →
  resolveGenericInstMethod 建）按每外层实例入类；codegen 发射**共享**模板体时按
  curTypeParams_/curTypeArgs_ 把 callTypeArgs 逐实例映射（resolveType）→ 实例名直调
  （非变异，不设 e.resolved 防跨实例串扰，同 v3.15.229 fn/static 映射）。三个插入点：
  ①裸方法调用（isTopLevelFunc==0 && hasMethodInClass 分支，echoU<U> 体里 `id<U>`）②
  Member obj=This（`this.id<U>`）③Member obj=实例变量（`c.id<T>`，cls2 经 varAstType/
  resolveType→classInstName）。新 helper：cgHasGenericMethodTemplate（typeParams>0 且
  instTypeArgs 空）、cgMethodInstName（mname_<args>_inst，存在性查 cls.actions）、
  cgMethodInstRetLt（实例 ret AstType→llvmType）、cgEmitInstMethodInstCall（统一发射，
  ptr 返回 addFreshTemp）。sema 已改名（memberName=mAct）的调用不触发（mname 是实例名
  非模板名 → 模板检查 0），无干扰。
- **227 物化回归修复（sema）**：`materializeTemplateMethodClones` 原用 `resolveBase`
  判定实例归属模板；v3.15.226 的 `registerInstClass` 把动态实例类名注册进 classIdx_
  （精确命中优先）→ resolveBase(Box_int_inst) 返回**自身**而非模板 Box → 物化跳过该
  实例（本类 this.id<T> 的 id_T_inst 缺失 → 发射裸无定义）。触发条件：泛型类模板的
  方法级泛型方法（echoU<U>）被调用 → 克隆体逐体分析先于物化把实例类注册 → 顺带
  Box_int 的 echoT 也缺。修：改**模板名前缀匹配**（`Str.startsWith(C.name(), tpl+"_")`
  ，isGenericInst 已排除模板类自身），不依赖 classIdx_ 污染源。
- 边界：泛型体内调泛型**接口**默认方法 / 跨实例类泛型（instance-of-instance 命名）未
  覆盖；where 约束模板体仍保守扫描。
- 正例 `tests/@test/generic_in_generic_inst.myp`（11 断言：非泛型类方法级泛型体
  this./裸、顶层 fn 变量收者 c.id<T>、泛型类模板方法级泛型体 + 227 echoT 双实例）；
  全量 526/526 + bugs 20/20；bootstrap MD5 一致。

### v3.15.229 — A1 余量：泛型体内调泛型 T 依赖实参（逐实例 name 映射）（selfhost）

**非破坏性硬化（generic_gaps A1 余量首片，codegen 非变异逐实例映射）**。v3.15.228
覆盖内层实参**全具体**；本版覆盖内层实参**引用外层类型参数**的调用：顶层泛型函数
体内 `id<T>`（T=自身类型参数）、@static 泛型方法体内 `M.pick<T>`（static 泛型链），
含两层嵌套 `via2<T>` → `mid<T>` → `id<T>`。此前 v3.15.228 walker 因共享模板体节点
不能统一改名（每实例的实例名不同）跳过 T 依赖 → codegen 发裸 `@id`/`@M_pick` →
opt-21 use of undefined value（codegen `codegen.myp`）。

- **机制**：v3.15.224 逐体分析对每个外层实例深克隆模板体（T 已 substitute 具体）
  → 内层 `id<int>`/`id<string>`、`__gs_M_pick_<T>_inst` 已按每外层实例各自建立；
  只差 codegen 发射**共享**模板体时按 `curTypeParams_/curTypeArgs_` 把 callTypeArgs
  逐实例映射成实例名。T 依赖**不能**设 `e.resolved`（共享节点唯一改名跨实例冲突：
  `f2<int>` 设 `id_int_inst` 后 `f2<string>` 发射会读到已设名）→ 走 codegen 非变异
  逐实例映射。
- **codegen 顶层函数 Identifier-callee 分支**：`callTypeArgs>0 && cgIsGenericFnTempl
  (fn)`（fn 是 typeParams>0 且 genericInst==0 的模板）→ `cgGenericFnInstName` 逐实参
  `resolveType`（当前函数实例的 curTypeArgs_ 映射外层 T）拼 `fn_<args>_inst`，实例已
  在 tu.functions 才发射（否则回落原路径不恶化）；`cgFuncRetLt` 按实例声明 ret 的
  AstType 取返回 LLVM 类型（`exprLlvmType` 是 i32 占位不可用，ptr 返回 addFreshTemp）。
- **codegen 泛型 static Member 分支**：obj Identifier 命名类 + callTypeArgs>0 →
  `cgGsInstName` 按 sema mangle 规则（`__gs_<cls>_<m>_<args>_inst`，`cgGsTypeArgName`
  镜像 sema.gsTypeArgName）映射发射。`findFunc` 只匹配 typeParams==0（泛型实例保留
  typeParams 名查不到）→ 新 helper 直接精确扫描 tu.functions。
- 边界：泛型体内调泛型**实例**方法（this.m<U> 内嵌 T 依赖，需每实例方法 clone 名
  映射 + 类前缀）仍留 A1 余量（更后续）。
- 正例 `tests/@test/generic_in_generic_tdep.myp`（7 断言：顶层 id<T> 双实例 / 两层
  嵌套 / static M.pick<T> / static 链 M.bump<T>）；全量 525/525 + bugs 20/20；
  bootstrap MD5 一致。

### v3.15.228 — A1 泛型体内调泛型（具体实参，generic-in-generic 落地）（selfhost）

**非破坏性硬化（generic_gaps A1 首片落地）**。泛型【体】（带类型参数的顶层函数 /
@static 方法 / 实例方法）内调用另一泛型（顶层函数 `id<int>` / @static 方法
`M.min2<int>`），内层类型实参**全具体**（不引用外层类型参数）——此前模板体
markAllStmt（体不经 visitExpr）→ 内层调用从不实例化 → codegen 发裸 `@id`/
`@Class_meth` → opt undefined。现支持（sema `sema.myp`）。

- **共享模板体具体内层调用解析**：`tplResolveFnStmt/tplResolveFnExpr` 遍历共享模板
  体（复用于 v3.15.224 起的逐体分析 `analyzeInstCloneBody`，在形参作用域内跑），
  Call 节点：①Identifier callee 是泛型顶层函数模板且 callTypeArgs 全具体
  （`tplArgRefsOuter` 不引用外层类型参数）→ `instantiateGenericFunction` 建实例 +
  `e.setResolved(实例名)` + 具体返回 kind/valueClass（codegen 既有 resolved 直调路径
  发射 `@实例名`）；②Member callee 泛型 static（`M.min2<int>`，findGenericStatic ≥0、
  callTypeArgs 具体）→ `resolveGenericStaticCall` 建 __gs_ 实例 + e.resolved。
- **仅外层独立的具体实参才安全**：共享节点唯一改名跨实例一致；外层 T 依赖的内层
  实参（`id<T>` / `M.pick<T>`）跳过（A1 余量：需逐实例 name 映射，记后续）。泛型
  体内调泛型**实例**方法也留 A1 余量。
- 逐体分析基础设施复用：三类克隆（fn/s/m）体都受益——顶层泛型函数 / @static 泛型
  方法 / 实例泛型方法 内调泛型顶层函数 + 泛型 static 具体实参均可。
- 正例 `tests/@test/generic_in_generic.myp`（4 断言：fn/static/实例方法体 ×
  id<int>/M.min2<int>）；全量 524/524 + bugs 20/20；bootstrap MD5 一致。

### v3.15.227 — 泛型类模板体内 this.m<T>（方法级泛型调用延迟登记，A2 遗留落地）（selfhost）

**非破坏性硬化（A2 遗留项）**。泛型类**模板**（`Box<T>`）的普通方法体内调用方法级泛型
（`this.id<string>` / 裸 `id<int>` / **方法实参=类类型参数** `this.id<T>`）此前在
`resolveGenericInstMethod` 被干净拒绝（"on a generic class template are not
supported"，isGenericInst==0、class T 未具体）。现支持（sema + codegen）。

- **延迟登记**（sema `sema.myp`）：Pass B 分析模板体时 class T 未具体 → 不建单一
  concrete clone（T 未绑定会幻影 mangle）→ `recordPendClone` 记 (模板类, 方法模板,
  concrete[可含类T占位])；末尾 `materializeTemplateMethodClones` 对每个实例类（类 T
  → 实例 concrete，`substituteType` 后）用 `buildInstMethodClone` 物化 clone 入实例
  actions + 排队逐体分析（复用 v3.15.226 实例克隆逐体分析）。`runInstBodyAnalysis`
  改 while：drain → 物化 → 重复到不再新增（guard 64）。
- **clone 名用原始 concrete、内容用实例 concrete**：共享模板体 call 节点改名用原始
  concrete（`this.id<T>` → `id_T_inst`，所有实例同名）——物化 clone 名也必须按原始
  concrete（否则 `id_int_inst`/`id_string_inst` 与 call 名不匹配 → 未定义符号）；每
  实例 actions 自独立，同名 clone 内容按各自类 concrete（T→int/string）各异。
- **codegen `this` 收者方法调用按当前发射类定名**（`codegen.myp` Member-call 加
  `obj.kind()=="This"` 分支：`fn = curClass_ + "_" + mname`）：此前用
  callee.resolvedClass()（sema 在模板体解析成模板名 Box）→ 实例发射时生成
  `@Box_id_string_inst`（模板前缀不存在）→ opt undefined。非泛型普通类两值一致无
  影响；顺带修泛型实例克隆方法内 `this.member()`（sema 不分析实例体 → resolvedClass
  空 → 曾落 Object）的潜在缺口。
- 边界：方法级泛型体**内**调方法级泛型（`echoU<U>` 体里 `this.id<U>`，A1 泛型内
  调泛型）仍不支持（泛型方法体 markAllStmt，不经此路径）；where 约束模板体不逐体
  不变。
- 正例 `tests/@test/generic_inst_tpl_body.myp`（8 断言：具体 string/int 实参 + 类T
  作方法实参 this.id<T>/裸 id<T> + 类T形参/方法U 组合，Box<int>/Box<string> 双实例）；
  移除旧负例 `tests/negative/generic_inst_on_generic_class.myp`（现受支持）。
  全量 523/523 + bugs 20/20；bootstrap MD5 一致。

### v3.15.226 — 泛型类实例方法克隆逐体分析扩展（类级 T + 方法级 U 双映射）（selfhost）

**非破坏性硬化（v3.15.224 克隆体逐体分析的后续扩展）**。v3.15.224 对实例化克隆做
深克隆+类型替换后的整体类型检查，但**跳过了泛型类实例**（`Box_int_inst`，typeParams>0）
的方法级克隆——其类级成员类型/属性也须替换（类 T 须映射），当时记后续扩展。现补上
（sema `sema.myp` `analyzeInstCloneBody`）。

- **类级 + 方法级映射叠加**：泛型类实例方法克隆体含**类 T 与 方法 U 两类占位**——
  `mpTps/mpArgs` 先 prepend 类级映射（`cobj.typeParams→instTypeArgs`，如 Box 的
  `T→int`），再 append 方法级（`a` 的 `U→concrete`），与 codegen `setupTypeParams`+
  方法级叠加序一致 → body 深克隆替换/`typeToKind` concrete 绑定双落到具体。
- **实例类注册进 sema 成员表**（`registerInstClass`，按实例名去重）：实例类由
  Pass A/B 动态生成、不在 Pass 0 的 `classNames_/classProps_/methods_`——若
  `currentClass_`=实例名，裸属性/`this.prop`/兄弟方法解析全失败（undefined symbol）。
  注册（具体 props/actions/funcs + `addMethod` 具体签名）后与普通类一致；在激活
  binding 下 add（方法级 U 占位可解析）。注册在延迟分析期，无副作用。
- 效果：`Box<int>.pick<Pt>(...)` 等泛型类实例上的方法级克隆体现在也逐体类型检查——
  体内 `int n = x`（U=class 时）、`string s = this.v`（类T=class 时）等错误按具体
  类型实例化时干净报出（此前静默 → 运行时错/缺符号）。
- 边界不变：`where T : Trait` 有界模板体仍不逐体；类T 本身取 class（`Box<Pt>`）并
  在类T 值上调用方法（`v.dup()`）是**既有 codegen 缺口**（与逐体分析无关，探测
  pgc2 opt-21 `retain(ptr )` 复现）。
- 负例 `tests/negative/generic_clsinst_body_{local,member}.myp`（U=class 局部错 /
  类T=class 属性成员错）；正例 `tests/@test/generic_clsinst_body.myp`（6 断言，
  类T=int/string + 方法U=class 不误报）。全量 523/523 + bugs 20/20；bootstrap MD5 一致。

### v3.15.225 — BUG-150 修复：net send 屏蔽 SIGPIPE（写已关闭连接不崩）（runtime）

**非破坏性运行时修复**。对已关闭 socket `send` 默认触发 SIGPIPE 终止进程（exit 141）；
myp_http 协程服务端向断开客户端写响应即崩（曾误判"高并发饿死"，实为崩溃后新连接
全失败）。修复：三处 Linux `send/sendto` flags 加 `MSG_NOSIGNAL`（0x4000）→ 写已关
连接返回 EPIPE(-1) 优雅处理。

- `runtime_myp/net.myp` `myp_net_send`：`send(fd, ptr, len, 0)` → `0x4000`（net.myp
  仅 Linux 构建，de-gcc libc ffi）。
- `runtime_myp/uds.myp` `myp_uds_send`：`sendto` syscall flags `0` → `0x4000`。
- `stdlib/bridges/net_bridge.c` `myp_net_send`（Linux）：`send(..., MSG_NOSIGNAL)`
  + `#ifndef MSG_NOSIGNAL` 回落 0；Windows 分支本就无 SIGPIPE，不受影响。
- 回归 `tests/bugs/b150_send_sigpipe.myp`（单进程，listen backlog 先连后 accept）：
  服务端 send 一包（客户端不读）→ 客户端带未读数据 close 发 RST → 服务端再写已 RST
  连接不崩。**灵敏度验证**：临时把 net.myp flags 改回 0 → 该测试 exit 141（SIGPIPE
  崩）；恢复修复 → 5 断言全绿。
- bugs 20/20 + 全量 520/520。

### v3.15.224 — 克隆体逐体分析（实例化克隆整体类型检查）（selfhost 大项落地）

**非破坏性硬化（v3.15.223 记录的"模板体完整类型检查"大项落地）**。泛型模板体
（顶层泛型函数 / 实例方法级 `<T>` / @static 泛型）原先在 Pass B 只做未定义符号
保守扫描（v3.15.222）+ `markAllStmt`，体内 `x.noSuch()`（x 具体类型无该成员）、
局部声明类型错等**只有按具体类型实例化时才暴露** → 现在对每个实例化克隆再跑一次
**深克隆 + 类型替换后** 的整体 `visitStmt`（sema `sema.myp`）。

- **深克隆 + 类型替换**（`anType/anTarget/anExpr/anStmt`）：无条件克隆 stmt/expr 的
  全部子节点与列表（未知 kind 也安全——不共享任何可被 visitExpr 改写的子节点），
  并对类型承载字段（VarDecl 类型、loopVar 类型、`new` 的 className/typeArgs、
  `callTypeArgs`、`elemType`、Convert `toKind`、Gpu sharedType、destructure target）
  做 `substituteType` T→concrete。`cloneExpr` 不拷 resolvedKind → 克隆体分析状态
  干净，**共享模板体不被污染**（其它同模板克隆的 codegen 走 `resolveType` 照旧）。
- **激活 concrete 绑定**：`curInstTps_/curInstArgs_`；`typeToKind` 顶层先按绑定把
  残留 T 占位解析为具体类型（再回落 inGeneric→int）。
- **延迟工作队列**（`enqueueInstAnalysis` + `runInstBodyAnalysis`）：三个克隆创建点
  （`instantiateGenericFunction` fn / `resolveGenericInstMethod` m /
  `resolveGenericStaticCall`→`cloneStaticToFunction` s）排队；Pass B 全部普通体分析
  完后统一 drain（动态上界——克隆体内再触发的新克隆也处理）；按 mangled 名去重
  防递归。分析在方法上下文（`this`+具体形参+currentClass_）或顶层函数上下文执行，
  全量保存/恢复 Sema 状态。
- **作用域边界**：带 `where T : Trait`（Numeric/Ordered/Float…）约束的模板体**不**逐体
  分析（保留保守扫描）——数值 trait 模板体故意用宽松算术（`byte` 上 `a+b` 提升 int
  再窄化返回；非泛型函数同样被拒、模板体靠跳过容忍且 codegen 窄化正确），逐体严格
  检查会误报。仅**无约束纯 `<T>`** 克隆体走整体类型检查。
- 负例 `tests/negative/generic_body_member.myp` / `generic_body_local.myp` /
  `generic_fn_body.myp` / `generic_static_body.myp`（fn/m/s 三形态 body 类型错）；
  正例 `tests/@test/generic_clone_body.myp`（6 断言，类 T 三形态不误报）。
  全量 520/520 + bugs 19/19；bootstrap MD5 一致。

### v3.15.223 — 畸形签名解析层加固（空形参名拒绝）（鲁棒性，selfhost）

**非破坏性硬化（承接 v3.15.222）**。无名参数（`void f(int )` / `T id<T>(T )`）此前被
`parseParam` 静默接受 → codegen 发**空名参数**（`i32 %`）→ `opt-21 "expected ')' at
end of argument list"`（实测非泛型与泛型模板同样坏 IR；并非受支持的"无名参数"特性）。

- **parser**（`parser.myp` `parseParam`）：类型后须有名——非 identifier（`)`/`,`）→
  干净拒绝 "expected parameter name (parameters must be named)"。函数类型 `(A,B)->R`
  与元组类型 `(A,B)` 不经 parseParam（parseType 直取类型，不受影响）。
- 回归 `tests/negative/param_empty_name.myp`；全量 515/515 + bugs 19/19；
  bootstrap MD5 一致。
- **记录（模板体完整类型检查 = 后续大项）**：对类 action（A2 泛型实例方法）做了
  「模板体整体 visitStmt」试点——不可行：方法级 T 占位在单次模板分析中产生**虚假
  类型错**（`T y=x` 被解析成 double/string 等），需「按实例化具体类型逐体分析」的
  单态化期类型检查架构（克隆体尚未做 sema 分析）。故保留 v3.15.222 的未定义符号
  保守扫描；完整类型检查列后续。

### v3.15.222 — 泛型模板体未定义符号扫描（鲁棒性硬化，selfhost）

**非破坏性硬化（承接 v3.15.221 崩溃扫描记录的既有缺口）**。泛型函数 / 泛型 static /
泛型实例方法（A2）/泛型 struct 方法的 body 此前由 `markAllStmt` **跳过完整类型检查**
→ 体内未定义标识符（如 `return y;` 的笔误）一路到 codegen → 坏 IR（`opt` 报
`use of undefined value '%y'`，非干净诊断）。

- **sema**（`sema.myp`）：新增保守两阶段**模板体符号扫描**
  `templateBodyUndefinedCheck`（接入 5 处泛型模板处理点：顶层泛型函数 / 类 action /
  类 function / @static action / struct 方法，均在 markAllStmt 前）：
  - ① 收集体内声明的局部名（VarDecl / For / ForIn / Lambda 形参）＋方法形参＋方法级
    与类级类型参数＋`this`＋当前类成员（props/actions/funcs/events/staticActions）或
    struct 字段名；
  - ② 走查 Identifier 引用（含 ArrayLit/StructLit/MapLit 元素），凡既非上述集合、也非
    已知全局（顶层函数/类/enum/struct/interface/const/intrinsic/`__myp_*`）→ 干净拒绝
    "undefined symbol 'X' (inside generic template body)"。
  - **零误报优先**：凡扫描器无法完整建模的语句形态（Destructure / Gpu* / Try / Match /
    Quote / TryExpr / GpuReduceExpr 等）→ 整段跳过不报（markAllStmt 照常）。
- 修掉 v3.15.221 记录的一类坏 IR 缺口（模板体内未定义符号）；变异模糊（1600×3 种子）
  该类归零。
- 回归 `tests/negative/generic_inst_undef_var.myp` /
  `generic_fn_undef_var.myp`；全量 514/514 + bugs 19/19；bootstrap MD5 一致。
- 注：此扫描只报"未定义符号"；模板体完整类型检查（占位 T 语义）仍属后续大项。

### v3.15.221 — 崩溃扫描修复：泛型方法非法显式类型实参干净拒绝（鲁棒性，selfhost）

**非破坏性硬化**。对新增特性做定向崩溃/坏 IR 扫描（词法畸形 78 + 通用 fuzz 3000 +
变异模糊 1600×2），无信号级 CRASH；发现并修复一类**坏 IR**（畸形输入 → 编译器发坏
IR → opt 报错而非干净诊断）：

- **泛型方法非法显式类型实参**（v3.15.217/218 A2 路径）：`Box<int>.id<ixt>(9)` /
  `c.id<ixt>(5)` 的 `<ixt>`（未知名）——非泛型类已能拒（void 级联），**泛型类实例**
  路径漏校验 → 把幻影类名 `ixt` mangle 进 `_inst`（`Box_int_inst_id_ixt_inst`）→
  codegen 实参按 ptr 传 int 字面量 → `opt-21 integer constant must have integer
  type`（坏 IR）。
- **sema**（`sema.myp`）：新增 `isResolvableTypeArg`（已知基本类型/类/enum/struct/
  interface/slice/当前泛型占位），`resolveGenericInstMethod` 显式类型实参逐个校验 →
  未知名干净拒绝 "unknown type argument 'X' for generic method 'C.m'"（泛型类实例与非
  泛型类统一）。
- 回归 `tests/negative/generic_inst_unknown_type_arg.myp`；全量 512/512 + bugs 19/19；
  bootstrap MD5 一致。
- **已记录既有缺口（非本次引入，未修）**：泛型模板体（顶层泛型函数 / 泛型 static /
  A2 泛型实例方法）经 markAllStmt 跳过符号检查——体内未定义符号（`return y;`）或畸形
  签名（空形参名）会一路到 codegen → 坏 IR（opt 报错，非进程崩溃）。此缺口 A2 之前
  即存在（顶层泛型函数同坏），修法=实例化克隆体做符号检查/类型检查（较大，后续路线）。

### v3.15.220 — 字典字面量 `{"k": v}`（additive，selfhost）

**非破坏性加法（selfhost 超前 / seed 冻结不解析）**。`{"k": v, ...}` 作 **StrHashMap<V>
实例**字面量（消掉 `new StrHashMap<V>()` + 逐条 put 样板；键 string、值同型可转 V）：

```myp
import collections;
StrHashMap<int> m = { "a": 1, "b": 2 };      // 声明初值（显式 V 目标）
StrHashMap<double> d = { "p": 1, "q": 2.5 }; // int → double 提升
StrHashMap<string> s = { "x": "hi" };
StrHashMap<int> e = {};                      // 空
m.put("a", 9);                               // 覆盖/后续 put 照常
```

- **grammar §Expression**：`Primary` 增 `MapLiteral ::= '{' MapEntry (',' MapEntry)* '}'`，
  `MapEntry ::= Expression ':' Expression`。
- **ast**（`ast.myp` `AstExpr`）：新 kind `MapLit`（键表达式列表 `mapKeys_` 与值
  `elements_` 平行；目标值类型存 `elemType_`；cloneExpr/dump 同步）。
- **parser**（`parser.myp` `parsePrimaryInner`）：新增 `{` primary（表达式内此前 `{`
  无合法形态）→ 解析 `{ k: v, … }` 建 MapLit。
- **sema**（`sema.myp`）：VarDecl 显式 `StrHashMap<V>` 目标（className "StrHashMap" +
  typeArgs）先 bindMapLitTo（设 elemType=V + 访问键/值并校验：**键须 string**、值
  `typesCompat` 可转 V）；`visitExpr(MapLit)` 已绑定 → 返回 `class`，未绑定（var/非
  StrHashMap 目标/赋值·实参·return 等位）→ 干净拒绝 "requires a typed 'StrHashMap<V>'
  target"。
- **codegen**（`codegen.myp`）：新增 `genMapLit` —— `myp_alloc_object` 分配
  `StrHashMap_<V>_inst`（无 ctor 类，清零）→ 逐条内联 `put` 调用（StrHashMap 惰性
  init 于首次 put 分配；键/值经 convertValue 到参数类型；fresh 语义同普通调用）；
  返回 fresh 实例 ptr（变量存储走既有 class 槽 storeRef）。`exprLlvmType(MapLit)`
  = `ptr`。
- AST dump 稳定（MapLit 专用分支）；bootstrap MD5 一致。
- 回归 `tests/@test/map_literal.myp`（int/string/double 提升/bool/空/覆盖 put，
  15 断言）+ 4 负例（key not string / value mismatch / var / 非 StrHashMap 目标）。
- 后续（路线）：赋值/实参/返回位置绑定、数值键 HashMap<K,V> 字面量、`var` 推断、
  表达式键（现限 string-kind 表达式均可，非字面量亦可）。

### v3.15.219 — 接口泛型方法声明干净拒绝 + vtable 约束界定（generic A2 边界，selfhost）

**非破坏性硬化（续 v3.15.217/218）**。界定泛型实例方法的合法边界：接口（含抽象
方法）上**禁止方法级泛型**——接口值经 **vtable 动态分派**（槽只存一个 fn ptr），
擦除泛型无法按类型参数单态化；此前接口声明 `U wrap<U>(U x);` 能解析、实现类同名
泛型方法能编译，但实现类的 `@__myp_vtable_I_Cls` 会把**未发射的模板** `@Cls_wrap`
塞进槽 → `opt-21 use of undefined value '@Cls_wrap'`（具体类变量调用同形态也崩）。

```myp
interface I { U wrap<U>(U x); }   // ✗ v3.15.219 起声明处干净拒绝
class Sq {
  interface class I;
  action:
    U wrap<U>(U x) { return x; }   // 泛型方法限具体类（非接口声明）→ 照常
}
Sq s = new Sq(); int r = s.wrap<int>(5);   // ✓（A2 单态化）
```

- **sema**（`sema.myp` 接口方法收集 pass）：接口 action `typeParams>0` → 声明处干净
  拒绝 "generic methods on interfaces are not supported (…vtable…cannot monomorphize
  the type parameter); … put generic methods on concrete classes…"（替代接口值调用处
  void 级联与 vtable 模板引用）。
- 具体类泛型方法（不依赖接口声明）路径不变（v3.15.217/218）；接口值上调用泛型方法
  因声明已拒而不可达。
- 模板体内 `this.m<T>`（泛型类模板方法、类 T 未具体化）维持干净拒绝——实例化时序
  （`instantiateClass` 克隆早于 Pass B 模板体登记）使其需延迟登记架构，属后续路线
  （见 generic_gaps A2）。
- 回归 `tests/negative/generic_iface_method.myp`；全量 506/506 + bugs 19/19；
  bootstrap MD5 一致。
- 设计说明：真「运行时接口值 + 泛型方法」分派需 witness/dictionary 或整对象图
  单态化，超出擦除泛型 vtable 模型——故接口泛型属设计取舍（干净拒绝），
  generic_gaps A2 已标注。

### v3.15.218 — 泛型类 + 方法级 `<T>` 组合（generic A2 扩展，selfhost additive）

**非破坏性加法（续 v3.15.217）**。在**已实例化的泛型类对象**上调方法级泛型实例
方法——类级类型参数（经类 `instTypeArgs`）与方法级类型参数（调用点绑定）同时生效，
body 可同时使用类 `T` 与方法级 `U`：

```myp
class Box<T> {
  property:
    T v;
  action:
    @constructor Box(T val) { v = val; }
    U id2<U>(U x) { U y = x; return y; }        // 类 T + 方法 U
    T pick<U>(U x) { T out = v; return out; }   // body 同时用类 T 属性与方法 U
    U ofBoth<U>(T a, U b) { return b; }
}
Box<int> b = new Box<int>(7);
int r = b.pick<int>(9);       // 方法 U=int；返回类T属性 this.v
int s = b.id2(3);             // 推断
```

- **sema**（`sema.myp` `resolveGenericInstMethod`）：拒绝条件从「泛型类 + 方法级一律
  拒」放宽为仅拒**泛型类模板**（`typeParams>0 && isGenericInst==0`——模板体内
  `this.m<T>` 可达、类 T 尚未具体化，无法发射）。在实例类（`Box_int_inst`）上正常走
  既有单态化：模板（类实例化时已把类 T→int 替换进 params/ret）再按方法级 typeParams
  替换 U；mangled `_inst` action 克隆入该实例类 actions。
- **codegen**（`codegen.myp` `genInstanceActionNamed`）：方法级 typeParams/instTypeArgs
  由「覆盖」改为**叠加**到类级映射之后（`setupTypeParams(cls)` 先填类级 [类T]→
  [类实参]，再 append 方法级 [U]→[U实参]）——body 内类 T 与方法级 U 都经 resolveType
  解析。
- 干净拒绝：泛型类模板体内调（报 "…on a generic class template are not supported;
  call it on an instantiated generic class (e.g. Box<int>)"）。
- 回归 `tests/@test/generic_inst_generic_class.myp`（Box<int>/Box<string> 双实例 +
  显式/推断 + 类T属性 + 方法U局部 + 类T形参，12 断言）；负例
  `generic_inst_on_generic_class.myp` 改为模板体内调（原「实例上组合不支持」已过时）。
- 全量 505/505 + bugs 19/19；bootstrap MD5 一致。

### v3.15.217 — 泛型【实例】方法（generic A2，additive，selfhost）

**非破坏性加法（selfhost 超前 / seed 冻结不解析）**。普通（非泛型）类的 action 现可带
**方法级类型参数** `<T>` 并在调用点单态化——此前只支持顶层泛型函数与泛型 static 方法
（方法级 `<T>` 能解析但 T 不绑定 → 形参/返回变 void）：

```myp
class C {
  action:
    T wrap<T>(T x) { return x; }          // 实例方法带方法级 <T>
    T first<T>(T[] xs) { return xs[0]; }
    T scaled<T>(T x) { return x; }        // T 可参与 body 运算
}
C c = new C();
int a  = c.wrap<int>(5);                  // 外部 obj.m<T>（显式）
int b  = this.wrap<int>(7);               // this.m<T>
int cc = wrap<int>(9);                    // 类内裸 self-call
double d = c.wrap(2.5);                   // 推导（T 出现于形参/元素）
int e  = c.first<int>(xs);                // T[] 形参
```

- 机制：调用点把模板 action **单态化为同类改名 action**（mangled 名
  `<method>_<types>_inst`，params/ret 经 `substituteType` 具体化，新增 AstAction
  `instTypeArgs` 记录方法级 T→具体）；调用 callee 的名字（Member→memberName，
  裸 Identifier→name）改为 mangled —— codegen 沿既有实例方法发射路径
  `Class_<mangled>(ptr this, ...)` 且 `resolveType` 依 instTypeArgs 把方法级 T 映射
  具体（body 内 `T` 局部/运算由此解析）。同类型重复调用按 mangled 名去重。
- **ast**（`ast.myp` `AstAction`）：新增 `instTypeArgs_`（类型实参映射，对齐 AstFunction）。
- **sema**（`sema.myp`）：`resolveGenericInstMethod(e, callee, cls, m)` —— 三类调用点
  前置拦截：Member `obj.m<T>`（cn 容器）、`this.m<T>`（kind "This"→cn 空 inClass
  回退）、类内裸 `m<T>`（Identifier inClass 分支）。解析：显式 `<...>` 或从形参推导
  （裸 `T`/`T[]` 元素，镜像 `resolveGenericStaticCall`）；替换后逐参类型校验；mangled
  action 克隆入同类 actions（去重）。干净拒绝：类型实参数不符 / 无法推断 / 实参类型
  不匹配 / **泛型类 + 方法级 `<T>`**（v1 限非泛型类，防类级+方法级双占位）。
- **codegen**（`codegen.myp`）：类实例 action 发射循环**跳过泛型模板**
  （typeParams>0 且 instTypeArgs 空——T 未绑定会产坏 IR），仅发 `_inst` 克隆；
  `genInstanceActionNamed` 在 instTypeArgs 非空时以方法级 typeParams/instTypeArgs
  覆盖 `curTypeParams_`/`curTypeArgs_`（body 内 T 占位 → 具体）。e.resolved 保持空
  （走既有 this 实例路径，不走无-this 的 resolved 分支）。
- Pass B 对泛型方法模板沿用 markAllStmt（body 不逐个类型检查，与泛型 static 模板一致，
  实例体由 codegen resolveType 具体化）。AST dump 稳定；bootstrap MD5 一致。
- 回归 `tests/@test/generic_inst_method.myp`（裸/this/外部 + 显式/推断 + T[] + T 局部/
  运算 + string ARC + 实例属性，18 断言）+ 4 负例（type-arg 数/实参错/推断失败/泛型类）。
- 全量 504/504 + bugs 19/19。
- 后续（路线）：泛型类 + 方法级 `<T>` 组合（双占位）、接口/抽象方法上泛型、A1
  （泛型体内调泛型）。

### v3.15.216 — 结构体/对象初始化器 `Pt{x:1, y:2}`（additive，selfhost）

**非破坏性加法（值式 struct 字面量，selfhost 超前 / seed 冻结不解析）**。`Pt{x:1,
y:2}` 作**值式 struct 字面量**（自带 struct 名，无目标绑定），消掉"声明 + 逐字段
赋值"样板，与数组字面量（v3.15.214/215）同族：

```myp
struct Point { int x; int y; }
Point a = Point{ x: 1, y: 2 };      // 声明初值
Point b = Point{ y: 7 };            // 缺省字段 → 零初始化
Point c = Point{ y: 3, x: 9 };      // 乱序（按字段名）
p = Point{ x: 5, y: 6 };            // 赋值位置
Point mk(int v) { return Point{ x: v, y: v * 2 }; }   // 返回位置
int s = sum(Point{ x: 1, y: 2 });   // 实参位置（一等 struct 值）
```

- **grammar §Expression**：`Primary` 增 `StructLiteral ::= Identifier '{'
  StructFieldInit (',' StructFieldInit)* '}'`，`StructFieldInit ::= Identifier ':'
  Expression`（产物文件级 struct 值，字段乱序/缺省）。
- **ast**（`ast.myp` `AstExpr`）：新 kind `StructLit`（struct 名存 className_，字段名
  列表 `initFields_` 与初值 `elements_` 平行存；cloneExpr/dump 同步）。
- **parser**（`parser.myp` `parsePrimaryInner`）：identifier 后紧跟 `{`（非 quote/fn）
  → 解析 `Name { f: e, … }` 建 StructLit（字段用 `:` 分隔）。
- **sema**（`sema.myp`）：`visitExpr(StructLit)`——按名解析**文件级 struct**（未知名 /
  类名 / 枚举名干净拒绝），字段存在性 + 重复 + 逐值 `typesCompat` 校验（int→double
  等提升允许，float 字段需 `3.0f`）；**v1 限纯标量字段**（string/类/接口/切片/数组/
  嵌套 struct/enum 引用字段干净拒绝——防浅拷贝 ARC/嵌套越界）；`var p = Pt{...}`
  干净拒绝（须显式类型）。设 resolvedKind "struct" + resolvedClass=struct 名。
- **codegen**（`codegen.myp`）：`genStructLit` = 零初始化临时槽 → 逐字段 GEP 填
  （structFieldIndex/structFieldType + convertValue）→ 整体 load `%Pt` 寄存器
  （与 `Point q = p` 浅拷贝同形态）；`exprLlvmType(StructLit)` = `%Pt`。声明/赋值/
  返回/实参均经既有 struct 值管道，无额外绑定位。
- AST dump 稳定（StructLit 专用分支）；bootstrap MD5 一致。
- 回归 `tests/@test/struct_literal.myp`（完整/缺省零初始化/乱序/提升/long·bool·
  float/赋值/返回/实参两形态，16 断言）+ 7 负例（unknown field/dup/type mismatch/
  class name/ARC field/var infer/unknown type）。
- 后续（路线）：类对象初始化器 `new P{…}`（经 accessor setter）、嵌套 struct 字段、
  `var` 推断、字符串/引用字段 struct（需逐字段 ARC 语义）。

### v3.15.215 — 数组字面量返回/赋值位置绑定（additive，selfhost）

**非破坏性加法（续 v3.15.214）**。`[e1, e2, …]` 除声明初值外，**返回位置**与
**赋值位置**现也绑定为动态 `T[]`：

```myp
int[] mk(int a, int b) { return [a, b]; }   // 函数返回 T[] → return [..] 绑定
int[] b;
b = [3, 4];                                  // 动态数组局部重赋值 = [..]
b = [5];                                     // 再次重赋值（缩容/换 backing）
P[] ps = [new P(1), new P(2)];               // 类元素：fresh 转移 + ARC
ps = [new P(9)];
```

- **sema**（`sema.myp`）：
  - Return 分支（checkStmt）：返回表达式是 `ArrayLit` 且函数返回类型 `T[]`
    （`currentRetAst_` 有 element、无定长）→ `bindArrayLitTo` 按返回元素类型绑定；
    返回类型为定长 `int[N]` → 干净拒绝 `cannot return it as a fixed array int[N]`
    （字面量产物是动态数组）+ 仍绑定防 generic 二次报错；非数组返回类型（如 int）
    不绑定 → generic 干净拒绝。
  - Assign 分支（visitExpr）：lhs 为动态数组**标识符**（SymbolEntry `type()=="array"`
    经 `entryToAstType` 重建元素 AstType）且 rhs 是 `ArrayLit` → 绑定；lhs 为定长
    `int[N]` 局部 → 拒绝 `cannot assign an array literal (dynamic array) to a fixed
    array int[N]`。lhs 为字段/下标（非标识符）保持 v1 干净拒绝。
  - `bindArrayLitTo` 容错 `v == null`（返回/赋值位无目标 var）与 `s == null`
    （赋值位无宿主语句），诊断位置回落字面量自身行列。
- **codegen**：零改动（`genArrayLit` 已通用；返回 fresh backing 走既有所有权转移，
  赋值 fresh 走动态数组槽重赋值路径）。
- 回归 `tests/@test/array_literal.myp` 扩为返回（含实参元素/提升）+ 赋值（重赋值缩容）
  + 类元素（fresh 转移/换 backing），18 断言；新负例
  `tests/negative/array_literal_return_fixed.myp` /
  `array_literal_assign_fixed.myp` / `array_literal_return_scalar.myp`。
- bootstrap myp_self2==myp_self3 MD5 一致；全量 491/491 + bugs 19/19。
- 后续（路线）：实参位置绑定、`var` 推断、定长退化填充、`@eval` 常量表（M-2）。

### v3.15.214 — 数组字面量 `[1,2,3]`（additive，selfhost）

**非破坏性加法（selfhost 超前 / seed 冻结不解析）**。`[e1, e2, …]` 作**动态数组
`T[]`** 字面量（一等值：堆 backing + ARC，绕开已坏的定长数组 return/pass 值管道）。
v1 支持**显式动态数组目标的声明初值**：

```myp
int[] a = [1, 2, 3];      // 动态数组
double[] d = [1, 2];      // int 字面量 → double 提升
string[] s = ["x", "y"];  // 字符串常量元素
int[] e = [];             // 空字面量（有类型目标）
```
- **grammar §Expression**：`Primary` 增 `ArrayLiteral ::= '[' Expression (',' Expression)* ']'`
  （产物动态 `T[]`，元素数任意；v1 需显式 `T[]` 目标）。
- **parser**（`parser.myp` `parsePrimaryInner`）：识别 `[` 建 `ArrayLit` 节点（elements）。
- **sema**（`sema.myp`）：VarDecl 显式 `T[]` 目标先在 generic visit **前** `bindArrayLitTo`
  （绑定 `expr.elemType` + 逐元素 `typesCompat` 校验齐性/可转换，int→double 等提升
  允许）；定长 `int[N]` 目标 / `var` 推断 / 赋值·实参位 → 干净拒绝（v1 范围）。
  generic `visitExpr(ArrayLit)`：已绑定 → 返回 `array`；未绑定（其它位置）→ 报
  "requires a typed dynamic array target"。
- **codegen**（`codegen.myp`）：新增 `genArrayLit` —— `myp_alloc_slice_backing(n,
  elemSize, elemKind)` 建 fresh backing，逐元素填（标量 `convertValue` 到元素类型；
  类/字符串/接口元素按 fresh 转移/别名 retain，镜像 `fixedArrayToDynamic`）；
  `exprLlvmType(ArrayLit)` = `ptr`。
- AST dump 稳定（ArrayLit 由既有 elements 表达）；bootstrap MD5 一致。
- 回归 `tests/@test/array_literal.myp`（int/double/string/empty/long，9 断言）+ 负例
  `tests/negative/array_literal_fixed.myp` / `array_literal_var.myp` /
  `array_literal_elem_mismatch.myp`。
- 后续（路线）：赋值/实参/返回位置绑定、`var` 推断、定长退化填充、`@eval` 常量表
  （M-2 语法部分即此）。

### v3.15.213 — 自动属性访问器 `{ get; set; }`（additive，selfhost）

**非破坏性加法（语法糖，selfhost 超前 / seed 冻结不解析）**。属性字段仍默认私有，
新增自动属性 accessor 声明让**外部 `obj.x` 读/写**直接路由到属性槽（编译器补全平凡
读写），消掉"手写 getter/setter action"样板：

```myp
class Point {
  property:
    int x { get; set; }          // 外部可读可写（槽即存储）
    string name { get; set; }    // ARC 字段同规则
    int ro { get; }              // 只读：外部写 → "property 'ro' ... read-only (no setter)"
}
Point p = ...; p.x = 5; int a = p.x;   // 类外（另一类方法内）
```

- **grammar §4**：`PropertyDecl` 增 `Type Identifier '{' PropAccessor* '}' ';'?`，
  `PropAccessor ::= 'get' ';' | 'set' ';'`（自动平凡形式）。
- **parser**（selfhost `parser.myp` `parsePropertyDecl`）：解析 `{ get; set; }` 组合置
  AstProp `accessorGet/accessorSet` 标志；`get`/`set` 后跟 `{…}`（自定义体）→ 干净
  拒绝 `custom accessor bodies are not supported yet`（不静默当平凡字段）。
- **sema**（selfhost `sema.myp`）：Member 读路径——外部 `obj.x` 读在 `{ get }` 放行
  （未声明 accessor 仍按私有拒，诊断不变）；Assign 写路径——外部 `obj.x = v` 在
  `{ set }` 放行（write-only `{ set; }` 经 `allowAccessorWrite_` 让读路径放行一次），
  仅 `{ get; }` 写报 read-only。类内 `this.x` 走同一槽（一致）。
- **codegen**：无需改动（属性槽即存储，Member 字段地址路径已通用）。
- AST dump 仅对 accessor 属性输出标志（既有 dump 稳定）；bootstrap MD5 一致。
- 回归 `tests/@test/prop_accessor.myp`（rw int/string、只读、类内读写，8 断言）+
  负例 `tests/negative/prop_accessor_readonly.myp` / `prop_accessor_private.myp` /
  `prop_accessor_custom_body.myp`。

### v3.15.211 — 接口方法返回自定义 class 不再回落 void（BUG-149，selfhost）

**非破坏性修复（selfhost sema.myp）**。`interface I { R m(); }` 经接口变量调用
`R r = v.m();` 此前编译错 `cannot initialize ... with value of type 'void'`——
接口收集 pass 早于类注册，`typeToKind(R)` 回落 "void" 被记进接口方法表（oracle
正常，属 selfhost 落后）。

- sema：新增 `ifaceRetTypeOf`（接口方法声明返回 AstType，MethodSig 本已保留）；
  成员调用解析处 `fr=="void"` 且对象为接口时，用 AstType **使用点惰性重算** kind
  并设 `valueClass`（声明返回类型）；devirt 具体类仍由 resolvedClass 驱动分派。
- 回归 `tests/bugs/iface_return_class.myp`（devirt new + 接口形参非 devirt + 链式
  `v.make().n()`，4 断言，双编译器）；bootstrap myp_self2==myp_self3 MD5 一致；
  全量 470/470。

### v3.15.212 — selfhost 修复战役（review 驱动，非破坏性）

**非破坏性修复（selfhost sema.myp / codegen.myp）**。按 `docs/review/*` 深挖结论
逐条修 selfhost 编译器缺陷；seed (mypc-seed) 冻结不升级，只改自举编译器。全部
bootstrap myp_self2==myp_self3 MD5 一致；全量 480/480 + bugs 19/19。

- **sem §3（bool 进数值比较 clean reject）**：去掉 BUG-118 关系比较检查的 bool 豁免，
  `0 < x < 10` 现报 `expected numeric type, got 'bool'`（对齐 oracle）。负测试
  `tests/negative/relop_bool_lhs.myp`。
- **generic B0（泛型体调非泛型类 static 返回类型占位）**：泛型 static 方法/顶层泛型
  函数体内 `Math.pi()`（返回 double）此前按占位 i32 发 call → 读错位得 v=0 静默错值。
  修复 `codegen.myp` `exprLlvmType` Call/Member 分支：obj 为**类名**（static 调用）时
  mcls3=该类自身并经 `methodRetAstType` 取声明返回类型；新增 `methodIsGeneric` 守卫防
  泛型 static（List.id<R> 返回类型参数）被误覆盖。回归
  `tests/@test/generic_body_static_ret.myp`（3 形态 3 断言）。
- **generic B1/E-G2（已支持形态；接口属性/受约束 T 调接口方法）**：(a) 接口类型类属性
  默认初值 `Shape s_ = new Circle()` 缺 fat-ptr upcast（BUG-032/033 只修局部/数组，
  属性默认初值漏网）→ opt "ptr vs {ptr,ptr}"；修复 NewExpr 属性默认初值块对接口属性
  `upcastIface` + 所有权（fresh 转移/别名 retain）。(b) 泛型受约束方法
  `T ret(T s){return s;}`（返回类型参数）模板期误报 `interface vs int`；修复 return 类型
  为类型参数（genericParam 或 className∈当前泛型上下文）时跳过模板期严格检查（实例化
  权威）。回归 `tests/@test/iface_prop_default.myp`（4 断言）+
  `tests/@test/generic_iface_bound.myp`（2 断言）。
- **concurrency C-1（@parallel 体内读类属性）**：并行 body 抽成独立函数只捕获局部，
  this 未传 → 体内 `tally[i]=step_` 引用外层 `%this.addr` → opt "use of undefined
  value"。修复：实例方法 @parallel 捕获 this（ptr，借用不 retain），body 解包把 this
  alloca 命名为 `this.addr`（loadThis 即读它）。回归
  `tests/@test/parallel_prop_read.myp`（裸属性/this.x/顶层对照，3 断言）。
- **sem §6.2（裸具名函数作一等值 clean reject）**：`(int)->int f = inc;` / `f = inc;`
  此前 selfhost 放行 → codegen 非法 {ptr,ptr} IR → opt 崩（oracle 拒 "undefined
  variable"）。修复函数类型变量**声明初始化**与**赋值**两路径对「注册顶层函数名、无函数
  类型变量条目」的 RHS 干净拒绝（复用函数实参既有 lambda 提示消息）。负测试
  `tests/negative/fntype_bare_fn_init.myp` / `fntype_bare_fn_assign.myp`。
- **generic B2（元组返回中的 T 未替换）**：泛型函数 `(int,T) pair<T>(...)` 实例化
  `pair<string>` 值类型得 `(int, void)`（取模板返回未替换）。修复 `destructureTupleElems`
  （sema）与 `exprLlvmType` 元组返回块（codegen）优先用 `CallExpr.resolved`（实例名，
  clone 已替换）而非 callee 模板名。回归
  `tests/@test/generic_tuple_return.myp`（单/多类型参数 2 调用 4 断言）。
- **numeric N-2/N-3（常量除零/越宽移位 clean reject，鲁棒性优于 oracle）**：
  `100/0`、`1<<33` 整数零除数/越宽移位是 LLVM poison/UB，此前静默垃圾值。sema Binary
  分支：字面量零除数（float/double 除零 = IEEE inf 除外）报 `division by zero`；字面量
  移位量 ≥ 运算宽度（long 系 64，其余 int 提升 32）或为负时报错。负测试
  `tests/negative/div_const_zero.myp` / `shift_too_wide.myp`。
- `.gitignore` 增 `docs/review/`（本地 review 文档不入库）。

### v3.15.210 — 固定数组 .size/.size() 返回编译期长度 N（BUG-148，selfhost）

**非破坏性（selfhost sema+codegen）**。定长数组 `T[N]`（如 `int[4] a`）此前无长度
API：`a.size()`/`a.size` 编译报错（无该成员 / void）。现支持 `.size` 与 `.size()` 属性/调用
两形式返回编译期常量 N（与 slice 的 `.size()` 一致）：

- sema：Member 属性与 Call 两分支对「定长数组表达式」放行（`fixedArrayLen`：局部/参数符号
  arraySize、this.属性/裸属性、struct 变量.字段、函数返回定长数组）；动态数组 `T[]`
  （arraySize=0）不拦截，维持「无运行时长度请用 slice<T>」报错。
- codegen：Member/Call 两处发射常量 N（局部/参数从 varAstType.arraySize，this.属性从
  propAstType，struct 字段从 memberFieldAstType）。
- 回归 `tests/@test/fixed_array_size.myp`（局部/this.属性/struct 字段的 `.size`+`.size()`，
  6 断言）+ 负测试 `tests/negative/dynarray_size.myp`（动态数组 `.size()` 干净拒绝，
  selfhost 此前在 int 返回语境漏到 codegen 坏 IR，现已 sema 拦截）；bootstrap MD5 一致；
  全量 469/469。

### v3.15.209 — struct 字段 slice 直接子访问走 slice 专路（BUG-146，selfhost+oracle）

**非破坏性修复**（selfhost `codegen.myp` + oracle `codegen_expr.cpp`）。struct 含
`slice<T>` 字段时直接子访问（`r.vec[i]` / `arr[i].vec[j]` / `r.vec.size/.length/.data`）
此前只对「局部 slice 变量」走 slice 专路，字段形态落入普通成员/数组路径：

- selfhost：`subscriptElemLt` 对 struct 字段 slice 默认 i32（slice<float> 位型当 i32 读
  垃圾）；`.size` 把 slice 值 `{ptr,i64}` 当对象指针二次 `%Object` GEP → opt-21 崩。
- oracle：`sliceTypeOfExpr` 只认 Identifier/嵌套 Subscript，`r.vec`/`arr[i].vec` 不识别
  → 下标把 slice 值当数组基址（LLVM 类型错）。

修复（对齐独立 slice 路径：load slice 值 → extractvalue data/len）：
- selfhost：slice 的 `.size/.length/.data` 判定放宽为「局部 slice 变量 **或** 对象 LLVM
  类型 `{ptr,i64}`」；`subscriptElemLt` Member 分支补 struct 字段回退（memberFieldAstType
  → sliceElemType / element）。
- oracle：`sliceTypeOfExpr` 增 MemberAccess（struct 字段 slice → 缓存
  `member_slice_types_`；对象含 struct 变量/数组元素/struct 返回调用）；
  `generateMemberAccess` 的 `.size/.length/.data` 以 `sliceTypeOfExpr(e.object)` 兜底
  （非 slice 不动）。

回归 `tests/bugs/b146_struct_slice_field.myp`（判别 A–F：`r.vec[0]` / `arr[1].vec[2]` /
`r.vec.size` / 字段 slice 写 `r.vec[1]=v` / 函数返回 struct 直取 `.vec[2]` /
slice-of-struct 元素写 + 对照局部；修复后双编译器 5 测试 12 断言）；bootstrap MD5 一致；
全量 467/467。

### v3.15.208 — 实例属性数组下标 this.buf[i] 元素类型（BUG-147，selfhost+oracle）

**非破坏性修复**（selfhost sema/codegen + oracle codegen）。向量二进制化的字节缓冲通常
存成类实例属性并 `this.buf[i]` 逐字节写，但显式 `this.` 形态的元素类型解析两编译器都缺
`ThisExpr` 对象分支：

- selfhost sema：`this.buf[i]` 下标元素类型落成 `array`（读/写均报类型错）。
- selfhost codegen：`subscriptElemLt`/`subscriptIsSlice`/`subscriptElemIfaceName` 的
  Member 分支只认静态类与链式对象，`this.` 落到默认 → `%Object`（opaque）。
- oracle codegen：`generateSubscript`/`generateAssignment` 的 MemberAccess 分支只认
  Identifier 对象；`this.buf[i]` 落到默认 i32 → ubyte[] 属性按 i32 GEP/store（步长 4、
  4 字节写）→ `str(this.bytes)` 读到 `[41 00 00 00]` 输出 "A"（本地 ubyte[] 却 i8 正确）。

修复：两编译器把 `this` 归到当前类再解析属性元素类型（镜像已有静态类/裸属性路径）。
回归 `tests/@test/this_prop_array_bytes.myp`（ubyte[] 逐字节写/读、str() 往返、int[]
属性、bytes() 入属性，6 断言）；全量 467/467。oracle 端 o1 探针 `prop=ABCD` 修正。

### v3.15.207 — 顶层 const 恢复值语义与正确 LLVM 类型（BUG-145）

**非破坏性修复**（C++ oracle + selfhost sema/codegen）。顶层 const 虽借用函数 AST
容器保存声明类型和初始化式，但语言语义是值。此前 BUG-050 通过生成同名零参函数并在
裸引用处隐式调用来兼容；selfhost `exprLlvmType` 又把该标识符回落为 i32，导致
`const bool` 生成 `call i1` 后按 i32 转换的非法 IR，`const string` 也存在 ptr/i32
推断分裂。

- const 标识符现在直接展开已折叠的初始化表达式，双 codegen 不再声明或生成同名函数。
- selfhost `exprLlvmType` 从 const 声明读取真实 LLVM 类型（string=ptr、bool=i1 等）。
- `CONST()` 调用语法按值语义明确拒绝：`'CONST' is not callable`。
- 回归：`const_string` 与 `eval` 全部改为裸值引用；新增 `negative/const_call.myp`；
  双编译器聚焦测试通过且 IR 中无 const 同名函数/调用。

### v3.15.206 — try 内 return 恢复异常 handler 深度（BUG-144）

**非破坏性**（C++ oracle + selfhost codegen + 双 runtime）。长期运行的 mypagent
高频在 `Json_Json_string` 后由 `__longjmp` 段错误；现场 handler depth 已达 63，栈中
保存了大量已返回函数帧的 jmp_buf。根因是无 finally 的 `try` 中执行 `return` 时直接
发射函数返回，绕过 try 正常出口的 `myp_exception_pop()`。

- 新增 `myp_exception_get_depth()` / `myp_exception_pop_to(base)`；仅对含 try 的函数在
  entry 保存调用前 handler 深度，并在每个真实 `ret` 前恢复到该深度。
- 函数级基线同时覆盖嵌套 try、catch 内 return，以及 finally 转发后的最终 return；
  正常路径已 pop 时 `pop_to(base)` 保持幂等。
- C++ oracle 在首次生成 try 时将基线捕获懒插入 entry；selfhost 用 `stmtHasTry` 仅为
  含 try 的函数发射入口和返回开销。
- 回归：`tests/@test/exception_propagation.myp` 连续 100 次跨函数 try 内提前 return，
  随后新建 handler 抛出并捕获；修复前稳定跳入悬垂 jmp_buf 段错误，修复后双编译器
  121/121 断言通过；bootstrap stage2/stage3 MD5 一致。

### v3.15.205 — selfhost struct 值拷贝/入槽 string 字段 ARC retain（BUG-143，struct_arc_string_fields 转绿）

**非破坏性**（selfhost `codegen.myp`）。mypagent 长 LLM 回答偶发 `__longjmp` 段错误
排查根因：`HttpsResult`（struct 含 string 字段）从 parseResponse→request→postJson
逐层 struct 值传递时 body 字段持有计数在值拷贝中丢失 → 长 body 悬垂 → 越界读破坏
异常帧 jmp_buf。C++ oracle（src/codegen `emitStructFieldsValue`）本就齐全；
**selfhost gap**。

- **BUG-143**：selfhost 只在 struct **字段 store**（storeRef）时 +1；struct **整体
  拷贝/赋值/入类属性**是裸字节拷贝 → 拷贝与源共享 string。源字段随后被重赋值/释放
  （`r.body = ""`）→ 拷贝读到被复用内存（判别：`Res r2 = r; r.body=""; return r2;`
  → Str.len(r2.body) 从 20000 变 20032 垃圾）。
- **修复**（镜像 C++ emitStructFieldsValue/emitArcFieldOp）：
  - 新增 `emitRetainStructValue`/`emitRetainArcFieldValue`/`structNameOfType`/
    `isFreshStructExpr`：struct 值拷贝逐字段 retain。
  - 覆盖点：VarDecl struct 别名初始化、整值赋值到 struct 局部、struct 型类属性存储、
    struct 字段写。fresh（调用返回 struct，字段已带 +1）跳过、不重复 retain。
  - **{ptr,ptr} 接口/函数值字段跳过**：selfhost 既有模型里 struct 接口字段是借用
    别名（字段 store 不 retain、无对应释放对），拷贝 +1 将泄漏（map_data_struct_iface
    用 liveObjectCount 断言该平衡）。只处理拥有型字段：ptr（string/class/动态数组）
    与 {ptr,i64}（slice），嵌套 struct 递归。
- **回归**：`tests/struct_arc_string_fields/test.myp`（拷贝+drop / 多层返回 / 入属性，
  毒化复用判别）；run_tests.sh 465/465、bootstrap MD5 门禁成立。
- **C++ oracle**：无需改动（emitStructFieldsValue 已覆盖拷贝/返回/属性，ASAN 探针
  全形状 20000）。
- **教训**：selfhost struct 字段的「拥有」语义须与字段 store 的 retain/release 对一致；
  只加 retain 不加 release（struct 局部作用域末不释放、类 destroy 不处理 struct 属性）
  会改变 liveObjectCount 平衡 → BUG-143 只补「拷贝丢失的 +1」，不引入新释放。

### v3.15.204 — exprLlvmType 关联类型返回解析（BUG-142，assoc 接口拼接 opt 类型错转绿）

**非破坏性**（selfhost `codegen.myp` `exprLlvmType`）。多文件引用测试矩阵期间发现；
**非跨文件特有**（单文件即可复现），selfhost gap（C++ oracle 已正常）。

- **BUG-142**：接口方法返回**关联类型** + 字符串拼接 → opt/llc 类型错
  （`interface Cont { type Item; Item getVal(); }`，`IntBox implements Cont` 且
  `type Item = int`，`"v=" + c.getVal()`）→ `opt-21: '%t21' defined with type
  'i32' but expected 'ptr'`（`myp_strcat(ptr, i32)`）。`type Item = double` 同样
  复现。拼接矩阵其余组合（int/long/double/bool/string/反序/链式/类方法返回/接口非
  关联返回/struct 字段）全正常。
  根因：`exprLlvmType` Member 分支对象是接口变量时跳过具体类覆盖（
  `isInterfaceName==0` 条件）→ `methodRetAstType(接口, m)` 返回关联类型占位 →
  llvmType 错成 "ptr"，与 genExpr 实际发射（sema devirt 已把具体类记到
  CallExpr.resolvedClass，`IntBox_getVal` → i32/double）脱节。
  修复：exprLlvmType Member 分支补 **B3/BUG-017 镜像**——`e.resolvedClass()`
  （具体类）非空且实现该接口时，用 `methodRetAstType(具体类, m)` 解析关联返回。
- **回归**：`tests/bugs/b142_assoc_concat.myp`（Item=int/double 拼接 + 链式，3 断言）；
  run_tests.sh 464/464、bugs 16 green（新增 b142）；bootstrap MD5 门禁成立。
- **C++ oracle**：无需改动（B3/BUG-017 已按具体类解析关联返回，c_assoc 探针原本正常）。
- **教训**：字符串拼接操作数的静态类型（exprLlvmType）必须与实际发射值类型一致；
  sema 的 devirt 具体类（CallExpr.resolvedClass）是解析接口关联返回的权威来源，
  exprLlvmType 与 genExpr 都要用它。

### v3.15.203 — selfhost @coro 参数 ARC 拥有修复（BUG-141，coro_incremental_spawn 转绿）

**非破坏性**（selfhost `codegen.myp` 函数体序言）。coro_incremental_spawn（BUG-002
复现 @test）此前唯一红：自举编译的分段素数筛输出含 `15`（3×5 复合数泄漏）。**C++ oracle
codegen + 同一 C runtime 全过、selfhost codegen + 同一 C runtime 复现**——坐实
selfhost codegen bug（非 runtime，MYP runtime 排查是红鲱鱼）。

- **根因**：oracle 早在 `codegen_class.cpp registerCoroParam`（BUG-002）对 **@coro 方法
  参数（含 this）在入口 `myp_retain`**，因 @coro 体比调用方作用域长寿；**selfhost 从未
  镜像**——参数当借用。调用方推进（sieve 的 `ch = nx` 释放旧 channel）时，parked 协程
  持有的 channel 被 free → 内存块被后续 `new Channel()` 复用 → parked 协程的 `out`
  悬垂别名到新 channel → 发错目标、值泄漏（F2 的 `%out.addr` 被覆写成后生 channel 句柄；
  F3/F5 永久 park）。
- **修复**：`genFuncBody` 参数 store 后，`curFnCoro_` 下对 ARC 参数（class/string/
  动态数组/接口/函数值/slice）入口 `myp_retain`，并注册进 `funcPtrSlots_`（借用参数提升
  为拥有槽的既有机制）——函数末 `releaseArcSlots` 释放配对，重赋值经 `funcPtrSlotHas`
  走 `storeRef` 释放旧值。struct 方法合成 this（className "Object"）跳过，避免对裸结构
  指针 retain。与 oracle 的 registerCoroParam 语义对齐（selfhost 侧未加 frame_set，
  MYP runtime 的 frame_set 为 stub no-op，强制销毁泄漏维持既有水平）。
- **验证**：独立 sieve `2 3 5 7 11 13 17 19`（MYP runtime 与 C runtime 双过）；
  `coro_incremental_spawn` GREEN（bugs 15 green/0 red）；run_tests.sh 464/464；
  bootstrap MD5 门禁成立。C++ oracle 无需改动（早已含 BUG-002 修复）。
- **教训**：排查初期自举+自举 runtime 复现、oracle+Cruntime 通过 → 误判 runtime 差异，
  直到补上「**selfhost codegen + C runtime**」对照组才定位 codegen——跨编译器对照必须
  补齐 2×2 全矩阵（codegen × runtime），只比一对会混淆变量。

### v3.15.202 — mapping 目标支持 static 方法 + 构造器接口形参 upcast（BUG-136/137）

**非破坏性**（selfhost `sema.myp`/`codegen.myp`）。两处自举 ↔ oracle divergence：

- **BUG-136**：mapping 目标为 **static: 方法**（`X.ev -> Console.write;` / `Sink.put`）被
  误拒——BUG-130 目标存在性校验只查 `actions()`，漏 `staticActions()` → 报
  `mapping target 'X.y' is not an action on class 'X'`；且即便放行，自举
  `genMappingChain` 目标解析先查 `hasInstanceGlobal`（mapping 会给目标类建
  `__myp_inst_<cls>` 全局）→ 静态函数被塞实例指针 → 载荷错位。oracle 两端均正常
  （`tests/test_thread.myp` 的 `Worker.output -> Console.write` 打印 10/20/30）。
  修复：① sema 目标校验补 `staticActions()`；② codegen 目标解析 **static 优先于实例
  全局**（静态目标 `instV=""`，镜像 C++ `codegen_class.cpp` 的 `is_static_action_`）。
- **BUG-137**：**构造函数接口形参未 upcast**——`new LLMPlanner(new RealBackend())`
  （ctor 形参为接口 `Backend`、实参为裸具体 `new`）只走 `convertValueU` 数值转换，不造
  `{data,vtable}` 胖指针 → `opt-21` `ptr vs {ptr,ptr}` 编译失败。oracle
  `generateNewExpr` 对 ctor 实参做接口 upcast（同 `generateCall`），自举缺失（BUG-034
  只覆盖方法调用实参/返回/字段写三路）。修复：New 分支按 ctor 形参类型建 `apTypes`，实参
  `at=="ptr" && 形参=="{ ptr, ptr }"` → `upcastIface` 上转。
- **回归**：`tests/bugs/b136_mapping_static_target.myp` + `b137_ctor_iface_param.myp`
  （GREEN）；run_tests.sh 464/464、bugs 套件新增两用例 GREEN；bootstrap MD5 门禁成立。
- **BUG-140**：逃逸分析健全性——此前对**方法调用接收者 `v.m()` 一律放行**，若方法把
  `this` 存进进程全局（@static 类属性 `Holder.set(this)`）→ `new T()` 被栈上分配后
  全局悬垂 → 退出清理双释放/跳 0x0（`b032` 崩溃根因，O0/O2 两编译器全崩；event 类是
  红鲱鱼，需实例含 ARC 数组字段才显形）。修复：接收者调用改 interprocedural
  （`escMethodThisEscapes`/`callReceiverEscapes`/`escVarClass_` + `This` 候选处理），
  方法逃逸 `this` 则接收者不栈上化。b032 转绿、run_tests.sh 464/464、bugs 14 green。
  C++ oracle `escape_analysis.cpp` 同逻辑已镜像修复（b032 两编译器均绿，bootstrap MD5
  逐字节一致）。

### v3.15.201 — runtime_myp 线程池防丢唤醒修复（@parallel 小 n 高频微并行死锁 ~50% → 0）

**非破坏性**（runtime_myp/pool.myp，@parallel 线程池）。deeplearning CPU 训练非确定性根因：
`poolWorkerEntry` 经 `poolWorkWait()` **先查空 deque、再快照 workSeq 去 futex_wait**——发布者
（`myp_pool_parallel_for`：复位计数 → 复位 deque → 推 chunk → 设 totalChunks → workSeq+1 →
futex_wake）在 worker 快照之后 ++/wake、且 worker 快照到新值 → `futex_wait(值==期望)` 真睡在
无等待者的 wake 上 → 块无人处理 → 主线程 barrier 永等 → 死锁。触发：小 n / 高频微 @parallel
（n=1..32 × 64k 次调用 `~50%` 挂起）；deeplearning CPU 训练小张量网（activ_chain/batch_matmul
等）同机制偶发跑偏/挂起/不收敛（大网 cnn/tanh/mse 并行调用少/块大 → 稳定）。修复：①
`poolWorkerEntry` **先快照 workSeq、再查 deque**（pop 自家 → 偷别人），复查仍空才
`futex_wait(快照值)`——发布者 ++ 前块已入 deque ⇒ 快照后必取到；删 `poolWorkWait`。②
`myp_pool_parallel_for` 的 deque 复位（bottom/top=0）包 `dqLock(t)/dqUnlock(t)`——防上一轮
刚跑完未停靠的 worker 并发 pop/steal 读到撕裂 bottom/top 取到幽灵块。⚠️ 运维要点：
runtime_myp/*.myp 由 build.sh 编译成归档 `libmyp_rt_myp.a`，mypc 链接用户程序按
`<exe_dir>/libmyp_rt_myp.a` 引用——只改磁盘源码**必须重建归档**
（`cmake --build build --target myp_rt_myp`）才生效（本次修复即因未重建归档致首验无效，
nm 见 `poolWorkWait` 仍在；重建后符号消失、挂起消除）。回归：par_smalln（64k 微 @parallel）
100/100 不挂（修前 ~50-60%）；batch_matmul/activ_chain CPU 各 20/20 确定性 OK（此前 6 跑
5 FAIL / 12 跑 3 偏）；`rt_pool_test` 增第 5 段高频小 n 回归防护 8/8（防丢唤醒，~0.2s）；
runtime_myp-only PASS；deeplearning GPU 金标准 135/135；bootstrap 16/16 + 主套件 467/467。

### v3.15.200 — 退出全量环收集跳过（编译/推理加速 21.5s → 5.7s，3.8x）

**非破坏性**（runtime_myp 退出路径）。perf 实测定位编译慢真正根因：**`ccAddrOk`
占 87.87%**（此前误判为 sema/LLVM 后端——selfhost 分段计时证明 compile() 内部
仅 ~5.7s，大头在 main 返回后 `myp_free_all` 的全量环收集）。机制：`alloc.myp
myp_free_all` 退出时调 `myp_collect_cycles()`——对**全部 live 对象**（编译器跑完
3d_unet_train 后 25 万+ 对象，分散在数千 chunk）markGray 级联释放，每对象
`myp_release`→`ccAddrOk` 慢路径遍历全 chunk 链（O(1) okHint 快路径在跨 chunk
场景命中率低）→ **O(N×chunks) ≈ 8 亿次内存读 → ~15s**。修复：`myp_free_all`
退出时**默认跳过环收集**（进程退出 OS 回收全部内存；环收集仅为了让
`myp_mem_report` 不把环报为泄漏）。`MYP_MEM_REPORT=1` 或 `MYP_FULL_EXIT_CC=1`
时保留收集（mem_report 需环已回收才报真泄漏）。收益：单次编译 21.5s → 5.7s
（3.8x）、峰值内存 214MB → 170MB；一次性 CLI 程序（编译器/推理）退出全部提速。
附带：selfhost 加 `MYP_SEMA_TIMING`/`MYP_COMPILE_TIMING` 分段计时（前端/sema/
codegen/link/退出各阶段，编译诊断用）。回归：3d_unet_train `3D UNET TRAIN OK`
acc=100%、llm_runtime_gpu_main `LLM RUNTIME GPU CHECK OK`、hello exit=42 全
正常；自举 2 级 MD5 一致（aee3b37b）。

### v3.15.199 — selfhost 定长接口数组类型修复（`IOp[128]` 错成 `[128 x ptr]` 溢出别名）

**非破坏性**（selfhost codegen）。自举编译器 `IrEmit.llvmType` 对定长数组用简单
元素类型递归：接口元素返回 `ptr` → `IOp[128]` 错成 `[128 x ptr]`（8B/元素，共
1024B），而 store/load 用 `{ptr,ptr}` GEP（16B/元素，2048B）→ **slot≥64 溢出
写穿数组边界、别名到后续字段**。暴露于 deeplearning 算子接口化：`bwdCpu_[70]`
与 `fwdGpu_[6]` 同址，SoftmaxOp 注册后被 GpuMatmulOp 覆盖 → `bwdCpu_[70]
.backward()` 调错类方法（GPU 过、CPU 挂；执行时逐 op checksum 追踪定位，非虚表
/分派/ARC 问题——LLVM vtable、upcast、分派、retain 全对）。修复：`codegen.myp`
`llvmType()` 增定长数组分支，元素类型用 codegen 自身 `llvmType`（接口/struct/
枚举/bitfield 感知），`IOp[128]` → `[128 x {ptr,ptr}]`（2048B），与 GEP 一致。
对齐 C++ oracle（`typeNodeToLLVMType` 本就正确）。回归：deeplearning 全接口对拍
`FULL IFACE CHECK OK`（修复前 prob 分歧 1016/1024）；bootstrap 95/95 + 主套件
466/466 全绿；自举 2 级 MD5 一致。

### v3.15.198 — selfhost GPU kernel 链接 libdevice（`Math.exp` 等真 GPU 返回 0 修复）

**非破坏性**（selfhost codegen/link）。selfhost 编译器在 `@gpu for`/`@gpu tile`
kernel 内遇到 `Math.exp/sin/log/pow/...` 超越函数时只发射 `declare @__nv_expf`
等外部声明（llc 通过即可），但运行期 `myp_gpu_load_kernel` 约定 **PTX 自包含
（编译期已链接 libdevice）**——C++ oracle 链接了 libdevice.10.bc，selfhost 没有 →
真 GPU 上 `__nv_expf` 未定义返回 0（softmax 输出全 0、CNN 训练 loss=0、acc 卡 50%）。
修复：`genKernelDeviceCall` 置 `gpuMathUsed_`，`gpuPtxFromLl` 对用数学的 kernel
先 `llvm-link kernel.ll libdevice.10.bc`，再 `opt -passes='internalize,globaldce'
--internalize-public-api-list=<kname>` 只保留 kernel 入口（删未用 libdevice 函数，
PTX 精简），最后 `llc`。`Link.findLlvmlink()` 探测 `llvm-link`（MYP_LLVM_LINK
覆盖），libdevice 路径 `$MYP_CUDA_LIBDEVICE` → 常见 CUDA toolkit 安装路径。
- 验证：`@gpu for` 内 `Math.exp(float/double)` 五种取值（-2..2）bit-exact；
  deeplearning `conv3d_grad_check_gpu` GPU 3D 反向三内核与 CPU maxDiff=0；
  3D CNN GPU 训练 acc=100（loss 与 CPU 逐轮一致）。回归 466/466 + GPU 61/61。

### v3.15.197 — opt InstCombine 爆炸修复（对象清零下沉分配器，2m33s → 亚秒）

**非破坏性**（oracle/selfhost codegen + C/MYP 双运行时）。类属性含巨型定长数组
（如 `string[1024]`/`double[1024]`）时，`new` 的对象清零以 codegen 侧
`llvm.memset` + selfhost 栈分配 `store zeroinitializer` 形式出现，opt-21 `-O2`
InstCombinePass 对其指数爆炸（单文件 2m33s，其中 InstCombine 155s）。
修复：把堆对象数据区清零下沉到 `myp_alloc_object`（C `runtime.c` +
`runtime_myp/alloc.myp`，`memset`/`__myp_memset`），oracle/selfhost 删除
`myp_alloc_object` 后的冗余 memset；selfhost 栈分配改裸 alloca
（`entryAllocaRaw`，数据区仍 memset、头部显式 store），移除 `[N x i8]`
`store zeroinitializer`。行为不变（未显式初始化属性仍归零），编译耗时
2m33s → 0.04s（约 940×）。回归：`tests/@test/manual_opt_fixedarray.myp`
（巨型定长数组类 + 清零断言，run_tests.sh 10s 超时即编译时间守卫）。

### v3.15.196 — 协程创建与 Go 同级（22ms → 14ms）

**非破坏性**（MYP runtime + oracle/selfhost codegen + 基准公平性修复）。调查
`coro_spawn` 的 7 倍表面差距后发现两类独立问题：MYP 每个协程首次启动时通过
`setjmp` 保存信号掩码，产生 20000 次 `rt_sigprocmask`；同时 runtime 按协程容量
提前分配每槽 32 项 ARC 帧表和未使用的等待表。现改用不保存信号掩码的 `_setjmp`
（oracle/selfhost/C trampoline 一致），ARC 帧明细与等待表均在首次实际使用时分配，
栈池同尺寸命中增加 O(1) 尾部快路径；trampoline 完成切回调用者后，由 `resume`
在安全栈上立即回收协程栈，普通完成不再累积 retired 元数据；每槽仅保留创建、切换
热字段，取消/自毁、等待结果和 async exec 结果拆为按功能首次使用的 sidecar。20000
个 64KB 活跃协程的 RSS 约 124MB → 87.4MB、minor faults 约 30.5k → 21.4k，
创建+完成耗时 22ms → 14ms。
- **融合首次启动 ABI**：oracle/selfhost 统一将原 create、逐项 set-arg、set-entry、
  first-resume 改为 `create_entry + start_args` 两段调用；entry 参数放在调用者
  entry-block 栈包中，首次 resume 同步读取，并保存/恢复嵌套 spawn 的参数包指针。
  单参数方法的外部 runtime 调用由 7 次降到 2 次；20k 并发驻留基准仍为 14ms
  （已由栈首次触页主导），但高参数协程不再受旧共享入口表 16 槽限制。C fallback
  同步实现新 ABI，新增 `this + 16` 显式参数回归，两套 runtime 均通过。
- **修复 MYP runtime 增量构建**：`libmyp_rt_myp.a` 的 CMake 自定义命令此前只依赖
  `myp_self`，修改 `runtime_myp/*.myp` 不会触发归档重建，可能静默使用旧 runtime
  或回退 C runtime。现用 `CONFIGURE_DEPENDS` 跟踪全部 MYP runtime 源和构建脚本。
- **修正比较语义**：旧 Go 用例允许 goroutine 在创建循环中直接结束并回收栈，而 MYP
  用例要求全部 20000 个协程先启动、挂起并同时存活，再统一恢复；这比较的是流水回收
  与并发驻留，不是同一生命周期。Go 用例现同样用 ready/barrier 保持全部 goroutine
  活跃，耗时 3ms → 13ms、RSS 约 6MB → 56MB。公平对比为 **MYP 14ms / Go 13ms**，
  MYP 仅慢 7.5%，已处同一性能级别。
- **全面结果**：30/30 verify 一致；MYP 胜 25、Go 胜 5，Go/MYP 几何均值
  **1.68**；双方均不少于 5ms 的 28 项几何均值 **1.66**。全量 **464/464**，
  stress **17/17**；ARC 帧释放、事件/FD 等待和协程异常定向回归均通过。

### v3.15.195 — Go 全面基准与锁外 Channel 同步交接（29ms → 3ms）

**非破坏性**（MYP runtime + 基准方法优化）。将 MYP/Go 对比扩到计算、内存、递归、
字节处理、协程、通道与 I/O 共 30 项，并统一为双方预热、交错运行、可选 CPU 亲和性、
只汇总 verify 一致项目；脚本新增 `BENCHES` 子集与整体几何均值，修复 verify 失败仍被
标为 OK 的 shell 判定错误。
- **发现并修复 Channel 性能回退**：v3.15.83 为解决跨线程通道竞态增加状态锁时，
  同线程 waiter 也从同步 `resume` 退化为下一轮 scheduler，`channel_pingpong` 从历史
  5ms 回退到 29ms。现锁内仅更新缓冲、弹出 waiter 和路由跨线程 mailbox；同线程 waiter
  在解锁后按深度上限 64 同步交接，既不持锁切换，也不增加调度轮次，实测 **29ms →
  3ms**，Go 为 6ms。
- **消除线程程序的 C runtime 回退**：单线程 ARC 快路径新增的
  `myp_arc_mark_threaded` 此前仅由 C runtime 定义，MYP `alloc.myp` 仍保留 FFI 未定义
  引用，导致 `@thread`/`@parallel` 程序首次纯 MYP 链接失败后追加 `libmyp_rt.a`。
  现由 MYP allocator 导出同名函数并设置 `CC.everThreaded`；归档符号由 `U` 变为 `T`，
  线程/通道程序直接输出 `(MYP runtime only)`。新增端到端回归，防止 fallback 掩盖
  MYP runtime 符号闭包缺口。
- **全面结果**：16 核、无钉核、每项预热后交错 7 轮取最小值，30/30 verify 一致；
  MYP 胜 26、Go 胜 4，Go/MYP 几何均值 **1.61**；双方均不少于 5ms 的 27 个稳健项
  几何均值 **1.73**。Go 仅在 `coro_spawn`（MYP 22ms / Go 3ms）、`fft`（72/64）、
  `sieve_odd`（9/8）、`io_socket`（83/78）领先；除 spawn 外缺口均为 6–13%。下一主攻
  方向是固定协程栈的创建/映射成本，而非纯计算代码生成。
- **验证**：`channel_stress`、`coro_churn`、多消费者回归通过，跨线程
  `xthread_storm` 连续 6 次通过；全量 **463/463**，完整 stress **17/17**。

### v3.15.194 — 叶类直达析构与 weak 空表内联快路径（8.75ms → 7.77ms）

**非破坏性**（oracle/selfhost codegen + C/MYP runtime 通用优化）。每个类对象归零时
此前都会经 release table 间接调用生成的析构桩；纯标量叶类的桩只再调用一次
`myp_free_object`，形成无效的两跳分发。同时，无 `@weak` 的常见程序仍需跨模块调用
死亡通知函数，函数内部才检查弱注册表为空。
- **按字段所有权生成表项**：无 ARC/weak 字段的 class 直接将 release table 表项
  指向 `myp_free_object`，且不再生成不可达的空析构桩；含 class/string/array/slice/
  interface/function/nested-struct ARC 字段或 `@weak` 字段的类继续使用级联析构桩。
- **weak 空表本地快路径**：MYP alloc 模块维护 weak 注册表非空标志，C runtime
  维护 release/acquire 原子 entry 计数；罕见的 weak 注册/移除负责更新，普通对象
  release 只做一次 load/branch，非空时才调用慢速死亡通知。保持 MYP alloc/weak
  独立归档模块，不引入全 runtime LTO 或用户链接开销。
- **效果与验证**：20 万个四元素 `ArrayList<Item>`、共 80 万叶对象的 mixed2，30 次
  均值 **8.75ms → 7.77ms**（约 11%），checksum 800000 不变，RSS 约 2MiB；规则不依赖
  容器、类型、方法名或循环次数。fixed-point 自举通过，全量 **462/462**，stress
  **17/17**；新增 IR 回归验证叶类直达与持引用类级联。

### v3.15.193 — liveTotalCount 逃逸可观测性修复

**非破坏性**（oracle/selfhost codegen 正确性修复）。逃逸分析的分配内省保护此前只
识别 `Memory.liveObjectCount()` 与 `liveObjectCountByType()`；仅使用
`Memory.liveTotalCount()` 的函数仍可能把局部 class 栈上化，使总存活数少计对象。
现将 total count 纳入全 TU 内省检测，保持可观测分配语义；新增局部纯 class 的计数
回归。fixed-point 自举通过，全量 **461/461**，stress **17/17**。

### v3.15.192 — ArrayList 小缓冲与循环栈对象复用（11–12ms → 8–9ms）

**非破坏性**（标准库 + oracle/selfhost codegen 优化）。短生命周期小列表此前仍为
每个实例分配动态 backing；同时，逃逸分析生成的 class `alloca` 若位于循环体，会在
每轮继续下移栈指针，较大对象长循环最终耗尽线程栈。
- **8 槽 inline storage**：`ArrayList<T>` 的前 8 个元素直接存入对象内，超过后才
  惰性分配 overflow backing；`get/set/remove/grow` 统一处理 inline/overflow 边界。
- **固定数组 ARC 析构**：oracle 与 selfhost 的 class destroy stub 对 `T[N]` 中的
  class/string/接口/函数引用逐槽 release；oracle 以单态化后的 LLVM 字段布局识别
  泛型 pointer 数组，避免依赖已离开作用域的类型参数映射。
- **循环栈对象复用**：固定大小的 stack-promoted class storage 提升到函数 entry，
  循环内每轮重写 sentinel/type-id、清零并重新构造，避免循环内 `alloca` 累积。
- **函数级栈预算**：oracle/selfhost 按函数累计 class 与常量维度数组的实际布局字节，
  仅在剩余预算可容纳候选时栈提升；默认总预算 64KB，可用
  `MYP_STACK_PROMOTION_BUDGET=<bytes>` 按目标平台覆盖（`0` 禁用）。因此 360KB
  `MappingParser` 自动回退 arena，不再依赖单对象尺寸特判，也避免巨型 entry alloca
  触发 LLVM 后端崩溃。
- **selfhost 聚合数组布局**：修正 `[N x T]` 尺寸解析未剥离末尾 `]`，导致
  `T={ptr,i64}` 等聚合元素被误按 8 字节、栈对象越界；泛型 class size 计算改用目标
  实例自身的类型参数上下文。
- **效果**：20 万个四元素 `ArrayList<Item>` 的容器负载由 **11–12ms** 降到稳定
  **8–9ms**，checksum 不变；新增 inline 边界、扩容、ARC 元素和 10 万轮栈复用回归。

### v3.15.191 — 单线程 ARC 快路径（容器负载 14–15ms → 11–12ms）

**非破坏性**（C/MYP runtime 协同优化）。ARC 此前即使程序从未启动 worker，也在
每次 retain/release 上执行原子 RMW；单变量 plain-release 探针将容器负载从
14–15ms 降至 11–12ms，确认引用计数原子指令已成为主要剩余开销。
- **单调线程模式**：`myp_cc_thread_enter()` 在创建 `@thread` 或执行 `@parallel`
  前设置 `everThreaded`，并调用 C 边界 `myp_arc_mark_threaded()`；标志一旦置位，
  进程余下生命周期永久使用 atomic ARC，不在线程退出时切回。
- **单线程 fast path**：从未创建 worker 的程序，MYP `myp_release` 使用普通
  load/store 递减，C `myp_retain` 使用 relaxed load/store 递增；strict 头校验保持。
  C runtime 自有 thread、pool 和 async-exec 三个 `pthread_create` 入口也在创建前
  标记，保证 fallback 路径并发安全。
- **效果与验证**：fixed-point mixed2 稳定 **11–12ms**（本阶段累计 29ms →
  11–12ms），checksum 不变；跨线程 ARC、parallel、cycle collector、strict 诊断
  定向回归通过。

### v3.15.190 — TLS raw arena（容器负载 17–18ms → 14–15ms）

**非破坏性**（纯 MYP runtime 分配器优化）。字符串、数组/slice backing 与 raw
scratch 原先仍在每次分配、回收和字符串原地扩容时获取进程级自旋锁；单线程去锁
探针将当前容器负载从 17–18ms 降至 14–15ms，确认仍有约 18% 热路径开销。
- **线程本地 raw 状态**：bump 指针、size-class free-list、诊断计数及字符串原地
  扩容状态迁入 `@static @thread ALocal`。常规分配、回收和尾部扩容无锁；仅新建
  64KB chunk 并挂入全局 `Arena` 链时加锁。
- **全局可发现性**：所有 raw chunk 仍挂入全局链，cycle collector 的数组/字符串
  地址校验保持完整；`Memory.arenaReservedBytes/UsedBytes` 与 C runtime 一致，报告
  当前线程 arena。跨线程释放的 backing 归释放线程本地 free-list。
- **效果与验证**：fixed-point 容器负载稳定 **14–15ms**，checksum 不变；扩展
  `cross_thread_arc`，4 线程各分配并校验 20000 个整数的 backing，强制并发 raw
  chunk 扩容。`parallel_string_new`、字符串原地 append、全量 **458/458** 与压力套件
  **17/17** 通过。顺带修正 `generic_boom.sh` 的 `r2$i` 多位索引变量重名，使
  10/20/50/100/200 类型单态化压力实际完整运行。

### v3.15.189 — TLS class arena（容器负载 29ms → 17–18ms）

**非破坏性**（纯 MYP runtime 分配器优化）。class arena 原先在每次分配和回收时
获取进程级自旋锁，即使单线程也执行原子 RMW；单变量去锁探针把容器负载从 29ms
降至 17–18ms，确认锁是当前主要开销。
- **线程本地热路径**：class bump 指针、size-class free-list 和分配计数迁入
  `@static @thread CLocal`；分配与回收不再获取全局锁。每线程按 64KB chunk bump，
  仅 chunk 扩容时锁住全局 `CArena` 链并发布新节点。
- **跨线程释放与环收集**：最后一个引用可在任意线程释放，空闲块归释放线程的
  free-list；所有 chunk 仍永久挂在全局链上，安全点 cycle collector 可继续完整枚举。
  扩展 `cross_thread_arc`，4 线程分别保留 3000 个对象，覆盖并发 chunk 扩容、字段
  完整性和跨线程最后释放；既有多环、数组环、泛型环回归全部通过。

### v3.15.188 — fresh 所有权转移（容器负载 36ms → 29ms）

**非破坏性**（LLVM 优化 pass + 自举工具链）。`new T()` 存入强引用槽时，内联后的
IR 原有 `retain(fresh) → release(old) → store fresh → release(fresh)`；首尾原子
操作净效果为零。fresh 分配地址不可能等于仍存活的旧槽值，因此无需自赋值保护，
可把初始 rc=1 直接转移给目标槽。
- **保守 ARC pass**：新增 `MypArcTransferPass`，仅匹配同一基本块、
  `myp_alloc_object` fresh 值、恰好一个中间 pointer store、随后同值 release，且
  fresh 值在窗口内无其他使用的形态；跨块、源变量继续使用、多 store、非 fresh
  一律不动。保留旧值 release 与全部 weak/cycle/析构语义。
- **oracle/selfhost 同步**：oracle 在默认 LLVM pipeline 后直接运行 pass；新增
  `libmyp_pass_plugin.so`，selfhost 外部 `opt` 使用
  `default<O#>,myp-arc-transfer,verify`。插件从 `MYP_PASS_PLUGIN`、编译器同目录或
  `build/` 探测；缺失或显式设为 `0` 时回退原生 `opt -O#`，语义不受影响。
- **效果与验证**：fixed-point `mypc` 上容器+对象负载 **36–37ms → 29ms**，完整
  mixed **71ms → 64–65ms**（本轮累计 75ms → 64ms），checksum 不变。新增
  `arc_transfer` 回归，覆盖 fresh 转移、源继续使用时拒绝优化、替换非空旧槽；
  插件关闭 fallback 同样通过。bootstrap 二级 MD5 一致；全量 **458/458**。

### v3.15.187 — ARC 字段 class 栈上化（容器负载 40ms → 36ms）

**非破坏性**（编译器 codegen + 运行时协议优化）。逃逸分析此前仅栈上化不含 ARC
字段的 class，导致局部 `ArrayList<T>` 即使不逃逸仍分配 wrapper 对象。
- **析构式栈上化**：oracle 与 selfhost 允许不逃逸、含 ARC/weak 字段的 class 使用
  栈存储；作用域退出时调用已有 `__myp_destroy_<Class>` 析构桩，正常释放 backing、
  元素和其他字段。异常区与协程继续保守使用堆分配，避免 longjmp/跨挂起点清理。
- **栈对象协议**：对象头 rc 使用 `0x7ffffffe` sentinel；C runtime 与纯 MYP
  runtime 的 `myp_free_object` 在字段析构后识别该标记，不更新 heap live 计数、不把
  栈地址送回 class arena。普通堆对象的 ARC/cycle/weak 路径保持不变。
- **效果与验证**：fixed-point `mypc` 上，容器+对象负载 **40ms → 36–37ms**，完整
  mixed 负载 **75ms → 71ms**，checksum 不变；新增 `ArrayList<Node>` wrapper 栈上化
  回归及 IR sentinel/析构调用检查。bootstrap 二级 MD5 一致；全量 **457/457**。

### v3.15.186 — 同 TU 参数 noescape 摘要（单对象传参 27ms → 0ms）

**非破坏性**（编译器 codegen 优化）。第一版跨函数逃逸分析：
- **顶层函数参数摘要**：oracle 在 TU 生成前预计算名称+参数数量唯一的顶层函数；
  若参数未被保存、返回、捕获或继续传给未知调用，则标记为 `noescape`。selfhost
  同步相同保守规则；泛型模板、重载歧义、方法/动态分派、复杂实参继续按逃逸处理。
- **调用方栈上化**：`T x = new T(); readOnly(x)` 中直接变量实参命中对应摘要后，
  不再迫使 `x` 堆分配；动态数组只读传参同样受益。保存进对象字段的参数仍保持
  堆分配，回归覆盖返回后继续访问的悬垂风险。
- **内存诊断语义**：`Memory.liveObjectCount[ByType]` 可跨函数观测分配，保护从
  “当前函数”提升为整个 TU；检测到内省调用时全 TU 禁用栈上化，保证 ARC/weak/
  cycle collector 诊断计数不被优化改变。
- **效果与验证**：100 万次 `new Item` + `process(Item)` 从 **27ms → 0ms**
  （强制 `MYP_NO_STACK_NEW=1` 对照）；新增/扩展 `esc_escape` noescape 与保存反例。
  bootstrap 二级 MD5 一致；全量 **457/457**。

### v3.15.176 — 修复多环大规模漏收集 + 长时间泄漏测试工具

**非破坏性**（纯 MYP 运行时）。长时间运行测试暴露 + 修复：
- **修复多环漏收集（重要）**：`ccAddrOk`（v3.15.175 悬垂防御）从 `Arena.head`
  （最旧 chunk）遍历 chunk 链——但 chunk 链表是**反向**的（`Arena.cur` 最新，
  next 指旧；head 最旧且 next=0）→ 从 head 遍历只检查首 chunk，多 chunk 时新
  对象全被误判为悬垂跳过 rc-- → **多环大规模漏收集**（实测 50 环全收、1000 环
  只收 204；环数越大漏越多）。修复：从 `Arena.cur`（最新）遍历。验证：1000 环
  显式收集 + 自动收集全部回收。
- **预扩容防御**：collectCycles 前 `ccEnsureCap(nroot*2+256)` 预分配 CC 哈希表，
  避免 trial 期扩容（`CC.keys = nk` 重赋值 ARC 释放旧数组 → trial 模式嵌套
  ccInsert 污染表，历史 1000 环崩溃根因）。
- **长时间泄漏测试工具**：`tests/leak_long.myp` + `tests/test_leak_long.sh`——
  混合负载（无环/有环/string/HashMap/嵌套容器）循环 + 定期报告 Node/total 存活
  计数（对象泄漏指标）与 arenaR（mmap 保留，内存占用指标）；脚本自动判断趋势。
  验证 90s~数分钟：**对象无泄漏**（Node/total 稳定有界）；混合负载 arenaR 最终
  趋平（碎片化稳态 pool 偏大但非无限增长，不会长时间 OOM）。
- **测试**：`tests/@test/cyclecollect_multi.myp`（2 tests：1000 环显式收集 + 自动
  收集下多环回收）。全量 456/456 + parity 95/95 + 自举 MD5 一致。

### v3.15.185 — string+int 组合单分配（strcat 2.3x，混合负载 1.65x）

**非破坏性**（运行时 + 编译器 codegen 优化）。strcat（`"item"+j`）100 万次
82ms vs Go 17ms——每次 `myp_to_string_*`（1 分配 + 除法）+ `myp_strcat`（1 分配 +
memcpy）两调用两分配。
- **`myp_itoa_concat(prefix, val, signed)`（runtime_myp/num.myp）**：string+int
  组合单次分配——算 prefix 长 + 十进制位数 → `myp_alloc(total+1)` → 复制 prefix
  + 从尾部 2 位查表填数字 + 负号。signed=1 有符号 / 0 无符号。
- **codegen（selfhost）**：string concat 分支，`string + int`（且非 bool/char）
  → 直接调 `myp_itoa_concat`（替代 stringifyConcat + strcat 两调用两分配）；
  无符号（ubyte/ushort/uint/ulong）须 **zext 到 i64**（sext 会把 0xFFFFFFFF 变
  -1 → 2^64-1 错值，修复 stringify_conv/numeric_underscore 回归）。
  新增 `exprIsUnsigned`/`exprIsItoaInt` helper + ir_emit preamble declare。
- **效果**：strcat 100 万次 **82ms → 46ms**（1.78x，累计 108→46 = 2.3x）；
  对象密集混合负载 **117ms → 75ms**（累计 124→75 = 1.65x，vs Go 37ms 差距缩至
  2x）。数字/拼接正确性全过（含 uint 边界 4294967295）；全量 457/457 +
  自举 MD5 一致。
- **剩余差距**：混合负载 75ms 里对象 ARC ~40ms（已近 Go 总量）+ string ~35ms。
  继续方向：编译器内联 itoa/concat（消调用+合并分配）、对象 ARC fast path 内联。

### v3.15.184 — int→string 单次分配（strcat 提速 1.3x，混合负载 +6%）

**非破坏性**（纯 MYP 运行时优化）。对象密集混合负载实测 MYP 124ms vs Go 37ms
（3.35x）——分解成本：容器 ARC 40ms + **string 84ms（最大头，含 `"item"+j`
strcat）**。strcat 100 万次 MYP 108ms vs Go 17ms（6.4x）。根因：
`myp_itoa_common`（int→string）**每次转换 2 次 arena 分配**——tmp 缓冲
`myp_alloc(25)` + 结果串 `myp_alloc(n+1)` + memcpy。
- **修复**：`myp_itoa_common` 改**单次分配**——先算十进制位数，直接分配结果串，
  从尾部 2 位查表填充（消除 tmp 缓冲分配 + memcpy）。
- **验证**：strcat 100 万次 **108ms → 82ms**（-24%）；混合负载 124→117ms；
  数字转换正确性全过（0/7/±负数/INT64_MAX/MIN/999999/50000/10/100/±10）；
  全量 457/457（runtime 改动影响所有数字格式化，回归全绿）。
- **剩余差距**：strcat 仍 82ms vs Go 17ms（4.8x）——每次 int→string 仍有
  1 次分配 + 除法循环；concat 另 1 次分配。**下一步：编译器内联 itoa + concat**
  （stringifyConcat 整数分支生成内联十进制转换，消除 to_string/strcat 调用 +
  合并单次分配——Go 的 Itoa 快路径同思路）。

### v3.15.183 — 逃逸分析第二版：动态数组栈上分配（分配基准追平 Go）

**非破坏性**（编译器 codegen 优化，语义不变）。触发：300 万次 `new int[8]`
分配基准 MYP **81ms vs Go 0ms**——Go 逃逸分析把 `make([]int,8)`（常量大小 + 本地
不逃逸）栈上化，MYP 每次都真分配 + 释放。第一版逃逸分析只处理 `new T()` 类对象，
**不处理 `new T[N]` 动态数组** → 补上。
- **数组候选**：`int[] a = new int[8]`（NewArrayExpr init，维度全为整数常量 +
  元素非 ARC）。ARC 元素（`Node[]`）需作用域末逐元素释放 → 第一版保守排除；
  非常量维度（`new int[n]`）→ 排除。
- **分析器放宽**：`escExprE`/`exprEscapes` 的 Subscript——`a[i]` 的 array 是候选
  变量 v 本身 → 允许（类似 MemberAccess object）；数组传参/返回/存容器/赋值 →
  逃逸（堆）。`escCollect`/`collectCandidates` 收集数组候选；生成时
  `isStackArrayCandidate` 复检。
- **codegen**：独立标志 `stackNewArray_`（与类对象 `stackNew_` 隔离，防构造器实参
  里嵌套 NewArray 误栈上化）；NewArray 栈上分支 alloca `[24 头 + N*elem_size]`，
  写头（count/i64@0、elem_size/i32@8、pad@12、rc=1@16、tid=ARR@20），data=+24，
  memset 数据区；不注册 ARC 释放槽（栈自动回收）。oracle + selfhost 双镜像。
- **效果**：分配基准 300 万次 `new int[8]` **81ms → 0ms**（追平 Go；`--emit-llvm`
  确认 0 个 `myp_alloc_slice_backing` 调用）；`MYP_MEM_REPORT` arrays=0（无泄漏）；
  新回归 `tests/@test/esc_escape.myp`（3 tests/6 assertions）；全量 456/456 +
  自举 MD5 一致。
- **基准全景（重要）**：`bench/run_compare.sh` 55 项计算基准 MYP 已**接近/超过
  C++(-O2)**——hashmap 2.44x、iface_dispatch 2.5x、raytracer 1.21x 快；纯计算
  基本持平（sieve 1.00/montepi 1.03/nbody 1.00）；落后项 sha256 0.83/alphabeta
  0.79/sobel 0.86（纯计算 codegen 细节，后续可查）。计算层面"达到 Go 水平"已
  达成；分配层面本次追平。剩余：leak_long 混合负载（ARC 密集，对象多逃逸）仍
  ~1097k/s——下一步方向：编译器内联 ARC/数组 release 路径。

### v3.15.182 — 修复栈上化对象 ARC 属性泄漏 + 逃逸分析混合负载实测

**非破坏性**（编译器 codegen 修复，语义修正）。逃逸分析（v3.15.181）的
**重大隐患**：栈上化对象作用域结束只回收对象本身、**不释放其 ARC 属性**
（string/动态数组/类/接口/slice/函数值，或嵌套 struct 含 ARC 字段）——对象不在
arena、跳过 ARC 槽注册 → 属性指向的堆对象**永不释放**。实测：循环 10 万次
`Holder h = new Holder(); h.set("hello-"+i);`（string 属性）退出时 `MYP_MEM_REPORT`
报告 **strings: 100001**（10 万泄漏）；禁用逃逸后 strings: 1。
- **修复（第一版保守）**：逃逸分析**排除含 ARC 属性的类**——`classHasArcProps`/
  `propHoldsArc`（selfhost codegen.myp）与 `CodeGen::classHasArcProps`（C++ oracle，
  用 `findClass` + `isArcFieldType` 递归嵌套 struct）双镜像；selfhost 在
  `escCollect` 收集候选时排除，oracle 在 `generateVarDecl` 的 `is_stack_escape`
  判定处排除。找不到类（泛型实例名等）→ 保守排除。
- **验证**：esc_leak strings 100001 → 1（无泄漏）；纯 int 对象微基准仍栈上化
  （`myp_alloc_object` 0 调用、3ms）；全量 456/456 + 自举 MD5 一致。
- **混合负载实测（重要发现）**：`tests/leak_long.myp` 60s，逃逸版 vs 无逃逸版
  （`MYP_NO_STACK_NEW=1`）均 **~1097k/s**（v3.15.180 基线 1075k/s，无差异）——
  真实代码里局部 `new` **大多逃逸**（存容器/传参/属性持有），逃逸分析只对
  "纯局部对象 + 方法调用" 模式（微基准 9.3x）有效。**真实瓶颈仍是编译器调用点
  代码生成**（跨模块小函数未内联 + ARC retain/release 簿记），下一步方向：
  调用点内联（`define internal` 小函数 `alwaysinline`）+ ARC retain/release 对
  合并消除。

### v3.15.181 — 逃逸分析 selfhost 同步（局部 new 栈上分配，微基准 9.3x）

**非破坏性**（编译器 codegen 优化，语义不变）。将 v3.15.180 已在 C++ oracle
（mypc-seed）验证的逃逸分析同步进自举编译器（build/mypc 用户级）——`let v =
new T()` 若 v 在函数内不逃逸（仅作 MemberAccess object / 方法调用接收者），
new 改发 `alloca [8+sz x i8]`（头 rc=1 + tid，data=+8）代替 `myp_alloc_object`，
且不注册 ARC 释放槽（栈自动回收）。LLVM SROA 可完全消除非逃逸对象。
- **分析器 MYP 版**（`tools/selfhost/src/codegen.myp`）：`escExprE`/`escStmtE`/
  `escCollect`/`analyzeEscapeStackVars`（与 `src/codegen/escape_analysis.cpp` 对拍）。
  保守逃逸：赋其他变量 / f(v) 实参 / 存容器 / 返回 / v=other / lambda / 并发 /
  异常 / Gpu* / 复杂语句。**放类内而非顶层**——selfhost sema 对顶层函数的
  泛型容器 `ArrayList<T>.get()` 返回类型推断为 int（类方法内正常）。
- **codegen 栈分支**：`genExpr` New → `stackNew_` 时 alloca + 写头 + memset；
  `genVarDecl` class 局部 ∈ `escapeStackVars_` 且非协程 → `stackNew_=1` 包生成、
  跳过 `arcSlotNames_`（不 release）、普通 store（不 retain）、不写 mapping 全局；
  `genFuncBody` 入口 `escapeStackVars_ = analyzeEscapeStackVars(body)`。
- **分配内省保守**：函数体调用 `Memory.liveObjectCount`/`liveObjectCountByType`
  → 整函数不栈上化（这些 API 观测 arena 堆对象，栈上化会使计数断言失真；
  oracle 与 selfhost 双镜像；修复回归 arc/cyclecollect/mem_diag/weak_multi_sub）。
  注意 selfhost 成员名用 `memberName()`（`name()` 对 Member 返回空）。
- **调试开关**（oracle 与 selfhost 一致）：`MYP_ESCAPE_DEBUG=1` 打印
  `[escape] fn: stackVars=.. hasLiveCall=..`；`MYP_NO_STACK_NEW=1` 全局禁用。
- **效果**：微基准（局部对象 + 方法调用 + 属性 100 万次）堆 28ms → 栈 3ms
  （≈9.3x；生成 LLVM 栈版 0 个 `myp_alloc_object` 调用，堆版 2 个）。全量
  456/456 + parity 95/95 + 自举 MD5 一致（stage0/1/2 三方字节一致）。
- **备注**：逃逸分析是第一版保守实现；后续可扩展（返回值/容器元素不逃逸、
  @region 集成、跨模块内联后分析）。

### v3.15.180 — ccAddrOk O(1) 快路径（混合负载 +5%）

**非破坏性**（纯 MYP 运行时）。perf 定位混合负载（`tests/leak_long.myp`）真实
热点：`myp_release` 占 81%，其中 `ccAddrOk`（trial 级联悬垂防御）的 chunk 链表
线性遍历（O(chunk数)）占其 ~86%——环收集 markGray 级联时每个对象 release 都遍历
全 chunk 链（对象数 × chunk数）。修复：
- **`ccAddrOk` 加 O(1) 快路径（alloc.myp）**：新增 `CC.okHint` 缓存上次命中的
  chunk + 先查最新 chunk（`CArena.cur`/`Arena.cur`），命中直接返回，未命中才走
  慢路径全遍历（命中的记入 hint）。
- **效果**：混合负载迭代 **820 万 → 860 万/8s（+5%）**；456/456 + parity 95/95
  + 自举 MD5 一致。
- **备注**：优化后 perf 无单一热点（释放 30% + 分配 16% + 回收 9% + retain
  1.8% 分散）——MYP 运行时已无明显局部低效点，进一步提速需编译器内联 ARC。

### v3.15.179 — 类对象独立 arena + 环收集 walk 免登记（混合负载提速 4.3x）

**非破坏性**（纯 MYP 运行时）。方案 A：彻底移除每次 `new` 类对象的 `ccAllAdd`
登记（v3.15.177 已去重压缩，但每分配仍有登记成本 + 收集期 allN 大遍历）。
- **根因**：`myp_alloc_object` 每 `new` 类对象把 `地址|type_id` 追加进 `CC.all`
  登记表（环收集枚举根用）——混排 arena 无法 walk 区分类对象与原始块（args/
  coro 的 `myp_arena_alloc` 无 type_id 头 → 假根段错误），故必须登记。登记每
  百万分配 ~5ms（实测禁用 24→20ms）+ 收集期遍历 allN（混合负载 20 万迭代 × 6
  Node = 120 万条登记）。
- **方案（alloc.myp）**：新增**独立 class arena `CArena`**（bump + size-class
  复用 + 自旋锁 + chunk used@16 维护，与 `Arena` 并行）；`myp_alloc_object`/
  `myp_free_object` 改走 `CArena`（字符串/数组/raw 块仍在 `Arena`）。环收集找根
  从"遍历 CC.all 登记表"改为 **walk `CArena`**（全是类对象，无原始块污染 → 无
  假根）：chunk 链表（新→旧）+ 块序列（活块 total∈[16,2^30) →
  rc@base0+8/tid@base0+12/data=base0+16；空闲块 next 指针大地址 → 原 total@+8），
  精确到 chunk used@16 边界（免扫未用区）。**删除 `ccAllAdd` + `CC.all/allCap/
  allN`**。`ccAddrOk`（trial 悬垂防御）同时查 `Arena` + `CArena`；`myp_arena_
  free_all` 复位两个 arena。
- **效果**：纯分配 `new`+存数组 **25→21ms**（省登记 4ms）；**混合负载
  （`tests/leak_long.myp`）240k/s → 1025k/s（4.3x）**（收集期从遍历 120 万登记
  → walk ~2700 活对象）；Node 存活稳定 ~6300（无泄漏），arenaR 稳定 2.4MB
  （内存复用不变）。
- **测试**：全量 456/456 + parity 95/95 + 自举 MD5 一致；环收集系列
  （cyclecollect/_arr/_auto/_generic/_multi/memreport）全 PASS。

### v3.15.177 — 修复 CC.all 登记表膨胀（混合负载内存降 1300x + 提速 3x）

**非破坏性**（纯 MYP 运行时）。定位并修复长时混合负载（`tests/leak_long.myp`）
内存峰值/速度问题的根因：
- **根因**：`CC.all`（环收集类对象登记表）按**分配事件**无限膨胀——`myp_alloc_
  object` 每次 `new` 类对象 `ccAllAdd` 追加登记，**同一地址被空闲链表复用就重复
  登记**（混合负载 20 万迭代×6 Node = 120 万登记）→ 收集器遍历 120 万假根 →
  CC 哈希表（keys）扩到 32MB+ → 内存大 + 收集慢拖垮整体。
- **修复（alloc.myp）**：`myp_collect_cycles` 开头**压缩 + 按地址去重** `CC.all`
  ——复用找根同款校验（ctid==rtid / rc>=1 / 活块）过滤死条目，再用 CC.keys 哈希
  （ccLookup/ccInsert）标记已见地址去重；压缩后若容量 >4x 实际且 >4096 则缩容
  （防峰值遗留大数组）。allN 回到存活对象数 → 收集器只处理真根。
- **附带改进**：size-class 分桶（2 幂类 32..32768+，`freeLists[16]`，`classOf`）
  替换单 freeHead 首适——桶内 first-fit 遍历（分桶更小 → 匹配更好 + 遍历更短）；
  归桶按 `classOf(total)`；`myp_arena_free_all` 清 freeLists。
- **效果（leak_long 混合负载 90s）**：RSS 峰值 **11.9GB → 9MB**（降 1300x，接近
  同负载 C++ 3.8MB），迭代 **86k/s → 255k/s**（提速 3x，较修复前 size-class 版
  33k/s 快 7.7x）；Node/total 存活计数稳定有界（无泄漏）；arenaR 764KB 趋平。
- **测试**：全量 456/456 + parity 95/95 + 自举 MD5 一致；环收集回归
  `cyclecollect_multi`（2 tests）通过。

### v3.15.178 — ARC 释放快路径优化（单对象分配提速 1.48x）

**非破坏性**（纯 MYP 运行时）。定位混合负载性能瓶颈（对照 C++/Go 基准）：
单对象 `new`+释放链是 ARC 最大开销（`weak_notify + release_table + destroy stub`
调用链）。优化：
- **`myp_weak_notify_death` 快路径（weak.myp）**：弱注册表空（`Weak.head==0`，
  绝大多数程序无 `@weak`）→ 直接返回 1（无观察者，应释放）——省 `weakLock` +
  `weakFindEntry`。并发安全：weak 添加前必先 retain 对象（防释放），故 rc→0 时
  不可能有正在添加的观察者。
- **合并冗余二次 weak 通知（alloc.myp）**：`myp_release` 类对象分支已调
  `myp_weak_notify_death` 一次，原再委托 `myp_release_class_obj_ex`（内部又调一次
  弱通知）→ 改为**内联分发**（查 release_table → destroy stub 或 `myp_free_object`），
  消除二次弱通知 + 一次函数调用。
- **效果**：单对象 `new`+释放 100 万次 **46ms → 31ms（1.48x）**；456/456 @test +
  自举 MD5 一致。
- **备注**：混合负载（`leak_long`）瓶颈另在 CC 环收集频率/副作用 + 容器，非本
  次优化范围（后续 v3.15.179 系列）。

### v3.15.175 — 泛型数组元素释放修复 + 泛型环收集

**非破坏性**（编译器 + 两运行时）。按「零用户操作 + 最少运行时内存操作」方向
第六步：修复泛型容器（`ArrayList<Cls>` 等）类元素永不释放的真实泄漏（长时间
运行 OOM 根因），并补全经数组/泛型容器的环收集：
- **编译器（selfhost + C++ oracle）**：`arrayElemKind` 先用 `resolveType` 解析
  泛型占位 T → 具体类 → `new T[n]`（`ArrayList.data_` 等）elem_kind 从恒 1（标
  量）改为 0（类引用，销毁逐元素释放）。此前泛型容器持有类元素永不释放 → 积
  累 OOM。同 C++ `isArcClassType` 对 `current_type_params_` 的占位解析。
- **编译器（selfhost + C++ oracle）**：`__myp_max_type_id` 由 `constant` 改
  `global`（可写全局，与 `__myp_release_table` 一致）——runtime 经
  `__myp_fn_addr` 的外部引用此前对 constant 链接错位（读垃圾 maxv，如
  `Box<Node>` tid=6 > 假 maxv=3）→ 泛型实例对象被误拒 → 泛型环/数组泄漏。
- **MYP 运行时**：泛型实例类（`ArrayList_Node_inst`/`Box_Node_inst`）正确参与
  环收集；trial 级联加**悬垂字段防御**（`ccAddrOk`：字段指针须在 arena chunk 映
  射范围 + type_id 合法，否则跳过）——max_type_id 修复后所有类参与 trial，暴露
  个别对象持有的悬垂引用（已释放 string/对象被复用），此前靠假 maxv 规避。
- **C 运行时**：补数组/切片 backing 元素级联（`myp_cc_array_cascade`，trial/
  restore/collectWhite 对齐 MYP `ccArrayCascade`）——此前数组在 trial 当叶子，
  经数组的环不检测；collectWhite 白数组此前留在原地泄漏，现释放。trial 加 8
  对齐悬垂防御。
- **测试**：`tests/@test/cyclecollect_generic.myp`（3 tests）——`ArrayList<Node>`
  环 / acyclic 释放 / 泛型 `Box<T>` 环。全量 455/455 + parity 95/95 + 自举 MD5 一致。

### v3.15.174 — 深环栈溢出保护 + 修复收集器 OOM

**非破坏性**（纯运行时 + stdlib，不碰编译器）。按「零用户操作 + 最少运行时内存
操作」方向第五步：修复一个真实 OOM + 深环健壮性：
- **修复收集器 OOM（重要）**：收集器 trial 分支残留的调试打印用
  `myp_to_string_u64/i64` 分配字符串 → 释放 → trial 模式又触发打印 → **无限递归
  分配字符串 → 内存爆炸 OOM**。已删除所有收集器调试打印（MYP 运行时），收集器
  恢复正常（全量 454/454 + 深环不崩）。
- **深环栈溢出保护**（MYP + C 运行时对齐）：级联递归深度超限（45000）→ 停止
  深入 + **按 `decs`（trial 递减计数）回滚 rc** + 放弃本次收集（保守保留，不
  崩溃）。20 万层深链环稳定不崩（此前段错误）。浅环正常收集。
- 已知限制（既有编译器 bug，非本次引入）：泛型 `new T[]` 的元素类别恒为标量
  （`arrayElemKind` 对泛型占位 T 不解析，selfhost 与 C++ oracle 一致行为）——
  `ArrayList<Cls>` 等泛型容器存类元素**不级联释放 → 泄漏**（无环也泄漏，长期
  可致 OOM）。修复需改编译器（泛型实例化替换 NewArray 的 elemType）+ bootstrap。

### v3.15.173 — 数组/切片环检测 + 报告纳入字符串/数组 + 并发窗口补收集

**非破坏性**（纯运行时 + stdlib，不碰编译器）。按「零用户操作 + 最少运行时内存
操作」方向第四步：补 v1 保守覆盖缺口 + 完善泄漏诊断 + 闭合并发窗口：
- **数组/切片环检测**：collectWhite 之前数组当叶子（`class→Node[]→Node→A` 的环
  泄漏）。现 `ccCascade` 统一模式级联——trial/restore 对数组/切片 backing 逐元素
  `myp_release`（pad==0 类元素 / pad==2 slice 元素；visited 守卫防共享元素重复
  级联）；collectWhite 回收**白数组 backing**（元素由各自 collectWhite 处理）。
  共享数组元素（活对象）经 trial+scanBlack 记账精确保活（不误释放）。
- **MYP_MEM_REPORT 纳入字符串/数组**：泄漏报告新增 `strings:` / `arrays:` 行
  （退出自动收集后残余真泄漏/程序期对象；`"cycle-string"` 随环回收不报）。
- **@parallel 结束补收集**：`myp_pool_parallel_for` 结束时（workers 已完成、主
  线程安全点）若 pending 保持 → 立即 `myp_cc_try_collect()`，闭合并发窗口泄漏
  （@thread 体结束在自身线程、其他线程可能并发 → 保持跳过，由事件循环/退出兜底）。
- 已知特性：`Memory.liveArrayCount()`/报告 arrays 行含收集器内部 backing
  （ccAll 登记表 + 哈希 keys/colors 常驻复用）——用相对值测量。
- `tests/@test/cyclecollect_arr.myp`：经数组环 / 经数组自环 / 共享数组元素保活 /
  活数组保留，4 tests。
- 压力测试：2000 普通环节点 + 1000 经数组环节点 → collect 后 1（全回收，幂等）；
  泄漏测试：环（含字符串）退出自动回收，报告只报真泄漏。
- 全量 454/454、平价 95/95、bootstrap MD5 门通过。

### v3.15.172 — 执行期自动环收集（分配水位 + 安全点 + 并发守卫）

**非破坏性**（纯运行时 + stdlib，不碰编译器）。按「零用户操作 + 最少运行时内存
操作」方向第三步：把环收集从「退出自动 + 显式调用」升级为**执行期自动**——
长跑程序（事件循环/服务器）不再需要任何用户操作，环在执行期自动回收：
- **分配水位**：`myp_alloc_object` 每类对象分配 +1 计数，到阈值（默认 100000，
  `Memory.setCollectThreshold(n)` 可调，<=0 关闭）置 `pending`。热路径仅一次
  add + 比较（==threshold 恰好触发一次，无重复写）。
- **安全点自动触发**：`myp_event_process_all()`（事件循环派发 + 协程调度器都走
  它）末尾调 `myp_cc_try_collect()`——水位到且无并发则收集。无事件循环的应用
  可在自选安全点调 `Memory.collectCyclesIfNeeded()`。
- **并发守卫**：进程级活动线程计数 `threadsActive`（原子，arena 4B 地址）。
  `@thread` 体存活（`myp_thread_spawn` 进 / `myp_thread_child_entry` 出）与
  `@parallel for` 进行中（`myp_pool_parallel_for` 全程）→ >0 → 自动收集跳过
  （并发下引用变更不安全，回退显式 + 退出自动）。
- `Memory.setCollectThreshold` / `collectThreshold` / `collectCyclesIfNeeded`
  （stdlib）。
- `tests/@test/cyclecollect_auto.myp`：事件循环自动收集 / 线程守卫 / 自环自动，
  3 tests（453/453 全量通过，平价 95/95，bootstrap MD5 门通过）。
- ⚠️ 计数器用 `myp_arena_alloc(4)` 后须**显式清零**——空闲链表复用返回脏内存
  （实测 thr=32 导致守卫误判"有活动线程"而跳过收集，v1 曾漏回收）。

### v3.15.171 — 环收集器（ARC + trial-deletion，自动回收引用环）

**非破坏性**（纯运行时 + stdlib，不碰编译器）。按「零用户操作 + 最少运行时内存
操作」方向第二步：ARC 无法释放的**引用环**（自环/互引用）由收集器自动回收——
**零每次分配/释放开销**，仅收集时一次 O(n) 扫描（Bacon-Rajan trial-deletion）：
- **C 运行时**（`src/runtime/runtime.c`）：`myp_collect_cycles()` 快照分配链表
  的 class 根 → markGray（trial=1：rc-- + 级联，不释放）→ scan（restore=1：
  rc>0 者 rc++ + 级联标黑；rc==0 标白）→ collectWhite（finalize=1：字段零操作，
  真实释放白类对象；白字符串直接回收；数组 v1 保守保留）。模式钩子接入
  `myp_release` / `myp_free_object` / `myp_weak_clear`（trial/restore 期不扰动
  弱注册表、不真实释放）。共享子对象经「trial 递减 + scanBlack 恢复」记账精确
  保活（不误释放、不泄漏）。
- **MYP 运行时**（`runtime_myp/alloc.myp` + `weak.myp`）：同名 `myp_collect_cycles`
  移植。**根枚举用类对象登记表 `ccAll`**（`myp_alloc_object` 每分配 O(1) 追加
  `(data, type_id)`，打包进 48 位地址 + 16 位 type_id）——替代 arena walk
  （walk 无法区分原始块如 args/coro 的 `myp_arena_alloc` 无 type_id 头 → 假根
  段错误）；收集时校验「当前 type_id == 记录 type_id」过滤被释放/复用的旧条目。
- **自动触发**：进程退出时 `myp_free_all` 开头自动收集（工作线程已 join、main
  局部已释放 → 安全点），随后 `MYP_MEM_REPORT` 只报**真泄漏**（环不算泄漏）。
- `Memory.collectCycles()`（stdlib）：执行期安全点（单线程阶段/空闲时）显式触发。
- `tests/@test/cyclecollect.myp`：2 节点互引用环 / 自环 / 活对象保留 / 共享子
  对象保活 / 幂等，5 tests（452/452 全量通过，平价 95/95，bootstrap MD5 门通过）。

### v3.15.170 — MYP_MEM_REPORT 泄漏报告（按 type_id 分组打印存活对象）

**非破坏性**（纯运行时 + stdlib，不碰编译器）。按「零用户操作」的内存管理方向
（ARC 自动释放 + 自动回收，不做手动 free/破环），第一步落地**泄漏定位**：
- 设环境变量 `MYP_MEM_REPORT=1`，进程退出时自动打印**仍存活**的 class 实例，
  按 type_id（类名，来自 `__myp_type_name_table`）分组 + 总数。自举程序（MYP
  运行时归档）在 `myp_free_all` 开头打（main 局部 ARC 槽已释放 → 真泄漏/程序期
  对象）；C 运行时经 atexit（协程清理后、原始释放前）。
- `Memory.memReport()`（stdlib）任意时刻打同款快照（stdout，@test 可捕获）。
- MYP 运行时读进程级 `TLive.counts`（跨线程累计，非 TLS）；C 运行时遍历进程级
  分配链表（按 类/串/数组 布局分类，STR/ARR 先判）。

**配套维护**（既有 BUG-074 之后 runtime_myp 未同步，归档一直无法重建）：
`runtime_myp/*.myp` 的 `while (1)`/`if (int)` 陈旧写法统一改 `while (true)`/
`if (x != 0)`（11 文件）+ `num.myp` 函数作用域 `c`→`sc`（避免与 while 块 `c`
重名）+ `event.myp` `if(dup)`→`if(dup!=0)`——归档恢复可重建（此前删 build/ 从零
构建会在 myp_rt_myp 步失败）。

- **测试**：`tests/@test/memreport.myp`（自环泄漏计数不回落 + memReport 输出含
  类名 + 非环对象释放回落）；端到端 `MYP_MEM_REPORT=1` 泄漏程序 stderr 报
  `Node: 1`。全量 451/0、parity 95/0、bootstrap 2 级 MD5 一致。

### v3.15.169 — ffi 形参校验设计统一（checkParamType 抽取复用）

**非破坏性**（selfhost sema 重构）。ffi 声明是「只登记不校验」路径（无 body、
不走 declareParam）——BUG-134 只补了 void 形参；本次把 `declareParam` 的形参
**校验**抽出为独立 `checkParamType`（void 参数 + 泛型类型实参实例化，纯校验不
声明符号），`declareParam`（常规路径）与 ffi 收集循环（只登记路径）都调用它，
保证所有声明点形参检查一致（单一校验源）。

- **修复**：`checkParamType(p)` 抽取 + `declareParam` 复用；ffi 收集循环改调
  `checkParamType`（每参）+ `checkDupParamNames`（重复形参名，BUG-106 常规检查
  镜像）——`ffi int cadd(int a, int a)` 新拒绝。
- **测试**：负测试 `ffi_dup_param.myp`；`ffi void 形参`、常规重复形参、有效 ffi
  均不受影响；bootstrap 自举成立。

### v3.15.168 — 修 ffi 声明 void 形参漏校验 → opt 崩（BUG-134）

**非破坏性**（selfhost sema）。`ffi int cadd(void v);`（ffi 声明 void 形参）
自举此前静默过 sema → codegen 发 `declare i32 @cadd(void)` → **opt-21 崩**
（void type only allowed for function results；oracle LLVM verify 也失败）。

- **根因**：ffi 收集循环只登记 funcNames_/funcRet_/funcParams_，不走
  declareParam（BUG-078 的 void 参数检查只在常规函数声明路径）→ ffi void 形参
  漏校验。
- **修复**：ffi 收集循环加 void 形参检查（镜像 BUG-078 判定：typeToKind=="void"
  && basicName=="void" && className 空）。
- **测试**：负测试 `tests/negative/ffi_void_param.myp`；有效 ffi 编译+运行正常；
  bootstrap 自举成立。

### v3.15.167 — 修 interface←class 转换未验实现接口 → opt 崩（BUG-133）

**非破坏性**（selfhost sema）。`IC ic = new NotImpl()`（类不实现接口）自举此前
typesCompat("interface","class") 无条件放行 → codegen 引用
`@__myp_vtable_IC_NotImpl` 未定义 → **opt-21 崩**。oracle 拒（4 处转换点：
var init / 赋值 / 实参 / 返回）。

- **修复**：`classImplementsIface`（类体 `interface class <Iface>;`）+ 
  `exprConcreteClass`（表达式具体类）+ `ifaceConversionOK` + `callIfaceParamName`
  （实参接口名解析），4 处转换点（var init / Assign / 实参 / Return）按类名
  校验接口实现；消息带具体接口名/类名（对齐 oracle）。
- **测试**：负测试 `tests/negative/iface_notimpl_assign.myp`；实现类 4 种转换
  编译+运行正确（v=42）；bootstrap 自举成立。

### v3.15.166 — 修事件成员访问/调用漏校验 → opt 崩（BUG-132）

**非破坏性**（selfhost sema）。`this.fire(...)` / `s.ev(...)`（事件作为成员
访问/调用）自举此前按方法解析 → codegen 发 `@<Cls>_<Ev>` 未定义 → **opt-21
崩**（use of undefined value）。oracle 拒 "cannot call expression"（即使实参
正确）。fire 应走裸名 `fire(...)`（事件成员访问/调用非法）。

- **修复**：Member 处理两处（Identifier 基座 / `this` 基座的 inClass_ 分支）加
  isEvent 检查 → "cannot call expression"；**await Class.event 专用路径须短路**
  （Await 处理含/不含 timeout 都跳过 visitExpr，否则无 timeout 落 Member 分支
  误拒）。
- **测试**：负测试 `tests/negative/event_member_call.myp`；`await Signal.go` 等
  coro/event 测试无回归；bootstrap 自举成立。

### v3.15.165 — 修事件 fire(...) 实参校验漏 → opt 崩/静默缺参（BUG-131）

**非破坏性**（selfhost sema）。`fire("x")`（事件 `fire(int v)` 实参类型不匹配）
自举此前静默过 sema → codegen `fire_T2_fire(ptr, i32 <string ptr>)` → **opt-21
崩**；`fire()`（缺参）静默接受。oracle 拒 "argument 1: expected 'int', got
'string'" / "expected 1 arguments, got 0"。

- **根因**：事件在 methods_ 以 0 参注册（mapping 裸名触发用）、isEvent 守卫跳过
  normalizeCallArgs → fire(...) 实参从不校验。
- **修复**：`eventParamsOf(cls, name)`（查 events() 真实参数）+ 类内未限定调用
  isEvent 分支加 fire 实参校验（数量 + 逐参 typesCompat 按 (形参, 实参) 序）。
- **测试**：负测试 `event_fire_arg_type.myp` + `event_fire_arg_count.myp`；
  `fire(42)` 编译+运行正确；bootstrap 自举成立。

### v3.15.164 — 修 mapping 源事件/目标 action 存在性漏校验（BUG-130）

**非破坏性**（selfhost sema）。`mapping() { Src.nonexist -> Dst.onOut; }`（源
事件不存在）自举此前**静默接受** → codegen findEventClass 空 → handler 不生成
→ no-op（oracle LLVM verify 失败）；`Src.output -> Dst.nonexist`（目标 action
不存在）→ codegen 发 @Dst_nonexist 未定义 → **opt-21 崩**（oracle 静默容忍）。

- **修复**：analyzeMapping 加存在性校验——节点 0（源）须为 src 类的事件；
  节点 1+（目标）isFunction → 顶层函数须存在、否则须为 src 类的 action。
  类不在 tu_.classes()（导入/泛型实例）跳过（保守不误报）。
- **测试**：负测试 `mapping_missing_source.myp` + `mapping_missing_target.myp`；
  有效 mapping 编译+运行正确；bootstrap 自举成立。

### v3.15.163 — 修带数据枚举变体裸引用漏校验 → 垃圾数据（BUG-129）

**非破坏性**（selfhost sema）。`Opt o = Opt.Some;`（带数据变体 `Some(int v)`
裸引用、无 `(data)`）自举此前静默接受 → 枚举值只有 tag、无 payload → 后续
match `Opt.Some(x)` 提取**垃圾数据**（曾得 x=1730215984）。oracle 把
`Opt.Some` 当 Function 类型 `(int)->unknown` 拒（应拒却接受）。

- **修复**：Member 处理枚举变体分支加数据 arity 检查——`enumVariantParamCount`
  > 0（带数据变体）且裸引用 → "enum variant 'X.Y' requires N data
  argument(s) (use 'X.Y(...)')"。Call 形式 `Opt.Some(5)` 走 Call 分支（不经
  此检查）；无数据变体 `Opt.None` 裸引用仍合法。
- **测试**：负测试 `tests/negative/enum_variant_bare_data.myp`；`Opt.Some(42)`
  match 得 42、`Opt.None` 正常；bootstrap 自举成立。

### v3.15.162 — 修嵌套 @parallel for 数据竞争（内层并行化 → 串行化）（BUG-128）

**非破坏性**（selfhost codegen）。`@parallel for` 嵌套在 `@parallel for` 体内
此前自举对内层也发 `myp_pool_parallel_for` → 共享全局 pool 的
work_fn/work_arg/barrier 被并发外层 worker 覆写 → 数据竞争、**非确定错误结果**
（total 应为 100 却得 11 / sum[0]=7 / 各 run 不同）。seed（build-cpu/mypc）用
emitKernelStmt 生成嵌套 for（只有最外层并行化），结果正确 100。

- **修复**：新增 `parallelDepth_` 守卫（genParallelBody 生成 body 期间 +1/-1）；
  `@parallel for` 且 `parallelDepth_>0`（已在并行 body 内）→ 走 `genSerialFor`
  串行化，不再发嵌套 pool 调用。
- **测试**：正测试 `tests/@test/parallel_nested.myp`（双层 + 三层混合嵌套，
  5 断言，3 次运行确定）；全量 441 通过 / 0 失败；oracle 对拍 95/0。
- **嵌套 @gpu for / 混合嵌套**：GPU kernel 生成失败（llc 报错）→ CPU 串行回退，
  结果正确（llc 噪音非致命）。

### v3.15.161 — 修 `this` 仅类 action 内合法漏校验 → opt 崩（BUG-127）

**非破坏性**（selfhost sema）。顶层函数里 `this.x`（非类/struct/lambda）自举
此前**接受** → codegen 对 %Object 值 GEP → **opt-21 崩**。oracle 干净拒
"'this' can only be used inside a class action"。

- **修复**：ThisExpr 处理加 `inClass_==0 && inStruct_==0` 检查（镜像 oracle
  visitThisExpr 双上下文标志）；struct 方法/类 action/lambda __call 放行。
- **测试**：负测试 `tests/negative/this_in_top_function.myp`；struct 方法 this
  与类 action this 编译+运行正确；bootstrap 自举成立。

### v3.15.160 — 修 nonlocal 仅 lambda body 内合法漏校验（BUG-126）

**非破坏性**（selfhost sema）。普通函数/类方法里 `nonlocal x;`（非 lambda
上下文）自举此前**接受**（应拒却接受）→ 静默忽略、语义无意义。oracle 拒
"'nonlocal' is only allowed inside a lambda body"。

- **修复**：新增 `inLambda_` 上下文标志（named lambda `__call` body 访问置位）+
  Nonlocal 语句处理加 `inLambda_==0` 检查；变量解析校验 lambda 创建时已有。
- **测试**：负测试 `tests/negative/nonlocal_outside_lambda.myp`；named lambda
  nonlocal 全 PASS（tests/@test/nonlocal.myp 5 断言）；bootstrap 自举成立。

### v3.15.159 — 修 match 枚举变体绑定 arity 漏校验（BUG-125）

**非破坏性**（selfhost sema）。`Opt.Some(x, y)`（变体 1 字段绑 2）/`Opt.Some(x)`
（变体 2 字段绑 1）自举此前**接受**（应拒却接受）→ 绑定截断/多余绑定未声明。
oracle 拒 "variant 'Some' expects 1 data fields, got 2"。

- **修复**：Match 枚举臂加绑定数量校验（绑定数 != 数据字段数 → 报错）；oracle
  报错后仍按 min 声明绑定并访问 body（防级联）。
- **测试**：负测试 `tests/negative/match_bind_arity.myp`；正确 arity 编译+运行
  正确；bootstrap 自举成立。

### v3.15.158 — 修实例化接口 new IC() 漏校验（BUG-124）

**非破坏性**（selfhost sema）。`IC ic = new IC()`（实例化接口）自举此前
**接受**（应拒却接受）→ 无实现类、无 vtable 的接口实例 → 运行时错。oracle 拒
"unknown class 'IC'"。

- **修复**：New 处理加接口检查——`inInterface(className)` → "cannot
  instantiate interface 'X'"；接口实例须 `new 实现类` 再转接口。
- **测试**：负测试 `tests/negative/new_interface.myp`；接口正常用法（new MyC →
  接口）编译+运行正确；bootstrap 自举成立。

### v3.15.157 — 修 struct == 比较无 @op("==") → opt 崩（BUG-123）

**非破坏性**（selfhost sema）。`Vec2 == 5` 与 `Vec2 == Vec2`（无 @op）自举此前
**接受** → codegen icmp 异构类型 → **opt-21 崩**（icmp requires integer
operands）。oracle 接受-垃圾或后端崩；manual 要求 struct 比较走 @op("==")。

- **修复**：`==`/`!=` 分支加 struct 检查（无 @op 匹配时）→ "struct comparison
  requires an '@op("==")' operator"。@op 匹配/class/interface 引用比较不受影响。
- **测试**：负测试 `tests/negative/struct_eq_no_op.myp`；@op 比较/引用比较编译
  +运行正确；bootstrap 自举成立。

### v3.15.156 — 修 `const` 局部变量 parser 误拒（BUG-122）

**非破坏性**（selfhost parser）。`const int x = 5;`（函数体/类 action/for-init
的 const 局部变量）自举此前 **parse 误拒**（"expected type"），oracle 接受
（const 局部当普通变量，可重赋值）。「拒合法代码」反向缺口。

- **修复**：parseVarDeclStmt 开头处理 `const` 前缀（isConst 检测 + 显式
  advance + v.setConst(1) 仅记录不强制）。只对 `const` advance、不对 `var`
  advance（var 由 parseType 消费，双消费会误拒 `var x = 5`）。
- **测试**：正测试 `tests/@test/const_local_var.myp`（4 断言）；var 声明/顶层
  const-decl 不受影响；bootstrap 自举成立。

### v3.15.155 — 修 @static 方法内使用 this → opt 崩（BUG-121）

**非破坏性**（selfhost sema）。`@static class S1 { static: int getK() {
return this.k; } }`——`this` 在 @static 方法内自举此前**接受** → codegen 访问
`%this.addr`（无实例）→ **opt-21 崩**（use of undefined value）。oracle 接受
但返回垃圾 0；静态状态应经 `Class.property` 访问。

- **修复**：新增 `inStatic_` 标志（@static action 循环置位），ThisExpr 处理
  `inStatic_!=0` → "cannot use 'this' inside a @static method" 干净拒绝。
- **测试**：负测试 `tests/negative/this_in_static.myp`；实例方法 this / @static
  类属性编译+运行正确；bootstrap 自举成立。

### v3.15.154 — 修 lambda 直调（正确计数）codegen 崩（BUG-120）

**非破坏性**（selfhost sema）。`(int x) => { return x + 1; } (5)`（lambda 直调
作语句）自举此前**接受** → codegen 生成 `call void @(...)`（空函数名）→
**opt-21 崩**（expected value token）。oracle codegen 拒 "cannot call
expression"（干净拒绝不崩）。

- **修复**：fallback 分支 lambda callee——计数不匹配仍报 BUG-103 消息；计数匹
  配改为拒绝 "cannot call expression"（镜像 oracle codegen 消息）。lambda 仅可
  作实参/赋给函数类型变量。
- **测试**：负测试 `tests/negative/lambda_direct_call.myp` /
  `lambda_direct_call_parallel.myp`；lambda 作实参/函数类型变量调用编译+运行
  正确；bootstrap 自举成立。

### v3.15.153 — 修泛型实例函数实参类型不匹配漏校验（BUG-119）

**非破坏性**（selfhost sema）。`id<string>(5)`（显式 type-arg string、实参
byte）自举此前**接受**（应拒却接受）→ 实参类型与替换后形参不匹配 → 运行时
垃圾。oracle 拒 "argument 1: expected 'string', got 'byte'"。

- **修复**：泛型函数调用 normalizeCallArgs 成功后加逐参类型校验（inst.params
  已替换具体类型，与实参 resolvedKind typesCompat）。
- **测试**：负测试 `tests/negative/generic_fn_arg_type.myp`；推导/正确显式形态
  编译+运行正确；bootstrap 自举成立。

### v3.15.152 — 修关系比较 < > <= >= 左操作数类型漏校验（BUG-118）

**非破坏性**（selfhost sema）。`a < 5`（a 为 struct，@op 不匹配）自举此前
**接受** → codegen 比较异构类型 → **opt-21 崩**（integer constant must have
integer type），oracle 拒 "expected numeric type"。

- **修复**：比较分支加 expectNumeric(lhs)（`<`/`>`/`<=`/`>=` 且 lhs 非
  string/bool/bit/bitvector/null/数字）；@op 匹配路径不受影响。算术分支已有、
  比较分支漏（不对称补齐）。
- **测试**：负测试 `tests/negative/relop_nonnumeric_lhs.myp`；@op 匹配/数字
  比较编译+运行正确；bootstrap 自举成立。

### v3.15.151 — 修 tuple 返回类型不匹配漏校验（BUG-117）

**非破坏性**（selfhost sema）。`(int, int) ret() { return (1, "a"); }` 自举
此前**接受** → codegen store 类型不匹配 → **opt-21 崩**（'%t2' defined with
type '{ i32, ptr }' but expected '{ i32, i32 }'），oracle 拒。

- **修复**：①新增字段 `currentRetAst_`（5 处声明点随 currentRet_ 同步设置）；
  ②Return 检查加 tuple 分支——`destructureTupleElems(s.value())` 与
  `currentRetAst_.funcParamTypes()` 比对 arity + 逐元素 typesCompat。消息与
  oracle 逐字一致。
- **测试**：负测试 `tests/negative/tuple_ret_type_mismatch.myp`；匹配形态编译
  +运行正确；bootstrap 自举成立。

### v3.15.150 — 修 tuple 变量初始化/赋值类型不匹配漏校验（BUG-116）

**非破坏性**（selfhost sema）。`(int, int) u = t`（t 为 `(int, string)` 元组
变量）与 `u = t;`（赋值语句）自举此前**接受** → codegen store 类型不匹配 →
**opt-21 崩**（'%t9'/'%t11' defined with type '{ i32, ptr }' but expected
'{ i32, i32 }'），oracle 拒。

- **修复**：①变量初始化——复用 `destructureTupleElems`（字面量/Identifier/Call
  三形态取元素 kind）与声明类型比对 arity + 逐元素 typesCompat；②赋值语句——
  `l==r=="tuple"` 时双端 destructureTupleElems 比较；新增 `tupleKindListName`
  显示名。消息与 oracle 逐字一致。
- **测试**：负测试 `tests/negative/tuple_var_type_mismatch.myp` /
  `tuple_assign_type_mismatch.myp`；匹配形态编译+运行正确；bootstrap 自举成立。

### v3.15.149 — 修枚举变体数据实参数漏校验（BUG-115）

**非破坏性**（selfhost sema）。`Opt.Some(1, 2)`（多参）与 `Opt.Some()`（少参）
自举此前**接受**（应拒却接受）→ 多余/缺失数据被忽略。oracle 拒 "expected 1
arguments, got 2/0"。

- **修复**：枚举变体构造分支加实参数校验（按变体 params().size() 与实参数比
  对）；无数据变体带参也拒（oracle 报 "not callable"，消息不同双端拒）。
- **测试**：负测试 `tests/negative/enum_variant_argc.myp` /
  `enum_variant_argc_few.myp`；`Opt.Some(5)` 正确；bootstrap 自举成立。

### v3.15.148 — 修 @gpu tile shared 数组名冲突 + block<dim 警告（BUG-114）

**非破坏性**（selfhost sema）。`@gpu tile (float[64] sm)` 与外层变量 `sm`
同名——自举此前静默覆盖外变量（应拒却接受）。oracle 当前源校验 "shared array
name 'X' already declared" + block 小于共享数组最大维度时 warning。

- **修复**：①声明前查外层同名 → error；②block < 最大维度 → diag_.warn
  （warning 记录到 dump 非阻塞，与 oracle 一致）。至此 oracle GPU 校验全镜像
  （reduce/scan/scatter/tile/for/resident/stream/grid/block/shared）。
- **测试**：负测试 `tests/negative/gpu_tile_shared_dup.myp`；正确 tile 编译
  正常；bootstrap 自举成立。

### v3.15.147 — 修 @gpu stream(s) 参数须 GpuStream 漏校验（BUG-113）

**非破坏性**（selfhost sema）。`@gpu for ... stream(s)`（s 为 int）自举此前
**接受**（应拒却接受）→ GPU 运行时错。oracle 当前源校验 "must be a
'GpuStream'"。

- **修复**：GpuFor 循环形 + GpuTile 两处 stream 访问加校验——Identifier 查
  `sym_.lookupClass(name)=="GpuStream"` / New 查 className；否则报错。
- **测试**：负测试 `tests/negative/gpu_stream_arg_type.myp`；stream(GpuStream)
  编译正常；bootstrap 自举成立。

### v3.15.146 — 修 stream/resident 仅限 @gpu for 归属漏校验（BUG-112）

**非破坏性**（selfhost sema）。普通 `for ... stream(s)` / `resident(...)`
（非 @gpu）此前自举**静默忽略**子句（应拒却接受）。oracle 当前源拒
"only valid on '@gpu for'"。

- **修复**：for 分支补两道——`s.gpu()==0 && stream!=null` / `resident 非空` →
  "'stream(...)' / 'resident(...)' is only valid on '@gpu for'"（block 已有
  归属检查、stream/resident 漏）。
- **测试**：负测试 `tests/negative/stream_not_gpufor.myp` /
  `resident_not_gpufor.myp`；@gpu for 带子句编译正常；bootstrap 自举成立。

### v3.15.145 — 修 @gpu tile shared 须数组类型漏校验（BUG-111）

**非破坏性**（selfhost sema）。`@gpu tile (float sm)`（标量 shared）自举此前
**接受**（应拒却接受）→ 静默当数组处理语义错。oracle 当前源拒 "requires an
array type"。

- **修复**：GpuTile 分支校验 `sharedType().element()==null` → "@gpu tile
  requires an array type (e.g. float[32][32])"；维度常量由 parser `float[n]`
  拒（不可达）；48KB 上限已有。
- **测试**：负测试 `tests/negative/gpu_tile_shared_nonarray.myp`；正确
  `float[64] sm` 编译正常；bootstrap 自举成立。

### v3.15.144 — 修 @gpu scatter 区间界类型漏校验（BUG-110）

**非破坏性**（selfhost sema）。`@gpu scatter(unique) a["x"..10)`（a 区间下界
string）自举此前**接受** → codegen 把 ptr 当 i64 索引 → **opt-21 崩**（constant
expression type mismatch）。oracle 当前源拒 "range bound must be an integer"。

- **修复**：GpuScatter 分支访问区间界前加 isNumKind 校验（aBegin/aEnd →
  range bound；idxBegin/idxEnd → index range bound）；失败 body markAll。
  冲突模式 parser 已限定。
- **测试**：负测试 `tests/negative/gpu_scatter_bound_type.myp` /
  `gpu_scatter_idx_bound_type.myp`；正确 scatter 编译正常；bootstrap 自举成立。

### v3.15.143 — 修 @gpu for/tile resident 子句校验漏（BUG-109）

**非破坏性**（selfhost sema）。`@gpu for ... resident(a = da)`（da 非 long
device 指针）与 `resident(b = db)`（b 未声明）此前自举**接受**（应拒却接受，
非 opt 崩）→ GPU 运行时错误。oracle 当前源校验 resident 子句。

- **修复**：镜像 oracle——GpuFor（for 循环形）与 GpuTile 两处遍历 resident
  子句：①数组名须在作用域且为数组；②device 名须存在且为 long。
- **测试**：负测试 `tests/negative/gpu_resident_dev_type.myp` /
  `gpu_resident_arr_missing.myp`；正确 resident 编译正常；bootstrap 自举成立。

### v3.15.142 — 修 @gpu tile grid(nb) 块数校验漏（BUG-108）

**非破坏性**（selfhost sema）。`@gpu tile ... grid("x")` 自举此前**接受** →
codegen 把 string 当 i64 → **opt-21 崩**（constant expression type
mismatch）；`grid(0)` 静默接受。oracle 当前源有类型+正数两道校验。

- **修复**：镜像 oracle——①gridExpr 非数字 → "grid must be an integer (block
  count)"；②Integer 字面量 ≤0 → "grid must be a positive block count"；③运行时
  表达式 → gridVal=-1 标记 host 求值（保持原行为）。
- **测试**：负测试 `tests/negative/gpu_tile_grid_type.myp` /
  `gpu_tile_grid_positive.myp`；grid(4) 编译正常；bootstrap 自举成立。

### v3.15.141 — 修 @gpu reduce/scan 声明式校验漏（BUG-107）

**非破坏性**（selfhost sema）。`@gpu reduce ... init "str"`（init 与元素类型
不匹配）自举此前**接受** → codegen 把 string 当 float 常量 → **opt-21 崩**
（constant expression type mismatch），oracle 当前源有全套校验。

- **修复**：镜像 oracle visitGpuReduceStmt/visitGpuScanStmt 六道校验——输入
  数组 T[]/元素类型 float/double/int/输出类型匹配/init 类型匹配/range 整型/
  op 体须含 return（GpuScatter 已有校验、reduce/scan 漏，补齐对称）。
- **测试**：负测试 `tests/negative/gpu_reduce_init_type.myp` /
  `gpu_reduce_op_noreturn.myp` / `gpu_scan_init_type.myp`；正确 reduce/scan
  编译正常；bootstrap 自举成立。

### v3.15.140 — 修重复形参名漏校验（BUG-106）

**非破坏性**（selfhost sema）。`int f(int a, int a)` 重复形参名自举此前**接受**
→ codegen 发 `define ...(i32 %a, i32 %a)`（LLVM 参数重名）→ **opt-21 崩**
（redefinition of argument '%a'）。oracle 容忍（last-wins），但重复形参名是
用户错误，自举干净拒绝。

- **修复**：新增 `checkDupParamNames` helper（StrHashMap 记录已见名），接入 5
  处形参声明循环（顶层函数/类 action/类 function/类 static action/struct
  方法），重名报 "duplicate parameter name 'X'"。
- **测试**：负测试 `tests/negative/dup_param_name.myp` / `dup_param_action.myp`；
  正常形参编译+运行正确；bootstrap 自举成立。

### v3.15.139 — 修一元 - / ~ 操作数类型漏校验（BUG-105）

**非破坏性**（selfhost sema）。`-"x"`（一元负号 string）与 `~d`（位取反
double/float）自举此前**接受** → codegen 生成 neg/not 非整型 → opt-21 崩
（integer constant must have integer type），oracle 拒。

- **修复**：Unary 分支补两道校验——①`-`（Negate）操作数须数字（isNumKind，
  镜像 expectNumeric），报 "expected numeric type, got 'X'"；②`~`（BitNot）
  须 bitvector/bit/整型（排除 float/double），报 "'~' requires an integer or
  bitvector operand (got 'X')"。
- **测试**：负测试 `tests/negative/unary_tilda_type.myp` / `unary_minus_type.myp`；
  `~int`/`-int`/`-double` 编译+运行正确；bootstrap 自举成立。

### v3.15.138 — 修三元分支类型不兼容漏校验（BUG-104）

**非破坏性**（selfhost sema）。`p ? 5 : "str"` 自举此前**接受并运行**（应拒却
接受）→ codegen 把 string 当 byte 存 → opt-21 崩（constant expression type
mismatch），oracle 拒。

- **修复**：visitTernary 非数值双分支加 `typesCompat(t, f)` 检查（镜像 oracle
  typesCompatible），不兼容报 "ternary branches have incompatible types: 'X'
  and 'Y'"。null 分支（string vs null）也拒——与 oracle 同文（typesCompatible
  只放行 null↔class）。
- **测试**：负测试 `tests/negative/ternary_branch_type.myp`；匹配/数值提升形态
  编译+运行正确；bootstrap 自举成立。

### v3.15.137 — 修函数类型实参数量漏校验（BUG-103）

**非破坏性**（selfhost sema）。`(int x)=>{...}(5,6)`（lambda 直调多参）自举
此前**接受** → codegen `call void @(i32 5, i32 6)` → **opt-21 崩**（expected
value token）；`f(5,6)`（f:(int)->int 函数类型变量多参）静默多参被忽略（应拒却
接受）。oracle 都拒 "expected 1 arguments, got 2"。

- **修复**：①函数类型变量调用分支——`fve.functionParamTypes()` 数量比对，不匹配
  报错；②lambda 直调 fallback 分支——用 `callee.params()` 数量比对，匹配时
  setCallParamTypes + setCallParamFuncSig 复用逐参类型校验。
- **测试**：负测试 `tests/negative/lambda_call_argc.myp` / `fntype_var_call_argc.myp`；
  正确调用形态仍编译+运行；lambda 直调作值表达式双端同拒；bootstrap 自举成立。

### v3.15.136 — 修 bitvector 比较/位运算/移位量同宽校验漏（BUG-102）

**非破坏性**（selfhost sema）。`bitvector<8> == bitvector<16>`、`bitvector<8> &
bitvector<16>`、`bitvector<8> << bitvector<16>` 自举此前**接受并运行**（应拒却
接受），oracle 当前源全部拒绝。

- **修复**：①新字段 `bitvectorWidths_`（bitvector 变量名→宽度）；②var 声明处
  记录宽度；③helper `bitvectorWidthOf`（Convert bitvector<N> 取节点 bw，
  Identifier 查映射）；④比较任一侧 bitvector 须双端同宽；⑤`&`/`|`/`^` 双端同宽；
  ⑥`<<`/`>>` 移位量为 bitvector 时须与左操作数同宽。
- **测试**：负测试 `tests/negative/bitvector_comp_width.myp` /
  `bitvector_bitwise_width.myp` / `bitvector_shift_width.myp`（EXPECT ERROR 子串）；
  同宽形态编译+运行正确；bootstrap 自举成立。

### v3.15.135 — 修 long→double / int↔long 构造器提升漏（BUG-101）

**非破坏性**（selfhost sema）。`double a = 1L;`（long→double 变量/实参）与
`new BoxD(1L)`（long→double 构造器）、`new BoxI(1L)`（int↔long 构造器）——oracle
均接受，自举此前**拒绝合法代码**。

- **修复**：①`promotesTo(long→double)`=1（64 位整数宽化到 double）；②
  `ctorArgCompat` promotesTo 失败时回落 typesCompat（覆盖 int↔long 双向）。
  歧义双构造器仍拒（自举 "no matching" / oracle "ambiguous"，双端拒）。
- **测试**：正测试 `tests/@test/ctor_promotion.myp`（2 tests / 8 断言）；
  `double a=1L`/`f(1L)`/`new BoxD(1L)`/`new BoxI(1L)` 编译+运行正确。
- 验证：bootstrap 自举成立；全量回归 **400 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-101。

### v3.15.134 — 修函数类型实参签名/裸函数名漏校验 → opt 崩（BUG-100）

**非破坏性**（selfhost sema + ast）。`apply(strFn, 5)`（形参 (int)->int 收
(string)->int）与 `apply(dbl, 5)`（裸函数名）此前自举放行 → codegen 非法闭包
IR → **opt-21 崩**（`defined with type 'i32' but expected '{ ptr, ptr }'`）。

- **修复**：①AstExpr 增 callParamFuncSig_（函数类型形参签名），4 处
  setCallParamTypes 同步设置；②arg-check：裸注册函数名作函数类型实参 → 拒
  `cannot use function name 'X' as a value; wrap it in a lambda`（自举 codegen
  不支持函数名→闭包）；函数类型变量/lambda → `argFuncSigOf` + `sigsMatch`
  签名比较（`argument 1: expected '(T) -> R', got ...`）。
- **测试**：负测试 `tests/negative/fntype_bare_fn.myp` +
  `tests/negative/fntype_var_mismatch.myp`；lambda/匹配函数类型变量编译+运行
  正确。
- 验证：bootstrap 自举成立；全量回归 **397 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-100。

### v3.15.133 — 修嵌套解构 arity 漏校验 → opt 崩（BUG-099）

**非破坏性**（selfhost sema）。`((int a,int b,int d),int c) = getNested()`
（内层值 2 元素绑 3 个）此前自举静默过 → codegen extractvalue 越界 → **opt-21
崩**（`invalid indices for extractvalue`）。C++ destructure walk 镜像。

- **修复**：新增 checkNestedDestructureArity（递归 walk：嵌套节点值须 tuple 且
  arity 一致；reported 防跨层重复），接入 Destructure 处理——Call rhs（嵌套
  AstType）与元组字面量 rhs（嵌套 expr）两形态；仅在顶层 arity 匹配后走。
- **测试**：负测试 `tests/negative/destructure_nested_arity.myp`；合法嵌套解构
  （tuple.myp 25 行）不受影响。
- 验证：bootstrap 自举成立；全量回归 **395 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-099。

### v3.15.132 — 修 `var x;` 无初始化器漏校验（BUG-098）

**非破坏性**（selfhost sema）。`var x;`（无初始化器）此前自举静默当 `int x = 0`
→ 语义错。C++ visitVarDecl 镜像 `'var' declaration requires an initializer`。

- **修复**：VarDecl 处理在推断前加校验（isInferred && init==null → 报错 +
  continue）。
- **测试**：负测试 `tests/negative/var_no_init.myp`；`var x = 5;`/`var s = "hi";`
  不受影响。
- 验证：bootstrap 自举成立；全量回归 **395 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-098。

### v3.15.131 — 修集合类缺 get/size 或元素是数组漏校验 → opt 崩（BUG-097）

**非破坏性**（selfhost sema）。`for (int x in ng)`（ng 是只有 size() 无 get(int)
的类）此前自举静默 → codegen 发 undefined `@NoGet_get` → **opt-21 崩**（`use of
undefined value '@NoGet_get'`）。集合 `int[] get(int)`（元素数组）也静默 → 循环
变量错配。C++ visitForInStmt 镜像。

- **修复**：ForIn class 路径显式校验 get(int)（1 参）+ size()（0 参），缺任一 →
  `'X' is not iterable: requires size() and get(int) methods`；get 返回 array →
  `cannot iterate a collection whose element is an array 'X'; wrap it in a class
  or use slice<T>`；新增 iterReported 抑制 generic 级联。
- **测试**：负测试 `tests/negative/forin_no_get.myp` +
  `tests/negative/forin_array_elem.myp`；ArrayList<int> 等合法集合 for-in 不受
  影响（for_in/collections_chain 回归 PASS）。
- 验证：bootstrap 自举成立；全量回归 **393 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-097。

### v3.15.130 — 修重复 interface 名漏校验（BUG-096）

**非破坏性**（selfhost sema）。两次 `interface I1 { ... }`（重复声明名）此前
自举静默 last-wins → 接口二义。C++ visitInterfaceDecl 镜像 `duplicate
interface name 'X'`（对比：重复 class/enum 自举已拒；重复 struct oracle 用
declared_struct_names_ 静默跳过、自举保持一致不报）。

- **修复**：interface 收集循环加重复检查（与已收集 interfaceNames_ 比对）。
- **测试**：负测试 `tests/negative/duplicate_interface.myp`；合法 interface
  （manual_ch6_class 回归）不受影响。
- 验证：bootstrap 自举成立；全量回归 **392 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-096。

### v3.15.129 — 修类内重复 action/event/function/struct 方法漏校验 → opt 崩（BUG-095）

**非破坏性**（selfhost sema）。`int go(){...} int go(){...}`（类内同名 action）
此前自举静默接受 → codegen 重定义同一 LLVM 函数 → **opt-21 崩**（`invalid
redefinition of function 'T2_go'`）。C++ visitClassDecl 镜像。

- **修复**：四处方法注册点加重复检查（methodSigIdx_ 同名即重）——类 action /
  类 function / 类 event / struct 方法；类+struct 构造器豁免（同名重载合法）。
- **测试**：负测试 `tests/negative/duplicate_action.myp` +
  `tests/negative/duplicate_event.myp`；构造器重载/合法 action 不受影响。
- 验证：bootstrap 自举成立（自举源码无重复）；全量回归 **390 通过 / 0 失败**；
  oracle 对拍 95/0。BUGLIST 记 BUG-095。

### v3.15.128 — 修 bitfield 重复名/字段漏校验（BUG-094）

**非破坏性**（selfhost sema）。`bitfield Flags { bit a; bit a; }`（重复字段）与
两次 `bitfield Flags { ... }`（重复声明名）此前自举静默 last-wins → 字段访问取
最后定义（位偏移错）→ 语义错。C++ declareBitfieldName/visitBitfieldDecl 镜像。

- **修复**：bitfield 收集循环加两道校验——重复声明名 → `duplicate bitfield
  name 'X'`；bitfield 内重复字段 → `duplicate bitfield field 'X' in 'Y'`。
- **测试**：负测试 `tests/negative/bitfield_dup_field.myp` +
  `tests/negative/bitfield_dup_name.myp`；合法 bitfield（bitfield.myp 回归）
  不受影响。
- 验证：bootstrap 自举成立；全量回归 **390 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-094。

### v3.15.127 — 修 var 推断元组 / 直接调用元组成员访问缺失（BUG-093）

**非破坏性**（selfhost sema）。`var r = pair()`（pair 返回 (int,bool)）后 `r.0`、
`pair().0` 直接成员访问——oracle 均支持，自举此前**拒绝合法代码**（var 推断元组
无 tupleTypes、成员访问只处理 Identifier 基）。

- **修复**：①var 推断元组分支：init 为 Call → findFuncRetType 取返回 tuple 的
  funcParamTypes → declareTuple（带元素类型）；②元组成员访问加 Call 基分支
  （越界报 tuple index N out of range）。tuple 字面量 var（`var tl=(7,false)`）
  codegen 布局未支持 → 保持回落（sema 拒，不崩），仅 Call 形态修复。
- **测试**：正测试 `tests/@test/tuple_var_infer.myp`（1 test / 6 断言）；
  `var r = pair()`/`pair().0`/`tri().1`/显式类型均编译运行正确。
- 验证：bootstrap 自举成立；全量回归 **388 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-093。

### v3.15.126 — 修 pipe 目标无 transform / lhs 类型不兼容漏校验（BUG-092）

**非破坏性**（selfhost sema）。`5 |> Foo`（目标类无 transform action）、
`5 |> Foo.helper`（MemberAccess 非算子）与 `5 |> ScaleOp`（transform(double[])
与 int lhs 不兼容）此前自举静默透传 lhs → 语义错。C++ visitPipe 镜像。

- **修复**：①目标类/实例无 1 参 transform action → `pipe '|>' requires an
  operator component with a single-argument 'transform' method`；②目标非类名/
  类实例（MemberAccess 等）→ 同一报错；③transform 找到时校验 lhs 与形参类型
  兼容 → `pipe: cannot apply 'X.transform' to operand of type 'Y'`。
- **测试**：负测试 `tests/negative/pipe_no_transform.myp` +
  `tests/negative/pipe_type_mismatch.myp`；`double[] A |> ScaleOp`/实例管道/链式
  不受影响（pipe 回归 PASS）。
- 验证：bootstrap 自举成立；全量回归 **385 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-092。

### v3.15.125 — 修内建实参校验缺失 → opt 崩（parse/位操作/bytes 族，BUG-091）

**非破坏性**（selfhost sema）。isNoVisitIntr 拦截名单内建（不 visit 实参）中
parse 族 / 位操作族 / bytes 族 / load4-store4 无实参校验 → 错参静默过 →
codegen 发非法内在签名 → **opt 崩**（`parseInt(123)`/`popcount(1.5)`/
`rotl(5,"x")`/`bytesOf("hi",2)`）。

- **修复**：①parse 族加「恰一实参 + string 类型」校验；②位操作族加实参数与
  整型校验（一元=1、rotl/rotr=2；移位量整型）；③bytes/bytesOf/str 加「恰一
  实参 + 类型匹配否则回落普通函数解析」（bytes←string、bytesOf←bitvector、
  str←数组）；④load4/store4 加 float[]/整型索引/float4 校验。
- **测试**：负测试 `tests/negative/parse_type.myp` + `bitop_type.myp` +
  `bytesof_args.myp`；parseInt("123")/popcount(255)/rotl(1,3)/bytes("s") 合法
  用编译+运行正确。
- 验证：bootstrap 自举成立；全量回归 **382 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-091。

### v3.15.124 — 修 slice 构造/成员调用参数漏校验（BUG-090）

**非破坏性**（selfhost sema）。`slice<int,int>` / `new slice<int>(4,5)` /
`new slice<string>("abc")` / `s.data(2)` 此前自举静默接受 → codegen 按单元素/
size 生成 → 垃圾/错配。C++ visitNewExpr + visitCall 镜像。

- **修复**：①New slice 加 3 项校验（恰一类型参数 / 恰一 size / size 为
  int/long/short/byte）；②slice 成员 .size/.length/.data（Identifier 基 +
  Call 返回 slice 基）加实参数为 0 校验。
- **测试**：负测试 `tests/negative/slice_type_args.myp` +
  `tests/negative/slice_size_args.myp`；`new slice<int>(3)`/`s.size()`/`s.data()`
  不受影响。
- 验证：bootstrap 自举成立；全量回归 **380 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-090。

### v3.15.123 — 修 for-in 变量类型不匹配漏校验（BUG-089）

**非破坏性**（selfhost sema）。`for (string s in int[2] arr)`（显式循环变量
类型与元素类型不匹配）此前自举静默接受 → 循环变量按 string 声明、迭代给 int →
codegen 存 int 入 string 槽 → 垃圾/opt 崩。C++ visitForInStmt 的
typesCompatible 校验镜像：`for-in variable type 'X' does not match element
type 'Y'`。

- **修复**：ForIn 声明循环变量前补校验——class 元素剥 "class:" 前缀比类名
  （同名 或 接口变量←元素类，inInterface 保守放行）；非 class 元素比 kind
  （同 kind 或 Int↔Long 或 bit↔bool）。
- **测试**：负测试 `tests/negative/forin_var_type.myp`；匹配/Int↔Long/范围
  for-in/同名类/接口变量均不受影响。
- 验证：bootstrap 自举成立；全量回归 **379 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-089。

### v3.15.122 — 修 non-void 函数缺 return 漏校验（BUG-088）

**非破坏性**（selfhost sema）。`int ret(){ int x=1; }`（non-void 函数体不保证
终止）与 `int bare(){ return; }`（裸 return 无值）此前自举静默接受 → codegen 发
`ret void` 于 i32 函数 → 运行时未定义。C++ 镜像：`missing return statement`
（checkMissingReturn + stmtGuaranteesTermination，空体 FFI 桩豁免）与
`missing return value`（visitReturnStmt 裸 return）。

- **修复**：①移植 stmtGuaranteesTermination（Return/Throw/Block 末语句/If 双
  分支/Match 全臂/Try 的 try 或 finally/while(true|非0字面量)/for(;;)）；
  ②checkMissingReturn 接 5 处方法体访问点（顶层函数/类 action/类 func/static
  action/struct 方法，仅非泛型 visitStmt 路径）；③Return 补无值分支校验。
- **测试**：负测试 `tests/negative/missing_return.myp` +
  `tests/negative/bare_return.myp`；末尾 return/if+else 双 return/while(true)/
  for(;;)/空体 FFI 桩均不受影响。
- 验证：bootstrap 自举成立（自举源码无 false positive）；全量回归
  **377 通过 / 0 失败**；oracle 对拍 95/0。BUGLIST 记 BUG-088。

### v3.15.121 — 修 throw 类型/裸重抛上下文漏校验 → opt 崩（BUG-087）

**非破坏性**（selfhost sema）。`throw 5`（非 string/类/接口表达式）此前自举
静默接受 → codegen 生成 `myp_throw_object(integer)` 非法 IR → **opt-21 崩**
（`integer constant must have integer type`）；catch 外裸 `throw;`（重抛）静默
接受 → 运行时未定义。C++ visitThrowStmt 镜像：非 string/类 → `throw requires
a string or class instance, got 'X'`；`throw;` 在 catch 外 → `'throw;' rethrow
is only valid inside a catch block`；void 表达式仅已报错时静默（级联恢复）。

- **修复**：①新增 `inCatchDepth_`（catch 块访问前 +1/后 -1，镜像 oracle
  in_catch_depth_）；②Throw 处理补类型/上下文校验——`throw;` 且
  inCatchDepth_==0 报错；非 string/class/interface 表达式报错（void 用
  errorCount() 判级联）。
- **测试**：负测试 `tests/negative/throw_type.myp` +
  `tests/negative/throw_rethrow_outside.myp`；`throw "msg"`/`throw e`/catch 内
  裸重抛（嵌套 try 传播）不受影响。
- 验证：bootstrap 自举成立；全量回归 **377 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-087。

### v3.15.120 — 修 for-in 迭代不可迭代对象漏校验 → opt 崩（BUG-086）

**非破坏性**（selfhost sema）。`for (int x in 5)` / `for (char c in strVar)`
（不可迭代对象）此前自举静默过 → 循环变量未声明 → codegen 找不到 → **opt 崩**
（`expected value token`）。C++ 镜像：不可迭代 → `cannot iterate over type 'X'`；
动态数组 → `cannot iterate a dynamic array ...; use slice<T> or a collection
class`。

- **修复**：ForIn 末尾补校验——动态数组标识符给专门消息；其余不可迭代给
  `cannot iterate over type 'X'`（resolvedKind void 跳过防级联）。
- **测试**：负测试 `tests/negative/forin_iterable.myp`；定长数组/slice/范围
  for-in 不受影响。
- 验证：bootstrap 自举成立；全量回归 **374 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-086。

### v3.15.119 — 修重复变量声明漏校验 + for 循环变量作用域（BUG-085）

**非破坏性**（selfhost sema）。`int x = 5; int x = 7;`（同作用域重复声明）此前
自举静默 last-wins shadow；C++ visitVarDecl 的 lookup 沿作用域链判重复（MYP 无
shadow 规则）。

- **修复**：①VarDecl 加重复检查（`_` 忽略符可重复）；②For 处理包
  enterScope/leaveScope——循环变量作用域弹出（镜像 oracle），顺序/循环后复用
  不误报、嵌套 shadow 正确拒。
- **防回归**：首版无 for 作用域，自举源码自身有顺序同名循环变量 → bootstrap 崩；
  正确解是 for 作用域（inForInit_ 豁免是 partial）。
- **测试**：负测试 `tests/negative/duplicate_var.myp`；顺序循环/循环后复用/_
  忽略符/@parallel for 均不受影响。
- 验证：bootstrap 自举成立；全量回归 **373 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-085。

### v3.15.118 — 修 checkedAdd/checkedMul 实参类型漏校验 → opt 崩（BUG-084）

**非破坏性**（selfhost sema）。`checkedAdd(1.5, 2.5)`（浮点实参）此前静默过
sema → codegen 发非法内在签名 → **opt 崩**（`invalid intrinsic signature`）。
C++ visitCheckedOp 校验实参为有符号整数（byte/short/int/long）。

- **根因**：自举 isNoVisitIntr 分支把 checkedAdd/checkedMul 直接设 ret="tuple"
  且不访问实参——实参类型无从校验。
- **修复**：checkedAdd/checkedMul 分支访问 2 实参并校验 byte/short/int/long，
  报 `checkedAdd expects signed integer arguments (int/long/byte/short)`；实参数
  !=2 报 `takes exactly two signed integer arguments`。
- **测试**：负测试 `tests/negative/checked_op_type.myp`；合法 checkedAdd/Mul
  不受影响。
- 验证：bootstrap 自举成立；全量回归 **372 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-084。

### v3.15.117 — 修 @async 非 @coro 上下文调用漏校验（应拒绝却接受）（BUG-083）

**非破坏性**（selfhost sema）。`@async` 方法/函数在非 @coro 上下文直接调用此前
自举静默当普通调用；C++ visitCall 报 `'@async' function can only be awaited
inside an '@coro' method`。

- **修复**：新增 `asyncFuncNames_`/`asyncMethodKeys_` 注册（仿 coroNames_）+ 助手
  isAsyncFunc/isAsyncMethod + checkAsyncCall + 三条调用路径接线（顶层函数/
  Member 方法/类内未限定裸调用）。
- **测试**：负测试 `tests/negative/async_method_outside_coro.myp`；@coro 内
  await 不受影响；async 系列测试全过。
- 验证：bootstrap 自举成立；全量回归 **371 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-083。

### v3.15.116 — 修 const 属性赋值漏校验（应拒绝却接受）（BUG-082）

**非破坏性**（selfhost sema）。`const int cap = 100; cap = 200;` 此前自举静默
改写（常量可变）；C++ 报 `cannot assign to const property 'X'`。

- **修复**：`classPropIsConst` + `assignTargetConst`（裸名/this.prop/同类实例
  .prop 三形态）+ Assign 处理报错。
- **防回归**：首版 obj.prop 分支未守卫 findClass(currentClass_)>=0 → struct 方法
  内 classProps_.get(-1) 越界 → **编译器段错误**（operators.myp 的 r.x_ 触发，
  exit 139）。重构提前算 ci 全局守卫。
- **测试**：负测试 `tests/negative/const_prop_assign.myp`；合法非 const 属性 +
  struct 字段赋值不受影响。
- 验证：bootstrap 自举成立；全量回归 **370 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-082。

### v3.15.115 — 修 catch 类型漏校验（应拒绝却接受）（BUG-081）

**非破坏性**（selfhost sema）。`catch (int e)`（int 非类/接口）此前自举静默
接受；C++ visitTryStmt 报 `catch type 'X' is not a class or interface`。

- **根因**：自举 Try 处理声明 catch 变量时不校验类型。
- **修复**：Try 处理加校验——catch 类型非空且非 string/类/接口/struct（自举
  超集保留）→ 报 `catch type 'X' is not a class or interface`。
- **测试**：负测试 `tests/negative/catch_type.myp`；合法 catch（兜底/string/
  类/接口）不受影响。
- 验证：bootstrap 自举成立；全量回归 **369 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-081。

### v3.15.114 — 修解构目标类型不匹配漏校验 → opt 崩（BUG-080）

**非破坏性**（selfhost sema）。`(int a, string b) = t`（t 是 (int,int) 元组变量）
此前静默过 sema → codegen string 槽存 int → **opt 崩**（`'%t11' defined with
type 'i32' but expected 'ptr'` 在 myp_retain 处）。C++ destructure walk 报
`destructure: variable 'b' declared as 'string' but element is 'int'`。

- **根因**：自举解构只在「元组字面量赋值形态」校验类型；标识符元组变量/调用
  返回元组/声明式解构都不校验。
- **修复**：新增 `destructureTupleElems`（三形态取元素 kind）+ `checkDestructureTypes`
  （递归 walk）+ `hasNestedDestructure` 守卫（嵌套保持旧行为）。
- **测试**：负测试 `tests/negative/destructure_type.myp`；合法声明式/赋值式解构 +
  嵌套解构不受影响。
- 验证：bootstrap 自举成立；全量回归 **368 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-080。

### v3.15.113 — 修 bitvector 移位量漏校验 → opt 崩（BUG-079）

**非破坏性**（selfhost sema）。`bitvector<8> v8; v8 << "x"`（string 移位量）
此前静默过 sema → codegen 把 ptr 当移位量 → **opt 崩**（`constant expression
type mismatch: got type 'ptr' but expected 'i8'`）。C++ visitBinaryOp 要求
bitvector 移位量是整数或 bitvector。

- **根因**：自举 bitvector 移位特殊分支只设标志返回 "bitvector"，不校验右操作
  数类型（比通用数字移位路径早 return，跳过 "expected numeric type" 检查）。
- **修复**：该分支加 `isNumKind(r)==0 && r!="bitvector"` → 报 `bitvector shift
  requires a bitvector left operand and an integer or bitvector shift amount`。
- **测试**：负测试 `tests/negative/bitvector_shift_type.myp`；合法 `v8 << 2` /
  `v8 << 1L` 不受影响。
- 验证：bootstrap 自举成立；全量回归 **367 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-079。

### v3.15.112 — 修 void 参数漏校验 → opt 崩（BUG-078）

**非破坏性**（selfhost sema）。`void take(void v)` 的 void 参数此前静默过 sema →
 codegen 发 `define internal void @T2_take(ptr %this, void %v)`（LLVM 非法）→
 **opt 崩**（`void type only allowed for function results`，非干净诊断）。C++
 sema.cpp 报 `cannot declare parameter of type 'void'`。

- **根因**：自举 `declareParam` 不校验 void 类型；字面 void 的 AstType
  basicName="void"（parser），typeToKind→"void"。
- **修复**：`declareParam` 加校验——typeToKind=="void" && basicName=="void" &&
  className 空 → 报 `cannot declare parameter of type 'void'`（未知类型
  className 非空不误伤）。
- **测试**：负测试 `tests/negative/void_param.myp`；合法方法参数不受影响。
- 验证：bootstrap 自举成立；全量回归 **366 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-078。

### v3.15.111 — 修 nonlocal 目标非外层变量漏校验 → opt 崩（BUG-077）

**非破坏性**（selfhost sema）。`nonlocal d`（d 是 lambda 参数）此前静默建 cell →
 codegen 取外层 cell 得 ptr 当 i32 → **opt 崩**（`'%t54' defined with type 'ptr'
but expected 'i32'`）。C++ capture 解析报 `nonlocal 'd' does not resolve to an
outer variable`。

- **根因**：自举 lambda 捕获收集对 `nonlocal name;` 只收集不校验——lambda 参数/
  内部局部不是外层变量，仍建 cell。
- **修复**：nonlocalNames 收集循环加校验——目标在 lambda 的 params/locals → 报
  `nonlocal 'X' does not resolve to an outer variable`；未声明 → 报 `nonlocal:
  undeclared variable 'X'`（镜像 visitNonlocalStmt）。
- **测试**：负测试 `tests/negative/nonlocal_param.myp`；合法 `nonlocal k`（外层
  变量，manual_ch5 counter()）不受影响。
- 验证：bootstrap 自举成立；全量回归 **365 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-077。

### v3.15.110 — 修 await timeout 类型漏校验（应拒绝却接受）（BUG-076）

**非破坏性**（selfhost sema）。`await T2.go timeout "x"`（string 当毫秒数）此前
静默过 sema；C++ visitAwaitExpr 对 timeout 双重校验（expectNumeric + `await
 timeout must be numeric (ms)`）。

- **根因**：自举 Await 有语句级（~3975，@coro 方法体实际路径）与表达式级
  （~5741）两处处理，都只 visit timeout 不校验类型。
- **修复**：两处都加 `isNumKind` 校验，报 `expected numeric type, got 'X'` +
  `await timeout must be numeric (ms)`（与 oracle 逐字节一致）。
- **测试**：负测试 `tests/negative/await_timeout_type.myp`；合法
  `await T2.go timeout 30` 不受影响。
- 验证：bootstrap 自举成立；全量回归 **364 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-076。

### v3.15.109 — 修非数组基址下标漏校验 → opt 崩（BUG-075）

**非破坏性**（selfhost sema）。`5[0]` / `d[0]` / `factory()[0]`（顶层函数返回
class）等非数组基址此前被静默当数组解析 → 赋值类型匹配时 codegen 对非指针做
GEP → **opt 崩**（`integer constant must have integer type` / `'%t16' defined
with type 'i32' but expected 'ptr'`）。C++ visitSubscript 仅允许
array/slice/string/bitvector 下标。

- **修复**：按基址形态检查——①顶层函数/内建 Call 用 `findFuncRetType` 判返回
  array/slice/string；②其余按 resolvedKind 严格判；③方法调用基址
  （`arrs.get(0)[0]`）resolvedKind 是元素 kind（传播模型怪癖）保守放行。
- **测试**：负测试 `tests/negative/subscript_nonarray.myp`；合法顶层函数返回
  array/slice/string 下标 + 链式 `arrs.get(0)[0]` 不受影响。
- 验证：bootstrap 自举成立；全量回归 **363 通过 / 0 失败**；oracle 对拍 95/0。
  BUGLIST 记 BUG-075。

### v3.15.108 — 修布尔上下文漏 expectBool 校验（应拒绝却接受）（BUG-074）

**非破坏性**（selfhost sema）。`p && q`（int 操作数）/ `!p` / `p ? a : b` /
`if (intExpr)` / `while (intExpr)` 此前静默当 bool（非零→true）；C++ oracle 四处
expectBool 只允许 bool/bit。手册 §三 要求整数判断写 `!= 0`。

- **修复**：①`&&`/`||` 操作数；②`!` 操作数；③三元条件；④if/while/for 条件——补
  expectBool（仅 bool/bit），报 `expected boolean expression, got 'X'`（消息用
  exprTypeName 解析类名）。
- **验证**：5 处均干净拒绝；合法 `b1 && !b2`（bit）/`if(boolVar)`/`while(b)`
  不受影响；stdlib/examples/mypview 无 truthy-int 依赖。
- **测试**：负测试 `tests/negative/bool_context.myp`；全量 **362 通过 / 0 失败**；
  oracle 对拍 95/0。BUGLIST 记 BUG-074。

### v3.15.107 — 修 new T[n] 数组大小漏校验 → opt 崩（BUG-073）

**非破坏性**（selfhost sema）。`new int["hi"]`（数组大小非整数）静默过 sema →
codegen 把 string 指针当 i64 大小 → **opt 崩**（`constant expression type
mismatch: got type 'ptr' but expected 'i64'`）。

- **根因**：自举 NewArray 处理不校验维度类型。
- **修复**：逐维度校验 `dk ∈ {int,long,short,byte}`（C++ visitNewArrayExpr
  镜像，不含 uint/float），否则报 `array size must be an integer expression`。
- **测试**：负测试 `tests/negative/array_size_type.myp`；合法 `new int[5]`/
  `new int[5L]` 不受影响。
- 验证：bootstrap 自举成立；全量回归 **132 @test PASS / 0 FAIL**；oracle 对拍
  95/0。BUGLIST 记 BUG-073。

### v3.15.106 — 修数组/字符串下标类型漏校验 → opt 崩（BUG-072）

**非破坏性**（selfhost sema）。`a["str"]`（string 下标）静默过 sema → codegen
把 string 指针当 i64 索引 → **opt 崩**（`invalid cast opcode for cast from
'ptr' to 'i64'`）。

- **根因**：自举 Subscript 处理不校验 index 类型。
- **修复**：Subscript 分支访问 index 后校验 `isNumKind(indexKind)`（C++
  expectNumeric 镜像，允许 byte/short/int/long/ubyte/ushort/uint/ulong/char/
  float/double），非数字报 `expected numeric type, got 'X'`。
- **测试**：负测试 `tests/negative/subscript_type.myp`；合法 `a[n]`/`a[1L]`/
  `s[1]`（string 下标 char）不受影响。
- 验证：bootstrap 自举成立；全量回归 **132 @test PASS / 0 FAIL**；oracle 对拍
  95/0。BUGLIST 记 BUG-072。

### v3.15.105 — 修显式转换 int(x) 非数字操作数漏校验 → opt 崩（BUG-071）

**非破坏性**（selfhost sema + parser）。`int(f)`（class 操作数）/`int(s)`
（string 操作数）静默过 sema → codegen 把对象/字符串指针当 i32 → **opt 崩**
（`%t9 defined with type 'ptr' but expected 'i32'`）；C++ `visitConvert` 校验
源/目标须数字或 bool（含 bit/bitvector/char）。

- **根因**：自举 Convert 处理完全不校验，只 setResolvedKind(target) 直接返回。
- **修复**：①sema 加 Convert 校验——源/目标 kind 须数字或 bool/bit/bitvector/
  char，否则报 `cannot convert 'X' to 'Y' (conversion operand and target must
  be numeric or bool)`；②parser 两处 Convert 节点（通用 + bitvector<N>）补
  `setPos`（此前 line/col=0，报错位置 0:0）。
- **测试**：负测试 `tests/negative/convert_nonnumeric.myp`；合法转换（数字/
  bool/bit/bitvector 互转）全过；bitvector 目录测试不受影响。
- 验证：bootstrap 自举成立（2 级 MD5 一致）；全量回归 **132 @test PASS / 0 FAIL**；
  oracle 对拍 95/0。BUGLIST 记 BUG-071。

### v3.15.104 — 修 bitcast 非数字操作数/源类型不匹配漏校验 → opt 崩（BUG-070）

**非破坏性**（selfhost sema）。内建 `bitcast<T,U>` 校验缺口：
`bitcast<int>(s)`（string 操作数，非数字）静默过 sema → codegen 发射
`bitcast ptr to i32`（ptr 64 位 vs i32 32 位，LLVM 非法 cast）→ **opt 崩**
（`invalid cast opcode for cast from 'ptr' to 'i32'`）；`bitcast<float,int>(x)`
（显式源 float 与 int 操作数不匹配）同样静默。

- **根因**：自举 bitcast 只查宽度 `if (sw != 0 && tw != 0 && sw != tw)`——非数字
  源（string/class）`bitcastWidth→0` 时条件为假 → 不报错放行；缺显式源类型与
  操作数兼容检查。
- **修复**：镜像 C++ visitBitcast 三段——①显式源与操作数 `typesCompat` 不匹配报
  `bitcast source type 'X' does not match operand type 'Y'`；②`sw==0 || tw==0`
  报 `bitcast requires numeric source and target types (integer/float/char)`；
  ③同宽检查保留。
- **测试**：负测试 `tests/negative/bitcast_numeric.myp`；合法 bitcast 位保持
  正确（int/long→double/int→float 全通）。
- 验证：bootstrap 自举成立（2 级 MD5 一致）；全量回归 **132 @test PASS / 0 FAIL**；
  oracle 对拍 95/0。BUGLIST 记 BUG-070。

### v3.15.103 — 修泛型 static 调用缺实参校验 → opt 崩（BUG-069）

**非破坏性**（selfhost sema）。续调用路径实参校验审查：`List.foldInt<int>(arr,
0)`（漏 fn 实参）与 `List.foldInt<int>(arr, "str", ...)`（string 实参 vs int
形参）静默过 sema → 错参 codegen 实参类型错（ptr 当 i32）→ **opt 崩**；C++
oracle 三处全拒。

- **根因**：`resolveGenericStaticCall`（自举）只校验类型实参个数/推导，完全不
  校验值实参数量与类型；C++ 同名函数对替换后的实例签名逐参 typesCompatible。
- **修复**：`resolveGenericStaticCall` 末尾加实参校验——`normalizeCallArgs` 管
  数量/默认/命名；逐参 `substituteType(param.type, tps, concrete)` 得替换后
  kind 与实参 resolvedKind 比（lambda 实参 → "function"，void/assoc 跳过）。
- **测试**：负测试 `tests/negative/generic_static_arg_mismatch.myp`；合法
  foldInt/map 调用不受影响。
- **已知限制（非本次）**：函数类型形参的签名比较（`(int) -> int` vs
  `(string) -> int`）自举在所有路径都不做（kind 均 "function"）——oracle 能报
  `expected '(int) -> int', got '(string) -> int'`。非崩溃缺口，另立待办。
- 验证：bootstrap 自举成立（2 级 MD5 一致）；全量回归 **132 @test PASS / 0 FAIL**；
  oracle 对拍 95/0。BUGLIST 记 BUG-069。

### v3.15.102 — 修接口变量方法缺实参类型校验（静默错参）（BUG-068）

**非破坏性**（selfhost sema）。`IBox ib = new IntBox(); ib.put("str")`（`put(int)`
的 string 实参）自举编译+运行通过（静默错参，ptr 截断成 i32）；oracle 干净拒绝
`argument 1: expected 'int', got 'string'`。

- **根因**：接口方法注册在 `interfaceMethods_`（不在 `methods_`/`methodSigIdx_`）
  → Member 分支 `findMethodParams` 返回 null → `callParamTypes` 不设 → 实参检查
  被跳过。
- **修复**：新增 `findIfaceMethodParams`；Member 分支 `anyMps` 为空时回退接口
  形参，走同一 normalizeCallArgs + setCallParamTypes 校验路径。
- **测试**：负测试 `tests/negative/iface_arg_type_mismatch.myp`；合法接口调用 +
  多实现类分派不受影响。
- 验证：bootstrap 自举成立；全量回归 **132 @test PASS / 0 FAIL**；oracle 对拍
  95/0。BUGLIST 记 BUG-068。

### v3.15.101 — 修泛型实例方法缺实参类型校验 → opt 崩（BUG-067）

**非破坏性**（selfhost sema）。续 BUG-066 探「oracle 有校验、自举漏」的调用
路径缺口：`ai.add("str")`（`ArrayList<int>` 的 add，string 实参 vs int 形参）
静默过 sema → codegen 发射 `i32 getelementptr(ptr @str, ...)`（ptr 当 i32）→
**opt 崩**（`constant expression type mismatch: got type 'ptr' but expected
'i32'`，非干净诊断）。

- **根因**：泛型实例方法经 `resolveBase(instCls)` 回落到**模板**类取形参——模板
  形参占位符 `T` 不在 curGeneric_ 时 `typeToKind(T)` → `"void"` → 实参类型检查的
  `anyVoidParam` 守卫整段跳过。Member 分支虽早设 `setCallParamTypes`，kind 是
  占位符（void），检查不生效。
- **修复**：新增 `substParamKinds`（形参 T → `typeToKind(targs[j])`）+ 
  `instTypeArgsOfObject`（镜像 BUG-062 三形态：Identifier 变量/裸属性/链式结果），
  Member 分支 `setCallParamTypes` 用替换后的 kind。**关联类型守卫**：`check(T c,
  T::Item v)` 参数 2 替换后仍 `assoc`（T::Item 未解析）→ 纳入 `anyVoidParam`
  守卫跳过，避免误报（manual_ch6_class/assoc_types 回归暴露）。
- **测试**：负测试 `tests/negative/generic_arg_type_mismatch.myp`；正测试
  `tests/@test/generic_arg_ok.myp`（1 测试/7 断言，双编译器 7/7）。诊断文本/位置
  与 oracle 逐字节一致（test_myp_self 对拍 95/0）。
- 验证：bootstrap 自举成立（2 级 MD5 一致）；全量回归 **132 @test PASS / 0 FAIL**。
  BUGLIST 记 BUG-067。

### v3.15.100 — 修类内未限定方法调用缺实参类型校验 → opt 崩（BUG-066）

**非破坏性**（selfhost sema）。「缺失编译期校验 → codegen 崩」族（BUG-007/008/
016/046/050/054 模式）续查：赋值/返回/数组元素读写校验已全面，但**类内未限定
方法调用实参**漏检——`takeInt("hello")`（string 实参 vs int 形参）静默过 sema →
codegen 发射 `i32 getelementptr(ptr @str, ...)`（ptr 当 i32）→ **opt 崩**
（`constant expression type mismatch: got type 'ptr' but expected 'i32'`，
非干净诊断）。C++ oracle 干净拒绝 `argument 1: expected 'int', got 'string'`。

- **根因**：Identifier-callee 的 `inClass_ != 0` 未限定方法调用路径只设
  ret/resolvedClass + fillDefaultArgs，**漏 `normalizeCallArgs` +
  `setCallParamTypes`** → 后续实参类型检查（`callParamTypes()` 非空才跑）被跳过。
  Member 分支（`obj.method()`）早有该检查 → 仅类内裸名调用漏网。
  Member-callee 的 `else if inClass_` 回退路径同样漏。
- **修复**：两条路径镜像 Member 分支补校验。**事件守卫**：事件（`event:` 段）
  在 `methods_` 里以 **0 参数**注册（mapping 触发用裸名 `emit(v)`，实参另处
  处理）——须 `isEvent(cls, name)` 守卫跳过，否则误报 "call takes no
  arguments, but 1 given"（C++ oracle 接受裸名事件触发）。
- **测试**：负测试 `tests/negative/arg_type_mismatch.myp`（EXPECT ERROR
  argument 1: expected 'int', got 'string'）；`where_mapping`（裸名事件触发）
  不受影响。诊断文本/位置与 oracle 逐字节一致（test_myp_self 对拍 95/0）。
- 验证：bootstrap 自举成立（2 级 MD5 一致）；全量回归 131 @test PASS / 0 FAIL。
  BUGLIST 记 BUG-066。

### v3.15.99 — 重构 interface upcast 具体类名解析为统一辅助函数

**非破坏性**（selfhost codegen 重构，无行为变化）。`upcastClsName`（BUG-029/033/
034/064/065 累积）长成 ~150 行多分支、每分支重复「取类型→resolve→判类→
classInstName」。按「源表达式形态 × 具体类名解析」抽成统一助手：

- `isConcreteClassKind(cn)`：统一判定非接口/struct/枚举。
- `exprAstTypeOf(e)`：按形态递归取声明/返回 AstType——Identifier（局部/参数/
  当前类字段裸名）、Member（this.field / struct 变量字段 / 链式结果 .字段，
  对象递归 + sema valueClass）、Subscript（数组/slice 元素，数组表达式递归）。
- `upcastClsName(e)`：统一入口——① 表达式自带 valueClass/resolvedClass（sema
  已设，覆盖 Call 返回/New/链式结果）；② New 类名；③ exprAstTypeOf 解析。
- 结果：~150 行 → ~85 行，形态枚举收敛一处；后续新增形态只需改 exprAstTypeOf。
- 验证：bootstrap 自举成立；全量回归 **353/0**（含 iface_upcast_chain 8 测试、
  collections/链式/2D 全回归）。

### v3.15.98 — 修 interface 转换从链式结果 .字段 漏具体类名（BUG-065）

**非破坏性**（selfhost codegen）。修完 BUG-064 后继续探 interface 转换族：接口参数
从 call 结果、返回 call 结果原已好；`Shape s = hs.get(0).c;`（集合 get 结果的
struct 字段 → interface）段错误（vtable 槽 null）。

- **根因**：`upcastClsName` Member 分支只处理对象是 Identifier（struct 变量）与
  this.field，对象是 Call（`hs.get(0).c`）时不解析字段具体类。
- **修复**：Member 分支对象加 Call/Subscript/Member/New——valueClass/resolvedClass
  取返回类型，再按 struct 字段/class 属性解析字段具体类。
- **测试**：`tests/@test/iface_upcast_chain.myp` 增补 test_coll_get_field /
  test_iface_param / test_iface_ret（8 测试/10 断言）。
- 验证：bootstrap 自举成立；全量回归 **353/0**。BUGLIST 记 BUG-065。

### v3.15.97 — 修 interface 转换从链式结果/struct 字段漏具体类名（BUG-064）

**非破坏性**（selfhost codegen）。review BUGLIST 找相似未修复：表里 4 个非 🟩
（029/032/046/051）详情实为已修复（表标记过时 U+FFFD，已修正）；按 interface 转换族
（BUG-029/033/034）探针发现真缺口——`Shape s = factory()`（顶层函数返回）、
`b.make()`（方法返回）、`cs.get(0)`（集合 get）、`h.c`（struct 字段）→ interface 全
段错误（vtable 槽 null）。

- **根因**：codegen `upcastClsName` 只处理 New/Identifier/this.field/Subscript，
  漏 Call（函数/方法/集合 get 返回）与 struct 变量 .字段 → 具体类名空 → 不存
  vtable → 派发段错误。
- **修复**：upcastClsName 加 Call 分支（用 sema BUG-057/062 已设的
  valueClass/resolvedClass）+ Member 分支（struct 变量字段经 structFieldAstType）。
- **测试**：`tests/@test/iface_upcast_chain.myp`（5 测试/6 断言：new/局部/函数
  返回/方法返回/集合 get/struct 字段）。
- 验证：bootstrap 自举成立；全量回归 **353/0**。BUGLIST 记 BUG-064 + 修 4 个过时
  状态标记。

### v3.15.96 — 修调用结果成员后下标 get().field[j]（BUG-063）

**非破坏性**（selfhost sema）。续链式访问族排查（BUG-057~062 后系统探测更多形态）：
多级方法链/元素类方法/三级集合链/构造链原本就过；`ps.get(0).data[1]`（调用结果
`.struct数组字段` 再下标）与 `bs.get(0).arr()[1]`（调用结果 `.方法返回数组` 再下标）
报 `with value of type 'array'`。

- **根因**：sema Subscript 的 Member 分支只处理对象是 `Identifier`，对象是 `Call`
  （`ps.get(0).data`）时不解析字段元素类型 → `et` 停留 "array"；slice 字段同样漏。
- **修复**：Member 分支对象加 `Call/Subscript/Member/New` 情况——用
  `valueClass()/resolvedClass()` 取类名，再查 class 属性/struct 字段元素（数组
  element + slice typeArgs + 嵌套 slice 深层元素）。
- **测试**：`tests/@test/collections_chain.myp` 增补 test_member_subscript /
  test_method_subscript（7 测试/20 断言）。
- 验证：bootstrap 自举成立；全量回归 **352/0**。BUGLIST 记 BUG-063。

### v3.15.95 — 修泛型集合方法返回链式访问（BUG-062）

**非破坏性**（selfhost sema）。用户问"collections 有没有此问题"（链式访问族）。
探针发现三个坏形态：`ArrayList<int[]>.get(i)[j]`、`ArrayList<slice<int>>.get(i)[j]`
（元素类型未传播，报 'array'）；`ArrayList<ArrayList<int>>.get(0).get(1)`（分派到
未实例化模板 `@ArrayList_get` 返回 ptr → opt 崩）。

- **根因（泛型方法返回 T 的替换不完整）**：
  1. 下标 Call-Member 分支取原始返回 `T`（无 element/typeArgs）→ 元素取不到。
  2. Call 泛型方法返回替换用 `substRet`（`SymbolEntry.instArgs` **拍平字符串**，
     只存 className 丢 typeArgs）→ 返回替换成基类名 → 链式 `.get()` 分派错。
- **修复**：下标分支用 `findInstTypeArgs`+`substituteType` 替换类型参数取元素；
  泛型方法返回替换优先用 `findInstTypeArgs(实例类名)`+`substRetAst`（完整 AstType
  实参）。
- **设计确认**：`HashMap<K,V>` 是整型键（`%` 哈希），字符串键用
  `StrHashMap<string,V>`（DJB2）。
- **测试**：`tests/@test/collections_chain.myp`（5 测试/14 断言：struct/map/数组
  元素/slice 元素/嵌套集合链）。
- 验证：bootstrap 自举成立；全量回归 **352/0**。BUGLIST 记 BUG-062。

### v3.15.94 — 修调用结果嵌套 slice 双下标 make2d()[i][j]（BUG-061）

**非破坏性**（selfhost sema）。用户追问 2D 数据访问：MYP 的 2D 是**嵌套 slice**
（`slice<slice<int>>`），非 `int[][]`。`make2d(4)[2][3]`（函数返回 2D + 链式双下标）
报 `expected numeric type, got 'array'`。

- **根因**：sema Subscript 的 Call 分支解析 `slice<slice<int>>` 返回时元素类型
  （`slice<int>`）→ "slice"，但没记 `setSliceElem`（深层元素 int）→ 二级下标
  `(make2d(4)[2])[3]` 取 `sa.sliceElem()` 为空 → 停留 "array"。变量版走
  `en.elementElem()` 正常，调用结果版漏。
- **修复**：sema Call 分支（Identifier + Member callee 两处）在元素为 slice 且
  `el.typeArgs().size()>0` 时 `e.setSliceElem(深层元素)`。
- **测试**：`tests/@test/slice_2d_chain.myp`（5 测试/12 断言：链式双下标/算术/
  单下标取行/方法返回 2D/变量对照）。
- 验证：bootstrap 自举成立；全量回归 **351/0**。BUGLIST 记 BUG-061。

### v3.15.93 — 修表达式 try 类型检查缺失 + catch 值转换块位错（BUG-060）

**非破坏性**（selfhost sema + codegen）。补表达式 try（`var n = try expr catch (e)
default;`）@test 时暴露两处：

- **060a**：`int a = try risky(5) catch (e) "oops";`（int vs string）自举静默过 sema
  （C++ visitTryExpr 有 typesCompatible 检查）→ codegen PHI i32/ptr 类型不匹配 → opt
  崩。修复：sema TryExpr 加 `typesCompat` 检查，报 `try/catch expressions have
  incompatible types` 干净拒绝。
- **060b**：`long v = try risky(5) catch (e) -1L;`（int/long 数字提升合法）→ opt
  verify "input module is broken"。根因：genTryExpr 把 catch 值转换
  （convertValue/trunc）放在 `openBlock(merge)` 之后 → trunc 落进 merge 块、phi 前
  → 违反「phi 必须在块首」+「指令支配」。修复：转换移到 catch 块、`emitBr(merge)` 前。
- **测试**：正 `tests/@test/expr_try.myp`（5 测试/9 断言：成功值/失败默认值/算术/
  数字提升/作实参/嵌套）；负 `tests/negative/expr_try_type_mismatch.myp`。
- 注：表达式 try 的 catch 变量是**占位符**，不在 catch 表达式绑定（C++ oracle 与
  自举一致）。
- 验证：bootstrap 自举成立；全量回归 **346/0**。BUGLIST 记 BUG-060。

### v3.15.92 — 修调用结果 slice 链式访问（下标/size 未解析）（BUG-059）

**非破坏性**（selfhost sema + codegen）。续 BUG-058 排查姊妹缺口：`makeSlice()`
返回 `slice<int>` 后 `makeSlice()[0]` 报 "int with array"、`makeSlice().size()` 报
"int with void"、`makeStrSlice()[0]`、`b.get()[0]`（方法返回 slice）全坏。

- **根因（slice 用 typeArgs 存元素，非 element()；多处只处理 Identifier）**：
  1. sema Subscript 的 Call 分支从返回 AstType 取 `element()`——slice 元素在
     `typeArgs` → 只修了数组没修 slice；Member-callee 分支同理。
  2. sema Call-Member-callee：slice 内建 `.size()/.length()/.data()` 只处理 slice
     变量（Identifier），slice 返回调用（Call）漏 → void。
  3. codegen：`subscriptElemLt` Call 分支补 slice 元素 LLVM 类型；`genCall` slice
     `.size()/.data()` 只处理 Identifier → Call 返回 slice 生成 `Object_size(ptr
     {ptr,i64})` 类型不匹配。
- **修复**：sema 两处 Call 分支（Identifier/Member callee）补 slice(typeArgs) 元素 +
  Call-Member-callee 补 slice 返回调用的 size/length/data；codegen 对应两处补
  slice 元素 LLVM 类型 + slice 返回调用的 size/data extractvalue。
- **测试**：`tests/@test/slice_call_chain.myp`（4 测试/11 断言：slice<int>/<string>
  返回下标、size/length、方法返回 slice 下标+size、下标算术）。
- 验证：bootstrap 自举成立；全量回归 **344/0**。BUGLIST 记 BUG-059。

### v3.15.91 — 修调用结果下标 f()[i] 元素类型丢失（BUG-058，BUG-057 姊妹）

**非破坏性**（selfhost sema + codegen）。测试缺口审计发现 `f()[i]`（函数/方法返回
`T[]` 后直接下标）@test 零覆盖，实测 `makeArr()[1]` 报 `expected numeric type, got
'array'`——返回数组的元素类型没传到 Subscript。

- **根因（两处）**：
  1. sema：Subscript 解析 `sa.kind()=="Call"` 分支只处理 `bytesOf`→ubyte，其它返回
     数组的调用不取元素类型 → `et` 停留 "array"。
  2. codegen：`subscriptElemLt` 对 Call 默认 "i32" → `string[]`/对象[] `load i32`
     后 `myp_retain(ptr)` 收到 i32 → LLVM 类型不匹配（opt 崩）。
- **修复**：sema 从返回 AstType 取 element（顶层函数 `findFuncRetType` / 方法
  `findMethodRetAst`，Member callee 用其 resolvedClass）；codegen `subscriptElemLt`
  补 Call 分支（`findFuncRetAstType`/`methodRetAstType` + `llvmType(element())`）。
- **测试**：`tests/@test/call_subscript_chain.myp`（5 测试/12 断言：int/double/string
  元素、方法链 `b.get()[i]`、`f()[i]` 算术、写路径）。
- 验证：bootstrap 自举成立；全量回归 **343/0**。BUGLIST 记 BUG-058。

### v3.15.90 — 修顶层函数调用结果链式成员访问回落当前类（BUG-057）

**非破坏性**（selfhost sema）。排查"泛型链式访问"发现：`rawStep(5).get()`（顶层函数
返回 `Result<int,string>` 后取成员）报 `class 'T2' has no member 'get'`——成员查找
**回落到当前类**；隔离发现不止泛型，`makeErr().message()`（具体类）、struct/interface
返回的链式访问全受影响。

- **根因**（sema.myp Call 解析）：普通顶层函数调用 `findFuncRet(fn)` 只返回返回
  **kind**，不把返回类名记到 CallExpr 的 `valueClass`/`resolvedClass`。而 Member 访问
  （对象为 Call）用 `arr.valueClass()`→`resolvedClass()` 取类名，都空 → 回落到当前类
  "class 'T2' has no member"。仅泛型显式实参/类 action/`new` 三条路径设了 → 不一致。
- **修复**：`findFuncRet` 命中且 kind 为 class/struct/interface 时，用
  `findFuncRetType`（返回 AstType）+ `gsRetValueClass`（取类名/泛型实例名）设
  `e.setValueClass`。对齐其它三条路径。
- **测试**：`tests/@test/call_result_chain.myp`（6 测试/12 断言：Result 返回
  `.get()/.isErr()/.getErr()/.getOr()` / 具体类 `.message()` / struct `.x/.y` /
  interface `.area()` 接口分发 / 两层链 / lambda 内）。
- 边界：`strVal().len()` 仍失败是设计使然（MYP 字符串值无 `.len()` 成员，须
  `Str.len(s)`），非本 bug。
- 验证：bootstrap 自举成立；全量回归 **342/0**。BUGLIST 记 BUG-057。

### v3.15.89 — 嵌套泛型实例化 O(N³) 时间爆炸守卫（BUG-056）+ 全嵌套守卫审计

**非破坏性**（selfhost sema 健壮性）。"审查多层嵌套是否都有守护"审计：表达式/语句/
括号/宏都有守卫（BUG-055），但**类型级嵌套泛型** `Box<Box<...<int>...>>` 的 sema
实例化路径无守卫——不是栈溢出，而是 **O(N³) 时间爆炸（DoS）**。

- **审计结论**（全部嵌套路径逐项实测，`ulimit -v` + `timeout`）：
  - ✅ 表达式（三元/`??`/赋值/一元）：`exprDepth_` 300 守卫（BUG-055）。
  - ✅ 语句（块/if/while）：`stmtDepth_` 300 守卫（BUG-055）；括号 `recursionDepth_`。
  - ✅ 嵌套元组类型 `((int,int),int)`：走表达式路径 → `expression nested too deeply`。
  - ✅ 嵌套函数类型：解析干净报错（无崩溃）；解析器嵌套泛型 50000 层**无栈溢出**
    （0.104s 平坦）——parseType 递归安全。
  - ✅ 嵌套数组字面量 / 宏展开（depth>100）：均有守卫。
  - ⚠️ **嵌套泛型 sema 实例化无守卫** → 本次修复。
- **根因**（O(N³)，sema.myp）：`SymbolTable.lookup` 线性扫描（entries_ 已 N 项 ×
  长名比较）+ `instClassName` 递归拼名（每层 O(k²)）→ 实例化总 O(N³)。实测
  depth 800 >25s、5000 不可完成；纯解析路径 0.1s 平坦 → 爆炸全在 sema。
- **修复**：`typeArgDepth(AstType)` O(N) 单遍测深 + `tryInstantiate` 入口顶层检查，
  `>64` → 单条 `generic type nested too deeply (N > 64)` 干净拒绝；`instDepth_`
  计数器作纵深防御。合法嵌套 ≤4 层，64 极其宽松。depth 200/500/5000 → 0.1~0.3s
  单条错误（原 7.5s/25s+/小时级）。
- **测试**：负例 `tests/negative/generic_nested_deep.myp`（70 层）；torture 新增
  `deep/generic_*`（+8 → 168 全过）；正向 `tests/@test/nested_generic.myp`
  （2/4/10 层声明+构造）；`test_myp_viz.sh` 对拍排除 torture/generated 病态文件
  （自举带守卫 vs oracle 无守卫，预期分歧）。
- 验证：bootstrap 自举成立（MD5 一致）；全量回归 **339/0**（+1 正向、+1 负例）；
  torture compile 40×3 + deep 56 + execute 72 = 168 全过。BUGLIST 记 BUG-056。

### v3.15.88 — 修解析器表达式/语句级递归无守卫（深嵌套栈溢出）+ torture deep 压测

**非破坏性**（selfhost parser 健壮性）。用户要求"上千层嵌套括号/嵌套条件表达式/嵌套
宏展开，专门搞栈溢出"压测——暴露 `recursionDepth_` 只数 `parsePrimary`（括号 300
守卫），而**三元 / `??` 合并 / 右结合赋值 / 一元链 / 嵌套块 / if 链**的右嵌套递归
都在 parsePrimary 之上、不经 parsePrimary → 50000 层 SIGSEGV 栈溢出。

- **修复**（parser.myp，三个独立计数器均 300 守卫 + 跳过恢复）：
  1. `exprDepth_`：`parseExpr`（三元；右结合赋值 RHS 改走 parseExpr，语义等价）、
     `parseCoalesce`（`??` 自递归）、`parseUnary`（拆 wrapper 守卫 + inner）。
  2. `stmtDepth_`：`parseBlock`（嵌套块，跳匹配 `}`）、`parseStatement`（if/while
     链，跳到 `;`/`}`）。
  - 报错文本 `expression/statement/block nested too deeply`，与既有 parsePrimary
    守卫一致；恢复后继续解析不卡死。
- **torture 新增 `deep/` 类别**（48 个：paren/ternary/blocks/ifchain/coalesce/
  unary，4000..32000 层）——编译**不得崩溃**（低于阈值编译成功 / 超阈值干净报错
  均算过，SIGSEGV/abort 判失败）。`run_torture.sh` 加 deep 段 + 汇总。
- **负测试**：`tests/negative/expr_recursion_deep.myp`（500 层三元 → `expression
  nested too deeply` 干净拒绝，原 50000 层 SIGSEGV）。
- 验证：括号/三元/合并/赋值/一元/块/if 链 50000 层全干净报错（原 SIGSEGV）；
  正常代码不受影响；全量回归 **334/0**。BUGLIST 记 BUG-055。

### v3.15.87 — 修 `exprLlvmType` 对 `+` Binary 指数爆炸（巨表达式不可编译）+ torture plus_bomb

**非破坏性**（selfhost codegen 健壮性）。用户要求添加"百个 `i++` + 一万个加号"的
变态压测 → 直接打爆编译器（10,000 加号 >120s 超时不可编译），暴露纯**时间**爆炸
（内存 7-24MB 不变）。

- **根因**（`tools/selfhost/src/codegen.myp` `exprLlvmType` Binary `+` 分支）：
  `+` 分支为判字符串拼接调用 `exprLlvmType(lhs/rhs)`，底部 `llt/rlt` **再算一次**
  → 左深链 `0+1+1+...` 每层 2 次递归、逐层翻倍 = 指数 2^N。实测深度 20=0.5s、
  24=6.2s、26>20s。gdb 热栈：`genExpr → exprLlvmType`（23 层递归）→ `intWidth`。
- **修复**：`+` 分支去掉递归（resolvedKind=="string" 快路径保留不递归）；「LLVM 类型
  为 ptr → 拼接结果 ptr」检测移到底部复用已算的 `llt/rlt`。操作数类型每节点算一次
  → O(n²)。深度 24：6.2s → **0.10s**；10,000 加号：>120s → **10.8s**。
- **torture 扩展**（`tests/torture/gen_torture.sh`）：
  - 新增 `plus_bomb_*`（8 个 execute，10000..17000 加号巨表达式 + 100..240 `i++` 语句，
    自验证 r/`i` 正确）——正是它暴露本 bug。
  - 新增 `struct_nest_*`（8 个 execute，6..20 层嵌套 struct 链式读写自验证）与
    `struct_deep_*`（8 个 compile，40..180 层嵌套 struct 定义压测）。
  - torture execute 56 → **72**、compile 32 → **40** 全过（含 plus_bomb 总 ~4min）。
- **附带修复 BUG-054（torture struct_nest 生成器踩中）**：**不同 struct 类型赋值/初始化
  静默过 sema**——`typesCompat` 只比 kind "struct"，`A a; B b; a = b;` 或链式字段
  `root.inner...inner = root.inner`（L4=L1）被放行 → LLVM opt 报 `defined with type
  %B but expected %A` 崩溃（非干净诊断）。修复：`exprStructName`（Identifier 查
  SymbolEntry.className；**Member 递归解析对象 struct + 查字段声明类型**——resolvedClass
  对 Member 是容器名，直接比会误拒合法 `L2 y = a.b.c;`）→ Assign / VarDecl init 比较
  具体 struct 名，报错带名：`cannot assign value of type 'B' to variable of type 'A'`。
  负测试 `tests/negative/struct_assign_mismatch.myp`；合法嵌套链/同型拷贝仍正常。
- 验证：字符串拼接/数值 `+` 语义对拍（strcon ok=4）；全量回归 **334/0**（+1 负例）；
  torture compile 40×3 + execute 72 全过。BUGLIST 记 BUG-053 / BUG-054。

### v3.15.86 — match 扩展：字面量模式 + `_` 通配臂（switch-case 等价物）

**非破坏性（additive）**。按决策点 D7（docs/next_improvements.md）：不新增 `switch`
关键字，直接扩展 `match`——Arm 从仅枚举变体扩为三类模式。

- **语法**（grammar.md `MatchPattern`）：枚举变体 `E.V0`（不变，可带数据绑定）/
  整型字面量 `0`/`-1`/`4294967295u`/`0L`/`'c'`（负数、uint 上界、long、char 码）/
  字符串字面量 `"open"` / 浮点字面量 `1.5` / 通配 `_`（默认臂，可选）。
- **实现**（selfhost，三处同步）：
  - `ast.myp` AstMatchArm 加 `patKind_/litInt_/litStr_/litDbl_`（默认 "enum"）+ dump。
  - `parser.myp` `parseMatchArmPattern`：识别 `_`、负数/整型/字符串/char/浮点字面量，
    枚举路径不变；保留推进保护。
  - `sema.myp`：按 subject 类型校验字面量臂（整型↔整型/string↔string/float↔
    float/double）；枚举臂与字面量臂混用 → 报错；`_` 无约束。
  - `codegen.myp` `genMatchStmt`：枚举 subject → extractvalue 判别式（与 C++ 指令
    顺序逐字节一致，-O0 oracle 对拍不回归）；标量 subject → 直接比较（整型 icmp /
    `myp_str_eq` / `fcmp oeq`）；`_` → 无条件进臂。
- **特性语义**：无 fallthrough（隐式 break）；标量 match 允许非穷尽（`_` 可选）；
  枚举 match 内 `_` 作兜底；不可混用枚举臂与字面量臂。
- **测试**：`tests/@test/match_scalar.myp`（7 tests：int/uint 上界/long/string/char/
  double/枚举+_）+ 3 负例（字面量对 string、字符串对 int、枚举+字面量混用）+
  **torture 扩展**（+8 大整型字面量 match、+8 大字符串 match 编译压测、+8 标量
  match 自验证、+8 嵌套 match → torture 88 全过）。
- 验证：bootstrap 自举成立（MD5 一致）；全量回归 **333/0**（+7 @test 断言、+3 负例）；torture compile 32×3 档 + execute 56 全过。

### v3.15.85 — 修枚举/match 错误恢复死循环 OOM（torture 大枚举暴露）+ torture 套件内存限制

**非破坏性**（编译器健壮性）。Torture 套件（GCC Torture 风格）大枚举压测暴露：解析器
**错误恢复不保证推进**——非法枚举/match 语法会让解析循环卡死，每次迭代 `perr` 追加
Diag → 无界内存分配（实测 ~18GB，OOM 崩溃系统）。

- **根因**（selfhost `parser.myp`）：
  - `parseEnumDecl` 变体循环：`consume(";")`/`parseIdentifier` 失败都**不 advance**。
    MYP 枚举须分号分隔 `enum Color { Red; Green; Blue; }`；若写成逗号 `V0, V1`（或
    漏分号），循环卡在同一个 token 上无限转 → Diag 无界增长 → OOM。
  - `parseMatchStmt` 臂循环同缺陷：`parseIdentifier`+`consume("."/"=>"/"{")` 失败
    不推进，非法模式（`E => {...}` 缺 `.`/变体名）同样死转。
  - 修复：两循环加**推进保护**（`int before = current_; ... if (current_ == before)
    advance();`，与顶层/mapping/class/block 循环同款）。
- **torture 套件**（`tests/torture/`）：
  - `gen_torture.sh` 生成枚举改为**分号语法**；big_match 构造器改变体字面量
    （MYP 不支持 `new E(index)` 枚举构造）。
  - `run_torture.sh` 加**内存限制**（`MEM_LIMIT_KB` 默认 8GB，`ulimit -v` 作用
    mypc/opt/llc/ld 全链路）——编译压测必须限内存，防病态输入 OOM 崩溃系统。
- **负测试**（`tests/negative/`）：新增 `enum_comma_separator.myp`、`match_missing_dot.myp`
  （此前会 OOM，现应有界报错）；负测试循环加超时 + 1GB 内存限。
- 验证：非法枚举/match 现 **7.5MB 有界报错**（原 18GB OOM）；torture **compile 16×3 档 +
  execute 40 全过**；全量回归 **329/0**（含 2 新负测试）。

### v3.15.77 — N×M:1 每线程状态地基：pthread 线程 + 编译器 `@static @thread`（LLVM thread_local）

**非破坏性**。续 v3.15.76；N×M:1 协程（每线程独立协程表、@thread 下真并行）的
两块地基：

1. **`thread.myp` 裸 clone → pthread_create**：裸 clone（无 CLONE_SETTLS）子线程
   共享父 FS → thread_local/errno/栈金丝雀全共享，无法 per-thread；pthread_create
   由 glibc 一次性建栈/TLS/信号，子线程拥有独立 TLS。入口地址直接作为 pthread
   arg（rdi）传 start routine——不再 `Thr.entry` 全局 + 握手（无竞态）；子线程
   return 即正常结束（glibc 清 TLS），不再 `syscall 60`（那是 exit 整个进程）。
   ⚠️ 注意：`pthread_create` 的 `thread` 输出参数是**必填**（无条件写），传 NULL
   会崩（`mov %r14,(%rax)` rax=0）→ 用 arena 8B 槽 `Thr.tidSlot`。
2. **编译器支持 `@static @thread class`**（LLVM `thread_local` 全局）：
   - `ast.myp` AstClass 加 `isThread` 标志；`parser.myp` 接受
     `@static @thread class`；`codegen.myp` `emitStaticClassGlobals` 对 isThread
     类发射 `thread_local global`——LLVM 自动生成 `%fs` 访问（initial-exec，一条
     指令，和 C `__thread` 同机制）。
   - 自举成立（myp_self2==myp_self3 MD5 一致），`build/mypc` 安装新特性。

验证：探针两 `@thread` 实例各写各读 `TL.val` → **A=111/B=222**（per-thread 隔离）；
IR 确认 `@__myp_static_TL = thread_local global`；pthread_key 探针同理。
全量 **325/0 无回归**（新增 `tests/thread_local/`：worker 设 TL.val=111 不改主线程
自己的，mainTL=0）。`test_myp_viz.sh` 对拍：参考（冻结 oracle）报错的新特性文件
跳过（冻结基线无法对新语法字节级对比）。

> 这是 N×M:1 协程迁移的地基（步骤 0/1）。下一步见 v3.15.78（per-thread 表迁移）。

### v3.15.84 — 修 catch 字符串泄漏（exception_stress 暴露）+ 新增 4 个压测

**非破坏性**。续 v3.15.83；`exception_stress`（10 万次 throw/catch + `Memory.live*`
泄漏检测）暴露 **catch 变量字符串每抛漏 1 个**（实测 33000/10 万）：

- **根因**（selfhost codegen `bindCatchVar`）：曾先 `err = myp_get_error()`（rc=1 新
  计数串）再 `dup = myp_strdup(err)` 存 catch 变量——`err` 临时**从未释放**（每抛漏 1
  串）；且 `myp_get_error` 已返回独立拷贝（`myp_alloc`+memcpy），strdup 完全冗余。
  修复：直接用 `err`（catch 变量按 ARC 槽在 catch 结束后释放）。bootstrap 重建 + 全量
  **327/0**。
- **新增 4 个压测**（stress 12→16）：`timer_stress`（400 协程定时器错开）、
  `exception_stress`（throw/catch + ARC 泄漏检测）、`json_stress`（300 JSON 文档）、
  `waitany_stress`（300 事件广播 + 100 超时多路复用）。全量 stress **16/16**。

### v3.15.83 — 压测深挖修 3 个跨线程竞态（TimeBuf 共享缓冲挂死 + 通道状态竞态）

**非破坏性**。续 v3.15.82；新增两个压力测试（`tests/stress/coro_churn.myp`
海量 spawn/destroy 混沌、`xthread_storm.myp` 跨线程 channel+事件风暴）暴露并修复：

1. **`time.myp` TimeBuf 跨线程共享 → 挂死（真根因）**：timespec 缓冲是 `@static`
   进程级共享。`Time.sleep`（nanosleep）整个调用期间，worker 的 `myp_now_ms`/sleep
   并发覆写同一 `ts/rem` → 损坏 timespec → EINVAL → `myp_sleep_ms` 的 EINTR 重试
   循环无限重试 → MAIN 永久卡死（xthread_storm 间歇挂死，忙等则 20/20 通过）。
   修复：改 `@static @thread` 每线程独立缓冲（v3.15.77 编译器支持）。此前的
   "极小竞态"注释实为毫秒级窗口，非纳秒级。
2. **`Chan.lastPopOwner` 进程级单槽竞态**：chanPop* 弹出的 waiter owner 存共享
   全局，两线程并发 pop 互相覆盖 → 跨线程唤醒路由错线程 mailbox → 对端不醒死锁。
   修复：改每线程 `ChanPop` stash（pop+读同线程）。
3. **通道状态无锁 → 跨线程丢失唤醒死锁**：count 检查+缓冲+waiter 注册无锁，两线程
   并发时互踩 → 丢失唤醒。修复：`Chan.stLock` 状态自旋锁（缓冲/waiter/唤醒路由
   串行化；park 注册锁内、yield 锁外）；`chanWakeOne` 同线程改 ready=1 由调度器
   推进（不再锁内内联 resume——持锁跨 ctx_switch 会自死锁）。

验证：stress **8/8**（新增 2 项）、全量 **327/0**；xthread_storm 20/20 稳定
（此前 ~50% 挂死）。协程/通道基准不受影响。

### v3.15.82 — CoroT SoA→AoS：18 并行数组 → 单 CoroSlot[] struct 数组（缓存布局）

**非破坏性**。续 v3.15.81；协程每槽状态改缓存友好布局：

1. **CoroT 改 AoS**：18 个并行数组（stack/stackSize/fn/result/yieldVal/resumeVal/
   execResult/active/ready/generation/resultPending/onFreeList/discardResult/
   cancelRequested/waitTimeout/lastWaitEventId/lastWaitIndex）合并为 `CoroSlot[]`
   （每槽 96B 连续 struct，2 缓存行）。create 字段写从 16 个分散缓存行 → 1 个连续
   struct。扩容 `coroGrowSlot`（struct 值拷贝）。
2. **实测结论（诚实记录）**：AoS 只给 coro_spawn ~1ms（26→25ms）——rdtsc 显示
   create 从 4060→3818 cycles，字段写散乱仅 ~240 cycles。**create 的 ~3800 cycles
   是聚合内存延迟**（ctx arena 冷页写 + CoroF.count + 各调用的小分散访问 + 表增长），
   非单一可修组件。AoS 方向正确（cache 布局、后续优化基础），但 coro_spawn 的真正
   瓶颈在 create 整体簿记，非字段布局。

验证：runtime 重建 shadow 通过；全量 **327/0 无回归**。基准：coro_switch 53ms
（MYP 6× Go）、channel_pingpong 3ms（MYP 2× Go）、io_socket 84ms（打平）、
coro_spawn 25ms。

### v3.15.81 — runtime build.sh 补 opt -O2（全 runtime 优化）+ coro_spawn 小栈批量分配

**非破坏性**。续 v3.15.80；两个运行时性能改进：

1. **runtime_myp/build.sh 补 opt -O2**（同 mypc 管线）：runtime 模块此前**不跑 opt**，
   全部运行时函数是未优化 IR（alloca/store 风暴，coro.myp 9996→829 行）→ 拖慢
   spawn/io/json 等所有走 runtime 的路径。build.sh 现对每模块 `opt -O2 -mcpu=generic`
   （v3.12.44 起 opt 跨 setjmp/longjmp 安全；`MYP_RT_NO_OPT=1` 跳过）。
   基准（run_compare_go.sh，全 verify 与 Go 对拍）：
   - coro_switch 83→51ms（MYP **6.24×** 快于 Go，此前 3.7×）
   - channel_pingpong 6→3ms（打平 Go）
   - io_socket 91→83ms、coro_spawn 26→23ms；主套件多数基准亦小幅提升
2. **协程小栈批量分配**（coro.myp）：<128KB 小栈从逐块分配改一次 mmap 64 个切分入池
   （冷启动 20000 栈 ~20000 syscall → ~313 次）；栈池 64→1024 项容纳批量余量 + 大栈。
   批量区整批不复用外 munmap（VA 懒提交，4GB 安全阀），大栈/默认保留 mmap+守护页
   （溢出 SIGSEGV）。淘汰：小栈 drop、大栈 munmap。
   `myp_diag_stack_pool_capacity` 64→1024（rt_coro_test 同步）。

验证：runtime 重建 shadow 通过；全量 **327/0 无回归**。

### v3.15.80 — coro_spawn 协程栈混合分配（小栈 malloc 快路径，53→28ms）+ CoroEvW 动态扩容

**非破坏性**。续 v3.15.79；协程 spawn 性能缓解 + 跨线程事件注册表健壮性：

1. **协程栈混合分配**（`runtime_myp/coro.myp`）：小栈（<128KB，如 `@coro(stack=64)`
   显式小栈）从 mmap+守护页（2 syscall/次）改走堆 `malloc`（C runtime 同款，
   glibc arena 出块零 syscall）；大栈/默认（≥128KB，1MB 动态栈）保留 mmap 懒提交
   + 4KB PROT_NONE 守护页（溢出干净 SIGSEGV）。`coroStackReturn` 按尺寸分支
   free / munmap。
   基准 `coro_spawn`（20000 @coro spawn+resume）：**53→28ms**（C runtime 20ms 的
   1.4×，此前 2.65×；Go 3ms 的差距 17.7×→10×）。spawn 阶段 50→25ms（剩余为 glibc
   对 1.28GB 总量的 mmap 回退 + create 簿记，与 C runtime 同构）。coro_switch 仍
   MYP 3.7× 快于 Go（85 vs 316ms）。
2. **CoroEvW 解除写死上限**：事件等待者注册表 `eid/owner/slot/active` 与跨线程唤醒
   mailbox 从定容 `[1024]`/`[64]` 改动态扩容（复用 `coroGrowInt/coroGrowLong`）——
   消除静默丢弃（≥容量时丢失跨线程唤醒）；`coroEvWaitAdd` 前 `coroEvWCompact()`
   压缩 inactive 防 count 无限增长。

验证：runtime 重建 shadow 通过；全量 **327/0 无回归**；4 个协程基准
（channel_pingpong / io_socket / coro_switch / coro_spawn）verify 与 Go 对拍一致。

### v3.15.79 — 跨线程 channel 修复：owner 追踪 + 唤醒 mailbox（崩→正确）

**非破坏性**。续 v3.15.78；修复跨线程 channel rendezvous 段错误：

1. **waiter 记录加 owner 线程**：`Chan` 加 `recvOwner/sendOwner` 并行数组 +
   `lastPopOwner`；park 路径（send 满/recv 空）存 `pthread_self()`。
2. **`chanWakeOne/chanWakeReady` 检测跨线程**：waiter owner ≠ 本线程 → 投递跨线程
   唤醒 mailbox（`Chan.cwLock` 自旋锁 + `cwOwner/cwSlot`）；owner 调度器每轮
   `chanWakePump` 泵入自己的 `CoroT.ready` → 本线程 resume。同线程路径不变
   （内联交接/ready）。

修复前：跨线程 rendezvous（consumer B 线程 park、producer A 线程 send）
`chanWakeOne` 用当前线程表访问对端槽 → 段错误 139（旧全局表模型靠全局锁"能用"，
per-thread 下崩）。修复后：C-got 42 正确。

验证：跨线程 channel 探针（B 线程 consumer park、A 线程 producer send）正确收到
42；新增 `tests/coro_chan_xthread/`（确定性输出）；全量 **326/0 无回归**。

3. **跨线程事件 notify 修复**：`CoroEvW` 全局事件等待者注册表（eid/owner/slot/
   active）+ 跨线程唤醒 mailbox。`coroWaitAdd` 对 kind=0（事件）自动注册；
   `__myp_coro_event_notify` 扫注册表——同线程防漏直接 ready、跨线程 → mailbox
   （owner 调度器 `coroEvWakePump` 泵入自己的 `CoroT.ready`）。事件等待截止期
   超时清注册表防残留。
   修复前：跨线程 fire（B 线程）只路由到 B 队列，notify 只扫 B 的 CoroW → A 的
   协程等待者优雅挂起不醒。修复后：W-got 正确（新增 `tests/coro_evt_xthread/`，
   确定性输出）。

验证：全量 **327/0 无回归**（coro_event / coro_chan_xthread / coro_evt_xthread
等全过）。

### v3.15.78 — coro.myp per-thread 迁移（N×M:1：每线程独立协程表，@thread 下真并行）

**非破坏性**。续 v3.15.77 地基；把协程表从进程级全局 + 全局锁改成每线程：

1. **表类改 `@static @thread`**：`CoroT/CoroW/CoroF/CoroCap/CoroPoll/CoroArgs/`
   `CoroRetired/CoroInit/CoroStackPool` 全部 per-thread（LLVM thread_local）。
2. **全局锁消失**：`coroLock/coroUnlock/coroMarkMultiThread` 变 no-op（40+ 调用点
   保留）——每线程表同一线程独占访问，无需互斥。`@thread` 实例的协程不再全局
   串行（旧模型 resume 持全局锁跨整个协程步）。
3. **exec worker 跨线程投递改 mailbox**（C runtime 同款模式）：worker 只往进程级
   `CoroExec` mailbox 写（带 owner `pthread_self()` + 自旋锁），owner 调度器每轮
   `coroExecPump` 把属于本线程的结果写进本线程 `CoroT.execResult` + `ready`——
   worker 从不碰 owner 的 per-thread 表。

验证：全量 **325/0 无回归**；coro_thread（多线程协程）/async_file（跨线程 exec）/async_sleep
全过；per-thread 隔离探针 A=111/B=222。

**并行基准**（每线程 3000 万次内存操作协程，wall-clock）：单 worker 15ms、双 worker
也 **15ms**（若串行应 30ms）——两个 `@thread` 实例的协程真正并行跑在独立核心。

> 测量注意：MYP 紧自旋会饿死 worker 线程（早期探针 2e9 自旋把 CPU 占满 → 假性偏慢）；
> 等待用 `Time.sleep` 循环；`Time.nowMs()` 会被 LLVM CSE 合并（中间无副作用调用时
> 测出 0ms），计时需在两次 nowMs 间夹有副作用调用（sleep/协程调用）。

### v3.15.76 — 协程 M:1 单线程无锁快路径（coro_switch 76→43ms）+ json 字符串构建优化（43→25ms）

**非破坏性**。续 v3.15.75；针对 MYP vs C 运行时剩余两大差距：

1. **协程 M:1 单线程快路径**（`runtime_myp/coro.myp`）：M:1 模型下协程表进程级
   全局、单线程独占访问，本无需锁。原 `__myp_coro_resume` 每次调 `coroLock()`
   都无条件 `gettid` 系统调用（probe 实测 1M 次 gettid = 34ms，恰是 coro 差距
   主体）。改为：
   - `CoroLock.multiThread` 标志——单线程（`multiThread==0`）走**纯深度记账**
     （无 syscall/原子/互斥）；`@thread` 出现（`myp_thread_spawn` 唯一入口，
     clone 前 `coroMarkMultiThread()`）后才切全局递归锁（gettid 身份）。
   - `coroMarkMultiThread` 处理锁窗口内 spawn 的边界（async_file 在持锁窗口内
     spawn worker）→ 无缝补 owner=gettid + mutex=1，无需释放重取。
2. **json 字符串构建优化**（`runtime_myp/json.myp`）：
   - `jsonSub` 逐字节循环改 `__myp_memcpy` 单次拷贝。
   - `jsonParseString` 无转义快路径：扫描闭合引号无 `\\` → 单次 memcpy 提取
     （避免逐字符 `val + __myp_chr(c)` 拼接）；有转义才走原逐字符路径。

实测（`bench/rt_myp_bench.myp`，-O2，verify 一致）：
coro_switch 76→43ms（vs C 35ms，2.2×→1.2×）、json 43→25ms（vs C 8ms，
5.4×→3.1×）；其余负载（strcat/alloc/hashmap/file_io/regex）持平。
全量测试 **324/0 无回归**（含多线程 coro_thread/async_file）；json 转义
（串内引号/换行/反斜杠/数组内转义）专项验证通过；bootstrap MD5 门禁通过。

### v3.15.75 — MYP 运行时 io 优化（readLine 复用缓冲 + select 快路径，file_io 90→39ms）

**非破坏性**。续 v3.15.74；针对 MYP vs C 运行时 file_io 差距（90 vs 27ms）优化
`runtime_myp/io.myp`：

1. **readLine 复用行拼接缓冲**（`IoBuf.scratch` 表 + `ioScratchEnsure`）：
   此前每次 `readLine` `myp_arena_alloc(4096)` + `reclaim`——file_io 200k 行 = 200k
   次 4KB arena 分配/归还；改为每句柄惰性分配一次、进程级复用。
2. **`ioReadLineInto` 的 base 地址提循环外**：fill 惰性分配一次、base 不变，
   不再每字节重读 `IoBuf.base` 表。
3. **`myp_io_select` 连续同句柄快路径**：`if (myp_io_cur_get() == handle) return;`
   ——连续 `hasNext`+`readLine`/`write` 同句柄时跳过自旋锁 + 查表（每行 2 次
   select）。

实测（`bench/rt_myp_bench.myp` file_io，200k 行写+读，-O2，verify 一致）：
写 42→26ms、读 47→13ms，**file_io 90→39ms**（MYP-only）；与 C runtime 27ms 的
差距从 3.3× 缩到 1.4×。其他负载（strcat/alloc/hashmap/json/regex/coro）持平。
全量测试 **324/0 无回归**；bootstrap MD5 门禁通过。

### v3.15.74 — 自举 GPU 原子加直降 atom.add.f64（sm_75）+ @thread 硬失败 exit_group

**非破坏性**。续 v3.15.73；真 GPU（RTX 2070 SUPER）验证发现并修复两个问题：

1. **自举 GPU kernel llc 加 `-mcpu=sm_75`**（`tools/selfhost/src/codegen.myp` `gpuPtxFromLl`）：
   - 此前 llc 未指定 `-mcpu` → NVPTX 默认老架构不支持 double 原子 → `atomicrmw fadd`
     降级成 `atom.global.cas` 循环。高竞争（1M 线程同抢 `acc[0]`）下 CAS 重试风暴
     → `Vectors.sum` 1M 元素 **25 秒**。
   - 加 `-mcpu=sm_75`（与 C++ oracle `gpuTargetArch()` 一致）→ 直降
     `atom.global.add.f64` → sum 1M 元素 **3ms**（与 seed 4ms 持平）。
   - 实测：`gpu_buffer_demo` L1（add+sum+max+argmax）25455ms → 26ms；PTX 从
     `atom.global.cas.b`（13 处）变为 `atom.global.add.f`（13 处）+ `atom.global.add.u`。
2. **`@thread` 下硬失败改用 `exit_group`**（`runtime_myp/test.myp` `myp_assert_abort` +
   `runtime_myp/exception.myp` 未捕获异常）：
   - `@thread` 子线程用 `CLONE_THREAD` + syscall 60（`exit`）只终止**当前线程**；
     此前 `myp_assert_abort` 的 `exit(1)` 在子线程里只退线程 → main 继续 `return 0`
     → `kernel.assert` 失败误报退出码 0（`test_gpu_assert_fail` FAIL）。
   - 改用 syscall 231（`exit_group`）终止整个进程 → 硬失败契约成立（退出码 1）。
   - 无 @thread 场景行为不变（main 线程 exit/exit_group 等价）。

验证：完整测试套件 **325 通过 / 0 失败**（含 `RUN_GPU_TESTS=1` GPU CPU 回退 61/0）；
真 GPU 下 backend=CUDA、`Vectors.sum`/`gpu_buffer_demo` 性能恢复正常；bootstrap MD5 门禁通过。

### v3.15.73 — GPU 数学走 __nv_* libdevice + opt -mcpu=generic + 修 io 归档致 GPU 测试挂起

**非破坏性**。续 v3.15.71/72 性能与运行时工作；本次修复 324 测试中 GPU @test 的挂起（回归）根因。

1. **自举 GPU 数学函数映射 __nv_* libdevice**（`tools/selfhost/src/codegen.myp`）：
   - `genKernelDeviceCall`：sqrt/abs/floor/ceil/trunc → LLVM intrinsic（NVPTX 原生指令）；sin/cos/tan/exp/log/asin/acos/atan/sinh/cosh/tanh/pow/atan2 → CUDA libdevice `__nv_*` 外部调用（double 无后缀、float 加 `f`，pow/atan2 双参）。
   - 原因：NVPTX 无法 select `llvm.sin.f64` 等超越 intrinsic → llc 致命 "Cannot select" abort → 自举收到 uncaught exception 崩溃。声明即可让 llc 通过，真 GPU 由 libdevice 解析。
   - 两处 kernel preamble（main @gpu + tile）同步把 `llvm.exp/sin/cos/pow...` 声明替换为 `__nv_*`。
   - **修复非泛型 Math 方法（`Math.pow`，rfn=`Math_pow`）内核漏映射**：`genKernelDeviceCall` 增 `Math_` 前缀分支（原只识别 `__gs_Math_*_inst` 泛型实例与 `Device_*`）→ kernel 内 `Math.pow` 现走 `__nv_pow`，不再生成未声明的 `@Math_pow` 调用（此前静默 CPU 回退，llc 报 undefined value）。
2. **opt 加 `-mcpu=generic`**（`tools/selfhost/src/link.myp`）：此前裸 `opt -O2 -mtriple=X` 无 CPU sub-target → TargetMachine cost model 失效 → 进位链循环不展开（bigint 74→62ms、floyd 69→57、sha256 17→15，与 seed/clang 持平）。
3. **修 GPU 测试挂起根因：io 归档过时**（`runtime_myp/io.myp` + 构建流程）：
   - build 里 `libmyp_rt_myp.a` 是旧版（io 槽泄漏：fclose 不清槽 → open 递增槽 1..63，64 次后耗尽 → FileError 异常崩溃），且 `mypc` 链接的是编译时的归档 → 编译 `import cuda`（43 个 @gpu 内核、大量 File 写）触发槽耗尽 + 崩溃变僵尸进程（表面"挂起"，伴随残留子进程）。
   - 修复：重建归档（`runtime_myp/build.sh`）+ 重连 `mypc`（bootstrap MD5 门禁验证）；`import cuda`/`gpu_paradigm`/`manual_ch9`/`cuda_force_cpu` 全部恢复通过。
   - 顺带把 `myp_io_fclose` 压缩成一行（多语句）展开为多行（语义不变，可读性）。
   - ⚠️ 构建注意：`myp_rt_myp` 的 CMake 依赖未覆盖 `runtime_myp/*.myp` 源变化，改 runtime 后须手动 `bash runtime_myp/build.sh` 重建归档，再 `cmake --build build --target mypc` 重连编译器。
4. **base64 用 `uint8[]`**（`bench/myp/base64.myp`）：`int[]`→`uint8[]` + 显式 `uint8()` 强转 → 17→12-13ms（与 clang++ -O2 持平），verify=1036217148 不变。
5. **run_compare.sh 方法论加固**（`bench/run_compare.sh`）：taskset 核绑定 + MYP/C++ 交错测量（best_pair 轮换顺序）+ warmup + 频率/governor 检查 + clang++ 缺失警告；CXXFLAGS 统一 `-O2`（与 LLVM `-O2` 对称，避免 g++ `-O3` 向量化 fdiv 造成 false 加速）。

验证：完整测试套件 **324 通过 / 0 失败**（含 GPU 测试）；bootstrap 2 级 MD5 门禁通过。

### v3.15.72 — MYP 运行时 file_io 写缓冲 + myp_io_cur_get 单线程快路径（396→89ms）

**非破坏性**。续 v3.15.71 读缓冲，继续消除 file_io 剩余瓶颈：

1. **写侧缓冲（每句柄 4KB）**：`myp_io_write/write_line/write_byte/write_i32be/
   write_double` 改为追加到写缓冲（`ioWriteBuf`/`ioWriteByte`），满才 `ioFlush`
   批量 write——不再每行 1 次 write syscall + 1 次分配拷贝。flush 时机：缓冲满 /
   `fclose` / `seek` 前 / 同句柄读前（`ioFlush` 无 wpos 时零开销）。
2. **`myp_io_cur_get` 单线程快路径**：仅 1 个已登记线程时免 `gettid` syscall
   （File API 每次操作先 select→cur_set 登记，count==1 ⟹ 唯一登记线程即调用者）。
   此前 gettid 占全部 syscall 的 **99.8%**（200K 行 ≈ 200 万次）——现在每行仅
   select 的 cur_set 1 次 gettid。

**验证**：file_io 基准 **396→168（读缓冲）→116（写缓冲）→89ms（cur_get 快路径）**
（4.5 倍），行数/内容与 C 一致（200001）；RSS ~41.8MB（每句柄 +8KB 读写缓冲）。
全量 **324/324**、shadow 冒烟通过（io/io_thread/async_file/coro_thread/threadpool
全过）。剩余差距（C 26ms）主要来自 select 每行 1 次 gettid——彻底消除需 TLS 线程
身份或接受多线程边界情况，暂留。

### v3.15.71 — MYP 运行时 file_io 读缓冲：readLine/read_byte/readAll 不再逐字节 syscall

**非破坏性**。性能回归分析定位 file_io 落后主因（MYP 396ms vs C 26ms，15 倍）：
`ioReadLineHandle`/`myp_io_read_byte`/流式 readAll 每字节一次 `read()` syscall
（200K 行 ≈ 160 万次）。新增**每句柄 4KB 读缓冲**（`IoBuf` @static 表，与 Io.table
同构；惰性分配、进程级复用）：

- `ioReadLineHandle`/`execIoReadLineRaw`（async worker）→ `ioReadLineInto`：先消费
  缓冲、耗尽才批量 read 4096。
- `myp_io_read_byte` → 缓冲取字节（read_i32be/read_double 自动受益）。
- `ioReadAllHandle`/`execIoReadAllRaw`：并入缓冲未消费字节（read ahead 已把 OS 位置
  推前）+ lseek 定位剩余批量读；流式路径用 `ioBufReadN`（上限 64KB）。
- `fopen`/`fclose`/`seek` 复位句柄缓冲；`ioBufInit` 全表显式清零（arena 非零初始化）。

**验证**：file_io 基准 **396ms → 168ms**（2.4 倍），行数/内容与 C 一致（200001）；
RSS 不变（每句柄 +4KB）。全量 **324/324**、shadow 冒烟通过（io/io_multi/io_thread/
async_file/async_sleep/stream 全过）。写侧（每行 1 write + 分配）仍是后续可优化点。

### v3.15.70 — BUG-051 收尾：回滚 @static 默认值 workaround + BUGLIST 标记已修复

**非破坏性**。v3.15.69 根因修复（@static 默认值显式常量初始化器）后，回滚因
"`--shared` 默认值不生效" 做的**显式初始化 workaround**（现在靠默认值初始化器生效）：

- `coro.myp`：`coroEnsureInit()` 不再显式 `CoroT.current = -1`（靠 `= -1` 初始值；
  BUG-051 的协程 0 误判根因）。
- `thread.myp`：`myp_thread_spawn` 不再显式 `Thr.stackSize = 1048576`（靠初始值）。
- `sync.myp`/`MIGRATION_STATUS.md`/`coro.myp` 注释修正：默认值已生效；syncInit 的
  arena **表内容**清零保留（`myp_arena_alloc` 非零初始化，与默认值无关）。
- `tests/BUGLIST.md`：**BUG-051 🟨 → 🟩 已修复**（含修复细节 + 回滚记录）。

**验证**：coro/coro_thread/threadpool/thread_atomic 全过；全量 **324/324**、shadow
冒烟通过（无行为变化，仅移除冗余显式初始化）。

### v3.15.69 — 根因修复：`@static class` 属性默认值真正生效（显式常量初始化器）

**非破坏性**。此前 `@static class` 全局一律发 `zeroinitializer`，**属性默认值被丢弃**
（`int a = 42` 实际得 0，`--shared` 亦然）——runtime 被迫用 syncInit/evInit 显式补
零/置值（"--shared 默认值不生效"注释）。现 selfhost codegen `emitStaticClassGlobals`
支持**显式结构体常量初始化器**：

- 存在**可折叠的非零默认值** → 发 `global %Cls { i32 42, i64 7, double 0x... }`；
  否则保持 `zeroinitializer`（零改动，保护既有模块/归档）。
- 支持：整数/浮点/bool/null/字符串字面量 + 一元负号 + Convert 透传 + 二元整数/浮点
  **常量折叠**（`+ - * / % & | ^`，`constIntEval` 递归处理 `2 * 3 + 1` 左结合嵌套）。
- 数组/结构体/向量属性 → `ft zeroinitializer` 元素；字符串默认 → `@str` 常量 GEP。
- 字段类型归一：int 字段数字串、float/double 十六进制位型（`floatLiteral`）、i1
  true/false、ptr null。

**验证**：`--shared` 与普通模式均发射默认值（`Foo.a=42` 运行正确）；新增
`tests/@test/manual_static_defaults.myp`（7 断言：int/long 折叠/一元浮点/bool/
无默认零/字符串/int 除法）；全量 **324/324**（+1）、自举不动点 16/16、shadow 冒烟。

### v3.15.68 — MYP 运行时性能回归修复：`s=s+i` O(N²) arena 内存爆炸（块容量原地扩展 + 空闲链表 + 回收修复）

**非破坏性**。性能回归查找（`bench/rt_myp_bench.myp` 8 用例 + `bench/probe_strcat.myp`
最小探针）定位并修复 MYP 运行时 3 处架构级内存缺陷：

1. **strcat O(N²) 爆炸（主修复）**：`s = s + i` 每次迭代分配临时串，破坏 bump-tail
   检查 → 每迭代重拷 + arena 永不回收 → N=60000 RSS 8.3GB（C 2.2MB，3700 倍）。
   - `alloc.myp`：`myp_arena_alloc_ex` 统一 **8B total 前缀**（块容量 @base0）+ **空闲链表**
     （首适复用，arenaReclaim 替代纯 bump）；`myp_arena_reclaim_raw` 供 raw 块回收。
   - `str.myp`：`myp_str_append` **块内原地扩展**（rc==1 且块容量够 → 直接在 s 数据区写，
     O(lb) 均摊几何增长，不再依赖 bump-tail）。
   - 实测：strcat 探针 8.3GB → **2.5MB**，strcat_dyn 27ms / strcat_const 23ms（**反超 C**）。

2. **io.myp 每调用 raw scratch 泄漏**：`ioReadLineHandle` 每行 `myp_arena_alloc(4096)`
   永不归还 → file_io 读回 20 万行 ≈ 819MB。全部 per-call 缓冲（readLine/readAll/
   write_line/read_byte/write_byte/i32be/double/read_double/read_line）用后
   `myp_arena_reclaim_raw` 回收。实测 file_io arena 920MB → **40MB**，525→399ms。

3. **json.myp `myp_json_free` 空操作泄漏**：改**计数器式释放**（`liveParses`）——最后
   存活 parse 释放时复位节点表，槽位复用 + 槽内旧串由 ARC 覆盖回收。实测 json
   311MB → **9.7MB**，114→44ms。

**连带修复（空闲链表暴露的"依赖 mmap 零页"潜伏 bug）**：
- `term.myp`：`TermRaw.raw` 未清零 → `myp_free_all→myp_restore_term` 在 raw 从未启用时
  仍 `tcsetattr(fd 0)` → 后台进程组触发 SIGTTOU 停止（async_file 测试套件挂死）。
- `sync.myp`：`syncInit` 全表未清零 → 票号锁 `serving/next` 为垃圾 → `syncAllocLock`
  自旋死循环（ch11 t_barrier_future 挂起）。

**验证**：完整基准 RSS **1.16GB → 41.5MB**（28 倍降），各 case 输出与 C 一致；
`tests/run_tests.sh` **323/323 全绿**；自举 `myp_self2==myp_self3` 不动点 **16/16**；
shadow 冒烟通过。新增 `bench/rt_myp_bench.myp`（含每 case arena 诊断）+
`bench/probe_strcat.myp`。

### v3.15.67 — 自举编译器即 mypc：oracle 降为种子 + 自举 2 级 MD5 门禁命令

**非破坏性**。工具链角色反转：**用户级 `mypc` 现在是自举不动点编译器**
（selfhost myp_self2 的副本，仅 MYP 归档链接）；原 C++ oracle 改名 `mypc-seed`
降为**种子编译器**（只负责编译 tools/selfhost → myp_self，stage0）。

- **CMake 自举链**：`mypc_seed → myp_self（stage0）→ myp_self2（stage1）→
  myp_self3（stage2）→ mypc`。`mypc` 目标带**自举 MD5 门禁**（
  `scripts/bootstrap_install.sh`）：只有 myp_self2 与 myp_self3 字节一致
  （MD5 相同，自举成立）才安装 myp_self2 为 `build/mypc`；不一致 → 构建失败
  （"只有自举 2 级 MD5 一致才编译成功"）。myp_self2/3 依赖 myp_rt_myp（归档）
  保证以同一链接方式产出，MD5 比较确定。
- **`mypc --bootstrap` 自举命令**（类 gcc）：当前编译器编译 tools/selfhost 源码
  → stage1，stage1 再编译 → stage2，比较 MD5；一致 → 0，不一致 → 1（编译失败）。
  stage 输出放 `<repo>/build/`（非 /tmp——mypRtLib 首个候选是
  `<exe_dir>/libmyp_rt_myp.a`，/tmp 下陈旧归档会先被拾取，曾致 process 未定义）。
- **测试适配**：`tests/test_myp_bootstrap.sh` stage0 默认改为 `./build/mypc-seed`
  （oracle 种子）；工具（myp_pm/myp_fmt2/myp_viz2）保持 seed 编译（被测者不被
  被测对象编译，稳健）。
- 验证：**全量 323/323（默认 mypc = selfhost）** + bootstrap 16/16（stage0=seed，
  myp_self2 == myp_self3 md5 一致）+ `mypc --bootstrap` 2 级 MD5 一致 +
  `mypc hello` MYP-only 链接 exit 42。

### v3.15.66 — 整体 runtime myp化：MYP 运行时默认 + 仅 MYP 归档链接（去 C runtime）

**非破坏性**。de-gcc 收官：MYP 运行时从"opt-in 归档"升级为**默认工具链**——
生成程序**优先仅链接 MYP 运行时归档**（完全绕开 C runtime / libmyp_rt.a / gcc），
实测常见语料 0 未定义符号。

- **link.myp 仅 MYP 归档链接**：归档存在时优先链接 `libmyp_rt_myp.a`（无
  `libmyp_rt.a` / runtime.c / C bridge 编译）；仅当链接失败（符号只有 C runtime
  提供，如 `myp_printf` varargs / cuBLAS 钩子）才回退 shadow（MYP 定义优先 + C
  runtime 后备，行为不变）。输出标记 `(MYP runtime only)`。
- **可行性实测**：hello/fib/io/process/json/coro_event/async_file/struct_arc/
  exception/regex + 真实工具（myp_fmt/myp_viz/myp_pm）**仅 MYP 归档链接全部成功、
  链接后 0 未定义非 libc 符号**。
- **编译器自身 MYP-only**：myp_self2 / myp_self3 均以 `(MYP runtime only)` 链接
  并正常运行（编译器本身不再依赖 C runtime）；bootstrap 不动点成立
  （myp_self2 == myp_self3 字节相同）。
- **归档默认产出**：`runtime_myp/build.sh` 默认 `MYP_MAKE_ARCHIVE=1`（显式
  `=0` 关闭）；新增 CMake 目标 `myp_rt_myp`（默认 ALL，依赖 myp_self，
  `MYP_SKIP_SMOKE=1` 跳过冒烟加速标准构建）——`cmake --build build` 即产出
  `build/libmyp_rt_myp.a`（36 模块 / 667 符号）。
- 验证：**仅 MYP 归档链接下 selfhost 全量 323/323** + coro_thread 10/10 +
  async_file 与 C 逐字一致 + bootstrap 16/16 + oracle 默认态 323/323 +
  shadow 冒烟 exit 0。剩余 C 边界仅 `myp_printf`（varargs，MYP 程序不调用）与
  cuBLAS 钩子（无调用方）——由回退路径兜底，不需迁移。

### v3.15.65 — 修复剩余 3 个架构级 MYP 运行时 bug（归档下 selfhost 323/323 全绿）

**非破坏性**。归档（`MYP_MAKE_ARCHIVE=1`）下最后 3 个架构级失败全部修复——
**归档下 selfhost 全量 323/323**（de-gcc 关键里程碑）：

- **coro_thread**（~80% 段错误）：MYP coro 表非 TLS（@static 全局），两个
  @thread 并发 create/schedule 竞态损坏。修 = coro.myp 新增**全局递归锁**
  `CoroLock`（owner gettid + depth）包住全部公共协程 API：
  - resume 持锁跨 ctx_switch（协程一步独占全局；yield 递归重入由 resume 统一
    释放）；**yield 不再加锁**——这是跨线程死锁的隐藏根因：yield 加锁后每次
    yield 泄漏一层深度（单线程递归无感；跨线程 worker 的 coroLock 永远等不到
    释放）。
  - scheduler 事件处理（`myp_event_process_all`）移出锁外（避免 coro→ev 与
    ev→coro 锁序反转）。
  - 验证：coro_thread 隔离 20/20 稳定；rt_coro/wait/chan_future/thread 冒烟全过。
- **myp_run**（args 透传段错误）：main 的 `string[] argv` 被 codegen 透传 raw
  char**（元素裸 C 串无 MYP 12B 头 → MYP myp_strlen 读头垃圾；C 版 strlen 容
  忍）。修 = **selfhost codegen** 在 main 入口对 `string[] argv` 参数调
  `__myp_build_argv()` 重建真 MYP string[]（读 /proc/self/cmdline）。C runtime
  （runtime.c）与 MYP args.myp 都提供同名符号（非归档路径走 C 版）。oracle 冻结
  不动。bootstrap 16/16 不动点不变（selfhost 编译器自身 main 无 argv）。
- **async_file**（输出顺序）：MYP io.myp 的 `myp_coro_file_read_line/all` 原为
  **同步读**（C exec worker 读 C FILE* 表，MYP shadow 后恒空 → 曾停用 worker）
  → await 阻塞协程线程 → R 全在前。修 = coro.myp 新增 **MYP 版 async exec
  worker**：
  - park（EXEC wait）+ clone worker（thread.myp myp_thread_spawn，共享任务槽
    capture 握手防覆盖）+ worker 独立线程阻塞读 + coroLock 内写 execResult/ready
    + resume 取结果。
  - io.myp 新增 `execIoReadLineRaw/AllRaw`：返回 **raw addr**（直接建 12B 头
    {len,rc=0,type_id}，不落 string 局部——worker 函数退出会 release → 悬垂）；
    协程侧 `__myp_addr_to_str` return retain(+1) → rc=1 干净。
  - 输出与 C runtime **逐字一致**（R/B 交错）。
- **rt_io_test hasNext 门禁**：测试按 peek 语义断言（读走最后字节后 hasNext=假），
  与 `!feof` 语义不符（C/MYP 双 runtime 都 exit 7）——测试改为先越界读触发 EOF
  再查 hasNext（build.sh shadow 冒烟门禁恢复全绿）。
- **CMakeLists sdl 遗留**：sdl/ttf 外部化后 `stdlib/bridges/sdl_bridge.c` 已移到
  `libs/sdl/bridges/`，CMake 仍引用旧路径（SDL2 检测到即编译失败）——移除过时
  引用（外部库由生成程序按需链接）。
- 验证：**归档下 selfhost 323/323** + 默认态 selfhost 323/323 + oracle 323/323
  + bootstrap 16/16（myp_self2==myp_self3 字节相同）+ shadow 冒烟 exit 0 +
  coro_thread 20/20 + async_file 输出与 C 逐字一致。

### v3.15.64 — 修复 7 个 MYP 运行时 bug（归档默认化推进，selfhost 320/323）

**非破坏性**。归档（`MYP_MAKE_ARCHIVE=1`）被 selfhost 拾取后暴露的 MYP 运行时
bug 修复——**10 失败 → 3**：

- **coro.myp 退役栈**：固定 `[256]` 列表满时**直接** `coroStackReturn`——此时
  trampoline 还在该栈上运行，栈被池复用 → 损坏。改动态扩容（同 C realloc），
  **绝不在 add 时释放栈**。修 coro_capacity/more/stack（1500 协程驱动崩溃）。
- **io.myp has_next**：C 语义是 `!feof`（**不探测**，feof 由读操作撞 EOF 置位）；
  MYP 版探测读 1 字节 → 撞 EOF 返回 0。加 per-handle **feof 标志**（独立并行
  数组），has_next=!feof。修 io（has3 语义）。
- **evInit / ioCurInit 清零**：arena **非零初始化**——锁/计数字段分配后未清零，
  晚初始化（重分配程序）时票号锁字段=垃圾 → futex 死锁（struct_arc 挂起）。
  显式清零锁/计数/队列头尾。修 struct_arc/io_thread/myp_fmt。
- **alloc.myp arena 锁**：全局 bump **无锁**，@parallel 并发分配竞态（C 用 TLS
  arena 隔离）。加 futex 票号锁包住 bump/try_extend。修 @test/parallel_string_new。
- **剩余 3 个（架构级，另立里程碑）**：coro_thread（MYP coro 表非 TLS，双线程
  竞态，~80% 崩）/ myp_run（codegen 透传 raw char\*\* 作 argv，MYP strlen 读头
  不兼容）/ async_file（异步投递时序差异）。详见 MIGRATION_STATUS §五。
- 验证：归档下 selfhost 全量 320/323；oracle 323/323 不受影响（不用归档）；
  bootstrap 16/16（仅改 runtime_myp，不动点不变）。

### v3.15.63 — runtime 迁移收尾审计 + exception_thread flaky 修复 + 归档固化

**非破坏性**。runtime myp化 收尾：**权威审计确认 de-gcc 目标达成**；顺手修复
预存在 flaky 测试；固化 MYP 运行时归档产出。

- **架构原则定案**：oracle（mypc）= **种子编译器，冻结、不扩展特性**——只负责
  编译 `tools/selfhost/src/*.myp` → `myp_self`（stage-0 自举）。runtime_myp
  模块/用户程序/stdlib 一律由不动点 selfhost 编译器（`myp_self`/`myp_self2`，
  全特性）编译。此前误把 oracle 当模块编译器（撞缺 `__myp_mem_store_i64`）已纠正。
- **权威审计（nm 口径）**：C runtime 顶层 `myp_*` **480** 个，`nm
  build/libmyp_rt_myp.a` 归档提供 **377**（~79%）；未影 **103** 个中**仅 3 个被
  codegen/stdlib 直接引用且均刻意保留 C**（`myp_printf` varargs / `myp_cublas_
  available` / `myp_cublas_sgemm`），其余 **100** 个是 C 内部 helper（gc-sections
  剥离或只藏在 MYP 化公共 API 背后）→ **runtime 迁移对 de-gcc 目标已达成**，
  `runtime_myp/MIGRATION_STATUS.md` 更新收尾结论 + 剩余可做项清单。
- **归档（OPT-IN，MYP_MAKE_ARCHIVE=1）**：`runtime_myp/build.sh` 可产出
  `build/libmyp_rt_myp.a`（MYP_RT_MYP 归档，供 selfhost 链接跳过 MYP 化 bridge
  的 gcc 编译）。**默认关闭**——⚠️ 审计发现 MYP 运行时（coro.myp 栈池 / io /
  struct_arc 等）在全量 323 套件的某些模式（如 1500 协程驱动）下有真实 bug：
  归档被 myp_self2 自动拾取（#51 mypRtLib）时 selfhost 全量变红（10 失败：
  coro_capacity/more/stack/thread、async_file、io、struct_arc 等），归档移走后
  恢复 323/323。**→ 修复这些 MYP 运行时 bug 是「归档成为默认」的前置**（下一
  里程碑）。e2e 假 gcc 验证仍有效（显式 MYP_RT_MYP 指向归档，7 个 MYP 化
  bridge 全过，gcc 完全绕过）。
- **陈旧二进制教训**：本次或此前给 `src/main.cpp` 加过 MYP_RT_MYP 归档支持又
  回滚源码，但 `build/mypc` 未重建（strace 显示仍打开 `libmyp_rt_myp.a`）→
  归档出现时 oracle 套件 10 失败。**回滚编译器源码后必须重建二进制**（touch
  三文件 + cmake build）。
- **e2e 假 gcc 验证**：`myp_self2` + `MYP_RT_MYP` 归档，单程序覆盖
  hash/json/regex/date/process/net/uds 全部 7 个 MYP 化 bridge，假 gcc（任何
  调用即 exit 99）下编译链接运行 exit 0——gcc 完全绕过。
- **flaky 修复**：`tests/exception_thread` 线程输出竞态——根因是嵌套 @thread
  （Main.run 里 spawn Worker）的句柄在**每函数作用域** `threadHandles_` 中，
  main 收尾只 join 本函数创建的句柄 → 嵌套 Worker 从未 join，进程退出时偶发
  截断输出（20 次里 2 次）。修复：Worker 直接在 main spawn（句柄进 main 的
  threadHandles_ → 收尾确定性 stop+join，startup 必在 join 前跑完），期望输出
  更新为确定性的 `thr_caught=worker_err` + `thr_done`。双编译器各 30/30 稳定。
- 验证：oracle+selfhost 全量 323/323（exception_thread 不再偶发 MISMATCH）；
  bootstrap 16/16。

### v3.15.62 — sdl/ttf 移出标准库为外部库（libs/，薄接口 + `.myp.libs` 侧车）

**非破坏性**。架构分层落地：**GPU 是语言能力**（`@gpu for` 绑定 → 留在运行时），
**SDL/TTF 是纯绑定**（业务逻辑由用户写）→ 移出 `stdlib/` 到 `libs/` 外部库，
通过 `--package-path` + `MYP_BRIDGES` 解析、`MYP_BRIDGES` 扫描桥目录，不占
运行时、不进编译器桥计数：

- **`libs/sdl/sdl.myp`**（原 `stdlib/sdl.myp`，git mv）：`import sdl` 便利层——
  仅导出接口 + SDL_* 常量，不含业务逻辑（薄接口原则）。
- **`libs/sdl/bridges/sdl_bridge.c` + `.cflags` + `.libs`**（git mv）：MYP_BRIDGES
  扫描路径解析到桥 .c 自动编译 + 追加 `-lSDL2`。
- **`libs/ttf/ttf.myp` + `libs/ttf/bridges/sdl_ttf_bridge.c(+cflags+libs)`**：同上
  （`import ttf`）。
- **`libs/sdl_ffi/sdl_ffi.myp`（新）**：`@static class SDLC` 常量的 SDL_* 1:1 薄
  ffi 接口（纯 `SDL_*` 调用，无包装逻辑）；`sdl_ffi.myp.libs` = `-lSDL2`——纯 ffi
  外部库**无需 gcc 桥文件**，编译期读取 `<模块>.libs` 侧车注入链接 flag（v3.15.58
  机制），仅 `--package-path libs` 即可。
- **构建入口**：`mypc|myp_self2 <src> --stdlib ./stdlib --package-path libs` +
  `MYP_BRIDGES="libs/sdl/bridges:libs/ttf/bridges"`。`mypview/tests/run.sh`（6 处
  MYPCC 调用）与 `MOS/CMakeLists.txt`（`myp_add_executable`）已加 `--package-path
  ${MOS_PKG}` + `MYP_BRIDGES=${MOS_BRIDGES}`。
- **验证**：`import sdl_ffi` + 侧车在**假 gcc** 下 `SDL_Init=0`（证明纯 ffi 免
  gcc）；mypview 回归 UIX/BNCT/JSON/DESIGN/UIXRUN/PIPE 全 PASS；MOS `mos_ui_demo`
  /`mos_ttf_demo`/`mos_desktop_launch` 链接 OK；oracle 323/323、selfhost 323/323、
  bootstrap 16/16（fixpoint 稳定）。tests/bench 源无 sdl/ttf 依赖。
- **stdlib 精简**：删 `stdlib/sdl.myp`、`stdlib/ttf.myp`、`stdlib/bridges/sdl_bridge.c*`、
  `sdl_ttf_bridge.c*`。桥计数维持 **75**（sdl/ttf 不计入运行时桥）。

### v3.15.61 — bridge 包 H 第五批：net 全 8 个 MYP 化（AF_INET TCP，libc ffi 薄接口）

**非破坏性**。TCP 网络 bridge MYP 化——shadow C `net_bridge.c` 的 8 个
`myp_net_*`（Linux，AF_INET 流套接字），libc ffi 薄接口、无业务逻辑：

- **`runtime_myp/net.myp`（新）**：socket/bind/listen/accept/connect/send/recv/
  close/fcntl/setsockopt/gethostbyname（libc ffi）。
  - **sockaddr_in** = `{sin_family:u16@0(=AF_INET=2), sin_port:u16@2(BE=htons),
    sin_addr:u32@4(NB), sin_zero[8]@8}` = 16B；无 store_i16 → family 2×i8 LE，
    port 手动 htons（高字节在前）；sin_addr/zero 显式清零（arena 非零初始化）。
  - **hostent** = `{h_name@0, h_aliases@8, h_addrtype@16, h_length@20,
    h_addr_list@24}`；`h_addr_list[0]` = 4B IPv4。
  - 错误码对齐 C：socket -1 / gethostbyname 失败 -2 / connect 失败 -3 / bind -2
    / listen -3。accept 传 NULL addr（不需对端信息）。
  - recv_line 逐字节剥 `\r\n`；返回串按实收长度构建。
- **`link.myp` mypifiedBridge**：加 `net_bridge.c`。
- **验证**：新 `bench/freestanding/rt_net_test.myp`（回环 server/connect/accept、
  双向 send/recv + recv_line、主机解析失败 -2、连接拒绝 -3、set_nonblock，11
  断言）——**一次通过**；**shadow 43/43**；bootstrap 16/16（fixpoint 稳定）；
  oracle 323/323。bridge 83 → **75**（net 8）。
- ⚠️ **发现预存在 flaky 测试**（与本批无关）：`tests/exception_thread` 是线程
  输出竞态——test.output 陈旧为 1 行（`main_done`），但 @thread Worker 现在可靠
  打印 `thr_caught`+`thr_done`（3 行）→ selfhost 全量偶发 MISMATCH。runtime.c
  未动、编译器未改 codegen，确认为时序 flake 而非回归（oracle 版同样 3 行）。
- **未做**：sdl/ttf 侧车模式（薄 ffi 接口，下批）。

### v3.15.60 — bridge 包 H 第四批：uds 全 9 个 MYP 化（AF_UNIX socket 纯 syscall）

**非破坏性**。Unix domain socket bridge MYP 化——shadow C `uds_bridge.c` 的 9 个
`myp_uds_*`（Linux），**纯 syscall 无 libc 包装、无 gcc**：

- **`runtime_myp/uds.myp`（新）**：socket 41 / bind 49 / listen 50 / accept 43 /
  connect 42 / sendto 44 / recvfrom 45 / close 3 / poll 7 / unlink 87。
  - **sockaddr_un** = `{sun_family:u16@0(=AF_UNIX=1), sun_path[108]@2}` = 110B；
    无 `__myp_mem_store_i16` → family 用 2 个 i8（LE 01 00）。
  - send/recv 走 sendto/recvfrom（已连接流套接字 dest=NULL）；返回串按实收
    长度构建（`myp_alloc(n+1)` → 头 len=n）。
  - `recv_line` 逐字节到 `\n` 剥 `\r`（`+` 累积，myp_str_append 快路径）。
  - `poll` 构造 pollfd 数组（8B：fd:i32@0 events:i16@4 revents:i16@6，POLLIN=1，
    revents 值均 <256 故 i8 判非 0），syscall 7，返回首个就绪索引。
  - 错误码对齐 C：bind 失败 -2、listen 失败 -3、connect 失败 -2。
- **`link.myp` mypifiedBridge**：加 `uds_bridge.c`。
- **验证**：新 `bench/freestanding/rt_uds_test.myp`（server/connect/accept、客户端
  →服务端 send/recv、服务端→客户端 recv_line 剥 \n、poll 多路复用、unlink 清理，
  9 断言）——**一次通过**；**shadow 42/42**；bootstrap 16/16（fixpoint 稳定）；
  oracle + selfhost 全量 323/323。bridge 92 → **83**（uds 9）。
- **未做**：net(14) 经 socket + getaddrinfo（下一批）；sdl/ttf 侧车模式。

### v3.15.59 — bridge 包 H 第三批：process 全 6 个 MYP 化（libc ffi，双 fork 后台进程）

**非破坏性**。进程管理 bridge MYP 化——shadow C `process_bridge.c` 的 6 个
`myp_process_*`（Linux），de-gcc ≠ 去 glibc（直接 ffi 调 libc）：

- **`runtime_myp/process.myp`（新）**：
  - `myp_process_run`：libc `system()` + `WIFEXITED`/`WEXITSTATUS` 解码
    （`(status&0x7F)==0 → (status>>8)&0xFF`，对齐 C bridge）。
  - `myp_process_output`：libc `popen("r")` + `fread` 分块（4095B）+ 增长缓冲
    （×2）+ 精确长度计数字符串（`myp_alloc(len+1)` → 头 len=len）。
  - `myp_process_get_pid`/`get_ppid`：libc `getpid()`/`getppid()`。
  - `myp_process_is_running`：libc `kill(pid, 0)`。
  - `myp_process_spawn`：双 fork 后台进程——`fork()` + 中间子再 `fork()` 后
    `_exit(0)`；孙进程 `setsid()` + `execve("/bin/sh", ["sh","-c",cmd,NULL],
    environ)`（execve 非常变参 → 手动构造 argv char* 数组 + envp 取 libc
    `environ`）；父 `waitpid` 回收中间子。全部 libc ffi，无 gcc、无 C 桥。
- **`link.myp` mypifiedBridge**：加 `process_bridge.c`（MYP_RT_MYP 归档存在时
  跳过其 C 编译，与其余 5 个一致）。
- **验证**：新 `bench/freestanding/rt_process_test.myp`（pid>0/ppid≥0/
  is_running 自身=1·不存在=0/run true=0·exit 42=42/output echo=hello\n·
  printf 多行·空 cmd/spawn true=0）——**一次通过**；`tests/process` MYP shadow
  链接输出与 C 一致（code=0/out=hello_myp_test/pid_gt0=1）；**shadow 41/41**；
  fixpoint `e7efd1b3` 不变（编译器仅加跳过列表）；oracle + selfhost 全量 323/323。
  bridge 98 → **92**（process 6）。
- **未做**：net(14)/uds(18) 经 syscall（下一批）；sdl/ttf 侧车模式。

### v3.15.58 — `.myp.libs` 侧车：纯 ffi 访问外部系统库（Go `#cgo LDFLAGS` 风格）

**非破坏性**。MYP 模块旁放 `<模块>.myp.libs` 侧车即可声明额外链接库——**纯 ffi 调
外部库（SDL/GL/zlib 等）无需 C 桥文件、无需 gcc**：

- **自举 `tools/selfhost/src/{link,main}.myp`**：`LinkSidecar` @static 累积器 +
  `Link.addLibsSidecar()`；`loadModule`（import 模块）与 `compile`（主文件）读
  `<file>.myp.libs` 累积；`link()` 在 bridge 发现后追加 `-l` 标志到链接命令
  （lld/shared/gcc 三路径都生效）。
- **oracle `src/main.cpp` 镜像**：`g_mypLibsSidecar` 累积器 + `addMypLibsSidecar()`
  在 `loadModule`/`compileSingle` 读侧车、bridge 发现后追加到 `bridge_libs`。
- **用法**：
  ```myp
  // sdlffi.myp — 无桥文件，ffi 直达 SDL2
  ffi int SDL_Init(int flags);
  ```
  ```sh
  # sdlffi.myp.libs（模块旁）
  -lSDL2
  ```
  ```sh
  myp_self foo.myp    # 自动带 -lSDL2
  ```
- **验证（决定性）**：假 gcc（任何调用即失败）+ 纯 ffi 调 SDL2 + `-lSDL2` 侧车 →
  自举 myp_self2 与 oracle mypc 均 `Link OK` + 运行 `SDL_Init(0)=0`（主文件侧车 +
  import 模块侧车两路径都过）。**无桥文件、无 gcc**。shadow PASS；bootstrap 16/16
  （fixpoint `e7efd1b3`，编译器已改）；oracle + selfhost 全量 323/323。
- **与既有机制的关系**：bridge .c 的 `.libs` 侧车照旧（MYP 化 bridge 走
  `MYP_RT_MYP` 归档）；本特性是**纯 ffi 用户的轻量替代**——不想写/不想要 C 桥时，
  直接 ffi + `.myp.libs`。

### v3.15.57 — de-gcc 第二步：自举编译器 bridge 跳过机制（MYP_RT_MYP，MYP 化 bridge 免 gcc）

**非破坏性**。自举编译器（`tools/selfhost/src/link.myp`）支持用**预编译 MYP 运行时
归档**替代已 MYP 化的 C bridge——这些 bridge（base64/date/hash/json/regex）不再用
gcc 现编译：

- **`Link.mypRtLib()`**：定位 `libmyp_rt_myp.a`（`MYP_RT_MYP` env 显式覆盖；默认
  取编译器二进制旁 / build/ / ./build/）。找不到返回 ""（回退 C bridge，行为不变）。
- **`Link.mypifiedBridge(c)`**：按 basename 判定 5 个已 MYP 化 bridge
  （base64/date/hash/json/regex_bridge.c），与 `runtime_myp/*.myp` 同步维护。
- **链接**：归档存在时 ① bridge 发现循环跳过 MYP 化 bridge 的 C 编译（`compileBridge`
  不被调用 → gcc 免）；② lld 命令把归档置于 `libmyp_rt.a` 之前 +
  `--allow-multiple-definition` → MYP 定义优先（shadow 机制，提供被跳过符号）。
- **验证（决定性）**：用**假 gcc**（任何调用即失败）+ `MYP_RT_MYP=/tmp/libmyp_rt_myp.a`
  （由 shadow 套件 /tmp/rt_myp_*.o 归档）编译 `tests/json` → `Link OK` + 运行输出
  与 C 逐字一致；json+date+regex+hash 综合程序同样编译/链接/运行全过（exit 0）——
  **gcc 完全被绕过**。二进制 `nm` 确认 `myp_json_*` 为 MYP 定义。默认路径（无归档）
  行为不变：oracle + selfhost 全量 323/323；shadow 40/40；bootstrap 16/16（新
  fixpoint `606bdca4`，link.myp 已改）。
- **说明**：归档默认不落 build/（避免默认激活改变所有程序链接面）；`MYP_RT_MYP`
  显式指定即启用。激活方式：`ar rcs libmyp_rt_myp.a /tmp/rt_myp_*.o`（shadow
  套件产物）。

### v3.15.56 — bridge 包 H 第二批：json 全 14 个 MYP 化（解析/查询/编辑/序列化）

**非破坏性**。JSON bridge 全量 MYP 化——shadow C `json_bridge.c` 的 14 个
`myp_json_*`（纯 MYP 递归下降 + libc strtod）：

- **`runtime_myp/json.myp`（新）**：节点表用 **@static 并行数组**（type/key/
  str_val/num_val/bool_val/child_count + 扁平 child_list，slot*64+children 定容
  64/节点，C 用 256）——沿用 coro.myp 模式，避开 struct 链式字段访问坑
  （BUG-029 族）。handle = slot+1（0=无效/未找到）。
- **解析**：递归下降（对象/数组/字符串含反斜杠转义/数字/true/false/null）；
  数字用 libc `strtod`（ffi，de-gcc ≠ 去 glibc）+ 词法原文存 str_val；字符串
  值用 `+` 拼接（myp_str_append 快路径）。
- **路径**：`jsonResolvePath` 按 '.' 切分（对象按键 / 数组按下标），空段跳过
  （对齐 C strtok）；`jsonFindChild` 供 set_value/remove。
- **编辑**：`set_value`（strtod 必须消费整串的 endptr 检查，同 C）/ `add_child`
  （`{`/`[` 开头走完整 JSON 解析，裸文本走标量）/ `remove`（左移删除）。
- **序列化**：美化 2 空格缩进 + 字符串转义（`\"` `\\` `\n` `\t` `\r` + 控制符
  `\u00xx`）。
- **⚠️ `myp_json_free` 为空操作**：MYP arena 进程级回收（C 即时释放树；bridge
  短生命周期用法，差异文档化）。
- **验证**：新 `bench/freestanding/rt_json_test.myp`（查询/嵌套+数组路径/非法
  输入返回 0/遍历/编辑/序列化精确比对/round-trip/转义，36 断言）——一次通过；
  `tests/json`（stdlib Json 包装）MYP shadow 链接输出**与 C 逐字一致**；
  **shadow 40/40**；bootstrap 16/16（fixpoint `e8033a53` 不变，编译器未改）；
  oracle + selfhost 全量 323/323。bridge 112 → **98**。
- **未做**：process/net/uds/sdl/ttf；编译器 bridge 跳过机制（link.myp 后续）。

### v3.15.55 — bridge 包 H 第一批：date/hash(md5·sha1)/regex MYP 化 + 字符串尾 `$` 编译器 bug 修复

**非破坏性**。bridge 纯算法层 MYP 化（shadow C bridge，libc ffi），并顺带修复
编译器字符串插值 bug：

- **`runtime_myp/date.myp`（新）**：`myp_date_format` / `myp_date_format_ms` /
  `myp_date_field` —— libc ffi（`time`/`localtime_r`/`strftime`；struct tm
  x86-64 布局 sec@0..yday@28；field 索引按 C bridge 语义映射，非结构体顺序）。
  **关键**：`myp_strlen` 读字符串头 len 字段（data-12，O(1)）——strftime 只写
  内容不更新头 → 回填 `data-12` 真实长度，否则 `Str.len` 读到 myp_alloc 头 255。
- **`runtime_myp/hash.myp` 补**：`myp_hash_md5`（RFC 1321，小端）+ `myp_hash_sha1`
  （FIPS 180-1，大端；rol n = rotr(32-n)）——沿用 sha256 的 uint 纯算法风格。
- **`runtime_myp/regex.myp`（新）**：`myp_regex_compile` / `myp_regex_match` /
  `myp_regex_free` —— libc ffi（`malloc(512)` 安全缓冲装 glibc regex_t +
  `regcomp`(REG_EXTENDED) / `regexec` / `regfree`）。de-gcc ≠ 去 glibc：消除
  「用 gcc 现编译 bridge .c」这一步，运行时仍用 -lc。
- **🐛 编译器 bug 修复（oracle `src/parser/parser_expr.cpp` + selfhost
  `tools/selfhost/src/parser.myp` 镜像）**：`expandDollarInterp` 对「`$` 后无
  合法标识符名」的 `$`（如**字符串结尾的 `$`**，典型 `"^[0-9]+$"` 正则）静默
  丢弃 → `"x$"` 编译成 `"x"`。修复：无合法名时 `$` 保留为字面量。影响所有以
  `$` 结尾的字符串；现有 `tests/regex`/`manual_ch11` 的正则模式此前被截断
  （断言恰好仍过），修后测的是正确模式。
- **验证**：新 `bench/freestanding/rt_hash_test.myp`（md5/sha1 标准向量 +
  长度）、`rt_date_test.myp`（TZ=UTC 定 epoch 断言，build.sh 特判）、
  `rt_regex_test.myp`（编译/匹配/释放/非法模式）；**shadow 39/39**；
  bootstrap 16/16（新 fixpoint `e8033a53`，编译器已改）；oracle + selfhost
  全量 323/323；`tests/regex` 过。bridge 122 → **112**（date 3 + hash
  md5/sha1 + regex 3，crc32/sha256 此前已影）。
- **未做**：json（14，下批）、process/net/uds/sdl/ttf。

### v3.15.54 — de-gcc 第一步：自举链接路径剥离 libgcc（lld 无 gcc 链接）

**非破坏性**。从自举编译器（`tools/selfhost`）链接路径彻底移除 gcc 的
`libgcc`/`libgcc_s` 依赖（工具链去 gcc 化的链接层收尾；编译层 bridge C 编译留待
包 H）：

- **`tools/selfhost/src/link.myp`**：删除 `gccLibDir()` 探测（`/usr/lib/gcc/
  <arch>/<ver>/libgcc.a` 扫描）与 lld 档A 链接命令里的 `-L<gccd> -lgcc -lgcc_s`
  （libc 两侧共 4 项）——现为 `-L<crt> -lc -lm -lpthread -ldl` + CRT，**纯 lld
  无 gcc**。依据：`myp_self2` 未定义符号审计为零 libgcc 风格（`__div*`/`__mod*`/
  `__multi3` 等），只拉 glibc（`__fprintf_chk`/`__stack_chk_fail`/`__isoc23_strtoll`）。
- **`runtime_myp/build.sh`** + **`bench/freestanding/run_float_bench.sh`**：同步移除
  `GCCD`（libgcc.a 目录）探测与链接命令里的 `-lgcc -lgcc_s`。
- **验证**：
  - 重建 stage0 `myp_self`（mypc 编译改后 link.myp）→ stage1 `myp_self2` → stage2
    `myp_self3`，**新 fixpoint 达成**：`myp_self2 == myp_self3` 字节相同（md5
    `5fe0a993`，旧 `9f5cf25b` 因 link.myp 内容变更而更新，属预期）。新 myp_self2
    `nm` 无 libgcc 符号。
  - **shadow 36/36**（`rt_bounds_fail_test`=134 / `rt_pkgA_fail_test`=1 均符合预期，
    全程无 `-lgcc` 链接）；**bootstrap 16/16**；**selfhost 全量 323/323**。
- **未做（编译层，包 H）**：`findCc()`（gcc）仍用于 bridge .c 编译（json/net/uds/
  sdl 等 122 个）与 shared/回退路径；bridge 包 H MYP 化后默认工具链可完全无 gcc。

### v3.15.53 — runtime myp化 #47：codegen 契约 5 个（bounds_error / RTTI 类型名 / free / 固定类数组释放）

**非破坏性**。把编译器依赖的 C runtime 函数继续清零——5 个 codegen 契约 MYP 化
（零编译器改动）：

- **`myp_bounds_error`（diag.myp）**：`slice<T>` 下标越界 → stderr
  `"MYP runtime error: slice index N out of bounds (length M)"` + `kill(SIGABRT)`
  /`exit(134)`（同 C abort()）。新 `bench/freestanding/rt_bounds_fail_test.myp`
  （应失败用例，build.sh `expected=134` 特判）：`slice<int>(4)[10]` → stderr 诊断
  + exit 134。
- **RTTI 类型名（rtti.myp 新增）**：`myp_obj_type_name`/`myp_type_name` ——
  `__myp_fn_addr("__myp_type_name_table")` 取 **selfhost 恒发射**的类型名表
  （无类程序也 `[1 x ptr]`，故无需编译器改动）+ 对象头 type_id（`addr-4`）→
  `myp_alloc` 计数拷贝（同 C myp_strdup；表内字符串无 ARC 头）。新
  `bench/freestanding/rt_rtti_test.myp`：`Rtti.typeOf`=Person / `typeId` 非 0 且
  跨类不同 / `sameType`=1 / null→"" / 头 type_id 直读一致。
- **`myp_free`（alloc.myp）**：libc `free`（`ffi void free(long)` 直调）。
- **`myp_release_fixed_class_array`（alloc.myp）**：固定类数组 count 个元素强引用
  槽循环 `myp_release`（backing 栈缓冲不释放，同 C）。
- **审计更新**：codegen 契约 C-only **仅剩 `myp_printf` 保留 C**（varargs 不便
  shadow；selfhost 只 `declare` 从不调用）+ `cublas` 2 钩子。shadow 375→380
  （~78%）；runtime_myp 模块 21→22。
- **验证**：shadow 36/36（34 + rt_rtti + rt_bounds_fail）；bootstrap 16/16
  fixpoint 不变（md5 9f5cf25b）；oracle + selfhost 全量 323/323（编译器未动）。

### v3.15.52 — runtime myp化 #46：协程动态表（解除 1024 上限）+ Go 式动态栈 + 栈池

**非破坏性**。coro.myp 两项结构性升级（包 D 收尾）：

- **解除协程数量上限（1024 → 动态）**：`CoroT`/`CoroW`/`CoroF` 的定长 `[1024]`/
  `[32768]` 并行数组改为**动态 `long[]`/`int[]` @static 数组**。`coroTEnsure`/
  `coroWEnsure`（初始 64，同 C `MYP_CORO_INITIAL_CAPACITY`；翻倍扩容，换引用 +
  逐元素拷贝）——**115 个 `CoroT.X[slot]` 访问点零改动**（与 C 的 AoS 扩表语义
  一致：扩表不动字段访问，只是换更大的数组）。ctx/retCtx arena 缓冲随容量扩容
  （新块 + 拷贝）。`myp_diag_coro_slot_capacity` 返回真实容量（`CoroCap.coro`）。
- **Go 式动态栈（大虚拟预留 + 按需分页 + 守护页）**：`coroStackAlloc` 预留
  `clamp(requested, 64KB, 64MB)`；编译器默认 128KB（无 `@coro` 注解）→ **提升到
  1MB 默认预留**（Go 式增长余量；显式 `@coro(stack=N)` 尊重 N）。`MAP_NORESERVE`
  惰性 mmap → **RSS 只算实际使用的页**（深递归才多占）；栈向下按需增长，**无拷贝/
  无指针修正/无编译器改动**（对比 Go 需精确栈图，MYP 不可行的拷贝方案）。底部
  4KB PROT_NONE **守护页** → 真跑飞（超上限）干净 SIGSEGV（替代静默堆破坏）。
- **栈池（C 平价）**：`coroStackTake/Return` 有界池（64 项 / VA 128MB，best-fit
  复用；退役 drain 回池，池满 munmap 含守护页）。`myp_diag_stack_pool_count/
  capacity/bytes/max_bytes` 从恒 0 → 真实值。
- **验证**：
  - rt_coro_test 扩展（section 7/8）：1500 并发槽（`slots=1500 cap=2048`，旧
    1024 上限处 create 返回 -1）+ `deepRec(20000)=20000`（旧 128KB 栈在 ~2-3k 层
    SIGSEGV；1MB 预留容纳）+ 栈池 capacity=64。
  - 探针：`slots=1500 cap=2048`、`1500 concurrent OK`、`deep recursion OK
    (20000 frames)` exit 0。
  - rt_coro_test / rt_coro_wait_test / rt_coro_chan_future_test 全过；**shadow
    34/34**；bootstrap fixpoint 不变；全量 323/323（oracle + selfhost）。
- **注**：`@coro` 调用返回**句柄**（非结果），须 `Coro.result(handle)` 取结果
  （既有约定，测试用对即无碍）。`@coro(stack=N)` 语义兼容（N 为预留，下限 64KB、
  上限 64MB）。

### v3.15.51 — runtime myp化 #45：包 G GPU Stage C（gpu.myp 异步拷贝/事件/CUDA Graph，包 G 收官）

**非破坏性**。GPU 流/事件/图层 MYP 化——shadow C `runtime_gpu.c` 剩余 17 个
`myp_gpu_*`，**包 G 三 Stage 全部完成（63 个 shadow）**：

- **gpu.myp 新增**：
  - **异步拷贝 5**：`myp_gpu_copy_h2d/d2h_async_d/f`（cuMemcpyHtoDAsync/
    cuMemcpyDtoHAsync）+ `d2d_async`（cuMemcpyDtoDAsync）——入口显式 `cuCtxSetCurrent`
    （201 INVALID_CONTEXT 教训）。
  - **事件 6**：`myp_gpu_event_create_h`（cuEventCreate，flags=0 计时）/ `record_h`
    （cuEventRecord）/ `wait_h`（cuStreamWaitEvent 跨流依赖）/ `sync_h`（cuEventSync）/
    `elapsed_ms`（cuEventElapsedTime，**float 出参 → `__myp_mem_load_i32` +
    `bitcast<float>` 位型重释**）/ `destroy_h`（cuEventDestroy）。
  - **CUDA Graph 6**：`myp_gpu_graph_capture_begin`（cuStreamBeginCapture THREAD_LOCAL=1）/
    `capture_end`（cuStreamEndCapture）/ `instantiate`（cuGraphInstantiate）/
    `launch`（cuGraphLaunch）/ `destroy`+`exec_destroy`。
- **发现并修复两个 400 STREAM_CAPTURE_INVALIDATED 根因**：
  - **`cuStreamEndCapture(CUstream, CUgraph*)` 参数顺序**：MYP 曾传
    `(Gpu.graphVal, stream)`（交换）→ 驱动返回 400、graph=0。改为
    `(stream, Gpu.graphVal)`。
  - **capture 期间禁 `cuCtxSetCurrent`**：捕获入口设当前上下文会使 EndCapture 返回
    400（纯 C 也不调用）→ 移除 capture_begin/end 的 setCurrent（cuGraphInstantiate
    仍保留，已验）。
- **⚠️ 本机驱动 595.84 怪癖（纯 C 复现，非迁移问题）**：`cuMemcpy*Async` 恒 201
  INVALID_CONTEXT（cuCtxGetCurrent 确认上下文已当前仍 201）→ 异步拷贝实现与 C
  runtime **平价**（返回 1，忽略 CUresult），数据迁移在本机不可验；事件/图真实路径
  全部正常。cuStreamEndCapture 修复前 capture 期间调 setCurrent 曾加剧（已去掉）。
- **rt_gpu_test.myp 扩展**：GPU 分支新增 `runStageC()`——流 create/sync/destroy +
  事件 create/record/sync/elapsed/destroy + 异步拷贝 C 平价（返回 1）+ CUDA Graph
  空捕获→instantiate→launch→destroy（新流避免失败 async 污染）。
- **验证**：
  - rt_gpu_test 双模式：fallback `GPU CPU-FALLBACK OK`；真 GPU `GPU OK`（含 Stage C
    流/事件/图）exit 0。
  - Stage C 探针（`/tmp/rt_gpu_stagec_probe`）：`STREAM OK / ASYNC-CALL OK /
    EVENT OK（elapsed≈0.002ms）/ GRAPH OK（graph+exec 非 0，instantiate/launch 全
    过）` exit 0。
  - **shadow 34/34**；bootstrap fixpoint 不变；全量 323/323（oracle + selfhost）。
    shadow 计数 358 → **375**（~77%）。**包 G 收官**（cublas 2 个厂商 hook 保留 C）。
- **未做**：cublas hook（myp_cublas_available/sgemm）；Stage C 的 async 数据路径在
  本机驱动下不可验（595.84 201 怪癖）。

### v3.15.50 — runtime myp化 #44：包 G GPU Stage B（gpu.myp 内核 load/launch/byoc/printf，@gpu for 真实 GPU）

**非破坏性**。GPU 内核执行层 MYP 化——shadow C `runtime_gpu.c` 内核路径 11 个
`myp_gpu_*`，**`@gpu for` 在 shadow 下首次跑真实 GPU**（状态分裂统一）：

- **gpu.myp 新增**（@static Gpu 扩展）：
  - 新 dlsym：`cuModuleLoadData`/`cuModuleGetFunction`/`cuModuleUnload`/
    `cuLaunchKernel`/`cuMemcpyDtoHAsync`（init 必需校验）。
  - **`myp_gpu_load_kernel`**：内核缓存（`kcPtx/kcName/kcRec` 128 槽按 (ptx,name)
    指针身份复用，同 C g_kcache；避免训练逐样本模块加载显存暴涨）。记录
    `{mod,fn,name}` = 24B arena，句柄 = 记录地址（同 C kernel_t*）。入口显式
    `cuCtxSetCurrent`（CUDA TLS 教训）。
  - **`myp_gpu_launch`**：`cuLaunchKernel` 11 参数 `__myp_indirect_i32` 编组
    （fn, gx,1,1, bx,1,1, 0, stream, args, NULL）；stream==0 同步
    （cuCtxSynchronize）。args = 编译器 void**（kernelParams 约定）直传。
  - **`myp_gpu_destroy_kernel`**：缓存命中保持不 unload；未缓存（缓存满回退）
    cuModuleUnload。
  - **`myp_gpu_to_host_async`**（cuMemcpyDtoHAsync，stream 模式回拷）。
  - **BYOC**：`myp_gpu_byoc_load`（→load_kernel）/`byoc_launch`（host long[] →
    void** arena 临时编组，n≤64）。
  - **kernel printk/assert**：`myp_gpu_printf_buf/cnt/fail`（惰性分配设备 staging
    pbuf 1024×56B / pcnt / pfail）+ `myp_gpu_flush_printf`（D2H 回读记录 → 宿主
    mini-printf：% 转换消费 int/double 由 mask 定，直接写 fd=1；assert 失败
    stderr+exit(1)）。**注**：selfhost `@gpu for` 不发射 printk staging（仅 declare），
    该路径为 oracle（走 C runtime）平价实现，shadow 套件不运行时触发。
  - **`myp_gpu_scatter_check_fail`**（noreturn：stderr+exit(1)，scatter unique 契约
    违约）。
- **rt_gpu_test.myp 扩展**：新增 `@gpu for` 真实内核执行段
  `data[i]=sqrt(4)+sin(1)→2.8414709848078967`（128 元素，1e-9 容差）——双模式同
  结果（CPU 回退 / 真 GPU）。
- **验证**：
  - rt_gpu_test 双模式：fallback `GPU CPU-FALLBACK OK`；真 GPU `name=NVIDIA GeForce
    RTX 2070 SUPER cap=705 memMB=7752` + `GPU OK`（含 @gpu for 内核执行）exit 0。
  - **直接探针**（`/tmp/rt_gpu_load_probe`）：`myp_gpu_load_kernel` 返回 `kctx=…`
    （非 0，真 PTX 加载）+ `launch=1` —— 确凿证明 GPU 路径。
  - manual_ch9 shadow 链接 **MYP_GPU=1** → 3 tests/11 assertions 全过（@gpu for +
    Vectors.add/sum 真 GPU）。
  - **shadow 34/34**；bootstrap fixpoint `9f5cf25b` 不变；全量 323/323（oracle +
    selfhost）。shadow 计数 347 → **358**（~73%）。
- **未做**：Stage C 流/事件/图（cuStream*/cuEvent*/cuGraph*，deeplearning 分项目
  changelog）。

### v3.15.49 — runtime myp化 #43：包 G GPU Stage A（gpu.myp，init/设备查询/内存/流）

**非破坏性**。GPU 运行时 MYP 化第一片——shadow C `runtime_gpu.c` 的初始化/设备
查询/内存+handle/流 35 个 `myp_gpu_*`：

- **runtime_myp/gpu.myp 新增**（`@static Gpu` 表）：
  - **init**：`myp_gpu_init` → `ffi long dlopen("libcuda.so.1", 1)` + dlsym 17 个
    `cu*` 函数指针（存 @static 字段）→ `__myp_indirect_*` 调用 cuInit(0) /
    cuDeviceGetCount / cuDeviceGet / cuCtxCreate_v2 / cuCtxSetCurrent（CUDA TLS
    修复：接触上下文的入口显式 setCurrent）。`MYP_GPU=1` env gate（无则 CPU
    回退）。
  - **设备查询 19**：`gpuAttr(id)` helper 走 `__myp_indirect_i32(fDeviceGetAttribute,
    attrVal, id, dev)`，out-param 用 arena 缓冲；name/cap/多处理器/时钟/内存等。
  - **内存+handle 12**：cuMemAlloc_v2 / cuMemFree / cuMemcpyHtoD_v2 / cuMemcpyDtoH_v2
    + 类型化 copy（h2d/d2h × double/float）+ d2d + alloc_handle/free_handle +
    sync_all。
  - **流 3**：cuStreamCreate / StreamSynchronize / StreamDestroy。
  - ⚠️ `@static Gpu` 访问约定：state 字段（initFlag/availFlag/devInit/devCount）
    直接值访问；arena address 字段经 `__myp_mem_load/store_*`（勿把值字段当地址
    解引用）。
- **新测试** `bench/freestanding/rt_gpu_test.myp` 双模式：
  - 无 MYP_GPU → CPU fallback（vendor="cpu"、devs=0、queries=0）exit 0。
  - MYP_GPU=1 → 真实 RTX 2070 SUPER：`name=NVIDIA GeForce RTX 2070 SUPER
    cap=705 memMB=7752` + cuMemAlloc/cuMemcpyHtoD/cuMemcpyDtoH/cuMemFree 内存
    roundtrip（host buffer i*3 校验）exit 0。
- **GPU MYP/C 状态分裂边界（已确认安全）**：MYP init 只设 MYP `Gpu.availFlag`；C
  `myp_gpu_load_kernel`/`launch`（Stage B 未 shadow）见 C static `avail`=0 →
  load_kernel 返回 NULL → codegen `@gpu for` 发射 `CreateCondBr(k_ok, launch_bb,
  cpu_bb)` → **CPU 回退**（不产生垃圾数据）。manual_ch9 shadow 链接 MYP_GPU=1
  → 3 tests/11 assertions 全过。shadow 二进制中 @gpu for 真实 GPU 需待 Stage B
  shadow load/launch 统一状态；oracle/deeplearning 用 C runtime 不受影响。
- **验证**：**shadow 34/34**；bootstrap fixpoint `9f5cf25b` 不变；全量 323/323
  （oracle + selfhost）。shadow 计数 312 → **347**（~71%）。
- **未做**：Stage B 内核（cuModuleLoadDataEx + cuLaunchKernel 12-arg 编组 + byoc/
  printf）与 Stage C 流/事件/图（deeplearning 分项目 changelog）。

### v3.15.48 — runtime myp化 #42 补：C-TLS 当前句柄 → MYP IoCur 表（hello/sync 二进制 0 C myp_*）

**非破坏性**。解决 #42 遗留的 C-TLS `myp_io_cur_get/set`（每线程当前文件句柄）：

- **io.myp 新增 MYP `myp_io_cur_get/set`**：`@static IoCur` gettid 键控表（append-only，
  每线程只写自己的槽 → get 无锁扫描、set 追加用 futex 票号锁）。
- **⚠️ 根因**：MYP clone @thread 无 `CLONE_SETTLS` → 共享父 TLS → C `__thread
  myp_io_cur` 对并发 @thread 实为**共享**（文件 I/O 会串号）。IoCur 表按 gettid 分
  线程 → 真正每线程隔离（既是去 C TLS 也是并发正确性修复）。
- **新测试** `bench/freestanding/rt_io_thread_test.myp`：2 个 @thread 各开自己的
  文件写 `AAAA`/`BBBB`，main 读回验证 `A=[AAAA] B=[BBBB] done=2` 无串号。
- **顺带发现**：ld.lld `--gc-sections` 会剥离未引用的 `.init_array` 项 → 新构建的
  hello/sync 二进制里 C static constructor（`myp_capture_args`/`__myp_coro_register_
  cleanup` 等）**也消失**（nm 实测 0 个 C myp_* 符号）。args 由 MYP args.myp 惰性
  接管、atexit 清理由 OS 回收兜底 → 运行正常。
- **验证**：**shadow 33/33**；hello（exit 42）/tests/sync（逐字一致）二进制 C
  myp_* 符号 = **0**；bootstrap fixpoint `9f5cf25b` 不变；全量 323/323（oracle +
  selfhost）。
- **未做**：GPU/bridges（包 G/H，功能层仍 C）。

### v3.15.47 — runtime myp化 #42：薄层收尾（10 个残留 C 函数 → MYP，去 C runtime 清理链）

**非破坏性**。按 #41 审计，shadow 掉程序**实际拉取**的残留 C 函数（hello 10 个、
@thread 程序 16 个）中的可 shadow 部分（10 个），使 hello 二进制 C 拉取 10→**5**、
`tests/sync` 16→**7**：

- **alloc.myp**：`myp_free_all`（codegen 在 main 退出调用；MYP 版 = restore_term +
  级联释放 class slice + region_free_all + arena_free_all）+ `myp_arena_free_all`。
  ⚠️ **只复位不 munmap**——MYP arena 进程级 mmap，退出时 OS 回收；显式 munmap 与
  main epilogue 的 ARC release / C atexit 产生 UAF（rt_str_test 段错误 139 实测）。
- **weak.myp**：`myp_weak_free_all`（MYP 弱表复位；节点随 arena 回收）。
- **env.myp**：`myp_env_set`/`myp_env_unset`——setenv/unsetenv 无 syscall 等价物
  → **ffi 直调 libc**（去掉 C runtime 包装，仍用 libc；与读侧 myp_env_get 走同一
  environ 一致）。
- **io.myp**：`myp_read_line`（read syscall 逐字节读 fd 0 至 '\n'，计数串）。
- **term.myp**：`myp_enable_raw`/`myp_restore_term`（termios ioctl=16，TCGETS/
  TCSETS）+ `myp_kbhit`（poll syscall）+ `myp_getch`（raw 读 1 字节）。
- **残留边界（shadow 无法消除）**：static constructor `myp_capture_args`（→fopen/
  fread）+ `__myp_coro_register_cleanup`（atexit 3 个清理）共 5 个——`.init_array`
  直指局部符号，需改 runtime.c 才能移除；`myp_io_cur_get/set`（C TLS 当前句柄，
  MYP 无 TLS）2 个。
- **验证**：新 `bench/freestanding/rt_thin_test.myp`（env set/get/unset 往返 +
  readLine，build.sh 管道喂 `hello thin`）；**shadow 32/32**；hello/sync 二进制
  C 拉取确认降至 5/7；bootstrap fixpoint `9f5cf25b` 不变；全量 323/323（oracle +
  selfhost）。
- **未做**：static constructor / C TLS 残留（需改 runtime.c 或留 TLS，见
  MIGRATION_STATUS 边界说明）。

### v3.15.46 — runtime myp化 #41 收尾：包 F 全链路零 C 依赖（不保留 C）

**非破坏性**。#41 补完 `myp_event_id_by_name`（codegen 对 `__myp_timer_create(
<运行时 string>, ...)` 生成调用的动态事件名查表），使包 F（`@thread`/`@threadpool`/
事件系统/定时器/每线程队列）**不再保留任何 C 实现**：

- **event.myp 新增** `myp_event_id_by_name(name, names, ids, count)`——逐字节
  strcmp（MYP string = `'\0'` 结尾 char*；names 是 char* 的 i64 数组，ids 是 i32
  数组），未命中/空名/count=0 → -1。
- **零 C 依赖证明**：`nm` 审计 6 个 shadow 链接二进制（threadpool / sync /
  coro_timer / coro_event / multi_event / mapping_chain），包 F 域（event/thread/
  timer/queue/work/handler/id_by_name）C-only 符号均为 **0**——`--gc-sections` 下
  C 的 myp_work_deque_*/myp_queue_*/myp_timer_wake_target 等内部静态函数无引用被
  剥离，不进入二进制。
- **验证**：新 `bench/freestanding/rt_evname_test.myp`（命中 beta=20/gamma=30、
  未命中 nope=-1、空名 -1、count=0 -1）；**shadow 31/31**；6 个 pkg F 测试二进制
  shadow 链接**逐字一致**；bootstrap fixpoint `9f5cf25b` 不变；全量 323/323
  （oracle + selfhost）。
- **未做**：@thread 子线程共享 arena 并发分配（文档化限制）；C 内部静态
  work/queue/exec worker（无程序引用，已死代码）。

### v3.15.45 — runtime myp化 #41：包 F `@thread` 生命周期 + 事件系统（event.myp，futex 无 libc/TLS）

**非破坏性**。`@thread`/`@threadpool` 全生命周期 + 事件系统 MYP 化——shadow C 的
`myp_thread_*` 8 个（create/run_loop/stop/destroy/self/is_current/
associate_instance/post_event）+ `myp_event_*` 7 个（register/push_scope/
pop_scope/fire/process_all/process_one/route_to_instance）+ `myp_timer_*` 3 个
（create/check/cancel_all）+ C 内部 helper 4 个（dispatch/route_to_thread/
timer_next_delay_ms/thread_current），**不依赖 pthread/TLS/libc**：

- **每线程事件队列** = arena 环形缓冲（256）+ 每队列票号锁 + futex seq 字。
  「当前线程」= `gettid`(186) 扫线程表（clone 子线程共享父 TLS → MYP 无 TLS，须
  自建）。线程槽 `@static EvTab`（slot 0=主线程保留；create 返回 slot，编译调用方
  当不透明 ptr，ABI i64/ptr 同寄存器 + ld.lld 不查签名）。
- **spawn 握手**：父设共享 spawnSlot → `myp_thread_spawn`（thread.myp clone 原语）
  → 子读后写 ready[slot]=1 → 父等 ready 才返回（逐线程 spawn 无竞态）。
- **子事件循环**：跑 `startup_fn(startup_arg, NULL)` → `while(running){ process_all +
  qWait(下个 timer 截止) }` → done=1 → syscall 60 退出；destroy 轮询 done（无 join）。
- **跨线程事件路由（BUG-005）**：dispatch 两遍——先路由他线程 handler（同目标只投
  一份，per-slot 去重缓冲；载荷 mmap 深拷贝 → munmap 释放，不用共享 arena——子线程
  不可并发 bump）→ 再跑本线程 handler → `__myp_coro_event_notify`（coro.myp）。
- **handler 表/定时器/实例映射**共用全局 futex 票号锁；timer 载荷用每线程 tPval
  缓冲（fire + process_one 立即处理，同 C 语义）。
- **顺带修复（潜伏 bug）**：alloc.myp `myp_diag_arena_reserved` 从 `Arena.head`
  （最旧 chunk）正向走 next 链只统计首 chunk——多 chunk 时 reserved 低估 → used >
  reserved 误报。evInit 一次 ~550KB 大分配首次暴露（rt_pkgA_test exit 12）。
  改为从 `Arena.cur`（最新）走 next 链（与 C 版一致）。
- **验证**：新 `bench/freestanding/rt_threadpool_test.myp`；**shadow 30/30**；
  `tests/threadpool`（@thread + 4 worker @threadpool "wwww"）与 `tests/sync`
  （4 @thread worker mutex_count=400 + condvar=42）shadow 链接**逐字一致**；
  coro_event/coro_timer/startup/multi_event/mapping_chain/scope_mapping/
  lambda_mapping shadow 逐字一致；manual_ch9（@parallel + @gpu）3/11 通过；
  bootstrap fixpoint `9f5cf25b` 不变（编译器未改）；全量 323/323（oracle+selfhost）。
- **未做**：`myp_event_id_by_name`（纯字符串查表，保留 C）、work 任务队列 / exec
  worker、@thread 子线程共享 arena 并发分配（文档化限制）。

### v3.15.44 — runtime myp化 #40：包 F `@parallel for` 线程池（pool.myp，futex 无 libc）

**非破坏性**。`@parallel for` 线程池 MYP 化——shadow C 的 `myp_pool_*` 8 个核心
API（`ensure_global`/`parallel_for`/`worker_id`/`thread_count`/`set_threads`/
`worker_count`/`is_active`/`destroy`），**不依赖 pthread/libc**：

- **全局单池** `@static PoolTab`（arena 缓冲字段 + 直接值字段分层；--shared
  默认值不生效 → 显式 lazy 分配）。首次 `ensure_global` 建池：`sched_getaffinity`
  （syscall 204）数硬件并发 → `myp_thread_spawn`（clone，#34）建 N worker。
- **spawn 握手**：父设共享 `spawnTid` + 子读后写 `workerReady`（父轮询等 ready
  才 spawn 下一个，无竞态）；worker_id 用 `gettid`（syscall 186）查表。
- **parallel_for**：`[start,end)` 按 step 分 ≤ nThreads*4 块，round-robin 推入
  各 worker 的 work-stealing deque（票号锁守护）→ `workSeq` futex 广播唤醒 →
  barrier 等 `doneCount==totalChunks`（futex seq 字）。worker：pop 自家底部 →
  steal 他人顶部 → 空则 futex 等 workSeq。
- **并行 body 经 `__myp_indirect_void` 调用**（body 4 参 + fn 地址 = 5 实参），
  arg = 捕获的局部变量结构体。⚠️ worker 并发跑 body——**body 不得分配 arena**
  （thread.myp 同限制）；Atomic/数组读写安全。
- **修复**：首版 `myp_pool_set_threads` 段错误（139）——`inited`/`nThreads` 等
  标量**值字段**被 `__myp_mem_load_i32(field)` 当**地址**解引用（值为 0 → NULL）。
  正解：值字段直接 `PoolTab.X` 访问；共享/原子字（running/spawnTid/workSeq/
  barSeq/doneCount/totalChunks）arena 分配后经地址访问。
- **验证**：新 `bench/freestanding/rt_pool_test.myp`（2 worker：int/long
  @parallel 1000 累加=499500、10×100 累加、isActive/workerCount/threadCount）；
  **shadow 29/29**；`tests/@test/manual_ch9_myp.myp`（含 @parallel for）用 MYP
  shadow 链接 **3 tests / 11 assertions 全过**；bootstrap 16/16（fixpoint
  `9f5cf25b` 不变，编译器未改）；全量 323/323（oracle + selfhost）。
- **未做**：`@thread`/`@threadpool` 事件路由（event 10）、`myp_pool_create/once/
  init_global` 等 C 内部旧 API、exec worker、任务队列。

### v3.15.43 — runtime myp化 #39：包 E 同步原语（sync.myp，futex 无 libc）

**非破坏性**。同步原语 MYP 化——shadow C 的 `myp_mutex_*`/`myp_sem_*`/`myp_cond_*`/
`myp_rwlock_*`/`myp_once_*`/`myp_barrier_*`（30 个），**不依赖 libc/pthread**：

- **futex 实现**（syscall 202，FUTEX_WAIT=128/WAKE=129 PRIVATE）+ 原子
  `__myp_atomic_add/sub/load/store_i32_addr`（atomicrmw，无 CAS）。
- **Mutex**：票号锁（serving/next 两字）+ futex；unlock 广播唤醒。recursive 用
  owner(tid via gettid 186) + depth，由票号锁守护。tryLock 用票号归还（安全：
  未持有票号 serving 不会越过）。
- **Semaphore**：原子计数（可负=等待者数）+ futex wait/wake；tryWait 归还。
- **CondVar**：waiters 计数 + seq（经典 futex seq 条件变量）；wait 释放关联
  Mutex、唤醒后重取；`while(!cond) wait()` 模式安全。
- **RWLock**：state（0 空闲 / >0 读者 / <0 写者）+ futex；tryRead/WriteLock 原子尝试。
- **Once**：done + **自带票号锁**（不复用 Mutex 表——once_create 在全局分配锁
  临界区内调 myp_mutex_create 会**嵌套分配锁自死锁**，首版 bug 已修）。
- **Barrier**：arrived 计数 + seq + futex（可复用，最后到达者换代唤醒全部）。
- **全局槽分配锁**：所有 create 用单一把 futex 票号锁互斥扫描空闲槽。
- **验证**：新 `bench/freestanding/rt_sync_test.myp`（单线程 API 全 + 2 worker
  跨线程互斥累加 200）；**shadow 28/28**；`tests/sync`（4 @thread worker
  mutex_count=400 + condvar=42 + tryLock/recursive/rwlock/sem/once API）用 MYP
  shadow 链接**输出逐字一致**；bootstrap 16/16（fixpoint 不变）；全量 323/323。
- **未做**：`myp_sem_getvalue` 等 C 内部 helper；future 已在 #38。

### v3.15.42 — runtime myp化 #38：包 D 协程 Phase C（channel + future + current 初始化修复）

**非破坏性**。协程 Channel + Future MYP 化——shadow C 的 `myp_channel_*`/
`myp_future_*`/`myp_coro_{wait,wake}_future`/`myp_coro_am_i_coro`（CORO_DESIGN
Phase C 切片）：

- **Channel**：`myp_channel_create/destroy/send/recv/try_send/try_recv/size/close`
  + 内部 `wake_one`（同步交接：协程调用方内联 resume 对端一步，深度守卫 64）/
  `wake_ready`（close/try 只 ready）。`@static Chan` 并行数组（环形缓冲
  head/count/capacity/closed + 256×256 recvW/sendW FIFO waiter）。协程 send 满/
  recv 空 → park；非协程满/空 → -1（不挂起）。
- **Future**：`myp_future_create/set/get/destroy` + `myp_coro_wait_future/
  wake_future`/`am_i_coro`。协程 get 未 ready → park（不阻塞线程）；同线程 set
  唤醒；非协程未 ready → 自旋 + sleep 回退（MYP 无 pthread_cond）。
- **⚠️ CoroT.current 初始化 bug（本里程碑发现+修复）**：`--shared` 库模式下
  @static 属性默认值不生效（全局 zeroinitializer）→ `CoroT.current = -1` 实际从
  0 起，恰等于首个协程槽号 → main（非协程）在 spawn 后被误判为「在协程 0」→
  channel/wait 走协程分支（内联 resume 改变输出时序/误 park）。修复：
  `coroEnsureInit()` 一次性显式置 current=-1，挂在全部公共 API 入口（26 处）。
  该 bug 亦影响此前 Phase A/B（被「首协程恰为槽 0」掩盖）。
- **验证**：新 `bench/freestanding/rt_coro_chan_future_test.myp`（channel 基本/
  producer park/consumer park/close + future ready/协程 park，6 项 exit=0）；
  **shadow 27/27**；`tests/coro_channel`/`coro_future`/`future`/
  `channel_multi_consumer` 用 MYP shadow 链接**输出逐字一致**；
  `tests/stress/channel_stress`（cap=1/8，sum=199990000）PASS；bootstrap 16/16
  （fixpoint 不变）；全量 323/323。
- **未做（Phase C 尾）**：非阻塞 exec worker、栈池、cleanup_all/thread_cleanup。

### v3.15.41 — runtime myp化 #37：包 D 协程 Phase B（事件/等待层 + 帧表 + 诊断）

**非破坏性**。协程事件/等待层 + ARC 帧表 MYP 化——shadow C 的 `__myp_coro_*`
事件契约（CORO_DESIGN Phase B 切片）：

- **事件/等待层（coro.myp）**：`__myp_coro_wait_event(_timeout)`/`wait_any`/
  `wait_any_of`/`sleep`/`wait_fd` + `__myp_coro_event_notify`。等待表 `@static CoroW`
  并行数组（kind/eventId/fd/fdEvents/handle/deadline/waitIndex，定容 1024）；等待 =
  注册 + park（ready=0）+ yield，唤醒由 scheduler 驱动。
- **调度器增强**：等待表压缩（drop inactive，防 O(N²) 累积）+ `myp_event_process_all()`
  （C 事件队列 → dispatch → MYP notify）+ 截止期过期（waitTimeout 标记区分超时 vs
  到达）+ fd 批量 poll（syscall 7，pollfd 8B 布局）。
- **C 桥**：`__myp_coro_event_notify` 去 static（runtime.c）——MYP 版 shadow 后可截获
  C `myp_event_dispatch` 的协程唤醒。
- **帧表 ARC 镜像**：`frame_set`/`frame_clear` 真实现（每协程 ≤32 槽，扁平数组；
  obj 为堆指针，`myp_release(__myp_addr_to_str(obj))`）+ `coroReleaseFrame` 挂进
  destroy（三路径）+ trampoline 收尾 → 强杀/未捕获异常零泄漏。
- **诊断**：`myp_diag_coro_slots/slot_capacity/free_slots` + `retired_count/bytes`
  （栈池无 → 恒 0，Phase C）。
- **验证**：新 `bench/freestanding/rt_coro_wait_test.myp`（事件到达/超时/waitAny/
  sleep/waitAnyOf 总体超时/waitFd-pipe，6 项 exit=0）；**shadow 26/26**；语言级
  `await evt`/`await evt timeout N` + `Coro.waitAny` + 帧 ARC 对拍——
  `tests/coro_timeout`/`coro_any`/`coro_frame_arc` 用 MYP shadow 链接**输出逐字一致**；
  bootstrap 16/16（fixpoint `9f5cf25b`，runtime 重链编译器逻辑不变）；全量 323/323。
- **未做（Phase C）**：通道、future（wait/wake_future）、exec worker、栈池、
  thread_cleanup/cleanup_all。

### v3.15.40 — 编译器修复：裸 const 标识符折叠 + selfhost `__myp_*` 去前缀豁免（BUG-050）

**非破坏性**。两个编译期解析缺口（runtime myp化 #36 暴露）双编译器修复：

- **裸 const 标识符折叠**：顶层 `const int CAP = 1024;` 解析为零参 const-decl 函数
  ——此前只有 `CAP()` 显式调用能折叠；裸引用 `CAP`（如 `CAP * 8`）报
  `expected numeric type, got 'function'`（selfhost）/'() -> int'（mypc）。修复：sema
  把 const-decl 零参函数的**裸引用**改判为其返回类型，codegen 发射隐式零参调用
  `call i32 @CAP()`（双编译器一致）。
  - **关键守卫**：`CAP()` 的 callee **不折叠**——`visitCall` 解析 callee 时置
    `in_call_callee_`，否则 `const string A; ... A()` 被误折叠成值类型 →
    `'A' is not callable`。
- **selfhost `__myp_*` 去前缀豁免**：codegen 通用 callee 路径的一刀切
  `__myp_`→去掉前缀 误伤真 `__myp_coro_*`/`__myp_destroy_*` 符号（未定义符号 +
  字面量实参不提升 i64）。修复：去前缀排除 `__myp_coro_`/`__myp_destroy_` 前缀
  （对齐 C++ oracle 显式 `intrinsic_map_` 的别名语义）。
- **验证**：裸 const `/tmp/const_bare5.myp` 双编译 exit=0（IR `call i32 @CAP()` 两处）；
  shadow 25/25（rt_coro_test 直调 `__myp_coro_resume` IR 全名 + i64 字面量）；
  bootstrap 16/16（新 fixpoint `091d2204`，编译器改）；全量 323/323（含
  const_string/eval 回归）。BUGLIST 记 BUG-050。

### v3.15.39 — runtime myp化 #36：包 D 协程核心（coro.myp，Phase A 生命周期切片）

**非破坏性**。协程运行时核心 MYP 化——shadow C 的 `__myp_coro_*` 编译器契约 20
个（CORO_DESIGN Phase A 切片：单线程生命周期核心）：

- **coro.myp**：`__myp_coro_create` / `set_entry` / `set_entry_arg` /
  `get_entry_arg` / `yield` / `resume` / `set_result` / `result` / `status` /
  `is_active` / `count` / `current_handle` / `request_cancel` /
  `cancel_requested` / `cancel_clear` / `destroy` / `scheduler` / `trampoline`
  （+ `frame_set`/`frame_clear` stub）。
- **关键机制**：
  - **协程表** `@static CoroT` 并行数组（ctx/retCtx 是**裸 arena 缓冲** `base+slot*8`
    ——ctx_switch 需要槽的**地址**写入保存的 rsp）；槽/代际句柄
    `{generation<<32|slot}`（陈旧句柄失效）+ 空闲槽复用。
  - **栈**：mmap 每协程；trampoline 完成 → 退役列表（不能就地 munmap，正运行其
    上）→ 下次 create/scheduler drain munmap。无栈池（Phase C）。
  - **trampoline 异常边界 = MYP try/catch**（编译器发射 setjmp + myp_exception_push，
    每协程进入 push 自己的 jb；单线程同时只跑一个协程 → LIFO 栈顶恒为当前协程
    边界）。
  - **scheduler**：合作式——每个 ready 协程推进一步（plain `await` = yield 且
    ready 保持）。
  - **entry args**：`@static long[16]` 共享槽（同 C 线程本地共享，只在 entry 读
    一次）。
- **两个编译器/语法发现（顺带规避）**：
  - MYP 顶层 `const int X = N;` 在 selfhost 下把 X 解析成 function 类型 → 常量
    内联字面量。
  - selfhost 模块内**直接调用 `__myp_*` 前缀函数**会丢 `__` 前缀 + 字面量参数
    错型（IR `call @myp_coro_resume(i32 0)`）→ 抽非 `__myp_` 前缀的
    `coroResumeIdx` helper 供内部调用。
- **验证**：`rt_coro_test`（eager 完成 20000 次槽复用 + 代际句柄失效 +
  status/is_active/count + 多参数 entry + await+scheduler 推进）。shadow **26 项**
  （25 exit0 + fail 探针 exit1）、bootstrap 16/16（fixpoint `1e6d4f7` 不变，编译器
  未改）、全量 323/323。
- **未做（Phase B/C，回落 C runtime 或 stub）**：`wait_event`/`sleep`/`wait_fd`/
  `wait_any`/`wait_any_of`（事件层）、`frame_set/clear`（ARC 帧表镜像 stub）、通道/
  未来、exec worker、栈池。

### v3.15.38 — runtime myp化 #35：包 B 诊断/统计（diag.myp：type_live + fail_alloc）

**非破坏性**。诊断/统计层 MYP 化——`Memory.liveObjectCountByType` 的 per-type
计数 + `Memory.failAlloc*` 确定性分配失败注入：

- **`diag.myp`**：
  - **type_live per-type 计数**：`myp_type_live_inc/dec`（挂 MYP alloc/free：
    `myp_alloc_object` inc + `myp_free_object` dec，按对象头 type_id 索引
    `@static TLive.counts[1024]` 定长数组）+ `myp_live_object_count_by_type`。
    替代 C 的 TLS 每线程数组（MYP 版进程级共享，非 TLS——符合 @static 模型）。
  - **fail_alloc 注入**：`myp_fail_alloc_enable/disable/get` + `myp_fail_alloc_check`
    （挂 `myp_arena_alloc_ex` 顶部注入点）。支持 `Memory.failAllocEnable(N)` 与
    `MYP_FAIL_ALLOC=N` 环境变量（首次检查读一次，经 `myp_env_get` + `myp_str_to_long`）。
    第 N 次分配到达 → stderr 稳定诊断 + `kill(pid,SIGABRT)`（同 C abort()）。
- **关键技巧**：
  - **注入 fire 前必须先禁用**（`FailA.at=0`）：fire 分支构造消息（
    `myp_to_string_u64`/`+` concat）会分配 → `myp_arena_alloc_ex` → 本 check 再
    触发 → seen 已 >= at → 无限递归栈溢出 139。C 版 fprintf 不经 MYP 分配器无此
    问题。
  - **MYP 版 vs C 版计数差异**：C 的 type_live 是 TLS 每线程；MYP 版全进程累计
    （文档化限制）。C 版 `myp_live_object_count_by_type` 因 MYP `myp_alloc_object`
    shadow 了 C 的分配路径而恒 0 → per-type 计数必须 MYP 侧自持（inc/dec 挂点）。
- **验证**：`rt_diag_test`（Node 对象按 type 计数增/减/回零、live 计数 churn 回基线、
  arena 诊断非负、fail_alloc 启→读→禁）；注入探针（`enable(1)`→`#1`+SIGABRT、
  `MYP_FAIL_ALLOC=3` 纯 env→`#3`+SIGABRT、极大 N 不误触发）。shadow **25 项**（24
  exit0 + fail 探针 exit1）、bootstrap 16/16（fixpoint `1e6d4f7` 不变，编译器未改）、
  全量 323/323。
- **未做（依赖包 D）**：`myp_diag_coro_*`/`stack_pool_*`/`retired_*` 读协程运行时
  状态 → 包 D 协程迁移后影。

### v3.15.37 — runtime myp化 #34：线程创建 clone 直建（thread.myp，不保留 C）

**非破坏性**。线程创建 MYP 化——**不保留 C/pthread**：`myp_thread_spawn` 用
clone syscall 直建线程，是**包 F 线程/pool 的地基原语**（用户指令"不保留 C"，
与既有"不要留胶水"策略一致）：

- **`thread.myp`**：`myp_thread_spawn(entryAddr)` —— mmap 1MB 新栈（16 对齐栈顶）
  + clone=56（CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM）+ 子线程跑共享入口后
  syscall 60 退出本线程。
- **关键机制/技巧**（探针 + 3 线程测试反复验证）：
  - **子线程只依赖 RAX(=0) 判定 + 共享全局**：父栈局部（alloca）在新栈不可见；
    entry 经 `@static Thr` 全局传（`__myp_indirect_void` 间接调用）。
  - **子分支不能声明局部**：子 RSP=新栈顶、无 prologue 帧 → codegen 的 `0x28(%rsp)`
    正偏移在栈顶之上越界（段错误）。修复：子分支只把 entry 经实参（rdi 寄存器）传
    进 helper `myp_thread_child_entry`（普通函数有自己的帧），helper 内 capture 后
    置 `Thr.done_read=1`。
  - **entry 共享槽竞态修复（确定性握手）**：多 spawn 复用同一 `Thr.entry` 会竞态
    （子未读父就覆盖 → 3 线程全跑最后一个 entry）→ helper 先 capture entry（寄存器/
    帧内）再置 `done_read=1`，父自旋等 `done_read` 才返回；子线程仍并发。
  - **@static 类属性初始化器在 --shared 模块下不生效**（`stackSize=1048576` 读出
    为 0 被钳到 65536）→ spawn 内**显式设置** `Thr.stackSize`。
  - **无 CLONE_SETTLS** → 子线程共享父 TLS（MYP @static 全局本就共享，符合当前
    "非 TLS"模型）；C TLS 变量未为子线程建立，子线程勿调依赖 C TLS 的代码。
  - 无 join API（靠共享标志/未来 futex）；arena bump 无锁，子线程勿并发分配。
- **顺带修复 BUG-049（selfhost codegen）**：`&&`/`||` 结果槽原来内联 `alloca i1`
  ——在循环体内每轮执行，LLVM 发射动态 `mov rsp; lea; mov rsp` 不恢复 → 无限循环
  RSP 跌破栈底崩溃。修复：改用 `entryAlloca("i1")`（提升到 entry 块，零初始化，
  `||` 默认 true 显式 store）。C++ oracle 本就正确（PHI 方案）。回归：rt_thread_test
  自旋改用 `||` 直写（原 `int done` 规避移除）。
- **验证**：`rt_thread_test`——3 并发子线程各写独立 @static 共享槽（独立入口函数
  消除编号竞态 + entry 握手确定性），父自旋校验全部完成且值正确，8/8 稳定。shadow
  **24 项**（23 exit0 + fail 探针 exit1）、bootstrap 16/16（新 fixpoint `1e6d4f7`，
  编译器改）、全量 323/323。

### v3.15.36 — runtime myp化 #33：包 C 异常机制（exception.myp）

**非破坏性**。异常运行时（try/catch/throw 的 jmp_buf 处理栈 + 错误消息 + 类型化
载体 + release_slot）MYP 化——**协程 Phase A 的 setjmp 异常边界前置之一**：

- **`exception.myp`**：`myp_throw`/`myp_throw_object`（记录消息/对象+类型）、
  `myp_get_error`（catch (string e) 取消息）、`myp_error_setup/is_active/clear`、
  `myp_exception_push/pop/get_jmpbuf/get_type/get_object`（处理栈）、`myp_try_escape`
  （IR 逃逸屏障）、`myp_release_slot`（异常 longjmp 路径释放 ARC 槽物理内存）、
  `__myp_longjmp`（ASan 感知包装，MYP 版跳过 ASan hook 直接调 libc longjmp）、
  `myp_diag_get/set_strict`（strict 标志 API）。
- **关键语义/技巧**：
  - **`setjmp` 用 libc**（生成代码发射 `call i32 @setjmp(ptr)` + ReturnsTwice，非
    myp_* 符号不影）；`__myp_longjmp` 经 ffi 调 libc `longjmp`（i64/i32 与
    ptr/i32 ABI 兼容，链接按名解析）。
  - 处理栈 @static Exc.handlerBufs（arena 64×8）+ depth；`myp_exception_get_jmpbuf`
    无 handler 时 stderr 打印未捕获 + exit(134)（abort 语义）。
  - 错误消息拷贝到 arena 缓冲（C 用静态 buf；MYP 拷贝保证抛出的字符串释放后仍可
    读）。
  - `myp_release_slot`：读 ARC 槽首 8 字节（类 ptr / 接口·函数胖指针 data 都是
    槽首）→ `myp_release(__myp_addr_to_str(obj))`（`__myp_addr_to_str` 是
    inttoptr 地址→ptr 重解释，非拷贝）。
- **验证**：`rt_exception_test`——字符串异常 catch 绑定消息 / 正常 try / 嵌套
  （内层捕获+外层捕获）/ 类型化（throw new MyErr + catch (MyErr e)）/ finally
  正常 + 异常路径。shadow **22 项**（21 exit0 + fail 探针 exit1）、bootstrap
  16/16（fixpoint `c998455d` 不变，编译器未改）、全量 323/323。

### v3.15.35 — runtime myp化 #32：console+test 框架包（print + @test 捕获/断言/报告）

**非破坏性**。包 A 收官：把 print 输出路径与 @test 框架（捕获/断言/报告）一起 MYP
化（之前因 C 内部 `myp_out_write`→`myp_capture_write` static 不可影、只影 print 会
与 C 捕获缓冲不一致而推迟）。新增 2 个模块：

- **`output.myp`**（print + 捕获）：`myp_print/println/print_int/print_long/
  print_bool/flush` + `myp_test_capture_start/stop/get/contains/eq`。统一出口
  `outWrite`：捕获态（`@static Cap.on`）追加 MYP 捕获缓冲（arena 增长），否则
  `write(fd=1)` 直接写 stdout（镜像 C myp_out_write → capture / fputs）。
  stdout 无缓冲（直接 syscall）→ `myp_flush` 空操作。格式化复用 num.myp
  （`myp_to_string_i32/i64`）。**`myp_print_float` 已在 float.myp**（经 `__myp_print`
  → 本模块 `myp_print`，自动接捕获，无需改）。
- **`test.myp`**（断言 + 报告）：`myp_assert/assert_msg/assert_eq/assert_neq/
  assert_long_eq/assert_long_neq/assert_str_eq/assert_str_neq/assert_float_neq/
  assert_null/assert_not_null/assert_abort` + `myp_test_set_msg/fail_msg/report/
  summary`。语义镜像 C：**@test 按退出码验证**（run_tests.sh 查 exit）、断言失败走
  **stderr**（fd=2，不经 stdout 捕获）、待消费消息（`myp_test_set_msg`）**仅失败
  消费并清空**（通过时遗留，同 C 行为）、`myp_test_report/summary` 直接 stdout
  （不经捕获，同 C printf）、`myp_test_summary` 返回 fail>0 ? 1 : 0（@test main
  退出码）。`myp_assert_null/not_null` 用 long 参数（指针地址，ABI 兼容）。
- **验证**：`rt_pkgA2_test`（pass 路径：print+捕获拼接/eq/contains/断言通过/
  summary=0）+ `rt_pkgA_fail_test`（fail 路径：故意失败断言 → stderr `1 != 2`/
  `custom msg`/空 detail + summary=1）。build.sh 加**按测试期望退出码**逻辑
  （rt_pkgA_fail_test 期望 exit 1）。shadow **21 项**（20 exit0 + fail 探针 exit1）、
  bootstrap 16/16（fixpoint `c998455d` 不变，编译器未改）、全量 323/323。

### v3.15.34 — runtime myp化 #31：包 A 残留薄层（str_parse_int_opt + diag_arena）

**非破坏性**。包 A 第一批（无前置快赢），新增 3 个 MYP shadow + 审计修正：

- **`myp_str_parse_int_opt`**（num.myp）：编译器内建 `parseIntOpt(s)` 发射
  `call i32 @myp_str_parse_int_opt(ptr, ptr)`——MYP 版用 **long 参数**（指针地址
  承载）**ABI 兼容 shadow**（ptr 与 i64 同为 64 位、rdi/rsi 传参，链接按名解析），
  **无需改编译器**。base-0 语义（空白/符号/0x 十六进制/前导 0 八进制/十进制）+
  有效性 `*ok`（区分合法 0 与失败；前导 '0' 八进制时 '0' 本身算数字 → "0"/"077"
  判有效）。
- **`myp_diag_arena_reserved/used`**（alloc.myp）：诊断读 arena。**发现**：MYP
  alloc.myp 只维护 `Arena.cur_used`（当前 chunk）+ 新增 **`Arena.total_used` 累计
  bump 计数器**（不写每 chunk 头 `used@16`，C 版逐 chunk 读字段不适用）→
  `myp_diag_arena_reserved` 遍历 chunk 链（next@0→cap@8）累加，`used` 直接返回
  total_used。bump 永不归还，total_used 即当前已用。
- **验证**：`rt_pkgA_test`——str_parse_int_opt 11 项（"123"/"0"合法零/"-42"/"+7"/
  "0x10"/"077"八进制/"abc"失败/""失败/"  42"前导空白/"12abc"截断）；diag_arena
  （分配后 used 增、reserved≥used）。shadow **19/19**、bootstrap 16/16（fixpoint
  `c998455d` 不变，编译器未改）、全量 323/323。
- **审计修正（MIGRATION_STATUS.md）**：正则漏 `ubyte[]` 等返回类型 → 实为 C 490
  个、已影 **137**、未影 353；`myp_str_to_bytes` 早已在 bytes.myp 迁移；`myp_str_
  cat/cpy/fmt/len` **无 MYP 调用方**（C 内部/死代码，无需影）；**print 层与 @test
  捕获（myp_capture_*）耦合**（C 内部 myp_capture_write 被 printf/assert 调用，
  迁移须连 test 框架一起，归入"console+test 框架"包，文档化推迟）。

### v3.15.33 — runtime myp化 #30：通用间接调用内建 `__myp_indirect_*`（GPU 迁移地基）

**非破坏性**。自举编译器新增**通用间接调用内建**（自举独有，同 `__myp_asm`/
`__myp_call_ptr`/`__myp_rtable_addr` 模式）——运行时 `dlopen`+`dlsym` 解析函数
地址后，按**任意签名**间接调用 C API（CUDA 驱动 / LLVM C API 绑定地基）：

- **sema/codegen**：`__myp_indirect_i32(addr, ...args)` → int / `_i64` → long /
  `_double` → double / `_void` → void。addr 是 i64 函数指针地址（dlsym /
  `__myp_fn_addr` 所得）；实参按调用点 MYP 实参的 LLVM 类型原样传（long 承载
  指针）。发射 `inttoptr i64 addr to ptr` + `call RET %fn(<类型化实参>)`。
- **用途**：GPU 层（`runtime_gpu.c` 65 个 `myp_gpu_*`）迁移的关键使能——CUDA
  是运行时 dlopen（`libcuda.so` 可能不存在、MYP_GPU 未设时 CPU 回退），不能
  静态 ffi 链 cu* 符号；44 个 `cu*` 函数指针签名各异（cuLaunchKernel 11 参等），
  现可经 `__myp_indirect_*` 调用。也为将来绑 LLVM C API 等复用。
- **验证**：`rt_indirect_test` 覆盖 i32/i64/double/void 四种返回 + 0/1/2/3 实参
  + 混合类型 ABI——i32(`myp_strlen` 1 个 ptr 实参)、i64(`myp_now_ms` 0 实参)、
  double(`myp_math_pow` 2 个 double / `myp_math_sqrt` 1 个)、void(用户 `myNop`)、
  用户函数 `myAdd`(2 个 i32)、`myMix`(i64+i32+double 混合) 全对。shadow **18/18**、
  bootstrap 16/16（新 fixpoint `c998455d`）、全量 323/323。IR 确认：`inttoptr
  i64 → to ptr` + `call <RET> %fn(<类型>)` 正确发射。
- **注意**：`__myp_fn_addr("name")` 只对**已声明/已定义**符号有效（preamble 里的
  runtime 函数或模块内定义；`myp_mfence` 等 runtime_myp 模块函数未在 preamble
  声明会报 "use of undefined value"）——CUDA 地址来自 dlsym（运行时 long），不
  受影响。

### v3.15.32 — runtime myp化 #29：math 层补齐（pow/atan2/sinh/cosh/tanh）

**非破坏性**。把 #28 遗留的 5 个 math 函数全部 MYP 化，`myp_math_*` 达 **19/19**
（零数值手写实现——**LLVM 21 原生支持全部 5 个 intrinsic**，实测确认）。

- **关键发现**：`llvm.pow.f64` / `llvm.atan2.f64` / `llvm.sinh.f64` /
  `llvm.cosh.f64` / `llvm.tanh.f64` 在 LLVM 21 全部存在（降级到与 C 相同的
  libm → 位级一致），且 LLVM **自动声明** intrinsic（无需 ir_emit declare）。
  sinh/cosh/tanh 因此也无需 #28 设想的 exp 组合数值实现。
- **自举 codegen 变更**：`genPolyMathIntrinsic` 扩展处理**二元** `__myp_math_pow`/
  `__myp_math_atan2`（两实参统一提 double → `llvm.pow.f64`/`llvm.atan2.f64`）；
  调度点移除 pow/atan2 排除（abs_int 仍走通用 FFI）。**顺带根治潜伏自递归**——
  原二元 `__myp_math_pow` 落通用路径发射 `call @myp_math_pow`，MYP shadow 版会
  自递归；现直发 llvm → `myp_math_pow` 内部 `__myp_math_pow` 安全。
- **`math.myp`** 19/19：新增 `myp_math_pow/atan2/sinh/cosh/tanh` 一行式
  （`return __myp_math_*(...)` → LLVM intrinsic，不递归）。oracle（mypc）不改：
  对 `__myp_math_*` 仍发射 `call @myp_math_*` → 链接到 MYP 定义 → 内部走 llvm.*。
- **验证**：`rt_math_test` 扩展 18 项断言（22-39）——pow 精确值（2^10=1024、
  2^-1=0.5、0^0=1）+ sqrt(2) 容差；atan2 四象限（0/π/2/π/4/-3π/4）；sinh/cosh
  已知值 + **溢出边界**（sinh/cosh(1000) > 1e300）；tanh 已知值 + **大参 ±1**
  （tanh(±20)=±1）。shadow **17/17**、bootstrap 16/16（新 fixpoint `e7075fb8`）、
  全量 323/323。反汇编：math.o 定义 19 个 `myp_math_*` T 符号；math.myp 内部
  仅 5 个 llvm intrinsic、**0 次 `call @myp_math_*`（无自递归）**。

### v3.15.31 — runtime myp化 #28：fs/env/args/term/math 薄层迁移

**非破坏性**。把五组"薄层" C runtime 符号 MYP 化（shadow 机制），新增 5 个
`runtime_myp` 模块 + 5 个验证程序：

- **`term.myp`**（2/2）：`myp_term_width/height`——`ioctl=16`(TIOCGWINSZ=0x5413)
  写 8B winsize 缓冲（ws_row@0/ws_col@2 u16），失败回退 80x24（同 C）。
- **`fs.myp`**（12/12）：`exists/is_dir/is_file/file_size/modified_time` 用
  `newfstatat=262`（AT_FDCWD=-100；内核 struct stat 144B：st_mode@24、
  st_size@48、st_mtime@88；S_IFDIR=0x4000/S_IFREG=0x8000）；`list_count/
  list_get` 用 `getdents64=217`（linux_dirent64：d_reclen@16 u16、d_name@19，
  跳过 . / ..）；`mkdir_p`（mkdir=83，0755=493，EEXIST=17 忽略，逐前缀）；
  `remove_recursive`（lstat=newfstatat+AT_SYMLINK_NOFOLLOW=0x100，getdents64
  递归 + rmdir=84/unlink=87，ENOENT 视为成功）；`dirname/basename/join` 纯字节
  操作。
- **`args.myp`**（2/2）：`myp_args_count/get` **惰性**读 `/proc/self/cmdline`
  （NUL 分隔，argc=NUL 数），@static 8B argc+8192B 原始缓冲缓存——免 C 构造器
  全局依赖。越界返回空串。
- **`env.myp`**（get 迁移；set/unset 保留 C）：**关键发现**——`/proc/self/
  environ` 只反映进程**启动时**的环境内存区（内核 mm->env_start），libc setenv
  使 environ 缓冲 realloc 移动后**看不到**新变量（实测：export 后 grep 不到）。
  正解：MYP `myp_env_get` 直接**遍历 libc 的 `environ` 弱符号**（char**，
  `environ@@GLIBC_2.2.5`）：`__myp_fn_addr("environ")` 发射 `ptrtoint ptr
  @environ to i64` 取址（复用 #25 内建），**自举 ir_emit preamble 加一行
  `@environ = external global ptr`**（声明非定义，未引用无害）→ 与 C getenv
  完全一致（含 setenv/unsetenv 实时修改）。`myp_env_set/unset` 无 syscall 等价
  物（libc 进程级 environ 操作）→ 保留 C，文档化。
- **`math.myp`**（14/19）：一元实数（sqrt/abs/floor/ceil/trunc/sin/cos/tan/
  asin/acos/atan/exp/log）+ `abs_int`——经 `__myp_math_*` 内建 → 自举 codegen
  发 **LLVM 标量 intrinsic**（llvm.sqrt.f64 等，与 libm 位级一致、不递归）。
  **未影（保留 C，另立里程碑）**：`myp_math_pow/atan2`（二元无对应一元 llvm
  直发路径，自举 `__myp_math_pow` 发射 `call @myp_math_pow` 会自递归）、
  `myp_math_sinh/cosh/tanh`（LLVM 无对应 intrinsic，需 exp 组合 + 大参数特殊
  处理 + 精度对拍）。
- **验证**：shadow **17/17**（新增 rt_term/rt_fs/rt_args/rt_env/rt_math；args 特例
  以 `alpha beta gamma` 运行验 argc=4；env 用 C setenv→MYP get 一致性验同 environ
  源）、bootstrap 16/16（新 fixpoint `7f88f563…`，ir_emit 改）、全量 323/323。
  反汇编确认：fs.o 定义 12 个 `myp_fs_*` T 符号；env.o `myp_env_get` 有
  `R_X86_64_REX_GOTPCRELX environ` 重定位（真走 libc environ）。
- **工程修复**：`build.sh` 测试循环 `set -e` 会在二进制返回非 0 时**静默退出**
  （`exit=$code` 打印前被 errexit 杀死）→ 用 `set +e` 包运行取退出码再 `set -e`。

### v3.15.30 — runtime myp化 #27：精选汇编原语标准库 `runtime_myp/asm.myp`

**非破坏性**。在 #26 通用 `__myp_asm` 内建之上，封装**精选 x86-64 汇编原语**为
类型安全的标准库，供运行时基础设施使用（计时/自旋锁/内存屏障），不暴露裸 asm：

- 新建 `runtime_myp/asm.myp`：
  - `myp_rdtsc()` → long——x86 时间戳计数器（纳秒级、恒定频率）。**缓冲法规避
    `$` 立即数**：`rdtsc; movl %eax, (%rdi); movl %edx, 4(%rdi)` 写 8B arena 缓冲
    （`@static TscBuf.addr` 惰性分配），MYP 用 `__myp_mem_load_i64` 读回完整 64 位。
  - `myp_pause()` / `myp_mfence()` / `myp_lfence()` / `myp_sfence()`——自旋锁提示
    与内存屏障，统一 `~{memory}` clobber。
- 新建 `bench/freestanding/rt_asm_test.myp`：rdtsc 非零 + 单调（100 采样）、
  pause/三种 fence 可执行；接入 `runtime_myp/build.sh` 测试循环。
- **实测踩坑记录（重要）**：
  - LLVM 内联 asm 把 `$N` 当**操作数引用**——AT&T `$32` 立即数报 `Invalid $
    operand number`，与 MYP 字符串 `$` 插值双重重叠。→ asm 一律不写 `$` 立即数，
    需要立即数/返回值走寄存器或缓冲。
  - `\$` 转义**无效**：lexer 虽能生成 `\`，但 `parser.myp expandDollarInterp`
    会对字符串值重新扫描 `$`（char 36）→ 仍被当插值前缀。**三引号原始串
    `"""..."""`（raw=1）才是字面 `$` 的正确机制**（跳过展开）。
  - 曾临时给 lexer 加 `\$` 转义（2 处）——**已回滚**，净零变更（git diff 为空）。
- 验证：shadow **12/12**（新增 rt_asm_test）、bootstrap 16/16（fixpoint
  `dfe0f3b6…`，与 #26 相同，确认 lexer 回滚干净）、全量 323/323。

### mypview 框架变更 → 见 `mypview/CHANGELOG.md`

mypview 框架（控件/布局/UIX/AppRunner/示例/测试）的变更记录迁移到
`mypview/CHANGELOG.md`（v3.12.58 AppRunner 框架化 / v3.12.59 JsonEditor /
v3.12.60 UixDesigner 所见即所得设计器等）。本文件继续记录编译器/运行时/stdlib
变更；mypview 专用 stdlib 扩展（如 json bridge 编辑 API）在主 changelog 与
mypview changelog 双向引用。

### v3.15.29 — runtime myp化 #26：通用内联汇编内建 + 标准库封装 myp_ctx_switch

**非破坏性**。把 #25 的硬编码 `__myp_ctx_switch` builtin 重构为「**通用内联汇编内建 +
标准库抽象**」（用户建议）：

- **通用内建**（自举 codegen/sema）：`__myp_asm(asmStr, consStr, ...args)` → void /
  `__myp_asm_r(...)` → long——asm/constraints 为编译期常量串（`constStrVal` 递归求
  字面量/拼接），后续实参按各自 LLVM 类型作 asm 操作数，发射 `call asm sideeffect`。
  具体汇编用途**不再在 codegen 写死**。`__myp_fn_addr` 保留。
- **标准库封装**：新建 `runtime_myp/coro.myp`，`myp_ctx_switch(save, load)` 用通用
  `__myp_asm` 实现（配方同 §8：`;` 分隔 / `{rdi}{rsi}` / caller-saved clobber /
  `1f` 数字标签落空 epilogue / 不发 ret）。
- 删除硬编码 `__myp_ctx_switch` builtin（sema/codegen）；`rt_ctx_probe` 改调标准库
  `myp_ctx_switch`（FFI）。
- 验证：shadow **11/11**（rt_ctx_probe 走标准库）、bootstrap 16/16（新 fixpoint
  `dfe0f3b6…`）、全量 323/323。`CORO_DESIGN §8.1` 更新架构说明。

### v3.15.28 — runtime myp化 #25：`__myp_ctx_switch` 内联汇编上下文切换探针通过

**非破坏性**。协程核心可行性验证：自举编译器新增两个内建——`__myp_fn_addr("name")`
（`ptrtoint ptr @name to i64` 取函数地址，设协程入口帧用）与 `__myp_ctx_switch(save,
load)`（内联汇编上下文切换，镜像 `coro_ctx.S myp_ctx_switch`，零 syscall）：

- **内联汇编配方（实测踩坑记录）**：指令分隔用 `;`（`\n` 不被 LLVM 内联 asm 解析、
  `%=` 标签不被该版本处理）；寄存器用 `%reg`（`%0` 报 invalid register name）；
  操作数用 `{rdi}/{rsi}` 硬编码约束（不能 `$0/$1`——MYP 字符串里 `$` 是插值前缀）；
  恢复地址 `leaq 1f(%rip), %rax` + `jmpq *%rax` + 数字标签 `1:` 落在 asm 块末尾 →
  resume 落空到 LLVM epilogue 正常返回（**不发 ret**）；**clobber 必须列全部
  caller-saved**（上下文切换只保存 callee-saved，worker 破坏 caller-saved 如 run()
  的 rdx 活值 → 不列会段错误）；ctx 是 8B 槽（存 rsp），入口地址≠rsp（实测把入口当
  rsp 会跳进代码段崩溃）。
- **探针通过**（`bench/freestanding/rt_ctx_probe.myp`）：main→worker（4KB arena 栈）→
  main 双切换 + entryHit 标记，exit=0。**协程 yield/resume/调度可全 MYP 化，消除
  coro_ctx.S 依赖**；剩余前置 = 异常边界 setjmp + 线程创建（OS 边界）。
- 验证：shadow **11/11**（新增 rt_ctx_probe）、bootstrap 16/16（新 fixpoint
  `5f8fe4db…`）、全量 323/323。`CORO_DESIGN.md §8` 记录探针结论与内联汇编配方。

### v3.15.27 — runtime myp化 #24：类对象 release 分发全 MYP 化（无 C 胶水）

**非破坏性**。**策略变更（用户定）**：mypc 不再冻结、**不留 C 胶水**——运行时↔程序生成
代码的硬边界也要全 MYP 化。`myp_release_class_obj_ex`（类对象 rc→0 后查程序生成的
`__myp_release_table` 分发 destroy stub）不再委托 C helper：

- **自举编译器新增两个内建**（runtime_myp 模块由自举编译；mypc 不用改——它已用
  `ExternalLinkage` 发射同名 `@__myp_release_table`）：
  - `__myp_rtable_addr()` → long：发射 `ptrtoint ptr @__myp_release_table to i64`。
    运行时模块自身也定义同名表，但**程序 .o 链接在前 + `--allow-multiple-definition`
    → 程序表胜出**，引用解析到程序真实表（含 destroy stub 地址）。
  - `__myp_call_ptr(long addr, string obj)` → void：发射 `inttoptr` + 间接
    `call void %fn(ptr %obj)`（LLVM 21 opaque ptr 下合法）。
- **`runtime_myp/alloc.myp` 实现 MYP `myp_release_class_obj_ex`**：weak 通知 →
  `__myp_rtable_addr` 取表 → `__myp_mem_load_i64(table + tid*8)` 读 stub →
  `__myp_call_ptr` 间接调 destroy stub（级联释放引用字段 + `myp_free_object`）；
  无 stub（tid<=0）→ 直接 `myp_free_object`。删除原 `ffi` 委托。
- 验证：新增 `bench/freestanding/rt_cls_release_test.myp`（Outer 强持 Inner 字段，
  释放 Outer → destroy stub 级联释放 Inner，Live 对象计数回基线；200 轮循环无泄漏）。
  shadow **10/10**、bootstrap 16/16（新 fixpoint `8eca1f53…`，编译器源码变更）、
  全量 323/323。反汇编确认 MYP 分发（`lea @__myp_release_table` + `call *reg` 间接
  调用 destroy stub）。

### v3.15.26 — runtime myp化 #23：时间层 myp_now_ms / myp_now_realtime_ms / myp_sleep_ms

**非破坏性**。新增 `runtime_myp/time.myp` shadow C runtime 时间函数（Time.nowMs /
Process.sleep 及 C 内部通道/协程/定时器广泛依赖），纯 raw syscall：

- `myp_now_ms`/`myp_now_realtime_ms`：`clock_gettime`（syscall 228，CLOCK_MONOTONIC=1
  / CLOCK_REALTIME=0）写 16B timespec 缓冲 → `sec*1000 + nsec/1000000`。
- `myp_sleep_ms`：`nanosleep`（syscall 35，req/rem 双缓冲）；EINTR（返回<0）时内核
  把剩余时间写进 rem → 拷回 req 重试（保持近似 ms 语义，同 C 版）。
- 缓冲 arena 一次性分配 + `@static` 缓存地址（io.myp/region.myp 同模式；⚠️ 非 TLS，
  多线程并发共享缓冲有纳秒级竞态，当前测试全单线程，文档化限制）。
- 验证：新增 `bench/freestanding/rt_time_test.myp`（realtime epoch 量级 / 单调 100
  采样 / sleep 50ms+1050ms 实际睡眠，跨秒验 nanosleep rem 路径）；shadow **9/9**、
  bootstrap 16/16（fixpoint 不变）、全量 323/323。反汇编确认 MYP 版（含
  `timeBufAddr` 符号 + inline syscall）。
- 附带清理：删除 str.myp 过时 TODO（`myp_str_split_get` 早已 MYP 化，上版误判）。

### v3.15.25 — runtime myp化 #22：字符串拼接层 myp_str_append（arena 原地扩展）

**非破坏性**。`runtime_myp/str.myp` 新增 `myp_str_append`（`s = s + x` 快路径），
`runtime_myp/alloc.myp` 增加 `last_base`/`last_aligned` 追踪 + `myp_arena_alloc_ex`
（grow 变体）+ `myp_alloc_str_grow`：

- **MYP 版**：`s = s + x`（owned 局部双字符串）经 codegen 发射 `@myp_str_append` +
  普通 store（消耗 s）。s 唯一（rc==1）且是 arena 最后一次分配 → **bump 原地扩展**
  （O(1) 均摊，写 x + 更新 len 字段 + rc 归零由 return-retain 补 1）；否则回退手动
  拷贝（`myp_alloc_str_grow` 2x 增长头）+ release(s)。
- **消除 C 版 latent bug**：C `myp_str_append` 对 rc==1 字符串读假头
  `node=data-sizeof(node)` 再 `realloc` —— shadow 下内存是 arena → realloc 假头/
  崩溃（此前 shadow 用例未触发因快路径需 owned 局部）。
- **ARC 约定（实测确认）**：`return <owned 局部>` = retain + releaseArcSlots →
  净 rc 不变；`return <参数>` = 仅 retain → 返回前须置 rc=0 让 retain 补成 1。
- **修复 shadow 内存爆炸**：回退若用普通 `myp_alloc`（chunk 恰好够），字符串每涨
  16B 就回退重拷整串 → 500k 累积实测 **7.5GB RSS**。`myp_alloc_str_grow` 给新
  chunk 2x 头 → 回退点几何递增（64K/128K/256K/512K）→ **O(n) 摊销**：500k 累积
  wall=0.00s、RSS=3MB、原地命中 499996/500000。
- 验证：`bench/freestanding/rt_str_test.myp` 新增 `RtAppendProbe`（2000 小累积 +
  自拼接 + 40000 跨 chunk 大串 + 空串起始）；shadow 8/8、bootstrap 16/16
  （fixpoint md5 不变）、全量 323/323。

### v3.15.24 — 浮点精确路径：%.f 大数 / %g·%e 高精度（uint32 大整数精确十进制展开）

**非破坏性**。`runtime_myp/float.myp` 新增**精确十进制展开**（uint32 小端词大整数，
≤40 词覆盖 subnormal 1074 位），解除 #19 的精度上限：

- **%.f |v|>=2^53**：double 反复 /10 会丢低位 → 改用大整数 `m<<e` 反复 /10 出
  精确整数位（2^63、1e100 等逐位正确）。
- **%.f prec>15**：精确小数展开 `rem/2^r`（分母仅 2 因子 → 十进制有限），每步
  **先 rem*=10 再取高 r 位**（digit=floor(rem*10/2^r)）——%.60f 的 0.1 给出完整
  55 位精确展开。
- **%g/%e P>15**：精确 P 位有效数字 + round-half-even（%g 仍去尾零），支持到
  prec=60。
- **发现 C runtime 缺陷**：`myp_fmt_double_f` 用 `char buf[160]` + snprintf →
  %.f 大数（如 1e308，309 位）被**截断到 159 位**；MYP 精确路径给出**完整精确值**
  （与 Python `format(1e308,'.0f')` 逐位一致）——**MYP 更正确**。
- 重构：`fmtSciBody`/`fmtFixedG` 尾部抽为 `assembleSci`/`assembleFixed`（快/精两
  路径共用），dispatch：%.f 在 `expf>=1076`（|v|≥2^53）或 prec>15、%g 在 prec>15、
  %e 在 prec>14 走精确路径。
- 验证：`rt_float_prec_test.myp` 24 断言（大整数 9 + 高精度小数 5 + %g P>15 7 +
  %e 4）C/shadow 双跑全过；次正规数 5e-324（r=1074）对拍一致；shadow 8/8、
  bootstrap 16/16、全量 323/323。

### v3.15.23 — 浮点层性能基准（MYP shadow vs libc，~1.1-1.26x）

**非破坏性**。新增 `bench/freestanding/rt_float_bench.myp` + `run_float_bench.sh`
复现脚本：`myp_atof`（strtod 解析）+ `myp_to_string_double`/`myp_fmt_double_f/e/g`
（%g/%e/%f 格式化）各 160 万次，C runtime（libc）vs MYP shadow（float.myp）对比，
`psum`/`fsum` 作位一致校验。实测（rounds=200000，8 操作/轮）：

| 操作 | C runtime (libc) | MYP shadow (float.myp) | 倍数 |
|------|------------------|------------------------|------|
| atof 解析 | ~30 ns/op | ~33-35 ns/op | **~1.1-1.17x** |
| fmt 格式化 | ~94-95 ns/op | ~117-118 ns/op | **~1.23-1.26x** |

- **位精确等价**：`psum=2e+105`、`fsum=11800000` 两模式完全一致。
- 反汇编确认 shadow 生效：shadow 二进制含 `myp_pow10`/`myp_math_pow`
  （MYP float.myp 特有符号），C 二进制无（`myp_atof` 直接 libc `atof`）。
- 结论：纯 MYP 重实现 strtod + %g 距 glibc 高度优化版仅 ~10-26%（`__myp_math_pow`
  小整数指数有 glibc 快路径 + LLVM 原生代码）；checksum 一致性同时佐证 #19 位精确。

### v3.15.22 — runtime myp化 #19：浮点层（strtod 解析 + %g/%e/%f 格式化）

**非破坏性**。新增 `runtime_myp/float.myp` shadow C runtime 的浮点解析与格式化
（去 libc `strtod`/`snprintf %g` 依赖，纯 MYP + `__myp_math_pow/floor/log` 内建）：

- **解析**：`myp_atof` / `myp_str_to_double` / `myp_str_to_float`（strtod 语义：
  跳过空白/可选符号/小数点/指数 `e/E`、`0x` 十六进制浮点（`p` 二进制指数）、
  `inf/infinity/nan`；无有效数字回 0；溢出→±inf、下溢→0）。
- **格式化**：`myp_to_string_double`/`myp_to_string_float`（`%g` 默认精度 6）、
  `myp_fmt_double_f/e/g`（`%.*f/e/g`）、`myp_print_float`（Console.writeFloat
  路径）。算法：十进制指数 `X=floor(log10|v|)`（log 粗估 + pow10 比较校正）；
  `%g` 在 X<-4 或 X>=prec 走 %e 风格否则 %f 风格；有效数字 `|v|/10^(X-P+1)`
  缩放到 `[10^(P-1),10^P)` 后 **round-half-even** 取整（P<=15 精确），%g 去尾零。
  `%.*f` 整数部分反复 /10 + 小数部分反复 *10 并按第 prec+1 位 half-even 进位。
- **验证**：`rt_float_test.myp` 111 断言（解析 20 + %g 17 + %f 12 + %e 8 + %g 11 +
  myp_print_float 3），期望值 = glibc strtod/snprintf 输出——C runtime 与 MYP
  shadow 双跑全过（字节对拍一致）。shadow 全套 7/7、bootstrap 16/16、全量 323/323。
- 已知边界：%g/%e 有效数字 P 截到 15（超出近似，实际极少用）；`%.*f` 对
  |v|>=2^53 的整数部分取位为近似；幂缩放用 libm `pow`（正确舍入 <1ulp）。

### v3.15.21 — 字符串头 len 字段：myp_strlen O(1)（根治 __strlen_evex 热点）

**非破坏性**。字符串 ABI 布局升级：计数字符串头从 8B `{rc, type_id}` 扩为
**12B `{len, rc, type_id}`**（`len` 在 data-12，不含 NUL；`rc/type_id` 仍在
data-8/-4 与类对象同偏移 → `myp_retain`/`myp_release`/`__myp_obj_type_id` 分发
零改动，仅字符串分配/释放/原地 realloc 路径用 `MYP_STR_HEADER_SIZE=12`）。

- **`myp_strlen` → O(1)**（C runtime + MYP shadow `str.myp` 都改读 len 字段）。
  旧版 C 走 libc strlen（perf 自举 67% 热点）、MYP 版逐字节扫描，均每次 O(n)。
  实证：200KB 串 × 200 万次 `Str.len` = **0.012s**（旧 O(n) 需扫 ~200GB）。
- **写 len 点全覆盖**：C `myp_alloc_str`、MYP shadow `myp_alloc`（12B 头）、两
  编译器字面量发射（`{i32 len, i32 rc, i32 type_id, [N x i8]}`，GEP 到元素 3）、
  C `myp_str_append` 原地 realloc 同步更新 len。
- **顺带修复 len 字段暴露的 3 处 bridge 潜伏 bug**（预分配 cap 缓冲直接返回 →
  len 字段=cap 而非实际长度；旧 strlen 扫描掩盖）：`myp_net_recv`（超时/EOF 空
  串 len=0、成功按实收 n 构建）、`myp_uds_recv`（同）、`myp_process_output`（按
  实际输出长度构建）。`async_socket` 超时路径 `timeout_len=0` 恢复正确（此前实测
  len=10 的 NUL 串）。
- **构建顺序（bootstrap 一致性）**：先改自举 codegen 字面量 → 用旧编译器双重建
  myp_self2（v1 新发射/旧内部、v2 新发射/新内部）→ 再切 C runtime + C++ 编译器
  → 重建 libmyp_rt.a/mypc → 用 v2 构建 v3（新内部 + 新 runtime）为最终 myp_self2。
- 验证：bootstrap 16/16（新定点 md5 `1def2c4e…`）、全量 323/323、shadow 6/6
  （str/num/alloc/region/weak/io）、自举自编译 16.2s 持平。

### v3.15.20 — runtime myp化 #17：异步文件读接管（myp_coro_file_read_line/all 同步读）

**非破坏性**。`runtime_myp/io.myp` 接管 C 的协程文件读入口
（`File.readLineAsync/readAllAsync` → `myp_coro_file_read_line/all`）：

- **修复 readAll shadow 破坏**：C 版 `myp_coro_file_read_all` 经
  `myp_io_lock_handle(io_handle)` 读 C 的 `FILE*` 表——MYP 表接管后该表恒空，
  shadow 下 readAll 返回空串（自举编译器 `tools/selfhost/src/link.myp`、
  `main.myp` 都依赖 readAll）。
- `myp_io_read_line` 重构为 handle 参数化 `ioReadLineHandle(handle)`（pending
  字节优先 + 逐字节到 \n/EOF + arena 4096 scratch），当前句柄读变包装。
- 新增 `ioReadAllHandle(handle)`：lseek(SEEK_CUR/SEEK_END) 取大小后回位分块读，
  lseek 失败（流式）自动退化为逐字节；字节级保真（含 \n，不剥）。
- **已知限制**：MYP 版为**同步**读（数据正确）；C 版协程 parking + exec
  worker 的非阻塞 await 语义未 MYP 化（依赖 C FILE* 表，shadow 后不适用）。
  323 套件的 `async_file` 测试走 C runtime 不受影响。
- 验证：bootstrap 16/16，全量 323/323，`rt_io_test` 新增 readAll 全文件读用例
  全部 exit=0。

### v3.15.19 — runtime myp化 #16：文件 I/O 层（myp_io_* raw syscall 实现）

**非破坏性**。`runtime_myp/io.myp` shadow C runtime 的整个文件 I/O 层
（File 类经 `__myp_io_*` 内建 → `myp_io_*`），**纯 raw syscall、无 libc stdio**：

- **open=2 / read=0 / write=1 / lseek=8 / close=3**；mode 串 → open flags
  （r/w/a + `+` 加 RDWR，新建权限 0644）；fd 表 64 槽（每槽 16B `[fd][pending]`，
  fd=-1 空闲）+ **add-原子自旋锁**（复用 `__myp_atomic_add_i32_addr`）。
- **`myp_io_cur`（当前句柄）留在 C TLS**（io_thread 并发 File 依赖每线程独立
  当前句柄），经新增 C helper `myp_io_cur_get/set` 读写——MYP 管 fd 表+操作，
  C 管每线程当前句柄，二者正确分工。
- 覆盖：fopen/current_handle/select/fclose/read_line/write/write_line/has_next/
  read_byte/read_i32be/read_double/write_byte/write_i32be/write_double/seek。
  `has_next` 用预读 1 字节存 pending（feof 语义）；`read_line` 逐字节到 \n/EOF；
  读写用 arena scratch 缓冲（I/O 量小，bump 不归还可接受）。
- 已知限制：`myp_io_table`/locks 从 C 的 FILE*+pthread mutex 换成 MYP fd+自旋锁
  （语义等价）；arena scratch 每次分配（不回收）。

验证：shadow 测试新增 **rt_io_test**（写文本/行/字节/大端 int/double → 读回 →
EOF 检测）exit=0；真实 io 测试用 MYP shadow 全过——**io_multi**（多文件交替读写
alt=A1B1A2B2）、**io_thread**（两个 @thread 并发 File 各写 200 行，a=200 b=200
wrong=0，验证 TLS 当前句柄 + 自旋锁）。bootstrap 16/16、全量 323/323。build.sh
循环新增 rt_io_test。

### v3.15.18 — runtime myp化 #15：类 slice 清理链 + @weak 弱引用注册表

**非破坏性**。补齐内存核心的最后两块（此前评估为"C 无法触及/死代码"的部分）：

**① `myp_alloc_class_slice` + `myp_release_class_slices_from_depth`**（region.myp）：
slice<类> backing 分配时按当前 `Region.depth` 注册进 @static 清理链
（node `[next@0][data@8][depth@16]`）；`myp_arena_release` 入口先释放 depth >=
当前值的注册 backing（保留外层 region 的），再回卷 arena（镜像 C）。当前无 codegen
调用方（slice backing 走 ref-counted `myp_alloc_slice_backing`），完整覆盖。

**② `@weak` 弱引用注册表**（新 `runtime_myp/weak.myp`）：
shadow C 的 `myp_weak_store`/`myp_weak_load`/`myp_weak_clear`。@static 全局单链表
注册表（C 用 64 桶哈希；weak 稀少线性扫描即可）+ **add-原子自旋锁**（用已有的
`__myp_atomic_add_i32_addr`——atomicrmw add 返回旧值，0=获取；无需新内建）。
`myp_alloc` 的 `myp_release` 类分支先调导出的 `myp_weak_notify_death`（null 本注册
表观察此对象的弱槽；并发 weak_load 在锁内重 bump rc → 返回 0 不释放），再委托
`myp_release_class_obj_ex`（其对 C 注册表做冗余 notify——MYP shadow 全部弱入口后
C 注册表恒空，不干扰）。弱槽地址经 raw 内存 `__myp_mem_load/store_i64` 读写。

验证：shadow 测试新增 **rt_weak_test**（弱存储不 retain / 弱读升级 rc+1 / 目标销毁
自动置空 / 持有者销毁注销）exit=0；真实 weak 测试用 MYP shadow 全过——weak_cycle
（断环+自动置空）、weak_multi_sub（多订阅者）、**cross_thread_arc（多线程 @weak，
验证自旋锁）**。bootstrap 16/16、全量 323/323。build.sh 循环新增 rt_weak_test。

### v3.15.17 — runtime myp化 #14：@region 层（myp_region_alloc + mark/release + 诊断）+ 修复 #13 类数组级联 bug

**非破坏性**。`runtime_myp/region.myp` shadow C runtime 的 @region 两级内存 region
层：

- **`myp_region_alloc`**：depth>0 用 MYP region bump，否则落到 MYP 进程 arena
  （`myp_arena_alloc`）。当前自举 codegen/stdlib 无调用方（slice/数组 backing 全走
  ref-counted `myp_alloc_slice_backing`），shadow 以完整覆盖。
- **`myp_arena_mark`/`myp_arena_release`**（codegen 在 @region 出入口发射）：mmap
  chunk 水位跟踪——mark materialize 一个 64KB chunk 并返回水位，release munmap 比
  水位新的 chunk 并回卷 used；嵌套 depth 计数。`myp_region_free_all` + region 字节
  诊断。
- **状态存 `@static class Region` 全局**（非 C 的 TLS）：@region 多线程并发会共享
  region 状态——现有测试全部单线程 @region，多线程场景暂未使用（文档化限制）。
- **修复 #13 潜伏 bug（alloc.myp `myp_release` 数组分支偏移）**：`pad` 应在
  **data-12**（非 -16，-16 是 elem_size）、`elem_size` 在 data-16（非 -20）。此前
  pad 读成 elem_size → `slice<Node>`（elem_size=8）pad 读 8 → CLASS/SLICE 逐元素
  级联永不执行 → 类数组/类 slice 元素泄漏。region_slice_class_arc 的 `live=128`
  暴露。rt_alloc_test 新增 **testClassArray** 级联回归。

验证：shadow 测试 rt_str_test/rt_num_test/rt_alloc_test/rt_region_test 全 exit=0；
region_slice_class_arc（live=0）与 region/test.myp 用 MYP shadow 输出正确；
bootstrap 16/16、全量 323/323。build.sh 循环新增 rt_region_test。

### v3.15.16 — runtime myp化 #13：内存核心——mmap bump arena + 分配/释放集群全量 MYP 化

**非破坏性**。核心分配器/ARC 层完整 MYP 化（`runtime_myp/alloc.myp` shadow C
runtime 的 `myp_alloc`/`myp_alloc_object`/`myp_alloc_class_array`/
`myp_alloc_slice_backing`/`myp_release`/`myp_free_object` + M9 存活计数）：

- **mmap bump arena**：`myp_arena_alloc` 用 `__myp_syscall`(mmap) 取块，16 对齐，
  chunk 头 32B（next/cap/used），超大分配独占块，bump 不归还（进程退出 OS 回收）。
  状态存 `@static class Arena` 全局（--shared 库模式验证可用）。
- **对象头布局与 C 一致**：字符串/类对象 8B 头 `{rc,type_id}` 在 data-8/-4；数组
  24B 头 `{count:u64,elem_size:u32,pad:u32,rc:u32,type_id:u32}`，rc/type_id 仍
  data-8/-4。跳过 C 的 16B 侵入链表 node（C 链表恒空，exit 清理无害）。
- **关键修复（ARC ABI 对齐）**：自举 codegen 对 `return __myp_addr_to_str(...)`
  （非 fresh 内建）发射 retain-on-return(+1)。MYP 分配器内部 rc 须设 **0**，让该
  retain 补成 1（等价正常代码 new 后 return 的净 rc=1）。此前设 1 → 返回 rc=2 →
  调用方 release 到 1 不归零 = 每字符串泄漏 1 引用，并引发巨型 main() 的
  `!= ""` 槽装载错乱（live 计数失控 → 内存布局偏移）。
- **myp_release 全分发**：rc→0 按 type_id——数组（pad 0=类逐元素 release / 2=slice
  胖指针逐元素）、字符串（计数-1）、类对象委托新增 C helper
  **`myp_release_class_obj_ex`**（weak 通知 + 每程序 `__myp_release_table` 分发 +
  `myp_free_object`——weak 注册表是 C 静态、release 表是生成代码产物，MYP 无法
  触及）。`myp_free_object` 经 --allow-multiple-definition interpose 回 MYP。
- **计数**：`Live.strings/objects/arrays` @static 全局（替代 C TLS），shadow
  `myp_live_*_count`。
- 已知限制：strict 头校验/逐类型计数/mmap 块 exit 回收未迁移（诊断性，非正确性）。

验证：**rt_str_test（260 字符串检查+ArcProbe）exit=0**、**rt_num_test exit=0**、
**新增 rt_alloc_test（字符串/数组/类对象/大块分配+release+计数）exit=0**；
bootstrap 16/16、全量 323/323。build.sh 循环新增 rt_alloc_test。

### v3.15.15 — runtime myp化 #12：核心 ARC/分配器层第一步（raw-address 原子内建 + myp_retain + myp_obj_type_id）

**非破坏性**。核心层（分配器/ARC）最难的障碍之一是**任意地址原子操作**——现有
`__myp_atomic_*` 内建是数组+下标寻址（GEP），不适用 rc 字段。自举新增 4 个
**raw-address 原子内建**（long 地址 → inttoptr → `atomicrmw`/`load atomic`/`store
atomic` seq_cst）：

- `__myp_atomic_add_i32_addr` / `__myp_atomic_sub_i32_addr` /
  `__myp_atomic_load_i32_addr` / `__myp_atomic_store_i32_addr`（sema + codegen）。

`runtime_myp/arc.myp` 迁移核心层第一步：

- **`myp_retain`**：对象头 `{_Atomic rc; type_id}` 在 data-8/data-4，`atomicrmw add`
  at (obj-8)。
- **`myp_obj_type_id`**：读 (obj-4)（RTTI，字符串=0xFFFFFFFE/-2）。

**核心层完整迁移的障碍评估**（`myp_release`）：rc→0 后按 type_id 分发——数组逐元素
release + `myp_free_class_array`、字符串 unlink 侵入分配链表 + free、类对象走
**每程序生成的 `__myp_release_table[tid]`**（codegen 产物，MYP 模块无法引用程序全局）
+ weak 表通知。完整 MYP 化需：raw-address 原子（本轮已加）+ mmap 自建分配器 +
程序全局访问机制——另立里程碑。

验证：bootstrap 16/16、全量 323/323、runtime_myp shadow PASS（ArcProbe：retain→rc+1、
type_id=-2）。

### v3.15.14 — runtime myp化 #11：SHA-256 摘要

**非破坏性**。`runtime_myp/hash.myp` 新增 `myp_hash_sha256`（bridge hash_bridge.c
纯函数，返回 64 位小写 hex）。全 `uint`（i32）算术——加法自然回绕、`uint >>` 发射
逻辑右移（SHA-256 的 Σ/σ 用 `>>>` 语义）、`rotr` 内建。K/H 常量用 `uint(0x…)`（hex
大字面量解析为 long 再截断）。固定数组 `uint[64] w/K`、`uint[8] hv` 作局部（传参会
触发 GEP 类型错，仅局部可用）。

标准向量全过：`""`→e3b0c442…b855、`"abc"`→ba7816bf…15ad、`"hello"`→2cf24dba…9824、
`"The quick brown fox…"`→d7a8fbb3…e592。验证：runtime_myp shadow PASS。

### v3.15.13 — runtime myp化 #10：CRC-32 校验和 + shadow 加固边缘测试

**非破坏性**。`runtime_myp/crc.myp` 新增 `myp_crc32`（zlib CRC-32，位运算法无查表，
多项式 0xEDB88320，返回 32 位位型）。标准向量验证：`""`→00000000、`"a"`→e8b7be43、
`"hello"`→3610a686、`"123456789"`→cbf43926（经典校验值）。

**shadow 加固**：shadow 机制修复（#9）后 MYP 实现真正执行——rt_str_test 加 11 条、
rt_num_test 加 7 条边缘用例（空 old 替换、纯空白 trim、尾分隔符 split、空前缀、
大写 hex、u64 2^63 等）全部通过，未发现新 bug。

验证：runtime_myp shadow PASS（str+num，MYP 版本真实执行）。

### v3.15.12 — runtime myp化 #9：base64 层 + **shadow 机制修复**（此前测试走 C runtime）+ 2 个真实 bug

**非破坏性**。`runtime_myp/base64.myp` 新增 `myp_base64_encode`/`myp_base64_decode`
（bridge 纯函数，shadow 验证；rt_str_test 加 12 条断言含 round-trip）。

**⚠️ shadow 机制修复（关键）**：此前 `runtime_myp/build.sh` 编译 MYP 模块**未加
`--shared`** → 函数是 `define internal`（局部符号 `t`），无法 shadow libmyp_rt.a 的
同名全局符号 → **之前所有 "shadow PASS" 实际测试的是 C runtime，MYP 版本从未被真正
调用**。同时 build.sh 模块循环复用同一 `/tmp/rt_myp_m.o` 路径，后编译模块覆盖前面的。
修复：`--shared`（库模式，函数外部链接 `define`）+ 每模块独立 `.o`。反汇编确认
`myp_strlen` 现为 MYP 的 charcode 扫描循环（非 C `call strlen`）。

**shadow 生效后抓到的 2 个真实 bug（均修复）**：
1. **`myp_charcode` 自递归**：MYP 版内部用 `__myp_charcode` 内建（发射 `call @myp_charcode`）
   → 无限递归段错误。改用 raw-memory 直接读字节 `__myp_mem_load_i8(ptr(s)+i)`。
2. **`myp_str_replace_all` 空 old_str 死循环**：`oldl==0` 检查放在 count 循环之后——
   oldl=0 时 `j<oldl` 循环不跑 → found=k=i → i 不前进 → 死循环（perf 定位 99.9%
   在该函数）。C 版对 `!*old_str` 提前返回拷贝，对齐修正。perf 定位：完整测试在
   多次分配后堆布局下触发，gdb/ASAN 布局不同不触发（掩蔽性极高）。

验证：runtime_myp shadow PASS（str+num 全断言，MYP 版本真实执行）。

### v3.15.11 — runtime myp化 #8：热字符串助手 myp_ord/charcode/chr/strdup

**非破坏性**。`runtime_myp/str.myp` 补 4 个高频小函数（shadow C runtime 验证，
rt_str_test 扩 9 断言）：

- `myp_ord` / `myp_charcode`：O(1) 取字符码（自举 lexer 等按字符扫描热路径）。
- `myp_chr`：码点 → 1-4 字节 UTF-8 计数字符串（非法码点 → 0xFFFD 替换符）。
- `myp_strdup`：拷贝计数串（含 NUL）。

**性能剖析结论（perf 自举编译自身）**：myp_* 函数自身耗时很小（最高 myp_release
1.25%）；真正的性能大头是 **`__strlen_evex` 67%**（libc strlen）——字符串无长度
字段，每次操作 O(n) 重扫。根治 = 字符串头加 len 字段（另立任务，影响 runtime 布局
+ 两编译器字面量 GEP）。**按静态调用点，剩余最多的是分配器/ARC
（myp_release 22k / retain 4k / alloc_object 862）——最难且 self 耗时小，另评。**

### v3.15.10 — runtime myp化 #7：bytes 层（bytes()/str(bytes)/bytesOf 三转换）

**非破坏性**。`runtime_myp/bytes.myp` 新增（codegen 直接发射的三个 bytes 转换，
shadow C runtime 验证）：

- **`myp_str_to_bytes`**：string → `ubyte[]` backing（`new ubyte[n]` + 逐字节拷贝，
  返回类型 `ubyte[]` 即 LLVM ptr，匹配 codegen `call ptr`）。
- **`myp_bytes_to_str`**：`ubyte[]` backing → string（count 在 data-24 头字段读长度）。
- **`myp_uint_to_bytes`**：位向量按小端 → `ubyte[]`（§5.1 `bytesOf`，nbytes 钳 1..8）。

**技巧**：`__myp_str_ptr(数组值)` 内部就是 `ptrtoint ptr %x to i64`，对任意 ptr 值
（含数组 backing）成立——拿数组地址无需新内建。`bitcast` 内建**不能** ptr→i64
（LLVM 需 ptrtoint）。

验证：runtime_myp shadow PASS（rt_num_test 加 7 条 bytes 断言：round-trip
`str(bytes("hello"))`=="hello"、空串、`bytesOf(0x1234)`→[52,18,0,0] 小端）。

### v3.15.9 — runtime myp化 #6：浮点位型 myp_f64_bits_hex / myp_f32_bits_hex

**非破坏性**。`runtime_myp/num.myp` 补浮点位型十六进制——自举 codegen 发射 double
常量时调用 `myp_f64_bits_hex`（LLVM 文本 IR 浮点常量 `0x + 16 大写 hex`）：

- **`myp_f64_bits_hex(double)`**：用 `bitcast<T,U>(x)` 内建（LLVM bitcast 指令）
  取 64 位位型 → 逐 nibble 大写 hex。
- **`myp_f32_bits_hex(float)`**：float 先精确拓宽为 double 再取 64 位（对齐 C 版）。
- `bitcast` 内建验证：`bitcast<long>(1.0)` 正确发射 `bitcast double to i64`。

验证：runtime_myp shadow PASS（rt_num_test 加 7 条位型断言，如
`myp_f64_bits_hex(-0.0)`→"0x8000000000000000"、`pi`→"0x400921FB54442D18"）。

### v3.15.8 — runtime myp化 #5：通用拼接 myp_strcat + 进制格式化 myp_fmt_u64_base

**非破坏性**。补两个自包含转换（shadow C runtime 验证，str+num 测试扩断言）：

- `myp_strcat`（str.myp）：通用字符串拼接——自举 codegen 对 `s + t` 发射
  `@myp_strcat`（非 `s=s+x` 快路径），MYP 化后所有拼接走 MYP 版本。
- `myp_fmt_u64_base`（num.myp）：32 位位型按无符号在 2..16 进制输出（upper 控制
  hex 大小写），`Fmt.u/x/X/o/b` 全部走这里（`Fmt.x(-1)`→"ffffffff"）。

float/double 解析（strtod/atof）与格式化（%g、myp_fmt_double_f/e/g）依赖 libc，
另立里程碑（TODO）。验证：runtime_myp shadow PASS（str+num 全断言）。

### v3.15.7 — runtime myp化 #4：数字层（整数解析+格式化，9 函数）+ 自举 FFI 实参转换修复

**非破坏性**。`runtime_myp/num.myp` 新增（shadow C runtime 验证，`runtime_myp/build.sh`
现在同时运行 rt_str_test + rt_num_test）：

- **整数解析**（strtoll/strtoull base-0：空白/符号/0x 十六进制/0 八进制/十进制）：
  `myp_str_to_long` / `myp_str_parse_int` / `myp_str_to_uint` / `myp_str_to_ulong`。
- **整数/布尔格式化**（itoa 2 位查表法，INT32/64_MIN、u32/u64 最大值安全）：
  `myp_to_string_i32` / `myp_to_string_i64` / `myp_to_string_u32` /
  `myp_to_string_u64` / `myp_to_string_bool`。`ulong` 除法发 udiv/urem 处理位模式。
- float/double 解析（strtod/atof）与格式化（%g）依赖 libc，另立里程碑（TODO）。

**自举编译器 bug 修复（对拍 parity）**：`funcParamLts` 只查顶层函数、不查 FFI 声明
→ FFI 调用整数实参不提升（`myp_to_string_i64(0)` 传 `i32 0` 进 `i64` 参数，高 32 位
垃圾；oracle mypc 正确 sext）。补 FFI 分支后实参经 `convertValueU` 正确提升。此缺陷
此前被 ABI"低 32 位恰好在低位"掩盖（小值侥幸正确）。验证：bootstrap 16/16、
全量 323/323、runtime_myp shadow PASS。

### v3.15.6 — runtime myp化 #3：字符串层全部 MYP 化（22 个函数）+ 自举 O(N²) 修复

**非破坏性**。自举编译器（`tools/selfhost/src/*.myp`，mypc 冻结）推进运行时 MYP 化——
`runtime_myp/str.myp` 补齐字符串层全部函数（shadow C runtime 验证：
`runtime_myp/build.sh` + `bench/freestanding/rt_str_test.myp`，63 断言）：

- **新增 8 个**：`myp_str_split_get` / `myp_str_replace` / `myp_str_replace_all` /
  `myp_str_repeat` / `myp_str_pad_left` / `myp_str_pad_right` / `myp_str_reverse` /
  `myp_str_join`。至此 stdlib `text.myp` 的 18 个 `myp_str_*` FFI **全部 MYP 化**
  （22 个，含内部 len/eq/cmp/hash）。
- `myp_str_join` 用 `string[]` 数组参数——自举 codegen `varElemType` 已支持动态数组
  参数（为 `vecAdd` 等数组 @op 所加），无需改编译器。

**性能修复（自举编译自身 2m46→16s，10x）**：
- preamble declare 剔除从 `splitGet` 逐行（O(N²)：`myp_str_split_get` 每次从串头
  strstr 数到 index）改单遍 charcode 扫描 + 组合串判定（commit 3e74d3a）。
- `link.myp` `nmSymbols`/`nmDynSymbols` 同款 O(N²) → 新增 `Link.splitLines` 单遍切行
  （commit 6ef50ad）。`runtime_myp/str.myp` 预留 split_get 带偏移/split_all 的 TODO。

### v3.15.5 — 自举编译器同步 P1 缩放：sema/codegen 热路径 O(N) 扫描 O(1) 索引化

**非破坏性（性能，行为不变）**，自举（selfhost，`tools/selfhost/src/*.myp`）端
同步 v3.15.4 的 O(1) 索引化（此前只修了 C++ oracle，自举端 P6 仍 O(N²) 37x/2N）。

对照 `bench/compiler/`（N=1000，`myp_self2` 旧 vs 新）：

| 基准 | 旧 selfhost | 新 selfhost | 加速 |
|------|------|------|------|
| P1 类×裸属性 | 0.95s | 0.74s | 1.3x |
| P2 接口×方法调用 | 0.90s | 0.50s | 1.8x |
| P3 接口×变量声明 | 1.24s | 0.76s | 1.6x |
| P4 struct×字段 | 0.84s | 0.51s | 1.6x |
| P5 enum×variant | 0.71s | 0.44s | 1.6x |
| P6 类×方法调用 | 11.95s | **0.76s** | **15.7x** |
| P7 泛型实例 | 6.32s | 1.67s | 3.8x |

主要改动（`codegen.myp` + `sema.myp`，`StrHashMap<int>` 索引）：

1. **codegen**：`generate()` 入口一次建 `classIdx_`/`ifaceIdx_`/`enumIdx_`/
   `structExist_`（裸名+`Parent::name`）/`topFunc_`（函数+FFI）/`typeIdNames_`+
   `typeIdMap_`（type-id 表预计算，原 `classTypeId`/`classTypeNames`/`classTypeCount`
   每调用 O(N²) seen 去重）。`isClassName`/`isInterfaceName`/`isEnumName`/
   `isStructName`/`isTopLevelFunc`/`findAction`/`hasMethodInClass`/`hasEventInClass`/
   `findEvent`/`isStaticAction`/`classImplements`/`methodParamLts`/`methodRetAstType`/
   `methodParamAstType` 等改 O(1) `classIndex`/索引；`emitArcSupport` 的每类线性扫全类
   改 `classIndex`。

2. **sema**：`classIdx_`/`structIdx_`/`enumIdx_`/`ifaceIdx_`/`funcIdx_`/
   `methodSigIdx_`（`"cls.meth"`→索引，注册点增量登记）并行维护；
   `findClass`/`findStruct`/`findEnum`/`inInterface`/`isFuncName`/`isGenericClass`/
   `resolveBase`（精确命中）/`findClassTypeParams` 及 `findMethodRet*`/
   `findMethodParams`/`isMethodCoro`/`isFuncSectionMethod`/`hasMethod` 改 O(1)。

3. **P6 残余说明**：P6 由 O(N²)（2.24→11.88→78.90s@500/1k/2k）降到近线性
   （0.47→0.75→1.70→4.85s@500/1k/2k/4k，~2.3–2.9x/2N）；残余超线性为自举编译器
   **字符串处理 ARC 开销**（perf：`myp_release`+`myp_retain` 32%、`strcmp`+`myp_str_eq`
   14%——自举用字符串拼接生成 IR，每串引用计数），非线性扫描。

**回归**：bootstrap 16/16 不动点（oracle↔selfhost token/ast/sema 三方字节一致、
myp_self2==myp_self3）；selfhost 全量 323/323（`exception_thread` 为既有 @thread
时序 flaky，复跑通过）。

### v3.15.4 — 编译器缩放 P1：sema/codegen 热路径 O(N) 扫描 O(1) 索引化（P1–P6 线性化）

**非破坏性（性能，行为不变）**，oracle（mypc）端。根因：sema/codegen 在**每个
表达式/语句**的热路径里 `for (auto& cls : current_tu_->classes)` / `interfaces` /
`structs` / `functions` 线性扫全表（方法解析、接口判定、enum 构造、struct 查找、
@op/@coro/@async 函数查找、泛型实例复用查找），N 个类 × N 次调用 = O(N²)。
对应 `bench/compiler/`（P1–P7，`docs/testing_benchmark_roadmap.md` §5）实测斜率：

| 基准 | 修复前（16k 或 4k） | 修复后 | 现状 |
|------|------|------|------|
| P1 类×裸属性 | 10.26s@16k | 0.49s@16k | 线性（1.90x/2N） |
| P3 接口×变量声明 | 8.27s@16k | 0.48s@16k | 近线性（2.74x/2N） |
| P6 类×方法调用 fallback | 11.21s@16k | 4.67s@16k | 线性（1.92x/2N） |
| P7 泛型实例 | 15.8s@4k | 10.5s@4k | 剩余超线性见下 |

主要改动（`src/sema/*`、`src/codegen/*`、`include/mylang/*`）：

1. **O(1) 声明索引**：codegen `generate()` 入口一次建全 `class_decls_`/
   `interface_decls_`/`enum_decls_`/`struct_decls_`（key=name 或 `Parent::name`）/
   `first_member_class_`（成员名→首个定义类）；sema `analyze()` 建 `class_indices_`
   /`function_indices_`（含单态化后 `indexFunction` 增量登记）/`struct_by_name_`。
   `findClass`/`findEnum`/`findStruct`/`findClassDecl`/`findFunctionDecl` 全部改
   哈希查找，`isInterfaceName` 等新增辅助。

2. **热路径扫描替换**（逐个 perf 定位）：
   - `visitMemberAccess`：static 类判定、接口判定 → `findClassDecl`/`isInterfaceName`。
   - `visitBinaryOp`：`@op` 函数扫描 → `any_op_functions_` 短路；`@coro` 返回句柄
     扫描 → `any_coro_functions_` 短路。
   - `visitCall`/`isAsyncCallee`：函数扫描 → `findFunctionDecl`。
   - `resolveGenericCall`/`resolveGenericStaticCall`：实例复用线性扫函数 →
     `findFunctionDecl`。
   - `generateCallImpl`：接口 dispatch 的类名判定、enum variant 构造、静态类判定、
     本类属性对象解析 → `findClass`/`findEnum`。
   - `memberObjectClassName`/`callReturnTypeNode`/`generateNewExpr`/`generateIdentifier`
     /`generateMemberAccess`/`generateAssignment` 等 15+ 处 → `findClass`/索引。

3. **P7 剩余超线性说明**：sema 与 -O0 codegen 已降到斜率 <3.0（泛型实例查找/表扩容的
   MYP 侧 O(N²) 已消除）；-O2 默认管道下 P7 仍 ~3.4–3.6x/2N，perf 归因为 **LLVM
   SROA/mem2reg（`PromoteMem2Reg::run` 65%）**——N 个互异 struct 类型 + N 个泛型实例
   全部内联进单个 `run()`（1 基本块、~4N alloca）后，LLVM O2 管道在单函数上呈超线性，
   非 MYP 自身线性扫描。属 LLVM 内部成本，不阻塞其余基准线性化。

**回归**：oracle 323/323、selfhost 323/323、bootstrap 16/16 不动点；bench P1–P6
斜率全部 <3.0。

### v3.15.3 — 借用 ARC 参数重赋值 UAF 修复 + 测试框架可信度（T1/T2）+ selfhost 诊断输出修复

**非破坏性（bug 修复 + 测试基础设施）**，oracle（mypc）与 selfhost 双端同步：

1. **借用 ARC 参数重赋值 UAF（P0）**：`string f(string s){ s = s + "a"; s = s + "b";
   return s; }` —— 字符串/类参数是**借用**（非 ARC 槽），此前赋值路径要么走就地追加
   `myp_str_append`（消费借用的入口值 → 改写/释放调用方字符串）、要么 fresh 临时在语句
   末被 flush 释放 → 参数槽悬垂、链式重赋值读已释放内存。修复：借用参数**首次重赋值**
   惰性提升为拥有槽（fresh 消费 / 别名 retain，不释放借用的入口值，注册到**函数作用域**
   而非块作用域），后续重赋值走普通 owned-slot 路径。C++ `codegen_stmt.cpp` +
   selfhost `codegen.myp`（`funcPtrSlots_` + `funcPtrSlotHas`）双端镜像。回归
   `tests/@test/str_param_append.myp`（链式/循环/别名/类参数 7 断言，双编译器）。

2. **测试框架可信度（T1/T2，`docs/testing_benchmark_roadmap.md` §二）**：
   - **T1 缺失 expected 默认失败**：普通模式找不到 `tests/expected/*.expected` 不再
     静默把输出当基线并计 PASS → 报 `MISSING BASELINE` 且计 FAIL；仅 `--update`
     允许创建 baseline（漏提交测试资产 CI 必然失败）。
   - **T2 负测试校验诊断原因**：解析 `// EXPECT ERROR: <substring>` 并按固定字符串
     （`grep -F`）断言 stderr 含该子串；意外 SIGSEGV/SIGABRT/ASan 归类为 `CRASH`
     而非负测试通过。同步修正 27 个历史漂移的 `EXPECT ERROR` 注释（此前是人工描述、
     从未机器断言，与实际诊断不符）。

3. **selfhost 诊断输出修复（T2 暴露的既有 parity 缺口）**：
   - **UTF-8 双重编码**：`main.myp` `Frontend.escape`/`dotToSlash` 用 `__myp_chr(c)`
     把 `myp_charcode` 返回的**字节**当**码点**再 UTF-8 编码（0xE5 → c3 a5，中文诊断
     变 mojibake）→ 改用 `Str.substring(s, i, i+1)` 字节透传（对齐 `ast.myp Dump.esc`
     与 C++ `escapeDumpString`）。
   - **BUG-046 镜像**：selfhost 补同名 static 方法（签名不同）诊断
     `duplicate static action '...' in class '...' (different signature)`（`sema.myp`
     `staticActionSig` 签名串比较；签名相同保持历史静默合并）。

**回归**：oracle 323/323、selfhost 323/323（parity 0 差距）、bootstrap 16/16
不动点（myp_self2 == myp_self3 md5 一致）。

### v3.15.2 — 自举 link.myp 硬编码重构（P0 工具链探测 / P1 缓存路径 / P2 集中配置 / P3 平台）

`tools/selfhost/src/link.myp`（自举编译器链接器）去硬编码：

- **P0 工具链探测**：`findLlc`/`findOpt`/`findHostTriple` 改为数据驱动——通用
  `probeTool(name, env, versions)`：`MYP_*` 环境变量 → `command -v`（PATH 优先，含
  `<name>-<ver>` 版本化命令）→ 版本绝对路径候选表（`/usr/bin/<name>-N`、
  `/usr/lib/llvm-N/bin/<name>`）→ 回退裸命令名。`$CC` 环境变量优先 + `command -v
  cc`/`gcc` 探测。
- **P1 缓存路径**：固定 `/tmp/myp_self_*.o`（并发 myp_self 互相覆盖）→ 内容哈希缓存
  `FNV-1a 64`（源码+标志 → `/tmp/myp_rt_cache/myp_rt_<hex>.o`，对齐 C++ `cacheObj`）：
  runtime.c / coro_ctx.S / runtime_gpu.c / runtime_lib.c / 各 bridge 全部走哈希缓存，
  跨进程共享复用、无并发覆盖；`myp_self2` 链接 ~0.35s（runtime.c 复用）。
- **P2 集中 Toolchain 配置**：消灭 5 处重复 gcc flags——`findCc()`/`baseCflags(inc)`/
  `ldLibs()` 单点维护。
- **P3 平台基础**：`isWindows()`（`$OS`/`uname -s` → MinGW）+ 平台链接库
  （`-lws2_32 -lwinmm`）+ 平台协程汇编（`coro_ctx_win.S`）选择，对齐 C++ 平台分支。

**回归**：selfhost 322/322、bootstrap 16/16 不动点、selfhost 对拍 95/95；`$CC`/
`MYP_LLC`/`MYP_OPT`/`MYP_LLVM_CONFIG` 环境覆盖验证通过。

### v3.15.1 — `@derive(Json)` 派生序列化（serde 式类级派生，P0 标量/string/bool）

**非破坏性（additive）**，oracle（mypc）与 selfhost 双端同步实现：

- **类级注解 `@derive(Json)`**：编译器在 sema 前为类自动注入 `string toJson()` 与
  `void fromJson(string j)` 两个方法（合成源码 → 复用既有 parser → 注入类 action 段）。
- **属性类型 v1**：int/long/short/byte/uint/ulong/ushort/ubyte、double/float、bool、
  string。toJson 输出合法 JSON（`Json.escape` 转义字符串）；fromJson 用 `json.myp`
  路径查询回填，round-trip 一致。数组/类/struct/元组/函数属性 → 编译期诊断（负测试）。
- 泛型类 `@derive` 暂不支持（v1 诊断）；非 `Json` 派生名 → 诊断。
- `json.myp` 新增 `Json.escape` 静态方法。

**回归**：oracle 322/322、selfhost 322/322、bootstrap 16/16 不动点、dump 对拍 95/95、
fmt 对拍 5/5。新正测试 `tests/@test/manual_serde_derive.myp`（round-trip + 合法 JSON）；
负测试 `tests/negative/derive_unsupported_type.myp`。

**附带发现**：字符串**参数**连续重赋值链有 ARC use-after-free 既有 bug（局部变量无碍，
`Json.escape` 已用局部规避）；待单独修复。

### v3.15.0 — 表达力三小改：多行字符串 / 字符串插值 / 空安全 `?.` `??`

**非破坏性（additive）语言特性**，oracle（mypc）与 selfhost 双端同步实现：

1. **多行字符串 `"""..."""`**：三个连续引号开始，到下一个未转义三连引号结束；
   内容可含换行与单个 `"`，`\` 转义与单行字符串一致。**raw 语义**：三引号串不做
   `$name` 插值展开、保留字面 `$`（`Token.raw` 标记；parser 对 raw 串跳过插值）。

2. **字符串插值**（两种形式）：
   - `${expr}` —— 任意表达式插值（lexer 在字符串内遇 `$` 后跟 `{` 时合成
     `interp_open`/`interp_close` token，parser 折叠为 `+` 拼接）。
   - `$name` —— 简单标识符插值（既有语法，保持不变）。
   - 字面 `{` `}` 不受影响（无 `$` 前缀即字面，JSON 等字符串零冲突）；
     多行 `"""..."""` 内不插值。

3. **空安全 `?.` / `??`**（parser 脱糖，复用既有 `!= null` 与三元语义）：
   - `a ?? b` → `(a != null ? a : b)`（右结合）。
   - `a?.m(args)` → `(a != null ? a.m(args) : null)`；`a?.f` 同理。
   - 结果须可空：类字段/类返回的方法可用；值类型成员（int）无法为 null，
     报 `ternary branches have incompatible types`（需 Option<T> 或 `??` 默认值）。
   - 注意：`a` 在脱糖后求值两次（条件 + 真分支），对变量/字段读取无副作用；
     副作用调用作左操作数时请先提局部变量。

**回归**：oracle 320/320、selfhost 320/320、自举不动点 16/16、tokens/ast/sema
对拍 95/95、fmt/viz 对拍全绿。新正测试：`tests/@test/manual_lexer_triple_string.myp`、
`manual_lexer_interp.myp`、`manual_null_safe.myp`。

### v3.14.2 — 深度学习框架：SD1.5 文生图全管线（D1–D6）

纯 MYP 实现的通用深度学习框架（`examples/deeplearning/`，LLVM 后端）扩展出
**SD1.5 文本→图像**全管线，三大网络全部数值验证通过：

- **D1 DDIM 调度器**（`diffusion/`）：`DDIM(η=0)` 纯 MYP，vs numpy float64 字节
  精确（diff==0）+ diffusers 0.39 交叉 3.5e-7。
- **D2 CLIP 文本编码器**：QuickGELU + 因果/padding 掩码 attention（类内私有规避
  BUG-046），77 位置 vs transformers maxAbsDiff=2.7e-4；D2b BPE 分词器（GPT-2
  `</w>` 词尾式）。
- **D3 UNet**：GroupNorm(32 组)/attention2(cross+self)/GEGLU/nearestUpsample2x，
  vs numpy 0~1.8e-7；D3b 完整前向 maxAbsDiff 1.29e-5。
- **D4 VAE decoder**：完整前向数值验证通过（VAE DECODE OK maxAbsDiff 9.4e-6）。
- **D5 端到端**：CLIP→UNet 50 步→VAE→PPM 出图（IMAGE OK 0.11%）；热点算子
  `@parallel for` ~14x。
- **D6 GPU 加速 + 交互工具**：`@gpu for` 加速扩散管线；`tools/.../gen_image.py`
  prompt → 分词/编码 → GPU DDIM → GPU VAE → PNG（交互循环/单次/--steps/--skip-ref）。

附 `examples/deeplearning/README.md` 各子目录文档 + 里程碑计划 `diffusion_plan.md`。

### v3.14.1 — 优化点推进：多文件并行编译 + GPU R0 止血 + parity 零差距

优化清单推进（多文件并行编译 ① / GPU R0 止血 ②）+ 前置两处修复：

1. **`==`/`!=` 字符串比较类型检查（58def81）**：`bool == string` 应报错，此前静默
   放行（oracle 与 selfhost 双端）。
2. **selfhost parity 零差距（c3d88ac）**：补齐 4 项 parity 差距（`arc` / `arc_m2` /
   `weak_cycle` / `closed-lib`），oracle 与 selfhost 同套 `tests/run_tests.sh`
   315/315 零差距。
3. **多文件并行编译（682ab9b）**：
   - `@parallel for` CPU 并行体从纯数值内核切到完整 `generateStmt`（支持字符串 /
     `new` / 方法调用 / 字段访问）；`@gpu for` 保持纯数值内核（NVPTX 无字符串/堆，
     错误消息只提 `'@gpu for'`）。
   - selfhost 前端多文件 lexer+parser 用 `@parallel for` 并行化 + 顺序合并。
   - 打通自举约束：selfhost 源码可被 mypc 编译（bootstrap 种子不受限）。
4. **GPU R0 止血（f803439）**：`gpu_check_err` 统一 CUDA 错误检查（失败记录 kernel
   名 + 错误码、置 `g_force_cpu`、首错打印一次明确诊断）；kernel 名跟踪（launch
   失败信息带 grid/block 定位 OOB）；全管线回退（`myp_gpu_init` 开头
   `if (g_force_cpu) return 0`）；新 FFI `myp_gpu_force_cpu()` + `Cuda.forceCpu()`
   （oracle/selfhost 双端注册）。

**回归**：oracle 317/317、selfhost 317/317、GPU 回退 61/61、bootstrap 16/16
不动点、parity 0 差距。

### v3.14.0 — Windows 移植里程碑 1/2：运行时层交叉编译通过（MinGW-w64）

**背景**：评估 MYP 全生态 Windows 适配可行性后，首个里程碑 = 让运行时层
（`runtime.c` + stdlib bridges）能在 Windows 交叉编译通过，收敛 POSIX 依赖。

**交叉编译验证工程**（新）：
- `cmake/win64-mingw.toolchain.cmake`：MinGW-w64 toolchain（Linux host →
  Windows x86_64）。注意 `CMAKE_TOOLCHAIN_FILE` 须用**绝对路径**（相对路径会被
  CMake 相对源目录解析）。
- `cmake/cross-runtime/CMakeLists.txt`：只编译 `runtime.c` + 无外部依赖 bridges
  （不依赖 LLVM——编译器本体 mypc/myp_lsp 需 Windows 版 LLVM 库，属下一步，
  可走 llvm-mingw 或 Windows/WSL2 原生构建）。

**Windows 平台适配层 `src/runtime/platform_win.h`（新）**：
- termios（raw 模式 → `GetConsoleMode`/`SetConsoleMode`；ICANON/ECHO/VMIN/VTIME）
- ioctl + TIOCGWINSZ（终端尺寸 → `GetConsoleScreenBufferInfo`）
- dirent（opendir/readdir/closedir → `FindFirstFile`/`FindNextFile`）
- stat 宏 S_ISDIR/S_ISREG、`mkdir→_mkdir`、`lstat→stat`
- `setenv/unsetenv → _putenv`、`sysconf(_SC_NPROCESSORS_ONLN) → GetSystemInfo`
- `poll → WSAPoll`（复用 winsock2.h 的 `pollfd`；runtime 协程 fd 就绪检测用）
- pthread/semaphore 由 MinGW 自带 winpthreads 提供（链接 -lpthread）

**bridges Windows 移植**（`#if defined(_WIN32)` 分支）：
- `net_bridge.c`：Winsock（winsock2.h + WSAStartup 一次性初始化 + closesocket
  + ioctlsocket 非阻塞）。TCP 语义与 POSIX 1:1 兼容，是网络移植最顺的一块。
- `process_bridge.c`：`system`/`_popen`/`_getpid` + `OpenProcess`（运行检测）+
  `CreateProcess`（spawn 后台进程）。`getppid` 无对应 → 返回 -1。
- `uds_bridge.c`：`_WIN32` 下 **stub**（UDS 在 Windows 用命名管道，属后续里程碑）。
- `regex_bridge.c`：`_WIN32` 下 **stub**（POSIX regex 后续换 PCRE 或移植 mini 引擎）。

**验证**：
- 交叉编译：`libmyp_win_runtime.a`（PE/COFF 目标）构建 **100% 通过**——
  runtime.c + net/uds/process/regex/json/base64/date/hash 全部 .obj 产出。
- Linux 零回归：`mypc` 重建 OK；`hello`、`tests/async_socket`（协程+网络+超时）
  实跑正常；coro_stack/async_socket/regex/process 编译 0 errors。

**协程 Win64 汇编（`src/runtime/coro_ctx_win.S`，新）——里程碑 3**：
- Win64 ABI 上下文切换：非易失寄存器 rbx/rbp/rsi/rdi/r12-r15 + **xmm6-xmm15**
  （10 个。SysV 的 xmm 全是 caller-saved 不用存，但 **Win64 的 xmm6-15 是
  callee-saved**——协程内大量 double，不保存必错乱）。
- 保存块 256 字节基址/有效 248：`myp_ctx_init` 设 base=top-256（base%16==0 →
  xmm `movaps` 对齐；恢复后 rsp=base+248，%16==8 满足 Win64 函数入口对齐）。
- `runtime.c` 的 `myp_ctx_init` 改三态（Linux 7 槽 / Win64 256 块 / ucontext 回退）。
- 交叉编译 + 链接验证：`coro_ctx_win.S.obj`（pe-x86-64）反汇编确认切换逻辑，
  `myp_ctx_switch` 符号链接解析；Linux `coro_nest` 测试零回归。
- 注：MinGW as（PE 目标）不支持 `.type`/`.size` 伪指令（ELF 专属）→ 已移除。
- **真机验证（协程内 double 跨切换的 xmm 保护）需 Windows 实机**（里程碑 4）。

**GPU 运行时交叉编译（`runtime_gpu.c` + `runtime_rocm.c`）**：
- dlfcn → `LoadLibrary`/`GetProcAddress`（`_WIN32` 宏兼容层：RTLD_* 定 0、
  dlsym→GetProcAddress、alloca.h→malloc.h）；`libcuda.so.1` → `nvcuda.dll`、
  `libamdhip64.so` → `amdhip64.dll`。
- CUDA driver API 类型由 runtime_gpu.c 自 typedef（无需 cuda.h）→ 交叉编译通过。
- 顺带修：`net_bridge.c` Winsock `setsockopt` optval 是 `const char*`（cast）；
  `runtime.c` `%zu` → `%llu`+cast（MSVCRT printf 不支持 %zu）。

**后续修复（M4）：SOCKET fd 表 + regex 迷你引擎 + warning 清零**：
- **SOCKET fd 表**（`net_bridge.c` + `platform_win.h`）：64 位 SOCKET → 小整数
  int fd（`myp_win_fd_alloc/lookup/free`，跨 TU），消除把 UINT_PTR SOCKET 塞进
  int 的截断隐患；runtime 协程 `poll` 查表还原 SOCKET 再 `WSAPoll`。
- **regex 迷你引擎**（`stdlib/bridges/regex_win.c`，新）：AST + 贪婪回溯 ERE 子集
  （字面量/`.`/`[...]`范围与取反/`*+?`/`^$`/`()`/`|`/`\`转义），30 用例全对；
  `regex_bridge.c` 的 `_WIN32` 分支改 include 此引擎（替代系统 `<regex.h>`）。
- 交叉编译 warning 清零：删冗余变量、`(void)dlflags`、修 `json_bridge` `strrchr`
  未初始化读（潜在 UB，顺带修 json 路径修改 bug）。

**遗留障碍（后续专项，需设计或 Windows 工具链）**：
- UDS→命名管道：MOS 用**多连接 + 多路复用**（`poll` 监听 fd + 多个客户端 fd），
  命名管道单实例模型不匹配，需 MYP 侧接口适配设计（涉及 MOS IPC）。
- 编译器本体（LLVM）：需 Windows 版 LLVM 库（llvm-mingw 或 Windows 原生）。

**编译器本体 Windows 化（`src/main.cpp` + `CMakeLists.txt`，LLVM 库之外的部分）**：
- `src/main.cpp`：新增平台配置，`_WIN32` 下生成程序用 MinGW gcc + `-lws2_32
  -lwinmm`（Linux 为 `-lm -ldl`）、协程汇编选 `coro_ctx_win.S`；8 处 gcc 命令
  改用 `kCC`/`kPlatformLibs`。Linux 生成程序行为不变（全量回归确认）。
- `CMakeLists.txt`：`MYP_THREAD_LIBS`（Linux pthread / Windows winpthreads）、
  `coro_ctx.S` → 按平台选 `coro_ctx_win.S`。Windows 配置验证：FindThreads 解析
  成功，仅卡在 `find_package(LLVM)`（装好 Windows LLVM 后 `-DLLVM_DIR` 指向即可）。
- Windows 分支编译验证需 Windows LLVM（用户侧安装）。

**测试套件 Windows 化（Git Bash 兼容层）**：
- 新增 `tests/lib/portable.sh`（共享移植层，各测试脚本 source）：平台检测
  （MINGW/MSYS/CYGWIN）+ `myp_timeout`（Linux 透传 GNU timeout；Windows 纯 bash
  后台轮询超时强杀，退出码 124 语义一致）+ `myp_resolve_bin`（.exe 前向兼容，
  仅 Windows 启用）+ `myp_guard_ulimit`（Linux 防 OOM；Windows 静默跳过）。
- `run_tests.sh`（4 处 timeout → myp_timeout）、`run_tests_tsan.sh`、
  `bugs/run_bugs.sh`、`stress/run_stress.sh`、`test_myp_self.sh` +
  `test_myp_bootstrap.sh`（ulimit → myp_guard_ulimit）全部接入。
- 新增 Windows 启动器 `tests/run_tests_win.bat`：自动定位 Git Bash/MSYS2
  bash.exe（常见安装路径 + PATH 兜底），切到仓库根调用 run_tests.sh。
- **Linux 全量回归确认**：run_tests 314/314、self 95/95、bootstrap 16/16、
  bugs 12/12，行为零变化（Linux 走 GNU timeout/ulimit 原路径）。
- 前提（Windows 侧）：Git Bash + Windows LLVM 构建 mypc.exe + MinGW gcc 在 PATH；
  mypc 在 Windows 仍按 `-o` 精确命名 `.out`（PE 文件，msys 运行时按魔数可直接执行）。

**`mypc run` 子命令 POSIX 依赖清零（编译器本体可完整编译）**：
- 头文件分平台：Windows 用 `runtime/platform_win_dirent.h`（新，dirent 模拟头，
  从 platform_win.h 提取供编译器共用，避免把 termios/poll/mkdir 宏拖进编译器 TU）
  + `windows.h`/`process.h`/`direct.h`；Linux 保持 `dirent.h`/`sys/wait.h`/`unistd.h`。
- 新增 `mkdirPortable`/`pidPortable`/`tempDir` 跨平台工具：Windows 用 `_mkdir`/
  `_getpid`/`GetTempPathA`（%TEMP%，替换硬编码 `/tmp`；runtime 缓存目录同改）。
- `selfExeDir`：Linux `readlink(/proc/self/exe)` → Windows `GetModuleFileNameA`。
- `nmSymbols`/`nmDynSymbols`：Windows 下 popen 走 cmd.exe，重定向 `2>/dev/null` →
  `2>NUL`（nm 来自 MinGW binutils，须在 PATH）。
- `mypc run` 执行：Windows 用 `CreateProcessA`（同步 + `GetExitCodeProcess` 透传
  退出码，临时二进制显式 `.exe`）替代 `fork/execv/waitpid`；Linux 分支不变。
- 验证：Linux 全量 314/314 + `mypc run` 8/8（含 args 透传、无残留）；
  移植片段用 MinGW 交叉编译链接成 PE（0 error，仅 NOMINMAX 重定义已修）；
  cross-runtime 交叉编译重构后仍通过。
- ⚠️ main.cpp 的 `_WIN32` 分支整体编译验证仍需 Windows LLVM（M5 已知限制）。

**全仓 POSIX 依赖审计（编译器本体之外残留清零）**：
- `src/runtime/runtime_lib.c`（cuBLAS hook）：补 `_WIN32` dlopen→LoadLibrary shim
  （同 runtime_gpu/rocm，M3.5 模式）；库名 `libcublas.so.*`→`cublas64_*.dll`。
  **此文件被 mypc 无条件编译，原裸 `dlfcn.h` 会让 Windows 每个生成程序编译失败**。
- `src/runtime/runtime.c myp_capture_args`（程序 argv 捕获）：Windows 用 CRT
  `__argc`/`__argv`（stdlib.h 已声明，构造期可用）替代 `read(/proc/self/cmdline)`，
  否则 Windows 上 MYP `main(argc, argv)`/`args` 模块的 Argc 恒为 0。
- `src/dap/dap_server.cpp`（myp_debug，DAP↔gdb MI）：fork/pipe/dup2/execlp/poll/
  read/write/usleep 全套 POSIX 编排 → **Windows 暂不构建**（CMake `if(NOT WIN32)`，
  Linux 保留）。Windows 版 DAP 调试（CreateProcess+管道+等待）为后续里程碑。
- `cmake/cross-runtime/`：纳入 runtime_lib.c + runtime_rocm.c（与 gpu 同享 shim
  cross 验证）。cross-runtime 构建 0 warning。
- 审计结论：编译器本体可执行文件（mypc/myp_lsp/myp_fmt/myp_viz）POSIX 依赖已全
  清零；tools/、mypview/ 全为 MYP 源码无原生代码。
- 回归：Linux 314/314 + cross-runtime 交叉编译 0 warning（含新 args/runtime_lib）。

### v3.13.8 — P6 ② 声明式 reduce/scan 块内并行（§8.2/8.3，用户选）
- **reduce 块内并行 halving 树**（§8.6 规范树，`emitBlockSumTreePtx`）：2 的幂块
  大小时 K1 改 ping-pong 共享内存树（每线程 1 元素，末块尾以 init 单位元填充），
  CPU 镜像 `emitSeqBlockTreeReduce` 同树 → **位级一致**（`test_gpu_reduce_bit`
  GPU==CPU==1177075682）；非 2 幂回退串行 K1（纯块和，修正 init 双计 bug）。
- **reduce 表达式形式**（`GpuReduceExpr`）：`float s = @gpu reduce ... over
  a[0..n);` 无 `-> out`，parser 3-token lookahead 区分 lambda，sema 合成
  `__gpu_rdtmp_N` 临时，可嵌套/参与运算（`bench/rdexpr.myp` 双模式 PASS）。
- **scan Hillis-Steele 块内并行**（`emitScanK2HsPtx`）：inclusive + 2 的幂块 →
  K2 改 ping-pong 双缓冲 HS（d∈{1,2,4,…}，kernel 名 `myp_scan_k2_hs`）；launch
  按 `use_hs` 选 kernel 名（否则静默回退 CPU）。
- **scan exclusive 变体**：`@gpu scan(exclusive) ...`（K2 写前落盘 / CPU 写前
  先存）；`test_gpu_scan.myp` 增 exclusive 全量/子区间/非零 init 三 case 双模式 PASS。
- **CPU 回退权衡**：scan 回退统一串行 `emitSeqScan`（HS 位一致镜像
  `emitSeqScanBlocked` 在串行 CPU 上慢 ~10× 不采用）→ GPU/CPU 浮点差几个 ulp
  （容差内）；reduce 位级一致不受影响。
- **性能验证**（`bench/gpu_reduce_scan.myp`，基线 `BASELINE_gpu_reduce_scan.md`）：
  GPU reduce 1M 0.87–0.97→0.67–0.9、scan 1M 1.6–2.0→1.2、reduce 4M 2.9–3.2→2.07、
  scan 4M 5.6–6.4→4.07 ms/op——**全面改善，无性能回退**；CPU 回退持平。
- **坑**：树 kernel `src[tid+half]` 越界（tid≥half 线程）→ 非法内存访问，须
  `select` 钳索引到 tid；scan j-loop 缺 j++ 回边 → partials 只算 block 0；offsets
  循环须重绑 acc/x（步骤 4 恢复后步骤 5 复用未绑定 → "undefined variable acc/x"）。
- 回归 266/266 + AMD 交叉编译 + GPU 双模式（reduce/scan/algo/reduce_bit）PASS。

### v3.13.7 — P6 ② 图内存（CUDA Graph）+ P6 ③ BYOC（§9.7）
  - **图内存**：`stdlib/gpu/graph.myp` 的 `GpuGraph`（captureBegin/captureEnd/
    instantiate）与 `GpuGraphExec`（launch 重放/destroy）。宿主 FFI
    `myp_gpu_graph_capture_begin/end/instantiate/launch/destroy/exec_destroy`
    （`runtime_gpu.c`，dlopen libcuda）。机制：**流捕获**（`cuStreamBeginCapture`
    THREAD_LOCAL → `cuStreamEndCapture` → `cuGraphInstantiate` → `cuGraphLaunch`
    重放）。约束：内核须 `resident()` + `GpuBuffer`（持久 `devicePtr`），捕获段
    只排内核。
  - **关键坑**：`cuGraphInstantiate` 在 MYP 协程上下文段错误（同 cuModuleGetGlobal
    的 TLS 问题）→ 所有图入口先 `cuCtxSetCurrent(ctx)` 强制上下文当前即修复。
  - **BYOC 自定义 PTX**：`stdlib/gpu/byoc.myp` 的 `GpuByoc`（load/launch），宿主
    FFI `myp_gpu_byoc_load/launch`（参数 `long[]`：指针放指针值、标量放数值、
    double 放位型）；启动手写自包含 PTX（`tests/test_gpu_byoc.myp` 的 `dbl`）。
  - **BYOC 厂商库 hook**：`runtime_lib.c`（独立编译，dlopen libcublas 惰性加载，
    缺库回退）暴露 `myp_cublas_available/sgemm` → `GpuLib` 列主序 SGEMM；
    测试与 host 参考误差 <1e-4。
  - 注册：sema `add_intrinsic`/`add_gpu_arr` + codegen `declareRuntimeFunctions`
    intrinsic_map（`__myp_gpu_graph_*`/`__myp_gpu_byoc_*`/`__myp_cublas_*`）。
  - 测试 `test_gpu_graph.myp`（3 resident 内核捕获→重放逐位一致 + 二次重放幂等，
    CPU 模式 no-op）、`test_gpu_byoc.myp`（PTX dbl 全 n 逐位 + cuBLAS SGEMM）。
    回归 266/266 + AMD 交叉编译 + 既有 GPU 测试（algo/printk/query）GPU 模式无回归。
  - 踩坑：range-for（`for k in 0..n`）解析提前 return，**不解析 resident/stream
    子句**——`resident`+`stream` 必须配标准 `for(;;)`；`@gpu for` 内嵌 PTX 手写
    时寄存器声明 `<N>` 含 %r0（用 %r4 须 `<5>`）；ptxas 拒绝 `[reg+reg]` 寻址
    （改用 `add.u64` 合成地址）。

### v3.13.6 — P5 ② kernel 内 printk/assert 调试（§9.6）
  - **`kernel.printk(fmt, v...)` / `kernel.assert(cond, fmt, v...)`**（`@gpu for`
    内核内；fmt 字符串字面量，值参 int/long/double/float ≤3）。
  - **GPU staging**：runtime 分配设备缓冲/计数器（`myp_gpu_printf_buf/cnt/fail`）
    + kernel 末尾 3 个附加 i64 参数传指针（**避开 cuModuleGetGlobal**——协程/
    @thread 上下文下返回 CUDA_ERROR_INVALID_CONTEXT）；kernel 内 atomicrmw 领槽
    （仅 printk/断言失败才领）+ 写 7×i64 记录；launch 后 `myp_gpu_flush_printf`
    回读 mini-printf 打印、清零；assert 失败 exit(1)。
  - **CPU 回退**：宿主 `myp_printf`/`myp_assert_abort`，前缀 `kernel[gid=<循环变量>] `
    与 GPU 一致 → 单格式双模式输出逐字节相同。
  - **格式**：值参类型匹配（int→%d、long→%ld、double→%g）；GPU 按 mask 宽容打印。
    多格式记录顺序受 GPU 线程调度影响（非确定性，同 CUDA printf）。
  - 测试 `test_gpu_printk.myp`（printk+assert 通过，双模式输出 IDENTICAL）、
    `test_gpu_assert_fail.myp`（断言失败双模式 exit 1 + 消息一致）；负测试 3
    （格式非字面量/参数过多/类型不符）。回归 266/266 + AMD 交叉编译无回归。
  - 踩坑：O2 GlobalDCE 删只写不读的模块全局 → 改 runtime 缓冲 + 附加参数；
    cuModuleGetGlobal 协程 201；领槽 atomicrmw 须 gate 在 do_write 内（否则通过
    assert 也消耗槽位）；`@gpu for` 边界按 `<` 处理（`i<=n` GPU 只到 n-1）。

### v3.13.5 — P5 ④ 并行算法库（stdlib/gpu/algo.myp，GpuAlgo）
  - **compact**（流压缩）：keep 的 inclusive 前缀和（§8 scan）→ 目标位置
    pos[i]=off[i]-keep[i]（exclusive）→ 条件写（@gpu for）；返回保留数。
  - **unique**（相邻去重）：change[i]=(i==0 或 a[i]!=a[i-1]) → 对 change 做 compact。
  - **histogram**：ones 数组 + `@gpu scatter(atomic_add)` → hist[idx[i]] += 1
    （整数原子计数位一致；host 预扫越界自保，越界返回 0）。
  - **sort**（原地升序）：确定性 odd-even 转置比较交换网络（每轮偶相+奇相两个
    独立 kernel launch 提供隐式全局同步；n 轮有序；O(n²)，radix/bitonic 留后续）。
  - **双实现位一致**：GPU 与 CPU 回退跑同一算法序列 → 输出逐字节相同。
    `tests/test_gpu_algo.myp`（n=2048：质数 compact / i/8 unique / LCG 16 桶
    histogram / 0..999 重复值 sort）MYP_GPU=0/1 双模式输出 IDENTICAL；AMD
    交叉编译（MYP_GPU_TARGET=amdgcn）二进制 CPU 回退语义 PASS。回归 263/263。
  - 踩坑记录：`@gpu scan` int init 须 `init int(0)`（字面量 0 为 byte）；`@gpu for`
    假设循环从 0 起（非 0 起始在 GPU 下执行 p=0 分支 → 越界读，须体内处理边界）；
    静态方法须类名限定调用。

### v3.13.4 — P4 §9.5 ④ 厂商探测 + 能力查询（§7.4）落地
  - **runtime_gpu.c 厂商探测**：`myp_gpu_vendor()` → "nvidia"/"cpu"（无 GPU），
    `myp_gpu_gfx_arch()` → ""（NV 无此概念）；ROCm 版返回 "amd"。
  - **runtime_gpu.c 能力查询**：`myp_gpu_shared_per_block/regs_per_block/
    max_grid_dim/max_block_dim/clock_mhz/concurrent_kernels/mem_alignment/
    double_precision/atomics64`，统一 `cuDeviceGetAttribute`（属性 ID 对齐
    /usr/include/cuda.h：MAX_BLOCK_DIM_X=2、MAX_GRID_DIM_X=5、MAX_SHARED_MEMORY_
    PER_BLOCK=8、MAX_REGISTERS_PER_BLOCK=12、CLOCK_RATE=13、CONCURRENT_KERNELS=31）。
  - **runtime_rocm.c HIP 镜像**：`hipDeviceGetAttribute` 同 ABI 补全（vendor="amd"）。
  - **codegen/sema**：注册 vendor-neutral intrinsic `__myp_gpu_vendor/gfx_arch/
    shared_per_block/regs_per_block/max_grid_dim/max_block_dim/clock_mhz/
    concurrent_kernels/mem_alignment/double_precision/atomics64`。
  - **stdlib/gpu/device.myp**：`GpuDevice` 补齐 §7.4 全部字段（vendor/gfxArch/
    sharedPerBlock/regsPerBlock/maxGridDim/maxBlockDim/clock/concurrentKernels/
    memAlignment/doublePrecision/atomics64）；`GpuHAL.vendor()` 改真实设备探测。
  - **实测 RTX 2070 SUPER**（`test_gpu_query.myp` 双模式 PASS）：vendor=nvidia、
    capability=705、sharedPerBlock=49152、regsPerBlock=65536、maxGridDim=2147483647、
    maxBlockDim=1024、clock=1815MHz、concurrent=1、atomics64=1（sm_60+）、
    doublePrecision=0（sm_75 消费卡 FP64 1/32）。CPU 回退 vendor=cpu、能力全 0。
  - 回归 263/263 + AMD 交叉编译（tests/cross_compile_amd.sh）无回归。

### v3.13.3 — P4 跨厂商（AMD）编译期落地（无 AMD 硬件，交叉编译验证）
  - **§9.5/§6.4 AMDGPU 后端 + GCN 交叉编译**：
    - `MYP_GPU_TARGET=amdgcn` 让 `@gpu for`/reduce/scan 内核编译期发射
      `amdgcn-amd-amdhsa` GCN ELF code object（`ObjectFile` 直接出 ELF，EM_AMDGPU，
      含 `myp_kernel` 符号），写 `MYP_GPU_EMIT_FILE`（默认 /tmp/myp_kernel.gcn）；
    - codegen 参数化：双后端 init（`ensureGpuTargetsInited`）、kernel CC
      （NV PTX_Kernel / AMD AMDGPU_KERNEL）、线程索引（NVVM sreg ↔ AMDGCN
      workitem/workgroup.id.x）、kernel alloca addrspace(5)（AMD private）、
      blockDim = launch 常量、`kernel.sync()`（bar.sync 0 ↔ s_barrier）、
      O2 管线消解 AMDGCN 无法选中的构造（声明式 kernel）。
  - **§9.5 ③ runtime_rocm.c 骨架**（`-DMYP_ENABLE_ROCM=ON`）：dlopen
    libamdhip64 + HIP 函数指针镜像 myp_gpu_* ABI（hipModuleLoadData/hipLaunchKernel/
    hipMemcpy/stream/event），无 ROCm 不构建。
  - **§9.5 ⑤ 交叉编译验证** `tests/cross_compile_amd.sh`（无硬件）：GCN ELF
    magic + kernel 符号（llvm-readobj）+ AMD 二进制 CPU 回退语义 + NV PTX 无回归。
  - **受限（无硬件留待）**：`@gpu stride`/scatter（gridDim 无 AMDGCN intrinsic）
    AMD 回退 CPU；`@gpu tile` __shared__ 对象发射；数学 NV libdevice（AMD 走
    LLVM intrinsic/ocml）。
  - CMake：AMDGPU LLVM 组件；`MYP_ENABLE_ROCM` 选项。回归 263/263。

### v3.13.2 — LSP 语义高亮（semantic tokens）：MYP 文件通过 LSP 有语法颜色

**背景**：vscode-myp 扩展的 LSP（`myp_lsp`）只提供诊断/补全/hover/文档符号，
未实现 `semanticTokensProvider` —— MYP 文件在 VS Code 中只有 TextMate 基础
高亮（关键字/类型/注释），函数名、方法、变量、属性等大量标识符是默认色，
用户反馈「LSP 这块语法没有颜色和高亮」。

**实现**（`src/lsp/lsp_server.cpp`）：
- capabilities 声明 `semanticTokensProvider`（legend 19 类型：comment/keyword/
  string/number/type/class/struct/interface/enum/function/method/property/
  variable/parameter/operator/namespace/annotation/boolean/macro，`full:true`）。
- 新增 `textDocument/semanticTokens/full` handler：注释扫描（正确跳过字符串/
  字符字面量内的 `//` 与 `/* */`）+ lexer tokenize + 标识符语义分类：
  - 关键字/类型/数字/字符串/操作符/注解 → 按 TokenKind 直接映射；
  - 标识符：`.xxx(` → method、`.xxx` → property、`@xxx` → annotation、
    大写开头 → type（类/struct/enum/interface 名）、`xxx(` → function、
    `int x`/`Foo bar`（前一个是类型）→ variable。
- UTF-16 偏移 delta 编码（`utf16CodeUnits` 处理多字节源文件）。
- **lexer 坑**：单字符 operator 的 range 是 `[offset,offset)`（`currentRange()`
  在 `advance()` 之后返回，len=0 且位置指向下一字符）→ 语义编码跳过空 token，
  操作符颜色交给 TextMate（tmLanguage 已有 operators 规则）。
- 扩展端无需改动（vscode-languageclient 自动请求 semantic tokens）。

**验证**：`textDocument/semanticTokens/full` 对 `demo_model.myp` 返回 296 个
语义 token（comment 10 / keyword 8 / string 4 / number 92 / type 23 /
function 3 / method 46 / variable 110）；`tests/test_lsp.js` **14/14 PASS**。
使用：VS Code 里 `MYP: Restart Language Server`（或重载窗口）让扩展加载新
`myp_lsp`。

### v3.13.1 — 编译器 DX：漏参/窄化诊断 + GPU fast-math + 自举 UTF-8 修复（95/95）

**1. 参数缺失报错改进**（`src/sema/sema_expr.cpp` + `tools/selfhost/src/sema.myp`）
- 漏参数时一次性列出**全部**缺失形参（而非只报尾部一个），并附期望/实得数量：
  `missing required argument(s) 'batch' 'dev' — expected 10 arguments, got 8`。
- 修复场景：GPU 算子调用漏 batch 时之前只报 `'dev'`（误导性极强），现能看出是
  中间漏参导致后续错位。C++ 报错文本已同步 selfhost（sema.myp）。

**2. 参数窄化诊断（narrowing warning）**（`include/mylang/Sema.h` +
`src/sema/sema_expr.cpp` + `tools/selfhost/src/sema.myp`）
- 新增 `Sema::isNarrowing`：实参→形参「兼容但有损窄化」时发 warning——
  `argument 1: passing 'long' to parameter of type 'int' is a narrowing
  conversion; ... use convert<int>(...) to make it explicit`。
- **为什么只有 Long→Int 命中**：MYP 数字隐式只允许 Int↔Long 双向 + 同符号拓宽 +
  f32→f64；`double→float`、`double→int`、`long→short` 等本就在 `typesCompatible`
  层报 error（`expected 'float', got 'double'`）。故「兼容但窄化」实际只有
  Long→Int 一种——正是漏参/换位时最常见的错位源（如 GPU dense 的 `dev`/`yOff`）。
- **warning 而非 error**：现有代码大量合法 long→int（ARC 计数断言
  `Test.assertEq(long,0,..)`、`Fmt.i(long ms)`），error 会破坏回归。
- **对拍约束**：`--frontend-dump sema` 输出含 Diagnostics 段，任何新诊断必须
  C++/selfhost **逐字节一致**（selfhost 无 param_names 元数据 → 诊断文本不带
  形参名）。新增对拍语料 `tests/narrow_sema.myp`（3 warning + 1 error，两编译器
  dump 字节一致）。

**3. GPU fast-math（opt-in）**（`src/codegen/codegen_gpu.cpp`）
- `MYP_FAST_MATH=1` 时给 GPU 内核 IRBuilder 打 fast flags（reassoc/contract/nnan/
  ninf/nsz/arcp），默认 OFF（不改变现有数值行为）。
- 实测 Qwen2 batch=4 LLM 反而变慢（21 vs 16 ms/step）——LLM decode 带宽受限，
  计算优化无益；保留 opt-in 供纯计算型内核（卷积/归约）使用。

**4. 自举 UTF-8 修复（Dump.esc，区别于 v3.12.48 词法器 BUG-039）**
  （`tools/selfhost/src/ast.myp`）
- `Dump.esc` 用 `__myp_charcode`+`__myp_chr` 逐字符 → 字节被当码点再 UTF-8 编码
  （双重编码，`"多参"`→`"å¤å"`）。改用 `Str.substring(s,i,i+1)` **原样字节透传**
  （与 C++ `escapeDumpString` 逐字节一致）。lexer 层本就正确（tokens 对拍已 PASS），
  无需改动。
- 自举 sema 对拍 **92/94 → 94/94**（+本次窄化语料 = **95/95**）。

**回归**：全量 `tests/run_tests.sh` **314/314**；`tests/test_myp_self.sh` **95/95**。

### v3.13.0 — 自举编译器 GPU 真机加速：resident 直传 + Device./Atomic 映射（BUG-045）

> 编译器版本序列自本条目起与 mypview 分离：**编译器/自举/运行时版本独立计数**
> （主 changelog 最后编译器条目 v3.12.57；v3.12.58–69 为 mypview 框架独占里程碑，
> 记于 `mypview/CHANGELOG.md`）。本文件后续编译器条目从 v3.13.0 起单独递增。

**背景**：自举编译器（`tools/selfhost/src/codegen.myp`）编译 deeplearning 框架时，
GPU kernel 对 `resident` 一律回退 CPU、`Device.*`/`Atomic.*` 设备调用发 undefined
符号（llc 拒 → 全量 GPU CPU 回退）。本次补齐，使自举产物在真机（RTX 2070 SUPER，
CUDA 13.2，`MYP_GPU=1`）真正发射 kernel。

**修复内容**（全部在 `tools/selfhost/src/codegen.myp`）：
- **resident 设备驻留直传**：`genGpuKernel`/`genGpuTileKernel` 不再拒绝 resident
  数组；新增 `gpuResidentDev(st, arr)` 从 `st.resident()` 解析 devVar，设备指针用
  `inttoptr(load devVar)`，跳过 H2D/D2H/free。
- **`gpuKernelMode_` 标志**：kernel 模块 body 生成期间置 1，区分 NVPTX kernel 与
  CPU 回退串行 codegen。
- **`genKernelDeviceCall(e, rfn)`**（对应 C++ oracle 的 `emitKernelExpr` 特判）：
  - `Device.sqrt/exp/log/sin/cos/pow/abs/floor/ceil/trunc` → `llvm.*` intrinsic
    （f32/f64 仅 intrinsic 名后缀，LLVM 类型用 float/double）；
  - `Atomic.addDouble/addFloat` → `atomicrmw fadd`、`addInt/addLong` →
    `atomicrmw add`（`getelementptr <elemT>, ptr, i64 idx`）。
- 两处拦截：resolved 分支 + member 调用路径（后者处理不走 `e.resolved()` 的
  `Device_*`/`Atomic_*` 静态调用）。
- 两个 kernel 模块前言（`emitGpuKernelModule`/`emitGpuTileModule`）补
  `llvm.exp/log/sin/cos/pow.f32/f64` intrinsic 声明。

**效果**：
- kernel 未定义符号 **21 → 1**（仅剩未用的 `Device_tan`——LLVM 无 tan intrinsic，
  回退 CPU 可接受）。
- GPU 重模型真发射且结果正确：`conv3d_main`/`coarselike`/`coarselike32`/
  `pad3d_avgpool3d` 全「GPU OK」（nonzero-grid launch 7/21/21/3 次）。
- r18 的 40 个 kernel 全 `grid=0 → cuLaunchKernel failed` 为**框架层预先存在 bug**
  （oracle 的 r18 GPU 同样 40 个 grid=0，行为一致），非编译器问题。

**BUG-045（🟩）——自举 `@parallel for` 数组参数元素类型缺失**：`genParallelFor`
捕获参数传 `elem=""` → 并行体内 `float[] arena` 被当 `i32[]`（GEP/store 用 i32）→
3D Conv3D/MaxPool3D/AveragePool3D 输出全 0。修复：参数捕获按 `paramAstTypes_`
取元素类型（slice→`sliceElemType`+slice 标记，数组→`llvmType(element())`），与
`varElemType`/`isSliceVar` 对齐。回归：`tests/bugs/b045_parallel_float_array.myp`
（4 断言双编译器通过）；infer_tests 18 入口自举与 oracle 一致。

**回归**：CPU 11 入口 0 mismatch；`tests/test_myp_self.sh` PASS=92/2（2 个 sema
对拍失败为预先存在：`mega/test.myp`、`@test/function.myp`）；`run_bugs.sh`
12 green 0 red。

### v3.12.57 — 再剥离 6 组 FFI 出 runtime（net/process/regex/base64/date/hash）

**背景**：继 JSON 之后，继续审计 `src/runtime/runtime.c`——凡是「只被单个 stdlib
模块引用、不被核心运行时依赖」的函数组都移出为独立 bridge（按需链接）。

**剥离 6 组**（runtime.c 5949→5436，-513 行）：
| 组 | bridge | 对应 stdlib | 行 |
|---|---|---|---|
| `myp_net_*` | `net_bridge.c` | net | 106 |
| `myp_process_*` | `process_bridge.c` | process | 63 |
| `myp_regex_*` | `regex_bridge.c` | regex | 31 |
| `myp_base64_*` | `base64_bridge.c` | base64 | 63 |
| `myp_date_*` | `date_bridge.c` | date | 45 |
| `myp_hash_*` (md5/sha) | `hash_bridge.c` | crypto | 205 |
- 每个 bridge 头统一 `#include "mylang/runtime.h"`（`myp_alloc`）+ 标准头；
  process 补 `<unistd.h>`（getpid/kill）。
- 修复一处剥离边界：base64 段后无分隔线直接接 alloc 核心段，初版误吞
  `myp_alloc` 核心 → 硬编码段边界重剥（base64 只到 decode 结束）。
- 仅用到这些模块的程序才链接对应 bridge（同 sdl/ttf/json）。

**保留核心基座**：ARC（alloc/retain/release）、GC pool、字符串、协程/事件、
线程/并行原语、类型转换、异常、断言等——被编译器生成代码/语言特性直接依赖。

**验证**：综合测试 `import regex/base64/date/crypto/process` 编译运行全对
（`re=1 b64=aGk= md5=900150... pid=...`，bridge 按需链接）；parent 313/313、
bootstrap 16/16、bugs 11/11、mypview UIX/BNCT/PIPE 全 PASS。
**后续候选**：`myp_fs_*`（fs，独立但高频用）、`myp_math_*`（编译器 intrinsic，
需专项验证）暂留 runtime。

### v3.12.56 — JSON 从 runtime.c 分离为独立 bridge（按需链接）

**背景**：JSON 解析/查询 FFI 常驻 `src/runtime/runtime.c`（约 300 行），每个
程序（无论用不用 JSON）都编译进运行时。JSON 仅在 `import json;` 时才有意义，
不应是运行时基座。

**分离**（对齐 sdl/ttf bridge 机制）：
- JSON 段从 `src/runtime/runtime.c` 移出（-299 行，6247→5948），新建
  `stdlib/bridges/json_bridge.c`（含 `myp_json_parse/get_type/get_string/
  get_number/get_bool/array_length/free` + 递归下降解析器）。
- 依赖 `myp_strdup`（runtime.h 声明，runtime 链接提供）。
- `stdlib/json.myp` 的 ffi 声明不变（符号名 `myp_json_*` 由 bridge 按需提供）。
- 效果：只有 `import json;`（调用 `myp_json_*`）的程序才链接 json_bridge.o，
  不用 JSON 的程序运行时更小。

**验证**：`import json` 程序编译链接运行正常
（`json a=1 name=hi ok=1 arrlen=3`，`nm` 确认 json_bridge 按需链接）；
parent 313/313、bootstrap 16/16、bugs 11/11、mypview UIX/BNCT/PIPE 全 PASS。

### v3.12.55 — mypview 新增 Card 控件 + BNCT 病例管理页示例

**Card 控件**（`mypview/src/controls/card.myp`，第 53 个控件）：圆角信息卡片——
标题/副标题/元信息 + 底部「详情/进入」双操作按钮，hover 提亮、选中 accent 描边。
事件：`Clicked`（整卡）/ `PrimaryAction` / `SecondaryAction`；声明式属性
title/subtitle/meta/primary/secondary/color/accent。适配 Web dashboard 卡片
（对齐 Theme 深色表面 + accent）。

**BNCT 病例管理页示例**（`mypview/examples/bnct_cases.myp`）：复刻 BNCT 治疗
计划系统病例管理页结构——Header（系统标题/页面标题/用户区）+ SearchBar +
排序/来源 Dropdown + 操作按钮行 + 病例/序列 Dropdown +「进入勾画」+ Card 卡片
网格（搜索过滤联动 + 卡片详情/进入事件）。**双模式**：
- **默认 SDL 窗口界面**（`bash build.sh bnct_cases`）：1100×700 dark blue 窗口，
  卡片渲染 + hover 提亮 + 点击事件（进入/详情）+ 键盘搜索实时过滤，ESC/关窗退出；
  帧循环已验证（`bnct ui running cards=2` → `done frame=5`）。
- **`BNCT_HEADLESS=1` headless 逻辑验证**：
```
bnct header=BNCT 治疗计划系统 / 病例管理 / Doctor demo
bnct cards=2 first=Patient-7736D7 (ID-C4A0)
bnct search '305891' shown=1        ← 搜索过滤
bnct enter=Patient-7736D7 (ID-C4A0) ← 点卡片「进入」
bnct detail=Patient-305891          ← 点卡片「详情」
```
`build.sh` 对 `bnct_cases` 自动加入 `backend/sdl_renderer.myp`（同 player）；both
对比模式走 headless。实现中修复：数组元素传接口参数（`RootView.add(cards_[i])`）
不自动接口上转 → 局部变量中转（同 BUG-041 类别）。

**测试**：`mypview/tests/run.sh` 两个 SRCS 列表加 `card.myp`，BNCT 节编译含
`backend/sdl_renderer.myp`，新增 `MYPVIEW-BNCT PASS`（7 断言，`BNCT_HEADLESS=1`）。
mypc 与 myp_self both 输出一致；parent 313/313、bootstrap 16/16、bugs 11/11、
mypview UIX/BNCT/PIPE 全 PASS。

### v3.12.54 — MYP 源码闭源：签名声明（无 body 方法→外部声明）+ 预编译库链接

**背景**：v3.12.53 打通了「C 下沉 + bridge 预编译 .so」闭源。但核心算法若本身
用 MYP 写（而非 C），`import` 是源码合并、分发必含 `.myp` 源码。本版本让
**MYP 源码也能闭源**：实现编译成 `.so`（`mypc --shared` 已有），分发「签名
`.myp`（只声明无 body）+ `.so`」。

**修复**（`src/codegen/codegen_class.cpp`）：类方法生成器（`generateClassAction`/
`generateStaticAction`/`generateClassFunction`）对**无 body 方法**不再生成
`ret 默认值` 的 internal stub，而是保持**外部声明**（ExternalLinkage、无 body）
——调用方生成对 `Class_method` 的 undefined 引用，链接器经 `MYP_BRIDGES`
从预编译 `.so`/`.a` 解析（配合 v3.12.53 的 bridge 预编译库支持）。
`markNonMainFunctionsInternal` 已跳过声明（`isDeclaration()`），无冲突。

**闭源分发闭环（MYP 源码）**：
- 实现 `secret.myp`（class Secret { static: mul/verify/greet }）→
  `mypc secret.myp --shared -o libsecret.so`（导出 `Secret_mul` 等符号）
- 分发：签名 `sig.myp`（`static: int mul(int a,int b);` 无 body）+ `libsecret.so`
- 用户：`import "./sig.myp";` 调 `Secret.mul(...)`，编译时
  `MYP_BRIDGES=<含 libsecret.so 目录>` → 自动链接
- 运行：`mul=24` / `verify=1` / `greet=hi Bob`（含字符串返回与 ARC，均正常）

**测试**：`tests/test_closed_lib.sh` 扩展第 7 节（MYP 源码闭源，6 断言），现
12 断言全过；parent 313/313、bugs 11/11、bootstrap 16/16、mypview UIX/PIPE PASS。

### v3.12.53 — bridge 机制支持预编译库（.so/.a）→ MYP 闭源分发

**背景**：MYP 的 import 是「源码合并」模型，包分发必然含 `.myp` 源码，无法闭源。
借鉴 Java（字节码 jar + 混淆 + JNI 下沉）/C（头文件 + 预编译库）模式，打通
「预编译库 + FFI 封装」闭源路径。

**实现**（`src/main.cpp` `linkObjects` bridge 逻辑）：
- bridge 发现扩展：`MYP_BRIDGES` 目录除 `.c` 外，也收集预编译库 `.so`/`.a`
  （新增 `listLibFiles`）。
- 预编译库按「用户程序未定义符号 ∩ 库已定义符号」固定点匹配直接链接，无需
  `.c` 源码（`selected_libs` + `bridge_obj_list` 直接带库路径）。
- `.so` 用动态符号表匹配（新增 `nmDynSymbols`，`nm -D`——strip 过的共享库也能
  读到导出符号）；`.a` 用普通 `nm`。
- 现有 `.c` bridge（sdl/ttf 等）逻辑不动，回归全绿。

**闭源分发闭环（端到端验证）**：
- 核心算法 `secret.c` → `gcc -shared -fPIC` → `secret.so`（分发物，不含 `.c`）
- 封装 `api.myp`：只 `ffi` 声明 + `static:` 薄封装类（核心实现不可见）
- 包化：`package.myp` + `src/secretpkg.myp` + `lib/secret.so`，`myp install` 后
  用户 `import secretpkg;`，编译时 `MYP_BRIDGES=<含 secret.so 的目录>` 自动链接
- 运行：`pkg mul(5,6)=60` / `verify=1`（逻辑来自 .so）；`.so` 与 `.a` 两种形态均可

**测试**：新增 `tests/test_closed_lib.sh`（6 断言：.so 链接/核心输出/闭源无 .c/
.a 链接）接入 `run_tests.sh`；parent 313/313、bugs 11/11、bootstrap 16/16、
mypview UIX/PIPE PASS。

### v3.12.52 — 点分模块名导入增强（a.b.c → 包内 src/ 子路径）+ mypview 子目录聚合

**点分导入（对齐 `gpu.hal` 惯例）**：编译器 import 解析对点分模块名
`a.b.c` 追加「首段=包名」解析——在现有 `<pkg>/a/b/c/src/a.b.c.myp` 形态未命中
后，尝试 `<pkg>/a/src/b/c.myp` 与 `<pkg>/a/b/c.myp`（向后兼容）。
- mypc：`src/main.cpp` `loadModule`；myp_self：`tools/selfhost/src/main.myp`
  `loadModule`（两编译器同步，bootstrap 16/16 不动点）。
- 效果：`import mypview.controls.app_icon;` → `mypview/src/controls/app_icon.myp`。

**mypview 子目录聚合**：新增 `src/core.myp` / `src/controls.myp` /
`src/layout.myp` / `src/uix.myp` / `src/animation.myp`（相对路径 import 聚合各
目录；controls/layout/uix/animation 内部 `import "./core.myp"` 自包含，
uix 额外依赖 controls+layout）。点分粒度导入：
- `import mypview.core;`（核心 6 文件）/ `import mypview.controls;`（49 控件）
  / `import mypview.layout;` / `import mypview.uix;` / `import mypview.animation;`
- 单文件：`import mypview.controls.app_icon;`（需补 `import mypview.core;`）

**验证**：mypc + myp_self 对 `import mypview.core + mypview.controls.app_icon`、
`import mypview.controls`（AppIcon/Button/NumberInput）、`import mypview.uix`
（UixLoader）编译运行输出一致；parent 312/312、bugs 11/11、bootstrap 16/16、
mypview UIX/PIPE PASS。

### v3.12.51 — mypview 打包为 MYP 标准包（`import mypview;`）+ BUG-044 修复

**mypview 包化**：mypview 从「源码集合」升级为 MYP 标准包——
- 新增 `mypview/package.myp`（`name: mypview, version: 1.0.0`）。
- 新增聚合主模块 `mypview/src/mypview.myp`：用相对路径 import 递归聚合
  src/ 下全部 51 个非 SDL 源文件（core → controls → layout → uix → animation）。
  借助 `loadModule` 的递归子 import 机制，`import mypview;` 一次合并整个框架，
  与「多文件编译」语义完全等价；`src/backend/sdl_renderer.myp`（需 SDL/ttf）
  默认不引入。
- 全链路验证：`myp install <mypview>` → `myp_packages/mypview/` → 消费者
  `import mypview;` + `mypc/myp_self --package-path` 编译运行输出一致
  （`pkg button=Login label=hello num=50 slider=60`）。README 增「方式二：
  作为 MYP 包」，路线图「包化分发」勾选。

**BUG-044（mypc 接口默认实现 stub 用残留返回类型生成 myp_retain(i32)）**
- 打包验证暴露：`import mypview;` 编译时 LLVM verify 失败
  `call void @myp_retain(i32 1)`；同一批文件直接多文件编译正常——类 codegen
  顺序不同暴露。
- 根因：`CodeGen::generateClassDefaultAction`（生成 `__ifdef_View_<m>_<C>` 接口
  默认实现 stub）**未设置 `current_ret_ti_`**，return 语句用上一函数残留类型判断
  是否 ARC retain。
- 修复：补设 `current_ret_ti_`，并与 generateClassAction 对齐
  `arc_skip_retain_return_`/`arc_pending_temps_`。
- 验证：parent 312/312、bugs 11/11、mypview UIX/PIPE PASS；双编译器 import 包
  消费者输出一致。

### v3.12.50 — 文件顺序敏感修复（BUG-041）+ 自举内部符号改 internal（BUG-042）

**背景**：对比 mypc 与自举编译器编译的 mypview 二进制时发现：
- 同样的 55 个文件，仅**顺序不同**（run.sh 手工顺序 vs 字母序），mypc 编译结果
  一个 69 行全绿、一个运行段错误（ConstraintLayout 对象悬垂被字符串覆盖）。
- 自举 myp_self 二进制比 mypc 多导出 ~90 个全局函数（`__myp_destroy_*`/
  `__myp_coro_*` 应为 internal）。

**BUG-041 — 方法调用 callee 选择 fallback 按类序 → 同名方法错调（已根治）**
- 根本原因（src/codegen/codegen_expr.cpp）：`cols_[i].layout()`（UixLoader
  `layoutAll()` 遍历 `LinearLayout[]` **类属性数组**元素）的方法解析用
  `memberObjectClassName`（查 `array_elem_class_map_`，该 map **只记录局部变量
  数组**）→ 类属性数组缺失 → **fallback 按类注册顺序找第一个同名 `layout`
  方法** → 字母序下 ConstraintLayout 先注册 → 错调 `ConstraintLayout_layout`
  → 对象 ARC/字段错乱崩溃。run.sh 顺序（linear 在 constraint 前）碰巧正确。
- 修复：callee 选择两处（主选 + fallback）优先用 sema 的
  `ma.resolved_object_class`（静态元素类型，**与文件顺序无关**），为空才回退
  `memberObjectClassName`。字母序 mypc 编译运行 uix_logic 69 行（原崩溃）。
- 验证：字母序全量 69 行；父级 312/312、bugs 11/11、bootstrap 16/16、
  mypview UIX/PIPE PASS。

**BUG-041b — myp_self 对「绝对路径源码 + 相对 target」混合路径丢 main**
- 根因：myp_self 对绝对/相对混合路径的 target 处理错，未生成/链接 main。
- 修复（mypview/examples/build.sh）：源码/标准库一律用相对路径。
- 验证：myp_self 编译 player/counter 全绿。

**BUG-042 — selfhost 内部析构/协程入口生成为 global 符号**
- 修复（tools/selfhost/src/codegen.myp）：`__myp_destroy_*`（10462）、
  `__myp_coro_entry_*` 方法/函数入口（2894/2922）三处改为 `define internal`。
- 效果：全局函数 T 符号 130 → 40（与 mypc 的 42 几乎对齐）；`.dynsym` 不变
  （58 个，不影响动态链接）；strip 无影响。
- 验证：mypview 69 行输出与修复前一致；bootstrap 16/16、父级全过、bugs 11/11、
  mypview UIX/PIPE PASS。

### v3.12.49 — 真实 SDL 绘制示例 player 自举运行全绿：接口局部变量借用 retain（BUG-040）

用 `myp_self` 编译 mypview 真实窗口示例 `examples/player.myp`（SDL 大窗口 +
全控件 + 帧循环）——**编译 + 运行成功**（120 帧全绿：`frame=120 list=8 q=低
vol=5`、ttf-cache hits=6310）。此前 uix_logic headless 测试 `.draw()` 调用为 0，
真实绘制路径的接口数组遍历 ARC 缺口在此暴露。

**BUG-040 — 接口局部变量初始化借用 fat 不 retain → draw 崩溃**
- 症状：`LinearLayout_draw` 里 `View kid = kids_[i]` 后 `kid.draw(r)` 段错误
  （`call *0x30(%rbx)` 中 rbx=0，或 this 对象 kids_ 字段 = 垃圾）。
- 根因（codegen.myp 局部接口变量初始化）：`View kid = kids_[i]`（接口数组元素
  **借用 fat**）走 `it=="{ptr,ptr}"` 分支直接 store **不 retain**；局部接口变量
  是 arcSlot，作用域末 `releaseArcSlots` 释放 data → **释放借用** → kids_ 悬垂。
  `buildIfaceFat` 分支（借用具体类实例）同样缺 retain。
- 修复：局部接口变量初始化两分支对**借用**（`isFreshTemp==0`）store 前
  `extractvalue 0 + myp_retain(data)`（局部持有，作用域末释放配对）；fresh（new）
  保持 `consumeTemp` 转移。
- **附带修复**：`mypview/examples/build.sh` 固定文件列表过时（缺
  sortable_list/long_press_button/gesture/theme/dialog 等）→ 改用目录通配符，
  player 编译通过（mypc 与 myp_self 均验证）。
- **验证**：player 120 帧全绿；bootstrap 16/16、父级 312/312、bugs 11/11、
  mypview UIX/PIPE PASS。
- **教训**：接口 fat「借用 vs fresh」判定须贯穿所有路径（局部接口变量/接口数组
  store/方法参数返回/赋值 RHS）。凡借用 fat 被 ARC 槽持有，作用域末 release 前
  必须先 retain；只有 new/fresh 才转移。headless 测试不 draw 会漏真实绘制 ARC 缺口。

### v3.12.48 — mypview 全集自举编译运行全绿：接口数组元素 ARC（BUG-037/038）+ UTF-8 双重编码（BUG-039）

用 `myp_self`（自举编译器）编译 mypview 全集（src/core+controls+layout+uix+animation +
uix_logic 测试，63 文件）——**编译通过 + 运行全绿**（69 行输出与 mypc 参考一致，含中文）。
连同上文 v3.12.47 之前的修复，自举接口 fat pointer 缺口全部补齐。

**BUG-037 — 接口数组元素 = 具体类 `new` 缺 fat 上转 + fresh 转移**
- 症状：`nodes_[i] = new Label(...)`（`View[]` 接口数组元素 = 具体类 new）时 LLVM
  报 `'%t55' defined with type 'ptr' but expected '{ ptr, ptr }'`（编译期）；修类型
  错误后运行期对象提前释放（缺 fresh 转移）。
- 根因（`tools/selfhost/src/codegen.myp` Subscript 左值分支）：`elemLt ==
  "{ ptr, ptr }"`（接口数组元素）走 else 直接 `store {ptr,ptr} rv`，未像 Member
  分支那样 `upcastIface/buildIfaceFat` 把具体类裸 ptr 上转为 `{data, vtable}`；
  上转后也未消费 `new` 的 fresh temp → 语句末 `flushTemps` 双重释放。
- 修复：新增 `subscriptElemIfaceName(arr)`（数组元素接口名解析，镜像
  `subscriptElemLt` 形态）；Subscript 分支 `elemLt=="{ptr,ptr}" && rt=="ptr"`
  → `buildIfaceFat` + `isFreshTemp(rawV)!=0 → consumeTemp(rawV)`（对齐局部接口
  变量初始化的 `arcConsumeTemp`）。

**BUG-038 — 接口数组元素 store 借用 fat 不 retain → 对象悬垂、内存被字符串复用**
- 症状：myp_self 编译的 mypview 运行段错误 139。gdb 逐层定位：sync 的
  `setAttr` 接口调用参数全部正确（this/name/val），但 this 对象 `text_` 槽 =
  `0x65646f6e20786975`（"uix node" 字符串内容）；watchpoint 捕获对象被
  `myp_strcat`（walkParents 拼接 "children"）写入 → **对象内存被字符串拼接复用**。
- 根因：build 里 `Label l = new Label(...)`（rc=1 局部），`registerNode` 的
  `nodes_[nodeCount_] = v`（v 为接口 fat **借用**）→ Subscript 分支对
  `rt=="{ptr,ptr}"` 直接 store **不 retain**；build 返回时 `l` 作用域末释放 →
  rc 0 → 对象释放 → nodes_ 数组悬垂 → 内存被 `myp_alloc/myp_strcat` 重新分配。
- 修复（codegen.myp Subscript 分支新增 else-if）：`elemLt=="{ptr,ptr}" &&
  rt=="{ptr,ptr}"`（接口 fat 借用）→ `extractvalue 0` + `myp_retain(data)`
  （数组槽持有借用 fat）。
- 教训：**接口数组元素 store 的引用语义**——具体类 `new` → fat 上转 + fresh 转移
  （数组接管 rc=1）；借用 fat（局部/参数）→ 必须 `retain`（数组持有），否则调用方
  作用域末释放 → 悬垂。与 C++ oracle 的接口数组 store 语义必须一致。

**BUG-039 — 词法器 UTF-8 双重编码（中文乱码）※重要，后续编码处理必读**
- 症状：`myp_self` 编译含中文字符串字面量的程序，输出乱码（`你好` → `ä½ å¥½`），
  `Str.len` 返回字节数翻倍（6 → 12）；IR 里常量变 `c"\C3\A4\C2\BD\C2\A0..."`
  （9 字节，源码是 6 字节 `E4 BD A0 E5 A5 BD`）。`mypc`（C++ oracle）正常。
- 根因（`tools/selfhost/src/lexer.myp` `scanString`）：非转义字符
  `val.append(__myp_chr(advance()))` —— `advance()` 返回源码**原始字节**
  （UTF-8 多字节的一部分，如 `0xE4`），而 `__myp_chr` 按 **Unicode 码点**生成
  UTF-8（`0xE4` 是 `ä` 的码点 → 编码成 2 字节 `C3 A4`）→ **双重编码**。
- 修复：`scanString` 对 `>=128` 的源码字节改用 `Str.substring(source_,
  pos_-1, pos_)` **原样保留字节**；`<128`（ASCII）仍走 `__myp_chr`。
- **UTF-8 处理铁律（避免以后再犯）**：
  1. 源码文件是 UTF-8，词法器按**字节**读（`__myp_charcode` O(1)）。多字节字符的
     每个字节单独出现，`>=128` 的字节**不是**字符码，只是 UTF-8 序列的一部分。
  2. `__myp_chr(code)` 按**码点**生成 1–4 字节 UTF-8——两者只对 ASCII（`<128`）
     等价；把源码字节直接喂 `__myp_chr` 会双重编码。
  3. **字节级 round-trip 一律用 substring/memcpy 原样保留**（同
     `stdlib/io.myp readAll` 注释：不能用 `__myp_chr(b)` 逐字节拼）。
  4. 字符串长度：`Str.len` 是字节数（strlen）。源码字面量需逐字节复制；含中文时
     按字节 len 与 `mypc` 参考一致才正确（如 `你好` = 6 字节 = mypc `n=6`）。
- 影响面：所有 `myp_self` 编译的含中文/非 ASCII 字符串字面量程序。ASCII 不受影响
  （bootstrap 固定点不破）。

**验证**：mypview 全集 `myp_self` 编译运行 69 行全绿（`ok=7aff`、`sync
status=ok-login`、中文 `t=你好 n=6` 与 mypc 一致）；bootstrap **16/16**
（固定点 `myp_self2==myp_self3`）；父级 **312/312**；bugs **11/11**；mypview
官方 UIX/PIPE PASS。BUGLIST 已登记 BUG-036（🟩 完结）+ 新增 BUG-037/038/039（🟩）。

### v3.12.47 — 修复 BUG-031：跨线程多 @thread 目标事件无限重投（广播可用）

**BUG-031 根因**：mapping handler 注册 `instance=NULL` → `myp_event_dispatch` 第一遍
按 handler 归属 route 副本（`routed=1`）后，第二遍跑**所有**同 event 的 NULL-instance
handler（无归属 → 当前线程跑）——副本在目标线程把**其他目标**的 handler 也跑了，每个
handler 内 BUG-005 的 `myp_thread_is_current(inst)==0 → myp_event_route_to_instance`
检查又把事件 route 回其他目标线程 → **无限乒乓**（route 8.7 万+ 次，各 @thread 目标
收到 5 万+ 次，正常应各 1 次）。

**修复（C++ mypc + runtime + 自举 myp_self 三端一致）**：

1. `src/codegen/codegen_class.cpp` `generateMappingDecl`：handler 注册 `instance`
   从 NULL 改为**目标实例全局地址 `&__myp_inst_X`**（传地址非值——注册发生在
   `__myp_init`，早于实例 new；仅单目标普通目标链生效，lambda/transformer/函数目标
   或无类级实例全局回落 NULL，保留原行为）。
2. `src/runtime/runtime.c`：`myp_event_dispatch` 用 `myp_handler_target()`——注册
   instance 为全局地址时**运行时解引用**得真实实例，按线程归属路由——routed 副本
   只跑归属本线程的 handler，跨线程多目标各收 1 次。
3. `tools/selfhost/src/codegen.myp`：镜像注册 instance 逻辑（`regInst`）。

**验证**：新回归 `tests/bugs/cross_thread_multi_target.myp`（@test 断言 A/B 各收 1 次，
2 断言，已接入 `tests/bugs/run_bugs.sh` 门禁）；`run_bugs.sh` 7/7 全绿；全量回归
311 通过 / 0 失败；selfhost `test_myp_self.sh` 94/94 + bootstrap 16/16（不动点
myp_self2 == myp_self3 字节相同）。MOS `app_lifecycle_demo` 应用从非 @thread 改为
@thread（解除原规避），验证真实跨线程广播：AppManager（@thread）广播 Lifecycle 到
两个 @thread 应用各收 1 次（Notes lifecycle=1/2/3 各 1 次，无乒乓）；MOS ctest 11/11。
跨线程事件广播（1 事件 → 多 @thread 目标）自本版起可用。

### v3.12.46 — 通用桥接发现（新增桥无需改编译器）+ SDL_ttf 中文渲染 + BUG-029/030

**通用桥接发现（核心重构，`src/main.cpp` linkObjects）**：删除 per-bridge 硬编码
（SDL / SDL_ttf 各一段），改为 **symbol 驱动的通用发现——新增 bridge 无需再改编译器**：

- 候选桥接 = `bridges` 目录下的 `*.c`：默认 `<stdlib>/bridges`；可用 `MYP_BRIDGES`
  环境变量追加目录（冒号分隔多个，如 `MYP_BRIDGES=/path/to/mos/bridges`）。
- 程序 `.o` 的 `nm -u` 未定义符号与桥接 `nm --defined-only` 取交集，命中即自动
  编译（`/tmp/myp_rt_cache` 缓存，按源码+标志哈希）+ 链接。
- **固定点迭代处理依赖链**：桥接自身的未定义符号并入待解集合（如
  `sdl_ttf_bridge.c` 依赖 `myp_sdl_get_renderer` → 自动拉入 `sdl_bridge.c`）。
- 侧车文件（可选）：`<名>.c.cflags` 附加编译标志、`<名>.c.libs` 链接库
  （如 `sdl_bridge.c.libs` = `-lSDL2`）。
- 桥接文件自 `src/runtime/` 迁至 `stdlib/bridges/`（`sdl_bridge.c` /
  `sdl_ttf_bridge.c` + 两个侧车）；父 `CMakeLists.txt` `myp_runtime` 路径同步。
- 验证：纯控制台程序不链 SDL；`import sdl` 链 `-lSDL2`；`import ttf` 自动拉
  `-lSDL2_ttf`+`-lSDL2`；`MYP_BRIDGES` 下新增测试桥（含 `-lm` 侧车）不改编译器
  即自动编译链接运行。
- **自举编译器镜像（`tools/selfhost/src/link.myp`）**：同样实现通用桥接发现——
  `bridgeDirs`（MYP_BRIDGES + `<stdlib>/bridges`）/ `listBridges` / `nmSymbols` /
  `readSidecar` / `compileBridge`（mtime 缓存）/ `symIntersect`，link() 内固定点
  迭代依赖链 + 链接命令追加桥 obj/libs。用 `myp_self` 编译 `import sdl` 链
  `-lSDL2`、`import ttf` 拉 `-lSDL2_ttf`+`-lSDL2`、`MYP_BRIDGES` 新桥自动链接
  运行（5/120/5）均验证通过。

**SDL_ttf 中文渲染（M1 里程碑）**：新增 `stdlib/ttf.myp`（`Ttf` 静态类：
`init(px)` / `drawText(x,y,text,scale,r,g,b,a)` / `close()`），经通用桥接自动链接
`stdlib/bridges/sdl_ttf_bridge.c`（TTF_RenderUTF8_Blended + Noto CJK 等系统字体，
抗锯齿中文，替代 5×7 位图仅 ASCII）。`sdl_bridge.c` 增加 `myp_sdl_get_renderer` /
`myp_sdl_get_window` 访问器供复用。MOS 新增 `TtfLabel` 控件 + `apps/ttf_demo.myp`
（headless 冒烟：init / drawText 返回 0）。

**修复 BUG-029（类字段 → interface vtable 空指针崩溃）**：sema/codegen 按当前类
属性表 + 表达式类型解析具体类名（含 `this` / 字段访问），自举 myp_self 镜像
`upcastClsName`。**修复 BUG-030（constructor 内发 mapping 事件崩溃）**：构造函数
入口先写 `class_instance_globals_` 实例指针，自举 `curFnIsCtor_` 标记。`SDL2` 在
`CMakeLists.txt` 改为 `pkg_check_modules(... QUIET)` 可选（无 SDL2 环境仍可构建
mypc 全工具链）。

### v3.12.45 — 接口分派去虚拟化（devirt）+ 自举 slice 边界检查补齐 + 新基准

**去虚拟化（C++ mypc 与自举 myp_self 双端镜像）**：接口方法调用
`Shape s = new Circle(...); s.area()` 此前一律 vtable 间接调用
（extract {data,vtable} → GEP vtable[midx] → load fn ptr → 间接 call）。当对象
具体类在调用点静态已知且从未被重赋值时，改为**直接调用具体类方法**，让 LLVM
内联 → 常量折叠/向量化复利。

- **安全边界（关键设计）**：devirt 仅在**接口变量从未被重赋值**时触发。sema 在
  变量声明时记录具体类快照（concreteClass），任何赋值（含复合赋值/条件分支内）
  标记 reassigned → 后续调用回退 vtable。流不敏感保守：条件分支内重赋值也放弃
  devirt。`new` 表达式接收者本就直接调构造，不涉 vtable。
- **selfhost**：
  - `sema.myp`：`SymbolEntry` 新增 `reassigned_` 字段；`Assign` 分支对接口变量
    重赋值标记；B3 块扩展——从仅 assoc 关联类型方法扩展为**所有接口方法调用**
    （对象是接口变量 + concreteClass 已知 + 未重赋值 → `CallExpr.resolvedClass`
    记具体类）。
  - `codegen.myp` `genIfaceCall`：`resolvedClass` 非空 → 直接调
    `<cls>_<method>`（类覆盖）或 `__ifdef_<iface>_<method>_<cls>`（trait 默认
    实现，复用 findIfaceDefault），跳过 vtable。发射的 extract/GEP/load 在 -O2
    被 DCE（无副作用）。
- **C++ 镜像**：
  - codegen 接口分派点（泛化分派）：接口变量（Identifier 对象）`var_class_map_`
    命中且未重赋值 → 直接调具体类方法（返回类型从具体函数取，含 assoc 真实
    类型）。
  - `generateAssignment` 对 `var_class_map_` 中的变量赋值 → 标记 `iface_reassigned_`。
  - **修复 catch(Error) 污染**：`catch (FileError e)` 曾设 `var_class_map_["e"]`
    残留具体类 → 后续 `catch (Error e)` 的 `e.message()` 被 devirt 误用 FileError
    （exception/exception_lib 回归暴露，`iface_parse` 输出 file error 而非 parse
    error）。修复：iface_catch 绑定处 `var_class_map_.erase(cc.var_name)`——
    catch 接口变量的具体类运行时决定，永不可 devirt。
- **边界检查补齐（自举对齐 C++，正确性）**：selfhost slice 下标此前是裸 GEP
  **无边界检查**（越界静默读穿），C++ `generateSliceElementAddress` 有完整
  `0<=idx<len` + `myp_bounds_error`。新增 `sliceElemAddrChecked` helper（读/写/
  `subscriptElemAddr` 三处统一），文本 IR 发射 `icmp sge/ult` + `and` + 分支到
  error block（`myp_bounds_error`）+ ok block GEP；`ir_emit.myp` 补
  `declare void @myp_bounds_error(i64,i64)`。越界双端一致报
  `slice index 5 out of bounds (length 4)` + abort。实测 LLVM -O2 对
  `for(i=0;i<n)` + `len==n` 形态完全消除检查 → **无性能损失**。
- **新基准**：
  - `bench/myp/iface_dispatch.myp`：接口热循环分派（Shape 接口 3 实现，**有状态
    方法 grow 改内部字段**防常量折叠，devirt 后内联仍须计算，测真实分派开销）。
  - `bench/myp/slicedot.myp`：`slice<double>` 点积（边界检查开销度量）。
  - 已加入 `run_compare.sh`（44 → 46 项）。
- **基准结果（devirt 后，3 轮取最小）**：iface_dispatch **MYP 30ms vs C++ 46ms
  （C++/MYP 1.53，devirt 后内联反超 g++ 保守 devirt）**；slicedot MYP 4ms vs
  C++ 6ms（1.50，边界检查被 LLVM 消除）；其余 44 项与基线一致无回归。
- **验证**：新增回归 `tests/@test/devirt_reassign.myp`（未重赋值 devirt / 重赋值
  回退 vtable / 条件分支重赋值保守 / 独立变量 devirt，双端 5/5）。全量 **311/0**
  （-O0 与 -O2）；selfhost `@test` 94/94；bootstrap 16/16 不动点（self2==self3
  字节相同，md5 6b67c55e…）。

### v3.12.44 — 修复 `-O2 × setjmp/longjmp` 根因：try 入口逃逸全部在作用域局部 + finally flag

根治 B2 遗留的「opt -O2 破坏 setjmp/longjmp 的 finally 语义」根因（C++ 与自举同修），
`-O2` 现在可以安全使用。

- **根因**：LLVM 的 CFG **不建模 longjmp 这条边**——try 块以 noreturn
  `__myp_longjmp` + `unreachable` 结尾，于是：
  1. try 块内对跨 setjmp 存活局部的 store 被死代码消除（DCE 认为 try 块后面不可达）；
  2. longjmp 路径（finally/catch）对这些局部的 load 被折叠成入口值——mem2reg/SROA
     把跨 setjmp 的局部提升为 SSA 寄存器，longjmp 恢复后读到的不是 try 内写的值。
  实测 `arc_throw` 的 `fin_run`：try 内 `fin_run=1` 被 DCE、finally 里 `fin_run+1`
  被整块消除，断言 `myp_assert_eq(i32 0, i32 2)`（应 2）。C++ `mypc -O2` 同样失败
  （v3.9.0 只对 ARC 槽逃逸，标量局部未覆盖）。
- **修复**：try 入口（setjmp 前）把**全部在作用域局部 + finally flag 的地址**传给
  `myp_try_escape` 无操作（运行时空函数），使它们成为 LLVM 眼中的逃逸内存——
  mem2reg/SROA 不再提升、DSE 不删 store，finally/catch 从物理内存读到真值。
  jmp_buf 经 `myp_exception_push`/`setjmp` 传地址本就逃逸，无需额外处理。
  - 自举 `tools/selfhost/src/codegen.myp` `genTryStmt`：遍历 `localAllocas_` 发射
    `myp_try_escape`；`ir_emit.myp` 补 `declare void @myp_try_escape(ptr)`。
  - C++ 镜像：`CodeGen::escapeSlot` 助手（抽出原 registerArcSlot 内联逻辑），
    `generateTryStmt` 遍历 `named_values_` 全作用域指针值 + finally flag。
  - 自举源码自身不用 try → 逃逸不会进入自举编译器自身 IR，bootstrap 不动点不受影响。
- **附带修复测试脚本引号 bug（MYPCC 带参数时失效，v3.9.0 同类遗留）**：`run_tests_O2.sh`
  设 `MYPCC="./build/mypc -O2"`（含空格）时，`test_coro_stack_warn.sh` /
  `test_package_path.sh` 的 `"$MYPCC"` 把整串当单个文件名、`tools/codegen/run_tests.sh`
  的绝对化 `basename` 带进 `-O2` → 均退出 127/找不到编译器。修复：调用处去引号
  （`$MYPCC`），`tools/codegen/run_tests.sh` 二进制取首词绝对化（`MYP_ABS`）+ 保留
  flags，`MYP_CC` 导出二进制绝对路径。
- **附带修复 compile 模式 stdlib 回退**：自举 CLI 直接 compile（非 `run` 子命令）未显式
  给 `--stdlib` 时 stdlib 为空 → 从子目录（如 `bench/`）编译时 `Link.link` 的 runtime
  路径回落 CWD 相对（`src/runtime/runtime.c` 找不到 → 链接失败）。修复：compile 模式
  stdlib 为空时回退 `Cli.selfStdlib()`（与 `run` 子命令一致）。`bench/run_compare.sh`
  用 `myp_self2` 跑全部 44 项基准不再失败。
- **验证**：
  - `arc_throw`：**C++ 与自举 -O0/-O2 均 15/15**；IR 复核：-O2 后 finally 里
    `add i32`（fin_run 自增）存活、断言不再折叠为 0。
  - 全量回归 **-O0 310/0**、**-O2 310/0**（此前 -O2 arc_throw 必挂）。
  - 自举 `@test` 全套 **-O0 104/0、-O2 104/0**；`test_myp_self.sh` 94/94；
    `test_myp_bootstrap.sh` 16/16 不动点（self2==self3 字节相同，md5 dd7fc3c7…）。
  - 异常专项（exception/exception_lib/exception_thread/exception_throwin +
    @test arc_throw/result）自举 -O2 全过。
- **默认优化级别调整为 -O2**：根因修复后 **C++ `mypc` 与自举统一默认 -O2**——
  mypc `src/main.cpp`（`compileSingle`/主流程/`run` 子命令默认 `opt_level` 0→2，
  `--help` 文本同步）、自举 `main.myp`/`link.myp`（CLI/`run`/`fmt` 统一默认
  `optLevel=2`；v3.12.10 曾默认 -O2、B2 回退 -O0、本版本恢复）。`-O0/-O1/-O3` 显式
  覆盖、`MYP_SELF_OPT=0` 强制关闭。opt 步骤编译开销 +24%，换取生成程序 3-23x 运行
  提升。自举链全程 -O2 编译，bootstrap 不动点保持（self2==self3 字节相同，
  md5 d62f9f45…）。
- **性能权衡**：逃逸只作用于含 try 的函数（try 入口每在作用域局部一次无操作调用，
  该函数局部不再提升寄存器）；自举编译器自身无 try → 编译/运行性能不受影响。
  默认 **-O2**（生产性能），`-O0`/`MYP_SELF_OPT=0` 快速编译/调试。

### v3.12.43 — 自举编译器 B 类缺口：assoc 关联类型 / 嵌套 struct / 多文件合并全绿

自举（selfhost）编译器追赶 C++ 编译器的 B 类功能缺口，**全量回归 310 通过 / 0 失败**。

- **B1 slice_class_chain**：`new Node(7).getVal()` 链式调用——CallExpr Member 分支
  cn 解析漏了 `New` 对象（`mo.kind()=="New"` 时取 `resolvedClass`）。自举
  `tools/selfhost/src/sema.myp` 补 `|| mo.kind() == "New"`（镜像 C++）。
- **B2 arc_throw finally**：自举 link 默认 `opt -O2` 把 setjmp/longjmp 跨 finally 的
  alloca mem2reg 提升 → longjmp 后 finally 局部值被清零。改为**默认 -O0**（仅
  `-O` 标志或 `MYP_SELF_OPT=1` 才跑 opt），对齐 C++ 默认。`arc_throw` 15/15。
- **B3 assoc 关联类型 `T::Item`**：接口变量 `Container sb = new StrBox(); sb.getVal()`
  此前返回接口占位 `assoc` → 与真实 `string/int` 类型不匹配。自举 sema 用
  **作用域感知的符号条目具体类字段**（`SymbolEntry.concreteClass_`，镜像 C++
  `var_class_map_`）解析接口变量具体实现类的同名方法返回类型，并把具体类记到
  `CallExpr.resolvedClass` 供 codegen 分派用；codegen `genIfaceCall` 关联类型返回
  类型改从具体类取。`assoc_string_dispatch` 4/4、`manual_ch6_class` 16/16。
- **B4 manual_ch7_struct**：嵌套/文件级限定 struct（`Sensor::Config`、`Device::Mode`）：
  - sema：`typeToKind` 限定 struct 名**优先于关联类型判定**（否则误判 assoc）；
    `findStruct`/`inStructName` 支持限定名→裸名回退；嵌套 struct 注册提前到顶层
    函数/测试体访问前。
  - codegen：`emitStructTypes` 补发嵌套 struct 类型并给全限定 key；LLVM 标识符
    mangling（`::` → `$`，`llvmSafeName`）用于 struct 类型名/方法函数名；struct
    方法内 `this.field` 分支（镜像 C++ `generateStructMemberAddress` ThisExpr）；
    struct 兄弟方法裸调用 → `struct_<key>_<fn>`。`manual_ch7_struct` 12/12。
- **B5 multifile 多文件合并**：compile 模式此前只编译首个输入文件。改为收集全部
  位置参数为输入文件，`Frontend.compile` 逐文件解析 + **全量合并**
  （imports/structs/bitfields/classes/interfaces/mappings/functions/enums/ffis/
  macros/typeAliases，镜像 C++ multi-file），命令行文件预置 loaded 防重复，再统一
  加载所有 imports。`test_multifile.sh` 4/4（含 BUG-025/026）。
- **run 子命令 stdlib 定位**：`Cli.selfStdlib()` 增 cwd 向上搜索
  （`../../stdlib`…），修复从子目录（tools/codegen）跑 `run` 时
  `cannot find import 'env'` → codegen 工具自测通过。
- **回归**：全量 **310 通过 / 0 失败**；自举 `test_myp_self.sh` 94/94；自举
  `test_myp_bootstrap.sh` 16/16 不动点（self2 == self3 字节一致）。自编译
  `build/myp_self2` 与仓库布局对齐后 pm/gitee/LSP 位置推断回归全绿。


### v3.12.42 — 修复 BUG-005：mapping 事件 action 在事件源线程执行（跨线程路由）
- **BUG-005 已修复**：mapping handler 的目标 action 此前在**事件源线程**执行，
  `@thread` 实例 B（action）的线程归属被忽略。改为按 **handler 实例**线程归属投递。
- **修复**（C++ + 自举同修）：
  - `src/runtime/runtime.c`：`myp_event_fire` 增加 `data_size` 参数（载荷深拷贝按
    字节数）；新增 `myp_thread_is_current(instance)` / `myp_event_route_to_instance(...)`
    运行时；`myp_event_t` 增 `data_size`/`data_owned`/`routed` 字段；dispatch 对归属
    其他线程的 handler 将事件深拷贝投到其线程队列，路由副本 `routed=1` 不再重复
    路由，处理后 free 拷贝；新增 `myp_thread_self()` 诊断 FFI（线程稳定 id）。
  - `src/codegen/codegen_class.cpp` `generateMappingDecl`：handler 内对首个非静态
    目标实例做 `myp_thread_is_current` 检查——目标在其他线程 → 调
    `myp_event_route_to_instance` 后返回；否则直接调用。
  - `src/codegen/codegen.cpp` / `include/mylang/CodeGen.h` / `runtime.h`：FFI 声明
    更新（fire 4 参 + 两个新运行时）。
  - 自举镜像：`tools/selfhost/src/codegen.myp` genMappingChain 同检查；
    `genThreadVar` 补存 `@__myp_inst_<Cls>` 全局（原缺失 → handler 取 null）；
    `ir_emit.myp` 更新 `myp_event_fire` 4 参声明 + 新增运行时 declare。
- **回归**：`tests/bugs/mapping_thread.myp`（`myp_thread_self()` 断言 handler 在
  handler 实例自己的线程执行，3 断言）。全量回归 **308 通过 / 0 失败**；bugs 4 绿；
  自举 `test_myp_self.sh` 94/94。


### v3.12.41 — 修复 BUG-011：函数内 mapping 用实例变量名节点 → 编译期诊断
- **BUG-011 已修复**：函数内 `mapping(){ s.e -> t.a; }`（s/t 为局部实例变量）此前
  在 handler 函数里 load 外层函数的局部 alloca → 跨函数指令引用 → LLVM verify
  `Referring to an instruction in another function!`。改为编译期诊断：mapping 节点
  须用类名（实例级映射暂不支持），消息带真实类名提示（`Source.e`）。
- **修复**：`src/sema/sema.cpp` MappingStmt 访问器 + 自举 `tools/selfhost/src/
  sema.myp` `analyzeMapping` 同镜像。
- **回归**：`tests/negative/instance_mapping.myp`（编译拒绝）。文件级/类名节点
  mapping（`tests/@test/instance_mapping.myp` 等）不受影响。


### v3.12.40 — 修复 BUG-014 + BUG-010：原子 load/store + 裸 struct 属性字段链
- **BUG-014 已修复**：`Atomic.loadInt`/`storeInt` 此前编译成**普通非原子** load/store
  （仅命名带 Atomic）。改用原子 `LoadInst`/`StoreInst` 构造器（seq_cst；LLVM 21
  IRBuilder 无 CreateAtomicLoad/Store），与 add/sub/xchg/addDouble 的 atomicrmw
  一致。自举 `tools/selfhost/src/codegen.myp` 同镜像（`load atomic`/`store atomic`）。
  回归：`tests/@test/atomic_load_store.myp`。
- **BUG-010 已修复**：裸 struct 属性字段读写 `p.x`（`property: Point p;`，即
  this.p.x）——读此前 `genExpr(p)` 加载 struct 值当指针 GEP → LLVM verify 失败；
  写落到 "external obj.prop" 错误/属性非首属性时 break 提前退出。
  - 读：`generateMemberAccess` 加裸 struct 属性分支；属性遍历 `continue`。
  - 写：`generateAssignment` 在 `if(!op)` 内、错误兜底之前加裸属性分支（原 2222
    块位于错误之后**不可达**——死代码，已移除）；`generateStructMemberAddress`
    ThisExpr 分支支持类 struct 属性（`this.s.x`）。
  - 自举镜像：memberAddr/memberFieldType/memberFieldAstType 加 `bareStructPropName`
    分支。
  - 回归：`tests/@test/struct_prop_chain.myp`（裸/显式 this 读写，9 断言；C++ 与
    自举均绿）。


### v3.12.39 — 修复 BUG-015 + BUG-008 + BUG-012 + BUG-009 + BUG-006：sema 校验类
- **BUG-015 已修复**：`mypc --package-path` 按 `:` 切分多路径（`src/main.cpp
  loadModule`），与自举 `myp_self` 一致。回归：`tests/test_package_path.sh`。
- **BUG-008 已修复**：接口 action 签名匹配升级为**精确签名**（名称 + 参数类型 +
  返回类型；`paramsMatch`），事件按名称 + 参数类型；关联类型保留仅名称匹配。
  自举 sema.myp 同镜像。回归：`tests/negative/interface_param_mismatch.myp`。
- **BUG-012 已修复**：对 `@thread` 实例直接调用普通 action → 编译拒绝
  （`cross-thread calls must go through mapping()`）；`@startup` 手动调用规则保留。
  自举 sema.myp 同镜像。回归：`tests/negative/cross_thread_call.myp`。
- **BUG-009 已修复**：一个类多个 `@startup` → 编译诊断 `at most one @startup per
  class`。自举 sema.myp 同镜像。回归：`tests/negative/multiple_startup.myp`。
- **BUG-006 已修复**：`main()` 直调检查被运算符/管道绕过——`visitBinaryOp`（外部
  `@op`）与 `visitPipe`（class transform）在 main() 内拒绝；struct 方法调用保留
  放行。自举 sema.myp 同镜像。回归：`tests/negative/main_not_wiring.myp`。


### v3.12.38 — 修复 BUG-022 + BUG-007：sema 校验类（@thread / bitvector）
- **BUG-022 已修复**：`@thread` 仅可用于 class 实例——struct 加 `@thread` 编译
  拒绝（`'@thread' can only be applied to a class instance variable`）。自举
  sema.myp 同镜像。回归：`tests/negative/struct_thread.myp`。
- **BUG-007 已修复**：`bitvector<N>` 宽度校验 ∈ {8,16,32,64}，否则编译拒绝
  （`bitvector width must be 8/16/32/64`）；codegen default 分支不再静默用 i32。
  自举 sema.myp 同镜像。回归：`tests/negative/bitvector_width.myp`。


### v3.12.37 — 修复 BUG-013：Coro.resume 返回值串值（yield/resume 值改每协程存储）
- **BUG-013 已修复**：`src/runtime/runtime.c` 用 `__thread` 线程本地共享槽
  `myp_coro_yield_val`/`myp_coro_resume_val` 存「上次挂起传出的值」与「上次 resume
  传入的值」。同线程多协程混用时后挂起者覆盖前者：echo 挂起 10 被 topLevel
  `Coro.yield(42)` 覆盖 → `Coro.resume(echo_h, 100)` 返回 42（应 10）；加
  `Async.sleep` 定时器挂起覆盖为 0。协程内部值传递始终正确，只有 resume 返回值串。
- **修复**：yield/resume 值改**每协程存储**——`myp_coro_t` 新增 `yield_val`/`resume_val`
  字段；`__myp_coro_yield`/`__myp_coro_resume` 按目标协程槽读写；`__myp_coro_create`
  槽复用/新建时清零。多协程、嵌套 resume 均按各自槽取回。
- **回归**：`tests/@test/coro_resume_value_mix.myp`（echo await 值挂起 10 + topLevel
  Coro.yield 42 + timerCoro Async.sleep 挂起 0 三协程混用，3 断言，3 次运行稳定）；
  `tests/bugs/coro_resume_value_mix.myp` 移除。
- 全量回归 **300 通过 / 0 失败**（含 tests/coro、tests/coro_top 既有协程用例）。
  design.md §8.6.1 的规避写法（不打印 resume 返回值）现可放开。


### v3.12.36 — 修复 BUG-018：类型参数全局作用域泄漏（collections + where 约束伪错误）
- **BUG-018 已修复**：`src/sema/sema.cpp` `visitClassDecl` 把类**通用类型参数**在
  `enterScope()` **之前**（全局作用域）声明——类作用域弹出后 T 残留全局符号表；后续
  同名类型参数泛型类覆盖全局 T。`import collections`（`Set<T>` 无约束 T→Int）+ 用户
  `Processor<T where T:Container>`（T→Container 接口）→ 检查 Set<T> 模板体
  （`val % cap_`、`data_[i] < x`）时 T 解析为 Container → 8 个伪错误
  `expected numeric type, got 'Container'`（行号落 stdlib）。
- **修复**：类型参数注册移到 `enterScope()` 之后（类作用域内），弹出即清除——同名
  类型参数不再跨类泄漏/覆盖。与 BUG-021（current_class_name_ 污染）同类。
- **回归**：`tests/@test/assoc_constraint_import.myp`（collections + `where
  T:Container` + `T::Item` + `Processor<IntBox>` 实例化，1 断言）；`tests/bugs/
  assoc_constraint_import.myp` 移除。自举编译器天然无此 bug（Pass A/B 隔离）。
- 全量回归 **299 通过 / 0 失败**。


### v3.12.35 — 修复 BUG-023：@parallel/@gpu 并行体直接访问 static 属性数组
- **BUG-023 已修复**：`@parallel for` / `@gpu for` 并行体直接读写 `@static class`
  属性数组（`X.arr[i] = i`）→ `emitKernelExpr` 静态属性分支要求类名在 `kernel_vars`
  （并行体只捕获外层局部变量）→ 落到 `i64 0` 占位 → 下标 GEP 基址为整数 0 → LLVM
  verify 失败（`getelementptr i32, i64 0, %0`）；`Atomic.addInt(X.sum,...)` 传 0 占位
  当数组指针 → 运行段错误 139。
- **修复**（`src/codegen/codegen_gpu.cpp`）：MemberAccess 静态属性分支直接以模块全局
  `__myp_static_<Class>` 为基址 GEP 进属性槽（CPU `@parallel` 同模块直取）；`@gpu`
  核函数（独立 PTX 模块）仍走捕获的 kernel arg（`kernel_vars` 命中时优先）。
- **回归**：`tests/@test/parallel_prop_access.myp`（静态属性数组写 + 读 +
  `Atomic.addInt` 原子累加，4 断言，3 次运行稳定）；`tests/bugs/parallel_prop_access.myp`
  移除。
- 全量回归 **298 通过 / 0 失败**；自举 94/94。


### v3.12.34 — 修复 BUG-028：类属性带 ARC 初始化器 → 悬垂/双释放
- **BUG-028 已修复**：`property: Foo f = new Foo();`（class/interface/string/slice/
  数组属性带初始化器）——属性默认值发射（`src/codegen/codegen_expr.cpp`）对 fresh
  `new Foo()` 直接 store 到属性槽，**未 `arcConsumeTemp`** → rc=1 留在语句末临时释放
  列表 → 语句末 release → 属性槽悬垂。读取 use-after-free；setter 重赋值释放悬垂旧值
  → 双释放 → 运行段错误 139。
- **修复**：属性初始化器与 `this.prop = value` 赋值路径同语义——ARC 引用属性
  （class/interface/string/slice/counted-array）`arcStoreRef`/`arcStoreSlice` +
  `arcConsumeTemp`；alias retain、fresh consume（`isFreshArcExpr`）。
- **自举镜像**：`tools/selfhost/src/codegen.myp` 属性默认值发射同样未 consumeTemp（IR
  复核：fresh 对象语句末 `myp_release` 悬垂）→ 同修复：`ft=="ptr" && isArcType` 走
  `storeRef(gep, pv, isFreshTemp(pv))`。IR 复核不再语句末释放；`test_myp_self.sh` 94/94。
- **回归**：`tests/@test/property_init_arc.myp`（初始化器对象存活 + 多次重赋值读取，
  3 断言）。BUG-021 修复验证时暴露（属性初始化器此前无编译通过的用例）。
- 全量回归 **296 通过 / 0 失败**。


### v3.12.33 — 修复 BUG-021：class 含泛型类属性时 `this.prop` sema 解析污染
- **BUG-021 已修复**：`src/sema/sema.cpp` `visitClassDecl`（泛型实例化入口）设置
  `current_class_name_` 后**不恢复** → class H 含 `Option<int> o` 属性时，Pass 2
  `buildCurrentClassMemberTypes` 解析属性类型触发 `Option<int>` 实例化 → 退出后
  `current_class_name_` 残留 `Option_int_inst` → 方法内 `this.v` 解析到实例类 →
  `class 'Option_int_inst' has no member 'v'`（读+写都中）。
- **修复**：`visitClassDecl` 开头保存、末尾恢复 `current_class_name_`（类上下文不污染）。
- **回归**：`tests/@test/this_generic_prop.myp`（`Option<int>` + `ArrayList<int>` 泛型
  属性 + `this.v` 读写 + 泛型属性方法调用，4 断言）；`tests/bugs/this_generic_prop.myp`
  移除。验证时顺带暴露 BUG-028（属性初始化器 ARC）。
- 全量回归 **296 通过 / 0 失败**。


### v3.12.32 — 修复 BUG-017：关联类型接口方法返回 string 经接口分派类型错误
- **BUG-017 已修复**：接口虚表动态分派处（`src/codegen/codegen_expr.cpp` 三处）返回
  类型一律取接口声明的关联类型占位符 → `typeNodeToCodegenType` 回落默认 **i32**，而
  具体类方法返回 string（ptr）→ `call i32 %iface_fn(ptr %4)` 把 string 当 i32 → 调用方
  （期望 ptr）LLVM verify 失败（单方法接口）/ 运行段错误 139（含其他方法时）。`Item=int`
  因默认类型恰为 i32 侥幸通过。
- **修复**：新增 `CodeGen::ifaceDispatchReturnType`——优先从对象已知具体类
  （`var_class_map_` / `array_elem_class_map_`）解析其同名方法返回类型（与 vtable 指向的
  具体方法一致），未知回落接口声明类型；三处分派点统一改用。
- **回归**：`tests/@test/assoc_string_dispatch.myp`（string+int 双关联类型 + 多方法接口
  动态分派，4 断言）；`tests/bugs/assoc_string_dispatch.myp` 移除。泛型单态化路径
  （`Processor<T where T:Container>` 静态直接调用）本就不受影响。
- 全量回归 **295 通过 / 0 失败**。


### v3.12.31 — 修复 BUG-016：void 值赋给变量导致编译器段错误
- **BUG-016 已修复**：`var r = <void调用>();` / `int x = <void调用>();` 此前被 sema
  放行 → codegen 用 Int(i32) alloca 存 void 值 → LLVM `getPrefTypeAlign(void)` 无限递归
  → **编译器段错误**（exit 139）。
- **根因纠错**：原诊断「`main(int argc, string[] argv)` 传参导致类型布局无限递归」不成立
  ——`int main(int argc, string[] argv) { return argc; }` 编译运行正常；真正触发是复现中
  `var r = report(argc, argv);`（report 返回 void）。与 argc/argv 无关。
- **修复**（`src/sema/sema.cpp` visitVarDecl + 自举 `tools/selfhost/src/sema.myp` 镜像）：
  1. 推断路径 `var r = voidCall();` → `cannot infer type of 'var' from a void expression`；
  2. 显式路径 `int x = voidCall();` → `cannot initialize variable 'x' of type 'int' with
     value of type 'void'`。
  两级均用 `diag_.errorCount()` 快照区分「已知 void 调用」（补报）与「未解析表达式
  （已级联报错）」（跳过），避免级联误报。
- **回归**：新增负测试 `tests/negative/var_void_init.myp` + `tests/negative/void_value_init.myp`
  （编译拒绝）；原复现 `tests/bugs/main_argc_argv_crash.myp` 移除。
- 全量回归 **295 通过 / 0 失败**（+2 新负测试）；自举 sema 对拍 **94/94** 全绿。


### v3.12.30 — 修复 BUG-024：相对路径导入去重解析 `..`
- **BUG-024 已修复**：`src/main.cpp` `normalizePath` 此前只移除 `./`/`/./`/`//`，不解析
  `..` —— 同一文件经不同相对路径（直导 `./helper.myp` + 子模块 `../helper.myp`）规范化后
  仍不同 → 双重载入 → `duplicate class name`/`duplicate function name`。
- **修复**：重写 `normalizePath` 为词法组件解析——按 `/` 分段，`.`/空段跳过，`..` 弹栈折叠
  （相对路径保留前导 `..`；绝对路径根 `..` 丢弃），`//` 自然合并；同一文件归一到同一规范键。
- **回归**：复现移入正测试 `tests/@test/relimport_dedup.myp`（+ helpers/b24_helper.myp +
  relimport_sub/sub.myp），直导 + `..` 递归同文件去重、2 断言通过；`tests/bugs/` 原复现移除。
- 全量回归 **293 通过 / 0 失败**。


### v3.12.29 — README.md / README_EN.md 更新对齐当前状态
- **Hello World**：旧的 `int main(){ Console.writeLine(...) }`（现已编译报错）→
  `@startup` + `mypc run` 写法（无需手写 main）。
- **组件与映射**：mapping 节点实例名 → 类名（`Sensor.valueRead -> Display.show`）。
- **标准库**：39 → 40 模块（实测顶层可导入 40 个）。
- **工具链表**：补 `myp_debug`（DAP 调试适配器）、`myp_self`/`myp_self2`（自举编译器，
  含 GPU NVPTX 发射、两级自举成立）、`tools/codegen`（schema 驱动代码生成框架）。
- **测试**：181 → 292 通过 / 0 失败（回归 110 / 负 74 / 测试框架 100 / 自举/LSP 等，
  实测当前汇总）。
- **项目结构**：tools/ 行补 selfhost（自举编译器）与 codegen。
- 中英文两份同步；纯文档，无代码变更。


### v3.12.28 — manual_en.md 内容全面对齐中文版（逐章修正过时/错误表述）
- **§1 Hello World**：旧的 `int main(){ Console.writeLine(...) }`（现已编译报错）→
  三种等价写法（@startup+mypc run / @constructor / @thread）+ main 接线规则注记。
- **§2 字面量**：补二进制/八进制/前导零八进制/下划线/后缀/null 语义；**运算符表**：
  旧 10..0 缺按位/移位/Range/结合性 → 重写为 15..0 + 结合性 + 位运算 + Range。
- **§3 类型系统**：基础表补 bit/bitvector；数字提升改为"仅无损隐式+有损显式"；
  补无符号类型/显式转换/位类型/bitcast/位操作/checkedAdd/parse/Math 多态各节。
- **§4 控制流**：补「枚举与 match」小节（v2.1）。
- **§5 函数**：main 接线 mapping 改类名节点；struct 函数式构造改为"仅位置实参、
  不支持命名实参"；补 nonlocal 按引用捕获 + Man or Boy 测试。
- **§6 Class**：三段式 → 四段式（+ function: 段）；段规则补 struct 行。
- **§8/§9**：mapping 节点统一类名（@thread/@threadpool 实例名/`pool[0]` 改类名）；
  @parallel 限制改"int 或 long 均可" + 补"并行体只捕获局部变量、属性先拷局部"。
- **§10**：补点分模块名（import gpu.hal）+ 相对路径去重按字符串、`..` 未规范化注记。
- **§11 标准库**：修正 4 处过时 API（Math.absInt→Math.abs、__myp_io_*→File 方法、
  stream 迭代器→事件驱动、SDL.init/shouldClose/quit→open/running/close）；补
  import result / setops / gpu（L1+L3）三个模块节。
- **§13**：CLI 表补 --frontend-dump；补「How to Add Tests」节。
- **§14 完整示例**：IoT 示例修正（Timeline 构造器初始化、t.now()、mapping 类名）。
- 纯文档对齐，无代码变更。


### v3.12.27 — manual_en.md 对齐中文版（章节重排 + 新增 codegen/自举/代码生成工具）
- **manual_en.md 全面对齐 manual.md**（英文版此前停留 v3.0 旧结构）：
  - 版本头 3.0 → 3.12；目录加 12. Metaprogramming，Compilation & Tools → §13、
    Complete Example → §14。
  - 元编程从 §13 的 `#### Metaprogramming` 摘出为独立 `## 12. Metaprogramming`，
    并展开为中文版同构的四层总览 + @eval/macro/@macro 详述 + 设计原则。
  - §13 Compilation & Tools 新增：`#### Codegen (LLVM Backend)`（管线/源码分工/
    internalize/-O 管线/myp-pass/--emit-llvm/MYP_FAST_MATH/语义交互）、
    `### Self-Hosted Compiler (myp_self)`（含 GPU 已实现）、
    `### Code Generation Tool (tools/codegen)`（生成器表/schema/--verify）。
  - 环境变量补 MYP_FAST_MATH、MYP_FMT；项目结构 tools/ 加 selfhost/codegen、
    build/ 加 myp_self/myp_self2。
- 纯文档对齐，无代码变更。


### v3.12.26 — 自举编译器 GPU 状态补正：已实现（非"非 GPU"）
- **纠正手册/文档过时表述**：自举编译器 `myp_self` 的 GPU 部分**已实现**
  （v3.12.4–v3.12.5 落地，实测 myp_self2 为 `@gpu for` 生成 NVPTX kernel .ll →
  llc → PTX → GPU/CPU 双路径，真机 launch 验证），但 manual §13 仍写"非 GPU codegen"、
  "GPU 已入自举范围"，roadmap P3-1 写"当前非 GPU"，self_hosting.md 写"不含 GPU"——全部过时。
- **修正**：
  - `docs/manual.md` §13 自举编译器：intro 改「codegen（含 GPU NVPTX 发射）」；
    范围 bullet 改「**GPU：已实现**——@gpu for/tile/scatter/reduce/scan 生成 NVPTX
    kernel（.ll → llc → PTX → 嵌入），GPU/CPU 双路径（MYP_GPU=1 真机 launch，失败
    CPU 回退）；kernel.* 上下文、float4/double2/int4 向量类型均支持」。
  - `tools/selfhost/roadmap.md` P3-1：补 ✅ GPU 已落地（v3.12.4–v3.12.5，60 检查），
    附注 GpuAlgo.sort 自举产物段错误（既有、非 GPU 相关）。
  - `docs/self_hosting.md`：范围改「全自举（含 GPU）」；T5 行改 codegen（含 GPU NVPTX）。
- 纯文档补正，无代码变更。


### v3.12.25 — 修复 BUG-027：tools/codegen 迁移到 BUG-001 属性私有规则，全量回归首次全绿
- **BUG-027 已修复**：`tools/codegen` 代码生成框架（serde/ffi/autodiff/idl/orm/embed/
  dsl/infer_ops）此前未迁移到 BUG-001 属性私有规则（301 个编译错误）。
  - 模型类加 getter（`get<Prop>()`，model.myp 15 类 + gen_autodiff 的 Expr）；统一命名
    使 `x.prop → x.getProp()` 与变量类型无关。
  - Python 脚本迁移跨类读（含 `).prop`/`].prop` 链式形态，跳过字符串/注释），224+6 处。
  - **gen_dsl 生成模板也犯 BUG-001**：生成的 `CalcExpr` 私有属性 + 生成的 `_eval`
    跨类读 → 生成类加 getter + 模板发 getter 调用。
  - **判断：全部加 getter，无 struct 转换**——`Expr` 是递归树（struct 无限大小）；其余
    类都是 `new`+`ArrayList` 堆对象（值语义破坏共享引用）；selfhost AST 先例即 getter。
  - 修复 run_tests.sh 相对 MYPCC 路径解析 + 接入 `tests/run_tests.sh`。
- **§13 补文档**：新增「代码生成工具（tools/codegen）」节（CLI/生成器表/schema 格式/
  --verify/自测），项目结构 tools/ 加 codegen。
- **全量回归 292 通过 / 0 失败（首次全绿）**；`tools/codegen/run_tests.sh` 11 个生成器
  round-trip 全过。


### v3.12.24 — §13 审计发现 tools/codegen 未迁移到 BUG-001 规则（BUG-027）
- **§13 编译与工具完整核对**：确认各工具节（编译器/自举编译器/测试框架/格式化/
  包管理/可视化/LSP/DAP）均有对应二进制；**发现 `tools/codegen` 代码生成框架缺失
  文档**（schema 驱动生成器：serde/ffi/autodiff/idl/orm/embed/dsl/infer_ops）。
- **新 bug BUG-027**：`tools/codegen` **未迁移到 BUG-001 属性私有规则**——模型类
  （`Expr`/`Field`/`TypeDecl`/`ServiceDecl`/`DslOp`/`Resource` 等 15 类）`property:`
  被生成器跨类读取 → `mypc tools/codegen/main.myp` 编译失败（**301 errors**，约 40 组
  类·属性对）；工具（含独立 `run_tests.sh`）整体不可用。根因：BUG-001 修复（08-16）
  后 `tools/codegen`（08-12 停更）未迁移 getter/struct（自举编译器当时已迁）。
- **决策**：按"手册只记录验证可用内容"约定，§13 **暂不**写入不可用工具；问题登记
  BUGLIST（修复后 `tools/codegen/run_tests.sh` 全绿再补文档）。未接入主套件故全量
  回归维持 288 通过。无代码变更。


### v3.12.23 — 手册 §12 元编程展开：四层能力总览 + @eval/macro/@macro 详述
- **§12 元编程章节大幅展开**（从"三层简例"扩为完整小节）：四层能力总览表
  （泛型 monomorphization 类型级 / `@eval` 值级 v3.4 / `macro` 语法级 v3.5 /
  `@macro` 全功能 v3.6）。
- **@eval**：编译期常量示例（FIB10=55/FIB20=6765/HALF=2.5/T5=165/BIGL=1000000，
  实测）、普通函数调用 @eval 常量折叠（`2*fib(10)`=110，实测）、求值时机
  （sema 后 codegen 前）、纯函数约束表（允许/禁止）、诊断错误原文
  （`construct not supported in @eval context`、`recursion depth exceeded`，实测）。
- **macro**：声明式宏语法（`$ident` 元变量 + AST 片段替换）、嵌套/重复展开
  （v=37，实测）、展开时机（parse 后 sema 前 `expandMacros`）、`--macro-expand`
  AST dump 输出形态（实测）。
- **@macro**：`quote {}` 代码模板 + `$` 插值表（标量→字面量/Expr→内联/
  StmtList·Stmt→内联语句）、AST 值类型（Expr/Stmt/StmtList 编译期专属）、
  `StmtList + StmtList` 拼接、makeCalls(3) 生成 3 条 write（实测输出
  x=42 + 0/1/2）、不生成运行时代码、深度/指令上限。
- **注**：`@eval int[8] t = {...}` 表生成（docs/metaprogramming.md §3.1 声称）
  **编译器实际拒绝**（`expected '(' after function name`）——手册不写入未实现
  特性；本地设计文档该行待修正。
- 纯文档展开，无代码变更；全量回归维持 288 通过。


### v3.12.22 — 手册章节重排：元编程从 §12 摘出为独立章节（新 §12），编译与工具 → §13、完整示例 → §14
- **结构**：原 §12「编译与工具」内的 `#### 元编程（@eval/macro/@macro）` 摘出，升级为
  独立 `## 12. 元编程`（置于编译与工具之前）；原 `## 12. 编译与工具` → `## 13.`，
  原 `## 13. 完整示例` → `## 14.`。同步更新目录、章节锚点与交叉引用
  （§1 的「见 §12」→「见 §13」）；元编程章节补「设计与实现详见 design.md §11」。
- 纯文档结构调整，内容无增删（三层元编程 @eval/macro/@macro 示例原样迁移）；
  无代码变更。


### v3.12.21 — 手册 §12 补充：codegen（LLVM 后端）与自举编译器（myp_self）两节
- **新增 §12「codegen（LLVM 后端）」**：编译管线（lexer→parser→sema→codegen→
  opt→目标文件→链接）；`src/codegen/` 源码分工（codegen/class/stmt/expr/gpu/test +
  myp_passes）；非库构建 internalize（仅保留 main 对外）让 LLVM IPO 常量特化+内联；
  `-O0/-O1/-O2/-O3` 优化管线内容（mem2reg/instcombine/GVN/DCE/内联/循环/SROA/向量化）；
  自定义 pass `--passes myp-pass`（`MypRedundantStorePass` 删 codegen 死 store，保守
  规则）；`--emit-llvm` 检查优化与对拍；`MYP_FAST_MATH=1` FP fast-math；优化 pass 与
  异常/协程/arena 的语义交互回归（-O0/-O2 双级别）。
- **新增 §12「自举编译器（myp_self）」**：T5 自举项目（`tools/selfhost/`）用 MYP 完全
  重写编译器本体；stage0 mypc 编 `myp_self`、自编 `myp_self2`；模块清单（token/lexer/
  ast/parser/diag/sema/ir_emit/codegen/link/main）；用法（编译/run/--frontend-dump/
  --emit-llvm/fmt，均已实测：`myp_self2 run` 输出 "self: hello"、`--frontend-dump
  sema` 输出契约头）；codegen 策略（发射 LLVM IR 文本 + llc + gcc）；oracle 对拍
  （frontend-dump 字节对拍 + 运行输出对拍）；两级不动点自举验证（self2→self3→self4
  字节全同 md5 52c81186…）；进度（F0–H1 完成，P2/P3 闭合）。
- **项目结构/环境变量同步**：tools/ 加 `tools/selfhost`、build/ 加 `myp_self,
  myp_self2`；环境变量补 `MYP_FAST_MATH=1`、`MYP_FMT`（myp_self fmt 的格式化器路径）。
- 纯文档补充（无代码变更）；全量回归维持 288 通过（3 个既有环境失败）。


### v3.12.20 — 手册 §12 编译与工具审计：多文件编译 import/struct 合并（BUG-025）+ `--test` 用户 main（BUG-026）
- **新 bug BUG-025（已修复）**：多文件编译 `mypc a.myp b.myp` 合并循环只搬
  classes/interfaces/mappings/functions，**漏了 imports/structs/bitfields/enums/ffis/
  macros/type_aliases**——第二文件的 `import env`/`import test` 静默丢弃
  （`Console`/`Test` 未定义，且错误行号错位到首文件合并区），第二文件的文件级
  struct/enum 变体同样不可见。修复 `src/main.cpp` 多文件分支：合并全部 11 个字段
  （`loadModule` 按模块名/规范化路径去重，跨文件重复 import 只加载一次）。
- **新 bug BUG-026（已修复）**：`mypc --test` + 源码含用户 `int main()` →
  `LLVM verify failed: Basic Block in function 'main' does not have terminator!`；
  且残留空占位使测试运行器 main 被改名为 `main.1` → 测试**静默不跑**（exit 0
  假过）。修复 `src/codegen/codegen_class.cpp`：test 模式跳过用户 main 时
  `func->eraseFromParent()` 擦除占位，运行器 main 保持名字并成为真正入口。
- **§12 工具链逐条实测**：`mypc --help` 全选项存在（-o/-O[0123]/--stdlib/
  --package-path/--trace/--shared/--static/--emit-llvm/--test/-g,--debug/--passes/
  --macro-expand/--frontend-dump/--version/--help）✓ / `mypc run` 自动 main
  （单 @startup 类输出 "Hello from @startup!"）✓ / `mypc run file args` 传参
  （argc=3）✓ / `mypc fmt --check` ✓ / `mypc --emit-llvm` 产出 .ll ✓ / 测试框架
  退出码 1 + 异常隔离 + 汇总 ✓ / 项目结构 ✓ / `MYP_PACKAGE_PATH`（包管理器读取）✓。
- **回归**：新增 `tests/test_multifile.sh`（4 用例：跨文件函数 / 第二文件 import env /
  第二文件 struct+enum+@test / 多文件 @test+用户 main），已接入 `tests/run_tests.sh`
  （`测试框架` 小节）；全量 288 通过（3 个自举工具 build/ 缺二进制的既有环境失败）。


### v3.12.19 — 手册 §10 模块与导入审计：去重 `..` 不规范化（BUG-024）+ 点分模块名补文档
- **新 bug BUG-024**：相对路径导入去重**不解析 `..`**——同一文件经不同相对路径
  （直导 `./helper.myp` + 子模块内 `../helper.myp`）`normalizePath` 后仍不同
  （`/mod/helper.myp` vs `/mod/sub/../helper.myp`）→ 双重载入 →
  `duplicate class name`/`duplicate function name`。design §9 声称「规范化路径去重」
  未真正实现（`normalizePath` 只清 `.`/`//`）。复现 `tests/bugs/relimport_dedup.myp`。
- **文档补齐**：manual §10 导入语法加点分模块名（`import gpu.hal;` → `stdlib/gpu/hal.myp`，
  实测可用但原文档未提）；导入规则加去重注记（按路径字符串、`..` 未规范化）。
- **§10 逐条实测通过**：标准库导入 env/timeline ✓ / 点分模块名 gpu.hal ✓ / 相对路径
  `import "./helper.myp"` ✓ / 绝对路径 ✓ / 同串去重 ✓ / 递归加载 ✓ / 搜索路径
  （--stdlib → ../stdlib/ → ./stdlib/ → --package-path）✓ / 包导入
  （`import foo;` + `--package-path`）✓。
- **回归**：`tests/@test/manual_ch10_myp.myp`（1 test / 2 断言：相对+同串去重+递归+
  点分模块名）+ helpers/ch10_sub.myp；全量 285 通过（3 个自举工具 build/ 缺二进制的
  既有环境失败）。

### v3.12.18 — 手册 §11 标准库抽查：API 全部准确，固化综合回归
- **§11 逐条抽查（编译+运行）**：option（Option()/Option(v)/isSome/get/getOr/set/clear）/
  collections（ArrayList/HashMap/Set）atomic（addInt/subInt/addDouble/xchgInt/loadInt/
  storeInt——确认无 loadDouble，与文档一致）/barrier/future/rtti（typeOf/typeId/
  sameType）/fmt（i/u/x/X/o/b/f/e/g/s/sR 精确输出）/crypto（crc32/md5/sha1/sha256 已知
  向量）/json（getString/getInt/path）/base64/date/regex/args/process/memory（alloc/
  realloc/free/liveObjectCount）——**全部与 manual 文档化签名一致，无文档错误**。
- 先前会话已核对的 §11 部分（env/time/timeline/random/rtti/fmt/crypto/http/net/text/
  atomic/collections/option/result/sync/barrier/future/coro/async/pool/memory/channel/
  fs/process/args/json/regex/base64/date/logger/ui/gpu + math/io/stream/sdl 修正）维持
  不变。
- **回归**：`tests/@test/manual_ch11_myp.myp`（9 tests / 46 断言）；全量 285 通过
  （3 个自举工具 build/ 缺二进制的既有环境失败）。

### v3.12.17 — 手册 §9 并发编程审计：@parallel/@gpu 属性访问 bug + @threadpool 示例修正
- **文档错误修复**：manual §9 `@thread` / `@threadpool` 示例的 mapping 用实例变量名
  节点（`sensor.valueRead -> worker.process`、`sensor.valueRead -> pool[0].process`）
  ——实例名节点函数内 mapping 无法编译（BUG-011），`pool[0].process` 数组下标节点
  parser 直接报错（`expected instance/class name in mapping`）。已统一改为**类名**
  节点（`Sensor.valueRead -> Worker.process`），与 §8 一致。
- **新 bug BUG-023**：`@parallel for` / `@gpu for` 并行体**直接访问 class/static
  属性数组** → LLVM verify 失败（`getelementptr i32, i64 0` GEP 基址为整数 0）/
  `Atomic.addInt` 时运行段错误 139。并行体只捕获外层局部变量；属性访问需先拷到
  局部（manual §9 BNCT 示例 `double[] depthDose = TallyData.depthDose` 已用该模式）。
  manual §9 @parallel for 限制与 @gpu for 限制均加说明。复现
  `tests/bugs/parallel_prop_access.myp`。
- **§9 逐条实测通过**：@thread（独立线程 @startup）/@threadpool（4 worker 启动）/
  @parallel for（int+long 循环变量 + Atomic.addInt，sum=499500；manual 精确示例）/
  sync 同步原语（Mutex/RWLock/CondVar/Semaphore/Once，tests/sync 覆盖）/@gpu for
  CPU 回退（sqrt(4)+sin(1)=2.84147）/import cuda Vectors（add/scale/sum，CPU 回退）/
  构造器/@startup。
- **回归**：`tests/@test/manual_ch9_myp.myp`（3 tests / 11 断言：@parallel for 三种/
  sync API/@gpu+vectors CPU 回退）；全量 283 通过（3 个自举工具 build/ 缺二进制
  的既有环境失败）。

### v3.12.16 — 手册 §8 事件与 Mapping 审计：mapping 节点统一类名
- **文档错误修复**：manual §8（及 §5 main 规则、§13 完整示例）的 mapping 示例
  全部用**实例变量名**节点（`sensor.valueRead -> display.show`）——实测函数内
  mapping 用实例名节点 → LLVM verify 失败（BUG-011，`%tgt = load ptr, ptr
  %display`）。已统一改为**类名**节点（`Sensor.valueRead -> Display.show`），
  与 design.md §7.2 及现有全部测试一致；§8 加注「mapping 节点一律用类名，即使
  声明在函数内（实例级）也如此」。
- **§8 逐条实测通过**（类名节点）：事件声明/类型级映射/实例级映射（函数内类名
  节点）/事件链返回值（`A.event -> B.process -> C.onResult`，result=10）/
  多目标映射（`-> a, b`）/`@scope` 解注册（run 返回后 handler 自动注销，实测
  AFTER 不再触发）/`where` 条件过滤（`where v >= 3`，2 被滤）/`lambda` 变换
  节点/`delay(ms)` 延迟转发/`throttle(ms)` 限频（3 连发只留第一个）。
- **§13 完整示例（iot_monitor.myp）**：类名节点修复后**编译通过**（此前实例名
  节点无法编译）。
- **回归**：`tests/@test/manual_ch8_mapping.myp`（3 tests / 6 断言：类型级/
  事件链/多目标/lambda + where 精确过滤 + 实例级）；升级 `tests/delay_throttle/
  test.myp` 真正用 delay/throttle（原测试未用）+ 更新 expected + 新增
  `throttle_drop.myp`（丢弃语义锁定）；全量 283 通过（仅 3 个自举工具因 build/
  缺二进制 exit 127 的既有环境失败）。

### v3.12.15 — 手册 §7 审计修复：`this.field = value` 写 + 文件级限定 struct 定义
- **BUG-019（C++ codegen）**：`this.field = value`（struct 方法与 class 方法）此前
  编译报 `not a valid assignment target`。根因：`generateAssignment` 的 `if (!op)` 块
  闭合花括号错位，把「struct 方法 this.field」与「class this.prop」两个分支（都需
  **非空** `op` = this 指针）误嵌套进 `if (!op)`（~1988–2271）；`this.x = v` 时 op
  非空 → 整块跳过 → 落到错误。修复：`if (!op)` 在链式/数组元素分支后闭合，struct/
  class 分支移到块外。自举 `myp_self` 本就支持 → C++ 专属。验证：
  `tests/@test/manual_ch7_struct.myp` t_this；`tests/test_smart_building.myp`（大量
  `this.count = s`）转绿；全量回归 281 通过（3 个自举工具因 build/ 缺二进制 exit 127
  的既有环境失败，无关）。
- **BUG-020（C++ parser）**：文件级限定 struct 定义 `struct A::B { }` 此前报
  `expected struct name`（EBNF 与自举 parser 均支持 → C++ 专属）。根因：顶层 struct
  分发 `current_--` 回退到 `struct` 关键字后调 `parseStruct()`，其内部限定检查
  `check(Identifier)` 看到的是关键字而非名称 → 限定分支永不命中。修复：删除回退，
  直接 `parseStruct()`。验证：`tests/@test/manual_ch7_struct.myp` t_nested_qualified。
- **新发现未修复**：BUG-021 class 含泛型类属性（`Option<int>` 等）时 `this.prop`
  sema 解析污染（`class 'X_inst' has no member`，复现 `tests/bugs/this_generic_prop.myp`）；
  BUG-022 `@thread` 用于 struct 实例被静默接受（应拒绝却接受）。均登记
  `tests/BUGLIST.md`。
- **§7 审计回归**：`tests/@test/manual_ch7_struct.myp`（7 tests / 12 断言）覆盖
  文件级 struct/方法/var 推断/this 读+写/返回 struct/兄弟方法/嵌套 struct
  （类内 + `Sensor::Config` 引用 + 文件级限定定义）/struct vs class 值拷贝与引用。

### v3.12.14 — 自举编译器 P3-4/P2 向量化缺口闭合：opt 加 `-mtriple` 启用 TTI
- **症状**：myp_self2 编译产物平均慢 mypc ~14%，最坏数值循环 matmul **2.43x**（SSE2
  `mulpd`/`addpd` vs 纯标量 `mulsd`/`addsd`）。
- **根因（反汇编 + pass-remarks 定位）**：外部 `opt` 无 target machine → 未注册
  TargetIRAnalysis(TTI) → **LoopVectorizer 没有 cost model**，所有循环被判
  “vectorization is not beneficial” 保持标量。mypc 进程内管线给 `PassBuilder` 传
  TargetMachine 故能向量化。`--enable-unsafe-fp-math` 是遗留 flag 不生效；`-O3` 也只
  部分改善（matmul 50→29ms）不向量化。
- **修复**：`link.myp` 的 `opt` 调用加 `-mtriple=<host>`（新增 `findHostTriple()`：
  `llvm-config --host-target` 探测、回退 uname、`MYP_LLVM_CONFIG` 可覆盖）。
- **效果**：matmul **50ms→20ms（2.43x→1.00 持平 mypc）**；多数基准 0.96–1.14
  （dot_f64 0.80、convolution 0.96、nqueens 0.99，部分反超）。
- **正确性**：verify 全一致；run-compare PASS=148 FAIL=0（无输出变化）；
  run_tests 274/275（仅已知 arc_throw -O2 缺陷）；bootstrap 不动点保持。

### v3.12.13 — 自举编译器 P3-2 去委托：`myp_self run`/`fmt` 原生化（不再 shell 到 mypc）
- 此前 `myp_self run`/`fmt` 经 `delegateToMypc` shell 到 C++ mypc。现改为自托管：
  - **`myp_self run <file.myp> [args...]`**（仿 go run）：原生编译+运行+清理，退出码透传。
    无 main 时注入合成 main（单类 @startup）——移植 C++ `Sema::injectAutoMainIfNeeded`
    到自举 sema（新增 `autoMain_` 标志，合成 main 豁免 main() 直接调用限制）；无 @startup /
    多 @startup 报错与 mypc 一致；临时产物（二进制/.o/.ll/.opt.ll）运行后清理。
  - **`myp_self fmt [--check] <file.myp> ...`**：改调自举格式化器 `myp_fmt2`
    （MYP_FMT env 覆盖，缺失时现场用自身编译 tools/fmt/main.myp）。
  - 删除 `delegateToMypc`/`findCompiler`；stdlib 解析统一走 `Cli.selfStdlib()`
    （MYP_STDLIB env → 相对 myp_self 二进制 → cwd 兜底）。
- 验证：test_myp_run.sh 8/8（self2 与 mypc 双跑）、test_myp_self.sh 94/94、
  bootstrap 16/16（不动点保持）、run_tests.sh 274/275（仅已知 arc_throw -O2 缺陷）、
  run-compare PASS=148 FAIL=0 GAP=1(sdl_demo) SKIP=17（无回归）。

### v3.12.10 — 自举编译器接入 LLVM 中端优化（`opt -O2`）
- 自举编译器此前**无任何 IR 优化**：codegen 只发 alloca 形态文本 IR（0 phi，局部
  变量全走栈内存），`llc` 无 `-O`（仅后端 codegen）→ 生成程序性能差。
- **`link.myp` 在 llc 前插入 `opt -O2` 步骤**（mem2reg/SROA/GVN/instcombine/内联）：
  新增 `findOpt()`（opt-21/opt-20 探测，`MYP_OPT` env 覆盖）；**默认开启**，
  `MYP_SELF_OPT=0` 关闭（调试/对比）。实测 opt 后 IR alloca 6→0（全部提成 SSA）。
- **效果对比**（生成程序执行，3 次取最小）：
  - 计算密集循环：46ms → **2ms（23x）**
  - raytracer：1023ms → **326ms（3.1x）**
  - 编译整个自举链：6441 → 7998ms（opt 步骤开销 **+24%**，换取生成的程序 3-23x）
- 正确性：hello/fib/raytracer/showcase/GPU scatter 输出（含 verify 值与 GPU kernel
  launch）与 opt 关**完全一致**；test_myp_self 94/94。
- 权衡：编译 +24% ↔ 运行 3-23x；生产用默认开，快速编译用 `MYP_SELF_OPT=0`。

### v3.12.12 — 自举 codegen：struct 数组元素大小 bug（`new Vec[n]` 分配 8 字节/元素）
- **症状**：soft2 编译 `dotprod`/`slicevec`（`Vec{x,y,z}` struct 数组/切片点积）
  段错误；mypc 正常。
- **根因**：codegen 的 `llvmType(struct)` 返回 `%Vec`，但 `IrEmit.typeSize("%Vec")`
  落**默认分支 8**（ptr 大小）→ `new Vec[n]` 分配 n×8 字节，GEP 却按 `%Vec`（12
  字节/元素，3×i32）索引 → 越界写。类数组（元素是指针）碰巧 8 正确，struct 数组
  （元素内联）错。
- **修复**：新增 `typeByteSize`/`typeAlign`/`structByteSize`/`structAlign`（struct
  按 LLVM 自然对齐递归布局：字段对齐放置、总大小对齐到最大字段对齐），替换 3 处
  `myp_alloc_slice_backing` 的 `IrEmit.typeSize`（`new T[]`、`new slice<T>`、
  `fixedArrayToDynamic`）。
- **验证**：dotprod/slicevec verify 4992059535 与 mypc 一致（ms 8/8 vs C++ 15/16）；
  不动点 stage1==stage2 保持（md5 13a4065b）、test_myp_self 94/94、bootstrap 16/16。
- **附带**：本次用 soft2（stage1 自编译 myp_self）跑全量测试 + C++/Go 基准，见下。

### v3.12.11 — 自举编译提速：M4 原地拼接 + 残留 O(n²) 清扫 + 单趟 `myp_str_join`
- **问题**：自举编译器编译自身很慢（`--frontend-dump sema main.myp` 合并全链约
  96s+）。三个叠加的 O(n²)：
  1. **`StringBuilder.toString` 逐片段 `result = result + x`**：C++ oracle 有 M4
     优化（`s = s + x` 同变量累加发射 `myp_str_append` 原地 realloc），自举
     codegen **没有** → 一律 `myp_strcat` 全量拷贝 → toString O(n²)。
  2. **字符串无长度字段**：`myp_str_append` 每次 `strlen(已累加串)`（O(len)）→
     即使拷贝被省，累加仍 O(n²)。
  3. **per-char `__myp_ord(Str.substring(s,i,i+1))`**：dump/parse/codegen 转义与
     解析路径残留（每个字符 O(n) strlen）。
- **修复**：
  1. **M4 移植到自举 codegen**（对齐 C++ codegen_stmt.cpp M4）：`Assign` 分支识别
     `s = s + x`（左侧同变量字符串局部）→ 发射 `myp_str_append`（rc==1 原地
     realloc，O(1) 均摊）+ `ir_emit.myp` 补 `declare ptr @myp_str_append`。
     **消除 stage0/自举 codegen 行为差异**（性能保真）。
  2. **`myp_str_join(char** arr, int32 n)` 单趟拼接**（runtime.c）：`StringBuilder.
     toString` 改为一次分配 + 两趟（长度求和→拷贝）O(n)，消灭 1.57TB strlen。
  3. **per-char substring → `__myp_charcode`**：15 处（ast Dump.esc、main escape/
     dotToSlash/normalizePath/stripExt、codegen isDigits/escapeLlvmString、
     parser parseInt/插值、sema isIntegerStr、ir_emit parseNum）。
- **实测（编译合并全链 main.myp，3 次取最小）**：
  - sema dump：~96s → **1.7s（56x）**；ast dump：11.9s → **0.47s（25x）**
  - 编译 main.myp：4.4s → **3.1s**；ast dump 内 strlen 扫描 **1.57TB → 54MB（29,000x）**
  - bootstrap 全 3-stage 测试：含 sema dump 阶段从几分钟 → **20s**。
- 修复 `test_myp_pass.sh` 既有 sed 模式 bug（`define internal i32 @compute(...)`
  匹配不上 → 基线误报 0 store）→ PASS 6/6。
- 回归：bootstrap 16/16（不动点 myp_self2==myp_self3 md5 991cec87）、test_myp_self
  94/94、run_tests 275/275、test_myp_gpu 60/60、test_myp_pass 6/6。O2 套件 `arc_throw`
  失败为**既有** -O2×异常展开问题（不经过本次改动的代码路径，见下）。
- 残余（已知）：用户代码里裸 `s = s + x` 累加仍 O(n²)（`myp_str_append` 无长度
  字段每次 strlen）；根治需给字符串头加长度字段（影响 runtime 布局 + 两个 codegen
  字面量，另立任务）。

### v3.12.9 — 自举编译器词法 O(n²) → O(n)（`__myp_charcode` + 缓存长度 + fillLineCol 双指针）
- **根因**：词法器 `peek()/advance()/match()/ordAt()` 对每个字符调用
  `__myp_ord(Str.substring(source_, pos_, pos_+1))`，而运行时 `myp_str_substring`
  每次 `strlen(source_)`（O(n)）→ 每字符 O(n)，整文件 **O(n²)**；`Str.len` 也是
  strlen，守卫里同样 O(n)。
- **修复**：
  1. 新增 **`__myp_charcode(str, i)`** FFI → 运行时 `myp_charcode`（`(unsigned char)s[i]`
     直接下标，O(1) 无 strlen；负数/空返回 0，上界由调用方保证）。C++ oracle
     （sema.cpp/codegen.cpp/CodeGen.h）与自举（sema.myp/ir_emit.myp）同步注册。
  2. 词法器构造时**缓存 `len_`**（一次性 strlen），光标函数改 `__myp_charcode` +
     `len_`，消除逐字符 strlen。
  3. **`fillLineCol` 双指针**：token 按 begin 单调递增 → 行起始指针只前进，
     O(tokens×lines) → O(tokens+lines)。
- **实测（自举编译自身源码，3 次取最小）**：
  - codegen.myp(9995 行) tokens：11086→**672ms（16.5x）**；编译 13304→**2570ms（5.2x）**
  - main.myp（合并全链 ~2.3 万行）编译：19747→**4387ms（4.5x）**
  - tokens 规模扫描由 O(n²)（行数×2 → 时间×4）转为近线性（×2 → ×2.7-3.3）。
- 残余：`--frontend-dump ast/sema` 仍偏慢（dumps 的 `StringBuilder.toString` 是
  O(总输出²)，验证模式）；小文件 myp_self 本就比 mypc 快（启动开销低）。
- 回归：test_myp_self 94/94（字节级对拍保持）、test_myp_gpu 60/60、run_tests 275/275。

### v3.12.8 — 自举编译器 `MYP_LLC` 环境变量覆盖 llc 路径
- `link.myp` `Link.findLlc()` 先读 `MYP_LLC`（非空即用），再回退既有 llc-21/llc-20
  探测 → `llc`（PATH）。跨机器/任意 LLVM 版本无需改源码。
- `codegen.myp` 重复的 `findGpuLlc()` 收敛为委托 `Link.findLlc()`（单一来源），
  GPU/NVPTX 阶段同样支持 `MYP_LLC` 覆盖。
- 验证：默认探测与 `MYP_LLC=/usr/lib/llvm-21/bin/llc` 产物一致；`MYP_LLC` 指向
  不存在路径 → `llc failed`（证明覆盖生效）；GPU scatter 仍真机 launch + PASS；
  test_myp_self 94/94。

### v3.12.7 — 自举编译器清理：删除 5 个空壳占位源文件
- **删除 `tools/selfhost/src/` 下 5 个 F0 时代空壳占位 `.myp`**（各仅 3 行注释
  "当前为空壳（F0 占位）"、不被任何源码 import、无实际代码）：
  `parser_expr.myp` / `type.myp` / `codegen_expr.myp` / `codegen_stmt.myp` /
  `codegen_class.myp`。其职责已分别并入 `parser.myp`（含表达式）/ `sema.myp`
  （含类型表示/提升）/ `codegen.myp`（表达式/语句/类/ARC/异常/泛型/mapping）。
- **同步更新**：`CMakeLists.txt` `MYP_SELF_MODULES` 移除 5 项（原仅作 DEPENDS）；
  `tools/selfhost/README.md` 目录树、`design.md` §4.3/§5 模块表、`roadmap.md`
  总览表与 F3/F4a/G2/G3 小节（标注"实现并入"）。`docs/CHANGELOG.md` 自包含。
- 清理源码树陈旧构建产物（`*.myp.o`、`codegen.out.ll/o`，gitignored）。
- 验证：`myp_self` 重建成功；`test_myp_self.sh` 94/94 全绿（import 链不受影响）。

### v3.12.6 — 自举编译器 `@gpu scatter/reduce/scan` GPU kernel 真机发射（补齐全部 GPU 构造）
- **自举编译器（tools/selfhost）`@gpu scatter` 真正的 GPU kernel**（§8.4）：
  - 固定 **grid-stride 散点 kernel**（无用户 body，直接生成 .ll）：`p ∈ [0, n)`
    grid-stride，`j = idx[loi+p]`（i32→i64 sext），`b[j] = a[loa+p]`；冲突模式
    `atomic_add` 用 **`atomicrmw`**（float `fadd`、i32/i64 `add`，真原子，修掉此前
    load+add+store 的竞态）；`double` 无原生 f64 原子 → 自动 CPU 回退。
  - host 发射双路径（6 参 `(n, loa, loi, a_dev, idx_dev, b_dev)`，grid=ceil(n/block)）；
    标量/数组 arg 全部经 alloca 槽传给 `myp_gpu_launch`；b 的 D2H+free 在 launch 后。
  - 真机验证：6 次 launch，unique/any/atomic_add/slice/int_atomic 全 PASS。
- **`@gpu reduce` 真正的 GPU kernel**（§8.2/§8.6）：
  - **K1 kernel = per-block 串行归约**（`bid=ctaid.x`，块 `[lo+bid*bs, min(..+bs, lo+n))`
    顺序 fold，opExpr 在 kernel 内求值，acc/x 绑定 kernel 局部 alloca）→ `partials[bid]`；
    host 再以 init 顺序折叠 partials（同 opExpr，host 上下文 bindGpuOpLocals）。
  - **CPU 回退改为同构规范分块归约**（§8.6 位级一致）：L1 每 blockSize 顺序部分和 +
    L2/L3 顺序合并——GPU 与 CPU 双跑 **bit 级一致**（`test_gpu_reduce_bit` diff 通过：
    `reduce_bits=1177075684` 等逐字节相等）。
  - 真机验证：sum/slice/max（block(128)）3 次 launch（grid 4/1/8 正确分块）全 PASS。
- **`@gpu scan` 真正的 GPU kernel**（§8.3）：
  - **GPU 两遍**：K1（复用 reduce 同款 per-block 和 kernel，写 blockSum[bid]）→ host
    顺序折叠 `blockPrefix[bid] = init∘blockSum[0..bid-1]` → K2 块内 scan kernel
    （线程 0 串行块内前缀，加 `bp[bid]` 块前缀偏移；inclusive/exclusive 两种写序）。
  - host 折叠需 **ngrid+1 项** bpHost 缓冲（折叠写 bpHost[0..ngrid]，修掉此前越界 1 项
    导致的 glibc `double free or corruption`）。
  - 真机验证：5 个 scan（full/slice/exclusive-full/exclusive-slice/exclusive-init5）
    各 2 次 launch，GPU/CPU 结果完全一致，全 PASS。
- **实现**：`tools/selfhost/src/codegen.myp` 新增 `genGpuScatterKernel`/
  `emitGpuScatterModule`、`genGpuReduceKernel`/`emitGpuReduceModule`、
  `genGpuScanKernel`/`emitGpuScanModule`；`genStmt` 的 GpuScatter/GpuReduce/GpuScan
  分支改为先试 kernel 再回退；`genGpuReduce` 重写为规范分块归约。
- **真机验证汇总**（本机 NVIDIA GPU + CUDA 13.2，`MYP_GPU=1`）：kernel_ctx/stride/vec4/
  tile/tile_degrade/scatter/reduce/reduce_bit/scan 全部真实 launch + PASS（launch 计数
  scatter=6、reduce=3、scan=10）；`test_gpu_block` 的 tile+`Math.abs` 段仍 CPU 回退
  （已知限制：自举 tile 内 Math.abs 解析到宿主静态函数，kernel 无定义）。
- 全量回归 275/275，test_myp_gpu 60/60，test_myp_self 94/94（CPU 模式）。

### v3.12.5 — 自举编译器 `@gpu for` GPU kernel 真机发射（NVPTX 内核 + PTX 嵌入 + 运行时 GPU/CPU 双路径）
- **自举编译器（tools/selfhost）`@gpu for` 真正的 GPU kernel 发射**（此前只有 CPU 回退）：
  - codegen 阶段为规范 `@gpu for`（无 resident/stream/stride、`i < B`/`i <= B`、捕获动态数组）
    生成 **NVPTX kernel**（独立模块，`gid = blockIdx*blockDim+threadIdx`），用外部
    `llc -mtriple=nvptx64-nvidia-cuda` 编译出 PTX，经 `!nvvm.annotations` 标记
    `.visible .entry` kernel，把 PTX 作为字符串全局嵌入主模块。
  - host 发射运行时 **GPU/CPU 双路径**：`myp_gpu_init()` → GPU 可用则
    `myp_gpu_load_kernel` + `myp_gpu_alloc`/`myp_gpu_to_device` + `myp_gpu_launch`
    （grid=ceil(n/block)，block 默认 256 或 `block(n)`）+ `myp_gpu_to_host`/free +
    destroy；否则跳 `gpu_cpu_fallback` 块跑串行回退。数组字节数从 ARC 头
    （obj-24=count、obj-16=elem_size）读取。
  - kernel body 内 `kernel.gid/tx/bx/bd/gx` 复用 CPU 回退的 `genKernelMember` 映射
    （gid=gid、tx=gid%bd、bx=gid/bd、bd=block、gx=ceil(n/bd)）；`kernel.sync()` 空操作。
  - 失败（llc 拒/复杂 body 引用宿主局部）→ 自动 CPU 回退，临时 .ll/.ptx 无论成败清理。
  - 实现：`tools/selfhost/src/codegen.myp` 新增 `genGpuKernel`/`emitGpuKernelModule`/
    `collectGpu{Expr,Stmt}Arrays`/`isDynamicArrayVar`/`findGpuLlc` 等；For 分支抽取
    `genSerialFor`。
  - **真机验证**（本机 NVIDIA GPU + CUDA 13.2，`MYP_GPU=1`）：`kernel_ctx`/`vec4`/
    `math_float`/`static` 均真实 launch kernel 且结果 PASS（`CUDA initialized` →
    `launching kernel` → `kernel done`）；无 `MYP_GPU` 时 CPU 回退结果一致。
  - 范围：`@gpu stride`/`tile`/`reduce`/`scan`/`scatter` 的 GPU kernel 留后续
    （继续 CPU 回退）；`test_gpu_block` 的 tile 段在 GPU 模式因自举 tile 仍为 CPU
    单线程降级（读未写 smem）而 FAIL（测试注释已知，仅 GPU 后端路径）。
- 全量回归 275/275，test_myp_gpu 60/60，test_myp_self 94/94。

### v3.12.4 — 自举编译器 GPU CPU 回退（`@gpu` 语句全量串行对齐 oracle）+ `float4` 向量类型
- **自举编译器（tools/selfhost）GPU CPU 回退落地**——`@gpu for/tile/reduce/scan/scatter`
  不再静默跳过，全部按与 C++ oracle 一致的 CPU 回退语义执行：
  - `@gpu for` / `@gpu stride for`：串行执行 + **模拟 `kernel.*` 上下文**
    （`gid`=循环变量、`bx`=p/block、`tx`=p%block、`bd`=block、`gx`=ceil(bound/bd)）；
    `@gpu stride for` CPU 回退 step 改 +1 顺序遍历（§3.5，同 oracle）。
  - `@gpu reduce`：顺序 fold `out = fold(init, a[lo..hi))`（§8.2）。
  - `@gpu scan`：顺序前缀扫描，inclusive/exclusive + 非零 init（§8.3）。
  - `@gpu scatter`：顺序散点 `b[idx[lo_i+i]] = a[lo_a+i]`，any/unique/atomic_add
    （float fadd / int add 累加）（§8.4）。
  - `@gpu tile`：单线程降级——smem → 宿主栈数组、遍历展平线程网格
    `p ∈ [0, grid*block)`、`kernel.sync()` 空操作（§8.5）。
  - `kernel.sync()` 空操作；`kernel.shfl_down(v,d)`/`kernel.block_reduce_{sum,max}(v)`
    恒等返回 v；`kernel.printk` 空操作；`kernel.assert` 失败 `myp_assert_abort` → exit(1)。
  - 实现：`tools/selfhost/src/codegen.myp` 新增 `genGpuReduce/Scan/Scatter/Tile`、
    `genKernelMember`、`bindGpuOpLocals`、`gpuArrayLoad/Store` 等 + `gpuCpuFallback_` 状态。
- **`float4` 向量类型落地**（§3.6）：`IrEmit.llvmType`/`kindType` 映射
  `float4`→`<4 x float>`、`double2`→`<2 x double>`、`int4`→`<4 x i32>`；`load4`/`store4`
  打包读写（元素偏移 i*4，align 4）；`v.x/y/z/w` 读 `extractelement`、写 `insertelement`；
  `zeroValue`/`alignOf`/`typeSize` 向量分支。
- **回归接入**：新增 `tests/test_myp_gpu.sh`（CPU 回退模式，60 项检查），
  `run_tests.sh` 新增第 10 部分 `RUN_GPU_TESTS=1` 可选启用。全部
  `tests/test_gpu_*.myp` 与 oracle CPU 回退输出对齐（reduce/scan/scatter/tile/kernel_ctx/
  vec4 数值逐项一致）；全量回归 275/275（`RUN_GPU_TESTS=1` 下 276/276）。
- **已知遗留（非本次引入）**：`stdlib/gpu/algo.myp` 的 `GpuAlgo.sort`（嵌套 while +
  `@gpu for`）在自举编译器产物中段错误（旧编译器同样 139，与 GPU 回退无关）；
  已记录待查。

### v3.12.3 — class property 私有化（破坏性语义变更）+ 自举编译器两级自举成立 + Bug 跟踪框架
- **⚠️ 破坏性变更：class `property:` 现为私有**——外部实例访问（读+写）→ 编译错误
  `cannot access property 'X' of 'Y' from outside the class`。此前 sema 允许外部读
  （"Properties — accessible from anywhere"），但 codegen 只正确支持 `this.prop` 与
  单级读，链式 `o.mid.inner.val` 产出垃圾值/段错误、链式写崩溃（BUG-001）。
  - 仍允许：`this.prop`、**同类另一实例**（C++ 私有成员语义，如 `GpuBuffer` 内
    `src.host_`）、`@static class` 的 `Class.prop`。
  - `struct` 字段不受影响（公开可读写）。
  - 修复位置：C++ oracle `sema_expr.cpp` + 自举 `tools/selfhost/src/sema.myp` 双侧同步。
  - 负测试：`tests/negative/external_property_{read,write,chain}.myp`。
- **自举编译器（tools/selfhost）两级自举成立**：
  - F0–F4（前端 oracle/词法/AST/表达式 parser/语义分析）、G1–G4（IR 发射/语句表达式
    codegen/类 ARC 异常泛型/驱动链接）、H1（两级自举验证）全部 ✅。
  - 自举 AST 纯数据 class 迁 getter 访问：跨实例直接读 `e.lhs_` → `e.lhs()`，
    新增 ~360 getter、改写 ~2900 处访问（关键字冲突字段 `ref_→isRef()` 等 8 特例 +
    无下划线字段 `AstPair.k/v`、`AstNonlocalSlot.slot/cell` 改名）。
  - 验证：`test_myp_self.sh` 94/94、`test_myp_bootstrap.sh` 15/15；
    全量回归 270 通过、仅剩 BUG-003 导致的 `generic_traits` 一处不一致。
- **Bug 跟踪框架**：`tests/BUGLIST.md` + `tests/bugs/`（@test 复现 + `run_bugs.sh`）。
  已登记 BUG-002（@coro 增量 spawn）、BUG-003（泛型 string 比较）、BUG-004（`Option<struct>`）。
- **BUG-003 修复：泛型 `T=string` 的 `<`/`>` 按指针比较**——codegen string 比较判定
  新增 `exprResolvedString(e)`（`resolved_kind==String` 优先，泛型类型参数按 alloca 指针
  类型兜底），并让 `exprIsString` 排除动态数组（`T[]` 误判）。`tests/bugs/generic_string_cmp.myp`
  6/6 转绿、`tests/generic_traits` 回归转绿；全量回归 273 通过（`coro_stack` 为既有
  flaky：深递归 3000 层恰在 2048KB 栈边界，非本 bug 引入）。
- **BUG-002 修复：@coro 参数/`this` 悬垂（增量 spawn 帧损坏）**——@coro 方法/函数的
  类引用参数（及 `this`）此前被借用不 retain，协程比调用方作用域长寿 → 主流程释放并
  复用 Channel 对象后，park 中的过滤器读 `in.handle_` 得新对象句柄 → 过滤链错位、
  复合数漏过。codegen 新增 `registerCoroParam`：@coro 入口 retain `this` 与所有 ARC
  参数（class/interface/function/slice/dyn-array/string/含 ARC 字段的 struct），注册为
  作用域槽（正常完成释放）+ 镜像进协程帧注册表（destroy/异常释放）。`tests/bugs/
  coro_incremental_spawn.myp`（go 素数筛）8/8 转绿；全量回归 273 通过。
- **BUG-004 修复：`Option<struct>` 泛型实例化**——struct 字段 `Option<Node>` 此前在
  `generic_classes_` 注册前解析，落到未实例化模板名 `Option` → 赋值/成员访问类型错。
  sema 将 generic 模板注册提前到 struct 字段校验前 + 类声明循环跳过 `is_generic_inst`；
  codegen `memberObjectClassName` 用 `resolved_object_class` + struct 字段类型兜底分发
  到 `Option_Node_inst_*`。`tests/bugs/option_struct.myp` 2/2 转绿；全量回归 274 通过。
  为后续“递归纯数据迁 struct、去掉 class+getter 折中”铺平道路。

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
  - **manual.md §3 文档重写（P4）**：类型系统章同步最新落地——无损隐式/有损显式提升格、bool 入转换链、char=u8、显式转换（含 bool/bit）、新增 `bit`/`bitvector<N>`/`bitfield`/`bitcast`/位操作原语/`checkedAdd`/`checkedMul`/`parse*` 与 `parseIntOpt`/数值 trait 与 `Math` 多态、字面量 `0b`/`0o`/下划线；示例全部经 mypc 验证。
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
- **GPU 原语（gpu_library_design §9 P0 §3.1-3.3 落地）**：
  - **§3.1 `kernel` 执行上下文**：`@gpu for`/`@gpu tile` body 内隐式保留标识符
    `kernel.gid(=p)/bx(blockIdx.x)/tx(threadIdx.x)/bd(blockDim.x)/gx(ceil(n/bd))`（long）
    + `kernel.sync()`（void）；sema 拦截 + codegen NVVM intrinsic 直映；CPU 回退模拟
    （gid=p/tx=p%256/bx=p/256/bd=256）；`tests/test_gpu_kernel_ctx` GPU PASS。
  - **§3.2 `@gpu tile`**：`@gpu tile (T name[dim...]) [grid(nb)] { body }`——块内
    `__shared__`（addrspace 3、编译期常量维度、sema 48KB 上限校验）；协作 body +
    `kernel.sync()` 两阶段；CPU 单线程降级；`tests/test_gpu_tile` GPU PASS。
  - **§3.3 块同步**：`kernel.sync()` → `llvm.nvvm.barrier.cta.sync.aligned.all(i32 0)`
    → PTX `bar.sync 0`；sema 发散分支检查（if/while 内 sync → 警告，防死锁）。
  - **P1 ① 数学 intrinsic 混合方案（§6.3）**：`@gpu for` 内 math 映射改为**混合**——
    native（sqrt/fabs/floor/ceil/trunc）走 `llvm.*` 原生指令（零 libdevice），超越
    （sin/cos/exp/log/tan/pow/atan2 等）保留 `__nv_*`+libdevice（NVPTX 对超越 intrinsic
    无 libcall，纯 intrinsic 会 "no libcall available for flog"）；`tests/test_gpu_math_float`
    GPU 全 PASS。
  - **P1 ② HAL + CPU 一等后端（§7.5/7.6）**：`GpuHAL`（`active()` 探测 cuda→cpu、
    `isGpu/isCpu/vendor`）；`CpuBackend`（available 恒 1、alloc 伪句柄 1、copy no-op、
    流/事件 no-op、sync 恒 1）；`GpuBackend` 按 HAL 分派 cuda/cpu（`backend.myp`）；
    `GpuBuffer/GpuBufferF` 加 `host_` 属性 + CPU 分支（构造 host 直通、copyFromHost/
    copyToHost/copyFromBuffer/async 全逐元素直通）——CPU 成为一等后端；
    `GpuDevice.sync` CPU 模式返回 1；`tests/test_gpu_hal`（手动 @startup 测试，双模式
    PASS：CPU backend=CPU / GPU CUDA）、`tests/@test/gpu_paradigm` 适配双模式 gate
    （流/事件/驻留/kernel-ops 用 `GpuHAL.isGpu()` 判断）。
  - **bug 修复（@gpu for float 标量捕获）**：`analyzeGpuCapturedVars`/`analyzeGpuTileCapturedVars`
    标量类型判断缺 `isFloatTy()` → 捕获 float 标量被置为 i64 → kernel 参数类型错位 →
    GPU 结果垃圾（影响所有捕获 float 标量的 `@gpu for`，如 GpuOps.mapF 的 s 参数）；
    已加 `isFloatTy()` 修复，`gpu_paradigm` GPU 模式 mapBufF 全 PASS。
  - **P1 ④ `@gpu stream(s)`（§4.1，语言级）**：`@gpu for (...) stream(s)` 与
    `@gpu tile (...) stream(s)`——把 kernel 异步排队到 `GpuStream`（launch 不阻塞；
    stream==0 默认流保持同步）。语法（for/tile 子句）+ sema（校验 GpuStream 类型）+
    codegen（launch 点求值 `s.handle()` 传 `myp_gpu_launch(..., stream)`；捕获数组 D2H
    回拷在 stream 模式改用同流异步 `myp_gpu_to_host_async`）+ runtime（`myp_gpu_launch`
    加 stream 参数，stream!=0 去掉自动 `cuCtxSynchronize`）。`GpuEvent.record(s)` +
    `e.wait(s2)` 支持跨流依赖。测试 `tests/test_gpu_stream.myp` GPU PASS（双流并发、
    同流有序、事件跨流依赖）；回归 109/109、gpu_paradigm GPU 57/57（非 stream 同步路径
    不变）。推理框架 `runGpu` 接入（H2D/D2H 重叠）待 P1 ③ 融合后 ⏳。
  - **P2 ① `kernel.shfl_down(v, delta)`（§3.4 warp shuffle）**：`@gpu for/tile` body 内
    块/warp shuffle——sema 拦截（返回 v 类型，支持 double/float/int）+ GPU codegen
    （NVPTX `shfl.sync.down`，LLVM 21 只有 i32/f32 → double 拆 2×i32 重组）+ CPU 回退
    （无 warp 语义返回 v）。
    - **driver 595.84 坑**：`shfl.sync.down` 用 `clamp=-1`（越界返回自身）时**整个
      shfl 不交换**（lane 0 也返回自身）；改用 `clamp=31`（nvcc 同款）保证交换，
      越界 lane（lane+delta>=32）手动用自身 v 替换（`lane >= 32-delta` select）。
    - NVPTX target 从默认 sm_30 改为 **sm_75**（RTX 2070，PTX `.target sm_75`，
      与 `kernel.sync()`/conv3d 兼容，回归全绿）。
    - 测试 `tests/test_gpu_shfl.myp` GPU PASS（double/float/int，delta=16/4/1，
      越界返回自身断言）。调试开关 `MYP_DUMP_PTX=1` 编译时打印 kernel PTX。
  - **P2 ① `kernel.block_reduce_sum/max(v)`（§3.4 块归约）**：块内归约（warp shuffle
    树 → lane0 写 shared[warp] → `bar.sync` → warp0 归约 shared → `bar.sync` →
    broadcast 读 shared[0]）。sema 拦截（返回 v 类型）+ GPU codegen（emitKernelExpr）。
    - **三个关键实现坑**：
      ① shared 必须是**kernel 模块 addrspace(3) GlobalVariable**（真 `.shared`）——
        `alloca(addrspace 3)` 会被 NVPTX 降到 `.local` + `cvta.shared` → error 700
        非法地址；
      ② `CreateGEP` 源类型用**数组类型 `[8 x T]`**（不是元素 T），否则地址算错 →
        PTX verify 失败 → GPU kernel 生成失败 → 静默 CPU fallback（表现为
        "undefined variable 'kernel'"）；
      ③ `smem[warp]`/`smem[0]` 写必须**仅 lane0 条件 store**（同 warp 全 lane 写同一
        slot 竞争 → 结果未定义/0）。
    - 测试 `tests/test_gpu_shfl.myp` 扩展 GPU PASS（block_reduce_sum=256/块、
      block_reduce_max=255/块、float 版）。回归 109/109 + 负测试 58 + 框架 82；
      conv3d vs ORT 7e-7；CPU 回退返回 v。
  - **P2 ② `@gpu stride for`（§3.5 grid-stride）**：`@gpu stride for (long i = 0L;
    i < n; i = i + nTh) { body }`——grid-stride 循环：i = kernel.gid；while (i < n)
    { body; i += nThreads }（nThreads = ntid*nctaid，kernel 内读 gridDim，忽略用户
    step）。语法（parser @gpu stride 分支）+ codegen（kernel 内 PHI 循环头 + body
    回跳 + i 步进；loop_var 映射到循环 PHI）+ CPU 回退（**step 改 +1 顺序遍历全部**，
    因为用户 step 是 GPU 步长，CPU 须遍历所有 i）。普通 `@gpu for` 是 nThreads==n
    的特例。测试 `tests/test_gpu_stride.myp` 双模式 PASS（GPU 覆盖所有 i、CPU 顺序）。
    注：每线程多元素（grid 受限）需配合 P2 ④ `@gpu block(n)` 控制 grid。
    回归 109/109 + 负测试 58 + 框架 82；gpu_paradigm GPU 57/57；conv3d 7e-7。
  - **P2 ④ `@gpu block(n)`（§3.7 块大小/占用率可调）**：`@gpu for (...) block(n)` /
    `@gpu tile (...) block(n)` 用 n 作块大小（默认 256），grid=ceil(n/block)（for /
    stride）或用户 grid（tile）；块大小须为 32 的倍数且 ≤1024（sema 校验，越界报错）。
    - 语法：AST `ForStmt`/`GpuTileStmt` 加 `block_val`；parser 加 `block(n)` 子句
      （tile 顺序：grid → resident → stream → block）。
    - codegen：`generateGpuKernel`/`generateGpuTile` launch 的 `block_i32` 与 grid
      用 block_val；CPU 回退的 `kernel.bd` 模拟（`gpu_cpu_block_` 新成员）随 block_val，
      `gx=ceil(bound/bd)` 同步。
    - 测试 `tests/test_gpu_block.myp` 双模式 PASS：block(128) 每线程写 kernel.bd=
      128 断言；block(512)+stride 每 i 恰一次；tile block(64) 共享归约（GPU 专属）。
      GPU launch 打印 grid/block 正确（64/128、16/512、32/64）。
    - 回归 109/109 + 负测试 58 + 框架 82（自举可视化 1/1，含 myp_viz 重建）。
  - **P2 ⑤ 工具层（§5 计时/错误友好化/静态检查）**：
    - **§5.1 per-kernel 计时**：`runtime_gpu.c` 的 `myp_gpu_launch` 在
      `MYP_PROF_GPU=1` 时用单调时钟量同步 launch（stream==0）耗时，打印
      `kernel done: X.XXX ms`（per-kernel，无需 GpuEvent）。
    - **§5.2 错误友好化**：CUDA 错误码 → 可读字符串表（`gpu_err_str`）；`MYP_GPU=1`
      初始化失败逐点诊断（dlopen/cuInit/无设备/cuCtxCreate）；PTX 加载与 kernel 查找
      失败打印详情；launch/sync 失败映射可读信息；移除过时"PTX kernel parameter
      issues"注释。**codegen 修复**：`load_kernel` 失败改走 CPU 回退（原跳
      `gpu_done_bb` 静默跳过 → 捕获数组未初始化 → 结果错）。
    - **§5.3 静态检查**：tile `block_val` < 共享最大维度 → 警告（防协作覆盖不完/
      越界）；负测试 3 个（block 非 32 倍数 / >1024 / 非 @gpu for 用 block）。
    - 回归 109/109 + 负测试 61 + 框架 82（259/259）；gpu_block 双模式 PASS。
  - **P2 ③ `float4/double2/int4` 向量类型 + `load4/store4` 打包访问（§3.6）**：
    - 语言级向量类型：TypeKind/BuiltinType/Token/lexer/parser/sema/codegen 全链路
      → LLVM `<4 x float>` / `<2 x double>` / `<4 x i32>`（FixedVectorType）。
    - 组件访问 `v.x/y/z/w`：sema 校验 + codegen extract/insertelement（CPU 侧
      generateMemberAccess/generateAssignment + GPU kernel 侧 emitKernelExpr）。
    - 打包原语 `load4(float[] a, long i)` / `store4(a, i, v)`：GEP + `<4 x float>`
      打包读/写（CPU/host 走 emitVec4Access，kernel 走 emitKernelExpr）。
    - **坑**：`ConstantInt::get` 不接受向量类型（未初始化向量清零须
      ConstantAggregateZero）；组件下标 `'w'-'x'` 在 ASCII 为 -1 须显式映射；
      动态数组数据指针不保证 16B 对齐 → 向量访问用 align 4（未对齐，host movups
      安全；NVPTX 对非 16B 对齐拆标量，功能正确）。
    - 测试 `tests/test_gpu_vec4.myp` 双模式 PASS（打包读分量和、打包写 +1000 只改
      每组第 0 分量）；格式化/LSP/viz/tmLanguage 关键字同步。
    - 回归 109/109 + 负测试 61 + 框架 82（259/259）。
  - **P3 ① `@gpu reduce`（§8.2 声明式归约）**：
    - 语法：`@gpu reduce (acc, x) => { return <op>; } init V over a[lo..hi) -> out;`
      （AST `GpuReduceStmt` + parser + sema + codegen）。
    - 语义：`out = fold(init, a[lo..hi))`，op 为 (acc, x) => acc⊕x（须可结合）。
    - sema 校验元素/init/op 返回/out 类型一致（float/double/int）；提取 return 表达式
      到 `stmt.op_expr`；op_body 访问时临时设 `current_return_type_` 为元素类型。
    - codegen：GPU H2D a 范围 → 单 kernel（每块 tx==0 串行归约块内区间 →
      partials[blockIdx]）→ D2H → host 顺序合并 → out；CPU 回退顺序 fold。op 用
      emitKernelExpr（GPU）/ generateExpr（CPU），acc/x 绑定。
    - **坑**：launch args 数组每元素须为"指向参数值的指针"（void** 约定），直接存
      设备指针值导致 cuLaunchKernel 内部 segfault。
    - 测试 `tests/test_gpu_reduce.myp` 双模式 PASS（sum 全量/子区间、max、
      block(128)）。回归 109/109 + 负测试 61 + 框架 82（259/259）。
  - **P3 ② `@gpu scan`（§8.3 声明式前缀和）**：
    - 语法：`@gpu scan (acc, x) => { return <op>; } init V over a[lo..hi) -> b;`
      （AST `GpuScanStmt` + parser + sema 校验 in/out 均 T[] + codegen）。
    - GPU 两遍：K1 块和（`emitBlockSumPtx`，从 emitReducePtx 重构通用化）→ D2H
      partials → host 顺序块前缀 offsets → H2D → K2 块内 scan（acc=offsets[bid]，
      扫块内 acc=op(acc,a[i])、b[i]=acc）→ D2H b；CPU 回退顺序前缀扫描
      （`emitSeqScan`）。
    - 测试 `tests/test_gpu_scan.myp` 双模式 PASS（全量前缀和、子区间前缀）。
      回归 109/109 + 负测试 61 + 框架 82（259/259）。
  - **P3 ③ `@gpu scatter`（§8.4 声明式散点，冲突语义显式）**：
    - 语法：`@gpu scatter [(unique|atomic_add|any)] a[lo..hi) to b by idx[lo..hi);`
      （AST `GpuScatterStmt` + parser + sema 校验 a/b 同 T[]、idx 须 int[]）。
      冲突模式默认 any（实现无关）；unique = idx 无重复（运行时预扫校验，越界/
      重复报错退出）；atomic_add = b[idx]+=a（GPU 原子 / CPU 顺序累加）。
    - GPU：H2D a 范围 + idx 范围 + 整块 b（保留未写槽）→ unique 预扫（host）→
      grid-stride 写/原子 kernel（`emitScatterPtx`，atomicrmw Add/FAdd）→ D2H 整块
      b；CPU 回退顺序写/累加（`emitSeqScatter`）。两区间长度运行时校验相等。
    - 测试 `tests/test_gpu_scatter.myp` 双模式 PASS（unique 全量/逆序、any 冲突、
      atomic_add 浮点/整型、子区间 + 未写槽保持）；负测试 3 个。
      回归 109/109 + 负测试 64 + 框架 82（262/262）。
  - **P3 ④ `@gpu tile`（§8.5 优化降级语义）**：
    - GPU 实现（§3.2 共享内存协作 kernel）已有；本次补齐 §8.5 CPU 降级语义：
      重写 `generateGpuTileCpuFallback` —— smem → host 栈数组；**顺序循环遍历展平
      线程网格 p ∈ [0, grid*block)**（kernel.gid=p、bx=p/bd、tx=p%bd），
      kernel.sync() 空操作；运行时 grid 表达式在降级点 host 求值。
    - 降级对"每线程读写自己/更低槽"的 tile 模式语义不变（协作载入 → 本线程写 →
      读回；thread0 全量载入 + sync；多块 smem 复用无残留污染）。
    - 测试 `tests/test_gpu_tile_degrade.myp` 双模式 PASS（块内前缀和/thread0-load/
      smem-reuse）；`tests/test_gpu_tile.myp` 现 CPU 降级也 PASS（旧回退只写
      out[0] → err=511；新回退覆盖全部输出 → err=0）。回归 262/262。
  - **P3 ⑤ 规范归约顺序 + CPU 回退 + 静态检查（§8.6-8.8）**：
    - §8.6 规范归约顺序（浮点位一致）：reduce CPU 回退改 `emitSeqBlockReduce`
      （L1 每块顺序部分和 + L2/L3 顺序合并，与 GPU 同分块同合并序）→ 位级一致。
      验证 `test_gpu_reduce_bit.myp`：100000 float 归约 block(256)/block(128)，
      `bitcast<int>(s)` 位模式双模式逐字节一致。
    - §8.8 静态检查：三原语（reduce/scan/scatter）空输入 n≤0 运行时守卫
      （reduce → out=init 单位元；scan/scatter → 输出不变；原实现 blocks=0 →
      grid=0 / partials[0] 越界）；tile 48KB 上限负测试 `gpu_tile_shared_too_big.myp`。
    - §8.7 CPU 回退效率：L1 块部分和天然可并行（跨块并行不改变单块计算 → 位一致
      保持），@parallel for 接入留性能类（同 P2⑥/P1③④）。
    - 回归 109/109 + 负测试 65 + 框架 82（263/263）。

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
  - 设计内联见 design.md §6.5；测试 `tests/constructor/` + `tests/copy/`。
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
