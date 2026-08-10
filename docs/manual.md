# MYP 编程手册

> 版本 3.0 | 事件驱动组件语言
> 语言规格 v1.0（语法冻结）：正式 EBNF 见 [grammar.md](grammar.md)，版本策略见 [CHANGELOG.md](CHANGELOG.md)。

---

## 目录

1. [快速开始](#1-快速开始)
2. [基础语法](#2-基础语法)
3. [类型系统](#3-类型系统)
4. [控制流](#4-控制流)
5. [函数](#5-函数)
6. [Class 组件系统](#6-class-组件系统)
7. [Struct 数据结构](#7-struct-数据结构)
8. [事件与 Mapping](#8-事件与-mapping)
9. [并发编程](#9-并发编程)
10. [模块与导入](#10-模块与导入)
11. [标准库](#11-标准库)
12. [编译与工具](#12-编译与工具)
13. [完整示例](#13-完整示例)

---

## 1. 快速开始

### 安装

```bash
# 编译 MYP 编译器
cd MYPLanguage/build
cmake .. -DCMAKE_PREFIX_PATH=/usr/lib/llvm-21/lib/cmake/llvm
make -j$(nproc)

# 验证安装
./build/mypc --help
```

### Hello World

```myp
// hello.myp
import env;

int main() {
    Console.writeLine("Hello, MYP!");
    return 0;
}
```

```bash
./build/mypc hello.myp
./hello.out
# 输出: Hello, MYP!
```

### 编译选项

```bash
mypc <file.myp>                  # 编译并链接
mypc -o myapp <file.myp>         # 指定输出文件名
mypc -O2 <file.myp>              # 优化级别
mypc --trace <file.myp>          # 启用运行时事件追踪
mypc --stdlib <path> <file.myp>  # 指定标准库路径
mypc --package-path <path> <file.myp>  # 指定本地包搜索路径
mypc --shared <file.myp>              # 编译为共享库 (.so)
mypc --static <file.myp>              # 编译为静态库 (.a)
```

---

## 2. 基础语法

### 注释

```myp
// 单行注释
/* 多行
   注释 */
```

### 变量声明

```myp
// 显式类型
int count = 0;
double pi = 3.14159;
string name = "MYP";
bool flag = true;
char letter = 'A';

// 类型推断 (v2+)
var x = 42;         // int
var y = 3.14;       // double
var s = "hello";    // string

// 常量——变量必须显式初始化
int a;              // 默认初始化为 0
```

> **`const`**：`const Type Name = Expr;` 声明常量（必须初始化）。可用在：
> `property:` 段（const 属性）、class 顶层（`const double THERMAL = 0.0253;`，等价 const 属性）、
> 局部变量（`const Type Name = Expr;`）。class 顶层 const 是**每实例一份的常量字段**
> （非类级共享；如需类级共享常量可用 `@static` 类）。属性默认值（`int x = 5;`）在 `new` 时生效。

### 字面量

```myp
42          // 整数 (自动推断为 byte/short/int/long)
3.14        // 浮点 (double)
1.0e-5      // 科学计数法
0xFF        // 十六进制
true false  // 布尔
'A' '\n'    // 字符 (支持 \n \t \\ \' \" \0)
"hello"     // 字符串 (支持 \n \t \\ \" \0)
null        // 空值
```

> **`null` 语义**：`null` 可赋给**引用类型**（class/interface/struct 指针），并可用
> `x == null` 判断。**`string` 是值语义**（字符缓冲），**不能**赋 `null`（编译报错）。
> 解引用一个 `null` 的 class 引用是运行时错误（不保证防护），调用前应判空。

### 字符串插值 (v2+)

```myp
var name = "world";
var msg = "Hello, $name!";   // → "Hello, world!"
var x = 42;
var s = "x = $x";            // → "x = 42"
```

### 运算符

| 优先级 | 类别 | 运算符 |
|--------|------|--------|
| 10 | 赋值 | `=` `+=` `-=` `*=` `/=` `%=` |
| 9 | 管道 | `\|>` |
| 8 | 三元 | `? :` |
| 7 | 逻辑或 | `||` |
| 6 | 逻辑与 | `&&` |
| 5 | 等值 | `==` `!=` |
| 4 | 关系 | `<` `>` `<=` `>=` |
| 3 | 加法 | `+` `-` |
| 2 | 乘法 | `*` `/` `%` |
| 1 | 一元 | `!` `-` `++` `--` |
| 0 | 后缀 | `.` `[]` `()` `++` `--` |

```myp
// 复合赋值
x += 5;      // x = x + 5
x *= 2;      // x = x * 2

// 自增/减
x++;         // x = x + 1
--x;         // 前缀也可

// 字符串拼接
var msg = "Hello, " + "world!";  // string + string
var s = "count: " + x;           // string + int (自动转换)

// 三元运算
var max = a > b ? a : b;

// Range (v2+)
var r = 0..10;  // 范围表达式

// 管道 (v2.4+): A |> Op，调用算子组件的 transform（左结合）
// Op 为算子类名（自动实例化）或实例（复用）
var v = A |> ScaleOp;
```

---

## 3. 类型系统

### 基本类型

| 类型 | 描述 | 大小 |
|------|------|------|
| `byte` | 有符号 8-bit（`int8` 别名） | 8 |
| `short` | 有符号 16-bit（`int16` 别名） | 16 |
| `int` | 有符号 32-bit（`int32` 别名） | 32 |
| `long` | 有符号 64-bit（`int64` 别名） | 64 |
| `ubyte` | 无符号 8-bit（`uint8` 别名） | 8 |
| `ushort` | 无符号 16-bit（`uint16` 别名） | 16 |
| `uint` | 无符号 32-bit（`uint32` 别名） | 32 |
| `ulong` | 无符号 64-bit（`uint64` 别名） | 64 |
| `char` | 字符 (8-bit) | 8 |
| `float` | 单精度浮点 | 32 |
| `double` | 双精度浮点 | 64 |
| `bool` | 布尔值 | 1 |
| `string` | 字符串指针 | 指针 |
| `void` | 无类型 | — |

### 数字提升

数字类型之间自动隐式转换：

```
byte/char → short → int → long → float → double
```

```myp
int a = 42;
long b = a;       // ✅ int → long 自动提升
double c = a;     // ✅ int → double 自动提升
int d = b;        // ❌ long → int 不会自动降级
```

### 无符号类型（uint 族）

无符号类型 `ubyte`/`ushort`/`uint`/`ulong`（固定宽度别名 `uint8`/`uint16`/
`uint32`/`uint64`）具有**无符号语义**，与 C 一致：

- **字面量 `u` 后缀**：`0xFFFFFFFFu` 是无符号整数字面量（按值定宽：≤0xFF→`ubyte`，
  ≤0xFFFF→`ushort`，≤0xFFFFFFFF→`uint`，更大→`ulong`）。`uint x = 0xFFFFFFFFu;`
  可直接初始化，无需 `long` 掩码。
- **逻辑右移**：`uint` 的 `>>` 是逻辑右移（`lshr`），不是算术右移。
- **无符号除法/取模**：`/`→`udiv`、`%`→`urem`。
- **无符号比较**：`<`/`>`/`<=`/`>=` 用无符号谓词。
- **回绕**：加减法自动按 32 位回绕，无需 `& 0xFFFFFFFF`。
- **uint→long 拓宽**用 ZExt（`0xFFFFFFFFu` → `4294967295L`，不是 -1）。
- **原生旋转**：`(x >> n) | (x << (32 - n))` 被 LLVM 识别为单条 `rorl`/`rol`。

```myp
uint a = 0xFFFFFFFFu;
uint b = a >> 4;          // 逻辑右移 → 0x0FFFFFFF
uint c = a / 3u;          // 无符号除法 → 1431655765
bool big = a > 100u;      // 无符号比较 → true
long v = a * 4294967296L; // uint→long ZExt → 4294967295 << 32
```

> 注：无符号值要打印为 `long` 时，经二元运算（如 `x * 1L`）显式拓宽；`ulong`
> 不隐式转 `long`（值可能溢出有符号范围）。

### 复合类型

```myp
int[] arr;              // 数组
string[] names;         // 字符串数组
int[10] fixed;          // 定长数组
ClassName obj;          // 类类型 (指针)
ClassName::StructType;  // 嵌套 struct 类型
```

### 类型别名 `type X = ...`（v3.x，additive）

`type Name = Type;` 为类型起别名，`Name` 可在后续任何类型位置使用（参数/返回/
属性/局部变量/泛型实参/数组元素），完全等价于 `Type`。`type` 是**上下文关键字**，
仅顶层 `type <Id> = <Type> ;` 形态被识别为声明。

```myp
type MyInt = int;
type Int3 = int[3];
type AliasAlias = MyInt;       // 别名套别名

MyInt x = 42;                  // ≡ int x = 42;
```

### 元组类型 (v3.x，additive)

`(Type, Type, ...)` ≥2 元素；支持**多值返回**、**解构**、**字段访问 `t.N`**。

```myp
// 多值返回
(int, string) getPair() { return (1, "x"); }

// 声明式解构
(int a, string b) = getPair();          // a=1, b="x"

// 元组变量 + 字段访问
(int, int) t = (3, 4);
int c = t.0;                            // 3

// 赋值式解构（变量须已声明）
int x; int y;
(x, y) = getPair();                     // x=1, y="x"

// 嵌套解构
((int p, int q), int z) = ((1, 2), 5);  // p=1, q=2, z=5

// `_` 忽略符：跳过不关心的元素（不绑定、不检查类型）
(int _, string name) = getPair();       // 丢弃第一个元素
(int a, string _) = getPair();          // 丢弃第二个元素
(int _, string _) = getPair();          // 只要多值返回副作用
```

> 与函数类型 `(A, B) -> R`、lambda `(a, b) => ...` 自动消歧；设计见 `docs/tuple.md`。
> `_` 本身是合法标识符，可在其他上下文正常使用。

### 可空类型 `Option<T>` / `T?`（v3.x，additive）

显式可空包装避免裸 `null` 解引用。需 `import option;`。

```myp
import option;
Option<int> none = new Option<int>();      // none
Option<int> some = new Option<int>(42);    // some
int? maybe = new Option<int>(7);           // T? ≡ Option<T>（类型位置）
if (some.isSome()) {
    int v = some.get();                    // 42（先 isSome 再 get）
}
int safe = maybe.getOr(0);                 // 安全取用（none → 默认值）
some.set(9);
some.clear();                              // 变回 none
```

> API：`isSome()`/`isNone()`/`get()`/`getOr(def)`/`set(v)`/`clear()`。

---

## 4. 控制流

### If / Else

```myp
if (x > 0) {
    Console.writeLine("正数");
} else if (x == 0) {
    Console.writeLine("零");
} else {
    Console.writeLine("负数");
}
```

### While

```myp
var i = 0;
while (i < 10) {
    Console.writeLong(i);
    i++;
}
```

### For

```myp
for (var i = 0; i < 10; i++) {
    Console.writeLong(i);
}
```

### For-in — 集合迭代（§四-2）

`for-in` 用统一语法遍历四种可迭代源（括号可选，类型可选）：

```myp
// 1) 固定数组 T[N]（编译期长度）
int[4] arr = ...;
for (x in arr) { ... }

// 2) slice<T>（用 .size() 求长）
slice<int> s = new slice<int>(3);
for (x in s) { ... }

// 3) 集合类——需实现 size() + get(int) 方法（de-facto 迭代器协议，如 ArrayList<T>）
ArrayList<int> list = new ArrayList<int>();
list.add(10); list.add(20);
for (x in list) { ... }

// 4) range：for (i in a..b) 等价 for (int i = a; i < b; i++)（右开区间）
for (i in 0..5) { ... }        // i = 0,1,2,3,4
```

四种形式：

```myp
for (x in coll) { ... }        // 括号 + 无类型（元素类型由迭代源推断）
for (int x in coll) { ... }    // 括号 + 显式元素类型
for x in coll { ... }          // 无括号（仅无类型）
for (i in 0..5) { ... }        // 无括号 range（右开 i < 5）
```

- 循环变量每次迭代重新声明（作用域级），支持 `break` / `continue` 与嵌套。
- 迭代源的表达式只求值一次；集合类迭代时临时引用在循环结束后释放。
- 类元素（如 `Node[]`、`ArrayList<Node>`）按 ARC 借用语义 retain，循环变量离开迭代作用域时释放——实测零泄漏。
- **不可迭代**（编译期报错）：动态数组 `int[]`（无运行时长度，请用 `slice<T>` 或集合类）、无 `size()/get(int)` 的类、非集合类型（如 `int`）、集合元素为数组的集合。

### Break / Continue

```myp
for (var i = 0; i < 10; i++) {
    if (i == 5) break;      // 跳出循环
    if (i % 2 == 0) continue; // 跳过偶数
}
```

### Return

```myp
int add(int a, int b) {
    return a + b;
}
void log(string msg) {
    Console.writeLine(msg);
    return;  // 可选
}
```

### 异常处理（try / catch / finally / throw）

MYP 用 `try` / `catch` / `finally` / `throw` 做结构化异常处理，机制基于 C `setjmp`/`longjmp`（每 try 独立 handler，线程本地）。

#### 基本 try/catch

```myp
try {
    int v = parseValue(s);
} catch (e) {                 // catch (e)：兜底，e 为 string 消息
    Console.writeLine("failed: " + e);
}
```

catch 子句形式：
- `catch (e)` —— 无类型兜底，`e` 为 string 消息
- `catch (string e)` —— 显式捕获 string 异常
- `catch (ClassName e)` —— 精确捕获某异常类
- `catch (Error e)` —— 捕获任意实现 `Error` 接口的异常对象（`e.message()`）

#### 多 catch 按序匹配

```myp
try {
    ...
} catch (FileError e) {
    ...
} catch (ParseError e) {
    ...
} catch (e) {                 // 兜底
    ...
}
```

不匹配的异常自动向外层传播；无外层 catch 时打印 `uncaught exception: <msg>` 并 abort。

#### throw

```myp
throw "some message";      // string 快捷
throw new FileError();     // 异常对象（实现 Error 接口）
```

`throw;` 在 catch 内**重新抛出**当前异常（保留消息/类型，带 finally 时先执行 finally）：

```myp
try {
    doWork();
} catch (e) {
    log("failed");
    throw;                  // 交给外层处理
}
```

#### finally

`finally` 块在所有退出路径都执行——try 正常结束、catch 匹配、异常传播，以及 `return` / `break` / `continue`：

```myp
try {
    File f = new File();
    f.open(path, "r");
} finally {
    Console.writeLine("cleanup");   // 任何路径都会执行
}
```

#### 表达式 try

`try <expr> catch (e) <expr>` 作为表达式，失败给默认值：

```myp
int n = try parseInt(s) catch (e) -1;   // 成功取 parseInt 结果，失败取 -1
```

#### 标准异常（`import error`）

标准异常类全部实现 `Error` 接口，可被 `catch (Error e)` 统一捕获：

| 异常 | 用途 | 主要属性 |
|---|---|---|
| `FileError` | 文件操作失败 | `op`、`path` |
| `IOError` | 通用 I/O 失败 | `op`、`detail` |
| `NetError` | 网络失败 | `op`、`host`、`port` |
| `ParseError` | 解析失败 | `source`、`line`、`detail` |
| `JsonError` | JSON 解析失败 | `line`、`col`、`detail` |
| `ArgumentError` | 参数错误 | `arg`、`detail` |
| `MathError` | 数学域错误 | `op`、`detail` |
| `IndexError` | 下标越界 | `index`、`size` |

用 setter 填充属性后抛出（MYP 类无带参构造）：

```myp
FileError e = new FileError();
e.setOp("open");
e.setPath("config.myp");
throw e;
```

#### 库接入

`io` / `json` / `net` 库在失败时抛标准异常：`File.open` 失败抛 `FileError`，`new Json(...)` 非法输入抛 `JsonError`，`TcpClient.connect` 失败抛 `NetError`。

```myp
import io;
try {
    File f = new File();
    f.open("/no/such/file", "r");
} catch (FileError e) {
    Console.writeLine(e.message());   // "file error: open /no/such/file"
}
```

#### 自定义异常对象

实现 `Error` 接口即可作为异常对象抛出：

```myp
import error;
class MyError {
    interface class Error;
    action:
        string message() { return "my error: " + code_; }
        void setCode(int v) { code_ = v; }
    property:
        int code_;
}

try {
    MyError e = new MyError();
    e.setCode(42);
    throw e;
} catch (Error e) {
    Console.writeLine(e.message());   // "my error: 42"
}
```

> 详细设计见 [`docs/exceptions.md`](exceptions.md)。

### 值式错误传播：Result\<T, E\>（§五-3，additive）

除 try/catch 外，`import result` 提供**值式错误传播**：`Result<T, E>` 是 Ok(值)/Err(错误)
二态容器，错误作为返回值显式传递（配合 `error.myp` 的错误类型分层）。

```myp
import result;

Result<int, string> ok  = new Result<int, string>(42);   // ok
Result<int, string> bad = new Result<int, string>();     // err（未初始化 E）
bad.setErr("oops");

if (ok.isOk())  Console.write(ok.get());      // 42（先 isOk 再 get）
if (bad.isErr()) Console.writeString(bad.getErr());   // "oops"
int v = bad.getOr(-1);                        // 安全取用 → -1
```

工厂（顶层泛型函数）：

```myp
Result<string, string> s = resultOk<string, string>("hi");
Result<int, string>    b = resultErr<int>("bad");   // T 显式、E 从实参推断
```

组合子（无异常错误传播）：

```myp
Result<string, string> m = resultMap(f1, (int x) => { return "v" + x; });  // 仅 ok 应用 f
Result<int, string>    a = resultAndThen(f1, (int x) => { return resultOk(x * 3); });
Result<int, string>    e = resultMapErr(f2, (string e) => { return "E:" + e; });
```

异常桥 `resultTry`（把可能抛异常的调用转成 `Result<T, string>`，错误统一为消息）：

```myp
Result<int, string> r = resultTry<int>(() => { return risky(); });
if (r.isErr()) Console.writeString(r.getErr());
// throw "msg" → err(msg)；throw <Error 对象> → err(e.message())
// 错误类型分层：精确捕获用具体异常类 + catch，统一处理用 Error 接口 + resultTry
```

> 组合子 `resultMap` 等为**顶层泛型函数**（泛型体内不能调用其它泛型函数，故直接构造
> Result）。`tests/result` 覆盖全路径。

---

## 5. 函数

### 顶层函数

```myp
int add(int a, int b) {
    return a + b;
}

int main() {
    var result = add(10, 20);  // 30
    return 0;
}
```

### main 函数规则

`main()` 是程序入口，**有严格的限制**——这是 MYP 事件驱动模型的核心：

```myp
int main() {
    // ✅ 允许：创建实例
    Sensor sensor = new Sensor();
    Display display = new Display();

    // ✅ 允许：声明 mapping
    mapping() {
        sensor.valueRead -> display.show;
    }

    // ❌ 禁止：直接调用方法
    sensor.readValue();       // 编译错误

    // ❌ 禁止：访问属性
    sensor.propertyName = 42; // 编译错误

    return 0;
}
```

> **原则**：main 只做"接线"，不做"操作"。所有逻辑在组件的 action/function 中实现。

### main(argc, argv)（v2+）

带参数的入口：接收命令行参数（配合 `import args` 使用）：

```myp
int main(int argc, string[] argv) {
    // argc: 命令行参数个数
    // argv: 参数字符串数组
    return 0;
}
```

### 泛型函数 (v3.x，additive)

函数名后可带类型参数 `T foo<T>(T x)`；调用时**显式类型实参**或**实参推断**。

```myp
T id<T>(T x) { return x; }
T max2<T>(T a, T b) { if (a > b) return a; return b; }

int a = id<int>(5);    // 显式
int b = id(7);         // 推断 → T=int
string s = id("hi");   // T=string
```

- 泛型函数按类型实参**单态化**（`id_int_inst`），模板本身不生成运行时代码。
- 支持 `T[]` 参数推元素类型；推断失败须显式给类型实参。

### 默认参数 / 命名实参 (v3.x，additive)

参数可带默认值；调用时可省略有默认值的形参，或用 `name = value` 命名实参（可乱序）。

```myp
// 定义：带默认值的参数可省略
int add(int a, int b = 100, int c = 200) { return a + b + c; }

add(1);            // 301（省略 b、c → 用默认值）
add(1, 2);         // 203（省略 c）
add(1, 2, 3);      // 6（全给）

// 命名实参（乱序）——`name = value` 在调用点按形参名匹配
add(1, c = 5);     // 106（b 用默认 100）
add(1, b = 5, c = 6); // 12
mul(b = 7, a = 6); // 42（命名实参可乱序）

// 构造器 / 方法 / 静态方法 / struct 构造同样支持
Rect r = new Rect(h = 5);            // w 用默认 1
Vec2 v = Vec2(px = 3.0, py = 4.0);   // struct 函数式构造命名实参
g.greet(name = "Al", suffix = "?");  // 方法命名实参
Greeter.scale(5);                    // 静态方法省略默认参数 f=2
```

- 适用：顶层函数、类方法（`action:`/`function:`）、静态方法、构造器（`new`）、struct 构造。
- **位置实参按序填前 N 个形参，命名实参按名填入**；同一形参不能同时被位置与命名提供。
- 默认表达式在**调用点**求值（通常为常量）；声明期校验默认值类型与形参兼容。
- `name = value` 在实参位置按赋值表达式解析，语义阶段按「目标标识符匹配形参名」重解释
  为命名实参——因此**宏的赋值实参**（`repeat(3, v = v + 10)`，宏参数 `$n/$body` 不匹配）不受影响。
- 负例（编译期报错）：未知/重复命名实参、位置+命名重叠、必填参数缺失、实参过多、默认值类型不匹配。

### 一等函数与闭包 (v3.x，additive)

函数类型 `(A, B) -> R` 是一等值：可存变量、作参数/返回值、直接调用。lambda
`(params) => { body }` 创建函数值（**按值捕获**外层局部）。

```myp
// 函数类型变量 + lambda
(int) -> int add1 = (int x) => { return x + 1; };
int r = add1(41);                       // 42

// 高阶函数：函数值作参数
int apply2(int v, (int) -> int f) { return f(v); }
int r2 = apply2(10, (int x) => { return x * 2; });   // 20

// 函数返回闭包（捕获参数 n）
(int) -> int makeAdder(int n) { return (int x) => { return x + n; }; }
(int) -> int add5 = makeAdder(5);
int r3 = add5(3);                       // 8

// 泛型高阶：Option.map 式组合
Option<R> mapOpt<T, R>(Option<T> o, (T) -> R f) { ... }
```

- 运行时表示：胖指针 `{closure, call_fn}` + 统一 tramp。
- 捕获：标量/字符串**深拷贝**、class 引用**浅拷贝**（共享实例）、支持嵌套 lambda。

---

## 6. Class 组件系统

### 三段式结构

MYP 的 class 是事件驱动组件，包含三个段：

```myp
class Sensor {
    action:          // 可被调用的方法（接收消息）
        void init(int id);
        float readValue() { return lastValue; }

    event:           // 可触发的事件（发送消息）
        valueRead(float temp);
        thresholdExceeded(float value);

    property:        // 内部状态（私有）
        int sensorId;
        float threshold;
        float lastValue;
}
```

### 段规则

| 段 | 内容 | 规则 |
|----|------|------|
| `action:` | 方法（有返回类型） | 可用 `;` 声明或 `{ }` 实现 |
| `event:` | 事件（无返回类型） | 仅 `;` 声明 |
| `property:` | 成员变量 | 仅变量声明 |
| `function:` | 内部方法 | 仅类内部可调用 |
| `static:` | 静态方法 | 无需实例，`ClassName.method()` 调用 |

### Interface 接口多态 (v2.3)

Interface 定义一组 action 签名，类通过 `interface class InterfaceName;` 声明实现：

```myp
interface IShape {
    double area();
    double perimeter();
}

class Circle {
    interface class IShape;
    action:
        double area() { return 3.14 * r * r; }
        double perimeter() { return 2 * 3.14 * r; }
    property: double r = 1.0;
}

// 接口变量：胖指针 {ptr data, ptr vtable}
IShape s = new Circle();
double a = s.area();        // 虚表分派 → Circle_area
```

适合算子模式实现自动微分（见 `examples/ad.myp`）。

#### 默认实现（trait 默认方法，v3.9.0，additive）

接口方法可以**带默认体**——实现类**省略该方法**时自动继承默认逻辑，实现则覆盖：

```myp
interface IShape {
    double area();                       // 纯签名 → 实现类必须实现
    double perimeter();
    string describe() {                  // 默认实现 → 实现类可省略
        return "area=" + area() + " perim=" + perimeter();
    }
}

class Circle {
    interface class IShape;
    action:
        double area() { return 3.14 * r * r; }
        double perimeter() { return 2 * 3.14 * r; }
        // 未写 describe() → 用 IShape.describe 默认实现（this.area → Circle_area）
    property: double r = 1.0;
}

class Square {
    interface class IShape;
    action:
        double area() { return side * side; }
        double perimeter() { return 4 * side; }
        string describe() { return "SQUARE(" + area() + ")"; }  // 覆盖默认
    property: double side = 3.0;
}

IShape c = new Circle();   IShape s = new Square();
c.describe();   // "area=3.14 perim=6.28"（默认，内部 this.area 分派到 Circle）
s.describe();   // "SQUARE(9)"（Square 覆盖）
```

- **语义**：默认方法按实现类**特化**（`__ifdef_<Iface>_<method>_<Class>`），体内
  `this.method()` / 裸方法调用**静态解析到该具体类**；默认方法调用另一个默认方法也支持。
- **约束**：纯签名（无默认体）的接口方法实现类**必须**实现；默认体引用的抽象方法由此保证存在。
- **价值**：给接口**新增方法**不破坏已有实现类（自动获得默认行为）；公共组合逻辑写在接口里。

#### 关联类型（associated types，v3.9.0，additive）

接口可以声明**关联类型**（Rust 的 associated type 语义）——接口方法签名引用该抽象
类型，由**各实现类绑定具体类型**（`int`/`string`/自定义类等）。同一接口可被不同
元素类型的实现类实例化：

```myp
interface Container {
    type Item;                    // 关联类型声明（抽象）
    bool contains(Item v);        // 方法参数引用关联类型
    Item getVal();                // 方法返回引用关联类型
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
```

- **绑定**：实现类**必须**用 `type Item = int;` 绑定（否则编译错误，负测试
  `assoc_unbound`）。
- **直接引用**：绑定类型通过 `X::Item` 语法引用——`IntBox::Item ≡ int`，可作局部
  变量、参数、返回类型：

```myp
IntBox::Item x = 5;          // ≡ int x = 5;
Container c = new IntBox();
bool r = c.contains(x);      // 接口变量上按虚表分派
```

- **泛型约束**：泛型类/函数用 `where T : I` 约束 T，内部以 `T::Item` 引用关联类型
  ——实例化时 `T` 绑定具体类，`T::Item` 自动单态化为该类绑定类型：

```myp
class Processor<T where T : Container> {
    action:
        T::Item peek(T c) { return c.getVal(); }      // 返回关联类型
        bool check(T c, T::Item v) { return c.contains(v); }
}

Processor<IntBox> pi = new Processor<IntBox>();
int iv = pi.peek(ib);        // T::Item = int → 42
Processor<StrBox> ps = new Processor<StrBox>();
string sv = ps.peek(sb);     // T::Item = string → "hi"
```

- **价值**：接口方法与**元素类型解耦**——同一份接口逻辑适配任意具体类型，泛型代码
  在保留类型安全的同时无需为每种元素类型重写。

### 访问控制

```myp
class Counter {
    action:
        void increment() { count = count + 1; }  // ✅ this.count 可读写
        int getCount() { return count; }          // ✅
    property:
        int count;     // 私有——外部不能直接访问
}

int main() {
    Counter c = new Counter();
    c.increment();     // ✅ action 可调用
    c.count = 5;       // ❌ 编译错误——property 私有
    return 0;
}
```

### function: 内部方法

```myp
class Calculator {
    action:
        int compute(int n) {
            return helper(n);  // ✅ 内部调用 function
        }
    function:
        int helper(int n) {   // 不参与 mapping
            return n * n;
        }
}
```

### static: 静态方法

```myp
class Math {
    static:
        double sqrt(double v) { return __myp_math_sqrt(v); }
}

int main() {
    var r = Math.sqrt(64.0);  // ✅ 直接调用，无需 new
    return 0;
}
```

#### 泛型静态方法（v3.x，additive）

`static:` 段内方法名后可带类型参数——**泛型静态方法**：模板在 stdlib/`@static class`
中定义，任意模块调用（`map`/`filter`/`reduce` 落位）。

```myp
@static class List {
    static:
        Option<R> map<T, R>(Option<T> o, (T) -> R f) {
            Option<R> r = new Option<R>();
            if (o.isSome()) r.set(f(o.get()));
            return r;
        }
        int foldInt<T>(ArrayList<T> arr, int init, (int, T) -> int f) { ... }
}

// 调用（跨模块）：显式类型实参 + 一等函数实参
Option<int> some = new Option<int>(5);
Option<string> m = List.map<int, string>(some, (int x) => { return "v" + x; });
```

- 单态化实例名 `__gs_<Class>_<method>_<types>_inst`；模板本身不生成运行时代码。
- 泛型静态方法无 `this`；支持显式类型实参与实参推断。

---

## 7. Struct 数据结构

### 文件级 struct

```myp
struct Vec2 {
    double x;
    double y;

    // struct 方法（可选）
    double length() {
        return Math.sqrt(x * x + y * y);
    }
}

// 使用
Vec2 v;
v.x = 3.0;
v.y = 4.0;
var len = v.length();  // 5.0
```

### Struct 方法高级特性

Struct 方法支持以下特性：

#### this 关键字

```myp
struct MyStruct {
    double x;
    void setX(double v) {
        this.x = v;  // this 指向当前实例
    }
}
```

#### 返回 struct 类型

```myp
struct Inner { double val; }

struct Outer {
    Inner inner;
    Inner getInner() {
        return inner;  // 返回 struct 值
    }
}
```

#### 兄弟方法互相调用

```myp
struct Helper {
    double calc(double x) { return x * 2.0; }
    double process(double v) {
        return calc(v) + 1.0;  // 直接调用 calc
    }
}
```

### 嵌套 struct

```myp
class Sensor {
    struct Config {
        int rate;
        bool enabled;
    }
}

// 外部定义（类外部展开）
struct Sensor::Config {
    int rate;
    bool enabled;
}
```

### struct vs class

| 维度 | `struct` | `class` |
|------|----------|---------|
| 分配位置 | 栈 | 堆（`new`） |
| 传递方式 | 值拷贝 | 引用（指针） |
| 字段访问 | 公开 | 私有 |
| 事件系统 | ❌ | ✅ 可参与 mapping |
| `@thread` | ❌ | ✅ |

---

## 8. 事件与 Mapping

### 事件声明

事件在 `event:` 段声明，无返回类型：

```myp
class Sensor {
    event:
        valueRead(float temp);
        thresholdExceeded(float value);
}
```

### Mapping 声明

Mapping 将事件连接到动作：

```myp
// 类型级映射（文件级全局）
mapping() {
    Sensor.valueRead -> Display.showTemperature;
}

// 实例级映射（函数内局部）
int main() {
    Sensor sensor;
    Display display;

    mapping() {
        sensor.valueRead -> display.showTemperature;
    }
}
```

### 事件链

```myp
mapping() {
    A.event -> B.process -> C.onResult;
}
// 语义：A 触发 event → B.process 被调用 → 返回值传入 C.onResult
```

### 多目标映射 (v2+)

```myp
mapping() {
    // 一个事件触发多个动作
    sensor.valueRead -> display.show, logger.log;

    // 等价于：
    sensor.valueRead -> display.show;
    sensor.valueRead -> logger.log;
}
```

### Mapping 语义

- 一个事件可映射到多个动作
- 多个事件可映射到同一个动作
- mapping 在运行时建立事件总线
- 同线程 = 同步处理，跨线程 = 异步投递

### 作用域管理 `@scope` (v2.3)

默认 mapping 永久有效。`@scope` 将 handler 生命周期绑定到函数作用域：

```myp
void run() {
    Sensor s;
    mapping() @scope {
        s.ready -> log.write;
    }
}  // 函数退出时 handler 自动解注册
```

### 条件过滤 `where` (v2.3)

只有满足条件的事件才会被转发：

```myp
mapping() {
    rs.valueEmitted where value >= 3 -> Console.write;
}
```

`where` 表达式可使用事件参数名，支持比较和算术运算。

### Lambda 变换节点 (v2.3)

在 mapping 链中用 lambda 做内联数据变换：

```myp
mapping() {
    rs.valueEmitted -> (int v) => { return v * 2; } -> display.show;
}
```

### 定时变换器 (v2.3)

- `delay(ms)` — 延迟转发
- `throttle(ms)` — 限频

```myp
mapping() {
    sensor.data -> delay(100) -> display.update;
    sensor.data -> throttle(50) -> logger.write;
}
```

---

## 9. 并发编程

### @thread 注解

```myp
int main() {
    // 不带 @thread：在当前线程运行
    Sensor sensor;

    // 带 @thread：在独立线程运行
    Worker worker @thread;

    mapping() {
        sensor.valueRead -> worker.process;
    }
    return 0;
}
```

### @threadpool

```myp
// 创建 4 个 Worker，每个在独立线程运行
Worker[4] pool @threadpool;

mapping() {
    sensor.valueRead -> pool[0].process;
}
```

### @parallel for — 数据并行

`@parallel for` 是 MYP 的数据并行原语，用于计算密集型循环的自动并行化。由编译器自动提取循环体到线程池执行，无需事件/消息传递：

```myp
import atomic;

int[1000] tally;
@parallel for (int i = 0; i < 1000; i = i + 1) {
    Atomic.addInt(tally, i, i);
}
```

#### 工作原理

```
编译器:
  1. 扫描外层作用域，收集被循环体引用的变量
  2. 构建捕获结构体，填充所有变量的当前值
  3. 提取循环体为独立函数 parallel_body(i, arg)
  4. 调用 myp_pool_parallel_for() 分发迭代

运行时:
  5. 16 线程 work-stealing 池平分迭代块
  6. 各线程串行执行各自迭代块
  7. barrier 等待全部完成 → 返回
```

#### 变量捕获

自动捕获外层变量到 struct，通过 `void* arg` 传递：

| 类型 | 方式 |
|------|------|
| `int`/`long`/`double` | 值捕获（线程独立拷贝） |
| `double[]`/`int[]` | 指针捕获（共享同一堆数组） |
| class 实例 | 指针捕获 |
| 静态方法调用 | 直接 LLVM 函数调用 |

#### 线程安全

必须使用 Atomic 操作保护共享数据写入：

```myp
@parallel for (int i = 0; i < size; i = i + 1) {
    // ✅ 正确
    Atomic.addDouble(tally, idx, value);
    // ❌ 错误：竞态条件
    // tally[idx] = tally[idx] + value;
}
```

#### 限制

- 循环变量用 `int`（`long` 自动截断为 int32）
- 每个迭代必须**无数据依赖**
- 不支持 `break` / `continue`
- 循环边界在进入时确定

#### BNCT 示例

```myp
class Transport {
    action:
        void runBatch(int batchId, int size) {
            double[] depthDose = TallyData.depthDose;
            @parallel for (int i = 0; i < size; i = i + 1) {
                long state = (batchId * size + i) * 152917L + 1L;
                double E = Physics.sampleEnergy(state);
                // ... 输运 ...
                Atomic.addDouble(depthDose, iz, energy);
            }
        }
}
```

#### 性能参考（16 核）

| 粒子数 | 时间 | 加速比 |
|--------|------|--------|
| 5M | ~3s | ~10x |
| 1e9 | ~9.5min | ~10x |

### 同步原语 stdlib（§五-2，v3.9.0，additive）

`import sync` 提供基于 pthread 的互斥锁/读写锁/条件变量/信号量/单次执行。
全部采用 **handle 模式**（同 `Barrier`）：`create` 返回 `int` 句柄，用后必须 `destroy`
释放（槽位有限，每类 64 个，destroy 后可复用）。

```myp
import sync;

// Mutex 互斥锁（普通 + 可重入）
int m = Mutex.create();
Mutex.lock(m);
try { ... } finally { Mutex.unlock(m); }   // 配合 finally 保证解锁
Mutex.destroy(m);

// RWLock 读写锁（多读单写）
int rw = RWLock.create();
RWLock.readLock(rw);   // 共享读
RWLock.writeLock(rw);  // 独占写
RWLock.unlock(rw);
RWLock.destroy(rw);

// CondVar 条件变量（必须持有关联 Mutex；while 循环惯例避免丢失唤醒）
int cv = CondVar.create();
Mutex.lock(m);
while (!ready) { CondVar.wait(cv, m); }   // 自动释放 m 并阻塞
CondVar.signal(cv);                        // 或 broadcast(cv)
Mutex.unlock(m);
CondVar.destroy(cv);

// Semaphore 信号量（P/V）
int s = Semaphore.create(2);   // 初始计数
Semaphore.wait(s);             // P：减一，为 0 阻塞
Semaphore.post(s);             // V：加一，唤醒等待者
Semaphore.destroy(s);

// Once 单次执行（多个线程竞争，仅首个执行初始化）
int once = Once.create();
if (Once.enter(once) == 1) { ...初始化...; Once.done(once); }
Once.destroy(once);
```

跨线程共享状态用 `@static class` 属性（全局变量）：

```myp
@static class Shared { property: int mutex; int count = 0; }

class Worker {
    action:
        @startup void run() {
            Mutex.lock(Shared.mutex);
            Shared.count = Shared.count + 1;   // 临界区
            Mutex.unlock(Shared.mutex);
        }
}
```

- **返回值**：`tryLock`/`tryWait`/`tryReadLock`/`tryWriteLock` 返回 `1`=成功、`0`=失败、
  `-1`=非法句柄；`Once.enter` 返回 `1`=本线程首个（执行初始化）、`0`=已完成。
- **约束**：句柄无自动回收（同 `Barrier`），显式 `destroy`；`CondVar.wait` 会释放并重新
  获取关联 Mutex，`while` 循环是标准防丢醒写法。

### @gpu for — GPU 卸载

`@gpu for` 是 MYP 的 GPU 并行原语，将计算密集型循环卸载到 NVIDIA CUDA GPU 执行：

```myp
import math;

long n = 1000000L;
double[] data = new double[n];
for (long i = 0L; i < n; i = i + 1L) data[i] = 1.0;

@gpu for (long i = 0L; i < n; i = i + 1L) {
    data[i] = Math.sqrt(data[i]) + Math.sin(1.0);
}
```

#### 工作原理

```
编译器:
  1. 生成 NVPTX 内核（myp_kernel），循环索引映射到 GPU 线程 id
  2. 收集被捕获的数组/标量变量，生成数据传输代码
  3. 使用 CUDA libdevice（libdevice.10.bc）链接数学函数
     → 生成的 PTX 完全自包含，无需运行时 JIT 链接
运行时:
  4. cuModuleLoadData 加载 PTX → 启动内核（grid/block 自动计算）
  5. 拷贝回数组结果 → 同步 → 完成
```

#### 启用与回退

- 需设置环境变量 `MYP_GPU=1`（默认使用 CPU）
- 需要 NVIDIA CUDA 驱动（`libcuda.so.1`）
- 无 GPU / 未设置 `MYP_GPU` 时**自动回退到 CPU 顺序执行**，结果一致
- 数学函数需 `libdevice.10.bc`（编译器自动查找；可用 `MYP_CUDA_LIBDEVICE` 指定路径）

#### GPU 数学函数

在 `@gpu for` 内核中，`Math` 的以下函数自动映射到 CUDA libdevice（GPU 上全精度执行）：

| 函数 | libdevice | 函数 | libdevice |
|------|-----------|------|-----------|
| `Math.sqrt` | `__nv_sqrt` | `Math.exp` | `__nv_exp` |
| `Math.sin` | `__nv_sin` | `Math.log` | `__nv_log` |
| `Math.cos` | `__nv_cos` | `Math.pow` | `__nv_pow` |
| `Math.tan` | `__nv_tan` | `Math.abs` | `__nv_fabs` |
| `Math.floor` | `__nv_floor` | `Math.ceil` | `__nv_ceil` |
| `Math.asin` | `__nv_asin` | `Math.acos` | `__nv_acos` |
| `Math.atan` | `__nv_atan` | `Math.atan2` | `__nv_atan2` |
| `Math.sinh` | `__nv_sinh` | `Math.cosh` | `__nv_cosh` |
| `Math.tanh` | `__nv_tanh` | | |

#### `import cuda` — CUDA 标准库

```myp
import cuda;
```

提供 GPU 编程的高层 API：

```myp
// Cuda — GPU 设备信息查询
int ok = Cuda.available();      // 1=GPU 可用，0=将使用 CPU
int n = Cuda.count();           // GPU 数量
string gpu = Cuda.name();       // GPU 名称（如 "NVIDIA GeForce RTX 2070 SUPER"）
long mem = Cuda.memory();       // 显存（字节）
int cc = Cuda.capability();     // 计算能力（如 705 = 7.5）
int sm = Cuda.multiProcessors();// 流式多处理器（SM）数量
int mt = Cuda.maxThreads();     // 每线程块最大线程数
int ws = Cuda.warpSize();       // 线程束大小（通常 32）

// Device — 内核内数学函数（GPU 全精度，CPU 用标准库）
// 支持：sqrt/abs/floor/ceil/trunc/sin/cos/tan/asin/acos/atan/atan2/
//       sinh/cosh/tanh/exp/log/pow（全部映射到 CUDA libdevice）
@gpu for (long i = 0L; i < n; i = i + 1L) {
    data[i] = Device.pow(data[i], 2.0) + Device.cos(0.0) + Device.atan2(1.0, 2.0);
}

// Vectors — 基于 @gpu for 的向量化运算（自动使用 GPU，不可用回退 CPU）
Vectors.add(a, b, out, n);       // out[i] = a[i] + b[i]
Vectors.sub(a, b, out, n);       // out[i] = a[i] - b[i]
Vectors.mul(a, b, out, n);       // out[i] = a[i] * b[i]
Vectors.scale(data, 2.0, n);     // data[i] *= 2.0
Vectors.addScalar(data, 1.0, n); // data[i] += 1.0
Vectors.fill(data, 0.0, n);      // data[i] = 0.0
Vectors.saxpy(3.0, x, y, out, n);// out[i] = 3.0*x[i] + y[i]
Vectors.copy(dst, src, n);       // dst[i] = src[i]
Vectors.negate(data, n);         // data[i] = -data[i]
Vectors.clamp(data, lo, hi, n);  // data[i] = clamp(data[i], lo, hi)
Vectors.pow(data, 2.0, n);       // data[i] = pow(data[i], 2.0)
Vectors.sqrt(data, n);           // data[i] = sqrt(data[i])
Vectors.sin(data, n);            // data[i] = sin(data[i])
Vectors.cos(data, n);            // data[i] = cos(data[i])
Vectors.tan(data, n);            // data[i] = tan(data[i])
Vectors.exp(data, n);            // data[i] = exp(data[i])
Vectors.log(data, n);            // data[i] = log(data[i])
Vectors.abs(data, n);            // data[i] = |data[i]|
Vectors.floor(data, n);          // data[i] = floor(data[i])
Vectors.ceil(data, n);           // data[i] = ceil(data[i])

// Vectors — 归约（sum/mean/variance/norm 用 GPU 原子累加）
double s = Vectors.sum(a, n);        // Σ a[i]
double m = Vectors.mean(a, n);       // 均值
double v = Vectors.variance(a, n);   // 方差（单遍）
double sd = Vectors.stddev(a, n);    // 标准差
double ns = Vectors.normSquared(a, n);// Σ a[i]^2
double no = Vectors.norm(a, n);      // sqrt(Σ a[i]^2)
Vectors.normalize(data, n);          // data[i] /= ||data||
double mn = Vectors.min(a, n);       // 最小值（CPU）
double mx = Vectors.max(a, n);       // 最大值（CPU）
double d = Vectors.dot(a, b, n);     // 内积（CPU）

// Matrix — 矩阵元素级运算（扁平行主序 double[]，大小 rows*cols）
Matrix.add(a, b, c, rows, cols);     // c = a + b（GPU）
Matrix.sub(a, b, c, rows, cols);     // c = a - b（GPU）
Matrix.mul(a, b, c, rows, cols);     // c = a .* b（GPU）
Matrix.scale(a, s, rows, cols);      // a *= s（GPU）
Matrix.fill(a, s, rows, cols);       // a = s（GPU）
Matrix.transpose(a, t, rows, cols);  // t = a^T（CPU）
```

#### 限制

- 循环变量用 `long`，边界 `i < n` 或 `i <= n`
- 捕获的数组在 GPU 内核启动前整体拷贝到设备（拷贝元素数 = 循环上界 n），运行后拷回
- 每个迭代必须**无数据依赖**
- 不支持 `break` / `continue`
- GPU 路径的数学函数需要 `libdevice.10.bc`
- `min`/`max`/`dot`/`transpose` 当前用 CPU 实现（正确但非 GPU 加速）

### 构造器（@constructor / 函数名==类名）

`new ClassName(args)` 创建实例时**自动调用构造器**进行对象初始化（设字段、分配资源、校验）。
构造器是 `action:`（或 `function:`）里加 `@constructor` 注解的方法；**当方法名与类名一致时，
默认视为构造器**（可省略注解，与 C++/Java 一致）：

```myp
class Window {
    action:
        @constructor
        Window() {                     // 显式 @constructor：无参构造（无返回类型）
            x = 0; y = 0; w = 80; h = 24;
        }
        void Window(int px, int py, int pw, int ph) {  // 函数名==类名 → 隐式构造器
            x = px; y = py; w = pw; h = ph;
        }
    property:
        int x;
        int y;
        int w;
        int h;
}

int main() {
    Window a = new Window();                // 无参构造
    Window b = new Window(10, 5, 100, 50);  // 重载构造
    return 0;
}
```

- **重载**：同名（=类名）不同参数即多个构造器，`new C(args)` 按实参类型匹配（数字可隐式提升
  `int → long → double`）；无匹配 / 歧义 → 编译报错。
- **执行顺序**：`new` 时 分配实例 → 应用 property 默认值 → 调用构造器体（可覆写默认值）。
- **泛型**：`new Box<double>(1.5)` 绑定单态化实例类的构造器，`T` 正确解析为 double。
- **struct**：函数式构造 `Vec2(1.0, 2.0)`——像调用函数一样创建栈上 struct 值。
- **深拷贝**：显式 `copy()` 约定方法（引用别名 `A b = a;` 不拷贝，见 `docs/constructor.md`）。

**构造器 ≠ `@startup`**：构造器管**初始化**（`new` 时同步执行）；`@startup` 管**开始操作**
（并行/事件驱动代码中实例的线程/事件循环启动时执行，见下节）。两者正交、互不取代。
设计详见 `docs/constructor.md`。

### @startup 注解

```myp
class Worker {
    action:
        @startup void run() {
            // 线程启动时自动执行
            Console.writeLine("worker started");
            taskCompleted(42);
        }
    event:
        taskCompleted(int result);
}
```

> **`@startup` 是启动信号，不是初始化器**：它在实例的线程/事件循环**开始操作**时执行
> （如 `@thread` 启动、启动定时器、触发首事件）。对象**初始化**（设字段/分配资源/校验）
> 走构造器（`@constructor` 注解或函数名==类名）——`new ClassName(args)` 时自动调用。
> 两者正交、互不取代；设计见 `docs/constructor.md`。

### 线程模型

```
主线程:  创建实例 → 注册 mapping → 事件循环
                                ↕ 异步
Worker 线程: @startup → 事件循环 → 处理事件 → 触发新事件
```

- 每线程独立事件队列（无锁竞争）
- 跨线程通信通过 `mapping()` 自动异步投递
- 无需显式加锁

---

## 10. 模块与导入

### 导入语法

```myp
import env;              // 标准库模块
import timeline;          // 标准库模块
import "./helper.myp";    // 用户文件（相对路径）
import "/abs/lib.myp";    // 用户文件（绝对路径）
```

### 导入规则

- 标准库在 `stdlib/` 目录查找
- 用户文件支持相对/绝对路径
- 自动去重（同一文件不会重复导入）
- 递归加载（导入的文件中的 `import` 也被加载）
- 搜索路径：`--stdlib` → 可执行文件 `../stdlib/` → 源文件 `./stdlib/` → `--package-path` 指定目录
- 包导入：`import mylib;` 会在包路径下查找 `mylib/src/mylib.myp` 或 `mylib/mylib.myp`

### 项目组织建议

```myp
// sensors.myp — 传感器组件
class TempSensor { /* ... */ }
class MotionSensor { /* ... */ }

// processing.myp — 处理逻辑
import "sensors.myp";
class DataProcessor { /* ... */ }

// main.myp — 顶层编排
import "processing.myp";
mapping() {
    TempSensor.valueRead -> DataProcessor.process;
}
```

---

## 11. 标准库

### `import env` — 控制台 I/O

```myp
import env;

Console.writeLine("hello");     // 输出字符串 + 换行
Console.writeString("text");    // 输出字符串（无换行）
Console.write(42);              // 输出整数
Console.writeLong(1234567890L); // 输出长整数
Console.writeFloat(3.14);       // 输出浮点数
Console.writeBool(true);        // 输出布尔值
Console.readString();           // 从 stdin 读一行
Console.kbhit();                // 非阻塞键盘检测
Console.getch();                // 非阻塞读一个字符
```

### `import option` — 可空容器（v3.x）

显式可空包装 `Option<T>`：`Option()`=none、`Option(T v)`=some。

```myp
import option;
Option<int> none = new Option<int>();
Option<int> some = new Option<int>(42);
int? maybe = new Option<int>(7);       // T? ≡ Option<T>（类型位置）
if (some.isSome()) {
    int v = some.get();                // 42
}
int safe = maybe.getOr(0);             // none → 默认值
some.set(9);
some.clear();                          // 变回 none
```

> 与元组/一等函数组合：`Option<R> mapOpt<T, R>(Option<T> o, (T) -> R f)`（见 §5 一等函数）。

### `import collections` — 集合类型

```myp
import collections;

// ArrayList<T> — 动态数组（惰性分配 + 自动扩容，无 1024 上限）
ArrayList<int> list = new ArrayList<int>();
list.add(10);
list.add(20);
int first = list.get(0);   // 10
list.set(1, 30);
int n = list.size();        // 2

// HashMap<K,V> — 哈希表（线性探测，自动扩容）
HashMap<int, string> map = new HashMap<int, string>();
map.put(1, "one");
map.put(2, "two");
string v = map.get(1, "?");  // "one"
bool has = map.contains(2);  // true
map.remove(1);

// Set<T> — 哈希集合（自动扩容）
Set<int> s = new Set<int>();
s.add(42);
s.add(17);
bool b = s.contains(42);     // true
s.remove(17);
int sz = s.size();
```

### `import math` — 数学函数

```myp
import math;

Math.sqrt(64.0);        // 8.0
Math.abs(-3.5);         // 3.5
Math.sin(3.14159);      // ~0
Math.cos(3.14159);      // ~-1
Math.pow(2.0, 10.0);    // 1024.0
Math.max(10, 20);       // 20
Math.min(10, 20);       // 10
Math.absInt(-42);       // 42
```

### `import time` — 时间与定时器

```myp
import time;

long t = Time.nowMs();        // 当前时间（毫秒）
Time.sleep(1000);             // 休眠 1 秒

// 定时器（与 mapping 配合使用）
// Timer(eventName, delayMs, intervalMs)
// intervalMs = 0 表示单次触发
```

### `import random` — 随机数

```myp
import random;

Random.init(12345);               // 设置种子
int r = Random.next();            // [0, RAND_MAX]
int d = Random.below(10);         // [0, 10)
double u = Random.uniform();      // [0.0, 1.0)
double g = Random.gaussian();     // 标准正态 N(0,1)（Box-Muller）
double rg = Random.range(5.0, 10.0);   // [lo, hi) 均匀分布
double e = Random.exponential(2.0);    // 指数分布（均值 1/lambda，逆变换采样）
int p = Random.poisson(3.0);           // 泊松分布整数（Knuth 算法）
Random.shuffle(arr, n);           // Fisher-Yates 洗牌
```

### `import rtti` — 运行时类型信息 / RTTI（§五-4，additive）

每个 class 实例的对象头 `{rc, type_id}` 携带运行时类型 id；codegen 生成
`__myp_type_name_table`（type_id → 类名字符串）。`Rtti` 静态类暴露类型查询接口：

```myp
import rtti;

Tank t = new Tank();
string n  = Rtti.typeOf<Tank>(t);            // "Tank"（运行时类名）
int    id = Rtti.typeId<Tank>(t);            // 运行时类型 id（同类恒定、跨类不同）
int    ok = Rtti.sameType<Tank, Tank>(t, t2); // 1（同类型）；跨类 → 0

Tank nul = null;
Rtti.typeId<Tank>(nul);                       // 0（null）
Rtti.typeOf<Tank>(nul);                       // ""（空串）
```

- 泛型 T/U 应传直接 class 引用（对象）；接口引用需先 `isa` 向下转型到具体类。
- 用途：日志 / 序列化 / 调试的类型身份查询；类型转换仍用 `isa`（§三-4）。

### `import fmt` — printf 风格格式化（§六-4，v3.9.0，additive）

补 `sprintf` 缺口：此前仅字符串插值 `${x}`，现提供宽度/精度/进制/填充控制。

```myp
import fmt;

Fmt.i(42)               // "42"          十进制（有符号）
Fmt.i(42, 6)            // "    42"      右对齐，空格填充
Fmt.i(42, 6, 48)        // "000042"      '0' 填充
Fmt.u(-1)               // "4294967295"  无符号十进制（位模式）
Fmt.x(255, 4)           // "00ff"        小写十六进制（无符号，默认 '0' 填充）
Fmt.X(255, 4)           // "00FF"        大写十六进制
Fmt.o(8)                // "10"          八进制
Fmt.b(5)                // "101"         二进制
Fmt.f(3.14159, 2)       // "3.14"        定点 %.2f
Fmt.f(123.456, 2, 10)   // "    123.46"  定点 + 宽度
Fmt.e(123.456, 2)       // "1.23e+02"    科学计数 %.2e
Fmt.g(0.0001, 4)        // "0.0001"      最短表示 %.4g
Fmt.s("hi", 5)          // "   hi"       字符串右对齐
Fmt.sR("hi", 5)         // "hi   "       字符串左对齐
```

默认参数签名：整数族 `(v, width = 0, pad = 空格/48)`，浮点族 `(v, precision = 6, width = 0, pad = 空格)`；`width <= 0` 不填充。

### `import crypto` — 校验和 / 哈希（§六-4，v3.9.0，additive）

补 `crypto/hash` 缺口。哈希核心在 C 运行时，MYP 侧为静态类封装。

```myp
import crypto;

string h = Crc32.crc32Hex("hello");   // 8 位小写十六进制（无符号显示）
int c = Crc32.crc32("hello");         // 原始 32 位值（最高位可能为 1 → int 为负）

Hash.md5("abc");     // 32 位小写十六进制
Hash.sha1("abc");    // 40 位小写十六进制
Hash.sha256("abc");  // 64 位小写十六进制
```

算法：CRC-32（IEEE 802.3）、MD5（RFC 1321）、SHA-1 / SHA-256（FIPS 180）。已用标准已知向量回归（含 56 字节跨块消息）。

### `import http` — HTTP 客户端（§六-4，v3.9.0，additive）

基于 `net.myp` 的 TCP 客户端实现 HTTP/1.1（仅 `http://`，无 TLS）。支持 GET/POST、
URL 解析、状态行/响应头解析、`Content-Length` 定长体、`Transfer-Encoding: chunked`
分块体、关闭定界体。

```myp
import http;

HttpResult r = Http.get("http://example.com/api?page=1");
if (r.isOk()) {
    string body = r.getBody();          // 响应体
    string ct  = r.header("Content-Type");   // 响应头（大小写不敏感）
}
int status = r.getStatus();             // 状态码（0 = 解析失败）
HttpResult p = Http.post("http://example.com/items", "{\"k\":1}");
```

网络层错误抛异常：连接失败抛 `NetError`（来自 `TcpClient.connect`）；非法 URL /
非 `http` scheme 抛 string 异常（`https` 需 TLS，暂不支持）。

### `import net` — TCP 套接字

```myp
import net;

// --- 服务器 ---
TcpServer srv = new TcpServer(8080);   // bind + listen
TcpClient cl = new TcpClient();
srv.accept(cl);                        // 阻塞接受连接（结果写入 cl）
string data = cl.recvLine();           // 同步读一行
cl.send("Hello!\n");
cl.close();
srv.close();

// --- 客户端 ---
TcpClient c = new TcpClient();
int ret = c.connect("example.com", 80);    // 成功 0；失败抛 NetError
c.sendLine("GET / HTTP/1.0");
string resp = c.recv(4096);
c.close();
```

- `TcpServer(port)` / `accept(client)` / `close()`
- `TcpClient.connect(host, port)` / `send(data)` / `sendLine(data)` / `recv(maxLen)` /
  `recvLine()` / `getFd()` / `close()`；连接失败抛 `NetError`（含 op/host/port）
- **异步**：`recvAsync` / `recvLineAsync` / `sendAsync`（见 `import async`，§五-5 P2）

### `import text` — 文本处理

```myp
import text;

StringBuilder sb = new StringBuilder();
sb.append("Hello");
sb.append(", World");
string result = sb.toString();  // "Hello, World"
```

### `import atomic` — 原子操作

```myp
import atomic;

// 数组原子操作（用于多线程安全累加）
int[100] counters;
double[50] values;

Atomic.addInt(counters, idx, 1);        // counter[idx] += 1
Atomic.subInt(counters, idx, 1);        // counter[idx] -= 1
Atomic.addDouble(values, idx, 3.14);    // values[idx] += 3.14
Atomic.xchgInt(counters, idx, 0);       // counter[idx] = 0（返回旧值）
Atomic.loadInt(counters, idx);           // 原子读取
Atomic.storeInt(counters, idx, 42);      // 原子写入
```

常与 `@parallel for` 配合使用，保护共享 Tally 数组的多线程写入。

### `import io` — 文件 I/O

```myp
import io;

File f = new File();
f.open("data.txt", "r");           // 打开文件读
bool has = f.hasNext();             // 是否有下一行
string line = f.readLine();         // 读一行
f.close();                          // 关闭文件

f.open("out.txt", "w");             // 打开文件写
f.write("hello");                   // 写字符串（无换行）
f.writeLine("world");               // 写字符串 + 换行

// 二进制 I/O（通过 __myp_io_* intrinsics）
int byte = __myp_io_read_byte();
int i32  = __myp_io_read_i32be();
__myp_io_write_byte(0xFF);
__myp_io_write_i32be(42);
__myp_io_write_double(3.14);
double d = __myp_io_read_double();
```

### `import stream` — 流式数据源

```myp
import stream;

// RangeStream: 整数范围迭代
RangeStream rs = new RangeStream(0, 10, 1);
while (rs.hasNext()) {
    int v = rs.next();
}

// IntStream / DoubleStream: 数组流式封装
int[] data = new int[5];
IntStream is = new IntStream(data, 5);
```

### `import barrier` — 屏障同步

```myp
import barrier;

int handle = Barrier.create(4);      // 创建屏障（等待 4 线程）
Barrier.wait(handle);                // 等待所有线程到达屏障
Barrier.destroy(handle);             // 销毁
```

### `import future` — 异步结果

```myp
import future;

int handle = Future.create();        // 创建 Future
Future.set(handle, 42);              // 设置结果（生产者）
int result = Future.get(handle);     // 获取结果（消费者，阻塞）
Future.destroy(handle);              // 销毁
```

### `import coro` — 协程

MYP 协程基于 ucontext 用户态纤程：`@coro` 注解的**类 action 方法**或**顶层函数** + `await` 挂起 +
`Coro.resume` 恢复（C1-C7 已实现）。用户通过静态类 `Coro` 访问调度/生命周期 API。

> `await` 只能在 `@coro` 方法或顶层 `@coro` 函数内使用；普通 action / `function:` /
> `static:` 段或普通顶层函数中的 `await` 会报编译错误
> `'await' is only allowed inside an '@coro' method`。

**声明协程方法**（`@coro`，可带参数，方法内可用 `await` 挂起；`@coro(stack=N)` 指定栈大小 KB，默认 128）：

```myp
import env;     // Console
import coro;    // 协程（静态类 Coro）

class Worker {
    property:
        string label_;
    action:
        void setLabel(string s) { label_ = s; }
        @coro void run() {                    // 协程方法（默认 128KB 栈）
            Console.writeString(label_); Console.writeString(":1\n");
            await;                            // 挂起，让出控制权
            Console.writeString(label_); Console.writeString(":2\n");
        }
        @coro(stack=2048) void heavy() {      // 深递归/大栈用：2MB 栈
            await;
        }
}

// 顶层 @coro 函数：无需类封装，调用即启动协程，返回 handle
@coro long worker(long n) {
    long x = Coro.yield(n * 2);     // 挂起并传出 n*2；恢复时 x = resume 传入值
    return x + 100;                 // 返回值经 Coro.result(h) 读取
}
```

**调用 = 启动协程**：`obj.meth(args)` 或顶层 `fn(args)` 返回 `long` handle（创建 + 首启到第一个 `await`）：

```myp
class Main {
    action:
        @startup void run() {
            Worker a = new Worker();  a.setLabel("A");
            long h = a.run();               // spawn，返回 handle
            Console.writeString("main\n");
            Coro.resume(h, 0);              // 恢复执行（从 await 处继续）
            Coro.destroy(h);                // 提前取消（可选）
        }
}
```

**C2 值传递 + 返回值**：

```myp
class Worker {
    action:
        @coro void echo(int n) {
            int v = await n * 2;            // 挂起传出 n*2；恢复后 v = resume 传入值
            Console.writeString("v="); Console.write(v); Console.writeString("\n");
        }
        @coro int compute() {
            await;
            return 42;                       // return 存入结果槽
        }
}

class Main {
    action:
        @startup void run() {
            Worker w = new Worker();
            long h = w.echo(5);
            long out = Coro.resume(h, 100); // 传 100 → v=100；out = 传出值 10
            long hc = w.compute();
            Coro.resume(hc, 0);
            int r = Coro.result(hc);        // 42
        }
}
```

**用户 API（静态类 `Coro`）**：

```myp
Coro.scheduler();                              // 自动调度：跑所有就绪协程各一步（C3）
long r  = Coro.resume(h, val);                 // 恢复并传入 val；返回协程传出值
long v  = Coro.yield(val);                     // 挂起并传出 val；恢复时返回传入值（= await expr）
long a  = Coro.isActive(h);                    // 是否仍活跃（1/0）
Coro.destroy(h);                               // 强杀（提前取消；不执行清理；销毁正在运行的协程不会释放其栈）
long r  = Coro.result(h);                      // 取协程返回值
long v  = Coro.waitEvent(eventId, val);        // 等待事件（= await ClassName.eventName）
long v  = Coro.waitEventTimeout(id, ms, val);  // 带超时事件等待：事件到达返回 val，超时返回 -1
long v  = Coro.waitAny(ids, count, ms, val);   // 多事件等待：返回触发的事件 id，超时返回 -1
long s  = Coro.sleep(ms);                      // §五-5 P1：挂起当前协程 ms 毫秒（不阻塞线程；非协程退化为同步 sleep）
long f  = Coro.waitFd(fd, rd, wr, ms);         // §五-5 P2：等待 fd 可读/可写；就绪返回 1，超时返回 -1（需 @coro 内调用）
long k  = Coro.waitAnyOf(spec, cnt, ms, val);  // §五-5 P4：混等 事件/定时器/fd，返回触发 spec 下标（见 `import async`）
long c  = Coro.current();                      // 当前正在执行的协程 handle（不在协程内 -1）
long n  = Coro.count();                        // 当前线程活跃协程数
long s  = Coro.status(h);                      // 状态：-1 无效 / 0 结束 / 1 就绪运行 / 2 等待事件
Coro.requestCancel(h);                         // 协作式取消：请求协程在 await/yield 后自行退出
long q  = Coro.cancelRequested();              // 当前协程是否被请求取消（协程内检查，1/0）
Coro.clearCancel();                            // 清除当前协程的取消请求
```

> **语言级超时语法**：`await Signal.go timeout 30;` 等价于 `Coro.waitEventTimeout(go, 30, 0)`，
> 返回 `-1` 表示超时，否则为 resume 传入值。
>
> **取消语义**：`Coro.destroy(h)` 是**强杀**（立即取消，不执行 `finally`/资源清理，与 Go 的
> 协作式取消不同）；若协程需清理，应使用协作式取消——外部 `Coro.requestCancel(h)` 设置标记，
> 协程在 `await`/`yield` 恢复后检查 `Coro.cancelRequested()` 为 1 时自行退出（可执行清理）。

> `__myp_coro_*` 是编译器内部实现（`Coro` 类为内建静态类，codegen 直接生成底层调用），
> **符号未注册**——用户代码调用会报 `undefined symbol`，无法直接使用。`stdlib/coro.myp`
> 无任何 FFI 声明，用户只能通过 `Coro` 静态类使用。

**C3 自动调度**：spawn 的协程自动加入就绪队列，`Coro.scheduler()` 每轮驱动所有
就绪协程各一步（先处理事件，再 round-robin 恢复），无需逐个手动 resume：

```myp
long h1 = a.run();
long h2 = b.run();
Coro.scheduler();   // 每轮所有就绪协程各前进一个 await
Coro.scheduler();
```

**C4 事件等待**：协程用 `await ClassName.eventName` 阻塞等待事件，事件 fire 派发后
自动重新就绪，由调度器驱动继续：

```myp
class Signal {
    action:
        void send() { go(); }        // 类 action 内裸名 fire 事件
    event:
        go();
}

@coro void waiter() {
    Console.writeString("waiting\n");
    await Signal.go;                 // 阻塞直到 go 事件
    Console.writeString("got go\n");
}
```

**协程 + 线程并用**：协程状态是**线程本地**的（绑定创建它的线程）。多个 `@thread`
线程可以各自独立创建/调度协程，互不干扰：

```myp
class Main {
    action:
        @startup void run() {
            // 本线程内跑协程
            Ping p = new Ping();
            long h = p.loop(3L);
            Worker w = new Worker() @thread;   // 启动另一线程（其 @startup 内也可跑协程）
            Coro.scheduler();
            Coro.scheduler();
            Coro.scheduler();
        }
}
```

> 线程内并发（协程）+ 线程间并行（@thread）组合；线程退出时其协程状态自动清理。

> **阻塞注意事项**：MYP 协程是协作式的——协程内调用阻塞操作（`Time.sleep`、阻塞 I/O、
> `Thread.join` 等）会**阻塞整个线程**（含同线程其他就绪协程）。需要等待时间用
> `await Timeline.timeout`（配 `Timeline.startTimeout(ms)`），等待外部条件用
> `await ClassName.eventName`；真正的阻塞工作（磁盘/网络/跨线程同步）应交给 `@thread` 线程。
> 详见 `coro.md` §10。

> 语义说明：`@coro` 方法/顶层 `@coro` 函数调用编译为 spawn（`create` + 参数槽 + `set_entry`
> + 首启 `resume`），返回 `long` handle；`await` 编译为 `__myp_coro_yield(val)`（`await expr`
> 是表达式，绑定完整操作数，恢复后其值 = `resume` 传入值）；`@coro` 代码 `return val` 存入
> per-协程结果槽，`Coro.result(h)` 读取。协程自然结束自动回收槽，进程退出统一释放栈。
> 顶层 `@coro` 函数入口包装无 `this` 槽（参数从槽 1 起），codegen 预扫描创建包装，
> 因此协程函数可定义在调用点之后。

**协程间通信**：
- **Channel**（`import channel`）：Go 风格有缓冲通道。`Channel ch = new Channel(); ch.init(n);`
  `ch.send(v)`（协程内缓冲满挂起）/ `ch.recv()`（协程内空挂起）/ `ch.trySend`/`ch.tryRecv`
  （非阻塞）/ `ch.size()`/`ch.close()`。非协程调用满/空返回 -1。
- **协程 await Future**（`import future`）：协程内 `Future.get(fh)` 未 ready 时挂起协程
  （不阻塞线程），同线程 `Future.set(fh, v)` 唤醒恢复。跨线程 set 不唤醒其他线程协程。

### `import async` — 异步 IO 统一抽象（§五-5，v3.9.0，additive）

把"等某件事完成"（定时器 / 套接字 / 文件 / 事件）统一到 `await` + 协程调度器（reactor
模型，对标 Rust tokio / Node 事件循环）。设计见 `docs/async_io.md`。

**`@async` 注解 + await 形态 3**：被 `@async` 标注的类 action / `static:` / 顶层函数，在
`@coro` 内用 `await f()` 调用时**直接执行**（函数内部经 park 原语挂起/恢复），`await` 的
值 = 函数返回值——不再走普通 `await` 的 yield-值握手。sema 限制：非 `@coro` 上下文调用
`@async` 函数报编译错误。

**异步睡眠（P1）**：
```myp
import coro;
import async;

@coro long worker() {
    await Async.sleep(100);      // 挂起当前协程 100ms，不阻塞线程
    Console.writeString("woke\n");
    return 0;
}
```
等价底层 API `Coro.sleep(ms)`（`Async.sleep` 即其封装；非协程上下文退化为同步 sleep）。

**异步套接字（P2，`import net`）**：fd 置 O_NONBLOCK，调度器每轮批量 `poll`，可读/可写才恢复协程：
```myp
@coro long client() {
    TcpClient c = new TcpClient();
    c.connect("127.0.0.1", 8080);
    string d1 = await c.recvAsync(4096);   // fd 可读后非阻塞读
    string l1 = await c.recvLineAsync();   // 逐字节读到 \n
    long n  = await c.sendAsync("hi");     // fd 可写后发送
    c.close();
    return 0;
}
```
带超时：`recvAsync(maxLen, timeoutMs)` 超时返回空串；`recvLineAsync(timeoutMs)` 超时返回已收内容；
`sendAsync(data, timeoutMs)` 超时返回已发字节。

**异步文件（P3，`import io`）**：阻塞 fgets/read-all 在 worker 线程池执行，完成后跨线程投递
结果并恢复协程（`readAllAsync` 返回余下内容，含末尾换行）：
```myp
@coro long reader() {
    File f = new File();
    f.open("/tmp/x.txt", "r");
    string line = await f.readLineAsync();   // worker 线程读一行
    string all  = await f.readAllAsync();    // worker 线程读全量
    f.close();
    return 0;
}
```

**统一 waitAnyOf（P4，`Coro.waitAnyOf`）**：一次等待混搭 事件/定时器/fd，返回最先触发的 spec
下标。`spec` 为扁平 `long[]`，每 3 个元素一个等待项 `[kind, id, flag]`：`kind` 0=EVENT
（id=事件 id，flag=0）、1=TIMER（id=-1，flag=相对毫秒）、2=FD（id=fd，flag=1/2/3 可读/可写/读写）。
返回：触发 spec 下标（0 起），总体超时 -1，非协程上下文 -2：
```myp
@coro long waitMix(int fd) {
    long[] spec = new long[9];
    spec[0] = 0; spec[1] = 0;  spec[2] = 0;     // EVENT 事件 id 0
    spec[3] = 2; spec[4] = fd; spec[5] = 1;     // FD 可读
    spec[6] = 1; spec[7] = -1; spec[8] = 300;   // TIMER 300ms
    return Coro.waitAnyOf(spec, 3, 1000, 0);    // 返回最先触发的下标
}
```

> **机制**：等待表统一为 `myp_coro_wait_t.kind`——0=EVENT / 1=TIMER / 2=FD / 3=EXEC。调度器
> 每轮顺序：处理事件 → 过期 deadline → 批量 poll fd → 执行器收件箱 → 就绪快照。全部
> additive，与既有 `await ClassName.eventName` / `Coro.waitEvent*` 并存。

### `import pool` — 并行计算工具

```myp
import pool;

// Parallel 静态类 —— 线程池查询 / 配置 API
// 语言级并行接口是 @parallel for（自动使用全局 work-stealing 线程池）

int cpus = Parallel.threadCount();      // 硬件并发线程数（sysconf）
Parallel.setThreads(4);                 // 首次 @parallel for 前指定池大小（0=自动）
@parallel for (int i = 0; i < 100; i = i + 1) {
    int wid = Parallel.workerId();      // 当前 worker 索引（0..N-1）
    // ...
}
int nw = Parallel.workerCount();        // 池实际 worker 线程数（0 = 未初始化）
int on = Parallel.isActive();           // 池是否已初始化（1=是 0=否）
```

| 方法 | 说明 |
|------|------|
| `threadCount()` | 硬件并发线程数——线程池默认大小 |
| `workerCount()` | 全局池实际 worker 线程数（首次 `@parallel for` 后可用；0 = 未初始化） |
| `workerId()` | 当前执行线程的池 worker 索引（`@parallel for` body 内为 0..N-1；非池线程为 -1） |
| `isActive()` | 线程池是否已初始化（1=是 0=否） |
| `setThreads(n)` | 设置线程池大小（0=自动=硬件并发数；仅在首次创建前生效，之后为 no-op） |

底层基于 work-stealing 线程池，配合 `Atomic` 使用。

### `import test` — 测试断言

```myp
import test;

Test.assert(1 == 1);                 // 断言条件为真
Test.assertEq(2 + 2, 4);             // 断言相等（int）
Test.assertStrEq("hi", "hi");        // 断言字符串相等
Test.report("test_name", true);      // 报告测试结果
```

### `import memory` — 动态内存管理

```myp
import memory;

// C 标准库 malloc/free/realloc 桥接（指针以 long 承载，同 json/regex 的 handle）
long p = Memory.alloc(1024);            // 分配（返回指针）
Memory.free(p);                         // 释放
p = Memory.realloc(p, 2048);            // 重新分配
Memory.release(p);                      // free 的别名
```

> **使用场景**：① **确定性释放**——arena 分配的 `new T[n]` 要等进程退出/@region 结束
> 才回收，生命周期明确的临时缓冲可用 `Memory` 手动即时释放（控制峰值内存）；
> ② **FFI 指针互操作**——传给 C 库（SDL/net/GPU/第三方）的裸指针；
> ③ **字节缓冲/手动布局**——二进制协议、文件格式的原始缓冲区。
> 动态数组请用 `collections` 的 `ArrayList<T>`（自动扩容），本模块只负责裸内存。

### `import channel` — 协程通道

Go 风格有缓冲通道（属于创建它的线程 TLS，与协程模型一致）：
```myp
import channel;
Channel ch = new Channel();
ch.init(4);                    // 缓冲容量 > 0
ch.send(v);                    // 协程内缓冲满 → 挂起等空位；非协程满返回 -1
long v = ch.recv();            // 协程内缓冲空 → 挂起等数据；非协程空返回 -1
ch.trySend(v);  ch.tryRecv();  // 永远非阻塞（0/值，满/空 -1）
ch.size();                     // 当前缓冲元素数
ch.close();                    // 关闭并唤醒等待者
ch.destroy();                  // 释放缓冲
```

### `import fs` — 文件系统

```myp
import fs;
Fs.exists("/tmp");         Fs.isDir("/tmp");        Fs.isFile("/tmp/x");
long sz = Fs.fileSize("/tmp/x");
long mt = Fs.modifiedTime("/tmp/x");
Fs.dirname(p);   Fs.basename(p);   Fs.join(dir, file);
string[] files = Fs.listDir("/tmp");      // 文件名数组（动态分配，无上限）
Fs.listCount("/tmp");
Fs.mkdirP("/a/b/c");                      // 递归建目录（mkdir -p）
Fs.removeRecursive("/a");                 // 递归删除（rm -rf）

// Path 封装：面向路径的方法
Path p = new Path("/home/user/file.txt");
p.dirname();  p.basename();  p.join("x.txt");
p.exists();  p.isDir();  p.isFile();  p.fileSize();  p.modifiedTime();
p.listDir();  p.toString();
```

### `import process` — 进程管理

```myp
import process;
int code = Process.run("ls -l");          // 执行命令并返回退出码
string out = Process.output("uname -a");  // 执行并捕获标准输出
int pid  = Process.getPid();              // 当前进程 PID
int ppid = Process.getParentPid();        // 父进程 PID
int alive = Process.isRunning(pid);       // 进程是否在运行
```

### `import args` — 命令行参数

```myp
import args;
int n = Args.count();                   // 参数个数（含程序名）
string a = Args.get(i);                 // 第 i 个参数（0 = 程序名）
Args.hasOption("-v");                  // 是否含某选项
string v = Args.getOption("-o", "def");// 取选项值（无则返回默认值）
```

### `import json` — JSON 解析

```myp
import json;
Json doc = new Json("{\"name\":\"myp\",\"v\":1}");  // 解析失败抛 JsonError
string n = doc.getString("name");
int v    = doc.getInt("v");
double d = doc.getDouble("pi");
int b    = doc.getBool("ok");
int len  = doc.arrayLen("items");      // 数组长度
int t    = doc.type("field");          // 值类型
string g = doc.getString("a.b[0]");    // path 支持嵌套字段/下标

doc.free();
```

### `import regex` — 正则表达式

```myp
import regex;
Regex re = new Regex("^[A-Z][a-z]+$");   // POSIX 扩展正则
re.test("Hello");   // 1
re.test("hello");   // 0
re.free();
```

### `import base64` — Base64

```myp
import base64;
string enc = Base64.encode("hello");
string dec = Base64.decode(enc);
```

### `import date` — 日期时间

```myp
import date;
long ms  = Date.nowMs();                 // wall-clock 毫秒
string s = Date.now();                   // "YYYY-MM-DD HH:MM:SS"
string f = Date.formatNow("%Y-%m-%d");  // 自定义格式当前时间
string t = Date.format(ms, "%H:%M:%S"); // 自定义格式指定时间
int y = Date.getYear();  Date.getMonth();  Date.getDay();
int h = Date.getHour();  Date.getMinute(); Date.getSecond();
Date.getWeekday();  Date.getDayOfYear();
Date.getYearOf(ms);  Date.getMonthOf(ms); Date.getSecondOf(ms);  // 指定时间取字段
```

### `import logger` — 日志

```myp
import logger;
Logger log = new Logger("myapp");
log.debug("...");  log.info("...");  log.warn("...");  log.error("...");
log.setLevel(0);   // 0=DEBUG 1=INFO(默认) 2=WARN 3=ERROR（LogLevel 枚举）
log.getLevel();
```

### `import timeline` — 时间线

```myp
import timeline;
Timeline tl = new Timeline();
long now = tl.now();  tl.sleep(100);
tl.startTimeout(1000);   // 1 秒后 timeout(ms) 事件触发
 tl.startInterval(500);  // 每 500ms interval(ms) 事件触发
tl.startTick(200);       // 每 200ms tick() 事件触发
// 事件：timeout(long ms) / interval(long ms) / tick()
Stopwatch sw = new Stopwatch();  sw.start();  ...  sw.elapsed();
```

### `import sdl` — SDL 图形窗口

```myp
import sdl;

// 基于 SDL2 的窗口和输入管理
SDL.init("Title", 800, 600);            // 创建窗口
while (!SDL.shouldClose()) {
    SDL.clear(0, 0, 0, 255);            // 清屏
    // ... 绘制 ...
    SDL.present();                       // 刷新
}
SDL.quit();

int key = SDL.getKey();                  // 获取按键
```

### `import ui` — 终端 TUI 框架

```myp
import ui;

// 纯 MYP 实现，基于 ANSI escape codes 渲染
Window win = new Window(0, 0, 80, 24, "MyApp");
win.add(new Button(10, 5, 12, 3, "Click"));
win.add(new ProgressBar(10, 10, 40, 3, 0.5));
win.render();                            // 渲染一帧

// 支持的组件: Window, Label, Button, TextBox, ProgressBar
```

---

## 12. 编译与工具

### 编译器

```bash
# 编译并运行
./build/mypc myapp.myp && ./myapp.out

# 一步编译 + 运行（仿 go run；单类文件无 main 也可，须类带 @startup）
./build/mypc run myapp.myp
./build/mypc run myapp.myp arg1 arg2     # 透传程序参数

# 指定输出
./build/mypc myapp.myp -o /tmp/myapp

# 事件追踪
./build/mypc --trace myapp.myp
./myapp.out 2>trace.log

# 指定包路径
./build/mypc --package-path myp_packages myapp.myp
```

#### `mypc run`（v3.9.0，仿 `go run`）

`mypc run file.myp [args...]` 编译 → 链接到临时二进制 → 直接运行 → 清理，一步到位：
- **透传参数**：`args...` 传给程序（`main(argc, argv)` / 构造器 `(int argc, string[] argv)`）。
- **单类文件自动 `main`**：文件**无 `main`** 时，若恰好一个类带 `@startup` 注解，
  编译器自动生成 `int main() { ClassName c = new ClassName(); c.startupAction(); return 0; }`
  并触发其 `@startup` 入口。
- **约束**：无 `@startup` 类 / 多个 `@startup` 类 → 编译报错（提示定义 `main()`）。
- **正常编译不受影响**：非 run 模式仍要求显式 `main`（链接期报错）。
- 临时产物自动清理，退出码 = 程序退出码。

```myp
// hello.myp — 无 main，run 时自动补
import env;
class Hello {
    action:
        @startup void go() {
            Console.writeString("Hello from @startup!\n");
        }
}
```
```bash
./build/mypc run hello.myp     # 输出: Hello from @startup!
```

#### 完整命令行选项

| 选项 | 说明 |
|---|---|
| `-o <file>` | 指定输出文件名 |
| `-O0` / `-O1` / `-O2` / `-O3` | 优化级别（默认 `-O0`；`-O2` 运行 IR 优化管线）|
| `-g`, `--debug` | 生成 DWARF 调试信息（断点/行号/变量）|
| `--passes <p>` | 运行自定义 MYP pass（如 `myp-pass`）|
| `--emit-llvm` | 输出 LLVM IR 到 `.ll` 文件（跳过链接）|
| `--test` | 生成并运行测试运行器（`@test`）|
| `--shared` / `--static` | 构建共享库 / 静态库 |
| `--trace` | 启用运行时事件追踪 |
| `--package-path <dir>` | 本地包目录 |
| `--macro-expand` | 宏展开后输出 AST dump |
| `--stdlib <path>` | stdlib 目录 |
| `--version` / `--help` | 版本 / 帮助 |

多文件编译（`mypc a.myp b.myp`）合并为单模块，天然享受跨文件优化（无需 LTO）。

#### 优化（`-O` 与自定义 pass）

```bash
./build/mypc -O2 myapp.myp        # IR 优化管线（mem2reg/GVN/内联/循环...）
./build/mypc -O0 myapp.myp        # 默认：快速编译、调试友好
./build/mypc --passes myp-pass -O0 myapp.myp   # 追加自定义 MYP pass
```

- `-O1/-O2/-O3` 运行 LLVM 标准优化管线（默认 `-O0` 不优化）。
- `--passes myp-pass` 运行 MYP 专用 pass（消除编译器生成的死 store）。
- 设计与实现见 `docs/optimization_debugging.md`。

#### 调试（`-g` DWARF + gdb）

```bash
./build/mypc -g myapp.myp          # 或 --debug；推荐 -g -O0
gdb ./myapp.out
(gdb) break myapp.myp:10           # 按源文件行号下断点
(gdb) run
(gdb) print 变量名                  # 查看参数/局部变量
(gdb) next / step / continue
```

- `-g` 生成 DWARF：函数断点、源码行号、参数/局部变量、类型信息。
- 类方法显示为 `Class_method` 符号；协程内调试为已知限制。
- 设计见 `docs/optimization_debugging.md`（Part B）。

#### 调试（VS Code DAP）

MYP 提供 `myp_debug`（DAP ↔ gdb 桥），可在 VS Code 内断点/单步/查变量：

```bash
# 编译带调试信息的可执行文件
./build/mypc -g myapp.myp

# 直接运行 DAP 服务器（供 VS Code / 任何 DAP 客户端调用）
./build/myp_debug
```

**VS Code**：安装 `vscode-myp` 扩展后，`.vscode/launch.json`：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "MYP Launch",
      "type": "myp",
      "request": "launch",
      "program": "${workspaceFolder}/myapp.out"
    }
  ]
}
```

- `program` 指向 `-g` 编译出的可执行文件。
- 设置 `myp.debuggerPath` 可指定 `myp_debug` 路径（默认自动探测）。
- 支持：断点（源码行号）、单步（next/stepIn/stepOut）、调用栈、局部变量、
  鼠标悬停求值（evaluate）。

#### 元编程（`@eval` / `macro` / `@macro`）

MYP 提供三层元编程（设计见 `docs/metaprogramming.md`）：

**1. `@eval` 编译期求值（纯函数）**

```myp
@eval int fib(int n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}
const int FIB10 = fib(10);   // 编译期算得 55（ret i32 55）
```

**2. `macro` 声明式宏（AST 模板）**

```myp
macro repeat($n, $body) {
    for (int _i = 0; _i < $n; _i++) { $body }
}
repeat(3, total = total + 10);   // 展开为 for 循环 ×3
```

**3. `@macro` 过程宏（`quote` 代码模板，可编程生成）**

```myp
@macro StmtList makeCalls(int n) {
    StmtList out = quote {};
    for (int i = 0; i < n; i++) {
        out = out + quote { Console.write($i); };
    }
    return out;
}
makeCalls(3);                    // 生成 3 条 Console.write(...)
```

- 调试：`--macro-expand` 输出展开后的 AST dump。

### 测试框架

```bash
# 编译并生成测试运行器
./build/mypc --test mytests.myp && ./a.out
# 输出:
# === MYP Test Runner ===
#   RUN: test_math
#   PASS: test_math
# === MYP Tests Complete ===
```

使用 `@test` 注解标记测试函数和 action，配合 `Test` 类使用断言：

```myp
import test;

@test void test_example() {
    Test.assert(1 == 1);
    Test.assertEq(2 + 2, 4);
    Test.assertStrEq("hello", "hello");
    Test.report("test_example", true);
}
```

### 代码格式化

```bash
# 格式化文件（原地修改）
./build/mypc fmt source.myp

# 仅检查格式
./build/mypc fmt --check source.myp

# 独立格式化工具
./build/myp_fmt [--check|--stdout] source.myp
```

### myp — 包管理工具

`myp` 是 MYP 的包管理命令行工具，提供项目初始化、构建和依赖管理：

```bash
# 创建新包
myp init mylib
# 输出：
#   mylib/package.myp
#   mylib/src/mylib.myp

# 构建当前包
cd myapp
myp build

# 安装依赖
myp install /path/to/mylib
# → 复制到 myp_packages/mylib/

# 构建并运行
myp run
```

### 包结构

```
mypackage/              # 包根目录
├── package.myp          # 包元数据
│   name: mypackage
│   version: 0.1.0
│   depends: other_lib
├── src/
│   └── mypackage.myp    # 主源文件
└── myp_packages/        # 安装的依赖（自动管理）
    └── other_lib/
        ├── package.myp
        └── src/
            └── other_lib.myp
```

### 项目结构

```
MYPLanguage/
├── tools/               # 自举 MYP 工具：tools/pm（包管理器→build/myp）、tools/fmt、tools/viz
├── CMakeLists.txt
├── include/mylang/      # 编译器头文件：AST/CodeGen/Sema/Parser/Lexer/Eval/
│   └── ...              #   Macro/MypPasses/Fmt/LSP/Token/Type/...
├── src/                 # 编译器源码
│   ├── main.cpp         # mypc 驱动（lexer→parser→sema→codegen→link）
│   ├── ast/ lexer/ parser/ sema/ codegen/ runtime/
│   ├── eval/            # @eval 编译期求值
│   ├── macro/           # 宏展开
│   ├── fmt/             # 格式化器
│   ├── lsp/             # 语言服务器（myp_lsp）
│   └── dap/             # 调试适配器（myp_debug，DAP↔gdb 桥）
├── stdlib/              # 标准库（纯 MYP 类）
│   ├── env/io/fs/text/stream/math/random/time/timeline
│   ├── collections/setops/atomic/pool/barrier/future/memory
│   ├── coro/channel/net/json/regex/base64/date/process/args
│   ├── logger/sdl/ui/error/cuda
│   └── test
├── tests/               # run_tests.sh / run_tests_O2.sh / run_tests_asan.sh /
│   └── ...              #   run_tests_tsan.sh / test_debug.sh / test_dap.py /
│                        #   expected/ / negative/ / <feature>/
├── examples/            # 完整示例（hello/fib/ad/BNCT/sdl/tui）
├── BNCTDoseEngine/      # BNCT 蒙特卡洛引擎（纯 MYP + HDF5 截面）
├── deeplearning/        # MLP + MNIST 训练/推理
├── vscode-myp/          # VS Code 扩展（语法高亮 + LSP + DAP）
├── docs/                # design/grammar/manual/manual_en/coro/exceptions/...
├── build/               # 构建产物：mypc, myp_debug, myp_lsp, myp_viz, myp_fmt
└── build-asan/          # ASAN/UBSAN 构建
```

### 环境变量

```bash
export MYP_PACKAGE_PATH=/path/to/packages:/path/to/more
```

### myp_viz — 可视化工具

```bash
# 生成 DOT 图
./build/myp_viz myapp.myp > graph.dot

# 渲染为 PNG（需要 graphviz）
dot -Tpng graph.dot -o graph.png

# 一步生成
./build/myp_viz myapp.myp | dot -Tpng -o graph.png
```

### myp_lsp — 语言服务器

`myp_lsp` 是 MYP 的 LSP（Language Server Protocol）实现，为编辑器提供智能编辑功能：

```bash
# 启动语言服务器（供编辑器调用）
./build/myp_lsp --stdlib ./stdlib

# 调试：输出 LSP 通信日志
./build/myp_lsp --stdlib ./stdlib 2>lsp.log
```

**编辑器支持**：

| LSP 能力 | 说明 |
|----------|------|
| 实时诊断 | 打开/编辑文件时自动显示编译错误 |
| 代码补全 | 关键字、类名、方法名、属性名自动弹出 |
| 悬停信息 | 鼠标悬停显示类型签名 |
| 文档符号 | 大纲视图显示类、函数、枚举 |
| 跳转定义 | 按住 Ctrl 点击跳转到定义 |

### VS Code 扩展

MYP 提供了 VS Code 扩展（`vscode-myp/`），安装后即获得语法高亮和 LSP 智能编辑：

```bash
# 方法 1：复制到扩展目录
cp -r vscode-myp ~/.vscode/extensions/myp-lang.vscode-myp
# 重启 VS Code 后生效

# 方法 2：打包安装（需要 @vscode/vsce）
cd vscode-myp
npm install
npx vsce package
code --install-extension vscode-myp-*.vsix
```

扩展配置（VS Code 设置中搜索 `myp`）：

| 设置 | 说明 |
|------|------|
| `myp.lspPath` | `myp_lsp` 路径（默认自动查找） |
| `myp.stdlibPath` | 标准库路径（默认自动查找） |
| `myp.trace.server` | LSP 通信日志级别 |

## 13. 完整示例

### IoT 温度监控系统

```myp
// ===== iot_monitor.myp =====
import env;
import timeline;
import math;

// === 传感器组件 ===
class TempSensor {
    action:
        @startup void run() {
            // 每 2 秒读取一次温度
            t.startInterval(2000);
        }
        double readValue() {
            // 模拟温度读数
            return 20.0 + Math.sin(Timeline.now() / 1000.0) * 5.0;
        }
    event:
        temperatureRead(double value);
    property:
        Timeline t;
}

// === 告警组件 ===
class Alarm {
    action:
        void check(double v) {
            if (v > 25.0 && !alarming) {
                alarming = true;
                alarmTriggered(v);
            } else if (v <= 25.0) {
                alarming = false;
            }
        }
        void sound() {
            Console.writeLine("🚨 ALARM! Temperature too high!");
        }
    event:
        alarmTriggered(double value);
    property:
        bool alarming;
}

// === 日志组件 ===
class Logger {
    action:
        void log(double v) {
            Console.writeString("[LOG] Temp: ");
            Console.writeFloat(v);
            Console.writeLine("°C");
        }
}

// === 显示组件 ===
class Display {
    action:
        void show(double v) {
            Console.writeString("Current: ");
            Console.writeFloat(v);
            Console.writeLine("°C");
        }
        void showAlert(double v) {
            Console.writeString("⚠️ High: ");
            Console.writeFloat(v);
            Console.writeLine("°C");
        }
}

// === 系统架构 ===
int main() {
    TempSensor sensor = new TempSensor();
    Alarm alarm = new Alarm();
    Logger logger = new Logger();
    Display display = new Display();

    sensor.t = new Timeline();

    mapping() {
        // 温度读取 → 显示和日志
        sensor.temperatureRead -> display.show, logger.log;

        // 温度读取 → 告警检测
        sensor.temperatureRead -> alarm.check;

        // 告警触发 → 声音 + 醒目显示
        alarm.alarmTriggered -> alarm.sound, display.showAlert;
    }

    return 0;
}
```

---

## 附录：MYP 设计哲学

### 事件驱动组件的核心原则

1. **组件间只通过事件通信** — 没有直接方法调用，没有共享状态
2. **架构即代码** — 读 `mapping()` 就能理解系统架构
3. **Actor 式并发** — `@thread` 实例天然隔离，无需加锁
4. **声明式组装** — 新增/移除/替换组件只改一行 mapping

### 什么时候用哪种？

| 构造 | 用途 |
|------|------|
| `class` + `action:` + `event:` | 事件驱动组件（主要的架构单元） |
| `class` + `function:` | 组件内部辅助逻辑 |
| `class` + `static:` | 工具函数命名空间（如 Math） |
| `@constructor` 注解 / 函数名==类名 | 对象初始化（`new` 时自动调用） |
| `struct` | 纯数据容器（值传递） |
| 顶层 `function` | 纯计算函数 |
| `action:` + `@startup` | 启动信号/开始操作（线程/事件循环启动时执行） |
| `@thread` | 需要独立线程的组件 |
