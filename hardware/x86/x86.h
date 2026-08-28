#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gemu/display.h"
#include "i8086.h"

typedef enum {
    X86_MACHINE_IBM5150,
    X86_MACHINE_IOPENER, /* data/machine/x86/iopener.xml - not implemented yet */
} X86MachineType;

typedef enum {
    X86_VGA_CGA,
} X86VgaType;

#define X86_MAX_ROM_LOADS 4

typedef struct {
    const char *path;
    const char *region; /* "bios", "basic", ... - NULL for single-image machines */
} X86RomLoad;

typedef struct X86Config {
    X86RomLoad      roms[X86_MAX_ROM_LOADS];
    int             n_roms;
    X86MachineType  machine;
    X86CpuType      cpu;
    X86VgaType      vga;
    uint32_t        ram_size; /* bytes */
    GemuDisplayType display_type;
    int             display_scale;
    const char     *vnc_addr;
    bool            no_shutdown;
} X86Config;
