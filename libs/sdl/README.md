# libs/sdl — SDL2 / SDL2_ttf 外部库（MYP）

> 设计原则：**外部 C 库绑定不进标准库**——没有 MYP 语言构造绑定（不像 `@gpu for`
> 之于 GPU），SDL 是纯第三方 C 库，按外部库维护。
>
> 判定标准：**有语言构造绑定 → 运行时/语言能力（runtime_myp）；纯 ffi 绑定 →
> 外部库（本目录）。**

## 结构

```
libs/sdl/
  sdl.myp          # 便利层薄 ffi：myp_sdl_*（窗口/渲染器/事件/绘图/图像，由 sdl_bridge.c 实现）
  sdl_ffi.myp      # 纯接口薄 ffi：SDL_* 1:1（用户自己写业务逻辑）
  bridges/
    sdl_bridge.c         # 便利层实现（C，预编译成 .a/.so 免 gcc，或按需 gcc 编译）
    sdl_bridge.c.cflags
    sdl_bridge.c.libs    # -lSDL2 -lpng
libs/ttf/
  ttf.myp          # SDL_ttf 薄 ffi（myp_ttf_*，依赖 sdl 渲染器）
  bridges/
    sdl_ttf_bridge.c
    sdl_ttf_bridge.c.libs # -lSDL2_ttf
```

## 用法

```sh
# import sdl / import ttf 经包路径解析到 libs/；桥经 MYP_BRIDGES 发现
MYP_BRIDGES="libs/sdl/bridges:libs/ttf/bridges" \
  mypc app.myp --package-path libs --stdlib ../stdlib -o app
```

```myp
// 便利层（简洁，含窗口状态/事件翻译/绘图辅助）
import sdl;
import ttf;

// 或纯接口（薄，直接调 SDL_*，业务逻辑自写）
import sdl_ffi;
```

## 为什么移出 stdlib

- 编译器/运行时不该耦合第三方 GUI 库；SDL 版本/ABI 演进独立。
- 便利层逻辑（事件翻译/绘图/文字）是应用框架的事，不是语言接口。
- 需要 SDL 的人才拉入；不需要的 MYP 安装不受牵连。

## 维护

便利层 `myp_sdl_*` 保留给 mypview 等框架；新用户推荐 `sdl_ffi.myp` 纯接口 +
自写逻辑。预编译 `.a`/`.so` 可免用户侧 gcc（`ar rcs` / `cc -shared` 一次构建）。
