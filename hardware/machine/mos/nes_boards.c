/*
 * NES cartridge boards - discrete-logic and simple ASIC mappers.
 *
 * Every board here banks through the fine windows machine_nes.c already
 * reads for MMC3 (mmc3_prg_offsets: 4 x 8 KB at $8000/$A000/$C000/$E000,
 * mmc3_chr_offsets: 8 x 1 KB at PPU $0000-$1FFF), so a board is just the
 * register decoding that points those windows somewhere, plus an IRQ
 * counter for the few that have one.  Register layouts follow the NESdev
 * wiki's mapper pages.
 */
#include "nes.h"
#include "nes_boards.h"
#include <string.h>

/* ── Banking helpers ─────────────────────────────────────────────────────── */

static unsigned prg_8k_count(const NesState *s) { return (unsigned)s->cart.prg_banks * 2u; }
static unsigned chr_1k_count(const NesState *s) { return (unsigned)s->cart.chr_banks * 8u; }
static unsigned last_8k(const NesState *s)      { return prg_8k_count(s) - 1u; }

static void prg8(NesState *s, int slot, unsigned bank) {
    s->mmc3_prg_offsets[slot] = (bank % prg_8k_count(s)) * 0x2000u;
}

static void prg16(NesState *s, int half, unsigned bank) {
    prg8(s, half * 2,     bank * 2u);
    prg8(s, half * 2 + 1, bank * 2u + 1u);
}

static void prg32(NesState *s, unsigned bank) {
    for (int i = 0; i < 4; i++) prg8(s, i, bank * 4u + (unsigned)i);
}

static void chr1(NesState *s, int slot, unsigned bank) {
    s->mmc3_chr_offsets[slot] = (bank % chr_1k_count(s)) * 0x400u;
}

static void chr2(NesState *s, int slot2, unsigned bank) {
    chr1(s, slot2 * 2,     bank * 2u);
    chr1(s, slot2 * 2 + 1, bank * 2u + 1u);
}

static void chr4(NesState *s, int half, unsigned bank) {
    for (int i = 0; i < 4; i++) chr1(s, half * 4 + i, bank * 4u + (unsigned)i);
}

static void chr8(NesState *s, unsigned bank) {
    for (int i = 0; i < 8; i++) chr1(s, i, bank * 8u + (unsigned)i);
}

/* The ROM byte under a write, for boards with bus conflicts. */
static uint8_t rom_at(const NesState *s, uint16_t addr) {
    return s->prg[s->mmc3_prg_offsets[(addr - 0x8000u) >> 13] + (addr & 0x1FFFu)];
}

/* Two-bit mirroring code used by several boards: V, H, single A, single B. */
static void mirror_vhab(NesState *s, unsigned m) {
    static const uint8_t map[] = {
        RP2C02_MIRROR_VERTICAL, RP2C02_MIRROR_HORIZONTAL,
        RP2C02_MIRROR_SINGLE_A, RP2C02_MIRROR_SINGLE_B,
    };
    s->ppu.mirror = map[m & 3u];
}

static void mirror_hv(NesState *s, bool horizontal) {
    if (s->cart.mirror != RP2C02_MIRROR_4SCREEN)
        s->ppu.mirror = horizontal ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
}

static void set_nt(NesState *s, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    s->board.nt[0] = a; s->board.nt[1] = b; s->board.nt[2] = c; s->board.nt[3] = d;
}

static void irq_raise(NesState *s) { s->cart_irq = true;  nes_update_irq(s); }
static void irq_ack(NesState *s)   { s->cart_irq = false; nes_update_irq(s); }

/* ── Konami VRC IRQ (VRC4; same counter as VRC6) ─────────────────────────── */

static void vrc_irq_clock(NesState *s) {
    NesBoard *b = &s->board;
    if (b->irq_counter == 0xFF) {
        b->irq_counter = b->irq_latch;
        irq_raise(s);
    } else {
        b->irq_counter++;
    }
}

