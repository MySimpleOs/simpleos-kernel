#include "compositor.h"
#include "surface.h"
#include "blit.h"
#include "anim.h"
#include "cursor.h"
#include "damage.h"
#include "parallel.h"

#include "../arch/x86_64/apic.h"
#include "../arch/x86_64/smp.h"
#include "../gpu/display.h"
#include "../kprintf.h"
#include "../sched/thread.h"

#include <stdint.h>
#include <stddef.h>

static struct surface *slots[COMPOSITOR_MAX_SURFACES];
static int             slot_count;

/* Damage that survives until the next frame eats it. Populated by
 * compositor_remove() (a removed surface must repaint its last rect to
 * expose the bg) and compositor_mark_full_damage(). compositor_frame
 * merges this in at the start, then resets. */
static struct damage carry_damage;
static int           full_damage_requested = 1;  /* first frame: paint all */

/* Frame-time stats. Writers: compositor thread only. Readers via
 * compositor_get_stats() take a consistent snapshot with cli/sti. */
static volatile uint64_t cs_frame_count;
static volatile uint32_t cs_last_us, cs_avg_us, cs_max_us, cs_drops;
static volatile uint32_t cs_damage_rects, cs_damage_px, cs_skipped;

void compositor_init(void) {
    for (int i = 0; i < COMPOSITOR_MAX_SURFACES; i++) slots[i] = NULL;
    slot_count = 0;
    damage_reset(&carry_damage);
    full_damage_requested = 1;
    kprintf("[compositor] init, %d surface slots\n", COMPOSITOR_MAX_SURFACES);
}

int compositor_add(struct surface *s) {
    if (!s) return -1;
    for (int i = 0; i < slot_count; i++) {
        if (slots[i] == s) return 0;
    }
    if (slot_count >= COMPOSITOR_MAX_SURFACES) return -1;
    slots[slot_count++] = s;
    /* New surface hasn't been seen before; its first frame will damage
     * the full rect via prev_known=0. Nothing else needed. */
    return 0;
}

void compositor_remove(struct surface *s) {
    if (!s) return;
    for (int i = 0; i < slot_count; i++) {
        if (slots[i] == s) {
            /* Leave a prev-rect-sized hole so the next frame repaints the bg. */
            if (s->prev_known) {
                int32_t px = s->prev_x, py = s->prev_y;
                int32_t pw = (int32_t) s->prev_w, ph = (int32_t) s->prev_h;
                struct rect r = rect_make(px, py, pw, ph);
                damage_add(&carry_damage, r);
            }
            for (int j = i; j < slot_count - 1; j++) slots[j] = slots[j + 1];
            slots[--slot_count] = NULL;
            return;
        }
    }
}

void compositor_mark_full_damage(void) {
    full_damage_requested = 1;
}

static int top_z(void) {
    int32_t max_z = 0;
    int     have  = 0;
    for (int i = 0; i < slot_count; i++) {
        if (!have || slots[i]->z > max_z) { max_z = slots[i]->z; have = 1; }
    }
    return have ? max_z : 0;
}

static int bottom_z(void) {
    int32_t min_z = 0;
    int     have  = 0;
    for (int i = 0; i < slot_count; i++) {
        if (!have || slots[i]->z < min_z) { min_z = slots[i]->z; have = 1; }
    }
    return have ? min_z : 0;
}

void compositor_raise(struct surface *s) {
    if (!s) return;
    s->z = top_z() + 1;
}

void compositor_lower(struct surface *s) {
    if (!s) return;
    s->z = bottom_z() - 1;
}

/* Insertion-sort slot indices by z ascending (low → high). Stable enough
 * for <=64 entries and avoids pulling in a qsort dep. */
static void sort_indices(int *idx) {
    for (int i = 0; i < slot_count; i++) idx[i] = i;
    for (int i = 1; i < slot_count; i++) {
        int key = idx[i];
        int32_t key_z = slots[key]->z;
        int j = i - 1;
        while (j >= 0 && slots[idx[j]]->z > key_z) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }
}

