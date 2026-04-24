#pragma once

#include <stdint.h>

/* xHCI USB HID mouse: not yet implemented (stub). Falls back to PS/2 aux or
 * virtio-tablet. External USB mice need a full host stack (see ROADMAP). */

int  usb_xhci_mouse_init(uint32_t screen_w, uint32_t screen_h);
void usb_xhci_mouse_shutdown(void);
int  usb_xhci_mouse_active(void);
void usb_xhci_mouse_poll(void);
