#include "virtio_tablet.h"

#include "mouse.h"

#include "../kprintf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../pci/pci.h"

#include <limine.h>

#include <stddef.h>
#include <stdint.h>

extern volatile struct limine_hhdm_request hhdm_request;

#define VIRTIO_PCI_CAP_VENDOR        9u
#define VIRTIO_PCI_CAP_COMMON_CFG    1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG    2u
#define VIRTIO_PCI_CAP_DEVICE_CFG    4u

#define VIRTIO_PCI_NOTIFY_CAP_MULT   16u

/* linux/include/uapi/linux/virtio_pci.h offsets within common cfg */
#define VIRTIO_PCI_COMMON_DFSELECT   0u
#define VIRTIO_PCI_COMMON_DF         4u
#define VIRTIO_PCI_COMMON_GFSELECT   8u
#define VIRTIO_PCI_COMMON_GF         12u
#define VIRTIO_PCI_COMMON_MSIX       16u
#define VIRTIO_PCI_COMMON_NUMQ       18u
#define VIRTIO_PCI_COMMON_STATUS     20u
#define VIRTIO_PCI_COMMON_CFGGEN     21u
#define VIRTIO_PCI_COMMON_Q_SELECT   22u
#define VIRTIO_PCI_COMMON_Q_SIZE     24u
#define VIRTIO_PCI_COMMON_Q_MSIX     26u
#define VIRTIO_PCI_COMMON_Q_ENABLE   28u
#define VIRTIO_PCI_COMMON_Q_NOFF     30u
#define VIRTIO_PCI_COMMON_Q_DESCLO   32u
#define VIRTIO_PCI_COMMON_Q_DESCHI   36u
#define VIRTIO_PCI_COMMON_Q_AVAILLO  40u
#define VIRTIO_PCI_COMMON_Q_AVAILHI  44u
#define VIRTIO_PCI_COMMON_Q_USEDLO   48u
#define VIRTIO_PCI_COMMON_Q_USEDHI   52u

#define VIRTIO_INPUT_CFG_ABS_INFO    0x10u

#define EV_SYN                       0x0000u
#define EV_KEY                       0x0001u
#define EV_ABS                       0x0003u
#define SYN_REPORT                   0x0001u
#define ABS_X                        0x0000u
#define ABS_Y                        0x0001u
#define BTN_LEFT                     0x0110u
#define BTN_RIGHT                    0x0111u
#define BTN_MIDDLE                   0x0112u

#define VIRTIO_CONFIG_S_ACKNOWLEDGE   1u
#define VIRTIO_CONFIG_S_DRIVER        2u
#define VIRTIO_CONFIG_S_FEATURES_OK   8u
#define VIRTIO_CONFIG_S_DRIVER_OK     4u
#define VIRTIO_CONFIG_S_FAILED        128u

#define VIRTQ_DESC_F_WRITE            2u

#define VIRTIO_ID_INPUT               18u
#define VIRTIO_PCI_DEVICE_ID_INPUT    (0x1040u + VIRTIO_ID_INPUT)

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    int32_t  value;
} __attribute__((packed));

static uint64_t hhdm(void) {
    return hhdm_request.response ? hhdm_request.response->offset : 0;
}

static uint32_t rd32_le(const volatile uint8_t *mm, unsigned off) {
    return *(const volatile uint32_t *) (mm + off);
}

static void wr32_le(volatile uint8_t *mm, unsigned off, uint32_t v) {
    *(volatile uint32_t *) (mm + off) = v;
}

static uint16_t rd16_le(const volatile uint8_t *mm, unsigned off) {
    return *(const volatile uint16_t *) (mm + off);
}

static void wr16_le(volatile uint8_t *mm, unsigned off, uint16_t v) {
    *(volatile uint16_t *) (mm + off) = v;
}

static void wr8(volatile uint8_t *mm, unsigned off, uint8_t v) {
    mm[off] = v;
}

