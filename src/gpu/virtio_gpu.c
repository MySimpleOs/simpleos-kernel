#include "virtio_gpu.h"
#include "virtio.h"

#include "../kprintf.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

extern volatile struct limine_hhdm_request hhdm_request;

/* --- VirtIO-GPU command codes --- */
enum {
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO       = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D     = 0x0101,
    VIRTIO_GPU_CMD_SET_SCANOUT            = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH         = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D    = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING= 0x0106,

    VIRTIO_GPU_RESP_OK_NODATA             = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO       = 0x1101,
};

enum {
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
};

#define SCANOUT_ID  0
#define RESOURCE_ID 1

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct {
        struct virtio_gpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[16];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* ------------- driver state ------------- */

static struct virtio_device dev;
static int            ready      = 0;

static uint32_t       disp_w     = 0;
static uint32_t       disp_h     = 0;
static uint32_t       disp_pitch = 0;

/* Backing buffer for the single RGBA resource. pmm gives us pages that are
 * contiguous in HHDM, so we allocate them in one batch and trust the run. */
static uint64_t       backing_phys = 0;
static uint64_t       backing_len  = 0;
static uint32_t      *backing      = NULL;

/* Command buffers live in pmm-allocated pages so we can take their phys
 * address cheaply via HHDM subtraction. Static .bss arrays live in the
 * kernel image — their virt→phys translation requires the kernel address
 * request, which we can avoid entirely by allocating in HHDM land. */
static uint8_t *cmd_buf = NULL;
static uint8_t *rsp_buf = NULL;
static uint64_t cmd_phys = 0;
static uint64_t rsp_phys = 0;

static void submit_two_desc(uint32_t req_len, uint32_t rsp_len) {
    struct virtio_queue *q = &dev.queues[0];
    q->desc[0].addr  = cmd_phys;
    q->desc[0].len   = req_len;
    q->desc[0].flags = VIRTQ_DESC_F_NEXT;
    q->desc[0].next  = 1;

    q->desc[1].addr  = rsp_phys;
    q->desc[1].len   = rsp_len;
    q->desc[1].flags = VIRTQ_DESC_F_WRITE;
    q->desc[1].next  = 0;

    virtio_submit_sync(&dev, 0, 0);
}

static int expect_ok(uint32_t actual, uint32_t expected, const char *where) {
    if (actual == expected) return 0;
    kprintf("[virtio-gpu] %s: unexpected response 0x%x (wanted 0x%x)\n",
            where, (unsigned) actual, (unsigned) expected);
    return -1;
}

static int get_display_info(void) {
    struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *) cmd_buf;
    for (size_t i = 0; i < sizeof(*hdr); i++) cmd_buf[i] = 0;
    hdr->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    submit_two_desc(sizeof(*hdr), sizeof(struct virtio_gpu_resp_display_info));

    struct virtio_gpu_resp_display_info *r =
        (struct virtio_gpu_resp_display_info *) rsp_buf;
    if (expect_ok(r->hdr.type, VIRTIO_GPU_RESP_OK_DISPLAY_INFO, "GET_DISPLAY_INFO")) {
        return -1;
    }

    /* Use pmodes[0] as our display. Everything else is multi-monitor
     * territory, which ROADMAP §4 handles. */
    if (!r->pmodes[0].enabled) {
        kprintf("[virtio-gpu] no enabled displays\n");
        return -1;
    }
    disp_w     = r->pmodes[0].r.width;
    disp_h     = r->pmodes[0].r.height;
    disp_pitch = disp_w * 4;

    kprintf("[virtio-gpu] display 0: %ux%u enabled\n",
            (unsigned) disp_w, (unsigned) disp_h);
    return 0;
}

static int resource_create_2d(void) {
    struct virtio_gpu_resource_create_2d *req =
        (struct virtio_gpu_resource_create_2d *) cmd_buf;
    for (size_t i = 0; i < sizeof(*req); i++) cmd_buf[i] = 0;
    req->hdr.type     = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    req->resource_id  = RESOURCE_ID;
    req->format       = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    req->width        = disp_w;
    req->height       = disp_h;

    submit_two_desc(sizeof(*req), sizeof(struct virtio_gpu_ctrl_hdr));

    struct virtio_gpu_ctrl_hdr *rsp = (struct virtio_gpu_ctrl_hdr *) rsp_buf;
    return expect_ok(rsp->type, VIRTIO_GPU_RESP_OK_NODATA, "RESOURCE_CREATE_2D");
}

