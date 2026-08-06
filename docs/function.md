# MYP 一等函数与闭包设计（First-Class Functions & Closures）

> 状态：**M-FN-1/M-FN-2/M-FN-3 已实施（2026-08-05）**——函数类型 + lambda 一等函数值
> （fat pointer + tramp）+ **按值闭包捕获**（字符串/class 引用浅拷贝/嵌套）+
> **高阶泛型函数**（`mapOpt`/`foldInt`，泛型参数 T/R + 一等函数实参；`Option.map`）+
> **泛型 `@static` 类方法**（`List.map<T,R>`，跨模块 stdlib 落位）
> 关联：`docs/next_improvements.md` §三-3、`docs/grammar.md`（规格 v1.0 冻结——本设计 **additive**，
> 新增函数类型语法，不改现有语法）、`docs/arc.md`（内存模型：lambda 对象为 class 实例）
> 决策背景：lambda 语法已在（`(x)=>{}`）但**无函数类型**，无法存储/传递/返回；
> 目标：高阶函数（map/filter/reduce）、`Option.map` 落地；为自举 T5（编译器重写，
> 大量访问者/回调）铺路。

---

## 1. 背景与现状

### 1.1 现状（已验证）

- **lambda 语法**：`(int x) => { ... }`（`parseLambdaExpr`，参数经 `parseParam` 须带类型）。
- **实现**：每个 lambda 生成隐藏类 `__lambda_N`（单 `__call` action），对象为 class 实例
  （`TypeKind::Class`）；调用走 `__lambda_N___call(instance, args)`。
- **限制**：lambda 仅用于 mapping 链节点 / pipe；**不能**存变量/传参/返回；**无捕获**
  （不能引用外层局部变量）；参数类型无上下文推断。
- **无函数类型**：`TypeKind::Function` 仅用于顶层函数签名（不可在类型位置书写）。

### 1.2 目标

1. **函数类型** `(int, string) -> bool`（类型位置可用）。
2. **lambda → 一等函数值**：可存变量、传参、返回、调用。
3. **闭包捕获**（按值）：lambda 引用外层局部变量。
4. **stdlib 高阶函数**：`map`/`filter`/`reduce`、`Option.map`。
5. **命名函数作值**（v2）。

---

## 2. 函数类型语法（additive）

```
FunctionType ::= '(' (Type (',' Type)*)? ')' '->' ReturnType
```

- 用现有 `Arrow`（`->`）令牌；与 mapping 链 `->` 在**类型位置**语境区分（类型位置只在
  声明/参数/返回/属性/泛型实参出现，无冲突）。
- `void` 返回允许：`(int) -> void`。
- 无参：`() -> int`。
- 示例：
  ```myp
  (int) -> int    double;      // 变量
  (int, string) -> bool pred;
  ```

**解析位置**：`parseType()` 内、在 `(` 分支之前加 `(` 探测（lookahead `)`...`->`），
仅在"`(` 类型列表 `)` `->`"形态识别为函数类型。

---

## 3. 运行时表示：函数值 = 胖指针（fat pointer）

函数类型 `(A,B)->R` 的 LLVM 表示：

```
┌─────────────┬─────────────────────┐
│ closure:ptr │ call_fn:ptr          │  （16 字节，与 interface fat pointer 同构）
└─────────────┴─────────────────────┘
```

- **closure**：lambda 对象（隐藏类实例，承载捕获状态）。
- **call_fn**：该 lambda 的**统一调用桩** `void* __lambda_N_tramp(void* self, A, B)`，
  内部 `return __lambda_N___call((__lambda_N*)self, A, B)`。

**为何胖指针**：函数值须**多态**——同一函数类型变量可先后持有不同 lambda
（`__lambda_3` → `__lambda_7`），调用点须动态分派；fat pointer 与 interface 派发同构，
无 vtable 需求（每个 lambda 一个 tramp）。

### 3.1 lambda 表达式求值

```
(x) => { ... }  ⇒  new __lambda_N()   （隐藏类实例，含捕获槽）
                  → { closure = obj, call_fn = __lambda_N_tramp }
```

### 3.2 调用函数值

```
fn(args)  ⇒  call_fn(fn.closure, args...)   （经 tramp 间接调用）
```

codegen 在 `generateCall` 中：callee 为函数类型变量 → 取 `{closure, call_fn}` →
`call call_fn(closure, args...)`（返回值类型按函数类型转）。

---

## 4. 闭包捕获（按值，v1）

### 4.1 语义：捕获 = 创建时拷贝

