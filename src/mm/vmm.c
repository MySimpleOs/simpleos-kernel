#include "vmm.h"
#include "pmm.h"
#include "../kprintf.h"
#include "../panic.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

#define PT_PRESENT (1ULL << 0)
#define PT_WRITE   (1ULL << 1)
#define PT_USER    (1ULL << 2)
#define PT_PCD     (1ULL << 4)
#define PT_HUGE    (1ULL << 7)
#define PT_NX      (1ULL << 63)

#define PAGE_SIZE 4096
#define ADDR_MASK 0x000FFFFFFFFFF000ULL  /* canonical 52-bit physaddr slot */

extern volatile struct limine_hhdm_request hhdm_request;

static uint64_t hhdm_off(void) {
    return hhdm_request.response ? hhdm_request.response->offset : 0;
}

/* Parent tables come either from Limine's own page-table tree (already
 * present in CR3) or from the PMM. In both cases the physical frame is
 * mapped in HHDM, so hhdm + phys gives a kernel-virtual view. */
static uint64_t *table_virt(uint64_t phys) {
    return (uint64_t *) ((phys & ADDR_MASK) + hhdm_off());
}

static uint64_t table_phys(uint64_t *table) {
    /* Works uniformly whether we allocated via PMM (HHDM region) or walked
     * into a Limine-created table (also HHDM). */
    return (uint64_t) table - hhdm_off();
}

static uint64_t *pt_alloc(void) {
    uint64_t phys = pmm_alloc_page();   /* pmm_alloc_page pre-zeroes */
    return (uint64_t *) (phys + hhdm_off());
}

static uint64_t *next_table(uint64_t *parent, size_t index) {
    uint64_t entry = parent[index];
    if (!(entry & PT_PRESENT)) {
        uint64_t *fresh = pt_alloc();
        parent[index] = table_phys(fresh) | PT_PRESENT | PT_WRITE | PT_USER;
        return fresh;
    }
    if (entry & PT_HUGE) {
        panic("vmm: mmio_map hit a huge page in a parent chain");
    }
    return table_virt(entry);
}

static uint64_t *pml4_root(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return table_virt(cr3);
}

static uint64_t leaf_flags(uint64_t flags) {
    uint64_t f = PT_PRESENT;
    if (flags & VMM_W)    f |= PT_WRITE;
    if (flags & VMM_USER) f |= PT_USER;
    if (flags & VMM_PCD)  f |= PT_PCD;
    if (flags & VMM_NX)   f |= PT_NX;
    return f;
}

void vmm_map(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    uint64_t *pml4 = pml4_root();
    uint64_t f     = leaf_flags(flags);

    uint64_t v = virt & ~(PAGE_SIZE - 1);
    uint64_t p = phys & ~(PAGE_SIZE - 1);
    uint64_t end = (virt + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (; v < end; v += PAGE_SIZE, p += PAGE_SIZE) {
        uint64_t *pdpt = next_table(pml4, (v >> 39) & 0x1FF);
        uint64_t *pd   = next_table(pdpt, (v >> 30) & 0x1FF);
        uint64_t *pt   = next_table(pd,   (v >> 21) & 0x1FF);
        pt[(v >> 12) & 0x1FF] = (p & ADDR_MASK) | f;
        __asm__ volatile ("invlpg (%0)" :: "r"(v) : "memory");
    }
}

void vmm_unmap(uint64_t virt, uint64_t size) {
    uint64_t *pml4 = pml4_root();
    uint64_t v   = virt & ~(PAGE_SIZE - 1);
    uint64_t end = (virt + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (; v < end; v += PAGE_SIZE) {
        uint64_t *pdpt, *pd, *pt;
        if (!(pml4[(v >> 39) & 0x1FF] & PT_PRESENT)) continue;
        pdpt = table_virt(pml4[(v >> 39) & 0x1FF]);
        if (!(pdpt[(v >> 30) & 0x1FF] & PT_PRESENT) ||
             (pdpt[(v >> 30) & 0x1FF] & PT_HUGE)) continue;
        pd = table_virt(pdpt[(v >> 30) & 0x1FF]);
        if (!(pd[(v >> 21) & 0x1FF] & PT_PRESENT) ||
             (pd[(v >> 21) & 0x1FF] & PT_HUGE)) continue;
        pt = table_virt(pd[(v >> 21) & 0x1FF]);

        pt[(v >> 12) & 0x1FF] = 0;
        __asm__ volatile ("invlpg (%0)" :: "r"(v) : "memory");
    }
}

void mmio_map(uint64_t virt, uint64_t phys, uint64_t size) {
    vmm_map(virt, phys, size, VMM_W | VMM_PCD);
}
