#include "blit.h"
#include "shadow.h"

#include "../arch/x86_64/simd.h"

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

    if (simd_has_avx2()) {
        for (int32_t y = 0; y < rh; y++) {
            blit_copy_row_avx2(drow, srow, rw);
            drow += dstride;
            srow += sstride;
        }
        return;
    }
    if (simd_has_sse2()) {
        for (int32_t y = 0; y < rh; y++) {
            blit_copy_row_sse2(drow, srow, rw);
            drow += dstride;
            srow += sstride;
        }
        return;
    }
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

    if (simd_has_avx2()) {
        for (int32_t y = 0; y < rh; y++) {
            blit_alpha_row_avx2(drow, srow, ga, rw);
            drow += dstride;
            srow += sstride;
        }
        return;
    }
    if (simd_has_sse2()) {
        for (int32_t y = 0; y < rh; y++) {
            blit_alpha_row_sse2(drow, srow, ga, rw);
            drow += dstride;
            srow += sstride;
        }
        return;
    }

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

/* ==========================================================================
 * Rounded + shadow variants (Faz 12.7). Scalar inner loops — the hot path
 * (middle band) still goes through the SIMD fast-paths; only the 2*cr-tall
 * top/bottom corner bands pay the per-pixel mask cost.
 * ==========================================================================
 */

/* Opaque-copy pixel modulated by an 8-bit coverage mask. mask=255 is a
 * straight copy; mask=0 keeps dst; anything in between alpha-blends src
 * over dst with the mask acting as src alpha. */
static inline void copy_masked_px(uint32_t *d, uint32_t s, uint32_t mask) {
    if (mask == 0)   return;
    if (mask == 255) { *d = s | 0xff000000u; return; }

    uint32_t inv = 255u - mask;
    uint32_t sr = (s >> 16) & 0xffu, sg = (s >> 8) & 0xffu, sb = s & 0xffu;
    uint32_t dv = *d;
    uint32_t dr = (dv >> 16) & 0xffu, dg = (dv >> 8) & 0xffu, db = dv & 0xffu;

    uint32_t r = mul8(sr, mask) + mul8(dr, inv);
    uint32_t g = mul8(sg, mask) + mul8(dg, inv);
    uint32_t b = mul8(sb, mask) + mul8(db, inv);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    *d = 0xff000000u | (r << 16) | (g << 8) | b;
}

/* Straight-alpha pixel, further modulated by a coverage mask (0..255).
 * Matches blit_alpha_scissor's math; the mask multiplies the effective
 * alpha — so mask=255 leaves behaviour unchanged. */
static inline void alpha_masked_px(uint32_t *d, uint32_t s,
                                   uint32_t ga, uint32_t mask) {
    uint32_t sa = mul8((s >> 24) & 0xffu, ga);
    sa = mul8(sa, mask);
    if (sa == 0) return;
    if (sa == 255) { *d = s | 0xff000000u; return; }

    uint32_t inv = 255u - sa;
    uint32_t sr = (s >> 16) & 0xffu, sg = (s >> 8) & 0xffu, sb = s & 0xffu;
    uint32_t dv = *d;
    uint32_t dr = (dv >> 16) & 0xffu, dg = (dv >> 8) & 0xffu, db = dv & 0xffu;

    uint32_t r = mul8(sr, sa) + mul8(dr, inv);
    uint32_t g = mul8(sg, sa) + mul8(dg, inv);
    uint32_t b = mul8(sb, sa) + mul8(db, inv);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    *d = 0xff000000u | (r << 16) | (g << 8) | b;
}

/* Choose the copy-row fast path that matches the active ISA baseline. */
static inline void copy_row_fast(uint32_t *d, const uint32_t *s, int32_t n) {
    if (simd_has_avx2())      blit_copy_row_avx2(d, s, n);
    else if (simd_has_sse2()) blit_copy_row_sse2(d, s, n);
    else {
        for (int32_t x = 0; x < n; x++) d[x] = s[x] | 0xff000000u;
    }
}

static inline void alpha_row_fast(uint32_t *d, const uint32_t *s,
                                  uint32_t ga, int32_t n) {
    if (simd_has_avx2())      blit_alpha_row_avx2(d, s, ga, n);
    else if (simd_has_sse2()) blit_alpha_row_sse2(d, s, ga, n);
    else {
        for (int32_t x = 0; x < n; x++) alpha_masked_px(&d[x], s[x], ga, 255);
    }
}

