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
