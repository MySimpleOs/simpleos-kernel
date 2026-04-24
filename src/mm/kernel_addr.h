#pragma once

#include <stdint.h>

/* Resolve a kernel virtual address to a physical address.
 * 1) Page-table walk (CR3) via vmm_virt_to_phys — covers HHDM, kmalloc, etc.
 * 2) If unmapped, Limine kernel image (virtual_base → physical_base) slide.
 * Returns 0 if unresolved (caller must not use for DMA). */
uint64_t kernel_virt_to_phys(const void *v);
