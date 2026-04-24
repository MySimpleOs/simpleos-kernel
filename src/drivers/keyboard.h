#pragma once

#include <stddef.h>
#include <stdint.h>

#define KEYBOARD_VECTOR 0x21
#define KEYBOARD_GSI    1

void keyboard_init(void);
void keyboard_handle_irq(void);
/* PS/2 data byte when the controller queues it on the aux IRQ path too. */
void keyboard_ps2_handle_byte(uint8_t sc);

/* Drain up to `max` bytes from the keyboard ring into `buf`. Returns the
 * number of bytes actually copied (0 if empty). Never blocks. */
size_t stdin_try_read(char *buf, size_t max);

/* True when at least one byte is waiting. */
int    stdin_has_data(void);
