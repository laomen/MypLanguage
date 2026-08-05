# MYP 宏 / 元编程 / 编译期求值 设计

> 状态：**设计草案**（v0.1）—— 尚未实施，规划为 v3.x 后续里程碑。
> 设计目标：为 MYP 提供**编译期代码生成与求值**能力，遵循 additive 变更策略（语言规格 1.0 不变更）。

---

## 1. 设计原则

MYP 的元编程设计受以下约束驱动：

1. **additive**：`@eval` / `macro` 作为新注解/关键字引入，不破坏现有语法，语言规格 1.0 保持不变。
2. **编译期确定性**：编译期执行的代码必须是**纯函数**（无副作用、无 I/O、无外部状态、无实例创建），
   保证同样的输入永远得到同样的输出，且不会在编译期产生悬挂引用。
3. **编译期诊断**：求值/展开错误在编译期以清晰的错误报告（维持编译器"永不崩溃"保证）。
4. **可观测**：提供 `--macro-expand` / `--eval-trace` 调试开关，便于理解展开结果。
5. **基于现有泛型**：MYP 已有泛型 monomorphization（`Box<int>` 独立生成代码）——
   这就是最基础的"类型级元编程"。宏与编译期求值建立在其之上，互为补充而非重叠。

---

## 2. 三阶段路线总览

| 阶段 | 能力 | 作用域 | 成本 | 状态 |
|---|---|---|---|---|
| （已有）| 泛型 monomorphization | 类型级 | — | ✅ 已实现 |
| **P1** | `@eval` 编译期求值 | 值级（常量/表生成）| 低 | 🔜 规划 |
| **P2** | 声明式宏 `macro`（AST 片段替换）| 语法级（代码生成）| 中 | 🔜 规划 |
| **P3** | 过程宏（编译器插件）| 全功能 | 高 | 远期 |

```
类型级（已有）      值级（P1）         语法级（P2）          全功能（P3）
泛型 monomorph     @eval 编译期求值   声明式宏              过程宏
Box<int> 独立生成    常量/表生成        AST 片段替换          编译器插件
```

三者分工：
- **泛型**：一份逻辑多种类型（类型参数化）。
- **@eval**：编译期算常量 / 查表 / 配置推导。
- **macro**：消除重复代码模式（模板代码生成）。

---

## 3. P1：`@eval` 编译期求值

### 3.1 语法

```myp
// @eval 函数：编译期执行（纯函数，无副作用）
@eval int fib(int n) {
    return n < 2 ? n : fib(n-1) + fib(n-2);
}

// 编译期常量
const int FIB10 = fib(10);          // 编译期算得 55

// 编译期表生成
@eval int[8] powTable = {1,2,4,8,16,32,64,128};

// 普通函数可在编译期调用 @eval 函数（常量折叠）
int doubleFIB10() { return 2 * fib(10); }
```

### 3.2 语义约束

`@eval` 函数必须是**纯函数子集**：

| 允许 | 禁止 |
|---|---|
| 标量/数组运算、递归、条件/循环、`@eval` 函数互调 | 实例创建（`new`）、I/O、外部状态、`@thread`、映射/事件 |
| 编译期常量参数 | 运行时依赖（参数来自非编译期上下文）|

- 求值时机：**sema 之后、codegen 之前**，结果作为 LLVM 编译期常量。
- 求值结果类型必须与上下文类型匹配（如 `const int FIB10` 必须是 int）。
- 求值失败（栈溢出、类型错误）→ 编译期诊断，不生成代码。

### 3.3 实现路径（mypc + LLVM 21）

- **方案 A（推荐）**：mypc 内嵌**轻量 MYP 解释器**——遍历 `@eval` 函数 AST 直接求值。
  纯函数子集解释执行简单可靠（预计 ~300-400 行），无运行时依赖。
- 方案 B：LLVM OrcJIT 编译 `@eval` 函数并在编译期执行——功能更全（可含复杂逻辑），
  但引入 JIT 复杂度与单文件编译器定位不符，不推荐作为首选。

### 3.4 价值

物理常量表、查表算法、代码生成参数、配置推导、数据驱动生成。

