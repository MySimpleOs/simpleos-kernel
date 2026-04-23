#pragma once

/* Pixel-level primitives used by the compositor. All naive C for Faz 12.1;
 * SSE2/AVX2 specialisations land in Faz 12.5 behind the same signatures.
 *
 * Rectangle semantics: rx/ry are top-left in destination, rw/rh are
 * size. Negative positions and out-of-bounds are clipped here; callers can
 * pass global coords freely.
 */

#include <stdint.h>

struct blit_dst {
    uint32_t *pixels;           /* XRGB8888 (A bits ignored)                */
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;           /* pixels per row                           */
};

struct blit_src {
    const uint32_t *pixels;     /* ARGB8888                                 */
    uint32_t        width;
    uint32_t        height;
    uint32_t        stride;
};

/* Fill a rectangle with a solid XRGB colour, clipped to dst bounds. */
void blit_fill(const struct blit_dst *dst,
               int32_t rx, int32_t ry, int32_t rw, int32_t rh,
               uint32_t color);

/* Opaque copy (ignores source alpha). Destination-position (dx, dy) in
 * dst coords; source reads from src top-left. Out-of-bounds are clipped
 * on both ends. */
void blit_copy(const struct blit_dst *dst, int32_t dx, int32_t dy,
               const struct blit_src *src);

/* Straight-alpha 'over' composite. src.a blended with dst underneath,
 * then scaled by per-surface global_alpha (0..255). When global_alpha=255
 * and src.a=255 for every pixel the result equals blit_copy. */
void blit_alpha(const struct blit_dst *dst, int32_t dx, int32_t dy,
                const struct blit_src *src, uint8_t global_alpha);
