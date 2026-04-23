#include "pit.h"
#include "io.h"

#define PIT_CH2_DATA     0x42
#define PIT_CMD          0x43
#define KEYBOARD_PORT_B  0x61   /* gate + speaker bits live here */

void pit_prepare_oneshot(uint16_t ticks) {
    /* bit 0 = gate for channel 2, bit 1 = speaker enable. We want gate on
     * and speaker off so the timer runs silently. */
    uint8_t portb = inb(KEYBOARD_PORT_B);
    portb = (portb & ~0x02) | 0x01;
    outb(KEYBOARD_PORT_B, portb);

    /* Channel 2, access lo+hi, mode 0 (interrupt on terminal count), binary. */
    outb(PIT_CMD,      0xB0);
    outb(PIT_CH2_DATA, (uint8_t) (ticks & 0xFF));
    outb(PIT_CH2_DATA, (uint8_t) ((ticks >> 8) & 0xFF));
}

int pit_oneshot_done(void) {
    /* Bit 5 of port 0x61 mirrors the channel 2 output pin — flips high when
     * the counter reaches zero in mode 0. */
    return (inb(KEYBOARD_PORT_B) & 0x20) != 0;
}
