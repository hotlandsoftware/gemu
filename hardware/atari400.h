#pragma once
#include "mos6502.h"
#include "mos6502cfg.h"
#include "antic.h"
#include "pokey.h"
#include "pia6821.h"
#include "gemu/monitor.h"
#include "gemu/gemu_display.h"
#include "gemu/vnc.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Atari 400 - 6502 @ 1.79 MHz, ANTIC + GTIA video, POKEY keyboard/sound,
 * 6520 PIA joystick ports, 10 KB OS ROM. Reference: MAME a400 driver,
 * Atari 400/800 OS listings (rev A/B), De Re Atari.
 *
 * Memory map ($0000-$FFFF, CPU-visible):
 *   $0000-ram_top  RAM (8-48 KB, -m; default 48 KB compatibility profile)
 *   $8000-$BFFF    cartridge ROM when inserted (-cartridge, 8 or 16 KB;
 *                  8 KB carts sit at $A000)
 *   $D000-$D0FF    GTIA registers
 *   $D200-$D2FF    POKEY registers
 *   $D300-$D3FF    PIA (joystick ports, active-low directions)
 *   $D400-$D4FF    ANTIC registers
 *   $D800-$FFFF    OS ROM (co12399b at $D800, co12499 at $E000,
 *                  co14599 at $F000)
 *   everything else reads $FF (open bus), writes ignored
 *
 * Timing: 262 scanlines x 114 CPU cycles (NTSC); VBI NMI at scanline 248;
 * WSYNC stalls the CPU to the end of the current scanline.
 *
 * Keyboard: host characters (SDL raw-key queue / VNC keysyms / monitor
 * `sendkey`) are translated to POKEY scan codes and paced a few frames
 * apart so the OS debounce logic registers each one.  Ctrl+arrows send the
 * Atari cursor-movement codes; plain arrows and Left Alt drive joystick 1.
 */

#define ATARI400_CPU_HZ_NTSC 1789790.0
#define ATARI400_CPU_HZ_PAL  1773447.0

#define ATARI400_OS_SIZE   0x2800u   /* $D800-$FFFF */
#define ATARI400_CART_MAX  0x4000u   /* 16 KB at $8000 */
#define ATARI400_RAM_MAX   0xC000u   /* 48 KB */

enum {
    A400_ACT_UP = 0,
    A400_ACT_DOWN,
    A400_ACT_LEFT,
    A400_ACT_RIGHT,
    A400_ACT_FIRE,
    A400_ACT_START,
    A400_ACT_SELECT,
    A400_ACT_OPTION,
    A400_ACT_BREAK,
    A400_NUM_ACTIONS,
};

#define A400_KEYQ_LEN 64

typedef struct Atari400State {
    Mos6502  cpu;
    Antic    antic;
    Pokey    pokey;
    Pia6821  pia;
    const MosConfig *cfg;

    uint8_t  ram[ATARI400_RAM_MAX];
    uint32_t ram_size;
    uint8_t  os_rom[ATARI400_OS_SIZE];
    uint8_t  cart[ATARI400_CART_MAX];
    uint32_t cart_size;
    uint16_t cart_base;     /* $A000 (8K) or $8000 (16K); 0 = no cart */

    uint8_t  *disk_data;    /* ATR payload (the 16-byte header is stripped) */
    size_t    disk_size;
    uint16_t  disk_sector_size;
    uint32_t  disk_sectors;

    bool     wsync;         /* WSYNC written; stall to end of scanline */

    /* Keyboard pacing: queued POKEY codes, one every few frames */
    uint8_t  keyq[A400_KEYQ_LEN];
    int      keyq_head, keyq_tail;
    int      key_hold_frames;   /* >0: key down, counting to release */
    int      key_gap_frames;    /* >0: wait before next queued key */

    uint32_t held_actions;
    uint8_t  vnc_console;   /* console keys held via VNC (bit0-2) */
    bool     prev_arrow[4]; /* ctrl+arrow edge detection */

    GemuDisplay   *display;
    GemuMonitor   *monitor;
    GemuVncServer *vnc;

    uint64_t frame;
} Atari400State;

Atari400State *atari400_create (const MosConfig *cfg);
void           atari400_run    (Atari400State *s, const MosConfig *cfg);
void           atari400_destroy(Atari400State *s);