---

## 4. P2：声明式宏 `macro`（AST 模板）

### 4.1 语法（借鉴 Rust `macro_rules`，`$ident` 捕获片段）

```myp
// 代码模板宏：展开为语法树（parse 后、sema 前的 AST 变换）
macro assertEq($a, $b) {
    if (!(($a) == ($b))) { throw "assertEq failed"; }
}

macro repeat($n, $stmt) {
    for (int _i = 0; _i < $n; _i++) { $stmt }
}

class M {
    action:
        @startup void run() {
            assertEq(1 + 1, 2);              // → if (!(1+1 == 2)) throw ...
            repeat(3, Console.writeLine("hi")); // → for 循环 ×3
        }
}
```

### 4.2 语义

- 宏体是**带元变量（`$ident`）的普通 MYP 代码片段**。
- 调用时捕获实参 **AST 片段**，替换元变量，**展开为语法树**。
- 纯编译期 AST 变换，**无需执行**，安全、可调试。
- 展开 pass 位置：**parser 之后、sema 之前**。
- `--macro-expand` 输出展开后的 AST/源码，便于核对。

### 4.3 实现路径

mypc 增加**宏展开 pass**：
1. parse 时收集顶层 `macro` 声明（宏名 + 参数 + 模板体 AST）。
2. 遍历各函数/方法 AST，匹配宏调用节点。
3. 用捕获的实参 AST 片段替换模板中的元变量占位，构造新 AST 节点替换调用点。
4. 迭代展开（宏可调用宏），直到无展开或达到深度上限。

复用现有 AST 节点构造（`AST.h` + parser 的工具函数）。中等成本。

---

## 5. P3：过程宏（M4）—— `@macro` + `quote` 代码模板

### 5.1 定位

M3 声明式宏（`macro`）是**模板**：固定的 AST 片段 + 参数替换。
M4 过程宏（`@macro`）是**可编程的宏函数**：用 MYP 编写，编译期执行，
函数式地构造/拼接 AST——支持循环、条件、算法驱动代码生成，
这是 M3 模板做不到的（M3 不能"根据 n 生成 n 条语句"）。

```
M3 声明式宏（模板）          M4 过程宏（宏函数）
macro repeat($n,$body)       @macro StmtList makeCalls(int n)
  → 固定形态 + 参数替换        → 编译期执行，算法生成 AST
```

与 Rust 对齐：`macro_rules!`（声明式，关键字）↔ M3 `macro`；
`#[proc_macro]`（过程式，属性修饰函数）↔ M4 `@macro`。

### 5.2 语法

```myp
// @macro 修饰函数：编译期宏函数
@macro StmtList genAssign(string name, int value) {
    return quote {
        int $name = $value;
    };
}

@macro StmtList makeCalls(int n) {
    StmtList out = quote {};
    for (int i = 0; i < n; i++) {
        out = out + quote { Console.write($i); };
    }
    return out;
}

// 调用：语句位置的宏函数调用 → 编译期展开
class Main {
    action:
        @startup void run() {
            genAssign("x", 42);
            makeCalls(3);
        }
}
```

### 5.3 代码模板 `quote { ... }`

`quote { <block> }` 是一个**编译期表达式**：把括号内的代码块解析为 AST
（语句集合）。`$ident` 插值：

| `$x` 的编译期值类型 | 嵌入为 |
|---|---|
| `int` / `long` / `double` / `float` / `bool` / `string` | 对应字面量 AST 节点 |
| `Expr`（AST 值）| 内联该表达式 AST |
| `StmtList` / `Stmt`（AST 值）| 内联该语句（组）AST |

`quote` 只允许出现在 `@macro` 函数体内（以及作为普通函数的编译期常量
表达式，V2）。

### 5.4 编译期 AST 值类型与操作

解释器 `EvalValue` 增加 **AST 变体**（V1 三个类型）：

| 编译期类型 | 含义 |
|---|---|
| `Expr` | 单个表达式 AST |
| `Stmt` | 单个语句 AST |
| `StmtList` | 语句列表（块）AST |

这些是**编译期专属类型**：不能作为普通运行时变量/参数类型（sema 校验）。

