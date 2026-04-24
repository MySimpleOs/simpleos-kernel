#include "font.h"

#include "../mm/heap.h"

#include <stddef.h>
#include <stdint.h>

/* ---- stb_truetype freestanding glue (no libm) ------------------------- */

#define FM_PI 3.14159265f
#define FM_HPI 1.5707963f

static int fm_ifloor(float x) {
    int i = (int) x;
    return i - (i > x);
}
static int fm_iceil(float x) {
    int i = (int) x;
    return i + (i < x);
}
static float fm_fabsf(float x) { return x < 0.f ? -x : x; }

static float fm_sqrtf(float x) {
    float y;
    if (x <= 0.f) return 0.f;
    y = x > 1.f ? x * 0.25f : x;
    for (int i = 0; i < 10; i++)
        y = 0.5f * (y + x / y);
    return y;
}

/* stb only uses STBTT_pow for real cube roots (exponent ⅓). */
static float fm_cbrtf(float v) {
    float sgn = v < 0.f ? -1.f : 1.f;
    float x = fm_fabsf(v);
    if (x == 0.f) return 0.f;
    float t = x;
    for (int i = 0; i < 14; i++)
        t = (2.f * t + x / (t * t)) * (1.f / 3.f);
    return t * sgn;
}

static float fm_fmodf(float a, float b) {
    if (b == 0.f) return 0.f;
    float bb = fm_fabsf(b);
    float aa = fm_fabsf(a);
    float r = aa - (float) (int) (aa / bb) * bb;
    if (r < 0.f) r += bb;
    return (a < 0.f) ? -r : r;
}

static float fm_atan_small(float u) {
    float u2 = u * u;
    return u * (1.f - u2 * (0.3333333f - u2 * 0.2f));
}

static float fm_atanf(float x) {
    int neg = x < 0.f;
    if (neg) x = -x;
    float res = (x <= 1.f) ? fm_atan_small(x) : (FM_HPI - fm_atan_small(1.f / x));
    return neg ? -res : res;
}

static float fm_atan2f(float y, float x) {
    if (x == 0.f) return y >= 0.f ? FM_HPI : -FM_HPI;
    if (x > 0.f) return fm_atanf(y / x);
    {
        float a = fm_atanf(y / x);
        return y >= 0.f ? a + FM_PI : a - FM_PI;
    }
}

static float fm_cosf(float x) {
    const float PI2 = 6.2831853f;
    while (x > FM_PI)  x -= PI2;
    while (x < -FM_PI) x += PI2;
    float x2 = x * x;
    return 1.f - x2 * (0.5f - x2 * (1.f / 24.f - x2 * (1.f / 720.f)));
}

static float fm_acosf(float x) {
    if (x >= 1.f) return 0.f;
    if (x <= -1.f) return FM_PI;
    return fm_atan2f(fm_sqrtf(1.f - x * x), x);
}

#define STBTT_ifloor(x)    fm_ifloor((float) (x))
#define STBTT_iceil(x)     fm_iceil((float) (x))
#define STBTT_sqrt(x)      fm_sqrtf((float) (x))
#define STBTT_pow(x, y)    fm_cbrtf((float) (x))
#define STBTT_fmod(x, y)   fm_fmodf((float) (x), (float) (y))
#define STBTT_cos(x)       fm_cosf((float) (x))
#define STBTT_acos(x)      fm_acosf((float) (x))
#define STBTT_fabs(x)      fm_fabsf((float) (x))

#define STBTT_malloc(x, u) ((void)(u), kmalloc(x))
#define STBTT_free(x, u)   ((void)(u), kfree(x))
#define STBTT_assert(x)    ((void)0)

static size_t font_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}
#define STBTT_strlen(x) font_strlen(x)

static void *font_memset(void *a, int v, size_t n) {
    unsigned char *p = (unsigned char *) a;
    while (n--) *p++ = (unsigned char) v;
    return a;
}
static void *font_memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = (unsigned char *) d;
    const unsigned char *b = (const unsigned char *) s;
    while (n--) *a++ = *b++;
    return d;
}
#define STBTT_memcpy(d, s, n) font_memcpy((d), (s), (n))
#define STBTT_memset(a, b, n) font_memset((a), (b), (n))

