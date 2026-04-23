#pragma once

#include <stdint.h>
#include <stddef.h>

/* Kernel's view of a 2D display. The backend is either the Limine linear
 * framebuffer (direct writes, no explicit flush) or a virt-GPU-backed
 * surface where every change must be TRANSFER + FLUSH'd. Everything the
 * console / future compositor does goes through these three functions.
 *
 * The pixel format is always 32-bit XRGB, pitch in bytes. */
struct display {
    uint32_t *pixels;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;                         /* bytes per row              */
    void    (*present)(void);                /* NULL for direct backends    */
};

void  display_init(void);
const struct display *display_get(void);

/* Cheap request to push a frame. Direct backends ignore it; virt-GPU does
 * the transfer + flush commands. */
void  display_present(void);
