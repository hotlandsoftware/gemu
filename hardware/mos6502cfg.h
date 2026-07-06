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
    MOS_MACHINE_5CLOWN,  /* IGS "Five Clown" arcade video poker (dual 6502) */
    MOS_MACHINE_MAGICFLY, /* misc/magicfly.cpp hardware: Magic Fly, 7 e Mezzo */
    MOS_MACHINE_UM6578,  /* UM6578/SH6578/NT6578 NES-clone-on-a-chip (plug & play TV games) */
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
    MOS_VGA_SH6578,       /* UM6578/SH6578 PPU — 256x256 pages, attribute-in-tile, no separate AT table */
} MosVgaType;

typedef enum {
    MOS_SOUND_NONE,
    MOS_SOUND_2A03,             /* Ricoh 2A03 built-in APU → SDL audio output */
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
    MOS_SOUND_2A03_MIDI,        /* Ricoh 2A03 built-in APU → MIDI output */
#endif
    MOS_SOUND_PCSPK,            /* magicfly/7mezzo: 1-bit delta-sigma bitstream DAC */
} MosSoundType;

/* Composable arcade sound cards (QEMU -soundhw-style: a comma-separated set
 * of independently present chips, ORed into MosConfig.sound_hw_mask — unlike
 * MosSoundType above, which is a single mutually-exclusive NES/Famicom APU
 * choice). Currently 5clown-only; both chips are stubbed (no synthesis). */
#define MOS_SOUNDHW_AY8910  0x01u
#define MOS_SOUNDHW_OKI6295 0x02u

typedef enum {
    NES_DEVICE_NONE = 0,
    NES_DEVICE_CONTROLLER,  /* NES Standard Controller */
    NES_DEVICE_ZAPPER,      /* NES Zapper light gun */
    NES_DEVICE_KEYBOARD,    /* Famicom Keyboard */
    NES_DEVICE_ROB,         /* R.O.B. (Robotic Operating Buddy) */
    NES_DEVICE_ROB_FAMICOM, /* Family Computer Robot */
    NES_DEVICE_FC2_MIC,     /* Famicom Controller 2 with built-in microphone */
    NES_DEVICE_MOUSE,       /* UM6578/PS/2-compatible mouse (24-bit reply via standard $4016 shift register) */
} NesDeviceType;

typedef enum {
    MOS_CG_AUTO,
    MOS_CG_CM2140,
    MOS_CG_CM2140_COMPAT,
} MosCharGenType;

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
    bool            no_shutdown;
    bool            has_start_addr;
    uint16_t        start_addr;
    const char     *cart_path;   /* iNES .nes cartridge file (NES machine) */
    bool            fds_enabled; /* Famicom Disk System addon active        */
    const char     *fda_path;    /* FDS disk image path (NULL = no disk)    */
    MosSoundType    sound;
    bool            sound_explicit; /* user passed -soundhw; skip auto-default */
    uint32_t        sound_hw_mask; /* MOS_SOUNDHW_* bits (5clown: AY8910/OKI6295) */
    MosCharGenType  char_gen;
    bool            char_gen_explicit;
    NesDeviceType   ports[NES_PORTS]; /* devices on controller ports 1–2 */
    int             n_ports;          /* how many ports were explicitly assigned */
    bool            ppu_debug;
    bool            is_pal;      /* PAL/Dendy: 312 lines, ~50 Hz frame rate */
    bool            is_dendy;   /* Dendy Famiclone: NTSC-compatible PPU/APU within PAL 50 Hz */
    bool            is_arcade;  /* NES-based arcade cabinet (VS. System, etc.) — coin-op, DIP switches */
    bool            is_7mezzo;  /* MOS_MACHINE_MAGICFLY game variant: "7 e Mezzo" vs Magic Fly */
    bool            kim_keyboard; /* KIM-1: -device kim-keypad → visual keypad overlay */
    bool            want_wozmon;  /* -device wozmon → patch wozmon into KIM-1 ROM at $1AA0 */
    bool            a1ci;         /* Apple I Cassette Interface attached */
    const char     *tape_path;   /* KIM-1: cassette tape image (.kim binary), NULL = none */
    uint16_t        tape_addr;   /* Apple I/COSMAC helper load address for raw tapes */
    bool            tape_addr_explicit; /* Apple I: true if -tape gave ADDR:FILE explicitly */
    GemuSerial     *serial;      /* serial terminal attached to this machine (NULL = none) */
    uint32_t        mem_size;  /* generic machine RAM in bytes (0 = default 64KB) */
    const char     *lua_path;  /* -lua FILE: run an FCEUX-Lua-subset script (NES only) */
} MosConfig;