static uint8_t rd8(const volatile uint8_t *mm, unsigned off) {
    return mm[off];
}

static struct pci_device *find_virtio_input(void) {
    struct pci_device *d = pci_find_id(0x1AF4u, VIRTIO_PCI_DEVICE_ID_INPUT);
    if (d) return d;
    for (uint32_t i = 0; i < pci_count(); i++) {
        struct pci_device *c = pci_at(i);
        if (!c || c->vendor_id != 0x1AF4u) continue;
        if (c->device_id == VIRTIO_PCI_DEVICE_ID_INPUT) return c;
        uint16_t sub = pci_cfg_read16(c, 0x2Eu);
        if (sub == VIRTIO_ID_INPUT) return c;
    }
    return NULL;
}

static volatile uint8_t *g_bar[PCI_BAR_COUNT];
static uint64_t          g_bar_phys[PCI_BAR_COUNT];
static uint32_t          g_bar_size[PCI_BAR_COUNT];

static volatile uint8_t *g_common;
static volatile uint8_t *g_notify_base;
static uint32_t          g_notify_mult;
static volatile uint8_t *g_device_cfg;

static uint64_t g_ring_phys;
static uint8_t *g_ring_virt;
static uint32_t g_qsz;
static uint16_t g_last_used;

static int      g_active;
static uint32_t g_scr_w, g_scr_h;
static int32_t  g_ax_min, g_ax_max, g_ay_min, g_ay_max;
static int32_t  g_abs_x, g_abs_y;
static int      g_have_abs_x, g_have_abs_y;
static int32_t  g_out_x, g_out_y;
static uint8_t  g_btn;

static void map_bar(struct pci_device *d, int bar_idx) {
    if (bar_idx < 0 || bar_idx >= PCI_BAR_COUNT) return;
    if (g_bar[bar_idx]) return;
    if (d->bars[bar_idx].type != PCI_BAR_MEM || d->bars[bar_idx].size == 0) return;
    uint64_t phys = d->bars[bar_idx].base;
    uint32_t sz = (uint32_t) (d->bars[bar_idx].size > 0x200000ull ? 0x200000ull : d->bars[bar_idx].size);
    uint64_t virt = phys + hhdm();
    mmio_map(virt, phys, sz);
    g_bar[bar_idx]     = (volatile uint8_t *) virt;
    g_bar_phys[bar_idx] = phys;
    g_bar_size[bar_idx] = sz;
}

static volatile uint8_t *bar_ptr(struct pci_device *d, unsigned bar_idx, uint32_t off_in_bar) {
    (void) d;
    if (bar_idx >= PCI_BAR_COUNT || !g_bar[bar_idx]) return NULL;
    if (off_in_bar >= g_bar_size[bar_idx]) return NULL;
    return g_bar[bar_idx] + off_in_bar;
}

static int walk_caps(struct pci_device *d) {
    g_common = g_notify_base = g_device_cfg = NULL;
    g_notify_mult = 1u;

    uint8_t cap = pci_cfg_read8(d, 0x34u);
    while (cap) {
        uint8_t id = pci_cfg_read8(d, cap);
        if (id == VIRTIO_PCI_CAP_VENDOR) {
            uint8_t cfg_type = pci_cfg_read8(d, (uint8_t) (cap + 3u));
            uint8_t bar      = pci_cfg_read8(d, (uint8_t) (cap + 4u));
            uint8_t cap_len  = pci_cfg_read8(d, (uint8_t) (cap + 2u));
            uint32_t off     = pci_cfg_read32(d, (uint8_t) (cap + 8u));
            map_bar(d, bar);
            volatile uint8_t *ptr = bar_ptr(d, bar, off);
            if (!ptr) return -1;

            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG)
                g_common = ptr;
            else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                g_notify_base = ptr;
                if (cap_len >= 20u)
                    g_notify_mult = pci_cfg_read32(d, (uint8_t) (cap + VIRTIO_PCI_NOTIFY_CAP_MULT));
            } else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG)
                g_device_cfg = ptr;
        }
        cap = pci_cfg_read8(d, (uint8_t) (cap + 1u));
    }
    return (g_common && g_notify_base && g_device_cfg) ? 0 : -1;
}

