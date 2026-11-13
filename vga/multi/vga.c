/* IBM VGA-compatible core; see vga.h. */
#include "vga.h"

#include "vgafont16.h"

#include <string.h>

typedef enum { VGA_ADDR_PLANAR, VGA_ADDR_CHAIN4, VGA_ADDR_ODDEVEN } VgaAddrMode;

/* Graphics controller register 6 bits 3:2 select which part of the legacy
 * window the planes answer in: 00 A0000 (128 KiB), 01 A0000 (64 KiB),
 * 10 B0000 (32 KiB), 11 B8000 (32 KiB). Returns the plane address for an
 * offset from 0xA0000, or -1 outside the decoded range. */
static int32_t vga_map_offset(const Vga *vg, uint32_t off) {
    switch ((vg->gc[6] >> 2) & 3) {
    case 0:
        return off < 0x20000 ? (int32_t)off : -1;
    case 1:
        return off < 0x10000 ? (int32_t)off : -1;
    case 2:
        return (off >= 0x10000 && off < 0x18000) ? (int32_t)(off - 0x10000) : -1;
    default:
        return (off >= 0x18000 && off < 0x20000) ? (int32_t)(off - 0x18000) : -1;
    }
}

static VgaAddrMode vga_addr_mode(const Vga *vg) {
    if (vg->seq[4] & 0x08) {
        return VGA_ADDR_CHAIN4;
    }
    if (vg->gc[5] & 0x10) {
        return VGA_ADDR_ODDEVEN;
    }
    return VGA_ADDR_PLANAR;
}

uint8_t vga_mem_read(Vga *vg, uint32_t off) {
    const int32_t moff = vga_map_offset(vg, off);
    if (moff < 0) {
        return 0xFF;
    }
    const uint32_t a = (uint32_t)moff;

    switch (vga_addr_mode(vg)) {
    case VGA_ADDR_CHAIN4:
        return vg->plane[a & 3u][(a >> 2) & 0xFFFF];
    case VGA_ADDR_ODDEVEN: {
        /* Even addresses read the even plane of the pair GC4 selects, odd
         * addresses the odd one, at half the address: in text mode that puts
         * characters in plane 0 and attributes in plane 1. */
        const unsigned plane = (unsigned)((vg->gc[4] & 2u) | (a & 1u));
        return vg->plane[plane][(a >> 1) & 0xFFFF];
    }
    default: {
        const uint32_t o = a & 0xFFFF;
        for (unsigned p = 0; p < 4; p++) {
            vg->latch[p] = vg->plane[p][o];
        }
        if (vg->gc[5] & 0x08u) {
            /* Read mode 1, colour compare: a result bit is 1 where every
             * plane enabled in Colour Don't Care (GC7) matches Colour Compare
             * (GC2). With GC7 = 0 the result is 0xFF. */
            uint8_t diff = 0;
            for (unsigned p = 0; p < 4; p++) {
                if (!(vg->gc[7] & (1u << p))) {
                    continue;
                }
                const uint8_t want = ((vg->gc[2] >> p) & 1u) ? 0xFFu : 0x00u;
                diff |= (uint8_t)(vg->latch[p] ^ want);
            }
            return (uint8_t)~diff;
        }
        return vg->plane[vg->gc[4] & 3][o];
    }
    }
}

static inline void plane_mark(Vga *vg, uint32_t o) {
    const uint32_t c = (o & 0xFFFFu) >> VGA_PLANE_CHUNK_SHIFT;
    vg->plane_dirty[c >> 6] |= UINT64_C(1) << (c & 63u);
}

