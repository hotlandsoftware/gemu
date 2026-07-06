#include "ppu_sh6578.h"
#include "rp2c02.h"  /* reuses the PPUCTRL/PPUMASK/PPUSTAT bit macros + rp2c02_palette_rgb */

#include <string.h>

void ppu_sh6578_init(Sh6578Ppu *ppu, double cpu_clock_hz, double refresh_hz, int lines_total, int vblank_line) {
    memset(ppu, 0, sizeof(*ppu));
    ppu->lines_total          = lines_total;
    ppu->vblank_line          = vblank_line;
    ppu->cycles_per_scanline  = cpu_clock_hz / (refresh_hz * (double)lines_total);
    ppu_sh6578_reset(ppu);
}

void ppu_sh6578_reset(Sh6578Ppu *ppu) {
    ppu->v = ppu->t = 0;
    ppu->x = 0;
    ppu->w = false;
    ppu->vram_addr = 0;
    ppu->ppuctrl = ppu->ppumask = ppu->ppustatus = 0;
    ppu->oamaddr = 0;
    ppu->colsel_pntstart = 0;
    ppu->read_buf = 0;
    ppu->data_latch = 0;
    ppu->cycle_acc = 0.0;
    ppu->scanline = 0;
    ppu->frame = 0;
    ppu->dirty = false;
    ppu->nmi_pending = false;
    ppu->scanline_tick = false;
}

uint8_t ppu_sh6578_read(Sh6578Ppu *ppu, uint8_t reg) {
    uint8_t val = ppu->data_latch;
    switch (reg & 7u) {
    case 2: /* PPUSTATUS */
        val = ppu->ppustatus & 0xE0u;
        ppu->w = false;
        ppu->ppustatus &= (uint8_t)~PPUSTAT_VBLANK;
        break;
    case 4: /* OAMDATA */
        val = ppu->oam[ppu->oamaddr];
        break;
    case 7: { /* PPUDATA — buffered read, delayed by one byte (no palette-in-space quirk here) */
        val = ppu->read_buf;
        ppu->read_buf = ppu->vram[ppu->vram_addr];
        ppu->vram_addr = (uint16_t)(ppu->vram_addr + ((ppu->ppuctrl & PPUCTRL_VRAM_INC) ? 32 : 1));
        break;
    }
    default:
        break;
    }
    ppu->data_latch = val;
    return val;
}

void ppu_sh6578_write(Sh6578Ppu *ppu, uint8_t reg, uint8_t val) {
    ppu->data_latch = val;
    switch (reg & 7u) {
    case 0: /* PPUCTRL */
        ppu->ppuctrl = val;
        ppu->t = (uint16_t)((ppu->t & 0xF3FFu) | ((uint16_t)(val & PPUCTRL_NT_SELECT) << 10));
        break;
    case 1: /* PPUMASK */
        ppu->ppumask = val;
        break;
    case 3: /* OAMADDR */
        ppu->oamaddr = val;
        break;
    case 4: /* OAMDATA — dropped while actively rendering, matching MAME's write_to_spriteram_with_increment */
        if (ppu->scanline > 239 || !(ppu->ppumask & (PPUMASK_SHOW_BG | PPUMASK_SHOW_SPR))) {
            ppu->oam[ppu->oamaddr] = val;
            ppu->oamaddr = (uint8_t)(ppu->oamaddr + 1);
        }
        break;
    case 5: /* PPUSCROLL */
        if (ppu->w) {
            ppu->t = (uint16_t)((ppu->t & 0x0C1Fu) | ((uint16_t)(val & 0xF8u) << 2) | ((uint16_t)(val & 0x07u) << 12));
        } else {
            ppu->t = (uint16_t)((ppu->t & 0xFFE0u) | (val >> 3));
            ppu->x = val & 7u;
        }
        ppu->w = !ppu->w;
        break;
    case 6: /* PPUADDR — full 16-bit here (SH6578 addresses up to 64KB of VRAM) */
        if (ppu->w) {
            ppu->t = (uint16_t)((ppu->t & 0xFF00u) | val);
            ppu->v = ppu->t;
            ppu->vram_addr = ppu->t;
        } else {
            ppu->t = (uint16_t)((ppu->t & 0x00FFu) | ((uint16_t)val << 8));
        }
        ppu->w = !ppu->w;
        break;
    case 7: /* PPUDATA */
        ppu->vram[ppu->vram_addr] = val;
        ppu->vram_addr = (uint16_t)(ppu->vram_addr + ((ppu->ppuctrl & PPUCTRL_VRAM_INC) ? 32 : 1));
        break;
    default:
        break;
    }
}

