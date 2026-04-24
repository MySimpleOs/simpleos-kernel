#include "surface.h"
#include "shadow.h"

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

    s->shadow_ox    = 0;
    s->shadow_oy    = 0;
    s->shadow_blur  = 0;
    s->shadow_color = 0x000000;
    s->shadow_alpha = 0;
    s->shadow_dirty = 0;
    s->shadow_mask  = NULL;
    s->shadow_mask_w = 0;
    s->shadow_mask_h = 0;

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
    s->prev_shadow_ox     = 0;
    s->prev_shadow_oy     = 0;
    s->prev_shadow_blur   = 0;
    s->prev_shadow_color  = 0;
    s->prev_shadow_alpha  = 0;
    return s;
}

void surface_destroy(struct surface *s) {
    if (!s) return;
    if (s->pixels) kfree(s->pixels);
    if (s->shadow_mask) kfree(s->shadow_mask);
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
    s->shadow_dirty  = 1;  /* silhouette changed */
}

void surface_set_shadow(struct surface *s, int32_t ox, int32_t oy,
                        uint32_t blur, uint32_t color, uint8_t alpha) {
    if (!s) return;
    if (blur > SURFACE_MAX_BLUR) blur = SURFACE_MAX_BLUR;
    if (s->shadow_blur != blur) s->shadow_dirty = 1;
    s->shadow_ox    = ox;
    s->shadow_oy    = oy;
    s->shadow_blur  = blur;
    s->shadow_color = color & 0x00ffffffu;
    s->shadow_alpha = alpha;
}

void surface_clear_shadow(struct surface *s) {
    if (!s) return;
    s->shadow_blur  = 0;
    s->shadow_alpha = 0;
    if (s->shadow_mask) {
        kfree(s->shadow_mask);
        s->shadow_mask = NULL;
        s->shadow_mask_w = 0;
        s->shadow_mask_h = 0;
    }
    s->shadow_dirty = 0;
}

struct rect surface_effective_rect(const struct surface *s) {
    if (!s) return rect_make(0, 0, 0, 0);
    int32_t x = s->x, y = s->y;
    int32_t w = (int32_t) s->width, h = (int32_t) s->height;
    if (s->shadow_blur == 0 || s->shadow_alpha == 0) {
        return rect_make(x, y, w, h);
    }
    /* Shadow rect = surface rect shifted by (ox, oy) and dilated by blur.
     * Union with surface rect so the result covers both the content and
     * its shadow halo. */
    int32_t b   = (int32_t) s->shadow_blur;
    int32_t sx  = x + s->shadow_ox - b;
    int32_t sy  = y + s->shadow_oy - b;
    int32_t sw  = w + 2 * b;
    int32_t sh  = h + 2 * b;
    int32_t x0  = x < sx ? x : sx;
    int32_t y0  = y < sy ? y : sy;
    int32_t x1a = x + w;
    int32_t x1b = sx + sw;
    int32_t x1  = x1a > x1b ? x1a : x1b;
    int32_t y1a = y + h;
    int32_t y1b = sy + sh;
    int32_t y1  = y1a > y1b ? y1a : y1b;
    return rect_make(x0, y0, x1 - x0, y1 - y0);
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

void surface_ensure_shadow(struct surface *s) {
    if (!s) return;
    if (s->shadow_blur == 0 || s->shadow_alpha == 0) return;
    if (!s->shadow_dirty && s->shadow_mask) return;
    shadow_regen(s);
    s->shadow_dirty = 0;
}
