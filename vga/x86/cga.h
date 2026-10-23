#pragma once
#include <stdint.h>
#include <stdbool.h>

/* IBM Color Graphics Adapter: 16KB VRAM at physical B8000-BBFFF, registers
 * at 0x3D4/0x3D5 (6845 CRTC index/data), 0x3D8 mode control, 0x3D9 color
 * select, 0x3DA status.
 *
 * Beam timing follows the real card in CPU clocks (its 14.318 MHz dot
 * clock is 3x the PC's 4.77 MHz): 304 clocks per scanline, 262 lines per
 * frame (~59.9 Hz), 200 visible lines, vertical sync on lines 224-239.
 * The status register reports display-enable and vertical retrace from
 * that position, which is what BIOS and games poll.
 *
 * Rendering covers 40/80-column text (with blink and the CRTC-shaped
 * cursor), 320x200 4-color and 640x200 2-color graphics. */

#define CGA_VRAM_SIZE 0x4000u /* 16 KiB */

#define CGA_CLOCKS_PER_LINE  304u
#define CGA_LINES_PER_FRAME  262u
#define CGA_CLOCKS_PER_FRAME (CGA_CLOCKS_PER_LINE * CGA_LINES_PER_FRAME)

typedef struct {
    uint8_t  vram[CGA_VRAM_SIZE];
    uint8_t  mode_ctrl;    /* 0x3D8 */
    uint8_t  color_select; /* 0x3D9 */
    uint8_t  crtc_index;   /* 0x3D4 */
    uint8_t  crtc[18];     /* 0x3D5, Motorola 6845 registers R0-R17 */
    uint32_t beam;         /* CPU clocks into the current frame */
    uint32_t frame;        /* frames since reset, for blinking */
} CgaDevice;

void    cga_reset(CgaDevice *c);
uint8_t cga_io_read(CgaDevice *c, uint16_t port);
void    cga_io_write(CgaDevice *c, uint16_t port, uint8_t val);
uint8_t cga_mem_read(const CgaDevice *c, uint32_t addr);  /* addr relative to B8000 */
void    cga_mem_write(CgaDevice *c, uint32_t addr, uint8_t val);

/* Advance the beam by `clocks` CPU clocks; returns true when a frame
 * (vertical retrace) started during the advance. */
bool    cga_tick(CgaDevice *c, uint32_t clocks);

/* Render the current screen into a 640-wide ARGB framebuffer: 640x400
 * doubles every scanline (the CGA's on-screen proportions), 640x200 is the
 * raw resolution.  40-column text and 320-wide graphics are doubled
 * horizontally.  font8x8 is 256 glyphs x 8 bytes, MSB = leftmost pixel. */
void    cga_render(const CgaDevice *c, uint32_t *argb, int fb_w, int fb_h, const uint8_t *font8x8);
