#pragma once

#include <stdint.h>

/* Axis-aligned screen-space rectangle. (x, y) top-left, (w, h) size in
 * pixels. Negative w/h means "empty". Used by the damage tracker, scissor
 * blit, and rect-aware present path. */
struct rect {
    int32_t x, y, w, h;
};

static inline int rect_empty(const struct rect *r) {
    return !r || r->w <= 0 || r->h <= 0;
}

static inline struct rect rect_make(int32_t x, int32_t y, int32_t w, int32_t h) {
    struct rect r = { x, y, w, h };
    return r;
}

/* Clip `r` against `c`. Returns 1 if a non-empty intersection survives. */
static inline int rect_intersect(const struct rect *r, const struct rect *c,
                                 struct rect *out) {
    int32_t x0 = r->x > c->x ? r->x : c->x;
    int32_t y0 = r->y > c->y ? r->y : c->y;
    int32_t x1 = (r->x + r->w) < (c->x + c->w) ? r->x + r->w : c->x + c->w;
    int32_t y1 = (r->y + r->h) < (c->y + c->h) ? r->y + r->h : c->y + c->h;
    if (x1 <= x0 || y1 <= y0) {
        if (out) { out->x = out->y = 0; out->w = out->h = 0; }
        return 0;
    }
    if (out) { out->x = x0; out->y = y0; out->w = x1 - x0; out->h = y1 - y0; }
    return 1;
}

/* Union (minimal enclosing rect) — always non-empty if a or b is. */
static inline struct rect rect_union(const struct rect *a, const struct rect *b) {
    if (rect_empty(a)) return *b;
    if (rect_empty(b)) return *a;
    int32_t x0 = a->x < b->x ? a->x : b->x;
    int32_t y0 = a->y < b->y ? a->y : b->y;
    int32_t x1 = (a->x + a->w) > (b->x + b->w) ? a->x + a->w : b->x + b->w;
    int32_t y1 = (a->y + a->h) > (b->y + b->h) ? a->y + a->h : b->y + b->h;
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

/* True when `a` and `b` share any interior pixel. */
static inline int rect_overlaps(const struct rect *a, const struct rect *b) {
    return !(a->x + a->w <= b->x || b->x + b->w <= a->x ||
             a->y + a->h <= b->y || b->y + b->h <= a->y);
}
