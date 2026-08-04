# MYP 语言设计文档

> 版本: 2.3 | 日期: 2026-07-27

---

## 目录

1. [概述](#1-概述)
2. [设计哲学](#2-设计哲学)
3. [范式分析](#3-范式分析)
   - 3.5 [接口多态与自动微分](#35-接口多态与自动微分)
4. [语法规范](#4-语法规范)
5. [类型系统](#5-类型系统)
6. [Class 系统](#6-class-系统)
7. [事件与 Mapping](#7-事件与-mapping)
8. [并发模型](#8-并发模型)
9. [导入系统](#9-导入系统)
10. [标准库设计](#10-标准库设计)
11. [实现计划](#11-实现计划)
12. [附录：EBNF 语法](#12-附录ebnf-语法)
13. [附录：完整示例](#13-附录完整示例)

---

## 1. 概述

| 项目 | 内容 |
|------|------|
| **语言名称** | MYP |
| **类型** | 编译型、静态类型、通用编程语言 |
| **范式** | 事件驱动组件 + 面向对象 |
| **后端** | LLVM (C++ API) |
| **实现语言** | C/C++ |
| **文件扩展名** | `.myp` |

### 核心创新

MYP 的核心是一种 **事件-动作解耦模型**，结合**接口多态**实现声明式算子链：

```
a.event -> b.action;
IOperator op = new ConcreteOp();
double y = op.forward(x);
```

```
a.event -> b.action;
```

当实例 `a` 触发 `event` 时，自动调用实例 `b` 的 `action`。事件源与动作目标之间通过 **mapping** 声明式绑定，完全解耦。

---

## 2. 设计哲学

### 2.1 事件驱动组件

MYP 的 class 是一个 **组件单元**，包含三部分：

```
class 组件名 {
    action:     // 可被调用的方法（有返回类型）
    event:      // 可触发的事件（无返回类型）
    property:   // 内部状态（成员变量）
}
```

### 2.2 声明式绑定

不写胶水代码，通过 `mapping()` 声明事件与动作的关系：

```
mapping() {
    sensor.readingReady -> alarm.trigger -> display.showMessage;
}
```

### 2.3 Actor 式并发

`@thread` 注解让实例在独立线程运行，跨线程通信全部走 mapping，无共享内存竞争。

### 2.4 C/Java 风格语法

- 花括号 `{}` 代码块，分号 `;` 结尾
- 类型前置：`int number;` `void foo() { }`
- 无 `func` 关键字
- `int main(int argc, string[] argv)` 入口

---

## 3. 范式分析

### 3.1 优点

| 优势 | 说明 |
|------|------|
| **极致解耦** | 事件源和动作目标完全分离。新增/移除/修改功能只需改一行 `mapping()`，零侵入 |
| **架构可见** | 阅读所有 `mapping()` 声明即可了解系统架构——代码即架构图 |
| **天然并发** | 三段式 class 天然隔离状态，`@thread` 让组件独立运行，无需加锁 |
| **高度可测试** | 每个 class 是独立单元：注入事件 → 断言 action 输出，无需 mock 框架 |
| **数据流管线** | 事件链 `A.e -> B.a -> C.a` 天然形成数据处理管道 |

### 3.2 缺点

| 劣势 | 说明 |
|------|------|
| **简单事情变复杂** | 简单的函数调用需要定义 class + event + action + mapping |
| **控制流不直观** | mapping 多了可能形成事件循环（`a→b→c→a`），代码中不易察觉 |
| **调试困难** | 事件是异步跳转的，传统断点无法直接追踪"谁触发了谁" |
| **不适合纯计算** | 矩阵运算、图像处理等计算密集型任务用事件驱动很别扭 |
| **运行时开销** | 每次事件需查 mapping 表 + 参数打包 + 消息分发，比直接函数调用慢 |
| **学习曲线** | 三段式 class + mapping 声明式思维需要转变编程范式 |

### 3.3 优化策略

| 缺点 | 优化方案 | 引入版本 | 效果 |
|------|---------|---------|------|
| 运行时开销 | **编译期 mapping 内联**：同线程 mapping 解析为直接函数指针 | v1 | 同线程零开销 |
| 事件循环 | **编译期环路检测**：静态分析 mapping 链，检测同一实例多次出现 | v1 ✅ | 代码写错时提前警告 |
| 调试困难 | **`--trace` 事件追踪**：每条事件链路带唯一 ID，因果清晰 | v2 | 事件流可视化 |
| 简单任务繁琐 | **轻量语法糖**：自由函数级事件绑定、匿名 mapping | v2 | 减少样板代码 |
| 不适合计算 | **Dual Paradigm**：action 内部是完整过程式代码，`@pure` 注解 | v1(过程式)/v3(@pure) | 计算部分不受影响 |
| 学习曲线 | **渐进式语法**：脚本→函数→class→event→@thread 分步学习 | v1 | 每步只学一个新概念 |

> **核心权衡**：用「简单事情变复杂」换「复杂系统变简单」。

### 3.4 适用场景

| 场景 | 适合度 | 说明 |
|------|--------|------|
| IoT / 传感器网络 | ⭐⭐⭐ 完美 | 每个传感器=class，事件驱动天然匹配 |
| GUI 应用 | ⭐⭐⭐ 很好 | 按钮点击→事件→动作，比 callback 优雅 |
| 游戏逻辑 | ⭐⭐⭐ 很好 | 实体组件系统(event→action)自然适配 |
| 微服务/事件驱动架构 | ⭐⭐⭐ 很好 | mapping 就是服务编排层 |
| CLI 工具 | ⭐ 差 | 杀鸡用牛刀 |
| Web 后端 CRUD | ⭐ 差 | 请求-响应模型不适配事件驱动 |
| 数值计算/算法 | ⭐ 差 | 过程式更直接 |
| 系统编程/驱动 | ⭐⭐ 好 | 事件驱动适合硬件中断处理 |

### 3.5 接口多态与自动微分

✅ **接口多态** — 已实现。

interface 变量以 Go 风格胖指针 `{ptr data, ptr vtable}` 存储，方法调用通过虚表分派。类通过 `interface class InterfaceName;` 声明实现某个接口。

```c
interface IOp {
    double forward(double x);
    double backward(double dy);
}

class MulOp {
    interface class IOp;
    action:
        double forward(double x) { saved = x; return x * w; }
        double backward(double dy) { gradW = dy * saved; return dy * w; }
    property: double w, saved, gradW;
}

// 接口多态引用：IOp 变量可指向任何实现 IOp 的类
IOp op = new MulOp();
double y = op.forward(3.0);   // 虚表分派 → MulOp_forward
```

每个算子前向保存输入，反向用链式法则计算梯度，构成声明式自动微分。示例见 `examples/ad.myp`。

---

### 3.6 未来展望

#### 众核架构的天然适配

MYP 的设计在「传统 CPU 架构」上有额外的间接开销，但在未来 **10,000+ 低频核心 + 片上网络 (NoC)** 的众核架构上将展现天然优势：

| 众核特性 | MYP 的对应 | 优势 |
|---------|-----------|------|
| 无共享缓存一致性 | MYP 无共享内存，只有消息传递 | 完全匹配 |
| 核间通信昂贵 | event→action 是显式消息传递 | 通信语义明确 |
| 每核私有内存 | 每个 `@thread` 实例独立 state | 天然分区 |
| 简单顺序核心 | 事件处理是「接收→处理→发送」 | 适合顺序执行 |
| 大量核心 | 每个 Sensor/Alarm/Display 各占一核 | 可线性扩展 |
| 低频率/低功耗 | 事件驱动 = 无事休眠，有事件唤醒 | 能耗匹配 |

```
MYP 在此类架构上的执行模型:
                 ┌──────────────────┐
  event ──────── │Core 0: Sensor    │ ── readingReady ──┐
                 └──────────────────┘                    │
                                          ┌──────────────▼──┐
                                          │Core 1: Alarm    │
                                          │  trigger(msg)   │
                                          └────────┬─────────┘
                                                   │ alarmTriggered
                                          ┌────────▼─────────┐
                                          │Core 2: Display   │
                                          │  showWarning(msg)│
                                          └──────────────────┘
  每个 A 独占核心，通信走片上网络，无可变状态共享
```

#### 类脑芯片编程

MYP 的事件驱动组件模型与**神经形态计算**（类脑芯片）存在深层概念对应：

| 类脑芯片 | MYP 对应 | 说明 |
|---------|---------|------|
| 神经元 (Neuron) | `class` | 带状态和行为的组件 |
| 动作电位 (Spike) | `event` | 事件触发 |
| 树突 (Dendrite) | `action` | 方法=输入端 |
| 轴突 (Axon) | `event: fired()` | 事件=输出端 |
| 突触 (Synapse) | `mapping() { a.e -> b.a; }` | 连接声明 |
| 突触权重 | `property` | 成员变量 |
| 阈值激发 | `mapping() when (v > threshold)` | 条件映射 |
| 可塑性 (学习) | action 中修改 property | 运行时调参 |

##### 神经元建模示例

```myp
class LIFNeuron {
    action:
        void excitatory(float spike);
        void inhibitory(float spike);
        void reset();
    event:
        fired(float membranePotential);
    property:
        float v;            // 膜电位
        float threshold;    // 激发阈值
        float leak;         // 泄漏系数
        float vRest;        // 静息电位
}

// 三层神经网络 = 声明式 mapping 集合
int main() {
    LIFNeuron in1, in2, in3, in4;
    LIFNeuron hid1, hid2, hid3, hid4;
    LIFNeuron out1, out2;

    mapping() {
        in1.fired -> hid1.excitatory;
        in2.fired -> hid2.excitatory;
        in3.fired -> hid3.excitatory;
        in4.fired -> hid4.excitatory;
        hid1.fired -> out1.excitatory;
        hid2.fired -> out2.excitatory;
        hid3.fired -> out2.excitatory;
        hid4.fired -> out1.excitatory;
        out1.fired -> out2.inhibitory;   // 侧向抑制
        out2.fired -> out1.inhibitory;
    }
}
```

##### 类脑芯片后端的编译器路径

```
MYP 源码 → 编译器
           ├── LLVM IR → x86 代码（传统 CPU 回退）
           └── 神经形态后端 → 芯片配置:
                              ├── mapping()  → 片上路由表（突触连接）
                              ├── @thread   → 核分配（神经元到核）
                              ├── event     → spike 路由
                              └── action    → 核内程序
```

##### 与现有类脑平台对比

| 平台 | 编程方式 | MYP 的对比优势 |
|------|---------|---------------|
| Intel Loihi | NxSDK (Python/C) | MYP 的 mapping 声明式连线更接近脑结构 |
| IBM TrueNorth | Corelet 专有语言 | MYP 更通用，非锁定特定芯片 |
| SpiNNaker | PyNN + C 微代码 | MYP 单语言体验更统一 |
| BrainScaleS | PyNN + 硬件描述 | MYP 的 event→action 语义天然匹配 |

> MYP 的独特价值在于：它是第一个把「事件 + 连接」作为语言一等公民的通用编程语言——事件和连接不是库模拟的，而是编译器理解并可以优化/映射到硬件的。

---

## 4. 语法规范

### 4.1 词法

| 项目 | 规则 |
|------|------|
| 单行注释 | `//` |
| 多行注释 | `/* */` |
| 标识符 | 字母/下划线开头，后接字母/数字/下划线 |
| 整数 | `42`, `0xFF` |
| 浮点 | `3.14`, `1.0e-5` |
| 布尔 | `true`, `false` |
| 字符串 | `"hello"` (双引号，支持 `\n` `\t` `\"` `\\` 转义) |
| 空值 | `null` |

### 4.2 关键字

```
class  action  event  property  interface  import
void  byte  short  int  long  ubyte  ushort  uint  ulong
char  float  double  bool  string
mapping  if  else  while  for  return  break  continue  static
true  false  null  this  new  struct  function
```

### 4.3 变量声明

```
int number;
string name = "MYP";
float pi = 3.14;
bool flag = true;
int[] arr;                // 数组
ClassName obj;            // 用户类型
ClassName obj @thread;    // 带注解
```

- 类型必须显式，不支持推断
- 可选初始化：`类型 名称 = 值;`

### 4.4 函数定义

```
int add(int a, int b) {
    return a + b;
}

void printHello() {
    // ...
}

int main(int argc, string[] argv) {
    return 0;
}

int main() {        // 简写
    return 0;
}
```

- 无 `func` 关键字
- 返回类型 + 函数名 + `(参数)` + 函数体
- 参数：`类型 名称`

### 4.5 控制流

```
if (cond) {
    // ...
} else if (cond2) {
    // ...
} else {
    // ...
}

while (cond) {
    // ...
}

for (int i = 0; i < 10; i = i + 1) {
    // ...
}

break;
continue;
return expr;
```

- 条件表达式必须带括号 `()`
- 函数体必须用花括号 `{}`

### 4.6 表达式与运算符

| 优先级 | 类别 | 运算符 | 结合性 |
|--------|------|--------|--------|
| 10 | 赋值 | `=` `+=` `-=` `*=` `/=` `%=` | 右 |
| 9 | 管道 | `\|>` | 左 |
| 8 | 三元 | `? :` | 右 |
| 7 | 逻辑或 | `\|\|` | 左 |
| 6 | 逻辑与 | `&&` | 左 |
| 5 | 等值 | `==` `!=` | 左 |
| 4 | 关系 | `<` `>` `<=` `>=` | 左 |
| 3 | 加法 | `+` `-` | 左 |
| 2 | 乘法 | `*` `/` `%` | 左 |
| 1 | 一元 | `!` `-` `++` `--` | 右 |
| 0 | 后缀 | `.` `[]` `()` `++` `--` | 左 |

---

## 5. 类型系统

### 5.1 基本类型

| 类型 | 描述 | LLVM 映射 | 大小 |
|------|------|-----------|------|
| `byte` | 有符号 8-bit 整数 | `i8` | 8-bit |
| `short` | 有符号 16-bit 整数 | `i16` | 16-bit |
| `int` | 有符号 32-bit 整数 | `i32` | 32-bit |
| `long` | 有符号 64-bit 整数 | `i64` | 64-bit |
| `ubyte` | 无符号 8-bit 整数 | `i8` | 8-bit |
| `ushort` | 无符号 16-bit 整数 | `i16` | 16-bit |
| `uint` | 无符号 32-bit 整数 | `i32` | 32-bit |
| `ulong` | 无符号 64-bit 整数 | `i64` | 64-bit |
| `char` | 单字节字符 | `i8` | 8-bit |
| `float` | 单精度浮点 | `float` | 32-bit |
| `double` | 双精度浮点 | `double` | 64-bit |
| `bool` | 布尔值 | `i1` | 1-bit |
| `string` | 字符串指针 | `i8*` | 指针 |
| `void` | 无类型（仅函数返回） | `void` | — |

### 5.3 类型规则

- **数字提升**：数字类型之间支持单向隐式提升，确保算术和函数调用时类型兼容：
  ```
  byte → short → int → long → float → double
  short → int → long → float → double
  int → long → float → double
  long → float → double
  float → double
  ```
  例如 `int` 值可直接传递给 `long` 参数的函数。
- **无符号类型**：`ubyte`/`ushort`/`uint`/`ulong` 与对应的有符号类型在 LLVM 底层表示相同，但在类型检查时视为不同类型（当前版本无符号类型不参与自动提升，需显式转换）
- **`char`**：单字节字符，字面量用单引号：`'A'`、`'\n'`

### 5.4 复合类型

```
int[]     // 整型数组
string[]  // 字符串数组
ClassName  // 用户定义的类类型
```

### 5.5 类型系统规则

- 静态类型：所有变量和表达式的类型在编译时确定
- 强类型：不支持隐式类型转换
- 类名可直接作为类型使用
- 第一版不支持泛型

---

## 6. Class 系统

### 6.1 三段式结构

```
class Sensor {
    action:                     // 方法区
        void init(int id);
        float readValue();

    event:                      // 事件区
        valueRead(float temp);
        thresholdExceeded(float value);

    property:                   // 属性区
        int sensorId;
        float threshold;
        float lastValue;

    interface class ISensor;    // 外部接口声明
}
```

### 6.2 段规则

| 段 | 声明内容 | 是否允许函数体 |
|----|---------|--------------|
| `action:` | 方法（有返回类型） | 允许 `;` 声明或 `{ }` 定义 |
| `event:` | 事件（无返回类型） | 仅 `;` 声明 |
| `property:` | 成员变量 | 仅变量声明 |
| `function:` | 内部方法（不参与 mapping） | `{ }` 定义 |
| `static:` | 静态方法（无需实例，`import` 即可调用） | `{ }` 定义 |
| `struct:` | 嵌套结构体 | 字段/方法定义 |

**注解：** Action 前可加 `@startup` 注解，表示实例创建后自动执行；变量声明后可加 `@thread` 注解，表示该实例在独立线程运行。

### 6.3 Interface（接口）

MYP 支持基于**编译期检查**的接口。通过 `interface class` 声明类实现了某个接口，编译器会验证类是否包含接口声明的所有 action 和 event：

```myp
interface IShape {
    double area();
    double perimeter();
}

class Circle {
    interface class IShape;  // 声明实现 IShape
    action:
        double area() { return 3.14159 * radius * radius; }
        double perimeter() { return 2.0 * 3.14159 * radius; }
    property:
        double radius;
}
```

- 缺少接口成员 → **编译错误**
- 不要求接口多态（无 vtable），仅编译期类型检查
- 组件间通信仍通过 `mapping()` 声明式连接，这是天然的 duck typing

### 6.4 访问控制

MYP 是**事件驱动组件**语言，访问控制规则服务于解耦目标：

| 成员 | 类内部（`this.`） | 外部（`obj.`） |
|------|------------------|---------------|
| `action:` | ✅ 可调用 | ✅ 可调用 |
| `event:` | ✅ 可触发 | ✅ 可触发（通过 mapping 或 fire 函数） |
| `property:` | ✅ 读写 | ❌ **不允许直接访问** |

**原则**：外部代码不能直接读写类的 property。所有跨组件数据传递必须通过 event→action 的 mapping 链完成。这保证了：
- 组件间完全解耦，没有隐式数据依赖
- 架构完全由 `mapping()` 声明可见
- 重构时只需修改 mapping，无需搜索属性使用点

### 6.5 继承、多态与 @startup 生命周期

#### @startup 自动初始化

任何 action 前加 `@startup` 注解，当实例通过 `new` 创建时自动调用：

```myp
class Sensor {
    action:
        @startup void init() { id = 1; threshold = 100; }
        float readValue();
    property:
        int id;
        float threshold;
}

int main() {
    Sensor s = new Sensor();  // 自动调用 init()
    return 0;
}
```

- 每个 class 可以有多个 `@startup` 方法
- `@startup` 是**显式声明**而非构造函数——名称自定义，逻辑可见
- 对于 `@thread` 实例，`@startup` 在目标线程执行

#### @thread 线程注解

```myp
Worker w = new Worker() @thread;  // 在独立线程运行
```

详见[并发模型](#8-并发模型)。

#### 继承与多态

MYP **不支持类继承**。在事件驱动模型中：
- 继承带来紧耦合，与解耦哲学冲突
- 组件间通信通过 `mapping()` 声明式连接，无需统一接口类型——天然 duck typing
- 代码复用通过 **mapping 组合**实现：将一个组件通过 mapping 连接到另一个组件的 action/event

**组合 vs 继承：**

| 维度 | OOP 继承 | MYP Mapping 组合 |
|------|---------|-----------------|
| 耦合度 | 紧耦合 | 零耦合 |
| 复用 | 继承父类实现 | 连接任意组件 |
| 切换 | 改类型层次 | 改一行 mapping |

---

## 7. 事件与 Mapping

### 7.1 事件声明

事件在 class 的 `event:` 段内声明，无返回类型：

```
event:
    valueRead(float temp);
    thresholdExceeded(float value);
```

事件可以被代码显式触发（由运行时实现）。

### 7.2 Mapping 声明

Mapping 将事件连接到动作，支持两种级别：

#### 类型级映射（文件级全局）

```
mapping() {
    Sensor.valueRead -> Display.showTemperature;
}
```

#### 实例级映射（函数内局部）

```
int main() {
    Sensor s;
    Display d;

    mapping() {
        s.valueRead -> d.showTemperature;
    }
}
```

### 7.3 事件链

一个 mapping 可以链式连接多个节点，前一个 action 的返回值自动传递给下一个节点：

```
mapping() {
    A.event -> B.process -> C.onResult;
}
// 语义：
// 1. A 触发 event
// 2. B.process() 被异步调用，返回 int
// 3. int 值自动传入 C.onResult(int)
```

### 7.4 Mapping 语义

#### 7.4.1 作用域管理 @scope

默认 mapping 永久有效。`@scope` 标记将 mapping 的 handler 生命周期绑定到函数作用域：

```
void run() {
    Sensor s;
    mapping() @scope {
        s.dataReady -> log.write;
    }
}  // ← 函数退出时 handler 自动解注册
```

#### 7.4.2 条件过滤 where

在事件源后加 `where 表达式`，只有满足条件的事件才会被转发：

```
mapping() {
    rs.valueEmitted where value >= 3 -> Console.write;   // 只转发 >=3 的
}
```

`where` 表达式可使用事件参数名（如 `value`），支持完整的比较和算术运算。

#### 7.4.3 Lambda 变换节点

在 mapping 链中用 lambda 表达式做内联数据变换：

```
mapping() {
    rs.valueEmitted -> (int v) => { return v * 2; } -> Console.write;
    rs.valueEmitted where value % 2 == 0 -> (int v) => { return v * 10; } -> output.save;
}
```

#### 7.4.4 定时变换器

- `delay(ms)` — 事件转发前阻塞等待指定毫秒
- `throttle(ms)` — 限频：间隔内到达的事件丢弃

```
mapping() {
    sensor.valueEmitted -> delay(100) -> display.update;     // 延迟 100ms
    sensor.valueEmitted -> throttle(50) -> logger.write;      // 50ms 限频
}
```

### 7.5 完整 Mapping 语法

```
mapping() [@scope] {
    source.event  [where 条件表达式] -> target1.action -> target2.action, ...;
    source.event2 -> lambda -> target.action;
    source.event3 -> delay(ms) -> target.action;
    source.event4 -> throttle(ms) -> target.action;
}

- 一个事件可以映射到多个动作
- 多个事件可以映射到同一个动作
- mapping 在运行时建立事件总线，事件触发时自动分发到所有绑定的动作

---

## 7.5 事件时间线 (Event Timeline)

理解事件如何在时间线上流动是利用好 MYP 事件驱动模型的关键。

### 7.5.1 事件的生命周期

每个事件从触发到处理完成，经历四个阶段：

```
  fire(ev)          queue.push()        dispatch()        handler()
 ────────→  ┌─────→ ░░░░░░░░░ ───────→ ░░░░░░░░░ ──────→ ░░░░░░░░░
             │      ┌──────────┐       ┌──────────┐      ┌──────────┐
  触发事件    │      │  事件队列  │       │ 事件分发   │      │ 处理器执行 │
             │      │(ring buf) │       │(handler   │      │(action)   │
             │      │ per-thread│       │  lookup)  │      │           │
             └──────┴──────────┘       └──────────┘      └──────────┘
                        ↑                    ↑
                   myp_event_fire()    myp_event_process_all()
```

### 7.5.2 时间线 = 线程

MYP 中**每个线程是一条独立的事件时间线**。每条时间线有自己的事件队列和事件循环：

```
时间线 A（主线程）:
  fire(X) ──→ [queue A] ──→ process A ──→ handler(X)
  fire(Y) ──→ [queue A] ──→ process A ──→ handler(Y)
                                   ↑
                              同步处理（fire 后立即 process_all）

时间线 B（Worker @thread）:
  ┌─ 事件循环 ─────────────────────────┐
  │  process_all → [queue B] → handler │  ← 异步处理
  │  process_all → [queue B] → handler │
  │  sleep 1ms                        │
  └────────────────────────────────────┘
```

### 7.5.3 同时间线 = 同步

当 fire 和 handler 在**同一条时间线**上时，事件从触发到处理是**同步**的：

```
fire(ev) → queue.push() → process_all() → dispatch() → handler()
                                                                 ↑
                                                 在同一个函数调用栈内完成
```

这意味着：
- 事件数据（参数）分配在栈上，安全无拷贝
- handler 的返回值立即可见
- 调用者等待 handler 完成后才继续
- **无竞态条件**、无需加锁

### 7.5.4 跨时间线 = 异步

当 fire 和 handler 在**不同时间线**上时，事件传递是**异步**的：

```
时间线 A（fire 线程）:          时间线 B（目标线程）:
fire(ev)                          (事件循环中)
  │                                 │
  ├─ instance→thread 映射查询       │
  ├─ queue B().push(ev)          │
  └─ 立即返回 ←── 不等待 ──→      │
                                  ├─ process_all()
                                  ├─ queue B().pop(ev)
                                  ├─ dispatch(ev)
                                  └─ handler(ev)  ← 在 B 线程执行
```

关键特性：
- **fire 线程永不阻塞**：入队后立即返回，不等待处理
- **handler 在目标线程执行**：数据流跨线程，但执行上下文在线程内
- **事件数据问题**：栈分配的数据在 fire 返回后即失效，因此跨时间线投递需要**堆分配数据**或**数据拷贝**（当前实现中，跨线程事件通过全局映射投递，同线程事件保持栈分配同步处理）
- **无共享内存竞争**：handler 操作的是目标线程的实例和数据

### 7.5.5 时间线隔离规则

| 规则 | 说明 |
|------|------|
| **数据归属** | 每个 `@thread` 实例的数据只被自己的时间线访问 |
| **通信唯一通道** | 跨时间线通信只能通过 `mapping()` + 事件 |
| **无需加锁** | 没有显式锁——时间线隔离本身就是并发安全 |
| **fire 即发即忘** | fire 后不依赖返回值，结果通过后续事件链返回 |

### 7.5.6 时间线可视化

对于一个多 `@thread` 系统，时间线如下：

```
时间线: main ─┬─ create Sensor ── create Alarm ── fire ─── sleep ── cleanup ── exit
              │                                          ↕ 异步
时间线: alarm ── @startup ── fire ── event loop ────────── process ── handler ── fire
                          │         ↕ 异步                          ↕ 异步
时间线: disp  ────────────── event loop ────────────────────────────── process ── handler
```

mapping 声明将不同时间线的组件连接成有向图：

```
mapping() {
    sensor.valueRead -> alarm.trigger -> disp.showMessage;
}
```

对应的时间线跳转：`main → alarm → disp`

### 7.5.7 与 Actor 模型的对应

| Actor 模型概念 | MYP 对应 |
|---------------|---------|
| Actor | `@thread` class 实例 |
| 邮箱 (Mailbox) | 每线程事件队列 |
| 消息 (Message) | `event` + 参数数据 |
| 行为 (Behavior) | `action` |
| 地址 (Address) | 实例指针 |
| 容错 | 组件崩溃不影响其他时间线 |

---

## 8. 并发模型

### 8.1 Actor + @thread

MYP 的并发基于 **Actor 模型**，通过实例级 `@thread` 注解控制：

```
int main() {
    Sensor s1;              // 当前线程（同步）
    Alarm alarm @thread;    // 独立线程（异步）
    Display disp @thread;   // 独立线程（异步）

    mapping() {
        s1.valueRead -> alarm.trigger -> disp.showMessage;
    }
}
```

### 8.2 跨线程调用规则

| 场景 | 行为 |
|------|------|
| 同线程 mapping | 同步调用（直接函数调用） |
| 跨线程 mapping | 异步调用（消息投递到目标线程的消息队列） |
| 直接跨线程调用 | ❌ 不允许，必须通过 `mapping()` |

### 8.3 事件链与并发

```
mapping() {
    s1.event -> alarm.trigger -> disp.showMessage;
}
```

当 `alarm` 带 `@thread` 时：
1. `s1.event` 触发（s1 的线程）
2. `alarm.trigger` 异步执行（alarm 的线程）
3. `alarm.trigger` 的返回值 → 投递到 `disp.showMessage`（disp 的线程）

### 8.4 共享引用

- 对象默认按**引用**传递（不拷贝）
- 同一时间线内的引用是安全的——无竞态条件
- 跨时间线（跨 `@thread`）的引用：**禁止直接共享**
  - 跨线程通信的唯一合法通道是 `mapping()` + 事件投递
  - 不能将一个 `@thread` 实例的 property 直接赋值给另一个线程的变量
  - 这条规则由**编译器检查**（未来版本）或**程序员自律**（当前版本）

### 8.5 未来：显式同步注解

当前 MYP **没有**显式锁机制——因为设计上就不需要：

| 场景 | 正确的做法 | 错误的做法 |
|------|-----------|-----------|
| 跨线程传递数据 | `mapping() { a.event -> b.action; }` | 直接写 `b.property = value` |
| 线程间共享状态 | 用独立 `@thread` 实例管理状态，通过事件查询/修改 | 多个线程读写同一个 property |
| 归约/聚合 | 每个线程维护自己的 tally，最后用事件汇总 | 共享全局数组 + 加锁 |

如果未来需要更细粒度的同步控制，可能引入：

```myp
@mutex class SafeCounter {     // 整个 class 实例受 mutex 保护
    action:
        void inc() { count = count + 1; }
    property:
        int count;
}

class BankAccount {
    action:
        @synchronized void withdraw(int amount) {  // 单方法加锁
            balance = balance - amount;
        }
    property:
        int balance;
}
```

但目前 **不推荐** 使用共享内存并发模式——MYP 的事件驱动模型已经为无共享并发设计好了。引入锁会让架构退化到传统的共享内存并发，丢失事件驱动的大部分优势。

### 8.6 未来：Event-driven Pool（方案 B）

事件驱动的 Worker Pool 模式——不修改编译器，纯运行时 + stdlib 实现。

#### 8.6.1 架构

```
Source ──batchReady──→ Pool.submit
                          │
                    ┌─────┴─────┐
                    │ Work Queue │ (线程安全，支持工作窃取)
                    └─────┬─────┘
                          │
         ┌────────────────┼────────────────┐
         ▼                ▼                ▼
    Worker0.runBatch  Worker1.runBatch  Worker2.runBatch
         │                │                │
         └───batchDone───┼────────────────┘
                         ▼
                    Tally.addBatch
```

#### 8.6.2 工作机制

1. **Pool** 持有 `工作窃取队列` + N 个 Worker 线程
2. `Pool.submit(batchId, size)` → 将任务入队 → 条件变量唤醒空闲 Worker
3. Worker 完成当前 batch → 从队列拿下一个任务（或从其他 Worker 偷任务）
4. Worker 完成一个 batch → 发射 `batchDone` 事件 → Tally 累加

#### 8.6.3 需要的运行时支持

| 组件 | 说明 |
|------|------|
| 工作窃取队列 | 线程安全 deque，每个 Worker 有自己的双端队列 + 全局队列 |
| 条件变量唤醒 | 替代 1ms 轮询，任务入队立即唤醒 Worker |
| 可伸缩事件队列 | 替代固定 1024 环缓冲，支持动态扩容（防止静默丢事件） |

#### 8.6.4 优势与局限

| 维度 | 评价 |
|------|------|
| 编译器改动 | ✅ 零改动（纯运行时 + stdlib） |
| 负载均衡 | ✅ 工作窃取，天然动态均衡 |
| 灵活性 | ✅ 可通过 mapping 任意组合 Source/Worker/Tally |
| 单 batch 开销 | ⚠️ 每次 batch 需 2 次事件排队（submit → dispatch → batchDone） |
| 实现复杂度 | ⚠️ ~400 行（工作窃取队列 + Pool 类 + mapping 配置） |
| 适用场景 | 每个 batch 计算量大（>1ms），事件排队开销可忽略 |

### 8.7 已实现：`@parallel for`（方案 A）

✅ v2.4 已实现。不需要 event/mapping，零运行时开销。

MYP 的 Actor 模型 (`@thread`) 擅长 IO/事件驱动型并发，但对于 **BNCT 蒙特卡洛输运**这类计算密集型、数据并行的场景，事件开销（排队、唤醒、调度）成为瓶颈。`@parallel for` 为这类场景提供编译期并行的解决方案。

#### 8.7.1 语法

```myp
@parallel for (int i = 0; i < n; i = i + 1) {
    // 循环体——每个线程执行一部分迭代
    Atomic.addDouble(tally, idx, value);
}
```

循环变量类型：`int`（推荐）或 `long`（自动截断为 int32 处理边界）。

#### 8.7.2 编译器视角

```
1. 收集所有外层作用域变量，构建捕获结构体 (parallel_captute)
2. 提取循环体为静态函数 fn(int i, void* arg)
3. 在调用处填充捕获结构体，调用 myp_pool_parallel_for()

捕获结构体：
  struct parallel_captute {
      int32   size;         // 值捕获
      double  nH;           // 值捕获
      double* depthDose;    // 指针捕获（堆数组，线程共享）
      // ... 所有外层变量
  };

并行体函数：
  void parallel_body_i(int i, void* arg) {
      auto* cap = (parallel_captute*)arg;
      // 解包捕获变量到局部 alloca
      int size = cap->size;
      // ... 原始循环体代码（使用局部变量）...
  }
```

#### 8.7.3 运行时视角

```
3. 线程池平分迭代：thread[0] 拿 [0..N/4), thread[1] 拿 [N/4..N/2) ...
4. 各线程串行执行 fn(i, ctx)
5. barrier 等待全部完成 → 返回
```

线程池使用 16 线程 work-stealing 池，全局懒创建 + 复用。

#### 8.7.4 关键实现细节

| 机制 | 说明 |
|------|------|
| **变量捕获** | `generateParallelFor` 遍历作用域栈，收集所有 named values → 构建 LLVM StructType → 填充 → `void* arg` 传递 |
| **数学函数** | emitKernelExpr 原使用 `__nv_log`/`__nv_exp`（CUDA device 函数），改为 `myp_math_log`/`myp_math_exp` 运行时函数 |
| **静态方法调用** | emitKernelExpr 直接调用 LLVM 模块中已声明的函数（如 `Physics_sampleEnergy`），避免内联复杂 IfStmt 导致返回 `i64(0)` |
| **Atomic 操作** | `Atomic.addDouble`/`Atomic.addInt` 通过 LLVM `atomicrmw` 指令编译，线程安全 |
| **线程安全** | 每粒子独立 RNG state（无竞争）+ Atomic 累加（无锁） |

#### 8.7.5 vs Actor 模型

```
Actor (@thread)              @parallel for
───────────────────────      ───────────────────────
事件驱动                     数据驱动
异步通信                     同步 barrier
无共享 (share-nothing)       共享读取 + 线程局部写入
适合 IO/状态管理              适合计算/数值模拟
通信延迟大（1ms 轮询）         通信零开销（共享内存）
```

两个模型互补——`@parallel for` 处理 BNCT 的核心计算循环，Actor 模型处理组件编排和结果汇总。

### 8.8 已实现：Atomic 操作

✅ v2.0 已实现。通过 LLVM `atomicrmw` 指令直接生成，零运行时开销。

当多个线程需要累加共享变量时（如 Tally 汇总），必须使用原子操作避免竞态条件：

```myp
import atomic;

class Tally {
    action:
        void addBatch(int nCap, double dose) {
            Atomic.addInt(totalCaptured, idx, nCap);   // i32 原子加
            Atomic.addDouble(totalDose, idx, dose);     // f64 原子加
        }
}
```

支持的操作：

| 函数 | 语义 | LLVM 指令 |
|------|------|-----------|
| `Atomic.addInt(arr, i, v)` | `arr[i] += v`（返回旧值） | `atomicrmw add` |
| `Atomic.subInt(arr, i, v)` | `arr[i] -= v` | `atomicrmw sub` |
| `Atomic.xchgInt(arr, i, v)` | `arr[i] = v`（返回旧值） | `atomicrmw xchg` |
| `Atomic.addDouble(arr, i, v)` | `arr[i] += v` | `atomicrmw fadd` |
| `Atomic.loadInt(arr, i)` | 返回 `arr[i]` | `atomicrmw add 0` |
| `Atomic.storeInt(arr, i, v)` | `arr[i] = v` | `atomicrmw xchg` |

### 8.5 已实现的功能

| 功能 | 状态 | 说明 |
|------|------|------|
| `@thread` 注解 | ✅ 已实现 | 实例在独立 pthread 线程运行 |
| 每线程独立事件队列 | ✅ 已实现 | ring buffer + mutex 保护 |
| 跨线程异步投递 | ✅ 已实现 | `myp_thread_for_instance` 查询 + 队列推送 |
| 定时器系统 | ✅ 已实现 | `__myp_timer_create` + `myp_timer_check` 事件循环 |
| 每线程独立定时器 | ✅ 已实现 | 定时器在所属线程的事件循环中触发 |
| 线程生命周期管理 | ✅ 已实现 | `myp_thread_create`/`destroy`/`stop`/`join` |
| `@threadpool` 注解 | ✅ 已实现 | 函数内 `Worker[N] pool @threadpool` 可用；不支持文件级声明（需在函数中初始化，通常放 `@startup` 或 `main()`） |

---

## 9. 导入系统

```
import env;           // 标准库导入（从 stdlib/ 目录加载 env.myp）
import timeline;      // 标准库导入
import "./helper.myp"; // 用户文件导入（相对当前源文件路径）
import "/abs/path/lib.myp"; // 用户文件导入（绝对路径）
```

| 项目 | 规则 |
|------|------|
| 语法 | `import 标识符;` 或 `import "路径";` |
| 标准库 | 无引号、无扩展名，在 `stdlib/` 目录查找 |
| 用户文件 | 双引号字符串路径，支持相对/绝对路径 |
| 路径搜索 | `--stdlib` 指定目录 → 可执行文件所在目录的 `../stdlib/` → 源文件所在目录的 `stdlib/` → `--package-path` 指定目录 |
| `--stdlib` | 编译器选项，指定标准库路径 |
| `--shared` | 编译为共享库 (.so)，无 main 要求 |
| `--static` | 编译为静态库 (.a)，无 main 要求 |
| `--package-path` | 编译器选项，指定本地包搜索路径（支持冒号分隔多路径、`MYP_PACKAGE_PATH` 环境变量） |
| `--shared` | 编译为共享库 (.so)，无需 main 函数 |
| `--static` | 编译为静态库 (.a)，无需 main 函数 |
| 包格式 | `myp_packages/<name>/src/<name>.myp` 或 `myp_packages/<name>/<name>.myp`，附带 `package.myp` 元数据 |
| 去重 | 同一文件不会重复导入（基于路径去重） |
| 递归 | 导入的文件中的 `import` 也会被递归加载 |
| 命名空间 | 扁平结构，无嵌套 |

---

## 10. 标准库设计

### 10.1 设计原则

标准库必须遵循 MYP 的**事件驱动风格**：
- 每个库是 **class**，通过 action/event/property 三段式定义
- **没有全局函数**——一切通过实例 + mapping
- 底层 Runtime C 函数是 **编译器 intrinsics**，不直接暴露给用户
- 用户 `import 库名;` 后创建实例，通过 action 调用或 mapping 连接

```
// ✅ MYP 风格
import env;
int main() {
    Console console;
    console.write(42);
    mapping() { worker.output -> console.write; }
}

// ❌ 非 MYP 风格（当前 transient 状态）
printInt(42);   // 全局函数——不是事件驱动
```

### 10.2 库目录布局

```
stdlib/
├── env.myp         # Console 类（write/writeString/writeLine/writeFloat/writeBool/writeLong/readString/kbhit/getch/flush）
├── timeline.myp    # Timeline / Stopwatch 类（now/sleep/elapsed/startTimeout/startInterval/startTick）
├── math.myp        # Math 类（sqrt/abs/floor/ceil/sin/cos/tan/exp/log/pow/absInt/min/max）
└── io.myp          # File I/O 类（open/close/readLine/write/hasNext）
```

> **当前状态**：所有 stdlib 文件均已实现为纯 MYP class，通过 `import` 加载。
> `static:` 区定义静态方法，无需 `new` 即可调用：`Console.writeLine("hello");`

### 10.3 `import env` — Console 类

```myp
// env.myp — 基本 I/O
class Console {
    action:
        void write(int v);              // 输出整数 + 换行
        void writeString(string s);     // 输出字符串
        void writeLine(string s);       // 输出字符串 + 换行
        void writeFloat(double v);      // 输出浮点数 + 换行
        void writeBool(bool v);         // 输出布尔值 + 换行
}
```

使用方式：

```myp
import env;

class Worker {
    action:
        @startup void run() { output(42); }
    event:
        output(int v);
}

int main() {
    Console console;
    Worker worker @thread;

    mapping() {
        worker.output -> console.write;  // 纯事件驱动
    }
    return 0;
}
```

### 10.4 `import timeline` — Timeline / Stopwatch 类

```myp
// timeline.myp
class Timeline {
    action:
        long now();
        void sleep(long ms);
        long elapsed(long since);
    event:
        timeout(long ms);
        interval(long ms);
        tick();
}

class Stopwatch {
    action:
        void start();
        void stop();
        long elapsed();
    event:
        timeout(long ms);
    property:
        long startTime;
        bool running;
}
```

使用示例：

```myp
import env;
import timeline;

class Sensor {
    action: float read();
    event: readingReady(float value);
}

int main() {
    Console console;
    Timeline timer;
    Sensor sensor;

    mapping() {
        timer.interval(1000) -> sensor.read;
        sensor.readingReady -> console.writeFloat;
    }
    return 0;
}
```

### 10.5 标准库规划

| 库 | 功能 | 当前状态 |
|----|------|---------|
| `env` | `Console` 类（write/writeString/writeLine/writeFloat/writeBool/writeLong/readString/kbhit/getch/flush） | ✅ 已实现：stdlib/env.myp |
| `time` | `Time` 类（nowMs/sleep） | ✅ 已实现：stdlib/time.myp |
| `timeline` | `Timeline` / `Stopwatch` 类（定时器事件 timeout/interval/tick） | ✅ 已实现：stdlib/timeline.myp |
| `math` | `Math` 类（sqrt/abs/floor/ceil/sin/cos/tan/exp/log/pow/absInt/min/max） | ✅ 已实现 |
| `io` | `File` 类（open/close/readLine/write/writeLine/hasNext + 二进制 r/w） | ✅ 已实现 |
| `stream` | 流式数据源（RangeStream/IntStream/DoubleStream） | ✅ 已实现 |
| `collections` | `ArrayList<T>` 动态数组（固定容量 1024）、`HashMap<K,V>` 哈希表（线性探测，容量 1024）、`Set<T>` 哈希集合、`Queue<T>` 队列 | ✅ 已实现 |
| `text` | `StringBuilder` 字符串构建器 | ✅ 已实现 |
| `atomic` | `Atomic` 类（addInt/subInt/xchgInt/addDouble/loadInt/storeInt），基于 LLVM atomicrmw | ✅ 已实现 |
| `random` | `Random` 类（init/next/below） | ✅ 已实现 |
| `pool` | `Parallel` 静态类（线程池任务工具） | ✅ 已实现 |
| `barrier` | `Barrier` 类（create/wait/destroy），基于 pthread_barrier | ✅ 已实现 |
| `future` | `Future` 类（create/set/get/destroy），异步结果容器 | ✅ 已实现 |
| `coro` | `Coro` 协程类（create/resume/yield/isActive/destroy），基于 ucontext | ⚠️ 实验性 |
| `memory` | `Memory` 类（alloc/free/realloc），直接调用 C malloc | ✅ 已实现 |
| `test` | `Test` 类（assert/assertEq/assertStrEq/report），配合 `@test` 注解 | ✅ 已实现 |
| `sdl` | `SDL` 图形类（init/quit/clear/present/getKey），基于 SDL2 FFI | ✅ 已实现 |
| `ui` | 终端 TUI 框架（Window/Label/Button/TextBox/ProgressBar），纯 MYP 实现，基于 ANSI escape codes 渲染 | ✅ 已实现：stdlib/ui.myp |

### 10.6 编译器 intrinsics 系统

标准库底层的 C 运行时函数通过 **编译器 intrinsics** 暴露给 MYP 代码，用户代码中直接调用 `__myp_*` 函数：

```myp
__myp_print("hello");              // 打印字符串（无换行）
__myp_print_int(42);               // 打印整数 + 换行
__myp_print_float(3.14);           // 打印浮点数
__myp_sleep_ms(100);               // 休眠 100ms
__myp_now_ms();                    // 当前时间戳
__myp_flush();                     // 刷新 stdout
__myp_term_width();                // 终端宽度
__myp_term_height();               // 终端高度
__myp_strlen("hi");                // 字串长度 → 2
__myp_chr(65);                     // ASCII 码 → 单字符串 "A"
__myp_timer_create("tick", 20, 20); // 创建周期定时器
__myp_io_fopen("file", "rb");      // 打开文件
__myp_io_read_byte();              // 读 1 字节
__myp_io_read_i32be();             // 读 4 字节大端
__myp_io_write_byte(c);            // 写 1 字节
__myp_io_write_i32be(v);           // 写 4 字节大端
__myp_io_write_double(v);          // 写 8 字节 double
__myp_io_read_double();            // 读 8 字节 double
```

intrinsics 在 `sema.cpp` 的 `registerIntrinsics()` 中注册类型签名，在 `codegen.cpp` 的 `getIntrinsic()` 中映射到 LLVM external function declarations。

### 10.7 当前状态：纯 MYP class

编译器通过 `import env;` 从 `stdlib/env.myp` 加载 `Console` 类：

```myp
import env;
Console.writeLine("hello");       // ✅ 直接调用
mapping() { ... -> Console.write; } // ✅ mapping 连接
```

无全局函数残留——所有功能已迁移完毕。

---

## 11. 实现状态

### ✅ 已完成功能

| 功能 | 版本 | 说明 |
|------|------|------|
| 基础类型系统 | 1.0 | int, double, string, bool, long 等 |
| Class + action/event/property | 1.0 | 组件模型三要素 |
| 事件触发 + 同步派发 | 1.0 | event → mapping → action |
| @thread / @threadpool | 1.0 | 每个实例独立线程 + 线程池 |
| 链式 mapping | 1.5 | 多节点链，前一个返回值传递给下一个 |
| 导入系统 | 1.5 | import + stdlib 路径搜索 |
| FFI | 1.5 | 直接调用 C 函数 |
| 泛型 | 2.0 | 类级别类型参数 + 单态化 |
| 枚举 + 模式匹配 | 2.0 | sealed enum + match 表达式 |
| Lambda 表达式 | 2.0 | 隐藏类 + __call 方法 |
| 包管理系统 (myp) | 2.0 | init/build/install/run |
| LSP 服务器 | 2.0 | 补全/悬停/跳转定义 |
| try/catch/throw | 2.1 | setjmp/longjmp 异常处理（对象异常 + finally + `catch (Error e)` 接口匹配 + 标准异常 + 库接入）|
| 共享/静态库输出 | 2.1 | --shared / --static |
| @test 测试框架 | 2.2 | --test 自动发现并运行 @test |
| myp fmt 格式化 | 2.2 | 独立格式化工具 |
| Stream 流类型 | 2.3 | RangeStream, IntStream, DoubleStream |
| mapping @scope | 2.3 | 作用域生命周期绑定 |
| mapping where | 2.3 | 条件过滤 |
| mapping lambda | 2.3 | lambda 作为链节点 |
| delay / throttle | 2.3 | 定时变换器 |
| 接口多态 | 2.3 | 基于胖指针的虚表分派 |
| 数组事件参数 | 2.3 | double[] 等作为事件参数 |
| TUI 库 (ui.myp) | 2.3 | 纯 MYP 终端 UI 框架 |
| Struct 方法完整支持 | 2.4 | 返回 struct 类型、this 关键字、兄弟方法互相调用 |
| @static 类属性外部访问 | 2.4 | `TallyData.depthDose = value` 通过类名直接读写静态属性 |
| 动态数组初始化 | 2.4 | `double[] buf = new double[n]` 正确生成分配代码 |
| BNCT Dose 引擎 | 2.4 | 完整 BNCT 蒙特卡罗模拟，mapping 事件驱动，HDF5 截面加载 |
| work-stealing 线程池 | 2.4 | `runtime.c` 16 线程 work-stealing 池 + codegen 接入（变量捕获 struct/直接函数调用） |
| @parallel for | 2.4 | 编译期循环体提取 + 线程池平分迭代 + barrier 归约 |
| 并行体变量捕获 | 2.4 | 自动捕获外层变量到 struct，通过 void* arg 传递 |
| 并行体数学函数修复 | 2.4 | emitKernelExpr 使用 myp_math_* 运行时函数替代 CUDA __nv_* |
| 并行体静态方法调用 | 2.4 | 直接 LLVM 函数调用替代内联，避免 IfStmt 返回 i64(0) |
| Atomic 操作 | 2.0 | Atomic.addDouble/Atomic.addInt 基于 LLVM atomicrmw |

### 技术栈

```
MYPLanguage/
├── myp                  # 包管理 CLI（Python 脚本：init/build/install/run）
├── CMakeLists.txt
├── docs/
│   ├── design.md
│   └── syntax-template.md
├── include/
│   └── mylang/
│       ├── AST.h
│       ├── CodeGen.h
│       ├── DiagnosticEngine.h
│       ├── Lexer.h
│       ├── Parser.h
│       ├── runtime.h
│       ├── Sema.h
│       ├── SourceLocation.h
│       ├── SymbolTable.h
│       └── Type.h
├── src/
│   ├── main.cpp
│   ├── lexer/
│   │   └── lexer.cpp
│   ├── ast/
│   │   └── ast.cpp
│   ├── parser/
│   │   └── parser.cpp
│   ├── sema/
│   │   ├── sema.cpp
│   │   ├── symbol_table.cpp
│   │   └── type.cpp
│   ├── codegen/
│   │   └── codegen.cpp
│   └── runtime/
│       └── runtime.c
├── stdlib/
│   ├── env.myp       # Console 类（I/O + 键盘）
│   ├── timeline.myp  # Timeline / Stopwatch
│   ├── math.myp      # Math 类（数学函数）
│   ├── io.myp        # File I/O 类
│   ├── collections.myp  # 集合类（ArrayList, Queue）
│   └── text.myp      # 文本处理（StringBuilder）
├── tests/
│   ├── run_tests.sh           # 回归测试框架
│   ├── fuzz_test.py           # 模糊测试
│   ├── expected/              # 预期输出
│   ├── negative/              # 负测试
│   ├── struct_linkedlist/     # struct 验证
│   ├── interface_shape/       # interface 验证
│   ├── hanoi/                 # 递归验证
│   ├── cli_args/              # 命令行参数验证
│   ├── mapping_chain/         # 事件链验证
│   ├── test_thread.myp
│   ├── test_struct.myp
│   └── test_full.myp
├── vscode-myp/          # VS Code 扩展（语法高亮 + LSP 客户端）
├── deeplearning/
│   ├── code/                  # MLP + MNIST 训练/推理
│   └── data/                  # MNIST IDX 数据集
├── docs/
│   ├── examples/              # 示例：snake, gol, dungeon, neural_net, 并行, 定时器
│   └── ...
└── build/
    ├── mypc              # MYP 编译器
    ├── myp_lsp           # MYP 语言服务器
    └── myp_viz           # Mapping 可视化工具
```

### 11.4 第一版范围（核心先行）

第一版只实现语言核心最小集：

```
Lexer    → 完整词法（含全部类型和关键字）
Parser   → 完整语法解析
Sema     → 符号表 + 类型检查
CodeGen  → 表达式 + 控制流 + 函数 + 基本 class（不含 event/mapping 运行时）
Runtime  → print/println + 基本运行时
```

### 11.5 当前状态与后续计划

**当前全部已实现：**

#### 语言核心
- ✅ 完整词法/语法/语义分析
- ✅ LLVM IR 代码生成 + 链接
- ✅ `--emit-llvm` 导出 IR
- ✅ 多文件编译（合并 AST 后单次 sema/codegen 通过）
- ✅ 错误恢复（解析错误后继续）
- ✅ `-o`, `-O[0123]` 编译选项

#### Class 系统
- ✅ 三段式 class（action/event/property）
- ✅ `@startup` 自动初始化
- ✅ `function:` 内部方法段
- ✅ `static:` 静态方法段
- ✅ `struct:` 嵌套结构体段
- ✅ `interface class` 编译期接口检查
- ✅ 访问控制（property 外部不可写）

#### 类型系统
- ✅ 全部基本类型（byte/short/int/long/ubyte/ushort/uint/ulong/char/float/double/bool/string）
- ✅ `struct` 值类型 + 嵌套 struct
- ✅ 数组类型（`Type[]`, `Type[N]`）
- ✅ 数字自动提升
- ✅ 字符串拼接 `+`
- ✅ 字符串比较 `==` `!=`（内容比较）

#### 控制流
- ✅ `if/else`、`while`、`for(;;)`
- ✅ `break`/`continue`
- ✅ `return`
- ✅ 复合赋值 `+= -= *= /= %=`
- ✅ 自增/减 `++` `--`（前缀和后缀）
- ✅ 三元运算符 `? :`

#### 事件与并发
- ✅ 事件运行时 + mapping 调度器
- ✅ 事件链（A.e → B.a → C.a，返回值自动传递）
- ✅ 编译期映射环路检测
- ✅ `@thread` 多线程（pthread 独立线程 + 事件循环）
- ✅ 每线程独立事件队列（ring buffer + mutex）
- ✅ 跨线程异步事件投递
- ✅ 定时器事件系统（`__myp_timer_create`）
- ✅ 多线程独立定时器
- ✅ 线程生命周期管理

#### 标准库
- ✅ `env`：Console（write/writeString/writeLine/writeFloat/writeBool/writeLong/readString/kbhit/getch/flush）
- ✅ `timeline`：Timeline/Stopwatch（定时器事件）
- ✅ `math`：Math（sqrt/abs/floor/ceil/sin/cos/tan/exp/log/pow/absInt/min/max）
- ✅ `io`：File（open/close/readLine/write/hasNext）
- ✅ `--stdlib` 编译器选项 + 自动路径检测

#### 运行时
- ✅ 终端原始模式 + 非阻塞键盘输入（kbhit/getch）
- ✅ 二进制文件 I/O（read_byte/read_i32be/write_byte/write_i32be/write_double/read_double）
- ✅ 权重持久化（深度学习模型保存/加载）
- ✅ ARC 内存管理（每个线程独立 alloc 链表）
- ✅ `atexit` 清理

#### 测试基础设施
- ✅ 回归测试框架 `tests/run_tests.sh`（编译+运行+输出比对）
- ✅ 负测试集 `tests/negative/`（编译错误验证）
- ✅ 模糊测试 `tests/fuzz_test.py`（随机代码生成 + 编译器稳定性）

#### 验证示例
- ✅ 贪吃蛇游戏（实时键盘输入 + 帧渲染）
- ✅ 康威生命游戏（40×20 网格 + 演化）
- ✅ 地牢探险 Roguelike
- ✅ XOR 神经网络（2→2→1 推理）
- ✅ 多层感知器训练（784→64→10 MNIST 97% 准确率）
- ✅ 多线程并行计数
- ✅ 多线程独立定时器

**后续版本：**

| 版本 | 特性 |
|------|------|
| **v3** | 字符串插值 `"Hello, $name"`、类型推断 `var x = 42`、Range `0..10` | ✅ 已实现 |
| **v3** | `myp viz` 可视化工具（mapping 关系图 + Graphviz） | ✅ 已实现 |
| **v3** | `--trace` 运行时事件追踪 | ✅ 已实现 |
| **v4** | 泛型（含 monomorphization：`Box<int>`/`Box<double>` 独立代码生成） | ✅ 已实现 |
| **v4** | 枚举 + 模式匹配 | ✅ 已实现 |
| **v4** | Lambda/闭包（`(int x) => { return x*2; }` 编译为隐藏类 `__lambda_N`） | ✅ 已实现 |
| **v5** | FFI | ✅ 已实现 |
| **v5** | 包管理器（myp init/build/install/run + --package-path 导入搜索） | ✅ 已实现 |
| **v5** | LSP 语言服务器（诊断/补全/悬停/符号/跳转定义/引用查找） | ✅ 已实现 |
| **v5** | VS Code 扩展（语法高亮 + LSP 集成） | ✅ 已实现 |
| **v5** | 错误处理（try/catch/finally/throw + 对象异常 + `catch (Error e)` 接口匹配 + `throw;` 重抛 + 标准异常 + 库接入；setjmp/longjmp）| ✅ 已实现（详见 `exceptions.md`）|
| **v5** | 共享库/静态库输出（--shared/--static） | ✅ 已实现 |
| **v5** | 内置测试框架（@test + --test 标志 + 断言内置函数） | ✅ 已实现 |
| **v5** | myp fmt 格式化工具（token 级格式化 + 注释保留） | ✅ 已实现 |
| **v5** | 标准库扩充（HashMap、Set、Math、Time、Random、File I/O、Atomic 等） | ✅ 已实现 |
| **v2.4** | 协程 `@coro` — 基于 ucontext 的用户态纤程，每线程可承载数万协程；`@coro` 方法 + `await` 挂起/恢复 + 入口参数槽 + 手动 `resume` + `await` 值传递（`int v = await expr;`）+ 返回值槽 + 自动调度器（就绪队列 + `__myp_coro_scheduler`）+ 事件等待（`await ClassName.eventName`）| ✅ C1-C4 已实现 |
| **v2.4** | Barrier 同步 — pthread_barrier 封装，多 epoch 并行 | 🔜 规划中 |
| **v2.4** | Future/Promise — 异步结果容器，future.get() 阻塞等待，promise.set() 唤醒等待者 | 🔜 规划中 |
| **v6** | **Event-driven Pool (方案 B)** — 事件驱动的工作分发池：Pool 持有工作窃取队列 + N 个 Worker 线程，通过 mapping 接收任务 → 自动分派给空闲 Worker → 结果事件汇总到 Tally。纯运行时方案，不改编译器 | 🔜 规划中 |
| **v2.4** | `@parallel for` (方案 A) — 编译期将循环体提取为独立函数，由线程池平分迭代执行，barrier 归约。零事件开销，天然负载均衡 | ✅ 已实现 |
| **v2.4** | 并行体变量捕获 — 自动捕获外层变量到 struct，通过 void* arg 传递 | ✅ 已实现 |
| **v2.4** | 并行体数学函数修复 — emitKernelExpr 使用 myp_math_* 代替 CUDA __nv_* | ✅ 已实现 |
| **v2.4** | 并行体静态方法调用 — 直接 LLVM 函数调用代替内联 | ✅ 已实现 |
| **v2.0** | Atomic 操作 — `Atomic.addDouble`/`Atomic.addInt` 基于 LLVM atomicrmw | ✅ 已实现 |
| **v2.4** | 工作窃取线程池 — 共享 work-stealing 队列 + codegen 接入 | ✅ 已实现 |
| **v2.4** | 事件队列优化 — 条件变量唤醒替代 1ms 空轮询；可伸缩队列替代固定 1024 环缓冲 | 🔜 规划中 |
| **v6** | Barrier / Future / Promise 的 MYP 层 stdlib 封装 — 基于现有 C 运行时提供 MYP 原生 API：`Barrier b = new Barrier(n)`, `Future<int> f` | 🔜 规划中 |
| **v6** | `long` 字面量后缀 — `152917L` 解析为 long 类型，避免大整数隐式转换溢出 | 🔜 规划中 |
| **v6** | Class 级 `const` — `const double THERMAL_E = 0.0253;` 在 class 体内生效，用于物理常量 | 🔜 规划中 |
| **v6** | Range for 循环 — `for i in 0..n { }` 替代 `for (int i = 0; i < n; i = i + 1)` | 🔜 规划中 |
| **未来** | 自举、JIT、宏/元编程、神经形态后端 |

---

## 12. 附录：EBNF 语法

### 词法规则

```
LineComment      ::= "//" {.} newline
BlockComment     ::= "/*" {.} "*/"
Identifier       ::= (Letter | "_") { Letter | Digit | "_" }
IntegerLiteral   ::= "0" ("x" | "X") HexDigit { HexDigit }
                   | Digit { Digit }
FloatLiteral     ::= Digit { "." Digit } [ ("e" | "E") ["+" | "-"] Digit { Digit } ]
CharLiteral      ::= "'" ( Char | Escape ) "'"
BoolLiteral      ::= "true" | "false"
StringLiteral    ::= '"' { Char | Escape } '"'
NullLiteral      ::= "null"

Letter           ::= "A".."Z" | "a".."z"
Digit            ::= "0".."9"
HexDigit         ::= Digit | "A".."F" | "a".."f"
Escape           ::= "\" ( "n" | "t" | "e" | "\" | '"' | "'" | "0" )
```

### 语法规则

```
Program          ::= { ImportDecl | StructDecl | ClassDecl | InterfaceDecl | MappingDecl | FuncDef }

ImportDecl       ::= "import" ( Identifier | StringLiteral ) ";"

StructDecl       ::= "struct" Identifier "{" { VarDecl | FuncDef } "}"
                   | "struct" Identifier "::" Identifier "{" { VarDecl | FuncDef } "}"

ClassDecl        ::= "class" Identifier "{" { ClassSection } [ InterfaceClassDecl ] "}"
ClassSection     ::= ActionSection | EventSection | PropertySection
                   | FunctionSection | StaticSection | StructSection
ActionSection    ::= "action" ":" { ActionDecl }
ActionDecl       ::= [ "@" "startup" ] Type Identifier "(" [ ParamList ] ")" ( ";" | Block )
EventSection     ::= "event" ":" { EventDecl }
EventDecl        ::= Identifier "(" [ ParamList ] ")" ";"
PropertySection  ::= "property" ":" { VarDecl }
FunctionSection  ::= "function" ":" { FuncDef }
StaticSection    ::= "static" ":" { ActionDecl }
StructSection    ::= "struct" Identifier "{" { VarDecl | FuncDef } "}"
InterfaceClassDecl ::= "interface" "class" Identifier ";"

InterfaceDecl    ::= "interface" Identifier "{" { InterfaceMember } "}"
InterfaceMember  ::= ActionDecl ";" | EventDecl

FuncDef          ::= Type Identifier "(" [ ParamList ] ")" Block

ParamList        ::= Param { "," Param }
Param            ::= Type [ Identifier ]

VarDecl          ::= Type Identifier [ "=" Expr ] [ Annotation ] ";"

Type             ::= BasicType | Identifier [ "::" Identifier ] | ArrayType | "void"
BasicType        ::= "byte" | "short" | "int" | "long"
                   | "ubyte" | "ushort" | "uint" | "ulong"
                   | "char" | "float" | "double" | "bool" | "string"
ArrayType        ::= Type "[" [ IntegerLiteral ] "]"

Statement        ::= VarDecl | ExprStmt | IfStmt | WhileStmt | ForStmt
                   | ReturnStmt | BreakStmt | ContinueStmt | Block | MappingStmt
ExprStmt         ::= Expr ";"
IfStmt           ::= "if" "(" Expr ")" Block [ "else" Block ]
WhileStmt        ::= "while" "(" Expr ")" Block
ForStmt          ::= "for" "(" [ VarDecl ] ";" [ Expr ] ";" [ Expr ] ")" Block
ReturnStmt       ::= "return" [ Expr ] ";"
BreakStmt        ::= "break" ";"
ContinueStmt     ::= "continue" ";"
Block            ::= "{" { Statement } "}"

Expr             ::= Assignment
Assignment       ::= Conditional { ("=" | "+=" | "-=" | "*=" | "/=" | "%=") Assignment }
Conditional      ::= LogicalOr [ "?" Expr ":" Conditional ]
LogicalOr        ::= LogicalAnd { "||" LogicalAnd }
LogicalAnd       ::= Equality { "&&" Equality }
Equality         ::= Relational { ("==" | "!=") Relational }
Relational       ::= Additive { ("<" | ">" | "<=" | ">=") Additive }
Additive         ::= Multiplicative { ("+" | "-") Multiplicative }
Multiplicative   ::= Unary { ("*" | "/" | "%") Unary }
Unary            ::= ("!" | "-" | "++" | "--") Unary | Postfix
Postfix          ::= Primary { "." Identifier | "[" Expr "]" | "(" [ ExprList ] ")"
                               | "++" | "--" }
Primary          ::= Literal | Identifier | "(" Expr ")"
                   | "new" Identifier "(" [ ExprList ] ")" | "this"
Literal          ::= IntegerLiteral | FloatLiteral | CharLiteral
                   | BoolLiteral | StringLiteral | NullLiteral
ExprList         ::= Expr { "," Expr }

MappingDecl      ::= "mapping" "(" ")" "{" { MappingStmt } "}"
MappingStmt      ::= MappingNode "->" MappingTarget ( "," MappingTarget )* ";"
MappingNode      ::= Identifier "." Identifier
MappingTarget    ::= MappingNode { "->" MappingNode }

Annotation       ::= "@" Identifier
```

---

## 13. 附录：完整示例

### 温度和报警系统

```myp
// ===== sensor.myp =====
class TemperatureSensor {
    action:
        void init(int id);
        float readValue();
    event:
        valueRead(float temp);
    property:
        int sensorId;
        float lastValue;
    interface class ISensor;
}

// ===== alarm.myp =====
class Alarm {
    action:
        void trigger(string level, float value);
        void silence();
    event:
        alarmTriggered(string message);
    property:
        string currentLevel;
}

// ===== display.myp =====
class Display {
    action:
        void showMessage(string msg);
        void showWarning(string msg);
    event:
        displayUpdated();
    property:
        string title;
}

// ===== main.myp =====
import env;
import sensor;
import alarm;
import display;

int main(int argc, string[] argv) {
    TemperatureSensor ts;
    Alarm alarm;
    Display disp @thread;

    // 实例级映射
    mapping() {
        ts.valueRead -> alarm.trigger -> disp.showMessage;
        alarm.alarmTriggered -> disp.showWarning;
    }

    return 0;
}
```
