# MYP 类构造器设计（Constructors / Copy / Assignment）

> 状态：**已实施**（v3.9.0）——M1-M4 完成 + @startup→构造器迁移完成
> 语法：`@constructor` 注解 + 函数名==类名隐式构造器
> 关联：语言规格 v1.0（`docs/grammar.md`）、版本策略（`docs/CHANGELOG.md`）、
> 类系统（`docs/manual.md` §6）、算子系统（`docs/operators.md`）

---

## 1. 背景与现状

### 1.1 当前缺口

| 项 | 现状 | 问题 |
|---|---|---|
| `new ClassName(args)` | 绑定到 `@startup init(args)`（legacy 构造器模式），参数正确传递（实测 `new C(42)` → `C_init(inst, 42)`） | `@startup` 被**误用为构造器**——它本义是"启动信号"，却被拿来初始化；单 `init` 无重载/无默认 |
| 实例初始化 | 靠 `@startup void init(...)` | `@startup` 承担双重语义（启动信号 + 初始化），语义混淆；无法重载、无参数默认 |
| 对象参数化构造 | 只能 `@startup init(args)` 或 setter 逐个赋值 | 无一等构造器；`@startup init` 与线程入口耦合（并行代码中应只做"启动信号"） |
| 泛型分发 | 单态化时有 bug（`new Box<double>()` 曾误调模板 init，T 占位成 int 致堆损坏，v3.8.0 已规避但机制脆弱） | 构造器绑定单态化类可根治 |

> **核心认知（重要）**：`@startup` 的语义是**并行/事件驱动代码中的启动信号**——
> 实例的线程/事件循环启动时"开始操作"的入口（如 `@thread` 启动、启动定时器、触发首事件）。
> 它**不是**初始化器。当前 stdlib 把 `@startup void init(...)` 当构造器用是**历史误用**；
> 本设计的构造器让初始化归位，`@startup` 回归启动信号，两者**正交、互不取代**。

### 1.2 目标

1. 一等公民**构造器**（`@constructor` 注解；函数名==类名时隐式视为构造器）：
   `new ClassName(args)` 优先绑定构造器，支持重载/默认值——这是**对象初始化**的正规机制。
2. **`@startup` 回归"启动信号"语义**：并行/事件驱动代码中实例开始操作的入口，
   不再兼任初始化。构造器与 `@startup` **正交、互不取代**（构造器=new 时初始化；
   @startup=启动信号）。
3. 构造器**绑定到单态化实例类**，从根上消除泛型分发 bug。
4. 明确 **拷贝构造** 与 **`=` 赋值算子** 的语义与三者关系（本文 §4/§5/§6）。

### 1.3 非目标（本期不做）

- 构造器默认参数 / 委托构造 / 初始化列表（保持简单，重载覆盖多数场景）。
- struct 聚合/花括号初始化 `Vec2 v = {1.0, 2.0}`（列 P2 语法糖；本期用函数式构造）。
- struct `=` 定制重载（列 P2；本期保持成员级拷贝）。

---

## 2. 语义模型：引用 vs 值（设计的地基）

MYP 的核心区分（manual §7 `struct vs class`）：

| 维度 | `class` | `struct` |
|---|---|---|
| 分配 | 堆（`new`） | 栈 |
| 传递 | **引用（指针）** | 值拷贝 |
| 运算符重载 | ❌（设计哲学：class 用 action/event/mapping，**不用数学算子重载**，见 `operators.md` §2.1） | ✅ `operator:` 节 |

**推论（本文所有设计的出发点）**：

1. **class 是引用类型**——`A b = a;` 的语义必须是"别名"（两变量指向同一对象），
   与 Java/Python/Go 一致。**任何隐式深拷贝都违背引用语义且令人惊讶**。
2. **class 不做算子重载**（既有设计原则）——因此 `=` 对 class **不提供用户重载**。
3. **struct 是值类型**——`=` 本来就是成员级拷贝，无需构造器干预。

---

## 3. 类构造器（Constructor）

### 3.1 语法：`@constructor` 注解 + 函数名==类名隐式构造器

构造器是 `action:`（或 `function:`）里加 `@constructor` 注解的方法；**当函数名与
类名一致时，默认视为构造器**（可省略 `@constructor`，与 C++/Java 一致）：

```myp
class Window {
    action:
        @constructor
        void Window() {                     // 显式 @constructor：无参构造
            x = 0; y = 0; w = 80; h = 24;
        }
        void Window(int px, int py, int pw, int ph) {  // 函数名==类名 → 隐式构造器（重载）
            x = px; y = py; w = pw; h = ph;
        }
    property:
        int x, y, w, h;
}
```

