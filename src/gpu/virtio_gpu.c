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
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO        = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      = 0x0101,
    VIRTIO_GPU_CMD_SET_SCANOUT             = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH          = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,

    VIRTIO_GPU_RESP_OK_NODATA              = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO        = 0x1101,
};

enum {
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
};

#define SCANOUT_ID      0
#define RESOURCE_ID_A   1
#define RESOURCE_ID_B   2
#define BUFFER_COUNT    2

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

/* Two full-screen buffers — ping-pong between front (on scanout) and back
 * (being drawn). virtio_gpu_backbuffer() returns buffers[back]. */
static uint32_t      *buffers     [BUFFER_COUNT] = { NULL, NULL };
static uint64_t       buffer_phys [BUFFER_COUNT] = { 0,    0    };
static uint64_t       buffer_len                  = 0;
static uint32_t       resource_ids[BUFFER_COUNT] = { RESOURCE_ID_A, RESOURCE_ID_B };
static int            back_idx                   = 1;   /* drawing target    */
static int            front_idx                  = 0;   /* scanout source    */

/* Scratch command + response buffers in HHDM-accessible physical pages. */
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

static int resource_create_2d(uint32_t id) {
    struct virtio_gpu_resource_create_2d *req =
        (struct virtio_gpu_resource_create_2d *) cmd_buf;
    for (size_t i = 0; i < sizeof(*req); i++) cmd_buf[i] = 0;
    req->hdr.type     = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    req->resource_id  = id;
    req->format       = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    req->width        = disp_w;
    req->height       = disp_h;

    submit_two_desc(sizeof(*req), sizeof(struct virtio_gpu_ctrl_hdr));

    struct virtio_gpu_ctrl_hdr *rsp = (struct virtio_gpu_ctrl_hdr *) rsp_buf;
    return expect_ok(rsp->type, VIRTIO_GPU_RESP_OK_NODATA, "RESOURCE_CREATE_2D");
}

static int resource_attach_backing_for(uint32_t id, uint64_t phys, uint64_t len) {
    struct virtio_gpu_resource_attach_backing *req =
        (struct virtio_gpu_resource_attach_backing *) cmd_buf;
    for (size_t i = 0; i < sizeof(*req) + sizeof(struct virtio_gpu_mem_entry); i++) {
        cmd_buf[i] = 0;
    }
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    req->resource_id = id;
    req->nr_entries  = 1;

    struct virtio_gpu_mem_entry *ent =
        (struct virtio_gpu_mem_entry *) (cmd_buf + sizeof(*req));
    ent->addr   = phys;
    ent->length = (uint32_t) len;

    submit_two_desc(sizeof(*req) + sizeof(*ent),
                    sizeof(struct virtio_gpu_ctrl_hdr));

    struct virtio_gpu_ctrl_hdr *rsp = (struct virtio_gpu_ctrl_hdr *) rsp_buf;
    return expect_ok(rsp->type, VIRTIO_GPU_RESP_OK_NODATA,
                     "RESOURCE_ATTACH_BACKING");
}

static int set_scanout_for(uint32_t id) {
    struct virtio_gpu_set_scanout *req = (struct virtio_gpu_set_scanout *) cmd_buf;
    for (size_t i = 0; i < sizeof(*req); i++) cmd_buf[i] = 0;
    req->hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    req->r.x         = 0;
    req->r.y         = 0;
    req->r.width     = disp_w;
    req->r.height    = disp_h;
    req->scanout_id  = SCANOUT_ID;
    req->resource_id = id;

    submit_two_desc(sizeof(*req), sizeof(struct virtio_gpu_ctrl_hdr));

    struct virtio_gpu_ctrl_hdr *rsp = (struct virtio_gpu_ctrl_hdr *) rsp_buf;
    return expect_ok(rsp->type, VIRTIO_GPU_RESP_OK_NODATA, "SET_SCANOUT");
}

static void transfer_for(uint32_t id) {
    struct virtio_gpu_transfer_to_host_2d *xfer =
        (struct virtio_gpu_transfer_to_host_2d *) cmd_buf;
    for (size_t i = 0; i < sizeof(*xfer); i++) cmd_buf[i] = 0;
    xfer->hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer->r.width     = disp_w;
    xfer->r.height    = disp_h;
    xfer->resource_id = id;

    submit_two_desc(sizeof(*xfer), sizeof(struct virtio_gpu_ctrl_hdr));
}

static void flush_for(uint32_t id) {
    struct virtio_gpu_resource_flush *flush =
        (struct virtio_gpu_resource_flush *) cmd_buf;
    for (size_t i = 0; i < sizeof(*flush); i++) cmd_buf[i] = 0;
    flush->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush->r.width     = disp_w;
    flush->r.height    = disp_h;
    flush->resource_id = id;

    submit_two_desc(sizeof(*flush), sizeof(struct virtio_gpu_ctrl_hdr));
}

void virtio_gpu_present(void) {
    if (!ready) return;

    /* Single-resource pipeline: scanout is bound to resource 0 once at
     * init. Each present pushes the guest backing to the host-side copy
     * of that same resource and flushes to refresh the display.
     * Swapping resources with SET_SCANOUT per frame made QEMU's host
     * display flicker (visible on a 60 Hz monitor even with 60 Hz
     * guest pacing). The second resource/backing stays allocated and
     * unused — cheap insurance for a future triple-buffer path. */
    transfer_for(resource_ids[0]);
    flush_for(resource_ids[0]);
}

int virtio_gpu_init(struct pci_device *pci) {
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

    if (get_display_info()      != 0) return -1;

    /* Allocate both full-screen backings and create matching resources. */
    buffer_len = (uint64_t) disp_w * disp_h * 4;
    buffer_len = (buffer_len + 4095) & ~4095ull;
    uint64_t pages = buffer_len / 4096;

    for (int i = 0; i < BUFFER_COUNT; i++) {
        buffer_phys[i] = pmm_alloc_contig(pages);
        if (!buffer_phys[i]) {
            kprintf("[virtio-gpu] could not allocate %u contiguous pages for buffer %d\n",
                    (unsigned) pages, i);
            return -1;
        }
        buffers[i] = (uint32_t *) (buffer_phys[i] + hhdm);
        if (resource_create_2d(resource_ids[i]) != 0) return -1;
        if (resource_attach_backing_for(resource_ids[i],
                                        buffer_phys[i], buffer_len) != 0) {
            return -1;
        }
    }

    /* Bind scanout 0 → resource 0 exactly once; stays put for the life
     * of the driver. Guest always draws into buffers[0] and the back
     * pointer never swaps. See virtio_gpu_present(). */
    front_idx = 0;
    back_idx  = 0;
    if (set_scanout_for(resource_ids[0]) != 0) return -1;

    ready = 1;
    kprintf("[virtio-gpu] ready: %ux%u, single-buffer scanout (%u KiB backing)\n",
            (unsigned) disp_w, (unsigned) disp_h,
            (unsigned) (buffer_len / 1024));
    return 0;
}

int       virtio_gpu_ready (void) { return ready; }
uint32_t *virtio_gpu_backbuffer(void) {
    return ready ? buffers[back_idx] : NULL;
}
uint32_t  virtio_gpu_width (void) { return disp_w; }
uint32_t  virtio_gpu_height(void) { return disp_h; }
uint32_t  virtio_gpu_pitch (void) { return disp_pitch; }
