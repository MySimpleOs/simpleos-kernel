#include "parallel.h"
#include "surface.h"

#include "../arch/x86_64/hypervisor.h"
#include "../arch/x86_64/smp.h"

#include <stdint.h>

/* Shared work context. Written by BSP before bumping `epoch`; APs read
 * it with acquire ordering paired to that release. Only atomic fields
 * change while APs are actively working. */
struct compose_ctx {
    struct blit_dst       dst;
    const struct damage  *dmg;
    struct surface      **z_sorted;
    int                   surface_count;
    uint32_t              bg;

    struct rect           bands[PARALLEL_MAX_BANDS];
    uint32_t              band_count;

    volatile uint32_t     epoch;          /* bumped once per frame      */
    volatile uint32_t     next_tile;      /* atomic claim counter       */
    volatile uint32_t     done_count;     /* APs increment on exit      */
};

static struct compose_ctx ctx;

static volatile uint32_t bsp_tiles_total;
static volatile uint32_t ap_tiles_total;
static volatile uint32_t last_frame_cpus;

/* BSP is inside the AP barrier; timer IRQ must not preempt into
 * thread_yield() here or barrier progress stalls (~1 FPS). */
static volatile int parallel_barrier_active;

/* Compose one band: for each damage rect, intersect with band, clear
 * bg, then blit every surface that overlaps the intersection (z-sorted
 * bottom-to-top). Band ∩ rect keeps the scissor tight. */
static void compose_band(uint32_t band_idx) {
    if (band_idx >= ctx.band_count) return;
    struct rect band = ctx.bands[band_idx];

    for (int ri = 0; ri < ctx.dmg->count; ri++) {
        struct rect sub;
        if (!rect_intersect(&ctx.dmg->rects[ri], &band, &sub)) continue;

        blit_fill_scissor(&ctx.dst, &sub, sub.x, sub.y, sub.w, sub.h, ctx.bg);

        for (int i = 0; i < ctx.surface_count; i++) {
            struct surface *s = ctx.z_sorted[i];
            if (!s || !s->pixels)                    continue;
            if (!s->visible || s->alpha == 0)        continue;

            struct rect srect = rect_make(s->x, s->y,
                                          (int32_t) s->width,
                                          (int32_t) s->height);
            if (!rect_overlaps(&srect, &sub)) continue;

            struct blit_src src = {
                .pixels = s->pixels,
                .width  = s->width,
                .height = s->height,
                .stride = s->width,
            };
            if (s->corner_radius) {
                if (s->opaque)
                    blit_copy_rounded_scissor (&ctx.dst, &sub, s->x, s->y,
                                               &src, s->corner_radius);
                else
                    blit_alpha_rounded_scissor(&ctx.dst, &sub, s->x, s->y,
                                               &src, s->alpha,
                                               s->corner_radius);
            } else {
                if (s->opaque) blit_copy_scissor (&ctx.dst, &sub, s->x, s->y, &src);
                else           blit_alpha_scissor(&ctx.dst, &sub, s->x, s->y, &src, s->alpha);
            }
        }
    }
}

/* AP worker loop. Called from ap_entry (smp.c) right after the AP
 * comes online. Spins on `epoch` waiting for the next frame; when
 * epoch changes, acquire-loads the context fields BSP set, claims
 * tiles via atomic fetch-add, and bumps done_count when its share
 * is empty. Never returns. */
void compositor_ap_worker(uint32_t cpu_id) {
    (void) cpu_id;
    uint32_t seen_epoch = 0;
    uint32_t my_tiles   = 0;
    for (;;) {
        while (__atomic_load_n(&ctx.epoch, __ATOMIC_ACQUIRE) == seen_epoch) {
            __asm__ volatile ("pause");
        }
        seen_epoch = __atomic_load_n(&ctx.epoch, __ATOMIC_ACQUIRE);

        for (;;) {
            uint32_t t = __atomic_fetch_add(&ctx.next_tile, 1, __ATOMIC_RELAXED);
            if (t >= ctx.band_count) break;
            compose_band(t);
            my_tiles++;
        }
        __atomic_add_fetch(&ap_tiles_total, my_tiles, __ATOMIC_RELAXED);
        my_tiles = 0;

        __atomic_add_fetch(&ctx.done_count, 1, __ATOMIC_ACQ_REL);
    }
}

int parallel_compose_active(void) {
    return __atomic_load_n(&parallel_barrier_active, __ATOMIC_ACQUIRE);
}

