#pragma once
/*
 * TIA - Television Interface Adaptor (Atari 2600 video, sound and inputs).
 *
 * Emulated one color clock at a time: 228 clocks per scanline, 68 of them
 * horizontal blank, 3 per CPU cycle.  The machine runs the TIA up to the
 * CPU's current cycle before every bus access, so register writes land on
 * the exact color clock they do on hardware.
 *
 * Objects (two players, two missiles, the ball) are modelled the way the
 * chip builds them: each has a 160-state position counter clocked once per
 * visible color clock, plus the extra clocks HMOVE sends during horizontal
 * blank.  A start decode on the counter (and, for players and missiles, the
 * NUSIZ copy decodes 16/32/64 clocks later) begins drawing after a fixed
 * delay.  That reproduces the documented behaviour: RESPx in the visible
 * area places a player 5 pixels on (missiles and ball 4), RESPx during
 * HBLANK puts it at pixel 3 (2), the main copy isn't drawn on the line it
 * was reset on, HMOVE moves objects by its value with the 8-clock HBLANK
 * extension ("comb"), and HMOVE late in a line moves them 8 further.
 * Register writes take effect after the chip's per-register delays.
 *
 * Frames are delimited by VSYNC (or forced after TIA_MAX_LINES lines when a
 * program never sends one).  Each frame is stored as COLUxx values per
 * scanline; the machine picks the visible window and palette.
 */
#include "tia_sound.h"
#include <stdbool.h>
#include <stdint.h>

#define TIA_LINE_CLOCKS 228
#define TIA_HBLANK      68
#define TIA_WIDTH       160
#define TIA_MAX_LINES   320

typedef struct {
    uint8_t counter;     /* position counter, 0-159 */
    bool    rendering;   /* a copy is being drawn */
    int8_t  render;      /* render counter: negative during the start delay */
    uint8_t sample;      /* players: graphics bit being shown, 0-7 */
    uint8_t copy;        /* which copy is drawing (1 = main) */
    uint8_t decode;      /* NUSIZ decode table */
    uint8_t width;       /* missiles/ball: pixels; players: clocks per bit (divider) */
    uint8_t eff_width;   /* missile/ball width in "starfield" HMOVE mode */
    uint8_t divider_pending;     /* players: NUSIZ size change in progress */
    int8_t  divider_change;      /* players: clocks until it takes effect, -1 = none */
    uint8_t trip;        /* players: render value of the first pixel (0, or 1 when wide) */
    bool    on;          /* output this color clock */
    bool    moving;      /* receiving HMOVE clocks */
    uint8_t hm_clocks;   /* HMOVE clocks this object takes: (HMxx >> 4) ^ 8 */
    uint8_t last_move;   /* ball: counter at the last HMOVE clock */
} TiaObject;

typedef struct {
    uint64_t when;       /* color clock the write takes effect on */
    uint8_t  reg, val;
} TiaDelayedWrite;

#define TIA_DELAY_QUEUE 16

typedef struct Tia {
    uint64_t clock;      /* color clocks run since power-on */
    int      hctr;       /* color clock within the line, 0-227 */
    int      scanline;   /* line within the current frame */
    bool     hblank;
    bool     ext_hblank; /* HMOVE extended this line's HBLANK to 76 clocks */

    /* Registers */
    uint8_t  vsync, vblank;
    uint8_t  nusiz[2], colup[2], colupf, colubk, ctrlpf, pf[3];
    bool     refp[2];
    uint8_t  grp_new[2], grp_old[2];
    bool     enam[2], enabl_new, enabl_old;
    bool     vdelp[2], vdelbl, resmp[2];

    TiaObject p[2], m[2], bl;
    bool     pf_bit;     /* playfield bit latched for the current 4-clock column */
    bool     pf_reflect; /* CTRLPF reflect, latched at the start of each half */

    bool     movement;   /* HMOVE in progress */
    uint8_t  movement_clock;

    uint8_t  cx[8];      /* collision latches as read: CXM0P..CXPPMM, bits 7/6 */

    /* Inputs */
    bool     fire[2];        /* INPT4/INPT5 buttons, true = pressed */
    bool     latch_inputs;   /* VBLANK bit 6 */
    bool     latch[2];       /* latched button state, false once pressed */
    uint64_t pot_start;      /* clock the paddle capacitors were released */
    uint32_t pot_charge[4];  /* clocks for INPT0-3 to charge; UINT32_MAX = never */

    TiaDelayedWrite dq[TIA_DELAY_QUEUE];
    int      dq_n;

    /* Frame output: COLUxx per pixel (bit 0 clear), 0 = black. */
    uint8_t  fb[2][TIA_MAX_LINES][TIA_WIDTH];
    int      draw;            /* fb being drawn; the other holds the last frame */
    bool     frame_ready;     /* a frame finished since the machine last looked */
    uint64_t frame_count;
    int      frame_lines;     /* lines in the last frame */
    int      vsync_end_line;  /* line VSYNC ended on, current frame */
    int      first_lit, last_lit;            /* lines with VBLANK off, current frame */
    int      done_first_lit, done_last_lit;  /* same, last finished frame */
    bool     line_lit;

    TiaSound sound;
} Tia;

void    tia_init(Tia *t);
void    tia_reset(Tia *t);    /* registers and counters; keeps the sound device */
/* Run until clock reaches target. */
void    tia_run(Tia *t, uint64_t target);
/* Returns true for WSYNC: the CPU must halt until the line ends. */
bool    tia_write(Tia *t, uint16_t addr, uint8_t val);
/* bus: the last value on the data bus, which supplies the undriven bits. */
uint8_t tia_read(Tia *t, uint16_t addr, uint8_t bus);
/* Pure read for the debugger (no side effects). */
uint8_t tia_peek(const Tia *t, uint16_t addr);
/* The last finished frame. */
static inline const uint8_t (*tia_frame(const Tia *t))[TIA_WIDTH] {
    return (const uint8_t (*)[TIA_WIDTH])t->fb[t->draw ^ 1];
}
