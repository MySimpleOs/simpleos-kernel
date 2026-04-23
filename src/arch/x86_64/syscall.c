#include "syscall.h"
#include "../../drivers/console.h"
#include "../../drivers/keyboard.h"
#include "../../fs/vfs.h"
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
    SYS_READ    = 0,
    SYS_WRITE   = 1,
    SYS_OPEN    = 2,
    SYS_CLOSE   = 3,
    SYS_EXIT    = 60,
    SYS_READDIR = 89,     /* arbitrary, not POSIX-standard                   */
};

static int64_t sys_write(int fd, const char *buf, size_t count) {
    if (fd != 1 && fd != 2) return -1;    /* only stdout/stderr for now      */
    /* Mirror to both the serial log (for remote debugging) and the on-
     * screen console (for the interactive user). */
    for (size_t i = 0; i < count; i++) {
        kprintf("%c", buf[i]);
    }
    console_write(buf, count);
    return (int64_t) count;
}

static int64_t sys_open(const char *path, int flags) {
    (void) flags;                          /* O_RDONLY only today             */
    struct vnode *v = vfs_lookup(path);
    if (!v) return -1;                     /* accept both files and dirs     */

    struct thread *t = thread_current();
    /* fds 0/1/2 are conceptually stdin/stdout/stderr even though we don't
     * formally track them, so start searching from 3. */
    for (int i = 3; i < THREAD_FD_MAX; i++) {
        if (!t->fds[i].in_use) {
            t->fds[i].node   = v;
            t->fds[i].offset = 0;
            t->fds[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static int64_t sys_close(int fd) {
    if (fd < 0 || fd >= THREAD_FD_MAX) return -1;
    struct thread *t = thread_current();
    if (!t->fds[fd].in_use) return -1;
    t->fds[fd].in_use = 0;
    t->fds[fd].node   = NULL;
    t->fds[fd].offset = 0;
    return 0;
}

static int64_t sys_read(int fd, void *buf, size_t count) {
    if (fd == 0) {
        /* Block on the keyboard ring: yield between polls so the timer and
         * other threads keep running. Returns as soon as any byte arrives. */
        while (!stdin_has_data()) {
            thread_yield();
        }
        return (int64_t) stdin_try_read((char *) buf, count);
    }
    if (fd < 0 || fd >= THREAD_FD_MAX) return -1;
    struct thread *t = thread_current();
    if (!t->fds[fd].in_use) return -1;

    int64_t n = vfs_read(t->fds[fd].node, t->fds[fd].offset, count, buf);
    if (n > 0) t->fds[fd].offset += (size_t) n;
    return n;
}

static int64_t sys_readdir(int fd, char *name_buf, size_t buf_size) {
    if (fd < 0 || fd >= THREAD_FD_MAX || buf_size == 0) return -1;
    struct thread *t = thread_current();
    if (!t->fds[fd].in_use) return -1;

    struct vnode *v = t->fds[fd].node;
    if (!v || v->type != VFS_DIR) return -1;

    size_t       idx = t->fds[fd].offset;
    struct vnode *c  = v->children;
    for (size_t i = 0; i < idx && c; i++) c = c->next_sibling;
    if (!c) return 0;                      /* end of directory                */

    size_t len = 0;
    while (c->name[len]) len++;
    if (len >= buf_size) len = buf_size - 1;
    for (size_t i = 0; i < len; i++) name_buf[i] = c->name[i];
    name_buf[len] = 0;

    t->fds[fd].offset = idx + 1;
    return (int64_t) (len + 1);
}

__attribute__((noreturn))
static void sys_exit(int code) {
    kprintf("[syscall] sys_exit: user thread requested exit (code=%d)\n", code);
    thread_exit();
}

void syscall_dispatch(struct syscall_frame *f) {
    switch (f->rax) {
        case SYS_READ:
            f->rax = (uint64_t) sys_read((int) f->rdi, (void *) f->rsi,
                                         (size_t) f->rdx);
            return;
        case SYS_WRITE:
            f->rax = (uint64_t) sys_write((int) f->rdi,
                                          (const char *) f->rsi,
                                          (size_t) f->rdx);
            return;
        case SYS_OPEN:
            f->rax = (uint64_t) sys_open((const char *) f->rdi, (int) f->rsi);
            return;
        case SYS_CLOSE:
            f->rax = (uint64_t) sys_close((int) f->rdi);
            return;
        case SYS_READDIR:
            f->rax = (uint64_t) sys_readdir((int) f->rdi,
                                            (char *) f->rsi,
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
