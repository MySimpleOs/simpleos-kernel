#pragma once

#include <stdint.h>
#include <stddef.h>

#define PMM_PAGE_SIZE 4096

void     pmm_init(void);
uint64_t pmm_alloc_page(void);
void     pmm_free_page(uint64_t phys);

/* Allocate `n_pages` physically contiguous 4 KiB frames. Returns the
 * first phys address, zeroed. Returns 0 if no contiguous run fits.
 * O(total_pages) in the worst case — use sparingly (large DMA buffers,
 * page-table arrays, etc). */
uint64_t pmm_alloc_contig(uint64_t n_pages);

void     pmm_stats(uint64_t *total_pages, uint64_t *used_pages);
