/* Clean xHCI HID boot-protocol mouse driver — single device, root ports only.
 *
 * Bring-up is synchronous from usb_xhci_mouse_init() with a hard time budget
 * (~3 s total) so the boot path cannot hang forever. After init succeeds the
 * Interrupt-IN endpoint is left armed; usb_xhci_mouse_poll() drains the event
 * ring and re-queues a fresh transfer for every report received.
 *
 * Scope on purpose: one xHCI controller at a time, no external hubs, boot
 * protocol only (3-byte report). Keeps the code readable and avoids the
 * topology corner cases that broke the previous async state machine.
 */

#include "usb_xhci_mouse.h"

#include "hid_boot_parse.h"
#include "mouse.h"

#include "../kprintf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../pci/pci.h"

#include <limine.h>

#include <stddef.h>
#include <stdint.h>

extern volatile struct limine_hhdm_request hhdm_request;

/* ------------------------------------------------------------------------- */
/* xHCI register / TRB constants                                              */
/* ------------------------------------------------------------------------- */

enum {
    TRB_NORMAL       = 1,
    TRB_SETUP        = 2,
    TRB_DATA         = 3,
    TRB_STATUS       = 4,
    TRB_LINK         = 6,
    TRB_ENABLE_SLOT  = 9,
    TRB_ADDR_DEV     = 11,
    TRB_CONFIG_EP    = 12,
    TRB_EVAL_CTX     = 13,
    TRB_RESET_EP     = 14,
    TRB_TRANSFER     = 32,
    TRB_COMPLETION   = 33,
    TRB_PORT_STATUS  = 34,
};

#define TRB_TYPE(t)         ((uint32_t)(t) << 10)
#define TRB_CYCLE           (1u << 0)
#define TRB_TC              (1u << 1)
#define TRB_CHAIN           (1u << 4)
#define TRB_IOC             (1u << 5)
#define TRB_IDT             (1u << 6)
#define TRB_DIR_IN          (1u << 16)
#define TRB_TX_TYPE(x)      ((uint32_t)(x) << 16)
#define TRB_LEN(n)          ((uint32_t)(n) & 0x1ffffu)
#define TRB_TD_SIZE(n)      (((uint32_t)((n) < 32u ? (n) : 31u)) << 17)

#define SLOT_FLAG           (1u << 0)
#define EP0_FLAG            (1u << 1)
#define EP_FLAG(n)          (1u << ((n) + 1))

#define LAST_CTX(n)         ((uint32_t)(n) << 27)
#define ROOT_HUB_PORT(p)    (((uint32_t)(p) & 0xffu) << 16)
#define EP_TYPE(t)          ((uint32_t)(t) << 3)
#define CTRL_EP             4u
#define INT_IN_EP           7u
#define MAX_PACKET(p)       (((uint32_t)(p) & 0xffffu) << 16)
#define ERROR_COUNT(p)      (((uint32_t)(p) & 3u) << 1)
#define MAX_BURST(p)        (((uint32_t)(p) & 0xffu) << 8)
#define EP_INTERVAL(p)      (((uint32_t)(p) & 0xffu) << 16)
#define EP_AVG_TRB_LEN(p)   ((uint32_t)(p) & 0xffffu)

#define PORT_CCS            (1u << 0)
#define PORT_PED            (1u << 1)
#define PORT_PR             (1u << 4)
#define PORT_PP             (1u << 9)
#define PORT_WPR            (1u << 31)  /* USB3 warm port reset */
#define PORT_CSC            (1u << 17)
#define PORT_PEC            (1u << 18)
#define PORT_WRC            (1u << 19)
#define PORT_OCC            (1u << 20)
#define PORT_PRC            (1u << 21)
#define PORT_PLC            (1u << 22)
#define PORT_CEC            (1u << 23)
#define PORT_SPEED_SHIFT    10
#define PORT_SPEED_MASK     (0xfu << 10)

/* RW1S bits: writing 1 sets, writing 0 leaves alone (PR/WPR/LWS/PortLinkState). */
#define PORT_RW1S_BITS      ((1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | \
                             (1u << 16) | (1u << 31))
/* RW1C bits: writing 1 clears, writing 0 leaves alone (PED is also RW1C). */
#define PORT_RW1C_BITS      ((1u << 1) | (1u << 17) | (1u << 18) | (1u << 19) | \
                             (1u << 20) | (1u << 21) | (1u << 22) | (1u << 23))

#define CMD_RUN             (1u << 0)
#define CMD_RESET           (1u << 1)
#define CMD_INTE            (1u << 2)
#define STS_HALT            (1u << 0)
#define STS_CNR             (1u << 11)

#define USBLEG_CAP_ID       1u
#define USBLEG_BIOS_OWN     (1u << 16)
#define USBLEG_OS_OWN       (1u << 24)

#define COMP_SUCCESS        1u
#define COMP_SHORT_PACKET   13u

#define HCS1_MAX_SLOTS(p)   ((p) & 0xffu)
#define HCS1_MAX_PORTS(p)   (((p) >> 24) & 0xffu)

#define RING_TRBS           64u   /* per command/transfer ring (incl. LINK) */
#define EVT_TRBS            64u

/* ------------------------------------------------------------------------- */
/* Tiny helpers                                                               */
/* ------------------------------------------------------------------------- */

static void dmb(void) { __asm__ volatile ("mfence" ::: "memory"); }

static uint64_t hhdm(void) {
    return hhdm_request.response ? hhdm_request.response->offset : 0;
}

static void *p2v(uint64_t phys) { return (void *) (phys + hhdm()); }

static uint32_t rd32(const volatile uint32_t *p) { return *p; }
static void     wr32(volatile uint32_t *p, uint32_t v) { *p = v; dmb(); }

static void delay_us(unsigned us) {
    /* ~1 GHz pause loop budget; conservative on real silicon. */
    for (unsigned i = 0; i < us; i++)
        for (volatile unsigned j = 0; j < 200u; j++)
            __asm__ volatile ("pause");
}

static void delay_ms(unsigned ms) { delay_us(ms * 1000u); }

static uint64_t alloc_dma(unsigned pages) {
    uint64_t phys = pages == 1 ? pmm_alloc_page() : pmm_alloc_contig(pages);
    if (!phys) return 0;
    uint8_t *v = (uint8_t *) p2v(phys);
    for (unsigned i = 0; i < pages * 4096u; i++)
        v[i] = 0;
    return phys;
}

static const char *speed_name(unsigned spd) {
    switch (spd) {
    case 1: return "FS";
    case 2: return "LS";
    case 3: return "HS";
    case 4: return "SS";
    case 5: return "SSP";
    default: return "?";
    }
}

static unsigned speed_default_mps(unsigned spd) {
    /* xHCI 4.3.4: initial Max Packet Size for EP0 by speed. */
    switch (spd) {
    case 1: return 8;    /* Full Speed: start with 8, then read bMaxPacketSize0 */
    case 2: return 8;    /* Low Speed */
    case 3: return 64;   /* High Speed */
    case 4:
    case 5: return 512;  /* SS / SSP */
    default: return 8;
    }
}

/* ------------------------------------------------------------------------- */
/* Driver state                                                               */
/* ------------------------------------------------------------------------- */

