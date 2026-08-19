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
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- 内部状态 ----
static SDL_Window*   g_window   = NULL;
static SDL_Renderer* g_renderer = NULL;
static int g_width  = 800;
static int g_height = 600;

// BMP 纹理缓存（myp_sdl_load_bmp → 句柄；MYP 侧传句柄绘制，最多 32 个）
#define MAX_TEXTURES 32
static SDL_Texture* g_textures[MAX_TEXTURES];
static int g_tex_count = 0;

// 释放全部纹理（myp_sdl_quit 前调用；定义在下方）
void myp_sdl_free_images(void);

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
        w, h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!g_window)
        return -1;

    g_renderer = SDL_CreateRenderer(
        g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    // 无 GPU / 无头环境（如 SDL_VIDEODRIVER=dummy）回退到软件渲染
    if (!g_renderer)
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);

    if (!g_renderer)
        return -1;

    // 启用文本输入（SDL_TEXTINPUT 事件；支持任意 Unicode/IME）
    SDL_StartTextInput();

    return 0;
}

// 运行时调整窗口大小（resizable 窗口经拖拽改变后由 MYP 侧同步全局宽高；
// 也可主动 set 固定尺寸）。返回 0=成功, -1=失败。
int myp_sdl_set_window_size(int w, int h) {
    if (!g_window) return -1;
    if (w <= 0 || h <= 0) return -1;
    SDL_SetWindowSize(g_window, w, h);
    g_width = w;
    g_height = h;
    return 0;
}

// 限制窗口最大尺寸（防止拖拽放大导致 SDL 线性插值模糊；
// 逻辑尺寸 = 初始尺寸时保持 1:1 渲染）。返回 0=成功, -1=失败。
int myp_sdl_set_max_window_size(int w, int h) {
    if (!g_window) return -1;
    if (w <= 0 || h <= 0) return -1;
    SDL_SetWindowMaximumSize(g_window, w, h);
    return 0;
}

// 锁定窗口尺寸（min=max=指定值）：完全禁止拖拽改变窗口大小，
// 渲染始终 1:1，杜绝 SDL 缩放导致的模糊。返回 0=成功, -1=失败。
int myp_sdl_lock_window_size(int w, int h) {
    if (!g_window) return -1;
    if (w <= 0 || h <= 0) return -1;
    SDL_SetWindowMinimumSize(g_window, w, h);
    SDL_SetWindowMaximumSize(g_window, w, h);
    return 0;
}

// 无边框模式（手机式应用：去掉标题栏/边框，保持应用自身窗口尺寸，不铺满
// 显示器）。on=1 去边框，on=0 恢复。返回 0=成功。
int myp_sdl_set_borderless_fullscreen(int on) {
    if (!g_window) return -1;
    if (on != 0) {
        SDL_SetWindowBordered(g_window, SDL_FALSE);
    } else {
        SDL_SetWindowBordered(g_window, SDL_TRUE);
    }
    return 0;
}

// 查询当前显示器尺寸（供应用按屏幕全屏布局）。返回 (h<<16)|w；失败 -1。
int myp_sdl_display_size(void) {
    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) != 0) return -1;
    int w = dm.w;
    int h = dm.h;
    return (h << 16) | (w & 0xFFFF);
}

// 设置逻辑尺寸（SDL_RenderSetLogicalSize）：MYP 侧所有绘制坐标按逻辑尺寸，
// 窗口实际可缩放显示（如显示器不够大时按 0.5 倍）。逻辑尺寸独立于窗口物理
// 尺寸，SDL 自动把逻辑渲染缩放到窗口。返回 0=成功, -1=失败。
int myp_sdl_set_logical_size(int w, int h) {
    if (!g_renderer) return -1;
    if (w <= 0 || h <= 0) return -1;
    if (SDL_RenderSetLogicalSize(g_renderer, w, h) != 0) return -1;
    return 0;
}

// ═══════════════════════════════════════════
// 窗口管理（多任务：前台/后台切换）
// ═══════════════════════════════════════════

// 隐藏窗口（应用退后台，手机式 home 键效果）。返回 0=成功。
int myp_sdl_hide_window(void) {
    if (!g_window) return -1;
    SDL_HideWindow(g_window);
    return 0;
}

// 显示窗口并置顶（应用回前台，从多任务恢复）。返回 0=成功。
int myp_sdl_show_window(void) {
    if (!g_window) return -1;
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    return 0;
}

