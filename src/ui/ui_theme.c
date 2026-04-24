#include "ui_theme.h"

#include "../arch/x86_64/serial.h"
#include "../fs/vfs.h"
#include "../kprintf.h"

#include <stddef.h>
#include <stdint.h>

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

typedef struct {
    const char *key;
    uint32_t    v;
} U32Ent;

typedef struct {
    const char *key;
    int v;
} I32Ent;

typedef struct {
    const char *key;
    unsigned v;
} UEnt;

typedef struct {
    const char *key;
    const char *s;
} StrEnt;

/* macOS Dark — docs/ui-theme.md §3 */
static const U32Ent k_u32_dark[] = {
    { "color.bg.base", 0xff1e1e1eu },
    { "color.bg.surface", 0xff2c2c2cu },
    { "color.bg.surface_elevated", 0xff3a3a3au },
    { "color.bg.surface_hover", 0xff333333u },
    { "color.bg.surface_pressed", 0xff2a2a2au },
    { "color.bg.surface_selected", 0xff2d3d52u },
    { "color.bg.surface_disabled", 0xff252528u },
    { "color.border.subtle", 0xff424242u },
    { "color.border.default", 0xff545458u },
    { "color.border.strong", 0xff6e6e73u },
    { "color.text.primary", 0xfff5f5f7u },
    { "color.text.secondary", 0xffa1a1a6u },
    { "color.text.tertiary", 0xff6e6e73u },
    { "color.text.disabled", 0xff636366u },
    { "color.accent.default", 0xff0a84ffu },
    { "color.accent.hover", 0xff409cffu },
    { "color.accent.pressed", 0xff0060dfu },
    { "color.semantic.success", 0xff32d74bu },
    { "color.semantic.warning", 0xffffd60au },
    { "color.semantic.error", 0xffff453au },
    { "color.semantic.info", 0xff64d2ffu },
    { "color.focus.ring", 0xff0a84ffu },
    { "shadow.elevation_0", 0x00000000u },
    { "shadow.elevation_1", 0x40000000u },
    { "shadow.elevation_2", 0x59000000u },
    { "shadow.elevation_3", 0x66000000u },
    { "font.ui.size_sm", 11u },
    { "font.ui.size_md", 13u },
    { "font.ui.size_lg", 15u },
};

/* macOS Light — docs/ui-theme.md §4 (+ state keys tuned for light surfaces) */
static const U32Ent k_u32_light[] = {
    { "color.bg.base", 0xfff5f5f7u },
    { "color.bg.surface", 0xffffffffu },
    { "color.bg.surface_elevated", 0xffffffffu },
    { "color.bg.surface_hover", 0xffe8e8edu },
    { "color.bg.surface_pressed", 0xffd1d1d6u },
    { "color.bg.surface_selected", 0xffdae8f8u },
    { "color.bg.surface_disabled", 0xfff2f2f7u },
    { "color.border.subtle", 0xffd1d1d6u },
    { "color.border.default", 0xffc6c6c8u },
    { "color.border.strong", 0xffaeaeb2u },
    { "color.text.primary", 0xff1d1d1fu },
    { "color.text.secondary", 0xff6e6e73u },
    { "color.text.tertiary", 0xffaeaeb2u },
    { "color.text.disabled", 0xffc7c7ccu },
    { "color.accent.default", 0xff007affu },
    { "color.accent.hover", 0xff0077edu },
    { "color.accent.pressed", 0xff006adbu },
    { "color.semantic.success", 0xff34c759u },
    { "color.semantic.warning", 0xffff9500u },
    { "color.semantic.error", 0xffff3b30u },
    { "color.semantic.info", 0xff5ac8fau },
    { "color.focus.ring", 0xff007affu },
    { "shadow.elevation_0", 0x00000000u },
    { "shadow.elevation_1", 0x18000000u },
    { "shadow.elevation_2", 0x22000000u },
    { "shadow.elevation_3", 0x30000000u },
    { "font.ui.size_sm", 11u },
    { "font.ui.size_md", 13u },
    { "font.ui.size_lg", 15u },
};

_Static_assert(sizeof(k_u32_dark) == sizeof(k_u32_light), "theme palette tables diverged");

#define NU32 (sizeof(k_u32_dark) / sizeof(k_u32_dark[0]))

