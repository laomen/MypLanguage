# MYP 编程手册

> 版本 2.4 | 事件驱动组件语言
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
| `byte` | 有符号 8-bit | 8 |
| `short` | 有符号 16-bit | 16 |
| `int` | 有符号 32-bit | 32 |
| `long` | 有符号 64-bit | 64 |
| `ubyte` | 无符号 8-bit | 8 |
| `ushort` | 无符号 16-bit | 16 |
| `uint` | 无符号 32-bit | 32 |
| `ulong` | 无符号 64-bit | 64 |
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

### 复合类型

```myp
int[] arr;              // 数组
string[] names;         // 字符串数组
int[10] fixed;          // 定长数组
ClassName obj;          // 类类型 (指针)
ClassName::StructType;  // 嵌套 struct 类型
```

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

### `import collections` — 集合类型

```myp
import collections;

// ArrayList<T> — 动态数组（容量 1024）
ArrayList<int> list = new ArrayList<int>();
list.add(10);
list.add(20);
int first = list.get(0);   // 10
list.set(1, 30);
int n = list.size();        // 2

// HashMap<K,V> — 哈希表（容量 1024，线性探测）
HashMap<int, string> map = new HashMap<int, string>();
map.put(1, "one");
map.put(2, "two");
string v = map.get(1, "?");  // "one"
bool has = map.contains(2);  // true
map.remove(1);

// Set<T> — 哈希集合（容量 1024）
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

Random.init(12345);           // 设置种子
int r = Random.next();         // [0, RAND_MAX]
int d = Random.below(10);      // [0, 10)
```

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

MYP 协程基于 ucontext 用户态纤程：`@coro` 注解的**类 action 方法** + `await` 挂起 +
`__myp_coro_resume` 恢复（C1/C2 已实现）。

**声明协程方法**（`@coro`，可带参数，方法内可用 `await` 挂起）：

```myp
import env;     // Console
import coro;    // 协程 FFI

class Worker {
    property:
        string label_;
    action:
        void setLabel(string s) { label_ = s; }
        @coro void run() {                    // 协程方法
            Console.writeString(label_); Console.writeString(":1\n");
            await;                            // 挂起，让出控制权
            Console.writeString(label_); Console.writeString(":2\n");
        }
}
```

**调用 = 启动协程**：`obj.meth(args)` 返回 `long` handle（创建 + 首启到第一个 `await`）：

```myp
class Main {
    action:
        @startup void run() {
            Worker a = new Worker();  a.setLabel("A");
            long h = a.run();               // spawn，返回 handle
            Console.writeString("main\n");
            __myp_coro_resume(h, 0);        // 恢复执行（从 await 处继续）
            __myp_coro_destroy(h);          // 提前取消（可选）
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
            long out = __myp_coro_resume(h, 100);   // 传 100 → v=100；out = 传出值 10
            long hc = w.compute();
            __myp_coro_resume(hc, 0);
            int r = __myp_coro_result(hc);          // 42
        }
}
```

**FFI 原语**（`stdlib/coro.myp`）：

```myp
long h  = __myp_coro_create();                       // 创建协程（编译器内部用）
__myp_coro_set_entry(h, fn_ptr);                     // 设置入口（编译器内部用）
long v  = __myp_coro_yield(val);                     // 挂起并传出 val；恢复时返回传入值
long r  = __myp_coro_resume(h, val);                 // 恢复并传入 val；返回协程传出值
long a  = __myp_coro_is_active(h);                   // 是否仍活跃（1/0）
__myp_coro_destroy(h);                               // 销毁（提前取消）
__myp_coro_set_entry_arg(idx, val);                  // 入口参数槽（编译器内部用）
long v  = __myp_coro_get_entry_arg(idx);             // 读取入口参数槽
__myp_coro_set_result(val);                          // @coro 方法 return 存返回值（内部用）
long r  = __myp_coro_result(h);                      // 取协程返回值
__myp_coro_scheduler();                              // 自动调度：跑所有就绪协程各一步（C3）
long v  = __myp_coro_wait_event(eventId, val);       // 等待事件（C4，await event 展开）
```

**C3 自动调度**：spawn 的协程自动加入就绪队列，`__myp_coro_scheduler()` 每轮驱动所有
就绪协程各一步（先处理事件，再 round-robin 恢复），无需逐个手动 resume：

```myp
long h1 = a.run();
long h2 = b.run();
__myp_coro_scheduler();   // 每轮所有就绪协程各前进一个 await
__myp_coro_scheduler();
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

> 语义说明：`@coro` 方法调用编译为 spawn（`create` + 参数槽 + `set_entry` + 首启 `resume`），
> 返回 `long` handle；`await` 编译为 `__myp_coro_yield(val)`（`await expr` 是表达式，
> 绑定完整操作数，恢复后其值 = `resume` 传入值）；`@coro` 方法 `return val` 存入 per-协程
> 结果槽，`__myp_coro_result(h)` 读取。协程自然结束自动回收槽，进程退出统一释放栈。

### `import pool` — 并行计算工具

```myp
import pool;

// Parallel 静态类提供简单的线程池任务分发
// 配合 @parallel for 和 Atomic 使用
// 底层基于 work-stealing 线程池
```

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

// 直接调用 C 标准库 malloc/free/realloc
// 通常不需要手动管理——MYP 有 ARC 自动回收
ptr = Memory.alloc(1024);            // 分配
Memory.free(ptr);                    // 释放
ptr = Memory.realloc(ptr, 2048);     // 重新分配
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

# 指定输出
./build/mypc myapp.myp -o /tmp/myapp

# 优化
./build/mypc -O2 myapp.myp

# 事件追踪
./build/mypc --trace myapp.myp
./myapp.out 2>trace.log

# 指定包路径
./build/mypc --package-path myp_packages myapp.myp
```

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
| `struct` | 纯数据容器（值传递） |
| 顶层 `function` | 纯计算函数 |
| `action:` + `@startup` | 组件初始化（自动调用） |
| `@thread` | 需要独立线程的组件 |
