/* NES mapper 5: Nintendo MMC5 (ExROM). */
#include "cart/nes_cart_internal.h"
#include <stdio.h>
#include <string.h>

static void mmc5_update_prg(NesCart *c) {
    NesMmc5 *m = &c->m.mmc5;
    uint32_t size = nc_prg_size(c);
    uint32_t unit = 0x2000u;
    uint8_t *r    = m->prg_regs;   /* r[0] = $5113 ... r[4] = $5117 */

    for (int i = 0; i < 4; i++) m->prg_is_ram[i] = false;

    switch (m->prg_mode) {
    case 0: { /* 32KB from $5117 (always ROM) */
        uint32_t base = ((uint32_t)(r[4] & 0x7Cu) * unit) % size;
        for (int i = 0; i < 4; i++) c->prg_off[i] = base + (uint32_t)i * unit;
        break;
    }
    case 1: { /* 16KB from $5115 at $8000, 16KB from $5117 at $C000 */
        if (!(r[2] & 0x80u)) m->prg_is_ram[0] = m->prg_is_ram[1] = true;
        uint32_t b0 = ((uint32_t)(r[2] & 0x7Eu) * unit) % size;
        uint32_t b1 = ((uint32_t)(r[4] & 0x7Eu) * unit) % size;
        c->prg_off[0] = b0;  c->prg_off[1] = b0 + unit;
        c->prg_off[2] = b1;  c->prg_off[3] = b1 + unit;
        break;
    }
    case 2: { /* 16KB from $5115, 8KB from $5116, 8KB from $5117 */
        if (!(r[2] & 0x80u)) m->prg_is_ram[0] = m->prg_is_ram[1] = true;
        if (!(r[3] & 0x80u)) m->prg_is_ram[2] = true;
        uint32_t b0 = ((uint32_t)(r[2] & 0x7Eu) * unit) % size;
        c->prg_off[0] = b0;  c->prg_off[1] = b0 + unit;
        c->prg_off[2] = ((uint32_t)(r[3] & 0x7Fu) * unit) % size;
        c->prg_off[3] = ((uint32_t)(r[4] & 0x7Fu) * unit) % size;
        break;
    }
    case 3: /* four 8KB banks from $5114-$5117 */
        for (int i = 0; i < 4; i++) {
            uint8_t reg = r[i + 1];
            if (i < 3 && !(reg & 0x80u)) m->prg_is_ram[i] = true;
            c->prg_off[i] = ((uint32_t)(reg & 0x7Fu) * unit) % size;
        }
        break;
    }
}

static uint8_t mmc5_prg_read(const NesCart *c, uint16_t addr) {
    unsigned slot = (addr >> 13) & 3u;
    if (c->m.mmc5.prg_is_ram[slot])
        return c->prg_ram[(c->prg_off[slot] + (addr & 0x1FFFu)) & 0x1FFFu];
    return c->prg[c->prg_off[slot] + (addr & 0x1FFFu)];
}

/* The 1 KB CHR offset for a register and a sub-slot. */
static uint32_t chr1kb(const NesCart *c, uint8_t reg_idx, uint8_t sub) {
    const NesMmc5 *m = &c->m.mmc5;
    uint16_t bank = (uint16_t)(((uint16_t)(m->chr_hi & 3u) << 8) | m->chr_regs[reg_idx]);
    return ((uint32_t)(bank + sub) * 0x400u) % nc_chr_size(c);
}

static uint8_t mmc5_chr_read(NesCart *c, uint16_t addr) {
    if (c->chr_is_ram) return c->chr[addr & 0x1FFFu];
    const NesMmc5 *m = &c->m.mmc5;
    const Rp2c02 *ppu = c->ppu;
    bool rendering = (ppu->ppumask & (PPUMASK_SHOW_BG | PPUMASK_SHOW_SPR)) &&
                     (ppu->scanline >= 0 && ppu->scanline < 240);

    /* Extended attributes: BG uses ExRAM bank bits instead */
    bool is_spr_fetch = rendering && ppu->dot == 257 && (ppu->ppuctrl & PPUCTRL_SPR_8x16);
    if (!is_spr_fetch && rendering && m->exram_mode == 1) {
        uint8_t bank8 = (uint8_t)(((m->chr_hi & 3u) << 6) | (m->exattr_latch & 0x3Fu));
        return c->chr[((uint32_t)bank8 * 0x1000u + (addr & 0x0FFFu)) % nc_chr_size(c)];
    }

    /* Sprite or BG banks */
    bool use_spr;
    if (ppu->ppuctrl & PPUCTRL_SPR_8x16)
        use_spr = is_spr_fetch;     /* 8x16: sprites at dot 257, BG elsewhere */
    else
        use_spr = !m->last_chr_bg;  /* 8x8: whichever set was written last */

    if (use_spr) {
        /* $5120-$5127 (0-7): 8 x 1KB */
        uint8_t s8 = (addr >> 10) & 7u;
        switch (m->chr_mode) {
        case 0: return c->chr[chr1kb(c, 7, s8) + (addr & 0x3FFu)];
        case 1: return c->chr[chr1kb(c, s8 >= 4 ? 7 : 3, s8 & 3u) + (addr & 0x3FFu)];
        case 2: return c->chr[chr1kb(c, (s8 >> 1) * 2u + 1u, s8 & 1u) + (addr & 0x3FFu)];
        case 3: return c->chr[chr1kb(c, s8, 0) + (addr & 0x3FFu)];
        }
    } else {
        /* $5128-$512B (8-11): 4 x 1KB, mirrored to $0000-$1FFF */
        uint8_t s4 = (addr >> 10) & 3u;
        switch (m->chr_mode) {
        case 0: return c->chr[chr1kb(c, 11, s4) + (addr & 0x3FFu)];
        case 1: return c->chr[chr1kb(c, 11, s4) + (addr & 0x3FFu)];
        case 2: return c->chr[chr1kb(c, (s4 >> 1) * 2u + 9u, s4 & 1u) + (addr & 0x3FFu)];
        case 3: return c->chr[chr1kb(c, 8u + s4, 0) + (addr & 0x3FFu)];
        }
    }
    return 0;
}

