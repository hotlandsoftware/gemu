#pragma once
/*
 * TI TMS9918A / TMS9928A / TMS9128 Video Display Processor.
 *
 * The 9918A, 9928A and 9128 share their register set, VRAM interface and
 * timing (the 9928A/9128 output Y/R-Y/B-Y instead of composite; the colors
 * are the same).  NTSC timing: a 10.738635 MHz crystal, 342 pixel clocks
 * per line, 262 lines, 192 of them active.
 *
 *   Graphics I, Graphics II, Multicolor and Text modes, plus the
 *   undocumented mode-bit combinations (Graphics II's table masking with
 *   R3/R4, and the "illegal" M1+M2 / M1+M3 / M2+M3 combinations).
 *   32 sprites, 8x8 or 16x16, magnified or not, the early clock bit,
 *   4 per line with the fifth-sprite flag and number, coincidence flag.
 *   Status register, the frame flag and the INT output; the address
 *   register's first-byte quirk; the read-ahead buffer.
 *
 * Rendering is per scanline, at the end of each line: register or VRAM
 * changes in the middle of a line take effect from the next one.  CPU
 * access-slot timing is not enforced (writes always land).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TMS9918_VRAM_SIZE   0x4000
#define TMS9918_CLOCKS_LINE 342      /* pixel clocks (crystal / 2) */
#define TMS9918_LINES       262
#define TMS9918_ACTIVE_W    256
#define TMS9918_ACTIVE_H    192
/* The picture the machine shows: the active area and a border of
 * backdrop color around it. */
#define TMS9918_BORDER_X    16
#define TMS9918_BORDER_Y    16
#define TMS9918_FB_W        (TMS9918_ACTIVE_W + 2 * TMS9918_BORDER_X)
#define TMS9918_FB_H        (TMS9918_ACTIVE_H + 2 * TMS9918_BORDER_Y)

#define TMS9918_STATUS_F    0x80u    /* frame (vertical blank) */
#define TMS9918_STATUS_5S   0x40u    /* fifth sprite */
#define TMS9918_STATUS_C    0x20u    /* sprite coincidence */

typedef struct Tms9918 {
    uint8_t  vram[TMS9918_VRAM_SIZE];
    uint8_t  reg[8];
    uint8_t  status;
    uint16_t addr;          /* 14-bit VRAM address register */
    uint8_t  latch;         /* first byte of a control-port pair */
    bool     latched;
    uint8_t  read_ahead;

    int      line;          /* scanline being drawn, 0 = first active line */
    uint32_t frame;         /* frames completed */
    bool     int_out;       /* /INT asserted */

    uint32_t fb[TMS9918_FB_W * TMS9918_FB_H];   /* ARGB */
} Tms9918;

void    tms9918_reset(Tms9918 *v);   /* power-on: registers and status cleared */
uint8_t tms9918_read(Tms9918 *v, int mode);              /* mode: the MODE pin */
void    tms9918_write(Tms9918 *v, int mode, uint8_t val);
uint8_t tms9918_peek_status(const Tms9918 *v);

/* Advance one scanline (draws it if active).  Returns true when this line
 * ended a frame (the picture in fb is complete). */
bool    tms9918_scanline(Tms9918 *v);

extern const uint32_t tms9918_palette[16];

/* "info vdp" text */
void    tms9918_describe(const Tms9918 *v, char *buf, size_t len);
