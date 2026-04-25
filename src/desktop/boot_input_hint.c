#include "boot_input_hint.h"

#include "../compositor/compositor.h"
#include "../compositor/font.h"
#include "../compositor/surface.h"
#include "../drivers/mouse.h"
#include "../drivers/usb_xhci_mouse.h"
#include "../gpu/display.h"

#include <stddef.h>
#include <stdint.h>

#define HINT_LINE_H  16u
#define HINT_HEAD_H  44u   /* line1 (yellow) + line2 (gray) header              */
#define HINT_DIAG_MAX 12u
#define HINT_H       (HINT_HEAD_H + HINT_LINE_H * HINT_DIAG_MAX + 8u)

static struct surface *s_hint;
static char            s_prev_l1[120];
static char            s_prev_l2[200];
static char            s_prev_diag[HINT_DIAG_MAX][160];
static unsigned        s_prev_diag_n;

static int str_changed(const char *a, const char *b, size_t cap) {
    for (size_t i = 0; i < cap; i++) {
        char ca = a[i], cb = b[i];
        if (ca != cb) return 1;
        if (ca == 0 && cb == 0) return 0;
        if (ca == 0 || cb == 0) return 1;
    }
    return 0;
}

static void str_copy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int diag_changed(void) {
    unsigned cur = usb_xhci_mouse_diag_count();
    if (cur != s_prev_diag_n) return 1;
    for (unsigned i = 0; i < cur && i < HINT_DIAG_MAX; i++) {
        if (str_changed(usb_xhci_mouse_diag_line(i), s_prev_diag[i], sizeof s_prev_diag[i]))
            return 1;
    }
    return 0;
}

static void diag_snapshot(void) {
    s_prev_diag_n = usb_xhci_mouse_diag_count();
    for (unsigned i = 0; i < HINT_DIAG_MAX; i++)
        str_copy(s_prev_diag[i],
                 i < s_prev_diag_n ? usb_xhci_mouse_diag_line(i) : "",
                 sizeof s_prev_diag[i]);
}

static void redraw(struct surface *s) {
    surface_clear(s, 0xe0101010u);
    font_draw_utf8(s, 8, 6,  mouse_boot_line1(), 0xffe0e080u);
    font_draw_utf8(s, 8, 26, mouse_boot_line2(), 0xffc0c0c0u);
    unsigned y = HINT_HEAD_H;
    unsigned n = usb_xhci_mouse_diag_count();
    if (n > HINT_DIAG_MAX) n = HINT_DIAG_MAX;
    for (unsigned i = 0; i < n; i++) {
        font_draw_utf8(s, 8, y, usb_xhci_mouse_diag_line(i), 0xff8fc8ffu);
        y += HINT_LINE_H;
    }
    surface_mark_dirty(s);
}

void boot_input_hint_show(void) {
    const struct display *d = display_get();
    if (!d || d->width < 160u) return;

    struct surface *s = surface_create("input-hint", d->width, HINT_H);
    if (!s) return;

    s->opaque   = 1;
    s->visible  = 1;
    s->alpha    = 255;
    s->x        = 0;
    s->y        = 0;
    s->z        = SURFACE_Z_USER_MAX - 2;

    redraw(s);

    if (compositor_add(s) != 0) {
        surface_destroy(s);
        return;
    }
    compositor_raise(s);
    s_hint = s;
    str_copy(s_prev_l1, mouse_boot_line1(), sizeof s_prev_l1);
    str_copy(s_prev_l2, mouse_boot_line2(), sizeof s_prev_l2);
    diag_snapshot();
}

void boot_input_hint_tick(void) {
    if (!s_hint) return;
    mouse_boot_hint_refresh();
    const char *l1 = mouse_boot_line1();
    const char *l2 = mouse_boot_line2();
    if (!str_changed(l1, s_prev_l1, sizeof s_prev_l1) &&
        !str_changed(l2, s_prev_l2, sizeof s_prev_l2) &&
        !diag_changed())
        return;
    str_copy(s_prev_l1, l1, sizeof s_prev_l1);
    str_copy(s_prev_l2, l2, sizeof s_prev_l2);
    diag_snapshot();
    redraw(s_hint);
}
