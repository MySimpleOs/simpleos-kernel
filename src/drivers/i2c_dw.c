#include "i2c_dw.h"

#include "../kprintf.h"
#include "../mm/vmm.h"
#include "../pci/pci.h"

#include <limine.h>

#include <stddef.h>
#include <stdint.h>

extern volatile struct limine_hhdm_request hhdm_request;

static uint64_t hhdm_off(void) {
    return hhdm_request.response ? hhdm_request.response->offset : 0;
}

static void dmb(void) { __asm__ volatile ("mfence" ::: "memory"); }

#define DW_IC_CON           0x00u
#define DW_IC_TAR           0x04u
#define DW_IC_DATA_CMD      0x10u
#define DW_IC_SS_SCL_HCNT   0x14u
#define DW_IC_SS_SCL_LCNT   0x18u
#define DW_IC_INTR_MASK     0x30u
#define DW_IC_RAW_INTR_STAT 0x34u
#define DW_IC_RX_TL         0x38u
#define DW_IC_TX_TL         0x3cu
#define DW_IC_CLR_INTR      0x40u
#define DW_IC_CLR_TX_ABRT   0x54u
#define DW_IC_CLR_STOP_DET  0x60u
#define DW_IC_ENABLE        0x6cu
#define DW_IC_STATUS        0x70u
#define DW_IC_TXFLR         0x74u
#define DW_IC_RXFLR         0x78u
#define DW_IC_TX_ABRT_SOURCE 0x80u
#define DW_IC_ENABLE_STATUS 0x9cu
#define DW_IC_FS_SPKLEN     0xa0u
#define DW_IC_COMP_TYPE     0xfcu

#define DW_IC_CON_MASTER      (1u << 0)
#define DW_IC_CON_SPEED_STD   (1u << 1)
#define DW_IC_CON_RESTART_EN  (1u << 5)
#define DW_IC_CON_SLAVE_DIS   (1u << 6)

#define DW_IC_DATA_CMD_READ  (1u << 8)
#define DW_IC_DATA_CMD_STOP   (1u << 9)
#define DW_IC_DATA_CMD_RESTART (1u << 10)

#define DW_IC_INTR_TX_EMPTY   (1u << 4)
#define DW_IC_INTR_TX_ABRT    (1u << 6)
#define DW_IC_INTR_RX_FULL    (1u << 2)
#define DW_IC_INTR_STOP_DET    (1u << 9)

#define DW_IC_ENABLE_BIT      (1u << 0)
#define DW_IC_STATUS_RFNE     (1u << 3)
#define DW_IC_STATUS_TFE      (1u << 2)

#define DW_COMP_TYPE_EXPECT   0x44570140u

static volatile uint32_t *g_regs;
static uint32_t           g_tx_depth = 32;
static uint32_t           g_rx_depth = 32;

static uint32_t rd(unsigned off) {
    return g_regs[off / 4u];
}

static void wr(unsigned off, uint32_t v) {
    g_regs[off / 4u] = v;
    dmb();
}

static void clr_all_intr(void) {
    (void) rd(DW_IC_CLR_INTR);
    (void) rd(DW_IC_CLR_TX_ABRT);
    (void) rd(DW_IC_CLR_STOP_DET);
}

static int wait_bits(unsigned off, uint32_t mask, uint32_t want, int spins) {
    for (int i = 0; i < spins; i++) {
        if ((rd(off) & mask) == want)
            return 0;
        __asm__ volatile ("pause");
    }
    return -1;
}

static int dw_disable(void) {
    if (!g_regs)
        return 0;
    wr(DW_IC_ENABLE, 0u);
    (void) wait_bits(DW_IC_ENABLE_STATUS, 1u, 0u, 200000);
    return 0;
}

static int dw_enable_controller(void) {
    wr(DW_IC_ENABLE, 0u);
    (void) wait_bits(DW_IC_ENABLE_STATUS, 1u, 0u, 200000);
    clr_all_intr();
    wr(DW_IC_INTR_MASK, 0u);

    wr(DW_IC_SS_SCL_HCNT, 480u);
    wr(DW_IC_SS_SCL_LCNT, 520u);
    wr(DW_IC_FS_SPKLEN, 2u);

    uint32_t con = DW_IC_CON_MASTER | DW_IC_CON_SPEED_STD | DW_IC_CON_RESTART_EN | DW_IC_CON_SLAVE_DIS;
    wr(DW_IC_CON, con);
    wr(DW_IC_TX_TL, g_tx_depth / 2u - 1u);
    wr(DW_IC_RX_TL, 0u);

    wr(DW_IC_ENABLE, DW_IC_ENABLE_BIT);
    if (wait_bits(DW_IC_ENABLE_STATUS, 1u, 1u, 200000) != 0)
        return -1;
    return 0;
}

void i2c_dw_unbind(void) {
    if (g_regs) {
        dw_disable();
        g_regs = NULL;
    }
}

int i2c_dw_bound(void) { return g_regs != NULL ? 1 : 0; }

static int pick_mmio_bar(const struct pci_device *d, uint64_t *phys, uint64_t *sz) {
    for (int i = 0; i < PCI_BAR_COUNT; i++) {
        if (d->bars[i].type != PCI_BAR_MEM) continue;
        if (d->bars[i].size < 0x1000ull) continue;
        if (d->bars[i].base == 0ull) continue;
        *phys = d->bars[i].base;
        *sz   = d->bars[i].size;
        return 0;
    }
    return -1;
}

