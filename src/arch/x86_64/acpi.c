#include "acpi.h"
#include "../../kprintf.h"
#include "../../mm/vmm.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

struct acpi_info acpi = {
    .lapic_phys      = 0xFEE00000,
    .ioapic_phys     = 0xFEC00000,
    .ioapic_gsi_base = 0,
    .lapic_count     = 0,
};

struct rsdp_v1 {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
} __attribute__((packed));

struct rsdp_v2 {
    struct rsdp_v1 v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct madt_header {
    struct sdt_header hdr;
    uint32_t lapic_address;
    uint32_t flags;
} __attribute__((packed));

struct madt_entry {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct madt_lapic {
    struct madt_entry e;
    uint8_t  processor_id;
    uint8_t  apic_id;
    uint32_t flags;
} __attribute__((packed));

struct madt_ioapic {
    struct madt_entry e;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_address;
    uint32_t gsi_base;
} __attribute__((packed));

extern volatile struct limine_rsdp_request rsdp_request;
extern volatile struct limine_hhdm_request hhdm_request;

static uint64_t hhdm(void) {
    return hhdm_request.response ? hhdm_request.response->offset : 0;
}

static void *phys_to_virt(uint64_t phys) {
    return (void *) (phys + hhdm());
}

/* Ensure the page covering [phys, phys+span) is mapped at HHDM+phys.
 * Safe to call on regions Limine already mapped — mmio_map respects
 * existing intermediate tables. */
static void *map_phys(uint64_t phys, uint64_t span) {
    mmio_map((uint64_t) phys_to_virt(phys), phys, span);
    return phys_to_virt(phys);
}

static int sig_equals(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int parse_madt(struct madt_header *madt) {
    acpi.lapic_phys = madt->lapic_address;
    acpi.lapic_count = 0;

    uint8_t *cursor = (uint8_t *) (madt + 1);
    uint8_t *end    = (uint8_t *) madt + madt->hdr.length;

    while (cursor < end) {
        struct madt_entry *e = (struct madt_entry *) cursor;
        if (e->length == 0) break;
        switch (e->type) {
            case 0: {
                struct madt_lapic *lapic = (struct madt_lapic *) e;
                if (lapic->flags & 1) {
                    acpi.lapic_count++;
                }
                break;
            }
            case 1: {
                struct madt_ioapic *io = (struct madt_ioapic *) e;
                acpi.ioapic_phys     = io->ioapic_address;
                acpi.ioapic_gsi_base = io->gsi_base;
                break;
            }
            default: break;
        }
        cursor += e->length;
    }
    return 1;
}

void acpi_init(void) {
    if (rsdp_request.response == NULL) {
        kprintf("[acpi] no RSDP from bootloader — using default LAPIC/IOAPIC addresses\n");
        return;
    }

    /* Under Limine base revision 3 the response->address field carries a
     * physical address, so fold HHDM in to reach the RSDP structure. */
    uint64_t rsdp_phys = (uint64_t) rsdp_request.response->address;
    struct rsdp_v1 *rsdp = (struct rsdp_v1 *) map_phys(rsdp_phys, sizeof(struct rsdp_v2));
    kprintf("[acpi] RSDP rev=%u at %p\n",
            (unsigned) rsdp->revision, rsdp);

    struct sdt_header *root = NULL;
    int use_xsdt = 0;

    if (rsdp->revision >= 2) {
        struct rsdp_v2 *rsdp2 = (struct rsdp_v2 *) rsdp;
        root = (struct sdt_header *) map_phys(rsdp2->xsdt_address,
                                              sizeof(struct sdt_header));
        root = (struct sdt_header *) map_phys(rsdp2->xsdt_address, root->length);
        use_xsdt = 1;
    } else {
        root = (struct sdt_header *) map_phys((uint64_t) rsdp->rsdt_address,
                                              sizeof(struct sdt_header));
        root = (struct sdt_header *) map_phys((uint64_t) rsdp->rsdt_address,
                                              root->length);
        use_xsdt = 0;
    }

    kprintf("[acpi] %s @ %p length=%u\n",
            use_xsdt ? "XSDT" : "RSDT", root, (unsigned) root->length);

    size_t entry_size  = use_xsdt ? sizeof(uint64_t) : sizeof(uint32_t);
    size_t entry_count = (root->length - sizeof(struct sdt_header)) / entry_size;
    uint8_t *entries   = (uint8_t *) (root + 1);

    int madt_found = 0;
    for (size_t i = 0; i < entry_count; i++) {
        uint64_t phys;
        if (use_xsdt) {
            phys = ((uint64_t *) entries)[i];
        } else {
            phys = ((uint32_t *) entries)[i];
        }
        struct sdt_header *table = (struct sdt_header *) map_phys(phys, sizeof(*table));
        table = (struct sdt_header *) map_phys(phys, table->length);
        if (sig_equals(table->signature, "APIC", 4)) {
            parse_madt((struct madt_header *) table);
            madt_found = 1;
            break;
        }
    }

    if (madt_found) {
        kprintf("[acpi] MADT: lapic=0x%x ioapic=0x%x (gsi=%u) cpus=%u\n",
                (unsigned) acpi.lapic_phys,
                (unsigned) acpi.ioapic_phys,
                (unsigned) acpi.ioapic_gsi_base,
                (unsigned) acpi.lapic_count);
        return;
    }

    kprintf("[acpi] MADT not found — using default LAPIC/IOAPIC addresses\n");
}
