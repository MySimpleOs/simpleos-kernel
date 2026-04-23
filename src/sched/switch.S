/* switch_context(uint64_t *old_rsp, uint64_t new_rsp)
 *
 * System V AMD64: rdi = old_rsp pointer, rsi = new_rsp.
 *
 * Save the six callee-saved registers onto the current stack, stash rsp
 * into *old_rsp, then load rsp from new_rsp and restore the callee-saved
 * registers that were stored by a previous call here (or seeded by
 * thread_create). The ret at the end pops the next rip — either the
 * caller's return address, or thread_trampoline for a brand-new thread. */

.section .text
.global switch_context
switch_context:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq  %rsp, (%rdi)
    movq  %rsi, %rsp

    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    retq