**规则**：
- `@constructor` 注解可加在 `action:`/`function:` 方法上，标记其为构造器。
- **隐式规则**：方法名 == 类名 → 自动视为构造器（等效于加 `@constructor`）；
  同名重载即多个构造器。显式注解与隐式命名**等价**。
- 构造器**无返回类型**（`void`）。
- 构造器体可用 `this`、可读/写 property、可调用 `function:`/`action:` 方法。
- **构造器 ≠ `@startup`**：构造器是 `new` 时同步的对象初始化；`@startup` 是
  并行/事件驱动代码中的**启动信号**（开始操作）。两者正交、可共存（见 §3.5）；
  同一方法不能同时标 `@constructor` 与 `@startup`。

### 3.2 `new` 绑定与重载解析

```
new ClassName()          → 绑定无参构造（若声明）；否则默认（分配 + property 默认值）
new ClassName(args)      → 按签名重载解析（参数个数 + 类型）
```

- **重载解析规则**（sema 编译期）：按实参类型逐参数匹配；数字可隐式提升
  （`int → long → double`）；歧义 → 编译报错。
- **参数个数不匹配 / 无匹配构造** → 编译报错（**不再静默忽略**——这是对现状的行为修正）。

### 3.3 执行顺序（关键）

```
new ClassName(args)
  1. 分配实例（arena）
  2. 应用 property 默认值（`int x = 5;` 等）
  3. 调用构造器体（可覆写默认值）   ← 对象初始化（同步）
  ...
  （实例开始操作时——@thread 线程启动 / 事件循环开始——调用 @startup） ← 启动信号
```

> 构造器在 `new` 时同步执行（对象初始化）；`@startup` 在实例**开始操作**时执行
> （启动信号）。两者不同时、不同职责，**互不取代**。

### 3.4 泛型构造器（根治 @startup 分发 bug）

- 构造器绑定到**单态化实例类**：`new Box<double>()` → 调用 `Box_double_inst` 的构造器
  （codegen 用 `current_type_params_` 解析 `T`，元素/参数类型正确）。
- 模板类自身的构造器**不注册**到实例分发，从根上避免"误调模板 init"。
- 泛型参数作构造器参数：`@constructor void Box(T v) { data_ = v; }`（函数名==类名时
  亦可省略注解）→ 实例化时 `T` 替换为实参类型。

### 3.5 与 `@startup` 的关系（正交，互不取代）

**核心**：构造器与 `@startup` 是两回事，**不能互相取代**（用户明确）：

| | 构造器 | `@startup` |
|---|---|---|
| 语义 | 对象**初始化**（`new` 时同步，状态准备） | **启动信号**（并行/事件驱动代码中实例开始操作） |
| 时机 | `new` 时 | 线程启动 / 事件循环开始 / 开始操作时 |
| 举例 | 设字段、分配资源、校验 | `@thread` 启动、启动定时器、触发首事件、进入处理循环 |
| 角色 | C++ 构造器 / Java 构造器 | Java `Runnable.run()` / Actor `preStart` / Go goroutine 体 |

**现状修正**：`@startup void init(...)` 目前被**误用为构造器**（`new` 时同步调用 + 带参，
json/net/fs/regex/time 等 stdlib 依赖它）。本设计把初始化归位给构造器，
`@startup` 回归启动信号。

**迁移策略（✅ 已全部执行，v3.9.0）**：

| 步骤 | 内容 | 状态 |
|---|---|---|
| 1 | 实现 `@constructor` 注解 + 函数名==类名隐式构造器识别，`new C(args)` 绑定构造器 | ✅ M1-M3 |
| 2 | **全部迁移** `@startup init(...)` → 构造器（stdlib 8 文件 + deeplearning + 41 测试 + examples/docs） | ✅ |
| 3 | **移除** `new C(args)` 自动调 `@startup` 的 legacy 绑定（codegen）——`@startup` 严格只作启动信号 | ✅ |
| 4 | 空占位 `@startup void init() {}`（stream/layers）直接删除 | ✅ |

**原则**：`@startup` 只保留"并行/事件驱动代码中开始操作"的语义（@thread 启动、
启动定时器、触发首事件）；初始化一律走构造器。两者语义不混淆。

### 3.6 约束

- 构造器不能是 `@coro` / `@test` / `@eval` / `@macro`。
- 构造器不能同时是 `@startup`（构造器=初始化，`@startup`=启动信号，互斥）。
- 构造器不能被直接调用（只在 `new` / 函数式构造 `Struct(args)` 时隐式调用）。
- 构造器内不允许 `return` 值（无返回类型）。

