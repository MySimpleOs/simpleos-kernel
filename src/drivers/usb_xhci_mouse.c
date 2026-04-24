#include "usb_xhci_mouse.h"

#include "mouse.h"

#include "../kprintf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../pci/pci.h"

#include <limine.h>

#include <stddef.h>
#include <stdint.h>

extern volatile struct limine_hhdm_request hhdm_request;

enum {
    TRB_NORMAL     = 1,
    TRB_SETUP      = 2,
    TRB_DATA       = 3,
    TRB_STATUS     = 4,
    TRB_LINK       = 6,
    TRB_ENABLE_SLOT = 9,
    TRB_DISABLE_SLOT = 10,
    TRB_ADDR_DEV   = 11,
    TRB_CONFIG_EP  = 12,
    TRB_EVAL_CTX   = 13,
    TRB_TRANSFER   = 32,
    TRB_COMPLETION = 33,
};

#define TRB_TYPE(t)      ((uint32_t)(t) << 10)
#define TRB_CYCLE        (1u << 0)
#define TRB_IOC          (1u << 5)
#define TRB_IDT          (1u << 6)
#define TRB_CHAIN        (1u << 4)
#define TRB_TC           (1u << 1)
#define TRB_ISP          (1u << 2)
#define TRB_DIR_IN       (1u << 16)
#define TRB_TX_TYPE(x)   ((uint32_t)(x) << 16)
#define TRB_DATA_IN      3u
#define TRB_DATA_OUT     2u
#define TRB_LEN(n)       ((uint32_t)(n) & 0x1ffffu)
#define TRB_TD_SIZE(n)   (((uint32_t)(n) < 32u ? (uint32_t)(n) : 31u) << 17)
#define TRB_INTR_TARGET(n) (((uint32_t)(n) & 0x3ffu) << 22)

#define SLOT_FLAG   (1u << 0)
#define EP0_FLAG    (1u << 1)
#define EP_FLAG(n)  (1u << ((n) + 1))

#define LAST_CTX(n)       ((uint32_t)(n) << 27)
#define ROOT_HUB_PORT(p)  (((uint32_t)(p) & 0xffu) << 16)
#define EP_TYPE(t)        ((uint32_t)(t) << 3)
#define CTRL_EP           4u
#define INT_IN_EP         7u
#define MAX_PACKET(p)     (((uint32_t)(p) & 0xffffu) << 16)
#define ERROR_COUNT(p)    (((uint32_t)(p) & 3u) << 1)
#define MAX_BURST(p)      (((uint32_t)(p) & 0xffu) << 8)
#define EP_INTERVAL(p)    (((uint32_t)(p) & 0xffu) << 16)
#define EP_AVG_TRB_LEN(p) ((uint32_t)(p) & 0xffffu)

#define PORT_CCS   (1u << 0)
#define PORT_PED   (1u << 2)
#define PORT_PR    (1u << 4)
#define PORT_SPEED_SHIFT 10

#define CMD_RUN    (1u << 0)
#define CMD_RESET  (1u << 1)
#define STS_HALT   (1u << 0)
#define STS_CNR    (1u << 11)
#define STS_EINT   (1u << 3)

#define ERDP_EHB   (1u << 3)

#define COMP_SUCCESS 1u

#define CTX_BYTES 32u

#define XHCI_MAX_PORTS(p) (((p) >> 24) & 0xffu)
#define HCS_MAX_SLOTS(p)  ((p) & 0xffu)
#define HCS_MAX_SPBUF(p)  (((p) >> 21) & 0x1fu)

#define USBLEG_CAP_ID   1u
#define USBLEG_BIOS_OWN (1u << 16)
#define USBLEG_OS_OWN   (1u << 24)

#define IMAN_IE         (1u << 1)

/* Completion codes — names aligned with Linux xhci COMP_* numbering. */
static const char *trb_comp_str(unsigned c) {
    switch (c) {
    case 1u:
        return "Success";
    case 2u:
        return "DataBufferError";
    case 3u:
        return "BabbleDetected";
    case 4u:
        return "USBTransactionError";
    case 5u:
        return "TRBError";
    case 6u:
        return "StallError";
    case 7u:
        return "ResourceError";
    case 8u:
        return "BandwidthError";
    case 9u:
        return "NoSlotsAvailable";
    case 10u:
        return "InvalidStreamType";
    case 11u:
        return "SlotNotEnabled";
    case 12u:
        return "EndpointNotEnabled";
    case 13u:
        return "ShortPacket";
    case 14u:
        return "RingUnderrun";
    case 15u:
        return "RingOverrun";
    case 16u:
        return "VFEventRingFull";
    case 17u:
        return "ParameterError";
    case 18u:
        return "BandwidthOverrun";
    case 19u:
        return "ContextStateError";
    case 20u:
        return "NoPingResponse";
    case 21u:
        return "EventRingFull";
    case 22u:
        return "IncompatibleDevice";
    case 23u:
        return "MissedService";
    case 24u:
        return "CommandRingStopped";
    case 25u:
        return "CommandAborted";
    case 26u:
        return "Stopped";
    case 27u:
        return "StoppedLengthInvalid";
    case 28u:
        return "StoppedShortPacket";
    case 29u:
        return "MaxExitLatencyTooLarge";
    case 31u:
        return "IsochBufferOverrun";
    case 32u:
        return "EventLost";
    case 33u:
        return "Undefined";
    case 34u:
        return "InvalidStreamID";
    case 35u:
        return "SecondaryBandwidthError";
    case 36u:
        return "SplitTransactionError";
    default:
        return "Unknown";
    }
}

static const char *port_speed_name(unsigned spd) {
    switch (spd) {
    case 0u:
        return "rsvd";
    case 1u:
        return "FS";
    case 2u:
        return "LS";
    case 3u:
        return "HS";
    case 4u:
        return "SS";
    case 5u:
        return "SSP";
    default:
        return "?";
    }
}

/* First memory BAR ≥ 4 KiB (xHCI often BAR0; some firmware uses 64-bit BAR pair). */
static int xhci_pick_mmio_bar(const struct pci_device *d, uint64_t *phys_out, uint64_t *size_out) {
    for (int bi = 0; bi < PCI_BAR_COUNT; bi++) {
        if (d->bars[bi].type != PCI_BAR_MEM) continue;
        if (d->bars[bi].size < 0x1000ull) continue;
        if (d->bars[bi].base == 0ull) continue;
        *phys_out = d->bars[bi].base;
        *size_out = d->bars[bi].size;
        return 0;
    }
    return -1;
}

static uint64_t hhdm(void) {
    return hhdm_request.response ? hhdm_request.response->offset : 0;
}

static void *phys_to_hhdm(uint64_t phys) {
    return (void *) (phys + hhdm());
}

static uint64_t virt_to_phys(const void *v) {
    return (uint64_t) v - hhdm();
}

static void dmb(void) { __asm__ volatile ("mfence" ::: "memory"); }

static int handshake32(volatile uint32_t *reg, uint32_t mask, uint32_t done, int spins) {
    for (int i = 0; i < spins; i++) {
        if ((volatile uint32_t) *reg == 0xffffffffu)
            return -1;
        if ((*reg & mask) == done)
            return 0;
        __asm__ volatile ("pause");
    }
    return -2;
}

static volatile uint32_t *g_cap = NULL;
static volatile uint32_t *g_op = NULL;
static volatile uint32_t *g_run = NULL;
static volatile uint32_t *g_db = NULL;
static uint32_t            g_hci_version;
static uint8_t             g_ctx_shift;
static uint32_t            g_ctx_size;

static int      g_active;
static uint32_t g_sw, g_sh;

static uint64_t g_dcbaa_phys;
static uint8_t *g_dcbaa;

static uint64_t g_cmd_ring_phys;
static uint32_t *g_cmd_ring;
static unsigned g_cmd_enq;
static unsigned g_cmd_cycle;

static uint64_t g_evt_ring_phys;
static uint32_t *g_evt_ring;
static unsigned g_evt_deq;
static unsigned g_evt_cycle;

static uint64_t g_erst_phys;

static uint64_t g_in_ctx_phys;
static uint8_t *g_in_ctx;

static uint64_t g_out_ctx_phys;
static uint8_t *g_out_ctx;

/* Second device context page for a HID device behind an internal USB hub
 * (common laptop touchpad topology). */
