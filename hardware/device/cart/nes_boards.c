/*
 * NES cartridge boards: discrete-logic and simple ASIC mappers.
 *
 *   11 Color Dreams       16 Bandai FCG/LZ93D50   19 Namco 163
 *   23 Konami VRC2b/VRC4  33 Taito TC0190         64 Tengen RAMBO-1
 *   69 Sunsoft FME-7      70 Bandai 74161         71 Camerica
 *   78 Irem/Jaleco 74161  79 AVE NINA-03/06       87 Jaleco/Konami CHR
 *   95 Namco 3425         112 NTDEC/Asder         113 NINA-03/06 multicart
 *   132 TXC 22211         150 Sachen SA-015       180 UNROM (fixed $8000)
 *   184 Sunsoft-1         185 CNROM + CHR protect 206 Namco 108/118
 *   243 Sachen SA-020A
 *
 * Each one is the register decoding that points the slot's windows, plus
 * an IRQ counter for the few that have one.  Register layouts follow the
 * NESdev wiki's mapper pages.
 */
#include "cart/nes_cart_internal.h"
#include <stdio.h>
#include <string.h>

static void set_nt(NesCart *c, uint8_t a, uint8_t b, uint8_t d, uint8_t e) {
    c->m.board.nt[0] = a; c->m.board.nt[1] = b; c->m.board.nt[2] = d; c->m.board.nt[3] = e;
}

/* Power-on state common to every board: first banks low, last banks high,
 * CHR bank 0, nametables from the solder pads. */
static void board_reset(NesCart *c) {
    memset(&c->m.board, 0, sizeof(c->m.board));
    nc_prg8(c, 0, 0);
    nc_prg8(c, 1, 1);
    nc_prg8(c, 2, nc_last_8k(c) - 1u);
    nc_prg8(c, 3, nc_last_8k(c));
    nc_chr8(c, 0);
    bool vertical = c->info.mirror == RP2C02_MIRROR_VERTICAL;
    set_nt(c, 0, vertical, !vertical, 1);
    c->irq = false;
}

static void board_reset32(NesCart *c) {
    board_reset(c);
    nc_prg32(c, 0);
}

static bool board_prg_ram(const NesCart *c) { return c->m.board.prg6_ram; }

/* Nametables through nt[]: CIRAM page 0/1, or (Namco 163, values below
 * $E0) a CHR ROM bank used as a nametable. */
static uint8_t board_nt_read(NesCart *c, uint16_t addr) {
    uint8_t  p   = c->m.board.nt[(addr >> 10) & 3u];
    uint16_t off = addr & 0x3FFu;
    if (c->info.mapper == 19 && p < 0xE0u && !c->chr_is_ram)
        return c->chr[((unsigned)p % nc_chr_1k(c)) * 0x400u + off];
    return c->ppu->vram[(p & 1u) * 0x400u + off];
}

static void board_nt_write(NesCart *c, uint16_t addr, uint8_t val) {
    uint8_t p = c->m.board.nt[(addr >> 10) & 3u];
    if (c->info.mapper == 19 && p < 0xE0u) return;   /* ROM */
    c->ppu->vram[(p & 1u) * 0x400u + (addr & 0x3FFu)] = val;
}

static void board_dump(const NesCart *c, char *buf, size_t len) {
    const NesBoard *b = &c->m.board;
    int n = snprintf(buf, len, "board cmd=%02X regs=", b->cmd);
    for (int i = 0; i < 16 && n > 0 && (size_t)n < len; i++)
        n += snprintf(buf + n, len - (size_t)n, "%02X%s", b->regs[i], i < 15 ? " " : "\n");
    if (n > 0 && (size_t)n < len)
        snprintf(buf + n, len - (size_t)n,
                 "board irq counter=%04X latch=%04X enabled=%d nt=%02X %02X %02X %02X\n",
                 b->irq_counter, b->irq_latch, b->irq_enabled,
                 b->nt[0], b->nt[1], b->nt[2], b->nt[3]);
}

