#include "pic.h"
#include "io.h"
#include "../../kprintf.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT_ICW4 0x11

void pic_disable(void) {
    /* Start init sequence on both PICs. */
    outb(PIC1_CMD,  ICW1_INIT_ICW4); io_wait();
    outb(PIC2_CMD,  ICW1_INIT_ICW4); io_wait();

    /* ICW2: remap IRQ bases out of the exception range. */
    outb(PIC1_DATA, 0x20); io_wait();  /* master IRQs 0..7  -> 0x20..0x27 */
    outb(PIC2_DATA, 0x28); io_wait();  /* slave  IRQs 8..15 -> 0x28..0x2F */

    /* ICW3: cascade wiring. */
    outb(PIC1_DATA, 0x04); io_wait();  /* slave on IRQ2            */
    outb(PIC2_DATA, 0x02); io_wait();  /* slave cascade identity   */

    /* ICW4: 8086 mode. */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Mask every IRQ on both controllers. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    kprintf("[pic] 8259 remapped to 0x20-0x2F and fully masked\n");
}
