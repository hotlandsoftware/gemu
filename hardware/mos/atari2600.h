#pragma once
#include "mos6502.h"
#include "mos6502cfg.h"
#include "tia.h"
#include "m6532.h"
#include "a2600_cart.h"
#include "hex_editor.h"
#include "gemu/monitor.h"
#include "gemu/gemu_display.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Atari 2600 (Video Computer System) - 6507 @ 1.19 MHz, TIA video/sound/
 * inputs, 6532 RIOT (128 bytes RAM, joystick and console switch ports,
 * timer), cartridge ROM.  No BIOS: the cartridge is all the software.
 *
 * The 6507 has 13 address pins, so the map repeats every 8 KB:
 *   A12=1               cartridge ($1000-$1FFF, bank-switched)
 *   A12=0, A7=0         TIA (writes: A5-A0, reads: A3-A0)
 *   A12=0, A7=1, A9=0   RIOT RAM ($80-$FF, also the stack at $180-$1FF)
 *   A12=0, A7=1, A9=1   RIOT ports and timer ($280-$297)
 *
 * The TIA is run to the CPU's cycle before every bus access; WSYNC holds
 * the CPU (RDY) until the end of the scanline.  Frames are what the
 * program makes them (VSYNC to VSYNC); the window shows 240 lines centred
 * on the part the program un-blanks, each TIA pixel two pixels wide.
 *
 * Controllers (-device): cx40 (default) / cx10 joysticks and cx30 paddles,
 * one per jack.  Console switches: Reset, Select, Color/B&W and the two
 * difficulty switches (keys, `sendkey reset`, `dipswitch`).
 */

#define A2600_COLOR_HZ_NTSC 3579545.0
#define A2600_COLOR_HZ_PAL  3546894.0
#define A2600_FB_W (TIA_WIDTH * 2)
#define A2600_FB_H 240

enum {
    A26_ACT_P1_UP = 0, A26_ACT_P1_DOWN, A26_ACT_P1_LEFT, A26_ACT_P1_RIGHT, A26_ACT_P1_FIRE,
    A26_ACT_P2_UP,     A26_ACT_P2_DOWN, A26_ACT_P2_LEFT, A26_ACT_P2_RIGHT, A26_ACT_P2_FIRE,
    A26_ACT_RESET, A26_ACT_SELECT, A26_ACT_COLOR, A26_ACT_LEFT_DIFF, A26_ACT_RIGHT_DIFF,
    A26_NUM_ACTIONS
};

typedef struct Atari2600State {
    Mos6502   cpu;
    Tia       tia;
    M6532     riot;
    A2600Cart cart;
    const MosConfig *cfg;

    uint8_t   bus;          /* last value on the data bus */
    uint16_t  last_addr;
    bool      wsync;        /* WSYNC written: hold the CPU to the end of the line */

    /* Console switches */
    bool      color;        /* Color (true) / B&W */
    bool      diff_a[2];    /* left/right difficulty: A (true) or B */

    uint32_t  held;          /* display actions held this frame */
    uint32_t  prev_held;
    uint32_t  inject_mask;   /* sendkey: actions to hold... */
    int       inject_frames; /* ...for this many frames */
    double    paddle[4];     /* 0 = fully clockwise ... 1 = counter-clockwise */

    double    color_hz;      /* NTSC or PAL color clock */
    bool      pal;           /* PAL palette in use (machine, or detected) */
    int       window_start;  /* first frame line shown */
    uint64_t  frame_start_cycle;

    char      cart_path[512];
    uint32_t  fb[A2600_FB_W * A2600_FB_H];

    GemuDisplay *display;
    GemuMonitor *monitor;
    HexEditor   *hex_editor;
} Atari2600State;

Atari2600State *atari2600_create (const MosConfig *cfg);
void            atari2600_run    (Atari2600State *s, const MosConfig *cfg);
void            atari2600_destroy(Atari2600State *s);