static uint8_t mmc5_nt_read(NesCart *c, uint16_t addr) {
    NesMmc5 *m = &c->m.mmc5;
    uint8_t  nt   = (addr >> 10) & 3u;
    uint8_t  mode = (m->nt_map >> (nt * 2u)) & 3u;
    uint16_t off  = addr & 0x3FFu;

    /* Tile fetches latch the ExRAM byte (extended attribute CHR banking) */
    if (off < 0x3C0u && m->exram_mode == 1)
        m->exattr_latch = m->exram[off];

    /* Extended attributes: attribute fetches get ExRAM's palette bits */
    if (off >= 0x3C0u && m->exram_mode == 1) {
        uint8_t pal   = (m->exattr_latch >> 6) & 3u;
        uint8_t shift = (uint8_t)(((c->ppu->v >> 4) & 4u) | (c->ppu->v & 2u));
        return (uint8_t)(pal << shift);
    }

    switch (mode) {
    case 0: return c->ppu->vram[off];
    case 1: return c->ppu->vram[0x400u + off];
    case 2: return m->exram_mode >= 2 ? m->exram[off & 0x3FFu] : 0;
    case 3: /* fill mode */
        return off >= 0x3C0u ? (uint8_t)((m->fill_attr & 3u) * 0x55u) : m->fill_tile;
    }
    return 0;
}

static void mmc5_nt_write(NesCart *c, uint16_t addr, uint8_t val) {
    NesMmc5 *m = &c->m.mmc5;
    uint8_t  nt   = (addr >> 10) & 3u;
    uint8_t  mode = (m->nt_map >> (nt * 2u)) & 3u;
    uint16_t off  = addr & 0x3FFu;
    switch (mode) {
    case 0: c->ppu->vram[off]          = val; break;
    case 1: c->ppu->vram[0x400u + off] = val; break;
    case 2: m->exram[off & 0x3FFu]     = val; break;
    case 3: break;   /* fill mode: writes ignored */
    }
}

static void mmc5_scanline(NesCart *c) {
    NesMmc5 *m = &c->m.mmc5;
    if (c->ppu->scanline >= 240) {
        m->in_frame = false;
        return;
    }
    if (!m->in_frame) {
        m->in_frame    = true;
        m->irq_counter = 0;
    } else {
        m->irq_counter++;
    }
    if (m->irq_target > 0 && m->irq_counter == m->irq_target) {
        m->irq_pending = true;
        if (m->irq_enabled) nc_irq_raise(c);
    }
}

/* Reset keeps ExRAM, the fill tile, the IRQ target and the multiplier. */
static void mmc5_reset(NesCart *c) {
    NesMmc5 *m = &c->m.mmc5;
    m->prg_mode     = 3;    /* four 8KB banks */
    m->chr_mode     = 3;    /* 1KB CHR banks */
    m->exram_mode   = 3;    /* read-only */
    m->nt_map       = 0;    /* all CIRAM page 0 */
    m->irq_enabled  = false;
    m->irq_pending  = false;
    m->in_frame     = false;
    m->irq_counter  = 0;
    m->last_chr_bg  = false;
    m->exattr_latch = 0;
    memset(m->prg_regs, 0xFF, sizeof(m->prg_regs));   /* $5117 = $FF: last bank at $E000 */
    memset(m->chr_regs, 0,    sizeof(m->chr_regs));
    m->chr_hi = 0;
    mmc5_update_prg(c);
}

