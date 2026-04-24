#include "ioapic.h"
#include "acpi.h"
#include "../../kprintf.h"
#include "../../mm/vmm.h"

#include <limine.h>
#include <stdint.h>

extern volatile struct limine_hhdm_request hhdm_request;

/* IOAPIC speaks through an indirect register pair:
 *   offset 0x00 IOREGSEL  — select the register to access
 *   offset 0x10 IOWIN     — read/write the selected register
 */
static volatile uint32_t *ioapic_mmio;

static uint32_t ioapic_read(uint32_t reg) {
    ioapic_mmio[0] = reg;
    return ioapic_mmio[4];
}

static void ioapic_write(uint32_t reg, uint32_t value) {
    ioapic_mmio[0] = reg;
    ioapic_mmio[4] = value;
}

void ioapic_init(void) {
    uint64_t hhdm_offset = hhdm_request.response ? hhdm_request.response->offset : 0;
    uint64_t virt        = acpi.ioapic_phys + hhdm_offset;
    mmio_map(virt, acpi.ioapic_phys, 0x1000);
    ioapic_mmio = (volatile uint32_t *) virt;

    uint32_t id  = (ioapic_read(0x00) >> 24) & 0x0F;
    uint32_t ver = ioapic_read(0x01);
    uint32_t max = ((ver >> 16) & 0xFF) + 1;
    kprintf("[ioapic] id=%u version=0x%x entries=%u @ %p\n",
            (unsigned) id, (unsigned) (ver & 0xFF),
            (unsigned) max, ioapic_mmio);
}

void ioapic_set_irq_extended(uint8_t gsi, uint8_t vector, uint8_t destination,
                             int level_trigger, int low_active) {
    uint32_t low = (uint32_t) vector;
    if (low_active) low |= (1u << 13);      /* polarity: active low */
    if (level_trigger) low |= (1u << 15);   /* trigger: level */
    uint32_t high = ((uint32_t) destination) << 24;

    uint32_t reg = 0x10 + (uint32_t) gsi * 2;
    ioapic_write(reg,     low);
    ioapic_write(reg + 1, high);
}
