#pragma once

#include <stdint.h>

/* Enable SYSCALL/SYSRET MSRs for the current CPU and point the entry at
 * syscall_entry. Call once on each CPU that should accept syscalls. */
void syscall_init(void);

/* Updated by the scheduler with the incoming thread's kernel stack top, so
 * syscall_entry.S can switch stacks without touching the TSS. */
extern uint64_t syscall_kernel_rsp;

/* Full saved state layout pushed by syscall_entry.S, used by the C
 * dispatcher to pull syscall args. */
struct syscall_frame {
    uint64_t r15, r14, r13, r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r11;            /* user rflags — also in iret frame           */
    uint64_t r10;            /* 4th arg                                    */
    uint64_t r9;             /* 6th arg                                    */
    uint64_t r8;             /* 5th arg                                    */
    uint64_t rcx;            /* user rip — also in iret frame              */
    uint64_t rdx;            /* 3rd arg                                    */
    uint64_t rsi;            /* 2nd arg                                    */
    uint64_t rdi;            /* 1st arg                                    */
    uint64_t rax;            /* syscall number in / return value out       */
};

/* C-side dispatcher called from syscall_entry.S. Reads rax for the number,
 * routes to a handler, stores the return value back into frame->rax. */
void syscall_dispatch(struct syscall_frame *frame);
