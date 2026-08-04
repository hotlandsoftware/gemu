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
 * UM6578/SH6578/NT6578 - "enhanced NES" clone-on-a-chip used in cheap
 * plug & play TV game cartridges/consoles (Senario Vs Maxx, dreamGEAR
 * Plug'N'Play, TimeTop, etc). Reference: MAME src/mame/nintendo/nes_sh6578.cpp.
 *
 * Single real M6502 (full decimal mode, unlike the NES's decimal-less
 * 2A03), a bespoke PPU (vga/ppu_sh6578.c - see that file for how its video
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
 *   $2040-$207F  PPU palette RAM (64 entries, direct - not in PPU space)
 *   $4000-$4013  APU registers (2A03, reused from audio/apu2a03.c)
 *   $4014        OAM DMA (write page -> 256-byte sprite RAM copy)
 *   $4015        APU status
 *   $4016        Joypad shift register read/write (strobe). A legacy
 *                24-bit mouse reply is also implemented for Subor-style
 *                devices, but Connie-chan's PS/2 mouse movement does not use
 *                this path once the PS/2 stream is enabled.
 *   $4017        Joypad 2 / EXT-adjacent read (unused here, returns 0)
 *   $4020        Keyboard byte queue. Connie-chan also drains PS/2 mouse
 *                command replies and movement packets here.
 *   $4021        PS/2-style mouse command byte for Connie-chan.
 *   $4022        Keyboard/mouse queue control/acknowledge.
 *   $4026        EXT/status port; idle high for Connie-chan.
 *   $4031        Initial startup protection sequence (write-only, logged only
 *                upstream - genuinely has no gating effect, safe to no-op)
 *   $4032        IRQ mask/ack (write): 1=acknowledge+block that source going
 *                forward, 0=unblock. Bit7=Timer, bit6=Mouse, bit5=Keyboard
 *                (see nesdev.org/wiki/UM6576/UM6578 - IRQ). Mouse packets
 *                raise the Keyboard bit (see above); we never raise the
 *                Mouse bit ourselves (no confirmed source models it).
 *   $4033        IRQ status (read): which sources currently have a pending,
 *                unacknowledged request
 *   $4034        Timer control [ERS. .PPP]: E=enable, R=repeat (else clear E
 *                on underflow), S=source (0=CPU M2 cycles, 1=PPU scanlines),
 *                PPP=prescaler (tick every 2^PPP source ticks)
 *   $4035        Timer preset (write) - also resets the live count to this
 *                value immediately
 *   $4036        Timer count (read)
 *   $4040-$4047  Bankswitch registers: 4KB window N -> ROM page N*0x1000
 *   $4048-$404F  DMA controller (control/bank/src/dst/length)
 *   $5000-$7FFF  RAM (12KB, no mirroring)
 *   $8000-$FFFF  8 x 4KB banked ROM windows
 *
 * Mouse (-device mouse, legacy alias -device subor-mouse): Connie-chan uses
 * a removable two-button PS/2 ball mouse. The ROM sends PS/2-style commands
 * through $4021, then drains command replies and 3-byte movement packets
 * through the $4020 byte queue on the Keyboard IRQ bit.
 *
 * $4016 can also return a 24-bit Subor-style mouse word when mouse mode is
 * strobed. Keep this separate from the Connie-chan PS/2 stream.
 *
 * $4016 reply when mouse mode is active - 24-bit word, MSB-first (bit23
 * first, same shift direction as a standard controller, so 8-bit-only
 * readers still see a plausible controller reply):
 *
 *   bit23 = left button          bit19 = up            bit20 = unused (0)
 *   bit22 = right button         bit18 = down           bit15 = unused (0)
 *   bit21 = E (magnitude valid   bit17 = left (X-)       bit7 = unused (0)
 *           flag; some real       bit16 = right (X+)
 *           units keep this
 *           always set)
 *   bits14-8 = X movement magnitude, 0-127 (valid when E=1)
 *   bits6-0  = Y movement magnitude, 0-127 (valid when E=1)
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

    /* UM6578 mouse (-device mouse), a PS/2-style byte queue drained via
     * $4020 (see um6578.h's top-of-file comment for the protocol). Sampled
     * once per frame, independent of $4016 strobing. */
    int      mouse_px, mouse_py; /* host pointer position as of the last PS/2 sample */
    int      mouse_legacy_px, mouse_legacy_py; /* last $4016 legacy sample */
    bool     mouse_have_pos;
    bool     mouse_legacy_have_pos;
    bool     mouse_synth_active;
    int      mouse_synth_x, mouse_synth_y;
    bool     mouse_synth_left, mouse_synth_right;
    bool     mouse_prev_left, mouse_prev_right;
    bool     mouse_stream_enabled;
    uint8_t  mouse_param_cmd;
    uint8_t  mouse_cmd_resp[4];
    uint8_t  mouse_cmd_resp_len, mouse_cmd_resp_pos;
    uint8_t  mouse_queue[16];
    uint8_t  mouse_qhead, mouse_qtail;

    uint8_t  prev_io;
    uint32_t iolatch;   /* wide enough for the mouse's 24-bit reply
                         * (standard controller replies only need 8 bits) */
    bool     io_mouse_active; /* true: $4016 shifts iolatch MSB-first (Subor,
                               * 24 bits); false: LSB-first (standard pad, 8 bits) */
    uint32_t held_actions;
    bool     trace_io;
    bool     trace_4016;
    bool     trace_timer;
    bool     trace_mouse_state;
    uint32_t input_inject_mask;
    int      input_inject_frames;

    uint64_t ppu_synced_cpu_cycle;

    GemuDisplay   *display;
    GemuMonitor   *monitor;
    GemuVncServer *vnc;

    uint64_t frame;
} Um6578State;

Um6578State *um6578_create (const MosConfig *cfg);
void         um6578_run    (Um6578State *s, const MosConfig *cfg);
void         um6578_destroy(Um6578State *s);
