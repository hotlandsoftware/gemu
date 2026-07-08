#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * ANTIC + GTIA — Atari 400/800 display pair, modelled as one unit.
 *
 * ANTIC is a display-list processor: a program in guest RAM (the display
 * list) tells it, row by row, which of 14 character/map modes to fetch and
 * where the screen data lives.  GTIA turns ANTIC's playfield stream into
 * colors (128-color hue/luma palette) and adds player/missile overlays.
 *
 * Scope: frame-based renderer (one full walk of the display list per
 * frame), all 14 playfield modes, narrow/standard/wide playfield widths,
 * character inversion/blank via CHACTL, and the VBI/DLI status plumbing
 * (NMIEN/NMIST/VCOUNT).  Not modelled: player/missile graphics, GTIA
 * modes (PRIOR>>6), horizontal/vertical fine scrolling, mid-frame register
 * changes (DLIs render with frame-start colors), light pen.
 *
 * References: ANTIC (C012296) and GTIA (C014805) data sheets; De Re Atari.
 */

#define ANTIC_FB_W 320
#define ANTIC_FB_H 240

#define ANTIC_LINES_NTSC        262
#define ANTIC_LINES_PAL         312
#define ANTIC_CYCLES_PER_LINE   114
#define ANTIC_FIRST_VISIBLE     8     /* framebuffer y0 == scanline 8 */
#define ANTIC_VBI_LINE          248   /* vertical blank NMI scanline */

/* NMIEN / NMIST bits */
#define ANTIC_NMI_DLI 0x80
#define ANTIC_NMI_VBI 0x40
#define ANTIC_NMI_RST 0x20   /* Reset key on the 400/800 console */

typedef uint8_t (*AnticMemRead)(uint16_t addr, void *ud);

typedef struct Antic {
    /* ANTIC registers */
    uint8_t  dmactl, chactl, hscrol, vscrol, pmbase, chbase;
    uint8_t  nmien, nmist;
    uint16_t dlist;
    int      scanline;      /* 0..lines_total-1, advanced by the machine */
    int      lines_total;   /* 262 NTSC / 312 PAL */
    bool     pal;

    /* GTIA registers */
    uint8_t colpm[4], colpf[4], colbk;
    uint8_t prior, gractl, consol_w;
    uint8_t hposp[4], hposm[4], sizep[4], sizem;
    uint8_t grafp[4], grafm;

    AnticMemRead mem;
    void        *mem_ud;

    uint8_t  pixels[ANTIC_FB_W * ANTIC_FB_H];       /* GTIA color codes */
    uint32_t pixels_argb[ANTIC_FB_W * ANTIC_FB_H];

    uint16_t warned_modes;  /* bitmask of modes already warned about */
} Antic;

/* 256-entry hue/luma → 0xAARRGGBB table (low luma bit significant). */
extern uint32_t antic_palette_rgb[256];

void antic_init (Antic *a, AnticMemRead mem, void *mem_ud, bool pal);
void antic_reset(Antic *a);

/* Register access; reg is the low byte of the address ($D4xx / $D0xx).
 * consol_in: CONSOL read value (console keys, active low; 0x07 = none). */
uint8_t antic_reg_read (Antic *a, uint8_t reg);
void    antic_reg_write(Antic *a, uint8_t reg, uint8_t v);
uint8_t gtia_reg_read  (Antic *a, uint8_t reg, uint8_t consol_in,
                        uint8_t trig[4]);
void    gtia_reg_write (Antic *a, uint8_t reg, uint8_t v);

/* Walk the display list once and paint the full frame. */
void antic_render_frame(Antic *a);
