/* The NES cartridge slot: see nes_cart.h. */
#include "cart/nes_cart_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const NesMapper *const mappers[] = {
    &nes_mapper_nrom, &nes_mapper_mmc1, &nes_mapper_uxrom, &nes_mapper_cnrom,
    &nes_mapper_mmc3, &nes_mapper_mmc5, &nes_mapper_axrom, &nes_mapper_mmc2,
    &nes_mapper_vrc6a, &nes_mapper_vrc6b, &nes_mapper_gxrom, &nes_mapper_tqrom,
    &nes_mapper_m178, &nes_mapper_m228,
};

static const NesMapper *find_mapper(unsigned number) {
    for (size_t i = 0; i < sizeof(mappers) / sizeof(mappers[0]); i++)
        if (mappers[i]->number == number) return mappers[i];
    for (int i = 0; nes_boards[i]; i++)
        if (nes_boards[i]->number == number) return nes_boards[i];
    return NULL;
}

bool nes_cart_supported(unsigned mapper) { return find_mapper(mapper) != NULL; }

/* Windows before the board's reset: NROM's layout, CHR straight through. */
static void default_windows(NesCart *c) {
    for (int i = 0; i < 4; i++)
        c->prg_off[i] = (uint32_t)(c->info.prg_banks > 1 ? i : i & 1) * 0x2000u;
    for (int i = 0; i < 8; i++) c->chr_off[i] = (uint32_t)i * 0x400u;
    c->chr_wmask = c->chr_is_ram ? 0xFFu : 0u;
}

bool nes_cart_load_mem(NesCart *c, const uint8_t *data, size_t len, const char *name) {
    if (len < 16 || data[0] != 'N' || data[1] != 'E' || data[2] != 'S' || data[3] != 0x1A) {
        fprintf(stderr, "nes: '%s' is not a valid iNES file\n", name);
        return false;
    }
    const uint8_t *hdr = data;
    NesCartInfo info = {
        .prg_banks   = hdr[4],
        .chr_banks   = hdr[5],
        .mapper      = (uint8_t)((hdr[7] & 0xF0) | (hdr[6] >> 4)),
        .has_battery = (hdr[6] >> 1) & 1,
    };
    bool four_screen = (hdr[6] >> 3) & 1;
    bool vertical    = hdr[6] & 1;
    if      (four_screen) info.mirror = RP2C02_MIRROR_4SCREEN;
    else if (vertical)    info.mirror = RP2C02_MIRROR_VERTICAL;
    else                  info.mirror = RP2C02_MIRROR_HORIZONTAL;

    const NesMapper *ops = find_mapper(info.mapper);
    if (!ops) {
        fprintf(stderr, "nes: mapper %u not supported!\n", info.mapper);
        return false;
    }
    if (info.prg_banks == 0) {
        fprintf(stderr, "nes: invalid PRG bank count 0\n");
        return false;
    }
    if ((info.mapper == 0 || info.mapper == 3) && info.prg_banks > 2) {
        fprintf(stderr, "nes: %s requires 1 or 2 PRG banks, got %u\n",
                info.mapper ? "CNROM" : "NROM", info.prg_banks);
        return false;
    }

    /* Skip the optional 512-byte trainer */
    size_t data_start = 16u + (((hdr[6] >> 2) & 1) ? 512u : 0u);
    long data_len = (long)len - (long)data_start;

    /* Some dumps claim more PRG than the file holds; if what's there is a
     * whole number of 16 KB banks plus the stated CHR, believe the file. */
    long chr_len  = (long)info.chr_banks * 0x2000;
    long prg_have = data_len - chr_len;
    if (prg_have > 0 && prg_have < (long)info.prg_banks * 0x4000 && prg_have % 0x4000 == 0) {
        fprintf(stderr, "nes: header says %u×16KB PRG but the file has %ld; using the file\n",
                info.prg_banks, prg_have / 0x4000);
        info.prg_banks = (uint8_t)(prg_have / 0x4000);
    }

    size_t prg_bytes = (size_t)info.prg_banks * 0x4000u;
    size_t avail = data_len > 0 ? (size_t)data_len : 0;
    uint8_t *prg = malloc(prg_bytes);
    if (!prg || avail < prg_bytes) {
        fprintf(stderr, "nes: failed to read PRG ROM\n");
        free(prg);
        return false;
    }
    memcpy(prg, data + data_start, prg_bytes);
    avail -= prg_bytes;

    uint8_t *chr;
    bool chr_is_ram;
    if (info.chr_banks > 0) {
        size_t chr_bytes = (size_t)info.chr_banks * 0x2000u;
        /* TQROM: 8 KB of CHR RAM after the ROM */
        size_t extra = ops == &nes_mapper_tqrom ? 0x2000u : 0u;
        chr = calloc(1, chr_bytes + extra);
        if (!chr || avail < chr_bytes) {
            fprintf(stderr, "nes: failed to read CHR ROM\n");
            free(chr); free(prg);
            return false;
        }
        memcpy(chr, data + data_start + prg_bytes, chr_bytes);
        chr_is_ram = false;
    } else {
        /* CHR RAM: 8 KB writable by PPU */
        chr = calloc(1, 0x2000);
        if (!chr) { free(prg); return false; }
        chr_is_ram = true;
        info.chr_banks = 1;
    }

    c->info = info;
    c->ops = ops;
    c->prg = prg;
    c->chr = chr;
    c->chr_is_ram = chr_is_ram;
    default_windows(c);
    printf("nes: loaded '%s' - mapper %u, %u×16KB PRG, %u×8KB CHR%s\n",
           name, info.mapper, info.prg_banks, info.chr_banks,
           chr_is_ram ? " (RAM)" : "");
    return true;
}

