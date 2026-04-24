#include "display.h"
#include "display_policy.h"

#include "../arch/x86_64/apic.h"
#include "../drivers/mouse.h"
#include "../kprintf.h"
#include "../mm/heap.h"
#include "../panic.h"
#include "../sched/thread.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

static struct display dsp;

/* Software compositor back buffer. Compose writes here; present copies to
 * the host-visible framebuffer (IRQ-off memcpy + sfence) so scanout
 * never sees a half-updated frame.
 *
 * Faz 12.5.5: Limine FB is the permanent CPU-composited 2D path (virtio-gpu
 * backend removed). */
static uint32_t *compos_buf     = NULL;
static uint32_t *hw_pixels    = NULL;    /* real front buffer              */
static uint32_t  compos_stride  = 0;       /* pixels per row (tight width)   */
static uint32_t  hw_stride      = 0;

/* Scoped interrupt disable. Publish MUST be atomic from the host-side
 * display's point of view: if the kernel thread driving the memcpy
 * gets preempted halfway, the host sees (top half = new frame,
 * bottom half = old frame) for a few milliseconds until we resume.
 * That is exactly the "bottom flickers, top is stable" symptom users
 * hit on moving surfaces. Keeping IF down for 1-3 ms per frame is
 * cheap relative to the visual fix. */
static inline uint64_t irq_save(void) {
    uint64_t rflags;
    __asm__ volatile ("pushfq\n\tpopq %0\n\tcli" : "=r"(rflags) :: "memory");
    return rflags;
}

static inline void irq_restore(uint64_t rflags) {
    __asm__ volatile ("pushq %0\n\tpopfq" :: "r"(rflags) : "cc", "memory");
}

/* rep-movsq row copy. Kernel is -mgeneral-regs-only (no SSE), so SIMD
 * is off the table; rep movsq saturates the write-combining path on
 * every x86 we care about, and on ERMS-capable CPUs (everything after
 * Ivy Bridge) it's close to memory-bandwidth-bound. 4-byte pixel rows
 * are always 4-byte aligned; we copy 8 bytes per iteration and fall
 * back to a single movsd for the odd trailing pixel. */
static inline void row_copy_u32(uint32_t *dst, const uint32_t *src, uint32_t npx) {
    uint64_t       *d  = (uint64_t *) dst;
    const uint64_t *s  = (const uint64_t *) src;
    uint64_t        qw = npx >> 1;
    uint32_t        tail = npx & 1;
    __asm__ volatile (
        "rep movsq"
        : "+D"(d), "+S"(s), "+c"(qw)
        :: "memory"
    );
    if (tail) {
        uint32_t       *d32 = (uint32_t *) d;
        const uint32_t *s32 = (const uint32_t *) s;
        *d32 = *s32;
    }
}

static void limine_present(void) {
    if (!compos_buf || !hw_pixels) return;
    const uint32_t h = dsp.height;
    const uint32_t w = dsp.width;
    const uint32_t *sp = compos_buf;
    uint32_t       *hp = hw_pixels;
    uint64_t flags = irq_save();
    for (uint32_t y = 0; y < h; y++) {
        row_copy_u32(hp, sp, w);
        sp += compos_stride;
        hp += hw_stride;
    }
    __asm__ volatile ("sfence" ::: "memory");
    irq_restore(flags);
}

static int limine_clamp_rect(struct rect r, int32_t *x0, int32_t *y0,
                             int32_t *x1, int32_t *y1) {
    if (!compos_buf || !hw_pixels) return 0;
    if (r.w <= 0 || r.h <= 0) return 0;
    *x0 = r.x > 0 ? r.x : 0;
    *y0 = r.y > 0 ? r.y : 0;
    *x1 = r.x + r.w < (int32_t) dsp.width  ? r.x + r.w : (int32_t) dsp.width;
    *y1 = r.y + r.h < (int32_t) dsp.height ? r.y + r.h : (int32_t) dsp.height;
    if (*x1 <= *x0 || *y1 <= *y0) return 0;
    return 1;
}

static void limine_present_rect_core(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    uint32_t rw = (uint32_t) (x1 - x0);
    for (int32_t y = y0; y < y1; y++) {
        const uint32_t *sp = compos_buf + (uint32_t) y * compos_stride + (uint32_t) x0;
        uint32_t       *hp = hw_pixels + (uint32_t) y * hw_stride     + (uint32_t) x0;
        row_copy_u32(hp, sp, rw);
    }
}

