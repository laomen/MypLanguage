# MYP 编程手册

> 版本 2.1 | 事件驱动组件语言

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
| 9 | 赋值 | `=` `+=` `-=` `*=` `/=` `%=` |
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
Console.writeLong(1234567890);  // 输出长整数
Console.writeFloat(3.14);       // 输出浮点数
Console.writeBool(true);        // 输出布尔值
```

### `import math` — 数学函数

```myp
import math;

var r = Math.sqrt(64.0);      // 8.0
var a = Math.abs(-3.5);       // 3.5
var s = Math.sin(3.14159);    // ~0
var c = Math.cos(3.14159);    // ~-1
var p = Math.pow(2.0, 10.0); // 1024.0
var m = Math.max(10, 20);     // 20
```

### `import timeline` — 时间与定时器

```myp
import timeline;

var now = Timeline.now();          // 当前时间 (ms)
Timeline.sleep(1000);              // 等待 1 秒
var elapsed = Timeline.now();      // 经过时间

// 定时器事件
Timeline timer;
timer.startTimeout(500);           // 500ms 后触发 timeout 事件
timer.startInterval(1000);         // 每秒触发 interval 事件

mapping() {
    timer.timeout -> handler.onTimeout;
    timer.interval -> handler.onInterval;
}
```

### `import io` — 文件 I/O

```myp
import io;

File file;
file.open("data.txt", "r");
if (file.hasNext()) {
    var line = file.readLine();
    Console.writeLine(line);
}
file.close();
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
