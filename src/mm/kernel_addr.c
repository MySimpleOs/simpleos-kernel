#include "kernel_addr.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

extern volatile struct limine_kernel_address_request kernel_address_request;

uint64_t kernel_virt_to_phys(const void *v) {
    if (!kernel_address_request.response)
        return (uint64_t) (uintptr_t) v;
    uint64_t vb = kernel_address_request.response->virtual_base;
    uint64_t pb = kernel_address_request.response->physical_base;
    return pb + ((uint64_t) (uintptr_t) v - vb);
}