- **按值捕获**：lambda 创建时，把引用的外层局部变量的**值**拷贝进 lambda 对象的捕获槽。
  之后外层变量改变不影响闭包内副本（MYP 无引用类型，按值最安全、无悬垂、线程安全）。
- 与 MYP"引用数据全在 arena、值类型按值"模型一致；与 `@thread`/并行天然兼容
  （闭包捕获副本无共享可变状态）。

### 4.2 实现（sema + codegen）

1. **捕获集分析**（`visitLambda` 内）：遍历 lambda body 的 `IdentifierExpr`，
   解析到**外层局部变量**（非 lambda 参数、非 `this`、非全局/类成员）→ 记入捕获集。
2. **隐藏类加捕获槽**：`__lambda_N` 的 property 段追加 `T cap_0; T cap_1; ...`
   （类型 = 捕获变量类型，保持顺序）。
3. **body 改写**：`__call` action 开头插入
   `T name = this.cap_i;`（按捕获名映射），使原 body 引用 `name` 自然命中局部副本。
   （改写加在共享 body 之前——lambda body 为 lambda 私有，不与其他共享。）
4. **创建时填充**：lambda 表达式求值处（codegen），`new __lambda_N()` 后
   `obj.cap_i = <外层局部当前值>`，再构造 fat pointer。

### 4.3 边界

- **捕获可变对象（class 引用）**：按值捕获的是**引用本身**（浅拷贝）——闭包与外层共享
  同一对象（对象内状态可变）。文档明示：按值捕获 = 标量/值类型深拷贝，class 引用浅拷贝。
- **`this` 捕获**：类方法内 lambda 引用 `this`/实例属性 → 捕获 `this`（v1 支持：把 `this`
  拷入捕获槽，body 用 `this` 访问实例）。
- **嵌套 lambda**：外层捕获作为内层的外层变量，逐层按值传递。

### 4.4 命名 lambda / 递归闭包（additive，M-FN-2）

匿名 lambda 赋给变量即获外部名字；**自引用（递归）** 需要内部名字，新增命名语法：

```
NamedLambda ::= 'fn' Identifier '(' ParamList? ')' '=>' '{' Stmt* '}'
```

```myp
(int) -> int fact = fn fact(int n) => { if (n <= 1) return 1; return n * fact(n - 1); };
```

- **语义**：`name` 在 lambda body 内绑定到**该闭包自身**（函数类型同 `(params)->ret`）。
- **实现（无需捕获外层变量、无需引用类型）**：lambda 本就是隐藏类对象，`__call` 内
  `this` 已指向自身；`name(args)` 编译为**经自身 tramp 的递归调用** `tramp(this, args...)`。
- **与捕获正交**：自引用 `name` 走 `this`，不进捕获集，不参与按值拷贝。
- **互递归**（A 调 B、B 调 A）：v1 不支持（需先行声明/环，v2）。
- **类型检查**：`fn name(...)` 的类型由参数/返回类型决定；body 内 `name` 在符号表
  声明为该函数类型（作用域 = body）。

---

## 5. 类型检查

- lambda 的 `TypeInfo` 由 `TypeKind::Class(__lambda_N)` 改为 `TypeKind::Function`
  （param_types + return_type），使函数类型可比较/可匹配。
- **参数类型解析优先级**：
  1. 上下文已知函数类型（变量声明类型 / 实参形参类型）→ 用其参数类型；
  2. lambda 显式参数类型（`(int x) =>`）→ 用之；
  3. 其余 → 保持现状默认（Int，mapping 链兼容）。
- **函数类型兼容**：结构化——参数个数、各参数类型、返回类型全同则兼容
  （`typesCompatible` 加 Function 分支）。
- **返回类型推断**：`__call` 的 return_type 现为默认 Int；改为：上下文已知 → 用之；
  否则从 body `return` 语句推断（首 return 表达式类型）。

---

## 6. stdlib 高阶函数（§三-3 价值落地）

```myp
// collections.myp 或新 func.myp（additive）
R[] map<T, R>(T[] arr, (T) -> R f) {
    R[] out = new R[arr.len];   // 动态数组
    for (int i = 0; i < arr.len; i = i + 1) out[i] = f(arr[i]);
    return out;
}
T[] filter<T>(T[] arr, (T) -> bool pred) { ... }
R reduce<T, R>(T[] arr, R init, (R, T) -> R f) { ... }
```

