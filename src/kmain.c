/* SimpleOS kernel entry — Faz 3 framebuffer + Faz 4.1 serial bring-up.
 *
 * Draws a test pattern on the framebuffer so the Limine handoff is visible
 * at a glance, then logs boot details over COM1 serial and halts. The
 * serial log is the primary debug channel from here on.
 */

#include <stddef.h>
#include <stdint.h>
#include <limine.h>

#include "kprintf.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/apic.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/ioapic.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/serial.h"
#include "arch/x86_64/smp.h"
#include "drivers/keyboard.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/thread.h"

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile uint64_t limine_base_revision[3];

static void hang(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static void idle(void) {
    for (;;) {
        __asm__ volatile ("sti; hlt");
    }
}

extern volatile uint64_t timer_ticks;

/* Demo thread body: print "tick N" five times, roughly one per second,
 * using the LAPIC timer count as a lazy clock. The thread gets preempted
 * off-CPU continuously between busy-wait iterations. */
static void thread_tick_demo(void *arg) {
    char tag = (char) (uintptr_t) arg;
    uint64_t last = timer_ticks;
    for (int i = 1; i <= 5; i++) {
        while (timer_ticks - last < 100) {
            __asm__ volatile ("pause");
        }
        last = timer_ticks;
        kprintf("[thread-%c] tick %d\n", tag, i);
    }
}

#define USER_CODE_VA  0x0000000000400000ULL
#define USER_STACK_VA 0x000000007ffff000ULL

/* Spawn a minimal ring-3 thread so we can watch a breakpoint fire at CPL=3.
 *
 * The user code page holds three bytes —
 *   cc      int3           ; trap to kernel, handler logs the cs=0x1b proof
 *   eb fe   jmp self       ; infinite loop after the return from int3
 * — padded to a page. int3 handler's iretq resumes the user thread at the
 * jmp, which then spins until the timer preempts it. That lets the log
 * show one breakpoint event followed by normal timer + scheduler activity
 * (because the kernel idle / other threads keep progressing while the
 * user thread lives in its jmp loop). */
static void spawn_user_demo(void) {
    uint64_t code_phys  = pmm_alloc_page();
    uint64_t stack_phys = pmm_alloc_page();

    vmm_map(USER_CODE_VA,  code_phys,  4096, VMM_USER);           /* RX, no W */
    vmm_map(USER_STACK_VA, stack_phys, 4096, VMM_USER | VMM_W | VMM_NX);

    /* Code bytes at virt 0x400000 via HHDM — vmm_map only added a user PTE,
     * it did not disturb Limine's kernel-side HHDM mapping of code_phys. */
    extern volatile struct limine_hhdm_request hhdm_request;
    uint64_t hhdm = hhdm_request.response ? hhdm_request.response->offset : 0;
    uint8_t *code = (uint8_t *) (code_phys + hhdm);
    code[0] = 0xcc;        /* int3 */
    code[1] = 0xeb;        /* jmp rel8 */
    code[2] = 0xfe;        /* -2 — jumps back to itself forever */

    thread_create_user("user-demo", USER_CODE_VA, USER_STACK_VA + 4096);
    kprintf("[user] thread ready at user rip=%p stack=%p\n",
            (void *) USER_CODE_VA, (void *) (USER_STACK_VA + 4096));
}

static void fill_rect(uint32_t *pixels, size_t stride,
                      size_t x0, size_t y0,
                      size_t w,  size_t h,
                      uint32_t color) {
    for (size_t y = y0; y < y0 + h; y++) {
        for (size_t x = x0; x < x0 + w; x++) {
            pixels[y * stride + x] = color;
        }
    }
}

void kmain(void) {
    serial_init();
    kprintf("\n[boot] SimpleOS kernel online\n");

    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        kprintf("[boot] PANIC: Limine base revision 3 not supported by loader\n");
        hang();
    }
    kprintf("[boot] limine base revision 3 accepted\n");

    gdt_init();
    idt_init();
    pic_disable();
    pmm_init();
    acpi_init();
    lapic_init();

    if (framebuffer_request.response == NULL
        || framebuffer_request.response->framebuffer_count < 1) {
        kprintf("[boot] PANIC: no framebuffer from bootloader\n");
        hang();
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    kprintf("[boot] framebuffer %ux%u bpp=%u pitch=%u @ %p\n",
            (unsigned) fb->width, (unsigned) fb->height,
            (unsigned) fb->bpp,   (unsigned) fb->pitch,
            fb->address);

    uint32_t *pixels = (uint32_t *) fb->address;
    size_t stride = (size_t) (fb->pitch / 4);
    size_t width  = (size_t) fb->width;
    size_t height = (size_t) fb->height;

    fill_rect(pixels, stride, 0, 0, width, height, 0xff0a1e3c);

    size_t box = 200;
    if (width > box && height > box) {
        size_t x0 = (width  - box) / 2;
        size_t y0 = (height - box) / 2;
        fill_rect(pixels, stride, x0, y0, box, box, 0xffff5533);
    }

    kprintf("[boot] framebuffer painted\n");

    lapic_timer_init(100);

    ioapic_init();
    keyboard_init();
    ioapic_set_irq(KEYBOARD_GSI, KEYBOARD_VECTOR, 0);

    heap_init();

    smp_init();

    /* Bootstrap the scheduler around the BSP's current context, then spawn
     * two kernel threads. Timer IRQ will preempt us into them once sti
     * happens in idle(). */
    static struct thread bsp_thread;
    sched_init(&bsp_thread);

    extern volatile uint64_t timer_ticks;
    thread_create("thread-A", thread_tick_demo, (void *) (uintptr_t) 'A');
    thread_create("thread-B", thread_tick_demo, (void *) (uintptr_t) 'B');
    spawn_user_demo();

    kprintf("[sched] ready with 2 kernel threads + 1 user thread, entering idle\n");
    idle();
}
