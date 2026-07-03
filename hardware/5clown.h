#pragma once
#include "mos6502.h"
#include "mos6502cfg.h"
#include "pia6821.h"
#include "gemu/monitor.h"
#include "gemu/gemu_display.h"
#include "gemu/vnc.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * IGS "Five Clown" (1993) — dual 6502 video poker arcade board.
 * Hardware reference: MAME src/mame/igs/5clown.cpp.
 *
 * Main CPU memory map:
 *   $0000-$07FF  NVRAM (battery-backed)
 *   $0800        CRTC address latch (write)
 *   $0801        CRTC register (r/w)
 *   $0844-$0847  PIA0 (multiplexed inputs / coin counters)
 *   $0848-$084B  PIA1 (SW4 / audio-CPU NMI trigger / input mux select)
 *   $1000-$13FF  video RAM (32x32 tile codes)
 *   $1800-$1BFF  color RAM (32x32 tile attributes)
 *   $2000-$7FFF  ROM (decrypted program, low 24KB)
 *   $C048        unknown sound-related write (logged, no effect)
 *   $C400        SW1 DIP bank read
 *   $CC00        SW2 DIP bank read
 *   $D400        SW3 DIP bank read
 *   $D800        main -> sound latch write
 *   $E000-$FFFF  ROM (decrypted program, top 8KB incl. vectors)
 *
 * Audio CPU memory map:
 *   $0000-$07FF  RAM
 *   $0800        AY8910 addr/data latch (mode selected by $0A02)
 *   $0A02        AY8910 latch mode (0xC0 = address, 0x00 = data)
 *   $0C04        OKI6295 write
 *   $0C06        OKI6295 read (busy status)
 *   $0E06        read main CPU's last $D800 write
 *   $E000-$FFFF  ROM (unencrypted)
 *
 * Both CPUs run at 10MHz XTAL / 8 = 1.25MHz. The CRTC runs at 10MHz/2 with
 * a 320x312 total raster, giving a 50.08Hz refresh -> 24960 CPU cycles per
 * frame for both CPUs.
 */

#define FIVECLOWN_CPU_HZ        1250000u
#define FIVECLOWN_CYCLES_PER_FRAME 24960u /* 1,250,000 / 50.08 */

#define FIVECLOWN_FB_WIDTH  256
#define FIVECLOWN_FB_HEIGHT 256
#define FIVECLOWN_TILE_COLS 32
#define FIVECLOWN_TILE_ROWS 32

#define FIVECLOWN_VRAM_SIZE  0x400
#define FIVECLOWN_NVRAM_SIZE 0x800
#define FIVECLOWN_GFXBANKS_SIZE 0x8000
#define FIVECLOWN_OKI_SIZE      0x10000
#define FIVECLOWN_PROM_SIZE     0x100

/* ── Input actions (multiplexed 4x5 button matrix + coin/key-in) ─────────── */
enum {
    FIVECLOWN_ACT_BET = 0,
    FIVECLOWN_ACT_RECORD,
    FIVECLOWN_ACT_DUP,
    FIVECLOWN_ACT_START,
    FIVECLOWN_ACT_KEYOUT,
    FIVECLOWN_ACT_PAYOUT,
    FIVECLOWN_ACT_COLLECT,
    FIVECLOWN_ACT_BIG,
    FIVECLOWN_ACT_SMALL,
    FIVECLOWN_ACT_HOLD1,
    FIVECLOWN_ACT_HOLD2,
    FIVECLOWN_ACT_HOLD3,
    FIVECLOWN_ACT_HOLD4,
    FIVECLOWN_ACT_HOLD5,
    FIVECLOWN_ACT_SETTING,
    FIVECLOWN_ACT_COIN,
    FIVECLOWN_ACT_KEYIN,
    FIVECLOWN_NUM_ACTIONS,
};

typedef struct FiveClownState {
    Mos6502 cpu;       /* main CPU */
    Mos6502 audiocpu;  /* sound CPU */

    const MosConfig *cfg;

    uint8_t nvram[FIVECLOWN_NVRAM_SIZE];   /* $0000-$07FF, battery-backed */
    uint8_t audio_ram[0x800];              /* audio CPU $0000-$07FF */

    uint8_t *prg;       /* main CPU program, decrypted: 0x2000 (2000-7FFF) + 0x2000 (E000-FFFF) laid out contiguously as [0..0x6000)=2000-7FFF, [0x6000..0x8000)=E000-FFFF */
    uint8_t *audio_prg; /* audio CPU program, 0x2000 bytes at E000-FFFF, unencrypted */
    uint8_t *gfxbanks;  /* decrypted tile graphics, 0x8000 */
    uint8_t *oki_rom;   /* decrypted OKI6295 samples, 0x10000 */
    uint32_t palette[256]; /* decoded from the 74S287 PROM, 0x00RRGGBB */

    uint8_t videoram[FIVECLOWN_VRAM_SIZE]; /* $1000-$13FF */
    uint8_t colorram[FIVECLOWN_VRAM_SIZE]; /* $1800-$1BFF */

    /* CRTC: register-file only, no scanline timing modeled (see machine_5clown.c) */
    uint8_t crtc_addr;
    uint8_t crtc_regs[32];

    Pia6821 pia0, pia1;

    uint8_t mux_data;         /* PIA1 port B: input mux select (inverted) */
    uint8_t main_latch_d800;  /* main -> sound command latch */
    uint8_t snd_latch_0800;
    uint8_t snd_latch_0a02;
    uint8_t ay8910_addr;

    uint8_t dip1, dip2, dip3, dip4; /* SW1/SW2/SW3 fixed-address reads; SW4 via PIA1 port A */

    bool audio_nmi_line; /* tracked level, to edge-detect for audiocpu.nmi */

    /* Coin-in pulse (frames remaining), like VS. System's coin latch */
    int coin_latch;

    uint32_t held_actions; /* cached each frame from gemu_display_poll() */

    uint32_t pixels_argb[FIVECLOWN_FB_WIDTH * FIVECLOWN_FB_HEIGHT];
    uint8_t  pixels_idx[FIVECLOWN_FB_WIDTH * FIVECLOWN_FB_HEIGHT]; /* palette index, for VNC */

    char     sav_path[512];

    GemuDisplay   *display;
    GemuMonitor   *monitor;
    GemuVncServer *vnc;

    uint64_t frame;
} FiveClownState;

FiveClownState *fiveclown_create (const MosConfig *cfg);
void            fiveclown_run    (FiveClownState *s, const MosConfig *cfg);
void            fiveclown_destroy(FiveClownState *s);
