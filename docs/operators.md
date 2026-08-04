# MYP 算子系统设计 (Operators)

> 状态：**已实施（v0.3）** —— P1+P2+P3 完成（v2.4.1/v2.4.2），P4 待实施
> 关联：语言规格 v1.0（`docs/grammar.md`）、变更策略（`docs/CHANGELOG.md`）
> 本文档是"运算符 = 算子"统一模型的形式化设计；后续变更受 `docs/CHANGELOG.md` 版本策略约束。

## 状态

> **v0.3（已实施）**：P1+P2+P3 已完成 —— 顶层 `@op` 函数 + struct `operator:` 节 +
> 二元算子重载分发 + `|>` 管道。P4（元素级提升 + 集合二元）待实施。
> 回归测试：`tests/operators/`、`tests/pipe/`（正常 + ASAN + TSan 套件通过）。

---

## 1. 核心洞察：运算符本身也是一种算子

MYP 中"算子"有两个既有含义，本设计将它们统一为一个概念在不同层级的实例：

| 算子 | 签名 | 粒度 | 定义者 |
|---|---|---|---|
| 内建运算符 `+ - * / == <` | `(a, b) → c` | 标量·二元 | 编译器内建 |
| 标量算子 `IOp.forward(x)` | `x → y` | 标量·一元 | 用户（组件） |
| 集合算子 `SetOp.transform(A)` | `A → B` | 集合·一元 | 用户（组件） |
| 数学算子 `Vector3.add(a, b)` | `(a, b) → c` | 值·二元 | 用户（struct） |
| 组合 `op2 ∘ op1` | `A → C` | 集合·复合 | 管道 |

**统一原则**：内建运算符是编译器预置的标量算子；用户通过统一机制定义自己的算子。
**设计结论**：值类型（struct）用数学算子（`+` 重载）；组件（class）用变换算子（action + 接口 + 管道）。

---

## 2. 三层设计总览

```
1) struct 内算子    值类型的数学运算（operator: 节 + @op 绑定符号）
2) 外部 @op 函数    内置类型 / 对称二元 / 跨模块（顶层 @op 函数）
3) 变换组件         class + action + 接口，走 mapping / |> 管道
```

### 2.1 struct 内算子（值类型的数学运算）

```myp
struct Vector3 {
    operator:
        @op("+") Vector3 add(Vector3 other) { ... }   // a + b → 返回新值
        @op("*") Vector3 mul(double s) { ... }         // a * s
    double x_ = 0;
    double y_ = 0;
    double z_ = 0;
}
```

- `operator:` 是 struct 的新类节（仅 struct，class 用 action/event/mapping 机制，不使用数学算子重载）
- 方法即算子，`@op("+")` 注解把方法绑定到符号 `+`（符号为字符串字面量）
- **值语义**：`a + b` 返回新值，不突变操作数（struct 按值传递，天然纯函数）
- 注意：MYP struct 属性为**裸属性、每行一个**（无 `property:` 节头、不支持逗号分隔）

### 2.2 外部 @op 函数（内置类型 / 对称二元 / 跨模块）

```myp
@op("+") Set add(Set A, Set B) { ... }          // 内置 double[] — 只能外部定义
@op("*") Vector3 mul(double s, Vector3 a) {...}  // s * v 对称 — struct 内挂不上
```

- 顶层 `@op` 注解函数，复用 `function` 声明 + 注解机制（零新关键字）
- **显式双操作数** `(a, b)`：对称、可混合、覆盖内置类型

### 2.3 变换组件（class + 接口 + 管道）

```myp
class ScaleOp {
    interface class SetOp;
    action:
        double[] transform(double[] A) { ... }
    property:
        double k_ = 2.0;
}

// 使用
Set B = A |> ScaleOp;        // 组件管道: A → ScaleOp → B
Vector3 w = v + u;           // struct 数学算子
Set C = A + B;               // 外部算子: 内置类型
```

- 复用 `stdlib/setops.myp` 的 `SetOp` 契约（`interface SetOp { double[] transform(double[] A); }`）
- `|>` 是管道运算符（新增 token），左→右数据流

---

## 3. 语义规则

### 3.1 值用算子，组件用事件（核心原则）

