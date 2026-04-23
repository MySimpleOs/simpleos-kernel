#pragma once

/* Surface = per-window offscreen ARGB8888 buffer + placement metadata.
 *
 * The compositor owns a z-sorted list of these; each frame, surfaces are
 * blitted onto the display back buffer bottom-to-top. Surface coordinates
 * live in global screen space (top-left origin). Per-pixel alpha and a
 * surface-wide alpha multiplier compose at blit time.
 *
 * Pixel format is straight-alpha ARGB8888 (A in bits 24..31). The display
 * back buffer is XRGB8888 (X ignored), so alpha only matters for surface
 * blends, not for what reaches scanout.
 */

#include <stdint.h>
#include <stddef.h>

#define SURFACE_NAME_LEN 24

struct surface {
    char      name[SURFACE_NAME_LEN];
    uint32_t *pixels;           /* ARGB8888, stride = width                  */
    uint32_t  width;
    uint32_t  height;
    int32_t   x;                /* screen-space top-left                     */
    int32_t   y;
    int32_t   z;                /* higher = in front                         */
    uint8_t   alpha;            /* 0..255 global multiplier                  */
    uint8_t   visible;          /* 0 hides without destroying                */
    uint8_t   opaque;           /* 1 = ignore per-pixel alpha at blit        */
    uint8_t   _pad;
};

/* Allocate a surface (heap-backed pixels, zeroed). Returns NULL on heap
 * exhaust or bad size. Caller owns the handle and must call destroy. */
struct surface *surface_create(const char *name, uint32_t w, uint32_t h);
void            surface_destroy(struct surface *s);

/* Convenience: fill entire pixel buffer with an ARGB color. */
void surface_clear(struct surface *s, uint32_t argb);

/* Placement / z-order / visibility helpers. These do not trigger a repaint
 * by themselves; the next compositor_frame() picks them up. */
void surface_move(struct surface *s, int32_t x, int32_t y);
void surface_set_z(struct surface *s, int32_t z);
void surface_set_alpha(struct surface *s, uint8_t a);
void surface_show(struct surface *s, int visible);
