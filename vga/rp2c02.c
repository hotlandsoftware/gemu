#include "rp2c02.h"

#include <stddef.h>
#include <string.h>

#include <stdio.h>
#define PPU_LOG(...) do { if (ppu->debug) fprintf(stderr, "[PPU sl=%3d dot=%3d] ", ppu->scanline, ppu->dot), fprintf(stderr, __VA_ARGS__); } while(0)

const uint32_t rp2c02_palette_rgb[64] = {
    0xFF666666, 0xFF002A88, 0xFF1412A7, 0xFF3B00A4,
    0xFF5C007E, 0xFF6E0040, 0xFF6C0600, 0xFF561D00,
    0xFF333500, 0xFF0B4800, 0xFF005200, 0xFF004F08,
    0xFF00404D, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFADADAD, 0xFF155FD9, 0xFF4240FF, 0xFF7527FE,
    0xFFA01ACC, 0xFFB71E7B, 0xFFB53120, 0xFF994E00,
    0xFF6B6D00, 0xFF388700, 0xFF0C9300, 0xFF008F32,
    0xFF007C8D, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFFFFEFF, 0xFF64B0FF, 0xFF9290FF, 0xFFC676FF,
    0xFFF36AFF, 0xFFFE6ECC, 0xFFFE8170, 0xFFEA9E22,
    0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082,
    0xFF48CDDE, 0xFF4F4F4F, 0xFF000000, 0xFF000000,
    0xFFFFFEFF, 0xFFC0DFFF, 0xFFD3D2FF, 0xFFE8C8FF,
    0xFFFBC2FF, 0xFFFEC4EA, 0xFFFECCC5, 0xFFF7D8A5,
    0xFFE4E594, 0xFFCFEF96, 0xFFBDF4AB, 0xFFB3F3CC,
    0xFFB5EBF2, 0xFFB8B8B8, 0xFF000000, 0xFF000000,
};

static uint16_t nt_mirror(const Rp2c02 *ppu, uint16_t addr) {
    static const uint8_t map[5][4] = {
        {0, 0, 1, 1},
        {0, 1, 0, 1},
        {0, 0, 0, 0},
        {1, 1, 1, 1},
        {0, 1, 2, 3},
    };
    uint8_t mode = ppu->mirror < 5 ? ppu->mirror : RP2C02_MIRROR_VERTICAL;
    uint16_t nt = (addr >> 10) & 3u;
    uint16_t off = addr & 0x03FFu;
    return (uint16_t)(((map[mode][nt] & 1u) * 0x400u) + off);
}

static uint8_t palette_index(uint8_t addr) {
    addr &= 0x1Fu;
    if ((addr & 0x13u) == 0x10u) addr &= (uint8_t)~0x10u;
    return addr;
}

static uint8_t palette_rd(const Rp2c02 *ppu, uint8_t addr) {
    uint8_t v = ppu->palette[palette_index(addr)] & 0x3Fu;
    if (ppu->ppumask & PPUMASK_GRAY) v &= 0x30u;
    return v;
}

static void palette_wr(Rp2c02 *ppu, uint8_t addr, uint8_t val) {
    ppu->palette[palette_index(addr)] = val & 0x3Fu;
}

static uint8_t ppu_bus_rd(Rp2c02 *ppu, uint16_t addr) {
    addr &= 0x3FFFu;
    if (addr < 0x2000u)
        return ppu->chr_read ? ppu->chr_read(addr, ppu->chr_ud) : 0;
    if (addr < 0x3F00u) {
        if (ppu->nt_read)
            return ppu->nt_read(addr & 0x2FFFu, ppu->nt_ud);
        return ppu->vram[nt_mirror(ppu, addr & 0x2FFFu)];
    }
    return palette_rd(ppu, (uint8_t)addr);
}

static void ppu_bus_wr(Rp2c02 *ppu, uint16_t addr, uint8_t val) {
    addr &= 0x3FFFu;
    if (addr < 0x2000u) {
        if (ppu->chr_write) ppu->chr_write(addr, val, ppu->chr_ud);
    } else if (addr < 0x3F00u) {
        if (ppu->nt_write)
            ppu->nt_write(addr & 0x2FFFu, val, ppu->nt_ud);
        else
            ppu->vram[nt_mirror(ppu, addr & 0x2FFFu)] = val;
    } else {
        palette_wr(ppu, (uint8_t)addr, val);
    }
}