uint8_t ppu_sh6578_read_ext(Sh6578Ppu *ppu)  { return ppu->colsel_pntstart; }
void    ppu_sh6578_write_ext(Sh6578Ppu *ppu, uint8_t val) { ppu->colsel_pntstart = val; }

uint8_t ppu_sh6578_palette_read (Sh6578Ppu *ppu, uint8_t offset) { return ppu->palette[offset & (SH6578_PALETTE_SIZE - 1)]; }
void    ppu_sh6578_palette_write(Sh6578Ppu *ppu, uint8_t offset, uint8_t val) { ppu->palette[offset & (SH6578_PALETTE_SIZE - 1)] = val; }

void ppu_sh6578_oam_dma(Sh6578Ppu *ppu, const uint8_t *bytes256) {
    memcpy(ppu->oam, bytes256, 256);
}

/* ── Background/sprite rendering (per-scanline batch, matching the
 * reference driver's own non-dot-exact draw_background()/draw_sprites()) ── */

static inline uint8_t pen_of(Sh6578Ppu *ppu, uint8_t palidx) {
    uint8_t palval = ppu->palette[palidx & (SH6578_PALETTE_SIZE - 1)] & 0x3Fu;
    if ((palval & 0x1Fu) == 0x1Fu)
        palval = ppu->palette[0] & 0x3Fu; /* transparent -> universal background pen */
    return palval;
}

static void draw_tile(Sh6578Ppu *ppu, uint8_t *line_priority, int color_byte, int address,
                       int start_x, uint32_t *row_argb, uint8_t *row_idx) {
    bool ext4bpp = (ppu->colsel_pntstart & SH6578_EXT_4BPP) != 0;
    int color = ext4bpp ? (color_byte & 0x0C) : (color_byte & 0x0F);

    uint8_t plane0 = ppu->vram[address & 0xFFFFu];
    uint8_t plane1 = ppu->vram[(address + 8) & 0xFFFFu];
    uint8_t ext0 = 0, ext1 = 0;
    if (ext4bpp) {
        ext0 = ppu->vram[(address + 16) & 0xFFFFu];
        ext1 = ppu->vram[(address + 24) & 0xFFFFu];
    }

    for (int i = 0; i < 8; i++) {
        int pix = ((plane0 & 0x80) >> 7) | ((plane1 & 0x80) >> 6);
        if (ext4bpp)
            pix |= ((ext0 & 0x80) >> 5) | ((ext1 & 0x80) >> 4);
        plane0 = (uint8_t)(plane0 << 1);
        plane1 = (uint8_t)(plane1 << 1);
        if (ext4bpp) { ext0 = (uint8_t)(ext0 << 1); ext1 = (uint8_t)(ext1 << 1); }

        int x = start_x + i;
        if (x >= 0 && x < SH6578_WIDTH) {
            uint8_t palidx = (uint8_t)(pix | (color << 2));
            uint8_t rawval = ppu->palette[palidx & (SH6578_PALETTE_SIZE - 1)] & 0x3Fu;
            bool trans = (rawval & 0x1Fu) == 0x1Fu;
            uint8_t pen = pen_of(ppu, palidx);
            row_idx[x]  = pen;
            row_argb[x] = rp2c02_palette_rgb[pen];
            if (!trans) line_priority[x] |= 0x02u;
        }
    }
}

