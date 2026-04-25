#pragma once

#include <stdint.h>

/* HID over I2C + Synopsys DesignWare I2C (Intel LPSS) — laptop touchpad path
 * modelled on Linux drivers/hid/i2c-hid/i2c-hid-core.c + i2c-designware-*.c */

int  i2c_hid_mouse_init(uint32_t screen_w, uint32_t screen_h);
void i2c_hid_mouse_poll(void);
int  i2c_hid_mouse_active(void);
