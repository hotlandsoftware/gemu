/* Rage 128 2D drawing engine (GUI), registers 0x1400-0x1FFF.
 *
 * Sources: RAGE 128 VR/GL Register Reference Guide chapter 7 ("RRG": the
 * registers and fields), the RAGE PRO and Derivatives Programmer's Guide
 * section 6.2.3 ("PRG": the engine's trajectories, which the Rage 128 keeps;
 * the RRG defers to the mach64 guide), and the conventions of the X.org r128
 * driver (DST_WIDTH_HEIGHT = width << 16 | height; a right-to-left or
 * bottom-to-top blit starts at the last column/row; scissor maximum 0x1FFF).
 *
 * Modelled:
 *   - trajectories (PRG 6.2.3.1/6.2.3.2): a rectangle, started by a write of
 *     a width register (DST_WIDTH, DST_HEIGHT_WIDTH and the other combined
 *     width registers), walked from (DST_X, DST_Y) by DP_CNTL DST_X_DIR /
 *     DST_Y_DIR; a Bresenham line, started by DST_BRES_LNTH;
 *   - sources (DP_MIX DP_SRC_SOURCE): 2 = memory, walked in the same
 *     directions from (SRC_X, SRC_Y); 3/4 = HOST_DATA, packed or with rows
 *     byte-aligned; mono sources expand to DP_SRC_FRGD_CLR / BKGD_CLR, or
 *     leave the pixel alone;
 *   - brushes (DP_BRUSH_DATATYPE): solid, 8x8 mono (both expansions), 8x8
 *     colour, aligned to BRUSH_Y_X;
 *   - DP_ROP3, DP_WRITE_MSK, the destination scissors (inclusive), the
 *     composite registers and DP_GUI_MASTER_CNTL's defaults (RRG 7.5);
 *   - status: each operation completes at once; GUI_STAT shows an idle
 *     engine with 64 free FIFO entries.
 *   - source/destination colour keys; RGB CCE scaling and additive alpha blending.
 * Reported when used: other datatypes (24 bpp, YUV, 3D), the
 * auxiliary scissors, trapezoids, sub-pixel coordinates, tiling, the CCE
 * custom microcode and the 3D registers (0x1800 up). */
#include "rage128_internal.h"

#include <stdio.h>
#include <string.h>

#define R(off) (r->regs[(off) / 4])

enum {
    DST_OFFSET = 0x1404, DST_PITCH = 0x1408, DST_WIDTH = 0x140C, DST_HEIGHT = 0x1410,
    SRC_X = 0x1414, SRC_Y = 0x1418, DST_X = 0x141C, DST_Y = 0x1420,
    SRC_PITCH_OFFSET = 0x1428, DST_PITCH_OFFSET = 0x142C, SRC_Y_X = 0x1434,
    DST_Y_X = 0x1438, DST_HEIGHT_WIDTH = 0x143C, DP_GUI_MASTER_CNTL = 0x146C,
    BRUSH_Y_X = 0x1474, DP_BRUSH_BKGD_CLR = 0x1478, DP_BRUSH_FRGD_CLR = 0x147C,
    BRUSH_DATA0 = 0x1480, DST_WIDTH_X = 0x1588, DST_HEIGHT_WIDTH_8 = 0x158C,
    SRC_X_Y = 0x1590, DST_X_Y = 0x1594, DST_WIDTH_HEIGHT = 0x1598,
    DST_WIDTH_X_INCY = 0x159C, DST_HEIGHT_Y = 0x15A0, DST_X_SUB = 0x15A4,
    DST_Y_SUB = 0x15A8, SRC_OFFSET = 0x15AC, SRC_PITCH = 0x15B0,
    DST_HEIGHT_WIDTH_BW = 0x15B4, CLR_CMP_CNTL = 0x15C0,
    CLR_CMP_CLR_SRC = 0x15C4, CLR_CMP_CLR_DST = 0x15C8, CLR_CMP_MSK = 0x15CC,
    DP_SRC_FRGD_CLR = 0x15D8, DP_SRC_BKGD_CLR = 0x15DC, DST_BRES_ERR = 0x1628,
    DST_BRES_INC = 0x162C, DST_BRES_DEC = 0x1630, DST_BRES_LNTH = 0x1634,
    SC_LEFT = 0x1640, SC_RIGHT = 0x1644, SC_TOP = 0x1648, SC_BOTTOM = 0x164C,
    SRC_SC_RIGHT = 0x1654, SRC_SC_BOTTOM = 0x165C, AUX_SC_CNTL = 0x1660,
    DP_CNTL = 0x16C0, DP_DATATYPE = 0x16C4, DP_MIX = 0x16C8, DP_WRITE_MSK = 0x16CC,
    DP_CNTL_XDIR_YDIR_YMAJOR = 0x16D0, DEFAULT_OFFSET = 0x16E0, DEFAULT_PITCH = 0x16E4,
    DEFAULT_SC_BOTTOM_RIGHT = 0x16E8, SC_TOP_LEFT = 0x16EC, SC_BOTTOM_RIGHT = 0x16F0,
    SRC_SC_BOTTOM_RIGHT = 0x16F4, WAIT_UNTIL = 0x1720, GUI_STAT = 0x1740,
    PC_GUI_MODE = 0x1744, PC_GUI_CTLSTAT = 0x1748, HOST_DATA0 = 0x17C0,
    HOST_DATA_LAST = 0x17E0,
};

