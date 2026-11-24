/* NES mapper 1: Nintendo MMC1 (SxROM). */
#include "cart/nes_cart_internal.h"
#include <stdio.h>

static void mmc1_update(NesCart *c) {
    NesMmc1 *m = &c->m.mmc1;
    uint8_t  prg_mode = (m->ctrl >> 2) & 3;
    uint8_t  chr_mode = (m->ctrl >> 4) & 1;
    uint8_t  prg_bank =  m->prg  & 0x0F;
    uint32_t prg_size = nc_prg_size(c);
    uint32_t chr_size = nc_chr_size(c);

    switch (prg_mode) {
    case 0: case 1: {   /* 32 KB: both halves together */
        uint32_t off = ((uint32_t)(prg_bank & ~1u) * 0x4000u) % prg_size;
        nc_prg16_at(c, 0, off);
        nc_prg16_at(c, 1, off + 0x4000u);
        break;
    }
    case 2:             /* first bank fixed at $8000, $C000 switched */
        nc_prg16_at(c, 0, 0);
        nc_prg16_at(c, 1, ((uint32_t)prg_bank * 0x4000u) % prg_size);
        break;
    case 3:             /* $8000 switched, last bank fixed at $C000 */
        nc_prg16_at(c, 0, ((uint32_t)prg_bank * 0x4000u) % prg_size);
        nc_prg16_at(c, 1, prg_size - 0x4000u);
        break;
    }

    if (chr_mode == 0) {   /* 8 KB */
        uint32_t off = ((uint32_t)(m->chr0 & ~1u) * 0x1000u) % chr_size;
        nc_chr4_at(c, 0, off);
        nc_chr4_at(c, 1, off + 0x1000u);
    } else {               /* two 4 KB */
        nc_chr4_at(c, 0, ((uint32_t)m->chr0 * 0x1000u) % chr_size);
        nc_chr4_at(c, 1, ((uint32_t)m->chr1 * 0x1000u) % chr_size);
    }

    static const uint8_t mirror_map[] = {
        RP2C02_MIRROR_SINGLE_A, RP2C02_MIRROR_SINGLE_B,
        RP2C02_MIRROR_VERTICAL, RP2C02_MIRROR_HORIZONTAL
    };
    c->ppu->mirror = mirror_map[m->ctrl & 3];
}

static void mmc1_reset(NesCart *c) {
    c->m.mmc1 = (NesMmc1){ .ctrl = 0x0C };   /* PRG mode 3: last bank at $C000 */
    mmc1_update(c);
}

static bool mmc1_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    NesMmc1 *m = &c->m.mmc1;
    if (val & 0x80) {          /* reset: PRG mode 3 */
        m->shift = 0;
        m->shift_count = 0;
        m->ctrl |= 0x0C;
        mmc1_update(c);
        return true;
    }
    /* bit 0 shifts in from the top; the fifth write loads the register */
    m->shift = (uint8_t)((m->shift >> 1) | ((val & 1u) << 4));
    if (++m->shift_count < 5) return true;
    switch ((addr >> 13) & 3) {
    case 0: m->ctrl = m->shift; break;
    case 1: m->chr0 = m->shift; break;
    case 2: m->chr1 = m->shift; break;
    case 3: m->prg  = m->shift; break;
    }
    m->shift = 0;
    m->shift_count = 0;
    mmc1_update(c);
    return true;
}

/* PRG register bit 4 disables the RAM */
static bool mmc1_prg_ram(const NesCart *c) { return !(c->m.mmc1.prg & 0x10); }

static void mmc1_dump(const NesCart *c, char *buf, size_t len) {
    const NesMmc1 *m = &c->m.mmc1;
    snprintf(buf, len, "ctrl=%02X chr0=%02X chr1=%02X prg=%02X shift=%02X/%u\n",
             m->ctrl, m->chr0, m->chr1, m->prg, m->shift, m->shift_count);
}

const NesMapper nes_mapper_mmc1 = {
    1, "MMC1", .reset = mmc1_reset, .write = mmc1_write,
    .prg_ram = mmc1_prg_ram, .dump = mmc1_dump,
};
