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
    s->pixels  = px;
    s->width   = w;
    s->height  = h;
    s->x       = 0;
    s->y       = 0;
    s->z       = 0;
    s->alpha   = 255;
    s->visible = 1;
    s->opaque  = 0;
    s->_pad    = 0;
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
}

void surface_move(struct surface *s, int32_t x, int32_t y) {
    if (!s) return;
    s->x = x; s->y = y;
}

void surface_set_z(struct surface *s, int32_t z)    { if (s) s->z = z; }
void surface_set_alpha(struct surface *s, uint8_t a){ if (s) s->alpha = a; }
void surface_show(struct surface *s, int visible)   { if (s) s->visible = visible ? 1 : 0; }
