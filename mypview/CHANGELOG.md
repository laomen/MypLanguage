# mypview 变更日志 (CHANGELOG)

> mypview 是 MYP 语言的通用 UI 框架（零 MOS 内部依赖，声明式 UIX + MVVM + AppRunner）。
> 本文件记录 mypview 框架自身的变更（控件/布局/UIX 引擎/AppRunner/示例/测试）。
> 编译器/运行时/stdlib bridge 的变更仍记录在主仓库 `docs/CHANGELOG.md`（mypview
> 专用 stdlib 扩展如 json bridge 编辑 API 在本文件与主 changelog 双向引用）。
>
> 版本号沿用主仓库编译器版本（mypc --version），标注 mypview 侧里程碑。

## v3.12.69 — 修复 AppRunner 按钮点击双触发（click+press 双路由）

**问题**：一次鼠标按下（SDL MOUSEBUTTONDOWN）在 bridge 里同时置
`getMouseClick`（g_mouse_x/y）与 `getMouseDown`（g_mouse_down）。AppRunner 帧循环
两路都派发：
1. `getMouseClick` → `rv.onTouch` → 控件 `Clicked`（#1）
2. 手势按下边沿 → `rv.onPress` → `RootView.onPress` 先调 `child.onTouch` →
   控件 `Clicked`（#2）

→ 每次真实按钮点击 `Clicked` **双触发**（幂等控件被掩盖；v3.12.68 运行按钮一按
开 3 个窗口即此根因 + release 合成 click）。

**修复（`src/backend/app_runner.myp`）**：
- 有手势路径（`getMousePos >= 0`）时，按下边沿的 `onPress→onTouch` 已派发点击，
  不再走 `getMouseClick` 分发；
- 仅无手势路径的「静止点击」（从未移动、`getMousePos=-1`）走 click 一次性分发；
- `getMouseClick` 每帧照常调用以消费（清残留），避免陈旧点击后续误触发。

**`examples/uix_designer.myp`**：移除运行按钮的 spawn 防抖（根因已修，防抖的
「0.5s 内快速重开被忽略」副作用随之消除）。

**验证**：
- 真实窗口合成鼠标点「运行」：`ds run spawn=0` **恰好一次**（修复前 3 次）✓
- 拖拽/静止点击两条路径都正常：设计器拖拽（testMouse 手势）与按钮点击都只
  派发一次 ✓
- 套件 6 段全 PASS（UIX/BNCT/JSON/DESIGN/UIXRUN/PIPE）。

## v3.12.68 — 设计器「运行」按钮：保存设计 → 弹 uix_run 独立新窗口跑当前 uix

**需求**：在编辑器加「运行」按钮，点一下弹出新的对话框（独立窗口）直接运行
已经修改的程序。

**`examples/uix_designer.myp`**：
- 工具栏「保存 .uix」右侧新增「运行」按钮（`runBtn_`）。
- 点击运行 → `enterRunMode()`：
  1. `saveUix()` 把当前（已修改）设计持久化到 `MYPVIEW_UIX_FILE`/`design.uix`；
  2. `Process.spawn("MYPVIEW_UIX_FILE=<file> MYP_PLAYER_MAXFRAME=0 ./uix_run")`
     **后台派生子进程**弹 uix_run 独立预览窗口跑当前 uix；
  3. 设计器保持响应（双击 fork 分离子进程，不阻塞主循环）；
  4. 预览窗口跑到自己被关闭（显式 `MYP_PLAYER_MAXFRAME=0`，不受设计器环境
     限帧变量影响）。
- **spawn 防抖**：AppRunner 对一次按下 click+press 双路由（既有行为）+ release
  合成 click → 一按可能触发 3 次 enterRunMode；30 帧（≈0.5s）内只 spawn 一次
  （`curFrame_`/`lastSpawnFrame_`）。
- headless（`DESIGN_HEADLESS=1`）不实际启动，只打印 `ds run cmd=... skip=1`。

**`stdlib/process.myp` + `stdlib/bridges/process_bridge.c`**：
- 新增 `Process.spawn(cmd)`（`myp_process_spawn`）：双击 fork + `setsid()` 分离
  子进程、不等待；子进程被 init 收养（无僵尸），父进程（设计器）立即返回。
- stdlib FFI 声明，无需改编译器（bridge 按符号匹配链接）。

**`src/core/root.myp`**：`RootView` 声明 `interface class View` + `onTouch/onPress`
返回 `int`→`void`（对齐 View 接口）。上版为同窗口运行模式引入，本版保留（RootView
作 View 的契约补齐，无害）。