static const I32Ent k_radius[] = {
    { "radius.none", 0 },
    { "radius.xs", 4 },
    { "radius.sm", 6 },
    { "radius.md", 10 },
    { "radius.lg", 14 },
    { "radius.xl", 20 },
    { "radius.full", 9999 },
};

#define NRAD (sizeof(k_radius) / sizeof(k_radius[0]))

static const UEnt k_space[] = {
    { "space.xs", 4u }, { "space.sm", 8u }, { "space.md", 12u },
    { "space.lg", 16u }, { "space.xl", 24u }, { "space.xxl", 32u },
};

#define NSPACE (sizeof(k_space) / sizeof(k_space[0]))

static const UEnt k_duration[] = {
    { "duration.fast", 150u },
    { "duration.normal", 250u },
    { "duration.slow", 400u },
};

#define NDUR (sizeof(k_duration) / sizeof(k_duration[0]))

static const StrEnt k_str[] = {
    { "font.ui.family", "Noto Sans" },
};

#define NSTR 1
#define FONT_FAMILY_CAP 72

static uint32_t g_u32[NU32];
static int      g_rad[NRAD];
static unsigned g_space[NSPACE];
static unsigned g_dur[NDUR];
static char     g_font_family[FONT_FAMILY_CAP];

static unsigned s_warn_unknown;
static void (*s_on_changed)(void);

static void warn_unknown(const char *key) {
    if (s_warn_unknown >= 8u) return;
    s_warn_unknown++;
    kprintf("[ui_theme] unknown key \"%s\" (see docs/ui-theme.md)\n", key ? key : "(null)");
}

static void notify_changed(void) {
    if (s_on_changed) s_on_changed();
}

static void baseline_palette(int light) {
    const U32Ent *src = light ? k_u32_light : k_u32_dark;
    for (size_t i = 0; i < NU32; i++) g_u32[i] = src[i].v;
    for (size_t i = 0; i < NRAD; i++) g_rad[i] = k_radius[i].v;
    for (size_t i = 0; i < NSPACE; i++) g_space[i] = k_space[i].v;
    for (size_t i = 0; i < NDUR; i++) g_dur[i] = k_duration[i].v;
    const char *defam = k_str[0].s;
    for (size_t i = 0; defam[i] && i + 1 < FONT_FAMILY_CAP; i++)
        g_font_family[i] = defam[i];
    g_font_family[FONT_FAMILY_CAP - 1] = '\0';
}

static int is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *skip_ws(const char *p) {
    while (*p && is_space((unsigned char) *p)) p++;
    return p;
}

static int from_hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex_u32(const char *s, size_t L, uint32_t *out) {
    if (!s || !L) return -1;
    if (L >= 1 && s[0] == '#') {
        s++;
        L--;
        if (L == 3) {
            int a = from_hex((unsigned char) s[0]);
            int b = from_hex((unsigned char) s[1]);
            int c2 = from_hex((unsigned char) s[2]);
            if (a < 0 || b < 0 || c2 < 0) return -1;
            uint32_t r = (uint32_t) (a * 17);
            uint32_t g = (uint32_t) (b * 17);
            uint32_t b_ = (uint32_t) (c2 * 17);
            *out = 0xff000000u | (r << 16) | (g << 8) | b_;
            return 0;
        }
        if (L == 6) {
            uint32_t rgb = 0;
            for (size_t i = 0; i < 6; i++) {
                int h = from_hex((unsigned char) s[i]);
                if (h < 0) return -1;
                rgb = (rgb << 4) | (uint32_t) h;
            }
            *out = 0xff000000u | rgb;
            return 0;
        }
        if (L == 8) {
            uint32_t v = 0;
            for (size_t i = 0; i < 8; i++) {
                int h = from_hex((unsigned char) s[i]);
                if (h < 0) return -1;
                v = (v << 4) | (uint32_t) h;
            }
            *out = v;
            return 0;
        }
        return -1;
    }
    if (L >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        L -= 2;
        if (L == 0 || L > 8) return -1;
        uint32_t v = 0;
        for (size_t i = 0; i < L; i++) {
            int h = from_hex((unsigned char) s[i]);
            if (h < 0) return -1;
            v = (v << 4) | (uint32_t) h;
        }
        if (L <= 6) v |= 0xff000000u;
        *out = v;
        return 0;
    }
    return -1;
}

