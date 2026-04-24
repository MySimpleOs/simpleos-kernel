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
            /* Leave a rect-sized hole where the surface was so the next
             * frame repaints the background (and any surfaces it used to
             * cover). */
            if (s->prev_known) {
                struct rect r = rect_make(s->prev_x, s->prev_y,
                                          (int32_t) s->prev_w,
                                          (int32_t) s->prev_h);
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

static int surface_changed(const struct surface *s) {
    if (!s->prev_known)                return 1;
    if (s->pixels_dirty)               return 1;
    if (s->x      != s->prev_x)        return 1;
    if (s->y      != s->prev_y)        return 1;
    if (s->width  != s->prev_w)        return 1;
    if (s->height != s->prev_h)        return 1;
    if (s->z      != s->prev_z)        return 1;
    if (s->alpha  != s->prev_alpha)    return 1;
    if (s->visible!= s->prev_visible)  return 1;
    if (s->opaque != s->prev_opaque)   return 1;
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
        .stride = dd->pitch / 4u,
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

        struct rect curr = rect_make(s->x, s->y,
                                     (int32_t) s->width,
                                     (int32_t) s->height);
        struct rect prev = rect_make(s->prev_x, s->prev_y,
                                     (int32_t) s->prev_w,
                                     (int32_t) s->prev_h);

        int prev_contrib = s->prev_known &&
                           surface_contributed(s->prev_visible, s->prev_alpha);
        int curr_contrib = surface_contributed(s->visible, s->alpha);

        if (!surface_changed(s)) continue;

        if (prev_contrib) damage_add(&dmg, prev);
        if (curr_contrib) damage_add(&dmg, curr);
    }

    damage_clip(&dmg, screen);
    cs_damage_rects = (uint32_t) dmg.count;
    cs_damage_px    = (uint32_t) damage_area_sum(&dmg);

    if (dmg.count == 0) {
        /* Nothing to draw and nothing to present. */
        cs_skipped++;
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
    /* display_present_rect walks the backend's "publish" path: on
     * Limine FB it memcpy's shadow → hw_fb for the rect; on virtio-gpu
     * it syncs shadow → backing + TRANSFER+FLUSH. Partial publish is
     * safe because the non-damage pixels in the front buffer already
     * hold the previous frame's final composite — nothing we write
     * would contradict what's there. Full-screen publish was tried
     * (atomicity argument) but the Limine-FB hw_fb is write-combined
     * / uncached and full-frame memcpy cost 30 ms+, blowing the 8.3 ms
     * frame budget at 120 Hz. Rect publish brings it back to ~2 ms. */
    display_present_rect(damage_bbox(&dmg));

    /* ---- phase 4: snapshot prev state + clear dirty bits ---- */
    for (int i = 0; i < slot_count; i++) {
        struct surface *s = slots[i];
        if (!s) continue;
        s->prev_x       = s->x;
        s->prev_y       = s->y;
        s->prev_w       = s->width;
        s->prev_h       = s->height;
        s->prev_z       = s->z;
        s->prev_alpha   = s->alpha;
        s->prev_visible = s->visible;
        s->prev_opaque  = s->opaque;
        s->prev_known   = 1;
        s->pixels_dirty = 0;
    }
}

struct compositor_args {
    uint32_t bg;
    uint32_t target_hz;
};

static struct compositor_args thread_args;

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
    if (!timer_hz) return;

    uint64_t fpt = timer_hz / thread_args.target_hz;
    if (!fpt) fpt = 1;

    uint64_t tsc_per_us = tsc_hz / 1000000ull;
    if (!tsc_per_us) tsc_per_us = 1;

    /* Fixed dt of exactly 1/target_hz. Measured dt (tsc-based) drifts
     * frame-to-frame — that's wall-clock-accurate but produces visibly
     * non-uniform motion (a 12 ms frame advances the animation farther
     * than an 8 ms frame, then the next lands in a different
     * sub-pixel, reading as judder/flicker on any surface that moves).
     * Fixed dt gives the animation engine a uniform input; if the
     * scheduler stalls for real (rare), motion pauses briefly rather
     * than teleporting. At 120 Hz the stutter is invisible; the
     * cure-better-than-the-disease calculus. */
    fx16 target_dt = fx_div(FX_ONE, FX_FROM_INT(thread_args.target_hz));
    fx16 dt_fx     = target_dt;

    uint64_t next    = timer_ticks + fpt;
    uint32_t window_count = 0;
    uint64_t window_sum   = 0;

    for (;;) {
        while ((uint64_t) timer_ticks < next) thread_yield();

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

        /* Re-average every 120 frames (≈1 s at 120 Hz). Keeps the
         * reported avg fresh without a full ring buffer. */
        if (window_count >= 120) {
            cs_avg_us    = (uint32_t) (window_sum / window_count);
            window_sum   = 0;
            window_count = 0;
            struct parallel_stats ps;
            parallel_get_stats(&ps);
            kprintf("[compositor] fps=%u last=%uus avg=%uus max=%uus drops=%u "
                    "dmg=%u/%upx skip=%u cpus=%u bsp-tiles=%u ap-tiles=%u\n",
                    (unsigned) thread_args.target_hz,
                    (unsigned) cs_last_us,
                    (unsigned) cs_avg_us,
                    (unsigned) cs_max_us,
                    (unsigned) cs_drops,
                    (unsigned) cs_damage_rects,
                    (unsigned) cs_damage_px,
                    (unsigned) cs_skipped,
                    (unsigned) ps.frame_cpus,
                    (unsigned) ps.bsp_tiles,
                    (unsigned) ps.ap_tiles);
            cs_max_us = 0;
            cs_skipped = 0;
        }

        uint64_t now = timer_ticks;
        if (now > next + fpt) {       /* >1 full frame behind            */
            cs_drops++;
            next = now + fpt;
        } else {
            next += fpt;
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
