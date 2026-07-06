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
 *   $4016        Joypad shift register read/write (strobe) — also where a
 *                Subor Mouse (-device subor-mouse) is read, see below
 *   $4017        Joypad 2 / EXT-adjacent read (unused here, returns 0)
 *   $4020        Keyboard byte queue (read; a real keyboard peripheral,
 *                not modelled — always reports empty, matching how even
 *                Furbtendulator's own keyboard feed is stubbed out)
 *   $4026        EXT port read/write
 *   $4031        Initial startup protection sequence (write-only, logged only
 *                upstream — genuinely has no gating effect, safe to no-op)
 *   $4032        IRQ mask/ack (write): 1=acknowledge+block that source going
 *                forward, 0=unblock. Bit7=Timer, bit6=Mouse, bit5=Keyboard
 *                (see nesdev.org/wiki/UM6576/UM6578 — IRQ). We never raise
 *                the Keyboard/Mouse bits ourselves (no source models them).
 *   $4033        IRQ status (read): which sources currently have a pending,
 *                unacknowledged request
 *   $4034        Timer control [ERS. .PPP]: E=enable, R=repeat (else clear E
 *                on underflow), S=source (0=CPU M2 cycles, 1=PPU scanlines),
 *                PPP=prescaler (tick every 2^PPP source ticks)
 *   $4035        Timer preset (write) — also resets the live count to this
 *                value immediately
 *   $4036        Timer count (read)
 *   $4040-$4047  Bankswitch registers: 4KB window N -> ROM page N*0x1000
 *   $4048-$404F  DMA controller (control/bank/src/dst/length)
 *   $5000-$7FFF  RAM (12KB, no mirroring)
 *   $8000-$FFFF  8 x 4KB banked ROM windows
 *
 * Mouse (-device subor-mouse): this is a "Subor Mouse" (nesdev.org/wiki/
 * Subor_Mouse) — a real, documented third-party NES/Famicom peripheral, NOT
 * chip-specific. It reuses the plain $4016 controller strobe/shift protocol
 * (no separate registers at all) with a wider 24-bit reply instead of 8:
 *
 *   bit23 = left button          bit19 = up            bit20 = unused (0)
 *   bit22 = right button         bit18 = down           bit15 = unused (0)
 *   bit21 = E (magnitude valid   bit17 = left (X-)       bit7 = unused (0)
 *           flag; some real       bit16 = right (X+)
 *           units keep this
 *           always set)
 *   bits14-8 = X movement magnitude, 0-127 (valid when E=1)
 *   bits6-0  = Y movement magnitude, 0-127 (valid when E=1)
 *
 * Bits shift out MSB-first (bit23 first), same direction as a standard
 * controller — by design, the first 8 bits alone already look like a
 * plausible controller reply, so code that only reads 8 bits (unaware of
 * the mouse) still gets sane input. This is genuinely unverified for THIS
 * specific chip/game (gogoconniechan.bin) — no MAME driver, no fork of
 * Nintendulator (mainline or the community VT/OneBus-focused "Furbtendulator"
 * fork, which has real working UM6578 PPU/APU/DMA emulation but leaves its
 * own keyboard *and* mouse input unimplemented) supports it. It's the most
 * concretely-documented candidate protocol available, reusing infrastructure
 * ($4016) this ROM is confirmed (via runtime tracing, not just static
 * disassembly of the banked, sometimes self-modified ROM) to actually poll
 * every frame — but whether this specific title's mouse detection actually
 * lives behind that same read is not confirmed.
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

    /* IRQ mux ($4032/4033): bit7=Timer, bit6=Mouse, bit5=Keyboard */
    uint8_t irq_mask;      /* $4032: 1=acked/blocked, 0=passthrough */
    uint8_t irq_pending;   /* $4033: raw pending flags, regardless of mask */

    /* Timer ($4034/4035/4036) */
    uint8_t  timer_ctrl;      /* raw $4034 value */
    uint8_t  timer_preset;    /* $4035 */
    uint8_t  timer_count;     /* live count, readable via $4036 */
    uint32_t timer_prescale_acc; /* source ticks accumulated toward next decrement */

    /* Subor Mouse (-device subor-mouse), read via the standard $4016 shift
     * register — see um6578.h's top-of-file comment for the protocol. */
    int      mouse_px, mouse_py; /* host pointer position as of the last strobe */

    uint8_t  prev_io;
    uint32_t iolatch;   /* wide enough for the Subor Mouse's 24-bit reply
                         * (standard controller replies only need 8 bits) */
    bool     io_mouse_active; /* true: $4016 shifts iolatch MSB-first (Subor,
                               * 24 bits); false: LSB-first (standard pad, 8 bits) */
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
