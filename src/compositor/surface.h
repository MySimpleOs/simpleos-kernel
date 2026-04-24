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
 *
 * prev_* snapshot the placement at the end of the previous composited
 * frame; the damage tracker diffs current vs prev to find the rects
 * that must be re-composited. `pixels_dirty` flags "content changed"
 * (caller wrote into `pixels` since the last frame) — set via
 * surface_mark_dirty() or surface_clear(). The compositor clears all
 * dirty bits after each frame.
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
    uint8_t   pixels_dirty;     /* set by surface_mark_dirty / surface_clear */

    /* End-of-last-frame snapshot used by the damage tracker. Compositor
     * writes these after a successful frame; callers should not touch. */
    int32_t   prev_x;
    int32_t   prev_y;
    uint32_t  prev_w;
    uint32_t  prev_h;
    int32_t   prev_z;
    uint8_t   prev_alpha;
    uint8_t   prev_visible;
    uint8_t   prev_opaque;
    uint8_t   prev_known;       /* 0 on first frame: whole rect is damage   */
};

/* Allocate a surface (heap-backed pixels, zeroed). Returns NULL on heap
 * exhaust or bad size. Caller owns the handle and must call destroy. */
struct surface *surface_create(const char *name, uint32_t w, uint32_t h);
void            surface_destroy(struct surface *s);

/* Convenience: fill entire pixel buffer with an ARGB color. Marks the
 * surface dirty so the compositor re-blits it. */
void surface_clear(struct surface *s, uint32_t argb);

/* Placement / z-order / visibility helpers. These do not trigger a repaint
 * by themselves; the next compositor_frame() picks them up. */
void surface_move(struct surface *s, int32_t x, int32_t y);
void surface_set_z(struct surface *s, int32_t z);
void surface_set_alpha(struct surface *s, uint8_t a);
void surface_show(struct surface *s, int visible);

/* Tell the compositor that `pixels` was modified since the last frame.
 * Must be called whenever callers write into the buffer directly (the
 * compositor cannot otherwise tell). Cleared automatically each frame. */
void surface_mark_dirty(struct surface *s);
