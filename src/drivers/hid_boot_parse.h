#pragma once

#include <stdint.h>

/* Shared HID boot / common mouse report decoding (USB + I2C-HID paths).
 * Behaviour aligned with Linux hid-input expectations for simple devices. */

int hid_boot_decode_mouse_report(const uint8_t *p, unsigned len, int32_t *odx, int32_t *ody, uint8_t *obtn);
