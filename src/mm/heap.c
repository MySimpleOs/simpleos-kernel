#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../kprintf.h"
#include "../panic.h"

#include <stdint.h>
#include <stddef.h>

#define KHEAP_BASE   0xFFFFFFFF90000000ULL
#define KHEAP_FIRST  (64 * PMM_PAGE_SIZE)     /* 256 KiB initial window      */
#define KHEAP_GROW   (16 * PMM_PAGE_SIZE)     /* 64 KiB minimum grow chunk   */
#define HEAP_MAGIC   0xB10C1DEEB10CAA57ULL

struct block {
    uint64_t      magic;
    uint64_t      size;    /* total block bytes, header included             */
    struct block *next;
    uint64_t      free;    /* 1 if free, 0 if allocated                      */
};

_Static_assert(sizeof(struct block) == 32, "block header must stay 32 bytes");

static struct block *head     = NULL;
static uint64_t      heap_end = 0;   /* next unmapped byte after the heap   */

static void grow_pages(size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        vmm_map(heap_end, phys, PMM_PAGE_SIZE, VMM_W);
        heap_end += PMM_PAGE_SIZE;
    }
}

void heap_init(void) {
    heap_end = KHEAP_BASE;
    grow_pages(KHEAP_FIRST / PMM_PAGE_SIZE);

    head        = (struct block *) (uintptr_t) KHEAP_BASE;
    head->magic = HEAP_MAGIC;
    head->size  = KHEAP_FIRST;
    head->next  = NULL;
    head->free  = 1;

    kprintf("[heap] %u KiB @ %p, header=%u bytes\n",
            (unsigned) (KHEAP_FIRST / 1024),
            (void *) KHEAP_BASE,
            (unsigned) sizeof(struct block));
}

static void split(struct block *b, uint64_t need) {
    /* Only split if the remainder can host another header plus meaningful
     * payload (16 bytes is the alignment granularity). */
    if (b->size >= need + sizeof(struct block) + 16) {
        struct block *n = (struct block *) ((uint8_t *) b + need);
        n->magic = HEAP_MAGIC;
        n->size  = b->size - need;
        n->next  = b->next;
        n->free  = 1;
        b->size  = need;
        b->next  = n;
    }
}

static void append_free_region(uint64_t base, uint64_t size) {
    struct block *n = (struct block *) (uintptr_t) base;
    n->magic = HEAP_MAGIC;
    n->size  = size;
    n->next  = NULL;
    n->free  = 1;

    struct block *tail = head;
    while (tail->next) tail = tail->next;
    tail->next = n;

    /* If the old tail was free and contiguous, merge straight away. */
    if (tail->free && (uint8_t *) tail + tail->size == (uint8_t *) n) {
        tail->size += n->size;
        tail->next  = n->next;
    }
}

static void grow_heap_for(uint64_t need) {
    size_t pages = (need + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    if (pages < KHEAP_GROW / PMM_PAGE_SIZE) {
        pages = KHEAP_GROW / PMM_PAGE_SIZE;
    }
    uint64_t base = heap_end;
    grow_pages(pages);
    append_free_region(base, pages * PMM_PAGE_SIZE);
}

void *kmalloc(size_t size) {
    if (!size) return NULL;
    size = (size + 15) & ~(size_t) 15;            /* 16-byte alignment     */
    uint64_t need = size + sizeof(struct block);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *b = head; b; b = b->next) {
            if (b->magic != HEAP_MAGIC) panic("heap: corrupt free-list magic");
            if (b->free && b->size >= need) {
                split(b, need);
                b->free = 0;
                return (void *) ((uint8_t *) b + sizeof(struct block));
            }
        }
        grow_heap_for(need);
    }

    panic("kmalloc: out of memory after grow");
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct block *b = (struct block *) ((uint8_t *) ptr - sizeof(struct block));
    if (b->magic != HEAP_MAGIC) panic("kfree: bad or already-freed pointer");
    b->free = 1;

    /* Forward coalesce: merge with the physically-adjacent next block. */
    if (b->next && b->next->free
        && (uint8_t *) b + b->size == (uint8_t *) b->next) {
        b->size += b->next->size;
        b->next  = b->next->next;
    }

    /* Backward coalesce: find the predecessor the linear way. The heap is
     * small and allocations are rare early on, so the O(n) walk is fine. */
    struct block *prev = head;
    while (prev && prev->next != b) prev = prev->next;
    if (prev && prev->free
        && (uint8_t *) prev + prev->size == (uint8_t *) b) {
        prev->size += b->size;
        prev->next  = b->next;
    }
}

void heap_dump(void) {
    uint64_t free_total = 0, used_total = 0;
    int free_count = 0, used_count = 0;
    for (struct block *b = head; b; b = b->next) {
        if (b->free) { free_total += b->size; free_count++; }
        else         { used_total += b->size; used_count++; }
    }
    kprintf("[heap] free=%u bytes (%u blocks) used=%u bytes (%u blocks) "
            "end=%p\n",
            (unsigned) free_total, (unsigned) free_count,
            (unsigned) used_total, (unsigned) used_count,
            (void *) heap_end);
}
