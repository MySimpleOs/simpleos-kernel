#pragma once

/* Damage accumulator used by the compositor every frame.
 *
 * The compositor walks its surface list, notes which surfaces moved /
 * changed visibility / changed pixels since the previous frame, and calls
 * damage_add() with the rect(s) that need to be re-composited. At the end
 * of the walk the list contains up to DAMAGE_MAX_RECTS non-overlapping
 * rects whose union covers everything that must be re-drawn.
 *
 * When the accumulator overflows it collapses to a single bounding box
 * — correctness is preserved at the cost of some over-clear.
 */

#include "rect.h"
#include <stdint.h>

#define DAMAGE_MAX_RECTS 16

struct damage {
    struct rect rects[DAMAGE_MAX_RECTS];
    int         count;
};

void        damage_reset(struct damage *d);
void        damage_add(struct damage *d, struct rect r);

/* Clip all rects in `d` to `bounds` (e.g. display dims). Rects that fall
 * entirely outside are dropped. Safe to call after accumulation. */
void        damage_clip(struct damage *d, struct rect bounds);

/* Bounding box of every rect in the list. Returns empty when count == 0. */
struct rect damage_bbox(const struct damage *d);

/* Total pixels covered by the *union* (no double-counting if rects were
 * kept disjoint, approximate otherwise). Small helper for stats. */
uint64_t    damage_area_sum(const struct damage *d);
