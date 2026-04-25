#include "mouse.h"

#include "../arch/x86_64/io.h"
#include "../gpu/display_policy.h"
#include "../sched/thread.h"
#include "keyboard.h"
#include "i2c_hid_mouse.h"
#include "usb_xhci_mouse.h"
#include "virtio_tablet.h"

#include "../kprintf.h"

#include <stddef.h>
#include <stdint.h>

#define PS2_DATA     0x60
#define PS2_CMD      0x64
#define PS2_STATUS   0x64

#define PS2_STATUS_OUT  0x01
#define PS2_STATUS_IN   0x02
#define PS2_STATUS_AUX  0x20

static uint8_t  pkt[4];
static int      pkt_i;
static int      pkt_expect = 3;

static volatile int32_t cur_x;
static volatile int32_t cur_y;
static volatile uint8_t cur_btn;
static volatile uint64_t events;
static int               g_probe_worker_started;

/* On-screen boot hint (no serial cable). Filled at end of mouse_init(). */
static char g_boot_l1[120];
static char g_boot_l2[160];

static void boot_scopy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!cap) return;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void boot_sapp(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    while (dst[i] && i < cap) i++;
    if (i >= cap) return;
    size_t j = 0;
    while (i + 1 < cap && src[j]) dst[i++] = src[j++];
    dst[i] = 0;
}

static const char *boot_policy_str(uint8_t p) {
    switch (p) {
    case DISPLAY_POINTER_AUTO: return "auto";
    case DISPLAY_POINTER_PS2: return "ps2";
    case DISPLAY_POINTER_VIRTIO: return "virtio";
    case DISPLAY_POINTER_USB: return "usb";
    case DISPLAY_POINTER_I2C: return "i2c";
    default: return "?";
    }
}

void mouse_boot_hint_refresh(void) {
    const struct display_policy *pol = display_policy_get();
    const char *back = "PS/2 aux";
    if (usb_xhci_mouse_probing())
        back = "USB xHCI (probing)";
    else if (virtio_tablet_active())
        back = "virtio-tablet";
    else if (i2c_hid_mouse_active())
        back = "I2C HID";
    else if (usb_xhci_mouse_active())
        back = "USB xHCI HID";

    boot_scopy(g_boot_l1, sizeof g_boot_l1, "Pointer: ");
    boot_sapp(g_boot_l1, sizeof g_boot_l1, back);

    boot_scopy(g_boot_l2, sizeof g_boot_l2, "policy pointer=");
    boot_sapp(g_boot_l2, sizeof g_boot_l2, boot_policy_str(pol->pointer));
    if (!pol->from_file)
        boot_sapp(g_boot_l2, sizeof g_boot_l2,
                  " | defaults only — initrd has no /etc/display.conf (edit rootfs/etc, make initrd)");
    else {
        int ps2_only =
            !virtio_tablet_active() && !i2c_hid_mouse_active() && !usb_xhci_mouse_active();
        if (ps2_only && pol->pointer == DISPLAY_POINTER_USB) {
            if (usb_xhci_mouse_probing())
                boot_sapp(g_boot_l2, sizeof g_boot_l2,
                          " | USB HID scan in progress (PS/2 idle until found)");
            else if (usb_xhci_mouse_no_device_after_full_scan())
                boot_sapp(g_boot_l2, sizeof g_boot_l2,
                          " | USB: no HID mouse after full scan — other port/hub or pointer=auto");
            else if (!usb_xhci_mouse_driver_began())
                boot_sapp(g_boot_l2, sizeof g_boot_l2,
                          " | USB: xHCI did not start — pointer=auto or serial [usb-mouse]");
            else
                boot_sapp(g_boot_l2, sizeof g_boot_l2,
                          " | USB: check serial [usb-mouse] (unexpected idle)");
        } else if (ps2_only) {
            if (pol->pointer == DISPLAY_POINTER_AUTO && usb_xhci_mouse_probing())
                boot_sapp(g_boot_l2, sizeof g_boot_l2,
                          " | USB xHCI enumerating — PS/2 works; [usb-mouse] serial if no progress");
            else
                boot_sapp(g_boot_l2, sizeof g_boot_l2,
                          " | PS/2 fallback: try pointer=i2c (Intel pad) or different USB port.");
        }
        else
            boot_sapp(g_boot_l2, sizeof g_boot_l2,
                      " | No COM1? Read this bar instead of serial.");

        /* Mirror short USB probe status onto screen for no-serial debugging. */
        const char *usb_dbg = usb_xhci_mouse_debug_status();
        if (usb_dbg && usb_dbg[0]) {
            boot_sapp(g_boot_l2, sizeof g_boot_l2, " | ");
            boot_sapp(g_boot_l2, sizeof g_boot_l2, usb_dbg);
        }
    }
}

