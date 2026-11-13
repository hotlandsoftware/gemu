#pragma once

/* Declarations shared between the 460GX source files. Nothing outside
 * hardware/device/chipset includes this header. */

#include "i460gx.h"

#include <stdbool.h>
#include <stdint.h>

static inline uint64_t i460gx_size_mask(unsigned size) {
    return size == 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
}

static inline bool i460gx_access_size_valid(unsigned size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

/* i460gx_smbus.c: IFB SMBus controller. Returns false when `port` is not in
 * the controller's enabled I/O block. */
bool i460gx_smbus_io(I460GX *s, uint64_t port, bool write, unsigned size,
                     uint64_t *value);

/* i460gx_pm.c: IFB power management. i460gx_pm_io() decodes the ACPI and GPIO
 * blocks and the APM ports; it returns false for any other port. */
bool i460gx_pm_io(I460GX *s, bool write, uint64_t port, unsigned size,
                  uint64_t *value);
void i460gx_pm_reset(I460GX *s, bool power_on);
/* BIOS_WEN (BIOSEN bit 2) was set; `ble` is BIOSEN bit 15. */
void i460gx_pm_bios_wen(I460GX *s, bool ble);

/* i460gx_fwh.c: firmware hubs. */
void i460gx_fwh_reset(I460GX *s);
bool i460gx_flash_range(uint64_t addr);

/* i460gx_pci.c: configuration space. */
void i460gx_pci_reset(I460GX *s);
uint8_t i460gx_ifb_cfg_read8(const I460GX *s, unsigned fun, unsigned reg);
