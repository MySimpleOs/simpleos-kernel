#include "display.h"

#include "../kprintf.h"
#include "../mm/heap.h"
#include "../panic.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

static struct display dsp;

/* Software shadow buffer. Compose always writes here, never into the
 * host-visible framebuffer. Every present() publishes the shadow to
 * the hardware surface in one IRQ-off memcpy followed by sfence. This
 * guarantees the host-visible surface is only ever updated in a single
 * contiguous operation per frame — no CPU-midway-through-compose
 * tearing, regardless of what refresh thread QEMU (or real hardware)
 * runs underneath us.
 *
 * Faz 12.5.5: the virtio-gpu backend was removed. CPU-composited 2D
 * straight onto the Limine framebuffer is the permanent render model;
 * GPU shader paths are explicitly out of scope for the foreseeable
 * future (see ROADMAP "Grafik & kompozisyon katmanı"). */
static uint32_t *shadow        = NULL;
static uint32_t *hw_pixels     = NULL;    /* real front buffer             */
static uint32_t  shadow_stride = 0;       /* pixels per row                */
static uint32_t  hw_stride     = 0;

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
    if (!shadow || !hw_pixels) return;
    const uint32_t h = dsp.height;
    const uint32_t w = dsp.width;
    const uint32_t *sp = shadow;
    uint32_t       *hp = hw_pixels;
    uint64_t flags = irq_save();
    for (uint32_t y = 0; y < h; y++) {
        row_copy_u32(hp, sp, w);
        sp += shadow_stride;
        hp += hw_stride;
    }
    __asm__ volatile ("sfence" ::: "memory");
    irq_restore(flags);
}

static void limine_present_rect(struct rect r) {
    if (!shadow || !hw_pixels) return;
    if (r.w <= 0 || r.h <= 0) return;
    int32_t x0 = r.x > 0 ? r.x : 0;
    int32_t y0 = r.y > 0 ? r.y : 0;
    int32_t x1 = r.x + r.w < (int32_t) dsp.width  ? r.x + r.w : (int32_t) dsp.width;
    int32_t y1 = r.y + r.h < (int32_t) dsp.height ? r.y + r.h : (int32_t) dsp.height;
    if (x1 <= x0 || y1 <= y0) return;

    uint32_t rw    = (uint32_t) (x1 - x0);
    uint64_t flags = irq_save();
    for (int32_t y = y0; y < y1; y++) {
        const uint32_t *sp = shadow    + (uint32_t) y * shadow_stride + (uint32_t) x0;
        uint32_t       *hp = hw_pixels + (uint32_t) y * hw_stride     + (uint32_t) x0;
        row_copy_u32(hp, sp, rw);
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
    dsp.width  = (uint32_t) fb->width;
    dsp.height = (uint32_t) fb->height;
    dsp.pitch  = (uint32_t) fb->pitch;

    hw_pixels     = (uint32_t *) fb->address;
    hw_stride     = dsp.pitch / 4u;
    shadow_stride = dsp.width;                       /* tight-packed        */

    size_t bytes = (size_t) dsp.width * (size_t) dsp.height * 4u;
    shadow = (uint32_t *) kmalloc(bytes);
    if (!shadow) panic("display: shadow kmalloc failed");
    for (size_t i = 0; i < (size_t) dsp.width * (size_t) dsp.height; i++) shadow[i] = 0;

    dsp.pixels       = shadow;
    dsp.present      = limine_present;
    dsp.present_rect = limine_present_rect;
    kprintf("[display] limine-fb %ux%u pitch=%u shadow-buffered\n",
            (unsigned) dsp.width, (unsigned) dsp.height, (unsigned) dsp.pitch);
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
