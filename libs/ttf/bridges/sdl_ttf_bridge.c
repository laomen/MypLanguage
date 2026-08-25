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
#include <stdio.h>
#include <string.h>

// sdl_bridge.c 提供的 renderer 访问器
extern SDL_Renderer* myp_sdl_get_renderer(void);

static TTF_Font* g_font = NULL;
static int g_font_loaded = 0;
static int g_font_px = 16;          // 当前基字号（scale 乘数基准）
static char g_font_path[512] = "";   // 当前字体文件路径（setSize 重新打开用）

// ── 文本纹理缓存 ────────────────────────────────────────────────
// 同一帧/滚动重绘会反复绘制相同文本（按钮标签/列表行/坐标），每次
// TTF_RenderUTF8_Blended + CreateTextureFromSurface 开销大。缓存
// key = 实际字号 + RGB + 文本（alpha 恒 255），命中直接 RenderCopy。
// FIFO 替换；字体加载/字号变更时清空（旧纹理基于旧字体）。
#define TCACHE 256
#define TKEY 384
static SDL_Texture* g_tcache[TCACHE];
static int g_tw[TCACHE];
static int g_th[TCACHE];
static char g_tkey[TCACHE][TKEY];
static int g_tcache_next = 0;
static int g_cache_hits = 0;      // 命中次数（诊断：同文本重绘复用）
static int g_cache_misses = 0;    // 未命中次数（新纹理渲染）

static void tcache_clear(void) {
    for (int i = 0; i < TCACHE; i++) {
        if (g_tcache[i]) { SDL_DestroyTexture(g_tcache[i]); g_tcache[i] = NULL; }
        g_tkey[i][0] = '\0';
    }
    g_tcache_next = 0;
}

// 构造缓存 key；文本过长（key 截断）返回 0=不可缓存（避免长文本共用 key 错渲染）
static int tcache_key(char* out, int n, int px, int rgb, const char* text) {
    int len = snprintf(out, (size_t)n, "%d#%06x#%s", px, rgb, text);
    if (len < 0 || (size_t)len >= (size_t)n) return 0;
    return 1;
}

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
    tcache_clear();   // 字体变化 → 旧纹理失效
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
    tcache_clear();
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
    tcache_clear();
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
// 纹理缓存：同文本+字号+颜色直接复用，避免每帧重复 TTF 渲染 + 建纹理解析。
int myp_ttf_draw_text(int x, int y, const char* text, int scale,
                      int r, int g, int b, int a) {
    if (!g_font_loaded || !g_font || !text) return -1;
    SDL_Renderer* ren = myp_sdl_get_renderer();
    if (!ren) return -1;
    // 按 scale 调字号渲染（清晰），而非渲染后像素放大（模糊）
    if (font_apply_scale(scale) != 0) return -1;

    int px = g_font_px * scale;            // 实际渲染字号（key 用）
    int rgb = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
    char key[TKEY];
    int cacheable = tcache_key(key, (int)sizeof(key), px, rgb, text);

    // 命中缓存：直接 blit（字号已在渲染时应用）
    if (cacheable) {
        for (int i = 0; i < TCACHE; i++) {
            if (g_tkey[i][0] && strcmp(g_tkey[i], key) == 0) {
                SDL_Rect dst = { x, y, g_tw[i], g_th[i] };
                SDL_RenderCopy(ren, g_tcache[i], NULL, &dst);
                font_restore();
                g_cache_hits++;
                return 0;
            }
        }
    }

    // 未命中：渲染新纹理
    g_cache_misses++;
    SDL_Color color = { (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a };
    SDL_Surface* surf = TTF_RenderUTF8_Blended(g_font, text, color);
    font_restore();
    if (!surf) return -1;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
    if (!tex) { SDL_FreeSurface(surf); return -1; }

    // 原始渲染尺寸（字号已含 scale，不再放大）
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_RenderCopy(ren, tex, NULL, &dst);

    // 入缓存（FIFO 替换；纹理保留，仅释放 surface）
    if (cacheable) {
        int slot = g_tcache_next;
        if (g_tcache[slot]) SDL_DestroyTexture(g_tcache[slot]);
        g_tcache[slot] = tex;
        g_tw[slot] = surf->w;
        g_th[slot] = surf->h;
        strncpy(g_tkey[slot], key, TKEY - 1);
        g_tkey[slot][TKEY - 1] = '\0';
        g_tcache_next = (slot + 1) % TCACHE;
    } else {
        SDL_DestroyTexture(tex);   // 不可缓存（key 超长）：用完即毁
    }
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

// 缓存命中/未命中次数（诊断：验证同文本重绘是否复用纹理）
int myp_ttf_cache_hits(void)   { return g_cache_hits; }
int myp_ttf_cache_misses(void) { return g_cache_misses; }

// 释放字体并退出 SDL_ttf。
void myp_ttf_quit(void) {
    tcache_clear();
    if (g_font) TTF_CloseFont(g_font);
    g_font = NULL;
    g_font_loaded = 0;
    if (TTF_WasInit()) TTF_Quit();
}