static int parse_u32_dec(const char *s, size_t L, uint32_t *out) {
    if (!s || !L) return -1;
    uint32_t v = 0;
    for (size_t i = 0; i < L; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10u + (uint32_t) (s[i] - '0');
        if (v > 0x7fffffffu) return -1;
    }
    *out = v;
    return 0;
}

static int set_u32_key(const char *key, uint32_t val) {
    if (!key) return -1;
    for (size_t i = 0; i < NU32; i++) {
        if (streq(key, k_u32_dark[i].key)) {
            g_u32[i] = val;
            return 0;
        }
    }
    warn_unknown(key);
    return -1;
}

static int set_rad_key(const char *key, int val) {
    for (size_t i = 0; i < NRAD; i++) {
        if (streq(key, k_radius[i].key)) {
            g_rad[i] = val;
            return 0;
        }
    }
    warn_unknown(key);
    return -1;
}

static int set_space_key(const char *key, unsigned val) {
    for (size_t i = 0; i < NSPACE; i++) {
        if (streq(key, k_space[i].key)) {
            g_space[i] = val;
            return 0;
        }
    }
    warn_unknown(key);
    return -1;
}

static int set_dur_key(const char *key, unsigned val) {
    for (size_t i = 0; i < NDUR; i++) {
        if (streq(key, k_duration[i].key)) {
            g_dur[i] = val;
            return 0;
        }
    }
    warn_unknown(key);
    return -1;
}

static int set_str_key(const char *key, const char *val, size_t vlen) {
    if (!streq(key, "font.ui.family")) {
        warn_unknown(key);
        return -1;
    }
    if (vlen >= FONT_FAMILY_CAP) vlen = FONT_FAMILY_CAP - 1;
    for (size_t i = 0; i < vlen; i++) g_font_family[i] = val[i];
    g_font_family[vlen] = '\0';
    return 0;
}

enum theme_sec {
    SEC_NONE,
    SEC_META,
    SEC_APPEARANCE,
    SEC_COLOR,
    SEC_RADIUS,
    SEC_SPACE,
    SEC_DURATION,
    SEC_SHADOW,
    SEC_FONT,
};

static int hdr_eq(const char *name, size_t n, const char *lit) {
    size_t i = 0;
    for (; lit[i]; i++) {
        if (i >= n || name[i] != lit[i]) return 0;
    }
    return i == n;
}

static int section_from_header(const char *name, size_t n) {
    if (hdr_eq(name, n, "meta")) return SEC_META;
    if (hdr_eq(name, n, "appearance")) return SEC_APPEARANCE;
    if (hdr_eq(name, n, "color")) return SEC_COLOR;
    if (hdr_eq(name, n, "radius")) return SEC_RADIUS;
    if (hdr_eq(name, n, "space")) return SEC_SPACE;
    if (hdr_eq(name, n, "duration")) return SEC_DURATION;
    if (hdr_eq(name, n, "shadow")) return SEC_SHADOW;
    if (hdr_eq(name, n, "font")) return SEC_FONT;
    return SEC_NONE;
}

static int scan_appearance_mode(const char *buf, size_t len) {
    int sec = SEC_NONE;
    size_t i = 0;
    int want_light = 0;
    while (i < len) {
        while (i < len && (buf[i] == '\n' || buf[i] == '\r')) i++;
        size_t ls = i;
        while (i < len && buf[i] != '\n' && buf[i] != '\r') i++;
        size_t le = i;
        const char *line = buf + ls;
        size_t L = le - ls;
        if (L == 0) continue;
        /* strip comments (# not in string — subset: whole-line #) */
        size_t cut = L;
        for (size_t j = 0; j < L; j++) {
            if (line[j] == '#') {
                cut = j;
                break;
            }
        }
        L = cut;
        while (L > 0 && is_space((unsigned char) line[L - 1])) L--;
        if (L == 0) continue;
        const char *p = skip_ws(line);
        if (*p == '#') continue;
        if (*p == '[') {
            p++;
            const char *name = p;
            size_t nn = 0;
            while (nn < L && name[nn] && name[nn] != ']' && !is_space((unsigned char) name[nn])) nn++;
            sec = section_from_header(name, nn);
            continue;
        }
        if (sec != SEC_APPEARANCE) continue;
        const char *eq = NULL;
        for (size_t j = 0; j < L; j++) {
            if (line[j] == '=') {
                eq = line + j;
                break;
            }
        }
        if (!eq) continue;
        const char *ke = eq;
        while (ke > p && is_space((unsigned char) ke[-1])) ke--;
        if ((size_t) (ke - p) != 4 || p[0] != 'm' || p[1] != 'o' || p[2] != 'd' || p[3] != 'e') continue;
        const char *vs = skip_ws(eq + 1);
        const char *ve = line + L;
        while (ve > vs && is_space((unsigned char) ve[-1])) ve--;
        if (ve > vs && *vs == '"' && ve[-1] == '"') {
            vs++;
            ve--;
        }
        size_t vlen = (size_t) (ve - vs);
        if (vlen == 5 && vs[0] == 'l' && vs[1] == 'i' && vs[2] == 'g' && vs[3] == 'h' && vs[4] == 't')
            want_light = 1;
        else
            want_light = 0;
    }
    return want_light;
}

