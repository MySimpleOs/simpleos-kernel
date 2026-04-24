#pragma once

#include <stdint.h>

/* Limine kernel_address_request: map a kernel VA inside the loaded image to
 * the physical address the bootloader used (required for DMA below 4G). */
uint64_t kernel_virt_to_phys(const void *v);
