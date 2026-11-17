/* Rage 128 CCE/PM4 command transport. ATI SDK-G04000 (August 1999),
 * sections 2.6.6, 5.2-5.3 and appendix F. The DMA read pointer describes
 * words fetched into the on-chip FIFO, not commands already executed.
 * In particular, Windows primes this FIFO before starting the microengine.
 *
 * The standard microcode's packets are interpreted here, ready at reset.
 * (Windows' IA64 ATI driver enables CCE via BUFFER_CNTL alone.) Microcode RAM is
 * retained for upload/readback, but arbitrary replacement microcode is not
 * executed. Unsupported packets stop the processor with a diagnostic. */
#include "rage128_internal.h"

#include <stdio.h>
#include <string.h>

#define R(off) (r->regs[(off) / 4])
enum {
    BUS_CNTL = 0x030, GEN_RESET_CNTL = 0x0f0, PCI_GART_PAGE = 0x17c,
    BUFFER_OFFSET = 0x700, BUFFER_CNTL = 0x704, BUFFER_WM_CNTL = 0x708,
    RPTR_ADDR = 0x70c, RPTR = 0x710, WPTR = 0x714,
    STAT = 0x7b8, MICRO_ADDR = 0x7d4, MICRO_RADDR = 0x7d8,
    MICRO_DATAH = 0x7dc, MICRO_DATAL = 0x7e0,
    BUFFER_ADDR = 0x7f0, MICRO_CNTL = 0x7fc,
    FIFO_EVEN = 0x1000, FIFO_ODD = 0x1004,
};

static unsigned fifo_size(const Rage128 *r) {
    unsigned mode = R(BUFFER_CNTL) >> 28;
    return mode == 1 || mode == 2 ? 192 :
           mode == 3 || mode == 4 ? 128 :
           (mode >= 5 && mode <= 8) || mode == 15 ? 64 : 0;
}

static bool bus_master(const Rage128 *r) {
    return (r->cfg[4] & 4) && !(R(BUS_CNTL) & (1u << 6));
}

static bool dma_word(const Rage128Dma *dma, uint64_t addr, uint32_t *v) {
    uint8_t b[4];
    if (!dma || !dma->read || !dma->read(dma->opaque, addr, b, 4))
        return false;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 |
         (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return true;
}

static bool fetch(Rage128 *r, const Rage128Dma *dma, uint32_t addr, uint32_t *v) {
    uint64_t pa = addr;
    if (!(R(PCI_GART_PAGE) & 1u)) {
        /* PCI cards expose 4 MiB through a 4 KiB-page scatter/gather table.
         * Entries contain addresses, without a present/valid flag. */
        uint32_t page;
        /* The engine's AGP-space alias starts at 32 MiB (SDK 5.2.3).
         * Windows also uses offsets relative to the start of GART space. */
        if ((addr & 0xfc000000u) == 0) addr &= ~0x02000000u;
        if (addr >= 0x400000u ||
            !dma_word(dma, (R(PCI_GART_PAGE) & ~0xfffu) +
                           ((addr >> 12) * 4u), &page))
            return false;
        pa = (page & ~0xfffu) | (addr & 0xfffu);
    }
    return dma_word(dma, pa, v);
}

static void writeback(Rage128 *r, const Rage128Dma *dma) {
    if ((R(BUFFER_CNTL) & (1u << 27)) || !dma || !dma->write)
        return;
    uint32_t v = R(RPTR);
    const uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24};
    dma->write(dma->opaque, R(RPTR_ADDR) & ~3u, b, sizeof b);
}

static void enqueue(Rage128 *r, uint32_t word) {
    Rage128Cce *c = &r->cce;
    c->fifo[(c->head + c->count) % 192] = word;
    c->count++;
}

/* Validate a whole packet before drawing any part of it. The second pass
 * writes the same register stream as the standard 2D microcode. */