int i2c_dw_bind_pci(const struct pci_device *pci) {
    i2c_dw_unbind();
    if (!pci) return -1;
    uint64_t phys, sz;
    if (pick_mmio_bar(pci, &phys, &sz) != 0)
        return -1;
    uint32_t map = (uint32_t) (sz > 0x10000ull ? 0x10000ull : sz);
    uint64_t virt = phys + hhdm_off();
    mmio_map(virt, phys, map);
    g_regs = (volatile uint32_t *) (uintptr_t) virt;

    uint32_t ctype = rd(DW_IC_COMP_TYPE);
    if (ctype != DW_COMP_TYPE_EXPECT) {
        kprintf("[i2c-dw] COMP_TYPE=0x%08x (expected 0x%08x) — not DesignWare?\n", (unsigned) ctype,
                (unsigned) DW_COMP_TYPE_EXPECT);
        g_regs = NULL;
        return -1;
    }

    uint32_t param = rd(0xf4u); /* IC_COMP_PARAM_1 */
    g_tx_depth = ((param >> 16) & 0xffu) + 1u;
    g_rx_depth = ((param >> 8) & 0xffu) + 1u;
    if (g_tx_depth < 2u) g_tx_depth = 32u;
    if (g_rx_depth < 2u) g_rx_depth = 32u;

    kprintf("[i2c-dw] PCI %02x:%02x.%u MMIO phys=0x%llx tx/rx fifo=%u/%u\n", (unsigned) pci->bus,
            (unsigned) pci->slot, (unsigned) pci->func, (unsigned long long) phys, (unsigned) g_tx_depth,
            (unsigned) g_rx_depth);
    return 0;
}

static int wait_stop_or_abort(int spins) {
    for (int i = 0; i < spins; i++) {
        uint32_t raw = rd(DW_IC_RAW_INTR_STAT);
        if (raw & DW_IC_INTR_TX_ABRT) {
            (void) rd(DW_IC_CLR_TX_ABRT);
            (void) rd(DW_IC_TX_ABRT_SOURCE);
            return -1;
        }
        if (raw & DW_IC_INTR_STOP_DET) {
            (void) rd(DW_IC_CLR_STOP_DET);
            return 0;
        }
        __asm__ volatile ("pause");
    }
    return -2;
}

static int wait_tx_space(int spins) {
    for (int i = 0; i < spins; i++) {
        uint32_t tfl = rd(DW_IC_TXFLR);
        if (tfl < g_tx_depth)
            return 0;
        __asm__ volatile ("pause");
    }
    return -1;
}

static int wait_rx_data(int spins) {
    for (int i = 0; i < spins; i++) {
        if (rd(DW_IC_STATUS) & DW_IC_STATUS_RFNE)
            return 0;
        __asm__ volatile ("pause");
    }
    return -1;
}

int i2c_dw_write_read(uint8_t addr7, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen) {
    if (!g_regs || !w || !r || wlen == 0 || rlen == 0)
        return -1;

    dw_disable();
    if (dw_enable_controller() != 0)
        return -1;

    wr(DW_IC_TAR, (uint32_t) addr7);

    for (size_t i = 0; i < wlen; i++) {
        if (wait_tx_space(500000) != 0)
            goto fail;
        uint32_t cmd = (uint32_t) w[i];
        if (i == wlen - 1u && rlen > 0)
            cmd |= DW_IC_DATA_CMD_RESTART;
        wr(DW_IC_DATA_CMD, cmd);
    }

    for (size_t j = 0; j < rlen; j++) {
        if (wait_tx_space(500000) != 0)
            goto fail;
        uint32_t cmd = DW_IC_DATA_CMD_READ;
        if (j == rlen - 1u)
            cmd |= DW_IC_DATA_CMD_STOP;
        wr(DW_IC_DATA_CMD, cmd);
    }

    if (wait_stop_or_abort(8000000) != 0)
        goto fail;

    for (size_t j = 0; j < rlen; j++) {
        if (wait_rx_data(8000000) != 0)
            goto fail;
        r[j] = (uint8_t) (rd(DW_IC_DATA_CMD) & 0xffu);
    }

    dw_disable();
    return 0;
fail:
    dw_disable();
    return -1;
}

int i2c_dw_master_write(uint8_t addr7, const uint8_t *w, size_t wlen) {
    if (!g_regs || !w || wlen == 0)
        return -1;

    dw_disable();
    if (dw_enable_controller() != 0)
        return -1;

    wr(DW_IC_TAR, (uint32_t) addr7);

    for (size_t i = 0; i < wlen; i++) {
        if (wait_tx_space(500000) != 0)
            goto fail;
        uint32_t cmd = (uint32_t) w[i];
        if (i == wlen - 1u)
            cmd |= DW_IC_DATA_CMD_STOP;
        wr(DW_IC_DATA_CMD, cmd);
    }

    if (wait_stop_or_abort(8000000) != 0)
        goto fail;

    dw_disable();
    return 0;
fail:
    dw_disable();
    return -1;
}

int i2c_dw_master_read(uint8_t addr7, uint8_t *r, size_t rlen) {
    if (!g_regs || !r || rlen == 0)
        return -1;

    dw_disable();
    if (dw_enable_controller() != 0)
        return -1;

    wr(DW_IC_TAR, (uint32_t) addr7);

    for (size_t j = 0; j < rlen; j++) {
        if (wait_tx_space(500000) != 0)
            goto fail;
        uint32_t cmd = DW_IC_DATA_CMD_READ;
        if (j == rlen - 1u)
            cmd |= DW_IC_DATA_CMD_STOP;
        wr(DW_IC_DATA_CMD, cmd);
    }

    if (wait_stop_or_abort(8000000) != 0)
        goto fail;

    for (size_t j = 0; j < rlen; j++) {
        if (wait_rx_data(8000000) != 0)
            goto fail;
        r[j] = (uint8_t) (rd(DW_IC_DATA_CMD) & 0xffu);
    }

    dw_disable();
    return 0;
fail:
    dw_disable();
    return -1;
}
