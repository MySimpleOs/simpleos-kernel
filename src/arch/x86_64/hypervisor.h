#pragma once

#include <stdint.h>

/* Best-effort hypervisor vendor (CPUID 1.HV bit + 0x40000000 signature). */
int hypervisor_is_virtualbox(void);
