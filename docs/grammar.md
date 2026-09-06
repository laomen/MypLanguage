# MYP 语言正式语法规范 (EBNF)

> 语言规格版本：**1.0**（语法冻结基准）
> 本文档是 MYP 语法的权威参考，由 `src/lexer/lexer.cpp` 与 `src/parser/parser.cpp`
> 的实际实现整理而成。自本版本起，语法改动视为**破坏性变更**（见 `CHANGELOG.md`）。

---

## 0. 记法约定

```
x*      — 0 次或多次
x+      — 1 次或多次
x?      — 0 次或 1 次
a | b   — 或
( ... ) — 分组
'…'     — 字面量/关键字
标识符  — 词法产生式
// …    — 注释
```

---

## 1. 词法结构

### 1.1 标识符

```
Identifier    ::= [A-Za-z_][A-Za-z0-9_]*
```

### 1.2 字面量

```
IntegerLiteral ::= [0-9]+ | '0x'[0-9a-fA-F]+ | '0b'[01]+ | '0o'[0-7]+  // 十进制 / 十六进制 / 二进制 / 八进制
FloatLiteral   ::= [0-9]+'.'[0-9]* (E[+-]?[0-9]+)?  // 浮点（含科学计数法，仅十进制）
BoolLiteral    ::= 'true' | 'false'
StringLiteral  ::= '"' (字符)* '"'                    // 双引号字符串，支持转义
NullLiteral    ::= 'null'
CharLiteral    ::= "'" 字符 "'"                       // 字符
```

> 整型字面量：十进制 `[0-9]+`、十六进制 `0x…`、二进制 `0b…`、八进制 `0o…`；
> 后缀 `L` 为 long、`u` 为无符号（独立 token `LongLiteral` / `UIntLiteral`）；浮点后缀
> `f`/`F` 为 float32（独立 token `FloatLiteral32`，仅浮点字面量）。
> 下划线可作数字分隔符（`1_000_000`、`0xFF_FF`、`1_000.5`、`1e1_0`），编译期剥离。
> 浮点字面量支持科学计数法 `1e3`、`1.5E-2`。

### 1.3 注释与空白

```
Comment   ::= '//' 到行尾 | '/*' 任意字符 '*''/'
空白       — 空格 / 制表 / 换行 / 回车（分隔 token）
```

### 1.4 关键字

```
class action event property interface import mapping struct function static
if else while for return break continue true false null this new void var
enum match ffi try catch finally throw where await const ref operator macro
nonlocal bitfield
// 注：`type` 为上下文关键字（仅顶层 `type <Id> = <Type> ;` 形态，见 §2.1）
```

### 1.5 类型关键字

```
byte short int long ubyte ushort uint ulong char float double bool string
uint8 uint16 uint32 uint64 int8 int16 int32 int64   // 定宽整型别名（v3.11）
float4 double2 int4                                 // 向量类型
bit bitvector                                       // 位类型（v3.12）
// 注：`slice<T>` 经 ClassType TypeArgList? 解析（见 §2）；`type` 为上下文关键字（见 §2.1）
```

### 1.6 运算符与标点

```
算术     + - * / % ++ --
比较     == != < > <= >=
位运算   << >> & ^ |
逻辑     && || !
赋值     = += -= *= /= %=
管道     |>
标点     ( ) { } [ ] ; : :: ? , . .. -> => @
```

---

## 2. 类型

```
Type          ::= BasicType TypeSuffix? | ClassType TypeArgList? TypeSuffix?
                | FunctionType | TupleType | 'var'
BasicType     ::= 'byte' | 'short' | 'int' | 'long'
                | 'ubyte' | 'ushort' | 'uint' | 'ulong'
                | 'char' | 'float' | 'double' | 'bool' | 'string' | 'void'
ClassType     ::= Identifier
TypeArgList   ::= '<' Type (',' Type)* '>'
TypeSuffix    ::= '[' IntegerLiteral? ']'   // 数组；'[]' 动态，'[N]' 定长
FunctionType  ::= '(' (Type (',' Type)*)? ')' '->' ReturnType
TupleType     ::= '(' Type (',' Type)+ ')'   // 元组；≥2 元素（含尾逗号）
```

