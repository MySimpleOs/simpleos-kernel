#pragma once

#include "../pci/pci.h"

#include <stdint.h>
#include <stddef.h>

/* Bring up the VirtIO-GPU as a single-buffer scanout source. One
 * full-screen XRGB resource is created, bound to scanout 0 once at
 * init, and every virtio_gpu_present() pushes the guest backing to the
 * host-side copy + flushes the refresh. A second resource/backing is
 * allocated but idle — reserved for a future triple-buffer path. */
int virtio_gpu_init(struct pci_device *pci);

int       virtio_gpu_ready(void);

/* Drawing target: guest writes land here and will be visible at the next
 * virtio_gpu_present*(). With single-buffer scanout this pointer is
 * stable across frames, but callers should keep re-reading it to stay
 * forward-compatible with a future ring-buffer backend. */
uint32_t *virtio_gpu_backbuffer(void);

uint32_t  virtio_gpu_width(void);
uint32_t  virtio_gpu_height(void);
uint32_t  virtio_gpu_pitch(void);

/* Full-screen present: transfer + flush the entire scanout. */
void      virtio_gpu_present(void);

/* Damage-aware present: transfer + flush only the given rect. (x, y)
 * top-left + (w, h) in pixels; clipped to the scanout bounds. No-op when
 * the clipped rect is empty. */
void      virtio_gpu_present_rect(int32_t x, int32_t y,
                                  uint32_t w, uint32_t h);
