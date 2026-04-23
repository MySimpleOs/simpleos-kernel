#pragma once

/* Remap the legacy 8259 PIC to vectors 0x20-0x2F (away from CPU exception
 * range) and mask every IRQ. We will drive interrupts via the LAPIC/IOAPIC
 * from here on; this call just guarantees a silent legacy controller. */
void pic_disable(void);
