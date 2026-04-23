/* SimpleOS kernel entry — Faz 3 "Hello, framebuffer".
 *
 * Paints the framebuffer a dark background and draws a centered square so a
 * successful Limine → kernel handoff is visible at a glance, then halts.
 */

#include <stddef.h>
#include <stdint.h>
#include <limine.h>

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile uint64_t limine_base_revision[3];

static void hang(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static void fill_rect(uint32_t *pixels, size_t stride,
                      size_t x0, size_t y0,
                      size_t w,  size_t h,
                      uint32_t color) {
    for (size_t y = y0; y < y0 + h; y++) {
        for (size_t x = x0; x < x0 + w; x++) {
            pixels[y * stride + x] = color;
        }
    }
}

void kmain(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        hang();
    }

    if (framebuffer_request.response == NULL
        || framebuffer_request.response->framebuffer_count < 1) {
        hang();
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

    uint32_t *pixels = (uint32_t *) fb->address;
    size_t stride = (size_t) (fb->pitch / 4);
    size_t width  = (size_t) fb->width;
    size_t height = (size_t) fb->height;

    fill_rect(pixels, stride, 0, 0, width, height, 0xff0a1e3c);

    size_t box = 200;
    if (width > box && height > box) {
        size_t x0 = (width  - box) / 2;
        size_t y0 = (height - box) / 2;
        fill_rect(pixels, stride, x0, y0, box, box, 0xffff5533);
    }

    hang();
}
