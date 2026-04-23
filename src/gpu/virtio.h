#pragma once

#include "../pci/pci.h"

#include <stdint.h>
#include <stddef.h>

/* --- VirtIO capability IDs (vendor-specific PCI cap cfg_type) --- */
#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

/* --- device_status bits --- */
#define VIRTIO_STATUS_ACKNOWLEDGE  1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED      128

#define VIRTIO_F_VERSION_1 32

/* --- descriptor flags --- */
#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2

#define VIRTIO_QUEUE_SIZE 64

struct virtio_pci_common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;

    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __attribute__((packed));

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_QUEUE_SIZE];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VIRTIO_QUEUE_SIZE];
} __attribute__((packed));

struct virtio_queue {
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint64_t            desc_phys;
    uint64_t            avail_phys;
    uint64_t            used_phys;
    uint16_t            size;
    uint16_t            last_used_idx;
    uint16_t            queue_notify_off;
    volatile uint16_t  *notify_ptr;   /* address to write for notify         */
};

struct virtio_device {
    struct pci_device                  *pci;
    volatile struct virtio_pci_common_cfg *common;
    volatile uint8_t                    *notify_base;
    uint32_t                             notify_multiplier;
    volatile uint8_t                    *isr;
    volatile uint8_t                    *device_cfg;

    struct virtio_queue queues[4];
};

/* Find and map the four modern-mode capabilities; returns 0 on success.
 * Caller still needs to do status bring-up (reset → ACK → DRIVER → features
 * → FEATURES_OK) and allocate queues. */
int  virtio_init(struct virtio_device *v, struct pci_device *pci);

/* Hard reset + walk the status flags through ACKNOWLEDGE / DRIVER. */
void virtio_reset(struct virtio_device *v);
void virtio_set_status(struct virtio_device *v, uint8_t bits);

/* Low-level feature negotiation. `want_lo` is bits 0..31 the driver
 * supports; `want_hi` is 32..63. Returns 0 if FEATURES_OK sticks. */
int  virtio_negotiate(struct virtio_device *v, uint32_t want_lo, uint32_t want_hi);

/* Allocate + enable queue q_index. pmm-backed rings aligned to page
 * boundaries. */
int  virtio_queue_setup(struct virtio_device *v, uint16_t q_index);

/* Submit a single (possibly chained) descriptor head and spin-wait until
 * the device writes the corresponding used entry. */
void virtio_submit_sync(struct virtio_device *v, uint16_t q_index,
                        uint16_t head_desc);
