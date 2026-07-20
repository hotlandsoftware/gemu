/*
 * Standard IBM-compatible VGA: sequencer, CRTC, graphics controller,
 * attribute controller, DAC, and the classic planar VRAM address/write/read
 * logic (write modes 0-2, read modes 0-1, odd/even and chain-4 addressing).
 * This is generic PC hardware behavior - not machine- or CPU-specific -
 * so it's implemented as a standalone, reusable device.
 */
#include "vga_ibm.h"
#include <string.h>

void vga_ibm_reset(VgaIbm *v) {
    memset(v, 0, sizeof(*v));
    v->misc_output = 0x01;         /* color emulation, so 0x3Dx ports are live */
    v->crtc[0x17] = 0x80;
}

static void planar_addr(const VgaIbm *v, uint32_t addr, uint32_t *idx,
                        int *plane, bool *forced_plane) {
    if (v->seq[4] & 8) {                        /* chain-4 (mode 0x13) */
        *idx = addr >> 2;
        *plane = (int)(addr & 3);
        *forced_plane = true;
    } else if (!(v->seq[4] & 4)) {              /* odd/even (text modes) */
        *idx = addr >> 1;
        *plane = (int)(addr & 1);
        *forced_plane = true;
    } else {
        *idx = addr;
        *plane = 0;
        *forced_plane = false;
    }
}

uint8_t vga_ibm_mem_read(VgaIbm *v, uint32_t addr) {
    uint32_t idx;
    int plane;
    bool forced;
    planar_addr(v, addr, &idx, &plane, &forced);
    if (idx >= 0x10000) return 0xFF;

    for (int p = 0; p < 4; p++) v->latch[p] = v->vram[p][idx];

    if (forced)
        return v->latch[plane];

    if (v->gc[5] & 8) {                          /* read mode 1: color compare */
        uint8_t compare = v->gc[2] & 0xF, dont_care = v->gc[7] & 0xF;
        uint8_t result = 0xFF;
        for (int p = 0; p < 4; p++) {
            if (!(dont_care & (1u << p))) continue;
            uint8_t want = (compare >> p) & 1;
            result &= want ? v->latch[p] : (uint8_t)~v->latch[p];
        }
        return result;
    }
    return v->latch[v->gc[4] & 3];
}

void vga_ibm_mem_write(VgaIbm *v, uint32_t addr, uint8_t val) {
    uint32_t idx;
    int plane;
    bool forced;
    planar_addr(v, addr, &idx, &plane, &forced);
    if (idx >= 0x10000) return;

    unsigned write_mode = v->gc[5] & 3;
    unsigned rotate = v->gc[3] & 7;
    unsigned func = (v->gc[3] >> 3) & 3;
    uint8_t bitmask = v->gc[8];
    uint8_t set_reset = v->gc[0];
    uint8_t enable_sr = v->gc[1];
    unsigned map_mask = forced ? (1u << plane) : (v->seq[2] & 0xF);

    for (int p = 0; p < 4; p++) {
        if (!(map_mask & (1u << p))) continue;
        uint8_t data;
        if (write_mode == 1) {
            data = v->latch[p];
        } else if (write_mode == 2) {
            uint8_t src = (val & (1u << p)) ? 0xFF : 0x00;
            switch (func) {
            case 0: data = src; break;
            case 1: data = src & v->latch[p]; break;
            case 2: data = src | v->latch[p]; break;
            default: data = src ^ v->latch[p]; break;
            }
            data = (data & bitmask) | (v->latch[p] & (uint8_t)~bitmask);
        } else {
            uint8_t d = val;
            if (rotate) d = (uint8_t)((d >> rotate) | (d << (8 - rotate)));
            uint8_t sr_bit = (set_reset >> p) & 1;
            uint8_t src = (enable_sr & (1u << p)) ? (sr_bit ? 0xFF : 0x00) : d;
            switch (func) {
            case 0: data = src; break;
            case 1: data = src & v->latch[p]; break;
            case 2: data = src | v->latch[p]; break;
            default: data = src ^ v->latch[p]; break;
            }
            if (write_mode == 0)
                data = (data & bitmask) | (v->latch[p] & (uint8_t)~bitmask);
        }
        v->vram[p][idx] = data;
    }
}

