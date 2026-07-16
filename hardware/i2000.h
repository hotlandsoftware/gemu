#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * HP i2000 - Itanium (Merced) workstation, Intel 460GX chipset.
 *
 * Skeleton physical memory map (what exists so far):
 *
 *   0x0000000000000000 +--------------------------+
 *                      | SDRAM (default 512 MiB,  |
 *                      | i2000 max 2 GiB)         |
 *          ram_size    +--------------------------+
 *                      | (open bus - 460GX SAC/   |
 *                      |  PXB/IFB decode later)   |
 *   0x00000000FFC00000 +--------------------------+
 *                      | 4 MiB firmware flash     |
 *                      | (PAL + SAL + EFI, e.g.   |
 *                      |  bios130.BIN)            |
 *   0x0000000100000000 +--------------------------+
 *
 * The flash tail carries the architected IA-64 entry bundles: PALE
 * entrypoints at 0xFFFFFF80..0xFFFFFFB0 (PALE_RESET fetch begins at
 * IP = 0xFFFFFFB0, 4 GiB - 0x50), firmware pointer slots below
 * 0xFFFFFFF0, and the IA-32 compatibility reset vector at 0xFFFFFFF0.
 */

#define I2000_FLASH_SIZE   0x400000u                              /* 4 MiB */
#define I2000_FLASH_BASE   (0x100000000ull - I2000_FLASH_SIZE)    /* 0xFFC00000 */
#define I2000_RAM_MAX      0x80000000ull                          /* 2 GiB */
#define IA64_RESET_VECTOR  0xFFFFFFB0ull

typedef struct {
    uint64_t ram_size;
    bool     no_shutdown;
} Ia64Config;

typedef struct Ia64I2000State Ia64I2000State;

Ia64I2000State *ia64_i2000_create(const Ia64Config *cfg);
void            ia64_i2000_run(Ia64I2000State *s, const Ia64Config *cfg);
void            ia64_i2000_destroy(Ia64I2000State *s);

/* Load a firmware flash image (at most 4 MiB, top-aligned into the flash
 * window so the reset bundles land at their architected addresses). */
bool ia64_i2000_load_firmware(Ia64I2000State *s, const char *path);

/* Physical address space accessors - the future Merced core (and 460GX
 * chipset models) go through these. Bundle/word fetch helpers will be
 * layered on top once the CPU lands. */
uint8_t ia64_i2000_phys_read8 (Ia64I2000State *s, uint64_t addr);
void    ia64_i2000_phys_write8(Ia64I2000State *s, uint64_t addr, uint8_t val);
