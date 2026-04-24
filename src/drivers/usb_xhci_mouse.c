/*
 * Minimal xHCI root-hub + USB2 HID boot mouse (polling, no MSI).
 * Physical TRB rings + contexts; MMIO via PCI BAR0/1 + HHDM.
 *
 * References: xHCI 1.1/1.2 spec (TRB types, slot/EP contexts), USB HID 1.11
 * boot protocol (3-byte relative reports), OSDev XHCI overview.
 */

#include "usb_xhci_mouse.h"

#include "../kprintf.h"
#include "../mm/pmm.h"
#include "../pci/pci.h"
#include "mouse.h"

#include <limine.h>

#include <stddef.h>
#include <stdint.h>

extern volatile struct limine_hhdm_request hhdm_request;

/* ---------------- MMIO / caps ---------------- */

#define TRB_SIZE 16u

#define TRB_TYPE_SHIFT         10u
#define TRB_IOC                (1u << 5)
#define TRB_CHAIN              (1u << 4)
#define TRB_IDT                (1u << 6)

#define TRB_NORMAL             1u
#define TRB_SETUP              2u
#define TRB_DATA               3u
#define TRB_STATUS             4u
#define TRB_CMD_NOOP           23u
#define TRB_CMD_ENABLE_SLOT    9u
#define TRB_CMD_ADDRESS_DEVICE 11u
#define TRB_CMD_EVAL_CONTEXT   13u
#define TRB_CMD_CONFIGURE_EP     12u

#define TRB_EVT_TRANSFER       32u
#define TRB_EVT_CMD_COMPLETION 33u

#define TRB_TRT_NO_DATA        (0u << 16)
#define TRB_TRT_OUT            (2u << 16)
#define TRB_TRT_IN             (3u << 16)

#define USBSTS_HCH             (1u << 0)
#define USBSTS_CNR             (1u << 11)
#define USBCMD_RS              (1u << 0)
#define USBCMD_HCRST           (1u << 1)

#define PORT_CONNECT           (1u << 0)
#define PORT_PED               (1u << 1)
#define PORT_RESET             (1u << 4)
#define PORT_SPEED_SHIFT       10u
#define PORT_SPEED_MASK        (0xFu << PORT_SPEED_SHIFT)

#define USB_REQ_GET_DESCRIPTOR 6u
#define USB_REQ_SET_CONFIGURATION 9u
#define USB_REQ_SET_INTERFACE  11u
#define USB_DT_DEVICE          1u
#define USB_DT_CONFIG          2u

#define HID_REQ_SET_PROTOCOL   11u
#define HID_PROTOCOL_BOOT      0u

#define XHCI_CONFIG_OFFSET     0x38u
#define XHCI_DCBAAP_OFFSET     0x30u
#define XHCI_CRCR_OFFSET       0x18u
#define XHCI_CMD_OFFSET        0x00u
#define XHCI_STS_OFFSET        0x04u
#define XHCI_PORTSC_BASE       0x400u

#define ERSTSZ_OFFSET          0x08u
#define ERSTBA_OFFSET          0x10u
#define ERDP_OFFSET            0x18u
#define IMAN_OFFSET            0x00u

#define CTX_ALIGN              64u
#define SLOT_CTX_ENTRIES       32u