#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"

extern const uint8_t font_noto_sans_start[];
extern const uint8_t font_noto_sans_end[];
extern const uint8_t font_noto_symbols2_start[];
extern const uint8_t font_noto_symbols2_end[];

#define FONT_PX           22
#define FONT_SDF_PAD      5
#define FONT_ONEDGE       180
#define FONT_PDIST        36.0f
#define FONT_SMOOTH       40.0f
#define FONT_CACHE_SLOTS  128

static stbtt_fontinfo g_sans;
static stbtt_fontinfo g_sym;
static int            g_ok;
static float          g_scale;

struct cache_entry {
    uint32_t         key;   /* codepoint | (font_id << 21) */
    uint8_t         *sdf;
    int              w, h, xoff, yoff;
};

static struct cache_entry g_cache[FONT_CACHE_SLOTS];

static int utf8_decode(const char **pp) {
    const unsigned char *p = (const unsigned char *) *pp;
    unsigned c = *p;
    if (c == 0) return -1;
    if (c < 0x80) {
        *pp += 1;
        return (int) c;
    }
    if ((c >> 5) == 6 && (p[1] & 0xc0) == 0x80) {
        int cp = (int) (((c & 0x1f) << 6) | (p[1] & 0x3f));
        *pp += 2;
        return cp;
    }
    if ((c >> 4) == 0xe && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        int cp = (int) (((c & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f));
        *pp += 3;
        return cp;
    }
    if ((c >> 3) == 0x1e && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80
        && (p[3] & 0xc0) == 0x80) {
        int cp = (int) (((c & 0x07) << 18) | ((p[1] & 0x3f) << 12)
                        | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f));
        *pp += 4;
        return cp;
    }
    *pp += 1;
    return (int) c;
}

static int glyph_empty(const stbtt_fontinfo *f, int glyph, float scale) {
    int ix0, iy0, ix1, iy1;
    stbtt_GetGlyphBitmapBox(f, glyph, scale, scale, &ix0, &iy0, &ix1, &iy1);
    return ix0 == ix1 || iy0 == iy1;
}

static int likely_emoji_plane(int cp) {
    if (cp >= 0x1f000 && cp <= 0x1ffff) return 1;
    if (cp >= 0x2600 && cp <= 0x27bf) return 1;
    return 0;
}

static int pick_font_id(int cp, float scale) {
    int gs = stbtt_FindGlyphIndex(&g_sans, cp);
    int empty_s = glyph_empty(&g_sans, gs, scale);

    if (likely_emoji_plane(cp)) {
        int gm = stbtt_FindGlyphIndex(&g_sym, cp);
        if (!glyph_empty(&g_sym, gm, scale)) return 1;
        return 0;
    }
    if (!empty_s) return 0;
    {
        int gm = stbtt_FindGlyphIndex(&g_sym, cp);
        if (!glyph_empty(&g_sym, gm, scale)) return 1;
    }
    return 0;
}

static const stbtt_fontinfo *font_info(int id) {
    return id ? &g_sym : &g_sans;
}

static uint32_t cache_key(int cp, int fid) {
    return (uint32_t) cp | ((uint32_t) fid << 21);
}

static void cache_evict_one(void) {
    size_t j = 0;
    for (size_t i = 1; i < FONT_CACHE_SLOTS; i++)
        if (g_cache[i].key < g_cache[j].key) j = i;
    if (g_cache[j].sdf) {
        stbtt_FreeSDF(g_cache[j].sdf, NULL);
        g_cache[j].sdf = NULL;
    }
    g_cache[j].key = 0;
}

static struct cache_entry *cache_lookup(uint32_t key) {
    for (int i = 0; i < FONT_CACHE_SLOTS; i++)
        if (g_cache[i].key == key) return &g_cache[i];
    return NULL;
}

static struct cache_entry *cache_insert(uint32_t key) {
    struct cache_entry *e = cache_lookup(key);
    if (e) return e;
    for (int i = 0; i < FONT_CACHE_SLOTS; i++) {
        if (g_cache[i].key == 0) return &g_cache[i];
    }
    cache_evict_one();
    for (int i = 0; i < FONT_CACHE_SLOTS; i++) {
        if (g_cache[i].key == 0) return &g_cache[i];
    }
    return &g_cache[0];
}

