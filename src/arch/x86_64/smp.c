#include "smp.h"
#include "apic.h"
#include "gdt.h"
#include "idt.h"

#include "../../kprintf.h"
#include "../../mm/heap.h"
#include "../../panic.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

#define AP_KERNEL_STACK_SIZE (16 * 1024)

extern volatile struct limine_smp_request smp_request;

static struct cpu_local *cpus       = NULL;
static uint64_t          cpu_count  = 0;
static uint64_t          online     = 0;    /* updated atomically            */

/* Entered by each application processor after Limine brings it to long mode
 * with paging on. We are still on a Limine-provided stack; we immediately
 * switch to our per-CPU stack, reload GDT/IDT, enable the local APIC, and
 * mark ourselves online. No interrupts here — a per-CPU TSS is needed
 * before APs can safely take IRQs, and that comes later. */
static void ap_entry(struct limine_smp_info *info) {
    struct cpu_local *me = (struct cpu_local *) info->extra_argument;

    /* Hand the AP a deterministic stack before doing anything else that
     * might push. */
    __asm__ volatile ("mov %0, %%rsp" :: "r"(me->kernel_stack_top) : "memory");

    gdt_load();
    idt_load();
    lapic_enable_local();

    __atomic_add_fetch(&online, 1, __ATOMIC_RELEASE);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void smp_init(void) {
    if (!smp_request.response) {
        kprintf("[smp] no response from bootloader — staying single-core\n");
        cpu_count = 1;
        online    = 1;
        return;
    }

    struct limine_smp_response *r = smp_request.response;
    cpu_count = r->cpu_count;
    uint32_t bsp_lapic = r->bsp_lapic_id;

    kprintf("[smp] %u CPU(s) reported, BSP lapic=%u, flags=0x%x\n",
            (unsigned) cpu_count, (unsigned) bsp_lapic,
            (unsigned) r->flags);

    cpus = kmalloc(cpu_count * sizeof(struct cpu_local));
    if (!cpus) panic("smp: kmalloc(cpus) failed");

    /* BSP is already running — fill its slot, count it online, skip kick. */
    online = 1;

    for (uint64_t i = 0; i < cpu_count; i++) {
        struct limine_smp_info *info = r->cpus[i];
        cpus[i].cpu_id           = info->processor_id;
        cpus[i].lapic_id         = info->lapic_id;
        cpus[i].kernel_stack_top = 0;

        if (info->lapic_id == bsp_lapic) {
            continue;
        }

        /* Give this AP its own 16 KiB kernel stack. kmalloc returns the
         * base; the stack grows down, so rsp starts at base+size. */
        void *stack = kmalloc(AP_KERNEL_STACK_SIZE);
        if (!stack) panic("smp: kmalloc(ap_stack) failed");
        cpus[i].kernel_stack_top = (uint64_t) stack + AP_KERNEL_STACK_SIZE;

        /* Pass our cpu_local pointer through extra_argument so the AP can
         * find it on entry. Writing goto_address last is what wakes the AP. */
        info->extra_argument = (uint64_t) &cpus[i];
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_RELEASE);
    }

    /* Wait for every AP to reach ap_entry's atomic increment. */
    while (__atomic_load_n(&online, __ATOMIC_ACQUIRE) < cpu_count) {
        __asm__ volatile ("pause");
    }

    kprintf("[smp] %u CPU(s) online\n", (unsigned) cpu_count);
    for (uint64_t i = 0; i < cpu_count; i++) {
        kprintf("  cpu[%u] lapic=%u stack_top=%p%s\n",
                (unsigned) cpus[i].cpu_id,
                (unsigned) cpus[i].lapic_id,
                (void *) cpus[i].kernel_stack_top,
                cpus[i].lapic_id == bsp_lapic ? "  (BSP)" : "");
    }
}

uint64_t smp_online_count(void) {
    return __atomic_load_n(&online, __ATOMIC_ACQUIRE);
}