struct xhci_trb {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

struct xhci_erst_entry {
    uint32_t seg_base_lo;
    uint32_t seg_base_hi;
    uint32_t seg_size;
    uint32_t rsvd;
} __attribute__((packed));

static inline uint64_t hhdm(void) {
    return hhdm_request.response ? hhdm_request.response->offset : 0;
}

static inline void *phys_to_virt(uint64_t phys) {
    return (void *) (phys + hhdm());
}

static inline void dma_wmb(void) {
    __asm__ volatile ("" ::: "memory");
}

static void dma_memzero(uint64_t phys, size_t n) {
    uint8_t *p = (uint8_t *) phys_to_virt(phys);
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

static uint32_t readl(const volatile uint32_t *r) {
    return *r;
}

static void writel(volatile uint32_t *r, uint32_t v) {
    *r = v;
}

static uint64_t readq(const volatile uint64_t *r) {
    return *r;
}

static void writeq(volatile uint64_t *r, uint64_t v) {
    *r = v;
}

/* ---------------- driver state ---------------- */

static volatile uint8_t *g_cap = NULL;
static uint8_t           g_caplen;
static volatile uint8_t *g_op = NULL;
static volatile uint8_t *g_run = NULL;
static volatile uint32_t *g_doorbells = NULL;

static uint32_t g_max_slots = 0;
static uint8_t  g_max_ports = 0;

static uint64_t g_phys_cmd_ring = 0;
static uint64_t g_phys_er_seg = 0;
static uint64_t g_phys_erst = 0;
static uint64_t g_phys_dcbaa = 0;
static uint64_t g_phys_scratch_ptrs = 0;
static uint64_t g_phys_scratch_bufs[8];
static unsigned g_scratch_count = 0;

static struct xhci_trb *g_cmd_ring = NULL;
static unsigned         g_cmd_idx = 0;
static int              g_cmd_cycle = 1;

static struct xhci_trb *g_er_seg = NULL;
static unsigned         g_er_size = 256;
static unsigned         g_er_cons = 0;
static int              g_er_ccs = 1;

static int      g_active = 0;
static uint8_t  g_slot_id = 0;
static uint8_t  g_ep_in = 1;       /* default EP1 IN for boot mouse */
static uint16_t g_ep_mps = 8;
static uint8_t  g_conf_value = 1;
static uint8_t  g_iface = 0;

static uint64_t g_phys_in_ctx = 0;
static uint64_t g_phys_out_ctx = 0;
static uint64_t g_phys_ep0_ring = 0;
static uint64_t g_phys_ep1_ring = 0;
static struct xhci_trb *g_ep0_ring = NULL;
static struct xhci_trb *g_ep1_ring = NULL;
static unsigned g_ep0_idx = 0;
static unsigned g_ep1_idx = 0;
static int      g_ep0_cycle = 1;
static int      g_ep1_cycle = 1;

static uint32_t g_scr_w = 1920;
static uint32_t g_scr_h = 1080;

static uint64_t g_portsc_phys = 0;
static uint8_t  g_root_port = 0;

/* ---------------- TRB helpers ---------------- */

static void trb_zero(struct xhci_trb *t) {
    t->param_lo = 0;
    t->param_hi = 0;
    t->status   = 0;
    t->control  = 0;
}

static void trb_pack_setup(struct xhci_trb *t, const uint8_t setup[8],
                          uint32_t trt, int cycle, int chain, int ioc) {
    uint64_t imm = 0;
    for (int i = 0; i < 8; i++) ((uint8_t *) &imm)[i] = setup[i];
    t->param_lo = (uint32_t) imm;
    t->param_hi = (uint32_t) (imm >> 32);
    t->status   = 8u; /* TRB transfer length for setup */
    t->control  = (uint32_t) ((TRB_SETUP << TRB_TYPE_SHIFT) | TRB_IDT | trt
                              | (cycle & 1u) | (chain ? TRB_CHAIN : 0u)
                              | (ioc ? TRB_IOC : 0u));
}

static void trb_pack_data(struct xhci_trb *t, uint64_t buf_phys, uint32_t len,
                           int dir_in, int cycle, int chain, int ioc) {
    t->param_lo = (uint32_t) buf_phys;
    t->param_hi = (uint32_t) (buf_phys >> 32);
    t->status     = len & 0x1FFFFu;
    uint32_t dirbit = dir_in ? (1u << 16) : 0u; /* DIRECTION bit in TRB */
    t->control    = (uint32_t) ((TRB_DATA << TRB_TYPE_SHIFT) | dirbit
                                | (cycle & 1u) | (chain ? TRB_CHAIN : 0u)
                                | (ioc ? TRB_IOC : 0u));
}

static void trb_pack_status(struct xhci_trb *t, int dir_in, int cycle, int ioc) {
    t->param_lo = 0;
    t->param_hi = 0;
    t->status   = 0;
    uint32_t dirbit = dir_in ? (1u << 16) : 0u;
    t->control    = (uint32_t) ((TRB_STATUS << TRB_TYPE_SHIFT) | dirbit
                                | (cycle & 1u) | (ioc ? TRB_IOC : 0u));
}

static void trb_pack_normal(struct xhci_trb *t, uint64_t buf_phys, uint32_t len,
                            int cycle, int ioc) {
    t->param_lo = (uint32_t) buf_phys;
    t->param_hi = (uint32_t) (buf_phys >> 32);
    t->status   = len & 0x1FFFFu;
    t->control  = (uint32_t) ((TRB_NORMAL << TRB_TYPE_SHIFT)
                              | (cycle & 1u) | (ioc ? TRB_IOC : 0u));
}

static void trb_pack_cmd(struct xhci_trb *t, uint32_t type, uint64_t param,
                          int cycle, int ioc) {
    t->param_lo = (uint32_t) param;
    t->param_hi = (uint32_t) (param >> 32);
    t->status   = 0;
    t->control  = (uint32_t) ((type << TRB_TYPE_SHIFT) | (cycle & 1u)
                              | (ioc ? TRB_IOC : 0u));
}

/* ---------------- xHCI low-level ---------------- */

static int wait_bits_clear32(volatile uint32_t *reg, uint32_t mask, unsigned spins) {
    for (unsigned i = 0; i < spins; i++) {
        if ((readl(reg) & mask) == 0) return 0;
        for (volatile int j = 0; j < 500; j++) { }
    }
    return -1;
}

static int wait_bits_set32(volatile uint32_t *reg, uint32_t mask, unsigned spins) {
    for (unsigned i = 0; i < spins; i++) {
        if ((readl(reg) & mask) != 0) return 0;
        for (volatile int j = 0; j < 500; j++) { }
    }
    return -1;
}

static volatile uint32_t *portsc_reg(unsigned port) {
    if (port == 0 || port > g_max_ports) return NULL;
    return (volatile uint32_t *) (g_op + XHCI_PORTSC_BASE + (port - 1u) * 16u);
}

static void ring_host_doorbell(void) {
    if (g_doorbells) writel(g_doorbells, 0u);
}

static void ring_device_doorbell(uint8_t slot, unsigned target) {
    if (g_doorbells && slot)
        writel((volatile uint32_t *) ((uintptr_t) g_doorbells + (unsigned) slot * 4u),
               (uint32_t) target);
}

static void cmd_enqueue(struct xhci_trb *trb) {
    g_cmd_ring[g_cmd_idx] = *trb;
    dma_wmb();
    g_cmd_idx++;
    if (g_cmd_idx >= 256u) {
        g_cmd_idx = 0;
        g_cmd_cycle ^= 1;
    }
}

static void cmd_issue(struct xhci_trb *trb) {
    trb->control &= ~1u;
    trb->control |= (uint32_t) (g_cmd_cycle & 1);
    cmd_enqueue(trb);
    ring_host_doorbell();
}

static int event_wait_cmd_completion(uint32_t *out_param, int spins) {
    for (unsigned n = 0; n < spins; n++) {
        struct xhci_trb *ev = &g_er_seg[g_er_cons];
        int c = (int) (ev->control & 1u);
        if (c != g_er_ccs) {
            for (volatile int w = 0; w < 200; w++) { }
            continue;
        }
        uint32_t type = (ev->control >> TRB_TYPE_SHIFT) & 0x3Fu;
        if (type == TRB_EVT_CMD_COMPLETION) {
            if (out_param) *out_param = ev->param_lo;
            uint32_t cc = (ev->status >> 24u) & 0xFFu;
            /* Advance consumer */
            g_er_cons++;
            if (g_er_cons >= g_er_size) {
                g_er_cons = 0;
                g_er_ccs ^= 1;
            }
            /* Ack ERDP — clear Event Handler Busy */
            volatile uint8_t *ir0 = g_run + 0x20u;
            uint64_t erdp = g_phys_er_seg + (uint64_t) g_er_cons * TRB_SIZE;
            writeq((volatile uint64_t *) (ir0 + ERDP_OFFSET),
                   erdp | (1ull << 3)); /* EHB clear */
            return (int) cc;
        }
        if (type == TRB_EVT_TRANSFER) {
            /* Consume stray transfer events */
            g_er_cons++;
            if (g_er_cons >= g_er_size) {
                g_er_cons = 0;
                g_er_ccs ^= 1;
            }
            volatile uint8_t *ir0 = g_run + 0x20u;
            uint64_t erdp = g_phys_er_seg + (uint64_t) g_er_cons * TRB_SIZE;
            writeq((volatile uint64_t *) (ir0 + ERDP_OFFSET), erdp | (1ull << 3));
            continue;
        }
        for (volatile int w = 0; w < 300; w++) { }
    }
    return -1;
}

static int hci_reset(void) {
    volatile uint32_t *cmd = (volatile uint32_t *) (g_op + XHCI_CMD_OFFSET);
    volatile uint32_t *sts = (volatile uint32_t *) (g_op + XHCI_STS_OFFSET);
    writel(cmd, readl(cmd) & ~USBCMD_RS);
    if (wait_bits_set32(sts, USBSTS_HCH, 200000) != 0) return -1;
    writel(cmd, readl(cmd) | USBCMD_HCRST);
    if (wait_bits_clear32(cmd, USBCMD_HCRST, 200000) != 0) return -1;
    for (volatile int i = 0; i < 500000; i++) { }
    if (wait_bits_clear32(sts, USBSTS_CNR, 500000) != 0) return -1;
    return 0;
}

static int xhci_start(void) {
    volatile uint32_t *cmd = (volatile uint32_t *) (g_op + XHCI_CMD_OFFSET);
    volatile uint32_t *cfg = (volatile uint32_t *) (g_op + XHCI_CONFIG_OFFSET);
    volatile uint64_t *dcbaap = (volatile uint64_t *) (g_op + XHCI_DCBAAP_OFFSET);
    volatile uint64_t *crcr = (volatile uint64_t *) (g_op + XHCI_CRCR_OFFSET);

    writel(cfg, (readl(cfg) & ~0xFFu) | (g_max_slots & 0xFFu));

    dma_memzero(g_phys_dcbaa, 4096u);
    uint64_t *dcbaa = (uint64_t *) phys_to_virt(g_phys_dcbaa);
    if (g_scratch_count) {
        dcbaa[0] = g_phys_scratch_ptrs;
    } else {
        dcbaa[0] = 0;
    }
    writeq(dcbaap, g_phys_dcbaa);

    g_cmd_idx = 0;
    g_cmd_cycle = 1;
    dma_memzero(g_phys_cmd_ring, 4096u);
    uint64_t crcr_val = g_phys_cmd_ring | 1u; /* RCS = cycle 1 */
    writeq(crcr, crcr_val);

    /* Event ring */
    volatile uint8_t *ir0 = g_run + 0x20u;
    dma_memzero(g_phys_er_seg, (uint64_t) g_er_size * TRB_SIZE);
    dma_memzero(g_phys_erst, 64u);
    struct xhci_erst_entry *erst = (struct xhci_erst_entry *) phys_to_virt(g_phys_erst);
    erst->seg_base_lo = (uint32_t) g_phys_er_seg;
    erst->seg_base_hi = (uint32_t) (g_phys_er_seg >> 32);
    erst->seg_size = g_er_size;

    writel((volatile uint32_t *) (ir0 + ERSTSZ_OFFSET), 1u);
    writeq((volatile uint64_t *) (ir0 + ERSTBA_OFFSET), g_phys_erst);
    g_er_cons = 0;
    g_er_ccs = 1;
    writeq((volatile uint64_t *) (ir0 + ERDP_OFFSET), g_phys_er_seg | (1ull << 3));

    writel((volatile uint32_t *) (ir0 + IMAN_OFFSET), 3u); /* clear + enable */

    writel(cmd, readl(cmd) | USBCMD_RS);
    volatile uint32_t *sts = (volatile uint32_t *) (g_op + XHCI_STS_OFFSET);
    if (wait_bits_clear32(sts, USBSTS_HCH, 500000) != 0) return -1;
    return 0;
}

static int port_reset(unsigned port) {
    volatile uint32_t *ps = portsc_reg(port);
    if (!ps) return -1;
    uint32_t v = readl(ps);
    if ((v & PORT_CONNECT) == 0) return -1;
    writel(ps, v | PORT_RESET);
    if (wait_bits_clear32(ps, PORT_RESET, 1000000) != 0) return -1;
    for (volatile int i = 0; i < 500000; i++) { }
    v = readl(ps);
    if ((v & PORT_PED) == 0) return -1;
    return 0;
}

static unsigned port_speed(unsigned port) {
    volatile uint32_t *ps = portsc_reg(port);
    if (!ps) return 0;
    return (readl(ps) & PORT_SPEED_MASK) >> PORT_SPEED_SHIFT;
}

/* ---------------- USB control / HID ---------------- */

static uint8_t g_setup_pkt[8];
static uint8_t g_desc_buf[256];
static uint8_t g_report_buf[8];

static void setup_get_descriptor(uint8_t type, uint8_t index, uint16_t len) {
    g_setup_pkt[0] = 0x80u;
    g_setup_pkt[1] = USB_REQ_GET_DESCRIPTOR;
    g_setup_pkt[2] = (uint8_t) ((uint16_t) ((uint16_t) type << 8) & 0xFFu);
    g_setup_pkt[3] = (uint8_t) (((uint16_t) type << 8) >> 8);
    g_setup_pkt[4] = index;
    g_setup_pkt[5] = 0;
    g_setup_pkt[6] = (uint8_t) (len & 0xFFu);
    g_setup_pkt[7] = (uint8_t) (len >> 8);
}

static void setup_set_configuration(uint8_t cfg) {
    g_setup_pkt[0] = 0u;
    g_setup_pkt[1] = USB_REQ_SET_CONFIGURATION;
    g_setup_pkt[2] = cfg;
    g_setup_pkt[3] = 0;
    g_setup_pkt[4] = 0;
    g_setup_pkt[5] = 0;
    g_setup_pkt[6] = 0;
    g_setup_pkt[7] = 0;
}

static void setup_set_interface(uint8_t iface, uint8_t alt) {
    g_setup_pkt[0] = 1u; /* recipient: interface */
    g_setup_pkt[1] = USB_REQ_SET_INTERFACE;
    g_setup_pkt[2] = alt;
    g_setup_pkt[3] = 0;
    g_setup_pkt[4] = iface;
    g_setup_pkt[5] = 0;
    g_setup_pkt[6] = 0;
    g_setup_pkt[7] = 0;
}

static void setup_set_protocol(uint8_t iface, uint8_t proto) {
    g_setup_pkt[0] = 0x21u; /* class, interface */
    g_setup_pkt[1] = HID_REQ_SET_PROTOCOL;
    g_setup_pkt[2] = proto;
    g_setup_pkt[3] = 0;
    g_setup_pkt[4] = iface;
    g_setup_pkt[5] = 0;
    g_setup_pkt[6] = 0;
    g_setup_pkt[7] = 0;
}

static int ep0_control_transfer(const uint8_t setup[8], void *data, uint32_t len,
                                  int is_in) {
    struct xhci_trb ring[3];
    trb_pack_setup(&ring[0], setup, len ? TRB_TRT_IN : TRB_TRT_NO_DATA,
                   g_ep0_cycle, len != 0, 0);
    if (len) {
        uint64_t p = (uint64_t) (uintptr_t) data - hhdm();
        trb_pack_data(&ring[1], p, len, is_in ? 1 : 0, g_ep0_cycle, 0, 0);
        trb_pack_status(&ring[2], is_in ? 0 : 1, g_ep0_cycle, 1);
    } else {
        trb_pack_status(&ring[1], 1, g_ep0_cycle, 1);
    }

    /* Copy chain to ep0 ring */
    unsigned n = len ? 3u : 2u;
    for (unsigned i = 0; i < n; i++) {
        g_ep0_ring[g_ep0_idx] = ring[i];
        g_ep0_idx++;
        if (g_ep0_idx >= 64u) {
            g_ep0_idx = 0;
            g_ep0_cycle ^= 1;
        }
    }
    dma_wmb();
    /* Update EP0 dequeue in output context — simplified: assume static ring
     * base programmed once; hardware walks from dequeue pointer in context.
     * For simplicity re-issue same ring base each transfer by resetting idx
     * is wrong. We append to ring — need Link TRB or reset ring each xfer.
     *
     * Simpler approach: rebuild 3 TRBs at ring index 0 each time and set
     * dequeue pointer in slot context to ring base (one transfer per poll).
     */
    (void) n;
    return -1; /* replaced below */
}

/* Simplified: place one control transfer at slot 0 of ep0 ring each call */
static int ep0_do_xfer(const uint8_t setup[8], uint64_t data_phys, uint32_t len,
                        int is_in) {
    g_ep0_idx = 0;
    g_ep0_cycle = 1;
    dma_memzero(g_phys_ep0_ring, 64u * TRB_SIZE);

    struct xhci_trb *r = g_ep0_ring;
    if (len) {
        trb_pack_setup(&r[0], setup, TRB_TRT_IN, g_ep0_cycle, 1, 0);
        trb_pack_data(&r[1], data_phys, len, 1, g_ep0_cycle, 0, 0);
        trb_pack_status(&r[2], 0, g_ep0_cycle, 1);
    } else {
        trb_pack_setup(&r[0], setup, TRB_TRT_NO_DATA, g_ep0_cycle, 0, 1);
    }
    dma_wmb();

    /* EP0 doorbell target = 1 (control endpoint) per xHCI */
    ring_device_doorbell(g_slot_id, 1u);

    /* Wait transfer event */
    for (unsigned spins = 0; spins < 5000000u; spins++) {
        struct xhci_trb *ev = &g_er_seg[g_er_cons];
        if ((int) (ev->control & 1u) != g_er_ccs) {
            for (volatile int w = 0; w < 100; w++) { }
            continue;
        }
        uint32_t type = (ev->control >> TRB_TYPE_SHIFT) & 0x3Fu;
        if (type == TRB_EVT_TRANSFER) {
            uint32_t cc = (ev->status >> 24u) & 0xFFu;
            g_er_cons++;
            if (g_er_cons >= g_er_size) {
                g_er_cons = 0;
                g_er_ccs ^= 1;
            }
            volatile uint8_t *ir0 = g_run + 0x20u;
            uint64_t erdp = g_phys_er_seg + (uint64_t) g_er_cons * TRB_SIZE;
            writeq((volatile uint64_t *) (ir0 + ERDP_OFFSET), erdp | (1ull << 3));
            return (cc == 1u || cc == 13u) ? 0 : -1; /* SUCCESS or SHORT_PACKET */
        }
        for (volatile int w = 0; w < 50; w++) { }
    }
    return -1;
}

/* Remove dead stub that referenced incomplete ep0_control_transfer */
static int ep0_get_descriptor(uint8_t dtyp, uint8_t idx, void *buf, uint16_t len) {
    setup_get_descriptor(dtyp, idx, len);
    uint64_t p = (uint64_t) (uintptr_t) buf - hhdm();
    return ep0_do_xfer(g_setup_pkt, p, len, 1);
}

static int ep0_set_configuration(uint8_t cfg) {
    setup_set_configuration(cfg);
    return ep0_do_xfer(g_setup_pkt, 0, 0, 0);
}

static int ep0_set_interface(uint8_t iface, uint8_t alt) {
    setup_set_interface(iface, alt);
    return ep0_do_xfer(g_setup_pkt, 0, 0, 0);
}

static int ep0_set_protocol(uint8_t iface, uint8_t proto) {
    setup_set_protocol(iface, proto);
    return ep0_do_xfer(g_setup_pkt, 0, 0, 0);
}

/* ---------------- slot / address / contexts ---------------- */

static void write_slot_ctx(uint64_t phys, uint8_t port, unsigned speed) {
    uint32_t *w = (uint32_t *) phys_to_virt(phys);
    for (int i = 0; i < (int) (0x20u / 4u); i++) w[i] = 0;
    /* Slot context dw0: route string 0; speed; hub=0; mtt=0; d=1 */
    uint32_t dev_info = (1u << 27) | ((uint32_t) speed << 20) | ((uint32_t) port << 16);
    w[1] = dev_info;
    w[2] = 0;
    w[3] = 0;
}

static void write_ep0_ctx(uint64_t phys, uint64_t ring_phys, unsigned max_packet,
                          unsigned speed) {
    uint32_t *e = (uint32_t *) phys_to_virt(phys);
    for (int i = 0; i < (int) (0x20u / 4u); i++) e[i] = 0;
    /* EP type Control = 4 in EP type field bits 3:1 of dword1 — spec 6.2.3 */
    uint32_t ep_type = 4u << 3;
    uint32_t cerr = 3u << 1;
    uint64_t tr_deq = ring_phys | 1ull; /* DCS */
    e[0] = (uint32_t) tr_deq;
    e[1] = (uint32_t) (tr_deq >> 32) | ep_type | cerr;
    /* max packet size in bits 31:16 of dword1 — layout per spec */
    e[1] |= (max_packet & 0xFFFFu) << 16;
    (void) speed;
}

static void write_ep_in_ctx(uint64_t phys, uint64_t ring_phys, unsigned epnum,
                            unsigned max_packet, unsigned interval) {
    uint32_t *e = (uint32_t *) phys_to_virt(phys);
    for (int i = 0; i < (int) (0x20u / 4u); i++) e[i] = 0;
    /* Interrupt IN EP type = 7 */
    uint32_t ep_type = 7u << 3;
    uint32_t cerr = 3u << 1;
    uint64_t tr_deq = ring_phys | 1ull;
    e[0] = (uint32_t) tr_deq;
    e[1] = (uint32_t) (tr_deq >> 32) | ep_type | cerr;
    e[1] |= (max_packet & 0xFFFFu) << 16;
    /* average TRB length / max ESIT payload — minimal */
    e[2] = (uint32_t) max_packet;
    e[4] = (uint32_t) interval; /* bInterval log */
    (void) epnum;
}

static int input_ctx_for_address(uint8_t port, unsigned speed, unsigned mps0) {
    dma_memzero(g_phys_in_ctx, 4096u);
    uint32_t *ic = (uint32_t *) phys_to_virt(g_phys_in_ctx);
    /* Input control: add slot (A0) and EP0 (A1) */
    ic[1] = (1u << 0) | (1u << 1);

    uint64_t slot_off = g_phys_in_ctx + CTX_ALIGN;
    uint64_t ep0_off  = g_phys_in_ctx + CTX_ALIGN * 2u;
    write_slot_ctx(slot_off, port, speed);
    write_ep0_ctx(ep0_off, g_phys_ep0_ring, mps0, speed);
    return 0;
}

static int issue_address_device(uint8_t port, unsigned speed) {
    input_ctx_for_address(port, speed, 8u);
    struct xhci_trb t;
    trb_pack_cmd(&t, TRB_CMD_ADDRESS_DEVICE,
                 g_phys_in_ctx | ((uint64_t) g_slot_id << 32), g_cmd_cycle, 1);
    cmd_issue(&t);
    uint32_t par = 0;
    int cc = event_wait_cmd_completion(&par, 5000000);
    return (cc == 1) ? 0 : -1;
}

static int issue_evaluate_context_mps0(unsigned mps) {
    dma_memzero(g_phys_in_ctx, 4096u);
    uint32_t *ic = (uint32_t *) phys_to_virt(g_phys_in_ctx);
    ic[1] = (1u << 1); /* drop/add EP0 only — simplified add EP0 */
    uint64_t ep0_off = g_phys_in_ctx + CTX_ALIGN * 2u;
    write_ep0_ctx(ep0_off, g_phys_ep0_ring, mps, port_speed(g_root_port));

    struct xhci_trb t;
    trb_pack_cmd(&t, TRB_CMD_EVALUATE_CONTEXT, g_phys_in_ctx | ((uint64_t) g_slot_id << 32),
                 g_cmd_cycle, 1);
    cmd_issue(&t);
    uint32_t par = 0;
    int cc = event_wait_cmd_completion(&par, 5000000);
    return (cc == 1) ? 0 : -1;
}

static int issue_configure_ep_interrupt(void) {
    dma_memzero(g_phys_in_ctx, 4096u);
    uint32_t *ic = (uint32_t *) phys_to_virt(g_phys_in_ctx);
    /* Add EP1 IN — context index 2 for EP1 IN in 1-based EP scheme? 
     * Input context index for EP{K} = 2*K for OUT and 2*K+1 for IN? 
     * xHCI: Slot context is 1, EP1 OUT is 2, EP1 IN is 3 ... */
    ic[1] = (1u << 2); /* add EP1 OUT? wrong */
    /* EP1 IN is context index 3 for first interface interrupt IN */
    ic[1] = (1u << 3);

    uint64_t ep_in_off = g_phys_in_ctx + CTX_ALIGN * 4u;
    write_ep_in_ctx(ep_in_off, g_phys_ep1_ring, g_ep_in, g_ep_mps, 10u);

    struct xhci_trb t;
    trb_pack_cmd(&t, TRB_CMD_CONFIGURE_EP, g_phys_in_ctx | ((uint64_t) g_slot_id << 32),
                 g_cmd_cycle, 1);
    cmd_issue(&t);
    uint32_t par = 0;
    int cc = event_wait_cmd_completion(&par, 5000000);
    return (cc == 1) ? 0 : -1;
}

static int parse_config_simple(const uint8_t *cfg, int len) {
    /* Find first HID boot mouse interface + interrupt IN endpoint */
    int i = 0;
    g_iface = 0;
    g_ep_in = 1;
    g_ep_mps = 8;
    g_conf_value = cfg[5];
    while (i + 8 <= len) {
        uint8_t l = cfg[i];
        uint8_t t = cfg[i + 1];
        if (l < 2) break;
        if (t == 4u) { /* interface */
            g_iface = cfg[i + 2];
        }
        if (t == 5u) { /* endpoint */
            uint8_t addr = cfg[i + 2];
            uint8_t attr = cfg[i + 3];
            uint16_t mps = (uint16_t) cfg[i + 4] | ((uint16_t) cfg[i + 5] << 8);
            if ((addr & 0x80u) && (attr == 3u)) { /* IN interrupt */
                g_ep_in = (uint8_t) (addr & 0xFu);
                g_ep_mps = (uint16_t) (mps & 0x7FFu);
                return 0;
            }
        }
        i += (int) l;
    }
    return -1;
}

/* ---------------- public API ---------------- */

void usb_xhci_mouse_shutdown(void) {
    if (g_op) {
        volatile uint32_t *cmd = (volatile uint32_t *) (g_op + XHCI_CMD_OFFSET);
        writel(cmd, readl(cmd) & ~USBCMD_RS);
    }
    g_active = 0;
    g_cap = NULL;
}

int usb_xhci_mouse_active(void) { return g_active; }

void usb_xhci_mouse_poll(void) {
    if (!g_active) return;

    g_ep1_idx = 0;
    g_ep1_cycle = 1;
    dma_memzero(g_phys_ep1_ring, 16u * TRB_SIZE);
    uint64_t rphys = (uint64_t) (uintptr_t) g_report_buf - hhdm();
    trb_pack_normal(&g_ep1_ring[0], rphys, 8u, g_ep1_cycle, 1);
    dma_wmb();

    unsigned db_target = (unsigned) g_ep_in * 2u + 1u; /* IN */
    ring_device_doorbell(g_slot_id, db_target);

    for (unsigned spins = 0; spins < 2000000u; spins++) {
        struct xhci_trb *ev = &g_er_seg[g_er_cons];
        if ((int) (ev->control & 1u) != g_er_ccs) {
            for (volatile int w = 0; w < 20; w++) { }
            continue;
        }
        uint32_t type = (ev->control >> TRB_TYPE_SHIFT) & 0x3Fu;
        if (type == TRB_EVT_TRANSFER) {
            g_er_cons++;
            if (g_er_cons >= g_er_size) {
                g_er_cons = 0;
                g_er_ccs ^= 1;
            }
            volatile uint8_t *ir0 = g_run + 0x20u;
            uint64_t erdp = g_phys_er_seg + (uint64_t) g_er_cons * TRB_SIZE;
            writeq((volatile uint64_t *) (ir0 + ERDP_OFFSET), erdp | (1ull << 3));

            uint8_t b = g_report_buf[0];
            int8_t dx = (int8_t) g_report_buf[1];
            int8_t dy = (int8_t) g_report_buf[2];
            int32_t cx, cy;
            mouse_get_state(&cx, &cy, NULL);
            int32_t nx = cx + (int32_t) dx;
            int32_t ny = cy - (int32_t) dy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx >= (int32_t) g_scr_w)  nx = (int32_t) g_scr_w - 1;
            if (ny >= (int32_t) g_scr_h) ny = (int32_t) g_scr_h - 1;
            uint8_t btn = 0;
            if (b & 1) btn |= MOUSE_BTN_LEFT;
            if (b & 2) btn |= MOUSE_BTN_RIGHT;
            if (b & 4) btn |= MOUSE_BTN_MIDDLE;
            mouse_absolute_inject(nx, ny, btn);
            return;
        }
        for (volatile int w = 0; w < 30; w++) { }
    }
}

int usb_xhci_mouse_init(uint32_t screen_w, uint32_t screen_h) {
    g_scr_w = screen_w ? screen_w : 1920u;
    g_scr_h = screen_h ? screen_h : 1080u;
    g_active = 0;

    struct pci_device *d = NULL;
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_device *c = pci_at(i);
        if (c && c->class_code == 0x0Cu && c->subclass == 0x03u && c->prog_if == 0x30u) {
            d = c;
            break;
        }
    }
    if (!d) return -1;

