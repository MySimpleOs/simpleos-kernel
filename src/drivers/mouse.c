#include "mouse.h"

#include "../arch/x86_64/io.h"
#include "../kprintf.h"

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

/* ---------- public API ------------------------------------------- */

void mouse_init(uint32_t screen_w, uint32_t screen_h) {
    scr_w  = screen_w  ? screen_w  : 1280;
    scr_h  = screen_h  ? screen_h  :  800;
    cur_x  = (int32_t) (scr_w / 2);
    cur_y  = (int32_t) (scr_h / 2);
    cur_btn = 0;
    pkt_i   = 0;

    /* Enable auxiliary (mouse) port. */
    ps2_cmd(0xA8);

    /* Read current config byte, enable IRQ12 (bit 1) and clear aux
     * clock disable (bit 5), keep everything else intact. */
    ps2_cmd(0x20);
    uint8_t cfg = ps2_read_data();
    cfg |= (1u << 1);
    cfg &= ~(1u << 5);
    ps2_cmd(0x60);
    ps2_write_data(cfg);

    /* Defaults + enable data reporting. Self-test reset omitted: some
     * QEMU configs throw stray bytes in response and the sequence below
     * is enough to get motion packets. */
    uint8_t r0 = aux_cmd_ack(0xF6);     /* set defaults        */
    uint8_t r1 = aux_cmd_ack(0xF4);     /* enable streaming    */

    /* Drain any leftover bytes (self-test 0xAA etc.) so the first
     * interrupt we handle begins a fresh packet. */
    for (int i = 0; i < 8; i++) {
        if (!(inb(PS2_STATUS) & PS2_STATUS_OUT)) break;
        (void) inb(PS2_DATA);
    }

    kprintf("[mouse] ps/2 aux enabled, defaults=0x%x stream=0x%x, cursor @ %u,%u\n",
            (unsigned) r0, (unsigned) r1,
            (unsigned) cur_x, (unsigned) cur_y);
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void mouse_handle_irq(void) {
    /* Only consume while the aux output buffer still has bytes — the
     * controller sometimes queues two packets by the time we handle the
     * first IRQ, and each IRQ does not guarantee exactly one byte. */
    while (inb(PS2_STATUS) & PS2_STATUS_OUT) {
        uint8_t status = inb(PS2_STATUS);
        if (!(status & PS2_STATUS_AUX)) return;   /* not our byte     */
        uint8_t b = inb(PS2_DATA);

        /* Resync on a lost byte: header byte always has bit 3 set. */
        if (pkt_i == 0 && !(b & (1u << 3))) continue;

        pkt[pkt_i++] = b;
        if (pkt_i < 3) continue;
        pkt_i = 0;

        uint8_t flags = pkt[0];

        /* X and Y are signed 9-bit; bits 4/5 of flags are sign, 6/7 are
         * overflow (ignore — it means huge motion we didn't keep up with). */
        int32_t dx = (int32_t) pkt[1];
        int32_t dy = (int32_t) pkt[2];
        if (flags & (1u << 4)) dx |= 0xFFFFFF00u;  /* sign-extend     */
        if (flags & (1u << 5)) dy |= 0xFFFFFF00u;

        /* PS/2 Y is inverted (up = positive); flip for screen coords. */
        cur_x = clamp(cur_x + dx,  0, (int32_t) scr_w - 1);
        cur_y = clamp(cur_y - dy,  0, (int32_t) scr_h - 1);

        uint8_t b_state = 0;
        if (flags & 0x01) b_state |= MOUSE_BTN_LEFT;
        if (flags & 0x02) b_state |= MOUSE_BTN_RIGHT;
        if (flags & 0x04) b_state |= MOUSE_BTN_MIDDLE;
        cur_btn = b_state;

        __atomic_add_fetch(&events, 1, __ATOMIC_RELAXED);
    }
}

void mouse_get_state(int32_t *x, int32_t *y, uint8_t *buttons) {
    if (x)       *x       = cur_x;
    if (y)       *y       = cur_y;
    if (buttons) *buttons = cur_btn;
}

uint64_t mouse_events(void) {
    return __atomic_load_n(&events, __ATOMIC_RELAXED);
}
