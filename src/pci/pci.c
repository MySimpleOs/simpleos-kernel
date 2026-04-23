#include "pci.h"
#include "../arch/x86_64/io.h"
#include "../kprintf.h"

#include <stdint.h>
#include <stddef.h>

#define CONFIG_ADDRESS 0xCF8
#define CONFIG_DATA    0xCFC

static struct pci_device devices[PCI_MAX_DEVICES];
static uint32_t          device_count = 0;

static uint32_t bdf_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (1u << 31)
         | ((uint32_t) bus  << 16)
         | ((uint32_t) slot << 11)
         | ((uint32_t) func << 8)
         | (off & 0xFC);
}

static uint32_t raw_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(CONFIG_ADDRESS, bdf_addr(bus, slot, func, off));
    return inl(CONFIG_DATA);
}

static void raw_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    outl(CONFIG_ADDRESS, bdf_addr(bus, slot, func, off));
    outl(CONFIG_DATA, v);
}

uint32_t pci_cfg_read32(const struct pci_device *d, uint8_t off) {
    return raw_read32(d->bus, d->slot, d->func, off);
}

uint16_t pci_cfg_read16(const struct pci_device *d, uint8_t off) {
    uint32_t v = raw_read32(d->bus, d->slot, d->func, off & ~3);
    return (uint16_t) (v >> ((off & 2) * 8));
}

uint8_t pci_cfg_read8(const struct pci_device *d, uint8_t off) {
    uint32_t v = raw_read32(d->bus, d->slot, d->func, off & ~3);
    return (uint8_t) (v >> ((off & 3) * 8));
}

void pci_cfg_write32(const struct pci_device *d, uint8_t off, uint32_t v) {
    raw_write32(d->bus, d->slot, d->func, off, v);
}

void pci_cfg_write16(const struct pci_device *d, uint8_t off, uint16_t v) {
    uint8_t  aligned = off & ~3;
    uint32_t cur     = raw_read32(d->bus, d->slot, d->func, aligned);
    uint32_t shift   = (off & 2) * 8;
    cur = (cur & ~(0xFFFFu << shift)) | ((uint32_t) v << shift);
    raw_write32(d->bus, d->slot, d->func, aligned, cur);
}

/* Probe a BAR by writing 0xFFFFFFFF, reading back, computing size from the
 * lowest set bit after masking the flag bits. 64-bit BARs span two slots. */
static void decode_bar(struct pci_device *d, int i, int *skip_next) {
    uint8_t off = 0x10 + i * 4;
    uint32_t orig = raw_read32(d->bus, d->slot, d->func, off);

    d->bars[i].base         = 0;
    d->bars[i].size         = 0;
    d->bars[i].is_64bit     = 0;
    d->bars[i].prefetchable = 0;
    d->bars[i].type         = PCI_BAR_MEM;

    if (orig == 0) return;

    /* Size discovery writes all-ones, reads back. */
    raw_write32(d->bus, d->slot, d->func, off, 0xFFFFFFFF);
    uint32_t probe = raw_read32(d->bus, d->slot, d->func, off);
    raw_write32(d->bus, d->slot, d->func, off, orig);

    if (orig & 0x1) {
        /* I/O BAR. */
        d->bars[i].type = PCI_BAR_IO;
        uint32_t mask = probe & ~0x3u;
        if (!mask) return;
        d->bars[i].base = orig & ~0x3u;
        d->bars[i].size = (~mask) + 1;
    } else {
        uint32_t kind   = (orig >> 1) & 0x3;
        d->bars[i].prefetchable = (orig >> 3) & 0x1;
        uint32_t base_lo   = orig & ~0xFu;
        uint32_t probe_lo  = probe & ~0xFu;
        if (kind == 0x2 && i < PCI_BAR_COUNT - 1) {
            /* 64-bit BAR — consume the next slot. */
            d->bars[i].is_64bit = 1;
            uint32_t orig_hi  = raw_read32(d->bus, d->slot, d->func, off + 4);
            raw_write32(d->bus, d->slot, d->func, off + 4, 0xFFFFFFFF);
            uint32_t probe_hi = raw_read32(d->bus, d->slot, d->func, off + 4);
            raw_write32(d->bus, d->slot, d->func, off + 4, orig_hi);

            uint64_t full_probe = ((uint64_t) probe_hi << 32) | probe_lo;
            uint64_t full_base  = ((uint64_t) orig_hi  << 32) | base_lo;
            d->bars[i].base = full_base;
            d->bars[i].size = full_probe ? ((~full_probe) + 1) : 0;
            *skip_next = 1;
        } else {
            d->bars[i].base = base_lo;
            d->bars[i].size = probe_lo ? ((~(uint64_t) probe_lo) + 1) & 0xFFFFFFFFu : 0;
        }
    }
}