static float smoothstep(float e0, float e1, float x) {
    if (e0 == e1) return x < e0 ? 0.0f : 1.0f;
    float t = (x - e0) / (e1 - e0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static float sdf_sample(const uint8_t *d, int w, int h, float fx, float fy) {
    if (w <= 0 || h <= 0) return 0.0f;
    if (fx < 0.0f) fx = 0.0f;
    if (fy < 0.0f) fy = 0.0f;
    if (fx > (float) (w - 1)) fx = (float) (w - 1);
    if (fy > (float) (h - 1)) fy = (float) (h - 1);
    int x0 = (int) fx;
    int y0 = (int) fy;
    int x1 = x0 + 1 < w ? x0 + 1 : x0;
    int y1 = y0 + 1 < h ? y0 + 1 : y0;
    float tx = fx - (float) x0;
    float ty = fy - (float) y0;
    float v00 = (float) d[y0 * w + x0];
    float v10 = (float) d[y0 * w + x1];
    float v01 = (float) d[y1 * w + x0];
    float v11 = (float) d[y1 * w + x1];
    float a = v00 * (1.0f - tx) + v10 * tx;
    float b = v01 * (1.0f - tx) + v11 * tx;
    return a * (1.0f - ty) + b * ty;
}

static inline uint32_t mul8x(uint32_t a, uint32_t b) {
    return (a * b + 128u) / 255u;
}

static void blend_lcd_px(uint32_t *dst, uint32_t fg,
                         float ar, float ag, float ab) {
    uint32_t fr = (fg >> 16) & 0xffu, fg_g = (fg >> 8) & 0xffu, fb = fg & 0xffu;
    uint32_t d = *dst;
    uint32_t dr = (d >> 16) & 0xffu, dg = (d >> 8) & 0xffu, db = d & 0xffu;

    uint32_t Ar = (uint32_t) (ar * 255.0f + 0.5f);
    uint32_t Ag = (uint32_t) (ag * 255.0f + 0.5f);
    uint32_t Ab = (uint32_t) (ab * 255.0f + 0.5f);
    if (Ar > 255) Ar = 255;
    if (Ag > 255) Ag = 255;
    if (Ab > 255) Ab = 255;

    uint32_t ir = 255u - Ar, ig = 255u - Ag, ib = 255u - Ab;
    uint32_t r = mul8x(fr, Ar) + mul8x(dr, ir);
    uint32_t g = mul8x(fg_g, Ag) + mul8x(dg, ig);
    uint32_t b = mul8x(fb, Ab) + mul8x(db, ib);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    *dst = 0xff000000u | (r << 16) | (g << 8) | b;
}

static int ensure_glyph(int cp, int fid, struct cache_entry **out) {
    uint32_t key = cache_key(cp, fid);
    struct cache_entry *e = cache_lookup(key);
    if (e && e->sdf) {
        *out = e;
        return 0;
    }
    e = cache_insert(key);
    if (e->sdf) {
        stbtt_FreeSDF(e->sdf, NULL);
        e->sdf = NULL;
    }
    const stbtt_fontinfo *fi = font_info(fid);
    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char *sdf = stbtt_GetCodepointSDF((stbtt_fontinfo *) fi, g_scale, cp,
                                                FONT_SDF_PAD, FONT_ONEDGE, FONT_PDIST,
                                                &w, &h, &xoff, &yoff);
    if (!sdf || w <= 0 || h <= 0) {
        e->key = 0;
        *out = NULL;
        return -1;
    }
    e->key   = key;
    e->sdf   = sdf;
    e->w     = w;
    e->h     = h;
    e->xoff  = xoff;
    e->yoff  = yoff;
    *out = e;
    return 0;
}

static void blit_sdf_glyph(struct surface *s, float xpen, int baseline,
                           const struct cache_entry *ce, uint32_t fg) {
    if (!s || !s->pixels || !ce || !ce->sdf) return;

    const uint8_t *d = ce->sdf;
    int w = ce->w, h = ce->h;
    int xoff = ce->xoff, yoff = ce->yoff;

    int ipen = (int) xpen;
    if ((float) ipen > xpen) ipen--;
    float sub = xpen - (float) ipen;
    int base_ix = ipen;

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int sx = base_ix + xoff + col;
            int sy = baseline + yoff + row;
            if (sx < 0 || sy < 0
                || sx >= (int32_t) s->width || sy >= (int32_t) s->height)
                continue;

            float gx = (float) col + sub + 0.5f;
            float gy = (float) row + 0.5f;
            /* Single-channel coverage (LCD triple sampling looks wrong on
             * arbitrary gradients / non-RGB-linear backdrops). */
            float v = sdf_sample(d, w, h, gx, gy);
            float a = smoothstep((float) FONT_ONEDGE - FONT_SMOOTH,
                                 (float) FONT_ONEDGE + FONT_SMOOTH, v);
            if (a <= 0.0f) continue;

            uint32_t *px = s->pixels + (uint32_t) sy * s->width + (uint32_t) sx;
            blend_lcd_px(px, fg, a, a, a);
        }
    }
}

