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

#define LAPIC_TIMER_VECTOR 0x20

void     lapic_init(void);
void     lapic_enable_local(void);   /* BSP + APs: enable the current CPU's LAPIC */
uint32_t lapic_read(uint32_t reg);
void     lapic_write(uint32_t reg, uint32_t value);
void     lapic_eoi(void);

/* Calibrate against the PIT, then program the LAPIC timer in periodic mode
 * firing at `hz` Hz on vector LAPIC_TIMER_VECTOR. */
void lapic_timer_init(uint32_t hz);

/* Monotonically increasing counter updated by the LAPIC timer handler. */
extern volatile uint64_t timer_ticks;