支持的操作：

| 操作 | 说明 |
|---|---|
| `StmtList + StmtList` | 拼接（`out = out + quote{...}`）|
| `quote { ... }` | 构造 AST |
| `Ast.expr("1+2")` / `Ast.stmt("x=1;")` | 从源码字符串解析（V2）|
| 字段访问（`expr.op` 等）| 遍历/检查 AST（V2）|

### 5.5 执行与展开流程

```
parse → 收集 @macro 函数（注解解析：FuncDecl::has_proc_macro）
      → quote 解析为 QuoteExpr
→ 展开 pass（复用 M3 MacroExpander 框架，扩展两处）：
      (1) 遇到 @macro 函数调用（语句位置）→ 编译期解释执行函数体
      (2) 执行结果（StmtList/Stmt/Expr 值）→ 深度克隆后替换调用点
→ 迭代展开（@macro 返回值可能含 quote/嵌套）直到稳定或达深度上限
→ sema → codegen
```

- 解释器复用 M1 `EvalInterpreter`（`src/eval/eval.cpp`），扩展：
  - `EvalValue` 加 `Stmt/StmtList/Expr` 变体（持有 AST 所有权）。
  - `quote` 求值：深度克隆 QuoteExpr 内的 BlockStmt，处理 `$x` 插值。
  - `+` 对 `StmtList` 重载；返回语句集合。
- `@macro` 函数**不生成运行时代码**（sema 只注册、不 emit）。

### 5.6 V1 范围与限制

| 支持 | 不做（V2） |
|---|---|
| `@macro` 返回 `StmtList`/`Stmt`/`Expr` | `Ast.*` 源码字符串解析 |
| `quote` 块 + `$expr`/`$stmt` 插值 | `quote` 内泛型/类型推导 |
| `@macro` 函数体 = M1 解释器子集（标量/循环/条件/递归）+ quote + AST 拼接 | @macro 内调用 M3 声明式宏 |
| 调用点在语句位置（`genAssign(...)` 作为语句）| 表达式位置的过程宏（V2）|

### 5.7 安全与诊断

- `@macro` 函数编译期执行，沿用 M1 纯函数约束（禁 `new`/I/O/非 `@eval`/`@macro` 调用）。
- 展开深度上限 + 指令数上限（防失控循环生成）。
- 返回类型不匹配（声明 `StmtList` 但 `quote` 产 `Expr`）→ 编译期诊断。
- 插值类型不匹配（`$x` 是 `int` 但需语句）→ 编译期诊断。
- 保持"编译器永不崩溃"：所有展开错误走 DiagnosticEngine。

### 5.8 验收（M4）

- `genAssign("x", 42)` 展开为 `int x = 42;` 且程序运行正确。
- `makeCalls(3)` 生成 3 条 `Console.write(...)` 调用。
- `tests/proc_macro/` + 负测试；`-O0`/`-O2`/ASAN 全套回归通过。

### 5.9 设计决策记录（关键字 vs 注解）

- `macro` **关键字** = 声明式宏（M3）：一种新的**顶层声明类型**，与
  `class`/`struct`/`enum`/`mapping` 平行——MYP 顶层声明一律用关键字。
- `@macro` **注解** = 过程宏（M4）：`@` 在 MYP 中的语义是**修饰已有声明**
  （`@test`/`@coro`/`@eval`/`@startup`），过程宏恰是"修饰一个函数使其编译期可调"。
- 对齐 Rust：`macro_rules!`（关键字）↔ M3；`#[proc_macro]`（属性）↔ M4。
- 代价：`macro` 占用关键字（MYP 已有先例：`where`/`await`/`operator` 均后加关键字，
  回归 113/113 无冲突）。

---

## 6. 泛型约束（前置补充，与宏互补）

在实施 P2 之前，建议先补**泛型约束**，让泛型成为更强的元编程工具：

```myp
// 约束 T 必须实现 Shape 接口
class DrawList<T where T : Shape> {
    action:
        void add(T s) { ... }
}
```

