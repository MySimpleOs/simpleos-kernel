#include "thread.h"
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
    struct thread *t = thread_current();
    if (t->fn) t->fn(t->arg);
    kprintf("[thread:%u] \"%s\" returned — parking\n", t->id, t->name);
    for (;;) { __asm__ volatile ("hlt"); }
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg) {
    struct thread *t = kmalloc(sizeof(*t));
    if (!t) panic("thread_create: kmalloc(thread) failed");

    uint8_t *stack = kmalloc(THREAD_STACK_SIZE);
    if (!stack) panic("thread_create: kmalloc(stack) failed");

    t->stack_base = (uint64_t) stack;
    t->stack_size = THREAD_STACK_SIZE;
    t->id         = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
    t->fn         = fn;
    t->arg        = arg;
    copy_name(t->name, name ? name : "thread");

    /* Seed the stack so switch_context can "return" into thread_trampoline.
     * Layout from top (high address) downward:
     *   [trampoline_addr]  <- retq target
     *   [rbp]
     *   [rbx]
     *   [r12]
     *   [r13]
     *   [r14]
     *   [r15]              <- rsp points here when resumed
     */
    uint64_t *sp = (uint64_t *) (t->stack_base + THREAD_STACK_SIZE);
    *--sp = (uint64_t) thread_trampoline;
    *--sp = 0; /* rbp */
    *--sp = 0; /* rbx */
    *--sp = 0; /* r12 */
    *--sp = 0; /* r13 */
    *--sp = 0; /* r14 */
    *--sp = 0; /* r15 */
    t->rsp = (uint64_t) sp;

    /* Insert at the tail of the circular run queue. */
    if (!current) {
        t->next = t;
        current = t;
    } else {
        t->next = current->next;
        current->next = t;
    }
    return t;
}

void sched_init(struct thread *bootstrap) {
    bootstrap->id       = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
    bootstrap->next     = bootstrap;
    bootstrap->fn       = NULL;
    bootstrap->arg      = NULL;
    copy_name(bootstrap->name, "bsp-idle");
    current = bootstrap;
}

void thread_yield(void) {
    struct thread *prev = current;
    if (!prev || prev->next == prev) return;

    struct thread *next = prev->next;
    current = next;
    switch_context(&prev->rsp, next->rsp);
}
