#pragma once

/* The SMBus devices on an Itanium (Merced) processor cartridge: the
 * Processor Information ROM (PIROM) and the Scratch EEPROM, per the Intel
 * Itanium Processor 800 MHz and 733 MHz Datasheet (249634-002), chapter 6.
 *
 * Addresses (Table 6-3): 1010 SMA1 SMA0 Y, with Y = 0 the Scratch EEPROM and
 * Y = 1 the PIROM, so 7-bit 50h/51h for cartridge 1, 52h/53h for cartridge 2,
 * and so on. Both are 128 bytes (the address byte's MSB is a don't care);
 * reads are current-address, random and sequential and roll over at 128; the
 * Scratch EEPROM takes byte and 8-byte page writes (6.3.3). The thermal
 * sensing device (6.4) is not modelled. */

#include "i2c/i2c.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct ItaniumCartRom {
    uint8_t mem[128];
    uint8_t ptr;
    bool addr_next;     /* the next written byte is the word address */
    bool writable;      /* the Scratch EEPROM (SMWP deasserted) */
    uint8_t page_base;  /* page writes roll over within 8 bytes */
} ItaniumCartRom;

/* What a cartridge's PIROM states (Table 6-4). Zero or NULL fields are
 * written as zero. */
typedef struct ItaniumPirom {
    const char *sspec;      /* e.g. "SL4LT" */
    bool production;        /* 14h[1:0] != 00b */
    uint64_t cpuid3;        /* CPUID[3]: arch revision, family, model, stepping */
    uint16_t max_core_mhz;
    uint16_t max_bus_mhz;   /* 12 bits */
    uint16_t l3_kb;
    uint8_t tap_devices;    /* 3 = 2 MB cartridge, 5 = 4 MB */
    bool scratch_present;
} ItaniumPirom;

/* Build the 128-byte image. */
void itanium_pirom_build(uint8_t out[128], const ItaniumPirom *pirom);

/* A PIROM (read-only, `image`) or a Scratch EEPROM (erased, writable; pass a
 * NULL image). */
void itanium_cart_rom_init(ItaniumCartRom *rom, const uint8_t *image,
                           bool writable);
I2cDevice itanium_cart_rom_device(ItaniumCartRom *rom, uint8_t addr7);
