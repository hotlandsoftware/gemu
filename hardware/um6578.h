#pragma once
#include "mos6502.h"
#include "mos6502cfg.h"
#include "ppu_sh6578.h"
#include "apu2a03.h"
#include "gemu/monitor.h"
#include "gemu/gemu_display.h"
#include "gemu/vnc.h"
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * UM6578/SH6578/NT6578 — "enhanced NES" clone-on-a-chip used in cheap
 * plug & play TV game cartridges/consoles (Senario Vs Maxx, dreamGEAR
 * Plug'N'Play, TimeTop, etc). Reference: MAME src/mame/nintendo/nes_sh6578.cpp.
 *
 * Single real M6502 (full decimal mode, unlike the NES's decimal-less
 * 2A03), a bespoke PPU (vga/ppu_sh6578.c — see that file for how its video
 * differs from a plain NES PPU), the NES's own 2A03 APU reused verbatim for
 * sound, an 8-bank (4 KB each) ROM windowing scheme addressing up to 1 MB
 * (2 MB on some titles, selected by a coarse half-select bank not modelled
 * here), and a simple byte-at-a-time DMA controller that can move data
 * between ROM/CPU-space and CPU-RAM/PPU-VRAM.
 *
 * Memory map ($0000-$FFFF, CPU-visible):
 *   $0000-$1FFF  RAM (8KB, no mirroring)
 *   $2000-$2007  PPU registers (standard NES-like PPUCTRL..PPUDATA)
 *   $2008        PPU extended register (nametable/sprite-page/4bpp select)
 *   $2040-$207F  PPU palette RAM (64 entries, direct — not in PPU space)
 *   $4000-$4013  APU registers (2A03, reused from audio/apu2a03.c)
 *   $4014        OAM DMA (write page -> 256-byte sprite RAM copy)
 *   $4015        APU status
 *   $4016        Joypad shift register read/write (strobe)
 *   $4017        Joypad 2 / EXT-adjacent read (unused here, returns 0)
 *   $4020        Timing setting control (write-only, stubbed)
 *   $4026        EXT port read/write
 *   $4031        Initial startup protection sequence (write-only, logged only
 *                upstream — genuinely has no gating effect, safe to no-op)
 *   $4032        IRQ mask (bit7 clear enables the scanline timer IRQ)
 *   $4033        IRQ status (read-only, stubbed)
 *   $4034        Timer config (arms/disarms the scanline-count IRQ timer)
 *   $4035        Timer reload value (in scanlines)
 *   $4040-$4047  Bankswitch registers: 4KB window N -> ROM page N*0x1000
 *   $4048-$404F  DMA controller (control/bank/src/dst/length)
 *   $5000-$7FFF  RAM (12KB, no mirroring)
 *   $8000-$FFFF  8 x 4KB banked ROM windows
 */

#define UM6578_CPU_HZ      1789772.667  /* NTSC_APU_CLOCK: 21.477272MHz XTAL / 12 */
#define UM6578_REFRESH_HZ  60.0988
#define UM6578_LINES_TOTAL 262
#define UM6578_VBLANK_LINE 241

#define UM6578_ROM_MAX  0x200000u  /* 2MB, largest known cart on this hardware */

enum {
    UM6578_ACT_A = 0,
    UM6578_ACT_B,
    UM6578_ACT_SELECT,
    UM6578_ACT_START,
    UM6578_ACT_UP,
    UM6578_ACT_DOWN,
    UM6578_ACT_LEFT,
    UM6578_ACT_RIGHT,
    UM6578_NUM_ACTIONS,
};

typedef struct Um6578State {
    Mos6502   cpu;
    Sh6578Ppu ppu;
    Apu2a03   apu;
    const MosConfig *cfg;

    uint8_t ram_lo[0x2000];   /* $0000-$1FFF */
    uint8_t ram_hi[0x3000];   /* $5000-$7FFF */

    uint8_t *rom;             /* full cartridge ROM image, up to UM6578_ROM_MAX */
    uint32_t rom_size;

    uint8_t bankswitch[8];    /* $4040-4047: 4KB window -> ROM page */

    uint8_t dma_control;
    uint8_t dma_bank;
    uint8_t dma_source[2];
    uint8_t dma_dest[2];
    uint8_t dma_length[2];

    uint8_t irqmask;
    int     timer_scanlines_left;
    bool    timer_armed;

    uint8_t prev_io;
    uint8_t iolatch;
    uint32_t held_actions;

    uint64_t ppu_synced_cpu_cycle;

    GemuDisplay   *display;
    GemuMonitor   *monitor;
    GemuVncServer *vnc;

    uint64_t frame;
} Um6578State;

Um6578State *um6578_create (const MosConfig *cfg);
void         um6578_run    (Um6578State *s, const MosConfig *cfg);
void         um6578_destroy(Um6578State *s);