static void unknown(const char *key, const char *what) {
    char k[48];
    snprintf(k, sizeof k, "gui-%s", key);
    r128_unimpl(k, "2D engine: %s not modelled", what);
}

/* A value of a numbered kind, reported under "<key><n>". */
static void unknown_n(const char *key, unsigned n, const char *what) {
    char k[32], w[64];
    snprintf(k, sizeof k, "%s%u", key, n);
    snprintf(w, sizeof w, "%s %u", what, n);
    unknown(k, w);
}

/* Signed 14-bit coordinate fields (RRG: range -8192 to 8191). */
static int s14(uint32_t v) {
    return (int)((v & 0x3FFFu) ^ 0x2000u) - 0x2000;
}

/* Bytes per pixel of a DP_DST_DATATYPE, 0 if not modelled. */
static unsigned datatype_bpp(unsigned t) {
    switch (t) {
    case 2: case 7: case 8: case 9:  /* 8 bpp pseudo-colour, RGB332, Y8, RGB8 */
        return 1;
    case 3: case 4: case 15:         /* 1555, 565, 4444 */
        return 2;
    case 6: case 14:                 /* aRGB 8888, aYUV 8888 */
        return 4;
    default:
        return 0;
    }
}

/* Keep signed surface coordinates through address calculation. In particular,
 * -4 must not become UINT32_MAX-3 and wrap a 32-bit end-address check to zero. */
static bool valid_pixel(const Rage128 *r, int64_t a, unsigned bpp) {
    return a >= 0 && (uint64_t)a <= r128_vram_bytes(r) &&
           bpp <= r128_vram_bytes(r) - (uint64_t)a;
}

static uint32_t rd_px(const Rage128 *r, int64_t a, unsigned bpp) {
    if (!valid_pixel(r, a, bpp)) {
        return 0;
    }
    uint32_t v = 0;
    for (unsigned k = 0; k < bpp; k++) {
        v |= (uint32_t)r->vga->vram[a + k] << (8 * k);
    }
    return v;
}

static void wr_px(Rage128 *r, int64_t a, unsigned bpp, uint32_t v) {
    if (!valid_pixel(r, a, bpp)) {
        return;
    }
    for (unsigned k = 0; k < bpp; k++) {
        r->vga->vram[a + k] = (uint8_t)(v >> (8 * k));
    }
    vga_vram_written(r->vga, a, bpp);
}

static uint32_t rop3(unsigned rop, uint32_t p, uint32_t s, uint32_t d) {
    uint32_t out = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (rop & (1u << i)) {
            out |= ((i & 4) ? p : ~p) & ((i & 2) ? s : ~s) & ((i & 1) ? d : ~d);
        }
    }
    return out;
}

/* ---- the current operation's parameters ---------------------------------- */

typedef struct {
    unsigned bpp, dst_type, brush_type, src_type, src_source, rop;
    uint32_t dst_base, dst_pitch;     /* bytes */
    uint32_t src_base, src_pitch;
    uint32_t mask;
    bool lsb_first;                   /* DP_BYTE_PIX_ORDER */
    int sc_l, sc_r, sc_t, sc_b;
} Op;

