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
     * two kernel threads that cooperatively yield to each other. */
    static struct thread bsp_thread;
    sched_init(&bsp_thread);

    thread_create("thread-A", ({
        void a_fn(void *_a) {
            (void) _a;
            for (int i = 1; i <= 3; i++) {
                kprintf("[thread-A] tick %d\n", i);
                thread_yield();
            }
        }
        a_fn;
    }), NULL);

    thread_create("thread-B", ({
        void b_fn(void *_a) {
            (void) _a;
            for (int i = 1; i <= 3; i++) {
                kprintf("[thread-B] tick %d\n", i);
                thread_yield();
            }
        }
        b_fn;
    }), NULL);

    /* First yield kicks off thread-A; when both finish, control comes back
     * here and we drop into the idle loop. */
    thread_yield();
    thread_yield();
    thread_yield();
    thread_yield();
    thread_yield();
    thread_yield();

    kprintf("[boot] kernel threads finished, entering idle\n");
    idle();
}
