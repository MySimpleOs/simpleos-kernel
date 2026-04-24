#include "input_routing.h"

#include "../kprintf.h"

static ipr_window_token_t g_kb_focus;
static ipr_window_token_t g_ptr_capture;
static enum ipr_dnd_phase g_dnd;
static ipr_window_token_t g_drag_source;
static uint32_t           g_drag_serial;

void input_routing_init(void) {
    g_kb_focus     = IPR_TOKEN_NONE;
    g_ptr_capture  = IPR_TOKEN_NONE;
    g_dnd          = IPR_DND_IDLE;
    g_drag_source  = IPR_TOKEN_NONE;
    g_drag_serial  = 0;
    kprintf("[input] routing init (focus / capture / dnd)\n");
}

void input_routing_set_keyboard_focus(ipr_window_token_t id) {
    g_kb_focus = id;
}

ipr_window_token_t input_routing_keyboard_focus(void) { return g_kb_focus; }

void input_routing_pointer_capture_set(ipr_window_token_t id) {
    g_ptr_capture = id;
}

void input_routing_pointer_capture_clear(void) { g_ptr_capture = IPR_TOKEN_NONE; }

ipr_window_token_t input_routing_pointer_capture(void) { return g_ptr_capture; }

enum ipr_dnd_phase input_routing_dnd_phase(void) { return g_dnd; }

void input_routing_drag_begin(ipr_window_token_t source,
                              int32_t x, int32_t y, uint32_t serial) {
    (void) x;
    (void) y;
    if (source == IPR_TOKEN_NONE) return;
    g_drag_source   = source;
    g_drag_serial   = serial;
    g_dnd           = IPR_DND_DRAGGING;
}

void input_routing_drag_motion(int32_t x, int32_t y) {
    (void) x;
    (void) y;
    if (g_dnd != IPR_DND_DRAGGING) return;
}

void input_routing_drag_cancel(void) {
    g_dnd         = IPR_DND_IDLE;
    g_drag_source = IPR_TOKEN_NONE;
}

int input_routing_drag_drop(ipr_window_token_t target, uint32_t serial) {
    if (g_dnd != IPR_DND_DRAGGING) return 0;
    if (serial != g_drag_serial) return 0;
    (void) target;
    input_routing_drag_cancel();
    return 1;
}

ipr_window_token_t input_routing_pointer_pressed(ipr_window_token_t hit_window,
                                               uint8_t mouse_buttons) {
    (void) mouse_buttons;
    if (g_ptr_capture != IPR_TOKEN_NONE) return g_ptr_capture;
    if (hit_window != IPR_TOKEN_NONE)
        input_routing_set_keyboard_focus(hit_window);
    return hit_window;
}