static bool setup(Rage128 *r, Op *o) {
    const uint32_t dt = R(DP_DATATYPE), mix = R(DP_MIX);
    o->dst_type = dt & 15u;
    o->brush_type = (dt >> 8) & 15u;
    o->src_type = (dt >> 16) & 3u;
    o->lsb_first = dt & (1u << 30);
    o->src_source = (mix >> 8) & 7u;
    o->rop = (mix >> 16) & 0xFFu;
    o->bpp = datatype_bpp(o->dst_type);
    if (!o->bpp) {
        unknown_n("datatype-", o->dst_type, "destination datatype");
        return false;
    }
    if (dt & (1u << 29)) {
        unknown("host-big-endian", "HOST_BIG_ENDIAN_EN");
    }
    if ((R(DST_PITCH) | R(SRC_PITCH)) & (1u << 16)) {
        unknown("tile", "tiled surfaces");
    }
    if ((R(DST_PITCH) >> 17) & 3u) {
        unknown("pitch-adj", "DST_PITCH_ADJ");
    }
    if (R(AUX_SC_CNTL) & 0x15u) {
        unknown("aux-sc", "auxiliary scissors");
    }
    o->dst_base = R(DST_OFFSET) & 0x03FFFFF0u;
    o->dst_pitch = (R(DST_PITCH) & 0x3FFu) * 8 * o->bpp;
    o->src_base = R(SRC_OFFSET) & 0x03FFFFF0u;
    o->src_pitch = (R(SRC_PITCH) & 0x3FFu) * 8 * o->bpp;
    o->mask = R(DP_WRITE_MSK);
    o->sc_l = s14(R(SC_LEFT));
    o->sc_r = s14(R(SC_RIGHT));
    o->sc_t = s14(R(SC_TOP));
    o->sc_b = s14(R(SC_BOTTOM));
    return true;
}

/* The brush pixel at destination (x, y); *skip for a transparent mono 0. */
static uint32_t brush(Rage128 *r, const Op *o, int x, int y, bool *skip) {
    *skip = false;
    const uint32_t org = R(BRUSH_Y_X);
    const int bx = (x - (int)(org & 0x1Fu)) & 7, by = (y - (int)((org >> 8) & 0x1Fu)) & 7;
    switch (o->brush_type) {
    case 13: case 15:
        return R(DP_BRUSH_FRGD_CLR);
    case 0: case 1: {
        /* 8x8 mono: BRUSH_DATA0-1 (RRG 7.4), a byte per row, bit order as
         * DP_BYTE_PIX_ORDER. */
        const uint8_t row = (uint8_t)(r->regs[BRUSH_DATA0 / 4 + by / 4] >> (8 * (by & 3)));
        const bool bit = o->lsb_first ? (row >> bx) & 1u : (row >> (7 - bx)) & 1u;
        if (bit) {
            return R(DP_BRUSH_FRGD_CLR);
        }
        if (o->brush_type == 1) {
            *skip = true;
        }
        return R(DP_BRUSH_BKGD_CLR);
    }
    case 10: {
        /* 8x8 colour: 64 pixels of the destination type in BRUSH_DATA. */
        const unsigned i = (unsigned)(by * 8 + bx) * o->bpp;
        uint32_t v = 0;
        for (unsigned k = 0; k < o->bpp; k++) {
            v |= ((r->regs[BRUSH_DATA0 / 4 + (i + k) / 4] >> (8 * ((i + k) & 3))) & 0xFFu)
                 << (8 * k);
        }
        return v;
    }
    default:
        unknown_n("brush-", o->brush_type, "brush datatype");
        return R(DP_BRUSH_FRGD_CLR);
    }
}

static bool rop_uses_pattern(unsigned rop) {
    return ((rop >> 4) ^ rop) & 0x0Fu;
}

static bool rop_uses_source(unsigned rop) {
    return ((rop >> 2) ^ rop) & 0x33u;
}

static bool compare(unsigned fn, uint32_t value, uint32_t ref, uint32_t mask) {
    switch (fn) {
    case 0: return true;
    case 1: return false;
    case 4: return (value & mask) == (ref & mask);
    case 5: return (value & mask) != (ref & mask);
    default: return true;
    }
}