    pci_enable_mmio_bus_master(d);
    if (d->bars[0].type != PCI_BAR_MEM || d->bars[0].base == 0) return -1;

    uint64_t bar = d->bars[0].base;
    if (d->bars[0].is_64bit) bar |= d->bars[1].base << 32;
    g_cap = (volatile uint8_t *) phys_to_virt(bar);
    g_caplen = g_cap[0];
    uint32_t hccp1 = readl((volatile uint32_t *) (g_cap + 0x10u));
    (void) hccp1;
    uint32_t hcsp1 = readl((volatile uint32_t *) (g_cap + 0x04u));
    g_max_slots = hcsp1 & 0xFFu;
    if (g_max_slots < 2u) g_max_slots = 2u;
    g_max_ports = (uint8_t) ((hcsp1 >> 24) & 0xFFu);
    if (g_max_ports == 0 || g_max_ports > 32u) g_max_ports = 16u;

    uint32_t rts = readl((volatile uint32_t *) (g_cap + 0x18u)) & ~3u;
    uint32_t dbo = readl((volatile uint32_t *) (g_cap + 0x14u)) & ~3u;
    g_op = g_cap + g_caplen;
    g_run = g_cap + rts;
    g_doorbells = (volatile uint32_t *) (g_cap + dbo);