bool nes_cart_load(NesCart *c, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nes: cannot open '%s'\n", path); return false; }
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 1u << 20;
            uint8_t *p = realloc(buf, cap);
            if (!p) { free(buf); fclose(f); return false; }
            buf = p;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        if (n == 0) break;
        len += n;
    }
    fclose(f);
    bool ok = nes_cart_load_mem(c, buf, len, path);
    free(buf);
    return ok;
}

bool nes_cart_init_vs(NesCart *c, uint8_t *prg32k, uint8_t *chr16k) {
    c->info = (NesCartInfo){ .prg_banks = 2, .chr_banks = chr16k ? 2 : 0,
                             .mapper = 0, .mirror = RP2C02_MIRROR_4SCREEN };
    c->ops = &nes_mapper_vs;
    c->prg = prg32k;
    c->chr = chr16k ? chr16k : calloc(1, 0x2000);
    if (!c->chr) return false;
    c->chr_is_ram = chr16k == NULL;
    default_windows(c);
    return true;
}

bool nes_cart_init_chr_ram(NesCart *c) {
    c->info = (NesCartInfo){ .chr_banks = 1, .mirror = RP2C02_MIRROR_HORIZONTAL };
    c->ops = &nes_mapper_nrom;
    c->prg = NULL;
    c->chr = calloc(1, 0x2000);
    if (!c->chr) return false;
    c->chr_is_ram = true;
    default_windows(c);
    return true;
}

void nes_cart_free(NesCart *c) {
    free(c->prg);
    free(c->chr);
    c->prg = c->chr = NULL;
    c->chr_is_ram = false;
    c->chr_wmask = 0;
    c->ops = &nes_mapper_nrom;
    memset(&c->m, 0, sizeof(c->m));
}

void nes_cart_reset(NesCart *c) {
    if (c->ops->reset) c->ops->reset(c);
}

bool nes_cart_prg_ram_mapped(const NesCart *c) {
    return c->ops->prg_ram && c->ops->prg_ram(c);
}

