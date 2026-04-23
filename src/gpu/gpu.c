#include "gpu.h"
#include "virtio_gpu.h"

#include "../kprintf.h"
#include "../pci/pci.h"

#include <stdint.h>

#define VENDOR_INTEL   0x8086
#define VENDOR_NVIDIA  0x10DE
#define VENDOR_AMD     0x1002
#define VENDOR_REDHAT  0x1AF4   /* VirtIO devices                            */
#define VENDOR_VMWARE  0x15AD
#define VENDOR_ORACLE  0x80EE   /* VirtualBox SVGA                           */

#define DEVICE_VIRTIO_GPU_LEGACY 0x1050
#define DEVICE_VIRTIO_GPU_MODERN 0x1050   /* same ID; distinguished by subsystem */

static const char *vendor_name(uint16_t v) {
    switch (v) {
        case VENDOR_INTEL:  return "Intel";
        case VENDOR_NVIDIA: return "NVIDIA";
        case VENDOR_AMD:    return "AMD";
        case VENDOR_REDHAT: return "Red Hat (VirtIO)";
        case VENDOR_VMWARE: return "VMware";
        case VENDOR_ORACLE: return "Oracle (VBox)";
        default:            return "unknown";
    }
}

static void log_display(struct pci_device *d) {
    kprintf("[gpu] %04x:%04x  %s  @ %x:%x.%u",
            (unsigned) d->vendor_id, (unsigned) d->device_id,
            vendor_name(d->vendor_id),
            (unsigned) d->bus, (unsigned) d->slot, (unsigned) d->func);

    /* Surface the primary framebuffer BAR if one exists — helpful for
     * quickly eyeballing what the device owns in memory. */
    for (int i = 0; i < 6; i++) {
        if (d->bars[i].type == 0 && d->bars[i].size >= 0x100000) {
            kprintf("  BAR%d=%p+%uM", i,
                    (void *) d->bars[i].base,
                    (unsigned) (d->bars[i].size / 1024 / 1024));
            break;
        }
    }
    kprintf("\n");

    switch (d->vendor_id) {
        case VENDOR_INTEL:
            kprintf("       Intel display — full driver in ROADMAP §1 (Gen9+ PRM-driven)\n");
            break;
        case VENDOR_NVIDIA:
            kprintf("       NVIDIA display — open-gpu-kernel-modules path; needs Turing+ and GSP firmware\n");
            break;
        case VENDOR_AMD:
            kprintf("       AMD display — ROADMAP §1 (amdgpu, longer term)\n");
            break;
        case VENDOR_REDHAT:
            if (d->device_id == DEVICE_VIRTIO_GPU_MODERN) {
                kprintf("       VirtIO-GPU — bringing up driver\n");
                if (virtio_gpu_init(d) != 0) {
                    kprintf("       VirtIO-GPU init failed; staying on limine-fb\n");
                }
            }
            break;
        default:
            break;
    }
}

void gpu_init(void) {
    int found = 0;

    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_device *d = pci_at(i);
        if (!d) continue;
        if (d->class_code != 0x03) continue;  /* display class               */
        log_display(d);
        found++;
    }

    if (!found) {
        kprintf("[gpu] no PCI display devices detected; falling back to Limine framebuffer only\n");
    }
}
