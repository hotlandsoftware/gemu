/*
 * TMS9918A / TMS9928A / TMS9128 VDP - see tms9918.h.
 *
 * References: TI "TMS9918A/TMS9928A/TMS9929A Video Display Processors"
 * data manual (SPPU004) and Sean Young's "TMS9918A" notes for the
 * undocumented behaviour (first-byte address quirk, illegal modes, the
 * status register's sprite number when no fifth sprite is found).
 */
#include "tms9918.h"
#include <stdio.h>
#include <string.h>

const uint32_t tms9918_palette[16] = {
    0xFF000000, 0xFF000000, 0xFF21C842, 0xFF5EDC78,
    0xFF5455ED, 0xFF7D76FC, 0xFFD4524D, 0xFF42EBF5,
    0xFFFC5554, 0xFFFF7978, 0xFFD4C154, 0xFFE6CE80,
    0xFF21B03B, 0xFFC95BBA, 0xFFCCCCCC, 0xFFFFFFFF,
};

void tms9918_reset(Tms9918 *v) {
    memset(v->reg, 0, sizeof(v->reg));
    v->status = 0;
    v->addr = 0;
    v->latched = false;
    v->latch = 0;
    v->read_ahead = 0;
    v->line = 0;
    v->int_out = false;
}

static void update_int(Tms9918 *v) {
    v->int_out = (v->status & TMS9918_STATUS_F) && (v->reg[1] & 0x20u);
}

/* ── CPU interface ───────────────────────────────────────────────────────── */

uint8_t tms9918_read(Tms9918 *v, int mode) {
    v->latched = false;
    if (mode) {   /* status: reading clears F, 5S and C */
        uint8_t s = v->status;
        v->status &= 0x1Fu;
        update_int(v);
        return s;
    }
    uint8_t d = v->read_ahead;
    v->read_ahead = v->vram[v->addr];
    v->addr = (v->addr + 1u) & 0x3FFFu;
    return d;
}

uint8_t tms9918_peek_status(const Tms9918 *v) { return v->status; }

void tms9918_write(Tms9918 *v, int mode, uint8_t val) {
    if (!mode) {
        v->latched = false;
        v->vram[v->addr] = val;
        v->read_ahead = val;
        v->addr = (v->addr + 1u) & 0x3FFFu;
        return;
    }
    if (!v->latched) {
        /* The first byte goes straight into the low half of the address. */
        v->latch = val;
        v->latched = true;
        v->addr = (uint16_t)((v->addr & 0x3F00u) | val);
        return;
    }
    v->latched = false;
    if (val & 0x80u) {
        v->reg[val & 7u] = v->latch;
        update_int(v);
        return;
    }
    v->addr = (uint16_t)(((val & 0x3Fu) << 8) | v->latch);
    if (!(val & 0x40u)) {   /* read setup: prefetch */
        v->read_ahead = v->vram[v->addr];
        v->addr = (v->addr + 1u) & 0x3FFFu;
    }
}

/* ── Rendering ───────────────────────────────────────────────────────────── */

static inline uint32_t color(const Tms9918 *v, unsigned c) {
    return tms9918_palette[c ? c : (v->reg[7] & 0x0Fu)];
}

/* Graphics II table addresses: R3/R4's low bits mask the character number. */
static inline uint16_t g2_pattern(const Tms9918 *v, unsigned chr, unsigned row) {
    unsigned off = (chr * 8u + row) & ((((unsigned)v->reg[4] & 3u) << 11) | 0x7FFu);
    return (uint16_t)((((unsigned)v->reg[4] & 4u) << 11) | off);
}

static inline uint16_t g2_color(const Tms9918 *v, unsigned chr, unsigned row) {
    unsigned off = (chr * 8u + row) & ((((unsigned)v->reg[3] & 0x7Fu) << 6) | 0x3Fu);
    return (uint16_t)((((unsigned)v->reg[3] & 0x80u) << 6) | off);
}

