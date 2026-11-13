#pragma once

/* A 256-byte I2C serial EEPROM with an 8-bit word address (24C02 class), as
 * used for DIMM serial presence detect. A write transfer sets the address
 * and stores the bytes after it; a read transfer returns bytes from the
 * address. Both advance it, wrapping at 256. Write cycles complete at once. */

#include "i2c/i2c.h"

#include <stdbool.h>
#include <stdint.h>

enum { AT24_SIZE = 256 };

typedef struct At24 {
    uint8_t mem[AT24_SIZE];
    uint8_t ptr;
    bool addr_next;     /* the next written byte is the word address */
    bool write_protect; /* data bytes are accepted but not stored */
} At24;

/* Erased (FFh) contents. */
void at24_init(At24 *eeprom);
/* The bus attachment that places `eeprom` at `addr7`. */
I2cDevice at24_device(At24 *eeprom, uint8_t addr7);
