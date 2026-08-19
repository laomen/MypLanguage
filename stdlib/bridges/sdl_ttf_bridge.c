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
static int g_font_px = 16;          // 当前基字号（scale 乘数基准）
static char g_font_path[512] = "";   // 当前字体文件路径（setSize 重新打开用）

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
        if (g_font) { strncpy(g_font_path, candidates[i], sizeof(g_font_path) - 1); break; }
    }
    if (!g_font) { TTF_Quit(); return -1; }
    g_font_loaded = 1;
    g_font_px = px;
    return 0;
}

// 加载指定字体文件 + 字号（替换当前字体；0=成功, -1=失败）。
int myp_ttf_load_font(const char* path, int px) {
    if (!path || !path[0] || px <= 0) return -1;
    if (!TTF_WasInit() && TTF_Init() < 0) return -1;
    TTF_Font* f = TTF_OpenFont(path, px);
    if (!f) return -1;
    if (g_font) TTF_CloseFont(g_font);
    g_font = f;
    g_font_loaded = 1;
    g_font_px = px;
    strncpy(g_font_path, path, sizeof(g_font_path) - 1);
    g_font_path[sizeof(g_font_path) - 1] = '\0';
    return 0;
}

// 调整当前字体字号（重新打开同路径；未加载时按自动候选 init）。
// 返回 0=成功, -1=失败。
int myp_ttf_set_size(int px) {
    if (px <= 0) return -1;
    if (!g_font_loaded) return myp_ttf_init(px);
    if (!g_font_path[0]) return -1;
    TTF_Font* f = TTF_OpenFont(g_font_path, px);
    if (!f) return -1;
    if (g_font) TTF_CloseFont(g_font);
    g_font = f;
    g_font_px = px;
    return 0;
}

// 渲染/测量前把字号临时调到 base*scale（scale>1），避免渲染后像素放大导致模糊。
// 返回 0=可继续（scale 已应用或 scale<=1）。
static int font_apply_scale(int scale) {
    if (scale <= 1) return 0;
    return TTF_SetFontSize(g_font, g_font_px * scale);
}
static void font_restore(void) {
    TTF_SetFontSize(g_font, g_font_px);
}

// 渲染 UTF8 文本到 (x,y)，scale 放大倍数（1 = 原始字号），颜色 (r,g,b,a)。
// 返回 0=成功, -1=失败（字体未就绪 / 渲染失败）。
int myp_ttf_draw_text(int x, int y, const char* text, int scale,
                      int r, int g, int b, int a) {
    if (!g_font_loaded || !g_font || !text) return -1;
    SDL_Renderer* ren = myp_sdl_get_renderer();
    if (!ren) return -1;
    // 按 scale 调字号渲染（清晰），而非渲染后像素放大（模糊）
    if (font_apply_scale(scale) != 0) return -1;

    SDL_Color color = { (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a };
    SDL_Surface* surf = TTF_RenderUTF8_Blended(g_font, text, color);
    font_restore();
    if (!surf) return -1;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
    if (!tex) { SDL_FreeSurface(surf); return -1; }

    // 原始渲染尺寸（字号已含 scale，不再放大）
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_RenderCopy(ren, tex, NULL, &dst);

    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
    return 0;
}

// 测量 UTF-8 文本渲染宽度（像素，字号按 scale 调整）。失败返回 -1（字体未就绪）。
int myp_ttf_text_width(const char* text, int scale) {
    if (!g_font_loaded || !g_font || !text) return -1;
    if (font_apply_scale(scale) != 0) return -1;
    int w = 0, h = 0;
    int rc = TTF_SizeUTF8(g_font, text, &w, &h);
    font_restore();
    if (rc != 0) return -1;
    return w;
}

// 字体行高（像素，字号按 scale 调整）。失败返回 -1（字体未就绪）。
int myp_ttf_text_height(int scale) {
    if (!g_font_loaded || !g_font) return -1;
    if (font_apply_scale(scale) != 0) return -1;
    int h = TTF_FontHeight(g_font);
    font_restore();
    return h;
}

// 释放字体并退出 SDL_ttf。
void myp_ttf_quit(void) {
    if (g_font) TTF_CloseFont(g_font);
    g_font = NULL;
    g_font_loaded = 0;
    if (TTF_WasInit()) TTF_Quit();
}