static void vrc_irq_tick(NesState *s) {
    NesBoard *b = &s->board;
    if (!b->irq_enabled) return;
    if (b->irq_mode) {
        vrc_irq_clock(s);
    } else {
        b->irq_prescaler -= 3;   /* 341 PPU dots per scanline, 3 per CPU cycle */
        if (b->irq_prescaler <= 0) {
            b->irq_prescaler += 341;
            vrc_irq_clock(s);
        }
    }
}

static void vrc_irq_write(NesState *s, unsigned reg, uint8_t val) {
    NesBoard *b = &s->board;
    switch (reg) {
    case 0: b->irq_latch = (b->irq_latch & 0xF0u) | (val & 0x0Fu); break;
    case 1: b->irq_latch = (b->irq_latch & 0x0Fu) | (uint16_t)((val & 0x0Fu) << 4); break;
    case 2:
        b->irq_after   = val & 1u;
        b->irq_enabled = (val & 2u) != 0;
        b->irq_mode    = (val & 4u) != 0;
        if (b->irq_enabled) {
            b->irq_counter   = b->irq_latch;
            b->irq_prescaler = 341;
        }
        irq_ack(s);
        break;
    case 3:
        irq_ack(s);
        b->irq_enabled = b->irq_after;
        break;
    }
}

/* ── Mapper 23: VRC2b / VRC4e / VRC4f ────────────────────────────────────── */

static void vrc4_update_prg(NesState *s) {
    NesBoard *b = &s->board;
    bool swap = (b->regs[2] & 2u) != 0;
    prg8(s, swap ? 2 : 0, b->regs[0]);
    prg8(s, swap ? 0 : 2, last_8k(s) - 1u);
    prg8(s, 1, b->regs[1]);
    prg8(s, 3, last_8k(s));
}

static void vrc4_write(NesState *s, uint16_t addr, uint8_t val) {
    NesBoard *b = &s->board;
    /* VRC2b/VRC4f use A0/A1, VRC4e A2/A3; OR-ing them serves both */
    unsigned reg = (unsigned)(((addr | (addr >> 2)) & 1u) | (((addr >> 1) | (addr >> 3)) & 1u) << 1);
    switch (addr & 0xF000u) {
    case 0x8000: b->regs[0] = val & 0x1Fu; vrc4_update_prg(s); break;
    case 0x9000:
        if (reg < 2) mirror_vhab(s, val);
        else if (reg == 2) { b->regs[2] = val; vrc4_update_prg(s); }
        break;
    case 0xA000: b->regs[1] = val & 0x1Fu; vrc4_update_prg(s); break;
    case 0xB000: case 0xC000: case 0xD000: case 0xE000: {
        int i = (int)(((addr & 0xF000u) - 0xB000u) >> 11) + (int)(reg >> 1);
        if (reg & 1u) b->chr[i] = (uint16_t)((b->chr[i] & 0x0Fu) | ((val & 0x1Fu) << 4));
        else          b->chr[i] = (uint16_t)((b->chr[i] & 0x1F0u) | (val & 0x0Fu));
        chr1(s, i, b->chr[i]);
        break;
    }
    case 0xF000: vrc_irq_write(s, reg, val); break;
    }
}

/* ── Mapper 64: Tengen RAMBO-1 ───────────────────────────────────────────── */

static void rambo_update(NesState *s) {
    NesBoard *b = &s->board;
    uint8_t *r = b->regs;
    unsigned c[8];
    if (b->cmd & 0x20u) {   /* K: 1 KB banks throughout */
        c[0] = r[0]; c[1] = r[8]; c[2] = r[1]; c[3] = r[9];
    } else {
        c[0] = r[0] & 0xFEu; c[1] = r[0] | 1u; c[2] = r[1] & 0xFEu; c[3] = r[1] | 1u;
    }
    c[4] = r[2]; c[5] = r[3]; c[6] = r[4]; c[7] = r[5];
    int inv = (b->cmd & 0x80u) ? 4 : 0;
    for (int i = 0; i < 8; i++) chr1(s, i ^ inv, c[i]);

    if (b->cmd & 0x40u) { prg8(s, 0, r[15]); prg8(s, 1, r[6]); prg8(s, 2, r[7]); }
    else                { prg8(s, 0, r[6]);  prg8(s, 1, r[7]); prg8(s, 2, r[15]); }
    prg8(s, 3, last_8k(s));
}