static void incr_coarse_x(Rp2c02 *ppu);
static void incr_y(Rp2c02 *ppu);

static void increment_vram_addr(Rp2c02 *ppu) {
    if ((ppu->ppumask & (PPUMASK_SHOW_BG | PPUMASK_SHOW_SPR)) &&
        ((ppu->scanline >= 0 && ppu->scanline < 240) || ppu->scanline == ppu->lines_total - 1)) {
        incr_coarse_x(ppu);
        incr_y(ppu);
        return;
    }
    ppu->v = (uint16_t)((ppu->v + ((ppu->ppuctrl & PPUCTRL_VRAM_INC) ? 32 : 1)) & 0x7FFFu);
}

uint8_t rp2c02_read(Rp2c02 *ppu, uint8_t reg) {
    uint8_t val = ppu->open_bus;

    switch (reg & 7u) {
    case 2:
        val = (uint8_t)((ppu->ppustatus & 0xE0u) | (ppu->open_bus & 0x1Fu));
        ppu->ppustatus &= (uint8_t)~PPUSTAT_VBLANK;
        ppu->nmi_pending = false;
        ppu->w = false;
        break;
    case 4:
        val = ppu->oam[ppu->oamaddr];
        break;
    case 7: {
        uint16_t addr = ppu->v & 0x3FFFu;
        if (addr < 0x3F00u) {
            val = ppu->read_buf;
            ppu->read_buf = ppu_bus_rd(ppu, addr);
        } else {
            val = palette_rd(ppu, (uint8_t)addr);
            ppu->read_buf = ppu_bus_rd(ppu, (uint16_t)(addr - 0x1000u));
        }
        increment_vram_addr(ppu);
        break;
    }
    default:
        break;
    }

    ppu->open_bus = val;
    return val;
}

void rp2c02_write(Rp2c02 *ppu, uint8_t reg, uint8_t val) {
    ppu->open_bus = val;

    switch (reg & 7u) {
    case 0: {
        PPU_LOG("PPUCTRL  <- %02X (NMI=%d SPR_PT=%d BG_PT=%d 8x16=%d inc=%d NT=%d)\n",
                val, !!(val & PPUCTRL_NMI_EN), !!(val & PPUCTRL_SPR_PT),
                !!(val & PPUCTRL_BG_PT), !!(val & PPUCTRL_SPR_8x16),
                !!(val & PPUCTRL_VRAM_INC), val & PPUCTRL_NT_SELECT);
        bool was_nmi_en = (ppu->ppuctrl & PPUCTRL_NMI_EN) != 0;
        ppu->ppuctrl = val;
        ppu->t = (uint16_t)((ppu->t & 0xF3FFu) | ((uint16_t)(val & PPUCTRL_NT_SELECT) << 10));
        if ((ppu->ppustatus & PPUSTAT_VBLANK) && (val & PPUCTRL_NMI_EN) && !was_nmi_en)
            ppu->nmi_pending = true;
        break;
    }
    case 1: {
        bool was = (ppu->ppumask & (PPUMASK_SHOW_BG | PPUMASK_SHOW_SPR)) != 0;
        ppu->ppumask = val;
        ppu->ppumask_pending = val;
        ppu->ppumask_delay = 0;
        bool now = (ppu->ppumask & (PPUMASK_SHOW_BG | PPUMASK_SHOW_SPR)) != 0;
        if (was != now)
            PPU_LOG("PPUMASK  <- %02X (rendering %s)\n", val, now ? "ON" : "OFF");
        else
            PPU_LOG("PPUMASK  <- %02X\n", val);
        break;
    }
    case 3:
        ppu->oamaddr = val;
        break;
    case 4:
        ppu->oam[ppu->oamaddr++] = val;
        break;
    case 5:
        if (!ppu->w) {
            PPU_LOG("PPUSCROLL X <- %d (coarse=%d fine=%d) t=%04X\n",
                    val, val >> 3, val & 7, ppu->t);
            ppu->t = (uint16_t)((ppu->t & 0xFFE0u) | (val >> 3));
            ppu->x = val & 7u;
        } else {
            PPU_LOG("PPUSCROLL Y <- %d (coarse=%d fine=%d) t=%04X\n",
                    val, val >> 3, val & 7, ppu->t);
            ppu->t = (uint16_t)((ppu->t & 0x8C1Fu) |
                                (((uint16_t)val & 0xF8u) << 2) |
                                (((uint16_t)val & 0x07u) << 12));
        }
        ppu->w = !ppu->w;
        break;
    case 6:
        if (!ppu->w) {
            PPU_LOG("PPUADDR  hi <- %02X t=%04X\n", val, ppu->t);
            ppu->t = (uint16_t)((ppu->t & 0x00FFu) | (((uint16_t)val & 0x3Fu) << 8));
        } else {
            ppu->t = (uint16_t)((ppu->t & 0xFF00u) | val);
            ppu->v = ppu->t;
            PPU_LOG("PPUADDR  lo <- %02X v=%04X\n", val, ppu->v);
        }
        ppu->w = !ppu->w;
        break;
    case 7:
        if (ppu->scanline >= 0 && ppu->scanline < 240 &&
            (ppu->ppumask & (PPUMASK_SHOW_BG | PPUMASK_SHOW_SPR))) {
            increment_vram_addr(ppu);
            break;
        }
        ppu_bus_wr(ppu, ppu->v, val);
        increment_vram_addr(ppu);
        break;
    default:
        break;
    }
}

