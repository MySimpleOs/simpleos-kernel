#include "hid_boot_parse.h"

#include "mouse.h"

#include <stdint.h>

int hid_boot_decode_mouse_report(const uint8_t *p, unsigned len, int32_t *odx, int32_t *ody, uint8_t *obtn) {
    *odx = *ody = 0;
    *obtn = 0;
    if (len < 3u)
        return 0;

    unsigned o = 0;
    if (len >= 4u && p[0] >= 1u && p[0] <= 16u && (p[1] & 0x08u) != 0u)
        o = 1u;

    if (len >= o + 3u) {
        uint8_t f = p[o];
        if ((f & 0x08u) != 0u) {
            int32_t dx = (int32_t)(int8_t) p[o + 1u];
            int32_t dy = (int32_t)(int8_t) p[o + 2u];
            if (f & 0x10u) dx -= 256;
            if (f & 0x20u) dy -= 256;
            uint8_t bt = 0;
            if (f & 1u) bt |= MOUSE_BTN_LEFT;
            if (f & 2u) bt |= MOUSE_BTN_RIGHT;
            if (f & 4u) bt |= MOUSE_BTN_MIDDLE;
            *odx = dx;
            *ody = dy;
            *obtn = bt;
            return 1;
        }
        /* Many dongles omit bit3 on boot-ish reports; accept small 8-bit deltas. */
        {
            int32_t dx = (int32_t)(int8_t) p[o + 1u];
            int32_t dy = (int32_t)(int8_t) p[o + 2u];
            if (dx != 0 || dy != 0 || (f & 7u)) {
                uint8_t bt = 0;
                if (f & 1u) bt |= MOUSE_BTN_LEFT;
                if (f & 2u) bt |= MOUSE_BTN_RIGHT;
                if (f & 4u) bt |= MOUSE_BTN_MIDDLE;
                if (f & 0x10u) dx -= 256;
                if (f & 0x20u) dy -= 256;
                *odx = dx;
                *ody = dy;
                *obtn = bt;
                return 1;
            }
        }
    }

    if (len >= 5u) {
        int16_t x = (int16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
        int16_t y = (int16_t) ((uint16_t) p[2] | ((uint16_t) p[3] << 8));
        if (x != 0 || y != 0) {
            *odx = (int32_t) x;
            *ody = (int32_t) y;
            if (len >= 5u) {
                uint8_t bb = p[4];
                if (bb & 1u) *obtn |= MOUSE_BTN_LEFT;
                if (bb & 2u) *obtn |= MOUSE_BTN_RIGHT;
                if (bb & 4u) *obtn |= MOUSE_BTN_MIDDLE;
            }
            return 1;
        }
    }

    if (len >= 6u) {
        int16_t x = (int16_t) ((uint16_t) p[1] | ((uint16_t) p[2] << 8));
        int16_t y = (int16_t) ((uint16_t) p[3] | ((uint16_t) p[4] << 8));
        if (x != 0 || y != 0) {
            uint8_t f = p[0];
            uint8_t bt = 0;
            if (f & 1u) bt |= MOUSE_BTN_LEFT;
            if (f & 2u) bt |= MOUSE_BTN_RIGHT;
            if (f & 4u) bt |= MOUSE_BTN_MIDDLE;
            *odx = (int32_t) x;
            *ody = (int32_t) y;
            *obtn = bt;
            return 1;
        }
    }

    return 0;
}