---

## 3A. struct 的处理（值类型）

class 是引用、struct 是值——**同一构造器特性（`@constructor` + 函数名==类名），两种调用形式**：

| | class | struct |
|---|---|---|
| 构造调用 | `new Class(args)` | `Struct(args)`（函数式构造） |
| 存储 | 堆（引用） | 栈（值） |
| 默认拷贝 | 引用别名（不拷贝） | 成员级拷贝 |
| 深拷贝 | `copy()` 显式 | `copy()` 显式 |
| `=` | 引用重绑定 | 成员级拷贝 |

### 3A.1 struct 构造器（`@constructor` 注解同样适用）

```myp
struct Vec2 {
    action:
        @constructor
        void Vec2(double px, double py) { x = px; y = py; }  // 显式注解
        void Vec2() { x = 0; y = 0; }                        // 函数名==类名 → 隐式构造器
    double x, y;
}
```

**调用：函数式构造 `Struct(args)`**——像调用函数一样构造一个 struct 值（栈临时量）：

```myp
Vec2 v = Vec2(1.0, 2.0);       // 声明 + 初始化
return Vec2(3.0, 4.0);          // 作为返回值
o.p = Vec2(5.0, 6.0);           // 赋给 struct 字段
Vec2 w;  w = Vec2(7.0, 8.0);    // 先声明后赋值
```

- `Struct(args)` 需要匹配的构造器声明，否则编译报错。
- `Vec2 v;`（声明）保持零初始化（无构造器时）；需要定制初始化就用 `Vec2 v = Vec2(...)`。
- 与 class 的差异只在**调用形式**（`new` vs 函数式）；构造器体语法/重载/语义完全一致。
- 构造器体可用 `this`、可算派生字段、可校验（如 `Fraction(num,den)` 里 `den != 0`）。

> **为什么不引入聚合初始化 `Vec2 v = {1.0, 2.0}`？** 聚合只能按位置填成员，
> 无校验、无派生字段、无重载；函数式构造 `Vec2(1.0,2.0)` 能全部做到，且与 class
> 构造器统一（一个语言特性两种调用）。聚合初始化列为 P2 语法糖（若确有需要）。

### 3A.2 struct 拷贝 / 赋值

- **默认拷贝/赋值 = 成员级拷贝**（现有值语义，保持不变）：`Vec2 b = a;`、`b = a;`。
- **含指针字段时成员级拷贝是浅拷贝**（如 `struct Buffer { int[] data; }`，`b = a` 共享数组）。
- 需要深拷贝 → `copy()` 方法（与 class 同一约定）：

```myp
struct Buffer {
    action:
        @constructor
        void Buffer(int n) { data = new int[n]; }
        Buffer copy() {
            Buffer c = Buffer(size);
            for (int i = 0; i < size; i = i + 1) c.data[i] = data[i];
            return c;
        }
    int size;
    int[] data;
}
```

- **不引入 struct 拷贝构造** `Buffer(Buffer other)`——与成员级拷贝重复，无独立价值。
- **不提供 struct `=` 重载**（`@op("=")` 列 P2；成员级拷贝已满足值语义）。

### 3A.3 与 class / 嵌套的交互

- class 内 struct 字段：声明时零初始化；class 构造器可 `b = Border(1,2,1,2);` 赋值。
  （MYP 无成员初始化列表，构造器体显式赋值即可。）
- struct 的 `operator:` 节（数学算子）与 `@constructor` 构造器**可共存**，互不冲突。
- `Struct(args)` 是 rvalue；传给函数/返回时按现有 struct 值传递机制（已支持）。

---

## 4. 拷贝构造（Copy Constructor）

### 4.1 语义定位：显式，非隐式

class 是引用类型，因此 **`A b = a;` 必须是引用别名**，**不**调用任何拷贝构造
（Java/Python/Go 模型）。深拷贝只发生在**显式**请求时：

**方案（推荐）**：约定方法 `copy()`——**不需要新语法**，就是普通 `action:`：

```myp
class Image {
    action:
        @constructor
        void Image(int w, int h) { width = w; height = h; data_ = new int[w * h]; }
        Image copy() {                    // 显式深拷贝（约定方法）
            Image c = new Image(width, height);
            for (int i = 0; i < width * height; i = i + 1) c.data_[i] = data_[i];
            return c;
        }
    property:
        int width, height;
        int[] data_;
}
```

用法：
```myp
Image a = new Image(10, 10);
Image b = a;        // 引用别名（不拷贝）
Image c = a.copy(); // 显式深拷贝
```

