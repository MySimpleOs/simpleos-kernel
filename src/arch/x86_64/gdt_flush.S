/* Load a new GDT, flush segment registers, and load the TSS.
 * System V AMD64 ABI: first arg in %rdi, so gdt_flush takes the descriptor
 * pointer there. tss_flush takes no args. */

.section .text

.global gdt_flush
gdt_flush:
    lgdt (%rdi)

    /* Reload data segment registers to the new kernel data selector. */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* Far return to reload %cs with the new kernel code selector. */
    popq %rcx                   /* save caller return address       */
    pushq $0x08                 /* new CS = KERNEL_CODE             */
    pushq %rcx                  /* return address we just popped    */
    lretq

.global tss_flush
tss_flush:
    movw $0x28, %ax             /* TSS selector (RPL=0)             */
    ltr %ax
    retq
