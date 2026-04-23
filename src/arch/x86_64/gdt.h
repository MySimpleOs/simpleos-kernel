#pragma once

#include <stdint.h>

enum {
    GDT_NULL        = 0x00,
    GDT_KERNEL_CODE = 0x08,
    GDT_KERNEL_DATA = 0x10,
    GDT_USER_CODE   = 0x18,
    GDT_USER_DATA   = 0x20,
    GDT_TSS         = 0x28,
};

void gdt_init(void);
void tss_set_kernel_stack(uint64_t rsp0);
