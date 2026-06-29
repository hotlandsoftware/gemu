#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gemu/display.h"
#include "gemu/serial.h"

typedef enum {
    MOS_MACHINE_GENERIC,
    MOS_MACHINE_APPLE1, /* Apple I */
    MOS_MACHINE_NES,    /* Nintendo Entertainment System */
    MOS_MACHINE_KIM1,   /* MOS KIM-1 single-board computer */
} MosMachineType;

typedef enum {
    MOS_CPU_6502,
    MOS_CPU_2A03,   /* Ricoh 2A03: 6502 without decimal mode, built into NES */
    MOS_CPU_2A07,   /* Ricoh 2A07: PAL NES CPU/APU, no decimal mode */
    MOS_CPU_6501,   /* 6800-pinout predecessor; same ISA as 6502 */
} MosCpuType;

typedef enum {
    MOS_VGA_NONE,
    MOS_VGA_RP2C02,  /* Ricoh RP2C02 — NES PPU */
    MOS_VGA_RP2C04_0004,  /* Ricoh RP2C04-0004 — VS. System RGB PPU */
} MosVgaType;

typedef enum {
    MOS_SOUND_NONE,
    MOS_SOUND_2A03,             /* Ricoh 2A03 built-in APU → SDL audio output */
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
    MOS_SOUND_2A03_MIDI,        /* Ricoh 2A03 built-in APU → MIDI output */
#endif
} MosSoundType;

typedef enum {
    NES_DEVICE_NONE = 0,
    NES_DEVICE_CONTROLLER,  /* NES Standard Controller */
    NES_DEVICE_ZAPPER,      /* NES Zapper light gun */
    NES_DEVICE_KEYBOARD,    /* Famicom Keyboard */
    NES_DEVICE_ROB,         /* R.O.B. (Robotic Operating Buddy) */
    NES_DEVICE_ROB_FAMICOM, /* Family Computer Robot */
    NES_DEVICE_FC2_MIC,     /* Famicom Controller 2 with built-in microphone */
} NesDeviceType;

#define NES_PORTS 2

#define MOS_MAX_ROM_LOADS 8

typedef struct {
    const char *path;
    const char *region;
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
    bool            is_pal;      /* PAL/Dendy: 312 lines, ~50 Hz frame rate */
    bool            is_dendy;   /* Dendy Famiclone: NTSC-compatible PPU/APU within PAL 50 Hz */
    bool            is_arcade;  /* NES-based arcade cabinet (VS. System, etc.) — coin-op, DIP switches */
    bool            kim_keyboard; /* KIM-1: -device kim-keypad → visual keypad overlay */
    bool            want_wozmon;  /* -device wozmon → patch wozmon into KIM-1 ROM at $1AA0 */
    const char     *tape_path;   /* KIM-1: cassette tape image (.kim binary), NULL = none */
    GemuSerial     *serial;      /* serial terminal attached to this machine (NULL = none) */
    uint32_t        mem_size;  /* generic machine RAM in bytes (0 = default 64KB) */
} MosConfig;
