#include "keyboard.h"
#include "../arch/x86_64/io.h"

#include <stdint.h>
#include <stddef.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_STATUS_OUTPUT_FULL 0x01

/* Scan code set 1 — base (no shift) */
static const char sc_base[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a', 's',
    'd','f','g','h','j','k','l',';','\'','`',  0, '\\','z','x','c','v',
    'b','n','m',',','.','/',  0,  '*', 0,  ' ', 0,   0,   0,   0,   0,   0,
};

/* Scan code set 1 — shifted */
static const char sc_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0, 'A', 'S',
    'D','F','G','H','J','K','L',':', '"','~',  0, '|', 'Z','X','C','V',
    'B','N','M','<', '>','?',  0,  '*', 0,  ' ', 0,   0,   0,   0,   0,   0,
};

static int shift_down = 0;
static int caps_lock  = 0;

#define RING_SIZE 256
static char   ring[RING_SIZE];
static size_t head = 0;            /* written by IRQ                         */
static size_t tail = 0;            /* written by kernel sys_read             */

static void ring_push(char c) {
    size_t h = __atomic_load_n(&head, __ATOMIC_RELAXED);
    size_t t = __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
    size_t next = (h + 1) % RING_SIZE;
    if (next == t) return;          /* full — drop                           */
    ring[h] = c;
    __atomic_store_n(&head, next, __ATOMIC_RELEASE);
}

size_t stdin_try_read(char *buf, size_t max) {
    size_t n = 0;
    while (n < max) {
        size_t t = __atomic_load_n(&tail, __ATOMIC_RELAXED);
        size_t h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
        if (t == h) break;
        buf[n++] = ring[t];
        __atomic_store_n(&tail, (t + 1) % RING_SIZE, __ATOMIC_RELEASE);
    }
    return n;
}

int stdin_has_data(void) {
    size_t h = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
    size_t t = __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
    return h != t;
}

void keyboard_init(void) {
    while (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) {
        (void) inb(PS2_DATA);
    }
}

void keyboard_ps2_handle_byte(uint8_t sc) {
    int release = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;

    if (code == 0x2A || code == 0x36) { shift_down = !release; return; }
    if (code == 0x3A && !release)     { caps_lock  = !caps_lock;  return; }

    if (release) return;

    int up = shift_down;
    char base = sc_base[code];
    if (caps_lock && base >= 'a' && base <= 'z') up = !up;

    char c = up ? sc_shift[code] : base;
    if (c) ring_push(c);
}

void keyboard_handle_irq(void) {
    keyboard_ps2_handle_byte(inb(PS2_DATA));
}
