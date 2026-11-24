#pragma once
#include "mos6502cfg.h"
#include "storage/fds.h"
#include "mos6502.h"
#include "rp2c02.h"
#include "apu2a03.h"
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
#  include "apu_midi.h"
#endif
#include "gemu/monitor.h"
#include "cart/nes_cart.h"
#include "nes_pins.h"
#ifdef HAVE_ROB
#  include "input/rob.h"
#endif
#include <stdint.h>
#include <stdbool.h>

/* ── NES controller buttons (shift-register order: A comes out first) ─────── */
#define NES_BTN_A      0x01u
#define NES_BTN_B      0x02u
#define NES_BTN_SELECT 0x04u
#define NES_BTN_START  0x08u
#define NES_BTN_UP     0x10u
#define NES_BTN_DOWN   0x20u
#define NES_BTN_LEFT   0x40u
#define NES_BTN_RIGHT  0x80u

#define NES_GG_MAX 16

typedef struct {
    uint16_t addr;
    uint8_t  val;
    uint8_t  cmp;
    bool     has_cmp;
    char     code[9]; /* 6- or 8-char code + NUL */
} NesGgPatch;

typedef struct NesState {
    Mos6502  cpu;
    Rp2c02   ppu;
    const MosConfig *cfg;

    uint8_t  ram[0x800];  /* 2 KB CPU-side work RAM */

    /* The cartridge slot (FDS: the RAM adapter's CHR RAM) */
    NesCart  cart;

    Apu2a03  apu;
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
    ApuMidi  apu_midi;
#endif

    /* OAM DMA state */
    bool     dma_pending;
    uint8_t  dma_page;

    /* Controllers */
    uint8_t  ctrl_state[2]; /* live button bits */
    uint8_t  ctrl_shift[2]; /* serial shift register */
    bool     ctrl_strobe;
    bool     mic_noise;     /* FC2 microphone active (drives $4016 D2) */
    bool     port2_controller; /* a controller in port 2 though none is configured
                                  (netplay's player 2) */

    char           cart_path_buf[512]; /* path of currently loaded cartridge */
    uint64_t       ppu_synced_cpu_cycle;
    uint64_t       apu_synced_cpu_cycle;   /* the APU, expansion audio and mapper counters */
    bool           apu_syncing;
    uint64_t       apu_deadline;           /* the APU may lag until this cycle (apu2a03_until_event) */

    /* The 2A03's DMA unit: OAM and DMC DMA halt the CPU on its next read */
    bool           dma_halt;               /* a DMA is waiting for a read cycle */
    bool           dmc_dma, dmc_dummy, dmc_abort;
    bool           oam_dma;
    uint8_t        oam_page;
    bool           in_dma;
    GemuMonitor   *monitor;

    /* Called before each instruction when set (Lua's exec hooks) */
    void         (*exec_hook)(void *ud, uint16_t pc);
    void          *exec_hook_ud;

    /* Game Genie patches */
    NesGgPatch gg_patches[NES_GG_MAX];
    int        gg_count;

    /* Zapper (port 2) */
    int      zapper_x, zapper_y;      /* cursor position in NES pixel coords */
    int      zapper_trigger_ttl;       /* frames remaining in trigger pulse */

    /* Monitor key injection */
    uint8_t  ctrl_inject_mask;        /* button bits to hold */
    int      ctrl_inject_frames;      /* frames remaining (counts down to 0) */

    /* VS. System arcade */
    uint32_t vs_palette_rgb[64]; /* 2C04 palette; loaded from .pal file if present */
    uint8_t  vs_dip;          /* 8-bit DIP switch bank (SW1 1–8) */
    uint8_t  vs_coin_counter; /* last value written/read through $4020-$5fff */
    int      vs_coin_latch[2]; /* coin pulse countdown in frames; >0 = coin signal active */
    bool     vs_coin_prev[2];  /* coin key state from previous frame (edge detection) */
    bool     vs_service;       /* service button state */

    /* Famicom Disk System */
    bool     fds_enabled;
    FdsState fds;

    /* Debug > Cartridge Pins (nes_pins.h) */
    NesPins  pins;
    unsigned pins_seen;      /* pins.version last applied to the PPU and APU */
    uint32_t clock_seen;     /* pins.clock_hz last applied */
    bool     cart_irq;       /* the cartridge (mapper, FDS) pulling /IRQ low */
    uint8_t  cpu_bus;        /* last value on the CPU data bus */

#ifdef HAVE_ROB
    RobState    rob;
#endif
} NesState;

/* The CPU's /IRQ: the APU's frame and DMC flags, and the cartridge's
 * line through connector pin 15.  Call after changing either. */
void      nes_update_irq(NesState *s);

/* ── The console ─────────────────────────────────────────────────────────────
 * machine_nes.c.  The front end (nes_frontend.c: window, input, dialogs,
 * save files, pacing; nes_netplay.c) drives it through these and through
 * the fields documented as inputs above (ctrl_state, zapper_*, mic_noise,
 * vs_coin_latch, vs_service, gg_patches, pins). */

/* The console with its cartridge (cfg->cart_path), FDS or VS. ROM chips
 * loaded, and its monitor; not yet reset.  NULL on failure. */
NesState *nes_core_create(const MosConfig *cfg);
void      nes_core_destroy(NesState *s);
void      nes_reset(NesState *s);
/* One instruction, the rest of the console catching up as far as needed. */
void      nes_step_insn(NesState *s);
/* After single-stepping: everything brought up to the CPU. */
void      nes_sync(NesState *s);
/* Until the PPU finishes a frame, or the monitor stops at a breakpoint or
 * pauses; then the frame's audio goes out. */
void      nes_run_frame(NesState *s);
/* Debug > Cartridge Pins changed: apply them to the PPU, APU and clock. */
void      nes_apply_pins(NesState *s);
/* The CPU's bus, as the CPU sees it (Lua's memory hooks wrap these). */
uint8_t   nes_cpu_read(uint16_t addr, void *ud);
void      nes_cpu_write(uint16_t addr, uint8_t val, void *ud);

/* FDS: a disk is in s->fds (at power-on or changed); HLE BIOS boots it. */
void      nes_fds_disk_inserted(NesState *s);

/* Cartridges.  insert: the slot must be empty (nes_eject_cartridge). */
bool      nes_insert_cartridge(NesState *s, const char *path);
void      nes_eject_cartridge(NesState *s);

/* Save states, in memory: the whole console, CHR RAM included. */
bool      nes_snapshot_save(NesState *s, uint8_t **data, size_t *len);
bool      nes_snapshot_load(NesState *s, const uint8_t *data, size_t len);

static inline bool nes_device_is_rob(NesDeviceType dev) {
    return dev == NES_DEVICE_ROB || dev == NES_DEVICE_ROB_FAMICOM;
}

/* The front end (nes_frontend.c): what mos_setup.c runs. */
NesState *nes_create (const MosConfig *cfg);
void      nes_run    (NesState *s, const MosConfig *cfg);
void      nes_destroy(NesState *s);