> `slice<T>` 为内置切片类型（`{ T* data; int64 len }`，运行时长度，见 [slice.md](slice.md)）；
> 经 `ClassType TypeArgList?` 语法解析（如 `slice<double>`），`new slice<T>(n)` 创建。

> `var` 类型由编译器推断（仅局部变量声明可用）。

> **元组/函数类型消歧**：`(A, B) -> R` 为函数类型（`->` 后必须有返回类型）；
> `(A, B)` 为元组类型（顶层逗号 + `)` 后非 `->`）；`(int)` 仍是普通括号类型。

### 2.1 类型别名（additive，v1.0+）

```
TypeAliasDecl  ::= 'type' Identifier '=' Type ';'      // 顶层声明
```

- **语义**：`type Name = Type;` 为 `Type` 起别名，`Name` 可在后续任何类型位置使用
  （参数/返回/属性/局部变量/泛型实参/数组元素等），完全等价于 `Type`。
- **上下文关键字**：`type` 仅在顶层 `type <Id> = <Type> ;` 这一形态被识别为声明；
  其余位置（属性名/方法名等）仍可作普通标识符（**非破坏性**，不占用保留字）。
- **先声明后使用**：别名须在同一文件内先声明再使用（同 C 的 typedef）；
  支持别名套别名；`type A = A;` 递归别名在语义分析报错。
- **非泛型**：v1 别名不接受类型参数（`type F<T> = ...` 暂不支持）。

```myp
type MyInt = int;
type Int3 = int[3];
type AliasAlias = MyInt;      // 别名套别名
```

### 2.2 `Option<T>` 可空容器 + `T?` 语法糖（additive，v1.0+）

- **stdlib `Option<T>`**（`stdlib/option.myp`，需 `import option;`）：显式可空包装，
  避免裸 `null` 解引用。构造器重载：`Option()`=none、`Option(T v)`=some；
  API：`isSome()`/`isNone()`/`get()`/`getOr(def)`/`set(v)`/`clear()`。
- **语法糖**：`Type?` ≡ `Option<Type>`（仅在**类型位置**——声明/参数/返回/属性；
  `new` 仍用显式 `new Option<T>(...)`）。`int?`=`Option<int>`，`int[]?`=`Option<int[]>`。
- **严格类型**：无隐式装箱——`Option<int> o = new Option<int>(42);`，取值须
  `o.get()`（先 `isSome()`）或 `o.getOr(def)`。

```myp
import option;
Option<int> none = new Option<int>();
Option<int> some = new Option<int>(42);
int? maybe = new Option<int>(7);   // T? 语法糖（类型位置）
int v = maybe.getOr(0);            // 安全取用
```

### 2.3 元组类型（additive，v1.0+）

- **语法**：`(Type, Type, ...)`，≥2 元素（含尾逗号）；元素可为任意类型（含嵌套元组）。
- **字面量**：`(expr, expr, ...)`（顶层逗号）；`(x)` 仍是括号表达式。
- **多值返回**：`(int, string) f() { return (1, "x"); }`。
- **解构**：声明式 `(int a, string b) = f();`、赋值式 `(a, b) = f();`（变量须已声明）、
  嵌套 `((int p, int q), int z) = g();`。
- **字段访问**：`t.0`/`t.1`（编译期常量索引，越界编译报错）。
- **消歧**：函数类型有 `->`；`(a, b) => ...` 是 lambda（FatArrow）；`(int, int) a = ...`
  是元组类型变量声明（`)` 后是变量名）。

```myp
(int, string) getPair() { return (1, "x"); }
(int a, string b) = getPair();      // 声明式解构 → a=1, b="x"
(int, int) t = (3, 4);
int c = t.0;                        // 字段访问 → 3
int x; int y;
(x, y) = getPair() ...;             // 赋值式解构（y 为 string 时报错）
```

> 设计见 [tuple.md](tuple.md)。

### 2.4 函数类型 / 一等函数（additive，v1.0+）