void parallel_compose_idle_barrier(void) {
    uint64_t cpus = smp_online_count();
    if (cpus < 1) cpus = 1;
    if (hypervisor_is_virtualbox())
        cpus = 1;
    uint64_t n_aps = cpus > 0 ? cpus - 1 : 0;

    ctx.band_count     = 0;
    ctx.dmg            = NULL;
    ctx.z_sorted       = NULL;
    ctx.surface_count  = 0;
    __atomic_store_n(&ctx.next_tile,  1, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx.done_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&parallel_barrier_active, 1, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_add_fetch(&ctx.epoch, 1, __ATOMIC_ACQ_REL);

    /* compose_band(0) returns immediately when band_count == 0 */
    compose_band(0);
    for (;;) {
        uint32_t t = __atomic_fetch_add(&ctx.next_tile, 1, __ATOMIC_RELAXED);
        if (t >= ctx.band_count) break;
        compose_band(t);
    }

    while (__atomic_load_n(&ctx.done_count, __ATOMIC_ACQUIRE) < n_aps)
        __asm__ volatile ("pause");

    __atomic_store_n(&parallel_barrier_active, 0, __ATOMIC_RELEASE);
}

void parallel_compose(struct blit_dst dst,
                      const struct damage *dmg,
                      struct surface **z_sorted,
                      int surface_count,
                      uint32_t bg) {
    if (!dmg || dmg->count == 0) return;

    uint64_t cpus = smp_online_count();
    if (cpus < 1) cpus = 1;
    /* VirtualBox: multi-AP compose + framebuffer WC/MMIO is much slower and
     * historically hit barrier/timer edge cases (~few FPS). QEMU/KVM OK. */
    if (hypervisor_is_virtualbox())
        cpus = 1;

    struct rect bbox = damage_bbox(dmg);
    if (bbox.w <= 0 || bbox.h <= 0) return;

    uint32_t N = (uint32_t) cpus;
    if (N > PARALLEL_MAX_BANDS) N = PARALLEL_MAX_BANDS;

    /* Don't split so fine that per-band overhead eats the parallel
     * speedup. 32 rows per band is a reasonable cut-off for naive C
     * blit at ~GB/s memory-bw ceilings. */
    const int32_t min_rows = 32;
    uint32_t cap_by_h = (uint32_t) (bbox.h / min_rows);
    if (cap_by_h < 1u) cap_by_h = 1u;
    if (N > cap_by_h) N = cap_by_h;

    for (uint32_t i = 0; i < N; i++) {
        int32_t y0 = bbox.y + (int32_t)(((int64_t) bbox.h * i) / N);
        int32_t y1 = bbox.y + (int32_t)(((int64_t) bbox.h * (i + 1)) / N);
        ctx.bands[i] = rect_make(bbox.x, y0, bbox.w, y1 - y0);
    }
    ctx.band_count    = N;
    ctx.dst           = dst;
    ctx.dmg           = dmg;
    ctx.z_sorted      = z_sorted;
    ctx.surface_count = surface_count;
    ctx.bg            = bg;

    __atomic_store_n(&ctx.next_tile,  1, __ATOMIC_RELAXED);  /* BSP takes 0 */
    __atomic_store_n(&ctx.done_count, 0, __ATOMIC_RELAXED);

    __atomic_store_n(&parallel_barrier_active, 1, __ATOMIC_RELEASE);

    /* Release fence: all plain stores to ctx must be visible before any
     * AP observes the new epoch (pairs with acquire load in AP loop). */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    /* Publishing epoch makes every context field visible to APs. */
    __atomic_add_fetch(&ctx.epoch, 1, __ATOMIC_ACQ_REL);

    /* BSP does tile 0 first, then steals. */
    uint32_t bsp_tiles = 0;
    compose_band(0);
    bsp_tiles++;
    for (;;) {
        uint32_t t = __atomic_fetch_add(&ctx.next_tile, 1, __ATOMIC_RELAXED);
        if (t >= ctx.band_count) break;
        compose_band(t);
        bsp_tiles++;
    }
    __atomic_add_fetch(&bsp_tiles_total, bsp_tiles, __ATOMIC_RELAXED);

    /* Wait for every AP to post done. n_aps is CPUs minus BSP. */
    uint64_t n_aps = cpus > 0 ? cpus - 1 : 0;
    while (__atomic_load_n(&ctx.done_count, __ATOMIC_ACQUIRE) < n_aps) {
        __asm__ volatile ("pause");
    }

    __atomic_store_n(&parallel_barrier_active, 0, __ATOMIC_RELEASE);

    /* How many CPUs actually did any work? At least BSP. APs counted
     * iff they managed to claim a tile. Rough: non-zero work_done_count
     * means they all fought for tiles — approximate with band_count. */
    last_frame_cpus = (N < (uint32_t) cpus) ? N : (uint32_t) cpus;
}

void parallel_get_stats(struct parallel_stats *out) {
    if (!out) return;
    out->bsp_tiles  = __atomic_load_n(&bsp_tiles_total, __ATOMIC_RELAXED);
    out->ap_tiles   = __atomic_load_n(&ap_tiles_total,  __ATOMIC_RELAXED);
    out->frame_cpus = last_frame_cpus;
}
