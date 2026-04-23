#pragma once

#include <stdint.h>

/* Install PTEs so the given 4 KiB-aligned virtual range maps to phys with
 * write permission and cache disabled (appropriate for device MMIO). The
 * page table intermediates come from a small static pool inside the kernel
 * image, so mmio_map is callable before any real allocator exists. */
void mmio_map(uint64_t virt, uint64_t phys, uint64_t size);
