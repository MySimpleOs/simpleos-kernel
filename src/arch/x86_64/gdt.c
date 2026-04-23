#include "gdt.h"
#include "../../kprintf.h"

#include <stdint.h>

struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_hi;
    uint8_t  base_hi;
} __attribute__((packed));

struct gdt_tss_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_hi;
    uint8_t  base_hi;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct gdt_descriptor {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

/* 5 regular entries + one 16-byte TSS entry (two slots) = 7 slots. */
static struct {
    struct gdt_entry     entries[5];
    struct gdt_tss_entry tss;
} __attribute__((packed)) gdt;

static struct gdt_descriptor gdt_desc;
static struct tss            kernel_tss;

/* 16 KiB boot kernel stack; rsp0 in the TSS points just past the top so
 * interrupts taken while in user mode (later) land here. */
static uint8_t kernel_stack[16 * 1024] __attribute__((aligned(16)));

static void set_entry(struct gdt_entry *e, uint8_t access, uint8_t flags) {
    e->limit_lo       = 0;
    e->base_lo        = 0;
    e->base_mid       = 0;
    e->access         = access;
    e->flags_limit_hi = flags & 0xF0;
    e->base_hi        = 0;
}

static void set_tss_entry(struct gdt_tss_entry *e, uint64_t base, uint32_t limit) {
    e->limit_lo       = limit & 0xFFFF;
    e->base_lo        = base  & 0xFFFF;
    e->base_mid       = (base >> 16) & 0xFF;
    e->access         = 0x89;              /* P=1 DPL=0 type=0x9 (64-bit TSS avail) */
    e->flags_limit_hi = (limit >> 16) & 0x0F;
    e->base_hi        = (base >> 24) & 0xFF;
    e->base_upper     = (uint32_t) (base >> 32);
    e->reserved       = 0;
}

extern void gdt_flush(struct gdt_descriptor *desc);
extern void tss_flush(void);

void tss_set_kernel_stack(uint64_t rsp0) {
    kernel_tss.rsp0 = rsp0;
}

void gdt_load(void) {
    gdt_flush(&gdt_desc);
}

void gdt_init(void) {
    set_entry(&gdt.entries[0], 0x00, 0x00);  /* null                                 */
    set_entry(&gdt.entries[1], 0x9A, 0xA0);  /* kernel code: P DPL0 S code R, L=1    */
    set_entry(&gdt.entries[2], 0x92, 0xA0);  /* kernel data: P DPL0 S data W         */
    set_entry(&gdt.entries[3], 0xFA, 0xA0);  /* user code:   P DPL3 S code R, L=1    */
    set_entry(&gdt.entries[4], 0xF2, 0xA0);  /* user data:   P DPL3 S data W         */

    kernel_tss = (struct tss){ 0 };
    kernel_tss.rsp0 = (uint64_t) (kernel_stack + sizeof(kernel_stack));
    kernel_tss.iopb_offset = sizeof(struct tss);

    set_tss_entry(&gdt.tss, (uint64_t) &kernel_tss, sizeof(kernel_tss) - 1);

    gdt_desc.limit = sizeof(gdt) - 1;
    gdt_desc.base  = (uint64_t) &gdt;

    gdt_flush(&gdt_desc);
    tss_flush();

    kprintf("[gdt] loaded @ %p (%u bytes), rsp0=%p\n",
            &gdt, (unsigned) sizeof(gdt), (void *) kernel_tss.rsp0);
}
