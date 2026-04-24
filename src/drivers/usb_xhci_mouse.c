#include "usb_xhci_mouse.h"

#include "../kprintf.h"
#include "../pci/pci.h"

#include <stddef.h>
#include <stdint.h>

/* Full xHCI + HID enumeration is several thousand lines (rings, contexts,
 * scratchpads, USB3 vs USB2 ports, Evaluate Context, Configure Endpoint).
 * A partial driver breaks boot on real HW — keep a safe stub until a
 * reviewed implementation lands (see ROADMAP / docs). */

static struct pci_device *find_xhci(void) {
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_device *d = pci_at(i);
        if (d && d->class_code == 0x0Cu && d->subclass == 0x03u && d->prog_if == 0x30u)
            return d;
    }
    return NULL;
}

void usb_xhci_mouse_shutdown(void) { }

int usb_xhci_mouse_active(void) { return 0; }

void usb_xhci_mouse_poll(void) { }

int usb_xhci_mouse_init(uint32_t screen_w, uint32_t screen_h) {
    (void) screen_w;
    (void) screen_h;
    struct pci_device *d = find_xhci();
    if (!d) {
        kprintf("[usb-mouse] no PCI xHCI (class 0C/03/30)\n");
        return -1;
    }
    pci_enable_mmio_bus_master(d);
    kprintf("[usb-mouse] xHCI %04x:%04x present — USB HID driver not merged yet; "
            "use PS/2/aux or QEMU virtio-tablet. External USB mice need the "
            "full xHCI stack (work in progress).\n",
            (unsigned) d->vendor_id, (unsigned) d->device_id);
    return -1;
}
