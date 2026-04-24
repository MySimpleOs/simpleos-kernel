#pragma once

#include "../compositor/damage.h"
#include "../compositor/rect.h"

#include <stdint.h>
#include <stddef.h>

/* Kernel's view of the CPU-rendered 2D display.
 *
 * `pixels` is the compositor's write target — a software back buffer.
 * Compose goes here, then display_present[_rect]() publishes to the
 * host-visible Limine framebuffer (IRQ-off memcpy + sfence).
 *
 * Pixel format: 32-bit XRGB (little-endian: B G R X bytes).
 * `pitch` is bytes per row of `pixels` (tight width*4 rows);
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

/* Publish the entire back buffer to the hardware framebuffer. */
void                 display_present(void);

/* Publish only `r` (in display coords). Cheapest path the compositor
 * takes: one rect = one tight memcpy, bounded by damage bbox. */
void                 display_present_rect(struct rect r);

/* Copy every damage rect from the compositor back buffer to scanout under
 * a single IRQ disable (avoids stale “holes” when the union bbox is wider
 * than the actual damaged pixels). */
void                 display_present_damage(const struct damage *dmg);

/* When display_policy.vsync is set and tsc_hz is known, yield until the
 * next nominal 1/refresh_hz boundary (TSC pacing; not a hardware scanout IRQ). */
void                 display_vsync_wait_after_present(void);