static uint64_t g_out_child_ctx_phys;
static uint8_t *g_out_child_ctx;
static int      g_hub_slot_id;

static uint64_t g_ep0_ring_phys;
static uint32_t *g_ep0_ring;
static unsigned g_ep0_enq;
static unsigned g_ep0_cycle;

static uint64_t g_ep_in_ring_phys;
static uint32_t *g_ep_in_ring;
static unsigned g_ep_in_cycle;

static uint64_t g_ctrl_buf_phys;
static uint8_t *g_ctrl_buf;

static uint64_t g_irq_buf_phys;
static uint8_t *g_irq_buf;
static size_t   g_irq_buf_sz;
static unsigned g_last_irq_trb_len;

static unsigned g_max_slots;
static unsigned g_max_ports;
static int      g_slot_id;
static unsigned g_root_port;
static unsigned g_speed;
static unsigned g_ep_in_xhci_idx;
static uint8_t  g_ep_in_addr;
static unsigned g_ep_in_mps;
static unsigned g_ep_in_interval_exp;
static uint8_t  g_config_value;
static uint8_t  g_iface_num;

static uint32_t rd32(const volatile uint32_t *p) { return *p; }
static void wr32(volatile uint32_t *p, uint32_t v) { *p = v; dmb(); }

/* xHCI extended caps: first DWORD = ID (7:0), next (15:8) *4, BIOS/OS bits. */
static void xhci_usb_legacy_handoff(volatile uint32_t *cap) {
    uint32_t hcc = rd32(cap + 0x10u / 4u);
    uint32_t off = (hcc >> 16) & 0xfffcu;
    if (off == 0u) {
        kprintf("[usb-mouse] ext caps: none (xECP=0)\n");
        return;
    }
    unsigned steps = 0;
    while (off != 0u && steps < 64u) {
        steps++;
        volatile uint32_t *w = (volatile uint32_t *) ((uintptr_t) cap + (uintptr_t) off);
        uint32_t v = rd32(w);
        unsigned id = v & 0xffu;
        unsigned next = (v >> 8) & 0xffu;
        if (id == USBLEG_CAP_ID) {
            kprintf("[usb-mouse] USB legacy cap @+0x%x BIOS=%u OS=%u\n", (unsigned) off,
                    (unsigned) ((v & USBLEG_BIOS_OWN) != 0u), (unsigned) ((v & USBLEG_OS_OWN) != 0u));
            if ((v & USBLEG_BIOS_OWN) != 0u && (v & USBLEG_OS_OWN) == 0u) {
                wr32(w, v | USBLEG_OS_OWN);
                for (int i = 0; i < 5000000; i++) {
                    v = rd32(w);
                    if ((v & USBLEG_BIOS_OWN) == 0u)
                        break;
                    __asm__ volatile ("pause");
                }
                kprintf("[usb-mouse] legacy handoff done BIOS=%u OS=%u\n",
                        (unsigned) ((v & USBLEG_BIOS_OWN) != 0u), (unsigned) ((v & USBLEG_OS_OWN) != 0u));
            }
            return;
        }
        if (next == 0u)
            break;
        off += next * 4u;
    }
    kprintf("[usb-mouse] USB legacy cap ID=1 not found (walked %u steps)\n", steps);
}

static int ep_in_queue_read(void);

static uint32_t op_off(unsigned r) {
    uint32_t caplen = rd32(g_cap) & 0xffu;
    return caplen + r;
}

static uint32_t portsc_off(unsigned port1) {
    return op_off(0x400u) + (port1 - 1u) * 16u;
}

static void db_host(void) {
    if (g_db)
        wr32(g_db + 0, 0u);
}

static void db_ep(unsigned slot, unsigned ep_index, unsigned stream) {
    if (!g_db || slot == 0 || slot > 255u)
        return;
    uint32_t v = (((ep_index) + 1u) & 0xffu) | ((uint32_t) stream << 16);
    wr32(g_db + slot, v);
}

static void cmd_trb(uint32_t p0, uint32_t p1, uint32_t p2, uint32_t typ) {
    unsigned i = g_cmd_enq;
    uint32_t *t = g_cmd_ring + i * 4u;
    t[0] = p0;
    t[1] = p1;
    t[2] = p2;
    dmb();
    t[3] = typ | (g_cmd_cycle & 1u);
    g_cmd_enq++;
    if (g_cmd_enq >= 255u) {
        uint32_t *lk = g_cmd_ring + 255u * 4u;
        lk[0] = (uint32_t) g_cmd_ring_phys;
        lk[1] = (uint32_t) (g_cmd_ring_phys >> 32);
        lk[2] = 0;
        dmb();
        lk[3] = TRB_TYPE(TRB_LINK) | TRB_TC | (g_cmd_cycle & 1u);
        g_cmd_enq = 0;
        g_cmd_cycle ^= 1u;
    }
}

static void advance_evt_ring(void) {
    g_evt_deq++;
    if (g_evt_deq >= 256u) {
        g_evt_deq = 0;
        g_evt_cycle ^= 1u;
    }
    uint64_t erdp = g_evt_ring_phys + (uint64_t) g_evt_deq * 16ull;
    volatile uint32_t *er = g_run + (0x20u + 0x18u) / 4u;
    wr32(er, (uint32_t) erdp | ERDP_EHB);
    wr32(er + 1, (uint32_t) (erdp >> 32));
    if (rd32(g_op + 0x04u / 4u) & STS_EINT)
        wr32(g_op + 0x04u / 4u, STS_EINT);
}

static int wait_event_spins(unsigned want_type, uint32_t *out0, uint32_t *out1, uint32_t *out2, uint32_t *out3,
                            int max_spins) {
    for (int spin = 0; spin < max_spins; spin++) {
        uint32_t *ev = g_evt_ring + g_evt_deq * 4u;
        uint32_t c = ev[3];
        if ((c & 1u) != (g_evt_cycle & 1u)) {
            __asm__ volatile ("pause");
            continue;
        }
        uint32_t t = (c >> 10) & 0x3fu;
        if (t == (uint32_t) want_type) {
            if (out0) *out0 = ev[0];
            if (out1) *out1 = ev[1];
            if (out2) *out2 = ev[2];
            if (out3) *out3 = c;
            advance_evt_ring();
            return 0;
        }
        /* Same producer cycle, different TRB: must dequeue or the ring deadlocks
         * (common: COMMAND COMPLETION while waiting for TRANSFER, or port-status
         * events on real silicon). */
        advance_evt_ring();
    }
    return -1;
}

static int wait_event(unsigned want_type, uint32_t *out0, uint32_t *out1, uint32_t *out2, uint32_t *out3) {
    return wait_event_spins(want_type, out0, out1, out2, out3, 8000000);
}

static int issue_cmd(const char *why, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t typ, uint32_t *slot_out) {
    kprintf("[usb-mouse] %s\n", why);
    cmd_trb(p0, p1, p2, typ);
    db_host();
    uint32_t a0, a1, a2, a3;
    if (wait_event(TRB_COMPLETION, &a0, &a1, &a2, &a3) != 0) {
        kprintf("[usb-mouse]   %s: completion TIMEOUT (no event)\n", why);
        return -1;
    }
    uint32_t cc = (a2 >> 24) & 0xffu;
    if (cc != COMP_SUCCESS) {
        kprintf("[usb-mouse]   %s: completion code=%u (%s) p0=%08x p1=%08x\n", why, (unsigned) cc,
                trb_comp_str(cc), (unsigned) a0, (unsigned) a1);
        return -1;
    }
    if (slot_out) {
        *slot_out = (a3 >> 24) & 0xffu;
        kprintf("[usb-mouse]   %s: ok slot=%u\n", why, (unsigned) *slot_out);
    } else
        kprintf("[usb-mouse]   %s: ok\n", why);
    return 0;
}

static uint8_t *in_slot(void) { return g_in_ctx + CTX_BYTES; }
static uint8_t *in_ep(unsigned ep_index) {
    return g_in_ctx + (unsigned) (ep_index + 2u) * CTX_BYTES;
}

static void wr_le32(void *p, unsigned off, uint32_t v) {
    *(uint32_t *) (void *) ((uint8_t *) p + off) = v;
}

static uint32_t rd_le32(const void *p, unsigned off) {
    return *(const uint32_t *) ((const uint8_t *) p + off);
}