#define BOARD(num, nm, ...) { num, nm, .dump = board_dump, __VA_ARGS__ }

/* ── 11: Color Dreams ────────────────────────────────────────────────────── */

static bool colordreams_write(NesCart *c, uint16_t addr, uint8_t val) {   /* CCCC ..PP */
    if (addr < 0x8000) return false;
    val &= nc_rom_at(c, addr);
    nc_prg32(c, val & 3u);
    nc_chr8(c, val >> 4);
    return true;
}

static const NesMapper colordreams = BOARD(11, "Color Dreams",
    .reset = board_reset32, .write = colordreams_write);

/* ── 16: Bandai FCG / LZ93D50 ────────────────────────────────────────────── */

/* $6000 (FCG) or $8000 (LZ93D50): register = A3..A0 */
static bool bandai_write(NesCart *c, uint16_t addr, uint8_t val) {
    NesBoard *b = &c->m.board;
    if (addr < 0x6000) return false;
    switch (addr & 0x0Fu) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        nc_chr1(c, addr & 7u, val);
        break;
    case 0x8: nc_prg16(c, 0, val & 0x0Fu); break;
    case 0x9: nc_mirror_vhab(c, val); break;
    case 0xA:
        b->irq_enabled = val & 1u;
        b->irq_counter = b->irq_latch;   /* LZ93D50 reload */
        nc_irq_ack(c);
        break;
    case 0xB:   /* FCG writes the counter, LZ93D50 the latch: do both */
        b->irq_latch   = (uint16_t)((b->irq_latch & 0xFF00u) | val);
        b->irq_counter = (uint16_t)((b->irq_counter & 0xFF00u) | val);
        break;
    case 0xC:
        b->irq_latch   = (uint16_t)((b->irq_latch & 0x00FFu) | (val << 8));
        b->irq_counter = (uint16_t)((b->irq_counter & 0x00FFu) | (val << 8));
        break;
    default: break;   /* $xD: serial EEPROM, not emulated */
    }
    return true;
}

static bool bandai_read(NesCart *c, uint16_t addr, uint8_t *val) {
    (void)c;
    if (addr < 0x6000) return false;
    *val = 0;   /* EEPROM data line low */
    return true;
}

static float bandai_tick(NesCart *c) {
    NesBoard *b = &c->m.board;
    if (b->irq_enabled) {
        if (b->irq_counter == 0) nc_irq_raise(c);
        b->irq_counter--;
    }
    return 0.0f;
}

static const NesMapper bandai = BOARD(16, "Bandai FCG",
    .reset = board_reset, .write = bandai_write, .read = bandai_read, .tick = bandai_tick);

/* ── 19: Namco 163 ───────────────────────────────────────────────────────── */

static void n163_reset(NesCart *c) {
    board_reset(c);
    c->m.board.prg6_ram = true;
    bool vertical = c->info.mirror == RP2C02_MIRROR_VERTICAL;
    set_nt(c, 0xE0, vertical ? 0xE1 : 0xE0, vertical ? 0xE0 : 0xE1, 0xE1);
}

static bool n163_write(NesCart *c, uint16_t addr, uint8_t val) {
    NesBoard *b = &c->m.board;
    if (addr >= 0x4800 && addr < 0x5000) {
        b->n163_ram[b->n163_addr & 0x7Fu] = val;
        if (b->n163_addr & 0x80u) b->n163_addr = (uint8_t)(0x80u | ((b->n163_addr + 1u) & 0x7Fu));
        return true;
    }
    if (addr >= 0x5000 && addr < 0x5800) {
        b->irq_counter = (uint16_t)((b->irq_counter & 0x7F00u) | val);
        nc_irq_ack(c);
        return true;
    }
    if (addr >= 0x5800 && addr < 0x6000) {
        b->irq_counter = (uint16_t)((b->irq_counter & 0x00FFu) | ((val & 0x7Fu) << 8));
        b->irq_enabled = (val & 0x80u) != 0;
        nc_irq_ack(c);
        return true;
    }
    if (addr < 0x8000) return false;
    if (addr < 0xC000)      nc_chr1(c, (addr - 0x8000u) >> 11, val);
    else if (addr < 0xE000) b->nt[(addr - 0xC000u) >> 11] = val;
    else if (addr < 0xE800) nc_prg8(c, 0, val & 0x3Fu);
    else if (addr < 0xF000) nc_prg8(c, 1, val & 0x3Fu);
    else if (addr < 0xF800) nc_prg8(c, 2, val & 0x3Fu);
    else                    b->n163_addr = val;
    return true;
}

