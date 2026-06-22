#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gemu/display.h"

typedef enum {
    RCA_MACHINE_GENERIC,
    RCA_MACHINE_COSMAC_VIP,
    RCA_MACHINE_DESTROYER,
    RCA_MACHINE_STUDIO2,
    RCA_MACHINE_PECOM32,
} RcaMachineType;

typedef enum {
    RCA_CPU_NONE,
    RCA_CPU_CDP1802,
} RcaCpuType;

typedef enum {
    RCA_VGA_NONE,
    RCA_VGA_CDP1861,
    RCA_VGA_CDP1869,
} RcaVgaType;

typedef enum {
    RCA_KEYBOARD_NONE,
    RCA_KEYBOARD_KEYPAD,
    RCA_KEYBOARD_VP601,
    RCA_KEYBOARD_BOTH,
    RCA_KEYBOARD_GENERIC,
} RcaKeyboardType;

typedef enum {
    RCA_SOUND_NONE,
    RCA_SOUND_PCSPK,
} RcaSoundHwType;

typedef enum {
    RCA_TV_NTSC = 0,
    RCA_TV_PAL,
} RcaTvMode;

#define RCA_MAX_ROM_LOADS 8

typedef struct {
    const char *path;
    uint32_t    addr;
} RcaRomLoad;

typedef struct RcaConfig {
    RcaRomLoad      roms[RCA_MAX_ROM_LOADS];
    int             n_roms;
    RcaMachineType  machine;
    RcaCpuType      cpu;
    RcaVgaType      vga;
    RcaKeyboardType keyboard;
    RcaSoundHwType  sound_hw;
    GemuDisplayType display_type;
    int             display_scale;
    const char     *vnc_addr;
    bool            has_start_addr;
    uint16_t        start_addr;
    /* Compact cassette tape */
    const char     *tape_path;       /* NULL = no tape inserted */
    uint16_t        tape_addr;       /* load address for raw binary tapes */
    /* Cartridge */
    const char     *cartridge_path;  /* NULL = no cartridge */
    /* TV system */
    RcaTvMode       tv_mode;         /* RCA_TV_NTSC (default) or RCA_TV_PAL */
} RcaConfig;
