#include "keyboard.h"
#include "../arch/x86_64/io.h"
#include "../kprintf.h"

#include <stdint.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_STATUS_OUTPUT_FULL 0x01

/* Scan code set 1 — break codes are `make | 0x80`. Entry 0 means "no
 * printable glyph yet". Enough to log typing; a proper keymap layer will
 * come with the TTY work. */
static const char scancode_set1[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a', 's',
    'd','f','g','h','j','k','l',';','\'','`',  0, '\\','z','x','c','v',
    'b','n','m',',','.','/',  0,  '*', 0,  ' ', 0,   0,   0,   0,   0,   0,
};

void keyboard_init(void) {
    /* Drain whatever is currently in the PS/2 output buffer so the first
     * real keypress does not get lost. */
    while (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) {
        (void) inb(PS2_DATA);
    }
    kprintf("[kbd] PS/2 channel 1 ready, listening on IRQ1 (vector 0x%x)\n",
            (unsigned) KEYBOARD_VECTOR);
}

void keyboard_handle_irq(void) {
    uint8_t sc = inb(PS2_DATA);

    if (sc & 0x80) {
        /* release — ignore for now */
        return;
    }

    char c = (sc < 128) ? scancode_set1[sc] : 0;
    if (c) {
        kprintf("[kbd] 0x%x '%c'\n", (unsigned) sc, c);
    } else {
        kprintf("[kbd] 0x%x\n", (unsigned) sc);
    }
}