static bool n163_read(NesCart *c, uint16_t addr, uint8_t *val) {
    NesBoard *b = &c->m.board;
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
}

static float n163_tick(NesCart *c) {
    NesBoard *b = &c->m.board;
    if (b->irq_enabled && b->irq_counter < 0x7FFFu && ++b->irq_counter == 0x7FFFu)
        nc_irq_raise(c);
    return 0.0f;
}

static const NesMapper n163 = BOARD(19, "Namco 163",
    .reset = n163_reset, .write = n163_write, .read = n163_read, .prg_ram = board_prg_ram,
    .nt_read = board_nt_read, .nt_write = board_nt_write, .tick = n163_tick);

/* ── 23: VRC2b / VRC4e / VRC4f (and its IRQ, the same counter as VRC6's) ─ */

static void vrc_irq_clock(NesCart *c) {
    NesBoard *b = &c->m.board;
    if (b->irq_counter == 0xFF) {
        b->irq_counter = b->irq_latch;
        nc_irq_raise(c);
    } else {
        b->irq_counter++;
    }
}

static float vrc4_tick(NesCart *c) {
    NesBoard *b = &c->m.board;
    if (!b->irq_enabled) return 0.0f;
    if (b->irq_mode) {
        vrc_irq_clock(c);
    } else {
        b->irq_prescaler -= 3;   /* 341 PPU dots per scanline, 3 per CPU cycle */
        if (b->irq_prescaler <= 0) {
            b->irq_prescaler += 341;
            vrc_irq_clock(c);
        }
    }
    return 0.0f;
}

static void vrc_irq_write(NesCart *c, unsigned reg, uint8_t val) {
    NesBoard *b = &c->m.board;
    switch (reg) {
    case 0: b->irq_latch = (uint16_t)((b->irq_latch & 0xF0u) | (val & 0x0Fu)); break;
    case 1: b->irq_latch = (uint16_t)((b->irq_latch & 0x0Fu) | ((val & 0x0Fu) << 4)); break;
    case 2:
        b->irq_after   = val & 1u;
        b->irq_enabled = (val & 2u) != 0;
        b->irq_mode    = (val & 4u) != 0;
        if (b->irq_enabled) {
            b->irq_counter   = b->irq_latch;
            b->irq_prescaler = 341;
        }
        nc_irq_ack(c);
        break;
    case 3:
        nc_irq_ack(c);
        b->irq_enabled = b->irq_after;
        break;
    }
}

static void vrc4_update_prg(NesCart *c) {
    NesBoard *b = &c->m.board;
    bool swap = (b->regs[2] & 2u) != 0;
    nc_prg8(c, swap ? 2 : 0, b->regs[0]);
    nc_prg8(c, swap ? 0 : 2, nc_last_8k(c) - 1u);
    nc_prg8(c, 1, b->regs[1]);
    nc_prg8(c, 3, nc_last_8k(c));
}

static void vrc4_reset(NesCart *c) {
    board_reset(c);
    c->m.board.prg6_ram = true;
}