/* Draw one pixel with source value s (valid unless src_skip). */
static void pixel(Rage128 *r, const Op *o, int x, int y, uint32_t s, bool src_skip) {
    if (x < o->sc_l || x > o->sc_r || y < o->sc_t || y > o->sc_b || x < 0 || y < 0) {
        return;
    }
    if (src_skip) {
        return;
    }
    bool pskip = false;
    const uint32_t p = rop_uses_pattern(o->rop) ? brush(r, o, x, y, &pskip) : 0;
    if (pskip) {
        return;
    }
    const int64_t a = o->dst_base + (int64_t)y * o->dst_pitch + (int64_t)x * o->bpp;
    const uint32_t d = rd_px(r, a, o->bpp);
    const uint32_t cmp = R(CLR_CMP_CNTL), mask = R(CLR_CMP_MSK);
    unsigned select = (cmp >> 24) & 3;
    unsigned dst_fn = (cmp >> 8) & 7;
    if (dst_fn == 4 || dst_fn == 5) dst_fn ^= 1; /* destination match preserves */
    if ((select == 0 || select == 2) &&
        !compare(dst_fn, d, R(CLR_CMP_CLR_DST), mask)) return;
    if ((select == 1 || select == 2) &&
        !compare(cmp & 7, s, R(CLR_CMP_CLR_SRC), mask)) return;
    if ((select == 1 || select == 2) && (cmp & 7) == 7 &&
        (s & mask) == (R(CLR_CMP_CLR_SRC) & mask)) s ^= R(DP_SRC_FRGD_CLR);
    uint32_t v = rop3(o->rop, p, s, d);
    v = (d & ~o->mask) | (v & o->mask);
    wr_px(r, a, o->bpp, v);
}

static uint32_t unpack_rgb(uint32_t v, unsigned type) {
    if (type == 6) return v;
    unsigned red = (v >> (type == 4 ? 11 : 10)) & 31;
    unsigned green = (v >> 5) & (type == 4 ? 63 : 31), blue = v & 31;
    unsigned a = type == 4 || (v & 0x8000) ? 255 : 0;
    red = (red << 3) | (red >> 2); blue = (blue << 3) | (blue >> 2);
    green = type == 4 ? (green << 2) | (green >> 4) : (green << 3) | (green >> 2);
    return a << 24 | red << 16 | green << 8 | blue;
}

static uint32_t pack_rgb(uint32_t v, unsigned type) {
    if (type == 6) return v;
    return type == 4 ? ((v >> 8) & 0xf800) | ((v >> 5) & 0x7e0) | ((v >> 3) & 31) :
           ((v >> 16) & 0x8000) | ((v >> 9) & 0x7c00) | ((v >> 6) & 0x3e0) | ((v >> 3) & 31);
}

static unsigned blend_factor(unsigned factor, uint32_t src, uint32_t dst, unsigned ch) {
    switch (factor) {
    case 0: return 0;
    case 1: return 255;
    case 2: return (src >> ch) & 255;
    case 3: return 255 - ((src >> ch) & 255);
    case 4: return src >> 24;
    case 5: return 255 - (src >> 24);
    case 6: return dst >> 24;
    case 7: return 255 - (dst >> 24);
    case 8: return (dst >> ch) & 255;
    case 9: return 255 - ((dst >> ch) & 255);
    default: return 0;
    }
}

/* SDK-G04000 F.13: RGB nearest-sample scaling and additive alpha blending.
 * Other texture-combine, fog and tiled operations remain explicit limits. */