static void memset_vol(volatile void *p, int c, size_t n) {
    volatile uint8_t *b = (volatile uint8_t *) p;
    while (n--)
        *b++ = (uint8_t) c;
}

static unsigned fls_u32(uint32_t x) {
    return x ? (unsigned) (32 - __builtin_clz(x)) : 0u;
}

static unsigned fs_int_interval_exp(uint8_t b_interval) {
    unsigned uframes = (unsigned) b_interval * 8u;
    unsigned exp = fls_u32(uframes);
    if (exp > 0) exp--;
    if (exp < 3u) exp = 3u;
    if (exp > 10u) exp = 10u;
    return exp;
}

static int ep0_wait_td(void) {
    uint32_t a0, a1, a2, a3;
    if (wait_event(TRB_TRANSFER, &a0, &a1, &a2, &a3) != 0) {
        kprintf("[usb-mouse] EP0 TD: transfer event TIMEOUT\n");
        return -1;
    }
    uint32_t cc = (a2 >> 24) & 0xffu;
    if (cc != COMP_SUCCESS && cc != 13u) {
        kprintf("[usb-mouse] EP0 TD: completion=%u (%s) len/rem=%08x\n", (unsigned) cc, trb_comp_str(cc),
                (unsigned) a2);
        return -1;
    }
    return 0;
}

static int control_xfer(unsigned slot, const uint8_t *setup, void *data, unsigned len, int dir_in) {
    unsigned need = 2u + (len ? 1u : 0u);
    if (g_ep0_enq + need >= 254u)
        return -1;
    uint32_t cy = (uint32_t) (g_ep0_cycle & 1u);
    uint32_t w0 = (uint32_t) setup[0] | ((uint32_t) setup[1] << 8) | ((uint32_t) setup[2] << 16) |
                  ((uint32_t) setup[3] << 24);
    uint32_t w1 = (uint32_t) setup[4] | ((uint32_t) setup[5] << 8) | ((uint32_t) setup[6] << 16) |
                  ((uint32_t) setup[7] << 24);
    uint32_t f_setup = TRB_IDT | TRB_TYPE(TRB_SETUP) | TRB_CHAIN | cy;
    if (g_hci_version >= 0x0100u && len > 0u)
        f_setup |= TRB_TX_TYPE(dir_in ? TRB_DATA_IN : TRB_DATA_OUT);
    unsigned i = g_ep0_enq;
    uint32_t *t0 = g_ep0_ring + i * 4u;
    t0[0] = w0;
    t0[1] = w1;
    t0[2] = TRB_LEN(8) | TRB_INTR_TARGET(0);
    dmb();
    t0[3] = f_setup;
    g_ep0_enq++;
    if (len) {
        uint64_t p = virt_to_phys(data);
        uint32_t *td = g_ep0_ring + g_ep0_enq * 4u;
        uint32_t fd = TRB_TYPE(TRB_DATA) | TRB_CHAIN | cy;
        if (dir_in)
            fd |= TRB_ISP | TRB_DIR_IN;
        td[0] = (uint32_t) p;
        td[1] = (uint32_t) (p >> 32);
        td[2] = TRB_LEN(len) | TRB_TD_SIZE(0) | TRB_INTR_TARGET(0);
        dmb();
        td[3] = fd;
        g_ep0_enq++;
    }
    uint32_t *ts = g_ep0_ring + g_ep0_enq * 4u;
    uint32_t fs = TRB_IOC | TRB_TYPE(TRB_STATUS) | cy;
    if (!(len && dir_in))
        fs |= TRB_DIR_IN;
    ts[0] = 0;
    ts[1] = 0;
    ts[2] = TRB_INTR_TARGET(0);
    dmb();
    ts[3] = fs;
    g_ep0_enq++;
    db_ep(slot, 0, 0);
    if (ep0_wait_td() != 0) {
        kprintf("[usb-mouse] control transfer failed (setup stage)\n");
        return -1;
    }
    g_ep0_cycle ^= 1u;
    return 0;
}

static int read_descriptor(unsigned slot, uint8_t type, uint8_t index, uint16_t lang, void *buf,
                           unsigned len) {
    uint16_t wv = (uint16_t) type << 8 | (uint16_t) index;
    uint8_t setup[8] = {
        0x80,
        6,
        (uint8_t) wv,
        (uint8_t) (wv >> 8),
        (uint8_t) lang,
        (uint8_t) (lang >> 8),
        (uint8_t) len,
        (uint8_t) (len >> 8),
    };
    return control_xfer(slot, setup, buf, len, 1);
}

static int set_configuration(unsigned slot, uint8_t cfg) {
    uint8_t setup[8] = {0x00, 9, cfg, 0, 0, 0, 0, 0};
    return control_xfer(slot, setup, NULL, 0, 0);
}

static int hid_set_protocol(unsigned slot, uint8_t iface, uint16_t proto) {
    uint8_t setup[8] = {0x21, 11, (uint8_t) proto, (uint8_t) (proto >> 8), iface, 0, 0, 0};
    return control_xfer(slot, setup, NULL, 0, 0);
}

static int hid_set_idle(unsigned slot, uint8_t iface, uint8_t report, uint8_t dur) {
    uint8_t setup[8] = {0x21, 10, report, dur, iface, 0, 0, 0};
    return control_xfer(slot, setup, NULL, 0, 0);
}

static int parse_cfg_simple(const uint8_t *cfg, unsigned cfg_len, uint8_t *cfg_val_out);
static int evaluate_ep0_mps(unsigned mps);
static int address_device_hub_child(unsigned root_port, unsigned speed, uint64_t ep0_phys, uint32_t route20,
                                    unsigned parent_hub_slot, unsigned parent_hub_port);
static int configure_ep_in(void);
static void ep_in_ring_install_link(void);

#define HUB_RT_GET_PORT  0xA3u
#define HUB_RT_SET_CLR   0x23u
#define PORT_FEAT_RESET  4u
#define PORT_FEAT_C_RESET 20u
#define PORT_STAT_CONN   0x0001u
#define PORT_STAT_ENABLE 0x0004u
#define PORT_STAT_HIGH   0x0200u

static int read_hub_descriptor(unsigned slot, void *buf, unsigned len) {
    /* Class IN to device (not standard GET_DESCRIPTOR). */
    uint8_t setup[8] = {0xA0, 6, 0, 0x29, 0, 0, (uint8_t) len, (uint8_t) (len >> 8)};
    return control_xfer(slot, setup, buf, len, 1);
}

static int hub_get_port_status(unsigned hub_slot, unsigned port1, uint32_t *out32) {
    uint8_t setup[8] = {HUB_RT_GET_PORT, 0, 0, 0, (uint8_t) port1, (uint8_t) (port1 >> 8), 4, 0};
    return control_xfer(hub_slot, setup, out32, 4u, 1);
}

static int hub_set_port_feature(unsigned hub_slot, unsigned port1, unsigned feat) {
    uint8_t setup[8] = {HUB_RT_SET_CLR, 3, (uint8_t) feat, (uint8_t) (feat >> 8), (uint8_t) port1,
                        (uint8_t) (port1 >> 8), 0, 0};
    return control_xfer(hub_slot, setup, NULL, 0, 0);
}

static int hub_clear_port_feature(unsigned hub_slot, unsigned port1, unsigned feat) {
    uint8_t setup[8] = {HUB_RT_SET_CLR, 1, (uint8_t) feat, (uint8_t) (feat >> 8), (uint8_t) port1,
                        (uint8_t) (port1 >> 8), 0, 0};
    return control_xfer(hub_slot, setup, NULL, 0, 0);
}

static unsigned hub_wport_speed(uint16_t wps) {
    if (wps & PORT_STAT_HIGH) return 3u;
    return 1u;
}

