#include "i2c/at24.h"

#include <string.h>

void at24_init(At24 *eeprom) {
    memset(eeprom, 0, sizeof(*eeprom));
    memset(eeprom->mem, 0xff, sizeof(eeprom->mem));
}

static bool at24_start(void *ctx, bool read) {
    At24 *eeprom = ctx;
    eeprom->addr_next = !read;      /* a write transfer opens with the address */
    return true;
}

static bool at24_write(void *ctx, uint8_t byte) {
    At24 *eeprom = ctx;
    if (eeprom->addr_next) {
        eeprom->ptr = byte;
        eeprom->addr_next = false;
    } else if (eeprom->write_protect) {
        eeprom->ptr++;
    } else {
        eeprom->mem[eeprom->ptr++] = byte;
    }
    return true;
}

static uint8_t at24_read(void *ctx, bool ack) {
    (void)ack;
    At24 *eeprom = ctx;
    return eeprom->mem[eeprom->ptr++];
}

I2cDevice at24_device(At24 *eeprom, uint8_t addr7) {
    return (I2cDevice){ .addr = addr7, .ctx = eeprom, .start = at24_start,
                        .write = at24_write, .read = at24_read };
}
