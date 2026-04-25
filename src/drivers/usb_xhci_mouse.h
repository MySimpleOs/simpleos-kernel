#pragma once

#include <stdint.h>

/* xHCI USB HID mouse: PCI xHCI, MMIO BAR, root ports + up to 3 hub tiers.
 * Port enumeration continues across compositor polls (non-blocking boot).
 * PS/2 may stay active until usb_xhci_mouse_active() becomes true. */

int  usb_xhci_mouse_init(uint32_t screen_w, uint32_t screen_h);
void usb_xhci_mouse_shutdown(void);
int  usb_xhci_mouse_active(void);
int  usb_xhci_mouse_driver_began(void);
int  usb_xhci_mouse_probing(void);
/* Async root scan finished with no HID mouse (exhausted ports / controllers). */
int  usb_xhci_mouse_no_device_after_full_scan(void);
/* Short on-screen debug status for USB probe path. */
const char *usb_xhci_mouse_debug_status(void);
/* Per-line diagnostic string (i = 0..count-1) for on-screen debug bar. */
const char *usb_xhci_mouse_diag_line(unsigned i);
unsigned    usb_xhci_mouse_diag_count(void);
void usb_xhci_mouse_poll(void);
