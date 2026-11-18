// The start-up screen's text (splash.h): each string rendered at its exact pixel size with stb_truetype from the
// menu's own font (Roboto Medium, Apache-2.0, ui/fonts/LICENSE-Roboto.txt, embedded by ui/menu.cpp) into one 8-bit
// coverage atlas, a row of the atlas per string. vk_splash.c hands the atlas and the boxes to the screen's shader.
#include "splash.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// A private copy of stb_truetype (public domain or MIT, Dear ImGui's imstb_truetype.h), as ui/menu.cpp keeps one.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "third_party/imgui/imstb_truetype.h"
#pragma clang diagnostic pop

const unsigned char *menu_font_ttf(size_t *size);   // ui/menu.cpp: the font file the menu embeds

struct SplashText {
    stbtt_fontinfo font;
    int ascent, descent, gap;     // font units
    uint8_t *px;                  // the atlas
    int w, h, cap_w, cap_h;       // its size in use and allocated
};

SplashText *splash_text_new(void) {
    size_t n = 0;
    const unsigned char *ttf = menu_font_ttf(&n);
    SplashText *t = calloc(1, sizeof *t);
    if (!t || !ttf || n < 1024 || !stbtt_InitFont(&t->font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        free(t);
        return NULL;
    }
    stbtt_GetFontVMetrics(&t->font, &t->ascent, &t->descent, &t->gap);
    return t;
}

void splash_text_free(SplashText *t) {
    if (!t) return;
    free(t->px);
    free(t);
}

void splash_text_begin(SplashText *t) {
    t->w = t->h = 0;
    if (t->px) memset(t->px, 0, (size_t)t->cap_w * t->cap_h);
}

const uint8_t *splash_text_atlas(SplashText *t, int *w, int *h) {
    *w = t->cap_w, *h = t->h;
    return t->px;
}

// The next code point of a UTF-8 string; malformed bytes count as themselves
static int next_cp(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++, extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (extra) {
        c &= 0x3f >> extra;
        for (int i = 0; i < extra && (*p & 0xc0) == 0x80; i++) c = c << 6 | (*p++ & 0x3f);
    }
    *s = (const char *)p;
    return c;
}

static float scale_of(SplashText *t, float px) { return stbtt_ScaleForMappingEmToPixels(&t->font, px); }

float splash_text_ascent(SplashText *t, float px) { return (float)t->ascent * scale_of(t, px); }

float splash_text_width(SplashText *t, const char *s, float px, float spacing) {
    float sc = scale_of(t, px), x = 0;
    int prev = 0;
    while (*s) {
        int cp = next_cp(&s), adv, lsb;
        if (prev) x += sc * (float)stbtt_GetCodepointKernAdvance(&t->font, prev, cp);
        stbtt_GetCodepointHMetrics(&t->font, cp, &adv, &lsb);
        x += sc * (float)adv + (*s ? spacing : 0);
        prev = cp;
    }
    return x;
}

// Room for rows more of the atlas, at least w wide (a multiple of 4: the shader reads it a word at a time)
static int reserve(SplashText *t, int w, int rows) {
    w = (w + 3) & ~3;
    if (w <= t->cap_w && t->h + rows <= t->cap_h) return 1;
    int nw = w > t->cap_w ? w : t->cap_w, nh = t->h + rows > t->cap_h ? (t->h + rows) * 3 / 2 + 64 : t->cap_h;
    uint8_t *n = calloc((size_t)nw * nh, 1);
    if (!n) return 0;
    for (int y = 0; y < t->h; y++) memcpy(n + (size_t)y * nw, t->px + (size_t)y * t->cap_w, (size_t)t->cap_w);
    free(t->px);
    t->px = n, t->cap_w = nw, t->cap_h = nh;
    return 1;
}

SplashGlyphBox splash_text_add(SplashText *t, const char *s, float px, float spacing, float x, float baseline, int align) {
    SplashGlyphBox b = {0};
    if (!s || !*s || px < 2) return b;
    float sc = scale_of(t, px), width = splash_text_width(t, s, px, spacing);
    int pad = (int)ceilf(px * 0.08f) + 3;
    int asc = (int)ceilf((float)t->ascent * sc), desc = (int)ceilf((float)-t->descent * sc);
    int bw = (int)ceilf(width) + 2 * pad, bh = asc + desc + 2 * pad;
    if (!reserve(t, bw, bh)) return b;
    int row = t->h;
    t->h += bh;
    float left = align ? x - width : x;
    float origin = floorf(left);   // the box starts on a whole pixel; the glyphs keep the fraction
    float pen = left - origin + (float)pad;
    int prev = 0;
    while (*s) {
        int cp = next_cp(&s), adv, lsb;
        if (prev) pen += sc * (float)stbtt_GetCodepointKernAdvance(&t->font, prev, cp);
        stbtt_GetCodepointHMetrics(&t->font, cp, &adv, &lsb);
        int x0, y0, x1, y1;
        float fx = pen - floorf(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&t->font, cp, sc, sc, fx, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            unsigned char *g = malloc((size_t)gw * gh);
            if (g) {
                stbtt_MakeCodepointBitmapSubpixel(&t->font, g, gw, gh, gw, sc, sc, fx, 0, cp);
                int ox = (int)floorf(pen) + x0, oy = pad + asc + y0;
                for (int y = 0; y < gh; y++) {
                    int ay = oy + y;
                    if (ay < 0 || ay >= bh) continue;
                    uint8_t *dst = t->px + (size_t)(row + ay) * t->cap_w;
                    for (int xx = 0; xx < gw; xx++) {
                        int ax = ox + xx;
                        uint8_t v = g[(size_t)y * gw + xx];
                        if (ax >= 0 && ax < t->cap_w && v > dst[ax]) dst[ax] = v;   // the larger where glyphs touch
                    }
                }
                free(g);
            }
        }
        pen += sc * (float)adv + (*s ? spacing : 0);
        prev = cp;
    }
    b.x = origin - (float)pad;
    b.y = floorf(baseline) - (float)(asc + pad);
    b.w = (float)bw, b.h = (float)bh;
    b.ax = 0, b.ay = row;
    return b;
}