static bool vrc4_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    NesBoard *b = &c->m.board;
    /* VRC2b/VRC4f use A0/A1, VRC4e A2/A3; OR-ing them serves both */
    unsigned reg = (unsigned)(((addr | (addr >> 2)) & 1u) | (((addr >> 1) | (addr >> 3)) & 1u) << 1);
    switch (addr & 0xF000u) {
    case 0x8000: b->regs[0] = val & 0x1Fu; vrc4_update_prg(c); break;
    case 0x9000:
        if (reg < 2) nc_mirror_vhab(c, val);
        else if (reg == 2) { b->regs[2] = val; vrc4_update_prg(c); }
        break;
    case 0xA000: b->regs[1] = val & 0x1Fu; vrc4_update_prg(c); break;
    case 0xB000: case 0xC000: case 0xD000: case 0xE000: {
        int i = (int)(((addr & 0xF000u) - 0xB000u) >> 11) + (int)(reg >> 1);
        if (reg & 1u) b->chr[i] = (uint16_t)((b->chr[i] & 0x0Fu) | ((val & 0x1Fu) << 4));
        else          b->chr[i] = (uint16_t)((b->chr[i] & 0x1F0u) | (val & 0x0Fu));
        nc_chr1(c, i, b->chr[i]);
        break;
    }
    case 0xF000: vrc_irq_write(c, reg, val); break;
    }
    return true;
}

static const NesMapper vrc4 = BOARD(23, "VRC2b/VRC4",
    .reset = vrc4_reset, .write = vrc4_write, .prg_ram = board_prg_ram, .tick = vrc4_tick);

/* ── 33: Taito TC0190 ────────────────────────────────────────────────────── */

static bool tc0190_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    switch (addr & 0xA003u) {
    case 0x8000: nc_prg8(c, 0, val & 0x3Fu); nc_mirror_hv(c, val & 0x40u); break;
    case 0x8001: nc_prg8(c, 1, val & 0x3Fu); break;
    case 0x8002: nc_chr2(c, 0, val); break;
    case 0x8003: nc_chr2(c, 1, val); break;
    default:     nc_chr1(c, 4 + (addr & 3u), val); break;   /* $A000-$A003 */
    }
    return true;
}

static const NesMapper tc0190 = BOARD(33, "Taito TC0190",
    .reset = board_reset, .write = tc0190_write);

/* ── 64: Tengen RAMBO-1 ──────────────────────────────────────────────────── */

static void rambo_update(NesCart *c) {
    NesBoard *b = &c->m.board;
    uint8_t *r = b->regs;
    unsigned ch[8];
    if (b->cmd & 0x20u) {   /* K: 1 KB banks throughout */
        ch[0] = r[0]; ch[1] = r[8]; ch[2] = r[1]; ch[3] = r[9];
    } else {
        ch[0] = r[0] & 0xFEu; ch[1] = r[0] | 1u; ch[2] = r[1] & 0xFEu; ch[3] = r[1] | 1u;
    }
    ch[4] = r[2]; ch[5] = r[3]; ch[6] = r[4]; ch[7] = r[5];
    int inv = (b->cmd & 0x80u) ? 4 : 0;
    for (int i = 0; i < 8; i++) nc_chr1(c, i ^ inv, ch[i]);

    if (b->cmd & 0x40u) { nc_prg8(c, 0, r[15]); nc_prg8(c, 1, r[6]); nc_prg8(c, 2, r[7]); }
    else                { nc_prg8(c, 0, r[6]);  nc_prg8(c, 1, r[7]); nc_prg8(c, 2, r[15]); }
    nc_prg8(c, 3, nc_last_8k(c));
}

static void rambo_clock(NesCart *c) {
    NesBoard *b = &c->m.board;
    if (b->irq_reload || b->irq_counter == 0) {
        b->irq_counter = b->irq_latch;
        b->irq_reload  = false;
    } else {
        b->irq_counter--;
    }
    if (b->irq_counter == 0 && b->irq_enabled) nc_irq_raise(c);
}

static void rambo_reset(NesCart *c) {
    board_reset(c);
    NesBoard *b = &c->m.board;
    b->regs[6] = 0; b->regs[7] = 1; b->regs[15] = (uint8_t)(nc_last_8k(c) - 1u);
    rambo_update(c);
}

static bool rambo_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    NesBoard *b = &c->m.board;
    switch (addr & 0xE001u) {
    case 0x8000: b->cmd = val; rambo_update(c); break;
    case 0x8001: b->regs[b->cmd & 0x0Fu] = val; rambo_update(c); break;
    case 0xA000: nc_mirror_hv(c, val & 1u); break;
    case 0xC000: b->irq_latch = val; break;
    case 0xC001:
        b->irq_mode      = val & 1u;
        b->irq_reload    = true;
        b->irq_prescaler = 0;
        break;
    case 0xE000: b->irq_enabled = false; nc_irq_ack(c); break;
    case 0xE001: b->irq_enabled = true; break;
    }
    return true;
}

/* CPU-cycle mode: a clock every four cycles */
static float rambo_tick(NesCart *c) {
    NesBoard *b = &c->m.board;
    if (b->irq_mode && ++b->irq_prescaler >= 4) {
        b->irq_prescaler = 0;
        rambo_clock(c);
    }
    return 0.0f;
}

static void rambo_scanline(NesCart *c) {
    if (!c->m.board.irq_mode) rambo_clock(c);
}

static const NesMapper rambo = BOARD(64, "Tengen RAMBO-1",
    .reset = rambo_reset, .write = rambo_write, .tick = rambo_tick, .scanline = rambo_scanline);

/* ── 69: Sunsoft FME-7: command at $8000, parameter at $A000 ─────────────── */

static bool fme7_write(NesCart *c, uint16_t addr, uint8_t val) {
    NesBoard *b = &c->m.board;
    if (addr >= 0x6000 && addr < 0x8000) {
        if (!b->prg6_rom && b->prg6_ram) c->prg_ram[addr & 0x1FFFu] = val;
        return true;
    }
    if (addr < 0x8000) return false;
    if (addr < 0xA000) { b->cmd = val & 0x0Fu; return true; }
    if (addr >= 0xC000) return true;   /* 5B audio: not emulated */
    switch (b->cmd) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        nc_chr1(c, b->cmd, val);
        break;
    case 8:
        b->prg6_rom    = !(val & 0x40u);
        b->prg6_ram    = (val & 0xC0u) == 0xC0u;
        b->prg6_offset = ((val & 0x3Fu) % nc_prg_8k(c)) * 0x2000u;
        break;
    case 9: case 10: case 11: nc_prg8(c, b->cmd - 9, val & 0x3Fu); break;
    case 12: nc_mirror_vhab(c, val); break;
    case 13:
        b->irq_enabled  = val & 1u;
        b->irq_counting = (val & 0x80u) != 0;
        nc_irq_ack(c);
        break;
    case 14: b->irq_counter = (uint16_t)((b->irq_counter & 0xFF00u) | val); break;
    case 15: b->irq_counter = (uint16_t)((b->irq_counter & 0x00FFu) | (val << 8)); break;
    }
    return true;
}

static bool fme7_read(NesCart *c, uint16_t addr, uint8_t *val) {
    NesBoard *b = &c->m.board;
    if (addr < 0x6000) return false;
    if (b->prg6_rom)      *val = c->prg[b->prg6_offset + (addr & 0x1FFFu)];
    else if (b->prg6_ram) *val = c->prg_ram[addr & 0x1FFFu];
    else                  *val = (uint8_t)(addr >> 8);   /* open bus */
    return true;
}

static float fme7_tick(NesCart *c) {
    NesBoard *b = &c->m.board;
    if (b->irq_counting && --b->irq_counter == 0xFFFFu && b->irq_enabled)
        nc_irq_raise(c);
    return 0.0f;
}

/* $6000-$7FFF goes through fme7_read/fme7_write: no prg_ram hook */
static const NesMapper fme7 = BOARD(69, "Sunsoft FME-7",
    .reset = board_reset, .write = fme7_write, .read = fme7_read, .tick = fme7_tick);

