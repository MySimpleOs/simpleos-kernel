#include "simple_desktop.h"

#include "../compositor/font.h"
#include "../compositor/surface.h"
#include "../drivers/mouse.h"
#include "../gpu/display.h"
#include "../kprintf.h"
#include "../sched/thread.h"
#include "../wm/window_manager.h"

#include <stddef.h>
#include <stdint.h>

#define DOCK_H       56u
#define BTN_PAD      8
#define BTN_W        112
#define BTN_H        40
#define MAX_TERMS    3

static struct surface *s_dock;
static int32_t         dock_screen_y;
static struct surface *s_terms[MAX_TERMS];
static int             n_terms;
static uint8_t         prev_btn;
static unsigned        boot_grace;

static void fill_rect(struct surface *s, int32_t x0, int32_t y0,
                      int32_t x1, int32_t y1, uint32_t argb) {
    if (!s || !s->pixels) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t) s->width)  x1 = (int32_t) s->width;
    if (y1 > (int32_t) s->height) y1 = (int32_t) s->height;
    if (x1 <= x0 || y1 <= y0) return;
    for (int32_t y = y0; y < y1; y++) {
        uint32_t *row = s->pixels + (uint32_t) y * s->width;
        for (int32_t x = x0; x < x1; x++) row[(uint32_t) x] = argb;
    }
    surface_mark_dirty(s);
}

static int hit_first_button(int32_t mx, int32_t my) {
    int32_t bx0 = BTN_PAD;
    int32_t by0 = dock_screen_y + BTN_PAD;
    int32_t bx1 = bx0 + BTN_W;
    int32_t by1 = by0 + BTN_H;
    return mx >= bx0 && mx < bx1 && my >= by0 && my < by1;
}

static void spawn_demo_terminal(void) {
    if (n_terms >= MAX_TERMS) return;
    const struct display *d = display_get();
    if (!d) return;

    uint32_t tw = (d->width > 120u) ? (d->width * 55u / 100u) : 400u;
    if (tw < 320u) tw = 320u;
    if (tw > 720u) tw = 720u;
    uint32_t th = (d->height > 200u) ? (d->height * 45u / 100u) : 260u;
    if (th < 200u) th = 200u;
    if (th > 520u) th = 520u;

    struct surface *s = surface_create("term", tw, th);
    if (!s) return;

    surface_clear(s, 0xff16161a);
    fill_rect(s, 0, 0, (int32_t) tw, 28, 0xff2d2d34);

    int32_t ox = 32 + n_terms * 36;
    int32_t oy = 32 + n_terms * 28;
    if (ox + (int32_t) tw > (int32_t) d->width - 8)  ox = (int32_t) d->width - (int32_t) tw - 8;
    if (oy + (int32_t) th > dock_screen_y - 8)      oy = dock_screen_y - (int32_t) th - 8;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    wm_window_id id = wm_register_window(s, ox, oy, 50 + n_terms);
    if (id == WM_ID_NONE) {
        surface_destroy(s);
        return;
    }

    (void) font_draw_utf8(s, 10, 8, "SimpleOS terminal (demo)", 0xffe8e8f0u);
    (void) font_draw_utf8(s, 10, 36, "$ uname -a\nSimpleOS 0.1  x86_64\n$ _",
                          0xffb0c4deu);

    s_terms[n_terms++] = s;
    wm_set_focus(id);
    kprintf("[desktop] spawned demo terminal #%d id=%u @ %d,%d\n",
            n_terms, (unsigned) id, (int) ox, (int) oy);
}

static void dock_poller(void *arg) {
    (void) arg;
    prev_btn   = 0;
    boot_grace = 0;
    for (;;) {
        thread_yield();
        if (boot_grace < 60u) {
            boot_grace++;
            mouse_poll();
            mouse_get_state(NULL, NULL, &prev_btn);
            continue;
        }
        mouse_poll();
        int32_t mx, my;
        uint8_t btn;
        mouse_get_state(&mx, &my, &btn);
        int down = (btn & MOUSE_BTN_LEFT) != 0;
        int was  = (prev_btn & MOUSE_BTN_LEFT) != 0;
        if (down && !was && hit_first_button(mx, my))
            spawn_demo_terminal();
        prev_btn = btn;
    }
}

void simple_desktop_init(void) {
    const struct display *d = display_get();
    if (!d || !d->pixels) return;

    if (font_init() != 0)
        kprintf("[desktop] font_init failed — terminal text may be missing\n");

    uint32_t dw = d->width;
    if (dw < 320u || DOCK_H >= d->height) return;

    s_dock = surface_create("dock", dw, DOCK_H);
    if (!s_dock) return;

    surface_clear(s_dock, 0xff25252b);
    fill_rect(s_dock, 0, 0, (int32_t) dw, 3, 0xff3d3d48);
    fill_rect(s_dock, BTN_PAD, BTN_PAD, BTN_PAD + BTN_W, BTN_PAD + BTN_H, 0xff3a5a9a);
    fill_rect(s_dock, BTN_PAD + BTN_W + BTN_PAD, BTN_PAD,
              BTN_PAD + BTN_W + BTN_PAD + BTN_W, BTN_PAD + BTN_H, 0xff3a4a5a);
    fill_rect(s_dock, BTN_PAD + 2 * (BTN_W + BTN_PAD), BTN_PAD,
              BTN_PAD + 3 * BTN_W + 2 * BTN_PAD, BTN_PAD + BTN_H, 0xff3a5a4a);

    dock_screen_y = (int32_t) d->height - (int32_t) DOCK_H;
    wm_window_id dock_id = wm_register_window(s_dock, 0, dock_screen_y, 5);
    if (dock_id == WM_ID_NONE) {
        surface_destroy(s_dock);
        s_dock = NULL;
        return;
    }
    (void) dock_id;

    kprintf("[desktop] dock %ux%u — click left blue tile for terminal (QEMU: virtio-tablet)\n",
            (unsigned) dw, (unsigned) DOCK_H);
}

void simple_desktop_start_poller(void) {
    if (!s_dock) return;
    thread_create("dock-poller", dock_poller, NULL);
}