typedef struct {
    Rage128 *r;
    const uint32_t *p;
    unsigned left;
    bool apply, ok;
} Packet;

static uint32_t take(Packet *p) {
    if (!p->left) { p->ok = false; return 0; }
    p->left--;
    return *p->p++;
}

static void wr(Packet *p, unsigned reg, uint32_t v) {
    if (p->apply && p->ok) r128_reg_write(p->r, reg, v, UINT32_MAX);
}

static void setting(Packet *p, unsigned reg) { wr(p, reg, take(p)); }

static bool settings(Packet *p) {
    uint32_t g = take(p);
    wr(p, 0x146c, g);
    if (g & 1) setting(p, 0x1428);  /* SRC_PITCH_OFFSET */
    if (g & 2) setting(p, 0x142c);  /* DST_PITCH_OFFSET */
    if (g & 4) setting(p, 0x16f4);  /* SRC_SC_BOTTOM_RIGHT */
    if (g & 8) { setting(p, 0x16ec); setting(p, 0x16f0); }
    unsigned brush = (g >> 4) & 15, words = 0;
    if (brush <= 9) {
        if (!(brush & 1)) setting(p, 0x1478);
        setting(p, 0x147c);
        words = brush <= 1 ? 2 : brush <= 7 ? 1 : 32;
    } else if (brush <= 12) {
        unsigned type = (g >> 8) & 15;
        unsigned bytes = type == 2 || type == 7 || type == 8 || type == 9 ? 1 :
                         type == 3 || type == 4 || type == 15 ? 2 : 4;
        words = (brush == 10 ? 16 : 2) * bytes;
    } else if (brush == 13) setting(p, 0x147c);
    else if (brush == 14) return false;
    for (unsigned i = 0; i < words; i++) setting(p, 0x1480 + i * 4);
    if (g & (1u << 31)) setting(p, 0x1474);
    return p->ok;
}

static int coord(uint32_t v) { return (int)((v & 0x3fff) ^ 0x2000) - 0x2000; }
static uint32_t xy(int x, int y) { return ((uint32_t)x & 0x3fff) << 16 | ((uint32_t)y & 0x3fff); }

static void bitmap(Packet *p, uint32_t pos, uint32_t size, unsigned words) {
    if (words > p->left) { p->ok = false; return; }
    wr(p, 0x1438, pos);
    wr(p, 0x143c, size);
    for (unsigned i = 0; i < words; i++)
        setting(p, i + 1 == words ? 0x17e0 : 0x17c0);
}