/* GET_CONFIG…INT pipe (uses g_slot_id); EP0 MPS already evaluated. */
static int xhci_hid_tail_from_config(unsigned port_log) {
    if (read_descriptor((unsigned) g_slot_id, 2u, 0u, 0u, g_ctrl_buf, 9u) != 0) {
        kprintf("[usb-mouse]   port %u: GET_CONFIG(9) failed\n", port_log);
        return -1;
    }
    uint16_t tot = (uint16_t) g_ctrl_buf[2] | ((uint16_t) g_ctrl_buf[3] << 8);
    if (tot < 9u || tot > 1024u)
        tot = 256u;
    if (read_descriptor((unsigned) g_slot_id, 2u, 0u, 0u, g_ctrl_buf, tot) != 0) {
        kprintf("[usb-mouse]   port %u: GET_CONFIG(len=%u) failed\n", port_log, (unsigned) tot);
        return -1;
    }
    if (parse_cfg_simple(g_ctrl_buf, tot, &g_config_value) != 0) {
        kprintf("[usb-mouse]   port %u: no HID interrupt-IN in config\n", port_log);
        return -1;
    }
    kprintf("[usb-mouse]   port %u: cfg_val=%u iface=%u ep_in=0x%02x xhci_ep_idx=%u mps=%u iv=%u\n", port_log,
            (unsigned) g_config_value, (unsigned) g_iface_num, (unsigned) g_ep_in_addr, g_ep_in_xhci_idx,
            g_ep_in_mps, g_ep_in_interval_exp);
    if (set_configuration((unsigned) g_slot_id, g_config_value) != 0) {
        kprintf("[usb-mouse]   port %u: SET_CONFIGURATION failed\n", port_log);
        return -1;
    }
    if (hid_set_protocol((unsigned) g_slot_id, g_iface_num, 1u) == 0)
        kprintf("[usb-mouse]   port %u: HID report protocol\n", port_log);
    else if (hid_set_protocol((unsigned) g_slot_id, g_iface_num, 0u) == 0)
        kprintf("[usb-mouse]   port %u: HID boot protocol\n", port_log);
    else
        kprintf("[usb-mouse]   port %u: SET_PROTOCOL failed (default decode)\n", port_log);
    if (hid_set_idle((unsigned) g_slot_id, g_iface_num, 0, 0) != 0)
        kprintf("[usb-mouse]   port %u: SET_IDLE (non-fatal)\n", port_log);
    if (configure_ep_in() != 0) {
        kprintf("[usb-mouse]   port %u: ConfigureEndpoint failed\n", port_log);
        return -1;
    }
    ep_in_ring_install_link();
    if (ep_in_queue_read() != 0)
        return -1;
    return 0;
}

static int xhci_hid_setup_after_address_device(unsigned port_log) {
    if (read_descriptor((unsigned) g_slot_id, 1u, 0u, 0u, g_ctrl_buf, 8u) != 0) {
        kprintf("[usb-mouse]   port %u: GET_DESCRIPTOR(8) failed\n", port_log);
        return -1;
    }
    unsigned mps0 = g_ctrl_buf[7];
    if (mps0 != 8u && mps0 != 16u && mps0 != 32u && mps0 != 64u)
        mps0 = 64u;
    if (evaluate_ep0_mps(mps0) != 0) {
        kprintf("[usb-mouse]   port %u: EvaluateContext failed\n", port_log);
        return -1;
    }
    if (read_descriptor((unsigned) g_slot_id, 1u, 0u, 0u, g_ctrl_buf, 18u) != 0) {
        kprintf("[usb-mouse]   port %u: GET_DEVICE(18) failed\n", port_log);
        return -1;
    }
    return xhci_hid_tail_from_config(port_log);
}

/* Internal USB2 hub (often internal to laptop); one hub tier, scan ports for HID. */
static int xhci_enumerate_via_usb2_hub(unsigned root_phys_port, unsigned hub_slot) {
    uint8_t hubd[16];
    if (read_descriptor(hub_slot, 2u, 0u, 0u, g_ctrl_buf, 9u) != 0)
        return -1;
    uint16_t htot = (uint16_t) g_ctrl_buf[2] | ((uint16_t) g_ctrl_buf[3] << 8);
    if (htot < 9u || htot > 1024u)
        htot = 32u;
    if (read_descriptor(hub_slot, 2u, 0u, 0u, g_ctrl_buf, htot) != 0)
        return -1;
    uint8_t cfgv = g_ctrl_buf[5];
    if (set_configuration(hub_slot, cfgv) != 0) {
        kprintf("[usb-mouse] hub slot %u: SET_CONFIGURATION failed\n", hub_slot);
        return -1;
    }
    if (read_hub_descriptor(hub_slot, hubd, sizeof hubd) != 0) {
        kprintf("[usb-mouse] hub slot %u: GET_HUB_DESCRIPTOR failed\n", hub_slot);
        return -1;
    }
    unsigned nports = hubd[2];
    if (nports == 0u || nports > 15u)
        nports = 4u;
    kprintf("[usb-mouse] hub slot %u: %u downstream port(s) on root %u\n", hub_slot, nports, root_phys_port);

    g_hub_slot_id = (int) hub_slot;

    for (unsigned hp = 1u; hp <= nports; hp++) {
        uint32_t pst = 0;
        if (hub_get_port_status(hub_slot, hp, &pst) != 0)
            continue;
        uint16_t wps = (uint16_t) (pst & 0xffffu);
        if ((wps & PORT_STAT_CONN) == 0u)
            continue;
        kprintf("[usb-mouse] hub port %u: device present, resetting\n", hp);
        if (hub_set_port_feature(hub_slot, hp, PORT_FEAT_RESET) != 0)
            continue;
        for (int w = 0; w < 8000000; w++) {
            if (hub_get_port_status(hub_slot, hp, &pst) != 0)
                break;
            uint16_t chg = (uint16_t) (pst >> 16);
            if (chg & (1u << 4)) {
                (void) hub_clear_port_feature(hub_slot, hp, PORT_FEAT_C_RESET);
                break;
            }
            __asm__ volatile ("pause");
        }
        wps = 0;
        for (int w = 0; w < 8000000; w++) {
            if (hub_get_port_status(hub_slot, hp, &pst) != 0)
                break;
            wps = (uint16_t) (pst & 0xffffu);
            if (wps & PORT_STAT_ENABLE)
                break;
            __asm__ volatile ("pause");
        }
        if ((wps & PORT_STAT_ENABLE) == 0u) {
            kprintf("[usb-mouse] hub port %u: not enabled after reset\n", hp);
            continue;
        }

        uint32_t ign = 0;
        if (issue_cmd("EnableSlot(child)", 0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &ign) != 0 || ign == 0)
            continue;
        unsigned child_slot = ign;
        if (!g_out_child_ctx_phys) {
            kprintf("[usb-mouse] no child device context buffer\n");
            (void) issue_cmd("DisableSlot(child)", 0, 0, 0,
                              TRB_TYPE(TRB_DISABLE_SLOT) | (child_slot << 24), NULL);
            continue;
        }
        {
            uint64_t *dc = (uint64_t *) g_dcbaa;
            dc[child_slot] = g_out_child_ctx_phys;
        }
        unsigned child_spd = hub_wport_speed(wps);
        g_slot_id = (int) child_slot;
        if (address_device_hub_child(root_phys_port, child_spd, g_ep0_ring_phys, hp & 0xfffffu, hub_slot, hp) !=
            0) {
            kprintf("[usb-mouse] hub port %u: AddressDevice(child) failed\n", hp);
            (void) issue_cmd("DisableSlot(child)", 0, 0, 0,
                              TRB_TYPE(TRB_DISABLE_SLOT) | (child_slot << 24), NULL);
            uint64_t *dc = (uint64_t *) g_dcbaa;
            dc[child_slot] = 0;
            g_slot_id = 0;
            continue;
        }
        if (xhci_hid_setup_after_address_device(root_phys_port) == 0) {
            kprintf("[usb-mouse] HID via hub root=%u hub_slot=%u port=%u child_slot=%u\n", root_phys_port,
                    hub_slot, hp, child_slot);
            return 0;
        }
        (void) issue_cmd("DisableSlot(child-fail)", 0, 0, 0,
                          TRB_TYPE(TRB_DISABLE_SLOT) | (child_slot << 24), NULL);
        {
            uint64_t *dc = (uint64_t *) g_dcbaa;
            dc[child_slot] = 0;
        }
        g_slot_id = 0;
    }
    return -1;
}

