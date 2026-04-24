/* 2D path rasterizer: curve flattening + non-zero winding fill + 4× SSAA.
 * Stroke builds quads along flattened segments. Integer math in 1/16 px. */

#include "path.h"
#include "path_priv.h"

#include "surface.h"

#include "../mm/heap.h"

#include <stddef.h>
#include <stdint.h>

#define UNIT 16

struct flat_ctx {
    int32_t *vx;
    int32_t *vy;
    int      nv;
    int      cap;
};

static int flat_ensure(struct flat_ctx *f, int need) {
    if (f->nv + need <= f->cap) return 0;
    return -1;
}

static void flat_emit(struct flat_ctx *f, int32_t x, int32_t y) {
    if (f->nv >= f->cap) return;
    f->vx[f->nv]   = x;
    f->vy[f->nv++] = y;
}

static int32_t isqrt64(int64_t n) {
    if (n <= 0) return 0;
    int64_t r = n;
    while (1) {
        int64_t nr = (r + n / r) >> 1;
        if (nr >= r) return (int32_t) r;
        r = nr;
    }
}

static int cubic_flat(struct flat_ctx *f, int32_t x0, int32_t y0,
                      int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      int32_t x3, int32_t y3, int depth) {
    if (depth > 14 || flat_ensure(f, 1)) {
        flat_emit(f, x3, y3);
        return 0;
    }
    int32_t dx = x3 - x0, dy = y3 - y0;
    int64_t L2 = (int64_t) dx * dx + (int64_t) dy * dy;
    if (L2 < (int64_t) UNIT * UNIT) {
        flat_emit(f, x3, y3);
        return 0;
    }
    int64_t c1 = (int64_t) (x1 - x0) * dy - (int64_t) (y1 - y0) * dx;
    int64_t c2 = (int64_t) (x2 - x0) * dy - (int64_t) (y2 - y0) * dx;
    int64_t lim = (int64_t) 36 * L2;
    if (c1 * c1 <= lim && c2 * c2 <= lim) {
        flat_emit(f, x3, y3);
        return 0;
    }

    int32_t x01 = (x0 + x1) / 2, y01 = (y0 + y1) / 2;
    int32_t x12 = (x1 + x2) / 2, y12 = (y1 + y2) / 2;
    int32_t x23 = (x2 + x3) / 2, y23 = (y2 + y3) / 2;
    int32_t x012 = (x01 + x12) / 2, y012 = (y01 + y12) / 2;
    int32_t x123 = (x12 + x23) / 2, y123 = (y12 + y23) / 2;
    int32_t x0123 = (x012 + x123) / 2, y0123 = (y012 + y123) / 2;

    if (cubic_flat(f, x0, y0, x01, y01, x012, y012, x0123, y0123, depth + 1)) return -1;
    if (cubic_flat(f, x0123, y0123, x123, y123, x23, y23, x3, y3, depth + 1)) return -1;
    return 0;
}

static int quad_flat(struct flat_ctx *f, int32_t x0, int32_t y0,
                     int32_t x1, int32_t y1, int32_t x2, int32_t y2, int depth) {
    if (depth > 12 || flat_ensure(f, 1)) {
        flat_emit(f, x2, y2);
        return 0;
    }
    int32_t dx = x2 - x0, dy = y2 - y0;
    int64_t L2 = (int64_t) dx * dx + (int64_t) dy * dy;
    if (L2 < (int64_t) UNIT * UNIT) {
        flat_emit(f, x2, y2);
        return 0;
    }
    int64_t c = (int64_t) (x1 - x0) * dy - (int64_t) (y1 - y0) * dx;
    int64_t lim = (int64_t) 36 * L2;
    if (c * c <= lim) {
        flat_emit(f, x2, y2);
        return 0;
    }

    int32_t q01x = (x0 + x1) / 2, q01y = (y0 + y1) / 2;
    int32_t q12x = (x1 + x2) / 2, q12y = (y1 + y2) / 2;
    int32_t mx = (q01x + q12x) / 2, my = (q01y + q12y) / 2;

    if (quad_flat(f, x0, y0, q01x, q01y, mx, my, depth + 1)) return -1;
    if (quad_flat(f, mx, my, q12x, q12y, x2, y2, depth + 1)) return -1;
    return 0;
}

