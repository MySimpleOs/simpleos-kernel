#include "display.h"
#include "virtio_gpu.h"

#include "../kprintf.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

static struct display dsp;

static void limine_present(void) {
    /* Direct framebuffer writes are visible immediately; no host-side
     * copy needed. Single buffer, no swap. */
}

static void virtio_present(void) {
    virtio_gpu_present();
    /* Single-buffer path now: dsp.pixels points at buffer 0 and never
     * changes, but keep the assignment so a future triple-buffer or
     * ring-buffer backend can swap freely without touching callers. */
    dsp.pixels = virtio_gpu_backbuffer();
}

void display_init(void) {
    if (virtio_gpu_ready()) {
        dsp.pixels          = virtio_gpu_backbuffer();
        dsp.width           = virtio_gpu_width();
        dsp.height          = virtio_gpu_height();
        dsp.pitch           = virtio_gpu_pitch();
        dsp.double_buffered = 0;
        dsp.present         = virtio_present;
        kprintf("[display] backend=virtio-gpu %ux%u pitch=%u single-buffer\n",
                (unsigned) dsp.width, (unsigned) dsp.height, (unsigned) dsp.pitch);
        return;
    }

    if (framebuffer_request.response
        && framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb =
            framebuffer_request.response->framebuffers[0];
        dsp.pixels          = (uint32_t *) fb->address;
        dsp.width           = (uint32_t) fb->width;
        dsp.height          = (uint32_t) fb->height;
        dsp.pitch           = (uint32_t) fb->pitch;
        dsp.double_buffered = 0;
        dsp.present         = limine_present;
        kprintf("[display] backend=limine-fb %ux%u pitch=%u single-buffered\n",
                (unsigned) dsp.width, (unsigned) dsp.height, (unsigned) dsp.pitch);
        return;
    }

    dsp.pixels          = NULL;
    dsp.double_buffered = 0;
    dsp.present         = limine_present;
    kprintf("[display] no backend available\n");
}

const struct display *display_get(void) { return &dsp; }

void display_present(void) {
    if (dsp.present) dsp.present();
}
