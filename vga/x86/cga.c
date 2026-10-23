#include "cga.h"
#include <string.h>

#define CGA_VISIBLE_LINES   200u
#define CGA_VISIBLE_CLOCKS  213u   /* 640 dots of 912 */
#define CGA_VSYNC_START     224u
#define CGA_VSYNC_LINES     16u

void cga_reset(CgaDevice *c) {
    memset(c, 0, sizeof *c);
}

uint8_t cga_io_read(CgaDevice *c, uint16_t port) {
    switch (port) {
    case 0x3D5:
        /* Only the cursor and start-address registers read back on a 6845. */
        return (c->crtc_index >= 12 && c->crtc_index <= 17) ? c->crtc[c->crtc_index] : 0;
    case 0x3DA: {
        uint32_t line = c->beam / CGA_CLOCKS_PER_LINE;
        uint32_t x    = c->beam % CGA_CLOCKS_PER_LINE;
        uint8_t v = 0xF0;
        if (line >= CGA_VISIBLE_LINES || x >= CGA_VISIBLE_CLOCKS)
            v |= 0x01;                       /* display enable inactive */
        if (line >= CGA_VSYNC_START && line < CGA_VSYNC_START + CGA_VSYNC_LINES)
            v |= 0x08;                       /* vertical retrace */
        return v;
    }
    default: return 0xFF;
    }
}

void cga_io_write(CgaDevice *c, uint16_t port, uint8_t val) {
    switch (port) {
    case 0x3D0: case 0x3D2: case 0x3D4: case 0x3D6:
        c->crtc_index = (uint8_t)(val & 0x1F); break;
    case 0x3D1: case 0x3D3: case 0x3D5: case 0x3D7:
        if (c->crtc_index < 18) c->crtc[c->crtc_index] = val;
        break;
    case 0x3D8: c->mode_ctrl = val; break;
    case 0x3D9: c->color_select = val; break;
    default: break;
    }
}

uint8_t cga_mem_read(const CgaDevice *c, uint32_t addr) { return c->vram[addr & (CGA_VRAM_SIZE - 1)]; }
void cga_mem_write(CgaDevice *c, uint32_t addr, uint8_t val) { c->vram[addr & (CGA_VRAM_SIZE - 1)] = val; }

bool cga_tick(CgaDevice *c, uint32_t clocks) {
    uint32_t before = c->beam;
    c->beam += clocks;
    bool vsync = before < CGA_VSYNC_START * CGA_CLOCKS_PER_LINE &&
                 c->beam >= CGA_VSYNC_START * CGA_CLOCKS_PER_LINE;
    while (c->beam >= CGA_CLOCKS_PER_FRAME) {
        c->beam -= CGA_CLOCKS_PER_FRAME;
        c->frame++;
        if (c->beam >= CGA_VSYNC_START * CGA_CLOCKS_PER_LINE) vsync = true;
    }
    return vsync;
}

static const uint32_t CGA_PALETTE[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF,
};

static void render_text(const CgaDevice *c, uint32_t *argb, int fb_w, int fb_h,
                        const uint8_t *font8x8) {
    bool wide80 = (c->mode_ctrl & 0x01) != 0;
    int cols = wide80 ? 80 : 40;
    int cell_w = fb_w / cols;                    /* 8, or 16 in 40 columns */
    bool blink_enable = (c->mode_ctrl & 0x20) != 0;
    bool char_blink_on = (c->frame & 16) != 0;   /* characters: 32-frame cycle */
    bool cursor_blink_on = (c->frame & 8) != 0;  /* cursor: 16-frame cycle */

    uint16_t start  = (uint16_t)((c->crtc[12] << 8) | c->crtc[13]);
    uint16_t cursor = (uint16_t)((c->crtc[14] << 8) | c->crtc[15]);
    unsigned cur_start = c->crtc[10] & 0x1F, cur_end = c->crtc[11] & 0x1F;
    bool cursor_off = (c->crtc[10] & 0x60) == 0x20;

    for (int row = 0; row < 25; row++) {
        for (int col = 0; col < cols; col++) {
            uint16_t pos = (uint16_t)(start + row * cols + col);
            uint32_t cell = (uint32_t)pos * 2u;
            uint8_t ch   = c->vram[cell & (CGA_VRAM_SIZE - 1)];
            uint8_t attr = c->vram[(cell + 1) & (CGA_VRAM_SIZE - 1)];
            uint32_t fg = CGA_PALETTE[attr & 0x0F];
            uint32_t bg;
            bool show_fg = true;
            if (blink_enable) {
                bg = CGA_PALETTE[(attr >> 4) & 0x07];
                if ((attr & 0x80) && !char_blink_on) show_fg = false;
            } else {
                bg = CGA_PALETTE[(attr >> 4) & 0x0F];
            }
            bool is_cursor = !cursor_off && cursor_blink_on && pos == cursor;

            const uint8_t *glyph = font8x8 + (unsigned)ch * 8;
            for (int y = 0; y < 8; y++) {
                uint8_t bits = glyph[y];
                bool cursor_row = is_cursor &&
                    (cur_start <= cur_end ? (y >= (int)cur_start && y <= (int)cur_end)
                                          : (y >= (int)cur_start || y <= (int)cur_end));
                int oy = row * 8 + y;
                if (oy >= fb_h) break;
                uint32_t *line = argb + oy * fb_w + col * cell_w;
                for (int x = 0; x < 8; x++) {
                    bool on = (bits & (0x80 >> x)) != 0;
                    uint32_t px = (cursor_row || (on && show_fg)) ? fg : bg;
                    if (cell_w == 16) { line[x * 2] = px; line[x * 2 + 1] = px; }
                    else line[x] = px;
                }
            }
        }
    }
}