static volatile uint32_t *g_cap;       /* CAP base               */
static volatile uint32_t *g_op;        /* OP  base = cap + caplen */
static volatile uint32_t *g_run;       /* RUN base = cap + rtsoff */
static volatile uint32_t *g_db;        /* DB  base = cap + dboff  */
static uint32_t            g_ctx_size; /* 32 or 64                */
static unsigned            g_max_slots;
static unsigned            g_max_ports;

static uint64_t            g_dcbaa_phys;
static uint64_t           *g_dcbaa;

static uint64_t            g_cmd_ring_phys;
static uint32_t           *g_cmd_ring;
static unsigned            g_cmd_enq;
static unsigned            g_cmd_cycle;

static uint64_t            g_evt_ring_phys;
static uint32_t           *g_evt_ring;
static unsigned            g_evt_deq;
static unsigned            g_evt_cycle;
static uint64_t            g_erst_phys;

static uint64_t            g_in_ctx_phys;
static uint8_t            *g_in_ctx;

static uint64_t            g_out_ctx_phys;
static uint8_t            *g_out_ctx;

static uint64_t            g_ep0_ring_phys;
static uint32_t           *g_ep0_ring;
static unsigned            g_ep0_enq;
static unsigned            g_ep0_cycle;

static uint64_t            g_int_ring_phys;
static uint32_t           *g_int_ring;
static unsigned            g_int_enq;
static unsigned            g_int_cycle;

static uint64_t            g_ctrl_buf_phys;
static uint8_t            *g_ctrl_buf;

static uint64_t            g_int_buf_phys;
static uint8_t            *g_int_buf;
static unsigned            g_int_mps;

static int                 g_slot_id;
static unsigned            g_root_port;
static unsigned            g_speed;
static uint8_t             g_ep_in_addr;
static uint8_t             g_ep_in_xhci_idx;
static uint8_t             g_ep_in_interval;
static uint8_t             g_iface_num;
static uint8_t             g_iface_alt;
static uint8_t             g_config_value;

static int                 g_active;
static int                 g_began;
static int                 g_scan_done;
static uint32_t            g_sw, g_sh;
static char                g_dbg[96];

#define DIAG_LINES 12u
#define DIAG_COLS  140u
static char     g_diag[DIAG_LINES][DIAG_COLS];
static unsigned g_diag_n;

static void dbg_set(const char *m) {
    unsigned i = 0;
    if (!m) { g_dbg[0] = 0; return; }
    while (i + 1 < sizeof g_dbg && m[i]) { g_dbg[i] = m[i]; i++; }
    g_dbg[i] = 0;
}

static void diag_reset(void) { g_diag_n = 0; for (unsigned i = 0; i < DIAG_LINES; i++) g_diag[i][0] = 0; }