static bool packet3(Rage128 *r, unsigned op, const uint32_t *data,
                    unsigned count, bool apply) {
    Packet p = {r, data, count, apply, true};
    if ((op & 0x80) || op == 0x28) {
        if (!settings(&p)) return false;
    }
    switch (op) {
    case 0x1e: /* SET_SCISSORS */
        setting(&p, 0x16ec); setting(&p, 0x16f0);
        break;
    case 0x1f: /* SET_MODE_24BPP */
        if (apply) r->cce.mode24 = take(&p) & 1;
        else take(&p);
        break;
    case 0x91: /* PAINT: top/left and exclusive bottom/right */
        while (p.left && p.ok) {
            uint32_t tl = take(&p), br = take(&p);
            int x = coord(tl), y = coord(tl >> 16);
            int w = coord(br) - x, h = coord(br >> 16) - y;
            if (w > 0 && h > 0) {
                wr(&p, 0x1438, tl);
                wr(&p, 0x143c, (uint32_t)h << 16 | (unsigned)w);
            }
        }
        break;
    case 0x9a: /* PAINT_MULTI: x/y and width/height */
        while (p.left && p.ok) {
            setting(&p, 0x1594); setting(&p, 0x1598);
        }
        break;
    case 0x95: { /* POLYLINE, SDK F.12: vertices are Y:X, unlike BITBLT. */
        if (p.left < 2) return false;
        uint32_t start = take(&p);
        uint32_t last = R(0x16c0) & 0x20u;
        while (p.left && p.ok) {
            uint32_t end = take(&p);
            int x = coord(start), y = coord(start >> 16);
            int dx = coord(end) - x, dy = coord(end >> 16) - y;
            unsigned dir = (dx >= 0 ? 1u : 0u) | (dy >= 0 ? 2u : 0u);
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            bool ymajor = dy > dx;
            int major = ymajor ? dy : dx, minor = ymajor ? dx : dy;
            uint32_t cntl = (R(0x16c0) & ~0x8127u) | dir | (ymajor ? 4u : 0u);
            if (!(dir & (ymajor ? 1u : 2u))) cntl |= 0x100u;
            /* Shared vertices belong to the following segment. Only the
             * final endpoint is subject to the programmed LAST_PEL bit. */
            cntl |= p.left ? 0x8000u : last;
            wr(&p, 0x16c0, cntl);
            wr(&p, 0x1438, start);
            /* The GUI walker tests the error before applying the increment. */
            wr(&p, 0x1628, (uint32_t)(2 * minor - major));
            wr(&p, 0x162c, (uint32_t)(2 * minor));
            wr(&p, 0x1630, (uint32_t)(2 * (minor - major)));
            wr(&p, 0x1634, (unsigned)major);
            start = end;
        }
        break;
    }
    case 0x28: /* ATI XP's BITBLT alias: SETTINGS, SRC_X_Y, DST_X_Y, W_H.
                * ati2draa's builder at +0x1aea0 packs POINTL.x in bits 31:16. */
    case 0x9b: /* BITBLT_MULTI, choose traversal to preserve overlapping pixels */
        while (p.left && p.ok) {
            uint32_t src = take(&p), dst = take(&p), wh = take(&p);
            int sx = coord(src >> 16), sy = coord(src);
            int dx = coord(dst >> 16), dy = coord(dst);
            unsigned w = (wh >> 16) & 0x3fff, h = wh & 0x3fff, dir = 3;
            if (sx < dx) { sx += w ? w - 1 : 0; dx += w ? w - 1 : 0; dir &= ~1u; }
            if (sy < dy) { sy += h ? h - 1 : 0; dy += h ? h - 1 : 0; dir &= ~2u; }
            /* The composite register uses bits 31/15, unlike DP_CNTL's
             * bits 0/1. Encoding dir directly makes every copy run backwards. */
            wr(&p, 0x16d0, (dir & 1u) << 31 | ((dir >> 1) & 1u) << 15);
            wr(&p, 0x1590, xy(sx, sy)); wr(&p, 0x1594, xy(dx, dy));
            wr(&p, 0x1598, wh);
        }
        break;
    case 0x9c: { /* TRANS_BITBLT */
        setting(&p, 0x15c0); setting(&p, 0x15c4); setting(&p, 0x15c8);
        setting(&p, 0x1434); setting(&p, 0x1438); setting(&p, 0x143c);
        break;
    }
    case 0x96: /* SCALE */
        if (p.left != 11 || !r128_gui_scale(r, p.p, apply)) return false;
        p.left = 0;
        break;
    case 0x94: /* HOSTDATA_BLT */
        setting(&p, 0x15d8); setting(&p, 0x15dc);
        while (p.left && p.ok) {
            uint32_t pos = take(&p), size = take(&p), words = take(&p);
            bitmap(&p, pos, size, words);
        }
        break;
    case 0x19: { /* NEXTCHAR uses the preceding text settings */
        uint32_t pos = take(&p), size = take(&p);
        bitmap(&p, pos, size, p.left);
        break;
    }
    case 0x93: { /* SMALL_TEXT */
        setting(&p, 0x15d8);
        uint32_t base = take(&p);
        int x = coord(base), y = coord(base >> 16);
        while (p.left && p.ok) {
            uint32_t glyph = take(&p);
            x += (int8_t)glyph;
            int top = y - (int8_t)(glyph >> 8);
            unsigned w = (glyph >> 16) & 255, h = glyph >> 24;
            bitmap(&p, ((uint32_t)top & 0xffff) << 16 | ((uint32_t)x & 0xffff),
                   h << 16 | w, (w * h + 31) / 32);
        }
        break;
    }
    case 0x98: { /* POLYSCANLINES */
        unsigned scans = take(&p);
        while (scans-- && p.ok) {
            unsigned segments = take(&p);
            uint32_t ht = take(&p);
            while (segments-- && p.ok) {
                uint32_t ends = take(&p);
                int x = coord(ends), w = coord(ends >> 16) - x;
                if (w > 0) {
                    wr(&p, 0x1594, xy(x, coord(ht)));
                    wr(&p, 0x1598, (uint32_t)w << 16 | (ht >> 16));
                }
            }
        }
        break;
    }
    case 0x1d: { /* PLY_NEXTSCAN: one scanline, no segment-count field */
        uint32_t ht = take(&p);
        while (p.left && p.ok) {
            uint32_t ends = take(&p);
            int x = coord(ends), w = coord(ends >> 16) - x;
            if (w > 0) {
                wr(&p, 0x1594, xy(x, coord(ht)));
                wr(&p, 0x1598, (uint32_t)w << 16 | (ht >> 16));
            }
        }
        break;
    }
    default:
        return false;
    }
    return p.ok && !p.left;
}