static void rambo_clock(NesState *s) {
    NesBoard *b = &s->board;
    if (b->irq_reload || b->irq_counter == 0) {
        b->irq_counter = b->irq_latch;
        b->irq_reload  = false;
    } else {
        b->irq_counter--;
    }
    if (b->irq_counter == 0 && b->irq_enabled) irq_raise(s);
}

static void rambo_write(NesState *s, uint16_t addr, uint8_t val) {
    NesBoard *b = &s->board;
    switch (addr & 0xE001u) {
    case 0x8000: b->cmd = val; rambo_update(s); break;
    case 0x8001: b->regs[b->cmd & 0x0Fu] = val; rambo_update(s); break;
    case 0xA000: mirror_hv(s, val & 1u); break;
    case 0xC000: b->irq_latch = val; break;
    case 0xC001:
        b->irq_mode      = val & 1u;
        b->irq_reload    = true;
        b->irq_prescaler = 0;
        break;
    case 0xE000: b->irq_enabled = false; irq_ack(s); break;
    case 0xE001: b->irq_enabled = true; break;
    }
}

/* ── Namco 108 family: 95, 206 (and the core of 112's layout) ────────────── */

static void namco108_update(NesState *s) {
    uint8_t *r = s->board.regs;
    uint8_t mask = s->cart.mapper == 95 ? 0x1Fu : 0x3Fu;   /* 95: bit 5 is the nametable */
    chr2(s, 0, (r[0] & mask) >> 1);
    chr2(s, 1, (r[1] & mask) >> 1);
    for (int i = 0; i < 4; i++) chr1(s, 4 + i, r[2 + i] & mask);
    prg8(s, 0, r[6] & 0x0Fu);
    prg8(s, 1, r[7] & 0x0Fu);
    prg8(s, 2, last_8k(s) - 1u);
    prg8(s, 3, last_8k(s));
    if (s->cart.mapper == 95) {
        uint8_t lo = (r[0] >> 5) & 1u, hi = (r[1] >> 5) & 1u;
        set_nt(s, lo, lo, hi, hi);
    }
}

/* ── Mapper 112: NTDEC / Asder ───────────────────────────────────────────── */

static void ntdec_update(NesState *s) {
    uint8_t *r = s->board.regs;
    prg8(s, 0, r[0]);
    prg8(s, 1, r[1]);
    prg8(s, 2, last_8k(s) - 1u);
    prg8(s, 3, last_8k(s));
    chr2(s, 0, r[2] >> 1);
    chr2(s, 1, r[3] >> 1);
    for (int i = 0; i < 4; i++) chr1(s, 4 + i, r[4 + i]);
}

/* ── Sachen 74LS374N: 150 (SA-015) and 243 (SA-020A) ─────────────────────── */

static void sachen_update(NesState *s) {
    uint8_t *r = s->board.regs;
    unsigned chr;
    if (s->cart.mapper == 150)
        chr = ((r[2] & 1u) << 3) | ((r[4] & 1u) << 2) | (r[6] & 3u);
    else
        chr = (r[2] & 1u) | ((r[4] & 1u) << 1) | ((r[6] & 3u) << 2);
    prg32(s, r[5] & 7u);
    chr8(s, chr);
    switch ((r[7] >> 1) & 3u) {
    case 0: set_nt(s, 0, 1, 0, 1); break;   /* vertical */
    case 1: set_nt(s, 0, 0, 1, 1); break;   /* horizontal */
    case 2: set_nt(s, 0, 1, 1, 1); break;
    case 3: set_nt(s, 1, 1, 1, 1); break;
    }
}