/* Contour boundaries in vx indices: cstart[k] .. cend[k]-1 inclusive */
static int build_contours_fill(const struct path *p, struct flat_ctx *f,
                               int *cstart, int *cend, int *ncont) {
    int32_t penx = 0, peny = 0, sub0x = 0, sub0y = 0;
    int     has_sub = 0;
    int     cur_start = 0;
    int     nc = 0;

    f->nv = 0;
    *ncont = 0;

    for (size_t i = 0; i < p->ncmds; i++) {
        const struct path_cmd *c = &p->cmds[i];
        switch (c->type) {
        case PATH_CMD_MOVE:
            if (has_sub && f->nv > cur_start) {
                if (f->nv - cur_start < 3) return -1;
                if (nc >= PATH_MAX_CONTOUR) return -1;
                cstart[nc] = cur_start;
                cend[nc]   = f->nv;
                nc++;
            }
            cur_start = f->nv;
            penx = c->x0 * UNIT;
            peny = c->y0 * UNIT;
            sub0x = penx;
            sub0y = peny;
            has_sub = 1;
            flat_emit(f, penx, peny);
            break;
        case PATH_CMD_LINE:
            penx = c->x0 * UNIT;
            peny = c->y0 * UNIT;
            flat_emit(f, penx, peny);
            break;
        case PATH_CMD_QUAD:
            if (quad_flat(f, penx, peny, c->x0 * UNIT, c->y0 * UNIT,
                           c->x1 * UNIT, c->y1 * UNIT, 0))
                return -1;
            penx = c->x1 * UNIT;
            peny = c->y1 * UNIT;
            break;
        case PATH_CMD_CUBIC:
            if (cubic_flat(f, penx, peny, c->x0 * UNIT, c->y0 * UNIT,
                           c->x1 * UNIT, c->y1 * UNIT, c->x2 * UNIT, c->y2 * UNIT, 0))
                return -1;
            penx = c->x2 * UNIT;
            peny = c->y2 * UNIT;
            break;
        case PATH_CMD_CLOSE:
            if (has_sub && (penx != sub0x || peny != sub0y)) {
                if (f->nv == 0 || f->vx[f->nv - 1] != sub0x || f->vy[f->nv - 1] != sub0y)
                    flat_emit(f, sub0x, sub0y);
            }
            penx = sub0x;
            peny = sub0y;
            break;
        default:
            break;
        }
    }
    if (has_sub && f->nv - cur_start >= 3) {
        if (nc >= PATH_MAX_CONTOUR) return -1;
        cstart[nc] = cur_start;
        cend[nc]   = f->nv;
        nc++;
    }
    *ncont = nc;
    return nc > 0 ? 0 : -1;
}

static int winding_at(int64_t px, int64_t py,
                      const int32_t *vx, const int32_t *vy, int n) {
    int w = 0;
    if (n < 2) return 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        int64_t x0 = vx[i], y0 = vy[i];
        int64_t x1 = vx[j], y1 = vy[j];
        int64_t den = y1 - y0;
        if (den == 0) continue;
        if ((y0 > py) == (y1 > py)) continue;
        int64_t num = x0 * (y1 - y0) + (x1 - x0) * (py - y0);
        if (den < 0) {
            den = -den;
            num = -num;
        }
        if (num > px * den) w += (y1 > y0) ? 1 : -1;
    }
    return w;
}

static uint32_t mul8(uint32_t a, uint32_t b) {
    uint32_t t = a * b + 128u;
    return (t + (t >> 8)) >> 8;
}

static void blend_over_px(uint32_t *d, uint32_t sargb, uint8_t sa) {
    if (sa == 0) return;
    uint32_t sr = (sargb >> 16) & 0xffu, sg = (sargb >> 8) & 0xffu, sb = sargb & 0xffu;
    uint32_t dv = *d;
    uint32_t da = (dv >> 24) & 0xffu;
    uint32_t dr = (dv >> 16) & 0xffu, dg = (dv >> 8) & 0xffu, db = dv & 0xffu;
    if (da == 0 && sa == 255) {
        *d = (uint32_t) sa << 24 | (sr << 16) | (sg << 8) | sb;
        return;
    }
    uint32_t inv = 255u - sa;
    uint32_t r = mul8(sr, sa) + mul8(dr, inv);
    uint32_t g = mul8(sg, sa) + mul8(dg, inv);
    uint32_t b = mul8(sb, sa) + mul8(db, inv);
    uint32_t outa = sa + mul8(da, inv);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (outa > 255) outa = 255;
    *d = (outa << 24) | (r << 16) | (g << 8) | b;
}

