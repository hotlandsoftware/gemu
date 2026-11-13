/* See itanium_cart.h. Table and section numbers are the 800/733 MHz
 * datasheet's (249634-002). */
#include "i2c/itanium_cart.h"

#include <string.h>

/* The checksum of a section: the 8-bit sum of its bytes before the checksum
 * byte. The header's sum over bytes 00h-0Ch is stored at 0Dh. */
static uint8_t sum8(const uint8_t *bytes, unsigned count) {
    uint8_t sum = 0;
    for (unsigned i = 0; i < count; i++)
        sum = (uint8_t)(sum + bytes[i]);
    return sum;
}

/* Fields from 20h on are packed on nibble boundaries: value bit 0 lands in
 * bit 4 of byte `at` (Table 6-4: "20h[7:4]-22h[3:0]"). */
static void put_nibbles(uint8_t *mem, unsigned at, uint32_t value,
                        unsigned bits) {
    for (unsigned i = 0; i < bits; i++) {
        unsigned bit = 4 + i;
        if ((value >> i) & 1)
            mem[at + bit / 8] |= (uint8_t)(1u << (bit % 8));
    }
}

void itanium_pirom_build(uint8_t out[128], const ItaniumPirom *pirom) {
    uint8_t *m = out;
    memset(m, 0, 128);

    /* Header 00h-0Dh: data format revision, size, section pointers. */
    m[0x01] = 0x80;
    m[0x03] = 0x0e; m[0x04] = 0x16; m[0x05] = 0x26; m[0x06] = 0x33;
    m[0x07] = 0x3b; m[0x08] = 0x61; m[0x09] = 0x65; m[0x0a] = 0x79;
    m[0x0d] = sum8(m, 0x0d);

    /* Processor 0Eh-15h: the S-spec without its first character, laid out
     * as the table's example ("SL4LT": 0Eh '0', 0Fh '0', 10h 'T', 11h 'L',
     * 12h '4', 13h 'L'). */
    m[0x0e] = '0'; m[0x0f] = '0';
    if (pirom->sspec && strlen(pirom->sspec) >= 5) {
        const char *tail = pirom->sspec + 1;
        m[0x10] = (uint8_t)tail[3]; m[0x11] = (uint8_t)tail[2];
        m[0x12] = (uint8_t)tail[1]; m[0x13] = (uint8_t)tail[0];
    }
    m[0x14] = pirom->production ? 0x01 : 0x00;
    m[0x15] = sum8(m + 0x0e, 7);

    /* Core 16h-25h: CPUID fields and maximum frequencies. The core voltage
     * fields stay zero. */
    m[0x16] = (uint8_t)(pirom->cpuid3 & 0xff);          /* architecture revision */
    m[0x17] = (uint8_t)(pirom->cpuid3 >> 24);           /* family */
    m[0x18] = (uint8_t)(pirom->cpuid3 >> 16);           /* model */
    m[0x19] = (uint8_t)(pirom->cpuid3 >> 8);            /* revision (stepping) */
    m[0x1d] = (uint8_t)pirom->max_core_mhz;
    m[0x1e] = (uint8_t)(pirom->max_core_mhz >> 8);
    m[0x1f] = (uint8_t)pirom->max_bus_mhz;
    m[0x20] = (uint8_t)((pirom->max_bus_mhz >> 8) & 0x0f);
    put_nibbles(m, 0x25, sum8(m + 0x16, 0x25 - 0x16), 8);

    /* L3 cache 26h-33h: the size. */
    put_nibbles(m, 0x2a, pirom->l3_kb, 16);
    put_nibbles(m, 0x32, sum8(m + 0x26, 0x32 - 0x26), 8);

    /* Cartridge 33h and part numbers 3Bh are left zero. */
    put_nibbles(m, 0x3a, sum8(m + 0x33, 0x3a - 0x33), 8);
    put_nibbles(m, 0x60, sum8(m + 0x3b, 0x60 - 0x3b), 8);
    /* Thermal reference 61h: absent. */
    put_nibbles(m, 0x64, sum8(m + 0x61, 0x64 - 0x61), 8);
    /* Features 65h: cartridge flag bit 2 says a scratch EEPROM is present;
     * 75h holds the TAP chain length. */
    put_nibbles(m, 0x71, pirom->scratch_present ? 0x04u : 0u, 32);
    put_nibbles(m, 0x75, pirom->tap_devices & 0x0fu, 4);
    m[0x78] = sum8(m + 0x65, 0x78 - 0x65);
}

void itanium_cart_rom_init(ItaniumCartRom *rom, const uint8_t *image,
                           bool writable) {
    memset(rom, 0, sizeof(*rom));
    if (image)
        memcpy(rom->mem, image, sizeof(rom->mem));
    else
        memset(rom->mem, 0xff, sizeof(rom->mem));
    rom->writable = writable;
}

static bool rom_start(void *ctx, bool read) {
    ItaniumCartRom *rom = ctx;
    rom->addr_next = !read;     /* a write transfer starts with the address */
    return true;
}

static bool rom_write(void *ctx, uint8_t byte) {
    ItaniumCartRom *rom = ctx;
    if (rom->addr_next) {
        rom->addr_next = false;
        rom->ptr = byte & 0x7f;
        rom->page_base = rom->ptr & 0x78;
        return true;
    }
    if (!rom->writable)
        return true;            /* the PIROM ACKs and keeps its data */
    rom->mem[rom->ptr] = byte;
    /* Writes roll over from the last byte of the current eight-byte page to
     * the first byte of the same page. */
    rom->ptr = (uint8_t)(rom->page_base | ((rom->ptr + 1) & 7));
    return true;
}

static uint8_t rom_read(void *ctx, bool ack) {
    (void)ack;
    ItaniumCartRom *rom = ctx;
    uint8_t value = rom->mem[rom->ptr];
    rom->ptr = (rom->ptr + 1) & 0x7f;   /* reads roll over at 128 */
    return value;
}

static void rom_stop(void *ctx) {
    ItaniumCartRom *rom = ctx;
    rom->addr_next = false;
}

I2cDevice itanium_cart_rom_device(ItaniumCartRom *rom, uint8_t addr7) {
    return (I2cDevice){ .addr = addr7, .ctx = rom, .start = rom_start,
                        .write = rom_write, .read = rom_read, .stop = rom_stop };
}