**`src/uix/uix_loader.myp`**：`fieldCount()`（上版引入，保留）。

**build.sh**：`uix_designer` 构建时若缺 `uix_run` 连带构建（运行按钮依赖）。

**测试**：
- headless：`ds run cmd=MYPVIEW_UIX_FILE=... MYP_PLAYER_MAXFRAME=0 ./uix_run
  skip=1` ✓
- 真实窗口（合成鼠标点「运行」）：`ds run spawn=0`（只开 1 个窗口）+
  `debounce skip` ×2 + 设计器 `designer-alive` ✓；子进程 uix_run 渲染设计、
  父进程退出后仍存活 ✓
- 套件 6 段全 PASS。

## v3.12.67 — UixRun：用 UixLoader 直接运行 .uix 设计（设计即开即跑）

**需求**：能不能根据设计器保存的 `design.uix` 生成程序，直接用 UixLoader 跑起来？

**`examples/uix_run.myp`**（通用 .uix 运行器，AppRunner 应用）：
- 读 `.uix` 文件（`MYPVIEW_UIX_FILE` 环境变量指定，缺省 `design.uix`）→
  `UixLoader` 解析 → `buildInto(RootView)` → 渲染成真实窗口，即「设计即程序」。
- 窗口尺寸取自设计根节点 Json 的 width/height（默认 900x600），背景 `0xD8D8D8`。
- F11 全屏、`MYP_PLAYER_MAXFRAME` 退出帧、frame==60 存 `uix_run.bmp` 截图
  （与 AppRunner 一致）。
- headless 验证模式（`MYPVIEW_RUN_HEADLESS=1`）：构建 + `nodeCount()` +
  `hitId()` 断言，供测试。

**build.sh / run.sh**：
- `build.sh uix_run` 后端 target；both 模式 headless 跑 `examples/design.uix`。
- `tests/run.sh` 新增 MYPVIEW-UIXRUN 段（用 `tests/uix_sample.uix` 夹具断言
  `nodes=4` / `hit=ok`）。

**验证**：
- `tests/uix_sample.uix`：`uixrun headless nodes=4` / `uixrun hit=ok` ✓
- 设计器保存的 `examples/design.uix`（BNCT 欢迎页：Label 标题 + TextField 用户名 +
  Slider + Switch）：headless `nodes=5`；窗口模式渲染正常（截图见 v3.12.67 提交）✓
- 套件 6 段全 PASS（UIX/BNCT/JSON/DESIGN/UIXRUN/PIPE）。

## v3.12.66 — 设计器调色板拖放（从外部拖控件进画布，Qt Design Studio 式）

**需求**：从外部（调色板）把控件拖进画布——按调色板 `+xxx` 按钮 → 拖到画布 →
松开在该位置添加控件，而非此前「点击固定加到 (40,40)」。

**AppRunner 窗口级手势回调**（`src/backend/app_runner.myp`）：
- `UiApp` 接口新增 `onPress(x,y)`/`onMove(x,y)`/`onRelease(x,y)`（默认空体，既有
  应用不受影响）。
- 帧循环手势分发在树内路由（`rv.onPress/onMove/onRelease`，命中控件拖拽）之外，
  **同时调 `app.onPress/onMove/onRelease`**——窗口级回调，供应用做跨控件拖放
  （调色板→画布）。两者并存：树路由给命中控件做控件内拖拽，窗口级给应用拖放。

**设计器调色板拖放**（`examples/uix_designer.myp`）：
- `DragGhost`（琥珀描边 + 类型名的预览框）加入画布渲染树，跟随光标显示拖拽目标。
- `UiApp.onPress`：按在 `+xxx` 调色板按钮上 → 记录 `paletteDrag_` 类型 + 显示幽灵
  （尺寸按类型默认）。
- `onMove`：幽灵跟随光标。
- `onRelease`：落到画布内 → `addControl(type, 画布局部坐标)` 在该位置添加；未落到
  画布（简单点击）→ 默认 (40,40)。
- 调色板按钮移出 `onButton` 路由（改由窗口级手势处理），避免点击重复添加。
- `addControl` 改为接受 (type, x, y)。

**验证**：headless 新增调色板拖放断言——按 `+Button`（窗口坐标）→ 拖到画布
(150,250) → 松开 → 画布局部 (140,192) 添加（`ds palette x140=1 y192=1 nodes=7`）；
mypview UIX/BNCT/JSON/DESIGN/PIPE 全 PASS（未改编译器，不跑父全量）。