static void diag_pushf_lit(const char *s) {
    if (g_diag_n >= DIAG_LINES) {
        for (unsigned r = 1; r < DIAG_LINES; r++) {
            unsigned c = 0;
            while (c + 1 < DIAG_COLS && g_diag[r][c]) {
                g_diag[r - 1][c] = g_diag[r][c];
                c++;
            }
            g_diag[r - 1][c] = 0;
        }
        g_diag_n = DIAG_LINES - 1;
    }
    char *d = g_diag[g_diag_n];
    unsigned i = 0;
    while (i + 1 < DIAG_COLS && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
    g_diag_n++;
}

/* Tiny formatter — supports %u %x %s only, used to fill diag lines. */
static unsigned u_to_str(unsigned v, unsigned base, char *out) {
    char tmp[16];
    unsigned n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v && n < sizeof tmp) {
        unsigned d = v % base;
        tmp[n++] = (char) (d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    }
    for (unsigned i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

static void diag_pushf(const char *fmt, unsigned a, unsigned b, unsigned c, unsigned d, unsigned e) {
    if (g_diag_n >= DIAG_LINES) return;
    char *o = g_diag[g_diag_n];
    unsigned w = 0;
    unsigned argi = 0;
    unsigned args[5] = { a, b, c, d, e };
    for (unsigned i = 0; fmt[i] && w + 1 < DIAG_COLS; i++) {
        if (fmt[i] != '%') { o[w++] = fmt[i]; continue; }
        char ch = fmt[++i];
        if (!ch) break;
        char buf[16];
        unsigned n = 0;
        if (ch == 'u') n = u_to_str(args[argi++], 10, buf);
        else if (ch == 'x') n = u_to_str(args[argi++], 16, buf);
        else { o[w++] = ch; continue; }
        for (unsigned j = 0; j < n && w + 1 < DIAG_COLS; j++) o[w++] = buf[j];
    }
    o[w] = 0;
    g_diag_n++;
}

/* ------------------------------------------------------------------------- */
/* Register helpers                                                           */
/* ------------------------------------------------------------------------- */

static volatile uint32_t *op_reg(unsigned r)        { return (volatile uint32_t *) ((uintptr_t) g_op + r); }
static volatile uint32_t *portsc(unsigned port1)    { return op_reg(0x400u + (port1 - 1u) * 16u); }
static volatile uint32_t *intr0(unsigned r)         { return (volatile uint32_t *) ((uintptr_t) g_run + 0x20u + r); }
static void portsc_set(unsigned port1, uint32_t set);
static void portsc_ack(unsigned port1, uint32_t bits);

static int handshake(volatile uint32_t *r, uint32_t mask, uint32_t want, unsigned us_budget) {
    for (unsigned i = 0; i < us_budget; i++) {
        uint32_t v = rd32(r);
        if (v == 0xffffffffu) return -1;
        if ((v & mask) == want) return 0;
        delay_us(1);
    }
    return -2;
}

/* ------------------------------------------------------------------------- */
/* Legacy BIOS handoff                                                        */
/* ------------------------------------------------------------------------- */

static void usb_legacy_handoff(void) {
    uint32_t hccparams1 = rd32(g_cap + 0x10u / 4u);
    uint32_t off = (hccparams1 >> 16) & 0xfffcu;
    if (!off) return;

    for (unsigned step = 0; step < 64u && off; step++) {
        volatile uint32_t *w = (volatile uint32_t *) ((uintptr_t) g_cap + off);
        uint32_t v = rd32(w);
        unsigned id   = v & 0xffu;
        unsigned next = (v >> 8) & 0xffu;
        if (id == USBLEG_CAP_ID) {
            if ((v & USBLEG_BIOS_OWN) && !(v & USBLEG_OS_OWN)) {
                wr32(w, v | USBLEG_OS_OWN);
                for (unsigned i = 0; i < 1000u; i++) {
                    v = rd32(w);
                    if (!(v & USBLEG_BIOS_OWN)) break;
                    delay_ms(1);
                }
                kprintf("[usb-mouse] legacy handoff BIOS=%u OS=%u\n",
                        (v & USBLEG_BIOS_OWN) != 0, (v & USBLEG_OS_OWN) != 0);
            }
            /* Disable SMI sources in USBLEGCTLSTS just past USBLEGSUP. */
            volatile uint32_t *ctl = w + 1;
            uint32_t cv = rd32(ctl);
            cv &= ~((1u << 0) | (1u << 4) | (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16));
            cv |= (0xe0000000u);
            wr32(ctl, cv);
            return;
        }
        if (!next) break;
        off += next * 4u;
    }
}

/* ------------------------------------------------------------------------- */
/* Ring helpers                                                               */
/* ------------------------------------------------------------------------- */

static void put_link(uint32_t *ring, unsigned at, uint64_t phys, unsigned cycle) {
    uint32_t *t = ring + at * 4u;
    t[0] = (uint32_t) phys;
    t[1] = (uint32_t) (phys >> 32);
    t[2] = 0;
    dmb();
    t[3] = TRB_TYPE(TRB_LINK) | TRB_TC | (cycle & 1u);
}

static void cmd_push(uint32_t p0, uint32_t p1, uint32_t p2, uint32_t ctl) {
    uint32_t *t = g_cmd_ring + g_cmd_enq * 4u;
    t[0] = p0; t[1] = p1; t[2] = p2;
    dmb();
    t[3] = ctl | (g_cmd_cycle & 1u);
    g_cmd_enq++;
    if (g_cmd_enq >= RING_TRBS - 1u) {
        put_link(g_cmd_ring, RING_TRBS - 1u, g_cmd_ring_phys, g_cmd_cycle);
        g_cmd_enq = 0;
        g_cmd_cycle ^= 1u;
    }
}

static void ep0_push(uint32_t p0, uint32_t p1, uint32_t p2, uint32_t ctl) {
    uint32_t *t = g_ep0_ring + g_ep0_enq * 4u;
    t[0] = p0; t[1] = p1; t[2] = p2;
    dmb();
    t[3] = ctl | (g_ep0_cycle & 1u);
    g_ep0_enq++;
    if (g_ep0_enq >= RING_TRBS - 1u) {
        put_link(g_ep0_ring, RING_TRBS - 1u, g_ep0_ring_phys, g_ep0_cycle);
        g_ep0_enq = 0;
        g_ep0_cycle ^= 1u;
    }
}

static void int_push(uint32_t p0, uint32_t p1, uint32_t p2, uint32_t ctl) {
    uint32_t *t = g_int_ring + g_int_enq * 4u;
    t[0] = p0; t[1] = p1; t[2] = p2;
    dmb();
    t[3] = ctl | (g_int_cycle & 1u);
    g_int_enq++;
    if (g_int_enq >= RING_TRBS - 1u) {
        put_link(g_int_ring, RING_TRBS - 1u, g_int_ring_phys, g_int_cycle);
        g_int_enq = 0;
        g_int_cycle ^= 1u;
    }
}

static void ring_db_host(void) { wr32(g_db + 0u, 0u); }
static void ring_db_ep(unsigned slot, unsigned ep_xhci_idx) {
    wr32(g_db + slot, ep_xhci_idx);
}

/* Update event ring dequeue pointer + clear EHB. */
static void evt_advance(int handled) {
    if (handled) {
        g_evt_deq++;
        if (g_evt_deq >= EVT_TRBS) {
            g_evt_deq = 0;
            g_evt_cycle ^= 1u;
        }
    }
    uint64_t deq = g_evt_ring_phys + g_evt_deq * 16u;
    wr32(intr0(0x18u), (uint32_t) deq | (1u << 3) /* EHB clear */);
    wr32(intr0(0x1cu), (uint32_t) (deq >> 32));
}

/* Wait for one event of given type. Returns completion code (>0) on success
 * or 0xff on timeout. cmd_trb_phys filters by source TRB pointer for command
 * completions; pass 0 to accept any. Same for transfer events. */
static unsigned wait_event(unsigned type, uint64_t source_trb, unsigned us_budget,
                           uint32_t *out_p2, uint32_t *out_p3) {
    for (unsigned t = 0; t < us_budget; t++) {
        uint32_t *e = g_evt_ring + g_evt_deq * 4u;
        uint32_t c = e[3];
        if ((c & 1u) == g_evt_cycle) {
            unsigned ty = (c >> 10) & 0x3fu;
            uint64_t src = ((uint64_t) e[1] << 32) | e[0];
            if (ty == type && (source_trb == 0 || src == source_trb)) {
                unsigned cc = (e[2] >> 24) & 0xffu;
                if (out_p2) *out_p2 = e[2];
                if (out_p3) *out_p3 = e[3];
                evt_advance(1);
                return cc ? cc : 0xfeu;
            }
            /* Other event we don't care about — drop and continue. */
            evt_advance(1);
            continue;
        }
        delay_us(1);
    }
    return 0xffu;
}

/* ------------------------------------------------------------------------- */
/* Controller bring-up                                                        */
/* ------------------------------------------------------------------------- */

static int xhci_pick_bar(const struct pci_device *d, uint64_t *phys, uint64_t *size) {
    for (int i = 0; i < PCI_BAR_COUNT; i++) {
        if (d->bars[i].type != PCI_BAR_MEM) continue;
        if (d->bars[i].size < 0x1000ull) continue;
        if (d->bars[i].base == 0) continue;
        *phys = d->bars[i].base;
        *size = d->bars[i].size;
        return 0;
    }
    return -1;
}

static void free_all_state(void) {
    g_cap = g_op = g_run = g_db = NULL;
    g_dcbaa = NULL; g_dcbaa_phys = 0;
    g_cmd_ring = NULL; g_cmd_ring_phys = 0;
    g_evt_ring = NULL; g_evt_ring_phys = 0;
    g_erst_phys = 0;
    g_in_ctx = NULL; g_in_ctx_phys = 0;
    g_out_ctx = NULL; g_out_ctx_phys = 0;
    g_ep0_ring = NULL; g_ep0_ring_phys = 0;
    g_int_ring = NULL; g_int_ring_phys = 0;
    g_ctrl_buf = NULL; g_ctrl_buf_phys = 0;
    g_int_buf = NULL; g_int_buf_phys = 0;
    g_slot_id = 0;
}

static int xhci_bringup(const struct pci_device *d) {
    free_all_state();

    uint64_t bar_phys, bar_size;
    if (xhci_pick_bar(d, &bar_phys, &bar_size) != 0) {
        kprintf("[usb-mouse] xhci: no MMIO BAR\n");
        return -1;
    }
    pci_enable_mmio_bus_master(d);

    uint64_t bar_virt = bar_phys + hhdm();
    mmio_map(bar_virt, bar_phys, bar_size);
    g_cap = (volatile uint32_t *) bar_virt;

    uint32_t caplen = rd32(g_cap) & 0xffu;
    uint32_t hcs1   = rd32(g_cap + 1u);
    uint32_t hcs2   = rd32(g_cap + 2u);
    uint32_t hccp1  = rd32(g_cap + 4u);
    uint32_t dboff  = rd32(g_cap + 5u) & ~0x3u;
    uint32_t rtsoff = rd32(g_cap + 6u) & ~0x1fu;

    g_op  = (volatile uint32_t *) (bar_virt + caplen);
    g_run = (volatile uint32_t *) (bar_virt + rtsoff);
    g_db  = (volatile uint32_t *) (bar_virt + dboff);

    g_max_slots = HCS1_MAX_SLOTS(hcs1);
    g_max_ports = HCS1_MAX_PORTS(hcs1);
    g_ctx_size  = (hccp1 & (1u << 2)) ? 64u : 32u;

    kprintf("[usb-mouse] xhci @ %p caplen=%u slots=%u ports=%u ctx=%u\n",
            (void *) bar_virt, caplen, g_max_slots, g_max_ports, g_ctx_size);
    diag_pushf("xhci ports=%u slots=%u ctx=%u", g_max_ports, g_max_slots, g_ctx_size, 0, 0);

    usb_legacy_handoff();

    /* Halt + reset. */
    wr32(op_reg(0x00u), rd32(op_reg(0x00u)) & ~CMD_RUN);
    if (handshake(op_reg(0x04u), STS_HALT, STS_HALT, 200000u) != 0)
        kprintf("[usb-mouse] warn: HC did not halt\n");

    wr32(op_reg(0x00u), CMD_RESET);
    if (handshake(op_reg(0x00u), CMD_RESET, 0, 1000000u) != 0) {
        kprintf("[usb-mouse] HC reset timeout\n");
        return -1;
    }
    if (handshake(op_reg(0x04u), STS_CNR, 0, 1000000u) != 0) {
        kprintf("[usb-mouse] HC not ready (CNR)\n");
        return -1;
    }

    wr32(op_reg(0x38u), g_max_slots);  /* CONFIG.MaxSlotsEn */

    /* DCBAA. */
    g_dcbaa_phys = alloc_dma(1);
    if (!g_dcbaa_phys) return -1;
    g_dcbaa = (uint64_t *) p2v(g_dcbaa_phys);

    /* Scratchpad buffers if controller wants any. */
    unsigned spbufs = ((hcs2 >> 27) & 0x1fu) | (((hcs2 >> 21) & 0x1fu) << 5);
    if (spbufs) {
        uint64_t arr_phys = alloc_dma(1);
        uint64_t *arr = (uint64_t *) p2v(arr_phys);
        for (unsigned i = 0; i < spbufs; i++) {
            uint64_t pg = alloc_dma(1);
            arr[i] = pg;
        }
        g_dcbaa[0] = arr_phys;
    }
    wr32(op_reg(0x30u), (uint32_t) g_dcbaa_phys);
    wr32(op_reg(0x34u), (uint32_t) (g_dcbaa_phys >> 32));

    /* Command ring. */
    g_cmd_ring_phys = alloc_dma(1);
    g_cmd_ring = (uint32_t *) p2v(g_cmd_ring_phys);
    g_cmd_enq = 0;
    g_cmd_cycle = 1;
    wr32(op_reg(0x18u), (uint32_t) g_cmd_ring_phys | 1u);
    wr32(op_reg(0x1cu), (uint32_t) (g_cmd_ring_phys >> 32));

    /* Event ring + ERST. */
    g_evt_ring_phys = alloc_dma(1);
    g_evt_ring = (uint32_t *) p2v(g_evt_ring_phys);
    g_evt_deq = 0;
    g_evt_cycle = 1;
    g_erst_phys = alloc_dma(1);
    {
        uint32_t *e = (uint32_t *) p2v(g_erst_phys);
        e[0] = (uint32_t) g_evt_ring_phys;
        e[1] = (uint32_t) (g_evt_ring_phys >> 32);
        e[2] = EVT_TRBS;
        e[3] = 0;
    }
    wr32(intr0(0x08u), 1u);  /* ERSTSZ                                   */
    wr32(intr0(0x10u), (uint32_t) g_erst_phys);
    wr32(intr0(0x14u), (uint32_t) (g_erst_phys >> 32));
    wr32(intr0(0x18u), (uint32_t) g_evt_ring_phys);
    wr32(intr0(0x1cu), (uint32_t) (g_evt_ring_phys >> 32));

    /* Run + interrupts (we poll, but keep INTE for safety). */
    wr32(op_reg(0x00u), CMD_RUN);
    if (handshake(op_reg(0x04u), STS_HALT, 0, 200000u) != 0) {
        kprintf("[usb-mouse] HC did not start running\n");
        return -1;
    }

    /* Power up every port (PP defaults to 0 on most chipsets — Linux does the
     * same). Real silicon needs a few hundred ms after PP=1 before CCS is
     * stable on USB2 ports; USB3 SS is faster but still not instant. */
    for (unsigned p = 1; p <= g_max_ports; p++) {
        uint32_t v = rd32(portsc(p));
        if (!(v & PORT_PP)) {
            portsc_set(p, PORT_PP);
        }
    }
    delay_ms(200);

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Port reset + slot enable                                                   */
/* ------------------------------------------------------------------------- */

/* Read-modify-write helper: keep RO/RW bits, clear all RW1C change bits we
 * don't want to ack, then OR in `set`. */
static void portsc_set(unsigned port1, uint32_t set) {
    uint32_t v = rd32(portsc(port1));
    /* Keep neutral state: clear RW1S/RW1C fields from the read value, then
     * write only what we intentionally set. */
    wr32(portsc(port1), (v & ~(PORT_RW1S_BITS | PORT_RW1C_BITS)) | set);
}

static void portsc_ack(unsigned port1, uint32_t bits) {
    portsc_set(port1, bits & PORT_RW1C_BITS);
}

/* Issue a port reset and wait for the controller to assert PRC (USB2) or
 * WRC (USB3 warm reset). Returns 0 on success with PED+CCS set. */
static int port_reset(unsigned port1) {
    uint32_t v = rd32(portsc(port1));
    if (!(v & PORT_CCS)) return -1;

    unsigned spd = (v >> PORT_SPEED_SHIFT) & 0xfu;
    int is_ss = (spd == 4 || spd == 5);

    /* Clear stale change bits before we kick PR. */
    portsc_ack(port1, PORT_CSC | PORT_PEC | PORT_PRC | PORT_WRC);

    if (is_ss)
        portsc_set(port1, PORT_WPR);
    else
        portsc_set(port1, PORT_PR);

    /* Some AMD chipsets do not always assert PRC/WRC reliably when firmware
     * already touched the port. Accept either:
     *  (1) expected change bit set, OR
     *  (2) reset bit deasserted + PED asserted.
     */
    uint32_t want_change = is_ss ? PORT_WRC : PORT_PRC;
    uint32_t reset_bit   = is_ss ? PORT_WPR : PORT_PR;
    int done = 0;
    for (unsigned i = 0; i < 1200u; i++) {
        v = rd32(portsc(port1));
        if (v & want_change) {
            done = 1;
            break;
        }
        if (((v & reset_bit) == 0) && (v & PORT_PED)) {
            done = 1;
            break;
        }
        delay_ms(1);
    }
    if (!done) {
        kprintf("[usb-mouse] port %u: reset timeout (PORTSC=0x%08x)\n", port1, v);
        return -1;
    }

    portsc_ack(port1, want_change | PORT_CSC | PORT_PEC | PORT_PLC);
    delay_ms(25);

    v = rd32(portsc(port1));
    if (!(v & PORT_CCS)) {
        kprintf("[usb-mouse] port %u: lost CCS after reset (PORTSC=0x%08x)\n", port1, v);
        return -1;
    }
    if (!(v & PORT_PED)) {
        /* Some USB2 LS/FS devices need a tiny extra wait. */
        for (unsigned i = 0; i < 200u; i++) {
            delay_ms(1);
            v = rd32(portsc(port1));
            if (v & PORT_PED) break;
        }
    }
    if (!(v & PORT_PED)) {
        kprintf("[usb-mouse] port %u: not enabled after reset (PORTSC=0x%08x)\n", port1, v);
        return -1;
    }
    return 0;
}

static int cmd_enable_slot(uint8_t *out_slot) {
    uint64_t trb_phys = g_cmd_ring_phys + g_cmd_enq * 16u;
    cmd_push(0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    ring_db_host();
    uint32_t p2 = 0, p3 = 0;
    unsigned cc = wait_event(TRB_COMPLETION, trb_phys, 500000u, &p2, &p3);
    if (cc != COMP_SUCCESS) {
        kprintf("[usb-mouse] EnableSlot cc=%u\n", cc);
        return -1;
    }
    *out_slot = (p3 >> 24) & 0xffu;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Device context / control endpoint                                          */
/* ------------------------------------------------------------------------- */

static uint8_t *slot_ctx(void)        { return g_in_ctx + g_ctx_size; }
static uint8_t *ep_ctx(unsigned n)    { return g_in_ctx + g_ctx_size * (n + 1u); }
static uint32_t *u32(uint8_t *p)      { return (uint32_t *) p; }

static int alloc_device_ctx(void) {
    g_in_ctx_phys  = alloc_dma(1);
    g_out_ctx_phys = alloc_dma(1);
    if (!g_in_ctx_phys || !g_out_ctx_phys) return -1;
    g_in_ctx  = (uint8_t *) p2v(g_in_ctx_phys);
    g_out_ctx = (uint8_t *) p2v(g_out_ctx_phys);

    g_ep0_ring_phys = alloc_dma(1);
    g_int_ring_phys = alloc_dma(1);
    if (!g_ep0_ring_phys || !g_int_ring_phys) return -1;
    g_ep0_ring = (uint32_t *) p2v(g_ep0_ring_phys);
    g_int_ring = (uint32_t *) p2v(g_int_ring_phys);
    g_ep0_enq = 0; g_ep0_cycle = 1;
    g_int_enq = 0; g_int_cycle = 1;

    g_ctrl_buf_phys = alloc_dma(1);
    g_int_buf_phys  = alloc_dma(1);
    if (!g_ctrl_buf_phys || !g_int_buf_phys) return -1;
    g_ctrl_buf = (uint8_t *) p2v(g_ctrl_buf_phys);
    g_int_buf  = (uint8_t *) p2v(g_int_buf_phys);
    return 0;
}

static void program_slot_and_ep0(unsigned mps) {
    /* Zero input context. */
    for (unsigned i = 0; i < 4096u; i++) g_in_ctx[i] = 0;

    uint32_t *ic = u32(g_in_ctx);
    ic[0] = 0;
    ic[1] = SLOT_FLAG | EP0_FLAG;

    uint32_t *sc = u32(slot_ctx());
    sc[0] = LAST_CTX(1) | ((uint32_t) g_speed << 20);
    sc[1] = ROOT_HUB_PORT(g_root_port);

    uint32_t *e0 = u32(ep_ctx(0));
    e0[1] = EP_TYPE(CTRL_EP) | MAX_PACKET(mps) | ERROR_COUNT(3);
    e0[2] = (uint32_t) g_ep0_ring_phys | 1u;
    e0[3] = (uint32_t) (g_ep0_ring_phys >> 32);
    e0[4] = EP_AVG_TRB_LEN(8);
}

static int cmd_address_device(uint8_t slot, int bsr) {
    g_dcbaa[slot] = g_out_ctx_phys;
    uint64_t trb_phys = g_cmd_ring_phys + g_cmd_enq * 16u;
    cmd_push((uint32_t) g_in_ctx_phys, (uint32_t) (g_in_ctx_phys >> 32), 0,
             TRB_TYPE(TRB_ADDR_DEV) | ((uint32_t) slot << 24) | (bsr ? (1u << 9) : 0));
    ring_db_host();
    unsigned cc = wait_event(TRB_COMPLETION, trb_phys, 500000u, NULL, NULL);
    if (cc != COMP_SUCCESS) {
        kprintf("[usb-mouse] AddressDevice%s cc=%u\n", bsr ? "(BSR)" : "", cc);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Control transfers                                                          */
/* ------------------------------------------------------------------------- */

static int control_xfer(uint8_t bmReq, uint8_t bReq, uint16_t wValue,
                        uint16_t wIndex, uint16_t wLength, int dir_in,
                        uint8_t *data) {
    /* SETUP. */
    uint32_t setup_lo = ((uint32_t) bmReq) | ((uint32_t) bReq << 8) | ((uint32_t) wValue << 16);
    uint32_t setup_hi = ((uint32_t) wIndex) | ((uint32_t) wLength << 16);
    uint32_t setup_p2 = TRB_LEN(8);
    uint32_t setup_p3 = TRB_TYPE(TRB_SETUP) | TRB_IDT |
                        TRB_TX_TYPE(wLength == 0 ? 0u : (dir_in ? 3u : 2u));
    ep0_push(setup_lo, setup_hi, setup_p2, setup_p3);

    /* DATA. */
    if (wLength) {
        if (dir_in) {
            for (uint16_t i = 0; i < wLength; i++) g_ctrl_buf[i] = 0;
        } else if (data) {
            for (uint16_t i = 0; i < wLength; i++) g_ctrl_buf[i] = data[i];
        }
        ep0_push((uint32_t) g_ctrl_buf_phys, (uint32_t) (g_ctrl_buf_phys >> 32),
                 TRB_LEN(wLength) | TRB_TD_SIZE(0),
                 TRB_TYPE(TRB_DATA) | (dir_in ? TRB_DIR_IN : 0));
    }

    /* STATUS (opposite direction; IOC). */
    int status_in = wLength == 0 ? 1 : (dir_in ? 0 : 1);
    uint64_t status_trb = g_ep0_ring_phys + g_ep0_enq * 16u;
    ep0_push(0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (status_in ? TRB_DIR_IN : 0));

    ring_db_ep(g_slot_id, 1u);
    unsigned cc = wait_event(TRB_TRANSFER, status_trb, 500000u, NULL, NULL);
    if (cc != COMP_SUCCESS && cc != COMP_SHORT_PACKET) {
        kprintf("[usb-mouse] ctrl bmReq=%02x bReq=%02x wVal=%04x cc=%u\n",
                bmReq, bReq, wValue, cc);
        return -1;
    }
    if (dir_in && data && wLength) {
        for (uint16_t i = 0; i < wLength; i++) data[i] = g_ctrl_buf[i];
    }
    return 0;
}

static int get_descriptor(uint8_t type, uint8_t index, uint16_t lang,
                          uint8_t *buf, uint16_t len) {
    return control_xfer(0x80u, 0x06u, ((uint16_t) type << 8) | index, lang, len, 1, buf);
}

static int set_address_followup_set_config(uint8_t cfg) {
    return control_xfer(0x00u, 0x09u, cfg, 0, 0, 0, NULL);
}

static int set_interface(uint8_t iface, uint8_t alt) {
    return control_xfer(0x01u, 0x0bu, alt, iface, 0, 0, NULL);
}

static int hid_set_protocol_boot(uint8_t iface) {
    /* SET_PROTOCOL=0 (boot). Class request to interface. */
    return control_xfer(0x21u, 0x0bu, 0, iface, 0, 0, NULL);
}

static int hid_set_idle(uint8_t iface) {
    /* SET_IDLE 0/0 — report only on change. */
    return control_xfer(0x21u, 0x0au, 0, iface, 0, 0, NULL);
}

/* ------------------------------------------------------------------------- */
/* Configuration parsing — find first HID interface with INT IN              */
/* ------------------------------------------------------------------------- */

static int parse_config(const uint8_t *cfg, unsigned len) {
    if (len < 9) return -1;
    g_config_value = cfg[5];

    /* Best candidate: HID boot mouse. Fall back to any HID INT-IN. */
    int found_boot = 0;
    int found_any  = 0;
    uint8_t cur_iface = 0xffu, cur_alt = 0;
    int cur_is_hid_mouse = 0;
    int cur_is_hid_any   = 0;

    unsigned i = 9;
    while (i + 2 <= len) {
        unsigned dlen = cfg[i];
        unsigned dty  = cfg[i + 1];
        if (dlen < 2 || i + dlen > len) break;

        if (dty == 0x04 && dlen >= 9) {
            cur_iface = cfg[i + 2];
            cur_alt   = cfg[i + 3];
            unsigned class_  = cfg[i + 5];
            unsigned subcls  = cfg[i + 6];
            unsigned proto   = cfg[i + 7];
            cur_is_hid_any   = (class_ == 0x03);
            cur_is_hid_mouse = (class_ == 0x03 && subcls == 0x01 && proto == 0x02);
        } else if (dty == 0x05 && dlen >= 7) {
            uint8_t addr     = cfg[i + 2];
            uint8_t attr     = cfg[i + 3];
            uint16_t mps     = (uint16_t) cfg[i + 4] | ((uint16_t) cfg[i + 5] << 8);
            uint8_t interval = cfg[i + 6];
            int is_int = (attr & 0x03) == 0x03;
            int is_in  = (addr & 0x80) != 0;
            if (is_int && is_in) {
                if (cur_is_hid_mouse && !found_boot) {
                    g_iface_num         = cur_iface;
                    g_iface_alt         = cur_alt;
                    g_ep_in_addr        = addr;
                    g_ep_in_xhci_idx    = ((addr & 0x0fu) * 2u) + 1u;
                    g_int_mps           = mps & 0x07ffu;
                    g_ep_in_interval    = interval;
                    found_boot = found_any = 1;
                } else if (cur_is_hid_any && !found_any) {
                    g_iface_num         = cur_iface;
                    g_iface_alt         = cur_alt;
                    g_ep_in_addr        = addr;
                    g_ep_in_xhci_idx    = ((addr & 0x0fu) * 2u) + 1u;
                    g_int_mps           = mps & 0x07ffu;
                    g_ep_in_interval    = interval;
                    found_any = 1;
                }
            }
        }
        i += dlen;
    }
    return found_any ? 0 : -1;
}

/* ------------------------------------------------------------------------- */
/* Endpoint configuration                                                     */
/* ------------------------------------------------------------------------- */

static unsigned compute_interval_exp(unsigned interval, unsigned speed) {
    /* xHCI Table 6-12: returns Interval field (3-bit exponent).
     * LS/FS interrupt: bInterval is in frames (1..255). xHCI wants log2(8 * bInterval).
     * HS/SS interrupt: bInterval is already an exponent (1..16); xHCI wants bInterval-1.
     */
    if (speed == 1 || speed == 2) {
        unsigned x = interval ? interval : 10;
        unsigned val = x * 8u;
        unsigned e = 0;
        while ((1u << e) < val && e < 15u) e++;
        return e + 3u;  /* see Linux xhci_parse_endpoint */
    }
    unsigned x = interval ? interval : 4;
    return (x > 0 ? x - 1u : 0u);
}

static int configure_int_in_ep(void) {
    for (unsigned i = 0; i < 4096u; i++) g_in_ctx[i] = 0;

    uint32_t *ic = u32(g_in_ctx);
    ic[0] = 0;
    ic[1] = SLOT_FLAG | EP_FLAG(g_ep_in_xhci_idx - 1u);

    uint32_t *sc = u32(slot_ctx());
    sc[0] = LAST_CTX(g_ep_in_xhci_idx) | ((uint32_t) g_speed << 20);
    sc[1] = ROOT_HUB_PORT(g_root_port);

    unsigned ie = compute_interval_exp(g_ep_in_interval, g_speed);
    uint32_t *ep = u32(ep_ctx(g_ep_in_xhci_idx - 1u));
    ep[0] = EP_INTERVAL(ie);
    ep[1] = EP_TYPE(INT_IN_EP) | MAX_PACKET(g_int_mps) | ERROR_COUNT(3) | MAX_BURST(0);
    ep[2] = (uint32_t) g_int_ring_phys | 1u;
    ep[3] = (uint32_t) (g_int_ring_phys >> 32);
    ep[4] = EP_AVG_TRB_LEN(8);

    uint64_t trb_phys = g_cmd_ring_phys + g_cmd_enq * 16u;
    cmd_push((uint32_t) g_in_ctx_phys, (uint32_t) (g_in_ctx_phys >> 32), 0,
             TRB_TYPE(TRB_CONFIG_EP) | ((uint32_t) g_slot_id << 24));
    ring_db_host();
    unsigned cc = wait_event(TRB_COMPLETION, trb_phys, 500000u, NULL, NULL);
    if (cc != COMP_SUCCESS) {
        kprintf("[usb-mouse] ConfigureEndpoint cc=%u\n", cc);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Interrupt-IN: queue + drain                                               */
/* ------------------------------------------------------------------------- */

static void int_arm_one(void) {
    /* Always (re)post a single Normal TRB targeted at our int_buf. */
    int_push((uint32_t) g_int_buf_phys, (uint32_t) (g_int_buf_phys >> 32),
             TRB_LEN(g_int_mps) | TRB_TD_SIZE(0),
             TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_db_ep(g_slot_id, g_ep_in_xhci_idx);
}

/* ------------------------------------------------------------------------- */
/* Probe one root port                                                        */
/* ------------------------------------------------------------------------- */

static int probe_port(unsigned port1) {
    if (port_reset(port1) != 0) {
        diag_pushf("port %u reset failed", port1, 0, 0, 0, 0);
        return -1;
    }

    uint32_t pv = rd32(portsc(port1));
    g_speed = (pv >> PORT_SPEED_SHIFT) & 0xfu;
    g_root_port = port1;
    kprintf("[usb-mouse] port %u up: speed=%s PORTSC=0x%08x\n",
            port1, speed_name(g_speed), pv);

    uint8_t slot = 0;
    if (cmd_enable_slot(&slot) != 0) {
        kprintf("[usb-mouse] port %u: EnableSlot failed\n", port1);
        diag_pushf("port %u: EnableSlot failed", port1, 0, 0, 0, 0);
        return -1;
    }
    g_slot_id = slot;
    kprintf("[usb-mouse] port %u: slot=%u\n", port1, slot);

    if (alloc_device_ctx() != 0) {
        kprintf("[usb-mouse] port %u: ctx alloc failed\n", port1);
        diag_pushf("port %u: ctx alloc failed", port1, 0, 0, 0, 0);
        return -1;
    }

    unsigned mps = speed_default_mps(g_speed);
    program_slot_and_ep0(mps);
    if (cmd_address_device(slot, 1) != 0) {
        kprintf("[usb-mouse] port %u: AddressDevice(BSR) failed\n", port1);
        diag_pushf("port %u: Address(BSR) failed", port1, 0, 0, 0, 0);
        return -1;
    }

    /* For Full Speed devices, EP0 actual max packet size is in dev desc[7]. */
    uint8_t devdesc[18];
    if (get_descriptor(0x01, 0, 0, devdesc, 8) != 0) {
        kprintf("[usb-mouse] port %u: GET_DESCRIPTOR(dev,8) failed\n", port1);
        diag_pushf("port %u: dev desc 8 failed", port1, 0, 0, 0, 0);
        return -1;
    }
    unsigned real_mps = devdesc[7];
    if (g_speed == 4 || g_speed == 5) real_mps = 1u << real_mps;
    if (!real_mps) real_mps = mps;

    if (real_mps != mps) {
        /* Update EP0 MaxPacket via Evaluate Context. */
        for (unsigned i = 0; i < 4096u; i++) g_in_ctx[i] = 0;
        uint32_t *ic = u32(g_in_ctx);
        ic[1] = EP0_FLAG;
        uint32_t *e0 = u32(ep_ctx(0));
        e0[1] = EP_TYPE(CTRL_EP) | MAX_PACKET(real_mps) | ERROR_COUNT(3);

        uint64_t trb_phys = g_cmd_ring_phys + g_cmd_enq * 16u;
        cmd_push((uint32_t) g_in_ctx_phys, (uint32_t) (g_in_ctx_phys >> 32), 0,
                 TRB_TYPE(TRB_EVAL_CTX) | ((uint32_t) slot << 24));
        ring_db_host();
        wait_event(TRB_COMPLETION, trb_phys, 500000u, NULL, NULL);
    }

    /* Issue real Address Device (no BSR). */
    {
        for (unsigned i = 0; i < 4096u; i++) g_in_ctx[i] = 0;
        uint32_t *ic = u32(g_in_ctx);
        ic[1] = SLOT_FLAG | EP0_FLAG;
        uint32_t *sc = u32(slot_ctx());
        sc[0] = LAST_CTX(1) | ((uint32_t) g_speed << 20);
        sc[1] = ROOT_HUB_PORT(g_root_port);
        uint32_t *e0 = u32(ep_ctx(0));
        e0[1] = EP_TYPE(CTRL_EP) | MAX_PACKET(real_mps) | ERROR_COUNT(3);
        e0[2] = (uint32_t) g_ep0_ring_phys | (g_ep0_cycle & 1u);
        e0[3] = (uint32_t) (g_ep0_ring_phys >> 32);
        e0[4] = EP_AVG_TRB_LEN(8);

        if (cmd_address_device(slot, 0) != 0) {
            kprintf("[usb-mouse] port %u: AddressDevice failed\n", port1);
            diag_pushf("port %u: Address failed", port1, 0, 0, 0, 0);
            return -1;
        }
    }

    if (get_descriptor(0x01, 0, 0, devdesc, 18) != 0) {
        kprintf("[usb-mouse] port %u: GET_DESCRIPTOR(dev,18) failed\n", port1);
        diag_pushf("port %u: dev desc 18 failed", port1, 0, 0, 0, 0);
        return -1;
    }
    kprintf("[usb-mouse] port %u: dev VID=%04x PID=%04x class=%u\n",
            port1, devdesc[8] | (devdesc[9] << 8),
            devdesc[10] | (devdesc[11] << 8), devdesc[4]);

    /* Read config descriptor (header first, then full). */
    uint8_t cfgh[9];
    if (get_descriptor(0x02, 0, 0, cfgh, 9) != 0) {
        kprintf("[usb-mouse] port %u: GET_DESCRIPTOR(cfg,9) failed\n", port1);
        diag_pushf("port %u: cfg hdr failed", port1, 0, 0, 0, 0);
        return -1;
    }
    unsigned total = (unsigned) cfgh[2] | ((unsigned) cfgh[3] << 8);
    if (total < 9 || total > 4095) {
        kprintf("[usb-mouse] port %u: bad cfg total=%u\n", port1, total);
        diag_pushf("port %u: bad cfg total=%u", port1, total, 0, 0, 0);
        return -1;
    }

    uint8_t *cfgbuf = (uint8_t *) p2v(alloc_dma(1));
    if (!cfgbuf) {
        diag_pushf("port %u: cfg alloc failed", port1, 0, 0, 0, 0);
        return -1;
    }
    if (get_descriptor(0x02, 0, 0, cfgbuf, total) != 0) {
        kprintf("[usb-mouse] port %u: GET_DESCRIPTOR(cfg,%u) failed\n", port1, total);
        diag_pushf("port %u: cfg desc %u failed", port1, total, 0, 0, 0);
        return -1;
    }

    if (parse_config(cfgbuf, total) != 0) {
        kprintf("[usb-mouse] port %u: device has no HID INT-IN endpoint "
                "(class=%u) — probably a hub or non-HID device\n",
                port1, devdesc[4]);
        diag_pushf("port %u: not HID (class=%u VID=%x PID=%x)",
                   port1, devdesc[4], devdesc[8] | (devdesc[9] << 8),
                   devdesc[10] | (devdesc[11] << 8), 0);
        return -1;
    }
    kprintf("[usb-mouse] port %u: HID iface=%u alt=%u ep=%02x mps=%u interval=%u\n",
            port1, g_iface_num, g_iface_alt, g_ep_in_addr, g_int_mps, g_ep_in_interval);

    if (set_address_followup_set_config(g_config_value) != 0) {
        kprintf("[usb-mouse] port %u: SET_CONFIGURATION(%u) failed\n", port1, g_config_value);
        diag_pushf("port %u: SET_CONFIG(%u) failed", port1, g_config_value, 0, 0, 0);
        return -1;
    }
    if (g_iface_alt) (void) set_interface(g_iface_num, g_iface_alt);
    (void) hid_set_protocol_boot(g_iface_num);
    (void) hid_set_idle(g_iface_num);

    if (configure_int_in_ep() != 0) {
        kprintf("[usb-mouse] port %u: ConfigureEndpoint failed\n", port1);
        diag_pushf("port %u: ConfigureEP failed", port1, 0, 0, 0, 0);
        return -1;
    }

    int_arm_one();

    kprintf("[usb-mouse] active: port=%u speed=%s slot=%u iface=%u ep=%02x mps=%u\n",
            port1, speed_name(g_speed), slot, g_iface_num, g_ep_in_addr, g_int_mps);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Top-level init / poll                                                      */
/* ------------------------------------------------------------------------- */

/* Wait up to `budget_ms` for at least one root port to assert CCS (a device
 * has been detected). Returns the number of CCS-asserted ports. Logs each
 * port's PORTSC and writes a compact summary to the on-screen diag area. */
static unsigned wait_for_any_ccs(unsigned budget_ms) {
    unsigned ccs = 0;
    for (unsigned t = 0; t < budget_ms; t++) {
        ccs = 0;
        for (unsigned p = 1; p <= g_max_ports; p++) {
            if (rd32(portsc(p)) & PORT_CCS) ccs++;
        }
        if (ccs) break;
        delay_ms(1);
    }
    /* On-screen: list ports as "p1=ccs/spd p2=ccs/spd ..." capped to one line. */
    char line[DIAG_COLS];
    unsigned w = 0;
    const char *prefix = "ports: ";
    for (unsigned i = 0; prefix[i] && w + 1 < DIAG_COLS; i++) line[w++] = prefix[i];
    for (unsigned p = 1; p <= g_max_ports && w + 12 < DIAG_COLS; p++) {
        uint32_t v = rd32(portsc(p));
        unsigned spd = (v >> PORT_SPEED_SHIFT) & 0xfu;
        char num[8]; unsigned n = u_to_str(p, 10, num);
        for (unsigned i = 0; i < n; i++) line[w++] = num[i];
        line[w++] = '=';
        line[w++] = (v & PORT_CCS) ? 'C' : '.';
        line[w++] = (v & PORT_PED) ? 'E' : '.';
        line[w++] = (v & PORT_PP)  ? 'P' : '.';
        line[w++] = '/';
        line[w++] = spd ? speed_name(spd)[0] : '-';
        line[w++] = ' ';
    }
    line[w] = 0;
    diag_pushf_lit(line);

    for (unsigned p = 1; p <= g_max_ports; p++) {
        uint32_t v = rd32(portsc(p));
        unsigned spd = (v >> PORT_SPEED_SHIFT) & 0xfu;
        kprintf("[usb-mouse] port %u: PORTSC=0x%08x CCS=%u PED=%u PP=%u speed=%s\n",
                p, v, (v & PORT_CCS) != 0, (v & PORT_PED) != 0,
                (v & PORT_PP) != 0, spd ? speed_name(spd) : "-");
    }
    return ccs;
}

static int try_controller(struct pci_device *d) {
    g_began = 1;
    if (xhci_bringup(d) != 0) return -1;
    dbg_set("usb: HC up, waiting for ports");

    unsigned ccs = wait_for_any_ccs(1500u);
    kprintf("[usb-mouse] %u of %u root ports report a connected device\n",
            ccs, g_max_ports);

    if (!ccs) {
        dbg_set("usb: no ports show CCS");
        return -1;
    }

    dbg_set("usb: enumerating ports");
    for (unsigned p = 1; p <= g_max_ports; p++) {
        uint32_t pv = rd32(portsc(p));
        if (!(pv & PORT_CCS)) continue;
        kprintf("[usb-mouse] enumerating port %u\n", p);
        diag_pushf("enum port %u speed=%u", p, (pv >> PORT_SPEED_SHIFT) & 0xfu, 0, 0, 0);
        if (probe_port(p) == 0) {
            g_active = 1;
            dbg_set("usb: HID mouse active");
            diag_pushf("ACTIVE port %u slot %u ep %x", p, (unsigned) g_slot_id, g_ep_in_addr, 0, 0);
            return 0;
        }
        if (g_slot_id) {
            g_dcbaa[g_slot_id] = 0;
            g_slot_id = 0;
        }
    }
    return -1;
}

int usb_xhci_mouse_init(uint32_t screen_w, uint32_t screen_h) {
    g_sw = screen_w;
    g_sh = screen_h;
    g_active = 0;
    g_began  = 0;
    g_scan_done = 0;
    diag_reset();
    dbg_set("usb: probing");

    unsigned tried = 0;
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_device *d = pci_at(i);
        if (!d) continue;
        if (d->class_code == 0x0c && d->subclass == 0x03 && d->prog_if == 0x30) {
            tried++;
            kprintf("[usb-mouse] try xHCI %02x:%02x.%u %04x:%04x\n",
                    d->bus, d->slot, d->func, d->vendor_id, d->device_id);
            diag_pushf("xHCI #%u %x:%x VID:DID %x:%x", tried,
                       (unsigned) d->bus, (unsigned) d->slot,
                       d->vendor_id, d->device_id);
            if (try_controller(d) == 0) {
                g_scan_done = 1;
                return 0;
            }
        }
    }
    g_scan_done = 1;
    if (!tried) {
        dbg_set("usb: no xHCI controller (class 0c.03.30)");
        diag_pushf_lit("no PCI xHCI (class 0c.03.30) found");
        kprintf("[usb-mouse] no xHCI controller found\n");
    } else {
        dbg_set("usb: scan finished, no HID mouse");
        diag_pushf("scan done: %u xHCIs tried, no HID found", tried, 0, 0, 0, 0);
        kprintf("[usb-mouse] %u xHCI controllers tried — no HID mouse\n", tried);
    }
    return -1;
}

void usb_xhci_mouse_poll(void) {
    if (!g_active) return;

    /* Drain all completed events; re-arm IN every report. */
    for (unsigned guard = 0; guard < 32u; guard++) {
        uint32_t *e = g_evt_ring + g_evt_deq * 4u;
        if ((e[3] & 1u) != g_evt_cycle) return;
        unsigned ty = (e[3] >> 10) & 0x3fu;
        unsigned cc = (e[2] >> 24) & 0xffu;
        unsigned slot = (e[3] >> 24) & 0xffu;
        unsigned ep   = (e[3] >> 16) & 0x1fu;
        evt_advance(1);

        if (ty != TRB_TRANSFER) continue;
        if ((int) slot != g_slot_id) continue;
        if (ep != g_ep_in_xhci_idx) continue;

        if (cc == COMP_SUCCESS || cc == COMP_SHORT_PACKET) {
            unsigned residue = e[2] & 0xffffffu;
            unsigned got = (g_int_mps > residue) ? (g_int_mps - residue) : 0;
            int32_t dx = 0, dy = 0;
            uint8_t btn = 0;
            if (hid_boot_decode_mouse_report(g_int_buf, got, &dx, &dy, &btn) == 0)
                mouse_rel_inject(dx, dy, btn);
        } else {
            kprintf("[usb-mouse] xfer cc=%u — re-arming\n", cc);
        }
        int_arm_one();
    }
}

void usb_xhci_mouse_shutdown(void) { g_active = 0; }

int  usb_xhci_mouse_active(void)        { return g_active; }
int  usb_xhci_mouse_driver_began(void)  { return g_began; }
int  usb_xhci_mouse_probing(void)       { return g_began && !g_scan_done; }
int  usb_xhci_mouse_no_device_after_full_scan(void) { return g_scan_done && !g_active; }
const char *usb_xhci_mouse_debug_status(void) { return g_dbg; }

const char *usb_xhci_mouse_diag_line(unsigned i) {
    if (i >= DIAG_LINES) return "";
    return g_diag[i];
}
unsigned usb_xhci_mouse_diag_count(void) { return g_diag_n; }
