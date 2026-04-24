#include "cursor.h"
#include "compositor.h"
#include "surface.h"

#include "../drivers/mouse.h"
#include "../kprintf.h"

#include <stdint.h>

/* 32×32 logical arrow @ 2× supersampling (64×64 tests), soft edge + cast
 * shadow. Hotspot at the tip (CUR_HOT_X, CUR_HOT_Y) so screen (mx,my)
 * matches the pointed pixel after offset. */

#define CUR_SZ      36
#define CUR_HOT_X   3
#define CUR_HOT_Y   3
static int64_t cross2(int ax, int ay, int bx, int by) {
    return (int64_t) ax * (int64_t) by - (int64_t) ay * (int64_t) bx;
}

static int in_tri(int px, int py, int x0, int y0, int x1, int y1, int x2, int y2) {
    int64_t d1 = cross2(x1 - x0, y1 - y0, px - x0, py - y0);
    int64_t d2 = cross2(x2 - x1, y2 - y1, px - x1, py - y1);
    int64_t d3 = cross2(x0 - x2, y0 - y2, px - x2, py - y2);
    int neg = (d1 < 0) | (d2 < 0) | (d3 < 0);
    int pos = (d1 > 0) | (d2 > 0) | (d3 > 0);
    return !(neg && pos);
}

static int in_rect(int px, int py, int x0, int y0, int x1, int y1) {
    return px >= x0 && px <= x1 && py >= y0 && py <= y1;
}

/* Geometry in supersampled space (64×64): tip (12,12), wide head, stem. */
static int arrow_cover_sup(int sx, int sy) {
    /* Triangle head */
    if (in_tri(sx, sy, 12, 12, 46, 36, 12, 58))
        return 1;
    /* Stem */
    if (in_rect(sx, sy, 12, 36, 22, 62))
        return 1;
    return 0;
}

static uint8_t body_alpha_at(int ix, int iy) {
    int c = 0;
    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            int sx = ix * 2 + dx;
            int sy = iy * 2 + dy;
            if (arrow_cover_sup(sx, sy)) c++;
        }
    }
    return (uint8_t) ((c * 255 + 2) / 4);
}

static uint32_t mul8(uint32_t a, uint32_t b) {
    uint32_t t = a * b + 128u;
    return (t + (t >> 8)) >> 8;
}

static uint32_t blend_over(uint32_t dst, uint32_t src) {
    uint32_t sa = (src >> 24) & 0xffu;
    if (sa == 0) return dst;
    uint32_t da = (dst >> 24) & 0xffu;
    if (da == 0) return src;
    if (sa == 255) return src | 0xff000000u;
    uint32_t inv = 255u - sa;
    uint32_t sr = (src >> 16) & 0xffu, sg = (src >> 8) & 0xffu, sb = src & 0xffu;
    uint32_t dr = (dst >> 16) & 0xffu, dg = (dst >> 8) & 0xffu, db = dst & 0xffu;
    uint32_t r = mul8(sr, sa) + mul8(dr, inv);
    uint32_t g = mul8(sg, sa) + mul8(dg, inv);
    uint32_t b = mul8(sb, sa) + mul8(db, inv);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

static struct surface *cursor_surf;

void cursor_init(void) {
    cursor_surf = surface_create("cursor", CUR_SZ, CUR_SZ);
    if (!cursor_surf) {
        kprintf("[cursor] surface_create failed\n");
        return;
    }

    uint8_t ba[CUR_SZ * CUR_SZ];
    for (int y = 0; y < CUR_SZ; y++) {
        for (int x = 0; x < CUR_SZ; x++) {
            if (x < CUR_HOT_X || y < CUR_HOT_Y) {
                ba[y * CUR_SZ + x] = 0;
                continue;
            }
            int ix = x - CUR_HOT_X;
            int iy = y - CUR_HOT_Y;
            if (ix >= 32 || iy >= 32) {
                ba[y * CUR_SZ + x] = 0;
                continue;
            }
            ba[y * CUR_SZ + x] = body_alpha_at(ix, iy);
        }
    }

    for (int y = 0; y < CUR_SZ; y++) {
        for (int x = 0; x < CUR_SZ; x++) {
            uint8_t a = ba[y * CUR_SZ + x];
            /* Edge darkening when partially covered */
            uint32_t rgb = 0xffffffu;
            if (a > 0 && a < 255) {
                uint32_t edge = (255u - (uint32_t) a) / 6u;
                uint32_t v = 255u - edge;
                if (v < 0x50u) v = 0x50u;
                rgb = (v << 16) | (v << 8) | v;
            } else if (a == 255) {
                rgb = 0xf8f8f8u;
            }

            uint32_t body = ((uint32_t) a << 24) | (rgb & 0x00ffffffu);

            /* Soft drop shadow from offset body alpha */
            int sx = x - 2, sy = y - 2;
            uint8_t sh = 0;
            if (sx >= 0 && sy >= 0 && sx < CUR_SZ && sy < CUR_SZ)
                sh = (uint8_t) ((uint32_t) ba[sy * CUR_SZ + sx] * 90u / 255u);

            uint32_t sh_px = ((uint32_t) sh << 24) | 0x000000u;
            uint32_t out = blend_over(0x00000000u, sh_px);
            out = blend_over(out, body);
            cursor_surf->pixels[y * CUR_SZ + x] = out;
        }
    }

    cursor_surf->z        = 2000000000;
    cursor_surf->alpha    = 255;
    cursor_surf->visible  = 1;
    cursor_surf->opaque   = 0;

    compositor_add(cursor_surf);
    cursor_tick();
    kprintf("[cursor] %ux%u ARGB (2× AA + shadow), hotspot (%d,%d)\n",
            CUR_SZ, CUR_SZ, CUR_HOT_X, CUR_HOT_Y);
}

void cursor_tick(void) {
    if (!cursor_surf) return;
    int32_t mx, my;
    mouse_get_state(&mx, &my, NULL);
    cursor_surf->x = mx - CUR_HOT_X;
    cursor_surf->y = my - CUR_HOT_Y;
}