/* ── 70: Bandai 74161: PPPP CCCC ─────────────────────────────────────────── */

static bool bandai74_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    val &= nc_rom_at(c, addr);
    nc_prg16(c, 0, val >> 4);
    nc_chr8(c, val & 0x0Fu);
    return true;
}

static const NesMapper bandai74 = BOARD(70, "Bandai 74161",
    .reset = board_reset, .write = bandai74_write);

/* ── 71: Camerica: $C000 PRG, $9000 one-screen select (Fire Hawk) ────────── */

static bool camerica_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    if (addr >= 0xC000) nc_prg16(c, 0, val & 0x0Fu);
    else if (addr >= 0x9000 && addr < 0xA000)
        c->ppu->mirror = (val & 0x10u) ? RP2C02_MIRROR_SINGLE_B : RP2C02_MIRROR_SINGLE_A;
    return true;
}

static const NesMapper camerica = BOARD(71, "Camerica",
    .reset = board_reset, .write = camerica_write);

/* ── 78: Irem (Holy Diver) / Jaleco JF-16: CCCC MPPP ─────────────────────── */

static void irem78_reset(NesCart *c) {
    board_reset(c);
    NesBoard *b = &c->m.board;
    b->regs[15] = c->info.mirror == RP2C02_MIRROR_4SCREEN;   /* Holy Diver: H/V */
    if (b->regs[15]) c->ppu->mirror = RP2C02_MIRROR_HORIZONTAL;
}

static bool irem78_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    val &= nc_rom_at(c, addr);
    nc_prg16(c, 0, val & 7u);
    nc_chr8(c, val >> 4);
    if (c->m.board.regs[15])
        c->ppu->mirror = (val & 8u) ? RP2C02_MIRROR_VERTICAL : RP2C02_MIRROR_HORIZONTAL;
    else
        c->ppu->mirror = (val & 8u) ? RP2C02_MIRROR_SINGLE_B : RP2C02_MIRROR_SINGLE_A;
    return true;
}

static const NesMapper irem78 = BOARD(78, "Irem/Jaleco 74161",
    .reset = irem78_reset, .write = irem78_write);

/* ── 79, 113: AVE NINA-03/06: register at $4100 (A8 set) ────────────────── */

static bool nina_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x4100 || addr >= 0x6000 || (addr & 0x0100u) == 0) return false;
    if (c->info.mapper == 79) {
        nc_prg32(c, (val >> 3) & 1u);
        nc_chr8(c, val & 7u);
    } else {   /* MCPP PCCC */
        nc_prg32(c, (val >> 3) & 7u);
        nc_chr8(c, (val & 7u) | ((val >> 3) & 8u));
        nc_mirror_hv(c, !(val & 0x80u));
    }
    return true;
}

static const NesMapper nina = BOARD(79, "AVE NINA-03/06",
    .reset = board_reset32, .write = nina_write);
static const NesMapper nina_multi = BOARD(113, "NINA-03/06 multicart",
    .reset = board_reset32, .write = nina_write);

/* ── 87: Jaleco/Konami: CHR bank at $6000, bits swapped ──────────────────── */

static bool jaleco87_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x6000 || addr >= 0x8000) return false;
    nc_chr8(c, ((val & 1u) << 1) | ((val >> 1) & 1u));
    return true;
}

static const NesMapper jaleco87 = BOARD(87, "Jaleco/Konami CHR",
    .reset = board_reset32, .write = jaleco87_write);

/* ── 95, 206: Namco 108 family: $8000 select, $8001 data ─────────────────── */