// 查询窗口是否可见（1=可见/前台，0=隐藏/后台）。返回 -1=无效。
int myp_sdl_window_visible(void) {
    if (!g_window) return -1;
    return (SDL_GetWindowFlags(g_window) & SDL_WINDOW_SHOWN) != 0 ? 1 : 0;
}

// 关闭窗口并清理 SDL
void myp_sdl_quit(void) {
    myp_sdl_free_images();
    SDL_StopTextInput();
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

// 绘制圆角矩形（iOS 风格应用图标背景）：主体矩形 + 四角填充为圆角。
// 用逐行扫描：每行两端在圆角区域向内收缩 radius 像素。
// 颜色用 r/g/b/a 分量。
void myp_sdl_fill_rounded_rect(int x, int y, int w, int h, int radius,
                               int r, int g, int b, int a) {
    if (w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    SDL_SetRenderDrawColor(g_renderer, r, g, b, a);
    for (int row = 0; row < h; row++) {
        // 顶部/底部圆角区：左右各缩进 dx；中部全宽
        int inset = 0;
        int top = radius - row;            // 距顶圆角中心
        int bot = row - (h - radius);      // 距底圆角中心
        if (top > 0) {
            // 顶部圆角：dx = radius - sqrt(radius^2 - top^2)
            int d2 = radius * radius - top * top;
            int dd = 0;
            while ((dd + 1) * (dd + 1) <= d2) dd++;
            inset = radius - dd;
        } else if (bot >= 0) {
            int d2 = radius * radius - bot * bot;
            int dd = 0;
            while ((dd + 1) * (dd + 1) <= d2) dd++;
            inset = radius - dd;
        }
        SDL_RenderDrawLine(g_renderer, x + inset, y + row,
                           x + w - 1 - inset, y + row);
    }
}

// 绘制矩形边框
void myp_sdl_draw_rect_outline(int x, int y, int w, int h, int r, int g, int b, int a) {
    SDL_Rect rect = {x, y, w, h};
    SDL_SetRenderDrawColor(g_renderer, r, g, b, a);
    SDL_RenderDrawRect(g_renderer, &rect);
}

// ═══════════════════════════════════════════
// 图片（BMP，SDL2 原生无额外依赖）
// ═══════════════════════════════════════════

// 加载 BMP 为纹理，返回句柄（0..31）；失败 -1。缓存避免重复加载。
int myp_sdl_load_bmp(const char* path) {
    if (!g_renderer || !path) return -1;
    if (g_tex_count >= MAX_TEXTURES) return -1;
    SDL_Surface* s = SDL_LoadBMP(path);
    if (!s) return -1;
    // 支持透明色键：左上角像素作为透明色（iOS 图标常带背景色）
    // （可选：默认不启用，避免误判。由调用侧决定是否传色键）
    SDL_Texture* t = SDL_CreateTextureFromSurface(g_renderer, s);
    SDL_FreeSurface(s);
    if (!t) return -1;
    int h = g_tex_count;
    g_textures[h] = t;
    g_tex_count++;
    return h;
}

// 绘制已加载的 BMP 纹理到 (x,y,w,h)。返回 0=成功, -1=无效句柄。
int myp_sdl_draw_image(int handle, int x, int y, int w, int h) {
    if (handle < 0 || handle >= g_tex_count || !g_textures[handle]) return -1;
    if (!g_renderer) return -1;
    SDL_Rect dst = {x, y, w, h};
    if (SDL_RenderCopy(g_renderer, g_textures[handle], NULL, &dst) != 0) return -1;
    return 0;
}

// ═══════════════════════════════════════════
// 图片（PNG，libpng 解码 → 纹理；复用 BMP 纹理缓存）
// ═══════════════════════════════════════════

// 从已读入内存的 PNG 数据解码为 RGBA 像素（libpng 标准流程）。
// 成功返回 0 并写 *out_w/*out_h/*out_px（px 需 free）；失败返回 -1。
static int decode_png_rgba(const unsigned char* data, size_t len,
                           int* out_w, int* out_h, unsigned char** out_px) {
    png_image image;   // libpng 简化 API（png_image）
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, data, len))
        return -1;
    // 转 RGBA（8 位每通道）
    image.format = PNG_FORMAT_RGBA;
    size_t pxsize = PNG_IMAGE_SIZE(image);
    unsigned char* px = (unsigned char*)malloc(pxsize);
    if (!px) {
        png_image_free(&image);
        return -1;
    }
    if (!png_image_finish_read(&image, NULL, px, 0, NULL)) {
        free(px);
        png_image_free(&image);
        return -1;
    }
    *out_w = (int)image.width;
    *out_h = (int)image.height;
    *out_px = px;
    return 0;
}