static void draw_background(Tms9918 *v, int y, uint8_t *pix) {
    const uint8_t *m = v->vram;
    unsigned nt = ((unsigned)v->reg[2] & 0x0Fu) << 10;
    unsigned ct = (unsigned)v->reg[3] << 6;
    unsigned pg = ((unsigned)v->reg[4] & 7u) << 11;
    bool m1 = v->reg[1] & 0x10u, m2 = v->reg[1] & 0x08u, m3 = v->reg[0] & 0x02u;
    unsigned fg_t = v->reg[7] >> 4, bg_t = v->reg[7] & 0x0Fu;
    unsigned row = (unsigned)y >> 3, line = (unsigned)y & 7u;

    if (m1) {   /* Text: 40 columns of 6 pixels, 8-pixel borders */
        memset(pix, (int)bg_t, 256);
        for (unsigned col = 0; col < 40; col++) {
            uint8_t p;
            if (m2 || m3) {
                p = 0xF0;   /* illegal mode: 4 pixels of text color, 2 of background */
            } else {
                uint8_t chr = m[(nt + row * 40u + col) & 0x3FFFu];
                p = m[(pg + chr * 8u + line) & 0x3FFFu];
            }
            for (unsigned b = 0; b < 6; b++)
                pix[8 + col * 6 + b] = (uint8_t)((p & (0x80u >> b)) ? fg_t : bg_t);
        }
        return;
    }
    for (unsigned col = 0; col < 32; col++) {
        unsigned chr = m[(nt + row * 32u + col) & 0x3FFFu];
        uint8_t *o = pix + col * 8;
        if (m2) {   /* Multicolor: 4x4 blocks */
            unsigned sub = ((row & 3u) << 1) | ((unsigned)y >> 2 & 1u);
            uint8_t c = m3 ? m[g2_pattern(v, (row >> 3) * 256u + chr, sub)]
                           : m[(pg + chr * 8u + sub) & 0x3FFFu];
            memset(o, c >> 4, 4);
            memset(o + 4, c & 0x0F, 4);
            continue;
        }
        uint8_t p, c;
        if (m3) {   /* Graphics II: the screen in thirds, colors per pixel row */
            unsigned n = (row >> 3) * 256u + chr;
            p = m[g2_pattern(v, n, line)];
            c = m[g2_color(v, n, line)];
        } else {    /* Graphics I */
            p = m[(pg + chr * 8u + line) & 0x3FFFu];
            c = m[(ct + (chr >> 3)) & 0x3FFFu];
        }
        for (unsigned b = 0; b < 8; b++)
            o[b] = (uint8_t)((p & (0x80u >> b)) ? (c >> 4) : (c & 0x0Fu));
    }
}

static void draw_sprites(Tms9918 *v, int y, uint8_t *pix) {
    const uint8_t *m = v->vram;
    unsigned sat = ((unsigned)v->reg[5] & 0x7Fu) << 7;
    unsigned spg = ((unsigned)v->reg[6] & 7u) << 11;
    bool big = v->reg[1] & 0x02u, mag = v->reg[1] & 0x01u;
    unsigned size = big ? 16u : 8u, h = size << (mag ? 1 : 0);

    uint8_t shown[256];   /* a higher-priority sprite's pattern bit is here */
    memset(shown, 0, sizeof(shown));
    int on_line = 0, s;
    for (s = 0; s < 32; s++) {
        const uint8_t *a = m + ((sat + (unsigned)s * 4u) & 0x3FFFu);
        if (a[0] == 0xD0) break;
        unsigned dy = (uint8_t)(y - a[0] - 1);
        if (dy >= h) continue;
        if (on_line == 4) {
            if (!(v->status & TMS9918_STATUS_5S))
                v->status = (uint8_t)((v->status & 0xA0u) | TMS9918_STATUS_5S | s);
            break;
        }
        on_line++;
        unsigned r = mag ? dy >> 1 : dy;
        unsigned name = big ? (a[2] & 0xFCu) : a[2];
        unsigned base = spg + name * 8u + r;
        uint16_t bits = (uint16_t)(m[base & 0x3FFFu] << 8);
        if (big) bits |= m[(base + 16u) & 0x3FFFu];
        int x = a[1] - ((a[3] & 0x80u) ? 32 : 0);
        unsigned c = a[3] & 0x0Fu;
        unsigned w = size << (mag ? 1 : 0);
        for (unsigned i = 0; i < w; i++) {
            int px = x + (int)i;
            if (px < 0 || px > 255) continue;
            if (!(bits & (0x8000u >> (mag ? i >> 1 : i)))) continue;
            if (shown[px]) { v->status |= TMS9918_STATUS_C; continue; }
            shown[px] = 1;
            if (c) pix[px] = (uint8_t)c;
        }
    }
    if (!(v->status & TMS9918_STATUS_5S))
        v->status = (uint8_t)((v->status & 0xE0u) | (s > 31 ? 31 : s));
}

