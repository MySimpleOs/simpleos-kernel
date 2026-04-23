#pragma once

#include "../pci/pci.h"

#include <stdint.h>
#include <stddef.h>

/* Bring up the VirtIO-GPU as a double-buffered scanout source. Two
 * full-screen XRGB resources (A and B) are created and each gets its
 * own contiguous backing. Scanout starts on A while the guest writes
 * into B; virtio_gpu_present() transfers B, switches scanout to B,
 * flushes, and swaps the roles for the next frame. */
int virtio_gpu_init(struct pci_device *pci);

int       virtio_gpu_ready(void);

/* Current back buffer: CPU writes land here and will be shown at the
 * next virtio_gpu_present(). After present() the returned pointer is
 * the OLD buffer (now on scanout); call virtio_gpu_backbuffer() again
 * to fetch the new back. */
uint32_t *virtio_gpu_backbuffer(void);

uint32_t  virtio_gpu_width(void);
uint32_t  virtio_gpu_height(void);
uint32_t  virtio_gpu_pitch(void);

void      virtio_gpu_present(void);
