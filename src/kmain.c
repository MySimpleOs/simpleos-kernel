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
#include "arch/x86_64/simd.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/syscall.h"
#include "compositor/anim.h"
#include "compositor/compositor.h"
#include "compositor/font.h"
#include "compositor/path.h"
#include "compositor/cursor.h"
#include "compositor/gradient.h"
#include "compositor/surface.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "fs/initrd.h"
#include "fs/tar.h"
#include "fs/vfs.h"
#include "gpu/display.h"
#include "gpu/display_policy.h"
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

/* Map demo art from 320×240 design coordinates to actual surface size. */
static int32_t demo_scx(uint32_t sw, int32_t x) {
    return (int32_t) (((int64_t) x * (int64_t) sw + 160) / 320);
}
static int32_t demo_scy(uint32_t sh, int32_t y) {
    return (int32_t) (((int64_t) y * (int64_t) sh + 120) / 240);
}

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
    simd_cpu_init(1);
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

    /* Heap before display so display_init() can kmalloc its software
     * shadow buffer. Only pmm + active VMM (Limine-installed page
     * tables) are needed, both live by this point. */
    heap_init();

    if (font_init() != 0)
        kprintf("[boot] font_init failed (missing assets/*.ttf?)\n");

    display_init();

    /* Faz 12.3 demo: three overlapping surfaces animated by the spring
     * + easing engine. s1.x springs back and forth, s2.y eases with
     * out-back bounce, s3.alpha pulses with a linear loop. The compositor
     * thread's tick_all(dt) advances these every frame before blitting. */
    compositor_init();
    {
        const struct display *ddev = display_get();
        uint32_t dw = (ddev && ddev->width)  ? ddev->width  : 1024u;
        uint32_t dh = (ddev && ddev->height) ? ddev->height : 768u;

        uint32_t mrg = dw / 25u;
        if (mrg < 8u)  mrg = 8u;
        if (mrg > 40u) mrg = 40u;

        uint32_t sw = (dw > 3u * mrg) ? (dw - 3u * mrg) / 3u : dw / 3u;
        if (sw < 160u && dw > 40u) sw = dw - 40u;
        if (sw > 420u) sw = 420u;
        if (sw < 120u) sw = (dw > 20u) ? dw - 20u : 120u;

        uint32_t sh = dh * 28u / 100u;
        if (sh < 120u) sh = (dh > 30u) ? dh - 30u : 120u;
        if (sh > 300u) sh = 300u;

        struct surface *s1 = surface_create("red",   sw, sh);
        struct surface *s2 = surface_create("green", sw, sh);
        struct surface *s3 = surface_create("blue",  sw, sh);
        if (s1 && s2 && s3) {
            int32_t swm = (int32_t) sw - 1, shm = (int32_t) sh - 1;
            if (swm < 1) swm = 1;
            if (shm < 1) shm = 1;

            gradient_fill_linear(s1, 0xffe03a3a, 0xffffaa20,
                                 0, 0, swm, shm);

            int32_t rcx = (int32_t) (sw / 2u), rcy = (int32_t) (sh / 2u);
            int32_t rad = (int32_t) ((sw + sh) / 4u);
            if (rad < 40) rad = 40;
            gradient_fill_radial(s2, 0xe060ff80, 0x8015602a,
                                 rcx, rcy, rad);

            {
                path_t *pv = path_create();
                if (pv) {
                    path_move_to(pv, demo_scx(sw, 30),  demo_scy(sh, 40));
                    path_cubic_to(pv, demo_scx(sw, 90),  demo_scy(sh, 15),
                                  demo_scx(sw, 150), demo_scy(sh, 130),
                                  demo_scx(sw, 120), demo_scy(sh, 200));
                    path_quad_to(pv,  demo_scx(sw, 60),  demo_scy(sh, 210),
                                  demo_scx(sw, 25),  demo_scy(sh, 150));
                    path_line_to(pv,  demo_scx(sw, 25),  demo_scy(sh, 70));
                    path_close(pv);
                    path_stroke_surface(s2, pv, 3, 0xdd204060);
                    path_fill_surface(s2, pv, 0xc0fff8e8);
                    path_destroy(pv);
                }
            }

            gradient_fill_linear(s3, 0xaa1a3080, 0xaa60c0ff,
                                 rcx, shm, rcx, 0);

            uint32_t cr = sw / 12u;
            if (cr > 24u) cr = 24u;
            if (cr < 4u)  cr = 4u;
            surface_set_corner_radius(s1, cr);
            surface_set_corner_radius(s2, cr);
            surface_set_corner_radius(s3, cr);
            surface_set_shadow(s1,  6,  6, 16, 0x000000, 180);
            surface_set_shadow(s2,  6,  6, 16, 0x000000, 180);
            surface_set_shadow(s3,  6,  6, 16, 0x000000, 180);

            int32_t x1 = (int32_t) mrg;
            int32_t x2 = (int32_t) (mrg + sw + mrg);
            int32_t x3 = (int32_t) dw - (int32_t) mrg - (int32_t) sw;
            if (x3 < x2 + (int32_t) (sw / 4u)) {
                x3 = x2 + (int32_t) (sw / 4u);
                if (x3 + (int32_t) sw > (int32_t) dw)
                    x3 = (int32_t) dw - (int32_t) sw - (int32_t) mrg;
            }

            int32_t y1 = (int32_t) (dh / 10u);
            int32_t y2 = (int32_t) (dh / 6u);
            int32_t y3 = (int32_t) (dh / 5u);
            if (y1 + (int32_t) sh > (int32_t) dh) y1 = (int32_t) dh - (int32_t) sh - 8;
            if (y2 + (int32_t) sh > (int32_t) dh) y2 = (int32_t) dh - (int32_t) sh - 8;
            if (y3 + (int32_t) sh > (int32_t) dh) y3 = (int32_t) dh - (int32_t) sh - 8;

            surface_move(s1, x1, y1); surface_set_z(s1, 0);
            surface_move(s2, x2, y2); surface_set_z(s2, 1);
            surface_move(s3, x3, y3); surface_set_z(s3, 2);
            compositor_add(s1);
            compositor_add(s2);
            compositor_add(s3);

            struct anim *a_x  = anim_new();
            struct anim *a_y  = anim_new();
            struct anim *a_al = anim_new();

            int32_t xa0 = x1;
            int32_t xa1 = (int32_t) dw - (int32_t) sw - (int32_t) mrg;
            if (xa1 <= xa0) xa1 = xa0 + 40;
            anim_ease(a_x, FX_FROM_INT(xa0), FX_FROM_INT(xa1),
                      FX_FROM_INT(2) + (FX_ONE >> 1), EASE_OUT_BACK);
            anim_bind_i32(a_x, &s1->x, FX_ONE, 0, xa0 - 400, xa1 + 400);
            anim_set_loop(a_x, 1);

            int32_t ya0 = y2;
            int32_t ya1 = (int32_t) dh - (int32_t) sh - (int32_t) mrg;
            if (ya1 <= ya0) ya1 = ya0 + 40;
            anim_ease(a_y, FX_FROM_INT(ya0), FX_FROM_INT(ya1),
                      FX_FROM_INT(2), EASE_IN_OUT_CUBIC);
            anim_bind_i32(a_y, &s2->y, FX_ONE, 0, ya0 - 300, ya1 + 300);
            anim_set_loop(a_y, 1);

            /* s3: alpha pulse 140 ↔ 230, 3 s half-cycle in-out-cubic —
             * slower than motion so it reads as "breathing". */
            anim_ease(a_al, FX_FROM_INT(140), FX_FROM_INT(230),
                      FX_FROM_INT(3), EASE_IN_OUT_CUBIC);
            anim_bind_u8(a_al, &s3->alpha, FX_ONE, 0, 0, 255);
            anim_set_loop(a_al, 1);

            /* Faz 12.9: UTF-8 + SDF cache + LCD subpixel (Noto Sans + Symbols2). */
            if (s1) {
                static const char k_font_demo[] =
                    "SimpleOS "
                    "\xc4\x9f\xc3\xbc\xc5\x9f\xc3\xb6\xc3\xa7\xc4\xb1 " /* ğüşöçı */
                    "\xf0\x9f\x98\x80 "                                  /* U+1F600 */
                    "\xe2\x98\xba";                                     /* U+263A */
                font_draw_utf8(s1, 12, 18, k_font_demo, 0xffffffffu);
            }
        }
    }
    compositor_frame(COMPOSITOR_DEFAULT_BG);
    kprintf("[boot] compositor demo painted (3 surfaces)\n");

    vfs_init();
    if (initrd_init() == 0) {
        tar_mount(initrd_bytes(), initrd_size());
        vfs_dump(NULL, 0);
        display_policy_try_load_vfs("/etc/display.conf");
    }

    /* LAPIC rate = integer multiple of configured refresh for stable pacing. */
    lapic_timer_init(display_policy_apic_timer_hz());

    ioapic_init();
    keyboard_init();
    ioapic_set_irq(KEYBOARD_GSI, KEYBOARD_VECTOR, 0);

    /* PS/2 aux (mouse): enable controller port, wire IRQ 12 → vector. */
    {
        const struct display *dd = display_get();
        mouse_init(dd ? dd->width : 0, dd ? dd->height : 0);
    }
    ioapic_set_irq(MOUSE_GSI, MOUSE_VECTOR, 0);
    cursor_init();

    smp_init();

    syscall_init();

    /* Bootstrap the scheduler around the BSP's current context, then spawn
     * the user-space init program (the shell). Timer IRQ will preempt the
     * BSP into it once sti happens in idle(). */
    static struct thread bsp_thread;
    sched_init(&bsp_thread);

    /* Target frame rate from /etc/display.conf (refresh_hz), capped. */
    compositor_start(COMPOSITOR_DEFAULT_BG, display_policy_compositor_hz());

    spawn_user_demo();

    kprintf("[sched] scheduler ready, handing off to init\n\n");
    idle();
}
