#include "thread.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/syscall.h"
#include "../kprintf.h"
#include "../mm/heap.h"
#include "../panic.h"

#include <stdint.h>
#include <stddef.h>

#define THREAD_STACK_SIZE (16 * 1024)

static struct thread *current = NULL;
static uint32_t       next_id = 0;

struct thread *thread_current(void) {
    return current;
}

static void copy_name(char *dst, const char *src) {
    int i = 0;
    while (i < THREAD_NAME_LEN - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* Landing pad for a freshly-scheduled thread. switch_context's retq pops
 * &thread_trampoline off the prepared stack, so execution starts here. We
 * read the current thread's fn/arg (set by thread_create) and call it.
 * When it returns, park the thread in a hlt loop — thread exit is a Faz
 * 7.2/7.4 feature. */
static void thread_trampoline(void) {
    /* We land here via switch_context's retq, which never touches rflags.
     * If the switch was driven from an IRQ handler (the usual case for
     * preemption), interrupts are still disabled. Re-enable so the new
     * thread can itself be preempted. */
    __asm__ volatile ("sti");

    struct thread *t = thread_current();
    if (t->fn) t->fn(t->arg);
    thread_exit();
}

/* Entered (via switch_context retq) by a brand-new ring-3 thread. Builds
 * an iret frame on the kernel stack and iretq's into user space. From
 * here the thread runs at CPL=3 until a fault or IRQ pulls it back in. */
static void user_trampoline(void) {
    struct thread *t = thread_current();
    tss_set_kernel_stack(t->kernel_stack_top);

    uint64_t user_cs  = GDT_USER_CODE | 3;  /* ring-3 RPL                  */
    uint64_t user_ss  = GDT_USER_DATA | 3;
    uint64_t rflags   = 0x202;              /* IF=1, reserved bit 1 = 1    */

    __asm__ volatile (
        "pushq %[ss]\n"
        "pushq %[rsp]\n"
        "pushq %[rfl]\n"
        "pushq %[cs]\n"
        "pushq %[rip]\n"
        "iretq\n"
        : /* no outputs */
        : [ss]  "r" (user_ss),
          [rsp] "r" (t->user_stack_top),
          [rfl] "r" (rflags),
          [cs]  "r" (user_cs),
          [rip] "r" (t->user_rip)
        : "memory"
    );
    __builtin_unreachable();
}

static struct thread *alloc_thread(const char *name) {
    struct thread *t = kmalloc(sizeof(*t));
    if (!t) panic("thread: kmalloc(thread) failed");

    uint8_t *stack = kmalloc(THREAD_STACK_SIZE);
    if (!stack) panic("thread: kmalloc(stack) failed");

    t->stack_base       = (uint64_t) stack;
    t->stack_size       = THREAD_STACK_SIZE;
    t->kernel_stack_top = (uint64_t) stack + THREAD_STACK_SIZE;
    t->id               = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
    t->is_user          = 0;
    t->fn               = NULL;
    t->arg              = NULL;
    t->user_rip         = 0;
    t->user_stack_top   = 0;
    copy_name(t->name, name ? name : "thread");
    return t;
}

static void insert_queue(struct thread *t) {
    if (!current) {
        t->next = t;
        current = t;
    } else {
        t->next = current->next;
        current->next = t;
    }
}

/* Seed the kernel stack so the first switch_context into this thread lands
 * at `entry`. Layout from top downward:
 *   [entry]           <- retq target after callee-saved pops
 *   [rbp][rbx][r12][r13][r14][r15]  <- 6 zeroed callee-saved */
static void seed_stack(struct thread *t, void (*entry)(void)) {
    uint64_t *sp = (uint64_t *) (t->stack_base + THREAD_STACK_SIZE);
    *--sp = (uint64_t) entry;
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;
    t->rsp = (uint64_t) sp;
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg) {
    struct thread *t = alloc_thread(name);
    t->fn  = fn;
    t->arg = arg;
    seed_stack(t, thread_trampoline);
    insert_queue(t);
    return t;
}

struct thread *thread_create_user(const char *name, uint64_t user_rip,
                                  uint64_t user_stack_top) {
    struct thread *t = alloc_thread(name);
    t->is_user        = 1;
    t->user_rip       = user_rip;
    t->user_stack_top = user_stack_top;
    seed_stack(t, user_trampoline);
    insert_queue(t);
    return t;
}

void sched_init(struct thread *bootstrap) {
    bootstrap->id               = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
    bootstrap->next             = bootstrap;
    bootstrap->fn               = NULL;
    bootstrap->arg              = NULL;
    bootstrap->is_user          = 0;
    bootstrap->stack_base       = 0;
    bootstrap->stack_size       = 0;
    bootstrap->kernel_stack_top = 0;
    bootstrap->user_rip         = 0;
    bootstrap->user_stack_top   = 0;
    copy_name(bootstrap->name, "bsp-idle");
    current = bootstrap;
}

void thread_yield(void) {
    struct thread *prev = current;
    if (!prev || prev->next == prev) return;

    struct thread *next = prev->next;
    /* Update TSS.rsp0 (IRQs from ring 3) and the syscall entry scratch
     * (SYSCALL from ring 3) so both paths land on this thread's own
     * kernel stack. No-op for kernel-only threads. */
    if (next->kernel_stack_top) {
        tss_set_kernel_stack(next->kernel_stack_top);
        syscall_kernel_rsp = next->kernel_stack_top;
    }
    current = next;
    switch_context(&prev->rsp, next->rsp);
}

void thread_exit(void) {
    struct thread *me = current;
    if (!me) panic("thread_exit: no current");

    /* Unlink from the circular run queue. */
    struct thread *prev = me;
    while (prev->next != me) prev = prev->next;
    prev->next = me->next;

    kprintf("[sched] thread:%u \"%s\" exited\n", me->id, me->name);

    /* Hand off to the remaining queue. We still call switch_context so
     * register state is consistent — the saved state on our own stack is
     * unreferenced afterward since we never run again. */
    struct thread *next = me->next;
    current = next;
    uint64_t dead_rsp;
    switch_context(&dead_rsp, next->rsp);
    (void) dead_rsp;

    /* Unreachable — but keep the compiler happy about noreturn. */
    for (;;) { __asm__ volatile ("cli; hlt"); }
}