**理由**：
- `copy()` 是显式的——调用点一目了然，不改变引用语义。
- 无需新语法/新关键字，零编译成本。
- 与既有 `function:`/`action:` 方法体系完全一致。

### 4.2 （可选，后续）`A(a)` 拷贝构造语法糖

若需要 `Image c = Image(a);` 这种"拷贝构造式 new"形式，可加一条：
```myp
action:
    @constructor
    void Image(Image other) { /* 深拷贝 */ }
```
`new Image(src)` / `Image(src)` 调用它。**注意**：这也只显式触发（`Image b = a;` 仍走引用别名）。

> **结论**：拷贝构造**不做**隐式触发。M1-M3 先做 `copy()` 约定（零语法）；
> `A(a)` 拷贝构造语法糖列为 P2（视需要）。

---

## 5. `=` 赋值算子（Assignment）

### 5.1 class：`=` 是引用重绑定，不提供用户重载

```myp
a = b;   // a 指向 b 的对象（引用重绑定），不是拷贝，不调用任何用户代码
```

- **不引入 `=` 重载**——与既有设计原则一致（`operators.md`：class 不用算子重载，
  用 action/event/mapping）。
- 若用户想要"赋值即深拷贝"，显式写 `a = b.copy();`。
- 防止 C++ 式陷阱：隐式拷贝赋值 + 析构会引发 double-free/悬垂引用，MYP 的 arena
  模型本就避免析构，更不需要 `=` 重载。

### 5.2 struct：`=` 已是成员级拷贝

```myp
Vec2 v1; Vec2 v2 = v1;   // 成员级值拷贝（现有行为，保持不变）
```

- struct 的 `=` 拷贝是**编译器生成**的，无用户干预。
- （可选，P2）允许 struct 用 `operator:` 定制 `=`——但当前成员拷贝已满足值语义，
  优先级低。

### 5.3 与 `@op` 系统的关系

- `@op("+")` 等数学算子仅作用于 struct 值类型（既有）。
- `=` **明确排除**在 `@op` 可绑定符号之外（对 class），避免语义混乱。

---

## 5A. 析构器（决策：不引入）

**结论：MYP 不需要析构器语言特性。**

### 5A.1 原因：arena 模型没有可靠的"析构调用点"

对象由 arena/region 分配（`myp_region_alloc`/`myp_alloc`），释放时机只有：
- **进程退出**（`myp_free_all`）
- **@region 作用域结束**（`myp_arena_release`）

正常函数作用域退出**不回收对象**。因此析构器没有可依赖的确定性触发点——
C++ 析构器靠栈展开/`delete` 保证确定性清理，MYP 无此生命周期模型。若强行加，
析构器只会在"进程退出"这种太晚、顺序不可靠的时机执行，给出**虚假的 RAII 保证**。

### 5A.2 现有清理机制已覆盖

| 需求 | MYP 现有手段 |
|---|---|
| 确定性资源释放 | 显式 `destroy()` / `close()`（`Coro.destroy`、`Barrier.destroy`、`File.close`、`Channel.close`） |
| 异常安全清理 | `finally` 块（所有退出路径执行） |
| 手动内存释放 | `Memory.free` / `Memory.release`（确定性） |

### 5A.3 与构造器的不对称是合理的

- **构造**可以保证（`new` 时必然执行）——对象创建是确定性的。
- **析构**无法保证（无确定性销毁点）——不该提供伪保证。
- 设计原则：**承诺"构造发生"，不承诺"析构发生"**；需要确定性清理就显式调用
  `destroy()`/`close()`。

### 5A.4 将来的"作用域退出清理"（若有需求）

应引入 **`defer { }`** 语句（Go 风格，作用域退出执行清理）或 @region 挂钩——
这是**语句级**机制，与对象生命周期解耦，不破坏 arena 模型。列为未来可选，与构造器正交。

---

## 6. 三者关系总结（Constructor / Copy / `=`）

| 操作 | 语法 | 语义 | 调用什么 |
|---|---|---|---|
| **构造（class）** | `new Class(args)` | 创建堆对象并初始化 | 匹配的构造器（或默认） |
| **构造（struct）** | `Vec2(x, y)` | 创建栈值并初始化 | 匹配的构造器 |
| **引用别名** | `A b = a;`（class） | b 与 a 指向同一对象 | 不调用拷贝构造 |
| **成员拷贝** | `Vec2 b = a;`（struct） | 成员级拷贝（浅） | 编译器生成 |
| **显式深拷贝** | `A c = a.copy();` | 创建深拷贝新对象 | 用户 `copy()` 方法 |
| **拷贝构造**（P2） | `A c = A(a);` | 创建深拷贝新对象 | 拷贝构造器 |
| **赋值（class）** | `a = b;` | 引用重绑定 | 不调用任何用户代码 |
| **赋值（struct）** | `s2 = s1;` | 成员级值拷贝 | 编译器生成 |

