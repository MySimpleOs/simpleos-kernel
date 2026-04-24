#pragma once

#include <stdint.h>

/* Minimal xHCI root-hub + USB2 full-speed HID boot mouse (relative deltas).
 * Works on many PCs with an xHCI controller and a boot-compatible mouse.
 * Polled from mouse_poll(); no MSI. Falls back silently if probe fails.
 *
 * True I2C-HID laptop touchpads without PS/2 are not covered here — use
 * USB or PS/2 path, or external USB mouse. */

int  usb_xhci_mouse_init(uint32_t screen_w, uint32_t screen_h);
void usb_xhci_mouse_shutdown(void);
int  usb_xhci_mouse_active(void);
void usb_xhci_mouse_poll(void);
