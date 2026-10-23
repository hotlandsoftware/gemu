#pragma once
#include <stdint.h>
#include <stdbool.h>

/* IBM Monochrome Display Adapter (1981): 4KB of text VRAM at physical
 * B0000 (mirrored through B7FFF), registers at 0x3B4/0x3B5 (6845 CRTC
 * index/data, also mirrored at 0x3B0-0x3B3/0x3B6-0x3B7), 0x3B8 mode control
 * and 0x3BA status.  (The printer port at 0x3BC is not modelled.)
 *
 * 80x25 text in a 9x14 cell - 720x350 - on a green-phosphor monitor, with
 * the MDA's attributes: normal, intense, underline, reverse video,
 * invisible and blink.  Character codes 0xC0-0xDF repeat their 8th column
 * into the 9th so box-drawing lines join.
 *
 * Beam timing follows the card in CPU clocks (16.257 MHz dot clock, 882
 * dots x 370 lines): ~259 clocks per scanline, ~49.8 Hz.  Status bit 0 is
 * horizontal retrace and bit 3 the video signal, both of which BIOS POST
 * checks for activity. */

#define MDA_VRAM_SIZE  0x1000u   /* 4 KiB */
#define MDA_WIDTH      720
#define MDA_HEIGHT     350
#define MDA_CLOCKS_PER_FRAME 95806u   /* CPU clocks per frame (~49.8 Hz) */

typedef struct {
    uint8_t  vram[MDA_VRAM_SIZE];
    uint8_t  mode_ctrl;    /* 0x3B8 */
    uint8_t  crtc_index;   /* 0x3B4 */
    uint8_t  crtc[18];     /* 0x3B5, Motorola 6845 R0-R17 */
    uint32_t beam;         /* CPU clocks x 16 into the current frame */
    uint32_t frame;        /* frames since reset, for blinking */
} MdaDevice;

void    mda_reset(MdaDevice *m);
uint8_t mda_io_read(MdaDevice *m, uint16_t port);
void    mda_io_write(MdaDevice *m, uint16_t port, uint8_t val);
uint8_t mda_mem_read(const MdaDevice *m, uint32_t addr);  /* addr relative to B0000 */
void    mda_mem_write(MdaDevice *m, uint32_t addr, uint8_t val);

/* Advance the beam by `clocks` CPU clocks (4.77 MHz); returns true when a
 * new frame started. */
bool    mda_tick(MdaDevice *m, uint32_t clocks);

/* Render the screen into a 720x350 ARGB framebuffer (font: 256 glyphs x 14
 * bytes, MSB = leftmost pixel). */
void    mda_render(const MdaDevice *m, uint32_t *argb, const uint8_t *font8x14);