bool r128_gui_scale(Rage128 *r, const uint32_t *p, bool apply) {
    unsigned type = p[3] & 15, sf = (p[0] >> 16) & 15, df = (p[0] >> 20) & 15;
    if ((type != 3 && type != 4 && type != 6) || (p[2] & 0x3c00f) ||
        (p[1] & ((1u << 7) | (1u << 10))) || (p[4] & 0xc0000000) ||
        (p[5] & 0x1e0000) || (p[5] >> 30) == 3 ||
        ((p[1] & (1u << 9)) && (sf > 9 || df > 9 || (p[0] & 0x3000))))
        return false;
    if (!apply) return true;
    Op o;
    if (!setup(r, &o) || (o.dst_type != 3 && o.dst_type != 4 && o.dst_type != 6))
        return false;
    unsigned bpp = datatype_bpp(type), pitch = ((p[5] & 511) * 8) << (p[5] >> 30);
    int x0 = s14(p[9] >> 16), y0 = s14(p[9]);
    unsigned w = p[10] & 0x3fff, h = (p[10] >> 16) & 0x3fff;
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
        int dx = x0 + (int)x, dy = y0 + (int)y;
        if (dx < 0 || dy < 0 || dx < o.sc_l || dx > o.sc_r || dy < o.sc_t || dy > o.sc_b)
            continue;
        uint64_t sx = ((uint64_t)x * (p[7] & 0xffff0)) >> 16;
        uint64_t sy = ((uint64_t)y * (p[8] & 0xffff0)) >> 16;
        uint64_t sa = (p[4] & 0x3ffffff) + (sy * pitch + sx) * bpp;
        if (sa + bpp > r128_vram_bytes(r)) continue;
        uint32_t color = unpack_rgb(rd_px(r, (uint32_t)sa, bpp), type);
        if (p[1] & (1u << 9)) {
            int64_t da = o.dst_base + (int64_t)dy * o.dst_pitch + (int64_t)dx * o.bpp;
            uint32_t dst = unpack_rgb(rd_px(r, da, o.bpp), o.dst_type), out = 0;
            for (unsigned ch = 0; ch < 32; ch += 8) {
                unsigned v = (((color >> ch) & 255) * blend_factor(sf, color, dst, ch) +
                              ((dst >> ch) & 255) * blend_factor(df, color, dst, ch) + 127) / 255;
                out |= (v > 255 ? 255 : v) << ch;
            }
            color = out;
        }
        pixel(r, &o, dx, dy, pack_rgb(color, o.dst_type), false);
    }
    return true;
}

/* A mono source bit expanded (DP_SRC_DATATYPE 0: frgd/bkgd, 1: frgd/leave). */
static uint32_t mono(Rage128 *r, const Op *o, bool bit, bool *skip) {
    *skip = !bit && o->src_type == 1;
    return bit ? R(DP_SRC_FRGD_CLR) : R(DP_SRC_BKGD_CLR);
}

static void rectangle(Rage128 *r, int w, int h) {
    Op o;
    if (!setup(r, &o)) {
        return;
    }
    const int x0 = s14(R(DST_X));
    int y = s14(R(DST_Y));
    const int xd = (R(DP_CNTL) & 1u) ? 1 : -1, yd = (R(DP_CNTL) & 2u) ? 1 : -1;
    if (w <= 0 || h <= 0) {
        return;
    }
    const bool need_src = rop_uses_source(o.rop);
    if (need_src && (o.src_source == 3 || o.src_source == 4)) {
        R128HostOp *hp = &r->host;
        if (hp->active) {
            unknown("host-restart", "a draw started while host data is still expected");
        }
        *hp = (R128HostOp){ .active = true, .x0 = x0, .x = x0, .y = y, .w = w, .h = h,
                            .xdir = xd, .ydir = yd, .left = w,
                            .byte_aligned = o.src_source == 4 };
        return;
    }
    if (need_src && o.src_source != 2) {
        unknown_n("src-source-", o.src_source, "DP_SRC_SOURCE");
        return;
    }
    const int sx0 = s14(R(SRC_X));
    int sy = s14(R(SRC_Y));
    for (int j = 0; j < h; j++, y += yd, sy += yd) {
        int x = x0, sx = sx0;
        for (int i = 0; i < w; i++, x += xd, sx += xd) {
            uint32_t s = 0;
            bool skip = false;
            if (need_src) {
                if (o.src_type == 3) {
                    s = rd_px(r, o.src_base + (int64_t)sy * o.src_pitch + (int64_t)sx * o.bpp,
                              o.bpp);
                } else {
                    /* A mono source in memory: a bit per pixel, pitch in
                     * pixels x 8 (RRG: a multiple of 64 pixels). */
                    const uint32_t bpitch = (R(SRC_PITCH) & 0x3FFu) * 8 / 8;
                    const uint8_t byte = rd_px(r, o.src_base + (int64_t)sy * bpitch +
                                                 (sx - (sx & 7)) / 8, 1);
                    const bool bit = o.lsb_first ? (byte >> (sx & 7)) & 1u
                                                 : (byte >> (7 - (sx & 7))) & 1u;
                    s = mono(r, &o, bit, &skip);
                }
            }
            pixel(r, &o, x, y, s, skip);
        }
    }
}

