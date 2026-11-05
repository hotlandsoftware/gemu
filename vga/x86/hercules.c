/* Hercules Graphics Card: see hercules.h.  Text mode is drawn by the
 * MDA's code (mda_draw_text); this file adds the graphics mode, the
 * second page and the CRTC-driven timing. */
#include "hercules.h"
#include <string.h>

#define DOT_HZ  16257000u
#define BLACK   0xFF000000u

void hercules_reset(HerculesDevice *h) {
    MdaPhosphor phosphor = h->phosphor;
    memset(h, 0, sizeof *h);
    h->phosphor = phosphor;
}

bool hercules_set_options(HerculesDevice *h, const char *opts, char *err, size_t err_len) {
    return mda_parse_options("hercules", opts, &h->phosphor, err, err_len);
}

/* The mode bits configuration allows: graphics and page need bit 0. */
static uint8_t mode(const HerculesDevice *h) {
    return (h->config & 0x01) ? h->mode_ctrl : (uint8_t)(h->mode_ctrl & ~0x82);
}

static bool graphics(const HerculesDevice *h) { return (mode(h) & 0x02) != 0; }

/* The frame as the CRTC sets it up, in dots and lines.  Before the BIOS
 * programs it, the MDA's 882 x 370. */
typedef struct { uint32_t line_dots, lines, vis_dots, vis_lines, vsync_line; } Timing;

static Timing timing(const HerculesDevice *h) {
    const uint8_t *r = h->crtc;
    uint32_t cw = graphics(h) ? 16 : 9, rows = (uint32_t)(r[9] & 0x1F) + 1;
    Timing t;
    if (!r[0] || !r[4]) {
        t = (Timing){ 882, 370, 720, 350, 350 };
        return t;
    }
    t.line_dots  = ((uint32_t)r[0] + 1) * cw;
    t.lines      = ((uint32_t)(r[4] & 0x7F) + 1) * rows + (r[5] & 0x1F);
    t.vis_dots   = (uint32_t)r[1] * cw;
    t.vis_lines  = (uint32_t)(r[6] & 0x7F) * rows;
    t.vsync_line = (uint32_t)(r[7] & 0x7F) * rows;
    return t;
}

uint8_t hercules_io_read(HerculesDevice *h, uint16_t port) {
    switch (port) {
    case 0x3B1: case 0x3B3: case 0x3B5: case 0x3B7:
        return (h->crtc_index >= 12 && h->crtc_index <= 17) ? h->crtc[h->crtc_index] : 0;
    case 0x3BA: {
        Timing t = timing(h);
        uint32_t line = (uint32_t)(h->dots / t.line_dots), x = (uint32_t)(h->dots % t.line_dots);
        bool active = line < t.vis_lines && x < t.vis_dots;
        bool vsync = line >= t.vsync_line && line < t.vsync_line + 16;   /* the 6845's 16 lines */
        uint8_t v = 0x70;
        if (!vsync) v |= 0x80;
        if (!active && line < t.vis_lines) v |= 0x01;
        if (active && (h->mode_ctrl & 0x08)) v |= 0x08;
        return v;
    }
    default:
        return 0xFF;
    }
}

void hercules_io_write(HerculesDevice *h, uint16_t port, uint8_t val) {
    switch (port) {
    case 0x3B0: case 0x3B2: case 0x3B4: case 0x3B6:
        h->crtc_index = (uint8_t)(val & 0x1F);
        break;
    case 0x3B1: case 0x3B3: case 0x3B5: case 0x3B7:
        if (h->crtc_index < 18) h->crtc[h->crtc_index] = val;
        break;
    case 0x3B8:
        h->mode_ctrl = val;
        break;
    case 0x3BF:
        h->config = val & 0x03;
        break;
    default:
        break;
    }
}

bool hercules_mem_decoded(const HerculesDevice *h, uint32_t addr) {
    return addr < 0x8000 || (addr < HERCULES_VRAM_SIZE && (h->config & 0x02));
}

uint8_t hercules_mem_read(const HerculesDevice *h, uint32_t addr) {
    return h->vram[addr & (HERCULES_VRAM_SIZE - 1)];
}

void hercules_mem_write(HerculesDevice *h, uint32_t addr, uint8_t val) {
    h->vram[addr & (HERCULES_VRAM_SIZE - 1)] = val;
}

bool hercules_tick(HerculesDevice *h, uint32_t clocks, uint32_t cpu_hz) {
    h->dot_frac += (uint64_t)clocks * DOT_HZ;
    h->dots += h->dot_frac / cpu_hz;
    h->dot_frac %= cpu_hz;
    Timing t = timing(h);
    uint64_t frame_dots = (uint64_t)t.line_dots * t.lines;
    if (h->dots < frame_dots) return false;
    while (h->dots >= frame_dots) {
        h->dots -= frame_dots;
        h->frame++;
    }
    return true;
}

uint32_t hercules_clocks_per_frame(const HerculesDevice *h, uint32_t cpu_hz) {
    Timing t = timing(h);
    return (uint32_t)((uint64_t)t.line_dots * t.lines * cpu_hz / DOT_HZ);
}

/* 720x348: each character row's scan lines from the four banks of the
 * shown page, 16 pixels per CRTC address. */
static void draw_graphics(const HerculesDevice *h, uint32_t *argb) {
    uint32_t on, bright;
    mda_phosphor_colors(h->phosphor, &on, &bright);
    (void)bright;
    const uint8_t *r = h->crtc;
    const uint8_t *page = h->vram + ((mode(h) & 0x80) ? 0x8000 : 0);
    uint32_t rows = (uint32_t)(r[9] & 0x1F) + 1;
    uint32_t start = ((uint32_t)r[12] << 8) | r[13];
    for (uint32_t row = 0; row < (uint32_t)(r[6] & 0x7F); row++) {
        for (uint32_t ra = 0; ra < rows; ra++) {
            uint32_t y = row * rows + ra;
            if (y >= HERCULES_HEIGHT) return;
            uint32_t *line = argb + y * HERCULES_WIDTH;
            for (uint32_t c = 0; c < r[1]; c++) {
                uint32_t x = c * 16;
                if (x >= HERCULES_WIDTH) break;
                uint32_t ma = start + row * r[1] + c;
                uint32_t addr = ((ra & 3) << 13) | ((ma * 2) & 0x1FFF);
                uint16_t bits = (uint16_t)(page[addr] << 8 | page[(addr + 1) & 0x7FFF]);
                for (int b = 0; b < 16 && x + b < HERCULES_WIDTH; b++)
                    line[x + b] = (bits & (0x8000 >> b)) ? on : BLACK;
            }
        }
    }
}

void hercules_render(const HerculesDevice *h, uint32_t *argb, const uint8_t *font8x14) {
    for (int i = 0; i < HERCULES_WIDTH * HERCULES_HEIGHT; i++) argb[i] = BLACK;
    uint8_t m = mode(h);
    if (!(m & 0x08)) return;                            /* video disabled */
    if (m & 0x02) {
        draw_graphics(h, argb);
    } else {
        /* text: the MDA's, in 32 KiB of the shown page */
        mda_draw_text(h->vram + ((m & 0x80) ? 0x8000 : 0), 0x7FFF, h->crtc, m, h->frame,
                      h->phosphor, argb, font8x14);
    }
}