- **语法**：`(A, B) -> R`；参数可为 0 个 `() -> R`；返回可为 `void`。
- **一等值**：函数类型变量存**胖指针** `{closure, call_fn}`；lambda 表达式
  `(params) => { body }` 创建函数值（编译期生成隐藏 class + 统一 tramp）。
- **捕获**：闭包**按值捕获**外层局部（标量/字符串深拷贝、class 引用浅拷贝、嵌套）。
- **调用**：函数值变量直接调用 `f(args)`（经 tramp 间接调用）；也可作参数/返回值传递。
- **高阶泛型**：泛型参数可为函数类型（`(T) -> R`），见 §3.1。

```myp
(int) -> int add1 = (int x) => { return x + 1; };   // 函数类型变量 + lambda
int apply2(int v, (int) -> int f) { return f(v); }  // 高阶函数
(int) -> int makeAdder(int n) { return (int x) => { return x + n; }; }  // 返回闭包（捕获 n）
```

> 设计见 [function.md](function.md)。

---

## 3. 顶层声明

```
Program          ::= TopLevelDecl*
TopLevelDecl     ::= ImportDecl | ClassDecl | StructDecl | InterfaceDecl
                   | MappingDecl | FunctionDecl | EnumDecl | FFIDecl

ImportDecl       ::= 'import' ImportPath ';'
ImportPath       ::= Identifier            // 标准库/包模块
                   | StringLiteral         // 相对/绝对路径文件

ClassDecl        ::= DeriveAnnot? 'class' Identifier GenericParamList? '{' ClassMember* '}'
DeriveAnnot      ::= '@' 'derive' '(' Identifier ')'   // 类级派生注解：@derive(Json) → 自动生成 toJson()/fromJson()（additive，v3.15.1）
GenericParamList ::= '<' Identifier (',' Identifier)* '>'

StructDecl       ::= 'struct' (Identifier '::')? Identifier '{' StructField* '}'
StructField      ::= Type Identifier ('=' Expression)? ';'      // 属性
                   | ReturnType Identifier '(' ParamList? ')' '{' Stmt* '}'  // 方法

InterfaceDecl    ::= 'interface' Identifier '{' InterfaceMember* '}'
InterfaceMember  ::= (返回类型 Identifier '(' ParamList? ')' (';' | Block))  // 动作：签名 或 默认实现（v1.0+，additive）
                   | ('event' Identifier '(' ParamList? ')' ';')

MappingDecl      ::= 'mapping' '(' ')' MappingAnnot? '{' MappingChain+ '}'
MappingAnnot     ::= '@' 'scope'                            // 生命周期跟随作用域
MappingChain     ::= Node (WhereClause)? '->' Target (',' Target)* ';'
WhereClause      ::= 'where' Expression                     // 事件过滤条件
Node             ::= Identifier '.' Identifier              // 源实例事件
Target           ::= Identifier ('.' Identifier)?          // 实例动作 / 类静态动作
                   | Identifier                            // 文件级函数
                   | 'delay' '(' IntegerLiteral ')'        // 延迟变换器
                   | 'throttle' '(' IntegerLiteral ')'     // 节流变换器
                   | LambdaExpr                            // λ 变换器（链中节点）

FunctionDecl     ::= FuncAnnot? ReturnType Identifier GenericParamList? '(' ParamList? ')' '{' Stmt* '}'
FFIDecl          ::= 'ffi' ReturnType Identifier '(' ParamList? ')' ';'
EnumDecl         ::= 'enum' Identifier '{' EnumVariant (',' EnumVariant)* '}'
EnumVariant      ::= Identifier ('(' Type (',' Type)* ')')?
```

### 3.1 泛型函数（additive，v1.0+）

```
GenericFunction ::= ReturnType Identifier GenericParamList? '(' ParamList? ')' ...
```

- **声明**：函数名后可带类型参数 `T foo<T>(T x)`；函数体内 `T` 作类型占位符。
- **调用**：显式类型实参 `foo<int>(5)` 或实参推断 `foo(5)`（T 从参数类型推出；
  支持 `T[]` 参数推元素类型）。推断失败须显式给类型实参。