static void cfg_write_device_u8(unsigned off, uint8_t v) {
    if (g_device_cfg) g_device_cfg[off] = v;
}

static void read_abs_range(uint16_t code, int32_t *min_out, int32_t *max_out) {
    *min_out = 0;
    *max_out = 32767;
    if (!g_device_cfg) return;
    cfg_write_device_u8(0, VIRTIO_INPUT_CFG_ABS_INFO);
    cfg_write_device_u8(1, (uint8_t) code);
    for (volatile int i = 0; i < 500; i++) __asm__ volatile ("pause");
    if (g_device_cfg[2] < 8u) return;
    int32_t umin = (int32_t) rd32_le(g_device_cfg, 8u);
    int32_t umax = (int32_t) rd32_le(g_device_cfg, 12u);
    if (umax > umin) {
        *min_out = umin;
        *max_out = umax;
    }
}

static int32_t scale_abs(int32_t v, int32_t lo, int32_t hi, uint32_t screen_max) {
    if (hi <= lo)
        return (int32_t) ((int64_t) v * (int64_t) screen_max / 32767);
    int64_t x = ((int64_t) v - lo) * (int64_t) screen_max;
    x /= (int64_t) (hi - lo);
    if (x < 0) x = 0;
    if (x > (int64_t) screen_max) x = (int64_t) screen_max;
    return (int32_t) x;
}

static void notify_queue(unsigned q_index) {
    if (!g_notify_base || !g_common) return;
    wr16_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_Q_SELECT, (uint16_t) q_index);
    __asm__ volatile ("mfence" ::: "memory");
    uint16_t noff = rd16_le(g_common, VIRTIO_PCI_COMMON_Q_NOFF);
    uint64_t addr = (uint64_t) g_notify_base + (uint64_t) noff * (uint64_t) g_notify_mult;
    if (g_notify_mult == 0u)
        addr = (uint64_t) g_notify_base + (uint64_t) noff;
    *(volatile uint16_t *) (uintptr_t) addr = (uint16_t) q_index;
    __asm__ volatile ("mfence" ::: "memory");
}

static void inject_mouse(void) {
    if (g_have_abs_x) {
        g_out_x = scale_abs(g_abs_x, g_ax_min, g_ax_max, g_scr_w ? g_scr_w - 1u : 0u);
    }
    if (g_have_abs_y) {
        g_out_y = scale_abs(g_abs_y, g_ay_min, g_ay_max, g_scr_h ? g_scr_h - 1u : 0u);
    }
    mouse_absolute_inject(g_out_x, g_out_y, g_btn);
    g_have_abs_x = g_have_abs_y = 0;
}

static void handle_event(const struct virtio_input_event *ev) {
    uint16_t t = ev->type;
    uint16_t c = ev->code;
    int32_t  v = ev->value;

    if (t == EV_ABS && c == ABS_X) {
        g_abs_x = v;
        g_have_abs_x = 1;
    } else if (t == EV_ABS && c == ABS_Y) {
        g_abs_y = v;
        g_have_abs_y = 1;
    } else if (t == EV_KEY) {
        if (c == BTN_LEFT)
            g_btn = (uint8_t) ((g_btn & ~1u) | (v ? 1u : 0u));
        else if (c == BTN_RIGHT)
            g_btn = (uint8_t) ((g_btn & ~2u) | (v ? 2u : 0u));
        else if (c == BTN_MIDDLE)
            g_btn = (uint8_t) ((g_btn & ~4u) | (v ? 4u : 0u));
    } else if (t == EV_SYN && c == SYN_REPORT) {
        inject_mouse();
    }
}

