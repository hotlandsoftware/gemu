#pragma once

#include "gemu/display.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Generic Itanium PC - a from-scratch GEMU test platform, not modeling any
 * real machine. Its firmware is our own (reference/gemu-efi), written and
 * grown alongside the emulator, so each new CPU/device feature can be
 * exercised by a small purpose-built test before being trusted against
 * unmodified, undocumented real-hardware firmware (HP i2000's).
 *
 * Physical memory map:
 *
 *   0x0000000000000000 +--------------------------+
 *                      | SDRAM (default 512 MiB)  |
 *          ram_size    +--------------------------+
 *                      | open bus                 |
 *   0x00000000000A0000 +--------------------------+
 *                      | VGA VRAM aperture         | (standard legacy
 *   0x00000000000C0000 +--------------------------+  0xA0000-0xBFFFF hole,
 *                      | open bus                 |   same convention real
 *   0x00000000C0000000 +--------------------------+  PC firmware expects)
 *                      | VGA control registers,   |
 *                      | memory-mapped 1:1 by     |
 *                      | legacy port number - see |
 *                      | GENERIC_VGA_IO_BASE      |
 *   0x00000000C0000030 +--------------------------+
 *                      | open bus                 |
 *   0x00000000FFF00000 +--------------------------+
 *                      | firmware ROM (1 MiB,     |
 *                      | top-aligned so its last  |
 *                      | bundle lands at the      |
 *                      | architected reset vector)|
 *   0x0000000100000000 +--------------------------+
 *
 * Unlike i2000, there's no legacy IA-32 real-mode reset path to model: our
 * own firmware starts directly in native IA-64 mode at the reset vector,
 * and VGA is exposed as ordinary memory-mapped registers rather than the
 * sparse legacy-I/O port encoding real x86-derived firmware expects.
 */

#define GENERIC_RAM_MAX      0x80000000ull                       /* 2 GiB */
#define GENERIC_ROM_SIZE      0x100000u                          /* 1 MiB */
#define GENERIC_ROM_BASE      (0x100000000ull - GENERIC_ROM_SIZE)
#define GENERIC_VGA_IO_BASE   0x00000000C0000000ull
#define GENERIC_VGA_IO_SIZE   0x30u                    /* ports 3B0h-3DFh */

typedef struct {
    uint64_t        ram_size;
    GemuDisplayType display_type;
    int             display_scale;
    bool            no_shutdown;
} GenericConfig;

typedef struct Ia64GenericState Ia64GenericState;

Ia64GenericState *ia64_generic_create(const GenericConfig *cfg);
void               ia64_generic_run(Ia64GenericState *s, const GenericConfig *cfg);
void               ia64_generic_destroy(Ia64GenericState *s);

bool ia64_generic_load_firmware(Ia64GenericState *s, const char *path);