uint8_t nes_cart_read(NesCart *c, uint16_t addr, uint8_t bus) {
    uint8_t v;
    if (c->ops->read && c->ops->read(c, addr, &v)) return v;
    if (addr >= 0x6000 && nes_cart_prg_ram_mapped(c)) return c->prg_ram[addr & 0x1FFFu];
    return bus;
}

void nes_cart_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (c->ops->write && c->ops->write(c, addr, val)) return;
    if (addr >= 0x6000 && addr < 0x8000 && nes_cart_prg_ram_mapped(c))
        c->prg_ram[addr & 0x1FFFu] = val;
}

void nes_cart_out(NesCart *c, uint8_t val) {
    if (c->ops->out) c->ops->out(c, val);
}

bool nes_cart_peek(const NesCart *c, uint16_t addr, uint8_t *val) {
    if (addr < 0x6000) return c->ops->peek && c->ops->peek(c, addr, val);
    if (addr < 0x8000) {
        if (!nes_cart_prg_ram_mapped(c)) return false;
        *val = c->prg_ram[addr & 0x1FFFu];
        return true;
    }
    if (!c->prg) return false;
    *val = nes_cart_prg(c, addr);
    return true;
}

uint8_t nes_cart_chr_read(uint16_t addr, void *ud) {
    NesCart *c = ud;
    if (!c->chr) return 0;
    if (c->ops->chr_read) return c->ops->chr_read(c, addr);
    return nc_chr_win(c, addr);
}

void nes_cart_chr_write(uint16_t addr, uint8_t val, void *ud) {
    NesCart *c = ud;
    unsigned w = (addr >> 10) & 7u;
    if ((c->chr_wmask >> w) & 1u) c->chr[c->chr_off[w] + (addr & 0x3FFu)] = val;
}

uint8_t nes_cart_nt_read(uint16_t addr, void *ud) {
    NesCart *c = ud;
    return c->ops->nt_read(c, addr);
}

void nes_cart_nt_write(uint16_t addr, uint8_t val, void *ud) {
    NesCart *c = ud;
    c->ops->nt_write(c, addr, val);
}

void nes_cart_scanline(void *ud) {
    NesCart *c = ud;
    c->ops->scanline(c);
}

uint8_t *nes_cart_chr_ram(const NesCart *c, size_t *len) {
    if (c->chr && c->chr_is_ram) { *len = (size_t)c->info.chr_banks * 0x2000u; return c->chr; }
    if (c->chr && c->ops == &nes_mapper_tqrom) {
        *len = 0x2000u;
        return c->chr + (size_t)c->info.chr_banks * 0x2000u;
    }
    *len = 0;
    return NULL;
}

void nes_cart_strip(NesCart *c) {
    c->ops = NULL;
    c->prg = c->chr = NULL;
    c->ppu = NULL;
    c->irq_changed = NULL;
    c->ud = NULL;
}

void nes_cart_rewire(NesCart *c, const NesCart *live) {
    c->ops = live->ops;
    c->prg = live->prg;
    c->chr = live->chr;
    c->ppu = live->ppu;
    c->irq_changed = live->irq_changed;
    c->ud = live->ud;
}

void nes_cart_dump(const NesCart *c, char *buf, size_t len) {
    int n = snprintf(buf, len,
        "mapper=%u (%s) irq=%d\n"
        "prg windows ($8000,$A000,$C000,$E000)=%06X %06X %06X %06X\n"
        "chr windows (1KB x8)=%05X %05X %05X %05X %05X %05X %05X %05X ram=%02X\n",
        c->info.mapper, c->ops->name, c->irq,
        c->prg_off[0], c->prg_off[1], c->prg_off[2], c->prg_off[3],
        c->chr_off[0], c->chr_off[1], c->chr_off[2], c->chr_off[3],
        c->chr_off[4], c->chr_off[5], c->chr_off[6], c->chr_off[7], c->chr_wmask);
    if (n > 0 && (size_t)n < len && c->ops->dump) c->ops->dump(c, buf + n, len - (size_t)n);
}