/* ── Public interface ────────────────────────────────────────────────────── */

bool nes_board_supported(unsigned mapper) {
    switch (mapper) {
    case 11: case 16: case 19: case 23: case 33: case 64: case 69: case 70:
    case 71: case 78: case 79: case 87: case 95: case 112: case 113: case 132:
    case 150: case 180: case 184: case 185: case 206: case 243:
        return true;
    default:
        return false;
    }
}

bool nes_board_wants_scanline(unsigned mapper) { return mapper == 64; }

bool nes_board_wants_nt(unsigned mapper) {
    return mapper == 19 || mapper == 95 || mapper == 150 || mapper == 243;
}

void nes_board_reset(NesState *s) {
    NesBoard *b = &s->board;
    memset(b, 0, sizeof(*b));
    /* Power-on default: first banks low, last banks high */
    prg8(s, 0, 0);
    prg8(s, 1, 1);
    prg8(s, 2, last_8k(s) - 1u);
    prg8(s, 3, last_8k(s));
    chr8(s, 0);
    bool vertical = s->cart.mirror == RP2C02_MIRROR_VERTICAL;
    set_nt(s, 0, vertical, !vertical, 1);

    switch (s->cart.mapper) {
    case 11: case 79: case 113: case 132: case 150: case 243:
    case 87: case 184: case 185:
        prg32(s, 0);
        break;
    case 16: case 69:
        b->ticks = true;
        break;
    case 19:
        b->ticks = true;
        b->prg6_ram = true;
        set_nt(s, 0xE0, vertical ? 0xE1 : 0xE0, vertical ? 0xE0 : 0xE1, 0xE1);
        break;
    case 23:
        b->ticks = true;
        b->prg6_ram = true;
        break;
    case 64:
        b->ticks = true;
        b->regs[6] = 0; b->regs[7] = 1; b->regs[15] = (uint8_t)(last_8k(s) - 1u);
        rambo_update(s);
        break;
    case 78:
        b->regs[15] = s->cart.mirror == RP2C02_MIRROR_4SCREEN;   /* Holy Diver: H/V */
        if (b->regs[15]) s->ppu.mirror = RP2C02_MIRROR_HORIZONTAL;
        break;
    case 95: case 206:
        b->regs[7] = 1;
        namco108_update(s);
        break;
    case 112:
        b->regs[1] = 1;
        ntdec_update(s);
        break;
    case 180:
        prg16(s, 0, 0);
        prg16(s, 1, 0);
        break;
    }
    if (s->cart.mapper == 150 || s->cart.mapper == 243)
        sachen_update(s);
}