- **实现**：按类型实参单态化（`foo_int_inst`），与泛型类同构；模板本身不生成运行时代码。
- **范围**：顶层函数 + **泛型 `@static` 类方法**（`List.map<T,R>`，见 §3.2）。
  泛型实例方法暂不支持（见 `next_improvements.md` §三-6）。

```myp
T id<T>(T x) { return x; }
T max2<T>(T a, T b) { if (a > b) return a; return b; }
int a = id<int>(5);    // 显式
int b = id(7);         // 推断 → T=int
```

### 3.2 泛型 `@static` 类方法（additive，v1.0+）

```
GenericStaticMethod ::= Annot? ReturnType Identifier GenericParamList? '(' ParamList? ')' Block?
```

- **声明**：`@static class List { static: Option<R> map<T,R>(...) { ... } }`——
  `static:` 段内方法名后可带类型参数；方法体共享，`T`/`R` 作占位符。
- **调用**：`List.map<int, string>(...)`（显式）或实参推断；单态化实例名为
  `__gs_<Class>_<method>_<types>_inst`。
- **跨模块**：`@static class` 顶层可见 → 泛型静态方法可在 stdlib 定义、任意模块调用
  （`map`/`filter`/`reduce` 落位）。
- **约束**：泛型静态方法无 `this`；模板本身不生成运行时代码（仅实例生成）。

```myp
@static class List {
    static:
        Option<R> map<T, R>(Option<T> o, (T) -> R f) {
            Option<R> r = new Option<R>();
            if (o.isSome()) r.set(f(o.get()));
            return r;
        }
}
// 调用（跨模块）
Option<int> some = new Option<int>(5);
Option<string> m = List.map<int, string>(some, (int x) => { return "v" + x; });
```

### 3.3 接口关联类型（additive，v1.0+）

```
InterfaceAssocType ::= 'type' Identifier ';'          // 接口内：抽象关联类型
ClassAssocBinding  ::= 'type' Identifier '=' Type ';' // 实现类内：绑定具体类型
AssocTypeRef       ::= ClassName '::' AssocName       // 引用绑定：IntBox::Item
                     | TypeParam '::' AssocName       // 泛型内引用：T::Item
```

- **声明**：接口内 `type Item;` 声明一个**关联类型**——由各实现类绑定具体类型
  （Rust 的 associated type 语义）。接口方法签名可引用该关联类型。
- **绑定**：实现类必须用 `type Item = int;` 绑定（负测试 `assoc_unbound`）；绑定
  通过 `X::Item` 语法直接引用（`IntBox::Item ≡ int`，可作局部变量/参数/返回类型）。
- **泛型**：`class Processor<T where T : I>` 内用 `T::Item` 引用关联类型——实例化时
  `T` 绑定具体类，`T::Item` 即该类的绑定类型（实例方法/返回/参数自动单态化）。
- **语义**：关联类型是类型级抽象——允许同一接口被 `int`/`string` 等不同元素类型
  的实现类实例化；约束类型参数注册为接口类型（约束检查在 sema 完成）。

```myp
interface Container {
    type Item;                    // 关联类型声明
    bool contains(Item v);
    Item getVal();
}
class IntBox {
    interface class Container;
    type Item = int;              // 绑定 int
    action:
        bool contains(int v) { return v == val; }
        int getVal() { return val; }
    property: int val = 42;
}
class StrBox {
    interface class Container;
    type Item = string;           // 绑定 string
    action:
        bool contains(string v) { return v == val; }
        string getVal() { return val; }
    property: string val = "hi";
}
// 泛型类：T 约束为 Container，内部用 T::Item（实例化后绑定具体类型）
class Processor<T where T : Container> {
    action:
        T::Item peek(T c) { return c.getVal(); }
}
int pmRun() {
    IntBox::Item x = 5;                       // 直接引用绑定类型 ≡ int
    Processor<IntBox> pi = new Processor<IntBox>();
    int iv = pi.peek(new IntBox());           // T::Item = int → 42
    Processor<StrBox> ps = new Processor<StrBox>();
    string sv = ps.peek(new StrBox());        // T::Item = string → "hi"
    return 0;
}
```

