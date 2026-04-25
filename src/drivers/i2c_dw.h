#pragma once

#include <stdint.h>
#include <stddef.h>

struct pci_device;

/* Bind Synopsys DesignWare I2C master at PCI MMIO BAR (Intel LPSS style). */
int  i2c_dw_bind_pci(const struct pci_device *pci);
void i2c_dw_unbind(void);
int  i2c_dw_bound(void);

/* 7-bit address. Combined write (register pointer) + read. */
int i2c_dw_write_read(uint8_t addr7, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen);

/* Plain master read (HID input poll path). */
int i2c_dw_master_read(uint8_t addr7, uint8_t *r, size_t rlen);

/* Write-only (SET_POWER / RESET opcodes). */
int i2c_dw_master_write(uint8_t addr7, const uint8_t *w, size_t wlen);
