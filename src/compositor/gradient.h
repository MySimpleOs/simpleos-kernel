#pragma once

/* Gradient fill helpers. Both variants write straight into s->pixels (the
 * surface's ARGB8888 content buffer) and mark the surface dirty, so the
 * compositor's next frame picks them up through the normal blit path —
 * no special-case rendering, no extra state. Corner rounding and shadow
 * come from the surface; gradients just paint the interior.
 *
 * Colors are ARGB8888 (A in bits 24..31). Per-channel linear interpolation
 * runs in 16-bit integer math; no floats. `t` is implicitly clamped to
 * [0, 1] at pixel time, so endpoint samples outside the geometry take the
 * nearest-edge color.
 */

#include <stdint.h>

struct surface;

/* Linear gradient between the line (x0,y0) → (x1,y1) in surface-local
 * coordinates. Pixels on the p0 side take `a`; pixels on the p1 side
 * take `b`; pixels along the line interpolate linearly. If the line has
 * zero length the whole surface fills with `a`. */
void gradient_fill_linear(struct surface *s,
                          uint32_t a, uint32_t b,
                          int32_t x0, int32_t y0,
                          int32_t x1, int32_t y1);

/* Radial gradient centered at (cx, cy) with full-opacity radius `radius`
 * (pixels). Pixel at center = `inner`; pixel at or beyond `radius`
 * distance = `outer`. Interpolates linearly with distance. */
void gradient_fill_radial(struct surface *s,
                          uint32_t inner, uint32_t outer,
                          int32_t cx, int32_t cy, uint32_t radius);