static bool packet(Rage128 *r) {
    Rage128Cce *c = &r->cce;
    uint32_t h = c->packet[0];
    unsigned type = h >> 30, n = c->need - 1;
    if (type == 0) {
        unsigned reg = (h & 0x7ffu) * 4;
        for (unsigned i = 0; i < n; i++) {
            r128_reg_write(r, reg, c->packet[i + 1], UINT32_MAX);
            if (!(h & 0x8000u)) reg = (reg + 4) & 0x1fffu;
        }
        return true;
    }
    if (type == 1) {
        r128_reg_write(r, (h & 0x7ffu) * 4, c->packet[1], UINT32_MAX);
        r128_reg_write(r, ((h >> 11) & 0x7ffu) * 4, c->packet[2], UINT32_MAX);
        return true;
    }
    if (type == 2 || (type == 3 && ((h >> 8) & 255) == 0x10))
        return true;
    if (type == 3 && packet3(r, (h >> 8) & 255, c->packet + 1, n, false))
        return packet3(r, (h >> 8) & 255, c->packet + 1, n, true);
    char key[32];
    snprintf(key, sizeof key, "cce-packet-%02x", (h >> 8) & 255);
    r128_unimpl(key, "CCE packet 0x%08x (%u body words), first words %08x %08x %08x",
                h, n, c->packet[1], n > 1 ? c->packet[2] : 0,
                n > 2 ? c->packet[3] : 0);
    return false;
}