## v3.12.65 — 拖拽手势全链路验证钩子（合成鼠标注入）

- bridge 加 `myp_sdl_test_mouse(down,x,y)`：注入合成鼠标事件（按下状态+位置+点击），
  无真实鼠标环境驱动 AppRunner 完整手势分发；stdlib `SDL.testMouse` 包装。
- 设计器 headless 拖拽测试改走完整手势路由（root.onPress→mainRow→canvas→事件→
  handler）；`MYPVIEW_TEST_DRAG=1` 注入合成鼠标验证真实窗口拖拽
  （`ds testdrag x100=1 y140=1`）。
- 确认拖拽（含容器手势链、AppRunner 分发）全链路可用。

## v3.12.64 — 设计器全屏 + 窗口缩放后画布/控件自适应（resize 修复）

**症状**：窗口全屏（或拖拽缩放）后，画布等控件不随窗口变大——只有宽度变，高度
被压住。

**根因**：嵌套容器的宽/高是**构造时定死的**（LinearLayout 的 `w_/h_` 由构造参数
决定）。设计器 `relayout()` 只重设了 `root_` 和 `canvas_`，没重设
`toolbar_/mainRow_/propPanel_` → `mainRow_` 高度停在构造值（如 832），其
`layout()` 把 `canvas_` 高度也压回 832。宽度之所以能变，是横向 layout 用子控件
自己的 `width()`（relayout 已更新）。

**修复**（`examples/uix_designer.myp`）：`relayout()` 显式 `setFrame` 重设全部
容器（root_/toolbar_/mainRow_/propPanel_/canvas_）到按新 W/H 计算的尺寸，再逐个
`layout()`。

**全屏入口**：
- `AppRunner` 帧循环支持 **F11 全屏切换**（`SDL.setFullscreen`），所有窗口应用
  通用；bridge `myp_sdl_get_key` 补返回 `SDL_SCANCODE_F11`。
- 设计器工具栏加 **「全屏」按钮**。
- 全屏/缩放 → `SDL_WINDOWEVENT_SIZE_CHANGED` → `g_width/height` 更新 →
  AppRunner 检测到尺寸变化 → `app.onResize` → `relayout` 重排。

**resize 链路自测**：设计器 `onFrame` 支持 `MYPVIEW_TEST_RESIZE=1`——frame 5 自
缩放窗口（+200,+150），frame 12 打印画布尺寸断言变大（如 1052x832 →
1252x982）。headless 可回归 resize 链路。

**验证**：mypview UIX/BNCT/JSON/DESIGN/PIPE 全 PASS（未改编译器，不跑父全量）；
resize 自测画布 1052x832 → 1252x982。

## v3.12.63 — 修复设计器工具栏按钮不显示（嵌套 LinearLayout 未布局）

**症状**：设计器窗口里工具栏 11 个按钮全部看不到，只看到属性面板的「应用属性」
一个蓝按钮。

**根因**：`LinearLayout.layout()` **不递归子容器**——只对直接子控件 `setFrame`。
设计器 `relayout()` 只调 `root_.layout()`，漏了 `toolbar_/mainRow_/propPanel_` 的
`layout()` → 工具栏按钮、属性面板字段全部停留在构造位置 (0,0) 重叠；最上层恰好是
后加入的「应用属性」（propPanel_ 最后 add）→ 窗口里只露出它一个蓝按钮。
bnct_cases 对每个嵌套容器都显式 `layout()`，设计器漏了。

**修复**（`examples/uix_designer.myp`）：`relayout()` 依次调
`root_.layout(); toolbar_.layout(); mainRow_.layout(); propPanel_.layout();`。

**连带**：布局修正后画布原点从 (0,0) 变为 (10,58)（工具栏 + padding），headless
命中/拖拽坐标相应更新（ok 按钮窗口位置 (70,178)，按压 (80,190) 拖到 (120,210) →
设计局部 (100,140)，断言不变）。

**验证**：窗口截图工具栏 11 个按钮正确铺开；mypview UIX/BNCT/JSON/DESIGN/PIPE
全 PASS（未改编译器，不跑父全量）。

## v3.12.62 — 设计器亮色主题 + 画布偏移 + 文件操作 + save_bmp 颜色修复

**背景**：设计器此前整体深色（"界面都是黑的"）；且设计控件按设计局部坐标 (0,0)
渲染在窗口绝对位置（压到工具栏、挤在画布左上角，观感"设计不在画布里"）；缺文件
打开/保存；`SDL.saveBmp` 截图 R/B 交换误导调试。

