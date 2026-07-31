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
IntegerLiteral ::= [0-9]+ | '0x'[0-9a-fA-F]+        // 十进制 / 十六进制
FloatLiteral   ::= [0-9]+'.'[0-9]* (E[+-]?[0-9]+)?  // 浮点（含科学计数法）
BoolLiteral    ::= 'true' | 'false'
StringLiteral  ::= '"' (字符)* '"'                    // 双引号字符串，支持转义
NullLiteral    ::= 'null'
CharLiteral    ::= "'" 字符 "'"                       // 字符
```

> 整型字面量：十进制 `[0-9]+`、十六进制 `0x…`；后缀 `L` 为 long（独立 token `LongLiteral`）。
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
enum match ffi try catch finally throw where await const ref
```

### 1.5 类型关键字

```
byte short int long ubyte ushort uint ulong char float double bool string
```

### 1.6 运算符与标点

```
算术     + - * / % ++ --
比较     == != < > <= >=
位运算   << >> & ^ |
逻辑     && || !
赋值     = += -= *= /= %=
标点     ( ) { } [ ] ; : :: ? , . .. -> => @
```

---

## 2. 类型

```
Type          ::= BasicType TypeSuffix? | ClassType TypeArgList? TypeSuffix? | 'var'
BasicType     ::= 'byte' | 'short' | 'int' | 'long'
                | 'ubyte' | 'ushort' | 'uint' | 'ulong'
                | 'char' | 'float' | 'double' | 'bool' | 'string' | 'void'
ClassType     ::= Identifier
TypeArgList   ::= '<' Type (',' Type)* '>'
TypeSuffix    ::= '[' IntegerLiteral? ']'   // 数组；'[]' 动态，'[N]' 定长
```

> `var` 类型由编译器推断（仅局部变量声明可用）。

---

## 3. 顶层声明

```
Program          ::= TopLevelDecl*
TopLevelDecl     ::= ImportDecl | ClassDecl | StructDecl | InterfaceDecl
                   | MappingDecl | FunctionDecl | EnumDecl | FFIDecl

ImportDecl       ::= 'import' ImportPath ';'
ImportPath       ::= Identifier            // 标准库/包模块
                   | StringLiteral         // 相对/绝对路径文件

ClassDecl        ::= 'class' Identifier GenericParamList? '{' ClassMember* '}'
GenericParamList ::= '<' Identifier (',' Identifier)* '>'

StructDecl       ::= 'struct' (Identifier '::')? Identifier '{' StructField* '}'
StructField      ::= Type Identifier ('=' Expression)? ';'      // 属性
                   | ReturnType Identifier '(' ParamList? ')' '{' Stmt* '}'  // 方法

InterfaceDecl    ::= 'interface' Identifier '{' InterfaceMember* '}'
InterfaceMember  ::= (返回类型 Identifier '(' ParamList? ')' ';')   // 动作签名
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

FunctionDecl     ::= ReturnType Identifier '(' ParamList? ')' '{' Stmt* '}'
FFIDecl          ::= 'ffi' ReturnType Identifier '(' ParamList? ')' ';'
EnumDecl         ::= 'enum' Identifier '{' EnumVariant (',' EnumVariant)* '}'
EnumVariant      ::= Identifier ('(' Type (',' Type)* ')')?
```

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

ActionDecl       ::= Annot? ReturnType Identifier '(' ParamList? ')' Block? ';'?
ActionAnnot      ::= '@' 'startup' | '@' 'test' | '@' 'coro'
FuncAnnot        ::= '@' 'test'
EventDecl        ::= Identifier '(' ParamList? ')' ';'
PropertyDecl     ::= 'const'? Type Identifier ('=' Expression)? ';'

ParamList        ::= Param (',' Param)*
Param            ::= Type Identifier
ReturnType       ::= Type | 'void'
Block            ::= '{' Stmt* '}'
```

---

## 5. 语句

```
Stmt             ::= Block
                   | VarDeclStmt
                   | IfStmt | WhileStmt | ForStmt
                   | ReturnStmt | BreakStmt | ContinueStmt
                   | AwaitStmt | MappingStmt
                   | MatchStmt | TryStmt | ThrowStmt
                   | ExprStmt ';'?

Block            ::= '{' Stmt* '}'

VarDeclStmt      ::= Type VarDeclarator (',' VarDeclarator)* ';'
                   | 'var' Identifier ('=' Expression)? ';'
                   | 'const' Type Identifier ('=' Expression)? ';'
VarDeclarator    ::= Identifier ('=' Expression)? VarAnnot?
VarAnnot         ::= '@' 'thread' | '@' 'threadpool'

IfStmt           ::= 'if' '(' Expression ')' Stmt ('else' Stmt)?
WhileStmt        ::= 'while' '(' Expression ')' Stmt

ForStmt          ::= 'for' '(' (VarDeclStmt | ';') Expression? ';' Expression? ')' Stmt
                   | 'for' Identifier 'in' RangeExpr Stmt        // 区间 for
                   | '@parallel' 'for' '(' … ')' Stmt            // 数据并行
                   | '@gpu' 'for' '(' … ')' Stmt                 // GPU 卸载
RangeExpr        ::= Expression '..' Expression

ReturnStmt       ::= 'return' Expression? ';'
BreakStmt        ::= 'break' ';'
ContinueStmt     ::= 'continue' ';'
AwaitStmt        ::= 'await' Expression ';'
MappingStmt      ::= 'mapping' '(' ')' MappingAnnot? '{' MappingChain+ '}'  // 局部 mapping

MatchStmt        ::= 'match' '(' Expression ')' '{' MatchArm+ '}'
MatchArm         ::= EnumName '.' VariantName BindingList? '=>' '{' Stmt* '}'
BindingList      ::= '(' Identifier (',' Identifier)* ')'
// 枚举变体匹配；变体可选携带数据绑定 (v1, v2, …)，臂体为块

TryStmt          ::= 'try' Block CatchClause* FinallyClause?
CatchClause      ::= 'catch' '(' Type Identifier ')' Block
FinallyClause    ::= 'finally' Block
ThrowStmt        ::= 'throw' Expression ';'

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
                   | '++' | '--'                  // 后缀增减

Primary          ::= IntegerLiteral | FloatLiteral | BoolLiteral
                   | StringLiteral | CharLiteral | NullLiteral
                   | 'this'
                   | Identifier                    // 变量 / 类名 / 枚举变体
                   | '(' Expression ')'
                   | 'new' ClassType TypeArgList? '(' ArgumentList? ')'
                   | 'new' Type '[' Expression ']' ( '[' Expression ']' )*
                   | LambdaExpression

LambdaExpression ::= '(' ParamList? ')' '=>' '{' Stmt* '}'   // FatArrow '=>'
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
- **`void`** 仅作为方法返回类型，不能作为变量类型。