static int fill_winding_ssaa(struct surface *surf,
                             const int32_t *vx, const int32_t *vy,
                             const int *cstart, const int *cend, int ncont,
                             int32_t minx, int32_t miny, int32_t maxx, int32_t maxy,
                             uint32_t argb) {
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= (int32_t) surf->width)  maxx = (int32_t) surf->width - 1;
    if (maxy >= (int32_t) surf->height) maxy = (int32_t) surf->height - 1;
    if (maxx < minx || maxy < miny) return 0;

    for (int32_t oy = miny; oy <= maxy; oy++) {
        for (int32_t ox = minx; ox <= maxx; ox++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    int64_t Px = (int64_t) ox * UNIT + (int64_t) sx * 4 + 2;
                    int64_t Py = (int64_t) oy * UNIT + (int64_t) sy * 4 + 2;
                    int wsum = 0;
                    for (int k = 0; k < ncont; k++) {
                        int cn = cend[k] - cstart[k];
                        wsum += winding_at(Px, Py, vx + cstart[k], vy + cstart[k], cn);
                    }
                    if (wsum != 0) hits++;
                }
            }
            if (hits == 0) continue;
            uint8_t alpha = (uint32_t) ((hits * 255 + 8) / 16);
            uint32_t *px = surf->pixels + (uint32_t) oy * surf->width + (uint32_t) ox;
            blend_over_px(px, (argb & 0x00ffffffu) | (0xffu << 24), alpha);
        }
    }
    return 0;
}

static void bbox_contours(const int32_t *vx, const int32_t *vy,
                          const int *cstart, const int *cend, int ncont,
                          int32_t *minx, int32_t *miny, int32_t *maxx, int32_t *maxy) {
    int32_t mnX = 0x7fffffff, mnY = 0x7fffffff;
    int32_t mxX = -0x7fffffff - 1, mxY = -0x7fffffff - 1;
    int any = 0;
    for (int k = 0; k < ncont; k++) {
        for (int i = cstart[k]; i < cend[k]; i++) {
            int32_t x = vx[i] / UNIT;
            int32_t y = vy[i] / UNIT;
            any = 1;
            if (x < mnX) mnX = x;
            if (y < mnY) mnY = y;
            if (x > mxX) mxX = x;
            if (y > mxY) mxY = y;
        }
    }
    if (!any) {
        *minx = *miny = *maxx = *maxy = 0;
        return;
    }
    *minx = mnX - 2;
    *miny = mnY - 2;
    *maxx = mxX + 2;
    *maxy = mxY + 2;
}

int path_fill_surface(struct surface *s, path_t *p, uint32_t argb) {
    if (!s || !p || !s->pixels) return -1;

    int32_t *vx = (int32_t *) kmalloc(sizeof(int32_t) * PATH_MAX_VERT * 2);
    if (!vx) return -1;
    int32_t *vy = vx + PATH_MAX_VERT;

    struct flat_ctx fc = { .vx = vx, .vy = vy, .nv = 0, .cap = PATH_MAX_VERT };

    int cstart[PATH_MAX_CONTOUR], cend[PATH_MAX_CONTOUR], ncont = 0;
    if (build_contours_fill(p, &fc, cstart, cend, &ncont) != 0) {
        kfree(vx);
        return -1;
    }

    int32_t minx, miny, maxx, maxy;
    bbox_contours(vx, vy, cstart, cend, ncont, &minx, &miny, &maxx, &maxy);
    fill_winding_ssaa(s, vx, vy, cstart, cend, ncont, minx, miny, maxx, maxy, argb);

    kfree(vx);
    surface_mark_dirty(s);
    return 0;
}

/* ---- stroke: flatten open polyline, quads per segment ---------------- */

