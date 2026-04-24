#pragma once

#include <stdint.h>

void ioapic_init(void);

/* Route a GSI to a LAPIC vector. Fixed delivery, physical destination, unmasked.
 * `level_trigger` / `low_active` follow Intel I/O APIC polarity/trigger bits
 * (many legacy PS/2 aux lines are active-low level on laptops). */
void ioapic_set_irq_extended(uint8_t gsi, uint8_t vector, uint8_t destination_lapic_id,
                             int level_trigger, int low_active);

static inline void ioapic_set_irq(uint8_t gsi, uint8_t vector, uint8_t destination_lapic_id) {
    ioapic_set_irq_extended(gsi, vector, destination_lapic_id, 0, 0);
}