| 类型 | 适用机制 |
|---|---|
| struct（值） | 数学算子（`operator:` 节 / 外部 `@op`） |
| class（组件） | action / event / mapping / 管道（不提供 `+` 重载） |

理由：
1. `a + b` 返回新值 = struct 的值语义；class 是引用，重载会引入"突变 vs 返回新对象"歧义
2. 强制"想用 `+` 的类型做成 struct"，恰好是好的类型设计（Matrix/Vector/Color 本就是值）
3. 简化重载解析：内建 → struct 内 → 外部

### 3.2 元数与操作数形态

| 元数 | struct 内算子 | 外部 @op 函数 |
|---|---|---|
| 一元 | `@op("-") T neg() { ... }`（this = 操作数） | `@op("-") double negate(double x)` |
| 二元 | `@op("+") T add(T other)`（this = 左） | `@op("+") T add(T a, T b)`（显式双参） |

- struct 内：`this` 是左操作数，参数是右操作数（OOP 直觉）
- 外部：显式双操作数（对称、可混合标量，解决 `s * v`）

### 3.3 集合提升（标量 → 集合）

```
元素级（可自动 lift）:  add(A, B)[i] = add(A[i], B[i])     // A + B, A * k
结构级（必须显式声明）: matmul(A, B)、dot(A, B)            // 不自动 lift
```

- **元素级**算子可自动提升为集合算子（逐元素 + 标量广播）
- **结构级**（如矩阵乘）必须显式声明为普通函数/方法，**不参与** `+` 重载的自动提升

### 3.4 符号绑定

- 用 `@op("+")` 注解显式绑定（推荐：无歧义、复用注解机制）
- 预留：方法名约定（`add→+`）作为未来可选简化，当前不启用

### 3.5 重载解析顺序（遇到 `a + b`）

```
1. 内建运算符（标量热路径，性能优先，永远最先）
2. struct 内算子（左操作数 a 的类型上定义的 @op("+")）
3. 外部 @op 函数（匹配 (a的类型, b的类型) 签名的 @op("+") 顶层函数）
```

- 类型不匹配或未找到 → 正常报错（与现有一致）
- `double + double` 永远走内建，性能不降

---

## 4. 语法提案（EBNF 增量，全部 additive）

```ebnf
// 新增注解（复用于 struct 方法和顶层函数）
OpAnnot      ::= '@' 'op' '(' OperatorSymbol ')'
OperatorSymbol ::= '+' | '-' | '*' | '/' | '==' | '!=' | '<' | '>' | '<=' | '>='

// struct 新增类节
StructSection ::= 'operator:' OperatorDecl+
OperatorDecl  ::= OpAnnot? ReturnType Identifier '(' ParamList? ')' '{' Stmt* '}'

// 顶层新增（复用 function + 注解，无需新顶层声明形式）
TopLevelDecl  ::= ... | OpAnnot FunctionDecl

// 管道运算符（新增 token）
PipeExpr      ::= LogicalOr ('|>' LogicalOr)*    // 左结合
```

对冻结语法的影响：
- 新 token：`|>`（`PipeForward`）——纯增量
- 新注解：`@op`——纯增量（`@test`/`@startup` 已有先例）
- 新类节：struct 的 `operator:`——纯增量（与 `property:`/`function:` 并列）
- 零关键字、零删除、零语义变更

---

## 5. 完整示例

```myp
// 1) struct 内算子
struct Vector3 {
    operator:
        @op("+") Vector3 add(Vector3 other) {
            Vector3 r;
            r.x_ = x_ + other.x_;
            r.y_ = y_ + other.y_;
            r.z_ = z_ + other.z_;
            return r;
        }
        @op("*") Vector3 mul(double s) {
            Vector3 r;
            r.x_ = x_ * s; r.y_ = y_ * s; r.z_ = z_ * s;
            return r;
        }
    property:
        double x_ = 0, y_ = 0, z_ = 0;
}

// 2) 外部 @op（内置类型）
@op("+") double[] add(double[] A, double[] B) {
    int n = 4;   // 注: 集合长度问题见 §7 待定
    double[] C = new double[n];
    int i = 0; while (i < n) { C[i] = A[i] + B[i]; i = i + 1; }
    return C;
}

// 3) 变换组件
class ScaleOp {
    interface class SetOp;
    action:
        double[] transform(double[] A) {
            int n = 4;
            double[] B = new double[n];
            int i = 0; while (i < n) { B[i] = A[i] * k_; i = i + 1; }
            return B;
        }
    property:
        double k_ = 2.0;
}

// 使用
int main() {
    Vector3 v; v.x_ = 1; v.y_ = 2; v.z_ = 3;
    Vector3 u; u.x_ = 4; u.y_ = 5; u.z_ = 6;
    Vector3 w = v + u;      // struct 数学算子: w = (5,7,9)
    Vector3 s = v * 2.0;    // struct 数学算子: s = (2,4,6)

    double[] A = new double[4]; ...
    double[] C = A + A;      // 外部算子: 内置类型
    Set B = A |> ScaleOp;    // 组件管道（待定 §7）
    return 0;
}
```