### 3.4 `@derive(Json)` 派生序列化（additive，v3.15.1）

类级注解 `@derive(Json)` 修饰 class → 编译器在 sema 前自动注入两个方法：

```
string toJson()            // 输出该类的 JSON 对象（属性 → 字段，转义正确）
void fromJson(string j)    // 用 json.myp 路径查询回填各属性（round-trip）
```

```myp
@derive(Json)
class Player {
    property:
        string name;
        int hp;
        bool alive;
}
// 自动注入：string toJson() / void fromJson(string j)
// 用法：player.toJson() → {"name":"A","hp":100,"alive":true}；q.fromJson(j) 还原。
```

**v1 规则**：
- 支持属性类型：`int/long/short/byte/uint/ulong/ushort/ubyte`、`double/float`、
  `bool`、`string`（字符串经 `Json.escape` 转义）。数组/类/struct/元组/函数属性 →
  编译期诊断（不支持）。
- 泛型类 `@derive` 暂不支持（编译期诊断）；非 `Json` 派生名 → 诊断。
- 需要 `import json;`（`Json.escape` + `Json` 路径查询）。

---

## 4. 类成员

```
ClassMember      ::= 'action:' ActionDecl+
                   | 'event:' EventDecl+
                   | 'property:' PropertyDecl+
                   | 'function:' FunctionDecl+
                   | 'struct:' StructDecl+
                   | 'static:' ActionDecl+              // 静态方法（无 this）
                   | 'interface' 'class' Identifier ';' // 声明实现某接口
                   | 'type' Identifier '=' Type ';'      // 关联类型绑定（§3.3，additive）
                   | 'const' Type Identifier '=' Expression ';'  // class 顶层 const（等价 property 段 const）

ActionDecl       ::= Annot? ReturnType Identifier GenericParamList? '(' ParamList? ')' Block? ';'?
                   // 类型参数仅对 static: 段内方法有效（泛型静态方法，§3.2）
ActionAnnot      ::= '@' 'startup' | '@' 'constructor' | '@' 'test' | '@' 'coro' ( '(' 'stack' '=' Integer ')' )? | '@' 'region'
FuncAnnot        ::= '@' 'test' | '@' 'region' | '@' 'coro' ( '(' 'stack' '=' Integer ')' )?   // 顶层 @coro 协程函数
EventDecl        ::= Identifier '(' ParamList? ')' ';'
PropertyDecl     ::= 'const'? Type Identifier ('=' Expression)? ';'
                   | Type Identifier '{' PropAccessor* '}' ';'?     // 自动属性（additive）
PropAccessor     ::= 'get' ';' | 'set' ';'                          // 编译器补全平凡读写

ParamList        ::= Param (',' Param)*
Param            ::= Type Identifier ('=' Expression)?    // §四-1 默认参数（函数/action/构造器；事件/枚举无）
ReturnType       ::= Type | 'void'
Block            ::= '{' Stmt* '}'
```

> **自动属性访问器（§5 OOP，additive，selfhost 超前 / seed 冻结不解析）**
> - `property: int x { get; set; }` ——属性槽即存储，编译器自动补全平凡读写：外部
>   `obj.x` 读/写直接路由到该槽（等价公开字段，可带存取在槽上无逻辑）。组合：
>   `{ get; }` = 只读（外部写报 `read-only (no setter)`）；`{ set; }` = 只写（外部读仍
>   按私有拒）。`{ get; set; }` 兼具。类内 `this.x` 走同一槽。
> - 属性仍默认私有（未声明 accessor 者外部不可访，诊断
>   `properties are private; use a getter action`）。
> - 自定义访问器体（`get {…}`/`set(v) {…}`）暂不支持 → 解析期干净拒绝
>   （`custom accessor bodies are not supported yet`）。ARC 字段（class/string）按
>   普通属性赋值/读规则（fresh/借用）。
> - 实现：selfhost parser/sema（`accessorGet/accessorSet` 标志 + 外部读写放行）；
>   seed(oracle) 冻结不解析该语法（selfhost 超前属允许 parity 方向）。