/* Geometry + appearance match last frame snapshot (pixel buffer may still
 * differ — used to emit tight damage for partial surface_mark_dirty_rect). */
static int surface_geom_matches_prev(const struct surface *s) {
    if (!s || !s->prev_known) return 0;
    if (s->x != s->prev_x || s->y != s->prev_y) return 0;
    if (s->width != s->prev_w || s->height != s->prev_h) return 0;
    if (s->z != s->prev_z) return 0;
    if (s->alpha != s->prev_alpha || s->visible != s->prev_visible) return 0;
    if (s->opaque != s->prev_opaque) return 0;
    if (s->corner_radius != s->prev_corner_radius) return 0;
    return 1;
}

static int surface_changed(const struct surface *s) {
    if (!s->prev_known)                        return 1;
    if (s->pixels_dirty)                       return 1;
    if (s->x      != s->prev_x)                return 1;
    if (s->y      != s->prev_y)                return 1;
    if (s->width  != s->prev_w)                return 1;
    if (s->height != s->prev_h)                return 1;
    if (s->z      != s->prev_z)                return 1;
    if (s->alpha  != s->prev_alpha)            return 1;
    if (s->visible!= s->prev_visible)          return 1;
    if (s->opaque != s->prev_opaque)           return 1;
    if (s->corner_radius != s->prev_corner_radius) return 1;
    return 0;
}

static inline int surface_contributed(uint8_t visible, uint8_t alpha) {
    return visible && alpha > 0;
}

