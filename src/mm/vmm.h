#pragma once

#include <stdint.h>

/* Page-table flag bits exposed to vmm_map callers. Present (bit 0) is
 * always set by vmm_map; others are opt-in. */
#define VMM_W    (1ULL << 1)   /* writable                               */
#define VMM_USER (1ULL << 2)   /* accessible from ring 3                 */
#define VMM_PCD  (1ULL << 4)   /* cache disable — MMIO                   */
#define VMM_NX   (1ULL << 63)  /* non-executable                         */

/* Install leaf PTEs so [virt, virt+size) maps to [phys, phys+size). Size is
 * rounded up to the nearest 4 KiB. Intermediate tables are allocated from
 * the PMM on demand. TLB is invalidated for the written range. */
void vmm_map(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

/* Clear leaf PTEs for [virt, virt+size). Intermediate tables are left
 * behind even if they end up empty; that's fine for the kernel address
 * space and keeps the code simple. */
void vmm_unmap(uint64_t virt, uint64_t size);

/* Legacy wrapper kept so existing callers (acpi.c, apic.c, ioapic.c) do not
 * need to change. Maps writable, cache-disabled — right for device MMIO. */
void mmio_map(uint64_t virt, uint64_t phys, uint64_t size);
