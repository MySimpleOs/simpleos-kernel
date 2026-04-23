#pragma once

#include <stdint.h>

/* LAPIC register offsets (in bytes from the LAPIC MMIO base). */
enum {
    LAPIC_ID       = 0x020,
    LAPIC_VERSION  = 0x030,
    LAPIC_TPR      = 0x080,
    LAPIC_EOI      = 0x0B0,
    LAPIC_SVR      = 0x0F0,
    LAPIC_ESR      = 0x280,
    LAPIC_LVT_TIMER= 0x320,
    LAPIC_LVT_LINT0= 0x350,
    LAPIC_LVT_LINT1= 0x360,
    LAPIC_LVT_ERR  = 0x370,
    LAPIC_TIMER_INITCNT = 0x380,
    LAPIC_TIMER_CURCNT  = 0x390,
    LAPIC_TIMER_DIV     = 0x3E0,
};

#define LAPIC_SPURIOUS_VECTOR 0xFF

void     lapic_init(void);
uint32_t lapic_read(uint32_t reg);
void     lapic_write(uint32_t reg, uint32_t value);
void     lapic_eoi(void);
