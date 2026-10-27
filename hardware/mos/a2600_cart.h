#pragma once
/*
 * Atari 2600 cartridges: ROM images and the bank-switching schemes of the
 * commercial library (and the common homebrew ones).
 *
 *   2K, 4K            plain ROM (2K mirrored)
 *   F8, F6, F4        Atari: 8/16/32K, any access to $1FF8.. / $1FF6.. /
 *                     $1FF4.. selects a 4K bank; "SC" adds the 128-byte
 *                     Superchip RAM (write $1000-$107F, read $1080-$10FF)
 *   EF, DF, BF        64/128/256K homebrew variants of the same (+SC)
 *   FE                Activision 8K: bank follows the return address high
 *                     byte seen on the bus after an access to $01FE
 *   E0                Parker Bros 8K: three switchable 1K slices + fixed
 *   3F, 3E            Tigervision: write to $3F picks the 2K bank at $1000
 *                     ($3E: 1K RAM banks instead)
 *   E7                M Network 16K: 2K banks, 1K + 4x256 bytes RAM
 *   FA                CBS RAM Plus 12K: 3 banks + 256 bytes RAM
 *   F0                Megaboy 64K: $1FF0 steps through 16 banks
 *   UA                UA Ltd 8K: $0220 / $0240 select the bank
 *   CV                CommaVid: 2K ROM + 1K RAM
 *   0840              Econobanking 8K: $0800 / $0840 select the bank
 *   SB                Superbanking: $0800-$0FFF select up to 64 banks
 *   DPC               Pitfall II: 8K program + DPC chip (data fetchers,
 *                     random numbers, 3-voice music)
 *   AR                Starpath Supercharger: 6K RAM, loads from tape
 *                     images (8448 bytes per load) with a built-in BIOS
 *
 * The scheme is detected from the image (size and code signatures, as
 * Stella does), or forced with a file extension such as ".f8" or ".e0".
 */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    A26_CART_2K, A26_CART_4K,
    A26_CART_F8, A26_CART_F6, A26_CART_F4,
    A26_CART_EF, A26_CART_DF, A26_CART_BF,
    A26_CART_FE, A26_CART_E0, A26_CART_3F, A26_CART_3E,
    A26_CART_E7, A26_CART_FA, A26_CART_F0, A26_CART_UA,
    A26_CART_CV, A26_CART_0840, A26_CART_SB, A26_CART_DPC,
    A26_CART_AR,
    A26_CART_COUNT
} A26CartType;

#define A26_CART_RAM_MAX 0x8000u   /* 3E: up to 32 x 1K */

typedef struct {
    uint8_t  tops[8], bottoms[8], flags[8];
    uint16_t counters[8];
    bool     music[3];
    uint8_t  random;
    uint64_t music_cycle;     /* CPU cycle of the last music fetcher update */
    double   music_frac;
} A26Dpc;

typedef struct {
    uint8_t  control;         /* $1FF8 control byte */
    uint32_t page[2];         /* RAM/ROM offsets mapped at $1000 and $1800 */
    bool     page_rom[2];
    bool     write_enabled;
    uint8_t  data_hold;       /* byte latched by an access to $1000-$10FF */
    uint64_t data_cycle;      /* cpu cycle of that access */
    bool     data_pending;
    uint32_t load_count;      /* loads in the tape image */
    uint8_t  bios[2048];
} A26Supercharger;

typedef struct A2600Cart {
    A26CartType type;
    bool     sc;               /* Superchip RAM */
    uint8_t *rom;
    uint32_t rom_size;
    uint32_t banks;            /* 4K banks (or the scheme's own bank size) */
    uint16_t bank[4];          /* bank registers, meaning per scheme */
    bool     fe_pending;       /* FE: last access was $01FE */
    uint8_t  ram[A26_CART_RAM_MAX];
    uint32_t ram_size;
    double   cpu_hz;           /* for the DPC music oscillator */
    uint64_t distinct;         /* bus accesses to a new address (the machine
                                  counts them; the Supercharger's write
                                  timing is measured in them) */
    A26Dpc   dpc;
    A26Supercharger ar;
} A2600Cart;

/* Load path (forced type from the extension if it names one). */
bool        a26_cart_load(A2600Cart *c, const char *path);
void        a26_cart_free(A2600Cart *c);
void        a26_cart_reset(A2600Cart *c);
const char *a26_cart_type_name(A26CartType t);

/* Accesses with A12 set ($1000-$1FFF). */
uint8_t     a26_cart_read(A2600Cart *c, uint16_t addr, uint8_t bus, uint64_t cycle);
void        a26_cart_write(A2600Cart *c, uint16_t addr, uint8_t val, uint64_t cycle);
/* Every bus access, after it completes: schemes that watch addresses
 * outside the cartridge space (FE, 3F, 3E, UA, 0840, SB). */
void        a26_cart_snoop(A2600Cart *c, uint16_t addr, uint8_t val, bool write);
/* Supercharger BIOS hotspot: lets the cartridge read and write 2600 RAM
 * ($80, $FE, $FF) when it loads a tape image. */
void        a26_cart_set_ram_access(uint8_t (*peek)(void *ud, uint8_t addr),
                                    void (*poke)(void *ud, uint8_t addr, uint8_t val),
                                    void *ud);
/* Debugger view of $1000-$1FFF under the current banking. */
uint8_t     a26_cart_peek(const A2600Cart *c, uint16_t addr);
/* Bank state as one line, for mapperdump. */
void        a26_cart_describe(const A2600Cart *c, char *buf, int len);