**UixDesigner 亮色主题**（`examples/uix_designer.myp`）：
- 画布 `DesignCanvas` 背景改亮灰 `Color.rgb(200,200,200)`（设计表面）；窗口背景
  `0xD8D8D8`。
- chrome 文字（属性面板标签/状态）改深色 `Color.dark()`（亮底上可见）。
- 示例设计标题 Label 显式 `"color":"#1F1F1F"`（亮画布上可见）；调色板新增 Label
  同样带深色。

**画布基准偏移（设计正确落在画布内）**：
- `UixLoader` 新增 `offX_/offY_` + `setOffset/getOffsetX/getOffsetY`；`moveNode`
  按 `offX_+x` 定位（设计局部坐标 → 窗口坐标）；`moveNode` 扩到 Panel/Column/
  Flow/Stack 容器。
- 设计器 `applyOffsetAll()`：buildInto 后按画布原点 `setOffset` 并把全部控件
  `moveNode` 平移；命中/拖拽/选中框全部改用窗口坐标。`relayout` 时画布位置变化
  重新平移。修复"设计栏不显示/设计挤在左上角"。

**文件操作**（`import io/error`，File stdlib）：
- 工具栏加 **新建 / 打开文件 / 保存 .uix**：`newDesign()` 空设计、`openUix()`
  （读 `.uix` 文件 → 重新构建，路径 `MYPVIEW_UIX_FILE` 环境变量或默认 `design.uix`）、
  `saveUix()`（写 `.uix` 文件）。headless 断言文件保存→打开往返。

**save_bmp 颜色修复**（`stdlib/bridges/sdl_bridge.c`）：
- `myp_sdl_save_bmp` 的 `SDL_CreateRGBSurface` 掩码写成 RGBA 序，而
  `RenderReadPixels` 填 ARGB8888（小端字节 = B,G,R,A）→ SDL 把 B/R 解释反 →
  保存的 BMP 蓝变橙（仅截图，实际窗口一直正确）。改掩码 `R=0x00ff0000/G=0x0000ff00/
  B=0x000000ff/A=0xff000000`。

**AppRunner 补 `MYP_PLAYER_MAXFRAME` 退出**（`src/backend/app_runner.myp`）：
- 此前 AppRunner 的 `while(SDL.running())` 只认 ESC/QUIT，窗口应用跑自动化/冒烟
  一直挂着（`MYP_PLAYER_MAXFRAME` 只在 player.myp 处理）。补 `frame >= maxFrame`
  break（对齐 player），窗口应用现在可限帧自动退出。

**回归**：mypview UIX/BNCT/JSON/DESIGN/PIPE 全 PASS（仅改 mypview 框架与
sdl_bridge，未改编译器，不跑父套件全量）。

## v3.12.61 — 框架手势路由 + 设计器拖拽移动控件

**背景**：RootView 原支持 onPress/onMove/onRelease 手势锁定（直接子控件），但
嵌套容器（LinearLayout/Panel/Flow/Grid/Stack）只转发 onTouch，不转发手势；且
AppRunner 只分发鼠标点击。设计器要「按住拖动即移动控件」（Qt Design Studio 式），
需打通整条手势链路。

**AppRunner**（`src/backend/app_runner.myp`）：帧循环加鼠标左键**边沿检测**（
`SDL.getMouseDown` 上一帧状态 prevDown）→ 按下 `rv.onPress` / 按住移动 `rv.onMove`
/ 抬起 `rv.onRelease`，分发给根树（点击 onTouch 路径保留，两者并存）。

**容器手势路由**：LinearLayout/FlowLayout/GridLayout/StackLayout/Panel 新增
`activeKid_`（按下锁定的子控件下标）+ `onPress`（命中锁定并下发）/ `onMove` /
`onRelease`（持续分发给锁定子）——与 onTouch/updateHover 同款「从后往前命中」模式。
**拖拽从此对任意嵌套控件可用**（ScrollView/Slider 等可据此实现拖动）。

**UixLoader**：补 `moveNode(id, x, y)`——设计器拖拽时对加载的控件实时 `setFrame`
（不整树重建，平滑）；保持宽高。

**UixDesigner**（`examples/uix_designer.myp`）：DesignCanvas 上报
`CanvasPress/CanvasMove/CanvasRelease`；按下命中选中 + 记录抓取偏移，移动实时
`loader_.moveNode` + 写回文档 x/y + 选中框跟随，抬起重建落定。headless 断言
拖拽移动（ok 从 (60,120) → (100,140)）。

