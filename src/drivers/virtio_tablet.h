#pragma once

#include <stdint.h>

/* virtio-tablet-pci (QEMU -device virtio-tablet-pci): absolute EV_ABS
 * coordinates for host-pixel alignment. */

int  virtio_tablet_probe_and_init(uint32_t screen_w, uint32_t screen_h);
void virtio_tablet_shutdown(void);
int  virtio_tablet_active(void);
void virtio_tablet_poll(void);
