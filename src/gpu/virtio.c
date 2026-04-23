#include "virtio.h"
#include "../kprintf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../panic.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

extern volatile struct limine_hhdm_request hhdm_request;

static uint64_t hhdm_off(void) {
    return hhdm_request.response ? hhdm_request.response->offset : 0;
}

static void *bar_virt(struct pci_device *d, uint8_t bar, uint32_t offset) {
    /* Map the BAR into HHDM on demand. mmio_map is idempotent so repeated
     * calls for the same region are cheap. */
    uint64_t phys = d->bars[bar].base;
    uint64_t size = d->bars[bar].size;
    if (!phys || !size) return NULL;

    uint64_t virt = phys + hhdm_off();
    mmio_map(virt, phys, (size + 0xFFF) & ~0xFFFull);
    return (void *) (virt + offset);
}

/* Walk the PCI capability list (offset 0x34) collecting the four modern
 * VirtIO vendor-specific caps. Returns 0 on success. */
int virtio_init(struct virtio_device *v, struct pci_device *pci) {
    for (size_t i = 0; i < sizeof(v->queues) / sizeof(v->queues[0]); i++) {
        v->queues[i].desc = NULL;
    }
    v->pci               = pci;
    v->common            = NULL;
    v->notify_base       = NULL;
    v->notify_multiplier = 0;
    v->isr               = NULL;
    v->device_cfg        = NULL;

    uint16_t status = pci_cfg_read16(pci, 0x06);
    if (!(status & (1u << 4))) {
        kprintf("[virtio] device has no capability list\n");
        return -1;
    }

    uint8_t cap_ptr = pci_cfg_read8(pci, 0x34) & 0xFC;
    int safety = 32;                         /* bound the cap list walk     */
    while (cap_ptr && safety-- > 0) {
        uint8_t cap_id = pci_cfg_read8(pci, cap_ptr);
        if (cap_id == 0x09) {                /* vendor-specific             */
            uint8_t  cfg_type = pci_cfg_read8(pci, cap_ptr + 3);
            uint8_t  bar      = pci_cfg_read8(pci, cap_ptr + 4);
            uint32_t offset   = pci_cfg_read32(pci, cap_ptr + 8);
            /* length at +12 is only used for notify_cfg sizing */

            switch (cfg_type) {
                case VIRTIO_PCI_CAP_COMMON_CFG:
                    v->common = bar_virt(pci, bar, offset);
                    break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG:
                    v->notify_base = bar_virt(pci, bar, offset);
                    v->notify_multiplier = pci_cfg_read32(pci, cap_ptr + 16);
                    break;
                case VIRTIO_PCI_CAP_ISR_CFG:
                    v->isr = bar_virt(pci, bar, offset);
                    break;
                case VIRTIO_PCI_CAP_DEVICE_CFG:
                    v->device_cfg = bar_virt(pci, bar, offset);
                    break;
            }
        }
        cap_ptr = pci_cfg_read8(pci, cap_ptr + 1) & 0xFC;
    }

    if (!v->common || !v->notify_base) {
        kprintf("[virtio] device %04x:%04x missing modern capabilities\n",
                (unsigned) pci->vendor_id, (unsigned) pci->device_id);
        return -1;
    }

    /* Enable bus-master + memory decoding so the device can DMA. */
    uint16_t cmd = pci_cfg_read16(pci, 0x04);
    cmd |= (1u << 1) | (1u << 2);            /* MEM_EN | BUS_MASTER          */
    pci_cfg_write16(pci, 0x04, cmd);

    return 0;
}

void virtio_reset(struct virtio_device *v) {
    v->common->device_status = 0;
    int spin = 1000000;
    while (v->common->device_status != 0 && spin-- > 0) {
        __asm__ volatile ("pause");
    }
    if (spin <= 0) {
        kprintf("[virtio] reset timeout — status=%x\n",
                (unsigned) v->common->device_status);
    }
}

