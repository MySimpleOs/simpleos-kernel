#pragma once

/* Mouse cursor overlay — a small ARGB surface painted with a classic
 * arrow sprite, pinned to the top of the compositor z stack. The
 * compositor thread calls cursor_tick() each frame to sync its
 * position from the PS/2 mouse driver state.
 */

#include <stdint.h>

void cursor_init(void);
void cursor_tick(void);   /* updates the overlay surface from mouse state */
