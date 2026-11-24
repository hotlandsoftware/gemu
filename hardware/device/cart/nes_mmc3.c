/* NES mappers 4 and 119: Nintendo MMC3 (TxROM), and TQROM (MMC3 with 8 KB
 * of CHR RAM beside the CHR ROM). */
#include "cart/nes_cart_internal.h"
#include <stdio.h>

static void mmc3_update(NesCart *c) {
    NesMmc3 *m = &c->m.mmc3;
    uint32_t prg_8k = nc_prg_8k(c);
    uint32_t chr_1k = nc_chr_1k(c);

    /* PRG: four 8 KB windows; R6 at $8000 or $C000, R7 at $A000, the
     * second-to-last bank in the other, the last at $E000 */
    uint32_t r6 = (m->regs[6] % prg_8k) * 0x2000u;
    uint32_t r7 = (m->regs[7] % prg_8k) * 0x2000u;
    uint32_t f0 = (prg_8k - 2) * 0x2000u;
    uint32_t f1 = (prg_8k - 1) * 0x2000u;
    bool prg_mode = (m->bank_sel >> 6) & 1;
    c->prg_off[0] = prg_mode ? f0 : r6;
    c->prg_off[1] = r7;
    c->prg_off[2] = prg_mode ? r6 : f0;
    c->prg_off[3] = f1;

    /* CHR: R0/R1 2 KB at $0000/$0800 (bit 0 ignored), R2-R5 1 KB at
     * $1000-$1FFF; the invert bit swaps the halves. */
    if (c->chr_is_ram || chr_1k == 0) return;
    bool inv = (m->bank_sel >> 7) & 1;
    uint8_t bank[8] = {
        (uint8_t)(m->regs[0] & 0xFEu), (uint8_t)(m->regs[0] | 1u),
        (uint8_t)(m->regs[1] & 0xFEu), (uint8_t)(m->regs[1] | 1u),
        m->regs[2], m->regs[3], m->regs[4], m->regs[5],
    };
    bool tqrom = c->ops == &nes_mapper_tqrom;
    c->chr_wmask = 0;
    for (int i = 0; i < 8; i++) {
        int w = inv ? i ^ 4 : i;
        if (tqrom && (bank[i] & 0x40u)) {   /* TQROM: the CHR RAM */
            c->chr_off[w] = chr_1k * 0x400u + (bank[i] & 7u) * 0x400u;
            c->chr_wmask |= (uint8_t)(1u << w);
        } else {
            c->chr_off[w] = (bank[i] % chr_1k) * 0x400u;
        }
    }
}

static void mmc3_reset(NesCart *c) {
    c->m.mmc3 = (NesMmc3){0};
    c->irq = false;
    mmc3_update(c);
}

static bool mmc3_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    NesMmc3 *m = &c->m.mmc3;
    bool odd = (addr & 1) != 0;
    if (addr < 0xA000) {
        if (!odd) m->bank_sel = val;
        else    { m->regs[m->bank_sel & 7] = val; mmc3_update(c); }
    } else if (addr < 0xC000) {
        if (!odd) nc_mirror_hv(c, val & 1);
        /* $A001 PRG-RAM protect: ignored */
    } else if (addr < 0xE000) {
        if (!odd) m->irq_latch = val;
        else    { m->irq_reload = true; m->irq_counter = 0; }
    } else {
        if (!odd) { m->irq_enabled = false; nc_irq_ack(c); }
        else        m->irq_enabled = true;
    }
    return true;
}

/* Clocked by PPU A12 rising, once a scanline while rendering. */
static void mmc3_scanline(NesCart *c) {
    NesMmc3 *m = &c->m.mmc3;
    if (m->irq_reload || m->irq_counter == 0) {
        m->irq_counter = m->irq_latch;
        m->irq_reload  = false;
    } else {
        m->irq_counter--;
    }
    if (m->irq_counter == 0 && m->irq_enabled) nc_irq_raise(c);
}

static bool always(const NesCart *c) { (void)c; return true; }

static void mmc3_dump(const NesCart *c, char *buf, size_t len) {
    const NesMmc3 *m = &c->m.mmc3;
    snprintf(buf, len, "bank_sel=%02X regs=%02X %02X %02X %02X %02X %02X %02X %02X\n"
             "irq latch=%02X counter=%02X reload=%d enabled=%d\n",
             m->bank_sel, m->regs[0], m->regs[1], m->regs[2], m->regs[3],
             m->regs[4], m->regs[5], m->regs[6], m->regs[7],
             m->irq_latch, m->irq_counter, m->irq_reload, m->irq_enabled);
}

const NesMapper nes_mapper_mmc3 = {
    4, "MMC3", .reset = mmc3_reset, .write = mmc3_write, .prg_ram = always,
    .scanline = mmc3_scanline, .dump = mmc3_dump,
};

const NesMapper nes_mapper_tqrom = {
    119, "TQROM", .reset = mmc3_reset, .write = mmc3_write, .prg_ram = always,
    .scanline = mmc3_scanline, .dump = mmc3_dump,
};
