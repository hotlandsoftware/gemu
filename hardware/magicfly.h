#pragma once
#include "mos6502.h"
#include "mos6502cfg.h"
#include "gemu/monitor.h"
#include "gemu/gemu_display.h"
#include "gemu/vnc.h"
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * "magicfly" hardware — single-6502 video poker/gambling arcade board.
 * Hardware reference: MAME src/mame/misc/magicfly.cpp, shared by:
 *   - Magic Fly       (P&A Games, 198?)  -- MOS_MACHINE_MAGICFLY, is_7mezzo=false
 *   - 7 e Mezzo       (unknown, 198?)    -- MOS_MACHINE_MAGICFLY, is_7mezzo=true
 * (a third game on this driver, "Bonne Chance!", is not implemented here.)
 *
 * Both games share the exact same CPU/memory-map/ROM-layout/timing; they
 * differ only in: the input matrix (different games, different controls),
 * the tile-attribute color mask and boot-check mirror bit, and the fixed
 * palette. All of that is handled at runtime in machine_magicfly.c based on
 * cfg->is_7mezzo — see MosConfig in mos6502cfg.h.
 *
 * Despite superficial similarity to 5clown (6502 + MC6845 + tile video +
 * gambling I/O), this is a genuinely different, simpler, unrelated board:
 * single CPU, no encryption, no PIA6821, no real sound chip (just a 1-bit
 * bitstream DAC).
 *
 * Memory map:
 *   $0000-$07FF  NVRAM (battery-backed; includes zero page + 6502 stack)
 *   $0800        CRTC address latch (write)
 *   $0801        CRTC register (r/w)
 *   $1000-$13FF  video RAM (32x29 tiles used out of a 1KB window)
 *   $1800-$1BFF  color RAM
 *   $2800        input port (read) — multiplexed by the low nibble last
 *                written to $3000; selector 0 reads the DIP switches
 *   $3000        output port (write): bits0-3 = input selector,
 *                bit4=Coin2, bit5=Payout, bit6=Coin1, bit7=DAC bitstream
 *   $C000-$FFFF  ROM (16KB, unencrypted)
 *
 * CPU/CRTC clock: 10MHz XTAL / 16 = 625kHz. Refresh is a flat 60Hz (per the
 * driver's screen.set_refresh_hz(60), not derived from CRTC timing like
 * 5clown) -> 625000/60 ~= 10417 CPU cycles/frame.
 *
 * Video: 32x29 8x8 tiles = 256x232 visible pixels. Two disjoint GFX decode
 * regions built from the combined gfx2+gfx1+gfx0 ROM blob at load time: a
 * 1bpp "chars" bank and a 3bpp "tiles" bank (see machine_magicfly.c for the
 * exact byte ranges — verified against the driver's ROM_COPY statements).
 */

#define MAGICFLY_CPU_HZ            625000u
#define MAGICFLY_CYCLES_PER_FRAME  10417u /* 625000 / 60 */

#define MAGICFLY_FB_WIDTH   256
#define MAGICFLY_FB_HEIGHT  232
#define MAGICFLY_TILE_COLS  32
#define MAGICFLY_TILE_ROWS  29

#define MAGICFLY_VRAM_SIZE    0x400
#define MAGICFLY_NVRAM_SIZE   0x800
#define MAGICFLY_PRG_SIZE     0x4000  /* maincpu, $C000-$FFFF */
#define MAGICFLY_GFX_FILE_SIZE 0x2000 /* each of gfx0/gfx1/gfx2 */
#define MAGICFLY_GFXBNK0_SIZE 0x0800  /* chars, 1bpp */
#define MAGICFLY_GFXBNK1_SIZE 0x1800  /* tiles, 3bpp (3 planes x 0x800) */
#define MAGICFLY_N_PALETTE    32

/* ── Input actions (union of both games' distinct named buttons) ─────────── */
enum {
    MAGICFLY_ACT_COIN = 0,
    MAGICFLY_ACT_BIG,       /* 7mezzo */
    MAGICFLY_ACT_SMALL,     /* 7mezzo */
    MAGICFLY_ACT_PAYOUT,    /* both */
    MAGICFLY_ACT_TAKE,      /* 7mezzo */
    MAGICFLY_ACT_DEAL,      /* both */
    MAGICFLY_ACT_STAND,     /* 7mezzo */
    MAGICFLY_ACT_SERVICE,   /* both */
    MAGICFLY_ACT_DUP,       /* 7mezzo */
    MAGICFLY_ACT_BET,       /* 7mezzo */
    MAGICFLY_ACT_JOY_RIGHT, /* magicfly: select balloon right */
    MAGICFLY_ACT_JOY_LEFT,  /* magicfly: select balloon left */
    MAGICFLY_ACT_CANCEL,    /* magicfly */
    MAGICFLY_ACT_BET10,     /* magicfly */
    MAGICFLY_ACT_SELECT,    /* magicfly: Bet1/Select */
    MAGICFLY_NUM_ACTIONS,
};

typedef struct MagicflyState {
    Mos6502 cpu;
    const MosConfig *cfg;

    uint8_t nvram[MAGICFLY_NVRAM_SIZE]; /* $0000-$07FF */
    uint8_t *prg;                       /* 0x4000, mapped at $C000-$FFFF */
    uint8_t *gfxbnk0;                   /* 0x0800, chars, 1bpp */
    uint8_t *gfxbnk1;                   /* 0x1800, tiles, 3bpp */
    uint32_t palette[MAGICFLY_N_PALETTE]; /* 0x00RRGGBB, hand-coded (no PROM) */

    uint8_t videoram[MAGICFLY_VRAM_SIZE];
    uint8_t colorram[MAGICFLY_VRAM_SIZE];

    uint8_t crtc_addr;
    uint8_t crtc_regs[32];

    uint8_t input_selector; /* low nibble of last $3000 write */
    uint8_t dsw0;           /* raw DIP byte, selector 0 read */

    int coin_latch; /* coin-in pulse, frames remaining */

    uint32_t pixels_argb[MAGICFLY_FB_WIDTH * MAGICFLY_FB_HEIGHT];
    uint8_t  pixels_idx[MAGICFLY_FB_WIDTH * MAGICFLY_FB_HEIGHT]; /* for VNC */

    /* 1-bit delta-sigma bitstream DAC (bit7 of $3000). Not a "PC speaker"
     * gated tone — the CPU shapes the actual waveform by toggling this bit
     * at high rate, so it needs true sample-accurate zero-order-hold
     * resampling like audio/apu2a03.c and audio/okim6295.c, not the
     * fixed-frequency-gate model in audio/pcspk.c. Exposed to the user
     * as "-soundhw pcspk" anyway, since it's conceptually the same kind
     * of primitive one-bit speaker. */
    bool   dac_bit;
    double dac_sample_acc, dac_clock_pps;
    float  dac_frame_buf[1024];
    int    dac_frame_n;
    SDL_AudioDeviceID audio_dev;

    uint32_t held_actions;

    char sav_path[512];

    GemuDisplay   *display;
    GemuMonitor   *monitor;
    GemuVncServer *vnc;

    uint64_t frame;
} MagicflyState;

MagicflyState *magicfly_create (const MosConfig *cfg);
void           magicfly_run    (MagicflyState *s, const MosConfig *cfg);
void           magicfly_destroy(MagicflyState *s);