static void namco108_update(NesCart *c) {
    uint8_t *r = c->m.board.regs;
    uint8_t mask = c->info.mapper == 95 ? 0x1Fu : 0x3Fu;   /* 95: bit 5 is the nametable */
    nc_chr2(c, 0, (r[0] & mask) >> 1);
    nc_chr2(c, 1, (r[1] & mask) >> 1);
    for (int i = 0; i < 4; i++) nc_chr1(c, 4 + i, r[2 + i] & mask);
    nc_prg8(c, 0, r[6] & 0x0Fu);
    nc_prg8(c, 1, r[7] & 0x0Fu);
    nc_prg8(c, 2, nc_last_8k(c) - 1u);
    nc_prg8(c, 3, nc_last_8k(c));
    if (c->info.mapper == 95) {
        uint8_t lo = (r[0] >> 5) & 1u, hi = (r[1] >> 5) & 1u;
        set_nt(c, lo, lo, hi, hi);
    }
}

static void namco108_reset(NesCart *c) {
    board_reset(c);
    c->m.board.regs[7] = 1;
    namco108_update(c);
}

/* nothing above $9FFF */
static bool namco108_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    NesBoard *b = &c->m.board;
    if (addr < 0xA000) {
        if (addr & 1u) { b->regs[b->cmd] = val; namco108_update(c); }
        else           b->cmd = val & 7u;
    }
    return true;
}

static const NesMapper namco3425 = BOARD(95, "Namco 3425",
    .reset = namco108_reset, .write = namco108_write,
    .nt_read = board_nt_read, .nt_write = board_nt_write);
static const NesMapper namco108 = BOARD(206, "Namco 108/118",
    .reset = namco108_reset, .write = namco108_write);

/* ── 112: NTDEC / Asder ──────────────────────────────────────────────────── */

static void ntdec_update(NesCart *c) {
    uint8_t *r = c->m.board.regs;
    nc_prg8(c, 0, r[0]);
    nc_prg8(c, 1, r[1]);
    nc_prg8(c, 2, nc_last_8k(c) - 1u);
    nc_prg8(c, 3, nc_last_8k(c));
    nc_chr2(c, 0, r[2] >> 1);
    nc_chr2(c, 1, r[3] >> 1);
    for (int i = 0; i < 4; i++) nc_chr1(c, 4 + i, r[4 + i]);
}

static void ntdec_reset(NesCart *c) {
    board_reset(c);
    c->m.board.regs[1] = 1;
    ntdec_update(c);
}

static bool ntdec_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    NesBoard *b = &c->m.board;
    switch (addr & 0xE001u) {
    case 0x8000: b->cmd = val & 7u; break;
    case 0xA000: b->regs[b->cmd] = val; ntdec_update(c); break;
    case 0xE000: nc_mirror_hv(c, val & 1u); break;
    default: break;   /* $C000: outer CHR bank (unused by single-chip carts) */
    }
    return true;
}

static const NesMapper ntdec = BOARD(112, "NTDEC/Asder",
    .reset = ntdec_reset, .write = ntdec_write);

/* ── 132: TXC 22211: registers at $4100-$4103, latch on $8000 writes ─────── */

static bool txc_write(NesCart *c, uint16_t addr, uint8_t val) {
    NesBoard *b = &c->m.board;
    if (addr >= 0x4100 && addr <= 0x4103) { b->regs[addr & 3u] = val; return true; }
    if (addr < 0x8000) return false;
    nc_prg32(c, (b->regs[2] >> 2) & 1u);
    nc_chr8(c, b->regs[2] & 3u);
    return true;
}

static bool txc_read(NesCart *c, uint16_t addr, uint8_t *val) {
    if (addr != 0x4100) return false;
    *val = (uint8_t)((c->m.board.regs[1] ^ c->m.board.regs[2]) | 0x40u);
    return true;
}

static const NesMapper txc = BOARD(132, "TXC 22211",
    .reset = board_reset32, .write = txc_write, .read = txc_read);

/* ── 150, 243: Sachen 74LS374N: index at $4100, data at $4101 ────────────── */

