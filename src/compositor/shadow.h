#pragma once

/* Drop-shadow mask cache.
 *
 * shadow_regen(s) (re)allocates s->shadow_mask and writes an 8-bit alpha
 * silhouette of the surface (including its rounded corners) dilated by
 * s->shadow_blur pixels, then runs a 3-pass separable box blur — a
 * textbook near-Gaussian that stays cheap. The result is sampled each
 * frame by blit_shadow_scissor, tinted with shadow_color and multiplied
 * by shadow_alpha * global_alpha.
 *
 * The cache is invalidated by surface_set_corner_radius() and by
 * surface_set_shadow() when blur changes. A compositor pass calls
 * surface_ensure_shadow() which invokes shadow_regen() iff dirty.
 */

struct surface;

/* Rebuilds surface->shadow_mask, resizing the buffer if needed. Safe to
 * call with shadow_blur == 0 (frees the mask and returns). */
void shadow_regen(struct surface *s);

/* Compute the premultiplied 8-bit mask value at (lx, ly) in surface
 * coordinates for a rounded rectangle of (w, h) with corner_radius r.
 * Returns 0 outside the rounded rect and 255 well inside; a 1-pixel AA
 * band runs along the boundary. Used by shadow_regen and by the blit
 * rounded-corner path. */
unsigned char shadow_corner_mask(int lx, int ly, int w, int h, int r);
