#include "display_policy.h"

#include "../fs/vfs.h"
#include "../kprintf.h"

#include <stddef.h>
#include <stdint.h>

static struct display_policy g_pol;

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char* skip_ws(const char* p) {
    while (*p && is_space((unsigned char) *p))
        p++;
    return p;
}

static int key_match(const char* a, const char* b, size_t blen) {
    size_t i = 0;
    for (; i < blen; i++) {
        if (!a[i] || a[i] != b[i])
            return 0;
    }
    /* Line is "key=value…"; key ends at '=', space, or end — not a longer id. */
    char c = a[i];
    return c == '\0' || c == '=' || is_space((unsigned char) c);
}

static int value_match(const char* vs, size_t vlen, const char* lit) {
    size_t n = 0;
    while (lit[n])
        n++;
    if (vlen != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (vs[i] != lit[i])
            return 0;
    return 1;
}

static int parse_u32(const char* s, uint32_t* out) {
    uint32_t v = 0;
    int any = 0;
    while (*s >= '0' && *s <= '9') {
        any = 1;
        v = v * 10u + (uint32_t) (*s - '0');
        if (v > 65535u)
            return -1;
        s++;
    }
    if (!any)
        return -1;
    *out = v;
    return 0;
}

void display_policy_init_defaults(void) {
    /* Defaults match common laptop / VirtualBox GOP modes until /etc/display.conf loads. */
    g_pol.width = 1920;
    g_pol.height = 1080;
    g_pol.refresh_hz = 120;
    g_pol.label[0] = 'p';
    g_pol.label[1] = 'r';
    g_pol.label[2] = 'i';
    g_pol.label[3] = 'm';
    g_pol.label[4] = 'a';
    g_pol.label[5] = 'r';
    g_pol.label[6] = 'y';
    g_pol.label[7] = '\0';
    g_pol.pointer = DISPLAY_POINTER_AUTO;
    /* TSC pacing after present caps compositor thread CPU when damage is steady. */
    g_pol.vsync = 1;
    g_pol.from_file = 0;
}

const struct display_policy* display_policy_get(void) {
    return &g_pol;
}

int display_policy_parse(const char* buf, size_t len) {
    if (!buf)
        return -1;
    size_t i = 0;
    while (i < len) {
        while (i < len && (buf[i] == '\n' || buf[i] == '\r'))
            i++;
        size_t line_start = i;
        while (i < len && buf[i] != '\n' && buf[i] != '\r')
            i++;
        size_t line_end = i;

        const char* line = buf + line_start;
        size_t L = line_end - line_start;
        if (L == 0)
            continue;

        if (line[0] == '#')
            continue;

        /* trim trailing ws */
        while (L > 0 && is_space((unsigned char) line[L - 1]))
            L--;
        if (L == 0)
            continue;

        const char* p = skip_ws(line);
        if (*p == '#' || *p == '\0')
            continue;

        const char* eq = NULL;
        for (size_t j = 0; j < L; j++) {
            if (line[j] == '=') {
                eq = line + j;
                break;
            }
        }
        if (!eq)
            continue;

        const char* ke = eq;
        while (ke > p && is_space((unsigned char) ke[-1]))
            ke--;
        const char* vs = eq + 1;
        vs = skip_ws(vs);
        const char* ve = line + L;
        while (ve > vs && is_space((unsigned char) ve[-1]))
            ve--;

        size_t vlen = (size_t) (ve - vs);
        if (vlen == 0)
            return -1;

        if (key_match(p, "width", 5)) {
            uint32_t v;
            if (parse_u32(vs, &v) || v < 320 || v > 16384)
                return -1;
            g_pol.width = v;
        } else if (key_match(p, "height", 6)) {
            uint32_t v;
            if (parse_u32(vs, &v) || v < 240 || v > 16384)
                return -1;
            g_pol.height = v;
        } else if (key_match(p, "refresh_hz", 10)) {
            uint32_t v;
            if (parse_u32(vs, &v) || v < 30 || v > 360)
                return -1;
            g_pol.refresh_hz = v;
        } else if (key_match(p, "label", 5)) {
            size_t c = vlen < sizeof(g_pol.label) - 1 ? vlen : sizeof(g_pol.label) - 1;
            for (size_t t = 0; t < c; t++)
                g_pol.label[t] = vs[t];
            g_pol.label[c] = '\0';
        } else if (key_match(p, "pointer", 7)) {
            if (value_match(vs, vlen, "auto"))
                g_pol.pointer = DISPLAY_POINTER_AUTO;
            else if (value_match(vs, vlen, "ps2"))
                g_pol.pointer = DISPLAY_POINTER_PS2;
            else if (value_match(vs, vlen, "virtio"))
                g_pol.pointer = DISPLAY_POINTER_VIRTIO;
            else if (value_match(vs, vlen, "usb"))
                g_pol.pointer = DISPLAY_POINTER_USB;
            else if (value_match(vs, vlen, "i2c"))
                g_pol.pointer = DISPLAY_POINTER_I2C;
            else
                return -1;
        } else if (key_match(p, "vsync", 5)) {
            if (value_match(vs, vlen, "0"))
                g_pol.vsync = 0;
            else if (value_match(vs, vlen, "1"))
                g_pol.vsync = 1;
            else
                return -1;
        } else {
            /* unknown key — skip line for forward compatibility */
        }
    }
    return 0;
}

static const char* policy_ptr_name(uint8_t p) {
    if (p == DISPLAY_POINTER_PS2)
        return "ps2";
    if (p == DISPLAY_POINTER_VIRTIO)
        return "virtio";
    if (p == DISPLAY_POINTER_USB)
        return "usb";
    if (p == DISPLAY_POINTER_I2C)
        return "i2c";
    return "auto";
}

static const char* policy_vsync_name(uint8_t v) {
    return v ? "on" : "off";
}

void display_policy_try_load_vfs(const char* path) {
    if (!path)
        return;
    struct vnode* v = vfs_lookup(path);
    if (!v || v->type != VFS_FILE || !v->data || v->size == 0) {
        kprintf("[display] policy file %s not found — built-in defaults\n", path);
        return;
    }

    char stackbuf[512];
    size_t n = v->size < sizeof(stackbuf) - 1 ? v->size : sizeof(stackbuf) - 1;
    int64_t r = vfs_read(v, 0, n, stackbuf);
    if (r <= 0)
        return;
    stackbuf[(size_t) r] = '\0';

    struct display_policy backup = g_pol;
    if (display_policy_parse(stackbuf, (size_t) r) != 0) {
        g_pol = backup;
        kprintf("[display] policy parse error in %s — keeping previous\n", path);
        return;
    }
    g_pol.from_file = 1;
    kprintf("[display] policy from %s: %ux%u @ %u Hz (%s) pointer=%s vsync=%s\n", path,
            (unsigned) g_pol.width, (unsigned) g_pol.height, (unsigned) g_pol.refresh_hz,
            g_pol.label, policy_ptr_name(g_pol.pointer), policy_vsync_name(g_pol.vsync));
}

uint32_t display_policy_apic_timer_hz(void) {
    uint32_t r = g_pol.refresh_hz;
    if (r < 30)
        r = 60;
    if (r > 360)
        r = 360;
    for (uint32_t mult = 10; mult <= 80; mult++) {
        uint32_t hz = r * mult;
        if (hz >= 1000u && hz <= 8000u)
            return hz;
    }
    return r * 10u;
}

uint32_t display_policy_compositor_hz(void) {
    uint32_t r = g_pol.refresh_hz;
    if (r < 30)
        r = 30;
    if (r > 360)
        r = 360; /* same upper bound as refresh_hz in display.conf */
    return r;
}