- 语法：`<T where T : Interface>` 或在类型参数表后加 `where` 子句。
- 语义：泛型实例化时检查类型参数满足约束，不满足则编译期报错。
- 与宏的关系：泛型约束是"类型级"的静态保证；宏是"语法级"的代码生成。两者互补。

---

## 7. EBNF 草案（additive 扩展）

```ebnf
// 顶层
TopLevelDecl     ::= ... | EvalFuncDecl | MacroDecl | ProcMacroFuncDecl
EvalFuncDecl     ::= '@eval' ReturnType Identifier '(' ParamList? ')' Block
MacroDecl        ::= 'macro' Identifier '(' MacroParamList? ')' Block
MacroParamList   ::= '$' Identifier (',' '$' Identifier)*
ProcMacroFuncDecl::= '@macro' AstType Identifier '(' ParamList? ')' Block   // M4
AstType          ::= 'Expr' | 'Stmt' | 'StmtList'                           // M4 编译期类型

// 表达式
PrimaryExpr      ::= ... | MacroCall | QuoteExpr
MacroCall        ::= Identifier '(' (Argument (',' Argument)*)? ')'   // 宏名与函数同命名空间冲突需 resolve
QuoteExpr        ::= 'quote' '{' Block '}'                            // M4 编译期 AST 模板

// 注解
ActionAnnot      ::= ... | '@' 'eval' | '@' 'macro'        // @eval 普通函数；@macro 过程宏函数
```

> 注意：宏名与函数/方法名**同名冲突**需在解析时区分（宏在 parse 阶段展开，函数在 sema 阶段解析；
> 约定宏名小写/`macro` 前缀或编译期报重复定义）。

---

## 8. 里程碑规划

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M1** | P1 `@eval`：内嵌解释器 + 编译期常量/表 | ✅ 已完成（`src/eval/eval.cpp` + `include/mylang/Eval.h`：轻量 MYP 解释器求值 `@eval` 纯函数——标量/递归/条件/循环/`@eval` 互调/const 引用；`const int X = fib(10);` 折叠为 `ret i32 55`；`tests/eval/`（FIB10/FIB20/HALF/BIG/T5/BIGL 输出断言）+ `--emit-llvm` 验证常量）|
| **M2** | 泛型约束 `where T : Interface` | ✅ 已完成（`<T where T : Shape>` 语法 + sema monomorphization 时约束检查：`DrawList<Circle>` 通过、`DrawList<int>` 编译期报 "does not satisfy constraint"；`tests/generic_constraint/` + `tests/negative/generic_constraint.myp`）|
| **M3** | P2 声明式宏 `macro`：AST 展开 pass + `--macro-expand` | ✅ 已完成（`src/macro/macro_expand.cpp` + `include/mylang/Macro.h`：`macro` 关键字 + `$param` token（Token/Lexer/Parser）；AST 深拷贝 + 宏体克隆实例化（`$param` → 实参 AST）；表达式/语句/赋值参数 + 嵌套宏（迭代展开、深度上限）；`--macro-expand` AST dump；`tests/macro/`（repeat/addN/twice/log → v=37））|
| **M4** | P3 过程宏 `@macro` + `quote` 代码模板 | ✅ 已完成（`@macro` 注解 → `FuncDecl::has_proc_macro`（sema 跳过 body / codegen 不生成）；`quote { ... }` 上下文关键字（仅 `quote {` 识别，`char quote` 变量不受影响）→ `QuoteExpr`；解释器 `EvalValue` 加 AST 值（`StmtList` 拼接 + quote 求值 + `$x` 插值：数值→字面量、字符串→标识符（变量名/赋值目标）、AST→内联）；展开 pass 集成 `evalProcMacro`（`main.cpp` Phase 3b 对 @macro 也触发）；`tests/proc_macro/`（genAssign 变量名+数值插值、makeCalls 循环+StmtList 拼接））|

每阶段独立可验证：构建（正常 + ASAN）+ 全套测试 + no-crash 回归。

---

## 9. 参考

- 泛型 monomorphization：`docs/design.md` §泛型
- 语言规格：`docs/grammar.md`（规格 1.0，additive 扩展）
- 版本策略：`docs/CHANGELOG.md`