void compositor_frame(uint32_t bg_xrgb) {
    const struct display *dd = display_get();
    if (!dd || !dd->pixels) return;

    struct blit_dst dst = {
        .pixels = dd->pixels,
        .width  = dd->width,
        .height = dd->height,
        .stride = dd->width, /* compositor back buffer: tight rows */
    };
    struct rect screen = rect_make(0, 0,
                                   (int32_t) dd->width, (int32_t) dd->height);

    /* ---- phase 1: damage accumulation ---- */
    struct damage dmg;
    damage_reset(&dmg);

    if (full_damage_requested) {
        damage_add(&dmg, screen);
        full_damage_requested = 0;
    }

    /* Merge any damage queued from previous remove()/explicit marks. */
    for (int i = 0; i < carry_damage.count; i++) damage_add(&dmg, carry_damage.rects[i]);
    damage_reset(&carry_damage);

    for (int i = 0; i < slot_count; i++) {
        struct surface *s = slots[i];
        if (!s) continue;

        struct rect curr = surface_effective_rect(s);

        int32_t prev_x0 = s->prev_x;
        int32_t prev_y0 = s->prev_y;
        int32_t prev_w  = (int32_t) s->prev_w;
        int32_t prev_h  = (int32_t) s->prev_h;
        struct rect prev = rect_make(prev_x0, prev_y0, prev_w, prev_h);

        int prev_contrib = s->prev_known &&
                           surface_contributed(s->prev_visible, s->prev_alpha);
        int curr_contrib = surface_contributed(s->visible, s->alpha);

        if (!surface_changed(s)) continue;

        if (s->pixels_dirty && s->dirty_rect_valid && surface_geom_matches_prev(s)) {
            struct rect loc = rect_make(s->dirty_lx0, s->dirty_ly0,
                                        s->dirty_lx1 - s->dirty_lx0,
                                        s->dirty_ly1 - s->dirty_ly0);
            struct rect scr = rect_make(s->x + loc.x, s->y + loc.y, loc.w, loc.h);
            if (curr_contrib) damage_add(&dmg, scr);
        } else {
            if (prev_contrib) damage_add(&dmg, prev);
            if (curr_contrib) damage_add(&dmg, curr);
        }
    }

    damage_clip(&dmg, screen);
    cs_damage_rects = (uint32_t) dmg.count;
    cs_damage_px    = (uint32_t) damage_area_sum(&dmg);

    if (dmg.count == 0) {
        /* Nothing to draw; still sync AP epoch so workers do not busy-spin,
         * then pace like a presented frame when vsync is enabled. */
        cs_skipped++;
        parallel_compose_idle_barrier();
        display_vsync_wait_after_present();
        return;
    }

    /* ---- phase 2: parallel compose across all CPUs ---- */
    /* Build the z-sorted pointer array once on BSP; parallel_compose
     * then dispatches horizontal bands of the damage bbox to every
     * online CPU (BSP included). APs pick up their share through the
     * epoch + atomic-tile dispatcher in parallel.c. */
    int idx[COMPOSITOR_MAX_SURFACES];
    sort_indices(idx);

    struct surface *z_sorted[COMPOSITOR_MAX_SURFACES];
    for (int i = 0; i < slot_count; i++) z_sorted[i] = slots[idx[i]];

    parallel_compose(dst, &dmg, z_sorted, slot_count, bg_xrgb);

    /* ---- phase 3: publish damage bbox to the front buffer ---- */
    /* display_present_rect walks the backend publish path: on Limine FB
     * it memcpy's the compositor back buffer → hw_fb for the rect. Partial
     * publish is
     * safe because the non-damage pixels in the front buffer already
     * hold the previous frame's final composite — nothing we write
     * would contradict what's there. Full-screen publish was tried
     * (atomicity argument) but the Limine-FB hw_fb is write-combined
     * / uncached and full-frame memcpy cost 30 ms+, blowing the 8.3 ms
     * frame budget at 120 Hz. Rect publish brings it back to ~2 ms. */
    /* Present each damaged rect under one IRQ disable. Copying only the
     * bbox would push stale back-buffer pixels in gaps between disjoint rects. */
    display_present_damage(&dmg);
    display_vsync_wait_after_present();

    /* ---- phase 4: snapshot prev state + clear dirty bits ---- */
    for (int i = 0; i < slot_count; i++) {
        struct surface *s = slots[i];
        if (!s) continue;
        s->prev_x              = s->x;
        s->prev_y              = s->y;
        s->prev_w              = s->width;
        s->prev_h              = s->height;
        s->prev_z              = s->z;
        s->prev_alpha          = s->alpha;
        s->prev_visible        = s->visible;
        s->prev_opaque         = s->opaque;
        s->prev_corner_radius  = s->corner_radius;
        s->prev_known          = 1;
        s->pixels_dirty        = 0;
        s->dirty_rect_valid    = 0;
    }
}

struct compositor_args {
    uint32_t bg;
    uint32_t target_hz;
};

static struct compositor_args thread_args;

/* TSC delta → anim dt (seconds, Q16.16). Caps long stalls. */
static fx16 tsc_elapsed_to_dt(uint64_t delta, uint64_t hz) {
    if (!hz || !delta) return 0;
    uint64_t cap = hz / 4u; /* ~250 ms at ~1 GHz TSC; scales with tsc_hz */
    if (!cap) cap = delta;
    if (delta > cap) delta = cap;
    return (fx16) (((int64_t) delta * (int64_t) FX_ONE) / (int64_t) hz);
}

/* Kernel thread body: paces compositor_frame() against timer_ticks.
 *
 * Pacing strategy — timer_hz is the periodic LAPIC tick rate (ms-ish
 * scale). ticks_per_frame = timer_hz / target_hz. We record a deadline
 * (`next_deadline`), yield until timer_ticks reaches it, render the
 * frame, advance the deadline. If we fell more than one full frame
 * behind (frame took too long or we were descheduled), the deadline is
 * snapped forward and a drop is counted so averages stay honest. */