---

## 6. 与现有实现的衔接（现状核实）

| 机制 | 现状 | 说明 |
|---|---|---|
| struct 方法 | ✅ 已有 | struct 支持属性和方法 |
| 接口多态 | ✅ 已有 | `interface class IOp`，`examples/ad.myp` 已验证 |
| SetOp 契约 | ✅ 已有 | `stdlib/setops.myp`（本次新增） |
| 顶层 `@op` | ❌ 需实现 | 复用 `function` + 注解机制 |
| struct `operator:` 节 | ❌ 需实现 | parser/sema/codegen |
| `|>` 管道 | ❌ 需实现 | lexer 新 token + 表达式 |

---

## 7. 待定 / 风险项

1. **集合长度**：动态数组 `double[]` 无运行时长度（arena 裸分配），`A.size()` 需改数组表示（高成本、高风险）——**当前算子里硬编码 n，或用 configure(n) 模式**。
   ➡️ 解决方案见 **`docs/slice.md`**（切片类型 `slice<T>`，待评审）
2. **泛型接口**：`interface SetOp<T>`（struct 集合 `Vector3[]` 的算子）——需验证 MYP 泛型是否支持带类型参数的 interface
3. **集合二元提升**：元素级自动 lift 的编译期实现（无函数类型时如何表达）
4. **管道 `|>`**：✅ 已解决（P3，v2.4.2）——`Op` 为算子**类名**（自动实例化）或**实例**（复用），
   调用其 `transform` 方法；左结合链式 `A |> Op1 |> Op2`
5. **class 是否支持 `operator:`**：按 §3.1 原则**不支持**（class 用事件/组件机制），保持设计纯粹

---

## 8. 实施状态

| 阶段 | 内容 | 状态 | 风险 |
|---|---|---|---|
| P1 | 顶层 `@op` 函数（parser 注解 + sema 注册 + 二元分发 + codegen 调用） | ✅ 已完成（v2.4.1） | 中 |
| P2 | struct `operator:` 节（parser + sema + codegen） | ✅ 已完成（v2.4.1） | 中 |
| P3 | `|>` 管道 token + 表达式 + 组件节点 | ✅ 已完成（v2.4.2） | 中 |
| P4 | 元素级提升 + 集合二元 | ⏳ 待实施（地基：`slice<T>`，见 `docs/slice.md`） | 高 |
| 贯穿 | 示例（Vector3 / Set）+ 回归测试 + grammar.md 增量 + CHANGELOG | ✅ 已完成 | — |

每阶段独立可验证：构建（正常 + ASAN）+ 全套测试 + fuzz + no-crash 回归。

---

## 决策记录

| 决策 | 结论 | 理由 |
|---|---|---|
| 运算符 = 算子 | ✅ 统一 | 同一概念不同层级，语法收敛 |
| struct 用数学算子 | ✅ | 值语义天然契合，class 避免引用歧义 |
| class 不用 `+` 重载 | ✅ | class = 组件 = 事件/管道机制 |
| 外部 `@op` 显式双操作数 | ✅ | 对称、可混合、覆盖内置类型 |
| 绑定用 `@op("+")` 注解 | ✅ | 显式、零新关键字、复用注解机制 |
| 集合提升分元素级/结构级 | ✅ | matmul 等结构级算子不自动 lift |
| `|>` 管道 | ✅ | 新增 token 增量，贴合 `A→算子→B` |