void vga_mem_write(Vga *vg, uint32_t off, uint8_t val) {
    const int32_t moff = vga_map_offset(vg, off);
    if (moff < 0) {
        return;
    }
    const uint32_t a = (uint32_t)moff;
    const uint8_t mapmask = (uint8_t)(vg->seq[2] & 0x0F);
    const VgaAddrMode mode = vga_addr_mode(vg);

    /* Chain-4 and odd/even addressing are selected by the sequencer and GC
     * registers, not by the address. The plane offset written is marked for
     * the renderer even if the map mask then stores nothing. */
    switch (mode) {
    case VGA_ADDR_CHAIN4: {
        plane_mark(vg, a >> 2);
        const unsigned plane = a & 3u;
        if (mapmask & (1u << plane)) {
            vg->plane[plane][(a >> 2) & 0xFFFF] = val;
        }
        return;
    }
    case VGA_ADDR_ODDEVEN: {
        plane_mark(vg, a >> 1);
        const unsigned plane = (unsigned)((vg->gc[4] & 2u) | (a & 1u));
        if (mapmask & (1u << plane)) {
            vg->plane[plane][(a >> 1) & 0xFFFF] = val;
        }
        return;
    }
    default:
        plane_mark(vg, a);
        break;
    }

    off = a & 0xFFFF;
    const uint8_t wmode = (uint8_t)(vg->gc[5] & 3);
    const uint8_t setres = (uint8_t)(vg->gc[0] & 0x0F);
    const uint8_t esr = (uint8_t)(vg->gc[1] & 0x0F);
    const uint8_t rot = (uint8_t)(vg->gc[3] & 7);
    const uint8_t op = (uint8_t)((vg->gc[3] >> 3) & 3);
    const uint8_t bitmask = vg->gc[8];

    /* A character generator load is a planar write enabling plane 2 alone
     * while the controller still says alphanumeric. A write that reaches plane
     * 2 together with others in a graphics mode overwrites it. */
    if (mapmask & 4u) {
        if (mapmask == 4u) {
            vg->font_uploaded = true;
        } else if (vg->gc[6] & 1u) {
            vg->font_uploaded = false;
        }
    }

    for (unsigned p = 0; p < 4; p++) {
        if (!(mapmask & (1u << p))) {
            continue;
        }
        uint8_t data;
        uint8_t bm = bitmask;
        switch (wmode) {
        case 0: {
            const uint8_t v = (uint8_t)((val >> rot) | (val << (8 - rot)));
            data = (esr & (1u << p)) ? (((setres >> p) & 1) ? 0xFF : 0x00) : v;
            break;
        }
        case 1:  /* latch to memory: the bit mask does not apply */
            data = vg->latch[p];
            bm = 0xFF;
            break;
        case 2:  /* the byte is a colour, replicated per plane */
            data = ((val >> p) & 1) ? 0xFF : 0x00;
            break;
        default:  /* mode 3: the byte ANDs into the bit mask */
            data = ((setres >> p) & 1) ? 0xFF : 0x00;
            bm = (uint8_t)(bitmask & val);
            break;
        }
        if (wmode == 0 || wmode == 3) {
            switch (op) {  /* GC3 bits 4:3, the logical function */
            case 1:
                data &= vg->latch[p];
                break;
            case 2:
                data |= vg->latch[p];
                break;
            case 3:
                data ^= vg->latch[p];
                break;
            default:
                break;
            }
        }
        vg->plane[p][off] = (uint8_t)((data & bm) | (vg->latch[p] & (uint8_t)~bm));
    }
}

/* ---- display state ------------------------------------------------------- */

/* Which address the legacy window's plane offset 0 corresponds to. */
static uint32_t vga_window_base(const Vga *vg) {
    switch ((vg->gc[6] >> 2) & 3) {
    case 2:
        return 0xB0000;
    case 3:
        return 0xB8000;
    default:
        return 0xA0000;
    }
}

/* GC6 bit 0 is Alphanumeric Mode Disable. */
bool vga_text_mode(const Vga *vg) {
    return (vg->gc[6] & 0x01) == 0;
}

/* The picture is blanked while the attribute controller's Palette Address
 * Source bit is clear, unless graphics-controller programming shows the
 * guest has set a mode up. */
bool vga_blanked(const Vga *vg) {
    return (vg->attr_idx & 0x20) == 0 && vg->gc[6] == 0;
}

