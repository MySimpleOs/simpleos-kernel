#pragma once

#include <stdint.h>

void ioapic_init(void);

/* Route a general-system-interrupt line to a LAPIC vector on a given
 * physical APIC destination. Edge-triggered, active-high, fixed delivery,
 * unmasked. */
void ioapic_set_irq(uint8_t gsi, uint8_t vector, uint8_t destination_lapic_id);
