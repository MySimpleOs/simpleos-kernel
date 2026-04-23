#include "display.h"
#include "virtio_gpu.h"

#include "../kprintf.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

static struct display dsp;

static void limine_present(void) { /* direct framebuffer, nothing to do */ }
static void virtio_present(void)  { virtio_gpu_present(); }

void display_init(void) {
    /* Prefer VirtIO-GPU when it's live — kernel owns the scanout and
     * future compositor effects can use the abstract present(). Fall back
     * to Limine's linear framebuffer so bring-up never regresses. */
    if (virtio_gpu_ready()) {
        dsp.pixels  = virtio_gpu_backbuffer();
        dsp.width   = virtio_gpu_width();
        dsp.height  = virtio_gpu_height();
        dsp.pitch   = virtio_gpu_pitch();
        dsp.present = virtio_present;
        kprintf("[display] backend=virtio-gpu %ux%u pitch=%u\n",
                (unsigned) dsp.width, (unsigned) dsp.height, (unsigned) dsp.pitch);
        return;
    }

    if (framebuffer_request.response
        && framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb =
            framebuffer_request.response->framebuffers[0];
        dsp.pixels  = (uint32_t *) fb->address;
        dsp.width   = (uint32_t) fb->width;
        dsp.height  = (uint32_t) fb->height;
        dsp.pitch   = (uint32_t) fb->pitch;
        dsp.present = limine_present;
        kprintf("[display] backend=limine-fb %ux%u pitch=%u\n",
                (unsigned) dsp.width, (unsigned) dsp.height, (unsigned) dsp.pitch);
        return;
    }

    dsp.pixels  = NULL;
    dsp.present = limine_present;
    kprintf("[display] no backend available\n");
}

const struct display *display_get(void) { return &dsp; }

void display_present(void) {
    if (dsp.present) dsp.present();
}