> **默认参数 / 命名实参（§四-1，additive）**
> - `Param ::= Type Identifier ('=' Expression)?`——带默认值的参数可省略（仅函数/action/构造器；
>   事件与枚举数据字段不允许默认值）。
> - 调用点 `CallArguments ::= Expression (',' Expression)*`，其中 `name = value`（解析为赋值表达式，
>   语义阶段按「目标标识符匹配形参名」重解释为命名实参，与宏的赋值实参 `$n/$body` 无歧义）。
> - 位置实参按序填前 N 个形参；命名实参按名填入（可乱序）；缺失且有默认值的形参用默认表达式。
>   负例：未知/重复命名实参、位置+命名重叠、必填缺失、实参过多、默认值类型不匹配均编译期报错。

---

## 5. 语句

```
Stmt             ::= Block
                   | VarDeclStmt
                   | IfStmt | WhileStmt | ForStmt
                   | ReturnStmt | BreakStmt | ContinueStmt
                   | AwaitStmt | MappingStmt
                   | MatchStmt | TryStmt | ThrowStmt
                   | NonlocalStmt
                   | ExprStmt ';'?

Block            ::= '{' Stmt* '}'

VarDeclStmt      ::= Type VarDeclarator (',' VarDeclarator)* ';'
                   | 'var' Identifier ('=' Expression)? ';'
                   | 'const' Type Identifier ('=' Expression)? ';'
VarDeclarator    ::= Identifier ('=' Expression)? VarAnnot?
VarAnnot         ::= '@' 'thread' | '@' 'threadpool'

DestructureStmt  ::= '(' DestructureTarget (',' DestructureTarget)+ ')' '=' Expression ';'
DestructureTarget ::= Type? Identifier                      // 叶子：可选类型（声明式）
                    | '(' DestructureTarget (',' DestructureTarget)* ')'  // 嵌套元组
                 // 声明式解构（叶子带类型）声明新变量；赋值式解构（无类型）写入已有变量

IfStmt           ::= 'if' '(' Expression ')' Stmt ('else' Stmt)?
WhileStmt        ::= 'while' '(' Expression ')' Stmt

ForStmt          ::= 'for' '(' (VarDeclStmt | ';') Expression? ';' Expression? ')' Stmt
                   | ForInStmt
                   | '@parallel' 'for' '(' … ')' Stmt            // 数据并行
                   | '@gpu' 'for' '(' … ')' Stmt                 // GPU 卸载
ForInStmt        ::= 'for' '(' Type? Identifier 'in' Expression ')' Stmt   // 集合迭代（§四-2）
                   | 'for' Identifier 'in' Expression Stmt                 // 无括号集合迭代
                   | 'for' Identifier 'in' RangeExpr Stmt                  // 区间 for（同下）
                 // 迭代源：固定数组 T[N]（编译期长度）、slice<T>（.size()）、
                 //   集合类（需 size()+get(int) 方法，如 ArrayList<T>）、range a..b（右开 i<b）
RangeExpr        ::= Expression '..' Expression

ReturnStmt       ::= 'return' Expression? ';'
BreakStmt        ::= 'break' ';'
ContinueStmt     ::= 'continue' ';'
AwaitStmt        ::= 'await' ';'                                   // 简单挂起（协程内）
                   | 'await' Expression ';'                        // 带值挂起（C2）
                   | 'await' ClassName '.' EventName ';'           // 等待事件（C4）
                   | 'await' ClassName '.' EventName 'timeout' Integer ';'  // 事件等待带超时（C10，毫秒）
                   // 仅允许在 '@coro' 注解的类 action 方法或顶层 @coro 函数内
AwaitExpr        ::= 'await' Expression                            // 表达式（C2）
                   | 'await' ClassName '.' EventName               // 表达式：事件等待（C4）
                   | 'await' ClassName '.' EventName 'timeout' Integer  // 表达式：事件等待带超时（C10）
                   // 挂起传出 Expression 值；恢复后表达式 = resume 传入值
                   // 仅允许在 '@coro' 注解的类 action 方法或顶层 @coro 函数内
MappingStmt      ::= 'mapping' '(' ')' MappingAnnot? '{' MappingChain+ '}'  // 局部 mapping

MatchStmt        ::= 'match' '(' Expression ')' '{' MatchArm+ '}'
MatchArm         ::= MatchPattern BindingList? '=>' '{' Stmt* '}'
MatchPattern     ::= EnumName '.' VariantName                     // 枚举变体（可携带数据绑定）
                   | '-'? IntegerLiteral                          // 整型字面量（含 long/uint/char 码）
                   | FloatLiteral                                 // 浮点字面量
                   | StringLiteral                                // 字符串字面量
                   | '_'                                          // 通配默认臂（= default，可选）
BindingList      ::= '(' Identifier (',' Identifier)* ')'
// 枚举变体匹配；变体可选携带数据绑定 (v1, v2, …)，臂体为块。字面量/通配臂
// 无绑定。字面量臂类型须与 subject 匹配（整型↔整型、string↔string、float↔
// float/double）；枚举臂与字面量臂不可混用；通配 _ 作默认兜底（可不穷尽）。
// 无 fallthrough（隐式 break）。

TryStmt          ::= 'try' Block CatchClause+ FinallyClause?
CatchClause      ::= 'catch' '(' (Type Identifier | Identifier)? ')' Block
                 // 有类型: 按类型匹配（'string' 或异常类名）; 无类型: 兜底（捕获一切，变量为 string 消息）
FinallyClause    ::= 'finally' Block
ThrowStmt        ::= 'throw' Expression? ';'   // 无表达式 = 在 catch 内重抛当前异常
NonlocalStmt     ::= 'nonlocal' Identifier (',' Identifier)* ';'
                 // 仅允许在 lambda body 内：按引用（共享可变）捕获外层函数/action
                 // 的参数或局部变量（v1：仅标量类型；嵌套 lambda / struct 方法内不支持）
TryExpr          ::= 'try' Expression 'catch' '(' Identifier ')' Expression
                 // 表达式式: 成功→try 值, 失败→catch 值（类型须兼容）

ExprStmt         ::= Expression ';'?
```

