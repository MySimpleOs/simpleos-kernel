#pragma once

#include <stdint.h>
#include <stddef.h>

/* Kernel's view of a 2D display backend. Pixel format is always 32-bit
 * XRGB, pitch in bytes. `pixels` points at the current *back* buffer —
 * after display_present() returns, the field is updated to point at the
 * next back buffer (the old back is now on scanout). Callers that draw
 * across frames must re-read pixels from display_get() after every
 * present. */
struct display {
    uint32_t *pixels;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;
    int       double_buffered;            /* 1 if present() flips buffers  */
    void    (*present)(void);
};

void                 display_init(void);
const struct display *display_get(void);

/* Flush the current back buffer to the scanout and swap roles. No-op for
 * single-buffer backends (Limine framebuffer fallback). */
void                 display_present(void);
