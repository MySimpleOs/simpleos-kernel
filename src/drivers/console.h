#pragma once

#include <stddef.h>

/* Framebuffer text console.
 *
 * One-time init with the Limine framebuffer geometry, then bytes are blitted
 * through 8x8 glyphs. Handles \n / \r / \b; everything else printable goes
 * through the font table. When the cursor hits the last row we scroll by
 * shifting every pixel row up 8 pixels and clearing the bottom strip. */

void console_init(void);
void console_putc(char c);
void console_write(const char *buf, size_t count);
void console_puts(const char *s);