static void fill_ep0_ctx(uint8_t *epctx, uint64_t ring_phys, unsigned mps) {
    uint32_t ep2 = EP_TYPE(CTRL_EP) | ERROR_COUNT(3u) | MAX_PACKET(mps);
    wr_le32(epctx, 0, 0);
    wr_le32(epctx, 4, ep2);
    wr_le32(epctx, 8, (uint32_t) (ring_phys | 1ull));
    wr_le32(epctx, 12, (uint32_t) (ring_phys >> 32));
    wr_le32(epctx, 16, EP_AVG_TRB_LEN(8));
    wr_le32(epctx, 20, 0);
    wr_le32(epctx, 24, 0);
    wr_le32(epctx, 28, 0);
}

static void fill_ep_in_ctx(uint8_t *epctx, uint64_t ring_phys, unsigned mps, unsigned interval_exp) {
    uint32_t ep2 = EP_TYPE(INT_IN_EP) | ERROR_COUNT(3u) | MAX_PACKET(mps) | MAX_BURST(0u);
    wr_le32(epctx, 0, EP_INTERVAL(interval_exp));
    wr_le32(epctx, 4, ep2);
    wr_le32(epctx, 8, (uint32_t) (ring_phys | 1ull));
    wr_le32(epctx, 12, (uint32_t) (ring_phys >> 32));
    wr_le32(epctx, 16, EP_AVG_TRB_LEN(mps));
    wr_le32(epctx, 20, 0);
    wr_le32(epctx, 24, 0);
    wr_le32(epctx, 28, 0);
}

static int address_device_path(unsigned port, unsigned speed, uint64_t ep0_phys) {
    memset_vol(g_in_ctx, 0, 4096);
    wr_le32(g_in_ctx, 0, 0);
    wr_le32(g_in_ctx, 4, SLOT_FLAG | EP0_FLAG);
    uint32_t dev_info = LAST_CTX(1u) | (((uint32_t) speed & 0xfu) << 20);
    wr_le32(in_slot(), 0, dev_info);
    wr_le32(in_slot(), 4, ROOT_HUB_PORT(port));
    wr_le32(in_slot(), 8, 0);
    wr_le32(in_slot(), 12, 0);
    fill_ep0_ctx(in_ep(0), ep0_phys, 8u);
    if (issue_cmd("AddressDevice", (uint32_t) g_in_ctx_phys, (uint32_t) (g_in_ctx_phys >> 32), 0,
                  TRB_TYPE(TRB_ADDR_DEV) | ((uint32_t) g_slot_id << 24), NULL) != 0)
        return -1;
    return 0;
}

/* xHCI slot context: route string (tier-1 hub port in low nibble), root port in
 * bits 0–7 of dev_info2, parent hub slot + parent port in 16–31 (Intel layout). */
static int address_device_hub_child(unsigned root_port, unsigned speed, uint64_t ep0_phys, uint32_t route20,
                                    unsigned parent_hub_slot, unsigned parent_hub_port) {
    memset_vol(g_in_ctx, 0, 4096);
    wr_le32(g_in_ctx, 0, 0);
    wr_le32(g_in_ctx, 4, SLOT_FLAG | EP0_FLAG);
    uint32_t dev_info = LAST_CTX(1u) | (((uint32_t) speed & 0xfu) << 20) | (route20 & 0xfffffu);
    wr_le32(in_slot(), 0, dev_info);
    uint32_t di2 = ((uint32_t) root_port & 0xffu) | (((uint32_t) parent_hub_slot & 0xffu) << 16) |
                   (((uint32_t) parent_hub_port & 0xffu) << 24);
    wr_le32(in_slot(), 4, di2);
    wr_le32(in_slot(), 8, 0);
    wr_le32(in_slot(), 12, 0);
    fill_ep0_ctx(in_ep(0), ep0_phys, 8u);
    if (issue_cmd("AddressDevice(hub_child)", (uint32_t) g_in_ctx_phys, (uint32_t) (g_in_ctx_phys >> 32), 0,
                  TRB_TYPE(TRB_ADDR_DEV) | ((uint32_t) g_slot_id << 24), NULL) != 0)
        return -1;
    return 0;
}

static int evaluate_ep0_mps(unsigned mps) {
    memset_vol(g_in_ctx, 0, 4096);
    wr_le32(g_in_ctx, 0, 0);
    wr_le32(g_in_ctx, 4, EP0_FLAG);
    fill_ep0_ctx(in_ep(0), g_ep0_ring_phys, mps);
    return issue_cmd("EvaluateContext(ep0_max_packet)", (uint32_t) g_in_ctx_phys, (uint32_t) (g_in_ctx_phys >> 32),
                     0, TRB_TYPE(TRB_EVAL_CTX) | ((uint32_t) g_slot_id << 24), NULL);
}

static uint8_t *out_ctx_for_slot(int slot) {
    if (slot > 0 && g_dcbaa && g_out_child_ctx_phys) {
        uint64_t ent = ((uint64_t *) g_dcbaa)[(unsigned) slot];
        if (ent == g_out_child_ctx_phys && g_out_child_ctx)
            return g_out_child_ctx;
    }
    return g_out_ctx;
}

static int configure_ep_in(void) {
    uint8_t *oc = out_ctx_for_slot(g_slot_id);
    if (!oc)
        return -1;
    memset_vol(g_in_ctx, 0, 4096);
    for (unsigned j = 0; j < CTX_BYTES; j++)
        g_in_ctx[CTX_BYTES + j] = oc[j];
    for (unsigned j = 0; j < CTX_BYTES; j++)
        g_in_ctx[2u * CTX_BYTES + j] = oc[CTX_BYTES + j];
    uint32_t add = SLOT_FLAG | EP_FLAG(g_ep_in_xhci_idx);
    wr_le32(g_in_ctx, 0, 0);
    wr_le32(g_in_ctx, 4, add);
    uint32_t last = (uint32_t) fls_u32(add) - 1u;
    uint32_t di = rd_le32(in_slot(), 0);
    di = (di & ~LAST_CTX(0x1fu)) | LAST_CTX(last);
    wr_le32(in_slot(), 0, di);
    memset_vol(in_ep(g_ep_in_xhci_idx), 0, CTX_BYTES);
    fill_ep_in_ctx(in_ep(g_ep_in_xhci_idx), g_ep_in_ring_phys, g_ep_in_mps, g_ep_in_interval_exp);
    return issue_cmd("ConfigureEndpoint(HID_int_in)", (uint32_t) g_in_ctx_phys, (uint32_t) (g_in_ctx_phys >> 32),
                     0, TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t) g_slot_id << 24), NULL);
}

static int parse_cfg_simple(const uint8_t *cfg, unsigned cfg_len, uint8_t *cfg_val_out) {
    unsigned i = 0;
    *cfg_val_out = 1;
    g_iface_num = 0;
    g_ep_in_addr = 0;
    g_ep_in_mps = 8;
    g_ep_in_interval_exp = 4;
    g_ep_in_xhci_idx = 0;

    uint8_t cur_iface = 0;
    uint8_t cur_class = 0xffu;
    uint8_t cur_sub = 0;
    uint8_t cur_proto = 0;

    int have_cand = 0;
    uint8_t cand_iface = 0;
    uint8_t cand_epa = 0;
    uint16_t cand_mps = 8;
    uint8_t cand_binterval = 4;
    unsigned cand_xhci_idx = 0;

    while (i + 2 < cfg_len) {
        uint8_t bl = cfg[i];
        if (bl < 2)
            break;
        uint8_t ty = cfg[i + 1];
        if (ty == 2u && bl >= 9u)
            *cfg_val_out = cfg[i + 5];
        if (ty == 4u && bl >= 9u) {
            cur_iface  = cfg[i + 2];
            cur_class  = cfg[i + 5];
            cur_sub    = cfg[i + 6];
            cur_proto  = cfg[i + 7];
        }
        if (ty == 5u && bl >= 7u) {
            uint8_t epa = cfg[i + 2];
            uint8_t attr = cfg[i + 3];
            int hid = (cur_class == 3u);
            if (hid && (epa & 0x80u) != 0u && ((attr & 3u) == 3u)) {
                uint16_t mps = (uint16_t) cfg[i + 4] | ((uint16_t) cfg[i + 5] << 8);
                unsigned epnum = epa & 0xfu;
                int dir_in = (epa & 0x80u) != 0;
                unsigned xhci_idx = epnum * 2u + (dir_in ? 1u : 0u) - 1u;
                unsigned iv_exp = fs_int_interval_exp(cfg[i + 6]);
                /* Prefer Boot Mouse (subclass 1, protocol 2) over first HID INT IN
                 * (avoids grabbing keyboard IN on composite NKRO + mouse). */
                if (cur_sub == 1u && cur_proto == 2u) {
                    g_iface_num           = cur_iface;
                    g_ep_in_addr          = epa;
                    g_ep_in_mps           = mps & 0x7ffu;
                    g_ep_in_interval_exp  = iv_exp;
                    g_ep_in_xhci_idx      = xhci_idx;
                    return 0;
                }
                if (!have_cand) {
                    have_cand        = 1;
                    cand_iface       = cur_iface;
                    cand_epa         = epa;
                    cand_mps         = mps & 0x7ffu;
                    cand_binterval   = cfg[i + 6];
                    cand_xhci_idx    = xhci_idx;
                }
            }
        }
        i += bl;
    }
    if (!have_cand)
        return -1;
    g_iface_num          = cand_iface;
    g_ep_in_addr         = cand_epa;
    g_ep_in_mps          = cand_mps;
    g_ep_in_interval_exp = fs_int_interval_exp(cand_binterval);
    g_ep_in_xhci_idx     = cand_xhci_idx;
    return 0;
}

