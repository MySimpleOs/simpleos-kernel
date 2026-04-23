#pragma once

/* Compositor: owns the active surface list, z-sorts it, and composites
 * onto the display back buffer. Faz 12.1 scope is a single-threaded,
 * on-demand compositor_frame() call. Faz 12.2 wraps the call in a
 * dedicated 120 Hz kernel thread.
 */

#include <stdint.h>

struct surface;

/* Capacity is fixed for simplicity — enough for early UI; can grow later.
 * Surfaces are held by pointer; ownership stays with the caller. */
#define COMPOSITOR_MAX_SURFACES 64

void compositor_init(void);

/* Register / unregister a surface. add() ignores duplicates and returns
 * 0 on success, -1 if capacity is full or s is NULL. */
int  compositor_add(struct surface *s);
void compositor_remove(struct surface *s);

/* Bring a surface to the front of the stack by resetting its z above
 * every other registered surface's z. */
void compositor_raise(struct surface *s);
void compositor_lower(struct surface *s);

/* Clear the back buffer to `bg_xrgb`, blit every visible surface in z
 * order (low → high), and present. Safe to call without any surfaces —
 * produces a solid-colour frame. */
void compositor_frame(uint32_t bg_xrgb);
