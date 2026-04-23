/* Limine boot protocol request table.
 *
 * All symbols live in the .limine_requests* sections so the bootloader can
 * discover them between the start and end markers defined below. The
 * attributes keep the linker from discarding them even though the C code
 * never references these symbols by name.
 */

#include <stdint.h>
#include <limine.h>

__attribute__((used, section(".limine_requests")))
volatile LIMINE_BASE_REVISION(3)

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER
