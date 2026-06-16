#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gemu/display.h"

typedef enum {
    MOS_MACHINE_GENERIC,
    MOS_MACHINE_NES,    /* Nintendo Entertainment System */
} MosMachineType;

typedef enum {
    MOS_CPU_6502,
    MOS_CPU_2A03,   /* Ricoh 2A03: 6502 without decimal mode, built into NES */
} MosCpuType;

typedef enum {
    MOS_VGA_NONE,
    MOS_VGA_RP2C02,  /* Ricoh RP2C02 — NES PPU */
} MosVgaType;

typedef enum {
    MOS_SOUND_NONE,
    MOS_SOUND_2A03,             /* Ricoh 2A03 built-in APU → SDL audio output */
#ifdef HAVE_ALSA
    MOS_SOUND_2A03_MIDI,        /* Ricoh 2A03 built-in APU → ALSA MIDI output */
#endif
} MosSoundType;

typedef enum {
    NES_DEVICE_NONE = 0,
    NES_DEVICE_CONTROLLER,  /* NES Standard Controller */
    NES_DEVICE_ZAPPER,      /* NES Zapper light gun */
    NES_DEVICE_KEYBOARD,    /* Famicom Keyboard */
} NesDeviceType;

#define NES_PORTS 2

#define MOS_MAX_ROM_LOADS 8

typedef struct {
    const char *path;
    uint32_t    addr;
} MosRomLoad;

typedef struct MosConfig {
    MosRomLoad      roms[MOS_MAX_ROM_LOADS];
    int             n_roms;
    MosMachineType  machine;
    MosCpuType      cpu;
    MosVgaType      vga;
    GemuDisplayType display_type;
    GemuRendererType display_renderer;
    int             display_scale;
    const char     *vnc_addr;
    bool            has_start_addr;
    uint16_t        start_addr;
    const char     *cart_path;   /* iNES .nes cartridge file (NES machine) */
    bool            fds_enabled; /* Famicom Disk System addon active        */
    const char     *fda_path;    /* FDS disk image path (NULL = no disk)    */
    MosSoundType    sound;
    bool            sound_explicit; /* user passed -soundhw; skip auto-default */
    NesDeviceType   ports[NES_PORTS]; /* devices on controller ports 1–2 */
    int             n_ports;          /* how many ports were explicitly assigned */
    bool            ppu_debug;
    bool            is_pal;      /* PAL mode: 312 lines, ~50 Hz, slower CPU clock */
    uint32_t        mem_size;  /* generic machine RAM in bytes (0 = default 64KB) */
} MosConfig;
