# myp_self 前端 dump 格式契约（FROZEN v1）

> 状态：**已冻结（2026-08-13，F0）**——C++ `mypc --frontend-dump <mode>` 已按此实现；
> MYP 侧 `myp_self --frontend-dump <mode>` 必须字节级复刻，**禁止反向迁就本契约**。
> 实现：`include/mylang/FrontendDump.h` + `src/frontend_dump.cpp`（C++ 权威）。
> 对应 `design.md` §5.1。

## 1. 总体结构

所有模式输出到 stdout，结构统一：

```
MYP_FRONTEND_DUMP v1          # 第 1 行：版本头
(Mode <tokens|ast|sema>)      # 第 2 行：模式标记
<mode 内容>                   # 模式特有记录
(Diagnostics)                 # 恒输出（空则 0 条）
  (Diag line=<L> col=<C> sev=<error|warning|info> msg="<M>")
  ...
(Result ok=<0|1> errors=<N>)  # 末行：0=成功 1=有错误
```

退出码：成功=0，有诊断错误=1。

## 2. 确定性规则（MYP 侧必须遵守）

1. **哈希容器排序**：所有 `unordered_map/set` 在输出前按 key（字符串字典序，逐字节 `<`）
   排序。C++ 侧 `type_param_constraints`/`associated_type_bindings` 已排序。
2. **字符串转义**：`\n → \\n`、`\t → \\t`、`\r → \\r`、`" → \\"`、`\ → \\\\`、
   `\0 → \\0`，其余原样。
3. **double**：`%.17g`（精确往返）。
4. **int64**：有符号十进制。

> **NUL 字节（MYP 侧哨兵约定，F1）**：MYP 字符串无法承载 NUL 字节（C 串截断），
> MYP lexer 把 `\0` 转义解码为 **0x01 哨兵**（T2 先例），`escape` 把 0x01 映射回 `\0`。
> 前提：MYP 源无 `\x01` 转义、无原始 0x01 字节 → 哨兵不冲突。

## 3. tokens 模式

```
token <begin>:<end> <kind> "<value>"
```

- `begin`/`end`：`SourceRange.begin_offset` / `end_offset`（字符偏移，0 基）。
- `<kind>` = 规范名 `canonicalTokenKindName`：
  - 字面量：`integer` `long` `uint` `float` `float32` `string` `char` `bool` `null`
  - `identifier` `eof` `unknown`
  - 关键字/类型/算子：`Token::keywordString` 原文（如 `class` `int` `=` `+=` `@` `(` `::`
    `|>` `<` `>` `&` `^` `|` `<<` `>>` `..` `->` `=>`）
- `<value>`：字面量/标识符原文（转义后）；关键字/算子/EOF 为空串。
- 顺序 = tokenize 顺序；EOF 收尾。

## 4. ast 模式

缩进树（2 空格/层），节点名与字段顺序固定（见下）。哈希容器字段排序；子节点按声明序。

顶层：

```
(TranslationUnit)
  (Import name="<n>" path="<p>" isPath=<0|1>)
  (Struct ...) (Bitfield ...) (Class ...) (Interface ...) (Mapping ...)
  (Function ...) (Enum ...) (FFI ...) (Macro ...) (TypeAlias name="<n>" type=<T>)
```

字段格式：`(NodeName key=value ...)`，字符串值双引号+转义，数值十进制。`type=<T>` 用
`typeNodeToString`（见 §6）。

各节点字段（实现见 `src/frontend_dump.cpp`）：

- `Class`: `name isStatic isGenericInst lambda iface` + 子：`TypeParam`、`Constraint
  name iface`（排序）、`InstTypeArg type`、`AssocType name type`（排序）、`NonlocalSlot
  slot var cell`、各 `Action`、`(Static)`+`Action`、`Event`、`Property`、`Function`、
  `Struct`。
- `Action`: `name ret startup ctor test coro async region stack` + 子：`TypeParam`、
  `Constraint`、`Param`、body `Stmt`。
- `Function`: `name ret genericInst test autoMain region coro async stack op eval macro
  ctor constDecl` + 子同上。
