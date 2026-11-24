/* NES boards with a latch or two: NROM (0), UxROM (2), CNROM (3), AxROM
 * (7), GxROM (66), Waixing (178), Action 52 (228) and the VS. System's
 * board. */
#include "cart/nes_cart_internal.h"
#include <stdio.h>

static bool always(const NesCart *c) { (void)c; return true; }

/* ── 0: NROM ─────────────────────────────────────────────────────────────── */

static void nrom_reset(NesCart *c) {
    /* 16 KB mirrored, or 32 KB */
    nc_prg16_at(c, 0, 0);
    nc_prg16_at(c, 1, c->info.prg_banks > 1 ? 0x4000u : 0);
}

const NesMapper nes_mapper_nrom = { 0, "NROM", .reset = nrom_reset };

/* ── 2: UxROM ────────────────────────────────────────────────────────────── */

static void uxrom_reset(NesCart *c) {
    nc_prg16_at(c, 0, 0);
    nc_prg16_at(c, 1, (uint32_t)(c->info.prg_banks - 1) * 0x4000u);
    nc_chr4_at(c, 0, 0);
    nc_chr4_at(c, 1, 0x1000u);
}

static bool uxrom_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    nc_prg16_at(c, 0, (uint32_t)(val % c->info.prg_banks) * 0x4000u);
    return true;
}

const NesMapper nes_mapper_uxrom = { 2, "UxROM", .reset = uxrom_reset, .write = uxrom_write };

/* ── 3: CNROM ────────────────────────────────────────────────────────────── */

static void cnrom_reset(NesCart *c) {
    nrom_reset(c);
    nc_chr4_at(c, 0, 0);
    nc_chr4_at(c, 1, 0x1000u);
}

static bool cnrom_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    val &= nc_rom_at(c, addr);   /* bus conflicts */
    uint32_t off = (uint32_t)(val % c->info.chr_banks) * 0x2000u;
    nc_chr4_at(c, 0, off);
    nc_chr4_at(c, 1, off + 0x1000u);
    return true;
}

const NesMapper nes_mapper_cnrom = { 3, "CNROM", .reset = cnrom_reset, .write = cnrom_write };

/* ── 7: AxROM ────────────────────────────────────────────────────────────── */

static void axrom_reset(NesCart *c) {
    nc_prg16_at(c, 0, 0);
    nc_prg16_at(c, 1, 0x4000u);
    c->ppu->mirror = RP2C02_MIRROR_SINGLE_A;
}

/* bits 2:0 the 32 KB PRG bank, bit 4 the one-screen page */
static bool axrom_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    uint32_t off = ((uint32_t)(val & 0x07u) * 0x8000u) % nc_prg_size(c);
    nc_prg16_at(c, 0, off);
    nc_prg16_at(c, 1, off + 0x4000u);
    c->ppu->mirror = (val & 0x10u) ? RP2C02_MIRROR_SINGLE_B : RP2C02_MIRROR_SINGLE_A;
    return true;
}

const NesMapper nes_mapper_axrom = { 7, "AxROM", .reset = axrom_reset, .write = axrom_write };

/* ── 66: GxROM ───────────────────────────────────────────────────────────── */

static void gxrom_reset(NesCart *c) {
    nc_prg16_at(c, 0, 0);
    nc_prg16_at(c, 1, 0x4000u);
    nc_chr4_at(c, 0, 0);
    nc_chr4_at(c, 1, 0x1000u);
}

/* ..PP ..CC, with bus conflicts */
static bool gxrom_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    val &= nc_rom_at(c, addr);
    uint32_t prg = (((uint32_t)(val >> 4) & 3u) * 0x8000u) % nc_prg_size(c);
    nc_prg16_at(c, 0, prg);
    nc_prg16_at(c, 1, prg + 0x4000u);
    uint32_t chr = (((uint32_t)val & 3u) * 0x2000u) % nc_chr_size(c);
    nc_chr4_at(c, 0, chr);
    nc_chr4_at(c, 1, chr + 0x1000u);
    return true;
}

const NesMapper nes_mapper_gxrom = { 66, "GxROM", .reset = gxrom_reset, .write = gxrom_write };

/* ── 178: Waixing FS ─────────────────────────────────────────────────────── */