static void ep_in_ring_install_link(void) {
    uint32_t *ilk = g_ep_in_ring + 255u * 4u;
    ilk[0] = (uint32_t) g_ep_in_ring_phys;
    ilk[1] = (uint32_t) (g_ep_in_ring_phys >> 32);
    ilk[2] = 0;
    dmb();
    ilk[3] = TRB_TYPE(TRB_LINK) | TRB_TC | (g_ep_in_cycle & 1u);
}

static void xhci_abort_active_slot(void) {
    uint64_t *dc = g_dcbaa ? (uint64_t *) g_dcbaa : NULL;
    if (g_slot_id > 0 && dc) {
        (void) issue_cmd("DisableSlot(cleanup)", 0, 0, 0,
                          TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t) g_slot_id << 24), NULL);
        dc[g_slot_id] = 0;
    }
    g_slot_id = 0;
    if (g_hub_slot_id > 0 && dc) {
        (void) issue_cmd("DisableSlot(hub)", 0, 0, 0,
                          TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t) g_hub_slot_id << 24), NULL);
        dc[g_hub_slot_id] = 0;
    }
    g_hub_slot_id = 0;
    if (g_ep0_ring) {
        memset_vol(g_ep0_ring, 0, 4096);
        g_ep0_enq   = 0;
        g_ep0_cycle = 1;
    }
    if (g_ep_in_ring) {
        memset_vol(g_ep_in_ring, 0, 4096);
        g_ep_in_cycle = 1;
        ep_in_ring_install_link();
    }
    if (g_out_ctx)
        memset_vol(g_out_ctx, 0, 4096);
    if (g_out_child_ctx)
        memset_vol(g_out_child_ctx, 0, 4096);
}

static int xhci_enumerate_hid_on_port(unsigned port) {
    volatile uint32_t *ps = (volatile uint32_t *) ((uintptr_t) g_cap + portsc_off(port));
    uint32_t v = rd32(ps);
    kprintf("[usb-mouse] enumerate: root port %u PORTSC=0x%08x\n", port, (unsigned) v);
    if (!(v & PORT_CCS)) {
        kprintf("[usb-mouse]   port %u: CCS=0 — nothing connected\n", port);
        return -1;
    }
    wr32(ps, v | PORT_PR);
    for (int w = 0; w < 5000000; w++) {
        v = rd32(ps);
        if (!(v & PORT_PR))
            break;
        __asm__ volatile ("pause");
    }
    for (int w = 0; w < 5000000; w++) {
        v = rd32(ps);
        if (v & PORT_PED)
            break;
        __asm__ volatile ("pause");
    }
    if (!(v & PORT_PED)) {
        kprintf("[usb-mouse]   port %u: not enabled (PED=0) PORTSC=0x%08x\n", port, (unsigned) rd32(ps));
        return -1;
    }
    g_root_port = port;
    g_speed     = (v >> PORT_SPEED_SHIFT) & 0xfu;
    kprintf("[usb-mouse]   port %u: link up speed=%s (%u)\n", port, port_speed_name(g_speed), g_speed);

    uint32_t ign = 0;
    if (issue_cmd("EnableSlot", 0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &ign) != 0 || ign == 0) {
        kprintf("[usb-mouse]   port %u: EnableSlot failed\n", port);
        return -1;
    }
    g_slot_id = (int) ign;

    {
        uint64_t *dc = (uint64_t *) g_dcbaa;
        dc[g_slot_id] = g_out_ctx_phys;
    }

    if (address_device_path(g_root_port, g_speed, g_ep0_ring_phys) != 0) {
        kprintf("[usb-mouse]   port %u: AddressDevice failed\n", port);
        xhci_abort_active_slot();
        return -1;
    }
    if (read_descriptor((unsigned) g_slot_id, 1u, 0u, 0u, g_ctrl_buf, 8u) != 0) {
        kprintf("[usb-mouse]   port %u: GET_DESCRIPTOR(8) failed\n", port);
        xhci_abort_active_slot();
        return -1;
    }
    unsigned mps0 = g_ctrl_buf[7];
    if (mps0 != 8u && mps0 != 16u && mps0 != 32u && mps0 != 64u)
        mps0 = 64u;
    kprintf("[usb-mouse]   port %u: EP0 mps0=%u (from device desc)\n", port, mps0);
    if (evaluate_ep0_mps(mps0) != 0) {
        kprintf("[usb-mouse]   port %u: EvaluateContext failed\n", port);
        xhci_abort_active_slot();
        return -1;
    }
    if (read_descriptor((unsigned) g_slot_id, 1u, 0u, 0u, g_ctrl_buf, 18u) != 0) {
        kprintf("[usb-mouse]   port %u: GET_DEVICE(18) failed\n", port);
        xhci_abort_active_slot();
        return -1;
    }
    if (g_ctrl_buf[4] == 9u) {
        unsigned hub_slot = (unsigned) g_slot_id;
        if (xhci_enumerate_via_usb2_hub(port, hub_slot) == 0)
            return 0;
        xhci_abort_active_slot();
        return -1;
    }
    if (xhci_hid_tail_from_config(port) != 0) {
        xhci_abort_active_slot();
        return -1;
    }
    return 0;
}

static void xhci_stop_before_detach(void) {
    if (!g_op)
        return;
    wr32(g_op + 0u, rd32(g_op + 0u) & ~CMD_RUN);
    (void) handshake32(g_op + 0x04u / 4u, STS_HALT, STS_HALT, 500000);
}

static void free_all(void) {
    /* PMM has no bulk free in API — leak frames on shutdown (rare). */
    g_active = 0;
    xhci_stop_before_detach();
    g_cap = g_op = g_run = g_db = NULL;
    g_irq_buf_sz        = 0;
    g_last_irq_trb_len  = 0;
}

void usb_xhci_mouse_shutdown(void) { free_all(); }

int usb_xhci_mouse_active(void) { return g_active; }

