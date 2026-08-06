# MYP 元组与解构设计（Tuples & Destructuring）

> 状态：**TUP-1/TUP-2 已实施（2026-08-05）**——元组类型 + 字面量 + 声明式/赋值式解构 +
> 嵌套解构 + 多值返回 + 字段访问 `t.N`；`tests/tuple` + 3 个负测试；全库回归
> 140/140（-O0 + ASAN）
> 关联：`docs/next_improvements.md` §三-2、`docs/function.md`（函数类型 `(A,B)->R` 复用
> 括号语法，需与之消歧）、`docs/grammar.md`（规格 v1.0 冻结——本设计 **additive**，
> 新增元组类型/字面量/解构语法，不改现有语法）
> 决策背景：多返回值目前只能靠自定义 struct；`match` 仅限枚举。目标：**多值返回 +
> 轻量异构组合**（不新建类）。为 BNCT 多量返回、自举访问者（返回多值）铺路。

---

## 1. 语法设计（additive）

### 1.1 元组类型

```
(int, string)            // 二元元组
(int, string, bool)      // n 元元组
(int, (string, int))     // 嵌套元组
```

- 括号内**顶层逗号**分隔的元素类型列表；**≥2 元素**（含尾逗号 `(int,)` 亦可，可选）。
- **消歧**（关键）：`(A, B) -> R` 已是函数类型。规则：`(` 后看顶层是否有逗号且 `)` 后**无** `->` → 元组；
  有 `->` → 函数类型。单元素 `(int)` 维持**普通括号**（≡ int），不构成元组。

### 1.2 元组字面量

```
(int, string) p = (1, "x");
return (1, "x");          // 返回元组
(2, ("a", 3));            // 嵌套
```

- 与现有圆括号表达式 `(expr)` 消歧：顶层有逗号 → 元组字面量。
- 空元组 `()` 不支持（v1）。

### 1.3 解构

```myp
// 声明式解构（新变量）
(int a, string b) = getPair();     // a=1, b="x"

// 赋值式解构（已有变量）
int x; string y;
(x, y) = getPair();                // 覆盖 x, y

// 嵌套解构
((int p, int q), int z) = getPair2();
```

- 解构目标：标识符（或嵌套元组）；不支持 `_` 忽略符（v1 可选，放 v2）。

### 1.4 元组字段访问（读）

```
t.0   // 第 0 个元素（编译期常量索引）
t.1   // 第 1 个元素
```

- `.N`（数字）作成员名 → 元组元素读取；越界编译期报错。

---

## 2. 表示

### 2.1 AST

- `TypeNode`：新增 `bool is_tuple` + 复用 `func_param_types` 存元素类型（函数类型以
  `func_return_type != nullptr` 区分，元组以 `is_tuple` 区分；二者互斥）。
- 新 `TupleExpr`（`ExprKind::TupleExpr`）：`std::vector<std::unique_ptr<Expr>> elements`。
- 新 `DestructureStmt`（`StmtKind::DestructureStmt`）：目标树（`tuple of {name | nested}`）+
  初始化表达式；声明式目标带类型。

### 2.2 Sema（TypeInfo）

- `TypeKind::Tuple`：`std::vector<TypeInfo> tuple_types`（深度共享指针元素）。
- `typeNodeToTypeInfo`：`is_tuple` → 逐元素转换。
- `typesCompatible`：Tuple 结构等价（元素个数+逐元素兼容，允许整型提升？**否**——严格，
  与函数类型一致；构造字面量时逐元素转换）。
- `typeName`：`(int, string)`。

### 2.3 Codegen

- LLVM 类型：`{ T0, T1, ... }`（无名 struct，按元素类型）。`tuple_structs_` 缓存（按
  元素 LLVM 类型签名）。
- 字面量：逐元素求值 → `CreateInsertValue` 组装。
- 字段读 `t.0`：`CreateExtractValue(idx)`。
- 解构：逐元素 `CreateExtractValue` → 存入各目标 alloca。
- 返回元组：函数返回 struct（按值）；调用处 `CreateExtractValue`。

---

## 3. 里程碑

- **TUP-1**：元组类型 + 字面量 + 声明式解构（多值返回 + `(A a, B b) = f()`）。✔ 已实施
- **TUP-2**：赋值式解构 + 嵌套 + 字段访问 `t.N`。✔ 已实施
- **TUP-3**：文档定稿 + 全库回归（-O0 + ASAN）。✔ 已实施

## 4. 边界（v1 不做）

- 元组作 class 属性（对象头不变，但可放属性类型——其实允许，只是属性 LLVM 类型为 struct）。
  → v1 **允许**属性为元组类型（getLLVMType 统一）。
- `_` 忽略符、可变元组、元组方法、模式匹配（v2）。

---

## 5. 风险与对策

| 风险 | 对策 |
|------|------|
| `(A,B)->R` 与 `(A,B)` 解析歧义 | `scanTupleType` 诊断免费 lookahead：顶层逗号 + `)` 后非 `->` |
| `(a,b)` 表达式与括号消歧 | 表达式 parse 时 `(` 后 lookahead 顶层逗号 → TupleExpr |
| 元组 struct 与函数 fat pointer 混淆 | 元组用无名 struct，函数用 `{ptr,ptr}` 具名——类型来源互斥 |
| 解构到类属性/数组下标 | v1 仅支持标识符与嵌套元组目标；其余报"暂不支持" |
