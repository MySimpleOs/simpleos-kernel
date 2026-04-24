#include "window_manager.h"

#include "../compositor/compositor.h"
#include "../compositor/surface.h"
#include "../gpu/display.h"
#include "../input/input_routing.h"
#include "../kprintf.h"

#include <stddef.h>

enum {
    WM_FLAG_USED       = 1 << 0,
    WM_FLAG_MINIMIZED  = 1 << 1,
    WM_FLAG_MAXIMIZED  = 1 << 2,
};

struct wm_slot {
    uint32_t       flags;
    struct surface *surf;
    uint32_t       desktop;
    int32_t        restore_x, restore_y;
    uint32_t       restore_w, restore_h;
    uint32_t       target_w, target_h;
};

static struct wm_slot g_slots[WM_MAX_WINDOWS];
static uint32_t       g_next_id = 1;
static uint32_t       g_active_desktop;
static wm_window_id  g_focus = WM_ID_NONE;

static struct wm_slot *slot_get(wm_window_id id) {
    if (id == WM_ID_NONE || id > WM_MAX_WINDOWS) return NULL;
    struct wm_slot *e = &g_slots[id - 1u];
    if (!(e->flags & WM_FLAG_USED) || !e->surf) return NULL;
    return e;
}

void wm_init(void) {
    for (size_t i = 0; i < WM_MAX_WINDOWS; i++) {
        g_slots[i].flags = 0;
        g_slots[i].surf  = NULL;
    }
    g_next_id         = 1;
    g_active_desktop  = 0;
    g_focus           = WM_ID_NONE;
    kprintf("[wm] init slots=%u desktops=%u\n",
            (unsigned) WM_MAX_WINDOWS, (unsigned) WM_MAX_DESKTOPS);
}

wm_window_id wm_register_window(struct surface *s, int32_t x, int32_t y, int32_t z) {
    if (!s) return WM_ID_NONE;
    if (g_next_id > WM_MAX_WINDOWS) return WM_ID_NONE;

    wm_window_id id = g_next_id++;
    struct wm_slot *e = &g_slots[id - 1u];
    e->flags    = WM_FLAG_USED;
    e->surf     = s;
    e->desktop  = g_active_desktop;
    e->restore_x = x;
    e->restore_y = y;
    e->restore_w = s->width;
    e->restore_h = s->height;
    e->target_w  = s->width;
    e->target_h  = s->height;

    surface_move(s, x, y);
    surface_set_z(s, z);
    if (compositor_add(s) != 0) {
        e->flags = 0;
        e->surf  = NULL;
        return WM_ID_NONE;
    }
    return id;
}

void wm_unregister_window(wm_window_id id) {
    struct wm_slot *e = slot_get(id);
    if (!e) return;
    if (g_focus == id) {
        g_focus = WM_ID_NONE;
        input_routing_set_keyboard_focus(WM_ID_NONE);
    }
    compositor_remove(e->surf);
    e->flags = 0;
    e->surf  = NULL;
}

struct surface *wm_window_surface(wm_window_id id) {
    struct wm_slot *e = slot_get(id);
    return e ? e->surf : NULL;
}

int wm_move(wm_window_id id, int32_t x, int32_t y) {
    struct wm_slot *e = slot_get(id);
    if (!e) return -1;
    surface_move(e->surf, x, y);
    if (!(e->flags & WM_FLAG_MAXIMIZED)) {
        e->restore_x = x;
        e->restore_y = y;
    }
    return 0;
}

int wm_resize(wm_window_id id, uint32_t w, uint32_t h) {
    struct wm_slot *e = slot_get(id);
    if (!e || !w || !h) return -1;
    e->target_w = w;
    e->target_h = h;
    /* Pixel buffer size unchanged until surface realloc exists. */
    return -1;
}

void wm_raise(wm_window_id id) {
    struct wm_slot *e = slot_get(id);
    if (!e) return;
    compositor_raise(e->surf);
}