- `Event`: `name` + `Param`。`Property`: `name type const weak`。`Param`: `name type ref`
  + `hasDefault=1` + 默认 expr 子节点。
- `Struct`: `name parent` + `Property`/`Function`。`Bitfield`: `name totalBits` +
  `Field name width offset`。`Enum`: `name` + `Variant name` + `Param`。`FFI`: `name ret`
  + `Param`。
- `Mapping`: `scope` + `Chain` + `Node src member isFunction isLambda isTransformer
  trKind trParam`（+ lambda expr 子节点）+ `Where` + where expr。`Macro`: `name` +
  `Param name` + body。

语句 `Stmt`：

- `(Block)` + stmts；`(VarDecl)` + `Var name type const thread threadpool`（+ init expr）；
  `(Destructure isDecl=0|1)` + target + value。
- `(If)`/`(While)`/`(Return)`/`(Break)`/`(Continue)`/`(Throw)`/`(Await)` + 子节点。
- `(For parallel=.. gpu=.. stride=.. block=..)` + `Resident arr dev` + init/cond/step/
  stream/body。
- `(ForIn name type hasType iterKind class sizeFn getFn arrSize)` + iterable + body。
- GPU 语句：`(GpuTile name hasGrid grid block)`/`(GpuReduce acc x arr out block)`/
  `(GpuScan acc x in out exclusive block)`/`(GpuScatter a b idx mode block)` + 子节点。
- `(MappingStmt)`+Mapping；`(Match)` + subject + `Arm enum variant index bindings=[..]`；
  `(Try)` + try/catches（`Catch var type`）/finally；`(Nonlocal names=[..])`。

表达式 `Expr`（每个末尾追加 ` : <type>` 仅 sema 模式）：

- `(Integer value long unsigned char)`；`(Float value f32)`；`(Bool value)`；
  `(String value)`；`(Null)`；`(Identifier name)`；`(This)`。
- `(Binary op=.. lhsUnsigned rhsUnsigned resultUnsigned opCall=0|1)` + lhs/rhs；
  `(Unary op=..)`；`(Convert to=.. bw=..)`。
- `(Call typeArgs=[..] resolved structType structCtor)` + callee + args；
  `(Member name resolvedClass)` + object；`(Subscript)` + array/index；
  `(New class typeArgs=[..] ctor)` + args；`(NewArray elem=..)` + dims；
  `(Assign)`/`(Ternary)`/`(Range)` + 子。
- `(EnumVariant enum index)` + args；`(Lambda name hiddenClass captures=[..] slots=[..]
  nonlocal=[..])` + params + body；`(Pipe target class method)` + lhs/rhs；
  `(TryExpr catchVar)`；`(Await)`；`(Tuple)` + elems；`(NamedArg name)` + value；
  `(MacroParam name)`；`(Quote)` + body；`(GpuReduceExpr result=..)` + stmt。
- 二元/一元 op 名：`+ - * / % == != < > <= >= && || & | ^ << >>` / `- ! ~`。

## 5. sema 模式

- 内容 = **sema 后的 AST**（泛型单态化实例会出现在 `classes`/`functions`），每个表达式
  行尾追加 ` : <type>`（`expr.type` 非空用 `typeNodeToString`，否则用 `resolved_kind`
  的 `typeKindToString`）。
- 诊断由调用方统一输出（见 §1），**sema 模式本身不输出 Diagnostics 段**。

## 6. 类型字符串 `typeNodeToString`

```
元素数组:   <elem>[]           定长: <elem>[N]
类:         <Name><A,B>         泛型参数: $T
函数:       (A,B)->R            元组: (A,B)
基本: byte short int long ubyte ushort uint ulong char float double bool string void
      float4 double2 int4 bit bitvector<N>
```

`typeKindToString`（sema 回退）：同上基本名 + `class struct enum interface array slice
function tuple assoc bitfield null void`。

## 7. 变更流程（禁止破坏）

- 本契约修改 = **破坏性变更**（对应语言规格冻结策略）。改动须：C++ 侧先改 →
  `format.md` 更新 → MYP 侧同步 → 全语料对拍重新通过。禁止只改一侧。
- 版本头 `MYP_FRONTEND_DUMP v1` 随破坏性变更递增。
