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
 * "7 e Mezzo" — single-6502 video poker arcade board.
 * Hardware reference: MAME src/mame/misc/magicfly.cpp ("magicfly" hardware
 * family, shared with Magic Fly and Bonne Chance!). Despite superficial
 * similarity to 5clown (6502 + MC6845 + tile video + gambling I/O), this is
 * a genuinely different, simpler, unrelated board: single CPU, no
 * encryption, no PIA6821, no real sound chip (just a 1-bit bitstream DAC).
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
 * regions built from the combined ns2+ns1+ns0 ROM blob at load time: a 1bpp
 * "chars" bank and a 3bpp "tiles" bank (see machine_7mezzo.c for the exact
 * byte ranges — verified against the driver's ROM_COPY statements).
 */

#define MEZZO7_CPU_HZ            625000u
#define MEZZO7_CYCLES_PER_FRAME  10417u /* 625000 / 60 */

#define MEZZO7_FB_WIDTH   256
#define MEZZO7_FB_HEIGHT  232
#define MEZZO7_TILE_COLS  32
#define MEZZO7_TILE_ROWS  29

#define MEZZO7_VRAM_SIZE    0x400
#define MEZZO7_NVRAM_SIZE   0x800
#define MEZZO7_PRG_SIZE     0x4000  /* ns3_1.bin, $C000-$FFFF */
#define MEZZO7_GFX_FILE_SIZE 0x2000 /* each of ns0/ns1/ns2 */
#define MEZZO7_GFXBNK0_SIZE 0x0800  /* chars, 1bpp */
#define MEZZO7_GFXBNK1_SIZE 0x1800  /* tiles, 3bpp (3 planes x 0x800) */
#define MEZZO7_N_PALETTE    32

/* ── Input actions ───────────────────────────────────────────────────────── */
enum {
    MEZZO7_ACT_COIN = 0,
    MEZZO7_ACT_BIG,
    MEZZO7_ACT_SMALL,
    MEZZO7_ACT_PAYOUT,
    MEZZO7_ACT_TAKE,
    MEZZO7_ACT_DEAL,
    MEZZO7_ACT_STAND,
    MEZZO7_ACT_SERVICE,
    MEZZO7_ACT_DUP,
    MEZZO7_ACT_BET,
    MEZZO7_NUM_ACTIONS,
};

typedef struct Mezzo7State {
    Mos6502 cpu;
    const MosConfig *cfg;

    uint8_t nvram[MEZZO7_NVRAM_SIZE]; /* $0000-$07FF */
    uint8_t *prg;                     /* 0x4000, mapped at $C000-$FFFF */
    uint8_t *gfxbnk0;                 /* 0x0800, chars, 1bpp */
    uint8_t *gfxbnk1;                 /* 0x1800, tiles, 3bpp */
    uint32_t palette[MEZZO7_N_PALETTE]; /* 0x00RRGGBB, hand-coded (no PROM) */

    uint8_t videoram[MEZZO7_VRAM_SIZE];
    uint8_t colorram[MEZZO7_VRAM_SIZE];

    uint8_t crtc_addr;
    uint8_t crtc_regs[32];

    uint8_t input_selector; /* low nibble of last $3000 write */
    uint8_t dsw0;           /* raw DIP byte, selector 0 read */

    int coin_latch; /* coin-in pulse, frames remaining */

    uint32_t pixels_argb[MEZZO7_FB_WIDTH * MEZZO7_FB_HEIGHT];
    uint8_t  pixels_idx[MEZZO7_FB_WIDTH * MEZZO7_FB_HEIGHT]; /* for VNC */

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
} Mezzo7State;

Mezzo7State *mezzo7_create (const MosConfig *cfg);
void         mezzo7_run    (Mezzo7State *s, const MosConfig *cfg);
void         mezzo7_destroy(Mezzo7State *s);
