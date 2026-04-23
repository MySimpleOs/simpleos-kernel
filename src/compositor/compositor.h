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

/* Spawn a dedicated kernel thread that calls compositor_frame(bg) at
 * `target_hz` Hz. Paces itself against timer_ticks (see apic.h). Starts
 * immediately; safe to call after sched_init. */
#define COMPOSITOR_DEFAULT_BG 0xff0a1e3c   /* dark navy desktop            */
void compositor_start(uint32_t bg_xrgb, uint32_t target_hz);

/* Frame-time statistics collected by the compositor thread. Values are
 * in microseconds (derived from TSC) and reset between reads. `drops` is
 * the count of frames skipped because the previous frame ran long enough
 * to miss its slot. */
struct compositor_stats {
    uint64_t frame_count;
    uint32_t last_us;
    uint32_t avg_us;
    uint32_t max_us;
    uint32_t drops;
};
void compositor_get_stats(struct compositor_stats *out);
