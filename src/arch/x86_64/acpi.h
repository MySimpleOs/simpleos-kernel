#pragma once

#include <stdint.h>
#include <stddef.h>

struct acpi_info {
    uint64_t lapic_phys;
    uint64_t ioapic_phys;
    uint32_t ioapic_gsi_base;
    uint32_t lapic_count;
};

extern struct acpi_info acpi;

/* Parse RSDP -> XSDT/RSDT -> MADT. Fills in acpi with sensible defaults
 * (LAPIC=0xFEE00000, IOAPIC=0xFEC00000) if parsing cannot complete. */
void acpi_init(void);