static void build_full_key(char *out, size_t outsz, int sec, const char *lk, size_t lklen) {
    const char *pre = "";
    if (sec == SEC_COLOR) pre = "color.";
    else if (sec == SEC_RADIUS) pre = "radius.";
    else if (sec == SEC_SPACE) pre = "space.";
    else if (sec == SEC_DURATION) pre = "duration.";
    else if (sec == SEC_SHADOW) pre = "shadow.";
    else if (sec == SEC_FONT) pre = "font.";
    size_t pl = 0;
    while (pre[pl]) pl++;
    size_t pos = 0;
    for (size_t i = 0; i < pl && pos + 1 < outsz; i++) out[pos++] = pre[i];
    for (size_t i = 0; i < lklen && pos + 1 < outsz; i++) out[pos++] = lk[i];
    out[pos] = '\0';
}

static int assign_value_for_key(const char *fullkey, const char *vs, size_t vlen) {
    if (streq(fullkey, "appearance") || streq(fullkey, "mode")) return 0;

    uint32_t u32;
    if (parse_hex_u32(vs, vlen, &u32) == 0)
        return set_u32_key(fullkey, u32);
    uint32_t dec;
    if (parse_u32_dec(vs, vlen, &dec) == 0) {
        if (fullkey[0] == 'r' && fullkey[1] == 'a') {
            if (dec > 100000u) return -1;
            return set_rad_key(fullkey, (int) dec);
        }
        if (fullkey[0] == 's' && fullkey[1] == 'p' && fullkey[2] == 'a') {
            if (dec > 10000u) return -1;
            return set_space_key(fullkey, dec);
        }
        if (fullkey[0] == 'd' && fullkey[1] == 'u') {
            if (dec > 60000u) return -1;
            return set_dur_key(fullkey, dec);
        }
        if (streq(fullkey, "font.ui.size_sm") || streq(fullkey, "font.ui.size_md")
            || streq(fullkey, "font.ui.size_lg"))
            return set_u32_key(fullkey, dec);
    }
    if (fullkey[0] == 'f' && streq(fullkey, "font.ui.family")) {
        if (vlen >= 2 && *vs == '"' && vs[vlen - 1] == '"') {
            vs++;
            vlen -= 2;
        }
        return set_str_key(fullkey, vs, vlen);
    }
    return -1;
}

static void merge_toml_body(const char *buf, size_t len) {
    int sec = SEC_NONE;
    size_t i = 0;
    while (i < len) {
        while (i < len && (buf[i] == '\n' || buf[i] == '\r')) i++;
        size_t ls = i;
        while (i < len && buf[i] != '\n' && buf[i] != '\r') i++;
        size_t le = i;
        const char *line = buf + ls;
        size_t L = le - ls;
        if (L == 0) continue;
        size_t cut = L;
        for (size_t j = 0; j < L; j++) {
            if (line[j] == '#') {
                cut = j;
                break;
            }
        }
        L = cut;
        while (L > 0 && is_space((unsigned char) line[L - 1])) L--;
        if (L == 0) continue;
        const char *p = skip_ws(line);
        if (*p == '#') continue;
        if (*p == '[') {
            p++;
            const char *name = p;
            size_t nn = 0;
            while (nn < L && name[nn] && name[nn] != ']' && !is_space((unsigned char) name[nn])) nn++;
            sec = section_from_header(name, nn);
            continue;
        }
        if (sec == SEC_NONE || sec == SEC_META || sec == SEC_APPEARANCE) continue;
        const char *eq = NULL;
        for (size_t j = 0; j < L; j++) {
            if (line[j] == '=') {
                eq = line + j;
                break;
            }
        }
        if (!eq) continue;
        const char *kl = p;
        const char *ke = eq;
        while (ke > kl && is_space((unsigned char) ke[-1])) ke--;
        size_t lklen = (size_t) (ke - kl);
        if (lklen == 0) continue;
        const char *vs = skip_ws(eq + 1);
        const char *ve = line + L;
        while (ve > vs && is_space((unsigned char) ve[-1])) ve--;
        size_t vlen = (size_t) (ve - vs);
        if (vlen == 0) continue;
        char full[96];
        build_full_key(full, sizeof full, sec, kl, lklen);
        (void) assign_value_for_key(full, vs, vlen);
    }
}