uint8_t vga_ibm_io_read(VgaIbm *v, uint16_t port) {
    switch (port) {
    case 0x3C0: return v->attr_index;
    case 0x3C1: return v->attr[v->attr_index & 0x1F];
    case 0x3C2: return 0;                        /* input status 0, unused here */
    case 0x3C4: return v->seq_index;
    case 0x3C5: return v->seq[v->seq_index & 7];
    case 0x3C6: return v->dac_mask;
    case 0x3C7: return 0;                        /* DAC state: not tracked precisely */
    case 0x3C8: return v->dac_write_index;
    case 0x3C9: {
        uint8_t val = v->dac[v->dac_read_index][v->dac_read_sub];
        if (++v->dac_read_sub == 3) {
            v->dac_read_sub = 0;
            v->dac_read_index++;
        }
        return val;
    }
    case 0x3CC: return v->misc_output;
    case 0x3CE: return v->gc_index;
    case 0x3CF: return v->gc[v->gc_index & 0xF];
    case 0x3B4: case 0x3D4: return v->crtc_index;
    case 0x3B5: case 0x3D5: return v->crtc[v->crtc_index & 0x1F];
    case 0x3BA: case 0x3DA:
        v->attr_flipflop = false;
        /* Alternate the retrace/display-enable bits each read so any
         * "wait for vsync" or "wait for display disable" poll of either
         * polarity terminates quickly instead of spinning forever. */
        v->retrace_toggle++;
        return (uint8_t)((v->retrace_toggle & 1) ? 0x09 : 0x00);
    default: return 0xFF;
    }
}

void vga_ibm_io_write(VgaIbm *v, uint16_t port, uint8_t val) {
    switch (port) {
    case 0x3C0:
        if (!v->attr_flipflop) v->attr_index = val;
        else v->attr[v->attr_index & 0x1F] = val;
        v->attr_flipflop = !v->attr_flipflop;
        break;
    case 0x3C2: v->misc_output = val; break;
    case 0x3C4: v->seq_index = val; break;
    case 0x3C5: v->seq[v->seq_index & 7] = val; break;
    case 0x3C6: v->dac_mask = val; break;
    case 0x3C7: v->dac_read_index = val; v->dac_read_sub = 0; break;
    case 0x3C8: v->dac_write_index = val; v->dac_write_sub = 0; break;
    case 0x3C9:
        v->dac[v->dac_write_index][v->dac_write_sub] = val & 0x3F;
        if (++v->dac_write_sub == 3) {
            v->dac_write_sub = 0;
            v->dac_write_index++;
        }
        break;
    case 0x3CE: v->gc_index = val; break;
    case 0x3CF: v->gc[v->gc_index & 0xF] = val; break;
    case 0x3B4: case 0x3D4: v->crtc_index = val; break;
    case 0x3B5: case 0x3D5: v->crtc[v->crtc_index & 0x1F] = val; break;
    default: break;
    }
}

void vga_ibm_aperture(const VgaIbm *v, uint32_t *base, uint32_t *size) {
    switch ((v->gc[6] >> 2) & 3) {
    case 0: *base = 0xA0000; *size = 0x20000; break;
    case 1: *base = 0xA0000; *size = 0x10000; break;
    case 2: *base = 0xB0000; *size = 0x08000; break;
    default: *base = 0xB8000; *size = 0x08000; break;
    }
}

bool vga_ibm_mode_set(const VgaIbm *v) {
    /* Sequencer reset (SR0) and CRTC max-scanline/overflow are always
     * programmed by any real mode-set sequence; a fresh reset leaves them
     * zero. Good enough signal that a real mode has been established. */
    return v->seq[1] != 0 || v->crtc[0x09] != 0 || v->crtc[0x00] != 0;
}

bool vga_ibm_is_text_mode(const VgaIbm *v) {
    return !(v->gc[6] & 1);
}