**踩坑**：AppRunner 帧循环里 hover 段已声明 `int pos`，手势段再声明同名变量 →
"duplicate variable 'pos'"（错误定位错乱到 focus_manager）。改手势段用 `gpos`。
FlowLayout 构造有 `rowGap_` 字段，加 `activeKid_` 时替换串须含它（否则属性声明
缺失，方法引用未定义符号，错误错乱到 focus_manager:132）。

**回归**：mypview UIX/BNCT/JSON/DESIGN/PIPE 全 PASS；mypc 与 myp_self 输出完全
一致；父套件 313/313、bootstrap 16/16、bugs 11/11。

## v3.12.60 — UixDesigner：所见即所得界面设计器（对标 Qt Design Studio）

**背景**：把 mypview 做成可用「界面设计工具」直接设计 UI——画布实时渲染、点选控件、
属性面板改属性即见效果、调色板一键添加、保存 `.uix` 声明文档（可再被 UixLoader 加载）。

**架构**（`examples/uix_designer.myp`，UiApp + AppRunner）：
- `.uix` 文档 = 设计的**唯一真源**（Json，用 v3.12.59 的编辑 API setValue/addChild/remove）。
- 画布 = `UixLoader.buildInto` 实时渲染（所见即所得）。`DesignCanvas`（自定义 View）
  持有设计渲染树、点击只上报 `CanvasClick`（不派发给控件 → 点选而非触发）。
- 选中 = 画布点击 → `loader.hitId(x,y)`（已存在的命中最上层 API）→ 琥珀 `SelBox`
  描边 + 属性面板回填（text/color/x/y/width/height）。
- 属性 = 编辑 → `Json.setValue`/`addChild` 写回文档 → 重建画布即见效果。
- 调色板 = Label/Button/TextField/Switch/Slider/Panel 一键添加（`Json.addChild` 注入
  `{...}` 对象节点）。
- 删除/保存：`Json.remove` 删节点；`serialize()` 输出 `.uix`（打印 + 截图）。
- headless（`DESIGN_HEADLESS=1`）断言：命中/改属性/加控件/删除/序列化。

**桥扩展**：`myp_json_add_child` 支持**完整 JSON 值**（raw 以 `{`/`[` 开头走完整解析
器 → 可注入对象/数组节点；否则回退标量——JsonEditor 行为不变）。

**UixLoader 补充**：补 `panelIdxOf`/`panelAt` 访问器（与其他控件类型一致，设计器
取节点实际宽高用）。

**踩坑**：
- **`Json.remove` 对带尾点路径失败**：`myp_json_remove` 用 `strrchr('.')` 取末段，
  路径 `children.5.`（尾点）→ 末段空 token → 删除失败。设计器 id→路径映射存对象
  路径时**必须去尾点**（`children.5`）。setValue 因 strrchr 取最后一个点恰好能扛。
- 属性面板行标签**纯局部**（`Label l1 = new Label(...)`）→ 出 build() 释放 → 容器
  悬垂 → 窗口模式绘制时 `myp_ttf_draw_text` strlen 崩（headless 不绘制不触发）。
  同 json_editor：控件一律字段持有。

**回归**：mypview UIX/BNCT/JSON/DESIGN/PIPE 全 PASS；mypc 与 myp_self 输出完全一致；
父套件 313/313、bootstrap 16/16、bugs 11/11。

## v3.12.59 — JsonEditor：可视化 JSON 树编辑器 + json bridge 编辑支持

**背景**：stdlib `json` 原本只读（parse + 按路径查询）。要给 mypview 做可视化
JSON 编辑器，需补遍历/修改/序列化能力。

**扩展 `stdlib/bridges/json_bridge.c` + `stdlib/json.myp`**（Json 类新方法）：
- 遍历：`childCount(path)`、`childKey(path, i)`（对象键/数组空）、`scalar(path)`
  （标量显示文本：字符串去引号、数字词法原文、bool/null 关键字）。
- 修改：`setValue(path, raw)`（标量原地改，raw 解析为数字/布尔/null/字符串）、
  `addChild(path, key, raw)`（对象加键/数组追加）、`remove(path)`（删子节点）。