- 泛型函数（§三-6 已落地）+ 函数类型 → 直接可用。
- `Option<T>.map((T)->R)` / `filter`：`option.myp` 补方法。
- 命名函数作值（`map(xs, max2)`）→ **v2**（需 thunk 包装）。

---

## 7. 与现有 lambda 用法的兼容

- **mapping 链 / pipe 的 lambda**：保持现有隐藏类对象 + `__lambda_N___call` 路径
  **不变**（非破坏）。新函数类型路径（fat pointer）只用于新增的一等函数用法。
- 若一个 lambda 同时出现在 mapping 链与函数上下文——分别走各自路径，语义独立。

---

## 8. 编译器改动清单

| 模块 | 改动 |
|------|------|
| `parser` | `parseType` 加 `(T,..)->R` 分支；lambda 参数类型可省略（上下文推断）；`fn name(...) =>` 命名 lambda |
| `AST` | `TypeNode` 加函数类型字段（`func_param_types` + `func_return_type`）或复用；`LambdaExpr` 加 `capture_names`/`self_name` |
| `Type.h` | `TypeInfo` 的 Function 完善（param_types/return_type 已有）；`typeName` 支持函数类型打印 |
| `sema` | lambda → `TypeKind::Function`；捕获集分析；隐藏类加捕获槽 + `__call` 前缀拷贝；函数类型兼容；上下文参数类型注入；命名 lambda 自引用解析（body 内 `name` → 经 `this` tramp 递归） |
| `codegen` | 函数类型降低为 fat pointer；lambda 求值 → 隐藏类 + tramp + 捕获填充；调用函数值 → tramp 间接调用；tramp 生成；`fn name` 自引用 → 自身 tramp 递归调用 |
| `stdlib` | `map`/`filter`/`reduce` + `Option.map`/`filter` |
| `tests` | `tests/function/`：存/传/返/调用、捕获（标量/值/class 引用）、高阶函数、`Option.map`、嵌套 lambda、mapping 链兼容回归 |

---

## 9. 调试与诊断

- `--trace`/调试构建：tramp 调用路径可追踪；捕获槽填充可校验。
- 捕获诊断：捕获集分析可打印（`--trace` 下）。
- 编译期检查：lambda 引用未捕获的外层变量（漏捕）→ 报错而非悬垂。

---

## 10. 迁移与兼容性

- **additive**：现有程序零改动；mapping 链/pipe lambda 行为不变。
- 行为差异：无（新语法/新类型，不触碰现有语义）。
- 全回归：-O0 / -O2 / ASAN / TSan / fuzz 全绿为准；myp_viz 对拍保持。

---

## 11. 待评审决策点

- **D-F1**：函数类型语法 `(A,B) -> R`（建议）vs `(A,B) => R`（与 lambda 混用易歧义）。
- **D-F2**：捕获语义**按值**（建议，安全/线程友好）vs 按引用（需引用类型，v2）。
- **D-F3**：fat pointer 表示（建议，多态统一）vs 每调用点静态隐藏类（限制赋值多态）。
- **D-F4**：命名函数作值放 v1 还是 v2（建议 v2，需 thunk）。
- **D-F5**：lambda 参数类型**可省略**（上下文推断）是否放开（建议放开，additive）。
- **D-F6**：命名 lambda `fn name(...) =>`（递归自引用，经 `this` tramp）——放 M-FN-2（建议）还是 v2。

---

## 12. 里程碑建议

- **M-FN-1**：函数类型语法 + TypeInfo + fat pointer 降低 + lambda → 函数值（**非捕获**）
  + `tests/function` 存/传/返/调用。✔ 已实施
- **M-FN-2**：闭包捕获（按值）+ tramp + 捕获填充 + `this` 捕获 + **命名 lambda / 递归**（`fn name`，D-F6）。✔ 已实施
- **M-FN-3**：泛型高阶函数（`mapOpt`/`foldInt`，泛型参数 + 一等函数实参）+ `Option.map`
  + 嵌套 lambda + 全库回归 + 文档定稿。✔ 已实施
  - **stdlib 落位**：✔ 已实施——**泛型 `@static` 类方法**（`List.map<T,R>`）：`ActionDecl.type_params`
    + `resolveGenericStaticCall`（单态化到 `tu.functions`，mangle `__gs_<Class>_<method>_<types>_inst`）
    + parser postfix 泛型调用扩展至 `ClassName.method<T>(...)` + codegen 跳过泛型模板。`tests/generic_static`。

与自举路线关系：T4/T5 大量访问者/回调将直接受益（一等函数是硬前置）；
`Option.map` 落地使 §三-1 完整。