void rage128_cce_tick(Rage128 *r, const Rage128Dma *dma) {
    Rage128Cce *c = &r->cce;
    unsigned mode = R(BUFFER_CNTL) >> 28, size = fifo_size(r);
    bool bm = mode == 2 || mode == 4 || mode == 6 || mode == 8;
    if (!size) return;
    if (c->head >= 192 || c->count > 192 || c->have > 16385 ||
        c->need > 16385 || c->have > c->need) {
        r128_unimpl("cce-state", "invalid CCE FIFO/packet state; processor stopped");
        return;
    }
    /* Both the transport and interpreter are bounded, including when the
     * guest supplies a command stream that changes its own control state. */
    for (unsigned budget = 0; budget < 8192; budget++) {
        bool progress = false;
        if (bm && bus_master(r) && c->count < size) {
            unsigned exponent = R(BUFFER_CNTL) & 0x3fu;
            if (exponent > 19) return; /* maximum PCI GART ring: 4 MiB */
            uint32_t mask = (2u << exponent) - 1;
            uint32_t rd = R(RPTR) & mask, wr = R(WPTR) & mask;
            if (rd != wr) {
                uint32_t word;
                uint64_t addr = (uint64_t)R(BUFFER_OFFSET) + (uint64_t)rd * 4;
                if (addr > UINT32_MAX || !fetch(r, dma, (uint32_t)addr, &word))
                    return;
                enqueue(r, word);
                R(RPTR) = (rd + 1) & mask;
                writeback(r, dma);
                progress = true;
            }
        }
        if (R(MICRO_CNTL) & (1u << 30)) {
            if (c->have && c->have == c->need) {
                if (!packet(r)) return;
                c->have = c->need = 0;
                progress = true;
            }
            if (c->count) {
                uint32_t word = c->fifo[c->head];
                c->head = (c->head + 1) % 192;
                c->count--;
                if (!c->have) {
                    unsigned type = word >> 30;
                    c->need = type == 2 ? 1 : type == 1 ? 3 :
                              ((word >> 16) & 0x3fffu) + 2;
                }
                c->packet[c->have++] = word;
                progress = true;
            }
        }
        if (!progress) break;
    }
}

bool r128_cce_read(Rage128 *r, uint32_t off, uint32_t *v) {
    Rage128Cce *c = &r->cce;
    switch (off) {
    case STAT:
        *v = c->count <= fifo_size(r) ? fifo_size(r) - c->count : 0;
        if ((R(MICRO_CNTL) & (1u << 30)) && (c->count || c->have))
            *v |= (1u << 16) | (1u << 31);
        if (r->host.active) *v |= 1u << 31;
        return true;
    case MICRO_DATAH: case MICRO_DATAL:
        *v = c->microcode[R(MICRO_RADDR) & 255][off == MICRO_DATAH];
        return true;
    case PCI_GART_PAGE:
    case BUFFER_OFFSET: case BUFFER_CNTL: case BUFFER_WM_CNTL:
    case RPTR_ADDR: case RPTR: case WPTR:
    case MICRO_ADDR: case MICRO_RADDR: case BUFFER_ADDR: case MICRO_CNTL:
        *v = R(off);
        return true;
    default:
        return false;
    }
}

bool r128_cce_write(Rage128 *r, uint32_t off, uint32_t v, uint32_t mask) {
    Rage128Cce *c = &r->cce;
    uint32_t merged = (R(off) & ~mask) | (v & mask);
    switch (off) {
    case GEN_RESET_CNTL:
        if (v & mask & 1u) {
            c->head = c->count = c->have = c->need = 0;
            R(RPTR) = R(WPTR) = 0;
            r128_gui_reset(r);
        }
        return false; /* retain the other reset controls in the register file */
    case STAT:
        return true;
    case BUFFER_CNTL:
        if ((merged ^ R(off)) & 0xf0000000u)
            c->head = c->count = c->have = c->need = 0;
        break;
    case MICRO_DATAH: case MICRO_DATAL:
        c->microcode[R(MICRO_ADDR) & 255][off == MICRO_DATAH] = merged;
        if (off == MICRO_DATAL) R(MICRO_ADDR) = (R(MICRO_ADDR) + 1) & 255;
        break;
    case FIFO_EVEN: case FIFO_ODD:
        if (c->count < fifo_size(r)) enqueue(r, merged);
        else r128_unimpl("cce-fifo-full", "CCE PIO write to a full FIFO");
        return true;
    case PCI_GART_PAGE:
    case BUFFER_OFFSET: case BUFFER_WM_CNTL: case RPTR_ADDR: case RPTR: case WPTR:
    case MICRO_ADDR: case MICRO_RADDR: case BUFFER_ADDR: case MICRO_CNTL:
        break;
    default:
        return false;
    }
    R(off) = merged;
    return true;
}