void virtio_set_status(struct virtio_device *v, uint8_t bit) {
    uint8_t cur = v->common->device_status;
    v->common->device_status = cur | bit;
}

int virtio_negotiate(struct virtio_device *v, uint32_t want_lo, uint32_t want_hi) {
    virtio_set_status(v, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_set_status(v, VIRTIO_STATUS_DRIVER);

    want_hi |= (1u << (VIRTIO_F_VERSION_1 - 32));

    v->common->driver_feature_select = 0;
    v->common->driver_feature        = want_lo;
    v->common->driver_feature_select = 1;
    v->common->driver_feature        = want_hi;

    virtio_set_status(v, VIRTIO_STATUS_FEATURES_OK);
    if (!(v->common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("[virtio] FEATURES_OK rejected\n");
        return -1;
    }
    return 0;
}

static void *alloc_zeroed_pages(size_t bytes, uint64_t *phys_out) {
    size_t pages = (bytes + 4095) / 4096;
    if (pages == 0) pages = 1;
    uint64_t phys_base = pmm_alloc_page();
    for (size_t i = 1; i < pages; i++) {
        uint64_t p = pmm_alloc_page();
        if (p != phys_base + i * 4096) {
            /* PMM doesn't guarantee contiguity; for queue ring bytes this
             * is fine if the whole block fits in one page. For bigger
             * allocations the caller must accept fragmentation — our
             * VIRTIO_QUEUE_SIZE rings fit in one 4KiB page so we never
             * hit this path. */
        }
    }
    *phys_out = phys_base;
    uint8_t *virt = (uint8_t *) (phys_base + hhdm_off());
    for (size_t i = 0; i < pages * 4096; i++) virt[i] = 0;
    return virt;
}

int virtio_queue_setup(struct virtio_device *v, uint16_t q_index) {
    if (q_index >= sizeof(v->queues) / sizeof(v->queues[0])) return -1;
    struct virtio_queue *q = &v->queues[q_index];

    v->common->queue_select = q_index;
    uint16_t max_size = v->common->queue_size;
    if (max_size == 0) {
        kprintf("[virtio] queue %u not available (size=0)\n", (unsigned) q_index);
        return -1;
    }
    uint16_t size = VIRTIO_QUEUE_SIZE;
    if (size > max_size) size = max_size;
    q->size          = size;
    q->last_used_idx = 0;

    /* Descriptors: size * 16 bytes. avail: 4 + size*2 + 2. used: 4 + size*8 + 2.
     * All three fit in a single 4KiB page when size == 64. */
    q->desc  = alloc_zeroed_pages(size * sizeof(struct virtq_desc),  &q->desc_phys);
    q->avail = alloc_zeroed_pages(sizeof(struct virtq_avail),        &q->avail_phys);
    q->used  = alloc_zeroed_pages(sizeof(struct virtq_used),         &q->used_phys);

    v->common->queue_size   = size;
    v->common->queue_desc   = q->desc_phys;
    v->common->queue_driver = q->avail_phys;
    v->common->queue_device = q->used_phys;

    q->queue_notify_off = v->common->queue_notify_off;
    q->notify_ptr = (volatile uint16_t *) (v->notify_base
                    + q->queue_notify_off * v->notify_multiplier);

    v->common->queue_enable = 1;
    return 0;
}

void virtio_submit_sync(struct virtio_device *v, uint16_t q_index,
                        uint16_t head_desc) {
    struct virtio_queue *q = &v->queues[q_index];

    /* Publish the head into the available ring. */
    uint16_t avail_idx = q->avail->idx % q->size;
    q->avail->ring[avail_idx] = head_desc;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;

    /* Notify the device. queue_notify_off * notify_multiplier yields the
     * per-queue doorbell inside the notify BAR. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *q->notify_ptr = q_index;

    /* Poll the used ring until the device has consumed one more entry. */
    while (q->used->idx == q->last_used_idx) {
        __asm__ volatile ("pause");
    }
    q->last_used_idx = q->used->idx;
}
