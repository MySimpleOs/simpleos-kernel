#pragma once

#include "../compositor/rect.h"

#include <stdint.h>
#include <stddef.h>

/* Kernel's view of a 2D display backend. Pixel format is always 32-bit
 * XRGB, pitch in bytes. `pixels` points at the current back buffer — for
 * the single-buffer scanout backend it never changes, but keep
 * re-reading from display_get() to stay forward-compatible with a
 * future ring-buffer backend.
 *
 * `present_rect` is an optional backend entry point that pushes only a
 * sub-region of the back buffer to the scanout. Backends that don't
 * support partial present leave it NULL; the public wrapper falls back
 * to the full-screen `present` in that case. */
struct display {
    uint32_t *pixels;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;
    int       double_buffered;            /* 1 if present() flips buffers  */
    void    (*present)(void);
    void    (*present_rect)(struct rect r);
};

void                 display_init(void);
const struct display *display_get(void);

/* Flush the entire back buffer to the scanout. No-op for single-buffer
 * backends (Limine framebuffer fallback). */
void                 display_present(void);

/* Flush only `r` (in display coords) to the scanout. Falls back to a
 * full present() when the backend has no partial-present path. */
void                 display_present_rect(struct rect r);
