// sdl_ttf_bridge.c — MYP ↔ SDL_ttf 桥接（中文/Unicode 文字渲染）
// ---------------------------------------------------------------------------
// 依赖 sdl_bridge.c 创建的 SDL 窗口/渲染器（经 myp_sdl_get_renderer 访问）。
// 用 TTF_RenderUTF8_Blended 渲染文本为纹理再 blit——支持中文/抗锯齿/任意
// 系统字体（Noto CJK），替代 sdl_bridge.c 的 5×7 位图（仅 ASCII）。
//
// 链接由 mypc 的通用桥接发现自动完成（无需改编译器）：程序引用 myp_ttf_* 未定义
// 符号时选中本文件（-lSDL2_ttf 来自侧车 sdl_ttf_bridge.c.libs），固定点迭代会把
// 依赖的 sdl_bridge.c（myp_sdl_get_renderer）一并拉入（-lSDL2）。编译标志来自
// sdl_ttf_bridge.c.cflags。
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string.h>

// sdl_bridge.c 提供的 renderer 访问器
extern SDL_Renderer* myp_sdl_get_renderer(void);

static TTF_Font* g_font = NULL;
static int g_font_loaded = 0;

// 初始化 SDL_ttf 并加载 Noto CJK 字体（px 字号）。
// 返回 0=成功, -1=失败（TTF_Init 或字体加载失败）。
int myp_ttf_init(int px) {
    if (g_font_loaded) return 0;
    if (px <= 0) px = 24;
    if (TTF_Init() < 0) return -1;
    // 常见 CJK 字体路径（.ttc 直接打开第一个字面，含中英日韩）
    static const char* candidates[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        g_font = TTF_OpenFont(candidates[i], px);
        if (g_font) break;
    }
    if (!g_font) { TTF_Quit(); return -1; }
    g_font_loaded = 1;
    return 0;
}

// 渲染 UTF8 文本到 (x,y)，scale 放大倍数（1 = 原始字号），颜色 (r,g,b,a)。
// 返回 0=成功, -1=失败（字体未就绪 / 渲染失败）。
int myp_ttf_draw_text(int x, int y, const char* text, int scale,
                      int r, int g, int b, int a) {
    if (!g_font_loaded || !g_font || !text) return -1;
    SDL_Renderer* ren = myp_sdl_get_renderer();
    if (!ren) return -1;

    SDL_Color color = { (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a };
    SDL_Surface* surf = TTF_RenderUTF8_Blended(g_font, text, color);
    if (!surf) return -1;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
    if (!tex) { SDL_FreeSurface(surf); return -1; }

    int w = surf->w;
    int h = surf->h;
    if (scale > 1) { w *= scale; h *= scale; }
    SDL_Rect dst = { x, y, w, h };
    SDL_RenderCopy(ren, tex, NULL, &dst);

    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
    return 0;
}

// 释放字体并退出 SDL_ttf。
void myp_ttf_quit(void) {
    if (g_font) TTF_CloseFont(g_font);
    g_font = NULL;
    g_font_loaded = 0;
    if (TTF_WasInit()) TTF_Quit();
}
