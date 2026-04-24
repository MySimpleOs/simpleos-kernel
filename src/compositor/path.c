#include "path.h"
#include "path_priv.h"

#include "../mm/heap.h"

#include <stddef.h>

path_t *path_create(void) {
    path_t *p = (path_t *) kmalloc(sizeof(*p));
    if (!p) return NULL;
    path_reset(p);
    return p;
}

void path_destroy(path_t *p) {
    if (p) kfree(p);
}

void path_reset(path_t *p) {
    if (!p) return;
    p->ncmds = 0;
}

static int append_cmd(path_t *p, const struct path_cmd *c) {
    if (!p || p->ncmds >= PATH_CMD_CAP) return -1;
    p->cmds[p->ncmds++] = *c;
    return 0;
}

void path_move_to(path_t *p, int32_t x, int32_t y) {
    if (!p) return;
    struct path_cmd c = { .type = PATH_CMD_MOVE, .x0 = x, .y0 = y };
    (void) append_cmd(p, &c);
}

void path_line_to(path_t *p, int32_t x, int32_t y) {
    if (!p) return;
    struct path_cmd c = { .type = PATH_CMD_LINE, .x0 = x, .y0 = y };
    (void) append_cmd(p, &c);
}

void path_quad_to(path_t *p, int32_t cx, int32_t cy, int32_t x, int32_t y) {
    if (!p) return;
    struct path_cmd c = {
        .type = PATH_CMD_QUAD,
        .x0 = cx, .y0 = cy,
        .x1 = x, .y1 = y,
    };
    (void) append_cmd(p, &c);
}

void path_cubic_to(path_t *p, int32_t cx1, int32_t cy1, int32_t cx2, int32_t cy2,
                    int32_t x, int32_t y) {
    if (!p) return;
    struct path_cmd c = {
        .type = PATH_CMD_CUBIC,
        .x0 = cx1, .y0 = cy1,
        .x1 = cx2, .y1 = cy2,
        .x2 = x,  .y2 = y,
    };
    (void) append_cmd(p, &c);
}

void path_close(path_t *p) {
    if (!p) return;
    struct path_cmd c = { .type = PATH_CMD_CLOSE };
    (void) append_cmd(p, &c);
}
