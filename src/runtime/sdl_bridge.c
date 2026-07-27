// sdl_bridge.c — MYP ↔ SDL2 桥接层
//
// SDL2 的 API 使用指针和 struct，MYP 的 FFI 目前只支持基本类型（int/double/string）。
// 此桥接层将所有 SDL2 操作封装为「无指针、无 struct」的纯基本类型函数，
// MYP 通过 FFI 直接调用这些函数。
//
// 每个函数使用全局或静态变量存储 SDL 内部对象（窗口、渲染器等），
// MYP 侧无需关心指针，只需要传 int/string 等基本类型。
//
// 编译链接:
//   gcc -c sdl_bridge.c -o sdl_bridge.o `sdl2-config --cflags`
//   gcc myp_output.o sdl_bridge.o -o program `sdl2-config --libs`

#include <SDL2/SDL.h>
#include <string.h>

// ---- 内部状态 ----
static SDL_Window*   g_window   = NULL;
static SDL_Renderer* g_renderer = NULL;
static int g_width  = 800;
static int g_height = 600;

// ═══════════════════════════════════════════
// 窗口管理
// ═══════════════════════════════════════════

// 初始化 SDL 并创建窗口
// 返回 0=成功, -1=失败
int myp_sdl_init(const char* title, int w, int h) {
    g_width = w;
    g_height = h;

    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return -1;

    g_window = SDL_CreateWindow(
        title ? title : "MYP SDL",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w, h, SDL_WINDOW_SHOWN);

    if (!g_window)
        return -1;

    g_renderer = SDL_CreateRenderer(
        g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!g_renderer)
        return -1;

    return 0;
}

// 关闭窗口并清理 SDL
void myp_sdl_quit(void) {
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window)   SDL_DestroyWindow(g_window);
    SDL_Quit();
    g_renderer = NULL;
    g_window   = NULL;
}

// 检查窗口是否应该关闭
// 返回 1=应关闭, 0=继续运行
int myp_sdl_should_close(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            return 1;
    }
    return 0;
}

// ═══════════════════════════════════════════
// 渲染
// ═══════════════════════════════════════════

// 清屏 (颜色用 int 传 0-255 的 r/g/b/a)
void myp_sdl_clear(int r, int g, int b, int a) {
    SDL_SetRenderDrawColor(g_renderer, r, g, b, a);
    SDL_RenderClear(g_renderer);
}

// 提交渲染结果到屏幕
void myp_sdl_present(void) {
    SDL_RenderPresent(g_renderer);
}

// ═══════════════════════════════════════════
// 绘制基本图形
// ═══════════════════════════════════════════

// 绘制矩形 (颜色用 r/g/b/a 分量)
void myp_sdl_draw_rect(int x, int y, int w, int h, int r, int g, int b, int a) {
    SDL_Rect rect = {x, y, w, h};
    SDL_SetRenderDrawColor(g_renderer, r, g, b, a);
    SDL_RenderFillRect(g_renderer, &rect);
}

// 绘制矩形边框
void myp_sdl_draw_rect_outline(int x, int y, int w, int h, int r, int g, int b, int a) {
    SDL_Rect rect = {x, y, w, h};
    SDL_SetRenderDrawColor(g_renderer, r, g, b, a);
    SDL_RenderDrawRect(g_renderer, &rect);
}

// 绘制圆形
void myp_sdl_draw_circle(int cx, int cy, int radius, int r, int g, int b, int a) {
    SDL_SetRenderDrawColor(g_renderer, r, g, b, a);
    // Bresenham 圆算法
    int x = radius - 1;
    int y = 0;
    int dx = 1;
    int dy = 1;
    int err = dx - (radius << 1);
    while (x >= y) {
        SDL_RenderDrawPoint(g_renderer, cx + x, cy + y);
        SDL_RenderDrawPoint(g_renderer, cx + y, cy + x);
        SDL_RenderDrawPoint(g_renderer, cx - y, cy + x);
        SDL_RenderDrawPoint(g_renderer, cx - x, cy + y);
        SDL_RenderDrawPoint(g_renderer, cx - x, cy - y);
        SDL_RenderDrawPoint(g_renderer, cx - y, cy - x);
        SDL_RenderDrawPoint(g_renderer, cx + y, cy - x);
        SDL_RenderDrawPoint(g_renderer, cx + x, cy - y);
        if (err <= 0) {
            y++;
            err += dy;
            dy += 2;
        }
        if (err > 0) {
            x--;
            dx += 2;
            err += dx - (radius << 1);
        }
    }
}

// 绘制线段
void myp_sdl_draw_line(int x1, int y1, int x2, int y2, int r, int g, int b, int a) {
    SDL_SetRenderDrawColor(g_renderer, r, g, b, a);
    SDL_RenderDrawLine(g_renderer, x1, y1, x2, y2);
}

// ═══════════════════════════════════════════
// 输入
// ═══════════════════════════════════════════

// 返回当前按下的键的 SDL scancode，无按键时返回 0
int myp_sdl_get_key(void) {
    SDL_PumpEvents();
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    // 扫描常用键
    // 返回 SDL scancode 值，MYP 侧用常量判断
    // 常用的:  SDL_SCANCODE_A..Z, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_SPACE
    // 方向键: SDL_SCANCODE_UP, DOWN, LEFT, RIGHT
    // 数字:   SDL_SCANCODE_0..SDL_SCANCODE_9
    // 不要返回 0 因为有按键
    for (int sc = SDL_SCANCODE_A; sc <= SDL_SCANCODE_Z; sc++)
        if (keys[sc]) return sc;
    for (int sc = SDL_SCANCODE_0; sc <= SDL_SCANCODE_9; sc++)
        if (keys[sc]) return sc;
    if (keys[SDL_SCANCODE_RETURN])  return SDL_SCANCODE_RETURN;
    if (keys[SDL_SCANCODE_ESCAPE])  return SDL_SCANCODE_ESCAPE;
    if (keys[SDL_SCANCODE_SPACE])   return SDL_SCANCODE_SPACE;
    if (keys[SDL_SCANCODE_UP])      return SDL_SCANCODE_UP;
    if (keys[SDL_SCANCODE_DOWN])    return SDL_SCANCODE_DOWN;
    if (keys[SDL_SCANCODE_LEFT])    return SDL_SCANCODE_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])   return SDL_SCANCODE_RIGHT;
    return 0;
}

// 检查是否有关闭事件 (窗口 X 按钮)
// 返回 1=有退出事件, 0=无
int myp_sdl_has_quit_event(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 1;
    }
    return 0;
}

// ═══════════════════════════════════════════
// 工具
// ═══════════════════════════════════════════

// 返回窗口宽度
int myp_sdl_width(void) { return g_width; }

// 返回窗口高度
int myp_sdl_height(void) { return g_height; }

// 延迟 (毫秒)
void myp_sdl_delay(int ms) { SDL_Delay(ms); }
