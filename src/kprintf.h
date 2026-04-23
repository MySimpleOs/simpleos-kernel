#pragma once

#include <stdarg.h>

/* Minimal kernel printf family. Supported conversions:
 *   %c %s %% %d %i %u %x %X %p
 * Supported flags: '0' zero-pad, field width (e.g. %08x). No precision,
 * no longs/shorts — width is fixed at int/pointer. Writes to COM1 serial.
 *
 * serial_init() must have been called first. */

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);
