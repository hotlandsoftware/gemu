#include "antic.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ── Palette ─────────────────────────────────────────────────────────────── */

uint32_t antic_palette_rgb[256];

static void palette_init(void) {
    if (antic_palette_rgb[15]) return;
    /* NTSC YUV approximation: hue 0 = grayscale, hues 1-15 spaced around
     * the color wheel.  Constants follow the commonly used Atari800
     * "default" palette generator closely enough for recognisable colors. */
    for (int c = 0; c < 256; c++) {
        int hue = c >> 4, lum = c & 0x0F;
        double y = lum / 15.0;
        double u = 0.0, v = 0.0;
        if (hue) {
            double angle = ((double)(hue - 1) / 15.0) * 2.0 * M_PI + 0.7;
            u = 0.28 * cos(angle);
            v = 0.28 * sin(angle);
        }
        double r = y + 1.140 * v;
        double g = y - 0.395 * u - 0.581 * v;
        double b = y + 2.032 * u;
        #define CLAMP8(x) ((uint32_t)(((x) < 0 ? 0 : (x) > 1 ? 1 : (x)) * 255.0 + 0.5))
        antic_palette_rgb[c] = 0xFF000000u |
            (CLAMP8(r) << 16) | (CLAMP8(g) << 8) | CLAMP8(b);
        #undef CLAMP8
    }
}

/* ── Init / registers ────────────────────────────────────────────────────── */

void antic_init(Antic *a, AnticMemRead mem, void *mem_ud, bool pal) {
    palette_init();
    memset(a, 0, sizeof(*a));
    a->mem         = mem;
    a->mem_ud      = mem_ud;
    a->pal         = pal;
    a->lines_total = pal ? ANTIC_LINES_PAL : ANTIC_LINES_NTSC;
}

void antic_reset(Antic *a) {
    AnticMemRead mem = a->mem;
    void *ud = a->mem_ud;
    bool pal = a->pal;
    antic_init(a, mem, ud, pal);
}

uint8_t antic_reg_read(Antic *a, uint8_t reg) {
    switch (reg & 0x0F) {
    case 0x0B: return (uint8_t)(a->scanline >> 1);         /* VCOUNT */
    case 0x0C: return 0;                                    /* PENH */
    case 0x0D: return 0;                                    /* PENV */
    case 0x0F: return a->nmist;                             /* NMIST */
    default:   return 0xFF;
    }
}

void antic_reg_write(Antic *a, uint8_t reg, uint8_t v) {
    switch (reg & 0x0F) {
    case 0x00: a->dmactl = v; break;
    case 0x01: a->chactl = v; break;
    case 0x02: a->dlist  = (uint16_t)((a->dlist & 0xFF00u) | v); break;
    case 0x03: a->dlist  = (uint16_t)((a->dlist & 0x00FFu) | ((uint16_t)v << 8)); break;
    case 0x04: a->hscrol = v; break;
    case 0x05: a->vscrol = v; break;
    case 0x07: a->pmbase = v; break;
    case 0x09: a->chbase = v; break;
    case 0x0A: /* WSYNC — CPU stall, handled by the machine */ break;
    case 0x0E: a->nmien  = v; break;
    case 0x0F: a->nmist  = 0; break;                        /* NMIRES */
    default: break;
    }
}

uint8_t gtia_reg_read(Antic *a, uint8_t reg, uint8_t consol_in, uint8_t trig[4]) {
    reg &= 0x1F;
    if (reg <= 0x0F) return 0;                    /* collision regs — none */
    switch (reg) {
    case 0x10: case 0x11: case 0x12: case 0x13:   /* TRIG0-3, 1 = released */
        return trig ? trig[reg - 0x10] : 1;
    case 0x14: return a->pal ? 0x01 : 0x0F;       /* PAL flag */
    case 0x1F: return consol_in & 0x0F;           /* CONSOL */
    default:   return 0x0F;
    }
}

void gtia_reg_write(Antic *a, uint8_t reg, uint8_t v) {
    reg &= 0x1F;
    switch (reg) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        a->hposp[reg] = v; break;
    case 0x04: case 0x05: case 0x06: case 0x07:
        a->hposm[reg - 0x04] = v; break;
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        a->sizep[reg - 0x08] = v; break;
    case 0x0C: a->sizem = v; break;
    case 0x0D: case 0x0E: case 0x0F: case 0x10:
        a->grafp[reg - 0x0D] = v; break;
    case 0x11: a->grafm = v; break;
    case 0x12: case 0x13: case 0x14: case 0x15:
        a->colpm[reg - 0x12] = v; break;
    case 0x16: case 0x17: case 0x18: case 0x19:
        a->colpf[reg - 0x16] = v; break;
    case 0x1A: a->colbk  = v; break;
    case 0x1B: a->prior  = v; break;
    case 0x1D: a->gractl = v; break;
    case 0x1E: /* HITCLR */ break;
    case 0x1F: a->consol_w = v; break;            /* keyboard speaker on 400 */
    default: break;
    }
}