static void draw_background(Sh6578Ppu *ppu, int sl, uint8_t *line_priority) {
    uint32_t *row_argb = &ppu->pixels_argb[sl * SH6578_WIDTH];
    uint8_t  *row_idx  = &ppu->pixels[sl * SH6578_WIDTH];

    uint8_t  scroll_x_coarse = ppu->v & 0x001Fu;
    uint8_t  scroll_y_coarse = (uint8_t)((ppu->v & 0x03E0u) >> 5);
    uint16_t nametable       = ppu->v & 0x0C00u;
    uint8_t  scroll_y_fine   = (uint8_t)((ppu->v & 0x7000u) >> 12);

    int x = scroll_x_coarse;
    int tile_index = (nametable << 1) + scroll_y_coarse * 64;
    int start_x = (ppu->x ^ 0x07) - 7;

    for (int tilecount = 0; tilecount < 34; tilecount++) {
        int index1 = tile_index + (x << 1);
        if (ppu->colsel_pntstart & SH6578_EXT_ALT_NAMETABLE)
            index1 = (index1 & 0x7FF) + 0x2000;
        else
            index1 &= 0x1FFF;

        uint8_t page2      = ppu->vram[index1 & 0xFFFF];
        uint8_t color_byte = ppu->vram[(index1 + 1) & 0xFFFF];

        if (start_x < SH6578_WIDTH) {
            int address = (((page2 | (color_byte << 8)) & 0x0FFF) << 4) + scroll_y_fine;
            draw_tile(ppu, line_priority, (color_byte >> 4) & 0xF, address, start_x, row_argb, row_idx);
            start_x += 8;
            x++;
            if (x > 31) { x = 0; tile_index ^= 0x800; }
        }
    }

    if (!(ppu->ppumask & PPUMASK_BG_LEFT)) {
        uint8_t backpen = ppu->palette[0] & 0x3Fu;
        for (int i = 0; i < 8; i++) {
            row_idx[i] = backpen;
            row_argb[i] = rp2c02_palette_rgb[backpen];
            line_priority[i] ^= 0x02u;
        }
    }
}

static void draw_sprites(Sh6578Ppu *ppu, int sl, uint8_t *line_priority) {
    bool spr_on = (ppu->ppumask & PPUMASK_SHOW_SPR) != 0;
    int size = (ppu->ppuctrl & PPUCTRL_SPR_8x16) ? 16 : 8;
    int first_pixel = (ppu->ppumask & PPUMASK_SPR_LEFT) ? 0 : 8;
    int sprite_count = 0;

    uint32_t *row_argb = &ppu->pixels_argb[sl * SH6578_WIDTH];
    uint8_t  *row_idx  = &ppu->pixels[sl * SH6578_WIDTH];

    for (int i = 0; i < 64; i++) {
        const uint8_t *e = &ppu->oam[i * 4];
        int sprite_ypos = e[0] + 1;
        if (sprite_ypos > sl || sl >= sprite_ypos + size) continue;

        if (sprite_count == 8) { ppu->ppustatus |= PPUSTAT_SPR_OVF; break; }
        sprite_count++;
        if (!spr_on) continue;

        int tile = e[1];
        int color = (e[2] & 0x03) + 4;
        bool behind = (e[2] & 0x20) != 0;
        bool flipx  = (e[2] & 0x40) != 0;
        bool flipy  = (e[2] & 0x80) != 0;
        int sprite_xpos = e[3];

        if (size == 16 && (tile & 1)) { tile &= ~1; tile |= 0x100; }

        int row = sl - sprite_ypos;
        if (flipy) row = size - 1 - row;
        if (size == 16 && row > 7) { tile++; row -= 8; }

        int index1 = tile * 16 + row;
        index1 += ((ppu->colsel_pntstart & SH6578_EXT_SPR_PAGE) >> 2) * 0x1000;

        uint8_t plane0 = ppu->vram[(index1 + 0) & 0x3FFF];
        uint8_t plane1 = ppu->vram[(index1 + 8) & 0x3FFF];

        for (int px = 0; px < 8; px++) {
            int bit = flipx ? px : (7 - px);
            int pixel_data = ((plane0 >> bit) & 1) | (((plane1 >> bit) & 1) << 1);
            int sx = sprite_xpos + px;
            if (sx < first_pixel || sx >= SH6578_WIDTH) continue;
            if (!pixel_data) continue;

            /* "behind" (low-priority) sprites only show through where nothing
             * else has drawn yet (bg opaque or an earlier sprite); "front"
             * sprites draw over an opaque background, only blocked by an
             * earlier (already-drawn) sprite. Matches MAME's
             * draw_sprite_pixel_low/_high exactly. */
            bool draw_ok = behind ? (line_priority[sx] == 0) : ((line_priority[sx] & 0x01u) == 0);
            if (draw_ok) {
                uint8_t pen = pen_of(ppu, (uint8_t)(pixel_data | (color << 2)));
                row_idx[sx]  = pen;
                row_argb[sx] = rp2c02_palette_rgb[pen];
            }
            line_priority[sx] |= 0x01u;

            if (i == 0 && sx < 255 && (line_priority[sx] & 0x02u))
                ppu->ppustatus |= PPUSTAT_SPR0_HIT;
        }
    }
}