bool nes_board_write(NesState *s, uint16_t addr, uint8_t val) {
    NesBoard *b = &s->board;
    switch (s->cart.mapper) {
    case 11:   /* Color Dreams: CCCC ..PP */
        if (addr < 0x8000) return false;
        val &= rom_at(s, addr);
        prg32(s, val & 3u);
        chr8(s, val >> 4);
        return true;

    case 16:   /* Bandai FCG ($6000) / LZ93D50 ($8000): register = A3..A0 */
        if (addr < 0x6000) return false;
        switch (addr & 0x0Fu) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            chr1(s, addr & 7u, val);
            break;
        case 0x8: prg16(s, 0, val & 0x0Fu); break;
        case 0x9: mirror_vhab(s, val); break;
        case 0xA:
            b->irq_enabled = val & 1u;
            b->irq_counter = b->irq_latch;   /* LZ93D50 reload */
            irq_ack(s);
            break;
        case 0xB:   /* FCG writes the counter, LZ93D50 the latch: do both */
            b->irq_latch   = (uint16_t)((b->irq_latch & 0xFF00u) | val);
            b->irq_counter = (uint16_t)((b->irq_counter & 0xFF00u) | val);
            break;
        case 0xC:
            b->irq_latch   = (uint16_t)((b->irq_latch & 0x00FFu) | (val << 8));
            b->irq_counter = (uint16_t)((b->irq_counter & 0x00FFu) | (val << 8));
            break;
        default: break;   /* $xD: serial EEPROM - not emulated */
        }
        return true;

    case 19:   /* Namco 163 */
        if (addr >= 0x4800 && addr < 0x5000) {
            b->n163_ram[b->n163_addr & 0x7Fu] = val;
            if (b->n163_addr & 0x80u) b->n163_addr = (uint8_t)(0x80u | ((b->n163_addr + 1u) & 0x7Fu));
            return true;
        }
        if (addr >= 0x5000 && addr < 0x5800) {
            b->irq_counter = (uint16_t)((b->irq_counter & 0x7F00u) | val);
            irq_ack(s);
            return true;
        }
        if (addr >= 0x5800 && addr < 0x6000) {
            b->irq_counter = (uint16_t)((b->irq_counter & 0x00FFu) | ((val & 0x7Fu) << 8));
            b->irq_enabled = (val & 0x80u) != 0;
            irq_ack(s);
            return true;
        }
        if (addr < 0x8000) return false;
        if (addr < 0xC000)      chr1(s, (addr - 0x8000u) >> 11, val);
        else if (addr < 0xE000) b->nt[(addr - 0xC000u) >> 11] = val;
        else if (addr < 0xE800) prg8(s, 0, val & 0x3Fu);
        else if (addr < 0xF000) prg8(s, 1, val & 0x3Fu);
        else if (addr < 0xF800) prg8(s, 2, val & 0x3Fu);
        else                    b->n163_addr = val;
        return true;

    case 23:
        if (addr < 0x8000) return false;
        vrc4_write(s, addr, val);
        return true;

    case 33:   /* Taito TC0190 */
        if (addr < 0x8000) return false;
        switch (addr & 0xA003u) {
        case 0x8000: prg8(s, 0, val & 0x3Fu); mirror_hv(s, val & 0x40u); break;
        case 0x8001: prg8(s, 1, val & 0x3Fu); break;
        case 0x8002: chr2(s, 0, val); break;
        case 0x8003: chr2(s, 1, val); break;
        default:     chr1(s, 4 + (addr & 3u), val); break;   /* $A000-$A003 */
        }
        return true;

    case 64:
        if (addr < 0x8000) return false;
        rambo_write(s, addr, val);
        return true;

    case 69:   /* Sunsoft FME-7: command at $8000, parameter at $A000 */
        if (addr >= 0x6000 && addr < 0x8000) {
            if (!b->prg6_rom && b->prg6_ram) s->prg_ram[addr & 0x1FFFu] = val;
            return true;
        }
        if (addr < 0x8000) return false;
        if (addr < 0xA000) { b->cmd = val & 0x0Fu; return true; }
        if (addr >= 0xC000) return true;   /* 5B audio: not emulated */
        switch (b->cmd) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            chr1(s, b->cmd, val);
            break;
        case 8:
            b->prg6_rom    = !(val & 0x40u);
            b->prg6_ram    = (val & 0xC0u) == 0xC0u;
            b->prg6_offset = ((val & 0x3Fu) % prg_8k_count(s)) * 0x2000u;
            break;
        case 9: case 10: case 11: prg8(s, b->cmd - 9, val & 0x3Fu); break;
        case 12: mirror_vhab(s, val); break;
        case 13:
            b->irq_enabled  = val & 1u;
            b->irq_counting = (val & 0x80u) != 0;
            irq_ack(s);
            break;
        case 14: b->irq_counter = (uint16_t)((b->irq_counter & 0xFF00u) | val); break;
        case 15: b->irq_counter = (uint16_t)((b->irq_counter & 0x00FFu) | (val << 8)); break;
        }
        return true;

    case 70:   /* Bandai 74161: PPPP CCCC */
        if (addr < 0x8000) return false;
        val &= rom_at(s, addr);
        prg16(s, 0, val >> 4);
        chr8(s, val & 0x0Fu);
        return true;

    case 71:   /* Camerica: $C000 PRG, $9000 one-screen select (Fire Hawk) */
        if (addr < 0x8000) return false;
        if (addr >= 0xC000) prg16(s, 0, val & 0x0Fu);
        else if (addr >= 0x9000 && addr < 0xA000)
            s->ppu.mirror = (val & 0x10u) ? RP2C02_MIRROR_SINGLE_B : RP2C02_MIRROR_SINGLE_A;
        return true;

    case 78:   /* Irem (Holy Diver) / Jaleco JF-16: CCCC MPPP */
        if (addr < 0x8000) return false;
        val &= rom_at(s, addr);
        prg16(s, 0, val & 7u);
        chr8(s, val >> 4);
        if (b->regs[15])
            s->ppu.mirror = (val & 8u) ? RP2C02_MIRROR_VERTICAL : RP2C02_MIRROR_HORIZONTAL;
        else
            s->ppu.mirror = (val & 8u) ? RP2C02_MIRROR_SINGLE_B : RP2C02_MIRROR_SINGLE_A;
        return true;

    case 79:   /* AVE NINA-03/06: register at $4100 (A8 set) */
    case 113:
        if (addr < 0x4100 || addr >= 0x6000 || (addr & 0x0100u) == 0) return false;
        if (s->cart.mapper == 79) {
            prg32(s, (val >> 3) & 1u);
            chr8(s, val & 7u);
        } else {   /* MCPP PCCC */
            prg32(s, (val >> 3) & 7u);
            chr8(s, (val & 7u) | ((val >> 3) & 8u));
            mirror_hv(s, !(val & 0x80u));
        }
        return true;

    case 87:   /* Jaleco/Konami: CHR bank at $6000, bits swapped */
        if (addr < 0x6000 || addr >= 0x8000) return false;
        chr8(s, ((val & 1u) << 1) | ((val >> 1) & 1u));
        return true;

    case 95: case 206:   /* Namco 108: $8000 select, $8001 data; nothing above $9FFF */
        if (addr < 0x8000) return false;
        if (addr < 0xA000) {
            if (addr & 1u) { b->regs[b->cmd] = val; namco108_update(s); }
            else           b->cmd = val & 7u;
        }
        return true;

    case 112:
        if (addr < 0x8000) return false;
        switch (addr & 0xE001u) {
        case 0x8000: b->cmd = val & 7u; break;
        case 0xA000: b->regs[b->cmd] = val; ntdec_update(s); break;
        case 0xE000: mirror_hv(s, val & 1u); break;
        default: break;   /* $C000: outer CHR bank (unused by single-chip carts) */
        }
        return true;

    case 132:  /* TXC 22211: registers at $4100-$4103, latch on $8000 writes */
        if (addr >= 0x4100 && addr <= 0x4103) { b->regs[addr & 3u] = val; return true; }
        if (addr < 0x8000) return false;
        prg32(s, (b->regs[2] >> 2) & 1u);
        chr8(s, b->regs[2] & 3u);
        return true;

    case 150: case 243:   /* Sachen 74LS374N: index at $4100, data at $4101 */
        if (addr < 0x4100 || addr >= 0x8000 || (addr & 0xC100u) != 0x4100u) return false;
        if (addr & 1u) { b->regs[b->cmd] = val; sachen_update(s); }
        else           b->cmd = val & 7u;
        return true;

    case 180:  /* UNROM with the switchable bank at $C000 */
        if (addr < 0x8000) return false;
        val &= rom_at(s, addr);
        prg16(s, 1, val & 7u);
        return true;

    case 184:  /* Sunsoft-1: .HHH .LLL 4 KB CHR banks at $6000 */
        if (addr < 0x6000 || addr >= 0x8000) return false;
        chr4(s, 0, val & 7u);
        chr4(s, 1, ((val >> 4) & 7u) | 4u);
        return true;

    case 185:  /* CNROM whose CHR only answers to the right latch value */
        if (addr < 0x8000) return false;
        val &= rom_at(s, addr);
        b->chr_disabled = !((val & 3u) && val != 0x13u);
        return true;
    }
    return false;
}