static void fill_row(Tms9918 *v, int fy, uint32_t c) {
    uint32_t *o = v->fb + fy * TMS9918_FB_W;
    for (int x = 0; x < TMS9918_FB_W; x++) o[x] = c;
}

bool tms9918_scanline(Tms9918 *v) {
    int y = v->line;
    if (y < TMS9918_ACTIVE_H) {
        uint32_t *o = v->fb + (y + TMS9918_BORDER_Y) * TMS9918_FB_W;
        uint32_t back = color(v, 0);
        for (int x = 0; x < TMS9918_BORDER_X; x++)
            o[x] = o[TMS9918_BORDER_X + TMS9918_ACTIVE_W + x] = back;
        uint8_t pix[256];
        if (!(v->reg[1] & 0x40u)) {   /* blanked: backdrop only */
            memset(pix, 0, sizeof(pix));
        } else {
            draw_background(v, y, pix);
            if (!(v->reg[1] & 0x10u)) draw_sprites(v, y, pix);
        }
        for (int x = 0; x < 256; x++) o[TMS9918_BORDER_X + x] = color(v, pix[x]);
    }
    bool frame_done = false;
    if (y == TMS9918_ACTIVE_H - 1) {   /* the bottom border starts: vertical blank */
        uint32_t back = color(v, 0);
        for (int fy = 0; fy < TMS9918_BORDER_Y; fy++) {
            fill_row(v, fy, back);
            fill_row(v, TMS9918_BORDER_Y + TMS9918_ACTIVE_H + fy, back);
        }
        v->status |= TMS9918_STATUS_F;
        update_int(v);
        v->frame++;
        frame_done = true;
    }
    v->line = (y + 1) % TMS9918_LINES;
    return frame_done;
}

void tms9918_describe(const Tms9918 *v, char *buf, size_t len) {
    static const char *const modes[8] = {
        "Graphics I", "Text", "Multicolor", "illegal (M1+M2)",
        "Graphics II", "illegal (M1+M3)", "Multicolor II", "illegal (M1+M2+M3)",
    };
    unsigned mode = ((v->reg[1] >> 4) & 1u) | ((v->reg[1] >> 2) & 2u) | ((v->reg[0] << 1) & 4u);
    snprintf(buf, len,
             "R0-7: %02X %02X %02X %02X %02X %02X %02X %02X  status %02X  addr %04X%s\n"
             "  %s, display %s, %s sprites%s, INT %s\n"
             "  name %04X  color %04X  pattern %04X  sprite attr %04X  sprite pattern %04X\n",
             v->reg[0], v->reg[1], v->reg[2], v->reg[3], v->reg[4], v->reg[5], v->reg[6], v->reg[7],
             v->status, v->addr, v->latched ? " (first byte latched)" : "",
             modes[mode], (v->reg[1] & 0x40u) ? "on" : "blanked",
             (v->reg[1] & 0x02u) ? "16x16" : "8x8", (v->reg[1] & 0x01u) ? " magnified" : "",
             (v->reg[1] & 0x20u) ? "enabled" : "disabled",
             ((unsigned)v->reg[2] & 0x0Fu) << 10, (unsigned)v->reg[3] << 6,
             ((unsigned)v->reg[4] & 7u) << 11, ((unsigned)v->reg[5] & 0x7Fu) << 7,
             ((unsigned)v->reg[6] & 7u) << 11);
}
