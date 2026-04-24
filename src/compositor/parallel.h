#pragma once

/* Multi-core compositor dispatcher (Faz 12.6).
 *
 * The BSP builds the damage region and the z-sorted surface list in
 * compositor_frame phase 1, then hands the work to every CPU via
 * parallel_compose(). The damage bbox is split into horizontal bands
 * (one per online CPU). Every CPU — BSP included — claims bands from
 * an atomic counter, composes them scissor-clipped against the damage
 * rects, and drops through a done barrier. BSP doesn't return from
 * parallel_compose() until every AP has posted "done" for this frame.
 *
 * APs live in compositor_ap_worker() (started from ap_entry in smp.c)
 * and spin on an epoch counter; parallel_compose() bumps the epoch
 * with release ordering so all the setup fields (dst, dmg, surface
 * list, bg) are visible before any AP sees the new epoch.
 */

#include "rect.h"
#include "damage.h"
#include "blit.h"

#include <stdint.h>

struct surface;

#define PARALLEL_MAX_BANDS 16     /* hard cap, covers any reasonable N_CPU */

/* Build bands + kick APs + BSP also pitches in. Returns after every
 * tile has been composed. `z_sorted[0..count)` is bottom-to-top
 * z-order; surfaces with pixels=NULL / visible=0 / alpha=0 are
 * ignored inside the band compose. dmg must stay live until this
 * call returns — we hold a pointer to it during the AP fan-out. */
void parallel_compose(struct blit_dst dst,
                      const struct damage *dmg,
                      struct surface **z_sorted,
                      int surface_count,
                      uint32_t bg);

/* Cumulative stats (single-writer per CPU, read by BSP after barrier).
 * Useful for proving APs are actually doing work. */
struct parallel_stats {
    uint32_t bsp_tiles;
    uint32_t ap_tiles;
    uint32_t frame_cpus;       /* how many CPUs contributed last frame   */
};
void parallel_get_stats(struct parallel_stats *out);
