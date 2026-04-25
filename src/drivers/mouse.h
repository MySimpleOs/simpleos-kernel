#pragma once

#include <stdint.h>

/* Pointer stack: virtio-tablet (QEMU), Intel LPSS + HID-over-I2C, USB xHCI, PS/2 aux.
 *
 * PS/2: IRQ 12 + polling. Standard 3-byte motion packets; IntelliMouse
 * probe enables a 4th wheel byte — many laptop touchpads use this mode
 * when routed through the legacy controller (I2C-only touchpads need a
 * separate driver, not implemented here).
 *
 * Screen coords: relative deltas accumulate into [0 .. width-1] ×
 * [0 .. height-1]; call mouse_set_screen() if the compositor resolution
 * changes after boot.
 */

#define MOUSE_VECTOR 0x2C
#define MOUSE_GSI    12

enum {
    MOUSE_BTN_LEFT   = 1 << 0,
    MOUSE_BTN_RIGHT  = 1 << 1,
    MOUSE_BTN_MIDDLE = 1 << 2,
};

void mouse_init(uint32_t screen_w, uint32_t screen_h);
void mouse_set_screen(uint32_t screen_w, uint32_t screen_h);
/* Absolute pointer (virtio-tablet): replaces PS/2 deltas for this frame. */
void mouse_absolute_inject(int32_t x, int32_t y, uint8_t buttons);
/* Relative motion + buttons (USB HID boot, etc.): same axis policy as PS/2. */
void mouse_rel_inject(int32_t dx, int32_t dy, uint8_t buttons);
void mouse_handle_irq(void);
/* IRQ1 may see PS/2 aux-port bytes on the same controller; forward them here
 * so they are not decoded as keyboard scan codes. */
void mouse_ps2_aux_byte(uint8_t data);
/* Drain PS/2 output (call from compositor each frame); fixes VBox when IRQ12
 * does not fire for every packet while data still appears in port 0x60. */
void mouse_poll(void);
/* Start background USB probe worker after scheduler init. */
void mouse_start_background_probe(void);

/* Snapshot the current cursor state. Out parameters may be NULL. */
void mouse_get_state(int32_t *x, int32_t *y, uint8_t *buttons);

/* Monotonic event counter — caller can detect activity without reading
 * position repeatedly. Increments on every completed packet. */
uint64_t mouse_events(void);

/* After mouse_init(): two short lines for on-screen boot hint when COM1
 * serial is unavailable (real hardware). ASCII only. */
const char *mouse_boot_line1(void);
const char *mouse_boot_line2(void);
/* Recompute boot hint strings (USB probing may finish after first paint). */
void mouse_boot_hint_refresh(void);