void rp2c02_oam_dma(Rp2c02 *ppu, const uint8_t *page) {
    memcpy(ppu->oam, page, 256);
}

static bool rendering_enabled(const Rp2c02 *ppu) {
    return (ppu->ppumask & (PPUMASK_SHOW_BG | PPUMASK_SHOW_SPR)) != 0;
}

static void incr_coarse_x(Rp2c02 *ppu) {
    if ((ppu->v & 0x001Fu) == 31) {
        ppu->v &= (uint16_t)~0x001Fu;
        ppu->v ^= 0x0400u;
    } else {
        ppu->v++;
    }
}

static void incr_y(Rp2c02 *ppu) {
    if ((ppu->v & 0x7000u) != 0x7000u) {
        ppu->v += 0x1000u;
    } else {
        ppu->v &= (uint16_t)~0x7000u;
        uint16_t cy = (ppu->v >> 5) & 0x1Fu;
        if (cy == 29) {
            cy = 0;
            ppu->v ^= 0x0800u;
        } else if (cy == 31) {
            cy = 0;
        } else {
            cy++;
        }
        ppu->v = (uint16_t)((ppu->v & ~0x03E0u) | (cy << 5));
    }
}

static void copy_x_from_t(Rp2c02 *ppu) {
    ppu->v = (uint16_t)((ppu->v & ~0x041Fu) | (ppu->t & 0x041Fu));
}

static void copy_y_from_t(Rp2c02 *ppu) {
    ppu->v = (uint16_t)((ppu->v & ~0x7BE0u) | (ppu->t & 0x7BE0u));
}

static void bg_shift_tick(Rp2c02 *ppu) {
    ppu->bg_shift_lo <<= 1;
    ppu->bg_shift_hi <<= 1;
    ppu->bg_attr_lo <<= 1;
    ppu->bg_attr_hi <<= 1;
}

static void bg_shift_reload(Rp2c02 *ppu) {
    ppu->bg_shift_lo = (uint16_t)((ppu->bg_shift_lo & 0xFF00u) | ppu->pt_lo_latch);
    ppu->bg_shift_hi = (uint16_t)((ppu->bg_shift_hi & 0xFF00u) | ppu->pt_hi_latch);
    ppu->bg_attr_lo = (uint16_t)((ppu->bg_attr_lo & 0xFF00u) | (ppu->at_latch_lo ? 0x00FFu : 0));
    ppu->bg_attr_hi = (uint16_t)((ppu->bg_attr_hi & 0xFF00u) | (ppu->at_latch_hi ? 0x00FFu : 0));
}

static uint8_t fetch_attr(Rp2c02 *ppu) {
    uint16_t addr = (uint16_t)(0x23C0u |
                              (ppu->v & 0x0C00u) |
                              ((ppu->v >> 4) & 0x38u) |
                              ((ppu->v >> 2) & 0x07u));
    uint8_t attr = ppu_bus_rd(ppu, addr);
    uint8_t shift = (uint8_t)(((ppu->v >> 4) & 4u) | (ppu->v & 2u));
    return (uint8_t)((attr >> shift) & 3u);
}

