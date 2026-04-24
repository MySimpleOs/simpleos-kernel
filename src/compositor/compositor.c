#include "compositor.h"
#include "surface.h"
#include "blit.h"
#include "anim.h"
#include "cursor.h"

#include "../arch/x86_64/apic.h"
#include "../gpu/display.h"
#include "../kprintf.h"
#include "../sched/thread.h"

#include <stdint.h>
#include <stddef.h>

static struct surface *slots[COMPOSITOR_MAX_SURFACES];
static int             slot_count;

/* Frame-time stats. Writers: compositor thread only. Readers via
 * compositor_get_stats() take a consistent snapshot with cli/sti. */
static volatile uint64_t cs_frame_count;
static volatile uint32_t cs_last_us, cs_avg_us, cs_max_us, cs_drops;

void compositor_init(void) {
    for (int i = 0; i < COMPOSITOR_MAX_SURFACES; i++) slots[i] = NULL;
    slot_count = 0;
    kprintf("[compositor] init, %d surface slots\n", COMPOSITOR_MAX_SURFACES);
}

int compositor_add(struct surface *s) {
    if (!s) return -1;
    for (int i = 0; i < slot_count; i++) {
        if (slots[i] == s) return 0;
    }
    if (slot_count >= COMPOSITOR_MAX_SURFACES) return -1;
    slots[slot_count++] = s;
    return 0;
}

void compositor_remove(struct surface *s) {
    for (int i = 0; i < slot_count; i++) {
        if (slots[i] == s) {
            for (int j = i; j < slot_count - 1; j++) slots[j] = slots[j + 1];
            slots[--slot_count] = NULL;
            return;
        }
    }
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

void compositor_frame(uint32_t bg_xrgb) {
    const struct display *dd = display_get();
    if (!dd || !dd->pixels) return;

    struct blit_dst dst = {
        .pixels = dd->pixels,
        .width  = dd->width,
        .height = dd->height,
        .stride = dd->pitch / 4u,
    };

    blit_fill(&dst, 0, 0, (int32_t) dd->width, (int32_t) dd->height, bg_xrgb);

    int idx[COMPOSITOR_MAX_SURFACES];
    sort_indices(idx);

    for (int i = 0; i < slot_count; i++) {
        struct surface *s = slots[idx[i]];
        if (!s || !s->pixels || !s->visible || s->alpha == 0) continue;

        struct blit_src src = {
            .pixels = s->pixels,
            .width  = s->width,
            .height = s->height,
            .stride = s->width,
        };
        if (s->opaque) blit_copy(&dst, s->x, s->y, &src);
        else           blit_alpha(&dst, s->x, s->y, &src, s->alpha);
    }

    display_present();
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

    /* Measured dt keeps animation speed wall-clock-accurate even when
     * frame times drift (virtio_gpu flushes, thread preemption). Start
     * with the target step; the first real tick replaces it. */
    uint64_t prev_tsc = rdtsc();
    fx16     target_dt = fx_div(FX_ONE, FX_FROM_INT(thread_args.target_hz));
    fx16     dt_cap    = target_dt * 2;
    fx16     dt_fx     = target_dt;

    uint64_t next    = timer_ticks + fpt;
    uint32_t window_count = 0;
    uint64_t window_sum   = 0;

    for (;;) {
        while ((uint64_t) timer_ticks < next) thread_yield();

        uint64_t frame_tsc = rdtsc();
        {
            uint64_t elapsed = frame_tsc - prev_tsc;
            /* seconds * ONE = (cycles * ONE) / tsc_hz; cap at 2× target
             * so a long stall doesn't teleport animations. */
            fx16 measured = (fx16) ((elapsed * (uint64_t) FX_ONE) / tsc_hz);
            if (measured <= 0)     measured = 1;
            if (measured > dt_cap) measured = dt_cap;
            dt_fx = measured;
            prev_tsc = frame_tsc;
        }

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
            kprintf("[compositor] fps=%u last=%uus avg=%uus max=%uus drops=%u\n",
                    (unsigned) thread_args.target_hz,
                    (unsigned) cs_last_us,
                    (unsigned) cs_avg_us,
                    (unsigned) cs_max_us,
                    (unsigned) cs_drops);
            cs_max_us = 0;
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
    out->frame_count = cs_frame_count;
    out->last_us     = cs_last_us;
    out->avg_us      = cs_avg_us;
    out->max_us      = cs_max_us;
    out->drops       = cs_drops;
}
