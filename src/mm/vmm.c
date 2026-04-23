#include "vmm.h"
#include "../kprintf.h"
#include "../panic.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

#define PT_PRESENT (1ULL << 0)
#define PT_WRITE   (1ULL << 1)
#define PT_PCD     (1ULL << 4)   /* cache disable — right for MMIO       */
#define PT_HUGE    (1ULL << 7)

#define PAGE_SIZE  4096
#define POOL_PAGES 8

/* Page-table staging pool. Living in .bss means Limine maps it when loading
 * the kernel image, so it is directly accessible. The kernel virt→phys
 * offset from the kernel address request lets us write its physical address
 * into parent PTEs. */
static uint8_t pt_pool[POOL_PAGES * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
static size_t  pt_pool_used = 0;

extern volatile struct limine_hhdm_request           hhdm_request;
extern volatile struct limine_kernel_address_request kernel_address_request;

static uint64_t hhdm_off(void) {
    return hhdm_request.response ? hhdm_request.response->offset : 0;
}

static uint64_t kernel_virt_to_phys(uint64_t v) {
    if (!kernel_address_request.response) {
        panic("vmm: no kernel address from bootloader");
    }
    uint64_t kv = kernel_address_request.response->virtual_base;
    uint64_t kp = kernel_address_request.response->physical_base;
    return (v - kv) + kp;
}

static uint64_t *pt_alloc(void) {
    if (pt_pool_used >= sizeof(pt_pool)) {
        panic("vmm: page-table pool exhausted");
    }
    uint8_t *p = pt_pool + pt_pool_used;
    pt_pool_used += PAGE_SIZE;
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        p[i] = 0;
    }
    return (uint64_t *) p;
}

static uint64_t pt_phys(uint64_t *table) {
    return kernel_virt_to_phys((uint64_t) table);
}

static uint64_t *next_table(uint64_t *parent, size_t index) {
    uint64_t entry = parent[index];
    if (!(entry & PT_PRESENT)) {
        uint64_t *tbl = pt_alloc();
        parent[index] = pt_phys(tbl) | PT_PRESENT | PT_WRITE;
        return tbl;
    }
    if (entry & PT_HUGE) {
        panic("vmm: mmio_map hit a huge page in the parent chain");
    }
    return (uint64_t *) ((entry & ~0xFFFULL) + hhdm_off());
}

void mmio_map(uint64_t virt, uint64_t phys, uint64_t size) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t *) ((cr3 & ~0xFFFULL) + hhdm_off());

    uint64_t v = virt & ~(PAGE_SIZE - 1);
    uint64_t p = phys & ~(PAGE_SIZE - 1);
    uint64_t end = (virt + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (; v < end; v += PAGE_SIZE, p += PAGE_SIZE) {
        uint64_t *pdpt = next_table(pml4, (v >> 39) & 0x1FF);
        uint64_t *pd   = next_table(pdpt, (v >> 30) & 0x1FF);
        uint64_t *pt   = next_table(pd,   (v >> 21) & 0x1FF);

        pt[(v >> 12) & 0x1FF] = p | PT_PRESENT | PT_WRITE | PT_PCD;
        __asm__ volatile ("invlpg (%0)" :: "r"(v) : "memory");
    }
}