static int merge_json_body(const char *buf, size_t len) {
    size_t i = 0;
    while (i < len && is_space((unsigned char) buf[i])) i++;
    if (i >= len || buf[i] != '{') return -1;
    i++;
    for (;;) {
        while (i < len && is_space((unsigned char) buf[i])) i++;
        if (i < len && buf[i] == '}') return 0;
        if (i >= len || buf[i] != '"') return -1;
        i++;
        size_t ks = i;
        while (i < len && buf[i] != '"') i++;
        if (i >= len) return -1;
        size_t klen = i - ks;
        i++;
        while (i < len && is_space((unsigned char) buf[i])) i++;
        if (i >= len || buf[i] != ':') return -1;
        i++;
        while (i < len && is_space((unsigned char) buf[i])) i++;
        if (i >= len) return -1;
        char keybuf[96];
        if (klen >= sizeof keybuf) klen = sizeof keybuf - 1;
        for (size_t j = 0; j < klen; j++) keybuf[j] = buf[ks + j];
        keybuf[klen] = '\0';
        if (buf[i] == '"') {
            i++;
            size_t vs = i;
            while (i < len && buf[i] != '"') i++;
            if (i >= len) return -1;
            size_t vlen = i - vs;
            i++;
            (void) assign_value_for_key(keybuf, buf + vs, vlen);
        } else {
            size_t vs = i;
            while (i < len && buf[i] != ',' && buf[i] != '}' && !is_space((unsigned char) buf[i])) i++;
            size_t vlen = i - vs;
            assign_value_for_key(keybuf, buf + vs, vlen);
        }
        while (i < len && is_space((unsigned char) buf[i])) i++;
        if (i < len && buf[i] == ',') {
            i++;
            continue;
        }
        if (i < len && buf[i] == '}') return 0;
        if (i >= len) return 0;
    }
}

static int scan_json_appearance_light(const char *b, size_t n) {
    static const struct {
        const char *needle;
        size_t      nlen;
    } keys[] = {
        { "\"mode\"", 6 },
        { "\"appearance\"", 12 },
    };
    for (unsigned t = 0; t < sizeof keys / sizeof keys[0]; t++) {
        const char *nd = keys[t].needle;
        size_t      nl = keys[t].nlen;
        for (size_t i = 0; i + nl <= n; i++) {
            size_t j = 0;
            for (; j < nl; j++) {
                if (b[i + j] != nd[j]) break;
            }
            if (j != nl) continue;
            size_t c = i + nl;
            while (c < n && c < i + 48u) {
                if (b[c] == ':') {
                    c++;
                    while (c < n && is_space((unsigned char) b[c])) c++;
                    if (c + 7u <= n && b[c] == '"' && b[c + 1] == 'l' && b[c + 2] == 'i'
                        && b[c + 3] == 'g' && b[c + 4] == 'h' && b[c + 5] == 't'
                        && b[c + 6] == '"')
                        return 1;
                    if (c + 5u <= n && b[c] == 'l' && b[c + 1] == 'i' && b[c + 2] == 'g'
                        && b[c + 3] == 'h' && b[c + 4] == 't') {
                        size_t z = c + 5u;
                        if (z >= n || is_space((unsigned char) b[z]) || b[z] == ','
                            || b[z] == '}')
                            return 1;
                    }
                    break;
                }
                c++;
            }
        }
    }
    return 0;
}

