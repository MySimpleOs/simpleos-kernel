#pragma once

/* Mouse cursor overlay — ARGB surface (36×36) with 2× supersampled arrow,
 * soft alpha edges, and drop shadow; hotspot matches the pointed pixel.
 * The compositor thread calls cursor_tick() each frame to sync position
 * from the PS/2 mouse driver.
 */

#include <stdint.h>

void cursor_init(void);
void cursor_tick(void);   /* updates the overlay surface from mouse state */
