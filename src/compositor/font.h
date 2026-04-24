#pragma once

#include "surface.h"
#include <stdint.h>

/* stb_truetype-backed text: UTF-8, per-glyph SDF cache, grayscale coverage.
 * Embedded faces: Noto Sans + Noto Sans Symbols 2 (hinted TTFs from the
 * official googlefonts/noto-fonts tree — refresh with scripts/fetch-google-fonts.sh). */

int  font_init(void);
void font_shutdown(void);

/* Draw UTF-8 on a surface. (x, y) is the top-left of the line box; the
 * first baseline is y + ascent. Returns total advance in pixels (rounded). */
int  font_draw_utf8(struct surface *s, int x, int y,
                    const char *utf8, uint32_t fg_argb);