static int resource_attach_backing(void) {
    /* Grab a physically-contiguous block so a single mem_entry describes
     * the whole backing to the device. The block is HHDM-mapped already,
     * so the CPU-side pointer is just phys + hhdm. */
    backing_len = (uint64_t) disp_w * disp_h * 4;
    backing_len = (backing_len + 4095) & ~4095ull;

    uint64_t pages = backing_len / 4096;
    backing_phys = pmm_alloc_contig(pages);
    if (!backing_phys) {
        kprintf("[virtio-gpu] could not allocate %u contiguous pages for backing\n",
                (unsigned) pages);
        return -1;
    }

    /* Map the same span into HHDM (already there) and keep a virt pointer. */
    uint64_t hhdm = hhdm_request.response ? hhdm_request.response->offset : 0;
    backing = (uint32_t *) (backing_phys + hhdm);

    /* Zero the backing. */
    for (uint64_t i = 0; i < backing_len / 4; i++) backing[i] = 0xFF101820;

    struct virtio_gpu_resource_attach_backing *req =
        (struct virtio_gpu_resource_attach_backing *) cmd_buf;
    for (size_t i = 0; i < sizeof(*req) + sizeof(struct virtio_gpu_mem_entry); i++) {
        cmd_buf[i] = 0;
    }
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    req->resource_id = RESOURCE_ID;
    req->nr_entries  = 1;

    struct virtio_gpu_mem_entry *ent =
        (struct virtio_gpu_mem_entry *) (cmd_buf + sizeof(*req));
    ent->addr   = backing_phys;
    ent->length = (uint32_t) backing_len;

    submit_two_desc(sizeof(*req) + sizeof(*ent),
                    sizeof(struct virtio_gpu_ctrl_hdr));

    struct virtio_gpu_ctrl_hdr *rsp = (struct virtio_gpu_ctrl_hdr *) rsp_buf;
    return expect_ok(rsp->type, VIRTIO_GPU_RESP_OK_NODATA,
                     "RESOURCE_ATTACH_BACKING");
}

static int set_scanout(void) {
    struct virtio_gpu_set_scanout *req = (struct virtio_gpu_set_scanout *) cmd_buf;
    for (size_t i = 0; i < sizeof(*req); i++) cmd_buf[i] = 0;
    req->hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    req->r.x         = 0;
    req->r.y         = 0;
    req->r.width     = disp_w;
    req->r.height    = disp_h;
    req->scanout_id  = SCANOUT_ID;
    req->resource_id = RESOURCE_ID;

    submit_two_desc(sizeof(*req), sizeof(struct virtio_gpu_ctrl_hdr));

    struct virtio_gpu_ctrl_hdr *rsp = (struct virtio_gpu_ctrl_hdr *) rsp_buf;
    return expect_ok(rsp->type, VIRTIO_GPU_RESP_OK_NODATA, "SET_SCANOUT");
}

void virtio_gpu_present(void) {
    if (!ready) return;

    struct virtio_gpu_transfer_to_host_2d *xfer =
        (struct virtio_gpu_transfer_to_host_2d *) cmd_buf;
    for (size_t i = 0; i < sizeof(*xfer); i++) cmd_buf[i] = 0;
    xfer->hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer->r.x         = 0;
    xfer->r.y         = 0;
    xfer->r.width     = disp_w;
    xfer->r.height    = disp_h;
    xfer->offset      = 0;
    xfer->resource_id = RESOURCE_ID;

    submit_two_desc(sizeof(*xfer), sizeof(struct virtio_gpu_ctrl_hdr));

    struct virtio_gpu_resource_flush *flush =
        (struct virtio_gpu_resource_flush *) cmd_buf;
    for (size_t i = 0; i < sizeof(*flush); i++) cmd_buf[i] = 0;
    flush->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush->r.x         = 0;
    flush->r.y         = 0;
    flush->r.width     = disp_w;
    flush->r.height    = disp_h;
    flush->resource_id = RESOURCE_ID;

    submit_two_desc(sizeof(*flush), sizeof(struct virtio_gpu_ctrl_hdr));
}

int virtio_gpu_init(struct pci_device *pci) {
    /* Allocate scratch command + response buffers in HHDM-accessible
     * physical pages before any submit_two_desc call. */
    uint64_t hhdm = hhdm_request.response ? hhdm_request.response->offset : 0;
    cmd_phys = pmm_alloc_page();
    rsp_phys = pmm_alloc_page();
    cmd_buf  = (uint8_t *) (cmd_phys + hhdm);
    rsp_buf  = (uint8_t *) (rsp_phys + hhdm);

    if (virtio_init(&dev, pci) != 0) return -1;
    virtio_reset(&dev);
    if (virtio_negotiate(&dev, 0, 0) != 0)  return -1;
    if (virtio_queue_setup(&dev, 0) != 0)   return -1;
    virtio_set_status(&dev, VIRTIO_STATUS_DRIVER_OK);

    if (get_display_info()       != 0) return -1;
    if (resource_create_2d()     != 0) return -1;
    if (resource_attach_backing()!= 0) return -1;
    if (set_scanout()            != 0) return -1;

    ready = 1;
    virtio_gpu_present();
    kprintf("[virtio-gpu] ready: %ux%u @ backing %p (%u KiB)\n",
            (unsigned) disp_w, (unsigned) disp_h,
            (void *) backing,
            (unsigned) (backing_len / 1024));
    return 0;
}

int       virtio_gpu_ready (void) { return ready; }
uint32_t *virtio_gpu_backbuffer(void) { return backing; }
uint32_t  virtio_gpu_width (void) { return disp_w; }
uint32_t  virtio_gpu_height(void) { return disp_h; }
uint32_t  virtio_gpu_pitch (void) { return disp_pitch; }
