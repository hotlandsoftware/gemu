#pragma once
#include <stdint.h>
#include <stdbool.h>

/* IBM Color Graphics Adapter - text mode only this pass (320x200x4 and
 * 640x200x2 graphics modes are a later TODO, see cga_render()). 16KB VRAM
 * at physical B8000-BBFFF, register ports 0x3D0-0x3DF (only the ones real
 * software actually touches are implemented: 0x3D4/0x3D5 CRTC index/data,
 * 0x3D8 mode control, 0x3D9 color select, 0x3DA status). */

#define CGA_VRAM_SIZE 0x4000u /* 16 KiB */

typedef struct {
    uint8_t  vram[CGA_VRAM_SIZE];
    uint8_t  mode_ctrl;    /* 0x3D8 */
    uint8_t  color_select; /* 0x3D9 */
    uint8_t  crtc_index;   /* 0x3D4 */
    uint8_t  crtc[18];     /* 0x3D5, Motorola 6845-style; only start-address
                             * (R12/R13) and cursor position (R14/R15) are
                             * interpreted for rendering - the rest are
                             * stored but not acted on. */
    uint32_t retrace_counter; /* advanced by cga_tick() so status-register
                                * polling loops (and cell blink) progress */
} CgaDevice;

void    cga_reset(CgaDevice *c);
uint8_t cga_io_read(CgaDevice *c, uint16_t port);
void    cga_io_write(CgaDevice *c, uint16_t port, uint8_t val);
uint8_t cga_mem_read(const CgaDevice *c, uint32_t addr);  /* addr relative to B8000 */
void    cga_mem_write(CgaDevice *c, uint32_t addr, uint8_t val);
void    cga_tick(CgaDevice *c, uint32_t amount);

/* Renders text mode (80x25 or 40x25, selected by mode_ctrl bit 0) into a
 * fixed 640x200 logical framebuffer - 40-column mode double-widens each
 * cell rather than shrinking the picture, so the machine's display size
 * never has to change with the video mode. Graphics modes (mode_ctrl bit 1
 * set) currently render as blank/black. font8x8 is 256 glyphs x 8 bytes,
 * one byte per scanline, MSB = leftmost pixel (vgafont16.c's convention). */
void cga_render(const CgaDevice *c, uint32_t *argb, int fb_w, int fb_h, const uint8_t *font8x8);