/* CRTC 0C/0D, the start address: scrolling a text screen moves this. */
static uint16_t vga_text_start(const Vga *vg) {
    return (uint16_t)(((uint16_t)vg->crtc[0x0C] << 8) | vg->crtc[0x0D]);
}

void vga_text_geometry(const Vga *vg, unsigned *cols, unsigned *rows,
                       unsigned *cell_w, unsigned *cell_h) {
    /* CRTC1 is the horizontal display end (characters - 1); CRTC12 the
     * vertical display end's low byte with bits 9:8 in the overflow register
     * (CRTC7). CRTC9 bits 4:0 are the maximum scan line (cell height - 1). An
     * unprogrammed CRTC falls back to the 80x25, 16-line page. */
    unsigned c = (unsigned)vg->crtc[1] + 1;
    unsigned vde = (unsigned)vg->crtc[0x12] + 1;
    if (vg->crtc[7] & 0x02) {
        vde += 0x100;
    }
    if (vg->crtc[7] & 0x40) {
        vde += 0x200;
    }
    const unsigned ch_raw = (unsigned)(vg->crtc[9] & 0x1F);
    const unsigned ch = ch_raw ? ch_raw + 1 : 16;
    unsigned r = vde / ch;
    if (c < 40 || c > 132) {
        c = 80;
    }
    if (r < 12 || r > 60) {
        r = 25;
    }
    *cols = c;
    *rows = r;
    *cell_w = (vg->seq[1] & 0x01) ? 8 : 9;
    *cell_h = ch;
}

/* Glyph row for character `ch`; attribute bit 3 selects the character map
 * SEQ3 names for it. The generator is plane 2 once the guest has loaded one
 * (32 rows per glyph, 8 KiB per map), else the 8x16 ROM font. */
static uint8_t vga_glyph(const Vga *vg, uint8_t ch, uint8_t attr, unsigned row) {
    if (vg->font_uploaded) {
        if (row >= 32) {
            return 0;
        }
        const uint8_t v = vg->seq[3];
        const uint32_t map = (attr & 0x08u)
            ? (uint32_t)(((v >> 5) & 1u) | ((v >> 1) & 6u))
            : (uint32_t)(((v >> 4) & 1u) | ((v << 1) & 6u));
        return vg->plane[2][(map * 8192u + (uint32_t)ch * 32 + row) & 0xFFFF];
    }
    return row < FONT_HEIGHT ? vgafont16[ch * FONT_HEIGHT + row] : 0;
}

/* The whole colour path of a VGA pixel value as ARGB: the DAC's pixel mask
 * ANDs the index before the lookup, and the attribute palette maps the 16
 * planar colours onto DAC entries. */
static void vga_palette(const Vga *vg, uint32_t *argb) {
    for (unsigned i = 0; i < 256; i++) {
        const unsigned idx = ((i < 16) ? (vg->attr[i] & 0x3Fu) : i) & vg->pixel_mask;
        uint32_t c = 0xFF000000u;
        for (unsigned k = 0; k < 3; k++) {
            const uint32_t v6 = vg->dac[idx * 3u + k] & 0x3Fu;
            c |= ((v6 << 2) | (v6 >> 4)) << (16 - 8 * k);
        }
        argb[i] = c;
    }
}

/* ---- register ports ------------------------------------------------------ */

