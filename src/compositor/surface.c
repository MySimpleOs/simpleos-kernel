#include "surface.h"

#include "../mm/heap.h"

#include <stddef.h>
#include <stdint.h>

static void name_copy(char dst[SURFACE_NAME_LEN], const char *src) {
    size_t i = 0;
    if (src) {
        for (; i < SURFACE_NAME_LEN - 1 && src[i]; i++) dst[i] = src[i];
    }
    for (; i < SURFACE_NAME_LEN; i++) dst[i] = 0;
}

struct surface *surface_create(const char *name, uint32_t w, uint32_t h) {
    if (!w || !h || w > 16384 || h > 16384) return NULL;

    struct surface *s = (struct surface *) kmalloc(sizeof(*s));
    if (!s) return NULL;

    size_t bytes = (size_t) w * (size_t) h * 4u;
    uint32_t *px = (uint32_t *) kmalloc(bytes);
    if (!px) { kfree(s); return NULL; }

    for (size_t i = 0; i < (size_t) w * h; i++) px[i] = 0u;

    name_copy(s->name, name);
    s->pixels       = px;
    s->width        = w;
    s->height       = h;
    s->x            = 0;
    s->y            = 0;
    s->z            = 0;
    s->alpha        = 255;
    s->visible      = 1;
    s->opaque       = 0;
    s->pixels_dirty = 1;       /* fresh buffer counts as "content changed"  */
    s->dirty_rect_valid = 0;
    s->dirty_lx0 = s->dirty_ly0 = s->dirty_lx1 = s->dirty_ly1 = 0;

    s->corner_radius = 0;

    s->prev_x       = 0;
    s->prev_y       = 0;
    s->prev_w       = 0;
    s->prev_h       = 0;
    s->prev_z       = 0;
    s->prev_alpha   = 0;
    s->prev_visible = 0;
    s->prev_opaque  = 0;
    s->prev_known   = 0;       /* first frame: full rect is damage           */

    s->prev_corner_radius = 0;
    return s;
}

void surface_destroy(struct surface *s) {
    if (!s) return;
    if (s->pixels) kfree(s->pixels);
    kfree(s);
}

void surface_clear(struct surface *s, uint32_t argb) {
    if (!s || !s->pixels) return;
    size_t n = (size_t) s->width * (size_t) s->height;
    for (size_t i = 0; i < n; i++) s->pixels[i] = argb;
    s->pixels_dirty     = 1;
    s->dirty_rect_valid = 0;
}

void surface_move(struct surface *s, int32_t x, int32_t y) {
    if (!s) return;
    s->x = x; s->y = y;
}

void surface_set_z(struct surface *s, int32_t z)    { if (s) s->z = z; }
void surface_set_alpha(struct surface *s, uint8_t a){ if (s) s->alpha = a; }
void surface_show(struct surface *s, int visible)   { if (s) s->visible = visible ? 1 : 0; }

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

void surface_set_corner_radius(struct surface *s, uint32_t r) {
    if (!s) return;
    uint32_t cap = min_u32(s->width, s->height) / 2u;
    if (r > cap)                  r = cap;
    if (r > SURFACE_MAX_CORNER)   r = SURFACE_MAX_CORNER;
    if (s->corner_radius == r) return;
    s->corner_radius = r;
}

struct rect surface_effective_rect(const struct surface *s) {
    if (!s) return rect_make(0, 0, 0, 0);
    return rect_make(s->x, s->y, (int32_t) s->width, (int32_t) s->height);
}

void surface_mark_dirty(struct surface *s) {
    if (!s) return;
    s->pixels_dirty       = 1;
    s->dirty_rect_valid   = 0;
}

void surface_mark_dirty_rect(struct surface *s,
                             int32_t lx0, int32_t ly0, int32_t lx1, int32_t ly1) {
    if (!s || !s->pixels) return;
    if (lx1 <= lx0 || ly1 <= ly0) return;

    int32_t w = (int32_t) s->width;
    int32_t h = (int32_t) s->height;
    if (lx0 < 0) lx0 = 0;
    if (ly0 < 0) ly0 = 0;
    if (lx1 > w) lx1 = w;
    if (ly1 > h) ly1 = h;
    if (lx1 <= lx0 || ly1 <= ly0) return;

    s->pixels_dirty = 1;
    if (!s->dirty_rect_valid) {
        s->dirty_lx0         = lx0;
        s->dirty_ly0         = ly0;
        s->dirty_lx1         = lx1;
        s->dirty_ly1         = ly1;
        s->dirty_rect_valid  = 1;
        return;
    }
    if (lx0 < s->dirty_lx0) s->dirty_lx0 = lx0;
    if (ly0 < s->dirty_ly0) s->dirty_ly0 = ly0;
    if (lx1 > s->dirty_lx1) s->dirty_lx1 = lx1;
    if (ly1 > s->dirty_ly1) s->dirty_ly1 = ly1;
}