static int setup_vq(unsigned q_index, unsigned want) {
    wr16_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_Q_SELECT, (uint16_t) q_index);
    __asm__ volatile ("mfence" ::: "memory");
    uint16_t qmax = rd16_le(g_common, VIRTIO_PCI_COMMON_Q_SIZE);
    if (qmax < 2u) return -1;
    unsigned qsz = want;
    if (qsz > qmax) qsz = qmax;
    while (qsz & (qsz - 1)) qsz &= qsz - 1;
    if (qsz < 2u) qsz = 2u;
    g_qsz = qsz;
    wr16_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_Q_SIZE, (uint16_t) qsz);
    __asm__ volatile ("mfence" ::: "memory");

    uint64_t ring_phys = pmm_alloc_contig(1);
    if (!ring_phys) return -1;
    g_ring_phys = ring_phys;
    g_ring_virt = (uint8_t *) (ring_phys + hhdm());
    for (unsigned i = 0; i < 4096; i++) g_ring_virt[i] = 0;

    struct vring_desc *desc = (struct vring_desc *) g_ring_virt;
    volatile uint16_t *avail_idx  = (volatile uint16_t *) (g_ring_virt + 0x200u + 2u);
    volatile uint16_t *avail_ring = (volatile uint16_t *) (g_ring_virt + 0x200u + 4u);
    volatile uint16_t *used_idxp  = (volatile uint16_t *) (g_ring_virt + 0x300u + 2u);

    *used_idxp = 0;
    *avail_idx  = 0;

    uint64_t data0 = ring_phys + 0x100u;
    for (unsigned i = 0; i < qsz; i++) {
        desc[i].addr  = data0 + (uint64_t) i * 8u;
        desc[i].len   = sizeof(struct virtio_input_event);
        desc[i].flags = VIRTQ_DESC_F_WRITE;
        desc[i].next  = 0;
        avail_ring[i] = (uint16_t) i;
    }
    __asm__ volatile ("mfence" ::: "memory");
    *avail_idx = (uint16_t) qsz;

    wr16_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_Q_MSIX, 0xFFFFu); /* no MSI-X */

    wr32_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_Q_DESCLO, (uint32_t) ring_phys);
    wr32_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_Q_DESCHI, (uint32_t) (ring_phys >> 32));
    wr32_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_Q_AVAILLO, (uint32_t) (ring_phys + 0x200u));
    wr32_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_Q_AVAILHI, 0u);
    wr32_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_Q_USEDLO, (uint32_t) (ring_phys + 0x300u));
    wr32_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_Q_USEDHI, 0u);

    wr16_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_Q_ENABLE, 1u);
    __asm__ volatile ("mfence" ::: "memory");
    g_last_used = 0;
    notify_queue(q_index);
    return 0;
}

