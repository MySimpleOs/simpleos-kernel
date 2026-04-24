#include "apic.h"

#include "../../drivers/keyboard.h"
#include "../../drivers/mouse.h"
#include "../../kprintf.h"
#include "../../panic.h"
#include "../../sched/thread.h"

#include <stdint.h>

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

static const char *exc_name(uint64_t v) {
    static const char *names[32] = {
        [0]  = "divide-error",        [1]  = "debug",
        [2]  = "nmi",                 [3]  = "breakpoint",
        [4]  = "overflow",            [5]  = "bound-range",
        [6]  = "invalid-opcode",      [7]  = "device-not-available",
        [8]  = "double-fault",        [9]  = "coprocessor-segment",
        [10] = "invalid-tss",         [11] = "segment-not-present",
        [12] = "stack-segment-fault", [13] = "general-protection",
        [14] = "page-fault",          [15] = "reserved-15",
        [16] = "x87-fpu",             [17] = "alignment-check",
        [18] = "machine-check",       [19] = "simd-fpu",
        [20] = "virtualization",      [21] = "control-protection",
        [22] = "reserved-22",         [23] = "reserved-23",
        [24] = "reserved-24",         [25] = "reserved-25",
        [26] = "reserved-26",         [27] = "reserved-27",
        [28] = "hypervisor-injection",[29] = "vmm-communication",
        [30] = "security",            [31] = "reserved-31",
    };
    return (v < 32 && names[v]) ? names[v] : "unknown";
}

static uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static void dump_frame(struct interrupt_frame *f) {
    kprintf("  rip=%p  cs=%x  rflags=%p\n",
            (void *) f->rip, (unsigned) f->cs, (void *) f->rflags);
    kprintf("  rsp=%p  ss=%x  cr2=%p\n",
            (void *) f->rsp, (unsigned) f->ss, (void *) read_cr2());
    kprintf("  rax=%p  rbx=%p  rcx=%p  rdx=%p\n",
            (void *) f->rax, (void *) f->rbx, (void *) f->rcx, (void *) f->rdx);
    kprintf("  rsi=%p  rdi=%p  rbp=%p\n",
            (void *) f->rsi, (void *) f->rdi, (void *) f->rbp);
    kprintf("  r8 =%p  r9 =%p  r10=%p  r11=%p\n",
            (void *) f->r8,  (void *) f->r9,  (void *) f->r10, (void *) f->r11);
    kprintf("  r12=%p  r13=%p  r14=%p  r15=%p\n",
            (void *) f->r12, (void *) f->r13, (void *) f->r14, (void *) f->r15);
}

void interrupt_dispatch(struct interrupt_frame *frame) {
    if (frame->vector < 32) {
        /* Breakpoint is the only exception we want to survive for now.
         * Print a short diagnostic and return; all others are fatal. */
        if (frame->vector == 3) {
            kprintf("[exc] breakpoint at %p  cs=0x%x  (ring %u)  rflags=%p\n",
                    (void *) frame->rip,
                    (unsigned) frame->cs,
                    (unsigned) (frame->cs & 3),
                    (void *) frame->rflags);
            return;
        }

        kprintf("\n[exc] %s  (vec=%u  err=0x%x)\n",
                exc_name(frame->vector),
                (unsigned) frame->vector,
                (unsigned) frame->error_code);
        dump_frame(frame);
        panic("CPU exception");
    }

    if (frame->vector == LAPIC_TIMER_VECTOR) {
        timer_ticks++;
        lapic_eoi();
        /* Preemption: every tick, hand off to the next runnable thread. */
        thread_yield();
        return;
    }

    if (frame->vector == KEYBOARD_VECTOR) {
        keyboard_handle_irq();
        lapic_eoi();
        return;
    }

    if (frame->vector == MOUSE_VECTOR) {
        mouse_handle_irq();
        lapic_eoi();
        return;
    }

    kprintf("[irq] unhandled vector %u — sending EOI\n",
            (unsigned) frame->vector);
    lapic_eoi();
}
