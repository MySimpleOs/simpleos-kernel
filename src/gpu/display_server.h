#pragma once

/* Display server — minimal Wayland-style contract: clients own
 * `struct surface` buffers; the compositor only stacks and blends them.
 *
 * Wire layout: every future IPC message (syscall, shared ring, virtio)
 * is prefixed with `ds_msg_header_t` (packed, versioned). Kernel helpers
 * below wrap compositor_add/remove + placement + damage. */

#include <stdint.h>

struct surface;

#define DS_PROTOCOL_MAGIC 0x44535031u /* 'DSP1' */
#define DS_PROTOCOL_VER   1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
} ds_msg_header_t;

/* v1 surface ops — wire payload follows this header in IPC; syscall ABI uses
 * the same numeric ids in rax for the dedicated SYS_DSP_* entry points. */
enum ds_surface_op {
    DS_OP_SURFACE_SUBMIT      = 1,
    DS_OP_SURFACE_WITHDRAW    = 2,
    DS_OP_SURFACE_PLACE       = 3,
    DS_OP_SURFACE_DAMAGE_FULL = 4,
};

enum {
    DS_OK          = 0,
    DS_ERR_INVAL   = -1,
    DS_ERR_NOSPC   = -2,
};

void display_server_init(void);

uint32_t display_server_protocol_magic(void);
uint16_t display_server_protocol_version(void);

/* Map a client surface into the compositor stack (geometry + z). */
int display_server_surface_submit(struct surface *s, int32_t x, int32_t y,
                                  int32_t z);

void display_server_surface_withdraw(struct surface *s);

int display_server_surface_place(struct surface *s, int32_t x, int32_t y,
                                 int32_t z);

/* After writing pixels, mark the whole buffer dirty for the next frame. */
void display_server_surface_damage_full(struct surface *s);