**三条铁律**：
1. **引用语义不可破坏**——class 的 `=` 与 `A b = a;` 永远是别名，不做隐式深拷贝。
2. **拷贝必须显式**——`copy()`（或 P2 的 `A(a)`），调用点清楚可见。
3. **`=` 不重载**——符合 MYP"class 不用算子重载"的既有哲学，避免 C++ 式陷阱。

---

## 7. 兼容性

- **additive 语法**：新增 `@constructor` 注解 + 函数名==类名隐式构造器识别，不删除/不改任何现有语法。
- 语言规格保持 v1.0（非破坏性变更，按 CHANGELOG 版本策略）。
- **一次性迁移（用户决定，不留 legacy）**：`new C(args)` 从"自动调 `@startup`"改为"绑定构造器"；
  当前库/测试里所有 `@startup init(...)` 迁到构造器（`@constructor` 或改名为类名）；
  `@startup` 严格只作启动信号。迁移后**移除** legacy 绑定，杜绝语义混淆。
- **调用点不破坏**：迁移只改类的内部定义（`@startup init` → `@constructor`/类名命名），
  调用点 `new Json("...")` 等保持不变（仍绑定同名构造器）。
- **`@startup` 启动信号语义保留**：@thread 线程入口等真正启动场景不变。

---

## 8. 实现里程碑

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M1 构造器语法 + AST** | parser：`@constructor` 注解 + 函数名==类名隐式构造器识别（class + struct）+ `ConstructorDecl`；sema：注册构造器签名 | `@constructor` 可解析；类名==函数名自动识别；无构造器类行为不变（legacy `@startup init` 继续工作） |
| **M2 class 构造器** | `new C(args)` 优先绑定构造器（否则 legacy 回退 `@startup init`）+ 重载解析（含数字提升）+ codegen（分配→默认值→构造体）；泛型绑定单态化实例类（根治 @startup bug）；负测试 | `new Window()` / `new Window(10,5,80,24)` 正确；`new Box<double>(1.5)` 正确；存量 `new Json("...")` 仍工作 |
| **M3 struct 构造器** | 函数式构造 `Vec2(args)`（栈临时 + 构造体）+ 重载；与 `operator:` 共存；负测试 | `Vec2 v = Vec2(1.0,2.0)` / `return Vec2(...)` / 字段赋值正确 |
| **M4 拷贝约定 + 文档** | `copy()` 约定（纯文档/示例，无新语法）；manual §6 构造器章节 + 文档澄清 `@startup`=启动信号；CHANGELOG v3.9.0 | `a.copy()` / `Buffer.copy()` 深拷贝示例通过；文档齐全 |
| **P2（可选）** | `A(a)` 拷贝构造语法糖；struct 聚合初始化；struct `@op("=")` 定制 | 视需求 |

**验证**：`tests/constructor/`（构造/重载/泛型/拷贝/负测试）；`-O0`/`-O2`/ASAN 全套回归不回归。

---

## 9. 决策记录（全部已确认）

1. **语法**：**`@constructor` 注解 + 函数名==类名隐式构造器**（✅ 已定，见 §3.1）。
2. **泛型构造器绑定**：**绑定单态化实例类、模板不注册**（✅ 已定，根治 @startup bug，见 §3.4）。
3. **struct 构造调用形式**：**函数式构造 `Vec2(args)`**（✅ 已定，见 §3A.1）；
   聚合初始化 `{...}` 列 P2。
4. **拷贝**：**`copy()` 约定**（✅ 已定，零语法，见 §4.1）；`A(a)` 拷贝构造语法糖列 P2。
5. **`=`**：**class 与 struct 均不重载**（✅ 已定，符合"class 不用算子重载"哲学，见 §5）；
   struct `@op("=")` 默认不允许，列 P2。
6. **迁移**：**彻底迁移、不留 legacy**（✅ 已定）——当前库/测试所有 `@startup init(...)`
   迁到构造器（`@constructor` 或改名为类名）；移除 `new` 自动调 `@startup` 的绑定；
   `@startup` 严格只作启动信号。
7. **析构器**：**不引入**（✅ 已定，arena 模型无确定性销毁点，见 §5A）。
   清理用显式 `destroy()`/`close()` + `finally` + `Memory`。

> ✅ 设计已通过审核，按 M1→M2→M3→M4 执行。