bool vga_io(Vga *vg, bool write, uint16_t port, unsigned len, uint64_t *val) {
    /* Misc Output bit 0 (I/O address select) puts the CRTC index/data and
     * input status 1 / feature control at 3D4/3D5/3DA (1) or 3B4/3B5/3BA (0);
     * the other set is not decoded at all, which is how a BIOS tells a mono
     * from a colour adapter. */
    if (port == 0x3B4 || port == 0x3B5 || port == 0x3BA) {
        if (vg->misc & 1u) {
            return false;
        }
        port = (uint16_t)(port + 0x20);
    } else if (port == 0x3D4 || port == 0x3D5 || port == 0x3DA) {
        if (!(vg->misc & 1u)) {
            return false;
        }
    }
    switch (port) {
    case 0x3C0:
    case 0x3C1:
        if (write) {
            if (port != 0x3C0) {
                return true;  /* 3C1 is the read port */
            }
            if (!vg->attr_flipflop) {
                vg->attr_idx = (uint8_t)*val;
            } else {
                vg->attr[vg->attr_idx & 0x1F] = (uint8_t)*val;
            }
            vg->attr_flipflop = !vg->attr_flipflop;
        } else {
            /* 3C0 reads the address register (index and Palette Address
             * Source), 3C1 the data register it selects; neither moves the
             * flip-flop. */
            *val = port == 0x3C0 ? vg->attr_idx : vg->attr[vg->attr_idx & 0x1F];
        }
        return true;
    case 0x3C2:  /* misc output (write) / input status 0 (read) */
        if (write) {
            vg->misc = (uint8_t)*val;
        } else {
            *val = 0x10;
        }
        return true;
    case 0x3CC:  /* misc output read-back */
        if (!write) {
            *val = vg->misc;
        }
        return true;
    case 0x3CA:  /* feature control read-back */
        if (!write) {
            *val = 0x00;
        }
        return true;
    /* A word write to an index port programs the data register too. */
    case 0x3C4:
        if (write) {
            vg->seq_idx = (uint8_t)*val;
            if (len >= 2) {
                vg->seq[vg->seq_idx & 7] = (uint8_t)(*val >> 8);
            }
        } else {
            *val = vg->seq_idx;
        }
        return true;
    case 0x3C5:
        if (write) {
            vg->seq[vg->seq_idx & 7] = (uint8_t)*val;
        } else {
            *val = vg->seq[vg->seq_idx & 7];
        }
        return true;
    case 0x3C6:  /* DAC pixel mask */
        if (write) {
            vg->pixel_mask = (uint8_t)*val;
        } else {
            *val = vg->pixel_mask;
        }
        return true;
    case 0x3C7:
        /* Write: the DAC read address, where reads at 3C9 start. Read: the
         * DAC state, 3 after a read address was written, 0 after a write
         * address. */
        if (write) {
            vg->dac_ridx = (uint8_t)*val;
            vg->dac_rsub = 0;
            vg->dac_reading = true;
        } else {
            *val = vg->dac_reading ? 0x03u : 0x00u;
        }
        return true;
    case 0x3C8:
        if (write) {
            vg->dac_idx = (uint8_t)*val;
            vg->dac_sub = 0;
            vg->dac_reading = false;
        } else {
            *val = vg->dac_idx;
        }
        return true;
    case 0x3C9:
        /* Writes walk the write address (3C8), reads the read address (3C7):
         * the DAC keeps the two apart. */
        if (write) {
            vg->dac[vg->dac_idx * 3 + vg->dac_sub] = (uint8_t)(*val & 0x3F);
            if (++vg->dac_sub == 3) {
                vg->dac_sub = 0;
                vg->dac_idx++;
            }
        } else {
            *val = vg->dac[vg->dac_ridx * 3 + vg->dac_rsub];
            if (++vg->dac_rsub == 3) {
                vg->dac_rsub = 0;
                vg->dac_ridx++;
            }
        }
        return true;
    case 0x3CE:
        if (write) {
            vg->gc_idx = (uint8_t)*val;
            if (len >= 2) {
                vg->gc[vg->gc_idx & 0xF] = (uint8_t)(*val >> 8);
            }
        } else {
            *val = vg->gc_idx;
        }
        return true;
    case 0x3CF:
        if (write) {
            vg->gc[vg->gc_idx & 0xF] = (uint8_t)*val;
        } else {
            *val = vg->gc[vg->gc_idx & 0xF];
        }
        return true;
    case 0x3D4:
        if (write) {
            vg->crtc_idx = (uint8_t)*val;
            if (len >= 2) {
                vg->crtc[vg->crtc_idx & 0x1F] = (uint8_t)(*val >> 8);
            }
        } else {
            *val = vg->crtc_idx;
        }
        return true;
    case 0x3D5:
        if (write) {
            vg->crtc[vg->crtc_idx & 0x1F] = (uint8_t)*val;
        } else {
            *val = vg->crtc[vg->crtc_idx & 0x1F];
        }
        return true;
    case 0x3DA:  /* input status 1; either access resets the attribute flip-flop */
        vg->attr_flipflop = false;
        if (!write) {
            *val = vg->retrace;
        }
        return true;
    default:
        return false;
    }
}