const char *mouse_boot_line1(void) { return g_boot_l1; }
const char *mouse_boot_line2(void) { return g_boot_l2; }

/* Until mouse_set_screen(): match display_policy_init_defaults() (1920x1080). */
static uint32_t scr_w = 1920;
static uint32_t scr_h = 1080;

static void ps2_io_delay(void) { outb(0x80, 0); }

static void wait_input_ready(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(PS2_STATUS) & PS2_STATUS_IN) == 0) return;
        ps2_io_delay();
    }
}

static void wait_output_ready(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & PS2_STATUS_OUT) return;
        ps2_io_delay();
    }
}

static void ps2_cmd(uint8_t c)          { wait_input_ready(); outb(PS2_CMD, c); }
static void ps2_write_data(uint8_t v)   { wait_input_ready(); outb(PS2_DATA, v); }
static uint8_t ps2_read_data(void)      { wait_output_ready(); return inb(PS2_DATA); }

static void aux_write(uint8_t v)        { ps2_cmd(0xD4); ps2_write_data(v); }

static uint8_t aux_cmd_ack(uint8_t c) {
    aux_write(c);
    return ps2_read_data();
}

static void aux_cmd_param(uint8_t cmd, uint8_t param) {
    aux_cmd_ack(cmd);
    aux_write(param);
    (void) ps2_read_data();
}

static void ps2_flush_output(void) {
    for (int n = 0; n < 32; n++) {
        if (!(inb(PS2_STATUS) & PS2_STATUS_OUT)) break;
        (void) inb(PS2_DATA);
    }
}

/* Read one byte from data port with timeout (no infinite hang if no device). */
static int ps2_read_byte_timeout(uint8_t *out, int max_iter) {
    for (int i = 0; i < max_iter; i++) {
        if (inb(PS2_STATUS) & PS2_STATUS_OUT) {
            *out = inb(PS2_DATA);
            return 0;
        }
        ps2_io_delay();
    }
    return -1;
}

/* PS/2 mouse reset: 0xFF → FA, AA, 00. Returns 0 if sequence looks valid.
 * Timeouts are bounded so a missing touchpad/USB-only laptop does not stall
 * boot for hundreds of ms per read. */
#define PS2_MOUSE_RESET_READ_ITERS 80000u

