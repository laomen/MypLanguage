// sdl_bridge.c — MYP ↔ SDL2 桥接层
//
// SDL2 的 API 使用指针和 struct，MYP 的 FFI 目前只支持基本类型（int/double/string）。
// 此桥接层将所有 SDL2 操作封装为「无指针、无 struct」的纯基本类型函数，
// MYP 通过 FFI 直接调用这些函数。
//
// 每个函数使用全局或静态变量存储 SDL 内部对象（窗口、渲染器等），
// MYP 侧无需关心指针，只需要传 int/string 等基本类型。
//
// 链接由 mypc 的通用桥接发现自动完成（无需改编译器）：程序引用 myp_sdl_* 未定义
// 符号时自动编译+链接本文件（-lSDL2 来自同目录侧车 sdl_bridge.c.libs，编译标志
// 来自 sdl_bridge.c.cflags）。本文件放在 <stdlib>/bridges/ 下，或经 MYP_BRIDGES
// 环境指定目录。

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

    // 无 GPU / 无头环境（如 SDL_VIDEODRIVER=dummy）回退到软件渲染
    if (!g_renderer)
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);

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

// ---- Accessors（供 sdl_ttf_bridge.c 复用窗口/渲染器）----
SDL_Renderer* myp_sdl_get_renderer(void) { return g_renderer; }
SDL_Window*   myp_sdl_get_window(void)   { return g_window; }

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

// ═══════════════════════════════════════════
// 文本渲染（5×7 位图字体，纯 SDL2，无 SDL_ttf 依赖）
// ═══════════════════════════════════════════
// 每个字符 5 列 × 7 行；每字符 5 字节，每字节=一列，bit r = 第 r 行（上→下）。
// 小写映射到大写字形（字体不分大小写）。未收录字符画为空格。
// 经典 5×7 字体表（ascii 32..126）。
static const unsigned char MYFONT5X7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 32 空格 */
    {0x00,0x00,0x5F,0x00,0x00}, /* 33 ! */
    {0x00,0x07,0x00,0x07,0x00}, /* 34 " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 35 # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 36 $ */
    {0x23,0x13,0x08,0x64,0x62}, /* 37 % */
    {0x36,0x49,0x55,0x22,0x50}, /* 38 & */
    {0x00,0x05,0x03,0x00,0x00}, /* 39 ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* 40 ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* 41 ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* 42 * */
    {0x08,0x08,0x3E,0x08,0x08}, /* 43 + */
    {0x00,0x50,0x30,0x00,0x00}, /* 44 , */
    {0x08,0x08,0x08,0x08,0x08}, /* 45 - */
    {0x00,0x60,0x60,0x00,0x00}, /* 46 . */
    {0x20,0x10,0x08,0x04,0x02}, /* 47 / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 48 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 49 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 50 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 51 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 52 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 53 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 54 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 55 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 56 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 57 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* 58 : */
    {0x00,0x56,0x36,0x00,0x00}, /* 59 ; */
    {0x00,0x08,0x14,0x22,0x41}, /* 60 < */
    {0x14,0x14,0x14,0x14,0x14}, /* 61 = */
    {0x41,0x22,0x14,0x08,0x00}, /* 62 > */
    {0x02,0x01,0x51,0x09,0x06}, /* 63 ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* 64 @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 65 A */
    {0x7F,0x49,0x49,0x49,0x36}, /* 66 B */
    {0x3E,0x41,0x41,0x41,0x22}, /* 67 C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 68 D */
    {0x7F,0x49,0x49,0x49,0x41}, /* 69 E */
    {0x7F,0x09,0x09,0x09,0x01}, /* 70 F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 71 G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 72 H */
    {0x00,0x41,0x7F,0x41,0x00}, /* 73 I */
    {0x20,0x40,0x41,0x3F,0x01}, /* 74 J */
    {0x7F,0x08,0x14,0x22,0x41}, /* 75 K */
    {0x7F,0x40,0x40,0x40,0x40}, /* 76 L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 77 M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 78 N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 79 O */
    {0x7F,0x09,0x09,0x09,0x06}, /* 80 P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 81 Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* 82 R */
    {0x46,0x49,0x49,0x49,0x31}, /* 83 S */
    {0x01,0x01,0x7F,0x01,0x01}, /* 84 T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 85 U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 86 V */
    {0x7F,0x20,0x18,0x20,0x7F}, /* 87 W */
    {0x63,0x14,0x08,0x14,0x63}, /* 88 X */
    {0x03,0x04,0x78,0x04,0x03}, /* 89 Y */
    {0x61,0x51,0x49,0x45,0x43}, /* 90 Z */
    {0x00,0x7F,0x41,0x41,0x00}, /* 91 [ */
    {0x02,0x04,0x08,0x10,0x20}, /* 92 \ */
    {0x00,0x41,0x41,0x7F,0x00}, /* 93 ] */
    {0x04,0x02,0x01,0x02,0x04}, /* 94 ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* 95 _ */
    {0x00,0x01,0x02,0x04,0x00}, /* 96 ` */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 97 a (→A) */
    {0x7F,0x49,0x49,0x49,0x36}, /* 98 b (→B) */
    {0x3E,0x41,0x41,0x41,0x22}, /* 99 c (→C) */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 100 d (→D) */
    {0x7F,0x49,0x49,0x49,0x41}, /* 101 e (→E) */
    {0x7F,0x09,0x09,0x09,0x01}, /* 102 f (→F) */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 103 g (→G) */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 104 h (→H) */
    {0x00,0x41,0x7F,0x41,0x00}, /* 105 i (→I) */
    {0x20,0x40,0x41,0x3F,0x01}, /* 106 j (→J) */
    {0x7F,0x08,0x14,0x22,0x41}, /* 107 k (→K) */
    {0x7F,0x40,0x40,0x40,0x40}, /* 108 l (→L) */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 109 m (→M) */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 110 n (→N) */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 111 o (→O) */
    {0x7F,0x09,0x09,0x09,0x06}, /* 112 p (→P) */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 113 q (→Q) */
    {0x7F,0x09,0x19,0x29,0x46}, /* 114 r (→R) */
    {0x46,0x49,0x49,0x49,0x31}, /* 115 s (→S) */
    {0x01,0x01,0x7F,0x01,0x01}, /* 116 t (→T) */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 117 u (→U) */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 118 v (→V) */
    {0x7F,0x20,0x18,0x20,0x7F}, /* 119 w (→W) */
    {0x63,0x14,0x08,0x14,0x63}, /* 120 x (→X) */
    {0x03,0x04,0x78,0x04,0x03}, /* 121 y (→Y) */
    {0x61,0x51,0x49,0x45,0x43}, /* 122 z (→Z) */
    {0x00,0x08,0x36,0x41,0x00}, /* 123 { */
    {0x00,0x00,0x7F,0x00,0x00}, /* 124 | */
    {0x00,0x41,0x36,0x08,0x00}, /* 125 } */
    {0x02,0x01,0x02,0x04,0x02}, /* 126 ~ */
};