void wm_set_focus(wm_window_id id) {
    if (id != WM_ID_NONE && !slot_get(id)) return;
    g_focus = id;
    input_routing_set_keyboard_focus(id);
    if (id != WM_ID_NONE) wm_raise(id);
}

wm_window_id wm_focused_window(void) { return g_focus; }

void wm_minimize(wm_window_id id) {
    struct wm_slot *e = slot_get(id);
    if (!e) return;
    e->flags |= WM_FLAG_MINIMIZED;
    surface_show(e->surf, 0);
    if (g_focus == id) {
        g_focus = WM_ID_NONE;
        input_routing_set_keyboard_focus(WM_ID_NONE);
    }
}

void wm_maximize(wm_window_id id) {
    struct wm_slot *e = slot_get(id);
    if (!e) return;
    const struct display *d = display_get();
    if (!d || !e->surf) return;

    if (!(e->flags & WM_FLAG_MAXIMIZED)) {
        e->restore_x = e->surf->x;
        e->restore_y = e->surf->y;
        e->restore_w = e->surf->width;
        e->restore_h = e->surf->height;
    }
    e->flags |= WM_FLAG_MAXIMIZED;
    e->flags &= ~WM_FLAG_MINIMIZED;
    surface_show(e->surf, 1);
    surface_move(e->surf, 0, 0);
    /* Size: keep buffer; position flush-left/top (tiling step until resize). */
    (void) d;
    compositor_raise(e->surf);
    surface_mark_dirty(e->surf);
}

void wm_restore(wm_window_id id) {
    struct wm_slot *e = slot_get(id);
    if (!e) return;
    if (e->flags & WM_FLAG_MINIMIZED) {
        e->flags &= ~WM_FLAG_MINIMIZED;
        surface_show(e->surf, 1);
    }
    if (e->flags & WM_FLAG_MAXIMIZED) {
        e->flags &= ~WM_FLAG_MAXIMIZED;
        surface_move(e->surf, e->restore_x, e->restore_y);
        (void) e->restore_w;
        (void) e->restore_h;
    }
    surface_mark_dirty(e->surf);
}

uint32_t wm_active_desktop(void) { return g_active_desktop; }

void wm_set_active_desktop(uint32_t index) {
    if (index >= WM_MAX_DESKTOPS) return;
    g_active_desktop = index;
    for (wm_window_id id = 1; id <= WM_MAX_WINDOWS; id++) {
        struct wm_slot *e = slot_get(id);
        if (!e) continue;
        int vis = (e->desktop == index) && !(e->flags & WM_FLAG_MINIMIZED);
        surface_show(e->surf, vis);
    }
}

void wm_snap_to_edges(wm_window_id id, uint32_t mask) {
    struct wm_slot *e = slot_get(id);
    if (!e || !mask) return;
    const struct display *d = display_get();
    if (!d) return;
    struct surface *s = e->surf;
    int32_t x = s->x, y = s->y;
    int32_t sw = (int32_t) s->width, sh = (int32_t) s->height;
    int32_t dw = (int32_t) d->width, dh = (int32_t) d->height;

    if (mask & WM_SNAP_LEFT)   x = 0;
    if (mask & WM_SNAP_RIGHT)  x = dw - sw;
    if (mask & WM_SNAP_TOP)    y = 0;
    if (mask & WM_SNAP_BOTTOM) y = dh - sh;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + sw > dw) x = dw - sw;
    if (y + sh > dh) y = dh - sh;
    surface_move(s, x, y);
    if (!(e->flags & WM_FLAG_MAXIMIZED)) {
        e->restore_x = x;
        e->restore_y = y;
    }
}

void wm_transition_begin(wm_window_id id, enum wm_transition_kind kind,
                         int32_t target_x, int32_t target_y,
                         uint32_t duration_frames) {
    (void) id;
    (void) kind;
    (void) target_x;
    (void) target_y;
    (void) duration_frames;
    /* Reserved for anim hook (compositor/anim) — no-op bootstrap. */
}