static int ps2_aux_mouse_reset(void) {
    aux_write(0xFF);
    uint8_t b;
    if (ps2_read_byte_timeout(&b, PS2_MOUSE_RESET_READ_ITERS) != 0 || b != 0xFA)
        return -1;
    if (ps2_read_byte_timeout(&b, PS2_MOUSE_RESET_READ_ITERS) != 0 || b != 0xAA)
        return -1;
    (void) ps2_read_byte_timeout(&b, PS2_MOUSE_RESET_READ_ITERS);
    return 0;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void mouse_absolute_inject(int32_t x, int32_t y, uint8_t buttons) {
    cur_x   = clamp(x, 0, (int32_t) scr_w - (int32_t) 1);
    cur_y   = clamp(y, 0, (int32_t) scr_h - (int32_t) 1);
    cur_btn = buttons;
    __atomic_add_fetch(&events, 1, __ATOMIC_RELAXED);
}

void mouse_rel_inject(int32_t dx, int32_t dy, uint8_t buttons) {
    cur_x   = clamp(cur_x + dx, 0, (int32_t) scr_w - (int32_t) 1);
    cur_y   = clamp(cur_y - dy, 0, (int32_t) scr_h - (int32_t) 1);
    cur_btn = buttons;
    __atomic_add_fetch(&events, 1, __ATOMIC_RELAXED);
}

void mouse_set_screen(uint32_t screen_w, uint32_t screen_h) {
    const struct display_policy *p = display_policy_get();
    if (!screen_w) screen_w = p->width ? p->width : 1920u;
    if (!screen_h) screen_h = p->height ? p->height : 1080u;
    scr_w = screen_w;
    scr_h = screen_h;
    cur_x = clamp(cur_x, 0, (int32_t) scr_w - 1);
    cur_y = clamp(cur_y, 0, (int32_t) scr_h - 1);
}

/* IntelliMouse magic: many PS/2 touchpads answer id 3/4 and send 4-byte
 * packets (wheel / gesture byte). */
static void ps2_detect_wheel_mode(void) {
    pkt_expect = 3;
    aux_cmd_param(0xF3, 200);
    aux_cmd_param(0xF3, 100);
    aux_cmd_param(0xF3, 80);
    uint8_t a = aux_cmd_ack(0xF2);
    if (a != 0xFA) return;
    uint8_t id = 0;
    if (inb(PS2_STATUS) & PS2_STATUS_OUT) id = ps2_read_data();
    if (id == 3 || id == 4) {
        pkt_expect = 4;
        kprintf("[mouse] PS/2 intellimouse id=%u — 4-byte packets\n", (unsigned) id);
    }
}

static void mouse_init_ps2_aux(void) {
    /* Quiet controller before reprogramming (OSDev 8042 bring-up pattern). */
    ps2_cmd(0xAD);
    ps2_io_delay();
    ps2_cmd(0xA7);
    ps2_io_delay();
    ps2_flush_output();

    ps2_cmd(0xA8);
    ps2_io_delay();

    ps2_cmd(0x20);
    uint8_t cfg = ps2_read_data();
    cfg |= (1u << 1);
    cfg &= ~(1u << 5);
    ps2_cmd(0x60);
    ps2_write_data(cfg);
    ps2_io_delay();

    ps2_cmd(0xAE);
    ps2_io_delay();

    if (ps2_aux_mouse_reset() == 0)
        kprintf("[mouse] PS/2 aux device reset OK (BAT)\n");
    else
        kprintf("[mouse] PS/2 aux reset timeout — many laptops use I2C touchpad "
                "(no PS/2 stream); continuing with defaults\n");

    (void) aux_cmd_ack(0xF6);
    (void) aux_cmd_ack(0xF4);

    ps2_flush_output();

    ps2_detect_wheel_mode();

    kprintf("[mouse] PS/2 aux pkt=%d @ %u,%u — internal touchpads are often I2C "
            "(no PS/2 stream); USB needs xHCI root HID (see [usb-mouse] log); "
            "QEMU: -device virtio-tablet; BIOS: legacy USB→PS/2 if offered\n",
            pkt_expect, (unsigned) cur_x, (unsigned) cur_y);
}

void mouse_init(uint32_t screen_w, uint32_t screen_h) {
    mouse_set_screen(screen_w, screen_h);
    cur_btn = 0;
    pkt_i   = 0;

    const struct display_policy *pol = display_policy_get();
    int force_ps2    = (pol->pointer == DISPLAY_POINTER_PS2);
    int force_virtio = (pol->pointer == DISPLAY_POINTER_VIRTIO);
    int force_usb    = (pol->pointer == DISPLAY_POINTER_USB);
    int force_i2c    = (pol->pointer == DISPLAY_POINTER_I2C);
    int try_virtio   = force_virtio || (pol->pointer == DISPLAY_POINTER_AUTO);
    int try_usb      = force_usb    || (pol->pointer == DISPLAY_POINTER_AUTO);
    int try_i2c      = force_i2c    || (pol->pointer == DISPLAY_POINTER_AUTO);

    if (force_i2c) {
        if (i2c_hid_mouse_init(scr_w, scr_h) == 0) {
            kprintf("[mouse] using I2C HID (Intel LPSS touchpad path)\n");
            goto mouse_init_done;
        }
        kprintf("[mouse] pointer=i2c: no I2C HID — trying USB xHCI then PS/2\n");
        if (!force_ps2) {
            usb_xhci_mouse_init(scr_w, scr_h);
            if (usb_xhci_mouse_active())
                goto mouse_init_done;
        }
        mouse_init_ps2_aux();
        goto mouse_init_done;
    }

    if (!force_ps2 && try_virtio && virtio_tablet_probe_and_init(scr_w, scr_h) == 0) {
        kprintf("[mouse] using virtio-tablet (typical QEMU path)\n");
        goto mouse_init_done;
    }

    if (force_virtio && !virtio_tablet_active())
        kprintf("[mouse] pointer=virtio but no virtio-tablet — trying I2C/USB/PS/2\n");

    if (!force_ps2 && !force_usb && try_i2c && i2c_hid_mouse_init(scr_w, scr_h) == 0) {
        kprintf("[mouse] using I2C HID touchpad (before USB scan)\n");
        goto mouse_init_done;
    }

    if (!force_ps2 && try_usb)
        usb_xhci_mouse_init(scr_w, scr_h);
    if (!force_ps2 && usb_xhci_mouse_active())
        goto mouse_init_done;

    if (force_usb && !usb_xhci_mouse_active())
        kprintf("[mouse] pointer=usb but enumeration failed — PS/2\n");

    mouse_init_ps2_aux();

mouse_init_done:
    mouse_boot_hint_refresh();
}

static void ps2_dispatch_mouse_byte(int aux, uint8_t b) {
    if (!aux) {
        if (pkt_i > 0)
            pkt_i = 0; /* keyboard byte during mouse packet — resync */
        if (!(b & (1u << 3))) {
            keyboard_ps2_handle_byte(b);
            return;
        }
        return;
    }

    if (pkt_i == 0 && !(b & (1u << 3)))
        return;

    pkt[pkt_i++] = b;
    if (pkt_i < pkt_expect)
        return;
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

void mouse_ps2_aux_byte(uint8_t data) {
    if (virtio_tablet_active()) return;
    if (i2c_hid_mouse_active()) return;
    if (usb_xhci_mouse_active()) return;
    ps2_dispatch_mouse_byte(1, data);
}

static void mouse_try_consume(int max_reads) {
    int reads = 0;
    while (inb(PS2_STATUS) & PS2_STATUS_OUT) {
        if (max_reads >= 0 && reads >= max_reads) break;
        reads++;
        uint8_t status = inb(PS2_STATUS);
        uint8_t b      = inb(PS2_DATA);
        int aux = (status & PS2_STATUS_AUX) != 0;
        ps2_dispatch_mouse_byte(aux, b);
    }
}

void mouse_handle_irq(void) {
    if (virtio_tablet_active()) return;
    if (i2c_hid_mouse_active()) return;
    if (usb_xhci_mouse_active()) return;
    mouse_try_consume(512);
}

void mouse_poll(void) {
    if (usb_xhci_mouse_driver_began()) {
        usb_xhci_mouse_poll();
        if (usb_xhci_mouse_active())
            return;
    }
    if (virtio_tablet_active()) {
        virtio_tablet_poll();
        return;
    }
    if (i2c_hid_mouse_active()) {
        i2c_hid_mouse_poll();
        return;
    }
    mouse_try_consume(128);
}

static void mouse_probe_worker(void *arg) {
    (void) arg;
    for (;;) {
        if (usb_xhci_mouse_driver_began() && !usb_xhci_mouse_active())
            usb_xhci_mouse_poll();
        thread_yield();
    }
}

void mouse_start_background_probe(void) {
    if (g_probe_worker_started)
        return;
    g_probe_worker_started = 1;
    thread_create("mouse-probe", mouse_probe_worker, NULL);
}

void mouse_get_state(int32_t *x, int32_t *y, uint8_t *buttons) {
    if (x)       *x       = cur_x;
    if (y)       *y       = cur_y;
    if (buttons) *buttons = cur_btn;
}

uint64_t mouse_events(void) {
    return __atomic_load_n(&events, __ATOMIC_RELAXED);
}
