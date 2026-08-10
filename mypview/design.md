# mypview — 轻量「类 QML」声明式 UI 库（设计文档）

> 状态：**M1 骨架 + M3 绑定已实施（2026-08-10）**——`src/{core,render,widgets,layout,binding,app}.myp`
> + `examples/{hello,counter}.myp` + `tests/test.myp`。M1 画出嵌套组件树（hello）；
> M3 验收通过（counter：按钮→事件→mapping→绑定→文本更新，counter=2/label=2/updates=1）。
> 开发中修复了 6 组编译器接口路径 bug（见提交 f2d405d，均在"bug 修正"例外内）。
> 定位：**类似 QML 的声明式界面库，但轻量** —— 只做"声明式组件树 + 属性绑定 +
> 事件接线 + 基础布局 + 一个渲染目标"，**不是** Qt/QML 的复刻。
> 纯 MYP 实现（可少量依赖 SDL2 FFI）。
> 关联：`stdlib/ui.myp`（TUI Screen）、`stdlib/sdl.myp`（SDL2）、事件/`mapping()`
> 模型（`docs/design.md` §11、`docs/grammar.md`）。

---

## 0. 硬约束（评审确认，必须遵守）

1. **不改编译器**：本库全部用现有 MYP 语言特性 + stdlib 实现，**不修改
   `src/`、不新增语言特性/语法/注解**。
2. **唯一例外**：开发中若发现**编译器 bug**（如 `tools/pm`/`tools/fmt`/`tools/viz`
   自举时挖出的那些），可**仅做 bug 修正**并同步回归（`tests/run_tests.sh` 181/181
   不破）。功能增强一律不碰编译器。
3. 因约束 1，绑定等新机制**只能用库方案**（见 D1 方案 A），不得依赖编译器支持。

---

## 1. 目标与验收

**目标**：让 MYP 的**事件驱动组件模型**直接映射为声明式 UI——组件是 `class`，
信号是 `event:`，接线是 `mapping()`，渲染目标是可替换的 `Renderer`。

**端到端验收**（评审通过后执行时以此为准）：
- 一个可运行的声明式 UI 应用：嵌套组件树 + 按钮点击 → 事件 → 属性绑定自动更新 →
  脏标记重绘，全部纯 MYP，**不修改编译器**（仅允许发现编译器 bug 时做 bug 修正）。

---

## 2. 范围边界

### ✅ 做
| 模块 | 内容 |
|------|------|
| 组件树 | `Widget` 基类 + 嵌套 children（组合式声明） |
| 事件接线 | 复用 `event:` + `mapping()`，组件内部事件 → 应用逻辑 |
| 属性绑定 | 轻量绑定引擎：源属性变更 → 目标属性/文本自动更新（脏标记） |
| 基础布局 | `Box`（垂直/水平）、`Flow`（自动换行）、`Stack`（重叠/Z 序） |
| 控件 | `Label`、`Button`、`TextBox`（最小可用集，够做示例即可） |
| 渲染 | `Renderer` 抽象 + `TuiRenderer`（ANSI，复用 `ui.myp` `Screen`） |
| 样式基础 | 前景/背景色、边框（复用 `Screen.fillRect/drawBorder/writeText`） |

### ❌ 不做（守住轻量边界）
- 场景图 / 重绘优化引擎、局部失效缓存
- 动画 / 转场框架
- 复杂主题 / 样式表系统（只做颜色/边框基础）
- 平台抽象层、跨平台后端（只锁定 TUI，SDL2 为可选二期）
- 无障碍 / 输入法 / 复杂焦点管理

---

## 3. 与现有能力的映射（零到少新增）

| QML 概念 | MYP 已有 | 本库需新增 |
|---------|---------|-----------|
| 声明式组件树 | `class` + `new` 嵌套 | 仅组织约定 |
| 信号 / 槽 | `event:` + `mapping()` | 无 |
| 属性 | `property:` + setter action | 无 |
| 属性绑定（数据流） | ❌ | **绑定引擎**（核心新增） |
| 生命周期 | `@constructor` / `@startup` | 无 |
| 渲染 | `ui.myp` `Screen`（ANSI） | `Renderer` 抽象层 |
| 输入 | `Console.getch()` / `kbhit()` | 命中检测 + 输入分发 |

