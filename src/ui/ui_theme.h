/* §3a — Design tokens (macOS HIG). See docs/ui-theme.md */

#ifndef UI_THEME_H
#define UI_THEME_H

#include <stddef.h>
#include <stdint.h>

void ui_theme_init(void);

uint32_t ui_theme_get_u32(const char *key);

int ui_theme_get_radius_dp(const char *key);

unsigned ui_theme_get_space_dp(const char *key);

unsigned ui_theme_get_duration_ms(const char *key);

const char *ui_theme_get_str(const char *key);

/* Hot reload (§3a.6): re-read /etc/ui/theme.toml (or theme.json). Returns 0 on success. */
int ui_theme_reload(void);

void ui_theme_subscribe_changed(void (*cb)(void));

/* Drain COM1 lines; on "theme reload" calls ui_theme_reload(). Poll from a kernel thread. */
void ui_theme_serial_poll(void);

#endif
