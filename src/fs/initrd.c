#include "initrd.h"
#include "../kprintf.h"

#include <limine.h>
#include <stdint.h>
#include <stddef.h>

extern volatile struct limine_module_request module_request;
extern volatile struct limine_hhdm_request   hhdm_request;

static const uint8_t *bytes = NULL;
static size_t         len   = 0;

const uint8_t *initrd_bytes(void) { return bytes; }
size_t         initrd_size (void) { return len;   }

int initrd_init(void) {
    if (!module_request.response || module_request.response->module_count == 0) {
        kprintf("[initrd] no modules from bootloader\n");
        return 1;
    }

    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file *m = module_request.response->modules[i];
        kprintf("[initrd] module[%u] path=%s size=%u addr=%p\n",
                (unsigned) i,
                m->path ? m->path : "(null)",
                (unsigned) m->size,
                m->address);

        if (i == 0) {
            /* Limine already hands module files back as HHDM-virtual
             * pointers (unlike rsdp_request.response->address, which is
             * a physical address). Use the pointer directly. */
            bytes = (const uint8_t *) m->address;
            len   = (size_t) m->size;
        }
    }

    if (bytes) {
        kprintf("[initrd] mounted at %p (%u bytes)\n", bytes, (unsigned) len);
        return 0;
    }
    return 1;
}