static void compositor_thread_body(void *arg) {
    (void) arg;
    /* Pacing uses TSC when available so frame rate does not depend on a
     * miscalibrated LAPIC tick (observed ~1 FPS when timer_hz was wrong). */
    if (!tsc_hz && !timer_hz) return;

    uint64_t fpt = timer_hz ? (uint64_t) timer_hz / thread_args.target_hz : 0;
    if (!fpt) fpt = 1;

    uint64_t tsc_per_frame = 0;
    if (tsc_hz) {
        tsc_per_frame = tsc_hz / (uint64_t) thread_args.target_hz;
        if (tsc_per_frame < 1000ull) tsc_per_frame = 1000ull;
    }

    uint64_t tsc_per_us = tsc_hz / 1000000ull;
    if (!tsc_per_us) tsc_per_us = 1;

    /* Nominal dt when wall clock is unknown (first frame). */
    fx16 nominal_dt = fx_div(FX_ONE, FX_FROM_INT(thread_args.target_hz));

    uint64_t next_tick = (uint64_t) timer_ticks + fpt;
    uint64_t next_tsc  = tsc_per_frame ? (rdtsc() + tsc_per_frame) : 0;
    uint32_t window_count = 0;
    uint64_t window_sum   = 0;

    uint64_t last_frame_start = rdtsc();
    int      anim_dt_first    = 1;

    for (;;) {
        if (tsc_per_frame) {
            while (rdtsc() < next_tsc) thread_yield();
        } else {
            while ((uint64_t) timer_ticks < next_tick) thread_yield();
        }

        uint64_t now_frame = rdtsc();
        fx16 dt_fx = nominal_dt;
        if (tsc_hz) {
            if (!anim_dt_first) {
                dt_fx = tsc_elapsed_to_dt(now_frame - last_frame_start, tsc_hz);
                if (dt_fx < FX_FROM_MILLI(1)) dt_fx = FX_FROM_MILLI(1);
            }
            anim_dt_first = 0;
        }
        last_frame_start = now_frame;

        anim_tick_all(dt_fx);
        cursor_tick();

        uint64_t t0 = rdtsc();
        compositor_frame(thread_args.bg);
        uint64_t t1 = rdtsc();

        uint32_t us = (uint32_t) ((t1 - t0) / tsc_per_us);
        cs_last_us = us;
        if (us > cs_max_us) cs_max_us = us;
        window_sum += us;
        window_count++;
        cs_frame_count++;

        if (window_count >= 120) {
            cs_avg_us = (uint32_t) (window_sum / window_count);
            window_sum    = 0;
            window_count  = 0;
            cs_max_us       = 0;
            cs_skipped      = 0;
        }

        if (tsc_per_frame) {
            next_tsc += tsc_per_frame;
            uint64_t nowt = rdtsc();
            if (nowt > next_tsc + tsc_per_frame) {
                cs_drops++;
                next_tsc = nowt + tsc_per_frame;
            }
        } else {
            uint64_t now = (uint64_t) timer_ticks;
            if (now > next_tick + fpt) {
                cs_drops++;
                next_tick = now + fpt;
            } else {
                next_tick += fpt;
            }
        }
    }
}

void compositor_start(uint32_t bg_xrgb, uint32_t target_hz) {
    if (!target_hz) target_hz = 120;
    thread_args.bg        = bg_xrgb;
    thread_args.target_hz = target_hz;
    thread_create("compositor", compositor_thread_body, NULL);
    kprintf("[compositor] thread spawned, target=%u Hz\n", (unsigned) target_hz);
}

void compositor_get_stats(struct compositor_stats *out) {
    if (!out) return;
    /* No cross-core writer yet (Faz 12.6); a plain copy is fine for now. */
    out->frame_count  = cs_frame_count;
    out->last_us      = cs_last_us;
    out->avg_us       = cs_avg_us;
    out->max_us       = cs_max_us;
    out->drops        = cs_drops;
    out->damage_rects = cs_damage_rects;
    out->damage_px    = cs_damage_px;
    out->skipped      = cs_skipped;
}
