#include "mouse.h"

#include "../arch/x86_64/io.h"
#include "../gpu/display_policy.h"
#include "keyboard.h"
#include "../kprintf.h"
#include "virtio_tablet.h"

#include <stdint.h>

#define PS2_DATA     0x60
#define PS2_CMD      0x64   /* write */
#define PS2_STATUS   0x64   /* read  */

#define PS2_STATUS_OUT  0x01  /* output buffer full (data ready at 0x60) */
#define PS2_STATUS_IN   0x02  /* input  buffer full (can't write cmd yet)*/
#define PS2_STATUS_AUX  0x20  /* data from aux (mouse) waiting           */

/* Packet assembly. Byte 0 always has bit 3 set — we use that to resync
 * if a stray byte slips in (rare, but happens on some BIOS handoffs). */
static uint8_t  pkt[3];
static int      pkt_i;

static volatile int32_t cur_x;
static volatile int32_t cur_y;
static volatile uint8_t cur_btn;
static volatile uint64_t events;

static uint32_t scr_w = 1280;
static uint32_t scr_h =  800;

/* ---------- PS/2 helpers ----------------------------------------- */

static void wait_input_ready(void) {
    /* Poll until the controller's input buffer drains, so our next
     * command or data byte is accepted. 100k iteration safety cap. */
    for (int i = 0; i < 100000; i++) {
        if ((inb(PS2_STATUS) & PS2_STATUS_IN) == 0) return;
    }
}

static void wait_output_ready(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & PS2_STATUS_OUT) return;
    }
}

static void ps2_cmd(uint8_t c)          { wait_input_ready(); outb(PS2_CMD, c); }
static void ps2_write_data(uint8_t v)   { wait_input_ready(); outb(PS2_DATA, v); }
static uint8_t ps2_read_data(void)      { wait_output_ready(); return inb(PS2_DATA); }

/* Write one byte to the second (aux/mouse) port. */
static void aux_write(uint8_t v)        { ps2_cmd(0xD4); ps2_write_data(v); }

/* Send an aux command, accept up to 2 response bytes (ACK + optional). */
static uint8_t aux_cmd_ack(uint8_t c) {
    aux_write(c);
    return ps2_read_data();   /* expect 0xFA ACK                      */
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---------- public API ------------------------------------------- */

void mouse_absolute_inject(int32_t x, int32_t y, uint8_t buttons) {
    cur_x   = clamp(x, 0, (int32_t) scr_w - (int32_t) 1);
    cur_y   = clamp(y, 0, (int32_t) scr_h - (int32_t) 1);
    cur_btn = buttons;
    __atomic_add_fetch(&events, 1, __ATOMIC_RELAXED);
}

static void mouse_init_ps2_aux(void) {
    /* Enable auxiliary (mouse) port. */
    ps2_cmd(0xA8);

    ps2_cmd(0x20);
    uint8_t cfg = ps2_read_data();
    cfg |= (1u << 1);
    cfg &= ~(1u << 5);
    ps2_cmd(0x60);
    ps2_write_data(cfg);

    uint8_t r0 = aux_cmd_ack(0xF6);
    uint8_t r1 = aux_cmd_ack(0xF4);

    for (int i = 0; i < 8; i++) {
        if (!(inb(PS2_STATUS) & PS2_STATUS_OUT)) break;
        (void) inb(PS2_DATA);
    }

    kprintf("[mouse] ps/2 aux enabled, defaults=0x%x stream=0x%x, cursor @ %u,%u\n",
            (unsigned) r0, (unsigned) r1,
            (unsigned) cur_x, (unsigned) cur_y);
}

void mouse_init(uint32_t screen_w, uint32_t screen_h) {
    scr_w  = screen_w  ? screen_w  : 1280;
    scr_h  = screen_h  ? screen_h  :  800;
    cur_x  = (int32_t) (scr_w / 2);
    cur_y  = (int32_t) (scr_h / 2);
    cur_btn = 0;
    pkt_i   = 0;

    const struct display_policy *pol = display_policy_get();
    int force_ps2    = (pol->pointer == DISPLAY_POINTER_PS2);
    int force_virtio = (pol->pointer == DISPLAY_POINTER_VIRTIO);
    int try_virtio   = force_virtio || (pol->pointer == DISPLAY_POINTER_AUTO);

    if (!force_ps2 && try_virtio && virtio_tablet_probe_and_init(scr_w, scr_h) == 0)
        return;

    if (force_virtio && !virtio_tablet_active())
        kprintf("[mouse] pointer=virtio but no virtio-tablet — using ps/2\n");

    mouse_init_ps2_aux();
}

/* max_reads < 0 = no limit (IRQ path). Poll path must be capped or a stuck
 * controller bit can spin the compositor thread for whole frames (0 FPS). */
static void mouse_try_consume(int max_reads) {
    int reads = 0;
    while (inb(PS2_STATUS) & PS2_STATUS_OUT) {
        if (max_reads >= 0 && reads >= max_reads) break;
        reads++;
        uint8_t status = inb(PS2_STATUS);
        uint8_t b      = inb(PS2_DATA);
        int aux = (status & PS2_STATUS_AUX) != 0;
        if (!aux && pkt_i == 0) {
            if (!(b & (1u << 3))) {
                keyboard_ps2_handle_byte(b);
                continue;
            }
        }

        if (pkt_i == 0 && !(b & (1u << 3))) continue;

        pkt[pkt_i++] = b;
        if (pkt_i < 3) continue;
        pkt_i = 0;

        uint8_t flags = pkt[0];

        int32_t dx = (int32_t) pkt[1];
        int32_t dy = (int32_t) pkt[2];
        if (flags & (1u << 4)) dx |= 0xFFFFFF00u;
        if (flags & (1u << 5)) dy |= 0xFFFFFF00u;

        cur_x = clamp(cur_x + dx,  0, (int32_t) scr_w - (int32_t) 1);
        cur_y = clamp(cur_y - dy,  0, (int32_t) scr_h - (int32_t) 1);

        uint8_t b_state = 0;
        if (flags & 0x01) b_state |= MOUSE_BTN_LEFT;
        if (flags & 0x02) b_state |= MOUSE_BTN_RIGHT;
        if (flags & 0x04) b_state |= MOUSE_BTN_MIDDLE;
        cur_btn = b_state;

        __atomic_add_fetch(&events, 1, __ATOMIC_RELAXED);
    }
}

void mouse_handle_irq(void) {
    if (virtio_tablet_active()) return;
    mouse_try_consume(512);
}

void mouse_poll(void) {
    if (virtio_tablet_active()) {
        virtio_tablet_poll();
        return;
    }
    mouse_try_consume(32);
}

void mouse_get_state(int32_t *x, int32_t *y, uint8_t *buttons) {
    if (x)       *x       = cur_x;
    if (y)       *y       = cur_y;
    if (buttons) *buttons = cur_btn;
}

uint64_t mouse_events(void) {
    return __atomic_load_n(&events, __ATOMIC_RELAXED);
}
