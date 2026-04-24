#pragma once

/* Compositor: owns the active surface list, z-sorts it, and composites
 * onto the display back buffer. Faz 12.1 scope was single-threaded
 * on-demand; 12.2 added a dedicated kernel thread; 12.5 added damage
 * tracking — only the rects that changed since the previous frame are
 * re-cleared + re-blitted, and the present path pushes just the damage
 * bounding box to the scanout.
 */

#include "rect.h"

#include <stdint.h>

struct surface;

/* Capacity is fixed for simplicity — enough for early UI; can grow later.
 * Surfaces are held by pointer; ownership stays with the caller. */
#define COMPOSITOR_MAX_SURFACES 64

void compositor_init(void);

/* Register / unregister a surface. add() ignores duplicates and returns
 * 0 on success, -1 if capacity is full or s is NULL. remove() queues
 * the surface's last-known rect as damage so the background shows
 * through on the next frame. */
int  compositor_add(struct surface *s);
void compositor_remove(struct surface *s);

/* Bring a surface to the front of the stack by resetting its z above
 * every other registered surface's z. */
void compositor_raise(struct surface *s);
void compositor_lower(struct surface *s);

/* Force the next compositor_frame() to paint the entire screen, even
 * when nothing else changed. Used when the caller changed external
 * state the damage tracker can't see (e.g. the desktop bg colour). */
void compositor_mark_full_damage(void);

/* Build the damage set from surface prev vs current state, scissor-clip
 * clears + blits to the damage rects, present only the damage bbox.
 * Safe to call without surfaces — on the first frame the full screen
 * is damaged. Returns with an empty damage list producing no work and
 * no present. */
void compositor_frame(uint32_t bg_xrgb);

/* Spawn a dedicated kernel thread that calls compositor_frame(bg) at
 * `target_hz` Hz. Paces itself against timer_ticks (see apic.h). Starts
 * immediately; safe to call after sched_init. */
#define COMPOSITOR_DEFAULT_BG 0xff0a1e3c   /* dark navy desktop            */
void compositor_start(uint32_t bg_xrgb, uint32_t target_hz);

/* Frame-time statistics collected by the compositor thread. Values are
 * in microseconds (derived from TSC) and reset between reads. `drops` is
 * the count of frames skipped because the previous frame ran long enough
 * to miss its slot. `damage_rects` and `damage_px` are the most-recent
 * frame's damage-list size and total pixel area (0 when no damage). */
struct compositor_stats {
    uint64_t frame_count;
    uint32_t last_us;
    uint32_t avg_us;
    uint32_t max_us;
    uint32_t drops;
    uint32_t damage_rects;
    uint32_t damage_px;
    uint32_t skipped;         /* frames short-circuited (zero damage)    */
};
void compositor_get_stats(struct compositor_stats *out);
