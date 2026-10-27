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
#include <stdio.h>

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

/* Debugger state (a2600_debug.c). */
#define A26_FULL_W (TIA_LINE_CLOCKS * 2)   /* full-frame view: every color clock */
#define A26_FULL_H TIA_MAX_LINES
#define A26_HISTORY 16

typedef struct {
    int      lines, vsync_end, first_lit, last_lit;
    uint64_t cycles;
} A26FrameStats;

typedef struct {
    int       break_line;      /* stop when the beam reaches it; -1 = off */
    uint32_t  stop_lines;      /* "scanline N": lines left before stopping */
    uint32_t  stop_frames;     /* "frame N": frames left before stopping */
    int       last_line;
    FILE     *trace;
    char      trace_path[256];
    bool      frame_watch;     /* report scanline-count changes */
    A26FrameStats hist[A26_HISTORY];
    int       hist_n, hist_pos;
    bool      autoreload;
    int64_t   cart_mtime;
    uint32_t  last_check_ms;
    uint32_t *full_fb;         /* A26_FULL_W x A26_FULL_H */
} A26Debug;

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
    uint32_t  lut[128];      /* palette in use, ARGB */

    A26Debug     dbg;

    GemuDisplay *display;
    GemuMonitor *monitor;
    HexEditor   *hex_editor;
} Atari2600State;

/* machine_atari2600.c, for the debugger */
bool a26_reload_cart(Atari2600State *s, char *err, size_t err_len);
uint8_t a26_peek(const Atari2600State *s, uint16_t addr);
const uint32_t *a26_palette(const Atari2600State *s);   /* 128 ARGB entries */

/* a2600_debug.c */
void a26_debug_init(Atari2600State *s);
void a26_debug_destroy(Atari2600State *s);
void a26_debug_bind(Atari2600State *s);
bool a26_debug_before_step(Atari2600State *s);   /* true = stop here */
bool a26_debug_after_step(Atari2600State *s);
bool a26_debug_frame_done(Atari2600State *s, uint64_t cycles);
void a26_debug_poll(Atari2600State *s);           /* once per host frame */
void a26_debug_present(Atari2600State *s);
void a26_debug_toggle_colors(void *ud);
void a26_debug_reload(void *ud);

Atari2600State *atari2600_create (const MosConfig *cfg);
void            atari2600_run    (Atari2600State *s, const MosConfig *cfg);
void            atari2600_destroy(Atari2600State *s);
