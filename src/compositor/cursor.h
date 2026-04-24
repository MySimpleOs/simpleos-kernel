#pragma once

/* Mouse cursor overlay — ARGB surface from default@2x.png (downscaled to
 * 32×32 at build time; see scripts/gen_cursor_rgba.py). Hotspot matches SVG
 * translate(10,7). compositor thread calls cursor_tick() each frame. */

#include <stdint.h>

void cursor_init(void);
void cursor_tick(void);   /* updates the overlay surface from mouse state */