/* ── Renderer ────────────────────────────────────────────────────────────── */

/* Display-list counter increments wrap within 1K; screen memory within 4K. */
static inline uint16_t dl_next(uint16_t dl) {
    return (uint16_t)((dl & 0xFC00u) | ((dl + 1u) & 0x03FFu));
}
static inline uint16_t msc_next(uint16_t msc) {
    return (uint16_t)((msc & 0xF000u) | ((msc + 1u) & 0x0FFFu));
}

typedef struct {
    Antic  *a;
    int     y;          /* current framebuffer row */
    uint16_t msc;       /* memory scan counter */
} Render;

static inline void put_run(Render *r, int x, int w, int line, uint8_t color) {
    if (r->y + line < 0 || r->y + line >= ANTIC_FB_H) return;
    int base = (r->y + line) * ANTIC_FB_W;
    if (x < 0) { w += x; x = 0; }
    if (x + w > ANTIC_FB_W) w = ANTIC_FB_W - x;
    for (int i = 0; i < w; i++) {
        r->a->pixels[base + x + i]      = color;
        r->a->pixels_argb[base + x + i] = antic_palette_rgb[color];
    }
}

/* Playfield width from DMACTL bits 0-1 → bytes for a 40-byte-standard mode,
 * plus the framebuffer x origin.  Wide overscans; we clip to the fb. */
static void playfield_geometry(const Antic *a, int std_bytes, int *bytes, int *x0) {
    int px_per_byte = 320 / std_bytes;
    switch (a->dmactl & 3) {
    case 1: *bytes = std_bytes * 8 / 10; *x0 = (320 - *bytes * px_per_byte) / 2; break;
    case 3: *bytes = std_bytes * 12 / 10; *x0 = (320 - *bytes * px_per_byte) / 2; break;
    default: *bytes = std_bytes; *x0 = 0; break;
    }
}

/* mode geometry tables, indexed by ANTIC mode 2..15 */
static const uint8_t mode_std_bytes[16] = {
    0,0, 40,40,40,40,20,20, 10,10,20,20,20,40,40,40 };
static const uint8_t mode_lines[16] = {
    0,0, 8,10,8,16,8,16, 8,4,4,2,1,2,1,1 };

static void render_text_row(Render *r, int mode, int bytes, int x0, int n_lines) {
    Antic *a = r->a;
    bool twenty = (mode == 6 || mode == 7);   /* 20-byte, 16px-wide chars */
    int char_px = twenty ? 16 : 8;
    /* Double-height modes (5,7) fetch 8 glyph lines over 16 scanlines. */
    bool dbl = (mode == 5 || mode == 7);
    uint16_t chbase = twenty ? (uint16_t)((a->chbase & 0xFEu) << 8)
                             : (uint16_t)((a->chbase & 0xFCu) << 8);

    /* Mode 2/3 hi-res colors: bg = COLPF2, fg takes COLPF1's luma. */
    uint8_t hires_bg = a->colpf[2];
    uint8_t hires_fg = (uint8_t)((a->colpf[2] & 0xF0u) | (a->colpf[1] & 0x0Fu));

    for (int col = 0; col < bytes; col++) {
        uint8_t ch = a->mem(r->msc, a->mem_ud);
        r->msc = msc_next(r->msc);

        for (int line = 0; line < n_lines; line++) {
            int glyph_line = dbl ? line / 2 : line;
            uint8_t bits;
            uint16_t gaddr;

            if (twenty)
                gaddr = (uint16_t)(chbase + (ch & 0x3Fu) * 8u + (glyph_line & 7));
            else
                gaddr = (uint16_t)(chbase + (ch & 0x7Fu) * 8u + (glyph_line & 7));
            /* Mode 3 shows descenders on lines 8-9 for the last quarter of
             * the charset; approximate with blank lines there. */
            bits = (glyph_line < 8) ? a->mem(gaddr, a->mem_ud) : 0;
            if (a->chactl & 0x04)                 /* vertical reflect */
                bits = a->mem((uint16_t)(gaddr - (glyph_line & 7) + (7 - (glyph_line & 7))), a->mem_ud);

            int x = x0 + col * char_px;
            switch (mode) {
            case 2: case 3: {
                bool inv   = (ch & 0x80u) && (a->chactl & 0x02);
                bool blank = (ch & 0x80u) && (a->chactl & 0x01);
                if (blank) bits = 0;
                if (inv)   bits = (uint8_t)~bits;
                for (int b = 0; b < 8; b++)
                    put_run(r, x + b, 1, line,
                            (bits & (0x80u >> b)) ? hires_fg : hires_bg);
                break;
            }
            case 4: case 5: {
                for (int p = 0; p < 4; p++) {
                    unsigned pair = (bits >> (6 - p * 2)) & 3u;
                    uint8_t c;
                    if      (pair == 0) c = a->colbk;
                    else if (pair == 3) c = (ch & 0x80u) ? a->colpf[3] : a->colpf[2];
                    else                c = a->colpf[pair - 1];
                    put_run(r, x + p * 2, 2, line, c);
                }
                break;
            }
            case 6: case 7: {
                uint8_t fg = a->colpf[ch >> 6];
                for (int b = 0; b < 8; b++)
                    put_run(r, x + b * 2, 2, line,
                            (bits & (0x80u >> b)) ? fg : a->colbk);
                break;
            }
            }
        }
    }
}