> 结论：唯一真正的新机制是**属性绑定引擎**；其余全部是现有特性的组织与封装。
> 这保证了"轻量"——大部分代码是组件/布局/渲染的 MYP 实现，而非语言机制。

---

## 4. 架构

```
┌─────────────────────────────────────────────┐
│  Application（入口：初始化 + 事件循环）        │
└───────────────┬─────────────────────────────┘
                │ 输入事件（getch/kbhit）
┌───────────────▼─────────────────────────────┐
│  Widget 树（组件 class 嵌套）                 │
│  - draw()  → 交给 Renderer 画                │
│  - hitTest(x,y) → 命中子组件                  │
│  - onInput() → 触发自身 event                │
└───────────────┬─────────────────────────────┘
                │ event  → mapping()  → action
┌───────────────▼─────────────────────────────┐
│  Binding 引擎（属性变更 → 通知 → 脏标记）      │
└───────────────┬─────────────────────────────┘
                │ 重绘请求
┌───────────────▼─────────────────────────────┐
│  Renderer 抽象                               │
│  ├── TuiRenderer（ANSI，复用 ui.myp Screen） │
│  └── SdlRenderer（可选，二期）                │
└─────────────────────────────────────────────┘
```

**数据流（一次点击的完整闭环）**：
1. `Application` 事件循环读 `getch()`/`kbhit()`
2. 命中检测：从根 `Widget` 递归 `hitTest(x,y)` 找到被点组件
3. 组件触发自身 `event:`（如 `Button.clicked`）
4. `mapping()` 把事件接到应用 `action:`（纯用户代码）
5. action 修改某组件 `property`（如 `label.text`）
6. 绑定引擎检测到变更 → 标记脏 → 请求重绘
7. `Renderer` 全量重绘根组件（轻量阶段不做局部失效）

---

## 5. 核心 API 草案（MYP 签名级）

```myp
// core.myp —— Widget 基类
class Widget {
    property:
        int x, y, w, h;
        int fg, bg;
        Widget[] children;      // 组合
        Widget parent;
        int dirty = 1;          // 脏标记

    function:
        void addChild(Widget c) { ... }
        int hitTest(int cx, int cy) { ... }      // 返回命中子组件或自身
        void markDirty() { dirty = 1; ... }
    action:
        void draw(Renderer r) { ... }            // 递归：背景 → 边框 → 子组件
        void onInput(int ch, int cx, int cy) { ... }
    event:
        void clicked();                          // 子类可触发
}

// widgets.myp —— 控件
class Button : Widget {
    property:
        string text;
        Widget label;
    action:
        @constructor Button(string t) { text = t; label = new Label(t); addChild(label); }
        void draw(Renderer r) { ... }            // 画边框 + 居中文字
        void onInput(int ch, int cx, int cy) { if (hit) clicked(); }
    event:
        void clicked();
}

class Label : Widget { property: string text; ... }

// binding.myp —— 属性绑定（纯库，无新语法）
class Binding {
    static:
        // 源组件.属性 → 目标组件.属性/文本；变更时自动 markDirty
        void bind(Widget src, string srcProp, Widget dst, string dstProp);
        void bindText(Widget src, string srcProp, Widget dst);  // 写 dst.text
        // 变更通知入口：Application 每帧调用，检查已注册绑定的源值
        void pump();
}

// layout.myp —— 布局
class Box : Widget {  ... }   // vertical/horizontal 排子组件
class Flow : Widget { ... }   // 自动换行
class Stack : Widget { ... }  // 重叠

// render.myp —— 渲染抽象
class Renderer {
    action: void drawRect(...); void drawText(...); void clear(); void flush();
}
class TuiRenderer : Renderer { /* 封装 ui.myp Screen */ }

// app.myp —— 入口与事件循环
class Application {
    action:
        @constructor Application(Widget root) { root_ = root; }
        void run() {  // 事件循环：kbhit → 命中 → 事件 → 绑定 pump → 重绘
            while (running) { ... }
        }
    property:
        Widget root_;
        int running = 1;
}
```