// 在 (x,y) 绘制文本（5×7 位图字体，scale 为像素放大倍数）
void myp_sdl_draw_text(int x, int y, const char* text, int scale, int r, int g, int b, int a) {
    if (!g_renderer || !text) return;
    if (scale < 1) scale = 1;
    SDL_SetRenderDrawColor(g_renderer, r, g, b, a);
    int cx = x;
    const unsigned char* p = (const unsigned char*)text;
    for (; *p; p++) {
        unsigned char ch = *p;
        if (ch < 32 || ch > 126) { cx += 6 * scale; continue; }
        const unsigned char* gl = MYFONT5X7[ch - 32];
        for (int c = 0; c < 5; c++) {
            unsigned char col = gl[c];
            for (int rr = 0; rr < 7; rr++) {
                if (col & (1u << rr)) {
                    SDL_Rect px = { cx + c * scale, y + rr * scale, scale, scale };
                    SDL_RenderFillRect(g_renderer, &px);
                }
            }
        }
        cx += 6 * scale;  // 5 列 + 1 列间隔
    }
}

// ═══════════════════════════════════════════

// 保存当前帧为 BMP（调试 / 冒烟测试可视化）
// 返回 0=成功, -1=失败
int myp_sdl_save_bmp(const char* path) {
    if (!g_renderer || !g_window) return -1;
    int w, h;
    SDL_GetWindowSize(g_window, &w, &h);
    SDL_Surface* s = SDL_CreateRGBSurface(
        0, w, h, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
    if (!s) return -1;
    if (SDL_RenderReadPixels(g_renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
                             s->pixels, s->pitch) != 0) {
        SDL_FreeSurface(s);
        return -1;
    }
    int ok = SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    return ok == 0 ? 0 : -1;
}
