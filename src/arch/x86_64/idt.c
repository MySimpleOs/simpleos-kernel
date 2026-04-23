#include "idt.h"
#include "gdt.h"
#include "../../kprintf.h"

#include <stdint.h>

struct idt_entry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} __attribute__((packed));

struct idt_descriptor {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry      idt[256];
static struct idt_descriptor idt_desc;

extern void idt_flush(struct idt_descriptor *desc);

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void isr32(void); extern void isr33(void); extern void isr34(void); extern void isr35(void);
extern void isr36(void); extern void isr37(void); extern void isr38(void); extern void isr39(void);
extern void isr40(void); extern void isr41(void); extern void isr42(void); extern void isr43(void);
extern void isr44(void); extern void isr45(void); extern void isr46(void); extern void isr47(void);

static void idt_set(int vec, void (*handler)(void), uint8_t type_attr) {
    uint64_t addr = (uint64_t) handler;
    idt[vec].offset_lo  = addr & 0xFFFF;
    idt[vec].selector   = GDT_KERNEL_CODE;
    idt[vec].ist        = 0;
    idt[vec].type_attr  = type_attr;
    idt[vec].offset_mid = (addr >> 16) & 0xFFFF;
    idt[vec].offset_hi  = (addr >> 32) & 0xFFFFFFFF;
    idt[vec].reserved   = 0;
}

void idt_load(void) {
    idt_flush(&idt_desc);
}

void idt_init(void) {
    void (*stubs[48])(void) = {
        isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
        isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
        isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39,
        isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
    };
    for (int i = 0; i < 48; i++) {
        /* 0x8E: present | DPL=0 | 64-bit interrupt gate */
        idt_set(i, stubs[i], 0x8E);
    }

    idt_desc.limit = sizeof(idt) - 1;
    idt_desc.base  = (uint64_t) idt;
    idt_flush(&idt_desc);

    kprintf("[idt] loaded @ %p (%u entries, vectors 0-47 wired)\n",
            idt, (unsigned) (sizeof(idt) / sizeof(idt[0])));
}
