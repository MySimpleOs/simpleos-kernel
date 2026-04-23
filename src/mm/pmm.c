#include "pmm.h"
#include "../kprintf.h"
#include "../panic.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_hhdm_request   hhdm_request;

static uint64_t total_pages      = 0;
static uint64_t used_pages       = 0;
static uint8_t *bitmap           = NULL;
static uint64_t bitmap_size      = 0;
static uint64_t bitmap_phys_base = 0;
static uint64_t bitmap_pages     = 0;
static uint64_t alloc_hint       = 0;

static uint64_t hhdm_off(void) {
    return hhdm_request.response ? hhdm_request.response->offset : 0;
}

static inline void bit_set(uint64_t idx)   { bitmap[idx >> 3] |=  (uint8_t)(1u << (idx & 7)); }
static inline void bit_clear(uint64_t idx) { bitmap[idx >> 3] &= (uint8_t)~(1u << (idx & 7)); }
static inline int  bit_get(uint64_t idx)   { return (bitmap[idx >> 3] >> (idx & 7)) & 1; }

void pmm_init(void) {
    if (!memmap_request.response) panic("pmm: no memmap from bootloader");

    /* Size the bitmap from real-RAM regions only. MMIO holes at the top of
     * the memmap (e.g. QEMU's 1 TiB reserved stub) would otherwise blow the
     * bitmap up to tens of megabytes. */
    uint64_t max_phys = 0;
    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *e = memmap_request.response->entries[i];
        int is_ram = (e->type == LIMINE_MEMMAP_USABLE)
                  || (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
                  || (e->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE)
                  || (e->type == LIMINE_MEMMAP_ACPI_NVS);
        if (is_ram) {
            uint64_t end = e->base + e->length;
            if (end > max_phys) max_phys = end;
        }
    }

    total_pages  = max_phys / PMM_PAGE_SIZE;
    bitmap_size  = (total_pages + 7) / 8;
    bitmap_pages = (bitmap_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    /* Park the bitmap at the start of the first USABLE region big enough. */
    bitmap_phys_base = 0;
    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *e = memmap_request.response->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE
            && e->length >= bitmap_pages * PMM_PAGE_SIZE) {
            bitmap_phys_base = e->base;
            break;
        }
    }
    if (!bitmap_phys_base) panic("pmm: no USABLE region large enough for bitmap");

    bitmap = (uint8_t *) (bitmap_phys_base + hhdm_off());

    /* Start with everything marked used, then free USABLE pages — except the
     * bitmap's own backing frames, which stay used forever. */
    for (uint64_t i = 0; i < bitmap_size; i++) bitmap[i] = 0xFF;
    used_pages = total_pages;

    for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *e = memmap_request.response->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t base = e->base;
        uint64_t end  = e->base + e->length;

        if (base == bitmap_phys_base) {
            base += bitmap_pages * PMM_PAGE_SIZE;
        }
        for (uint64_t p = base; p < end; p += PMM_PAGE_SIZE) {
            uint64_t idx = p / PMM_PAGE_SIZE;
            if (idx < total_pages && bit_get(idx)) {
                bit_clear(idx);
                used_pages--;
            }
        }
    }

    uint64_t free_pages = total_pages - used_pages;
    kprintf("[pmm] total=%u free=%u used=%u bitmap@%p (%u pages, %u bytes)\n",
            (unsigned) total_pages, (unsigned) free_pages, (unsigned) used_pages,
            (void *) bitmap_phys_base,
            (unsigned) bitmap_pages, (unsigned) bitmap_size);
}

uint64_t pmm_alloc_page(void) {
    /* Two-pass scan from the hint so successive allocations stay near each
     * other without an explicit cursor maintenance step. */
    for (uint64_t i = alloc_hint; i < total_pages; i++) {
        if (!bit_get(i)) {
            bit_set(i);
            used_pages++;
            alloc_hint = i + 1;
            uint64_t phys = i * PMM_PAGE_SIZE;
            /* Zero the page via HHDM so callers never see stale data. */
            uint8_t *v = (uint8_t *) (phys + hhdm_off());
            for (int j = 0; j < PMM_PAGE_SIZE; j++) v[j] = 0;
            return phys;
        }
    }
    for (uint64_t i = 0; i < alloc_hint; i++) {
        if (!bit_get(i)) {
            bit_set(i);
            used_pages++;
            alloc_hint = i + 1;
            uint64_t phys = i * PMM_PAGE_SIZE;
            uint8_t *v = (uint8_t *) (phys + hhdm_off());
            for (int j = 0; j < PMM_PAGE_SIZE; j++) v[j] = 0;
            return phys;
        }
    }
    panic("pmm: out of physical memory");
}

uint64_t pmm_alloc_contig(uint64_t n_pages) {
    if (n_pages == 0) return 0;
    if (n_pages > total_pages) return 0;

    for (uint64_t start = 0; start + n_pages <= total_pages; ) {
        int run_ok = 1;
        uint64_t i;
        for (i = 0; i < n_pages; i++) {
            if (bit_get(start + i)) { run_ok = 0; break; }
        }
        if (!run_ok) {
            start += i + 1;           /* skip past the used bit              */
            continue;
        }
        for (uint64_t j = 0; j < n_pages; j++) {
            bit_set(start + j);
        }
        used_pages += n_pages;
        uint64_t phys = start * PMM_PAGE_SIZE;
        uint8_t *v = (uint8_t *) (phys + hhdm_off());
        for (uint64_t b = 0; b < n_pages * PMM_PAGE_SIZE; b++) v[b] = 0;
        return phys;
    }
    return 0;
}

void pmm_free_page(uint64_t phys) {
    uint64_t idx = phys / PMM_PAGE_SIZE;
    if (idx >= total_pages) return;
    if (!bit_get(idx))       return;  /* already free — swallow double frees */
    bit_clear(idx);
    used_pages--;
    if (idx < alloc_hint) alloc_hint = idx;
}

void pmm_stats(uint64_t *total, uint64_t *used) {
    if (total) *total = total_pages;
    if (used)  *used  = used_pages;
}
