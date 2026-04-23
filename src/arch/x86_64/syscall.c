#include "syscall.h"
#include "../../kprintf.h"
#include "../../sched/thread.h"

#include <stdint.h>
#include <stddef.h>

#define MSR_IA32_EFER  0xC0000080
#define MSR_STAR       0xC0000081
#define MSR_LSTAR      0xC0000082
#define MSR_FMASK      0xC0000084

#define EFER_SCE       (1ULL << 0)

/* GDT selectors at current layout (gdt.h). SYSCALL / IRETQ back to ring 3
 * will use these literal values. */
#define KERNEL_CS 0x08
#define USER_CS   (0x18 | 3)
#define USER_SS   (0x20 | 3)

uint64_t syscall_kernel_rsp = 0;

extern void syscall_entry(void);

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t) hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t) value;
    uint32_t hi = (uint32_t) (value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

void syscall_init(void) {
    /* STAR: kernel base in bits 47:32, user base in bits 63:48. We use
     * IRETQ (not SYSRETQ) to return, so the user base is informational and
     * can reuse our kernel CS here — only the kernel base actually matters
     * for SYSCALL entry. */
    uint64_t star =
          ((uint64_t) KERNEL_CS)    << 32   /* CS = 0x08 on syscall entry */
        | ((uint64_t) KERNEL_CS)    << 48;
    wrmsr(MSR_STAR,  star);

    wrmsr(MSR_LSTAR, (uint64_t) syscall_entry);

    /* Mask IF and DF when entering syscall: interrupts off during the
     * critical kernel-stack swap, direction flag cleared so rep memcpy-like
     * paths are safe regardless of what userland left behind. */
    wrmsr(MSR_FMASK, (1ULL << 9) | (1ULL << 10));

    uint64_t efer = rdmsr(MSR_IA32_EFER);
    wrmsr(MSR_IA32_EFER, efer | EFER_SCE);

    kprintf("[syscall] entry=%p, STAR=%p, EFER.SCE on\n",
            (void *) syscall_entry, (void *) star);
}

/* ---------------- syscall table ---------------- */

enum {
    SYS_WRITE = 1,
    SYS_EXIT  = 60,
};

static int64_t sys_write(int fd, const char *buf, size_t count) {
    (void) fd;
    /* Ring-3 buffer — single address space today, so the kernel can touch
     * it directly. A real VM-safe copy will arrive with per-process PML4s. */
    for (size_t i = 0; i < count; i++) {
        kprintf("%c", buf[i]);
    }
    return (int64_t) count;
}

__attribute__((noreturn))
static void sys_exit(int code) {
    kprintf("[syscall] sys_exit: user thread requested exit (code=%d)\n", code);
    thread_exit();
}

void syscall_dispatch(struct syscall_frame *f) {
    switch (f->rax) {
        case SYS_WRITE:
            f->rax = (uint64_t) sys_write((int) f->rdi,
                                          (const char *) f->rsi,
                                          (size_t) f->rdx);
            return;
        case SYS_EXIT:
            sys_exit((int) f->rdi);
            /* not reached */
        default:
            kprintf("[syscall] unknown number %u\n", (unsigned) f->rax);
            f->rax = (uint64_t) -1;
            return;
    }
}
