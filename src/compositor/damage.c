#include "damage.h"

#include <stdint.h>

void damage_reset(struct damage *d) {
    if (!d) return;
    d->count = 0;
}

/* Collapse the list to a single enclosing rect. Used as the overflow
 * fallback so damage_add() can never run out of slots. */
static void damage_collapse(struct damage *d) {
    if (d->count <= 1) return;
    struct rect u = d->rects[0];
    for (int i = 1; i < d->count; i++) u = rect_union(&u, &d->rects[i]);
    d->rects[0] = u;
    d->count    = 1;
}

void damage_add(struct damage *d, struct rect r) {
    if (!d || rect_empty(&r)) return;

    /* Greedy merge: if the new rect touches any existing rect, union them
     * and restart (the merged rect may now overlap others). This keeps
     * the list disjoint in the common case. */
    int merged = 1;
    while (merged) {
        merged = 0;
        for (int i = 0; i < d->count; i++) {
            if (rect_overlaps(&d->rects[i], &r)) {
                r = rect_union(&d->rects[i], &r);
                d->rects[i] = d->rects[d->count - 1];
                d->count--;
                merged = 1;
                break;
            }
        }
    }

    if (d->count >= DAMAGE_MAX_RECTS) {
        /* Out of slots. Merge everything (including r) into a single
         * bounding box. Correct, just over-clears a bit. */
        d->rects[0] = r;
        for (int i = 1; i < d->count; i++) {
            d->rects[0] = rect_union(&d->rects[0], &d->rects[i]);
        }
        d->count = 1;
        damage_collapse(d);
        return;
    }

    d->rects[d->count++] = r;
}

void damage_clip(struct damage *d, struct rect bounds) {
    if (!d) return;
    int w = 0;
    for (int i = 0; i < d->count; i++) {
        struct rect out;
        if (rect_intersect(&d->rects[i], &bounds, &out)) {
            d->rects[w++] = out;
        }
    }
    d->count = w;
}

struct rect damage_bbox(const struct damage *d) {
    if (!d || d->count == 0) return rect_make(0, 0, 0, 0);
    struct rect u = d->rects[0];
    for (int i = 1; i < d->count; i++) u = rect_union(&u, &d->rects[i]);
    return u;
}

uint64_t damage_area_sum(const struct damage *d) {
    if (!d) return 0;
    uint64_t s = 0;
    for (int i = 0; i < d->count; i++) {
        s += (uint64_t) d->rects[i].w * (uint64_t) d->rects[i].h;
    }
    return s;
}