/* ---- lifecycle ----------------------------------------------------------- */

void vga_init(Vga *vg, uint8_t *vram, size_t vram_size) {
    memset(vg, 0, sizeof *vg);
    vga_reset(vg);
    vga_attach_vram(vg, vram, vram_size);
}

void vga_attach_vram(Vga *vg, uint8_t *vram, size_t vram_size) {
    vg->vram = vram;
    vg->vram_size = vram_size < VGA_VRAM_MAX ? vram_size : VGA_VRAM_MAX;
    vga_touch_all(vg);
}

void vga_reset(Vga *vg) {
    vg->seq_idx = vg->gc_idx = vg->crtc_idx = vg->attr_idx = 0;
    memset(vg->seq, 0, sizeof vg->seq);
    memset(vg->gc, 0, sizeof vg->gc);
    memset(vg->crtc, 0, sizeof vg->crtc);
    memset(vg->attr, 0, sizeof vg->attr);
    vg->attr_flipflop = false;
    /* Misc Output bit 0 selects the colour I/O range (3Dx) over the mono one
     * (3Bx); a card that powered up mono would have firmware probing the wrong
     * ports. */
    vg->misc = 0x01;
    vg->dac_idx = vg->dac_sub = vg->dac_ridx = vg->dac_rsub = 0;
    vg->pixel_mask = 0xFF;
    vg->dac_reading = false;
    memset(vg->latch, 0, sizeof vg->latch);
    vg->font_uploaded = false;
    vg->retrace = 0;
    vga_touch_all(vg);
}

bool vga_vram_take_any(Vga *vg) {
    bool any = false;
    for (size_t i = 0; i < VGA_VRAM_DIRTY_WORDS; i++) {
        if (vg->vram_dirty[i]) {
            any = true;
            vg->vram_dirty[i] = 0;
        }
    }
    return any;
}

void vga_touch_all(Vga *vg) {
    vg->disp_gen++;
    memset(vg->vram_dirty, 0xFF, sizeof vg->vram_dirty);
    memset(vg->plane_dirty, 0xFF, sizeof vg->plane_dirty);
}

/* A hash of every register the renderer reads: mode and window (GC5/6), cell
 * width, character map and chain-4 (SEQ1/3/4), misc, the CRTC and attribute
 * files with the attribute index (its Palette Address Source bit blanks), the
 * DAC and the font flag. The write-path registers of the graphics controller
 * and the map mask are left out: a planar driver reprograms them around every
 * store. */
uint64_t vga_render_key(const Vga *vg) {
    uint64_t h = UINT64_C(1469598103934665603);
#define KEY_BYTES(p, n) do { \
        const uint8_t *b_ = (const uint8_t *)(p); \
        for (size_t i_ = 0; i_ < (n); i_++) { \
            h = (h ^ b_[i_]) * UINT64_C(1099511628211); \
        } \
    } while (0)
    const uint8_t regs[] = { vg->gc[5], vg->gc[6], vg->seq[1], vg->seq[3],
                             vg->seq[4], vg->misc, vg->attr_idx,
                             (uint8_t)vg->font_uploaded };
    KEY_BYTES(regs, sizeof regs);
    KEY_BYTES(vg->crtc, sizeof vg->crtc);
    KEY_BYTES(vg->attr, sizeof vg->attr);
    KEY_BYTES(vg->dac, sizeof vg->dac);
#undef KEY_BYTES
    return h;
}

bool vga_planes_dirty(const Vga *vg) {
    for (size_t i = 0; i < VGA_PLANE_DIRTY_WORDS; i++) {
        if (vg->plane_dirty[i]) {
            return true;
        }
    }
    return false;
}

