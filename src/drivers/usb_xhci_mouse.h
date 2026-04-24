#pragma once

#include <stdint.h>

/* xHCI USB HID mouse: all PCI xHCI controllers, first MMIO BAR, root ports only.
 * Boot + common report layouts (report ID, 16-bit relative XY). No hubs / I2C
 * touchpad — those need additional drivers like other OSes. */

int  usb_xhci_mouse_init(uint32_t screen_w, uint32_t screen_h);
void usb_xhci_mouse_shutdown(void);
int  usb_xhci_mouse_active(void);
void usb_xhci_mouse_poll(void);
