#pragma once

/* Input routing — keyboard focus, pointer capture, drag/drop token flow.
 * Window identity is an opaque token (same numeric space as wm_window_id). */

#include <stdint.h>

typedef uint32_t ipr_window_token_t;

#define IPR_TOKEN_NONE 0u

enum ipr_dnd_phase {
    IPR_DND_IDLE = 0,
    IPR_DND_DRAG_PENDING,
    IPR_DND_DRAGGING,
    IPR_DND_DROP_HOVER,
};

void input_routing_init(void);

void               input_routing_set_keyboard_focus(ipr_window_token_t id);
ipr_window_token_t input_routing_keyboard_focus(void);

void               input_routing_pointer_capture_set(ipr_window_token_t id);
void               input_routing_pointer_capture_clear(void);
ipr_window_token_t input_routing_pointer_capture(void);

enum ipr_dnd_phase input_routing_dnd_phase(void);

void input_routing_drag_begin(ipr_window_token_t source,
                              int32_t x, int32_t y, uint32_t serial);
void input_routing_drag_motion(int32_t x, int32_t y);
void input_routing_drag_cancel(void);
/* Returns 1 if drop accepted by routing policy. */
int  input_routing_drag_drop(ipr_window_token_t target, uint32_t serial);

/* Policy hook: map hit-tested topmost window + buttons to focus/capture. */
ipr_window_token_t input_routing_pointer_pressed(ipr_window_token_t hit_window,
                                                 uint8_t mouse_buttons);