**关键开放决策（D1）—— 绑定如何表达**，评审重点：
- **方案 A（采用，§0 硬约束下唯一可行）**：纯库命令式绑定 `Binding.bind(src,"value",dst,"text")` +
  `Application` 每帧 `Binding.pump()` 轮询已注册源值做变更检测。**零编译器改动**，
  简单可靠；代价是"绑定是命令式注册"而非 QML 的声明式 `text: src.value`。
- **方案 B**：用现有 `macro`/泛型做小 DSL，逼近 `text: { src.value }` 形态。更 QML，
  但引入宏复杂度，且仍属"库内 DSL"——可选演进。
- **方案 C**：编译器加 `@bind` 注解。**违反 §0 硬约束，排除**；除非日后放开约束再议。

> 决策（依 §0）：**采用 A**。B 可作后续可选演进；C 在硬约束未放开前不实施。

---

## 6. 目录结构

```
mypview/
├── design.md           # 本文档
├── src/
│   ├── core.myp        # Widget 基类（属性/children/命中/dirty）
│   ├── binding.myp     # 属性绑定引擎（方案 A）
│   ├── layout.myp      # Box / Flow / Stack
│   ├── widgets.myp     # Label / Button / TextBox
│   ├── render.myp      # Renderer 抽象 + TuiRenderer
│   └── app.myp         # Application（入口 + 事件循环）
├── examples/
│   ├── hello.myp       # 最小：标签 + 边框，声明式组件树
│   └── counter.myp     # 按钮点击 + 计数标签绑定更新（验收用例）
└── tests/
    └── test.myp        # 组件树 / 命中 / 绑定 断言
```

---

## 7. 里程碑

| 里程碑 | 内容 | 验收 |
|--------|------|------|
| **M1 骨架** | `core.myp` + `render.myp` + `TuiRenderer` + `hello.myp` | 能画出嵌套组件树（边框/文字/颜色） |
| **M2 交互** | 命中检测 + 输入分发 + `Button.clicked` | 键盘/鼠标点击命中并触发事件 |
| **M3 绑定** | `binding.myp`（方案 A）+ `counter.myp` | 按钮改属性 → 标签自动更新 → 重绘 |
| **M4 控件+布局** | `widgets.myp` 完善 + `layout.myp` | 组合示例（Box 排 Button/Label） |
| **M5（可选）** | `SdlRenderer` | SDL2 窗口渲染同一组件树 |

---

## 8. 验收标准（评审后执行依据）

1. `examples/counter.myp` 可运行：点按钮 → 计数标签更新、界面重绘正确。
2. 组件树/绑定/布局均为**纯 MYP**，不引入新语言特性（方案 A）。
3. 全量回归不破坏现有 stdlib/编译器（`tests/run_tests.sh` 仍 181/181）。
4. `tests/test.myp` 覆盖：组件树嵌套、`hitTest`、绑定泵检测、脏标记重绘。

---

## 9. 风险与开放问题

| # | 问题 | 影响 | 建议 |
|---|------|------|------|
| R1 | 方案 A 的 `pump()` 每帧轮询绑定开销 | 轻量可接受；组件多时变慢 | 先 A，后续可加"源 setter 主动通知" |
| R2 | TUI 输入用 `getch` 是逐字符，鼠标支持弱 | 交互手感一般 | 首版键盘导航足够；SDL 二期补齐鼠标 |
| R3 | 全量重绘在终端大尺寸下闪烁 | 观感 | 可先接受；后续帧缓冲 diff |
| R4 | `property:` 外部只读，绑定写目标属性受限 | 设计约束 | 绑定走公开 setter action（已有惯例） |

---

## 10. 依赖

- **必须**：`stdlib/env.myp`（Console/getch/kbhit）、`stdlib/ui.myp`（Screen）、
  `stdlib/collections.myp`（子组件表）
- **可选**：`stdlib/sdl.myp`（SDL 渲染，M5）
- **编译**：`./build/mypc -O2 mypview/examples/counter.myp -o /tmp/counter`
