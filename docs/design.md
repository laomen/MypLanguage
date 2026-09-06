# MYP 语言设计文档

> 版本: 3.16 | 日期: 2026-09-07

---

## 目录

1. [概述](#1-概述)
2. [设计哲学](#2-设计哲学)
3. [范式分析](#3-范式分析)
   - 3.5 [接口多态](#35-接口多态)
4. [语法规范](#4-语法规范)
5. [类型系统](#5-类型系统)
6. [Class 系统](#6-class-系统)
7. [事件与 Mapping](#7-事件与-mapping)
8. [并发模型](#8-并发模型)
9. [导入系统](#9-导入系统)
10. [标准库设计](#10-标准库设计)
11. [元编程](#11-元编程)
12. [实现状态](#12-实现状态)
13. [附录：EBNF 语法](#13-附录ebnf-语法)
14. [附录：完整示例](#14-附录完整示例)

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

除 `event→action` 外，`mapping()` 链支持 **action→action 事件传导**：`a.event -> b.process -> c.onResult`——前一个 action 的返回值自动作为下一个 action 的入参（§7.3 事件链）；同一事件可并行接多个目标 `a.event -> b.show, c.log`（§7.4 多目标映射），还可在链中插入 lambda 变换、`delay`/`throttle`、`where` 过滤等节点。

---

## 2. 设计哲学

### 2.1 事件驱动组件

MYP 的 class 是一个 **组件单元**，包含四个主要段：

```
class 组件名 {
    action:     // 可被调用的方法（有返回类型）
    event:      // 可触发的事件（无返回类型）
    property:   // 内部状态（成员变量）
    function:   // 内部方法（不参与 mapping，仅供类内调用）
}
```

> 另有 `static:`（静态方法，无需实例，`import` 即可调用）与 `struct:`（嵌套结构体）段，见 §6.2 段规则。

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
| **天然并发** | class 组件状态天然隔离，`@thread` 让组件独立运行；share-nothing 场景无需加锁（确需共享状态用 `import sync` 原语，v3.9+） |
| **高度可测试** | 每个 class 是独立单元：注入事件 → 断言 action 输出，无需 mock 框架；配合 `@test` 内建测试框架（v2.2+） |
| **数据流管线** | 事件链 `A.e -> B.a -> C.a`（含 action→action 传导）天然形成数据处理管道 |

### 3.2 缺点

| 劣势 | 说明 | 现状 |
|------|------|------|
| **简单事情变复杂** | 简单逻辑若坚持组件化需 class + event + action + mapping | ⚠️ 已缓解：顶层函数/`static:` 方法可直接写过程式逻辑（Dual Paradigm） |
| **控制流不直观** | mapping 多了可能形成事件循环（`a→b→c→a`），代码中不易察觉 | ✅ 已缓解：编译期环路检测（§3.3） |
| **调试困难** | 事件是异步跳转的，传统断点难以追踪"谁触发了谁" | ⚠️ 已缓解：DAP 调试器（v3.7，`myp_debug`，VS Code 断点/单步/变量） |
| **纯计算别扭（仅指事件驱动风格）** | 用 event→action 表达矩阵/图像等计算密集循环确实别扭 | ✅ 非语言缺陷：MYP 是双范式——计算直接用过程式（顶层函数/`static:`/action 体内）+ `@parallel for`（v2.4）/`@gpu for` 数据并行，matmul 性能持平 C++（§8.8） |
| **运行时开销** | 事件分发需查 mapping 表 + 参数打包 + 消息分发，比直接函数调用慢 | ⚠️ 基本属实：同线程仍走事件队列 + 线性查表 + 间接调用（非零开销） |
| **学习曲线** | class 组件 + mapping 声明式思维需要转变编程范式 | ✅ 属实 |

### 3.3 优化策略

| 缺点 | 优化方案 | 状态 |
|------|---------|------|
| 运行时开销 | **事件分发内联**（规划）：同线程 mapping 直接内联为函数调用 | 🔜 规划中，未实现——当前同线程 dispatch 仍走事件队列 + 线性查表（`myp_handlers[]`）+ 间接调用 |
| 事件循环 | **编译期环路检测**：静态分析 mapping 链，检测同一实例多次出现 | ✅ 已实现（sema `checkMappingCycles`，编译期警告） |
| 调试困难 | **DAP 调试器** `myp_debug`（断点/单步/变量） | ✅ 已实现（v3.7） |
| 简单任务繁琐 | 轻量语法糖（匿名 mapping、自由函数级事件绑定） | 🔜 规划中，未实现（可先用顶层函数/`static:` 方法缓解） |
| 不适合计算 | **Dual Paradigm**：action 内是完整过程式代码 + `@parallel for`/`@gpu for` 并行原语 | ✅ 已实现（过程式自 v1；并行 v2.4；`@pure` 注解 🔜 规划中，未实现） |
| 学习曲线 | **渐进式语法**：脚本→函数→class→event→@thread 分步学习 | ✅ 已实现 |

> **核心权衡**：用「简单事情变复杂」换「复杂系统变简单」。

### 3.4 适用场景

| 场景 | 适合度 | 说明 |
|------|--------|------|
| IoT / 传感器网络 | ⭐⭐⭐ 完美 | 每个传感器=class，事件驱动天然匹配 |
| GUI 应用 | ⭐⭐⭐ 很好 | 按钮点击→事件→动作，比 callback 优雅 |
| 游戏逻辑 | ⭐⭐⭐ 很好 | 实体组件系统(event→action)自然适配 |
| 微服务/事件驱动架构 | ⭐⭐⭐ 很好 | mapping 就是服务编排层 |
| **数值计算/算法** | ⭐⭐⭐ 很好 | `@parallel for` 线程池并行 + `@gpu for` NVPTX 卸载 + SIMD 向量化（matmul 持平 C++）；计算循环不用事件驱动，直接写数据并行 |
| **系统编程/驱动** | ⭐⭐ 好 | 事件驱动适合硬件中断处理 |
| **CLI 工具** | ⭐⭐ 可用 | 有顶层函数/`static:` 方法，过程式可直接写（"杀鸡用牛刀"只针对强行组件化） |
| **Web 后端 CRUD** | ⭐⭐ 可做 | 请求-响应模型非事件驱动甜区，但 `net.myp`/`http.myp` + 手写 HTTP server + `@coro` 并发可支撑；TLS/DB 需 FFI 或外部服务 |

### 3.5 接口多态

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

> **示例应用**：每个算子前向保存输入、反向用链式法则计算梯度，即构成声明式**自动微分**（见 `examples/ad.myp`）——注意 AD 是接口多态的一个**应用示例**，并非语言特性本身。

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
| 整数 | `42`, `0xFF`, `0b1010`, `0o17`, `1_000_000`, `42L`/`0xFFu`（后缀 `l`/`u` 亦可；下划线可作数字分隔符，编译期剥离） |
| 浮点 | `3.14`, `1.0e-5`, `1e3`, `1.5f`（`f`/`F` 后缀 = float32，仅浮点字面量） |
| 布尔 | `true`, `false` |
| 字符 | `'A'`, `'\n'`, `'\0'`（单引号，u8 语义） |
| 字符串 | `"hello"`（双引号；转义 `\n` `\t` `\r` `\e`(ESC) `\0` `\"` `\'` `\\`） |
| 空值 | `null` |

### 4.2 关键字

```
class  action  event  property  interface  import  mapping  struct  function
static  if  else  while  for  return  break  continue  true  false  null
this  new  void  var  enum  match  ffi  try  catch  finally  throw  where
await  const  ref  operator  macro  nonlocal  bitfield
byte  short  int  long  ubyte  ushort  uint  ulong  uint8  uint16  uint32
uint64  int8  int16  int32  int64  char  float  double  float4  double2
int4  bool  string  bit  bitvector
```

### 4.3 变量声明

```
int number;
string name = "MYP";
float pi = 3.14;
bool flag = true;
int[] arr;                  // 数组
ClassName obj;              // 用户类型
var count = 42;             // 类型推断（v2+，仅局部变量）
const int MAX_N = 100;      // 常量（须初始化）
ClassName worker @thread;   // 独立线程实例
Worker[4] pool @threadpool; // 线程池实例数组
int a, b, c;                // 多声明符
```

- 显式类型 `类型 名称 [= 值];`——未初始化时按类型默认值初始化（数值 0 / `false` / 空引用）
- `var 名称 = 值;` 类型推断（v2+，仅局部变量，不能用于参数/属性）
- `const 类型 名称 = 值;` 常量（必须初始化；`property:` 段 / class 顶层 / 局部变量均可）
- 变量注解：`@thread`（独立线程实例）、`@threadpool`（实例数组线程池）；`@weak` 弱引用（仅限 class/interface 引用字段）
- 支持多声明符 `Type a, b;` 与解构 `(int x, string y) = f();`（元组，见 §5 语法）

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

T id<T>(T x) { return x; }                  // 泛型函数（v3.x，单态化）
int add2(int a, int b = 100) { return a + b; }  // 默认参数（v3.x）
(int) -> int add1 = (int x) => { return x + 1; };  // 函数类型 + lambda（v3.x）
@coro long worker(long n) { return n + 1; }   // 顶层 @coro 协程函数
@test void t1() { /* ... */ }                 // @test 测试函数
@region void r() { /* ... */ }                // 内存 region 函数
```

- 无 `func` 关键字；返回类型 + 函数名 + `(参数)` + 函数体；参数：`类型 名称`
- 泛型函数 `T foo<T>(T x)` 按类型实参单态化（`foo_int_inst`），模板本身不生成运行时代码
- 默认参数可省略；命名实参 `name = value` 可乱序（`add(1, c = 5)`），详见 grammar §四-1
- 一等函数：函数类型 `(A, B) -> R` + lambda `(params) => { body }`（运行时为胖指针 `{closure, call_fn}`，按值捕获外层局部）
- 函数注解：`@test` / `@region` / `@coro(stack=N)`；`main()` 规则——只做“接线”（创建实例 + mapping），不直接调用方法/访问属性，逻辑都在 class action 中

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
for (;;) { /* ... */ }                        // 空子句
for (int x in coll) { /* ... */ }             // for-in 集合迭代（§四-2）
for (i in 0..5) { /* ... */ }                 // 无括号 range（右开 i<5）
@parallel for (int i = 0; i < n; i = i + 1) { /* ... */ }  // 数据并行（work-stealing 线程池）
@gpu for (long i = 0L; i < n; i = i + 1L) { /* ... */ }   // GPU 卸载（CUDA，无 GPU 回退 CPU）

break;
continue;
return expr;

match (status) {                              // 枚举模式匹配
    State.Ready -> { /* ... */ }
}

try { /* ... */ } catch (e) { /* ... */ } finally { /* ... */ }  // 异常处理
throw "msg";
await Signal.go;                              // 协程挂起（仅 @coro 上下文）

mapping() {                                   // 局部 mapping（事件总线）
    sensor.valueRead -> display.show;
}
```

- 条件表达式必须带括号 `()`；函数体必须用花括号 `{}`
- for-in 迭代源：定长数组 `T[N]` / `slice<T>` / 集合类（`size()`+`get(int)`）/ range `a..b`；动态数组 `T[]` 不可迭代（无运行时长度）
- `@parallel for`：编译器提取循环体到线程池（无 `break`/`continue`、迭代须独立）；`@gpu for`：卸载到 CUDA（无 GPU 自动回退顺序 CPU，结果一致）
- `match` 按枚举变体匹配，变体可绑定数据 `(v1, v2, …)`
- `try/catch/finally/throw` 基于 setjmp/longjmp；`catch (e)` 兜底 / `catch (Type e)` 按类型 / `catch (Error e)` 接口匹配；`throw;` 在 catch 内重抛
- `await` 仅限 `@coro` 方法/顶层 `@coro` 函数；形态见 §8 协程（`await expr` / `await Class.event timeout N` 等）
- 表达式 try：`var n = try expr catch (e) default;`（失败给默认值）

### 4.6 表达式与运算符

| 优先级 | 类别 | 运算符 | 结合性 |
|--------|------|--------|--------|
| 15 | 赋值 | `=` `+=` `-=` `*=` `/=` `%=` | 右 |
| 14 | 管道 | `\|>` | 左 |
| 13 | 三元 | `? :` | 右 |
| 12 | 逻辑或 | `\|\|` | 左 |
| 11 | 逻辑与 | `&&` | 左 |
| 10 | 按位或 | `\|` | 左 |
| 9 | 按位异或 | `^` | 左 |
| 8 | 按位与 | `&` | 左 |
| 7 | 等值 | `==` `!=` | 左 |
| 6 | 关系 | `<` `>` `<=` `>=` | 左 |
| 5 | 移位 | `<<` `>>` | 左 |
| 4 | 加法 | `+` `-` | 左 |
| 3 | Range | `..` | 左 |
| 2 | 乘法 | `*` `/` `%` | 左 |
| 1 | 一元 | `!` `~` `-` `++` `--` | 右 |
| 0 | 后缀 | `.` `[]` `()` `++` `--` | 左 |

> 优先级与 C 家族一致（数值越大越宽松）：`..`（range）位于加减与乘除之间；`~` 为按位取反（整型/bitvector/bit）；一元 `++`/`--` 亦出现在后缀列（`a++`/`a--`）。

#### 4.6.1 运算符注解（`@op` 算子绑定）

MYP 的运算符扩展采用**注解式绑定**，特意不叫"运算符重载"以区别于 C++——C++ 用 `operator+` 成员/友元函数名重载；MYP 用 `@op("符号")` 注解把方法/函数绑定到运算符字符串，且只作用于**值类型**。核心理念是「**运算符 = 算子**」：内建运算符是编译器预置的标量算子，用户通过统一机制定义自己的算子。

**三层设计**：

| 层 | 机制 | 适用 |
|----|------|------|
| ① struct 内算子 | struct `operator:` 节 + `@op("+")` 注解 | 值类型的数学运算（Vector/Matrix/Color 本就是值） |
| ② 外部 `@op` 函数 | 顶层 `@op("+")` 函数（显式双操作数） | 内置类型 / 对称二元（`s * v`）/ 跨模块 |
| ③ 变换组件 | class + action + 接口，走 `mapping` / `\|>` 管道 | 组件流水线（`A |> ScaleOp`） |

**值用算子，组件用事件**：struct（值类型）用 `operator:` 节 + `@op` 绑定；class（组件）**不提供** `+` 重载——`a + b` 返回新值，而 class 是引用，重载会引入「突变 vs 返回新对象」歧义；且强制「想用 `+` 的类型做成 struct」恰是好的类型设计。class 改用 action/event/mapping/`|>` 管道。

**元数与操作数形态**：

| 元数 | struct 内算子 | 外部 `@op` 函数 |
|------|---------------|----------------|
| 一元 | `@op("-") T neg() { ... }`（this = 操作数） | `@op("-") double negate(double x)` |
| 二元 | `@op("+") T add(T other)`（this = 左操作数） | `@op("+") T add(T a, T b)`（显式双参） |

**集合提升**：元素级算子（逐元素 + 标量广播）可自动提升为集合算子（`slice<T>` 的 `A + B`、`A * k`）；结构级算子（如矩阵乘、点积）必须显式声明为普通函数，**不参与** `+` 重载的自动提升。

**符号绑定与解析顺序**：`@op("+")` 注解参数为运算符字符串（`"+"` `"-"` `"*"` `"/"` `"=="` `"!="` `"<"` `">"` `"<="` `">="`）。遇到 `a + b` 时按 ① 内建运算符（标量热路径，永远最先）→ ② 左操作数类型上定义的 struct `@op("+")` → ③ 外部 `@op("+")` 函数（按签名匹配）解析；`double + double` 永远走内建，性能不降。

```myp
// ① struct operator: 节（值类型数学算子；this = 左操作数）
struct Vector3 {
    operator:
        @op("+") Vector3 add(Vector3 other) {
            Vector3 r;
            r.x_ = x_ + other.x_; r.y_ = y_ + other.y_; r.z_ = z_ + other.z_;
            return r;
        }
        @op("*") Vector3 mul(double s) {
            Vector3 r;
            r.x_ = x_ * s; r.y_ = y_ * s; r.z_ = z_ * s;
            return r;
        }
    double x_ = 0;          // struct 属性每行一个（不支持逗号多声明符）
    double y_ = 0;
    double z_ = 0;
}

// ② 外部 @op 函数（内置类型 / 对称二元）
@op("+") double[] vecAdd(double[] A, double[] B) {   // 逐元素（double[] 只能外部定义）
    int n = 4;
    double[] C = new double[n];
    int i = 0; while (i < n) { C[i] = A[i] + B[i]; i = i + 1; }
    return C;
}
@op("*") Vector3 vecMul(double s, Vector3 a) {        // s * v 对称——struct 内挂不上
    Vector3 r; r.x_ = a.x_ * s; r.y_ = a.y_ * s; r.z_ = a.z_ * s;
    return r;
}

// ③ 变换组件（class）：不走数学算子，用 |> 管道（类名自动实例化）
interface SetOp { double[] transform(double[] A); }
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

class Main {
    action:
        @constructor Main() {
            Vector3 v; v.x_ = 1; v.y_ = 2; v.z_ = 3;
            Vector3 u; u.x_ = 4; u.y_ = 5; u.z_ = 6;
            Vector3 w = v + u;          // ① struct 数学算子：w = (5,7,9)

            double[] A = new double[4];
            double[] B = new double[4];
            double[] C = A + B;         // ② 外部算子：逐元素
            Vector3 m = 3.0 * v;        // ② 对称混合：s * v

            double[] D = A |> ScaleOp;  // ③ 组件管道：调用 transform（类名自动实例化）
        }
}

int main() {
    Main m = new Main();   // main() 只做接线（创建实例 + mapping），逻辑在 class action 中
    return 0;
}
```

> `slice<T>` 集合二元同样支持（逐元素 `A + B` / 标量广播 `A * k` / 对称 `k * A`，基于 `slice.size()` 运行时长度）。

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
| `bool` | 布尔值 | `i1` | LLVM i1——**内存占 1 字节**（i1 存储粒度 1B，不按位打包） |
| `string` | 字符串（ARC 引用计数，值语义） | `ptr` | 指针 |
| `void` | 无类型（仅函数返回） | `void` | — |
| `uint8/16/32/64`、`int8/16/32/64` | 定宽整型别名（v3.11） | `iN` | 8/16/32/64-bit |
| `float4`/`double2`/`int4` | 向量类型 | — | — |
| `bit` | 位（布尔位，`bit(x)` = x≠0） | `i1` | LLVM i1——**内存占 1 字节**（i1 存储粒度 1B，不按位打包）；亚字节按位压缩用 `bitfield` |
| `bitvector<N>` | 定宽位向量（N = 8/16/32/64，字节对齐） | `i8`/`i16`/`i32`/`i64` | N bit = N/8 字节 |

### 5.3 类型规则

- **数字提升（无损隐式 / 有损显式）**：隐式转换仅限无损——同符号拓宽（SExt/ZExt）、32 位内整型→f64、f32→f64、char→int（ZExt）。任何有损（跨符号、`i64/u64→f64`、`int/long→float`、窄化）必须显式 cast：`int(x)`/`double(x)` 等。例如 `int` 可直接传 `long`/`double` 参数，`long→int`、`double→float` 需显式。
- **无符号类型**：`ubyte`/`ushort`/`uint`/`ulong`（及 `uint8/16/32/64` 别名）有无符号语义——`>>` 逻辑右移、`/`→`udiv`、`%`→`urem`、比较用无符号谓词、加减自动按位宽回绕；`u` 后缀字面量按值定宽；同符号拓宽走 ZExt，跨符号/浮点显式。
- **`char`**：单字节字符（u8 语义），字面量用单引号：`'A'`、`'\n'`；`char→int` ZExt（`0xFF` → 255，非 -1）。

### 5.4 复合类型

```
int[]     // 整型数组
string[]  // 字符串数组
ClassName  // 用户定义的类类型
```

### 5.5 类型系统规则

- 静态类型：所有变量和表达式的类型在编译时确定
- 强类型：**无损拓宽隐式、有损转换须显式 cast**（见 §5.3）
- 类名可直接作为类型使用
- 泛型：类 / 泛型函数 / 泛型 `@static` 方法均支持（v2.1+，单态化）；另有 `Option<T>`/`Result<T,E>`、`slice<T>`、元组、函数类型（v3.9+）

---

## 6. Class 系统

### 6.1 四段式结构

class 是**组件单元**，包含四个主要段 `action:` / `event:` / `property:` / `function:`（另有 `static:` 与 `struct:` 段，见 §6.2）：

```
class Sensor {
    action:                     // 方法区（外部可调用的方法）
        void init(int id);
        float readValue();

    event:                      // 事件区（可触发的事件）
        valueRead(float temp);
        thresholdExceeded(float value);

    property:                   // 属性区（内部状态，外部不可直接读写）
        int sensorId;
        float threshold;
        float lastValue;

    function:                   // 内部方法区（不参与 mapping，仅供类内调用）
        void calibrate() { }

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

**注解：** action 前可加 `@constructor` 注解——`new` 创建实例时自动调用（对象初始化）；
当方法名==类名时默认视为构造器（可省略注解）；action 前可加 `@startup` 注解——实例的线程/
事件循环启动时执行（启动信号/开始操作，非初始化器）；变量声明后可加 `@thread` 注解，
表示该实例在独立线程运行。

**为什么没有析构函数：** MYP 无用户可写的析构函数（无 C++ 的 `~Class()`）——class 实例/
`string`/`T[]`/`slice`/struct 引用字段的释放完全由 ARC（自动引用计数）承担：引用计数归零时
编译器自动插入销毁桩（`__myp_destroy_<Class>`）确定性释放；非内存资源（文件/通道/锁/GPU 流）
按 stdlib 惯例用显式 `open/close`、`create/destroy` handle 模式管理。详见 §6.5
「为什么没有析构函数（ARC 自动管理）」。

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
- **接口是纯协议，不携带状态**：只允许声明 `action` 与 `event`（另可声明关联类型 `type Item;`，v3.9）——**没有 `property:`/字段段**（负测试 `tests/negative/interface_property.myp`）；状态由实现类持有
- 接口多态（v2.3+）：interface 变量为胖指针 `{ptr data, ptr vtable}`，方法调用经虚表分派；类通过 `interface class Iface;` 声明实现；支持默认实现（trait，v3.9）与关联类型 `type Item;`（v3.9）
- 组件间通信仍通过 `mapping()` 声明式连接，这是天然的 duck typing

#### 接口与实现不一致：编译期检查（`sema checkInterfaceImpl`）

接口检查是 **opt-in**：只有写了 `interface class X;` 的类才会被逐一核对（sema
`checkInterfaceImpl`），**不声明即不检查**——普通类可以拥有任意同名方法，互不干扰。
声明后，以下不一致情形全部在**编译期**报错：

| 不一致情形 | 编译期行为 |
|-----------|-----------|
| 接口不存在 | ❌ `interface 'X' not found` |
| 缺少接口的 action | ❌ `class 'C' does not implement action 'a' from interface 'I'` |
| 缺少接口的 event | ❌ `class 'C' does not implement event 'e' from interface 'I'` |
| action 返回类型不匹配 | ❌ 同上（action 按 名称 + 返回类型 basic_type 匹配） |
| 关联类型未绑定 | ❌ `class 'C' does not bind associated type 'T' from interface 'I'`（提示 `add type T = ...;`） |

规则细节（均为实测行为）：

- **带默认体的接口方法（trait 默认实现）可省略**：接口 action 带 `{ }` 函数体时，实现类
  可不提供同名方法，虚表回退到默认实现（负测试
  `tests/negative/interface_default_missing_abstract.myp`：纯签名方法强制实现，默认体不豁免）。
- **action 匹配是粗粒度的**：按 名称 + 返回类型 basic_type 核对（事件仅按名称）。
  ⚠️ **参数类型/个数不校验**——如接口 `double area(int a, int b)`、实现 `double area(int a)`
  也会**通过**检查（当前已知限制；后续可收紧为精确签名匹配）。
- **关联类型**：接口声明 `type Item;` 时实现类必须 `type Item = ...;` 绑定，且绑定类型须可解析。

负测试：`tests/negative/interface_incomplete.myp`（缺 action）、`interface_property.myp`
（接口含 property 段）。

### 6.4 访问控制

MYP 是**事件驱动组件**语言，访问控制规则服务于解耦目标：

| 成员 | 类内部（`this.`） | 外部（`obj.`） |
|------|------------------|---------------|
| `action:` | ✅ 可调用 | ✅ 可调用 |
| `event:` | ✅ 可触发 | ✅ 可触发（通过 mapping 或 fire 函数） |
| `function:` | ✅ 可调用（内部方法，供类内 action 复用） | ❌ **不可调用**（`cannot access function ... from outside class`） |
| `property:` | ✅ 读写 | ❌ **不允许直接访问** |

**原则**：外部代码不能直接读写类的 property，也不能调用 `function:` 内部方法。
所有跨组件数据传递必须通过 event→action 的 mapping 链完成。这保证了：
- 组件间完全解耦，没有隐式数据依赖
- 架构完全由 `mapping()` 声明可见
- 重构时只需修改 mapping，无需搜索属性使用点

### 6.5 继承、多态与构造器 / @startup 生命周期

#### 构造器（`@constructor` / 函数名==类名）——对象初始化

`new` 创建实例时**自动调用构造器**。构造器是 `action:`（或 `function:`）段里**方法名==
类名**的方法（隐式构造器，可省略注解，与 C++/Java 一致）；`action:` 段还可加 `@constructor`
注解显式标注。注意：`@constructor` 注解**只能在 `action:` 段使用**——`function:` 段里
只有同名隐式构造器可用（实测 `new V(3)` 会调用 `function: void V(int)` 构造器，`a=300`），
带 `@constructor` 注解的构造器放 `function:` 段会报错。

```myp
class Sensor {
    action:
        @constructor
        Sensor(int i, double t) { id = i; threshold = t; }
        void Sensor() { id = 0; threshold = 0.0; }   // 隐式（函数名==类名）
        float readValue();
    property:
        int id;
        double threshold;
}

int main() {
    Sensor s = new Sensor(1, 100.0);  // 自动调用构造器 Sensor(int, double)
    return 0;
}
```

- `new ClassName(args)` 绑定匹配的构造器（重载解析）；**构造器无返回类型**（`@constructor`
  后直接是类名，带返回类型的写法报错）。
- **无匹配时报错**：类定义了构造器但实参无匹配时，`new C(...)` 报
  `no matching constructor for 'C(...)'`；仅当类**未定义任何构造器**时才走默认
  （分配 + property 默认值）。
- **构造器重载**：同一类可定义多个不同签名的构造器，`new` 按实参**自动选择**——
  参数**个数**不同按个数区分（`Vec(int)` vs `Vec(int,int)` 精确无歧义）；**类型**不同
  按类型匹配（含数字提升，`int` 实参可匹配 `double` 参数）。
  ⚠️ **数字提升参与时可能判歧义**：`int` 字面量同时可匹配 `(int)` 与 `(double)` 两个
  构造器时，编译器**不区分「精确匹配优先于提升」**，直接报
  `ambiguous constructor call`——需用对应类型字面量（`new Vec(1.0)`）或显式 cast
  （`new Vec(double(1))`）消歧。
- **拷贝构造器（显式）**：可定义 `@constructor C(C other)` 作为拷贝构造器，
  `new C(existingInstance)` 按重载解析调用它（实测：`new V(v1)` → 调用 `V(V)`）。
  但 **class 赋值不是拷贝**——class 是 ARC 引用语义，`C b = a;` 只是**别名**
  （引用计数 +1，指向同一对象，改 `b` 会反映到 `a`），不会自动触发拷贝构造器；
  深拷贝需**显式** `new C(existing)` 或用 `copy()` 约定方法。struct 为值类型，赋值天然
  逐字段值拷贝，无需拷贝构造器。
- **右值实参（函数返回的临时对象）**：`new C(makeInstance())` 可传函数返回的临时右值作
  构造器实参（实测 `new V(makeV())` → 匹配 `V(V)`，构造器内 `other.a` 读正确）——ARC 下
  临时对象在 `new` 语句**期间存活**，构造器内**借用访问安全**。
- 构造器是**对象初始化**（`new` 时同步执行）：设字段、分配资源、校验。
- 泛型 `new Box<double>(1.5)` 绑定单态化实例类的构造器，`T` 正确解析为 double。
- struct 用**函数式构造** `Vec2(1.0, 2.0)`；struct
  **不支持 `@constructor` 注解**（实测报错）。

#### `@startup` —— 启动信号（开始操作），不是初始化器

`@startup` 标注"实例开始操作"的入口，在并行/事件驱动代码中当实例的线程/事件循环
启动时执行（如 `@thread` 启动、启动定时器、触发首事件）：

```myp
class Worker {
    action:
        @startup void run() {          // 启动信号：目标线程启动时执行
            t.startInterval(2000);     // 开始操作（启动定时器、进入循环...）
        }
    property:
        Timeline t;
}
```

- 对 `@thread` 实例：`@startup` 在**目标线程**执行（线程入口）。
- **`@thread` 实例的 `@startup` 不能手动调用**：由运行时在目标线程自动触发，手动调用
  报 `cannot manually call '@startup' method ... on a @thread instance`；
  **非 `@thread` 实例的 `@startup` 可手动调用**（实测通过）。
- `@startup` 注解只能加在**普通 action 名**上，不能加在构造器/类名上（与构造器互斥）。
- **一个类内多个 `@startup`**：编译允许（无诊断），但运行时**只取其一**且两条路径
  **不一致**——`@thread` 实例启动取**最后一个** `@startup` 为线程入口（codegen 覆盖式），
  而 `mypc run` 合成 main 调用**第一个**（实测）。建议一个类只声明一个 `@startup`。
- **`mypc run` 自动 main**：无用户 `main` 时，恰好**一个类**带 `@startup` 才注入合成
  main（`ClassName c = new ClassName(); c.startupAction(); return 0;`）；**多个类**带
  `@startup` 或全无 → 报错 `run: 无 'main' 函数且有多个类带 '@startup'——请显式定义
  main()`。
- 语义类比：Java `Runnable.run()` / Actor `preStart`——不是构造器。
- **构造器管初始化，`@startup` 管开始操作**：两者正交、互不取代。

> v3.9 起迁移到构造器（`@constructor` 注解或函数名==类名），`@startup` 严格只作启动信号
> （历史设计详见本节构造器/@startup 两小节）。

#### 为什么没有析构函数（ARC 自动管理）

MYP **没有用户可写的析构函数**（无 C++ 的 `~Class()`），对象的释放完全由 **ARC
（自动引用计数，见 §5.3）** 承担：

- **内存自动回收**：class 实例 / `string` / `T[]` / `slice<T>` / struct 引用字段全部
  引用计数；引用计数归零时由编译器自动插入的**销毁桩**（`__myp_destroy_<Class>`，
  经按 type_id 分派的 `__myp_release_table` 分发）释放对象——**内存生命周期无需
  用户干预**。
- **确定性**：ARC 是确定性的——引用计数在赋值点/作用域退出处归零即释放，与代码点
  精确对应。这替代了 C++ 手动 `delete` 与拷贝构造/赋值（三/五法则）的负担和错误源，
  也不引入 GC 的不确定回收时机。
- **非内存资源走显式 RAII**：文件句柄、网络连接、锁、通道等**非内存资源**由 stdlib
  以显式 `open/close`、`create/destroy` 包装类管理（handle 模式，幂等），如
  `File.close()`、`Channel.destroy()`、`sync` 的 `Mutex.create/destroy`、GPU
  `Stream.destroy()`——资源生命周期**显式可见**，不靠析构副作用。
- **与事件驱动/组件模型一致**：class 是组件，生命周期由引用计数 + `@thread` 归属决定。
  引入析构会引入"析构在哪个线程/时间线执行"的语义歧义与跨线程竞争；ARC 销毁桩由
  编译器在引用归零处**确定性插入**，从根上回避该问题。
- **对称性**：初始化用 `@constructor`（对象初始化）与 `@startup`（开始操作）；释放
  完全交给 ARC，无需用户配对编写析构。相关机制：`@weak` 弱引用在目标对象释放时
  **自动置空**，避免悬垂指针。

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

函数内 mapping 的节点使用**类名**（`Class.event` / `Class.action`）：

```
int main() {
    Sensor s;
    Display d;

    mapping() {
        Sensor.valueRead -> Display.showTemperature;
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
        Sensor.dataReady -> Log.write;
    }
}  // ← 函数退出时 handler 自动解注册
```

#### 7.4.2 条件过滤 where

在事件源后加 `where 表达式`，只有满足条件的事件才会被转发：

```
mapping() {
    Source.valueEmitted where value >= 3 -> Console.write;   // 只转发 >=3 的
}
```

`where` 表达式可使用事件参数名（如 `value`），支持完整的比较和算术运算。

#### 7.4.3 Lambda 变换节点

在 mapping 链中用 lambda 表达式做内联数据变换：

```
mapping() {
    Source.valueEmitted -> (int v) => { return v * 2; } -> Console.write;
    Source.valueEmitted where value % 2 == 0 -> (int v) => { return v * 10; } -> Output.save;
}
```

#### 7.4.4 定时变换器

- `delay(ms)` — 事件转发前阻塞等待指定毫秒
- `throttle(ms)` — 限频：间隔内到达的事件丢弃

```
mapping() {
    Sensor.valueEmitted -> delay(100) -> Display.update;     // 延迟 100ms
    Sensor.valueEmitted -> throttle(50) -> Logger.write;      // 50ms 限频
}
```

### 7.5 完整 Mapping 语法

```
// 节点一律用类名（Class.event / Class.action）
mapping() [@scope] {
    ClassA.event  [where 条件表达式] -> ClassB.action -> ClassC.action, ...;
    ClassA.event2 -> lambda -> ClassB.action;
    ClassA.event3 -> delay(ms) -> ClassB.action;
    ClassA.event4 -> throttle(ms) -> ClassB.action;
}

- 一个事件可以映射到多个动作
- 多个事件可以映射到同一个动作
- mapping 在运行时建立事件总线，事件触发时自动分发到所有绑定的动作

---

## 7.6 事件时间线 (Event Timeline)

理解事件如何在时间线上流动是利用好 MYP 事件驱动模型的关键。

### 7.6.1 事件的生命周期

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

### 7.6.2 时间线 = 线程

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

### 7.6.3 同时间线 = 同步

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

### 7.6.4 跨时间线 = 异步

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

### 7.6.5 时间线隔离规则

| 规则 | 说明 |
|------|------|
| **数据归属** | 每个 `@thread` 实例的数据只被自己的时间线访问 |
| **通信唯一通道** | 跨时间线通信只能通过 `mapping()` + 事件 |
| **无需加锁** | 没有显式锁——时间线隔离本身就是并发安全 |
| **fire 即发即忘** | fire 后不依赖返回值，结果通过后续事件链返回 |

### 7.6.6 时间线可视化

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

### 7.6.7 与 Actor 模型的对应

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
- **只读契约**：同线程共享引用默认可改；如需「接收方不改」，用**接口只读视图**（只声明
  读 action，不暴露 setter/写 action）或显式 `copy()` 传副本——**不引入 const 引用**：
  const 引用无法保证跨线程安全（其他引用路径仍可并发写），且组件模型下共享可变对象本身
  是反模式；`const` 目前仅表示「不可重新赋值的常量」（见 §4.3）

### 8.5 同步原语与共享状态

**已实现（v3.9，`import sync`）**：Mutex（含递归）/ RWLock / CondVar / Semaphore / Once（handle 模式，`create`→`destroy`）。事件驱动模型仍主打无共享并发；确需跨线程共享状态时用 `@static class` 属性 + `Mutex` 保护（见 manual §9）。

#### 同步原语真实用法（`import sync`，handle 模式：`create()` → 用 → `destroy()`）

```myp
import sync;

// ① Mutex 互斥锁（含递归）
int m  = Mutex.create();            // -1 = 失败
int mr = Mutex.createRecursive();   // 可重入：同一线程可重复加锁
Mutex.lock(m);
try { /* 临界区 */ } finally { Mutex.unlock(m); }   // finally 保证异常也解锁
Mutex.tryLock(m);                   // 1=成功 / 0=被占用 / -1=非法句柄
Mutex.destroy(m);

// ② RWLock 读写锁（多读单写）
int rw = RWLock.create();
RWLock.readLock(rw);                // 读锁共享，多个读者可并存
RWLock.writeLock(rw);               // 写锁独占，等待所有读者释放
RWLock.tryReadLock(rw); RWLock.tryWriteLock(rw);  // 1/0/-1
RWLock.unlock(rw);                  // 读锁或写锁均可解锁
RWLock.destroy(rw);

// ③ CondVar 条件变量（必须与 Mutex 配合）
int cv = CondVar.create();
Mutex.lock(m);
while (!ready) { CondVar.wait(cv, m); }   // 自动释放 mutex 阻塞；signal 后重新持有
CondVar.signal(cv);                 // 唤醒一个等待者；CondVar.broadcast(cv) 唤醒全部
Mutex.unlock(m);
CondVar.destroy(cv);

// ④ Semaphore 信号量（P/V）
int sem = Semaphore.create(0);      // 初始计数 >= 0
Semaphore.wait(sem);                // P：计数 -1，为 0 则阻塞
Semaphore.post(sem);                // V：计数 +1，唤醒一个等待者
Semaphore.tryWait(sem);             // 1/0/-1
Semaphore.destroy(sem);

// ⑤ Once 单次执行（call-once）
int once = Once.create();
if (Once.enter(once) == 1) {        // 1=本线程是首个调用者（应执行初始化）
    /* 初始化 */
    Once.done(once);                // 释放锁，其余线程的 enter 返回 0 并跳过
}
Once.destroy(once);
```

> **共同约定**：全部 handle 模式——`create()` 返回 int 句柄（-1 = 失败）、用完必须
> `destroy()`（句柄槽位有限，每类 64 个，不释放会耗尽）；锁配合 `try/finally` 保证
> 异常路径也解锁；**`main()` 里不能直接调用**（main 只做接线），锁逻辑放 class
> action / `@constructor` / `@startup`。实测 `tests/sync/test.myp` 五原语全部通过
> （`sync all ok`）；跨线程共享状态推荐 `@static class` 属性 + `Mutex` 保护。

| 场景 | 正确的做法 | 错误的做法 |
|------|-----------|-----------|
| 跨线程传递数据 | `mapping() { a.event -> b.action; }` | 直接写 `b.property = value` |
| 线程间共享状态 | 用独立 `@thread` 实例管理状态，通过事件查询/修改 | 多个线程读写同一个 property |
| 归约/聚合 | 每个线程维护自己的 tally，最后用事件汇总 | 共享全局数组 + 加锁 |

事件驱动模型仍主打无共享并发；确需共享状态时用 `import sync` 原语，而非把整个架构退化成全面加锁的共享内存模型。

### 8.6 协程与异步（`@coro` / `await`）

协程是**线程内协作式并发**：`@coro` 方法/函数以用户态纤程（x86-64 寄存器级汇编切换）运行，`await` 挂起、调度器恢复，单线程内可承载大量轻量任务——与 `@thread`（抢占式独立时间线）互补。

#### 8.6.1 语法（真实可编译运行，实测输出 `start / v=100 / r=0 / result=42 / resumed`）

```myp
import env;
import coro;
import async;
import time;

class Signal {
    action:
        void send() { go(); }     // fire go 事件（裸名 → fire_Signal_go）
    event:
        go();
}

class Worker {
    action:
        // ① 无参协程：await 挂起/恢复 + 非阻塞睡眠
        @coro void run() {
            Console.writeString("start\n");
            await;                       // 简单挂起（交给调度器）
            await Async.sleep(20);       // 挂起 20ms（不阻塞线程，import async）
            Console.writeString("resumed\n");
        }
        // ② 带值挂起：int v = await expr; 恢复时 v = resume 传入值
        @coro void echo(int n) {
            int v = await n * 2;
            Console.writeString("v=");
            Console.write(v);
            Console.writeString("\n");
        }
        // ③ 等待事件 + 超时：await ClassName.event timeout N（超时返回 -1）
        @coro void waitDone() {
            long r = await Signal.go timeout 200;
            Console.writeString("r=");
            Console.write(r);
            Console.writeString("\n");
        }
        // ④ 自定义栈大小（KB，默认 1MB 动态预留）
        @coro(stack=64) void big() { await; }
}

// ⑤ 顶层 @coro 函数（无需类封装），返回值经 Coro.result(h) 取出
@coro long topLevel(long n) { return n + 1; }

class Main {
    action:
        @constructor Main() {
            Worker w = new Worker();
            long h1 = w.run();            // spawn：创建并首启到第一个 await，返回 handle
            long h2 = w.echo(5);
            long h3 = w.waitDone();
            long h4 = topLevel(41);

            Coro.resume(h1, 0);           // 恢复 run → 到 Async.sleep(20) 挂起（定时器）
            Coro.resume(h2, 100);         // 带值恢复 echo：v=100
            Signal s = new Signal();
            s.send();                     // fire go → waitDone 重新就绪
            Coro.resume(h3, 0);           // waitDone 恢复：r=0（事件到达）
            Coro.resume(h4, 0);           // topLevel 恢复并结束
            Console.writeString("result=");
            Console.write(Coro.result(h4));   // 42
            Console.writeString("\n");

            for (int i = 0; i < 3; i++) {     // 驱动定时器到期 → run 的 sleep 恢复
                Coro.scheduler();
                Time.sleep(30);
            }
        }
}

int main() {
    Main m = new Main();   // main() 只做接线，逻辑在 class action 中
    return 0;
}
```

- `@coro` 作用于**类 action 方法**和**顶层函数**；`await` 仅限 `@coro` 上下文（sema 检查）
- `await` 形态：`await;` / `await expr;` / `int v = await expr;` /
  `await ClassName.event` / `await ClassName.event timeout N`（事件等待用**类名**，
  不是实例名） / `await Async.sleep(ms)`（非阻塞睡眠，`import async`）
- 调用 `@coro` 方法/函数即创建并启动协程（spawn，跑到第一个 `await` 挂起），返回
  `long` 句柄；恢复用手动 `Coro.resume(h, val)` 或自动 `Coro.scheduler()`（每轮驱动
  所有就绪协程各一步）；返回值经 `Coro.result(h)` 取出

#### 8.6.2 运行时（寄存器级汇编纤程）

| 机制 | 说明 |
|------|------|
| 上下文切换 | x86-64 用**寄存器级汇编**（`coro_ctx.S`，无 syscall，~20-40ns）；非 x86-64 平台回退 `ucontext` `swapcontext`（~200ns）——「ucontext」为历史特性名，主流 x86-64 实际走 asm 快路径 |
| 底层原语 | `__myp_coro_*`：create/set_entry/set_entry_arg/get_entry_arg/yield/resume/set_result/result/is_active/destroy/current/count/status/wait_event/wait_event_timeout/wait_any/wait_any_of/sleep/wait_fd/request_cancel/cancel_requested/cancel_clear 等 |
| 调度 | 手动 `Coro.resume(h, val)` + 自动调度器（就绪队列，`Coro.scheduler()` 每轮驱动所有就绪协程各一步；阻塞等待事件的协程跳过） |
| 栈 | 默认 1MB 动态预留（RSS 按实际使用提交）；`@coro(stack=N)` 显式指定 64KB–64MB，`stack=128`/省略等价默认；`N<16` 仅编译警告建议 ≥16KB，过小运行期栈溢出 + 栈池复用（同尺寸 O(1) 尾取，缓存上限 1024 项 / 每线程池内 VA 总字节上限 128MB） |
| 线程绑定 | 协程状态线程本地（`__thread`）——协程绑定创建线程，可与 `@thread` 线程并用 |
| 事件集成 | `await Class.event` 阻塞等待，事件 fire 后重新就绪；事件等待表动态扩容（自 64 起倍增，无硬上限） |
| 超时/多事件 | `waitEventTimeout` / `Coro.waitAny(ids, count, timeoutMs, val)`（返回触发事件 id，超时 -1）/ `Coro.waitAnyOf(spec, …)`（P4 混等事件/定时器/fd，返回触发的 spec 下标）/ `Coro.waitFd(fd, …)` |
| 取消 | 协作式取消 `requestCancel`/`cancelRequested`/`clearCancel`（区别于 destroy 强杀） |
| 协程帧 ARC（M9） | 协程内局部 ARC 槽值镜像到帧表（`__myp_coro_frame_set/clear`）；`Coro.destroy` 强杀或未捕获异常时释放仍存活的帧对象，防泄漏 |
| 诊断 | `Coro.status(h)`（-1 无效/0 结束/1 就绪/2 等待）+ `current()`/`count()` |

#### 8.6.3 协程通信与异步 IO

| 库 | 说明 |
|----|------|
| `channel` | Go 风格**有缓冲通道**：`Channel`（init/send/recv/trySend/tryRecv/size/close/destroy）；协程内 send 缓冲满→挂起等空位、recv 缓冲空→挂起等数据（会合）；**非协程**（主流程/@thread）send/recv 满/空返 -1（不挂起）；trySend/tryRecv 永远非阻塞 |
| `async` | 统一异步 IO：`Async.sleep` 等 `@async` 方法，须在 `@coro` 内 `await f()` 调用（**非协程上下文退化为同步阻塞**）；`Coro.waitFd(fd, wantRead, wantWrite, timeoutMs)` 等待 fd 可读/可写 |
| `net` | 协程内跑 TCP：`TcpServer`/`TcpClient`；`@async` `recvAsync`/`recvLineAsync`/`sendAsync` 用 `Coro.waitFd` 驱动**非阻塞** IO（超时返回已收/已发内容） |
| `http` | HTTP/1.1 客户端（GET/POST，仅 `http://`，无 TLS）：`Http.get/post/request` → `HttpResult`（status/body/header）。**同步阻塞**实现（基于 `TcpClient.recv/recvLine`，不走 waitFd）——非协程专用，普通上下文亦可调用 |

#### 8.6.4 vs 线程（`@thread`）

| 维度 | 协程（`@coro`） | 线程（`@thread`） |
|------|----------------|------------------|
| 调度 | 协作式（用户态，显式 `await`/`yield`） | 抢占式（pthread） |
| 数量 | 轻量，单线程内可大量（栈池复用 + 动态槽位） | 受系统资源限制 |
| 共享 | 同线程内天然无竞争，可安全共享 | 跨线程需走 mapping 或 `sync` 原语 |
| 适合 | IO 等待、状态机、高并发连接 | 计算/阻塞任务、多核并行 |

> **协程 vs 事件链**：两者都表达「等待-唤醒」，但语义不同——事件链是组件解耦（mapping 声明式连接），协程是函数内顺序挂起（代码内 `await`）。可混合使用：协程内 `await Class.event` 把两者桥接起来。

### 8.7 未来：Event-driven Pool（方案 B）

事件驱动的 Worker Pool 模式——不修改编译器，纯运行时 + stdlib 实现。

#### 8.7.1 架构

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

#### 8.7.2 工作机制

1. **Pool** 持有 `工作窃取队列` + N 个 Worker 线程
2. `Pool.submit(batchId, size)` → 将任务入队 → 条件变量唤醒空闲 Worker
3. Worker 完成当前 batch → 从队列拿下一个任务（或从其他 Worker 偷任务）
4. Worker 完成一个 batch → 发射 `batchDone` 事件 → Tally 累加

#### 8.7.3 需要的运行时支持

| 组件 | 说明 |
|------|------|
| 工作窃取队列 | 线程安全 deque，每个 Worker 有自己的双端队列 + 全局队列 |
| 条件变量唤醒 | 替代 1ms 轮询，任务入队立即唤醒 Worker |
| 可伸缩事件队列 | 替代固定 1024 环缓冲，支持动态扩容（防止静默丢事件） |

#### 8.7.4 优势与局限

| 维度 | 评价 |
|------|------|
| 编译器改动 | ✅ 零改动（纯运行时 + stdlib） |
| 负载均衡 | ✅ 工作窃取，天然动态均衡 |
| 灵活性 | ✅ 可通过 mapping 任意组合 Source/Worker/Tally |
| 单 batch 开销 | ⚠️ 每次 batch 需 2 次事件排队（submit → dispatch → batchDone） |
| 实现复杂度 | ⚠️ ~400 行（工作窃取队列 + Pool 类 + mapping 配置） |
| 适用场景 | 每个 batch 计算量大（>1ms），事件排队开销可忽略 |

### 8.8 已实现：`@parallel for`（方案 A）

✅ v2.4 已实现。不需要 event/mapping，零运行时开销。

MYP 的 Actor 模型 (`@thread`) 擅长 IO/事件驱动型并发，但对于 **BNCT 蒙特卡洛输运**这类计算密集型、数据并行的场景，事件开销（排队、唤醒、调度）成为瓶颈。`@parallel for` 为这类场景提供编译期并行的解决方案。

#### 8.8.1 语法

```myp
@parallel for (int i = 0; i < n; i = i + 1) {
    // 循环体——每个线程执行一部分迭代
    Atomic.addDouble(tally, idx, value);
}
```

循环变量类型：`int`（推荐）或 `long`（自动截断为 int32 处理边界）。

#### 8.8.2 编译器视角

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

#### 8.8.3 运行时视角

```
3. 线程池平分迭代：thread[0] 拿 [0..N/4), thread[1] 拿 [N/4..N/2) ...
4. 各线程串行执行 fn(i, ctx)
5. barrier 等待全部完成 → 返回
```

线程池使用 16 线程 work-stealing 池，全局懒创建 + 复用。

#### 8.8.4 关键实现细节

| 机制 | 说明 |
|------|------|
| **变量捕获** | `generateParallelFor` 遍历作用域栈，收集所有 named values → 构建 LLVM StructType → 填充 → `void* arg` 传递 |
| **数学函数** | emitKernelExpr 原使用 `__nv_log`/`__nv_exp`（CUDA device 函数），改为 `myp_math_log`/`myp_math_exp` 运行时函数 |
| **静态方法调用** | emitKernelExpr 直接调用 LLVM 模块中已声明的函数（如 `Physics_sampleEnergy`），避免内联复杂 IfStmt 导致返回 `i64(0)` |
| **Atomic 操作** | `Atomic.addDouble`/`Atomic.addInt` 通过 LLVM `atomicrmw` 指令编译，线程安全 |
| **线程安全** | 每粒子独立 RNG state（无竞争）+ Atomic 累加（无锁） |

#### 8.8.5 vs Actor 模型

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

### 8.9 已实现：Atomic 操作

✅ 已实现。通过 LLVM `atomicrmw` 指令直接生成，零运行时开销。

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

### 8.10 已实现的功能

| 功能 | 状态 | 说明 |
|------|------|------|
| `@thread` 注解 | ✅ 已实现 | 实例在独立 pthread 线程运行 |
| 每线程独立事件队列 | ✅ 已实现 | ring buffer + mutex 保护 |
| 跨线程异步投递 | ✅ 已实现 | `myp_thread_for_instance` 查询 + 队列推送 |
| 定时器系统 | ✅ 已实现 | `__myp_timer_create` + `myp_timer_check` 事件循环 |
| 每线程独立定时器 | ✅ 已实现 | 定时器在所属线程的事件循环中触发 |
| 线程生命周期管理 | ✅ 已实现 | `myp_thread_create`/`destroy`/`stop`/`join` |
| `@threadpool` 注解 | ✅ 已实现 | 函数内 `Worker[N] pool @threadpool` 可用；不支持文件级声明（需在函数中初始化，通常放 `@startup` 或 `main()`） |
| `@coro`/`await` 协程 | ✅ 已实现 | ucontext 纤程 + 自动调度器 + 事件/超时/取消（v3.0，C1–C10） |
| `channel` 缓冲通道 | ✅ 已实现 | 协程间 rendezvous 通信（stdlib/channel.myp） |
| `async` 统一异步 IO | ✅ 已实现 | `Async.sleep` 等 + `Coro.waitFd`（stdlib/async.myp） |

---

## 9. 导入系统

```
import env;             // 标准库导入（从 stdlib/ 目录加载 env.myp）
import gpu.hal;         // 标准库子模块（点分模块名 → stdlib/gpu/hal.myp）
import timeline;        // 标准库导入
import "./helper.myp";  // 用户文件导入（相对路径，以被导入文件所在目录为基准）
import "/abs/path/lib.myp"; // 用户文件导入（绝对路径）
```

| 项目 | 规则 |
|------|------|
| 语法 | `import 标识符;`（标准库/包，点分子模块名）或 `import "路径";`（用户文件） |
| 标准库 | 无引号、无扩展名，在 `stdlib/` 目录查找；**点分模块名** `import gpu.hal;` → `stdlib/gpu/hal.myp`（`gpu.backend` → `stdlib/gpu/backend.myp`；扁平名 `gpu` → `stdlib/gpu.myp` 为兜底）——`stdlib/gpu.myp` 内部即用点分名导入 `gpu.backend`/`gpu.backend_cuda`/`gpu.hal`/`gpu.device` |
| 用户文件 | 双引号字符串路径，支持相对/绝对路径；相对路径以**被导入文件所在目录**为基准解析（stdlib 子模块内部 `import "./gpu/backend.myp"` 也能正确加载，不依赖用户源码目录） |
| 路径搜索（名字导入） | `--stdlib` 指定目录 → 源文件目录的 `../stdlib/` → 源文件目录的 `stdlib/` → `--package-path` 指定目录（包格式见下） |
| `--stdlib` | 编译器选项，指定标准库路径 |
| `--package-path` | 编译器选项，指定本地包搜索路径（设计支持冒号分隔多路径、`MYP_PACKAGE_PATH` 环境变量；`myp` 包管理器把本地 `myp_packages/` 与该环境变量合并后传给此选项） |
| `--shared` | 编译为共享库 (.so)，无 main 要求 |
| `--static` | 编译为静态库 (.a)，无 main 要求 |
| 包格式 | `<package-path>/<name>/src/<name>.myp` 或 `<package-path>/<name>/<name>.myp`，附带 `package.myp` 元数据（name/version/depends） |
| 去重 | 同一文件不会重复导入（基于规范化路径 / 模块名去重） |
| 递归 | 导入的文件中的 `import` 也会被递归加载（合并 7 类声明：class/interface/mapping/function/ffi/enum/struct） |
| 命名空间 | 扁平结构，无嵌套；**顶层函数跨模块不可见**——跨模块共享逻辑须放进（`@static`）class 的 `static:` 方法 |

### 9.1 `myp` 包管理器（v2，registry + lockfile）

`myp`（`tools/pm/`，纯 MYP 自举实现）管理包与依赖：

| 命令 | 说明 |
|------|------|
| `myp init <name>` | 创建包脚手架（`package.myp` + `src/<name>.myp`） |
| `myp build [opts]` | 编译当前包（`--stdlib` + `--package-path`；缺失依赖自动从 registry 安装） |
| `myp run [opts]` | build + 运行 |
| `myp install <path>` | 从本地路径安装包 → `myp_packages/<name>/` |
| `myp add <pkg>[@ver]` | 从 registry 解析版本（latest 或 `@ver`）→ 安装 → 写 `myp.lock` → 更新 `package.myp` depends |
| `myp remove <pkg>` | 清理 `myp.lock` + depends |
| `myp update` | 按 lockfile 重装 / 升级 |
| `myp list` | 列出锁定依赖 |

- **registry**：`MYP_REGISTRY` 指向本地目录或 git URL（`git clone --depth 1` 到缓存）；
  未设置时回退本地 `./registry`（演示/测试）。形态为纯 git 子目录
  `registry/packages/<name>/<version>/package.myp` + `src/<name>.myp`，版本数值比较。
- **lockfile**：`myp.lock`（`key: value` 文本）；`myp build` 按 lock 版本装依赖，
  未锁定则取 latest。
- 详见 `docs/pkg_manager.md`（v2 实施记录 §11）。

---

## 10. 标准库设计

### 10.1 设计原则

标准库分**静态工具类**与**实例组件类**两类，均**不暴露裸全局函数**：

- **静态工具类**（`class X { static: ... }`，stdlib **主流**形态）：无状态，`import`
  后直接 `X.方法()` 调用，**无需 `new` 实例、不依赖 mapping**——如
  `Console.write(42)`、`Math.sqrt(x)`、`Fs.exists(p)`、`Time.nowMs()`、
  `Coro.scheduler()`、`Atomic.addInt(...)`、`Mutex.lock(m)`、`Str.len(s)`…
  （env/math/fs/time/coro/atomic/sync/fmt/crypto/rtti/process/args/base64/date/
  random/test/gpu/cuda/sdl/memory/pool/barrier/future/http 等库均以静态类为主；
  fs/http 另含实例类 Path/HttpResult）。
- **实例组件类**：有内部状态，`new` 创建后走 action 调用或 mapping 连接——如
  `File`/`Path`/`Timeline`/`Stopwatch`/`Timer`/`Channel`/`TcpClient`/`Json`/`Regex`/
  `Logger`/`StringBuilder`/UI 控件（Window/Label/Button…）/`HttpResult`。

> **两种静态形态的区别**：stdlib 用 `class X { static: ... }`（普通类，方法全静态，
> `X.f()` 直接调）；自举工具 用 `@static class X` 注解（静态类，可含全局静态
> 属性，见 §4.3）——`Console` 属前者。

```
// ✅ 静态工具类：import 后直接调用，无需 new（stdlib 主流；直调放 class action）
import env;
class Demo {
    action:
        @constructor Demo() {
            Console.write(42);            // Console 的 write 是 static: 方法
            Console.writeString("hi\n");
        }
}
int main() {
    Demo d = new Demo();      // main() 只做接线
    return 0;
}

// ✅ 实例组件类（有状态）：new 创建，action 调用或 mapping 连接
import env;
class Worker {
    action:
        @startup void run() { output(42); }
    event:
        output(int v);
}
int main() {
    Worker worker @thread;
    mapping() { Worker.output -> Console.write; }   // 事件驱动（节点用类名）
    return 0;
}

// ❌ 无此形态
printInt(42);   // 裸全局函数——一律收进 static 类方法，不暴露全局函数
```

### 10.2 库目录布局

```
stdlib/
├── 基础 I/O 与环境
│   ├── env.myp         # Console（write/writeString/writeLine/writeFloat/writeBool/writeLong/
│   │                   #   readString/kbhit/getch/flush）+ 环境变量 getEnv/setEnv/unsetEnv
│   ├── io.myp          # File（open/close/readLine/write/writeLine/hasNext + 二进制 r/w）
│   ├── fs.myp          # Fs（exists/isDir/isFile/dirname/basename/join/listDir/mkdirP/...）
│   │                   #   + Path（路径操作，实例化）
│   ├── text.myp        # Str（字符串工具，静态）+ StringBuilder（构建器，实例化）
│   ├── fmt.myp         # Fmt（printf 风格格式化）
│   ├── logger.myp      # Logger（日志，实例化）
│   ├── args.myp        # Args（命令行参数）
│   ├── process.myp     # Process（进程管理）
│   ├── date.myp        # Date（日期时间）
│   ├── base64.myp      # Base64（编解码）
│   ├── random.myp      # Random
│   ├── math.myp        # Math（sqrt/abs/floor/ceil/sin/cos/tan/exp/log/pow/absInt/min/max）
│   ├── time.myp        # Time（nowMs/sleep）+ Timer（定时器）
│   └── timeline.myp    # Timeline / Stopwatch（定时器事件 timeout/interval/tick，实例化）
├── 数据结构
│   ├── collections.myp # ArrayList/HashMap/Set/Deque/Queue/Stack/PriorityQueue/LinkedList/
│   │                   #   StrHashMap（动态扩容）+ Sort
│   ├── stream.myp      # RangeStream/IntStream/DoubleStream（流式数据源）
│   ├── setops.myp      # 集合运算（Set 辅助）
│   ├── option.myp      # Option<T> / T? 可空容器
│   └── result.myp      # Result<T,E> 值式错误传播
├── 并发 / 协程
│   ├── coro.myp        # Coro（scheduler/resume/yield/result/waitEvent/waitAny/...）
│   ├── async.myp       # Async（统一异步 IO：Async.sleep / @async）
│   ├── channel.myp     # Channel（Go 风格缓冲通道）
│   ├── sync.myp        # Mutex（含递归）/RWLock/CondVar/Semaphore/Once（handle 模式）
│   ├── atomic.myp      # Atomic（addInt/subInt/xchgInt/addDouble/loadInt/storeInt）
│   ├── pool.myp        # Parallel（线程池任务工具）
│   ├── barrier.myp     # Barrier（pthread_barrier）
│   └── future.myp      # Future（异步结果容器）
├── 网络 / HTTP
│   ├── net.myp         # TcpServer / TcpClient（TCP，实例化）
│   └── http.myp        # Http / HttpResult（HTTP/1.1 客户端，静态）
├── 数据解析 / 安全
│   ├── json.myp        # Json（解析/查询/序列化）
│   ├── regex.myp       # Regex（正则匹配）
│   ├── crypto.myp      # Crc32 / Hash（CRC-32/MD5/SHA-1/SHA-256）
│   └── rtti.myp        # Rtti（运行时类型信息）
├── GPU（@gpu for 配套）
│   ├── gpu.myp         # Gpu 门面（backend/alloc/free/...）
│   ├── cuda.myp        # Cuda / Device / Vectors / Matrix
│   └── gpu/            # 子模块（点分模块名导入：import gpu.hal;）
│       ├── backend.myp / backend_cuda.myp / backend_cpu.myp   # 后端抽象 + CUDA/CPU 实现
│       ├── hal.myp / device.myp / memory.myp / stream.myp     # HAL/设备/显存/流
│       ├── ops.myp / graph.myp / algo.myp                     # 算子/图/算法
│       └── byoc.myp                                           # 自编译 kernel（BYOC）
├── UI
│   ├── sdl.myp         # SDL（init/quit/clear/present/getKey，SDL2 FFI）
│   └── ui.myp          # 终端 TUI（Screen/Window/Label/Button/TextBox/ProgressBar，ANSI）
├── 错误 / 测试
│   ├── error.myp       # 标准异常类（StringError/FileError/...）+ Error 接口
│   └── test.myp        # Test（assert/assertEq/assertStrEq/report），配合 @test
└── memory.myp          # Memory（alloc/free/realloc/release，FFI 桥接 libc）
```

> **当前状态**：所有 stdlib 文件均为纯 MYP class，通过 `import` 加载。大部分是**静态工具类**
> （`class X { static: ... }`，`Console.writeLine("hello")` 直接调用，无需 `new`）；
> 有状态者（File/Timeline/Channel/TcpClient/StringBuilder/UI 控件…）为实例组件类。GPU
> 子模块用点分模块名导入（`import gpu.hal;` → `stdlib/gpu/hal.myp`，见 §9）。

### 10.3 `import env` — Console 类

```myp
// env.myp — 基本 I/O（静态工具类：static: 方法，无需 new 即可调用）
class Console {
    static:
        void write(int v);              // 输出整数 + 换行
        void writeString(string s);     // 输出字符串
        void writeLine(string s);       // 输出字符串 + 换行
        void writeFloat(double v);      // 输出浮点数 + 换行
        void writeBool(bool v);         // 输出布尔值 + 换行
        void writeLong(long v);         // 输出长整型 + 换行
        string readString();            // 读一行
        int kbhit();                    // 非阻塞按键检测
        int getch();                    // 读一个字符（原始模式）
        void flush();                   // 刷新 stdout
        string getEnv(string name);     // 读取环境变量
        int setEnv(string name, string value);   // 设置环境变量
        int unsetEnv(string name);      // 删除环境变量
}
```

使用方式：

```myp
import env;

class Demo {
    action:
        @constructor Demo() {
            Console.writeLine("hello");   // 静态方法，直接调用，无需 new
            Console.write(42);
        }
}

class Worker {
    action:
        @startup void run() { output(42); }
    event:
        output(int v);
}

int main() {
    Demo d = new Demo();
    Worker worker @thread;

    mapping() {
        Worker.output -> Console.write;   // 事件驱动（mapping 节点用类名）
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

### 10.5 `import math` — Math（静态工具类）

一元数学函数为泛型（`where T : Float`，f32 返回 f32）；`min/max/clamp` 为 `Ordered` 泛型
（int/long/float/double/string 通用）；`abs` 为 `Numeric` 泛型。

```myp
import math;
double s  = Math.sqrt(16.0);        // 4.0
int a     = Math.abs(-5);           // 5
double fl = Math.floor(3.7);        // 3.0
double pw = Math.pow(2.0, 10.0);    // 1024.0
int lo    = Math.min(3, 5);         // 3
double l  = Math.lerp(0.0, 10.0, 0.5);   // 5.0
```

完整清单：`pi/e/sqrt/abs/floor/ceil/trunc/sin/cos/tan/asin/acos/atan/atan2/sinh/cosh/
tanh/exp/log/pow/min/max/clamp/lerp/degToRad/radToDeg`。

### 10.6 `import text` — Str / StringBuilder（字符串处理）

`Str` 静态工具类（len/contains/indexOf/startsWith/endsWith/substring/replace/toUpper/
toLower/trim/split/toInt/repeat/padLeft/padRight/reverse/ord/chr/cmp）；
`StringBuilder` 实例构建器（动态扩容，收集后一次拼装减少中间对象）：

```myp
import text;
int n    = Str.len("hello");                // 5
int has  = Str.contains("hello", "ell");   // 1
string up = Str.toUpper("hi");             // "HI"
string sub = Str.substring("hello", 1, 3); // "el"
int k    = Str.splitCount("a,b,c", ",");   // 3
string p = Str.splitGet("a,b,c", ",", 1);  // "b"
int n2   = Str.toInt("42");                // 42

StringBuilder sb = new StringBuilder();
sb.append("Hello"); sb.append(" World");
string joined = sb.toString();              // "Hello World"
```

### 10.7 `import io` — File（文件 I/O，实例类）

`File` 有状态实例类：`open(path, mode)`（失败抛 `FileError`）→ 读写 → `close()`。
文本（readLine/write/writeLine/hasNext/readAll）与二进制（readByte/writeByte/
readI32BE/writeI32BE/readDouble/writeDouble/seek）都支持；`readLineAsync/readAllAsync`
为 `@async`（须在 `@coro` 内 `await`，worker 线程执行阻塞 IO）：

```myp
import io;
File f = new File();
f.open("/tmp/out.txt", "w");
f.writeLine("hello file");
f.close();
File g = new File();
g.open("/tmp/out.txt", "r");
string line = g.readLine();   // "hello file"
g.close();
```

### 10.8 `import fs` — Fs / Path（文件系统）

`Fs` 静态（exists/isDir/isFile/fileSize/modifiedTime/dirname/basename/join/listDir/
listCount/mkdirP/removeRecursive）；`Path` 为实例化路径操作：

```myp
import fs;
int ex = Fs.exists("/tmp");            // 1
string p = Fs.join("/tmp", "x.txt");  // "/tmp/x.txt"
string[] dirs = Fs.listDir("/tmp");
Fs.mkdirP("/tmp/myp/a/b");            // 递归建目录
Fs.removeRecursive("/tmp/myp");       // 递归删除
```

### 10.9 `import collections` — 数据结构（动态扩容）

9 个容器（`ArrayList<T>`/`HashMap<K,V>`/`Set<T>`/`Deque<T>`/`Queue<T>`/`Stack<T>`/
`PriorityQueue<T>`/`LinkedList<T>`/`StrHashMap<V>`）+ 静态 `Sort`。全部**动态扩容**
（v3.8.0 起突破固定 1024 上限）。`HashMap` 键须支持 `%`/`==`（int/long 等）；
字符串键用 `StrHashMap<V>`：

```myp
import collections;
ArrayList<int> arr = new ArrayList<int>();
arr.add(1); arr.add(2); arr.add(3);
int n = arr.size();                  // 3
arr.remove(0);

StrHashMap<int> m = new StrHashMap<int>();
m.put("k", 7);
int v = m.get("k", 0);               // 7

HashMap<int,string> m2 = new HashMap<int,string>();
m2.put(1, "one");
string w = m2.get(1, "?");            // "one"
```

### 10.10 `import json` — Json（JSON 解析/查询）

`Json` 实例类：构造解析（无效输入抛 `JsonError`），`getString/getInt/getBool/type/
arrayLength` 按路径查询，用后 `free()`：

```myp
import json;
Json doc = new Json("{\"name\":\"myp\",\"ver\":1}");
string name = doc.getString("name"); // "myp"
int ver = doc.getInt("ver");         // 1
doc.free();
```

### 10.11 `import crypto` / `import fmt` — 哈希与格式化

`Crc32`/`Hash` 静态：CRC-32（原始值/crc32Hex）、MD5、SHA-1、SHA-256（十六进制串）；
`Fmt` 静态：printf 风格格式化（`i/u/x/X/o/b` 整数、`f/e/g` 浮点、`s/sR` 字符串 +
宽度/填充）：

```myp
import crypto;
import fmt;
string crc = Crc32.crc32Hex("hello");      // "3610a686"
string md5 = Hash.md5("abc");             // "900150983cd24fb0d6963f7d28e17f72"
string sha = Hash.sha256("abc");
string hx  = Fmt.x(255, 4);                // "00ff"
string fix = Fmt.f(3.14159, 2);            // "3.14"
string sci = Fmt.e(123.456, 2);            // "1.23e+02"
string ri  = Fmt.i(42, 6);                 // "    42"
```

### 10.12 `import random` — Random（随机数，静态）

`init(seed)` 播种后 `next/below/uniform/gaussian/range/exponential/poisson/shuffle`
取各分布随机数：

```myp
import random;
Random.init(42);
int d  = Random.below(6);       // 0..5
int b  = Random.next();         // [0, RAND_MAX]
double u = Random.uniform();    // [0,1)
double g = Random.gaussian();   // N(0,1)
```

### 10.13 并发原语 — sync / atomic / pool / barrier / future

MYP 并发三件套：`@thread`（线程）、`@coro`（协程），以及本节的**显式同步/共享原语**。
五个库都是**静态类**（`import` 即用，无需 `new`）。

**`sync`（v3.9）——互斥/读写锁/条件变量/信号量/一次性**——handle 模式
`create → 用 → destroy`（句柄槽位每类 64 个，用完必须 `destroy`；锁配合 `try/finally`
保证异常路径解锁）。**真实用法与约定见 §8.5**：

| 类 | 方法 |
|----|------|
| `Mutex` | `create` / `createRecursive`（可重入）/ `lock` / `tryLock` / `unlock` / `destroy` |
| `RWLock` | `create` / `readLock` / `writeLock` / `tryReadLock` / `tryWriteLock` / `unlock` / `destroy` |
| `CondVar` | `create` / `wait(cv, mutex)` / `signal` / `broadcast` / `destroy`（须配 Mutex） |
| `Semaphore` | `create(initial)` / `wait`（P）/ `tryWait` / `post`（V）/ `destroy` |
| `Once` | `create` / `enter`（1=首个调用者）/ `done` / `destroy`（call-once 初始化） |

**`atomic`——原子数组操作**——`Atomic.addInt/subInt/xchgInt/addDouble/loadInt/storeInt`
（对 `int[]`/`double[]` 下标做原子读改写），基于 LLVM `atomicrmw`（seq_cst）——见 **§8.9**。

**`pool`——work-stealing 线程池查询/配置**——语言级并行接口是 `@parallel for`（自动用
全局池）；`Parallel` 暴露池的底层查询（`src/runtime/runtime.c`）：

| 方法 | 说明 |
|------|------|
| `threadCount()` | 硬件并发线程数（池默认大小） |
| `setThreads(n)` | 指定池大小（**必须在首次 `@parallel for` 之前调用**；`0`=自动=硬件并发数，之后 no-op） |
| `workerCount()` | 池实际 worker 数（首次 `@parallel for` 后可用；0=未初始化） |
| `workerId()` | 当前线程的池内索引（`@parallel for` body 内 0..N-1；**池外为 -1**） |
| `isActive()` | 池是否已初始化（1/0） |

```myp
import pool;
Parallel.setThreads(2);              // 首次 @parallel for 之前设置
int hc = Parallel.threadCount();     // 硬件并发数
int[64] out;
@parallel for (int i = 0; i < 64; i = i + 1) {
    out[i] = Parallel.workerId();    // 每迭代记录 worker 索引 0..1
}
int nw = Parallel.workerCount();     // 2
```

**`barrier`——同步屏障**——`create(count)`（等待 count 个线程到齐）→ 每线程
`wait(h)` 阻塞至全部到达 → `destroy`。典型用途：多个 `@thread` 线程的
**start/done 双屏障**（`tests/io_thread/test.myp` 三线程会合）；`count=1` 可做
self-only barrier：

```myp
import barrier;
int h = Barrier.create(3);   // 3 个线程会合
// ... 每个线程: Barrier.wait(h);   // 等齐 3 个才继续
Barrier.destroy(h);
```

**`future`——异步结果容器**——`create` → 某处 `set(h, v)` → `get(h)` 取回 →
`destroy`；配合协程 `await Future`（v3.0）：

```myp
import future;
int h = Future.create();
Future.set(h, 42);
int v = Future.get(h);       // 42
Future.destroy(h);
```

### 10.14 协程生态 — coro / async / channel

- **`coro`**：`Coro` 静态类（scheduler/resume/yield/result/waitEvent/waitAny/status/
  requestCancel…），配合 `@coro`/`await`——见 **§8.6** 与 `docs/coro.md`。
- **`async`**：`Async.sleep` 等 `@async` 方法，`@coro` 内 `await f()` 调用；`Coro.waitFd`
  驱动非阻塞 IO——见 **§8.6.3**。
- **`channel`**：Go 风格缓冲通道 `Channel`（send/recv/trySend/tryRecv/close），协程内
  满/空挂起、非协程返 -1——见 **§8.6.3**。

### 10.15 网络 — net / http

- **`net`**：`TcpServer`/`TcpClient`（实例类），`recvAsync/recvLineAsync/sendAsync` 为
  `@async`（`Coro.waitFd` 驱动非阻塞 IO）——见 **§8.6.3**。
- **`http`**：`Http.get/post/request`（静态）→ `HttpResult`（status/body/header）。
  **同步阻塞**实现（非协程专用）——见 **§8.6.3**。

### 10.16 GPU — gpu / cuda（`@gpu for` 配套）

GPU 编程分**四层**（`import gpu;` 门面是用户唯一入口）：

> **CPU 回退机制（默认即 CPU）**：GPU 卸载**默认关闭**——须设环境变量 `MYP_GPU=1`
> 才尝试加载 CUDA 驱动（`runtime_gpu.c` `myp_gpu_init`）。**未设置 `MYP_GPU`、或设了
> `MYP_GPU=1` 但无驱动/无硬件**（`dlopen("libcuda.so.1")` 失败）时，一律**回退 CPU**：
> `Gpu.backend()` 返回 "CPU"、`GpuBuffer` 退化为 host 数组直通（"CPU 一等后端"）、
> `@gpu for` 走顺序/`@parallel for`——**结果一致、用户代码零改动**（设 `MYP_GPU=1` 却
> 无法加载驱动时会打印 `[myp GPU] MYP_GPU=1 but cannot load libcuda.so.1 ... falling
> back to CPU` 明确诊断）。

| 层 | 机制 | 说明 |
|----|------|------|
| **L0** | `@gpu for` 编译器指令 | 数据并行循环卸载为 **NVPTX kernel**（`nvptx64-nvidia-cuda` → llc → PTX 嵌入 `myp_gpu_load_kernel` 启动），无 GPU 自动回退顺序 CPU |
| **L1** | `Gpu.*` 宿主数组原语 | 传普通 `double[]`/`float[]`，自动 H2D→kernel→D2H（或 CPU 回退），**零配置** |
| **L2** | `GpuBuffer`/`GpuBufferF` 显式显存 | 手动分配/拷贝/释放；异步流拷贝 |
| **L3** | `gpu/` 子模块 `Gpu*` 类 | 后端/HAL/设备/显存/流/算子/图/BYOC 底层（点分模块名 `import gpu.hal;`） |

**L1 宿主数组原语（`Gpu.*`，`stdlib/gpu.myp` 门面 → `cuda.myp` `Vectors`/`Matrix`）**：
向量 `add/sub/mul/scale/addScalar/fill/saxpy/copy/negate/clamp/dot/dotF` + 逐元素数学
（`sqrt/sin/cos/exp/log/pow/tan/abs/floor/ceil`）+ 归约（`sum/max/min/argmax` +
`mean/variance/stddev/norm/normSquared/normalize`，float 变体 `*F`）+ 矩阵
（`Matrix.matmul/matmulF` → `Gpu.gemm/gemmF`）。

> 归约实现：`sum/mean/variance/stddev/norm/normSquared/dot/dotF` 用 **GPU 原子归约**
> （`Atomic.addDouble` → `atom.add.f64`，kernel llc `-mcpu=sm_75` 直降）；`max/min/argmax`
> 用 **GPU 块归约**（每块局部极值 + CPU 扫描块表）；仅 `transpose` 用 CPU。

**L2 显式显存（`GpuBuffer` double / `GpuBufferF` float）**：构造即「分配设备内存 + 一次
H2D」；`copyToHost/copyFromHost`（元素偏移 srcOff/dstOff/len）、异步版带 `GpuStream`；
`free()` 显式释放（无 finalizer，设备内存须手动释放，重复调用安全）；`valid()==0` 表示
分配失败。**CPU 一等后端**：无 GPU 时 `GpuBuffer` 持有 host 数组引用（数据即 host），
拷贝为直通——"CPU 一等后端"（`gpu_library_design §7.6`）。

```myp
import gpu;
import fmt;

// L1 零配置：宿主数组自动传输（自动 GPU / CPU 回退）
Gpu.add(a, b, out, n);
double s = Gpu.sum(a, n);
Gpu.gemm(A, B, C, m, n, k);

// L2 显式显存：分配 + H2D → 拷回 → 显式释放
GpuBuffer buf = new GpuBuffer(a, n);
double[] y = new double[n];
buf.copyToHost(y, 0, 0, 8);        // 元素偏移 (srcOff, dstOff, len)
buf.free();

// L0 @gpu for：数据并行卸载（无 GPU 自动回退 CPU）
@gpu for (long i = 0L; i < n; i = i + 1L) { out[i] = a[i] + b[i]; }
```

实测输出（CPU 回退）：`backend=CPU / add[0]=3.0 / sum=524800.0 / buf[0]=1.0 / acc=4950`。

**设备信息**：`Gpu.backend()`（"CUDA"/"CPU"）、`Gpu.deviceCount()`、`Gpu.deviceName()`；
更全的查询在 `GpuDevice`（count/name/memory/capability/multiProcessors/maxThreads/
warpSize/sharedPerBlock/regsPerBlock/clock/concurrentKernels/memAlignment…）。

**`cuda.myp`**：`Cuda`（blockSize/available/count/name/memory/capability/multiProcessors/
maxThreads/warpSize）、`Device`、`DevMath`（设备端数学 = `Math` 泛型包装）、`Vectors`
（宿主数组原语实现）、`Matrix`（matmul/matmulF）。

**L3 `gpu/` 子模块（点分模块名导入）**：

| 类 | 职责 |
|----|------|
| `GpuHAL` | 激活后端抽象（`active()` → "cuda"/"cpu"） |
| `GpuBackend` | 底层句柄：alloc/free、copy H2D/D2H/D2D（double/float/async）、sync、stream/event、graph capture |
| `GpuDevice` | 设备属性查询（见上） |
| `GpuBuffer`/`GpuBufferF`/`GpuPool` | 显存缓冲（double/float）+ 缓冲池 |
| `GpuStream`/`GpuEvent` | 异步流/事件（record/wait/sync/elapsed） |
| `GpuOps` | 设备端算子（`*D`/`*F`：add/sub/mul/scale/axpy/map/gemm/sum/max…，直接吃 `devicePtr`） |
| `GpuAlgo` | 数据并行算法（histogram/compact/unique） |
| `GpuGraph`/`GpuGraphExec` | CUDA Graph 捕获/回放（captureBegin/captureEnd/launch） |
| `GpuByoc`/`GpuLib` | BYOC 自编译 kernel（`load(ptx, name)` + `launch(kctx, grid, block, args…)`）、cuBLAS 封装 |

> 详见 mypdeeplearning 独立仓 `docs/gpu_paradigm.md`（https://gitee.com/tomatosoft_0/mypdeeplearning）、**§8.8**（`@parallel for`
> 对照）、§12.2（自举 GPU 收口）。

### 10.17 UI — ui / sdl

- **`ui`**：纯 MYP 终端 TUI 框架——`Screen`（静态，ANSI 渲染）+ `Window/Label/Button/
  TextBox/ProgressBar` 控件。
- **`sdl`**：`SDL.init/quit/clear/present/getKey`（SDL2 FFI，图形窗口）。

### 10.18 其他工具类

| 库 | 说明 |
|----|------|
| `args` | 命令行参数（`Args.get/count`） |
| `process` | 进程管理（`Process.run`…） |
| `date` | 日期时间工具（`Date`） |
| `base64` | Base64 编解码（`Base64`） |
| `logger` | 日志工具（`Logger`，实例类） |
| `memory` | 内存工具（`Memory.alloc/free/realloc`，FFI 桥接 libc） |
| `rtti` | 运行时类型信息（`Rtti`，v3.9） |
| `test` | `Test.assert/assertEq/assertStrEq/report`，配合 `@test`（v2.2） |
| `stream` | 流式数据源（`RangeStream`/`IntStream`/`DoubleStream`） |
| `setops` | 集合运算（`SetOp` 接口） |
| `option` | `Option<T>` / `T?` 可空容器（v3.9） |
| `result` | `Result<T,E>` 值式错误传播（v3.9） |
| `error` | 标准异常类（StringError/FileError/…）+ `Error` 接口（v3.9） |
| `regex` | 正则匹配（`Regex`，实例类） |

### 10.19 标准库规划

| 库 | 功能 | 当前状态 |
|----|------|---------|
| `env` | `Console` 类（write/writeString/writeLine/writeFloat/writeBool/writeLong/readString/kbhit/getch/flush） | ✅ 已实现：stdlib/env.myp |
| `time` | `Time` 类（nowMs/sleep） | ✅ 已实现：stdlib/time.myp |
| `timeline` | `Timeline` / `Stopwatch` 类（定时器事件 timeout/interval/tick） | ✅ 已实现：stdlib/timeline.myp |
| `math` | `Math` 类（泛型 sqrt/abs/floor/ceil/trunc/sin/cos/tan/exp/log + pow/min/max/clamp/lerp） | ✅ 已实现 |
| `io` | `File` 类（open/close/readLine/write/writeLine/hasNext + 二进制 r/w） | ✅ 已实现 |
| `stream` | 流式数据源（RangeStream/IntStream/DoubleStream） | ✅ 已实现 |
| `collections` | `ArrayList<T>`/`HashMap<K,V>`/`Set<T>`/`Deque<T>`/`Queue<T>`/`Stack<T>`/`PriorityQueue<T>`/`LinkedList<T>`/`StrHashMap<V>`——**动态扩容**（v3.8.0 起突破固定 1024 上限） | ✅ 已实现 |
| `text` | `Str` 字符串工具 + `StringBuilder` 字符串构建器 | ✅ 已实现 |
| `atomic` | `Atomic` 类（addInt/subInt/xchgInt/addDouble/loadInt/storeInt），基于 LLVM atomicrmw | ✅ 已实现 |
| `random` | `Random` 类（init/next/below） | ✅ 已实现 |
| `pool` | `Parallel` 静态类（线程池任务工具） | ✅ 已实现 |
| `barrier` | `Barrier` 类（create/wait/destroy），基于 pthread_barrier | ✅ 已实现 |
| `future` | `Future` 类（create/set/get/destroy），异步结果容器 | ✅ 已实现 |
| `coro` | `Coro` 协程类（scheduler/resume/yield/isActive/destroy/result/waitEvent/current/count/status/waitEventTimeout/waitAny/requestCancel/cancelRequested/clearCancel），基于寄存器级汇编纤程；`@coro` 方法/顶层函数 + `await` 语法由编译器支持 | ✅ 已实现（C1-C10，详见 `coro.md`）|
| `memory` | `Memory` 类（alloc/free/realloc/release），FFI 桥接 libc malloc/free/realloc（指针以 `long` 承载） | ✅ 已实现（v3.8.0 修复） |
| `test` | `Test` 类（assert/assertEq/assertStrEq/report），配合 `@test` 注解 | ✅ 已实现 |
| `sdl` | `SDL` 图形类（init/quit/clear/present/getKey），基于 SDL2 FFI | ✅ 已实现 |
| `ui` | 终端 TUI 框架（Window/Label/Button/TextBox/ProgressBar），纯 MYP 实现，基于 ANSI escape codes 渲染 | ✅ 已实现：stdlib/ui.myp |
| `net` | `TcpServer`/`TcpClient` 网络类（FFI） | ✅ 已实现：stdlib/net.myp |
| `json` | `Json` 类（解析/查询/序列化） | ✅ 已实现：stdlib/json.myp |
| `regex` | `Regex` 类（正则匹配） | ✅ 已实现：stdlib/regex.myp |
| `base64` | base64 编解码 | ✅ 已实现：stdlib/base64.myp |
| `date` | 日期时间工具 | ✅ 已实现：stdlib/date.myp |
| `process` | 进程管理 | ✅ 已实现：stdlib/process.myp |
| `args` | 命令行参数 | ✅ 已实现：stdlib/args.myp |
| `logger` | 日志工具 | ✅ 已实现：stdlib/logger.myp |
| `channel` | Go 风格缓冲通道（协程通信） | ✅ 已实现：stdlib/channel.myp |
| `setops` | 集合运算（`Set` 辅助） | ✅ 已实现：stdlib/setops.myp |
| `cuda` | GPU 设备信息/向量化/矩阵（配合 `@gpu for`） | ✅ 已实现：stdlib/cuda.myp |
| `fs` | 文件系统（存在性/目录/路径） | ✅ 已实现：stdlib/fs.myp |
| `option` | `Option<T>` / `T?` 可空容器（v3.9） | ✅ 已实现：stdlib/option.myp |
| `result` | `Result<T,E>` 值式错误传播（v3.9） | ✅ 已实现：stdlib/result.myp |
| `sync` | Mutex/RWLock/CondVar/Semaphore/Once（v3.9） | ✅ 已实现：stdlib/sync.myp |
| `async` | 统一异步 IO：`Async.sleep` / `@async` + `await f()`（v3.9） | ✅ 已实现：stdlib/async.myp |
| `http` | HTTP/1.1 客户端（v3.9，仅 http://，无 TLS） | ✅ 已实现：stdlib/http.myp |
| `fmt` | printf 风格格式化 `Fmt.*`（v3.9） | ✅ 已实现：stdlib/fmt.myp |
| `crypto` | CRC-32 / MD5 / SHA-1 / SHA-256（v3.9） | ✅ 已实现：stdlib/crypto.myp |
| `rtti` | 运行时类型信息 `Rtti`（v3.9） | ✅ 已实现：stdlib/rtti.myp |
| `error` | 标准异常类 + `Error` 接口（v3.9） | ✅ 已实现：stdlib/error.myp |

### 10.20 编译器 intrinsics 系统

标准库底层的部分 C 运行时函数通过**编译器 intrinsics**（`__myp_*`）暴露给 MYP（主要供
stdlib 使用）。以下为**示例清单**（非完整，共约百项）：

```myp
__myp_print("hello");              // 打印字符串（无换行）
__myp_print_int(42);               // 打印整数 + 换行
__myp_print_float(3.14);           // 打印浮点数
__myp_sleep_ms(100);               // 休眠 100ms（参数 long）
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

**实现机制**：`sema.cpp` 的 `registerIntrinsics()` 注册类型签名（`add_intrinsic`：返回类型 +
参数类型列表，数组参数走 `add_atomic`/`add_gpu_arr`）；`codegen.cpp` 的
`declareRuntimeFunctions()` 创建对应 LLVM **external 声明**（运行时名 `myp_*`，如
`__myp_print_int` → `myp_print_int`）；调用处（`generateCall`/`generatePolyMathIntrinsic`）
把 `__myp_X` **剥掉 `__` 前缀**映射到运行时函数。

**范围与边界**：
- intrinsics 表覆盖：env/io（print/sleep/flush/term/kbhit/getch/read_line）、`math`、
  字符串基础（strlen/chr/ord/charcode）、`timer`、`type_id`/`type_name`、CUDA/GPU 查询
  与显存/流/事件/图/BYOC/cuBLAS、`atomic`（`__myp_atomic_*` 直接生成 LLVM 原子指令）、
  `@test` 断言（assert/assertEq/capture…）、`__myp_pool_thread_count`、`__myp_trunc` 等。
- **协程 `__myp_coro_*` 不注册**：`Coro` 类是编译器内建（codegen 直接生成运行时调用），
  不进符号表——**用户代码不可见**（调用即 undefined）。
- **多数更新库不走 intrinsic 表**：fs/text/process/json/memory/barrier/future/sync/net/
  crypto 等用 stdlib 内的 **`ffi` 声明**（如 `ffi int myp_fs_exists(string path);`）直接
  绑定运行时 C 函数，不经 `registerIntrinsics`（旧基础字符串 `__myp_strlen`/`__myp_chr`
  仍在表内，但 `Str` 包装实际走 `ffi myp_strlen`）。

### 10.21 当前状态：纯 MYP class

编译器通过 `import env;` 从 `stdlib/env.myp` 加载 `Console` 类：

```myp
import env;
Console.writeLine("hello");       // ✅ 直接调用
mapping() { ... -> Console.write; } // ✅ mapping 连接
```

无全局函数残留——所有功能已迁移完毕。

---

## 11. 元编程

MYP 的元编程为**增量式(additive)扩展**,叠加在泛型 monomorphization(§5/§6)之上,
全部编译期完成、不引入运行时代价。四层由基础到高级:

```
类型级                  值级                  语法级                    全功能
泛型约束 where          @eval 编译期求值       macro 声明式宏           @macro + quote 过程宏
<T where T : Trait>     纯函数编译期执行        AST 片段替换             编译期算法生成 AST
```

> **实现**:`@eval` = `src/eval/eval.cpp` + `include/mylang/Eval.h`(轻量 MYP 解释器);
> `macro` = `src/macro/macro_expand.cpp` + `include/mylang/Macro.h`(AST 展开 pass);
> `@macro` = `src/eval/eval.cpp` 解释器扩展(`EvalValue` 加 AST 值)+ 展开 pass 集成。
> **时机**(`main.cpp` Phase):`macro`/`@macro` 在 Phase 3b `expandMacros`(parse 后、
> sema 前);`@eval` 在 Phase 4b `evaluateCompileTimeConstants`(sema 后、codegen 前)。
> **调试开关**:`--macro-expand`(dump 展开后 AST)。
> **测试**:`tests/@test/eval.myp`、`tests/macro/`、`tests/proc_macro/`、
> `tests/generic_traits/` + `tests/negative/{eval_recursion,generic_constraint,where_constraint}.myp`。
> **自举**:`tools/selfhost/`(`myp_self`)已同步实现四层(`parser.myp` 的 `parseMacroDecl`/
> quote 上下文关键字与 `$param` 捕获,`sema.myp` 的宏收集与 `@macro` 编译期展开)。

### 11.1 泛型约束 `where T : Trait`(类型级,v3.4)

泛型参数用 `<T where T : Trait>` 声明约束,单态化实例化时检查实参满足约束,
不满足则编译期报错(负测试 `tests/negative/generic_constraint.myp` /
`where_constraint.myp`)。内置数值 trait:`Numeric`(+ - * / %)、`Ordered`(< <= > >=,
含 string)、`Float`(仅 float/double)、`Integer`(整型族);用户自定义 interface
亦可作约束(如 `<T where T : Shape>`)。

```myp
T min<T where T : Ordered>(T a, T b) { if (a < b) return a; return b; }
T add<T where T : Numeric>(T a, T b) { return a + b; }

class Util {
    static:
        T clamp<T where T : Ordered>(T v, T lo, T hi) {
            if (v < lo) return lo;
            if (v > hi) return hi;
            return v;
        }
}

Console.writeString("s=" + min("abc", "abd"));  // abc(Ordered 含 string)
Console.writeLong(Util.clamp(0, 1, 10));        // 1
```

### 11.2 `@eval` 编译期求值(值级,v3.4)

`@eval` 标注的**顶层纯函数**由内嵌解释器在编译期执行。支持子集:标量字面量
(int/long/double/float/bool/string)、算术/比较/逻辑/位运算、if/while/for/block/
return/break/continue、递归与 `@eval` 函数互调、引用先前顶层 `const`。
禁止:`new`、成员访问、非 `@eval` 调用、数组(V1)、线程、I/O——遇之编译期诊断。
结果用于顶层 `const` 初始化,编译期折叠(`--emit-llvm` 可见 `ret i32 55`)。

```myp
@eval int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
@eval int triple(int n) { return 3 * n; }

const int FIB10 = fib(10);          // 编译期算得 55
const int FIB20 = fib(20);          // 6765
const double HALF = 10.0 / 4;       // 2.5
const bool BIG  = FIB10() > 50;     // true(const 以 FIB10() 形式调用)
const int T5    = triple(FIB10());  // 165(@eval 互调 + const 引用)
```

深递归有深度上限,超限编译期优雅报错(负测试 `tests/negative/eval_recursion.myp`:
`compile-time evaluation: recursion depth exceeded`),编译器"永不崩溃"。

**只读常量表(M-2,v3.15.243)**:模块级 `@eval <T>[N] <name> = [c1, c2, ...];`
(或 `<T>[]`,N 由元素数定)把数组初值编译期固化为**只读常量数组**(LLVM private
constant,`.rodata`),运行时可下标读 `name[i]`(i 可为运行时变量/表达式;元素支持
字面量/纯常量算术/顶层 const 标量引用)。表只能下标读,裸表名引用/多语句体 clean
拒绝——与 `@eval` 函数互补(函数算标量,表固化数组)。

### 11.3 声明式宏 `macro`(语法级,v3.5)

`macro` 是**顶层关键字声明**的代码模板,与 class/struct/enum/mapping 平行。
宏体是带元变量(`$param`)的普通 MYP 代码片段;调用时捕获实参 **AST 片段**,
深拷贝克隆宏体、`$param` → 实参 AST 替换,展开为语法树。支持表达式/语句/
赋值参数,迭代展开 + 深度上限(宏可调用宏)。

```myp
macro repeat($n, $body) {
    for (int _i = 0; _i < $n; _i++) { $body }
}
macro addN($x, $n) { $x = $x + $n; }   // 赋值参数($x 作赋值目标)
macro twice($body) { $body $body }      // body 重复展开两次

@constructor Main() {
    int v = 0;
    repeat(3, v = v + 10);   // → for ×3: v = 30
    addN(v, 5);              // → v = 35
    twice(v = v + 1);        // → v = 37
    Console.writeString("v=");
    Console.write(v);        // v=37
}
```

**表达式/值位展开(M-1,v3.15.244)**:宏体为**恰一条 ExprStmt**(单表达式宏,如
`macro twice($e) { ($e)+($e); }`)时,可在任意**表达式位置**调用并展开为表达式:
`int r = twice(21);` 得 42;可作实参/三元/赋值 RHS/return/嵌套(宏内嵌宏递归)。
多语句宏体仍只能语句位(表达式位 clean 拒绝)。
**宏卫生(M-3,v3.15.241)**:宏体**自声明的局部**展开时自动 gensym(原名 →
`原名_m<seq>`),不再与调用方同名变量冲突(此前 `int tmp=7; setV(tmp,1);` 报
duplicate variable)。

### 11.4 过程宏 `@macro` + `quote`(全功能,v3.6)

`@macro` 注解修饰的**函数**在编译期执行,函数式地构造/拼接 AST——支持循环、条件、
算法驱动代码生成(声明式宏做不到"根据 n 生成 n 条语句")。`quote { ... }` 是编译期
表达式:把代码块解析为 AST 语句集合;`$x` 插值——数值→对应字面量、字符串→标识符
(变量名/赋值目标)、AST 值→内联。编译期 AST 类型 `Expr`/`Stmt`/`StmtList`,
支持 `StmtList + StmtList` 拼接。`@macro` 函数**不生成运行时代码**(sema 跳过 body、
codegen 不 emit)。

```myp
@macro StmtList genAssign(string name, int value) {
    return quote { int $name = $value; };   // 字符串→标识符,数值→字面量
}
@macro StmtList makeCalls(int n) {
    StmtList out = quote {};
    for (int i = 0; i < n; i++) {
        out = out + quote { Console.write($i); };
    }
    return out;
}

@constructor Main() {
    genAssign("x", 42);        // → int x = 42;
    Console.write(x);          // 42
    makeCalls(3);              // → 生成 3 条 Console.write(i) → 0 1 2
}
```

### 11.5 安全与诊断

- 全部编译期执行,沿用纯函数约束(禁 `new`/I/O/线程/事件)。
- 宏迭代展开深度上限、`@eval` 递归深度上限、插值/返回类型不匹配
  (如 `$x` 是 int 但需语句)→ 编译期诊断,编译器"永不崩溃"。
- `--macro-expand` 输出展开后 AST,便于核对展开结果。
- 语言规格 1.0 不变:`@` 注解修饰已有声明(`@eval`/`@macro`/`@test`/`@coro` 同列),
  `macro` 关键字与 `where`/`await`/`operator` 同列后加;`quote` 为上下文关键字
  (仅 `quote {` 识别,`char quote = ...` 变量不受影响)——回归无冲突。
- EBNF 见附录 §13 / `docs/grammar.md`。

---

## 12. 实现状态

### ✅ 已完成功能

| 功能 | 版本 | 说明 |
|------|------|------|
| 基础类型系统 | 1.0 | int, double, string, bool, long 等 |
| Class + action/event/property | 1.0 | 组件模型三要素 |
| 事件触发 + 同步派发 | 1.0 | event → mapping → action |
| @thread / @threadpool | 1.0 | 每个实例独立线程 + 线程池 |
| 链式 mapping | 1.5 | 多节点链，前一个返回值传递给下一个 |
| 导入系统 | 1.5 | import + stdlib 路径搜索 |
| FFI | 2.1 | 直接调用 C 函数 |
| 泛型 | 2.1 | 类级别类型参数 + 单态化 |
| 枚举 + 模式匹配 | 2.1 | sealed enum + match 表达式 |
| Lambda 表达式 | 2.1 | 隐藏类 + __call 方法 |
| 包管理系统 (myp) | 2.1 | init/build/install/run |
| LSP 服务器 | 2.1 | 补全/悬停/跳转定义 |
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
| Atomic 操作 | 2.3 | Atomic.addDouble/Atomic.addInt 基于 LLVM atomicrmw |

### 技术栈

```
MYPLanguage/
├── tools/               # 自举工具链（MYP 实现）
│   ├── pm/              # 包管理 CLI（main/meta/build/install/util/lockfile/registry → build/myp）
│   ├── fmt/             # 格式化器（lexer/fmt/main → build/myp_fmt2）
│   ├── viz/             # 可视化器（lexer/viz/main → build/myp_viz2）
│   └── selfhost/        # 自举编译器（token/lexer/ast/parser/sema/codegen/ir_emit/link → myp_self）
├── CMakeLists.txt
├── LICENSE
├── README.md / README_EN.md
├── scripts/
│   └── mirror_gitee.sh   # gitee 镜像推送
├── include/
│   └── mylang/
│       ├── AST.h              # AST 节点（含 NewExpr/NewArrayExpr/MacroParamExpr/QuoteExpr）
│       ├── CodeGen.h          # LLVM 代码生成器
│       ├── DiagnosticEngine.h
│       ├── Eval.h             # @eval 编译期求值器（EvalValue）
│       ├── Fmt.h              # 格式化器
│       ├── LSP.h              # 语言服务器
│       ├── Lexer.h
│       ├── Macro.h            # 声明式宏（MacroExpander）
│       ├── MypPasses.h        # 自定义 LLVM pass（myp-pass）
│       ├── Parser.h
│       ├── Sema.h
│       ├── SourceLocation.h
│       ├── SymbolTable.h
│       ├── Token.h
│       ├── Type.h
│       └── runtime.h
├── src/
│   ├── main.cpp               # C++ seed 编译器入口（驱动各 phase；构建 mypc-seed）
│   ├── myp_viz.cpp            # 可视化工具
│   ├── token.cpp / SourceLocation.cpp
│   ├── ast/
│   ├── lexer/
│   ├── parser/
│   ├── sema/                  # sema.cpp + symbol_table.cpp + type.cpp
│   ├── codegen/               # codegen.cpp + myp_passes.cpp + codegen_fixes.h
│   ├── runtime/               # runtime.c（事件/线程/协程/arena/GPU）+ runtime_gpu.c
├── runtime_myp/              # MYP 运行时（runtime 的 MYP 实现，de-gcc 迁移：shadow C runtime → 归档 libmyp_rt_myp.a，仅 MYP 链接 (MYP runtime only)；进度/构建注意见 runtime_myp/MIGRATION_STATUS.md）
│   ├── eval/                  # @eval 解释器（eval.cpp）
│   ├── macro/                 # 宏展开（macro_expand.cpp）
│   ├── fmt/                   # 格式化器
│   ├── lsp/                   # LSP 服务器（lsp_server.cpp）
│   └── dap/                   # DAP 调试适配器（dap_server.cpp → myp_debug）
├── stdlib/                    # 标准库（纯 MYP class，40+ 库，见 §10.2）
│   ├── env / io / fs / text / stream / math / random / time / timeline / date
│   ├── collections / setops / option / result / atomic / pool / barrier / future / sync / memory
│   ├── coro / async / channel / net / http / json / regex / base64 / process / args
│   ├── logger / fmt / crypto / rtti / sdl / ui / error / cuda / gpu
│   ├── gpu/                   # GPU 子模块（点分模块名：import gpu.hal;）
│   └── test
├── tests/
│   ├── run_tests.sh           # 回归测试（-O0）
│   ├── run_tests_O2.sh        # 回归测试（-O2）
│   ├── run_tests_asan.sh      # ASAN 回归
│   ├── run_tests_tsan.sh      # TSan 回归
│   ├── test_debug.sh / test_myp_pass.sh / test_dap.py
│   ├── regression_no_crash.sh / fuzz_test.py
│   ├── expected/              # 预期输出
│   ├── negative/              # 负测试（编译错误验证）
│   └── <feature>/             # 每特性一个目录（test.myp + expected）
├── examples/                  # 完整示例（hello/fib/ad/BNCT/sdl/tui 等）
├── BNCTDoseEngine/            # BNCT 蒙特卡洛引擎（纯 MYP + HDF5 截面）
│   ├── transport / physics / material / nuclide / mesh / tally
│   ├── xs_loader / hdf5_bridge.c / cross_section_db.myp
│   └── ...
├── (mypdeeplearning)          # 深度学习推理/训练框架 → 独立仓 https://gitee.com/tomatosoft_0/mypdeeplearning
├── vscode-myp/                # VS Code 扩展（语法高亮 + LSP + DAP 调试）
├── docs/                      # 设计文档（design/grammar/manual/manual_en/coro/
│   └── ...                    #   exceptions/operators/metaprogramming/constructor/
│                              #   optimization_debugging/slice/UPGRADE_V3/CHANGELOG）
├── build/                     # 构建产物
│   ├── mypc              # MYP 编译器（自举不动点，myp_self2 副本）
│   ├── mypc-seed         # C++ seed 引导编译器（冻结）
│   ├── myp_debug         # DAP 调试适配器（gdb MI 桥）
│   ├── myp_lsp           # MYP 语言服务器
│   ├── myp               # 自举包管理（tools/pm）
│   ├── myp_fmt2          # 自举格式化器（tools/fmt）
│   ├── myp_viz2          # 自举可视化器（tools/viz）
│   ├── myp_viz           # 可视化工具（C++ 版）
│   └── myp_fmt           # 格式化工具（C++ 版）
└── build-asan/               # ASAN/UBSAN 构建
```

### 12.1 第一版范围（核心先行）

第一版只实现语言核心最小集：

```
Lexer    → 完整词法（含全部类型和关键字）
Parser   → 完整语法解析
Sema     → 符号表 + 类型检查
CodeGen  → 表达式 + 控制流 + 函数 + 基本 class（不含 event/mapping 运行时）
Runtime  → print/println + 基本运行时
```

### 12.2 当前状态与后续计划

**当前全部已实现：**

#### 语言核心
- ✅ 完整词法/语法/语义分析
- ✅ LLVM IR 代码生成 + 链接
- ✅ `--emit-llvm` 导出 IR
- ✅ 多文件编译（合并 AST 后单次 sema/codegen 通过）
- ✅ 错误恢复（解析错误后继续）
- ✅ `-o`, `-O[0123]` 编译选项

#### Class 系统
- ✅ 四段式 class（action/event/property/function + `static:`/`struct:` 段）
- ✅ `@constructor` 注解 / 函数名==类名（对象初始化，`new` 自动调用）
- ✅ `@startup`（启动信号/开始操作，线程/事件循环启动时执行）
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

#### 标准库（40+ 库全为纯 MYP class；完整清单 §10.2、规划表 §10.19、各库介绍 §10.5–§10.18）
- ✅ `env`：Console（write/writeString/writeLine/writeFloat/writeBool/writeLong/readString/kbhit/getch/flush）+ 环境变量 getEnv/setEnv/unsetEnv
- ✅ `timeline`：Timeline/Stopwatch（定时器事件）；`math`：泛型数学；`io`：File 文件读写
- ✅ 并发：`sync`/`atomic`/`pool`/`barrier`/`future`（§10.13）、`coro`/`async`/`channel`（§10.14）
- ✅ 数据：`collections`/`json`/`regex`/`option`/`result`/`stream`/`setops`/`text`
- ✅ 系统：`fs`/`process`/`args`/`date`/`base64`/`logger`/`memory`/`rtti`/`fmt`/`crypto`/`error`/`test`
- ✅ 网络/GPU/UI：`net`/`http`（§8.6.3）、`gpu`/`cuda`/`gpu/` 子模块（§10.16）、`ui`/`sdl`（§10.17）
- ✅ `--stdlib` 编译器选项 + 自动路径检测

#### 运行时
- ✅ 终端原始模式 + 非阻塞键盘输入（kbhit/getch）
- ✅ 二进制文件 I/O（read_byte/read_i32be/write_byte/write_i32be/write_double/read_double）
- ✅ 权重持久化（深度学习模型保存/加载）
- ✅ ARC 内存管理（class/string/数组/slice/struct 字段全计数；`_Atomic` rc 跨线程原子 + 全局自旋锁分配链；`@weak` 弱引用自动置空；`Memory.*` 内存诊断与失败注入）
- ✅ `@region` 区域内存（事务/帧级批量回收）+ 非类分配 bump arena
- ✅ 协程运行时（x86-64 asm / ucontext 切换 + 调度器 + 栈池 + 帧 ARC，见 §8.6.2）
- ✅ GPU 运行时（`runtime_gpu.c` dlopen 胶水 + CPU 一等后端回退，见 §10.16）
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
- ✅ BNCT 剂量引擎（`BNCTDoseEngine/`，事件驱动 + `@parallel for` + HDF5 截面）
- ✅ ONNX 推理框架（原 `deeplearning/infer/`，已迁出 → mypdeeplearning 独立仓）
- ✅ 编译器自举（`tools/selfhost/` → `myp_self` 重建编译器，不动点字节全同）
- ✅ 多线程并行计数
- ✅ 多线程独立定时器

**版本实现历史（v2.0 → v3.12，均已完成）：**

| 版本 | 特性 |
|------|------|
| **v2.0** | 字符串插值 `"Hello, $name"`、类型推断 `var x = 42`、Range `0..10`、`myp viz` 可视化工具 |
| **v2.1** | 泛型（monomorphization）、枚举 + 模式匹配、Lambda/闭包、FFI、包管理器（myp）、LSP、VS Code 扩展、共享/静态库（--shared/--static） |
| **v2.2** | 内置测试框架（@test + --test）、myp fmt 格式化、标准库扩充 |
| **v2.3** | Barrier 同步、Future/Promise、Atomic 操作、Stream 流类型、接口多态、mapping @scope/where/lambda 节点/delay/throttle、TUI（ui.myp） |
| **v2.4** | 错误处理完善（finally/throw;/对象异常/接口匹配）、`@parallel for` + 工作窃取线程池 + 并行体捕获/数学/静态方法、Barrier/Future stdlib 封装 |
| **v3.0** | 协程完整体系（C1-C10：`@coro`/`await`/调度器/事件等待/超时/取消等，详见 `coro.md`）、协程 Channel/await Future、事件队列动态化、property 默认值修复、`long` 后缀、Class 级 `const`、Range for |
| **v3.1** | IR 优化管线（-O1/-O2/-O3 NewPM） |
| **v3.2** | DWARF 调试信息（-g） |
| **v3.3** | 自定义 pass（--passes myp-pass） |
| **v3.4** | `@eval` 编译期求值 + 泛型约束 `where T : Interface` |
| **v3.5** | 声明式宏 `macro` |
| **v3.6** | 过程宏 `@macro` + `quote` |
| **v3.7** | DAP 调试（`myp_debug`，VS Code 断点/单步/变量） |
| **v3.8** | 集合动态扩容、泛型 `new T[n]`、`function:` 跨方法、LSP 解析死循环修复、`memory.myp` 修复 |
| **v3.9** | class 实例 ARC、构造器/`@startup` 语义迁移、`Option<T>`/`T?`/`Result<T,E>`、RTTI、`sync` 同步原语、统一异步 IO（`Coro.waitAnyOf`）、`slice<T>`、元组、`fmt`/`crypto`/`http` 库、包管理器 v2（registry/lockfile） |
| **v3.10** | showcase/probe 差分测试驱动的语言修复、系统探测 |
| **v3.11** | 定宽整型别名（int8/16/32/64、uint8/16/32/64）、协程/Channel 性能（rendezvous）、C 运行时 -O2、perf 优化 |
| **v3.12** | 内存系列收尾：`string`/`T[]`/`slice` 引用计数 + in-place 字符串拼接（O(n²)→O(n)）、struct 引用字段值语义 ARC、跨线程原子 ARC（M6）、`@weak` 弱引用（M7）、内存诊断/失败注入/strict 校验（M9）、协程句柄世代化 + 栈池字节上限（M1/M2） |
| **v3.13 → v3.15** | 编译器/自举/运行时里程碑（自 v3.13.0 起条目独立递增）：自举收口（不动点字节全同）、`runtime_myp` de-gcc 迁移、协程/异常线程安全硬化、元编程补齐（M-1/M-2/M-3）等——逐条见 `docs/CHANGELOG.md` |
| **v3.16.0** | 里程碑批次（首个 git annotated tag `v3.16.0`）；此后 minor=功能/里程碑批次（收敛后升 + tag）、patch=批次内小修复 |

**未来 / 规划中：**

- ✅ **工具链自举**（`tools/pm`、`tools/fmt`、`tools/viz` 已落地，见 `docs/self_hosting.md` T1–T3）
- 🔜 **Event-driven Pool（方案 B）**——事件驱动工作分发池：Pool 持有 work-stealing 队列 + N 个 Worker 线程，通过 mapping 接收任务 → 自动分派给空闲 Worker → 结果事件汇总到 Tally。纯运行时方案，不改编译器。
- **编译器自举（T5，`tools/selfhost`）——已完成**：用 MYP 实现编译器前端 + codegen + GPU（roadmap 终极目标）。**已实现**：
  - **F0–F4 前端**：`mypc --frontend-dump {tokens,ast,sema}` Oracle 契约（`format.md`）+ `token.myp`/`lexer.myp`/`ast.myp`/`parser.myp`/`sema.myp`——token/AST 对拍 **448/448** 字节一致、sema 正语料 548/565。
  - **G1–G2 codegen**：`ir_emit.myp`/`codegen.myp`（LLVM IR 文本发射）——hello 级运行对拍；控制流/数组/字符串/短路/intrinsic/`slice<T>`/定长数组/lambda(按值捕获)/元组/默认命名参数均已对拍。
  - **G3 类/ARC/异常/泛型 codegen**：已完成——@static/实例/构造器/属性默认值/成员访问/泛型单态化/ARC/function 段/异常全链路/mapping/@startup/协程/@parallel for/@test 均已对拍落地。
  - **GPU（已实现）**：`@gpu for`/`@gpu tile`（`__shared__` smem）/`@gpu scatter`/`@gpu reduce`/`@gpu scan` 均发射 **NVPTX kernel**（`nvptx64-nvidia-cuda` → `llc -mcpu=sm_75` → PTX 嵌入 `myp_gpu_load_kernel` 启动），GPU/CPU 双路径；`-mcpu=sm_75` 与 C++ oracle `gpuTargetArch()` 对齐——默认老架构不支持 double 原子，`atomicrmw fadd` 会降级成 `atom.cas` 循环（高竞争重试风暴，`Vectors.sum` 1M 元素 25s），加 sm_75 后直降 `atom.add.f64`（3ms，与 seed 持平）；CUDA intrinsics（`__myp_cuda_*`/`__myp_gpu_*`）已注册并映射到 `myp_gpu_*` 运行时；链接接入 `runtime_gpu.c`（dlopen 胶水）。
  - **P2/P3 完全自举**：`myp_self run/fmt` 已去委托（不再调 mypc）；仅用 `myp_self` 重建编译器与全部 MYP 工具（self2→self3→self4 **不动点字节全同**，md5 一致）；opt 加 `-mtriple` 启用 TTI 向量化对齐（matmul 2.43x→1.00，多数基准持平）。
  - **范围**：GPU 已入自举范围；C 运行时 `runtime.c` 视作 libc 保留 C（外部）。链接仍经 LLVM 后端（opt/llc/gcc）。
  - **收口（已闭合）**：G3 剩余（mapping/@startup/协程/ARC/@parallel for/@test）、G4 驱动/链接（run/fmt 原生化、link 接 llc/opt/gcc）、性能基线（≤10x）、文档收口 均已完成；仅剩 P4 已知项（`-O2`×异常展开、`--emit-llvm` i0 类型瑕疵、nqueens/alphabeta 基准源，见 `tools/selfhost/roadmap.md` P4 表）。
- 🔜 **`const` 参数（只读承诺）**——函数参数标 `const`，函数体内不允许修改该对象，
  编译期保证「接收方只读」；定位为 API 只读契约（对应 §8.4 只读契约的补强），
  不承担线程安全职责。
- 🔜 **JIT**
- 🔜 **神经形态后端**

---

---

## 13. 附录：EBNF 语法

> **注**：完整权威 EBNF 以 `docs/grammar.md` 为准；本附录为简化历史草稿（保留主要结构，细节以 grammar.md 为准）。

### 词法规则

```
LineComment      ::= "//" {.} newline
BlockComment     ::= "/*" {.} "*/"
Identifier       ::= (Letter | "_") { Letter | Digit | "_" }
IntegerLiteral   ::= "0" ("x" | "X") HexDigit { HexDigit }    // 0x 十六进制
                   | "0" ("b" | "B") ( "0" | "1" ) { "0" | "1" }   // 0b 二进制
                   | "0" ("o" | "O") OctalDigit { OctalDigit }     // 0o 八进制
                   | Digit { Digit }                               // 十进制
                   // 后缀 L/l → LongLiteral、U/u → UIntLiteral；下划线分隔（编译期剥离）
FloatLiteral     ::= Digit { "." Digit } [ ("e" | "E") ["+" | "-"] Digit { Digit } ]
                   // 后缀 f/F → FloatLiteral32（仅浮点字面量）
CharLiteral      ::= "'" ( Char | Escape ) "'"
BoolLiteral      ::= "true" | "false"
StringLiteral    ::= '"' { Char | Escape } '"'
NullLiteral      ::= "null"

Letter           ::= "A".."Z" | "a".."z"
Digit            ::= "0".."9"
HexDigit         ::= Digit | "A".."F" | "a".."f"
OctalDigit       ::= "0".."7"
Escape           ::= "\" ( "n" | "t" | "r" | "e" | "\" | '"' | "'" | "0" )
```

### 语法规则

```
Program          ::= { ImportDecl | StructDecl | ClassDecl | InterfaceDecl | MappingDecl | FuncDef
                     | EnumDecl | FFIDecl | MacroDecl | TypeAliasDecl | OpFuncDecl }

ImportDecl       ::= "import" ( Identifier | StringLiteral ) ";"

StructDecl       ::= "struct" Identifier "{" { VarDecl | FuncDef | OpSection } "}"
                   | "struct" Identifier "::" Identifier "{" { VarDecl | FuncDef | OpSection } "}"
OpSection        ::= "operator" ":" { OpFuncDecl }
OpFuncDecl       ::= OpAnnot Type Identifier "(" [ ParamList ] ")" Block
OpAnnot          ::= "@" "op" "(" StringLiteral ")"

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

VarDecl          ::= Type Identifier [ "=" Expr ] [ Annotation ] ";"   // 显式类型（可多声明符 Type a, b;）
                   | "var" Identifier [ "=" Expr ] ";"                 // 类型推断（v2+）
                   | "const" Type Identifier [ "=" Expr ] ";"          // 常量（须初始化）

Type             ::= BasicType | Identifier [ "::" Identifier ] | ArrayType | "void"
BasicType        ::= "byte" | "short" | "int" | "long"
                   | "ubyte" | "ushort" | "uint" | "ulong"
                   | "uint8" | "uint16" | "uint32" | "uint64"
                   | "int8" | "int16" | "int32" | "int64"
                   | "float4" | "double2" | "int4"
                   | "bit" | "bitvector" [ "<" IntegerLiteral ">" ]
                   | "char" | "float" | "double" | "bool" | "string"
ArrayType        ::= Type "[" [ IntegerLiteral ] "]"

Statement        ::= VarDecl | ExprStmt | IfStmt | WhileStmt | ForStmt | ForInStmt
                   | ReturnStmt | BreakStmt | ContinueStmt | Block | MappingStmt
                   | MatchStmt | TryStmt | ThrowStmt | AwaitStmt | NonlocalStmt
                   | ParallelForStmt | GpuForStmt
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
Postfix          ::= Primary { "." Identifier | "." Integer           // 成员访问 / 元组字段 t.N
                             | "[" Expr "]" | "(" [ ExprList ] ")"
                             | "++" | "--" }
Primary          ::= Literal | Identifier | "(" Expr ")" | "this"
                   | "new" Identifier [ "<" TypeList ">" ] "(" [ ExprList ] ")"  // 泛型构造
                   | "new" Type "[" Expr "]"                       // 数组分配
                   | LambdaExpr                                    // (params) => { body }
                   | TupleLiteral                                  // (a, b, …)（顶层逗号）
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

## 14. 附录：完整示例

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
