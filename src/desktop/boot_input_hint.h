#pragma once

/* Top-of-screen status when COM1 serial is not wired (typical laptop). */
void boot_input_hint_show(void);
/* Compositor thread: refresh text when USB probing finishes (first paint is stale). */
void boot_input_hint_tick(void);