/* Graphics VRAM is interleaved: even scanlines at 0x0000, odd at 0x2000,
 * 80 bytes per line. */
static const uint8_t *gfx_line(const CgaDevice *c, int y) {
    return c->vram + ((y & 1) ? 0x2000 : 0) + (y >> 1) * 80;
}

static void render_graphics(const CgaDevice *c, uint32_t *argb, int fb_w, int fb_h) {
    if (c->mode_ctrl & 0x10) {                   /* 640x200, 2 colors */
        uint32_t fg = CGA_PALETTE[c->color_select & 0x0F];
        for (int y = 0; y < 200 && y < fb_h; y++) {
            const uint8_t *src = gfx_line(c, y);
            uint32_t *line = argb + y * fb_w;
            for (int x = 0; x < 640 && x < fb_w; x++)
                line[x] = (src[x >> 3] & (0x80 >> (x & 7))) ? fg : 0xFF000000;
        }
        return;
    }

    /* 320x200, 4 colors: color 0 is the background from the color select
     * register; 1-3 come from palette 0 (green/red/brown), palette 1
     * (cyan/magenta/white), or the B/W-mode palette (cyan/red/white). */
    unsigned bright = (c->color_select & 0x10) ? 8 : 0;
    uint32_t pal[4];
    pal[0] = CGA_PALETTE[c->color_select & 0x0F];
    if (c->mode_ctrl & 0x04) {
        pal[1] = CGA_PALETTE[3 + bright]; pal[2] = CGA_PALETTE[4 + bright]; pal[3] = CGA_PALETTE[7 + bright];
    } else if (c->color_select & 0x20) {
        pal[1] = CGA_PALETTE[3 + bright]; pal[2] = CGA_PALETTE[5 + bright]; pal[3] = CGA_PALETTE[7 + bright];
    } else {
        pal[1] = CGA_PALETTE[2 + bright]; pal[2] = CGA_PALETTE[4 + bright]; pal[3] = CGA_PALETTE[6 + bright];
    }
    for (int y = 0; y < 200 && y < fb_h; y++) {
        const uint8_t *src = gfx_line(c, y);
        uint32_t *line = argb + y * fb_w;
        for (int x = 0; x < 320 && x * 2 + 1 < fb_w; x++) {
            unsigned px = (src[x >> 2] >> (6 - (x & 3) * 2)) & 3;
            line[x * 2] = line[x * 2 + 1] = pal[px];
        }
    }
}

void cga_render(const CgaDevice *c, uint32_t *argb, int fb_w, int fb_h, const uint8_t *font8x8) {
    /* Render the 200 CGA scanlines, then double them if there's room: a
     * real CGA monitor's scanlines are about 2.4x as tall as its pixels
     * are wide, and 640x400 keeps that shape at integer scale. */
    bool doubled = fb_h >= 400;
    int lines = doubled ? 200 : fb_h;
    for (int i = 0; i < fb_w * fb_h; i++) argb[i] = 0xFF000000;
    if (!(c->mode_ctrl & 0x08)) return;          /* video disabled */
    if (c->mode_ctrl & 0x02) render_graphics(c, argb, fb_w, lines);
    else                     render_text(c, argb, fb_w, lines, font8x8);
    if (doubled) {
        for (int y = 199; y >= 0; y--) {         /* bottom-up: in place */
            memcpy(argb + (2 * y + 1) * fb_w, argb + y * fb_w, (size_t)fb_w * sizeof(*argb));
            if (y) memcpy(argb + 2 * y * fb_w, argb + y * fb_w, (size_t)fb_w * sizeof(*argb));
        }
    }
}
