/* SYSCALL entry.
 *
 * On entry (from ring 3):
 *   rcx = user rip
 *   r11 = user rflags (with IF/DF masked out per FMASK)
 *   rsp = user rsp (unchanged by SYSCALL)
 *   cs / ss loaded from STAR
 *   interrupts disabled (FMASK cleared IF)
 *
 * We switch to this thread's kernel stack, lay an iret-compatible frame
 * plus the full GP register save, call into the C dispatcher, then iretq
 * back to user space. IRETQ is used instead of SYSRETQ so we can reuse the
 * existing GDT layout without rearranging the user code/data slots.
 */

.section .text
.global syscall_entry
syscall_entry:
    /* Swap to kernel stack. syscall_kernel_rsp is updated by the scheduler
     * on every yield so it always holds the current thread's kernel top. */
    movq %rsp, user_rsp_scratch(%rip)
    movq syscall_kernel_rsp(%rip), %rsp

    /* Build an iretq frame so we can return via iretq. */
    pushq $0x23                 /* user SS  (data | RPL 3)                 */
    pushq user_rsp_scratch(%rip)
    pushq %r11                  /* user rflags                             */
    pushq $0x1b                 /* user CS  (code | RPL 3)                 */
    pushq %rcx                  /* user rip                                */

    /* Save GP registers in the order the struct syscall_frame expects
     * (top-of-stack is the first field). */
    pushq %rax
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    movq %rsp, %rdi             /* pointer to struct syscall_frame         */
    call syscall_dispatch

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rax

    iretq

/* Tiny scratch slot for the user rsp during the two-instruction stack
 * swap.  Single-CPU today; a per-CPU slot arrives with swapgs later. */
.section .bss
.global user_rsp_scratch
user_rsp_scratch:
    .quad 0