/* Returns number of subpath start indices written to br (capacity 64). */
static int build_open_polyline(const struct path *p, struct flat_ctx *f,
                               int *br, int max_br) {
    int32_t penx = 0, peny = 0, sub0x = 0, sub0y = 0;
    int     has_sub = 0;
    int     moved = 0;
    int     nb = 1;

    if (max_br < 1) return -1;
    br[0] = 0;
    f->nv = 0;
    for (size_t i = 0; i < p->ncmds; i++) {
        const struct path_cmd *c = &p->cmds[i];
        switch (c->type) {
        case PATH_CMD_MOVE:
            if (moved && f->nv > 0) {
                if (nb >= max_br) return -1;
                br[nb++] = f->nv;
            }
            moved = 1;
            penx = c->x0 * UNIT;
            peny = c->y0 * UNIT;
            sub0x = penx;
            sub0y = peny;
            has_sub = 1;
            flat_emit(f, penx, peny);
            break;
        case PATH_CMD_LINE:
            penx = c->x0 * UNIT;
            peny = c->y0 * UNIT;
            flat_emit(f, penx, peny);
            break;
        case PATH_CMD_QUAD:
            if (quad_flat(f, penx, peny, c->x0 * UNIT, c->y0 * UNIT,
                           c->x1 * UNIT, c->y1 * UNIT, 0))
                return -1;
            penx = c->x1 * UNIT;
            peny = c->y1 * UNIT;
            break;
        case PATH_CMD_CUBIC:
            if (cubic_flat(f, penx, peny, c->x0 * UNIT, c->y0 * UNIT,
                           c->x1 * UNIT, c->y1 * UNIT, c->x2 * UNIT, c->y2 * UNIT, 0))
                return -1;
            penx = c->x2 * UNIT;
            peny = c->y2 * UNIT;
            break;
        case PATH_CMD_CLOSE:
            if (has_sub && (penx != sub0x || peny != sub0y)) {
                if (f->nv == 0 || f->vx[f->nv - 1] != sub0x || f->vy[f->nv - 1] != sub0y)
                    flat_emit(f, sub0x, sub0y);
            }
            penx = sub0x;
            peny = sub0y;
            break;
        default:
            break;
        }
    }
    return (f->nv >= 2) ? nb : -1;
}

static int fill_quad(struct surface *s, int32_t x0, int32_t y0,
                     int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                     int32_t x3, int32_t y3, uint32_t argb) {
    int32_t vx[4] = { x0, x1, x2, x3 };
    int32_t vy[4] = { y0, y1, y2, y3 };
    int cs[1] = { 0 };
    int ce[1] = { 4 };
    int32_t minx = vx[0] / UNIT, miny = vy[0] / UNIT;
    int32_t maxx = minx, maxy = miny;
    for (int i = 1; i < 4; i++) {
        int32_t x = vx[i] / UNIT, y = vy[i] / UNIT;
        if (x < minx) minx = x;
        if (y < miny) miny = y;
        if (x > maxx) maxx = x;
        if (y > maxy) maxy = y;
    }
    minx -= 2;
    miny -= 2;
    maxx += 2;
    maxy += 2;
    return fill_winding_ssaa(s, vx, vy, cs, ce, 1, minx, miny, maxx, maxy, argb);
}

int path_stroke_surface(struct surface *s, path_t *p, int32_t width_px,
                        uint32_t argb) {
    if (!s || !p || !s->pixels || width_px < 1) return -1;

    int32_t *vx = (int32_t *) kmalloc(sizeof(int32_t) * PATH_MAX_VERT * 2);
    if (!vx) return -1;
    int32_t *vy = vx + PATH_MAX_VERT;
    struct flat_ctx fc = { .vx = vx, .vy = vy, .nv = 0, .cap = PATH_MAX_VERT };

    int br[64];
    int nb = build_open_polyline(p, &fc, br, 64);
    if (nb < 0) {
        kfree(vx);
        return -1;
    }

    int32_t half = (width_px * UNIT) / 2;
    if (half < UNIT / 4) half = UNIT / 4;

    for (int k = 0; k < nb; k++) {
        int seg0 = br[k];
        int seg1 = (k + 1 < nb) ? br[k + 1] : fc.nv;
        if (seg1 - seg0 < 2) continue;
        for (int i = seg0; i + 1 < seg1; i++) {
            int32_t x0 = fc.vx[i], y0 = fc.vy[i];
            int32_t x1 = fc.vx[i + 1], y1 = fc.vy[i + 1];
            int32_t dx = x1 - x0, dy = y1 - y0;
            int64_t L2 = (int64_t) dx * dx + (int64_t) dy * dy;
            if (L2 == 0) continue;
            int32_t ilen = isqrt64(L2);
            if (ilen < 1) ilen = 1;
            int32_t px = (int32_t) (((int64_t) (-dy) * half) / ilen);
            int32_t py = (int32_t) (((int64_t) (dx)*half) / ilen);

            int32_t ax = x0 + px, ay = y0 + py;
            int32_t bx = x0 - px, by = y0 - py;
            int32_t cx = x1 - px, cy = y1 - py;
            int32_t ex = x1 + px, ey = y1 + py;
            fill_quad(s, ax, ay, bx, by, cx, cy, ex, ey, argb);
        }
    }

    kfree(vx);
    surface_mark_dirty(s);
    return 0;
}
