#include "mda.h"
#include <string.h>

/* Beam position is kept in 1/16 CPU clocks so the line length (258.9
 * clocks) stays exact: 4.772727 MHz / 18.432 kHz line rate. */
#define BEAM_PER_CLOCK   16u
#define BEAM_PER_LINE    4143u    /* 258.94 clocks x 16 */
#define LINES_PER_FRAME  370u
#define VISIBLE_LINES    350u
#define VISIBLE_BEAM     3383u    /* 720 of 882 dots */
#define BEAM_PER_FRAME   (BEAM_PER_LINE * LINES_PER_FRAME)

/* IBM 5151 green phosphor */
#define MDA_BLACK   0xFF000000u
#define MDA_NORMAL  0xFF18B030u
#define MDA_BRIGHT  0xFF70FF80u

void mda_reset(MdaDevice *m) {
    memset(m, 0, sizeof *m);
}

uint8_t mda_io_read(MdaDevice *m, uint16_t port) {
    switch (port) {
    case 0x3B1: case 0x3B3: case 0x3B5: case 0x3B7:
        /* Only the cursor and start-address registers read back on a 6845. */
        return (m->crtc_index >= 12 && m->crtc_index <= 17) ? m->crtc[m->crtc_index] : 0;
    case 0x3BA: {
        uint32_t line = m->beam / BEAM_PER_LINE;
        uint32_t x    = m->beam % BEAM_PER_LINE;
        bool active = line < VISIBLE_LINES && x < VISIBLE_BEAM;
        uint8_t v = 0xF0;
        if (!active && line < VISIBLE_LINES) v |= 0x01;   /* horizontal retrace */
        if (active && (m->mode_ctrl & 0x08)) v |= 0x08;   /* video signal */
        return v;
    }
    default:
        return 0xFF;
    }
}

void mda_io_write(MdaDevice *m, uint16_t port, uint8_t val) {
    switch (port) {
    case 0x3B0: case 0x3B2: case 0x3B4: case 0x3B6:
        m->crtc_index = (uint8_t)(val & 0x1F);
        break;
    case 0x3B1: case 0x3B3: case 0x3B5: case 0x3B7:
        if (m->crtc_index < 18) m->crtc[m->crtc_index] = val;
        break;
    case 0x3B8:
        m->mode_ctrl = val;
        break;
    default:
        break;
    }
}

uint8_t mda_mem_read(const MdaDevice *m, uint32_t addr) { return m->vram[addr & (MDA_VRAM_SIZE - 1)]; }
void mda_mem_write(MdaDevice *m, uint32_t addr, uint8_t val) { m->vram[addr & (MDA_VRAM_SIZE - 1)] = val; }

bool mda_tick(MdaDevice *m, uint32_t clocks) {
    m->beam += clocks * BEAM_PER_CLOCK;
    if (m->beam < BEAM_PER_FRAME) return false;
    while (m->beam >= BEAM_PER_FRAME) {
        m->beam -= BEAM_PER_FRAME;
        m->frame++;
    }
    return true;
}

void mda_render(const MdaDevice *m, uint32_t *argb, const uint8_t *font8x14) {
    for (int i = 0; i < MDA_WIDTH * MDA_HEIGHT; i++) argb[i] = MDA_BLACK;
    if (!(m->mode_ctrl & 0x08)) return;                 /* video disabled */

    bool blink_enable = (m->mode_ctrl & 0x20) != 0;
    bool char_blink_on = (m->frame & 16) != 0;          /* 32-frame cycle */
    bool cursor_blink_on = (m->frame & 8) != 0;         /* 16-frame cycle */
    uint16_t start  = (uint16_t)((m->crtc[12] << 8) | m->crtc[13]);
    uint16_t cursor = (uint16_t)((m->crtc[14] << 8) | m->crtc[15]);
    unsigned cur_start = m->crtc[10] & 0x1F, cur_end = m->crtc[11] & 0x1F;
    bool cursor_off = (m->crtc[10] & 0x60) == 0x20;

    for (int row = 0; row < 25; row++) {
        for (int col = 0; col < 80; col++) {
            uint16_t pos = (uint16_t)(start + row * 80 + col);
            uint32_t cell = (uint32_t)pos * 2u;
            uint8_t ch   = m->vram[cell & (MDA_VRAM_SIZE - 1)];
            uint8_t attr = m->vram[(cell + 1) & (MDA_VRAM_SIZE - 1)];

            /* Attribute decoding, as the MDA's attribute logic does it. */
            uint32_t fg, bg = MDA_BLACK;
            bool underline = false;
            bool blink = blink_enable && (attr & 0x80);
            if ((attr & 0x77) == 0x00) {                 /* invisible */
                fg = MDA_BLACK;
            } else if ((attr & 0x77) == 0x70) {          /* reverse video */
                fg = MDA_BLACK;
                bg = (!blink_enable && (attr & 0x80)) ? MDA_BRIGHT : MDA_NORMAL;
            } else {
                fg = (attr & 0x08) ? MDA_BRIGHT : MDA_NORMAL;
                underline = (attr & 0x07) == 0x01;
            }
            bool show_fg = !(blink && !char_blink_on);
            bool is_cursor = !cursor_off && cursor_blink_on && pos == cursor;
            bool ninth = ch >= 0xC0 && ch <= 0xDF;       /* line-drawing range */

            /* The cursor is drawn in the text colour (normal green over
             * black-on-green and invisible cells, so it stays visible). */
            uint32_t cursor_col = fg == MDA_BLACK ? MDA_NORMAL : fg;
            const uint8_t *glyph = font8x14 + (unsigned)ch * 14;
            for (int y = 0; y < 14; y++) {
                uint8_t bits = glyph[y];
                bool underline_row = underline && y == 12 && show_fg;
                bool cursor_row = is_cursor && (cur_start <= cur_end
                                  ? (y >= (int)cur_start && y <= (int)cur_end)
                                  : (y >= (int)cur_start || y <= (int)cur_end));
                uint32_t *line = argb + (row * 14 + y) * MDA_WIDTH + col * 9;
                for (int x = 0; x < 9; x++) {
                    bool on = x < 8 ? (bits & (0x80 >> x)) != 0 : (ninth && (bits & 1));
                    uint32_t px = (underline_row || (on && show_fg)) ? fg : bg;
                    line[x] = cursor_row ? cursor_col : px;
                }
            }
        }
    }
}
