#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gemu/display.h"

typedef enum {
    Z80_MACHINE_GENERIC,
    Z80_MACHINE_LAGONDA,   /* data/machine/z80/lagonda.xml - not emulated yet */
} Z80MachineType;

#define Z80_MAX_ROM_LOADS 8

typedef struct {
    uint32_t    addr;
    const char *path;
    const char *region;   /* romdb region name, NULL for -rom ADDR:FILE */
} Z80RomLoad;

typedef struct Z80Config {
    Z80MachineType  machine;
    Z80RomLoad      roms[Z80_MAX_ROM_LOADS];
    int             n_roms;
    const char     *cpm_path;       /* -cpm FILE: CP/M .COM program at 0100h */
    uint16_t        start_addr;
    bool            has_start_addr;
    uint32_t        clock_hz;
    GemuDisplayType display_type;
    int             display_scale;
    bool            no_shutdown;
} Z80Config;
