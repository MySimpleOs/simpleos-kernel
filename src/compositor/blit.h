#pragma once

/* Pixel-level primitives used by the compositor. All naive C for Faz 12.1;
 * SSE2/AVX2 specialisations land in Faz 12.5 behind the same signatures.
 *
 * Rectangle semantics: rx/ry are top-left in destination, rw/rh are
 * size. Negative positions and out-of-bounds are clipped here; callers can
 * pass global coords freely.
 *
 * The `*_scissor` variants take an additional rect that further clips the
 * destination — used by the damage tracker so each primitive paints only
 * inside the damage region for this frame. Pass NULL scissor to disable
 * (equivalent to the non-scissor variant).
 */

#include "rect.h"
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

void blit_fill_scissor(const struct blit_dst *dst, const struct rect *scissor,
                       int32_t rx, int32_t ry, int32_t rw, int32_t rh,
                       uint32_t color);

/* Opaque copy (ignores source alpha). Destination-position (dx, dy) in
 * dst coords; source reads from src top-left. Out-of-bounds are clipped
 * on both ends. */
void blit_copy(const struct blit_dst *dst, int32_t dx, int32_t dy,
               const struct blit_src *src);

void blit_copy_scissor(const struct blit_dst *dst, const struct rect *scissor,
                       int32_t dx, int32_t dy, const struct blit_src *src);

/* Straight-alpha 'over' composite. src.a blended with dst underneath,
 * then scaled by per-surface global_alpha (0..255). When global_alpha=255
 * and src.a=255 for every pixel the result equals blit_copy. */
void blit_alpha(const struct blit_dst *dst, int32_t dx, int32_t dy,
                const struct blit_src *src, uint8_t global_alpha);

void blit_alpha_scissor(const struct blit_dst *dst, const struct rect *scissor,
                        int32_t dx, int32_t dy,
                        const struct blit_src *src, uint8_t global_alpha);

/* Rounded-corner variants. `corner_radius` is in surface-local pixels;
 * passing 0 is equivalent to the non-rounded variant above. The function
 * uses the SDF corner mask defined in shadow.h — the interior stays on
 * the SIMD fast path (full opacity), only the corner bands pay the
 * per-pixel mask cost. The src buffer is assumed to cover the full
 * surface (src->width/height = surface size, stride = width); the
 * compositor's compose_band builds src exactly that way. */
void blit_copy_rounded_scissor(const struct blit_dst *dst,
                               const struct rect *scissor,
                               int32_t dx, int32_t dy,
                               const struct blit_src *src,
                               uint32_t corner_radius);

void blit_alpha_rounded_scissor(const struct blit_dst *dst,
                                const struct rect *scissor,
                                int32_t dx, int32_t dy,
                                const struct blit_src *src,
                                uint8_t global_alpha,
                                uint32_t corner_radius);

/* Shadow composite. Draws a solid-color silhouette modulated by an 8-bit
 * alpha mask (`mask_pixels`, w=mask_w, h=mask_h, stride=mask_w) over the
 * destination, at absolute (dx, dy). `color` is XRGB (alpha bits
 * ignored); `global_alpha` scales the mask's opacity. */
void blit_shadow_scissor(const struct blit_dst *dst,
                         const struct rect *scissor,
                         int32_t dx, int32_t dy,
                         const uint8_t *mask_pixels,
                         uint32_t mask_w, uint32_t mask_h,
                         uint32_t color, uint8_t global_alpha);

/* ---- SIMD row fast-paths (see compositor/blit_simd.c) ---------------------
 *
 * These compose `n` pixels from `src` onto `dst` using the same math as the
 * scalar inner loops in blit.c. They live in a TU compiled without
 * -mgeneral-regs-only (the rest of the kernel bans XMM/YMM use), so the
 * dispatcher in blit.c calls these only after simd_cpu_init() has enabled
 * SSE/AVX on the current CPU. `ga` is the per-surface global_alpha in
 * [0, 255]; `n` is the pixel count (may be 0).
 */
void blit_copy_row_sse2(uint32_t *dst, const uint32_t *src, int32_t n);
void blit_alpha_row_sse2(uint32_t *dst, const uint32_t *src,
                         uint32_t ga, int32_t n);
void blit_copy_row_avx2(uint32_t *dst, const uint32_t *src, int32_t n);
void blit_alpha_row_avx2(uint32_t *dst, const uint32_t *src,
                         uint32_t ga, int32_t n);