---

## 6. 表达式（优先级从低到高）

```
Expression       ::= Assignment

Assignment       ::= Conditional
                   | UnaryExpr AssignOp Assignment
AssignOp         ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='

Conditional      ::= LogicalOr ('?' Expression ':' Expression)?
LogicalOr        ::= LogicalAnd ('||' LogicalAnd)*
LogicalAnd       ::= BitwiseOr ('&&' BitwiseOr)*
BitwiseOr        ::= BitwiseXor ('|' BitwiseXor)*
BitwiseXor       ::= BitwiseAnd ('^' BitwiseAnd)*
BitwiseAnd       ::= Equality ('&' Equality)*
Equality         ::= Relational (('==' | '!=') Relational)*
Relational       ::= Shift (('<' | '>' | '<=' | '>=') Shift)*
Shift            ::= Additive (('<<' | '>>') Additive)*
Additive         ::= Range (('+' | '-') Range)*
Range            ::= Multiplicative ('..' Multiplicative)?
Multiplicative   ::= Unary (('*' | '/' | '%') Unary)*
Unary            ::= ('!' | '-' | '++' | '--')* Postfix

Postfix          ::= Primary PostfixOp*
PostfixOp        ::= '(' ArgumentList? ')'        // 调用
                   | '[' Expression ']'           // 下标
                   | '.' Identifier               // 成员访问
                   | '.' IntegerLiteral           // 元组字段访问 t.0（编译期索引）
                   | '++' | '--'                  // 后缀增减

Primary          ::= IntegerLiteral | FloatLiteral | BoolLiteral
                   | StringLiteral | CharLiteral | NullLiteral
                   | 'this'
                   | Identifier                    // 变量 / 类名 / 枚举变体
                   | '(' Expression ')'
                   | TupleLiteral                  // (a, b, …)：顶层逗号
                   | ArrayLiteral                  // [e1, e2, …] → 动态数组 T[]
                   | MapLiteral                     // {"k": v, …} → StrHashMap<V>
                   | StructLiteral                 // Pt{x: 1, y: 2} → struct 值
                   | 'new' ClassType TypeArgList? '(' ArgumentList? ')'
                   | 'new' Type '[' Expression ']' ( '[' Expression ']' )*
                   | LambdaExpression

TupleLiteral     ::= '(' Expression (',' Expression)+ ')'   // ≥2 元素（含尾逗号）
ArrayLiteral     ::= '[' Expression (',' Expression)* ']'   // 动态数组字面量（additive，v1 需显式 T[] 目标）
MapLiteral       ::= '{' MapEntry (',' MapEntry)* '}'       // 字典字面量（additive v1：键 string、值可转 V，显式 StrHashMap<V> 目标）
MapEntry         ::= Expression ':' Expression
StructLiteral    ::= Identifier '{' StructFieldInit (',' StructFieldInit)* '}'   // 结构体/对象初始化器（additive v1：文件级 struct + 纯标量字段；值式，字段可缺省 → 零初始化，乱序按名）
StructFieldInit  ::= Identifier ':' Expression
LambdaExpression ::= '(' ParamList? ')' '=>' '{' Stmt* '}'   // FatArrow '=>'
                 // body 内可用 NonlocalStmt 按引用捕获外层函数变量（共享可变）
ArgumentList     ::= Expression (',' Expression)*
```

