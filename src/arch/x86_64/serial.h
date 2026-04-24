#pragma once

#include <stddef.h>

/* Initialize COM1 (0x3F8) at 38400 baud, 8N1, FIFO enabled. Must be called
 * before any serial_write / kprintf use. Safe to call more than once. */
void serial_init(void);

void serial_putc(char c);
void serial_write(const char *s, size_t len);
void serial_puts(const char *s);

/* Non-blocking RX on COM1. Returns byte 0–255, or -1 if no data. */
int serial_try_getc(void);
