#pragma once

#include <stdint.h>
#include <stddef.h>

#define PMM_PAGE_SIZE 4096

void     pmm_init(void);
uint64_t pmm_alloc_page(void);
void     pmm_free_page(uint64_t phys);
void     pmm_stats(uint64_t *total_pages, uint64_t *used_pages);
