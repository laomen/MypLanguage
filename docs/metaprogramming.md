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

## 5. P3：过程宏 / 编译期插件（远期）

- 用 MYP 编写宏函数（接收 AST 返回 AST），mypc 在编译期执行。
- 依赖 P1 解释器扩展为"AST 操作环境"（或编译宏为插件二进制，Rust proc-macro 风格）。
- 建议：P3 作为远期路线；P1+P2 已覆盖绝大多数元编程需求。

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
TopLevelDecl     ::= ... | EvalFuncDecl | MacroDecl
EvalFuncDecl     ::= '@eval' ReturnType Identifier '(' ParamList? ')' Block
MacroDecl        ::= 'macro' Identifier '(' MacroParamList? ')' Block
MacroParamList   ::= '$' Identifier (',' '$' Identifier)*

// 表达式
PrimaryExpr      ::= ... | MacroCall
MacroCall        ::= Identifier '(' (Argument (',' Argument)*)? ')'   // 宏名与函数同命名空间冲突需 resolve

// 注解
ActionAnnot      ::= ... | '@' 'eval'        // 若允许 @eval 作用于普通函数
```

> 注意：宏名与函数/方法名**同名冲突**需在解析时区分（宏在 parse 阶段展开，函数在 sema 阶段解析；
> 约定宏名小写/`macro` 前缀或编译期报重复定义）。

---

## 8. 里程碑规划

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M1 | P1 `@eval`：内嵌解释器 + 编译期常量/表 | `const int X = fib(10);` 编译为常量 55；`tests/eval/` |
| M2 | 泛型约束 `where T : Interface` | `DrawList<Circle>` 通过、`DrawList<int>` 编译期报错；`tests/generic_constraint/` |
| M3 | P2 声明式宏 `macro`：AST 展开 pass + `--macro-expand` | `assertEq`/`repeat` 展开正确；`tests/macro/` |
| M4 | P3 过程宏（远期）| 视需求 |

每阶段独立可验证：构建（正常 + ASAN）+ 全套测试 + no-crash 回归。

---

## 9. 参考

- 泛型 monomorphization：`docs/design.md` §泛型
- 语言规格：`docs/grammar.md`（规格 1.0，additive 扩展）
- 版本策略：`docs/CHANGELOG.md`
