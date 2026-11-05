#pragma once
/*
 * Hercules Graphics Card (1982): the MDA's text mode, plus 720x348
 * monochrome graphics, in 64 KiB of display memory.
 *
 * Ports are the MDA's (6845 CRTC at 3B4h/3B5h, mirrored over 3B0h-3B7h,
 * mode control at 3B8h, status at 3BAh), plus the configuration switch:
 *
 *   3B8h  mode control: bit 1 graphics, 3 video enable, 5 blink, 7 page
 *   3BAh  status: bit 0 horizontal retrace, 3 video, 7 vertical retrace
 *         (low during it).  The MDA's bit 7 never changes, which is how
 *         software tells the two apart.
 *   3BFh  configuration (write only): bit 0 allows graphics mode and the
 *         page bit, bit 1 maps the second page at B8000h.  Both are clear
 *         at power-on, when the card is an MDA with 32 KiB.
 *
 * Memory is two 32 KiB pages, B0000h and B8000h; the second is only on
 * the bus with configuration bit 1, so a colour card can share B8000h.
 * Graphics mode interleaves each page in four 8 KiB banks: scan line y
 * of a character row is in bank y & 3, at 90 bytes a line.
 *
 * Timing follows the CRTC's registers on the card's 16.257 MHz dot clock,
 * with 9 dots a character in text mode and 16 in graphics mode, so the
 * line and frame rates change with the mode as on the card (50 Hz text,
 * ~51 Hz in the usual graphics mode).
 */
#include "mda.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HERCULES_VRAM_SIZE 0x10000u   /* 64 KiB */
#define HERCULES_WIDTH     720        /* the picture: text's 720x350; */
#define HERCULES_HEIGHT    350        /* graphics' 720x348 at its top */

typedef struct {
    uint8_t     vram[HERCULES_VRAM_SIZE];
    MdaPhosphor phosphor;   /* kept across hercules_reset() - it's the monitor */
    uint8_t     mode_ctrl;  /* 3B8h as written */
    uint8_t     config;     /* 3BFh */
    uint8_t     crtc_index;
    uint8_t     crtc[18];
    uint64_t    dots;       /* dot clocks into the current frame */
    uint64_t    dot_frac;   /* CPU clocks x dot clock not yet a whole dot */
    uint32_t    frame;      /* frames since reset, for blinking */
} HerculesDevice;

void    hercules_reset(HerculesDevice *h);
/* "phosphor=green|amber|white" (from -vga hercules,...). */
bool    hercules_set_options(HerculesDevice *h, const char *opts, char *err, size_t err_len);
uint8_t hercules_io_read(HerculesDevice *h, uint16_t port);     /* 3B0h-3BFh */
void    hercules_io_write(HerculesDevice *h, uint16_t port, uint8_t val);
/* Display memory, addr relative to B0000h.  The second page (8000h-FFFFh)
 * answers only when configured; hercules_mem_decoded() says. */
bool    hercules_mem_decoded(const HerculesDevice *h, uint32_t addr);
uint8_t hercules_mem_read(const HerculesDevice *h, uint32_t addr);
void    hercules_mem_write(HerculesDevice *h, uint32_t addr, uint8_t val);

/* Advance by `clocks` CPU clocks of a cpu_hz CPU; true when a frame started. */
bool     hercules_tick(HerculesDevice *h, uint32_t clocks, uint32_t cpu_hz);
/* CPU clocks per frame in the current mode, for pacing. */
uint32_t hercules_clocks_per_frame(const HerculesDevice *h, uint32_t cpu_hz);

/* Render into a 720x350 ARGB framebuffer (font: the MDA's 9x14 ROM). */
void    hercules_render(const HerculesDevice *h, uint32_t *argb, const uint8_t *font8x14);
