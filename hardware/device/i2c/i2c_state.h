#pragma once
/* Save state (gemu/dev/state.h) for the I2C/SMBus devices: the LM87 hardware
 * monitor, the 24C02 EEPROM and the Itanium cartridge ROMs.  prefix goes
 * before every item name ("" for none); false if any registration failed.
 * The bus (I2cBus) and the attachments (I2cDevice) are wiring and aren't
 * registered.  The memories are saved whole, read-only images included:
 * they are the chips' own arrays. */
#include "gemu/dev/device.h"
#include "i2c/at24.h"
#include "i2c/itanium_cart.h"
#include "i2c/lm87.h"
#include <stdbool.h>

bool lm87_state_add(GemuDevice *d, const char *prefix, Lm87 *x);
bool at24_state_add(GemuDevice *d, const char *prefix, At24 *x);
bool itanium_cart_rom_state_add(GemuDevice *d, const char *prefix, ItaniumCartRom *x);
