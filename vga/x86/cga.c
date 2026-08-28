#include "cga.h"
#include <string.h>

void cga_reset(CgaDevice *c) {
    memset(c, 0, sizeof *c);
}

uint8_t cga_io_read(CgaDevice *c, uint16_t port) {
    switch (port) {
    case 0x3D4: return c->crtc_index;
    case 0x3D5: return c->crtc_index < 18 ? c->crtc[c->crtc_index] : 0;
    case 0x3D8: return c->mode_ctrl;
    case 0x3D9: return c->color_select;
    case 0x3DA: {
        /* bit0: 1 during horizontal-or-vertical retrace (safe to write
         * VRAM without snow); bit3: 1 during vertical retrace. Both tied
         * to retrace_counter so any BIOS/game polling loop terminates. */
        uint8_t v = 0;
        if (c->retrace_counter & 0x08) v |= 0x01;
        if ((c->retrace_counter & 0xFF) < 0x10) v |= 0x08;
        return v;
    }
    default: return 0xFF;
    }
}

void cga_io_write(CgaDevice *c, uint16_t port, uint8_t val) {
    switch (port) {
    case 0x3D4: c->crtc_index = (uint8_t)(val & 0x1F); break;
    case 0x3D5: if (c->crtc_index < 18) c->crtc[c->crtc_index] = val; break;
    case 0x3D8: c->mode_ctrl = val; break;
    case 0x3D9: c->color_select = val; break;
    default: break;
    }
}

uint8_t cga_mem_read(const CgaDevice *c, uint32_t addr) { return c->vram[addr & (CGA_VRAM_SIZE - 1)]; }
void cga_mem_write(CgaDevice *c, uint32_t addr, uint8_t val) { c->vram[addr & (CGA_VRAM_SIZE - 1)] = val; }

void cga_tick(CgaDevice *c, uint32_t amount) { c->retrace_counter += amount; }

static const uint32_t CGA_PALETTE[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF,
};

void cga_render(const CgaDevice *c, uint32_t *argb, int fb_w, int fb_h, const uint8_t *font8x8) {
    for (int i = 0; i < fb_w * fb_h; i++) argb[i] = 0xFF000000;
    if (!(c->mode_ctrl & 0x08)) return;   /* video disabled */
    if (c->mode_ctrl & 0x02) return;      /* graphics mode - not implemented yet */

    bool wide80 = (c->mode_ctrl & 0x01) != 0;
    int cols = wide80 ? 80 : 40;
    int rows = 25;
    int cell_h = fb_h / rows;             /* 8 for the standard 640x200 target */
    int cell_w = fb_w / cols;             /* 8 in 80-col, 16 in 40-col */
    bool blink_enable = (c->mode_ctrl & 0x20) != 0;
    bool blink_phase = (c->retrace_counter & 0x2000) != 0; /* ~toggles a few times/sec at typical tick rates */

    uint16_t start_addr = (uint16_t)(((c->crtc[12] << 8) | c->crtc[13]));
    uint16_t cursor_pos  = (uint16_t)(((c->crtc[14] << 8) | c->crtc[15]));

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            uint32_t cell = (uint32_t)(start_addr + row * cols + col) * 2u;
            uint8_t ch   = c->vram[cell & (CGA_VRAM_SIZE - 1)];
            uint8_t attr = c->vram[(cell + 1) & (CGA_VRAM_SIZE - 1)];
            uint32_t fg = CGA_PALETTE[attr & 0x0F];
            uint32_t bg;
            bool blink;
            if (blink_enable) { bg = CGA_PALETTE[(attr >> 4) & 0x07]; blink = (attr & 0x80) != 0; }
            else { bg = CGA_PALETTE[(attr >> 4) & 0x0F]; blink = false; }
            bool show_fg = !(blink && !blink_phase);
            bool is_cursor = wide80
                ? ((uint32_t)(start_addr + row * cols + col) == cursor_pos)
                : ((uint32_t)(start_addr + row * cols + col) == cursor_pos);

            const uint8_t *glyph = font8x8 + (unsigned)ch * 8;
            for (int y = 0; y < 8 && y < cell_h; y++) {
                uint8_t bits = glyph[y];
                bool cursor_row = is_cursor && y >= 6; /* underline-style cursor on the bottom rows */
                for (int x = 0; x < 8; x++) {
                    bool on = (bits & (0x80 >> x)) != 0;
                    uint32_t px = (on && show_fg) ? fg : bg;
                    if (cursor_row) px = fg;
                    int ox = col * cell_w + x * (cell_w / 8);
                    int oy = row * cell_h + y;
                    if (cell_w == 16) {
                        argb[oy * fb_w + ox] = px;
                        argb[oy * fb_w + ox + 1] = px;
                    } else if (ox < fb_w && oy < fb_h) {
                        argb[oy * fb_w + ox] = px;
                    }
                }
            }
        }
    }
}