/* HOST_DATA: 32 bits of source for the waiting draw. Mono data is taken a
 * byte at a time in memory order (the register's low byte first), each byte's
 * bits in DP_BYTE_PIX_ORDER; colour data is packed pixels, low byte first. */
static void host_data(Rage128 *r, uint32_t v, bool last) {
    R128HostOp *hp = &r->host;
    if (!hp->active) {
        unknown("host-idle", "HOST_DATA with no draw waiting for it");
        return;
    }
    Op o;
    if (!setup(r, &o)) {
        hp->active = false;
        return;
    }
    if (o.src_type != 3) {
        for (unsigned b = 0; b < 32 && hp->active; b++) {
            if (hp->skip) {  /* the rest of a byte-aligned row's last byte */
                hp->skip--;
                continue;
            }
            const unsigned byte = b / 8, bit = b % 8;
            const uint8_t by = (uint8_t)(v >> (8 * byte));
            const bool set = o.lsb_first ? (by >> bit) & 1u : (by >> (7 - bit)) & 1u;
            bool skip;
            const uint32_t s = mono(r, &o, set, &skip);
            pixel(r, &o, hp->x, hp->y, s, skip);
            hp->x += hp->xdir;
            hp->bits++;
            if (--hp->left == 0) {
                hp->left = hp->w;
                hp->x = hp->x0;
                hp->y += hp->ydir;
                if (--hp->h == 0) {
                    hp->active = false;
                }
                if (hp->byte_aligned && (hp->bits & 7u)) {
                    hp->skip = 8 - (hp->bits & 7u);
                }
                hp->bits = 0;
            }
        }
    } else {
        for (unsigned k = 0; k + o.bpp <= 4 && hp->active; k += o.bpp) {
            const uint32_t s = (v >> (8 * k)) &
                               (o.bpp == 4 ? 0xFFFFFFFFu : (1u << (8 * o.bpp)) - 1);
            pixel(r, &o, hp->x, hp->y, s, false);
            hp->x += hp->xdir;
            if (--hp->left == 0) {
                hp->left = hp->w;
                hp->x = hp->x0;
                hp->y += hp->ydir;
                if (--hp->h == 0) {
                    hp->active = false;
                }
            }
        }
    }
    if (last && hp->active) {
        hp->active = false;  /* HOST_DATA_LAST ends it */
    }
}

/* PRG 6.2.3.2 */
static void line(Rage128 *r, int len) {
    Op o;
    if (!setup(r, &o)) {
        return;
    }
    if (rop_uses_source(o.rop)) {
        unknown("line-src", "a line with a source");
        return;
    }
    const uint32_t cntl = R(DP_CNTL);
    int x = s14(R(DST_X)), y = s14(R(DST_Y));
    int err = (int)(R(DST_BRES_ERR) << 12) >> 12;
    const int inc = (int)(R(DST_BRES_INC) << 12) >> 12;
    const int dec = (int)(R(DST_BRES_DEC) << 12) >> 12;
    const bool xdir = cntl & 1u, ydir = cntl & 2u, ymajor = cntl & 4u;
    const bool last = cntl & 0x20u, sign = cntl & 0x100u;
    for (int i = 0; i < len; i++) {
        pixel(r, &o, x, y, 0, false);
        if (ymajor) {
            y += ydir ? 1 : -1;
        } else {
            x += xdir ? 1 : -1;
        }
        if (err < 0 || (sign && err == 0)) {
            err += inc;
        } else {
            err += dec;
            if (ymajor) {
                x += xdir ? 1 : -1;
            } else {
                y += ydir ? 1 : -1;
            }
        }
    }
    if (last) {
        pixel(r, &o, x, y, 0, false);
    }
}

/* RRG 7.5 DP_GUI_MASTER_CNTL: fields mapped into DP_DATATYPE / DP_MIX, and
 * defaults applied unless the "leave alone" bits are set. */
