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
#include "arch/x86_64/syscall.h"
#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "drivers/keyboard.h"
#include "fs/initrd.h"
#include "fs/tar.h"
#include "fs/vfs.h"
#include "gpu/display.h"
#include "gpu/gpu.h"
#include "pci/pci.h"
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

#define USER_CODE_VA    0x0000000000400000ULL
#define USER_STACK_VA   0x000000007ffff000ULL
#define USER_IMAGE_PAGES 32           /* 128 KiB — fits blob + BSS arena     */

/* Supplied by kernel/src/userdemo.S — flat binary of libc/examples/hello
 * linked to start at 0x400000, ready to run once copied into a user page. */
extern const uint8_t userdemo_start[];
extern const uint8_t userdemo_end[];

static void spawn_user_demo(void) {
    uint64_t blob_size = (uint64_t) (userdemo_end - userdemo_start);

    /* Reserve a fixed USER_IMAGE_PAGES-page window at USER_CODE_VA so the
     * freshly-allocated BSS region (which lives past the blob bytes) is
     * also mapped. pmm_alloc_page zeroes frames, so BSS is implicitly
     * clean even before crt0 runs rep stosb over it. */
    extern volatile struct limine_hhdm_request hhdm_request;
    uint64_t hhdm = hhdm_request.response ? hhdm_request.response->offset : 0;

    for (uint64_t i = 0; i < USER_IMAGE_PAGES; i++) {
        uint64_t phys = pmm_alloc_page();
        vmm_map(USER_CODE_VA + i * 4096, phys, 4096, VMM_USER | VMM_W);

        uint64_t off = i * 4096;
        if (off < blob_size) {
            uint8_t *dst = (uint8_t *) (phys + hhdm);
            uint64_t cpy = blob_size - off;
            if (cpy > 4096) cpy = 4096;
            for (uint64_t b = 0; b < cpy; b++) dst[b] = userdemo_start[off + b];
        }
    }

    uint64_t stack_phys = pmm_alloc_page();
    vmm_map(USER_STACK_VA, stack_phys, 4096, VMM_USER | VMM_W | VMM_NX);

    thread_create_user("user-demo", USER_CODE_VA, USER_STACK_VA + 4096);
    kprintf("[user] ring 3 program: %u bytes blob, %u page image @ %p, stack %p\n",
            (unsigned) blob_size,
            (unsigned) USER_IMAGE_PAGES,
            (void *) USER_CODE_VA,
            (void *) (USER_STACK_VA + 4096));
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

    pci_init();
    gpu_init();

    display_init();

    /* Heap up before compositor: surface pixel buffers come from kmalloc.
     * pmm + active VMM (Limine-installed page tables) are all it needs. */
    heap_init();

    /* Faz 12.1 demo: three overlapping surfaces on a dark-navy desktop.
     * No thread yet (Faz 12.2) — we just composite twice so both VirtIO
     * buffers hold the same frame and swaps don't reveal garbage. */
    compositor_init();
    {
        struct surface *s1 = surface_create("red",   320, 240);
        struct surface *s2 = surface_create("green", 320, 240);
        struct surface *s3 = surface_create("blue",  320, 240);
        if (s1 && s2 && s3) {
            surface_clear(s1, 0xffe03a3a);   /* opaque red    */
            surface_clear(s2, 0xcc30c060);   /* 80% alpha green */
            surface_clear(s3, 0xaa3080ff);   /* 67% alpha blue  */
            surface_move(s1,  80,  80); surface_set_z(s1, 0);
            surface_move(s2, 260, 160); surface_set_z(s2, 1);
            surface_move(s3, 440, 240); surface_set_z(s3, 2);
            compositor_add(s1);
            compositor_add(s2);
            compositor_add(s3);
        }
    }
    compositor_frame(COMPOSITOR_DEFAULT_BG);
    if (display_get()->double_buffered) compositor_frame(COMPOSITOR_DEFAULT_BG);
    kprintf("[boot] compositor demo painted (3 surfaces)\n");

    /* 1200 Hz gives 10 timer ticks per 120 Hz compositor frame, exactly.
     * Also tightens scheduler preemption granularity for UI responsiveness. */
    lapic_timer_init(1200);

    ioapic_init();
    keyboard_init();
    ioapic_set_irq(KEYBOARD_GSI, KEYBOARD_VECTOR, 0);

    smp_init();

    syscall_init();

    vfs_init();
    if (initrd_init() == 0) {
        tar_mount(initrd_bytes(), initrd_size());
        vfs_dump(NULL, 0);
    }

    /* Bootstrap the scheduler around the BSP's current context, then spawn
     * the user-space init program (the shell). Timer IRQ will preempt the
     * BSP into it once sti happens in idle(). */
    static struct thread bsp_thread;
    sched_init(&bsp_thread);

    compositor_start(COMPOSITOR_DEFAULT_BG, 120);

    spawn_user_demo();

    kprintf("[sched] scheduler ready, handing off to init\n\n");
    idle();
}