static void bg_fetch(Rp2c02 *ppu, int dot) {
    switch (dot & 7) {
    case 1:
        bg_shift_reload(ppu);
        ppu->nt_latch = ppu_bus_rd(ppu, (uint16_t)(0x2000u | (ppu->v & 0x0FFFu)));
        break;
    case 3:
        ppu->at_latch = fetch_attr(ppu);
        ppu->at_latch_lo = ppu->at_latch & 1u;
        ppu->at_latch_hi = (ppu->at_latch >> 1) & 1u;
        break;
    case 5: {
        uint16_t base = (ppu->ppuctrl & PPUCTRL_BG_PT) ? 0x1000u : 0x0000u;
        uint8_t fine_y = (uint8_t)((ppu->v >> 12) & 7u);
        ppu->pt_lo_latch = ppu_bus_rd(ppu, (uint16_t)(base + ((uint16_t)ppu->nt_latch << 4) + fine_y));
        break;
    }
    case 7: {
        uint16_t base = (ppu->ppuctrl & PPUCTRL_BG_PT) ? 0x1000u : 0x0000u;
        uint8_t fine_y = (uint8_t)((ppu->v >> 12) & 7u);
        ppu->pt_hi_latch = ppu_bus_rd(ppu, (uint16_t)(base + ((uint16_t)ppu->nt_latch << 4) + fine_y + 8u));
        incr_coarse_x(ppu);
        break;
    }
    default:
        break;
    }
}

