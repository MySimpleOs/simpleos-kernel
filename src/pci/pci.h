#pragma once

#include <stdint.h>

#define PCI_MAX_DEVICES 64
#define PCI_BAR_COUNT   6

enum {
    PCI_BAR_MEM     = 0,
    PCI_BAR_IO      = 1,
};

struct pci_bar {
    uint64_t base;
    uint64_t size;
    uint8_t  type;          /* PCI_BAR_MEM / PCI_BAR_IO                       */
    uint8_t  is_64bit;
    uint8_t  prefetchable;
};

struct pci_device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    struct pci_bar bars[PCI_BAR_COUNT];
};

void pci_init(void);

/* First match on (class_code, subclass) pair; NULL if nothing found. Pass
 * 0xFF for "any" on either field. */
struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass);

/* First match on (vendor, device); NULL if nothing found. Pass 0xFFFF for
 * "any" on either field. */
struct pci_device *pci_find_id(uint16_t vendor, uint16_t device);

/* Iteration for higher layers (e.g. a GPU probe that tries several IDs). */
uint32_t           pci_count(void);
struct pci_device *pci_at(uint32_t index);

/* Raw config-space accessors, mainly for device-specific features and
 * capability lists. `offset` is in bytes, DWORD-aligned reads are cheapest
 * but the helpers work on any alignment. */
uint32_t pci_cfg_read32(const struct pci_device *dev, uint8_t offset);
uint16_t pci_cfg_read16(const struct pci_device *dev, uint8_t offset);
uint8_t  pci_cfg_read8 (const struct pci_device *dev, uint8_t offset);
void     pci_cfg_write32(const struct pci_device *dev, uint8_t offset, uint32_t v);
void     pci_cfg_write16(const struct pci_device *dev, uint8_t offset, uint16_t v);

/* Set PCI COMMAND memory + bus-master bits (needed for USB DMA). */
void pci_enable_mmio_bus_master(const struct pci_device *dev);