bool nes_board_read(NesState *s, uint16_t addr, uint8_t *val) {
    NesBoard *b = &s->board;
    switch (s->cart.mapper) {
    case 16:
        if (addr < 0x6000) return false;
        *val = 0;   /* EEPROM data line low */
        return true;
    case 19:
        if (addr >= 0x4800 && addr < 0x5000) {
            *val = b->n163_ram[b->n163_addr & 0x7Fu];
            if (b->n163_addr & 0x80u) b->n163_addr = (uint8_t)(0x80u | ((b->n163_addr + 1u) & 0x7Fu));
            return true;
        }
        if (addr >= 0x5000 && addr < 0x5800) { *val = (uint8_t)b->irq_counter; return true; }
        if (addr >= 0x5800 && addr < 0x6000) {
            *val = (uint8_t)(((b->irq_counter >> 8) & 0x7Fu) | (b->irq_enabled ? 0x80u : 0u));
            return true;
        }
        return false;
    case 69:
        if (addr < 0x6000) return false;
        if (b->prg6_rom)      *val = s->prg[b->prg6_offset + (addr & 0x1FFFu)];
        else if (b->prg6_ram) *val = s->prg_ram[addr & 0x1FFFu];
        else                  *val = (uint8_t)(addr >> 8);   /* open bus */
        return true;
    case 132:
        if (addr != 0x4100) return false;
        *val = (uint8_t)((b->regs[1] ^ b->regs[2]) | 0x40u);
        return true;
    case 150:
        if (addr < 0x4100 || addr >= 0x6000 || (addr & 0xC100u) != 0x4100u) return false;
        *val = (uint8_t)(0x40u | (~b->cmd & 0x3Fu));
        return true;
    }
    return false;
}

