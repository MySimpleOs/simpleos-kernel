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
 *
 * Faz 12.7 extensions (rounded corners + shadow + gradient):
 *   - `corner_radius` turns the surface into a rounded rectangle. Blit
 *     path applies an SDF-based anti-aliased alpha mask only in the
 *     corner bands; the interior band runs the SSE2/AVX2 fast path at
 *     full speed.
 *   - `shadow_*` render a cached, box-blurred alpha silhouette of the
 *     (rounded) surface underneath, tinted with `shadow_color` and
 *     modulated by `shadow_alpha`. The mask is regenerated lazily when
 *     geometry / corner_radius / blur_radius change.
 *   - Gradients are not fields — `gradient.h` writes straight into
 *     `pixels`, so they coexist with everything else.
 */

#include "rect.h"

#include <stdint.h>
#include <stddef.h>

#define SURFACE_NAME_LEN      24
#define SURFACE_MAX_CORNER    64u  /* keeps sdf int math inside u32         */
#define SURFACE_MAX_BLUR      64u  /* bounds cached mask size               */

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
    uint8_t   dirty_rect_valid; /* 1 → use dirty_* as local pixel bbox only   */
    uint8_t   _pad_dirty[3];

    /* Local-space (0..width/height) dirty region for partial repaints.
     * Half-open: [lx0, lx1) × [ly0, ly1). Meaningful only if dirty_rect_valid. */
    int32_t   dirty_lx0, dirty_ly0, dirty_lx1, dirty_ly1;

    /* Rounded corners. 0 = sharp. Capped at SURFACE_MAX_CORNER. */
    uint32_t  corner_radius;

    /* Drop shadow. shadow_blur == 0 disables. shadow_mask is a cached
     * w+2*blur x h+2*blur alpha buffer; compositor_frame ensures it is
     * up to date before blitting. */
    int32_t   shadow_ox;
    int32_t   shadow_oy;
    uint32_t  shadow_blur;
    uint32_t  shadow_color;     /* XRGB; alpha comes from the mask           */
    uint8_t   shadow_alpha;     /* 0..255 global shadow multiplier           */
    uint8_t   shadow_dirty;     /* 1 → regen mask on next frame              */
    uint8_t   _pad0[2];

    uint8_t  *shadow_mask;      /* 8-bit alpha, stride = shadow_mask_w       */
    uint32_t  shadow_mask_w;
    uint32_t  shadow_mask_h;

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

    uint32_t  prev_corner_radius;
    int32_t   prev_shadow_ox;
    int32_t   prev_shadow_oy;
    uint32_t  prev_shadow_blur;
    uint32_t  prev_shadow_color;
    uint8_t   prev_shadow_alpha;
    uint8_t   _pad1[3];
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

/* Rounded corner radius in pixels. Clamped to min(w,h)/2 and
 * SURFACE_MAX_CORNER. Passing 0 disables rounding. Also invalidates the
 * shadow mask (silhouette depends on corner_radius). */
void surface_set_corner_radius(struct surface *s, uint32_t r);

/* Enable a drop shadow. ox/oy are pixel offsets relative to the surface
 * top-left (positive = down/right), blur is the box-blur radius (0
 * disables), color is XRGB tint, alpha is the global shadow intensity
 * (255 = fully visible). */
void surface_set_shadow(struct surface *s, int32_t ox, int32_t oy,
                        uint32_t blur, uint32_t color, uint8_t alpha);
void surface_clear_shadow(struct surface *s);

/* Computes the screen-space rect the surface *visually* covers, including
 * shadow bleed. Used by the damage tracker so moving / showing a surface
 * also damages the area behind its shadow. */
struct rect surface_effective_rect(const struct surface *s);

/* Tell the compositor that `pixels` was modified since the last frame.
 * Must be called whenever callers write into the buffer directly (the
 * compositor cannot otherwise tell). Cleared automatically each frame. */
void surface_mark_dirty(struct surface *s);

/* Mark a sub-rectangle in surface-local coordinates (tight damage for text,
 * small overlays). Unions with any previous partial dirty until the frame
 * ends. Use surface_mark_dirty() for full-surface edits (clears partial). */
void surface_mark_dirty_rect(struct surface *s,
                             int32_t lx0, int32_t ly0, int32_t lx1, int32_t ly1);

/* Lazily (re)generate the cached shadow_mask. No-op if shadow_blur == 0
 * or the mask is already current. Called by compositor_frame before
 * dispatching compose. */
void surface_ensure_shadow(struct surface *s);
