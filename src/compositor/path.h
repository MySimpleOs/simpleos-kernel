#pragma once

#include "surface.h"
#include <stdint.h>

/* 2D vector path: move / line / quadratic / cubic / close. Rasterize with
 * 4×4 supersampled coverage (anti-aliased fill) and polygonal stroke caps. */

typedef struct path path_t;

path_t *path_create(void);
void    path_destroy(path_t *p);
void    path_reset(path_t *p);

void path_move_to(path_t *p, int32_t x, int32_t y);
void path_line_to(path_t *p, int32_t x, int32_t y);
void path_quad_to(path_t *p, int32_t cx, int32_t cy, int32_t x, int32_t y);
void path_cubic_to(path_t *p, int32_t cx1, int32_t cy1, int32_t cx2, int32_t cy2,
                   int32_t x, int32_t y);
void path_close(path_t *p);

/* Straight-alpha ARGB over existing surface pixels. Clips to surface;
 * marks surface dirty. Returns 0 on success, -1 on OOM / overflow. */
int path_fill_surface(struct surface *s, path_t *p, uint32_t argb);
int path_stroke_surface(struct surface *s, path_t *p, int32_t width_px,
                        uint32_t argb);