void blit_copy_rounded_scissor(const struct blit_dst *dst,
                               const struct rect *scissor,
                               int32_t dx, int32_t dy,
                               const struct blit_src *src,
                               uint32_t corner_radius) {
    if (!dst || !dst->pixels || !src || !src->pixels) return;

    if (corner_radius == 0) {
        blit_copy_scissor(dst, scissor, dx, dy, src);
        return;
    }

    int32_t rw, rh, sx, sy;
    if (!clip_src(dst, scissor, src, &dx, &dy, &rw, &rh, &sx, &sy)) return;

    uint32_t dstride = dst->stride;
    uint32_t sstride = src->stride;
    int      surf_w  = (int) src->width;
    int      surf_h  = (int) src->height;
    int      cr      = (int) corner_radius;

    uint32_t *drow = dst->pixels + (uint32_t) dy * dstride + (uint32_t) dx;
    const uint32_t *srow = src->pixels + (uint32_t) sy * sstride + (uint32_t) sx;

    for (int32_t y = 0; y < rh; y++) {
        int ly = sy + y;
        int in_corner_row = (ly < cr) || (ly >= surf_h - cr);
        if (!in_corner_row) {
            copy_row_fast(drow, srow, rw);
        } else {
            for (int32_t x = 0; x < rw; x++) {
                int lx = sx + x;
                unsigned char m = shadow_corner_mask(lx, ly,
                                                     surf_w, surf_h, cr);
                copy_masked_px(&drow[x], srow[x], m);
            }
        }
        drow += dstride;
        srow += sstride;
    }
}

void blit_alpha_rounded_scissor(const struct blit_dst *dst,
                                const struct rect *scissor,
                                int32_t dx, int32_t dy,
                                const struct blit_src *src,
                                uint8_t global_alpha,
                                uint32_t corner_radius) {
    if (!dst || !dst->pixels || !src || !src->pixels) return;
    if (global_alpha == 0) return;

    if (corner_radius == 0) {
        blit_alpha_scissor(dst, scissor, dx, dy, src, global_alpha);
        return;
    }

    int32_t rw, rh, sx, sy;
    if (!clip_src(dst, scissor, src, &dx, &dy, &rw, &rh, &sx, &sy)) return;

    uint32_t dstride = dst->stride;
    uint32_t sstride = src->stride;
    int      surf_w  = (int) src->width;
    int      surf_h  = (int) src->height;
    int      cr      = (int) corner_radius;
    uint32_t ga      = global_alpha;

    uint32_t *drow = dst->pixels + (uint32_t) dy * dstride + (uint32_t) dx;
    const uint32_t *srow = src->pixels + (uint32_t) sy * sstride + (uint32_t) sx;

    for (int32_t y = 0; y < rh; y++) {
        int ly = sy + y;
        int in_corner_row = (ly < cr) || (ly >= surf_h - cr);
        if (!in_corner_row) {
            alpha_row_fast(drow, srow, ga, rw);
        } else {
            for (int32_t x = 0; x < rw; x++) {
                int lx = sx + x;
                unsigned char m = shadow_corner_mask(lx, ly,
                                                     surf_w, surf_h, cr);
                alpha_masked_px(&drow[x], srow[x], ga, m);
            }
        }
        drow += dstride;
        srow += sstride;
    }
}

void blit_shadow_scissor(const struct blit_dst *dst,
                         const struct rect *scissor,
                         int32_t dx, int32_t dy,
                         const uint8_t *mask_pixels,
                         uint32_t mask_w, uint32_t mask_h,
                         uint32_t color, uint8_t global_alpha) {
    if (!dst || !dst->pixels || !mask_pixels) return;
    if (global_alpha == 0 || mask_w == 0 || mask_h == 0) return;

    int32_t rw = (int32_t) mask_w;
    int32_t rh = (int32_t) mask_h;
    int32_t mx = 0, my = 0;
    if (!clip_rect(dst, scissor, &dx, &dy, &rw, &rh, &mx, &my)) return;

    uint32_t dstride = dst->stride;
    uint32_t *drow   = dst->pixels + (uint32_t) dy * dstride + (uint32_t) dx;
    const uint8_t *mrow = mask_pixels + (size_t) my * mask_w + (size_t) mx;

    uint32_t ga = global_alpha;
    uint32_t cr = (color >> 16) & 0xffu;
    uint32_t cg = (color >>  8) & 0xffu;
    uint32_t cb =  color        & 0xffu;

    for (int32_t y = 0; y < rh; y++) {
        for (int32_t x = 0; x < rw; x++) {
            uint32_t m = mrow[x];
            if (m == 0) continue;
            uint32_t sa = mul8(m, ga);
            if (sa == 0) continue;

            uint32_t d = drow[x];
            uint32_t dr = (d >> 16) & 0xffu;
            uint32_t dg = (d >>  8) & 0xffu;
            uint32_t db =  d        & 0xffu;

            uint32_t inv = 255u - sa;
            uint32_t r = mul8(cr, sa) + mul8(dr, inv);
            uint32_t g = mul8(cg, sa) + mul8(dg, inv);
            uint32_t b = mul8(cb, sa) + mul8(db, inv);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;

            drow[x] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
        drow += dstride;
        mrow += mask_w;
    }
}
