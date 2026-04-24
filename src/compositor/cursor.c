#include "cursor.h"
#include "compositor.h"
#include "surface.h"

#include "../drivers/mouse.h"
#include "../kprintf.h"

#include <stdint.h>

/* 12×16 classic arrow. '#' = black outline, 'X' = white fill,
 * '.' = transparent. Hotspot at (0,0) — the top-left pixel points at
 * the click location, like every OS cursor. */
static const char *arrow[] = {
    "#...........",
    "##..........",
    "#X#.........",
    "#XX#........",
    "#XXX#.......",
    "#XXXX#......",
    "#XXXXX#.....",
    "#XXXXXX#....",
    "#XXXXXXX#...",
    "#XXXXXXXX#..",
    "#XXXXX#####.",
    "#XXX#X#.....",
    "#XX#.#X#....",
    "#X#..#X#....",
    "##....#X#...",
    "#.....#X#...",
};

#define CURSOR_W 12
#define CURSOR_H 16

static struct surface *cursor_surf;

void cursor_init(void) {
    cursor_surf = surface_create("cursor", CURSOR_W, CURSOR_H);
    if (!cursor_surf) { kprintf("[cursor] surface_create failed\n"); return; }

    for (int y = 0; y < CURSOR_H; y++) {
        const char *row = arrow[y];
        for (int x = 0; x < CURSOR_W; x++) {
            uint32_t c;
            switch (row[x]) {
            case '#': c = 0xff000000u; break;   /* outline */
            case 'X': c = 0xffffffffu; break;   /* fill    */
            default:  c = 0x00000000u; break;   /* alpha=0 */
            }
            cursor_surf->pixels[y * CURSOR_W + x] = c;
        }
    }

    /* Opaque pixels in the mask should blit as 255, transparent should
     * skip. blit_alpha honours per-pixel alpha so we stay in the alpha
     * path (opaque=0). Z sits above everything else the app-layer will
     * create; future surfaces should use z well below INT32_MAX. */
    cursor_surf->z       = 2000000000;
    cursor_surf->alpha   = 255;
    cursor_surf->visible = 1;
    cursor_surf->opaque  = 0;

    compositor_add(cursor_surf);
    cursor_tick();
    kprintf("[cursor] overlay registered %dx%d @ z=%d\n",
            CURSOR_W, CURSOR_H, cursor_surf->z);
}

void cursor_tick(void) {
    if (!cursor_surf) return;
    int32_t mx, my;
    mouse_get_state(&mx, &my, NULL);
    cursor_surf->x = mx;
    cursor_surf->y = my;
}