static bool mmc5_write(NesCart *c, uint16_t addr, uint8_t val) {
    NesMmc5 *m = &c->m.mmc5;
    if (addr < 0x5000 || addr >= 0x6000) return false;
    if (addr == 0x5100) { m->prg_mode = val & 3u; mmc5_update_prg(c); return true; }
    if (addr == 0x5101) { m->chr_mode = val & 3u; return true; }
    /* $5102/$5103: PRG-RAM protect, not enforced */
    if (addr == 0x5104) { m->exram_mode = val & 3u; return true; }
    if (addr == 0x5105) { m->nt_map = val; return true; }
    if (addr == 0x5106) { m->fill_tile = val; return true; }
    if (addr == 0x5107) { m->fill_attr = val & 3u; return true; }
    if (addr >= 0x5113 && addr <= 0x5117) {
        m->prg_regs[addr - 0x5113u] = val;
        mmc5_update_prg(c);
        return true;
    }
    if (addr >= 0x5120 && addr <= 0x5127) {
        m->chr_regs[addr - 0x5120u] = val;
        m->last_chr_bg = false;
        return true;
    }
    if (addr >= 0x5128 && addr <= 0x512B) {
        m->chr_regs[8u + (addr - 0x5128u)] = val;
        m->last_chr_bg = true;
        return true;
    }
    if (addr == 0x5130) { m->chr_hi = val & 3u; return true; }
    if (addr == 0x5203) { m->irq_target = val; return true; }
    if (addr == 0x5204) {
        m->irq_enabled = (val & 0x80u) != 0;
        if (!m->irq_enabled) { m->irq_pending = false; nc_irq_ack(c); }
        return true;
    }
    if (addr == 0x5205) { m->mul[0] = val; return true; }
    if (addr == 0x5206) { m->mul[1] = val; return true; }
    if (addr >= 0x5C00 && m->exram_mode <= 2) m->exram[addr - 0x5C00u] = val;
    return true;
}

static bool mmc5_read(NesCart *c, uint16_t addr, uint8_t *val) {
    NesMmc5 *m = &c->m.mmc5;
    if (addr < 0x5000 || addr >= 0x6000) return false;
    if (addr == 0x5204) {
        *val = (uint8_t)((m->irq_pending ? 0x80u : 0u) | (m->in_frame ? 0x40u : 0u));
        m->irq_pending = false;
        nc_irq_ack(c);
        return true;
    }
    if (addr == 0x5205) { *val = (uint8_t)((uint16_t)m->mul[0] * m->mul[1]); return true; }
    if (addr == 0x5206) { *val = (uint8_t)(((uint16_t)m->mul[0] * m->mul[1]) >> 8); return true; }
    if (addr >= 0x5C00 && m->exram_mode >= 2) { *val = m->exram[addr - 0x5C00u]; return true; }
    return false;   /* open bus */
}

static bool mmc5_peek(const NesCart *c, uint16_t addr, uint8_t *val) {
    const NesMmc5 *m = &c->m.mmc5;
    if (addr == 0x5205) { *val = (uint8_t)((uint16_t)m->mul[0] * m->mul[1]); return true; }
    if (addr == 0x5206) { *val = (uint8_t)(((uint16_t)m->mul[0] * m->mul[1]) >> 8); return true; }
    if (addr >= 0x5C00 && addr < 0x6000 && m->exram_mode >= 2) {
        *val = m->exram[addr - 0x5C00u];
        return true;
    }
    return false;
}

/* $6000-$7FFF is always RAM ($5113's bank; one 8 KB bank here) */
static bool always(const NesCart *c) { (void)c; return true; }

static void mmc5_dump(const NesCart *c, char *buf, size_t len) {
    const NesMmc5 *m = &c->m.mmc5;
    snprintf(buf, len, "prg_mode=%u chr_mode=%u exram_mode=%u nt_map=%02X "
             "prg=%02X %02X %02X %02X %02X chr_hi=%u\n"
             "irq target=%u counter=%u enabled=%d pending=%d in_frame=%d\n",
             m->prg_mode, m->chr_mode, m->exram_mode, m->nt_map,
             m->prg_regs[0], m->prg_regs[1], m->prg_regs[2], m->prg_regs[3], m->prg_regs[4],
             m->chr_hi, m->irq_target, m->irq_counter, m->irq_enabled, m->irq_pending,
             m->in_frame);
}

const NesMapper nes_mapper_mmc5 = {
    5, "MMC5", .reset = mmc5_reset, .write = mmc5_write, .read = mmc5_read,
    .prg_ram = always, .prg_read = mmc5_prg_read, .chr_read = mmc5_chr_read,
    .nt_read = mmc5_nt_read, .nt_write = mmc5_nt_write, .scanline = mmc5_scanline,
    .peek = mmc5_peek, .dump = mmc5_dump,
};
