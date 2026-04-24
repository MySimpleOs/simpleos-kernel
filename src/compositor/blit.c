#include "blit.h"

#include <stdint.h>
#include <stddef.h>

/* Effective destination clip = dst bounds ∩ scissor (if given). Returns
 * the clip as (cx0, cy0, cx1, cy1). The primitives then intersect their
 * own rect against it before walking pixels. */
static int effective_clip(const struct blit_dst *dst, const struct rect *scissor,
                          int32_t *cx0, int32_t *cy0,
                          int32_t *cx1, int32_t *cy1) {
    int32_t x0 = 0, y0 = 0;
    int32_t x1 = (int32_t) dst->width;
    int32_t y1 = (int32_t) dst->height;
    if (scissor) {
        if (scissor->x > x0) x0 = scissor->x;
        if (scissor->y > y0) y0 = scissor->y;
        if (scissor->x + scissor->w < x1) x1 = scissor->x + scissor->w;
        if (scissor->y + scissor->h < y1) y1 = scissor->y + scissor->h;
    }
    if (x1 <= x0 || y1 <= y0) return 0;
    *cx0 = x0; *cy0 = y0; *cx1 = x1; *cy1 = y1;
    return 1;
}

/* Clip (rx, ry, rw, rh) against the effective clip and adjust src offsets
 * so source sampling stays in sync. Returns 1 if any pixels survive. */
static int clip_rect(const struct blit_dst *dst, const struct rect *scissor,
                     int32_t *rx, int32_t *ry,
                     int32_t *rw, int32_t *rh,
                     int32_t *sx, int32_t *sy) {
    int32_t cx0, cy0, cx1, cy1;
    if (!effective_clip(dst, scissor, &cx0, &cy0, &cx1, &cy1)) return 0;

    int32_t x0 = *rx, y0 = *ry, w = *rw, h = *rh;
    int32_t sox = sx ? *sx : 0;
    int32_t soy = sy ? *sy : 0;

    if (w <= 0 || h <= 0) return 0;

    if (x0 < cx0) { sox += cx0 - x0; w -= cx0 - x0; x0 = cx0; }
    if (y0 < cy0) { soy += cy0 - y0; h -= cy0 - y0; y0 = cy0; }

    if (x0 >= cx1 || y0 >= cy1) return 0;

    if (x0 + w > cx1) w = cx1 - x0;
    if (y0 + h > cy1) h = cy1 - y0;

    if (w <= 0 || h <= 0) return 0;

    *rx = x0; *ry = y0; *rw = w; *rh = h;
    if (sx) *sx = sox;
    if (sy) *sy = soy;
    return 1;
}

void blit_fill(const struct blit_dst *dst,
               int32_t rx, int32_t ry, int32_t rw, int32_t rh,
               uint32_t color) {
    blit_fill_scissor(dst, NULL, rx, ry, rw, rh, color);
}

void blit_fill_scissor(const struct blit_dst *dst, const struct rect *scissor,
                       int32_t rx, int32_t ry, int32_t rw, int32_t rh,
                       uint32_t color) {
    if (!dst || !dst->pixels) return;
    if (!clip_rect(dst, scissor, &rx, &ry, &rw, &rh, NULL, NULL)) return;

    uint32_t stride = dst->stride;
    uint32_t *row = dst->pixels + (uint32_t) ry * stride + (uint32_t) rx;
    for (int32_t y = 0; y < rh; y++) {
        for (int32_t x = 0; x < rw; x++) row[x] = color;
        row += stride;
    }
}

static int clip_src(const struct blit_dst *dst, const struct rect *scissor,
                    const struct blit_src *src,
                    int32_t *dx, int32_t *dy,
                    int32_t *rw, int32_t *rh,
                    int32_t *sx, int32_t *sy) {
    *rw = (int32_t) src->width;
    *rh = (int32_t) src->height;
    *sx = 0; *sy = 0;
    return clip_rect(dst, scissor, dx, dy, rw, rh, sx, sy);
}

void blit_copy(const struct blit_dst *dst, int32_t dx, int32_t dy,
               const struct blit_src *src) {
    blit_copy_scissor(dst, NULL, dx, dy, src);
}

void blit_copy_scissor(const struct blit_dst *dst, const struct rect *scissor,
                       int32_t dx, int32_t dy, const struct blit_src *src) {
    if (!dst || !dst->pixels || !src || !src->pixels) return;

    int32_t rw, rh, sx, sy;
    if (!clip_src(dst, scissor, src, &dx, &dy, &rw, &rh, &sx, &sy)) return;

    uint32_t dstride = dst->stride;
    uint32_t sstride = src->stride;
    uint32_t *drow = dst->pixels + (uint32_t) dy * dstride + (uint32_t) dx;
    const uint32_t *srow = src->pixels + (uint32_t) sy * sstride + (uint32_t) sx;

    for (int32_t y = 0; y < rh; y++) {
        for (int32_t x = 0; x < rw; x++) drow[x] = srow[x] | 0xff000000u;
        drow += dstride;
        srow += sstride;
    }
}

/* 8-bit saturating multiply: (a * b + 127) / 255 implemented without div. */
static inline uint32_t mul8(uint32_t a, uint32_t b) {
    uint32_t t = a * b + 128u;
    return (t + (t >> 8)) >> 8;
}

void blit_alpha(const struct blit_dst *dst, int32_t dx, int32_t dy,
                const struct blit_src *src, uint8_t global_alpha) {
    blit_alpha_scissor(dst, NULL, dx, dy, src, global_alpha);
}

void blit_alpha_scissor(const struct blit_dst *dst, const struct rect *scissor,
                        int32_t dx, int32_t dy,
                        const struct blit_src *src, uint8_t global_alpha) {
    if (!dst || !dst->pixels || !src || !src->pixels) return;
    if (global_alpha == 0) return;

    int32_t rw, rh, sx, sy;
    if (!clip_src(dst, scissor, src, &dx, &dy, &rw, &rh, &sx, &sy)) return;

    uint32_t dstride = dst->stride;
    uint32_t sstride = src->stride;
    uint32_t *drow = dst->pixels + (uint32_t) dy * dstride + (uint32_t) dx;
    const uint32_t *srow = src->pixels + (uint32_t) sy * sstride + (uint32_t) sx;
    uint32_t ga = global_alpha;

    for (int32_t y = 0; y < rh; y++) {
        for (int32_t x = 0; x < rw; x++) {
            uint32_t s  = srow[x];
            uint32_t sa = mul8((s >> 24) & 0xffu, ga);
            if (sa == 0) continue;
            if (sa == 255) { drow[x] = s | 0xff000000u; continue; }

            uint32_t d = drow[x];
            uint32_t inv = 255u - sa;

            uint32_t sr = (s >> 16) & 0xffu;
            uint32_t sg = (s >>  8) & 0xffu;
            uint32_t sb =  s        & 0xffu;
            uint32_t dr = (d >> 16) & 0xffu;
            uint32_t dg = (d >>  8) & 0xffu;
            uint32_t db =  d        & 0xffu;

            uint32_t r = mul8(sr, sa) + mul8(dr, inv);
            uint32_t g = mul8(sg, sa) + mul8(dg, inv);
            uint32_t b = mul8(sb, sa) + mul8(db, inv);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;

            drow[x] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
        drow += dstride;
        srow += sstride;
    }
}