static void probe_function(uint8_t bus, uint8_t slot, uint8_t func) {
    if (device_count >= PCI_MAX_DEVICES) return;

    uint32_t id = raw_read32(bus, slot, func, 0x00);
    uint16_t vendor = (uint16_t) id;
    if (vendor == 0xFFFF) return;

    struct pci_device *d = &devices[device_count++];
    d->bus = bus; d->slot = slot; d->func = func;
    d->vendor_id = vendor;
    d->device_id = (uint16_t) (id >> 16);

    uint32_t class_info = raw_read32(bus, slot, func, 0x08);
    d->revision   = (uint8_t) (class_info >> 0);
    d->prog_if    = (uint8_t) (class_info >> 8);
    d->subclass   = (uint8_t) (class_info >> 16);
    d->class_code = (uint8_t) (class_info >> 24);

    uint32_t hdr = raw_read32(bus, slot, func, 0x0C);
    d->header_type = (uint8_t) ((hdr >> 16) & 0x7F);

    /* Only type 0 (endpoints) have 6 BARs. Type 1 is PCI-to-PCI bridge
     * with 2 BARs; we log it but don't rely on BARs. */
    int bar_cap = (d->header_type == 0x00) ? PCI_BAR_COUNT : 2;
    int skip = 0;
    for (int i = 0; i < bar_cap; i++) {
        if (skip) { skip = 0; continue; }
        decode_bar(d, i, &skip);
    }
}

static const char *class_str(uint8_t c) {
    switch (c) {
        case 0x00: return "unclass";
        case 0x01: return "storage";
        case 0x02: return "network";
        case 0x03: return "display";
        case 0x04: return "multimedia";
        case 0x05: return "memory";
        case 0x06: return "bridge";
        case 0x0C: return "serial-bus";
        default:   return "other";
    }
}

void pci_init(void) {
    device_count = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = raw_read32((uint8_t) bus, slot, 0, 0x00);
            if ((uint16_t) id == 0xFFFF) continue;

            uint8_t hdr = (uint8_t) ((raw_read32((uint8_t) bus, slot, 0, 0x0C) >> 16) & 0xFF);
            uint8_t funcs = (hdr & 0x80) ? 8 : 1;
            for (uint8_t f = 0; f < funcs; f++) {
                probe_function((uint8_t) bus, slot, f);
            }
        }
    }

    kprintf("[pci] %u device(s) enumerated\n", (unsigned) device_count);
    for (uint32_t i = 0; i < device_count; i++) {
        struct pci_device *d = &devices[i];
        kprintf("  %x:%x.%u  %04x:%04x  %s/%02x  hdr=%x\n",
                (unsigned) d->bus, (unsigned) d->slot, (unsigned) d->func,
                (unsigned) d->vendor_id, (unsigned) d->device_id,
                class_str(d->class_code), (unsigned) d->subclass,
                (unsigned) d->header_type);
    }
}

struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (uint32_t i = 0; i < device_count; i++) {
        struct pci_device *d = &devices[i];
        if (class_code != 0xFF && d->class_code != class_code) continue;
        if (subclass   != 0xFF && d->subclass   != subclass)   continue;
        return d;
    }
    return NULL;
}

struct pci_device *pci_find_id(uint16_t vendor, uint16_t device) {
    for (uint32_t i = 0; i < device_count; i++) {
        struct pci_device *d = &devices[i];
        if (vendor != 0xFFFF && d->vendor_id != vendor) continue;
        if (device != 0xFFFF && d->device_id != device) continue;
        return d;
    }
    return NULL;
}

uint32_t pci_count(void)                  { return device_count; }
struct pci_device *pci_at(uint32_t index) {
    return index < device_count ? &devices[index] : NULL;
}
