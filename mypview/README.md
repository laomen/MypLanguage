# mypview — 通用 MYP UI 框架（零 MOS 依赖，声明式 UIX + MVVM）

> mypview 是 MYP 语言的通用 UI 框架：**零 MOS 内部依赖**（只依赖 MYP 标准库），
> 任何 MYP 项目把 `src/` 源码集合加入编译列表即可使用。提供**声明式 UIX**（JSON
> 组件树 + 绑定 + 命令）、**MVVM**（PropBag + ViewModel）、**可插拔渲染后端**
> （Renderer 抽象，headless / SDL2）。

## 快速开始

```bash
# 独立示例：UIX 声明式计数器（headless，无 SDL）
bash examples/build.sh          # 产物 examples/counter，并运行
examples/counter
# 输出：
#   cmd=inc text=count: 1
#   cmd=inc text=count: 2
#   cmd=dec text=count: 1
#   final text=count: 1
```

`examples/build.sh` 展示用法：把 `src/` 下 `core + controls + layout + uix`
文件加入 mypc 编译列表 + 你的 main，即可获得声明式 UI 应用。**不需要 MOS 的
CMake 或任何系统服务。**

## 框架构成（`src/`）

| 子目录 | 内容 | 依赖 |
|---|---|---|
| `core/` | View 接口 / Renderer 抽象 / RootView / Theme | stdlib |
| `controls/` | Label / Button / TextField / Panel / List / Switch / Checkbox / Slider / ProgressBar / Dialog / ScrollView / NotificationBanner / AppIcon / TtfLabel | stdlib |
| `layout/` | LinearLayout / GridLayout（盒模型流式布局） | stdlib |
| `animation/` | Tween / CoroAnim | stdlib + coro |
| `uix/` | **UIX 声明式 UI 引擎**（UixLoader + PropBag + ViewModel，headless） | stdlib + json |
| `backend/` | SDL 渲染后端（可插拔） | stdlib + sdl |

**依赖边界**：`src/` 全部文件只 `import` MYP 标准库（env/text/json/fmt/coro/sdl/ttf），
**零 MOS 内部依赖**。UIX 引擎（uix/）纯 headless——不依赖 SDL，可在任何环境测逻辑。

## UIX 声明式 UI

`.uix` 描述 = JSON 组件树（属性 + 绑定 + 命令）；`.usp` 样式表 = JSON 选择器
（类型 / #id / .class → 属性集）。引擎 `UixLoader` 构建树 / 应用样式 / 管理绑定
与命令。见 `tests/uix_logic.myp`（headless 回归）。

```myp
// 声明式界面（内嵌 .uix）
UixLoader loader = new UixLoader(uix, "");   // .uix 描述 + .usp 样式（可空）
RootView rv = new RootView(16, 400, 300);
loader.buildInto(rv);                          // .uix → View 树
CounterVM vm = new CounterVM();
loader.sync(vm);                               // 绑定：VM 状态 → 控件
// 交互：Button.Clicked → mapping → cmdFor → vm.runCmd → loader.sync(vm)
```

### 样式伪状态（:hover / :checked / :focus / :disabled）

`.usp` 选择器支持伪状态后缀，运行时按控件状态切换样式：

```
[{"sel":"Button.primary",      "props":[{"k":"bg","v":"#007AFF"}]},
 {"sel":"Button.primary:hover","props":[{"k":"bg","v":"#0A84FF"}]},
 {"sel":"Checkbox.opt:checked","props":[{"k":"color","v":"#34C759"}]}]
```

- 构建时只应用**无伪状态**选择器（基线样式）；伪状态由运行时 API 驱动：
  - `loader.applyPseudo("hover")` — 应用匹配该伪状态的选择器（覆盖属性）
  - `loader.clearPseudo("hover")` — 重新应用全部无伪状态样式（恢复基线）
- 应用层在状态变化时调用（鼠标 hover 到控件 / 勾选 / 聚焦 / 禁用）。
  可复用：`applyPseudo("checked")` / `applyPseudo("focus")` / `applyPseudo("disabled")`。
- 伪状态只是"额外属性覆盖"，控件自身仍需支持对应 setAttr 属性（如 Button.bg）。

### 交互状态（hover / focus 视觉反馈）

- **hover**：`Button` / `IconButton` 带 `setHovered(int)` + 悬停提亮（`Color.lighten`）。
  桌面端用 `SDL.getMousePos()`（常驻鼠标位置，不消费）逐帧查询 + `hit()` 判定：
  ```myp
  int mpos = SDL.getMousePos();            // (y<<16)|x，从未移动 -1
  if (mpos >= 0) {
      int mx = mpos & 0xFFFF, my = (mpos >> 16) & 0xFFFF;
      play.setHovered(play.hit(mx, my));   // hit 返回 int 直接传（勿用 !=0）
  }
  ```