bool nes_board_has_prg_ram(const NesState *s) {
    return s->board.prg6_ram && s->cart.mapper != 69;
}

void nes_board_tick(NesState *s, unsigned cycles) {
    NesBoard *b = &s->board;
    for (unsigned i = 0; i < cycles; i++) {
        switch (s->cart.mapper) {
        case 16:
            if (b->irq_enabled) {
                if (b->irq_counter == 0) irq_raise(s);
                b->irq_counter--;
            }
            break;
        case 19:
            if (b->irq_enabled && b->irq_counter < 0x7FFFu && ++b->irq_counter == 0x7FFFu)
                irq_raise(s);
            break;
        case 23:
            vrc_irq_tick(s);
            break;
        case 64:
            if (b->irq_mode && ++b->irq_prescaler >= 4) {
                b->irq_prescaler = 0;
                rambo_clock(s);
            }
            break;
        case 69:
            if (b->irq_counting && --b->irq_counter == 0xFFFFu && b->irq_enabled)
                irq_raise(s);
            break;
        default:
            return;
        }
    }
}

void nes_board_scanline(void *ud) {
    NesState *s = ud;
    if (s->cart.mapper == 64 && !s->board.irq_mode) rambo_clock(s);
}

/* Nametables through nt[]: CIRAM page 0/1, or (Namco 163, values below
 * $E0) a CHR ROM bank used as a nametable. */
uint8_t nes_board_nt_read(uint16_t addr, void *ud) {
    NesState *s = ud;
    uint8_t  p   = s->board.nt[(addr >> 10) & 3u];
    uint16_t off = addr & 0x3FFu;
    if (s->cart.mapper == 19 && p < 0xE0u && !s->chr_is_ram)
        return s->chr[((unsigned)p % chr_1k_count(s)) * 0x400u + off];
    return s->ppu.vram[(p & 1u) * 0x400u + off];
}

void nes_board_nt_write(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;
    uint8_t p = s->board.nt[(addr >> 10) & 3u];
    if (s->cart.mapper == 19 && p < 0xE0u) return;   /* ROM */
    s->ppu.vram[(p & 1u) * 0x400u + (addr & 0x3FFu)] = val;
}
