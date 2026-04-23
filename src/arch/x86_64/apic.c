#include "apic.h"
#include "acpi.h"
#include "pit.h"
#include "../../kprintf.h"
#include "../../mm/vmm.h"

#include <limine.h>
#include <stdint.h>

volatile uint64_t timer_ticks = 0;

#define IA32_APIC_BASE_MSR 0x1B
#define LAPIC_GLOBAL_ENABLE (1u << 11)

extern volatile struct limine_hhdm_request hhdm_request;

static volatile uint32_t *lapic_mmio;

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t) hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = value & 0xFFFFFFFF;
    uint32_t hi = value >> 32;
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

uint32_t lapic_read(uint32_t reg) {
    return lapic_mmio[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t value) {
    lapic_mmio[reg / 4] = value;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

void lapic_init(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    base |= LAPIC_GLOBAL_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base);

    uint64_t hhdm_offset = hhdm_request.response ? hhdm_request.response->offset : 0;
    uint64_t lapic_virt  = acpi.lapic_phys + hhdm_offset;
    mmio_map(lapic_virt, acpi.lapic_phys, 0x1000);
    lapic_mmio = (volatile uint32_t *) lapic_virt;

    /* Spurious interrupt vector + enable bit. */
    lapic_write(LAPIC_SVR, (1u << 8) | LAPIC_SPURIOUS_VECTOR);

    /* Task priority 0: accept all interrupt priority classes. */
    lapic_write(LAPIC_TPR, 0);

    uint32_t id      = lapic_read(LAPIC_ID)      >> 24;
    uint32_t version = lapic_read(LAPIC_VERSION) & 0xFF;
    kprintf("[lapic] enabled id=%u version=0x%x @ %p\n",
            (unsigned) id, (unsigned) version, lapic_mmio);
}

void lapic_timer_init(uint32_t hz) {
    /* Divide configuration: divide-by-16 (binary 0b1011 but encoded weird). */
    lapic_write(LAPIC_TIMER_DIV, 0x3);

    /* Kick off a 10 ms PIT oneshot and let the LAPIC timer tick down from
     * 0xFFFFFFFF in the meantime — the delta tells us ticks/10 ms. */
    uint16_t pit_ticks = (uint16_t) (PIT_FREQUENCY / 100);
    pit_prepare_oneshot(pit_ticks);
    lapic_write(LAPIC_TIMER_INITCNT, 0xFFFFFFFF);

    while (!pit_oneshot_done()) {
        __asm__ volatile ("pause");
    }

    uint32_t remaining = lapic_read(LAPIC_TIMER_CURCNT);
    uint32_t consumed  = 0xFFFFFFFF - remaining;

    uint32_t count = (uint32_t) ((uint64_t) consumed * 100 / hz);

    /* LVT timer: vector | periodic (bit 17). */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1u << 17));
    lapic_write(LAPIC_TIMER_INITCNT, count);

    kprintf("[lapic] timer: %u ticks/10ms, periodic @ %u Hz (count=%u)\n",
            (unsigned) consumed, (unsigned) hz, (unsigned) count);
}