static void render_map_row(Render *r, int mode, int bytes, int x0, int n_lines) {
    Antic *a = r->a;
    /* 1-bit modes: 9, B, C.  2-bit modes: 8, A, D, E.  Hi-res: F. */
    bool onebit = (mode == 9 || mode == 0xB || mode == 0xC);
    int units_per_byte = onebit ? 8 : 4;
    int unit_px = (320 / mode_std_bytes[mode]) / units_per_byte;

    uint8_t hires_bg = a->colpf[2];
    uint8_t hires_fg = (uint8_t)((a->colpf[2] & 0xF0u) | (a->colpf[1] & 0x0Fu));

    for (int col = 0; col < bytes; col++) {
        uint8_t bits = a->mem(r->msc, a->mem_ud);
        r->msc = msc_next(r->msc);
        int x = x0 + col * units_per_byte * unit_px;

        for (int u = 0; u < units_per_byte; u++) {
            uint8_t c;
            if (mode == 0xF) {
                c = (bits & (0x80u >> u)) ? hires_fg : hires_bg;
            } else if (onebit) {
                c = (bits & (0x80u >> u)) ? a->colpf[0] : a->colbk;
            } else {
                unsigned pair = (bits >> (6 - u * 2)) & 3u;
                c = pair ? a->colpf[pair - 1] : a->colbk;
            }
            for (int line = 0; line < n_lines; line++)
                put_run(r, x + u * unit_px, unit_px, line, c);
        }
    }
}

void antic_render_frame(Antic *a) {
    /* Background fill (border + blank lines + DMA-off). */
    uint8_t bg = a->colbk;
    for (int i = 0; i < ANTIC_FB_W * ANTIC_FB_H; i++) {
        a->pixels[i]      = bg;
        a->pixels_argb[i] = antic_palette_rgb[bg];
    }
    if (!(a->dmactl & 0x20) || (a->dmactl & 3) == 0)
        return;                                   /* display-list DMA off */

    if (a->prior & 0xC0) {                        /* GTIA modes GR.9/10/11 */
        if (!(a->warned_modes & 0x8000u)) {
            fprintf(stderr, "antic: GTIA graphics modes (PRIOR=%02X) not implemented\n",
                    a->prior);
            a->warned_modes |= 0x8000u;
        }
    }

    Render r = { .a = a, .y = 0, .msc = 0 };
    uint16_t dl = a->dlist;

    for (int safety = 0; safety < 1024 && r.y < ANTIC_FB_H; safety++) {
        uint8_t op = a->mem(dl, a->mem_ud);
        dl = dl_next(dl);
        int mode = op & 0x0F;

        if (mode == 0) {                          /* 1-8 blank lines */
            r.y += ((op >> 4) & 7) + 1;
            continue;
        }
        if (mode == 1) {
            uint8_t lo = a->mem(dl, a->mem_ud); dl = dl_next(dl);
            uint8_t hi = a->mem(dl, a->mem_ud);
            if (op & 0x40) break;                 /* JVB — frame done */
            dl = (uint16_t)(lo | ((uint16_t)hi << 8));
            continue;
        }

        if (op & 0x40) {                          /* LMS */
            uint8_t lo = a->mem(dl, a->mem_ud); dl = dl_next(dl);
            uint8_t hi = a->mem(dl, a->mem_ud); dl = dl_next(dl);
            r.msc = (uint16_t)(lo | ((uint16_t)hi << 8));
        }

        int bytes, x0;
        playfield_geometry(a, mode_std_bytes[mode], &bytes, &x0);
        int n_lines = mode_lines[mode];

        if (mode <= 7) render_text_row(&r, mode, bytes, x0, n_lines);
        else           render_map_row (&r, mode, bytes, x0, n_lines);
        r.y += n_lines;
    }
}