- 序列化：`serialize()`（美化打印，2 空格缩进，字符串转义 `"` `\` `\n` 等）。
- 全部保持 M8 约定：返回 string 一律 `myp_strdup`（计数拷贝）。

**新增控件 `mypview/src/controls/json_editor.myp`**：
- `JsonEditor` 把 JSON 解析为可展开/折叠树行（键蓝色 + 类型着色值：string 绿/
  number 琥珀/bool 青/null 灰/容器白），纯逻辑 headless 可测。
- 编辑 API：`toggle(i)`（展开/折叠，跨编辑持久）、`setValueAt(i, raw)`、
  `addChildAt(i, key, raw)`、`removeAt(i)`、`serialize()`、行查询
  `rowCount/rowKeyAt/rowValueAt/rowLevelAt/rowTypeAt/rowHasKids`。
- 自绘 + 命中（同 tree_view 范式）：点箭头展开折叠、点行选中触发 `Selected`。

**示例 `examples/json_editor.myp`**（UiApp + AppRunner）：标题行 + JsonEditor 树
+ 操作栏（值 TextField + 设值/加键/删行/序列化 4 按钮）。`JSON_HEADLESS=1` headless
断言：树行/改值/加键/删除/折叠往返/序列化。build.sh 的 backend target 加 json_editor；
run.sh 新增 MYPVIEW-JSON 测试段（SRCS/PIPESRCS 亦补 json_editor.myp 依赖顺序项）。

**踩坑**：
- LinearLayout 的 kids_ 是接口数组不 retain——示例里控件必须**字段持有**（纯局部
  控件出作用域即释放 → layout 时悬垂段错误）。bnct 同款规避。
- **myp_self 对固定大数组字段缺陷**：`string[512]` 字段按 `[512 x ptr]` 布局，
  `new string[512]` 返回 `ptr` → `store [512 x ptr]` verify 失败。改用**动态数组**
  `string[]`（`new T[n]`）两编译器皆稳。

**回归**：mypview UIX/BNCT/JSON/PIPE 全 PASS；mypc 与 myp_self 输出完全一致；父套件
313/313、bootstrap 16/16、bugs 11/11。

## v3.12.58 — AppRunner：应用运行器框架化（帧循环不再手写）

**背景**：此前每个窗口示例都要在 Boot 里手写 `while (SDL.running())` 帧循环
（窗口事件/鼠标命中/hover 遍历/绘制/协程调度/退出条件）。把这段样板收敛进框架，
应用只需实现 UiApp 生命周期接口。

**新增 `mypview/src/backend/app_runner.myp`**：
- `interface UiApp`：应用生命周期接口——`onCreate(rv, r, w, h)`（构建界面树）、
  `onResize(w, h)`（窗口尺寸变化，框架已 setLogicalSize 1:1）、
  `onFrame(frame)`（每帧业务更新）、`onKeyChar(c)`（输入字符）。
- `class AppRunner`：`int run(UiApp app, string title, w, h, bgColor)`——创建
  SDL 窗口 + SdlRenderer + RootView，驱动帧循环直到退出（ESC/关窗/
  `MYP_PLAYER_MAXFRAME`）。框架内完成：鼠标点击→`rv.onTouch` 命中分发、
  输入字符→`app.onKeyChar`、鼠标位置→`rv.updateHover` 悬停遍历、窗口尺寸变化
  →`setLogicalSize` + `app.onResize`、`app.onFrame` + `Coro.scheduler()`、
  清屏→`rv.draw`→present、16ms 帧间隔。
- **放置 backend/**：AppRunner 强依赖 SDL 窗口 + SdlRenderer（具体后端类），
  放 core 会迫使所有示例（含 headless/纯逻辑）链接 SDL；放 backend 后仅运行
  SDL 窗口的 target（build.sh 显式加 backend）才编译它。
- **hover 框架化**：View 接口新增 `setHovered(on)`/`updateHover(x, y)`（默认
  按自身 hit 设置）；RootView 与 LinearLayout/FlowLayout/GridLayout/StackLayout/
  Panel 覆写 `updateHover` 为从后往前递归子控件。AppRunner 每帧自动遍历，控件
  只需覆写 `setHovered` 做悬停视觉（如 Card/Button 提亮）。

**示例迁移**：`examples/bnct_cases.myp` 由手写帧循环改为实现 `UiApp`（Boot 仅
三行：`AppRunner rn = new AppRunner(); rn.run(a, "标题", 1880, 956, 0x14141C);`），
headless 断言不变。build.sh 的 backend 包含改为目录通配（sdl_renderer + app_runner）。

**回归**：mypview UIX/BNCT/PIPE 全 PASS；mypc 与 myp_self 输出完全一致；父套件
313/313、bootstrap 16/16、bugs 11/11。
