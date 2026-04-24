#include "cursor.h"
#include "compositor.h"
#include "surface.h"

#include "../assets/cursor_default.inc"

#include "../drivers/mouse.h"
#include "../kprintf.h"

#include <stdint.h>

static struct surface *cursor_surf;

void cursor_init(void) {
    cursor_surf = surface_create("cursor", CURSOR_DEFAULT_W, CURSOR_DEFAULT_H);
    if (!cursor_surf) {
        kprintf("[cursor] surface_create failed\n");
        return;
    }

    for (uint32_t i = 0; i < CURSOR_DEFAULT_W * CURSOR_DEFAULT_H; i++)
        cursor_surf->pixels[i] = cursor_default_rgba[i];

    cursor_surf->z        = SURFACE_Z_CURSOR_LAYER;
    cursor_surf->alpha    = 255;
    cursor_surf->visible  = 1;
    cursor_surf->opaque   = 0;

    compositor_add(cursor_surf);
    cursor_tick();
    kprintf("[cursor] default asset %ux%u ARGB, hotspot (%d,%d)\n",
            (unsigned) CURSOR_DEFAULT_W, (unsigned) CURSOR_DEFAULT_H,
            CURSOR_DEFAULT_HOT_X, CURSOR_DEFAULT_HOT_Y);
}

void cursor_tick(void) {
    if (!cursor_surf) return;
    cursor_surf->z = SURFACE_Z_CURSOR_LAYER;
    mouse_poll();
    int32_t mx, my;
    mouse_get_state(&mx, &my, NULL);
    cursor_surf->x = mx - CURSOR_DEFAULT_HOT_X;
    cursor_surf->y = my - CURSOR_DEFAULT_HOT_Y;
}