static void sachen_update(NesCart *c) {
    uint8_t *r = c->m.board.regs;
    unsigned chr;
    if (c->info.mapper == 150)
        chr = ((r[2] & 1u) << 3) | ((r[4] & 1u) << 2) | (r[6] & 3u);
    else
        chr = (r[2] & 1u) | ((r[4] & 1u) << 1) | ((r[6] & 3u) << 2);
    nc_prg32(c, r[5] & 7u);
    nc_chr8(c, chr);
    switch ((r[7] >> 1) & 3u) {
    case 0: set_nt(c, 0, 1, 0, 1); break;   /* vertical */
    case 1: set_nt(c, 0, 0, 1, 1); break;   /* horizontal */
    case 2: set_nt(c, 0, 1, 1, 1); break;
    case 3: set_nt(c, 1, 1, 1, 1); break;
    }
}

static void sachen_reset(NesCart *c) {
    board_reset32(c);
    sachen_update(c);
}

static bool sachen_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x4100 || addr >= 0x8000 || (addr & 0xC100u) != 0x4100u) return false;
    NesBoard *b = &c->m.board;
    if (addr & 1u) { b->regs[b->cmd] = val; sachen_update(c); }
    else           b->cmd = val & 7u;
    return true;
}

static bool sachen150_read(NesCart *c, uint16_t addr, uint8_t *val) {
    if (addr < 0x4100 || addr >= 0x6000 || (addr & 0xC100u) != 0x4100u) return false;
    *val = (uint8_t)(0x40u | (~c->m.board.cmd & 0x3Fu));
    return true;
}

static const NesMapper sachen150 = BOARD(150, "Sachen SA-015",
    .reset = sachen_reset, .write = sachen_write, .read = sachen150_read,
    .nt_read = board_nt_read, .nt_write = board_nt_write);
static const NesMapper sachen243 = BOARD(243, "Sachen SA-020A",
    .reset = sachen_reset, .write = sachen_write,
    .nt_read = board_nt_read, .nt_write = board_nt_write);

/* ── 180: UNROM with the switchable bank at $C000 ────────────────────────── */

static void unrom180_reset(NesCart *c) {
    board_reset(c);
    nc_prg16(c, 0, 0);
    nc_prg16(c, 1, 0);
}

static bool unrom180_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    val &= nc_rom_at(c, addr);
    nc_prg16(c, 1, val & 7u);
    return true;
}

static const NesMapper unrom180 = BOARD(180, "UNROM (fixed $8000)",
    .reset = unrom180_reset, .write = unrom180_write);

/* ── 184: Sunsoft-1: .HHH .LLL 4 KB CHR banks at $6000 ───────────────────── */

static bool sunsoft1_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x6000 || addr >= 0x8000) return false;
    nc_chr4(c, 0, val & 7u);
    nc_chr4(c, 1, ((val >> 4) & 7u) | 4u);
    return true;
}

static const NesMapper sunsoft1 = BOARD(184, "Sunsoft-1",
    .reset = board_reset32, .write = sunsoft1_write);

/* ── 185: CNROM whose CHR only answers to the right latch value ──────────── */

static bool cnrom185_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    val &= nc_rom_at(c, addr);
    c->m.board.chr_disabled = !((val & 3u) && val != 0x13u);
    return true;
}

static uint8_t cnrom185_chr_read(NesCart *c, uint16_t addr) {
    return c->m.board.chr_disabled ? 0xFF : nc_chr_win(c, addr);
}

static const NesMapper cnrom185 = BOARD(185, "CNROM + CHR protect",
    .reset = board_reset32, .write = cnrom185_write, .chr_read = cnrom185_chr_read);

/* ────────────────────────────────────────────────────────────────────────── */

const NesMapper *const nes_boards[] = {
    &colordreams, &bandai, &n163, &vrc4, &tc0190, &rambo, &fme7, &bandai74,
    &camerica, &irem78, &nina, &nina_multi, &jaleco87, &namco3425, &namco108,
    &ntdec, &txc, &sachen150, &sachen243, &unrom180, &sunsoft1, &cnrom185,
    NULL,
};
