#pragma once
/* Shared by the NES board files: banking helpers over NesCart's windows. */
#include "cart/nes_cart.h"

static inline unsigned nc_prg_8k(const NesCart *c)  { return (unsigned)c->info.prg_banks * 2u; }
static inline unsigned nc_chr_1k(const NesCart *c)  { return (unsigned)c->info.chr_banks * 8u; }
static inline unsigned nc_last_8k(const NesCart *c) { return nc_prg_8k(c) - 1u; }
static inline uint32_t nc_prg_size(const NesCart *c) { return (uint32_t)c->info.prg_banks * 0x4000u; }
static inline uint32_t nc_chr_size(const NesCart *c) { return (uint32_t)c->info.chr_banks * 0x2000u; }

/* Windows by number, wrapped to the ROM. */
static inline void nc_prg8(NesCart *c, int slot, unsigned bank) {
    c->prg_off[slot] = (bank % nc_prg_8k(c)) * 0x2000u;
}
static inline void nc_prg16(NesCart *c, int half, unsigned bank) {
    nc_prg8(c, half * 2,     bank * 2u);
    nc_prg8(c, half * 2 + 1, bank * 2u + 1u);
}
static inline void nc_prg32(NesCart *c, unsigned bank) {
    for (int i = 0; i < 4; i++) nc_prg8(c, i, bank * 4u + (unsigned)i);
}
static inline void nc_chr1(NesCart *c, int slot, unsigned bank) {
    c->chr_off[slot] = (bank % nc_chr_1k(c)) * 0x400u;
}
static inline void nc_chr2(NesCart *c, int slot2, unsigned bank) {
    nc_chr1(c, slot2 * 2,     bank * 2u);
    nc_chr1(c, slot2 * 2 + 1, bank * 2u + 1u);
}
static inline void nc_chr4(NesCart *c, int half, unsigned bank) {
    for (int i = 0; i < 4; i++) nc_chr1(c, half * 4 + i, bank * 4u + (unsigned)i);
}
static inline void nc_chr8(NesCart *c, unsigned bank) {
    for (int i = 0; i < 8; i++) nc_chr1(c, i, bank * 8u + (unsigned)i);
}

/* Windows by byte offset, unwrapped: a 16 KB PRG half, a 4 KB / 8 KB CHR
 * area. */
static inline void nc_prg16_at(NesCart *c, int half, uint32_t off) {
    c->prg_off[half * 2] = off;
    c->prg_off[half * 2 + 1] = off + 0x2000u;
}
static inline void nc_chr4_at(NesCart *c, int half, uint32_t off) {
    for (int i = 0; i < 4; i++) c->chr_off[half * 4 + i] = off + (uint32_t)i * 0x400u;
}

/* The ROM byte under a write, for boards with bus conflicts. */
static inline uint8_t nc_rom_at(const NesCart *c, uint16_t addr) {
    return c->prg[c->prg_off[(addr >> 13) & 3u] + (addr & 0x1FFFu)];
}

/* The CHR windows read directly. */
static inline uint8_t nc_chr_win(const NesCart *c, uint16_t addr) {
    return c->chr[c->chr_off[(addr >> 10) & 7u] + (addr & 0x3FFu)];
}

/* Two-bit mirroring code used by several boards: V, H, single A, single B. */
static inline void nc_mirror_vhab(NesCart *c, unsigned m) {
    static const uint8_t map[] = {
        RP2C02_MIRROR_VERTICAL, RP2C02_MIRROR_HORIZONTAL,
        RP2C02_MIRROR_SINGLE_A, RP2C02_MIRROR_SINGLE_B,
    };
    c->ppu->mirror = map[m & 3u];
}

/* H/V from the board, unless the cartridge has four-screen VRAM. */
static inline void nc_mirror_hv(NesCart *c, bool horizontal) {
    if (c->info.mirror != RP2C02_MIRROR_4SCREEN)
        c->ppu->mirror = horizontal ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
}

static inline void nc_set_irq(NesCart *c, bool level) {
    c->irq = level;
    if (c->irq_changed) c->irq_changed(c->ud);
}
static inline void nc_irq_raise(NesCart *c) { nc_set_irq(c, true); }
static inline void nc_irq_ack(NesCart *c)   { nc_set_irq(c, false); }

/* The board files' tables. */
extern const NesMapper nes_mapper_nrom, nes_mapper_uxrom, nes_mapper_cnrom,
                       nes_mapper_axrom, nes_mapper_gxrom, nes_mapper_m178,
                       nes_mapper_m228, nes_mapper_vs;
extern const NesMapper nes_mapper_mmc1, nes_mapper_mmc2, nes_mapper_mmc3,
                       nes_mapper_tqrom, nes_mapper_mmc5, nes_mapper_vrc6a,
                       nes_mapper_vrc6b;
/* nes_boards.c: NULL-terminated */
extern const NesMapper *const nes_boards[];
