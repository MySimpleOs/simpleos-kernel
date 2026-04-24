#include "apic.h"
#include "acpi.h"
#include "pit.h"
#include "../../kprintf.h"
#include "../../mm/vmm.h"

#include <limine.h>
#include <stdint.h>

volatile uint64_t timer_ticks = 0;
volatile uint32_t timer_hz    = 0;
volatile uint64_t tsc_hz      = 0;

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

void lapic_enable_local(void) {
    /* Flip the global-enable bit on this CPU's LAPIC, then program the
     * spurious-vector register and clear TPR. Both MMIO writes hit the
     * CPU's own LAPIC — the physical address is the same per spec, but
     * each core sees its local unit. */
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    base |= LAPIC_GLOBAL_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base);

    lapic_write(LAPIC_SVR, (1u << 8) | LAPIC_SPURIOUS_VECTOR);
    lapic_write(LAPIC_TPR, 0);
}

uint8_t lapic_current_id(void) {
    return (uint8_t) ((lapic_read(LAPIC_ID) >> 24) & 0xFFu);
}

void lapic_init(void) {
    uint64_t hhdm_offset = hhdm_request.response ? hhdm_request.response->offset : 0;
    uint64_t lapic_virt  = acpi.lapic_phys + hhdm_offset;
    mmio_map(lapic_virt, acpi.lapic_phys, 0x1000);
    lapic_mmio = (volatile uint32_t *) lapic_virt;

    lapic_enable_local();

    uint32_t id      = lapic_read(LAPIC_ID)      >> 24;
    uint32_t version = lapic_read(LAPIC_VERSION) & 0xFF;
    kprintf("[lapic] enabled id=%u version=0x%x @ %p\n",
            (unsigned) id, (unsigned) version, lapic_mmio);
}

void lapic_timer_init(uint32_t hz) {
    /* Divide configuration: divide-by-16 (binary 0b1011 but encoded weird). */
    lapic_write(LAPIC_TIMER_DIV, 0x3);

    /* Kick off a 10 ms PIT oneshot and let the LAPIC timer tick down from
     * 0xFFFFFFFF in the meantime — the delta tells us ticks/10 ms. TSC is
     * latched on both sides of the same window to derive tsc_hz. */
    uint16_t pit_ticks = (uint16_t) (PIT_FREQUENCY / 100);
    pit_prepare_oneshot(pit_ticks);
    uint64_t tsc_start = rdtsc();
    lapic_write(LAPIC_TIMER_INITCNT, 0xFFFFFFFF);

    while (!pit_oneshot_done()) {
        __asm__ volatile ("pause");
    }
    uint64_t tsc_end   = rdtsc();

    uint32_t remaining = lapic_read(LAPIC_TIMER_CURCNT);
    uint32_t consumed  = 0xFFFFFFFF - remaining;

    /* Reload value for periodic mode: (ticks counted in 10 ms) * 100 / hz.
     * Broken calibration (some VMs / PIT quirks) yields tiny `consumed` or
     * a `count` so large the LAPIC fires ~1 Hz — the compositor then paces
     * at ~1 FPS when it waits on timer_ticks. Clamp implied IRQ rate. */
    if (hz < 100u) hz = 100u;
    if (consumed < 500u) {
        kprintf("[lapic] warn: consumed=%u low; assume 200000 ticks/10ms\n",
                (unsigned) consumed);
        consumed = 200000u;
    }

    uint64_t wide = (uint64_t) consumed * 100ull / (uint64_t) hz;
    /* Implied Hz = consumed*100/count; keep implied in [40, 10000]. */
    uint64_t c_fast = (uint64_t) consumed * 100ull / 10000ull; /* count floor -> max 10 kHz */
    uint64_t c_slow = (uint64_t) consumed * 100ull / 40ull;    /* count ceil  -> min ~40 Hz */
    if (c_fast < 32ull) c_fast = 32ull;
    if (c_slow < c_fast + 1ull) c_slow = c_fast + 1000ull;
    if (wide < c_fast) wide = c_fast;
    if (wide > c_slow) wide = c_slow;

    uint32_t count = (uint32_t) wide;

    /* LVT timer: vector | periodic (bit 17). */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1u << 17));
    lapic_write(LAPIC_TIMER_INITCNT, count);

    timer_hz = hz;
    tsc_hz   = (tsc_end - tsc_start) * 100ull;   /* 10ms -> 1s             */

    kprintf("[lapic] timer: %u ticks/10ms, periodic @ %u Hz (count=%u)\n",
            (unsigned) consumed, (unsigned) hz, (unsigned) count);
    kprintf("[lapic] tsc=%u MHz (calibrated over 10 ms PIT window)\n",
            (unsigned) (tsc_hz / 1000000ull));
}