static void render_scanline(Sh6578Ppu *ppu, int sl) {
    uint8_t line_priority[SH6578_WIDTH];
    memset(line_priority, 0, sizeof(line_priority));

    bool blanked = !(ppu->ppumask & (PPUMASK_SHOW_BG | PPUMASK_SHOW_SPR));

    if (!blanked)
        ppu->v = (uint16_t)((ppu->v & (uint16_t)~0x041Fu) | (ppu->t & 0x041Fu));

    if (ppu->ppumask & PPUMASK_SHOW_BG) {
        draw_background(ppu, sl, line_priority);
    } else {
        uint8_t backpen = ppu->palette[0] & 0x3Fu;
        uint32_t rgb = rp2c02_palette_rgb[backpen];
        uint32_t *row_argb = &ppu->pixels_argb[sl * SH6578_WIDTH];
        uint8_t  *row_idx  = &ppu->pixels[sl * SH6578_WIDTH];
        for (int x = 0; x < SH6578_WIDTH; x++) { row_idx[x] = backpen; row_argb[x] = rgb; }
    }

    draw_sprites(ppu, sl, line_priority);

    if (!blanked) {
        ppu->v = (uint16_t)(ppu->v + 0x1000u);
        if (ppu->v & 0x8000u) {
            uint16_t tmp = (uint16_t)((ppu->v & 0x03E0u) + 0x20u);
            ppu->v &= 0x7C1Fu;
            if (tmp == 0x03C0u) ppu->v ^= 0x0800u;
            else ppu->v |= (tmp & 0x03E0u);
        }
    }
}

void ppu_sh6578_tick(Sh6578Ppu *ppu) {
    ppu->cycle_acc += 1.0;
    if (ppu->cycle_acc < ppu->cycles_per_scanline)
        return;
    ppu->cycle_acc -= ppu->cycles_per_scanline;

    if (ppu->scanline <= 239)
        render_scanline(ppu, ppu->scanline);

    ppu->scanline++;
    ppu->scanline_tick = true;

    if (ppu->scanline == ppu->vblank_line) {
        ppu->ppustatus |= PPUSTAT_VBLANK;
        if (ppu->ppuctrl & PPUCTRL_NMI_EN)
            ppu->nmi_pending = true;
    }

    if (ppu->scanline == ppu->lines_total - 1) {
        ppu->ppustatus &= (uint8_t)~(PPUSTAT_VBLANK | PPUSTAT_SPR0_HIT | PPUSTAT_SPR_OVF);
    } else if (ppu->scanline == ppu->lines_total) {
        bool blanked = !(ppu->ppumask & (PPUMASK_SHOW_BG | PPUMASK_SHOW_SPR));
        if (!blanked)
            ppu->v = ppu->t;
        ppu->scanline = 0;
        ppu->frame++;
        ppu->dirty = true;
    }
}
