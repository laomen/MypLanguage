# uikit — 通用 MYP UI 框架（独立示例）

> **uikit 是一个零 MOS 依赖的通用 MYP UI 框架**。它不依赖 MOS 的操作系统服务、
> 桌面壳或构建系统——只要你有 `mypc` 编译器和 MYP 标准库，把 uikit 源码集合加入
> 编译列表即可使用。本目录是它在 MOS 之外独立运行的证明。

## 快速开始

```bash
# 用 mypc 直接编译 uikit 源码集合（仅依赖 stdlib）
bash build.sh        # 产物 ./counter，并运行
./counter
# 输出：
#   cmd=inc text=count: 1
#   cmd=inc text=count: 2
#   cmd=dec text=count: 1
#   final text=count: 1
```

`build.sh` 展示了「通用框架」的用法：把 `MOS/uikit` 下的
`core + controls + layout + uix` 文件加入 mypc 编译列表，加上你的 `main`，即可
获得一个声明式 UI 应用。**不需要 MOS 的 CMake 或任何系统服务**。

## 框架构成（`MOS/uikit/`）

| 子目录 | 内容 | 依赖 |
|---|---|---|
| `core/` | View 接口 / Renderer 抽象 / RootView / Theme | stdlib |
| `controls/` | Label / Button / TextField / Panel / List / Switch / Slider / Dialog / ScrollView 等 | stdlib |
| `layout/` | LinearLayout / GridLayout（盒模型流式布局） | stdlib |
| `animation/` | Tween / CoroAnim | stdlib + coro |
| `uix/` | **UIX 声明式 UI 引擎**（UixLoader + PropBag + ViewModel） | stdlib + json（headless） |
| `backend/` | SDL 渲染后端（可插拔） | stdlib + sdl |

**依赖边界**：uikit 全部文件只 `import` MYP 标准库（env/text/json/fmt/coro/sdl/ttf），
**零 MOS 内部依赖**。UIX 引擎（uix/）纯 headless——不依赖 SDL，可在任何环境测试逻辑。

## UIX 声明式 UI（对标"声明式 DSL"的自研方案）

`.uix` 描述 = JSON 组件树（属性 + 绑定 + 命令）；`.usp` 样式表 = JSON 选择器
（类型 / #id / .class → 属性集）。见 `MOS/assets/uix/login.uix|usp` 与
`MOS/tests/uix_logic.myp`。

```myp
// 声明式界面（内嵌 .uix）
string uix = "{\"type\":\"Column\",\"id\":\"root\",..."
    + "{\"type\":\"Label\",\"id\":\"label\",\"text\":\"count: 0\",\"bind\":\"text\"},"
    + "{\"type\":\"Button\",\"id\":\"inc\",\"text\":\"+\",\"onCmd\":\"inc\"}"
    + "]}";

// 加载：.uix → View 树（绑定表 + 命令表）
UixLoader loader = new UixLoader(uix, "");
RootView rv = new RootView(16, 400, 300);
loader.buildInto(rv);

// MVVM：ViewModel 组合 PropBag 存状态，sync() 刷新绑定，runCmd() 执行命令
CounterVM vm = new CounterVM();
loader.sync(vm);
// 交互：Button.Clicked → mapping → cmdFor → vm.runCmd → loader.sync(vm)
```

## MVVM 分层

- **View** = `.uix` 声明式描述（组件树 + 绑定 + 命令），无逻辑
- **ViewModel** = MYP 类，组合 `PropBag` 存状态（`getProp`/`runCmd` 实现
  `ViewModel` 接口），供 `UixLoader.sync()` 读取
- **Model** = 数据类
- **引擎** = `UixLoader`（构建树 + 应用样式 + 管理绑定/命令）

## 渲染后端（可插拔）

`core/renderer.myp` 的 `Renderer` 是抽象接口（fillRect/drawText/drawImage/...）。
`backend/sdl_renderer.myp` 是 SDL2 实现。UIX 与控件逻辑不依赖具体后端——
headless 测试用 MockRenderer，桌面用 SdlRenderer，未来可加软件帧缓冲 / GPU 后端。

## 在自己的项目里用 uikit

1. 拷贝 `MOS/uikit/` 目录到你的项目（或直接引用它）
2. 把需要的文件加入 mypc 编译列表（见 `build.sh` 的 `SRCS`）
3. `--stdlib` 指向 MYP 标准库（mypc 通用桥接会自动链 SDL/ttf）
4. 写你的 `.uix` + ViewModel + main

## 路线图（通用框架化）

- [x] 零 MOS 依赖、headless 引擎、Renderer 后端抽象
- [x] UIX 声明式 DSL + MVVM
- [x] 独立示例（本目录）
- [ ] UIX 控件注册扩展点（第三方自定义控件接入 .uix）
- [ ] 软件渲染后端（无 SDL 环境可跑）
- [ ] 包化分发（MYP package-path 目录结构）
