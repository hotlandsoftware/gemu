#pragma once

#include "gemu/display.h"
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
 *   0x000000007FF00000 +--------------------------+
 *                      | chipset scratch (1 MiB), |
 *                      | always backed regardless |
 *                      | of ram_size - firmware   |
 *                      | places fixed-address     |
 *                      | descriptors here         |
 *   0x0000000080000000 +--------------------------+
 *                      | (open bus)               |
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

/* The SDV 0.99 debug BIOS places a small per-processor descriptor just
 * below I2000_RAM_MAX regardless of how much RAM is actually configured:
 * confirmed live (ar.lc tracing with -m 64M) that it zeroes a field there,
 * reads it straight back to compute a loop trip-count, and separately
 * stores a pointer ~0x3E000 below the same boundary into that descriptor.
 * Real chipsets route this whole architected window even when DRAM below
 * I2000_RAM_MAX isn't fully populated - the open-bus fallback elsewhere in
 * bus_read() returns all-ones, not the zero this probe expects, so it read
 * back garbage and spun forever. Backing it separately from `ram` keeps it
 * present at any -m size without inflating detected RAM. Sized well above
 * the largest offset seen so far (~0x3E000) since more of this pattern may
 * turn up with further tracing. */
#define I2000_CHIPSET_SCRATCH_SIZE 0x100000ull                    /* 1 MiB */
#define I2000_CHIPSET_SCRATCH_BASE (I2000_RAM_MAX - I2000_CHIPSET_SCRATCH_SIZE)

typedef struct {
    uint64_t        ram_size;
    const char     *cdrom_path;
    /* -hda FILE: raw read-write disk image (generic machine only for now). */
    const char     *hda_path;
    GemuDisplayType display_type;
    int             display_scale;
    const char     *vnc_addr;
    const char     *vga;
    bool            no_shutdown;
    bool            menu_disabled; /* machine XML's menu="disabled" */
    bool            mouse_enabled; /* -device mouse (i2000 only) */
    bool            i82559_enabled; /* -net nic,model=i82559/baset */
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