static uint32_t dac_to_argb(const VgaIbm *v, unsigned idx) {
    unsigned r = v->dac[idx][0] & 0x3F, g = v->dac[idx][1] & 0x3F, b = v->dac[idx][2] & 0x3F;
    /* 6-bit DAC channels scaled to 8-bit. */
    r = (r << 2) | (r >> 4); g = (g << 2) | (g >> 4); b = (b << 2) | (b >> 4);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static uint32_t palette_argb(const VgaIbm *v, unsigned attr_index) {
    uint8_t pal = v->attr[attr_index & 0xF] & 0x3F;
    /* Attribute Controller register 0x10 bit 7 (P54S) substitutes the DAC's
     * top two bits with attr[0x14] bits 0-1 in some legacy modes; skipped -
     * essentially never used outside EGA-compat corner cases. */
    return dac_to_argb(v, pal);
}

void vga_ibm_render(const VgaIbm *v, uint32_t *argb, int fb_w, int fb_h,
                    const uint8_t *font8x16) {
    for (int i = 0; i < fb_w * fb_h; i++) argb[i] = 0xFF000000u;

    if (vga_ibm_is_text_mode(v)) {
        int cols = v->crtc[0x01] + 1;
        if (cols <= 0 || cols > 132) cols = 80;
        unsigned max_scan = (v->crtc[0x09] & 0x1F) + 1;
        int rows = (int)((v->crtc[0x12] | ((v->crtc[7] & 2) << 7) |
                          ((v->crtc[7] & 0x40) << 3)) + 1) / (int)max_scan;
        if (rows <= 0 || rows > 60) rows = 25;
        int cw = 8, ch = 16;
        int ox = (fb_w - cols * cw) / 2, oy = (fb_h - rows * ch) / 2;
        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;

        for (int row = 0; row < rows; row++) {
            for (int col = 0; col < cols; col++) {
                uint32_t off = (uint32_t)((row * cols + col) * 2);
                uint8_t ch_code = off < 0x10000 ? v->vram[0][off] : 0;
                uint8_t attr = off < 0x10000 ? v->vram[1][off] : 0x07;
                uint32_t fg = palette_argb(v, attr & 0xF);
                uint32_t bg = palette_argb(v, (attr >> 4) & 0x7);
                const uint8_t *glyph = font8x16 + (size_t)ch_code * 16;
                int px = ox + col * cw, py = oy + row * ch;
                if (px + cw > fb_w || py + ch > fb_h) continue;
                for (int y = 0; y < ch; y++) {
                    uint8_t bits = glyph[y];
                    for (int x = 0; x < cw; x++)
                        argb[(py + y) * fb_w + (px + x)] =
                            (bits & (0x80 >> x)) ? fg : bg;
                }
            }
        }
        return;
    }

    /* Graphics modes: chain-4 (mode 0x13, 320x200x256) is the common case
     * for BIOS-era graphics; planar 16-color modes are approximated via the
     * same latch-free direct plane read used for mem reads. */
    bool chain4 = (v->seq[4] & 8) != 0;
    int gw = 320, gh = 200;
    int scale = fb_w / gw;
    if (scale < 1) scale = 1;
    int ox = (fb_w - gw * scale) / 2, oy = (fb_h - gh * scale) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;

    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            uint32_t color;
            if (chain4) {
                uint32_t addr = (uint32_t)(y * gw + x);
                color = dac_to_argb(v, v->vram[addr & 3][addr >> 2]);
            } else {
                uint32_t byteoff = (uint32_t)(y * (gw / 8) + x / 8);
                unsigned bit = 7 - (unsigned)(x & 7);
                unsigned idx = 0;
                for (int p = 0; p < 4; p++)
                    if (byteoff < 0x10000 && (v->vram[p][byteoff] & (1u << bit)))
                        idx |= 1u << p;
                color = palette_argb(v, idx);
            }
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    int px = ox + x * scale + sx, py = oy + y * scale + sy;
                    if (px < fb_w && py < fb_h)
                        argb[py * fb_w + px] = color;
                }
        }
    }
}
