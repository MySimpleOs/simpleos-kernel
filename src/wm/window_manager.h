#pragma once

/* Window manager — stacks decorated surfaces, focus, desktops, snap hints.
 * Compositor still owns pixels; WM owns policy (placement, minimize, z). */

#include <stdint.h>

struct surface;

typedef uint32_t wm_window_id;

#define WM_ID_NONE       0u
#define WM_MAX_WINDOWS   64u
#define WM_MAX_DESKTOPS  8u

enum wm_snap_mask {
    WM_SNAP_NONE   = 0,
    WM_SNAP_LEFT   = 1u << 0,
    WM_SNAP_RIGHT  = 1u << 1,
    WM_SNAP_TOP     = 1u << 2,
    WM_SNAP_BOTTOM  = 1u << 3,
};

enum wm_transition_kind {
    WM_TRANSITION_NONE = 0,
    WM_TRANSITION_MOVE,
    WM_TRANSITION_RESIZE,
};

void wm_init(void);

/* Register an existing surface as a managed window (compositor_add + z). */
wm_window_id wm_register_window(struct surface *s, int32_t x, int32_t y, int32_t z);
void         wm_unregister_window(wm_window_id id);

struct surface *wm_window_surface(wm_window_id id);

int wm_move(wm_window_id id, int32_t x, int32_t y);
/* Logical size; buffer realloc not implemented — stores target for future. */
int wm_resize(wm_window_id id, uint32_t w, uint32_t h);

void wm_raise(wm_window_id id);
void wm_set_focus(wm_window_id id);
wm_window_id wm_focused_window(void);

void wm_minimize(wm_window_id id);
void wm_maximize(wm_window_id id);
void wm_restore(wm_window_id id);

uint32_t wm_active_desktop(void);
void     wm_set_active_desktop(uint32_t index);

/* Snap window origin to screen edges (size unchanged until resize lands). */
void wm_snap_to_edges(wm_window_id id, uint32_t snap_mask);

/* Placeholder for future frame-synced motion (roadmap: transition anim). */
void wm_transition_begin(wm_window_id id, enum wm_transition_kind kind,
                           int32_t target_x, int32_t target_y,
                           uint32_t duration_frames);
