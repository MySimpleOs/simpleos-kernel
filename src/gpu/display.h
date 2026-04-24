#pragma once

#include "../compositor/rect.h"

#include <stdint.h>
#include <stddef.h>

/* Kernel's view of the CPU-rendered 2D display.
 *
 * `pixels` is the compositor's write target — always a software shadow
 * buffer. Compose goes here, then display_present[_rect]() publishes the
 * shadow onto the host-visible Limine framebuffer in one IRQ-off
 * rep-movsq memcpy + sfence. The shadow-then-publish model is what
 * kept moving surfaces tear-free at 120 Hz (Faz 12.5.4).
 *
 * Pixel format: 32-bit XRGB (little-endian: B G R X bytes).
 * `pitch` is bytes per row of `pixels` (the shadow buffer), i.e. width*4;
 * the hardware framebuffer may have a larger stride — that is internal
 * to display.c for present(). Compositor stride in pixels is pitch/4. */
struct display {
    uint32_t *pixels;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;
    void    (*present)(void);
    void    (*present_rect)(struct rect r);
};

void                 display_init(void);
const struct display *display_get(void);

/* Publish the entire shadow to the hardware framebuffer. */
void                 display_present(void);

/* Publish only `r` (in display coords). Cheapest path the compositor
 * takes: one rect = one tight memcpy, bounded by damage bbox. */
void                 display_present_rect(struct rect r);