/* ---- screen composition -------------------------------------------------- */

/* `chunks` is NULL to draw everything, else the plane chunks written since
 * the last draw: a planar mode then redraws only the rows over them. */
static bool vga_draw(const Vga *vg, uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
                     uint32_t w, uint32_t h, unsigned xscale, uint32_t *out_w,
                     uint32_t *out_h, const uint64_t *chunks, uint32_t *y0,
                     uint32_t *y1) {
    *y0 = *y1 = 0;
    *out_w = 0;
    *out_h = 0;
    if (!fb_w || !fb_h) {
        return false;
    }

    /* Decide first, clear second: a blanked screen leaves the previous
     * picture where it is. */
    if (vga_blanked(vg)) {
        return false;
    }
    if (xscale == 0) {
        xscale = 1;
    }

    uint32_t rgb[256];
    vga_palette(vg, rgb);

    if (vga_text_mode(vg)) {
        unsigned cols, rows, cw, ch;
        vga_text_geometry(vg, &cols, &rows, &cw, &ch);
        for (size_t i = 0; i < (size_t)fb_w * fb_h; i++) {
            fb[i] = 0xFF000000u;
        }
        /* Text sits at B8000: its plane offset within the window the GC
         * selects, in cells, plus the CRTC start address. */
        const uint32_t wbase = vga_window_base(vg);
        const uint32_t text_cells = (0xB8000u > wbase) ? ((0xB8000u - wbase) / 2u) : 0u;
        const uint16_t start = vga_text_start(vg);
        const uint32_t tstart = (uint32_t)start + text_cells;
        const uint16_t cursor = (uint16_t)(((uint16_t)vg->crtc[0x0E] << 8) | vg->crtc[0x0F]);
        const unsigned cstart = vg->crtc[0x0A] & 0x1Fu, cend = vg->crtc[0x0B] & 0x1Fu;
        const bool cursor_on = !(vg->crtc[0x0A] & 0x20u) && cstart <= cend;

        for (unsigned r = 0; r < rows; r++) {
            for (unsigned c = 0; c < cols; c++) {
                const uint32_t cell = (uint32_t)(r * cols + c);
                /* Plane 0 is the character, plane 1 the attribute: the
                 * odd/even split, not two consecutive bytes. */
                const uint8_t chr = vg->plane[0][(tstart + cell) & 0xFFFF];
                const uint8_t att = vg->plane[1][(tstart + cell) & 0xFFFF];
                const uint32_t fg = rgb[att & 0x0Fu];
                const uint32_t bg = rgb[(att >> 4) & 0x07u];
                const bool is_cursor = cursor_on && cursor == (uint16_t)(start + cell);

                for (unsigned gy = 0; gy < ch; gy++) {
                    const uint32_t py = r * ch + gy;
                    if (py >= fb_h) {
                        break;
                    }
                    const uint8_t bits = vga_glyph(vg, chr, att, gy);
                    const bool on_cursor_row = is_cursor && gy >= cstart && gy <= cend;
                    for (unsigned gx = 0; gx < cw; gx++) {
                        /* Glyphs are 8 pixels wide. A 9-dot cell's ninth
                         * column is background, except that the line-drawing
                         * characters C0-DF repeat column 8 when ATC Mode bit 2
                         * (Line Graphics Enable) is set. */
                        bool set;
                        if (gx < 8u) {
                            set = (bits >> (7u - gx)) & 1u;
                        } else {
                            set = chr >= 0xC0 && chr <= 0xDF &&
                                  (vg->attr[0x10] & 0x04u) && (bits & 1u);
                        }
                        const uint32_t v = (set || on_cursor_row) ? fg : bg;
                        for (unsigned s = 0; s < xscale; s++) {
                            const uint32_t px = (c * cw + gx) * xscale + s;
                            if (px < fb_w) {
                                fb[py * fb_w + px] = v;
                            }
                        }
                    }
                }
            }
        }
        *out_w = (uint32_t)(cols * cw * xscale);
        *out_h = (uint32_t)(rows * ch);
        *y1 = fb_h;
        return true;
    }

    /* Graphics geometry from the CRTC: width (CRTC1 + 1) * 8, height from
     * CRTC12 and the overflow bits, line stride CRTC13 * 2 bytes per plane,
     * start address CRTC0C/0D. The caller's size is the fallback for an
     * unprogrammed CRTC. */
    uint32_t stride = w / 8u, start = 0;
    {
        uint32_t vde = (uint32_t)vg->crtc[0x12];
        if (vg->crtc[7] & 0x02) {
            vde |= 0x100;
        }
        if (vg->crtc[7] & 0x40) {
            vde |= 0x200;
        }
        if (vg->crtc[1] && vde) {
            w = ((uint32_t)vg->crtc[1] + 1u) * 8u;
            h = vde + 1u;
            stride = vg->crtc[0x13] ? (uint32_t)vg->crtc[0x13] * 2u : w / 8u;
            start = ((uint32_t)vg->crtc[0x0C] << 8) | vg->crtc[0x0D];
        }
    }
    if (!w || !h) {
        return false;
    }
    if (!chunks) {
        for (size_t i = 0; i < (size_t)fb_w * fb_h; i++) {
            fb[i] = 0xFF000000u;
        }
    }
    const uint32_t dh = (h < fb_h) ? h : fb_h;
    *out_w = (w * xscale < fb_w) ? w * xscale : fb_w;
    *out_h = dh;
    /* 4-bit planar: one byte per 8 pixels per plane. */
    uint32_t lo = chunks ? dh : 0, hi = chunks ? 0 : fb_h;
    for (uint32_t y = 0; y < dh; y++) {
        if (chunks) {
            /* The row's plane bytes, [row, row + w/8), wrapping at 64 KiB. */
            const uint32_t row = start + y * stride, n = (w + 7u) / 8u;
            bool any = false;
            for (uint32_t o = row; o < row + n + 63u && !any; o += 64u) {
                const uint32_t c = ((o < row + n ? o : row + n - 1u) & 0xFFFFu) >>
                                   VGA_PLANE_CHUNK_SHIFT;
                any = (chunks[c >> 6] >> (c & 63u)) & 1u;
            }
            if (!any) {
                continue;
            }
            if (y < lo) {
                lo = y;
            }
            hi = y + 1u;
        }
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t off = (start + y * stride + (x >> 3)) & 0xFFFF;
            const unsigned bit = 7u - (x & 7u);
            uint8_t pix = 0;
            for (unsigned p = 0; p < 4; p++) {
                pix |= (uint8_t)(((vg->plane[p][off] >> bit) & 1u) << p);
            }
            const uint32_t v = rgb[pix];
            for (unsigned s = 0; s < xscale; s++) {
                const uint32_t px = x * xscale + s;
                if (px < fb_w) {
                    fb[y * fb_w + px] = v;
                }
            }
        }
    }
    if (hi > lo) {
        *y0 = lo;
        *y1 = hi;
    }
    return true;
}

bool vga_blit(const Vga *vg, uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
              uint32_t w, uint32_t h, unsigned xscale, uint32_t *out_w,
              uint32_t *out_h) {
    uint32_t y0, y1;
    return vga_draw(vg, fb, fb_w, fb_h, w, h, xscale, out_w, out_h, NULL, &y0, &y1);
}

bool vga_blit_ex(Vga *vg, uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
                 uint32_t w, uint32_t h, unsigned xscale, uint32_t *out_w,
                 uint32_t *out_h, bool full, uint32_t *y0, uint32_t *y1) {
    uint64_t chunks[VGA_PLANE_DIRTY_WORDS];
    memcpy(chunks, vg->plane_dirty, sizeof chunks);
    memset(vg->plane_dirty, 0, sizeof vg->plane_dirty);
    return vga_draw(vg, fb, fb_w, fb_h, w, h, xscale, out_w, out_h,
                    full ? NULL : chunks, y0, y1);
}