- **focus**：`FocusManager` 键盘导航（Tab/回车）时 `setFocus(1)` → 控件绘制高亮
  外框（Button=amber，TextField=green）。控件实现 `focusable()` 即可入导航。

### 手势系统（按下→移动→抬起 连续事件流）

把"一次性点击"升级为连续事件流，驱动**拖拽/长按**：

- **View 接口**新增默认方法：`onPress(x,y)` / `onMove(x,y)` / `onRelease(x,y)`。
  未覆写时点击行为不变（RootView 在 onPress 前先调 onTouch 兼容）。
- **RootView 分发**：`onPress` 命中即锁定（activeIdx），`onMove`/`onRelease`
  持续发给锁定控件 → 拖拽过程中不因鼠标移出控件而丢失。
- **已接入拖拽的控件**：`ScrollView`（拖动滚动，`dragTo` 增量）、`Slider`（拖动设值）。
- **GestureDetector**（`core/gesture.myp`）识别 **Tap / LongPress / Drag**：
  ```myp
  GestureDetector g = new GestureDetector();
  g.press(x, y);                    // 按下
  g.tick();                         // 每帧（长按计时）
  g.move(x, y);                     // 按住移动（超位移阈值→Drag）
  int type = g.release(x, y);       // 1=Tap 2=LongPress 3=Drag
  if (g.tap() != 0) { ... }         // 或查询 g.dragging()/g.longPressed()
  ```
- **桌面驱动**：`SDL.getMouseDown()`（持续按下状态）+ `SDL.getMousePos()` 组合出
  按下边沿 → `rv.onPress`、按住移动 → `rv.onMove`、抬起 → `rv.onRelease`
  （player 帧循环已实现）。

## MVVM 分层

- **View** = `.uix` 声明式描述（组件树 + 绑定 + 命令），无逻辑
- **ViewModel** = MYP 类，组合 `PropBag` 存状态（实现 `ViewModel` 接口的
  `getProp`/`runCmd`），供 `UixLoader.sync()` 读取
- **Model** = 数据类
- **引擎** = `UixLoader`（构建树 + 应用样式 + 管理绑定/命令）

## 渲染后端（可插拔）

`core/renderer.myp` 的 `Renderer` 是抽象接口（fillRect/drawText/drawImage/...）。
`backend/sdl_renderer.myp` 是 SDL2 实现。UIX 与控件逻辑不依赖具体后端——headless
测试用 MockRenderer，桌面用 SdlRenderer，未来可加软件帧缓冲 / GPU 后端。

### TTF 文本纹理缓存

`sdl_ttf_bridge.c` 内建 FIFO 纹理缓存：key = 实际字号 + RGB + 文本。同一文本在
帧间/滚动重绘时直接 `SDL_RenderCopy` 复用，避免每帧 `TTF_RenderUTF8_Blended` +
建纹理解析（实测 player 60 帧命中率 ~98.5%，hits=3072/misses=48）。
字体加载/字号变更自动清空缓存（旧纹理失效）。诊断：

```myp
Ttf.cacheHits()      // 命中次数（复用）
Ttf.cacheMisses()    // 未命中次数（新渲染）
```

## 在自己的项目里用 mypview

1. 拷贝 `src/` 目录到你的项目（或直接引用）
2. 把需要的文件加入 mypc 编译列表（见 `examples/build.sh` 的 `SRCS`）
3. `--stdlib` 指向 MYP 标准库（mypc 通用桥接会自动链 SDL/ttf）
4. 写你的 `.uix` + ViewModel + main

## 测试

```bash
# headless 逻辑回归（UIX 构建/样式/绑定/命令/命中）
cd tests && bash ../.. /build/mypc 2>/dev/null   # 或用 mypc --test tests/uix_logic.myp
mypc --test tests/uix_logic.myp -o /tmp/uix_logic --stdlib ../stdlib && /tmp/uix_logic
```

## 路线图（通用框架化）

- [x] 零 MOS 依赖、headless 引擎、Renderer 后端抽象
- [x] UIX 声明式 DSL + MVVM
- [x] 独立示例 + headless 回归
- [x] UIX 控件注册扩展点（`ViewBuilder` + `registerControl`，见 `tests/uix_logic.myp` 的 Badge 示例）
- [ ] 软件渲染后端（无 SDL 环境可跑）
- [ ] 包化分发（MYP package-path 目录结构）
