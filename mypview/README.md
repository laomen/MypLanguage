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

## 控件速查表（52 控件）

> 构造参数统一 `(x, y, w, h)` 省略为 `(x,y,w,h)`。**声明式属性** = 控件 `setAttr`
> 支持的名字（`.uix` 里 `"prop":"值"` 或 `.usp` 里 `props`），值可为字符串 / `#RRGGBB` / 数字。
> **事件** 经应用层 `mapping` 绑定（如 `Button.Clicked -> App.onOk`）。

### 输入类（13）

| 控件 | 构造 | 声明式属性 | 事件 | 说明 |
|---|---|---|---|---|
| Button | `Button(label,x,y,w,h)` | text, color, bg | `Clicked(x,y)` | setEnabled / setHovered / focusable |
| LongPressButton | `LongPressButton(label,x,y,w,h)` | text, color, bg | `Clicked(x,y)` / `LongPressed(x,y)` | 区分 短按点击 / 长按（lastGesture 查询） |
| Checkbox | `Checkbox(label,x,y,w,h)` | text, checked, color | `Changed(v)` | setChecked |
| RadioButton | `RadioButton(label,x,y,w,h)` | text, checked, color | `Changed(v)` | 单选组 |
| Switch | `Switch(x,y,w,h)` | on, color | `Toggled(v)` | setOn |
| Slider | `Slider(x,y,w,h,min,max)` | value,min,max,color | `ValueChanged(v)` | 拖动设值（手势） |
| TextField | `TextField(text,x,y,w,h)` | text, color | `Focused` / `Edited(text)` | UTF-8 退格 / 密码 mask / focusable |
| TextArea | `TextArea(x,y,w,h)` | text, color | `Edited(text)` | 多行（\n 分行） |
| SearchBar | `SearchBar(x,y,w,h)` | text, color | `Edited(text)` | append/backspace/unfocus |
| Dropdown | `Dropdown(x,y,w,h)` | selected, color | `Selected(idx)` | addItem / 展开列表浮层 |
| Rating | `Rating(x,y,w,h)` | value, max, color | `Changed(v)` | 星级 |
| IconButton | `IconButton(icon,x,y,size)` | icon, color | `Clicked(x,y)` | 圆形图标 / hover |
| SegmentedControl | `SegmentedControl(x,y,w,h)` | selected, color | `Selected(idx)` | addSegment |
| Stepper | `Stepper(x,y,w,h)` | value,min,max,step,color | `Changed(v)` | +/− 步进 |

### 显示类（19）

| 控件 | 构造 | 声明式属性 | 事件 | 说明 |
|---|---|---|---|---|
| Label | `Label(text,x,y,scale)` | text, color | — | 5×7 位图（scale 放大） |
| TtfLabel | `TtfLabel(text,x,y,px)` | text, color | — | TTF 中文字体（px 字号） |
| ProgressBar | `ProgressBar(x,y,w,h,min,max)` | value,min,max,color | — | setProgress |
| ProgressSpinner | `ProgressSpinner(x,y,radius)` | — | — | 8 点转圈（tick） |
| AppIcon | `AppIcon(label,x,y,w,h)` | — | `Clicked(x,y)` | 图标+文字入口 |
| Toast | `Toast(w,h)` | text, color | — | show / tick 淡出 |
| NotificationBanner | `NotificationBanner(w,h)` | — | `BannerClicked(x,y)` | show / tick 倒计时 |
| List | `List(x,y,w,h)` | — | `ItemClicked(idx,x,y)` | addItem / itemCount / 可见窗口渲染 / 拖拽滚动 |
| SortableList | `SortableList(x,y,w,h)` | — | `Reordered(from,to)` | 长按拖拽换位排序 / getItem / lastFrom/dragLast |
| Panel | `Panel(x,y,w,h)` | color, bg | — | 容器卡片 |
| TabView | `TabView(x,y,w,h)` | current, color | `TabChanged(idx)` | addTab / 分页容器 |
| Dialog | `Dialog(title,msg,screenW,screenH)` | — | `Confirm()` / `Cancel()` | show/hide |
| ScrollView | `ScrollView(x,y,w,h)` | — | — | setContent / scrollTo / 拖拽滚动 |
| Image | `Image(x,y,w,h)` | color, mode | — | setHandle(SDL 纹理) |
| Divider | `Divider(x,y,w,h)` | color | — | 细分割线 |
| Badge | `Badge(text,x,y)` | text, color | — | setValue（数字角标） |
| Avatar | `Avatar(name,x,y,size)` | text, color | — | 圆形首字头像 |
| Chip | `Chip(label,x,y,w,h)` | text, checked, color | `Clicked` / `Deleted` | 胶囊标签（可删） |
| Tooltip | `Tooltip(w,h)` | text, color | — | show 气泡 |
| Banner | `Banner(w,h)` | text, color | — | 顶部横幅 show/tick |

### 容器 / 浮层（20）

| 控件 | 构造 | 声明式属性 | 事件 | 说明 |
|---|---|---|---|---|
| BottomNav | `BottomNav(x,y,w,h)` | current, color | `Selected(idx)` | addItem 底部导航 |
| Drawer | `Drawer(w,h)` | color | — | open/close/toggle/add 侧边抽屉 |
| RefreshIndicator | `RefreshIndicator(x,y,w,h)` | color | `Refresh()` | 下拉刷新 |
| ContextMenu | `ContextMenu(w)` | color | `Selected(idx)` | addItem / show 右键菜单 |
| ColorPicker | `ColorPicker(x,y,w,h)` | selected | `Selected(color)` | addColor 预置色板 |
| DatePicker | `DatePicker(x,y,w,h)` | year,month,day | `Selected(y,m,d)` | prev/nextMonth |
| DataGrid | `DataGrid(x,y,w,h)` | color | `CellTapped(r,c)` | addColumn/addRow/setCell |
| TreeView | `TreeView(x,y,w,h)` | color | `Toggled(idx)` / `Selected(idx)` | addNode 树形 |
| TimePicker | `TimePicker(x,y,w,h)` | hour,minute,color | `Changed(h,m)` | 时分 |
| ActionSheet | `ActionSheet(w)` | color | `Selected(idx)` | addAction / show 底部操作表 |
| Pagination | `Pagination(x,y,w,h)` | total,current,color | `Selected(page)` | 分页 |
| PageView | `PageView(x,y,w,h)` | current, color | `Selected(idx)` | addPage 翻页容器 |
| Popover | `Popover(w,h)` | text, color | — | show 气泡弹层 |

### 布局与框架

| 类别 | 名称 | 说明 |
|---|---|---|
| 布局 | LinearLayout(dir,w,h) | 0=垂直 1=水平；add/spacing/padding |
| 布局 | FlowLayout / GridLayout / StackLayout | 流式 / 网格 / 层叠 |
| 布局 | ConstraintLayout(w,h) | add(v, align 0-8, mx, my) 相对定位 |
| 框架 | FocusManager | 键盘焦点导航 register/next/prev/activate |
| 框架 | GestureDetector | Tap/LongPress/Drag 识别 press/tick/move/release |
| 框架 | Theme | dark/light 配色（bg/surface/text/accent） |
| 引擎 | UixLoader | `.uix` 构建 + `.usp` 样式 + 绑定/命令 + applyPseudo |

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
