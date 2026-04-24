#include "kernel_addr.h"
#include "vmm.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

extern volatile struct limine_kernel_address_request kernel_address_request;

uint64_t kernel_virt_to_phys(const void *v) {
    uint64_t va = (uint64_t) (uintptr_t) v;
    uint64_t p  = 0;

    /* Primary: walk active page tables (HHDM, heap, mmio_map, Limine maps). */
    if (vmm_virt_to_phys(va, &p) == 0)
        return p;

    /* Fallback: Limine kernel image linear slide (before extra mappings). */
    if (kernel_address_request.response) {
        uint64_t vb = kernel_address_request.response->virtual_base;
        uint64_t pb = kernel_address_request.response->physical_base;
        return pb + (va - vb);
    }

    return 0;
}
