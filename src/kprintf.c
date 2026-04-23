#include "kprintf.h"
#include "arch/x86_64/serial.h"

#include <stdint.h>
#include <stddef.h>

static void emit_str(const char *s) {
    serial_puts(s);
}

static void emit_padded(const char *s, int width, char pad) {
    size_t len = 0;
    while (s[len]) len++;
    while ((int) len < width) {
        serial_putc(pad);
        width--;
    }
    emit_str(s);
}

static void utoa(uint64_t v, unsigned base, int upper, char *out) {
    static const char *lower = "0123456789abcdef";
    static const char *upperd = "0123456789ABCDEF";
    const char *digits = upper ? upperd : lower;

    char buf[32];
    size_t i = 0;
    if (v == 0) {
        buf[i++] = '0';
    } else {
        while (v) {
            buf[i++] = digits[v % base];
            v /= base;
        }
    }
    /* buf is LSB-first; reverse into out */
    size_t j = 0;
    while (i) {
        out[j++] = buf[--i];
    }
    out[j] = '\0';
}

static void itoa_signed(int64_t v, char *out) {
    if (v < 0) {
        out[0] = '-';
        utoa((uint64_t) -v, 10, 0, out + 1);
    } else {
        utoa((uint64_t) v, 10, 0, out);
    }
}

void kvprintf(const char *fmt, va_list ap) {
    char buf[32];

    while (*fmt) {
        if (*fmt != '%') {
            serial_putc(*fmt++);
            continue;
        }
        fmt++;  /* skip '%' */

        char pad = ' ';
        int width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt) {
            case 'c': {
                char c = (char) va_arg(ap, int);
                serial_putc(c);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                emit_padded(s, width, pad);
                break;
            }
            case 'd':
            case 'i': {
                int v = va_arg(ap, int);
                itoa_signed(v, buf);
                emit_padded(buf, width, pad);
                break;
            }
            case 'u': {
                unsigned v = va_arg(ap, unsigned);
                utoa(v, 10, 0, buf);
                emit_padded(buf, width, pad);
                break;
            }
            case 'x': {
                unsigned v = va_arg(ap, unsigned);
                utoa(v, 16, 0, buf);
                emit_padded(buf, width, pad);
                break;
            }
            case 'X': {
                unsigned v = va_arg(ap, unsigned);
                utoa(v, 16, 1, buf);
                emit_padded(buf, width, pad);
                break;
            }
            case 'p': {
                uintptr_t v = (uintptr_t) va_arg(ap, void *);
                serial_puts("0x");
                utoa(v, 16, 0, buf);
                emit_padded(buf, 16, '0');
                break;
            }
            case '%':
                serial_putc('%');
                break;
            case '\0':
                return;
            default:
                serial_putc('%');
                serial_putc(*fmt);
                break;
        }
        fmt++;
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}