// 加载 PNG 文件为纹理，返回句柄（0..31）；失败 -1。复用 BMP 纹理缓存。
int myp_sdl_load_png(const char* path) {
    if (!g_renderer || !path) return -1;
    if (g_tex_count >= MAX_TEXTURES) return -1;
    // 读整个文件
    FILE* fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return -1; }
    unsigned char* buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(fp); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) { free(buf); return -1; }

    int w = 0, h = 0;
    unsigned char* px = NULL;
    if (decode_png_rgba(buf, (size_t)sz, &w, &h, &px) != 0 || !px || w <= 0 || h <= 0) {
        free(buf);
        if (px) free(px);
        return -1;
    }
    free(buf);

    // RGBA → SDL_Texture（保留 alpha 通道）
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(
        px, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    if (!s) { free(px); return -1; }
    SDL_Texture* t = SDL_CreateTextureFromSurface(g_renderer, s);
    SDL_FreeSurface(s);
    free(px);
    if (!t) return -1;

    int hh = g_tex_count;
    g_textures[hh] = t;
    g_tex_count++;
    return hh;
}

// 释放全部纹理（SDL_quit 前）
void myp_sdl_free_images(void) {
    for (int i = 0; i < g_tex_count; i++) {
        if (g_textures[i]) SDL_DestroyTexture(g_textures[i]);
        g_textures[i] = NULL;
    }
    g_tex_count = 0;
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

// 最近一次鼠标左键点击坐标（每次 poll 消耗后清零；-1=无新点击）
static int g_mouse_x = -1;
static int g_mouse_y = -1;
// 最近一次鼠标位置（SDL_MOUSEMOTION 常驻更新；-1=从未移动）。
// 与点击坐标分离：点击是一次性消费，位置持续可查（hover 检测用）。
static int g_hover_x = -1;
static int g_hover_y = -1;
// 左键当前是否按下（MOUSEBUTTONDOWN/UP 维护；拖拽 onMove 判定用）。
static int g_mouse_down = 0;

// ---- 文本输入队列（SDL_TEXTINPUT 的 UTF-8 字符 + 控制键码点）----
// getChar 返回 Unicode 码点：普通字符=字符码点；退格=8；回车=13。无输入 -1。
#define INPUT_QUEUE 128
static unsigned char g_ichar[INPUT_QUEUE][5];   // 每项一个 UTF-8 字符（≤4 字节+\0）
static int g_ihead = 0;
static int g_icount = 0;

// UTF-8 首字符字节数（无效序列按 1 字节处理避免卡死）
static int utf8_seq_len(const unsigned char* s) {
    if (!s || !s[0]) return 0;
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// UTF-8 解码 → 码点；失败返回 -1
static int utf8_decode_cp(const unsigned char* s, int n) {
    if (!s || n <= 0) return -1;
    unsigned char c = s[0];
    if (c < 0x80) return (int)c;
    if (n == 2 && (c & 0xE0) == 0xC0)
        return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    if (n == 3 && (c & 0xF0) == 0xE0)
        return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    if (n == 4 && (c & 0xF8) == 0xF0)
        return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
               ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    return -1;
}

// 码点 → UTF-8 入队
static void input_enqueue_cp(int cp) {
    unsigned char tmp[4];
    int n;
    if (cp < 0x80) { tmp[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) {
        tmp[0] = 0xC0 | (cp >> 6); tmp[1] = 0x80 | (cp & 0x3F); n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = 0xE0 | (cp >> 12); tmp[1] = 0x80 | ((cp >> 6) & 0x3F);
        tmp[2] = 0x80 | (cp & 0x3F); n = 3;
    } else {
        tmp[0] = 0xF0 | (cp >> 18); tmp[1] = 0x80 | ((cp >> 12) & 0x3F);
        tmp[2] = 0x80 | ((cp >> 6) & 0x3F); tmp[3] = 0x80 | (cp & 0x3F); n = 4;
    }
    if (g_icount >= INPUT_QUEUE) return;
    int slot = (g_ihead + g_icount) % INPUT_QUEUE;
    memcpy(g_ichar[slot], tmp, (size_t)n);
    g_ichar[slot][n] = '\0';
    g_icount++;
}

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
    if (keys[SDL_SCANCODE_TAB])     return SDL_SCANCODE_TAB;
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
        // 文本输入：把 SDL_TEXTINPUT 的 UTF-8 片段逐字符入队（IME/中文/符号）
        if (e.type == SDL_TEXTINPUT) {
            const unsigned char* p = (const unsigned char*)e.text.text;
            while (*p) {
                int n = utf8_seq_len(p);
                if (n <= 0) break;
                if (g_icount < INPUT_QUEUE) {
                    int slot = (g_ihead + g_icount) % INPUT_QUEUE;
                    if (n > 4) n = 4;
                    memcpy(g_ichar[slot], p, (size_t)n);
                    g_ichar[slot][n] = '\0';
                    g_icount++;
                }
                p += n;
            }
        }
        // 控制键（SDL_TEXTINPUT 不含）：退格/回车以特殊码点入队
        else if (e.type == SDL_KEYDOWN) {
            if (e.key.keysym.sym == SDLK_BACKSPACE) input_enqueue_cp(8);
            else if (e.key.keysym.sym == SDLK_RETURN) input_enqueue_cp(13);
        }
        // 鼠标左键按下 → 记录点击坐标（下一帧 MYP 侧 poll 消费）+ 置按下状态
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            g_mouse_x = e.button.x;
            g_mouse_y = e.button.y;
            g_mouse_down = 1;
        }
        // 鼠标左键抬起 → 清除按下状态（拖拽结束判定）
        if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
            g_mouse_down = 0;
        }
        // 鼠标移动 → 常驻记录位置（hover 检测；坐标随窗口缩放自动映射）
        if (e.type == SDL_MOUSEMOTION) {
            g_hover_x = e.motion.x;
            g_hover_y = e.motion.y;
        }
        // 拖拽调整窗口大小 → 同步全局宽高（MYP 侧 get_window_size 可查）
        if (e.type == SDL_WINDOWEVENT &&
            e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            g_width = e.window.data1;
            g_height = e.window.data2;
        }
    }
    return 0;
}

