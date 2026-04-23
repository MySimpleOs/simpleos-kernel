#pragma once

#include <stdint.h>

#define THREAD_NAME_LEN 16

struct thread {
    uint64_t rsp;                          /* saved when not running          */
    uint64_t stack_base;                   /* kmalloc'd stack                 */
    uint64_t stack_size;
    uint32_t id;
    char     name[THREAD_NAME_LEN];
    struct thread *next;                   /* run-queue link (circular)       */
    void   (*fn)(void *);
    void    *arg;
};

/* Context switch: save callee-saved registers into the current stack and
 * store the resulting rsp into *old_rsp, then switch rsp to new_rsp and
 * restore callee-saved registers from there. */
extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);

/* Prepare a new thread. Stack is allocated via kmalloc and pre-populated so
 * the first switch into the thread lands inside thread_trampoline, which
 * invokes fn(arg) and then parks the thread when fn returns. */
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg);

/* Cooperative yield: hand control to the next runnable thread. Safe to call
 * from inside a thread body; does nothing if no other thread is ready. */
void thread_yield(void);

/* Install an initial scheduler state rooted at `bootstrap` (the BSP's
 * "thread 0"). Must be called once, from kmain, before thread_yield. */
void sched_init(struct thread *bootstrap);

struct thread *thread_current(void);
