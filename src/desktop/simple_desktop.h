#pragma once

/* Minimal dock: bottom bar + click first tile to spawn a demo terminal surface.
 * QEMU: use virtio-tablet (run-qemu.sh) to click; bare metal needs PS/2/USB. */

void simple_desktop_init(void);
void simple_desktop_start_poller(void);
