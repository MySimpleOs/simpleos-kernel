#include "serial.h"
#include "io.h"

#define COM1 0x3F8

/* 16550 UART register offsets from the base port. */
enum {
    UART_DATA       = 0,  /* RX buffer (read) / TX holding (write)  */
    UART_INT_ENABLE = 1,  /* interrupt enable                       */
    UART_FIFO_CTRL  = 2,  /* FIFO control                           */
    UART_LINE_CTRL  = 3,  /* line control                           */
    UART_MODEM_CTRL = 4,  /* modem control                          */
    UART_LINE_STAT  = 5,  /* line status                            */
};

/* LSR bit 5: transmitter holding register empty — safe to write next byte. */
#define LSR_THR_EMPTY 0x20

static int initialized = 0;

void serial_init(void) {
    if (initialized) {
        return;
    }

    outb(COM1 + UART_INT_ENABLE, 0x00);  /* disable interrupts                 */
    outb(COM1 + UART_LINE_CTRL,  0x80);  /* enable DLAB to set baud divisor    */
    outb(COM1 + UART_DATA,       0x03);  /* divisor lo: 115200 / 3 = 38400 bps */
    outb(COM1 + UART_INT_ENABLE, 0x00);  /* divisor hi = 0                     */
    outb(COM1 + UART_LINE_CTRL,  0x03);  /* 8 bits, no parity, 1 stop, DLAB=0  */
    outb(COM1 + UART_FIFO_CTRL,  0xC7);  /* enable FIFO, clear, 14-byte thresh */
    outb(COM1 + UART_MODEM_CTRL, 0x0B);  /* IRQ enabled, RTS/DSR set           */

    initialized = 1;
}

static inline int transmit_empty(void) {
    return inb(COM1 + UART_LINE_STAT) & LSR_THR_EMPTY;
}

void serial_putc(char c) {
    /* Convert bare LF to CRLF so terminals scroll cleanly. */
    if (c == '\n') {
        while (!transmit_empty()) { }
        outb(COM1 + UART_DATA, '\r');
    }
    while (!transmit_empty()) { }
    outb(COM1 + UART_DATA, (uint8_t) c);
}

void serial_write(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        serial_putc(s[i]);
    }
}

void serial_puts(const char *s) {
    while (*s) {
        serial_putc(*s++);
    }
}
