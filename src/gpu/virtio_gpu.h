#pragma once

#include "../pci/pci.h"

#include <stdint.h>
#include <stddef.h>

/* Probe and bring up the VirtIO-GPU device at the given PCI address, create
 * a full-screen 32-bit RGBA resource with a kernel-backed linear buffer,
 * and set it as scanout 0. On success, virtio_gpu_present() on the
 * exposed backing buffer is what the display layer calls to push a frame.
 *
 * Returns 0 on success, non-zero if the device is missing, negotiation
 * fails, or any command returns an error response. */
int virtio_gpu_init(struct pci_device *pci);

/* True once init succeeded and the backing buffer is usable. */
int virtio_gpu_ready(void);

/* Linear backing buffer: 32-bit pixels, `pitch` bytes per row. Write into
 * this buffer freely, then call virtio_gpu_present() to publish. */
uint32_t *virtio_gpu_backbuffer(void);
uint32_t  virtio_gpu_width(void);
uint32_t  virtio_gpu_height(void);
uint32_t  virtio_gpu_pitch(void);

/* Copy the backing buffer to the host resource and flush the scanout. */
void virtio_gpu_present(void);