    uint32_t hcsp2 = readl((volatile uint32_t *) (g_cap + 0x08u));
    g_scratch_count = (unsigned) ((hcsp2 >> 21) & 0x1Fu);
    if (g_scratch_count > 8u) g_scratch_count = 8u;

    if (hci_reset() != 0) {
        kprintf("[usb-mouse] HCRST failed\n");
        return -1;
    }

    /* DMA pages */
    g_phys_cmd_ring = pmm_alloc_page();
    g_phys_er_seg   = pmm_alloc_page();
    g_phys_erst     = pmm_alloc_page();
    g_phys_dcbaa    = pmm_alloc_page();
    g_phys_in_ctx   = pmm_alloc_page();
    g_phys_out_ctx  = pmm_alloc_page();
    g_phys_ep0_ring = pmm_alloc_page();
    g_phys_ep1_ring = pmm_alloc_page();
    if (!g_phys_cmd_ring || !g_phys_er_seg || !g_phys_erst || !g_phys_dcbaa
        || !g_phys_in_ctx || !g_phys_out_ctx || !g_phys_ep0_ring
        || !g_phys_ep1_ring) {
        kprintf("[usb-mouse] pmm alloc failed\n");
        return -1;
    }

    if (g_scratch_count) {
        g_phys_scratch_ptrs = pmm_alloc_page();
        if (!g_phys_scratch_ptrs) return -1;
        dma_memzero(g_phys_scratch_ptrs, 4096u);
        uint64_t *sp = (uint64_t *) phys_to_virt(g_phys_scratch_ptrs);
        for (unsigned s = 0; s < g_scratch_count; s++) {
            g_phys_scratch_bufs[s] = pmm_alloc_page();
            if (!g_phys_scratch_bufs[s]) return -1;
            sp[s] = g_phys_scratch_bufs[s];
        }
    }