// 取一个输入字符码点（UTF-8 解码；控制键 8=退格 13=回车）；无输入返回 -1。
// 消费即出队，避免帧循环重复读到同字符。MYP 侧用 Str.chr(cp) 转字符串。
int myp_sdl_get_char(void) {
    if (g_icount <= 0) return -1;
    int n = utf8_seq_len(g_ichar[g_ihead]);
    int cp = utf8_decode_cp(g_ichar[g_ihead], n);
    g_ihead = (g_ihead + 1) % INPUT_QUEUE;
    g_icount--;
    return cp;
}

// 显式开关文本输入（init 已默认开启；需要临时禁用/恢复时用）
void myp_sdl_start_text_input(void) { SDL_StartTextInput(); }
void myp_sdl_stop_text_input(void)  { SDL_StopTextInput(); }

// 取一次鼠标左键点击：返回 (y<<16)|x（x,y 为窗口坐标），无新点击返回 -1。
// 消费即清零，避免帧循环重复触发同一点击。
int myp_sdl_get_mouse_click(void) {
    if (g_mouse_x < 0) return -1;
    int packed = (g_mouse_y << 16) | (g_mouse_x & 0xFFFF);
    g_mouse_x = -1;
    g_mouse_y = -1;
    return packed;
}

// 取当前鼠标位置：返回 (y<<16)|x（窗口逻辑坐标，随缩放自动映射），
// 从未移动过返回 -1。不消费（帧循环可反复查询做 hover 检测）。
int myp_sdl_get_mouse_pos(void) {
    if (g_hover_x < 0) return -1;
    return (g_hover_y << 16) | (g_hover_x & 0xFFFF);
}

// 取左键当前是否按下：1=按住 0=未按（由 DOWN/UP 事件维护，不消费）。
// 配合 getMousePos 做拖拽（onPress→onMove→onRelease 事件流驱动）。
int myp_sdl_get_mouse_down(void) {
    return g_mouse_down;
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
    if (!g_renderer || !g_window) { fprintf(stderr, "save_bmp: no renderer/window\n"); return -1; }
    int w, h;
    SDL_GetWindowSize(g_window, &w, &h);
    SDL_Surface* s = SDL_CreateRGBSurface(
        0, w, h, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000);
    if (!s) { fprintf(stderr, "save_bmp: CreateRGBSurface: %s\n", SDL_GetError()); return -1; }
    if (SDL_RenderReadPixels(g_renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
                             s->pixels, s->pitch) != 0) {
        fprintf(stderr, "save_bmp: RenderReadPixels: %s\n", SDL_GetError());
        SDL_FreeSurface(s);
        return -1;
    }
    int ok = SDL_SaveBMP(s, path);
    if (ok != 0) fprintf(stderr, "save_bmp: SaveBMP: %s\n", SDL_GetError());
    SDL_FreeSurface(s);
    return ok == 0 ? 0 : -1;
}