int font_init(void) {
    for (int i = 0; i < FONT_CACHE_SLOTS; i++) {
        g_cache[i].key = 0;
        g_cache[i].sdf = NULL;
        g_cache[i].w = g_cache[i].h = 0;
        g_cache[i].xoff = g_cache[i].yoff = 0;
    }
    if (!stbtt_InitFont(&g_sans, (unsigned char *) font_noto_sans_start, 0))
        return -1;
    if (!stbtt_InitFont(&g_sym, (unsigned char *) font_noto_symbols2_start, 0))
        return -1;

    g_scale = stbtt_ScaleForPixelHeight(&g_sans, (float) FONT_PX);
    g_ok    = 1;
    return 0;
}

void font_shutdown(void) {
    for (int i = 0; i < FONT_CACHE_SLOTS; i++) {
        if (g_cache[i].sdf) {
            stbtt_FreeSDF(g_cache[i].sdf, NULL);
            g_cache[i].sdf = NULL;
        }
        g_cache[i].key = 0;
    }
    g_ok = 0;
}

int font_draw_utf8(struct surface *s, int x, int y,
                   const char *utf8, uint32_t fg_argb) {
    if (!g_ok || !s || !utf8) return 0;

    int asc = 0, desc = 0, linegap = 0;
    stbtt_GetFontVMetrics(&g_sans, &asc, &desc, &linegap);
    int baseline = y + (int) ((float) asc * g_scale + 0.5f);

    float xpen = (float) x;
    int prev_cp = -1;
    int prev_fid = -1;
    const char *p = utf8;

    for (;;) {
        int cp = utf8_decode(&p);
        if (cp < 0) break;
        if (cp == '\r') continue;
        if (cp == '\n') {
            prev_cp = -1;
            prev_fid = -1;
            continue;
        }

        int fid = pick_font_id(cp, g_scale);
        const stbtt_fontinfo *fi = font_info(fid);

        if (prev_cp >= 0 && fid == prev_fid) {
            int kern = stbtt_GetCodepointKernAdvance((stbtt_fontinfo *) fi,
                                                     prev_cp, cp);
            xpen += (float) kern * g_scale;
        }

        struct cache_entry *ce = NULL;
        if (ensure_glyph(cp, fid, &ce) != 0 || !ce) {
            int g = stbtt_FindGlyphIndex((stbtt_fontinfo *) fi, cp);
            int adv = 0, lsb = 0;
            stbtt_GetGlyphHMetrics((stbtt_fontinfo *) fi, g, &adv, &lsb);
            xpen += (float) adv * g_scale;
            prev_cp = cp;
            prev_fid = fid;
            continue;
        }

        blit_sdf_glyph(s, xpen, baseline, ce, fg_argb);

        int g = stbtt_FindGlyphIndex((stbtt_fontinfo *) fi, cp);
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics((stbtt_fontinfo *) fi, g, &adv, &lsb);
        xpen += (float) adv * g_scale;

        prev_cp = cp;
        prev_fid = fid;
    }

    surface_mark_dirty(s);
    return (int) (xpen - (float) x + 0.5f);
}
