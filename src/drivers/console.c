#include "console.h"
#include "../gpu/display.h"

#include <stdint.h>
#include <stddef.h>

extern const uint8_t font8x8[95][8];

#define GLYPH_W 8
#define GLYPH_H 8

/* Solarized-ish dark. Theming lands in ROADMAP §3. */
#define FG 0xFFE6DBC4u
#define BG 0xFF12181Cu

static uint32_t *pixels     = NULL;
static size_t    pitch_px   = 0;
static size_t    fb_width   = 0;
static size_t    fb_height  = 0;
static size_t    cols       = 0;
static size_t    rows       = 0;
static size_t    cx         = 0;
static size_t    cy         = 0;
static int       ready      = 0;

static void fill_cell(size_t col, size_t row, uint32_t color) {
    size_t x0 = col * GLYPH_W;
    size_t y0 = row * GLYPH_H;
    for (size_t y = 0; y < GLYPH_H; y++) {
        uint32_t *line = pixels + (y0 + y) * pitch_px;
        for (size_t x = 0; x < GLYPH_W; x++) line[x0 + x] = color;
    }
}

static void draw_glyph(size_t col, size_t row, char ch) {
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *g = font8x8[(uint8_t) ch - 0x20];
    size_t x0 = col * GLYPH_W;
    size_t y0 = row * GLYPH_H;
    for (size_t y = 0; y < GLYPH_H; y++) {
        uint32_t *line = pixels + (y0 + y) * pitch_px;
        uint8_t   bits = g[y];
        for (size_t x = 0; x < GLYPH_W; x++) {
            line[x0 + x] = (bits & (1u << x)) ? FG : BG;
        }
    }
}

static void clear_screen(void) {
    for (size_t y = 0; y < fb_height; y++) {
        uint32_t *line = pixels + y * pitch_px;
        for (size_t x = 0; x < fb_width; x++) line[x] = BG;
    }
    cx = cy = 0;
}

static void scroll_up(void) {
    size_t strip = GLYPH_H;
    size_t used_h = rows * GLYPH_H;
    for (size_t y = 0; y + strip < used_h; y++) {
        uint32_t *dst = pixels + y          * pitch_px;
        uint32_t *src = pixels + (y + strip) * pitch_px;
        for (size_t x = 0; x < fb_width; x++) dst[x] = src[x];
    }
    for (size_t y = used_h - strip; y < used_h; y++) {
        uint32_t *line = pixels + y * pitch_px;
        for (size_t x = 0; x < fb_width; x++) line[x] = BG;
    }
}

static void newline(void) {
    cx = 0;
    cy++;
    if (cy >= rows) {
        scroll_up();
        cy = rows - 1;
    }
}

void console_init(void) {
    const struct display *d = display_get();
    if (!d || !d->pixels || d->width == 0) return;

    pixels    = d->pixels;
    pitch_px  = d->pitch / 4;
    fb_width  = d->width;
    fb_height = d->height;
    cols      = fb_width  / GLYPH_W;
    rows      = fb_height / GLYPH_H;

    clear_screen();
    ready = 1;
    display_present();
}

void console_putc(char c) {
    if (!ready) return;
    if (c == '\n') { newline(); return; }
    if (c == '\r') { cx = 0;    return; }
    if (c == '\b') {
        if (cx > 0) cx--;
        fill_cell(cx, cy, BG);
        return;
    }
    if (c == '\t') { cx = (cx + 8) & ~7u; if (cx >= cols) newline(); return; }

    if ((unsigned char) c < 0x20 || (unsigned char) c > 0x7E) return;
    draw_glyph(cx, cy, c);
    cx++;
    if (cx >= cols) newline();
}

void console_write(const char *buf, size_t count) {
    for (size_t i = 0; i < count; i++) console_putc(buf[i]);
    display_present();
}

void console_puts(const char *s) {
    while (*s) console_putc(*s++);
    display_present();
}