static void limine_present_rect(struct rect r) {
    int32_t x0, y0, x1, y1;
    if (!limine_clamp_rect(r, &x0, &y0, &x1, &y1)) return;
    uint64_t flags = irq_save();
    limine_present_rect_core(x0, y0, x1, y1);
    __asm__ volatile ("sfence" ::: "memory");
    irq_restore(flags);
}

void display_present_damage(const struct damage *dmg) {
    if (!compos_buf || !hw_pixels || !dmg) return;
    uint64_t flags = irq_save();
    for (int i = 0; i < dmg->count; i++) {
        int32_t x0, y0, x1, y1;
        if (!limine_clamp_rect(dmg->rects[i], &x0, &y0, &x1, &y1)) continue;
        limine_present_rect_core(x0, y0, x1, y1);
    }
    __asm__ volatile ("sfence" ::: "memory");
    irq_restore(flags);
}

void display_init(void) {
    if (!framebuffer_request.response
        || framebuffer_request.response->framebuffer_count == 0) {
        dsp.pixels       = NULL;
        dsp.present      = NULL;
        dsp.present_rect = NULL;
        kprintf("[display] no framebuffer from Limine — display is dark\n");
        return;
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    uint32_t fbw = (uint32_t) fb->width;
    uint32_t fbh = (uint32_t) fb->height;

    const struct display_policy *pol = display_policy_get();
    uint32_t dw = fbw, dh = fbh;
    if (pol->from_file) {
        dw = pol->width;
        dh = pol->height;
        if (dw > fbw || dh > fbh) {
            kprintf("[display] policy %ux%u exceeds scanout %ux%u — clamping\n",
                    (unsigned) dw, (unsigned) dh,
                    (unsigned) fbw, (unsigned) fbh);
            if (dw > fbw) dw = fbw;
            if (dh > fbh) dh = fbh;
        }
    }
    dsp.width  = dw;
    dsp.height = dh;

    uint32_t hw_pitch_bytes = (uint32_t) fb->pitch;
    hw_pixels     = (uint32_t *) fb->address;
    hw_stride     = hw_pitch_bytes / 4u;
    compos_stride = dsp.width;

    size_t bytes = (size_t) dsp.width * (size_t) dsp.height * 4u;
    compos_buf = (uint32_t *) kmalloc(bytes);
    if (!compos_buf) panic("display: compositor buffer kmalloc failed");
    for (size_t i = 0; i < (size_t) dsp.width * (size_t) dsp.height; i++) compos_buf[i] = 0;

    dsp.pixels       = compos_buf;
    /* Rows are width pixels; do not use HW pitch for compositor indexing. */
    dsp.pitch        = dsp.width * 4u;
    dsp.present      = limine_present;
    dsp.present_rect = limine_present_rect;

    {
        const struct display_policy *p = display_policy_get();
        kprintf("[display] limine scanout %ux%u hw_pitch=%u; compositor %ux%u row=%u bytes\n",
                (unsigned) fbw, (unsigned) fbh,
                (unsigned) hw_pitch_bytes,
                (unsigned) dsp.width, (unsigned) dsp.height,
                (unsigned) dsp.pitch);
        kprintf("[display] policy %ux%u @ %u Hz (%s)%s\n",
                (unsigned) p->width, (unsigned) p->height,
                (unsigned) p->refresh_hz, p->label,
                p->from_file ? " (from /etc/display.conf)" : " (defaults until initrd)");
        if (p->from_file && (p->width != dsp.width || p->height != dsp.height))
            kprintf("[display] note: compositor size clamped to scanout\n");
    }
    if (dsp.width && dsp.height)
        mouse_set_screen(dsp.width, dsp.height);
}

const struct display *display_get(void) { return &dsp; }

void display_present(void) {
    if (dsp.present) dsp.present();
}

void display_present_rect(struct rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    if (dsp.present_rect) { dsp.present_rect(r); return; }
    if (dsp.present)      { dsp.present(); }
}

static uint64_t vsync_next_tsc;

void display_vsync_wait_after_present(void) {
    const struct display_policy *pol = display_policy_get();
    if (!pol->vsync || !tsc_hz) return;

    uint32_t hz = pol->refresh_hz;
    if (hz < 30) hz = 30;
    if (hz > 360) hz = 360;
    uint64_t per = tsc_hz / (uint64_t) hz;
    if (per < 2000ull) per = 2000ull;

    uint64_t now = rdtsc();
    if (vsync_next_tsc == 0)
        vsync_next_tsc = now + per;

    while (now >= vsync_next_tsc)
        vsync_next_tsc += per;

    while (rdtsc() < vsync_next_tsc)
        thread_yield();

    vsync_next_tsc += per;
    now = rdtsc();
    while (now >= vsync_next_tsc)
        vsync_next_tsc += per;
}
