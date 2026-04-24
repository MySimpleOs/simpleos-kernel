#include "gradient.h"
#include "surface.h" /* surface_mark_dirty */

#include <stdint.h>

/* ---- integer sqrt — shared copy so gradient doesn't need shadow.c ------ */
static uint32_t isqrt_u32(uint32_t x) {
    uint32_t r = 0;
    uint32_t bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
        uint32_t rb = r + bit;
        if (x >= rb) { x -= rb; r = (r >> 1) + bit; }
        else         { r >>= 1; }
        bit >>= 2;
    }
    return r;
}

/* Lerp one 8-bit channel using a Q0.8 fraction t in [0, 256]. Rounds to
 * nearest; identical to floor((a*(256-t) + b*t + 128) / 256) within the
 * channel domain. */
static inline uint32_t lerp8(uint32_t a, uint32_t b, int32_t t) {
    int32_t delta = (int32_t) b - (int32_t) a;
    return (uint32_t) ((int32_t) a + ((delta * t + 128) >> 8));
}

static uint32_t lerp_argb(uint32_t a, uint32_t b, int32_t t) {
    /* Clamp t defensively — the callers already do this but keep the
     * function safe if reused elsewhere. */
    if (t < 0)   t = 0;
    if (t > 256) t = 256;
    uint32_t ar = (a >> 24) & 0xffu, rr = (a >> 16) & 0xffu;
    uint32_t gr = (a >>  8) & 0xffu, br =  a        & 0xffu;
    uint32_t ab = (b >> 24) & 0xffu, rb = (b >> 16) & 0xffu;
    uint32_t gb = (b >>  8) & 0xffu, bb =  b        & 0xffu;
    uint32_t A = lerp8(ar, ab, t);
    uint32_t R = lerp8(rr, rb, t);
    uint32_t G = lerp8(gr, gb, t);
    uint32_t B = lerp8(br, bb, t);
    return (A << 24) | (R << 16) | (G << 8) | B;
}

void gradient_fill_linear(struct surface *s,
                          uint32_t a, uint32_t b,
                          int32_t x0, int32_t y0,
                          int32_t x1, int32_t y1) {
    if (!s || !s->pixels) return;

    int32_t dx = x1 - x0;
    int32_t dy = y1 - y0;
    int64_t len2 = (int64_t) dx * dx + (int64_t) dy * dy;

    int32_t w = (int32_t) s->width;
    int32_t h = (int32_t) s->height;

    if (len2 == 0) {
        for (int32_t y = 0; y < h; y++) {
            uint32_t *row = s->pixels + (size_t) y * w;
            for (int32_t x = 0; x < w; x++) row[x] = a;
        }
        surface_mark_dirty(s);
        return;
    }

    /* Per pixel: t = clamp(dot(p - p0, dir) / |dir|², [0, 1]). Scale to
     * Q0.8 (t in [0, 256]) so lerp_argb works without a second divide. */
    for (int32_t y = 0; y < h; y++) {
        uint32_t *row = s->pixels + (size_t) y * w;
        for (int32_t x = 0; x < w; x++) {
            int64_t num = (int64_t) (x - x0) * dx + (int64_t) (y - y0) * dy;
            int32_t t;
            if      (num <= 0)       t = 0;
            else if (num >= len2)    t = 256;
            else                     t = (int32_t) ((num * 256) / len2);
            row[x] = lerp_argb(a, b, t);
        }
    }
    surface_mark_dirty(s);
}

void gradient_fill_radial(struct surface *s,
                          uint32_t inner, uint32_t outer,
                          int32_t cx, int32_t cy, uint32_t radius) {
    if (!s || !s->pixels) return;
    int32_t w = (int32_t) s->width;
    int32_t h = (int32_t) s->height;

    if (radius == 0) {
        for (int32_t y = 0; y < h; y++) {
            uint32_t *row = s->pixels + (size_t) y * w;
            for (int32_t x = 0; x < w; x++) row[x] = outer;
        }
        surface_mark_dirty(s);
        return;
    }

    /* d16 = sqrt(dx²+dy²) scaled by 16 (matches shadow_corner_mask's
     * fixed point). Then t = (d16 * 256) / (radius*16) = d16*16/radius. */
    uint32_t r16 = radius * 16u;
    for (int32_t y = 0; y < h; y++) {
        uint32_t *row = s->pixels + (size_t) y * w;
        int32_t dy = y - cy;
        for (int32_t x = 0; x < w; x++) {
            int32_t dx = x - cx;
            uint32_t d2  = (uint32_t) (dx * dx + dy * dy);
            uint32_t d16 = isqrt_u32(d2 * 256u);
            int32_t t;
            if      (d16 <= 0)      t = 0;
            else if (d16 >= r16)    t = 256;
            else                    t = (int32_t) ((d16 * 256u) / r16);
            row[x] = lerp_argb(inner, outer, t);
        }
    }
    surface_mark_dirty(s);
}
