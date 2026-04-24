#pragma once

#include <stdint.h>

/* PS/2 mouse driver.
 *
 * Wired through the 8042 auxiliary port. IRQ 12 (GSI 12) delivers a
 * 3-byte packet on each motion or button event; the driver assembles
 * those bytes and maintains a cursor position in screen coordinates.
 * The compositor reads mouse_get_state() every frame to position the
 * cursor overlay surface.
 */

#define MOUSE_VECTOR 0x2C
#define MOUSE_GSI    12

enum {
    MOUSE_BTN_LEFT   = 1 << 0,
    MOUSE_BTN_RIGHT  = 1 << 1,
    MOUSE_BTN_MIDDLE = 1 << 2,
};

void mouse_init(uint32_t screen_w, uint32_t screen_h);
/* Absolute pointer (virtio-tablet): replaces PS/2 deltas for this frame. */
void mouse_absolute_inject(int32_t x, int32_t y, uint8_t buttons);
void mouse_handle_irq(void);
/* Drain PS/2 output (call from compositor each frame); fixes VBox when IRQ12
 * does not fire for every packet while data still appears in port 0x60. */
void mouse_poll(void);

/* Snapshot the current cursor state. Out parameters may be NULL. */
void mouse_get_state(int32_t *x, int32_t *y, uint8_t *buttons);

/* Monotonic event counter — caller can detect activity without reading
 * position repeatedly. Increments on every completed packet. */
uint64_t mouse_events(void);