static uint8_t bit_reverse(uint8_t b) {
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

static void evaluate_sprites(Rp2c02 *ppu) {
    int target = ppu->scanline + 1;
    if (target >= ppu->lines_total) target = 0;

    int spr_h = (ppu->ppuctrl & PPUCTRL_SPR_8x16) ? 16 : 8;
    ppu->n_spr = 0;
    ppu->spr0_next = false;
    memset(ppu->oam2, 0xFF, sizeof(ppu->oam2));
    memset(ppu->spr_is_zero, 0, sizeof(ppu->spr_is_zero));

    if (target >= 240) return;

    for (int i = 0; i < 64; i++) {
        int y = ((int)ppu->oam[i * 4] + 1) & 0xFF;
        if (target < y || target >= y + spr_h) continue;

        if (ppu->n_spr < 8) {
            memcpy(&ppu->oam2[ppu->n_spr * 4], &ppu->oam[i * 4], 4);
            ppu->spr_is_zero[ppu->n_spr] = (i == 0);
            if (i == 0) ppu->spr0_next = true;
        } else {
            ppu->ppustatus |= PPUSTAT_SPR_OVF;
            break;
        }
        ppu->n_spr++;
    }
}

static void load_sprite_shifters(Rp2c02 *ppu) {
    int target = ppu->scanline + 1;
    if (target >= ppu->lines_total) target = 0;

    for (int i = 0; i < (int)ppu->n_spr; i++) {
        uint8_t spr_y = ppu->oam2[i * 4 + 0];
        uint8_t tile = ppu->oam2[i * 4 + 1];
        uint8_t attr = ppu->oam2[i * 4 + 2];
        uint8_t spr_x = ppu->oam2[i * 4 + 3];
        bool flip_v = (attr & 0x80u) != 0;
        bool flip_h = (attr & 0x40u) != 0;
        int row = target - (((int)spr_y + 1) & 0xFF);

        uint16_t addr;
        if (ppu->ppuctrl & PPUCTRL_SPR_8x16) {
            uint16_t base = (tile & 1u) ? 0x1000u : 0x0000u;
            uint8_t t = tile & 0xFEu;
            if (flip_v) row ^= 15;
            if (row >= 8) {
                row -= 8;
                t++;
            }
            addr = (uint16_t)(base + ((uint16_t)t << 4) + (uint8_t)row);
        } else {
            uint16_t base = (ppu->ppuctrl & PPUCTRL_SPR_PT) ? 0x1000u : 0x0000u;
            if (flip_v) row ^= 7;
            addr = (uint16_t)(base + ((uint16_t)tile << 4) + (uint8_t)row);
        }

        uint8_t lo = ppu->chr_read ? ppu->chr_read(addr, ppu->chr_ud) : 0;
        uint8_t hi = ppu->chr_read ? ppu->chr_read((uint16_t)(addr + 8), ppu->chr_ud) : 0;
        if (flip_h) {
            lo = bit_reverse(lo);
            hi = bit_reverse(hi);
        }

        ppu->spr_shift_lo[i] = lo;
        ppu->spr_shift_hi[i] = hi;
        ppu->spr_attr[i] = attr;
        ppu->spr_x[i] = spr_x;
    }

    for (int i = (int)ppu->n_spr; i < 8; i++) {
        ppu->spr_shift_lo[i] = 0;
        ppu->spr_shift_hi[i] = 0;
        ppu->spr_attr[i] = 0;
        ppu->spr_x[i] = 0xFFu;
        ppu->spr_is_zero[i] = false;
    }

    ppu->spr0_active = ppu->spr0_next;
}

static uint8_t output_pixel(Rp2c02 *ppu, int x) {
    uint8_t bg_px = 0;
    uint8_t bg_pal = 0;

    if ((ppu->ppumask & PPUMASK_SHOW_BG) &&
        (x >= 8 || (ppu->ppumask & PPUMASK_BG_LEFT))) {
        uint16_t bit = (uint16_t)(0x8000u >> ppu->x);
        bg_px = (uint8_t)(((ppu->bg_shift_lo & bit) ? 1 : 0) |
                          ((ppu->bg_shift_hi & bit) ? 2 : 0));
        bg_pal = (uint8_t)(((ppu->bg_attr_lo & bit) ? 1 : 0) |
                           ((ppu->bg_attr_hi & bit) ? 2 : 0));
    }

    uint8_t spr_px = 0;
    uint8_t spr_pal = 0;
    bool spr_front = false;
    bool spr0_candidate = false;

    if ((ppu->ppumask & PPUMASK_SHOW_SPR) &&
        (x >= 8 || (ppu->ppumask & PPUMASK_SPR_LEFT))) {
        for (int i = 0; i < (int)ppu->n_spr; i++) {
            if (ppu->spr_x[i] != 0) continue;

            uint8_t lo = (ppu->spr_shift_lo[i] >> 7) & 1u;
            uint8_t hi = (ppu->spr_shift_hi[i] >> 7) & 1u;
            uint8_t px = (uint8_t)(lo | (hi << 1));
            if (!px) continue;

            spr_px = px;
            spr_pal = (uint8_t)((ppu->spr_attr[i] & 3u) + 4u);
            spr_front = (ppu->spr_attr[i] & 0x20u) == 0;
            spr0_candidate = ppu->spr_is_zero[i];
            break;
        }
    }

    if (spr0_candidate && bg_px && x < 255) {
        if (!(ppu->ppustatus & PPUSTAT_SPR0_HIT))
            PPU_LOG("SPR0 HIT at x=%d\n", x);
        ppu->ppustatus |= PPUSTAT_SPR0_HIT;
    }

    uint8_t px;
    uint8_t pal;
    if (!bg_px && !spr_px) {
        px = 0;
        pal = 0;
    } else if (!bg_px) {
        px = spr_px;
        pal = spr_pal;
    } else if (!spr_px) {
        px = bg_px;
        pal = bg_pal;
    } else if (spr_front) {
        px = spr_px;
        pal = spr_pal;
    } else {
        px = bg_px;
        pal = bg_pal;
    }

    return palette_rd(ppu, px ? (uint8_t)((pal << 2) | px) : 0);
}

void rp2c02_tick(Rp2c02 *ppu) {
    int sl = ppu->scanline;
    int dot = ppu->dot;
    bool rendering = rendering_enabled(ppu);
    bool visible = sl >= 0 && sl < 240;
    bool prerender = sl == ppu->lines_total - 1;
    bool active = visible || prerender;

    if (sl == ppu->vblank_line && dot == 1) {
        ppu->ppustatus |= PPUSTAT_VBLANK;
        if (ppu->ppuctrl & PPUCTRL_NMI_EN) {
            ppu->nmi_pending = true;
            PPU_LOG("NMI fired (vblank)\n");
        }
    }

    if (prerender && dot == 1) {
        ppu->ppustatus &= (uint8_t)~(PPUSTAT_VBLANK | PPUSTAT_SPR0_HIT | PPUSTAT_SPR_OVF);
        ppu->nmi_pending = false;
    }

    if (rendering && active) {
        bool fetch = (dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 336);
        if (fetch) {
            bg_shift_tick(ppu);
            bg_fetch(ppu, dot);
        }
        if (dot == 256) incr_y(ppu);
        if (dot == 257) {
            copy_x_from_t(ppu);
            evaluate_sprites(ppu);
            load_sprite_shifters(ppu);
        }
        if (prerender && dot >= 280 && dot <= 304)
            copy_y_from_t(ppu);
    }

    if (visible && dot >= 1 && dot <= 256) {
        size_t idx = (size_t)sl * RP2C02_WIDTH + (size_t)(dot - 1);
        uint8_t color = output_pixel(ppu, dot - 1);
        ppu->pixels[idx] = color;
        const uint32_t *pal = ppu->alt_palette_rgb ? ppu->alt_palette_rgb : rp2c02_palette_rgb;
        ppu->pixels_argb[idx] = pal[color & 0x3Fu];
    }

    if (rendering && visible && dot >= 1 && dot <= 256) {
        for (int i = 0; i < 8; i++) {
            if (ppu->spr_x[i] > 0) {
                ppu->spr_x[i]--;
            } else {
                ppu->spr_shift_lo[i] <<= 1;
                ppu->spr_shift_hi[i] <<= 1;
            }
        }
    }

    if (dot == 260 && active && rendering && ppu->irq_scanline)
        ppu->irq_scanline(ppu->irq_ud);

    ppu->dot++;
    if (ppu->dot == 341 || (ppu->dot == 340 && prerender && ppu->odd_frame && rendering)) {
        ppu->dot = 0;
        ppu->scanline++;
        if (ppu->scanline == ppu->lines_total) {
            ppu->scanline = 0;
            ppu->odd_frame = !ppu->odd_frame;
            ppu->frame++;
            ppu->dirty = true;
        }
    }
}

void rp2c02_reset(Rp2c02 *ppu) {
    ppu->ppuctrl = 0;
    ppu->ppumask = 0;
    ppu->ppumask_pending = 0;
    ppu->ppumask_delay = 0;
    ppu->ppustatus &= PPUSTAT_VBLANK;
    ppu->v = 0;
    ppu->t = 0;
    ppu->x = 0;
    ppu->w = false;
    ppu->oamaddr = 0;
    ppu->read_buf = 0;
    ppu->open_bus = 0;
    ppu->scanline = ppu->lines_total - 1;
    ppu->dot = 0;
    ppu->odd_frame = false;
    ppu->nmi_pending = false;
    ppu->dirty = false;
    ppu->nt_latch = 0;
    ppu->at_latch = 0;
    ppu->pt_lo_latch = 0;
    ppu->pt_hi_latch = 0;
    ppu->bg_shift_lo = 0;
    ppu->bg_shift_hi = 0;
    ppu->bg_attr_lo = 0;
    ppu->bg_attr_hi = 0;
    ppu->at_latch_lo = 0;
    ppu->at_latch_hi = 0;
    memset(ppu->oam2, 0xFF, sizeof(ppu->oam2));
    memset(ppu->spr_shift_lo, 0, sizeof(ppu->spr_shift_lo));
    memset(ppu->spr_shift_hi, 0, sizeof(ppu->spr_shift_hi));
    memset(ppu->spr_attr, 0, sizeof(ppu->spr_attr));
    memset(ppu->spr_x, 0xFF, sizeof(ppu->spr_x));
    memset(ppu->spr_is_zero, 0, sizeof(ppu->spr_is_zero));
    ppu->n_spr = 0;
    ppu->spr0_next = false;
    ppu->spr0_active = false;
}

void rp2c02_init(Rp2c02 *ppu, bool is_pal) {
    memset(ppu, 0, sizeof(*ppu));
    ppu->mirror      = RP2C02_MIRROR_VERTICAL;
    ppu->lines_total = is_pal ? 312 : 262;
    ppu->vblank_line = 241;   /* overridden to 291 by caller for Dendy */
    rp2c02_reset(ppu);
}