static int load_buffer(const char *buf, size_t len, int is_json) {
    int want_light = 0;
    if (is_json)
        want_light = scan_json_appearance_light(buf, len);
    else
        want_light = scan_appearance_mode(buf, len);
    baseline_palette(want_light);
    if (is_json) {
        if (merge_json_body(buf, len) != 0) return -1;
    } else {
        merge_toml_body(buf, len);
    }
    return 0;
}

static int try_read_theme_file(const char *path, char *stack, size_t cap) {
    struct vnode *v = vfs_lookup(path);
    if (!v || v->type != VFS_FILE || !v->data || v->size == 0) return -1;
    size_t n = v->size < cap - 1 ? (size_t) v->size : cap - 1;
    int64_t r = vfs_read(v, 0, n, stack);
    if (r <= 0) return -1;
    stack[(size_t) r] = '\0';
    return (int) r;
}

static void try_load_from_vfs(void) {
    char buf[8192];
    int n = try_read_theme_file("/etc/ui/theme.toml", buf, sizeof buf);
    int is_json = 0;
    if (n < 0) {
        n = try_read_theme_file("/etc/ui/theme.json", buf, sizeof buf);
        if (n < 0) {
            kprintf("[ui_theme] no /etc/ui/theme.toml — compiled defaults\n");
            baseline_palette(0);
            return;
        }
        is_json = 1;
    }
    const char *p = skip_ws(buf);
    if (*p == '{') is_json = 1;
    s_warn_unknown = 0;
    if (load_buffer(buf, (size_t) n, is_json) != 0) {
        kprintf("[ui_theme] parse error — reverting to dark baseline\n");
        baseline_palette(0);
        return;
    }
    kprintf("[ui_theme] loaded %s\n", is_json ? "/etc/ui/theme.json" : "/etc/ui/theme.toml");
}

void ui_theme_init(void) {
    s_warn_unknown = 0;
    s_on_changed = NULL;
    try_load_from_vfs();
    kprintf("[ui_theme] tokens active — docs/ui-theme.md\n");
}

int ui_theme_reload(void) {
    s_warn_unknown = 0;
    try_load_from_vfs();
    notify_changed();
    return 0;
}

void ui_theme_subscribe_changed(void (*cb)(void)) { s_on_changed = cb; }

uint32_t ui_theme_get_u32(const char *key) {
    if (!key) return 0;
    for (size_t i = 0; i < NU32; i++) {
        if (streq(key, k_u32_dark[i].key)) return g_u32[i];
    }
    warn_unknown(key);
    return 0;
}

int ui_theme_get_radius_dp(const char *key) {
    if (!key) return -1;
    for (size_t i = 0; i < NRAD; i++) {
        if (streq(key, k_radius[i].key)) return g_rad[i];
    }
    warn_unknown(key);
    return -1;
}

unsigned ui_theme_get_space_dp(const char *key) {
    if (!key) return 0;
    for (size_t i = 0; i < NSPACE; i++) {
        if (streq(key, k_space[i].key)) return g_space[i];
    }
    warn_unknown(key);
    return 0;
}

unsigned ui_theme_get_duration_ms(const char *key) {
    if (!key) return 0;
    for (size_t i = 0; i < NDUR; i++) {
        if (streq(key, k_duration[i].key)) return g_dur[i];
    }
    warn_unknown(key);
    return 0;
}

const char *ui_theme_get_str(const char *key) {
    if (!key) return NULL;
    if (streq(key, "font.ui.family")) return g_font_family;
    warn_unknown(key);
    return NULL;
}

static int line_streq_trimmed(const char *line, const char *lit) {
    const char *a = skip_ws(line);
    const char *b = lit;
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    if (*b) return 0;
    while (*a && is_space((unsigned char) *a)) a++;
    return *a == '\0';
}

#define SERIAL_LINE_CAP 80
static char   s_serial_line[SERIAL_LINE_CAP];
static size_t s_serial_pos;

void ui_theme_serial_poll(void) {
    int c;
    while ((c = serial_try_getc()) >= 0) {
        if (c == '\r' || c == '\n') {
            s_serial_line[s_serial_pos] = '\0';
            s_serial_pos = 0;
            if (line_streq_trimmed(s_serial_line, "theme reload"))
                (void) ui_theme_reload();
        } else if ((unsigned) c < 32u) {
            /* ignore other control chars */
        } else if (s_serial_pos + 1 < SERIAL_LINE_CAP) {
            s_serial_line[s_serial_pos++] = (char) c;
        }
    }
}
