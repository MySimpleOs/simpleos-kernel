#include "i2c_hid_mouse.h"

#include "hid_boot_parse.h"
#include "i2c_dw.h"
#include "mouse.h"

#include "../kprintf.h"
#include "../pci/pci.h"

#include <stddef.h>
#include <stdint.h>

#define OPC_RESET      0x01u
#define OPC_GET_REPORT 0x02u
#define OPC_SET_POWER  0x08u

#define I2C_HID_PWR_ON    0
#define I2C_HID_PWR_SLEEP 1

static void delay_ms(unsigned ms) {
    for (unsigned a = 0; a < ms; a++) {
        for (volatile unsigned b = 0; b < 250000u; b++)
            __asm__ volatile ("pause");
    }
}

static uint16_t get_le16(const uint8_t *p) { return (uint16_t) p[0] | ((uint16_t) p[1] << 8); }

static size_t encode_command(uint8_t *buf, uint8_t opcode, int report_type, int report_id) {
    size_t n = 0;
    if (report_id < 0x0F) {
        buf[n++] = (uint8_t) (report_type << 4) | (uint8_t) report_id;
        buf[n++] = opcode;
    } else {
        buf[n++] = (uint8_t) (report_type << 4) | 0x0Fu;
        buf[n++] = opcode;
        buf[n++] = (uint8_t) report_id;
    }
    return n;
}

static int hid_xfer_write(uint8_t slave, uint16_t cmd_reg_le, const uint8_t *tail, size_t tail_len) {
    uint8_t w[16];
    if (tail_len + 2u > sizeof w)
        return -1;
    w[0] = (uint8_t) (cmd_reg_le & 0xffu);
    w[1] = (uint8_t) (cmd_reg_le >> 8);
    for (size_t i = 0; i < tail_len; i++) w[2 + i] = tail[i];
    return i2c_dw_master_write(slave, w, 2u + tail_len);
}

static int hid_set_power(uint8_t slave, uint16_t cmd_reg, int state) {
    uint8_t tail[4];
    size_t n = encode_command(tail, OPC_SET_POWER, 0, state);
    int r  = hid_xfer_write(slave, cmd_reg, tail, n);
    if (r == 0 && state == I2C_HID_PWR_ON)
        delay_ms(60u);
    return r;
}

static int hid_send_reset(uint8_t slave, uint16_t cmd_reg) {
    uint8_t tail[4];
    size_t n = encode_command(tail, OPC_RESET, 0, 0);
    return hid_xfer_write(slave, cmd_reg, tail, n);
}

static int read_hid_descriptor(uint8_t slave, uint16_t dreg, uint8_t out[30]) {
    uint8_t ptr[2] = {(uint8_t) (dreg & 0xffu), (uint8_t) (dreg >> 8)};
    return i2c_dw_write_read(slave, ptr, 2u, out, 30u);
}

static int pci_is_intel_lpss_i2c(const struct pci_device *d) {
    if (d->class_code == 0x0cu && d->subclass == 0x80u && d->prog_if == 0u)
        return 1;
    if (d->vendor_id != 0x8086u)
        return 0;
    /* Subset of Linux i2c-designware-pci IDs (Skylake → Alder Lake LPSS). */
    static const uint16_t ids[] = {
        0x9d60, 0x9d61, 0x9d62, 0x9d63, 0xa160, 0xa161, 0xa162, 0xa163, 0x31d8, 0x31d9, 0x31da, 0x31db,
        0x34e8, 0x34e9, 0x34ea, 0x34eb, 0x4de8, 0x4de9, 0x4dea, 0x4deb, 0x02e8, 0x02e9, 0x02ea, 0x02eb,
        0x4c60, 0x4c61, 0x4c62, 0x4c63, 0x51e8, 0x51e9, 0x51ea, 0x51eb,
    };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        if (d->device_id == ids[i]) return 1;
    }
    return 0;
}

static uint8_t  g_slave;
static uint16_t g_cmd_reg;
static uint16_t g_data_reg;
static uint16_t g_max_in;
static int      g_active;
static uint8_t  g_buf[256];

static int try_one_bus_slave_hidreg(struct pci_device *pci, uint8_t slave, uint16_t hid_desc_reg) {
    uint8_t desc[30];

    pci_enable_mmio_bus_master(pci);
    if (i2c_dw_bind_pci(pci) != 0)
        return -1;

    if (read_hid_descriptor(slave, hid_desc_reg, desc) != 0) {
        i2c_dw_unbind();
        return -1;
    }

    if (get_le16(desc + 2) != 0x0100u) {
        i2c_dw_unbind();
        return -1;
    }
    if (get_le16(desc + 0) != 30u) {
        i2c_dw_unbind();
        return -1;
    }

    g_cmd_reg  = get_le16(desc + 16);
    g_data_reg = get_le16(desc + 18);
    g_max_in   = get_le16(desc + 10);
    if (g_max_in == 0u || g_max_in > sizeof g_buf)
        g_max_in = (uint16_t) sizeof g_buf;

    if (hid_set_power(slave, g_cmd_reg, I2C_HID_PWR_ON) != 0) {
        i2c_dw_unbind();
        return -1;
    }

    if (hid_send_reset(slave, g_cmd_reg) != 0) {
        i2c_dw_unbind();
        return -1;
    }
    delay_ms(100u);
    (void) hid_set_power(slave, g_cmd_reg, I2C_HID_PWR_ON);

    g_slave   = slave;
    g_active  = 1;
    kprintf("[i2c-hid] touchpad/HID slave=0x%02x hid_reg=0x%04x max_in=%u cmd=0x%x data=0x%x\n",
            (unsigned) slave, (unsigned) hid_desc_reg, (unsigned) g_max_in, (unsigned) g_cmd_reg,
            (unsigned) g_data_reg);
    return 0;
}

int i2c_hid_mouse_init(uint32_t screen_w, uint32_t screen_h) {
    (void) screen_w;
    (void) screen_h;
    g_active = 0;

    static const uint8_t slaves[] = {0x10u, 0x15u, 0x2cu, 0x34u, 0x2bu, 0x40u};
    static const uint16_t hid_regs[] = {0x0001u, 0x0020u};

    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_device *pci = pci_at(i);
        if (!pci || !pci_is_intel_lpss_i2c(pci))
            continue;

        for (size_t hs = 0; hs < sizeof hid_regs / sizeof hid_regs[0]; hs++) {
            for (size_t si = 0; si < sizeof slaves / sizeof slaves[0]; si++) {
                if (try_one_bus_slave_hidreg(pci, slaves[si], hid_regs[hs]) == 0)
                    return 0;
            }
        }
    }

    kprintf("[i2c-hid] no HID-over-I2C touchpad on probed LPSS buses\n");
    return -1;
}

int i2c_hid_mouse_active(void) { return g_active; }

void i2c_hid_mouse_poll(void) {
    if (!g_active)
        return;
    if (i2c_dw_master_read(g_slave, g_buf, (size_t) g_max_in) != 0)
        return;

    uint16_t sz = get_le16(g_buf);
    if (sz <= 2u || sz > g_max_in)
        return;

    int32_t dx, dy;
    uint8_t bt;
    if (!hid_boot_decode_mouse_report(g_buf + 2u, (unsigned) (sz - 2u), &dx, &dy, &bt))
        return;
    mouse_rel_inject(dx, dy, bt);
}