static void m178_update(NesCart *c) {
    NesM178 *m = &c->m.m178;
    uint8_t  prg_mode = (m->mode >> 1) & 3u;
    uint32_t size  = nc_prg_size(c);
    uint32_t outer = (uint32_t)m->prg_hi * 0x20000u;
    uint32_t inner = (uint32_t)(m->prg_lo & 7u) * 0x4000u;

    switch (prg_mode) {
    case 0: { /* NROM-256/BNROM: 32KB page, CPU A14 = PRG A14 */
        uint32_t base = (outer + (uint32_t)(m->prg_lo & 6u) * 0x4000u) % size;
        nc_prg16_at(c, 0, base);
        nc_prg16_at(c, 1, (base + 0x4000u) % size);
        break;
    }
    case 2:   /* NROM-128: the same 16KB bank in both windows */
        nc_prg16_at(c, 0, (outer + inner) % size);
        nc_prg16_at(c, 1, (outer + inner) % size);
        break;
    default:  /* 1 UNROM: switchable $8000, last inner bank at $C000; 3 likewise */
        nc_prg16_at(c, 0, (outer + inner) % size);
        nc_prg16_at(c, 1, (outer + 7u * 0x4000u) % size);
        break;
    }
    c->ppu->mirror = (m->mode & 1u) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
}

static void m178_reset(NesCart *c) {
    c->m.m178 = (NesM178){0};
    m178_update(c);
    nc_chr4_at(c, 0, 0);
    nc_chr4_at(c, 1, 0x1000u);
}

static bool m178_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x4800 || addr > 0x4803) return false;
    switch (addr) {
    case 0x4800: c->m.m178.mode   = val; break;
    case 0x4801: c->m.m178.prg_lo = val; break;
    case 0x4802: c->m.m178.prg_hi = val; break;
    default: break;   /* $4803: PRG-RAM bank; one 8 KB bank only */
    }
    m178_update(c);
    return true;
}

static void m178_dump(const NesCart *c, char *buf, size_t len) {
    snprintf(buf, len, "mode=%02X prg_lo=%02X prg_hi=%02X\n",
             c->m.m178.mode, c->m.m178.prg_lo, c->m.m178.prg_hi);
}

const NesMapper nes_mapper_m178 = {
    178, "Waixing FS", .reset = m178_reset, .write = m178_write,
    .prg_ram = always, .dump = m178_dump,
};

/* ── 228: Action 52 ──────────────────────────────────────────────────────── */

static void m228_reset(NesCart *c) {
    /* the last two 16KB banks, so the reset vector is readable */
    uint32_t last = (uint32_t)(c->info.prg_banks - 1) * 0x4000u;
    nc_prg16_at(c, 0, last >= 0x4000u ? last - 0x4000u : 0);
    nc_prg16_at(c, 1, last);
    nc_chr4_at(c, 0, 0);
    nc_chr4_at(c, 1, 0x1000u);
}

/* The register is the address and data bus (NESdev):
 *   FEDCBA98 76543210  (address)
 *   1.MHHPPP PPS.CCCC
 * D(13)=mirror  C:B(12:11)=chip  A-6(10:6)=page  5=PRG mode  3:0=CHR high,
 * data 1:0 = CHR low.  Three 512KB chips at chip 0, 1 and 3; 2 is open. */
static bool m228_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    uint8_t chip     = (uint8_t)((addr >> 11) & 3u);
    uint8_t page     = (uint8_t)((addr >> 6) & 0x1Fu);
    uint8_t prg_mode = (uint8_t)((addr >> 5) & 1u);
    uint8_t chr_hi   = (uint8_t)(addr & 0xFu);
    uint8_t mirror   = (uint8_t)((addr >> 13) & 1u);
    uint8_t chr_bank = (uint8_t)((chr_hi << 2) | (val & 3u));
    if (chip == 2u) return true;   /* no PRG chip there */

    uint32_t bank = chip == 3u ? 64u + page : (uint32_t)chip * 32u + page;
    uint32_t size = nc_prg_size(c);
    if (!prg_mode) {   /* 32KB: the even/odd pair */
        uint32_t base = (bank & ~1u) * 0x4000u;
        nc_prg16_at(c, 0, base % size);
        nc_prg16_at(c, 1, (base + 0x4000u) % size);
    } else {           /* 16KB in both windows */
        uint32_t base = (bank * 0x4000u) % size;
        nc_prg16_at(c, 0, base);
        nc_prg16_at(c, 1, base);
    }
    uint32_t chr = ((uint32_t)chr_bank * 0x2000u) % nc_chr_size(c);
    nc_chr4_at(c, 0, chr);
    nc_chr4_at(c, 1, chr + 0x1000u);
    c->ppu->mirror = mirror ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
    return true;
}

const NesMapper nes_mapper_m228 = { 228, "Action 52", .reset = m228_reset, .write = m228_write };

/* ── VS. System ──────────────────────────────────────────────────────────── */

/* OUT2 ($4016 bit 2) picks the 8 KB CHR bank: 0 the first, 1 the second. */
static void vs_out(NesCart *c, uint8_t val) {
    if (c->chr_is_ram) return;
    uint32_t off = (val & 0x04u) ? 0x2000u : 0u;
    nc_chr4_at(c, 0, off);
    nc_chr4_at(c, 1, off + 0x1000u);
}

const NesMapper nes_mapper_vs = { 0, "VS. System", .prg_ram = always, .out = vs_out };