static void master_cntl(Rage128 *r, uint32_t v) {
    if (!(v & 1u)) {
        R(SRC_OFFSET) = R(DEFAULT_OFFSET);
        R(SRC_PITCH) = R(DEFAULT_PITCH);
    }
    if (!(v & 2u)) {
        R(DST_OFFSET) = R(DEFAULT_OFFSET);
        R(DST_PITCH) = R(DEFAULT_PITCH);
    }
    const uint32_t dsc = R(DEFAULT_SC_BOTTOM_RIGHT);
    if (!(v & 4u)) {
        R(SRC_SC_RIGHT) = dsc & 0x3FFFu;
        R(SRC_SC_BOTTOM) = (dsc >> 16) & 0x3FFFu;
    }
    if (!(v & 8u)) {
        R(SC_LEFT) = R(SC_TOP) = 0;
        R(SC_RIGHT) = dsc & 0x3FFFu;
        R(SC_BOTTOM) = (dsc >> 16) & 0x3FFFu;
    }
    unsigned src_type = (v >> 12) & 3u;
    if (src_type == 2) {
        src_type = 3;  /* "2 = color (pixel type same as DST)=3" */
    }
    R(DP_DATATYPE) = (R(DP_DATATYPE) & 0x20000000u) | ((v >> 4) & 15u) << 8 |
                     ((v >> 8) & 15u) | src_type << 16 | ((v >> 14) & 1u) << 30 |
                     ((v >> 15) & 1u) << 31;
    R(DP_MIX) = ((v >> 16) & 0xFFu) << 16 | ((v >> 24) & 7u) << 8;
    if (v & (1u << 28)) {
        R(CLR_CMP_CNTL) &= ~0x707u;
    }
    if (v & (1u << 29)) {
        R(AUX_SC_CNTL) &= ~0x15u;
    }
    if (v & (1u << 30)) {
        R(DP_WRITE_MSK) = R(CLR_CMP_MSK) = 0xFFFFFFFFu;
    }
    /* DP_CNTL: DST_X_DIR, DST_Y_DIR and POLY_LINE are set to 1 by a
     * GUI_MASTER_CNTL write. */
    R(DP_CNTL) |= 0x8003u;
}

bool r128_gui_read(Rage128 *r, uint32_t off, uint32_t *v) {
    if (off < 0x1400 || off >= 0x2000) {
        return false;
    }
    switch (off) {
    case GUI_STAT:
        *v = 0x40;                    /* GUI_FIFOCNT 64 free, nothing busy */
        return true;
    case PC_GUI_CTLSTAT:
        *v = R(off) & ~3u;            /* flushes complete at once */
        return true;
    case DST_WIDTH:
        *v = R(off) & 0x7FFFu;        /* bit 15 is write only */
        return true;
    default:
        *v = R(off);
        return true;
    }
}

