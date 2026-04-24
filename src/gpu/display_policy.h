#pragma once

#include <stddef.h>
#include <stdint.h>

/* User-facing monitor policy (logical resolution + nominal refresh).
 * Physical framebuffer size remains whatever Limine hands us until EDID/KMS
 * lands; this block drives compositor Hz, LAPIC tick granularity, and boot
 * logs. Loaded from /etc/display.conf on the initrd when present. */

struct display_policy {
    uint32_t width;        /* configured horizontal pixels (e.g. 2560) */
    uint32_t height;       /* configured vertical pixels (e.g. 1440)   */
    uint32_t refresh_hz;   /* nominal refresh (e.g. 185)                */
    char     label[48];    /* short name, e.g. "primary"                */
    int      from_file;    /* 1 if last load came from VFS              */
};

void display_policy_init_defaults(void);

/* Parse key=value lines (# comments, blank lines). Returns 0 on success,
 * -1 on garbage. Partial keys update only recognised fields. */
int display_policy_parse(const char *buf, size_t len);

/* Try to read path from VFS (e.g. "/etc/display.conf"). Ignores failure. */
void display_policy_try_load_vfs(const char *path);

const struct display_policy *display_policy_get(void);

/* LAPIC periodic Hz: multiple of refresh_hz in [1000,8000] for stable
 * compositor ticks_per_frame = timer_hz / refresh_hz (integer). */
uint32_t display_policy_apic_timer_hz(void);

/* Compositor target frame rate: clamped refresh from policy. */
uint32_t display_policy_compositor_hz(void);