/* Decode boot (3B) / report-id + boot / 16-bit relative XY (common vendor HID). */
static int decode_hid_mouse(const uint8_t *p, unsigned len, int32_t *odx, int32_t *ody, uint8_t *obtn) {
    *odx = *ody = 0;
    *obtn = 0;
    if (len < 3u)
        return 0;

    unsigned o = 0;
    if (len >= 4u && p[0] >= 1u && p[0] <= 16u && (p[1] & 0x08u) != 0u)
        o = 1u;

    if (len >= o + 3u) {
        uint8_t f = p[o];
        if ((f & 0x08u) != 0u) {
            int32_t dx = (int32_t)(int8_t) p[o + 1u];
            int32_t dy = (int32_t)(int8_t) p[o + 2u];
            if (f & 0x10u) dx -= 256;
            if (f & 0x20u) dy -= 256;
            uint8_t bt = 0;
            if (f & 1u) bt |= MOUSE_BTN_LEFT;
            if (f & 2u) bt |= MOUSE_BTN_RIGHT;
            if (f & 4u) bt |= MOUSE_BTN_MIDDLE;
            *odx = dx;
            *ody = dy;
            *obtn = bt;
            return 1;
        }
    }

    if (len >= 5u) {
        int16_t x = (int16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
        int16_t y = (int16_t) ((uint16_t) p[2] | ((uint16_t) p[3] << 8));
        if (x != 0 || y != 0) {
            *odx = (int32_t) x;
            *ody = (int32_t) y;
            if (len >= 5u) {
                uint8_t bb = p[4];
                if (bb & 1u) *obtn |= MOUSE_BTN_LEFT;
                if (bb & 2u) *obtn |= MOUSE_BTN_RIGHT;
                if (bb & 4u) *obtn |= MOUSE_BTN_MIDDLE;
            }
            return 1;
        }
    }

    if (len >= 6u) {
        int16_t x = (int16_t) ((uint16_t) p[1] | ((uint16_t) p[2] << 8));
        int16_t y = (int16_t) ((uint16_t) p[3] | ((uint16_t) p[4] << 8));
        if (x != 0 || y != 0) {
            uint8_t f = p[0];
            uint8_t bt = 0;
            if (f & 1u) bt |= MOUSE_BTN_LEFT;
            if (f & 2u) bt |= MOUSE_BTN_RIGHT;
            if (f & 4u) bt |= MOUSE_BTN_MIDDLE;
            *odx = (int32_t) x;
            *ody = (int32_t) y;
            *obtn = bt;
            return 1;
        }
    }

    return 0;
}

static int ep_in_queue_read(void) {
    unsigned maxpl = (g_irq_buf_sz > 1024u) ? 1024u : (unsigned) g_irq_buf_sz;
    if (maxpl < 4u)
        maxpl = 4u;
    unsigned plen = g_ep_in_mps;
    if (plen < 4u)
        plen = 4u;
    if (plen > maxpl)
        plen = maxpl;
    g_last_irq_trb_len = plen;
    uint32_t cy = (uint32_t) (g_ep_in_cycle & 1u);
    uint32_t *t = g_ep_in_ring;
    t[0] = (uint32_t) g_irq_buf_phys;
    t[1] = (uint32_t) (g_irq_buf_phys >> 32);
    t[2] = TRB_LEN(plen) | TRB_TD_SIZE(0) | TRB_INTR_TARGET(0);
    dmb();
    t[3] = TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP | cy;
    db_ep((unsigned) g_slot_id, g_ep_in_xhci_idx, 0);
    return 0;
}

void usb_xhci_mouse_poll(void) {
    static unsigned s_poll_xfer_errs;
    if (!g_active)
        return;
    uint32_t a0, a1, a2, a3;
    if (wait_event_spins(TRB_TRANSFER, &a0, &a1, &a2, &a3, 262144) != 0)
        return;
    uint32_t cc = (a2 >> 24) & 0xffu;
    if (cc != COMP_SUCCESS && cc != 13u) {
        if (s_poll_xfer_errs < 8u) {
            kprintf("[usb-mouse] poll: INT transfer cc=%u (%s) a2=0x%08x\n", (unsigned) cc, trb_comp_str(cc),
                    (unsigned) a2);
            s_poll_xfer_errs++;
        }
        g_ep_in_cycle ^= 1u;
        (void) ep_in_queue_read();
        return;
    }
    unsigned res = (unsigned) (a2 & 0xffffffu);
    unsigned rx = g_last_irq_trb_len;
    if (res < g_last_irq_trb_len)
        rx = g_last_irq_trb_len - res;
    if (rx == 0u || rx > (unsigned) g_irq_buf_sz)
        rx = (unsigned) g_irq_buf_sz;

    int32_t dx, dy;
    uint8_t bt;
    if (!decode_hid_mouse(g_irq_buf, rx, &dx, &dy, &bt)) {
        g_ep_in_cycle ^= 1u;
        (void) ep_in_queue_read();
        return;
    }
    mouse_rel_inject(dx, dy, bt);
    g_ep_in_cycle ^= 1u;
    (void) ep_in_queue_read();
}

static int xhci_try_attach(struct pci_device *d) {
    uint64_t bar_phys, bar_sz;
    if (xhci_pick_mmio_bar(d, &bar_phys, &bar_sz) != 0) {
        kprintf("[usb-mouse] %02x:%02x.%u: no usable MMIO BAR\n", (unsigned) d->bus, (unsigned) d->slot,
                (unsigned) d->func);
        return -1;
    }
    pci_enable_mmio_bus_master(d);
    uint64_t bar_virt = bar_phys + hhdm();
    uint32_t map_sz = (uint32_t) (bar_sz > 0x200000ull ? 0x200000ull : bar_sz);
    mmio_map(bar_virt, bar_phys, map_sz);
    g_cap = (volatile uint32_t *) (uintptr_t) bar_virt;
    uint32_t cap0 = rd32(g_cap);
    uint32_t caplen = cap0 & 0xffu;
    g_hci_version = (cap0 >> 16) & 0xffffu;
    uint32_t hccp1 = rd32(g_cap + 0x10u / 4u);
    g_ctx_shift = (uint8_t) ((hccp1 & (1u << 2)) ? 1u : 0u);
    g_ctx_size = 32u << g_ctx_shift;
    (void) g_ctx_size;
    g_op = (volatile uint32_t *) ((uintptr_t) g_cap + caplen);
    uint32_t hcsp1 = rd32(g_cap + 4u / 4u);
    g_max_slots = HCS_MAX_SLOTS(hcsp1);
    g_max_ports = XHCI_MAX_PORTS(hcsp1);
    if (g_max_slots == 0u || g_max_slots > 255u) {
        kprintf("[usb-mouse] bad MaxSlots %u\n", g_max_slots);
        free_all();
        return -1;
    }
    uint32_t rts = rd32(g_cap + 0x18u / 4u) & ~3u;
    uint32_t dbo = rd32(g_cap + 0x14u / 4u) & ~3u;
    g_run = (volatile uint32_t *) ((uintptr_t) g_cap + rts);
    g_db = (volatile uint32_t *) ((uintptr_t) g_cap + dbo);

    kprintf("[usb-mouse] PCI %02x:%02x.%u %04x:%04x MMIO phys=0x%llx map=%uk caplen=%u HCI=0x%04x "
            "slots=%u ports=%u ctx64=%u\n",
            (unsigned) d->bus, (unsigned) d->slot, (unsigned) d->func, (unsigned) d->vendor_id,
            (unsigned) d->device_id, (unsigned long long) bar_phys, (unsigned) (map_sz / 1024u),
            (unsigned) caplen, (unsigned) g_hci_version, g_max_slots, g_max_ports,
            (unsigned) g_ctx_shift);

    xhci_usb_legacy_handoff(g_cap);

    wr32(g_op + 0u, rd32(g_op + 0u) & ~CMD_RUN);
    if (handshake32(g_op + 0x04u / 4u, STS_HALT, STS_HALT, 2000000) != 0) {
        kprintf("[usb-mouse] halt timeout USBSTS=0x%08x (expected HCHalted=1)\n",
                (unsigned) rd32(g_op + 0x04u / 4u));
        free_all();
        return -1;
    }
    kprintf("[usb-mouse] HC halted, issuing HCRESET\n");
    wr32(g_op + 0u, rd32(g_op + 0u) | CMD_RESET);
    if (handshake32(g_op + 0u, CMD_RESET, 0, 2000000) != 0) {
        kprintf("[usb-mouse] HCRST timeout\n");
        free_all();
        return -1;
    }
    if (handshake32(g_op + 0x04u / 4u, STS_CNR, 0, 2000000) != 0) {
        kprintf("[usb-mouse] CNR timeout\n");
        free_all();
        return -1;
    }

    uint32_t hcsp2 = rd32(g_cap + 8u / 4u);
    unsigned nsp = HCS_MAX_SPBUF(hcsp2);
    uint64_t sp_arr_phys = 0;
    uint64_t *sp_arr_virt = NULL;
    if (nsp > 0u) {
        kprintf("[usb-mouse] scratchpad buffers=%u\n", nsp);
        sp_arr_phys = pmm_alloc_page();
        if (!sp_arr_phys) {
            free_all();
            return -1;
        }
        sp_arr_virt = (uint64_t *) phys_to_hhdm(sp_arr_phys);
        for (unsigned i = 0; i < nsp; i++) {
            uint64_t f = pmm_alloc_page();
            if (!f) {
                free_all();
                return -1;
            }
            sp_arr_virt[i] = f;
        }
    }

    g_dcbaa_phys = pmm_alloc_page();
    g_cmd_ring_phys = pmm_alloc_page();
    g_evt_ring_phys = pmm_alloc_page();
    g_erst_phys     = pmm_alloc_page();
    g_in_ctx_phys = pmm_alloc_page();
    g_out_ctx_phys       = pmm_alloc_page();
    g_out_child_ctx_phys = pmm_alloc_page();
    g_ep0_ring_phys = pmm_alloc_page();
    g_ep_in_ring_phys = pmm_alloc_page();
    g_ctrl_buf_phys = pmm_alloc_page();
    g_irq_buf_phys = pmm_alloc_contig(2);
    if (g_irq_buf_phys) {
        g_irq_buf_sz = 8192u;
    } else {
        g_irq_buf_phys = pmm_alloc_page();
        g_irq_buf_sz = g_irq_buf_phys ? 4096u : 0u;
    }
    if (!g_dcbaa_phys || !g_cmd_ring_phys || !g_evt_ring_phys || !g_erst_phys || !g_in_ctx_phys ||
        !g_out_ctx_phys || !g_out_child_ctx_phys || !g_ep0_ring_phys || !g_ep_in_ring_phys ||
        !g_ctrl_buf_phys || !g_irq_buf_phys) {
        kprintf("[usb-mouse] pmm alloc failed\n");
        free_all();
        return -1;
    }
    g_dcbaa = (uint8_t *) phys_to_hhdm(g_dcbaa_phys);
    g_cmd_ring = (uint32_t *) phys_to_hhdm(g_cmd_ring_phys);
    g_evt_ring = (uint32_t *) phys_to_hhdm(g_evt_ring_phys);
    g_in_ctx = (uint8_t *) phys_to_hhdm(g_in_ctx_phys);
    g_out_ctx       = (uint8_t *) phys_to_hhdm(g_out_ctx_phys);
    g_out_child_ctx = (uint8_t *) phys_to_hhdm(g_out_child_ctx_phys);
    g_ep0_ring = (uint32_t *) phys_to_hhdm(g_ep0_ring_phys);
    g_ep_in_ring = (uint32_t *) phys_to_hhdm(g_ep_in_ring_phys);
    g_ctrl_buf = (uint8_t *) phys_to_hhdm(g_ctrl_buf_phys);
    g_irq_buf = (uint8_t *) phys_to_hhdm(g_irq_buf_phys);
    memset_vol(g_irq_buf, 0, g_irq_buf_sz);

    memset_vol(g_dcbaa, 0, 4096);
    memset_vol(g_cmd_ring, 0, 4096);
    memset_vol(g_evt_ring, 0, 4096);
    memset_vol(phys_to_hhdm(g_erst_phys), 0, 4096);
    memset_vol(g_in_ctx, 0, 4096);
    memset_vol(g_out_ctx, 0, 4096);
    memset_vol(g_out_child_ctx, 0, 4096);
    memset_vol(g_ep0_ring, 0, 4096);
    memset_vol(g_ep_in_ring, 0, 4096);

    if (nsp > 0u)
        *(uint64_t *) g_dcbaa = sp_arr_phys;

    {
        uint32_t *erst = (uint32_t *) phys_to_hhdm(g_erst_phys);
        erst[0] = (uint32_t) g_evt_ring_phys;
        erst[1] = (uint32_t) (g_evt_ring_phys >> 32);
        erst[2] = 256u;
        erst[3] = 0u;
    }

    wr32(g_op + 0x38u / 4u, (uint32_t) g_max_slots);

    wr32(g_op + 0x30u / 4u, (uint32_t) g_dcbaa_phys);
    wr32(g_op + 0x34u / 4u, (uint32_t) (g_dcbaa_phys >> 32));

    g_cmd_enq = 0;
    g_cmd_cycle = 1;
    uint32_t *lk = g_cmd_ring + 255u * 4u;
    lk[0] = (uint32_t) g_cmd_ring_phys;
    lk[1] = (uint32_t) (g_cmd_ring_phys >> 32);
    lk[2] = 0;
    dmb();
    lk[3] = TRB_TYPE(TRB_LINK) | TRB_TC | 1u;

    wr32(g_op + 0x18u / 4u, (uint32_t) (g_cmd_ring_phys | 1u));
    wr32(g_op + 0x1cu / 4u, (uint32_t) (g_cmd_ring_phys >> 32));

    volatile uint32_t *intr0 = g_run + 0x20u / 4u;
    wr32(intr0 + (0x28u / 4u), 1u);
    wr32(intr0 + (0x30u / 4u), (uint32_t) g_erst_phys);
    wr32(intr0 + (0x34u / 4u), (uint32_t) (g_erst_phys >> 32));
    wr32(intr0 + (0x38u / 4u), (uint32_t) g_evt_ring_phys | ERDP_EHB);
    wr32(intr0 + (0x3cu / 4u), (uint32_t) (g_evt_ring_phys >> 32));
    wr32(intr0, rd32(intr0) | IMAN_IE);

    g_evt_deq = 0;
    g_evt_cycle = 1;
    g_ep0_enq = 0;
    g_ep0_cycle = 1;
    g_ep_in_cycle = 1;

    wr32(g_op + 0u, rd32(g_op + 0u) | CMD_RUN);
    if (handshake32(g_op + 0x04u / 4u, STS_HALT, 0, 4000000) != 0) {
        kprintf("[usb-mouse] run start failed (USBSTS.HCHalted not cleared)\n");
        free_all();
        return -1;
    }
    kprintf("[usb-mouse] HC running — scanning root ports (max=%u)\n", g_max_ports);

    g_slot_id   = 0;
    g_root_port = 0;
    for (unsigned p = 1; p <= g_max_ports && p <= 32u; p++) {
        volatile uint32_t *ps = (volatile uint32_t *) ((uintptr_t) g_cap + portsc_off(p));
        uint32_t           pv = rd32(ps);
        kprintf("[usb-mouse] PORTSC[%u]=0x%08x CCS=%u PED=%u PR=%u spd=%s\n", p, (unsigned) pv,
                (unsigned) ((pv & PORT_CCS) != 0u), (unsigned) ((pv & PORT_PED) != 0u),
                (unsigned) ((pv & PORT_PR) != 0u), port_speed_name((pv >> PORT_SPEED_SHIFT) & 0xfu));
    }

    for (unsigned p = 1; p <= g_max_ports && p <= 32u; p++) {
        volatile uint32_t *ps = (volatile uint32_t *) ((uintptr_t) g_cap + portsc_off(p));
        if ((rd32(ps) & PORT_CCS) == 0u)
            continue;
        xhci_abort_active_slot();
        if (xhci_enumerate_hid_on_port(p) == 0) {
            g_active = 1;
            kprintf("[usb-mouse] READY: HID mouse slot=%d port=%u ep_idx=%u mps=%u (serial: grep "
                    "[usb-mouse])\n",
                    g_slot_id, g_root_port, g_ep_in_xhci_idx, g_ep_in_mps);
            return 0;
        }
        kprintf("[usb-mouse] port %u: enumeration aborted, trying next CCS port\n", p);
    }

    kprintf("[usb-mouse] FAIL: no HID mouse on root CCS ports — hubs, EHCI-only, and I2C touchpads "
            "need more stack (see ROADMAP); check TRB/PORTSC above\n");
    free_all();
    return -1;
}

int usb_xhci_mouse_init(uint32_t screen_w, uint32_t screen_h) {
    g_sw = screen_w ? screen_w : 1280u;
    g_sh = screen_h ? screen_h : 800u;
    g_irq_buf_sz = 0;
    int any_xhci = 0;
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_device *dev = pci_at(i);
        if (!dev || dev->class_code != 0x0Cu || dev->subclass != 0x03u || dev->prog_if != 0x30u)
            continue;
        any_xhci = 1;
        kprintf("[usb-mouse] try xHCI PCI %02x:%02x.%u %04x:%04x\n", (unsigned) dev->bus, (unsigned) dev->slot,
                (unsigned) dev->func, (unsigned) dev->vendor_id, (unsigned) dev->device_id);
        if (xhci_try_attach(dev) == 0)
            return 0;
    }
    if (!any_xhci)
        kprintf("[usb-mouse] no PCI xHCI\n");
    return -1;
}