int virtio_tablet_probe_and_init(uint32_t screen_w, uint32_t screen_h) {
    virtio_tablet_shutdown();
    struct pci_device *d = find_virtio_input();
    if (!d) return -1;

    uint16_t cmd = pci_cfg_read16(d, 0x04u);
    pci_cfg_write16(d, 0x04u, (uint16_t) (cmd | 0x6u));

    if (walk_caps(d) != 0 || !g_common) {
        kprintf("[virtio-tablet] missing virtio PCI caps\n");
        virtio_tablet_shutdown();
        return -1;
    }

    g_scr_w = screen_w ? screen_w : 1920u;
    g_scr_h = screen_h ? screen_h : 1080u;
    g_out_x = (int32_t) (g_scr_w / 2u);
    g_out_y = (int32_t) (g_scr_h / 2u);
    read_abs_range(ABS_X, &g_ax_min, &g_ax_max);
    read_abs_range(ABS_Y, &g_ay_min, &g_ay_max);
    kprintf("[virtio-tablet] ABS X [%d,%d] Y [%d,%d] screen %ux%u\n",
            (int) g_ax_min, (int) g_ax_max, (int) g_ay_min, (int) g_ay_max,
            (unsigned) g_scr_w, (unsigned) g_scr_h);

    wr8(g_common, VIRTIO_PCI_COMMON_STATUS, 0u);
    __asm__ volatile ("mfence" ::: "memory");
    wr8(g_common, VIRTIO_PCI_COMMON_STATUS, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    wr8(g_common, VIRTIO_PCI_COMMON_STATUS,
        (uint8_t) (VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER));

    wr32_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_DFSELECT, 0u);
    (void) rd32_le(g_common, VIRTIO_PCI_COMMON_DF);
    wr32_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_GFSELECT, 0u);
    wr32_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_GF, 0u);

    wr32_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_DFSELECT, 1u);
    uint32_t dev_hi = rd32_le(g_common, VIRTIO_PCI_COMMON_DF);
    wr32_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_GFSELECT, 1u);
    wr32_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_GF, dev_hi & 1u); /* VIRTIO_F_VERSION_1 */

    wr8(g_common, VIRTIO_PCI_COMMON_STATUS,
        (uint8_t) (VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER | VIRTIO_CONFIG_S_FEATURES_OK));
    __asm__ volatile ("mfence" ::: "memory");
    if (!(rd8(g_common, VIRTIO_PCI_COMMON_STATUS) & VIRTIO_CONFIG_S_FEATURES_OK)) {
        kprintf("[virtio-tablet] FEATURES_OK failed (dev_hi=0x%x)\n", (unsigned) dev_hi);
        wr8(g_common, VIRTIO_PCI_COMMON_STATUS, VIRTIO_CONFIG_S_FAILED);
        virtio_tablet_shutdown();
        return -1;
    }

    wr16_le((volatile uint8_t *) g_common, VIRTIO_PCI_COMMON_MSIX, 0xFFFFu);

    if (setup_vq(0u, 16u) != 0) {
        kprintf("[virtio-tablet] queue setup failed\n");
        virtio_tablet_shutdown();
        return -1;
    }

    wr8(g_common, VIRTIO_PCI_COMMON_STATUS,
        (uint8_t) (VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER
                   | VIRTIO_CONFIG_S_FEATURES_OK | VIRTIO_CONFIG_S_DRIVER_OK));
    g_active = 1;
    kprintf("[virtio-tablet] ready (absolute)\n");
    return 0;
}

void virtio_tablet_shutdown(void) {
    g_active = 0;
    g_common = g_notify_base = g_device_cfg = NULL;
    g_notify_mult = 0;
    for (int i = 0; i < PCI_BAR_COUNT; i++) g_bar[i] = NULL;
    g_ring_phys = 0;
    g_ring_virt = NULL;
    g_qsz = 0;
    g_last_used = 0;
}

int virtio_tablet_active(void) { return g_active; }

void virtio_tablet_poll(void) {
    if (!g_active || !g_ring_virt) return;

    volatile uint16_t *used_idxp = (volatile uint16_t *) (g_ring_virt + 0x302u);
    volatile struct {
        uint32_t id;
        uint32_t len;
    } *used_ring = (volatile void *) (g_ring_virt + 0x304u);

    volatile uint16_t *avail_idxp = (volatile uint16_t *) (g_ring_virt + 0x202u);
    volatile uint16_t *avail_ring  = (volatile uint16_t *) (g_ring_virt + 0x204u);

    __asm__ volatile ("mfence" ::: "memory");
    uint16_t used_idx = *used_idxp;

    while (g_last_used != used_idx) {
        uint32_t id = used_ring[g_last_used % g_qsz].id;
        (void) used_ring[g_last_used % g_qsz].len;
        if (id < g_qsz) {
            struct virtio_input_event *ev =
                (struct virtio_input_event *) (g_ring_virt + 0x100u + id * 8u);
            handle_event(ev);
        }
        g_last_used++;
        uint16_t ai = *avail_idxp;
        avail_ring[ai % g_qsz] = (uint16_t) id;
        __asm__ volatile ("mfence" ::: "memory");
        *avail_idxp = (uint16_t) (ai + 1u);
        notify_queue(0u);
    }
}