    g_cmd_ring = (struct xhci_trb *) phys_to_virt(g_phys_cmd_ring);
    g_er_seg   = (struct xhci_trb *) phys_to_virt(g_phys_er_seg);
    g_ep0_ring = (struct xhci_trb *) phys_to_virt(g_phys_ep0_ring);
    g_ep1_ring = (struct xhci_trb *) phys_to_virt(g_phys_ep1_ring);

    if (xhci_start() != 0) {
        kprintf("[usb-mouse] xhci_start failed\n");
        return -1;
    }

    /* NOOP to prime command ring */
    struct xhci_trb noop;
    trb_pack_cmd(&noop, TRB_CMD_NOOP, 0, g_cmd_cycle, 1);
    cmd_issue(&noop);
    if (event_wait_cmd_completion(NULL, 2000000) != 1) {
        kprintf("[usb-mouse] NOOP completion missing\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }

    /* Find port */
    g_root_port = 0;
    for (unsigned p = 1; p <= (unsigned) g_max_ports; p++) {
        volatile uint32_t *ps = portsc_reg(p);
        if (!ps) continue;
        if (readl(ps) & PORT_CONNECT) {
            if (port_reset(p) == 0) {
                g_root_port = (uint8_t) p;
                break;
            }
        }
    }
    if (g_root_port == 0) {
        kprintf("[usb-mouse] no connected root port\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }

    /* Enable slot */
    struct xhci_trb es;
    trb_pack_cmd(&es, TRB_CMD_ENABLE_SLOT, 0, g_cmd_cycle, 1);
    cmd_issue(&es);
    uint32_t cparam = 0;
    if (event_wait_cmd_completion(&cparam, 5000000) != 1) {
        kprintf("[usb-mouse] enable slot failed cc=%d\n", cparam);
        usb_xhci_mouse_shutdown();
        return -1;
    }
    g_slot_id = (uint8_t) ((cparam >> 24) & 0xFFu);
    if (g_slot_id == 0 || g_slot_id > g_max_slots) {
        kprintf("[usb-mouse] bad slot id\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }

    uint64_t *dcbaa = (uint64_t *) phys_to_virt(g_phys_dcbaa);
    dcbaa[g_slot_id] = g_phys_out_ctx;
    dma_wmb();

    unsigned spd = port_speed(g_root_port);
    if (issue_address_device(g_root_port, spd) != 0) {
        kprintf("[usb-mouse] address device failed\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }

    /* Device descriptor — first 8 bytes for bMaxPacket0 */
    if (ep0_get_descriptor((uint8_t) (USB_DT_DEVICE << 8), 0, g_desc_buf, 8) != 0) {
        kprintf("[usb-mouse] GET_DESCRIPTOR(8) failed\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }
    uint8_t mps0 = g_desc_buf[7];
    if (mps0 != 8u && mps0 != 16u && mps0 != 32u && mps0 != 64u) mps0 = 8u;
    if (issue_evaluate_context_mps0((unsigned) mps0) != 0) {
        kprintf("[usb-mouse] evaluate context failed\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }

    if (ep0_get_descriptor((uint8_t) (USB_DT_DEVICE << 8), 0, g_desc_buf, 18) != 0) {
        kprintf("[usb-mouse] GET_DESCRIPTOR(18) failed\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }

    if (ep0_get_descriptor((uint8_t) (USB_DT_CONFIG << 8), 0, g_desc_buf, 255) != 0) {
        kprintf("[usb-mouse] GET_CONFIG failed\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }
    uint16_t cfg_len = (uint16_t) g_desc_buf[2] | ((uint16_t) g_desc_buf[3] << 8);
    if (cfg_len > sizeof(g_desc_buf)) cfg_len = sizeof(g_desc_buf);
    if (ep0_get_descriptor((uint8_t) (USB_DT_CONFIG << 8), 0, g_desc_buf, cfg_len) != 0) {
        kprintf("[usb-mouse] GET_CONFIG full failed\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }

    if (parse_config_simple(g_desc_buf, (int) cfg_len) != 0) {
        kprintf("[usb-mouse] no interrupt IN in config\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }

    if (ep0_set_configuration(g_conf_value) != 0) {
        kprintf("[usb-mouse] SET_CONFIGURATION failed\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }
    if (ep0_set_interface(g_iface, 0) != 0) {
        kprintf("[usb-mouse] SET_INTERFACE failed\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }
    if (ep0_set_protocol(g_iface, HID_PROTOCOL_BOOT) != 0) {
        kprintf("[usb-mouse] SET_PROTOCOL failed (non-fatal)\n");
    }

    if (issue_configure_ep_interrupt() != 0) {
        kprintf("[usb-mouse] configure endpoint failed\n");
        usb_xhci_mouse_shutdown();
        return -1;
    }

    g_active = 1;
    kprintf("[usb-mouse] HID boot mouse on slot %u port %u ep%u IN mps=%u\n",
            (unsigned) g_slot_id, (unsigned) g_root_port,
            (unsigned) g_ep_in, (unsigned) g_ep_mps);
    return 0;
}
