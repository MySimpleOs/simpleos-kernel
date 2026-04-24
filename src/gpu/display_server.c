#include "display_server.h"

#include "../compositor/compositor.h"
#include "../compositor/surface.h"
#include "../kprintf.h"

void display_server_init(void) {
    kprintf("[display_server] DSP1 magic=0x%x ver=%u\n",
            (unsigned) DS_PROTOCOL_MAGIC, (unsigned) DS_PROTOCOL_VER);
}

uint32_t display_server_protocol_magic(void) { return DS_PROTOCOL_MAGIC; }

uint16_t display_server_protocol_version(void) { return (uint16_t) DS_PROTOCOL_VER; }

int display_server_surface_submit(struct surface *s, int32_t x, int32_t y,
                                    int32_t z) {
    if (!s) return DS_ERR_INVAL;
    surface_move(s, x, y);
    surface_set_z(s, z);
    if (compositor_add(s) != 0) return DS_ERR_NOSPC;
    return DS_OK;
}

void display_server_surface_withdraw(struct surface *s) {
    compositor_remove(s);
}

int display_server_surface_place(struct surface *s, int32_t x, int32_t y,
                                 int32_t z) {
    if (!s) return DS_ERR_INVAL;
    surface_move(s, x, y);
    surface_set_z(s, z);
    return DS_OK;
}

void display_server_surface_damage_full(struct surface *s) {
    if (s) surface_mark_dirty(s);
}
