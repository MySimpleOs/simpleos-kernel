#pragma once

#include <stddef.h>

/* Reserve the backing virtual range, map the first window, and initialise
 * the free list. Must run after pmm_init and vmm setup. */
void  heap_init(void);

void *kmalloc(size_t size);
void  kfree(void *ptr);

/* Optional — dump free-list stats to serial, useful after tests. */
void  heap_dump(void);
