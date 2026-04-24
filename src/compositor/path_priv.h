#pragma once

#include <stddef.h>
#include <stdint.h>

#define PATH_CMD_CAP   512
#define PATH_MAX_VERT  16384
#define PATH_MAX_CONTOUR 32

enum path_cmd_type {
    PATH_CMD_MOVE = 0,
    PATH_CMD_LINE,
    PATH_CMD_QUAD,
    PATH_CMD_CUBIC,
    PATH_CMD_CLOSE,
};

struct path_cmd {
    uint8_t type;
    int32_t x0, y0, x1, y1, x2, y2, x3, y3;
};

struct path {
    struct path_cmd cmds[PATH_CMD_CAP];
    size_t ncmds;
};
