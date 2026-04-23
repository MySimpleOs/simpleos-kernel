#pragma once

#include <stddef.h>
#include <stdint.h>

#define THREAD_NAME_LEN 16
#define THREAD_FD_MAX   32

struct vnode;

struct file_desc {
    struct vnode *node;
    size_t        offset;
    int           in_use;
};

struct thread {
    uint64_t rsp;                          /* saved when not running          */
    uint64_t stack_base;                   /* kmalloc'd kernel stack          */
    uint64_t stack_size;
    uint64_t kernel_stack_top;             /* top of kernel stack, for TSS.rsp0 */
    uint32_t id;
    uint32_t is_user;                      /* 0 = kernel, 1 = ring-3 payload  */
    char     name[THREAD_NAME_LEN];
    struct thread *next;                   /* run-queue link (circular)       */
    void   (*fn)(void *);
    void    *arg;
    uint64_t user_rip;                     /* entry point in user space       */
    uint64_t user_stack_top;               /* top of ring-3 stack             */
    struct   file_desc fds[THREAD_FD_MAX]; /* per-thread open files           */
};

/* Context switch: save callee-saved registers into the current stack and
 * store the resulting rsp into *old_rsp, then switch rsp to new_rsp and
 * restore callee-saved registers from there. */
extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);

/* Prepare a new thread. Stack is allocated via kmalloc and pre-populated so
 * the first switch into the thread lands inside thread_trampoline, which
 * invokes fn(arg) and then parks the thread when fn returns. */
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg);

/* Kernel-side constructor for a user thread. Caller arranges user_rip and
 * user_stack_top inside already-mapped USER pages; thread_create_user just
 * wires up the kernel side and seeds a trampoline that iretqs to ring 3. */
struct thread *thread_create_user(const char *name,
                                  uint64_t user_rip,
                                  uint64_t user_stack_top);

/* Cooperative yield: hand control to the next runnable thread. Safe to call
 * from inside a thread body; does nothing if no other thread is ready.
 * Also safe to invoke from the timer IRQ handler so it doubles as the
 * preemption entry point. */
void thread_yield(void);

/* Remove the current thread from the run queue and switch away forever.
 * Stack and thread struct are leaked for now — a reaper comes with ring 3. */
__attribute__((noreturn))
void thread_exit(void);

/* Install an initial scheduler state rooted at `bootstrap` (the BSP's
 * "thread 0"). Must be called once, from kmain, before thread_yield. */
void sched_init(struct thread *bootstrap);

struct thread *thread_current(void);
