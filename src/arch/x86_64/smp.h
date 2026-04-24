#pragma once

#include <stdint.h>

struct cpu_local {
    uint32_t cpu_id;
    uint32_t lapic_id;
    uint64_t kernel_stack_top;
};

void smp_init(void);

/* Number of CPUs that completed ap_entry (includes BSP). */
uint64_t smp_online_count(void);

/* Called by each AP right after it comes online. Implemented by the
 * compositor (compositor/worker.c); the AP spins in a work-queue loop
 * forever, claiming tiles whenever BSP bumps the work epoch. Declared
 * here (rather than pulled as a header) to keep arch/ independent of
 * compositor/ includes. */
void compositor_ap_worker(uint32_t cpu_id);
