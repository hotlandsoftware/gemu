/* NES mapper 9: Nintendo MMC2 (PxROM). */
#include "cart/nes_cart_internal.h"
#include <stdio.h>

/* $8000 switched (no wrap), the last three 8 KB banks fixed above. */
static void mmc2_update_prg(NesCart *c) {
    uint32_t n = nc_prg_8k(c);
    c->prg_off[0] = (uint32_t)(c->m.mmc2.prg_reg & 0x0Fu) * 0x2000u;
    c->prg_off[1] = (n - 3u) * 0x2000u;
    c->prg_off[2] = (n - 2u) * 0x2000u;
    c->prg_off[3] = (n - 1u) * 0x2000u;
}

static void mmc2_reset(NesCart *c) {
    c->m.mmc2 = (NesMmc2){0};
    mmc2_update_prg(c);
}

/* Register map (CPU A14..A12):
 *   $A000-$AFFF  PRG bank (bits 3:0; this board decodes $8000-$9FFF too)
 *   $B000-$BFFF  left CHR,  FD latch (bits 4:0)
 *   $C000-$CFFF  left CHR,  FE latch
 *   $D000-$DFFF  right CHR, FD latch
 *   $E000-$EFFF  right CHR, FE latch
 *   $F000-$FFFF  mirroring (bit 0: 0 vertical, 1 horizontal) */
static bool mmc2_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    NesMmc2 *m = &c->m.mmc2;
    if (addr < 0xB000)      { m->prg_reg = val & 0x0Fu; mmc2_update_prg(c); }
    else if (addr < 0xC000) m->chr_lfd = val & 0x1Fu;
    else if (addr < 0xD000) m->chr_lfe = val & 0x1Fu;
    else if (addr < 0xE000) m->chr_rfd = val & 0x1Fu;
    else if (addr < 0xF000) m->chr_rfe = val & 0x1Fu;
    else c->ppu->mirror = (val & 1u) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
    return true;
}

/* 4 KB CHR windows picked by latches.  A latch changes after the fetch
 * that trips it: the $FD/$FE tile itself still uses the old bank. */
static uint8_t mmc2_chr_read(NesCart *c, uint16_t addr) {
    if (c->chr_is_ram) return c->chr[addr & 0x1FFF];
    NesMmc2 *m = &c->m.mmc2;
    bool     right = (addr & 0x1000u) != 0;
    uint16_t tile  = addr & 0x0FF8u;
    uint8_t  bank  = right ? (m->latch_r ? m->chr_rfe : m->chr_rfd)
                           : (m->latch_l ? m->chr_lfe : m->chr_lfd);
    uint8_t data = c->chr[((uint32_t)bank * 0x1000u + (addr & 0x0FFFu)) % nc_chr_size(c)];
    /* only the high bitplane fetch ($xFD8-$xFDF / $xFE8-$xFEF) trips it */
    if (tile == 0x0FD8u) {
        if (right) m->latch_r = false; else m->latch_l = false;
    } else if (tile == 0x0FE8u) {
        if (right) m->latch_r = true;  else m->latch_l = true;
    }
    return data;
}

static void mmc2_dump(const NesCart *c, char *buf, size_t len) {
    const NesMmc2 *m = &c->m.mmc2;
    snprintf(buf, len, "prg=%02X left=%02X/%02X right=%02X/%02X latches=%c%c\n",
             m->prg_reg, m->chr_lfd, m->chr_lfe, m->chr_rfd, m->chr_rfe,
             m->latch_l ? 'E' : 'D', m->latch_r ? 'E' : 'D');
}

const NesMapper nes_mapper_mmc2 = {
    9, "MMC2", .reset = mmc2_reset, .write = mmc2_write,
    .chr_read = mmc2_chr_read, .dump = mmc2_dump,
};