bool r128_gui_write(Rage128 *r, uint32_t off, uint32_t v, uint32_t mask) {
    if (off < 0x1400 || off >= 0x2000) {
        return false;
    }
    if (mask != 0xFFFFFFFFu) {
        unknown("partial", "a partial-width register write (\"Access: 32\")");
        return true;
    }
    switch (off) {
    case DST_PITCH_OFFSET:
        R(DST_OFFSET) = (v & 0x1FFFFFu) << 5;  /* 32-byte units */
        R(DST_PITCH) = (R(DST_PITCH) & ~0x103FFu) | ((v >> 21) & 0x3FFu) | ((v >> 31) << 16);
        return true;
    case SRC_PITCH_OFFSET:
        R(SRC_OFFSET) = (v & 0x1FFFFFu) << 5;
        R(SRC_PITCH) = (R(SRC_PITCH) & ~0x103FFu) | ((v >> 21) & 0x3FFu) | ((v >> 31) << 16);
        return true;
    case DST_Y_X:
        R(DST_X) = v & 0x3FFFu;
        R(DST_Y) = (v >> 16) & 0x3FFFu;
        return true;
    case DST_X_Y:
        R(DST_Y) = v & 0x3FFFu;
        R(DST_X) = (v >> 16) & 0x3FFFu;
        return true;
    case SRC_Y_X:
        R(SRC_X) = v & 0x3FFFu;
        R(SRC_Y) = (v >> 16) & 0x3FFFu;
        return true;
    case SRC_X_Y:
        R(SRC_Y) = v & 0x3FFFu;
        R(SRC_X) = (v >> 16) & 0x3FFFu;
        return true;
    case DST_X_SUB:
    case DST_Y_SUB:
        R(off) = v & 0x3FFFFu;
        if (v & 0xFu) {
            unknown("subpixel", "sub-pixel coordinates");
        }
        R(off == DST_X_SUB ? DST_X : DST_Y) = (v >> 4) & 0x3FFFu;
        return true;
    case SC_TOP_LEFT:
        R(SC_LEFT) = v & 0x3FFFu;
        R(SC_TOP) = (v >> 16) & 0x3FFFu;
        return true;
    case SC_BOTTOM_RIGHT:
        R(SC_RIGHT) = v & 0x3FFFu;
        R(SC_BOTTOM) = (v >> 16) & 0x3FFFu;
        return true;
    case SRC_SC_BOTTOM_RIGHT:
        R(SRC_SC_RIGHT) = v & 0x3FFFu;
        R(SRC_SC_BOTTOM) = (v >> 16) & 0x3FFFu;
        return true;
    case DP_CNTL_XDIR_YDIR_YMAJOR:
        R(DP_CNTL) = (R(DP_CNTL) & ~7u) | (v >> 31 & 1u) | ((v >> 15) & 1u) << 1 | (v & 4u);
        R(off) = v;
        return true;
    case DP_GUI_MASTER_CNTL:
        R(off) = v;
        master_cntl(r, v);
        return true;
    case DST_WIDTH:
        R(off) = v & 0x3FFFu;
        rectangle(r, (int)(v & 0x1FFFu), (int)(R(DST_HEIGHT) & 0x1FFFu));
        return true;
    case DST_HEIGHT_WIDTH:
    case DST_HEIGHT_WIDTH_BW:
        R(DST_WIDTH) = v & 0x3FFFu;
        R(DST_HEIGHT) = (v >> 16) & 0x3FFFu;
        rectangle(r, (int)(v & 0x1FFFu), (int)((v >> 16) & 0x1FFFu));
        return true;
    case DST_WIDTH_HEIGHT:
        R(DST_HEIGHT) = v & 0x3FFFu;
        R(DST_WIDTH) = (v >> 16) & 0x3FFFu;
        rectangle(r, (int)((v >> 16) & 0x1FFFu), (int)(v & 0x1FFFu));
        return true;
    case DST_HEIGHT_WIDTH_8:
        R(DST_WIDTH) = (v >> 16) & 0xFFu;
        R(DST_HEIGHT) = (v >> 24) & 0xFFu;
        rectangle(r, (int)((v >> 16) & 0xFFu), (int)(v >> 24));
        return true;
    case DST_WIDTH_X:
        R(DST_X) = v & 0x3FFFu;
        R(DST_WIDTH) = (v >> 16) & 0x3FFFu;
        rectangle(r, (int)((v >> 16) & 0x1FFFu), (int)(R(DST_HEIGHT) & 0x1FFFu));
        return true;
    case DST_HEIGHT_Y:
        R(DST_Y) = v & 0x3FFFu;
        R(DST_HEIGHT) = (v >> 16) & 0x3FFFu;
        rectangle(r, (int)(R(DST_WIDTH) & 0x1FFFu), (int)((v >> 16) & 0x1FFFu));
        return true;
    case DST_WIDTH_X_INCY:
        unknown("width-x-incy", "DST_WIDTH_X_INCY");
        return true;
    case DST_BRES_LNTH:
        R(off) = v & 0x3FFFu;
        line(r, (int)(v & 0x3FFFu));
        return true;
    case HOST_DATA_LAST:
        host_data(r, v, true);
        return true;
    case WAIT_UNTIL:                  /* the engine is always idle */
    case PC_GUI_MODE:                 /* pixel cache mode: no cache modelled */
    case PC_GUI_CTLSTAT:              /* flushes complete at once */
        R(off) = v;
        return true;
    default:
        if (off >= HOST_DATA0 && off < HOST_DATA_LAST) {
            host_data(r, v, false);
            return true;
        }
        if ((off >= 0x1600 && off < 0x1628) || (off >= 0x1638 && off < 0x1640)) {
            unknown("trapezoid", "trapezoid registers");
            R(off) = v;
            return true;
        }
        if (off >= 0x1800) {
            char k[16], w[48];
            snprintf(k, sizeof k, "reg-%04x", off);
            snprintf(w, sizeof w, "register 0x%04X (3D / CCE)", off);
            unknown(k, w);
            R(off) = v;
            return true;
        }
        R(off) = v;                   /* trajectory/datapath parameters */
        return true;
    }
}

void r128_gui_reset(Rage128 *r) {
    memset(&r->host, 0, sizeof r->host);
}