---

## 7. 附注

- **分号**：语句末尾分号在部分上下文可省略（解析器在块/`}` 前自动补）。
- **运算符优先级**：与 C 家族一致；`..`（range）位于加减与乘除之间。
- **数组**：`int[]` 动态数组（`new int[n]`），`int[10]` 定长数组。
- **泛型**：类声明用 `<T, U>`，实例化用 `<int, string>`。
- **`@gpu for`** 数学函数映射到 CUDA libdevice；`@parallel for` 用线程池。
- **`var`** 仅用于局部变量类型推断，不能用于参数/属性。

> **算子系统（已实施，additive）**：`运算符 = 算子` 统一模型。
> - struct `operator:` 节 + `@op("+")` 注解（结构内数学算子）
> - 顶层 `@op("+")` 函数（外部算子：内置类型/对称二元/跨模块）
> - 管道 `|>`（`A |> Op`，算子组件流水线；`Op` 为类名或实例，调用其 `transform`）
> - 新增关键字 `operator`、token `|>`；`@op` 注解参数为运算符字符串
> - 完整规范见 [operators.md](operators.md)

> **切片与内存 region（已实施，additive）**：
> - `slice<T>`：`{ T* data; int64 len }` fat pointer；`new slice<T>(n)` 创建；
>   `s[i]`（带边界检查）、`s.size()`/`s.length`、`s.data()`；值传递
> - `@region` 注解：函数调用作用域为内存 region（RMM）；入口 mark、出口 release，
>   内部 slice/数组临时对象自动回收；返回引用类型的 `@region` 自动禁用（逃逸安全）
> - 完整规范见 [slice.md](slice.md)

> **异常机制（已实施，additive）**：
> - `try { } catch (e) { }`：多 catch 按类型分发（`catch (string e)` / `catch (FileError e)` /
>   兜底 `catch (e)`）；内层不匹配自动 rethrow 到外层；`finally` 总是执行
> - `throw expr;`：字符串快捷或异常对象（实现 `interface Error` 的 class）
> - 表达式式 try：`var n = try expr catch (e) default;`（失败给默认值）
> - 完整规范见 [exceptions.md](exceptions.md)

### 附：算子语法（EBNF 增量）

```ebnf
OpAnnot        ::= '@' 'op' '(' StringLiteral ')'   // 符号如 "+" "*" "=="
StructSection  ::= 'operator:' (OpAnnot ReturnType Identifier '(' ParamList? ')' '{' Stmt* '}')+
TopLevelDecl   ::= ... | OpAnnot FunctionDecl
StructDecl     ::= 'struct' (Identifier '::')? Identifier '{' (StructField | StructSection)* '}'
Pipe           ::= Conditional ('|>' Conditional)*   // 低优先级左结合: A |> Op1 |> Op2
```
- **`void`** 仅作为方法返回类型，不能作为变量类型。
