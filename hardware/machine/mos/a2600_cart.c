/*
 * Atari 2600 cartridges - see a2600_cart.h for the schemes.  Detection and
 * the DPC / Supercharger details follow Stella's CartDetector, CartDPC and
 * CartAR, which in turn follow the hardware documentation of each board.
 */
#include "a2600_cart.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const struct { const char *name; A26CartType type; bool sc; } type_names[] = {
    { "2K", A26_CART_2K, false }, { "4K", A26_CART_4K, false },
    { "F8", A26_CART_F8, false }, { "F6", A26_CART_F6, false }, { "F4", A26_CART_F4, false },
    { "F8SC", A26_CART_F8, true }, { "F6SC", A26_CART_F6, true }, { "F4SC", A26_CART_F4, true },
    { "F8S", A26_CART_F8, true }, { "F6S", A26_CART_F6, true }, { "F4S", A26_CART_F4, true },
    { "EF", A26_CART_EF, false }, { "EFSC", A26_CART_EF, true },
    { "DF", A26_CART_DF, false }, { "DFSC", A26_CART_DF, true },
    { "BF", A26_CART_BF, false }, { "BFSC", A26_CART_BF, true },
    { "FE", A26_CART_FE, false }, { "E0", A26_CART_E0, false }, { "3F", A26_CART_3F, false }, { "3E", A26_CART_3E, false },
    { "E7", A26_CART_E7, false }, { "FA", A26_CART_FA, false }, { "F0", A26_CART_F0, false }, { "MB", A26_CART_F0, false },
    { "UA", A26_CART_UA, false }, { "CV", A26_CART_CV, false }, { "0840", A26_CART_0840, false },
    { "084", A26_CART_0840, false }, { "SB", A26_CART_SB, false }, { "DPC", A26_CART_DPC, false },
    { "AR", A26_CART_AR, false },
};

const char *a26_cart_type_name(A26CartType t) {
    for (size_t i = 0; i < sizeof(type_names) / sizeof(type_names[0]); i++)
        if (type_names[i].type == t && !type_names[i].sc) return type_names[i].name;
    return "?";
}

/* ── Detection ───────────────────────────────────────────────────────────── */

static int count_bytes(const uint8_t *img, uint32_t size, const uint8_t *sig, int len) {
    int n = 0;
    for (uint32_t i = 0; i + (uint32_t)len <= size; i++)
        if (memcmp(img + i, sig, (size_t)len) == 0) n++;
    return n;
}

static bool has_any(const uint8_t *img, uint32_t size, const uint8_t (*sigs)[5],
                    int n_sigs, int len, int min_count) {
    for (int i = 0; i < n_sigs; i++)
        if (count_bytes(img, size, sigs[i], len) >= min_count) return true;
    return false;
}

/* Superchip carts repeat the RAM area: the first 128 bytes of each 4K bank
 * equal the next 128. */
static bool probably_sc(const uint8_t *img, uint32_t size) {
    for (uint32_t off = 0; off + 4096 <= size; off += 4096)
        if (memcmp(img + off, img + off + 128, 128) != 0) return false;
    return true;
}

static bool probably_e0(const uint8_t *img, uint32_t size) {
    static const uint8_t sigs[][5] = {
        { 0x8D, 0xE0, 0x1F }, { 0x8D, 0xE0, 0x5F }, { 0x8D, 0xE9, 0xFF },
        { 0x0C, 0xE0, 0x1F }, { 0xAD, 0xE0, 0x1F }, { 0xAD, 0xE9, 0xFF },
        { 0xAD, 0xED, 0xFF }, { 0xAD, 0xF3, 0xBF },
    };
    return has_any(img, size, sigs, 8, 3, 1);
}

static bool probably_3f(const uint8_t *img, uint32_t size) {
    static const uint8_t sig[] = { 0x85, 0x3F };   /* STA $3F */
    return count_bytes(img, size, sig, 2) >= 2;
}

static bool probably_3e(const uint8_t *img, uint32_t size) {
    static const uint8_t sig[] = { 0x85, 0x3E, 0xA9, 0x00 };   /* STA $3E; LDA #0 */
    return count_bytes(img, size, sig, 4) >= 1;
}

static bool probably_ua(const uint8_t *img, uint32_t size) {
    static const uint8_t sigs[][5] = {
        { 0x8D, 0x40, 0x02 }, { 0xAD, 0x40, 0x02 }, { 0xBD, 0x1F, 0x02 },
    };
    return has_any(img, size, sigs, 3, 3, 1);
}

static bool probably_fe(const uint8_t *img, uint32_t size) {
    static const uint8_t sigs[][5] = {
        { 0x20, 0x00, 0xD0, 0xC6, 0xC5 }, { 0x20, 0xC3, 0xF8, 0xA5, 0x82 },
        { 0xD0, 0xFB, 0x20, 0x73, 0xFE }, { 0x20, 0x00, 0xF0, 0x84, 0xD6 },
    };
    return has_any(img, size, sigs, 4, 5, 1);
}

static bool probably_0840(const uint8_t *img, uint32_t size) {
    static const uint8_t sigs[][5] = {
        { 0xAD, 0x00, 0x08 }, { 0xAD, 0x40, 0x08 }, { 0x2C, 0x00, 0x08 },
    };
    return has_any(img, size, sigs, 3, 3, 1);
}

static bool probably_e7(const uint8_t *img, uint32_t size) {
    static const uint8_t sigs[][5] = {
        { 0xAD, 0xE2, 0xFF }, { 0xAD, 0xE5, 0xFF }, { 0xAD, 0xE5, 0x1F },
        { 0xAD, 0xE7, 0x1F }, { 0x0C, 0xE7, 0x1F }, { 0x8D, 0xE7, 0xFF },
        { 0x8D, 0xE7, 0x1F },
    };
    return has_any(img, size, sigs, 7, 3, 1);
}

static bool probably_cv(const uint8_t *img, uint32_t size) {
    static const uint8_t sigs[][5] = { { 0x9D, 0xFF, 0xF3 }, { 0x99, 0x00, 0xF4 } };
    return has_any(img, size, sigs, 2, 3, 1);
}

static bool probably_ef(const uint8_t *img, uint32_t size) {
    static const uint8_t sigs[][5] = {
        { 0x0C, 0xE0, 0xFF }, { 0xAD, 0xE0, 0xFF }, { 0x0C, 0xE0, 0x1F }, { 0xAD, 0xE0, 0x1F },
    };
    return has_any(img, size, sigs, 4, 3, 1) || count_bytes(img, size, (const uint8_t *)"EFEF", 4) ||
           count_bytes(img, size, (const uint8_t *)"EFSC", 4);
}

static void detect(A2600Cart *c) {
    const uint8_t *img = c->rom;
    uint32_t size = c->rom_size;
    c->sc = false;
    if (size % 8448 == 0)                     c->type = A26_CART_AR;
    else if (size <= 2048)                    c->type = probably_cv(img, size) ? A26_CART_CV : A26_CART_2K;
    else if (size == 4096)                    c->type = probably_cv(img, size) ? A26_CART_CV : A26_CART_4K;
    else if (size == 8192) {
        static const uint8_t f8a[] = { 0x8D, 0xF9, 0x1F }, f8b[] = { 0x8D, 0xF9, 0xFF };
        bool f8 = count_bytes(img, size, f8a, 3) >= 2 || count_bytes(img, size, f8b, 3) >= 2;
        if (probably_sc(img, size))                     { c->type = A26_CART_F8; c->sc = true; }
        else if (memcmp(img, img + 4096, 4096) == 0)    c->type = A26_CART_4K;
        else if (probably_e0(img, size))                c->type = A26_CART_E0;
        else if (probably_3e(img, size))                c->type = A26_CART_3E;
        else if (probably_3f(img, size))                c->type = A26_CART_3F;
        else if (probably_ua(img, size))                c->type = A26_CART_UA;
        else if (probably_fe(img, size) && !f8)         c->type = A26_CART_FE;
        else if (probably_0840(img, size))              c->type = A26_CART_0840;
        else                                            c->type = A26_CART_F8;
    }
    else if (size >= 10240 && size <= 10496)  c->type = A26_CART_DPC;
    else if (size == 12288)                   c->type = A26_CART_FA;
    else if (size == 16384) {
        if (probably_sc(img, size))           { c->type = A26_CART_F6; c->sc = true; }
        else if (probably_e7(img, size))      c->type = A26_CART_E7;
        else if (probably_3e(img, size))      c->type = A26_CART_3E;
        else if (probably_3f(img, size))      c->type = A26_CART_3F;
        else                                  c->type = A26_CART_F6;
    }
    else if (size == 32768) {
        if (probably_sc(img, size))           { c->type = A26_CART_F4; c->sc = true; }
        else if (probably_3e(img, size))      c->type = A26_CART_3E;
        else if (probably_3f(img, size))      c->type = A26_CART_3F;
        else                                  c->type = A26_CART_F4;
    }
    else if (size == 65536) {
        if (probably_3e(img, size))           c->type = A26_CART_3E;
        else if (probably_3f(img, size))      c->type = A26_CART_3F;
        else if (probably_ef(img, size))      { c->type = A26_CART_EF; c->sc = probably_sc(img, size); }
        else                                  c->type = A26_CART_F0;
    }
    else if (size == 131072) {
        if (probably_3e(img, size))           c->type = A26_CART_3E;
        else if (probably_3f(img, size))      c->type = A26_CART_3F;
        else                                  { c->type = A26_CART_DF; c->sc = probably_sc(img, size); }
    }
    else if (size == 262144) {
        if (probably_3e(img, size))           c->type = A26_CART_3E;
        else if (probably_3f(img, size))      c->type = A26_CART_3F;
        else                                  { c->type = A26_CART_BF; c->sc = probably_sc(img, size); }
    }
    else if (probably_3e(img, size))          c->type = A26_CART_3E;
    else if (probably_3f(img, size))          c->type = A26_CART_3F;
    else                                      c->type = A26_CART_4K;
}

/* ".f8", ".e0", ".f6sc"... */
static bool type_from_extension(A2600Cart *c, const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot || strchr(dot, '/')) return false;
    for (size_t i = 0; i < sizeof(type_names) / sizeof(type_names[0]); i++)
        if (strcasecmp(dot + 1, type_names[i].name) == 0) {
            c->type = type_names[i].type;
            c->sc = type_names[i].sc;
            return true;
        }
    return false;
}

/* ── Supercharger BIOS ───────────────────────────────────────────────────── *
 *
 * A stand-in for the Supercharger's 2K BIOS (loads come from the image, not
 * a tape).  $F80A (reset): clear the TIA and RAM, load #0.  $F800 (games'
 * multiload entry, load number in $FA): load that one.  Reading $F850 with
 * the BIOS mapped makes the emulator copy the load into RAM and leave its
 * bank configuration in $80 and start address in $FE/$FF; the code there
 * then builds "CMP $F000,X / LDA $FFF8 / JMP ($00FE)" in zero page, which
 * writes the configuration (the byte is latched by the $F0xx access) and
 * starts the game. */
static void build_ar_bios(uint8_t *rom) {
    static const uint8_t entry[] = {
        0xA5, 0xFA, 0x85, 0x80, 0x4C, 0x50, 0xF8,             /* F800 */
        0xEA, 0xEA, 0xEA,
        0x78, 0xD8, 0xA2, 0x00, 0x8A, 0x95, 0x00, 0xE8,       /* F80A */
        0xD0, 0xFB, 0x4C, 0x50, 0xF8,
    };
    static const uint8_t load[] = {
        0xA6, 0x80,                                           /* F850: LDX $80 */
        0xA9, 0xDD, 0x85, 0xF5, 0xA9, 0x00, 0x85, 0xF6,
        0xA9, 0xF0, 0x85, 0xF7, 0xA9, 0xAD, 0x85, 0xF8,
        0xA9, 0xF8, 0x85, 0xF9, 0xA9, 0xFF, 0x85, 0xFA,
        0xA9, 0x6C, 0x85, 0xFB, 0xA9, 0xFE, 0x85, 0xFC,
        0xA9, 0x00, 0x85, 0xFD,
        0x4C, 0xF5, 0x00,                                     /* JMP $00F5 */
    };
    memset(rom, 0xEA, 2048);
    memcpy(rom, entry, sizeof(entry));
    memcpy(rom + 0x50, load, sizeof(load));
    rom[0x7FC] = 0x0A; rom[0x7FD] = 0xF8;   /* reset */
    rom[0x7FE] = 0x0A; rom[0x7FF] = 0xF8;   /* IRQ/BRK */
}

/* ── Loading ─────────────────────────────────────────────────────────────── */

bool a26_cart_load(A2600Cart *c, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "atari2600: cannot open cartridge '%s'\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size <= 0 || size > 512 * 1024) {
        fprintf(stderr, "atari2600: '%s': unsupported cartridge size %ld\n", path, size);
        fclose(f);
        return false;
    }
    uint8_t *rom = malloc((size_t)size);
    if (!rom || fread(rom, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "atari2600: read error on '%s'\n", path);
        free(rom); fclose(f);
        return false;
    }
    fclose(f);

    double hz = c->cpu_hz;
    a26_cart_free(c);
    memset(c, 0, sizeof(*c));
    c->cpu_hz = hz;
    c->rom = rom;
    c->rom_size = (uint32_t)size;
    if (!type_from_extension(c, path)) detect(c);

    switch (c->type) {
    case A26_CART_E0: case A26_CART_E7: case A26_CART_3F: case A26_CART_3E:
        c->banks = c->rom_size / (c->type == A26_CART_E0 ? 1024u : 2048u);
        break;
    case A26_CART_AR:
        c->ar.load_count = c->rom_size / 8448u;
        c->banks = 4;
        break;
    case A26_CART_DPC:
        c->banks = 2;
        break;
    default:
        c->banks = c->rom_size >= 4096 ? c->rom_size / 4096u : 1;
        break;
    }
    if (c->banks == 0) c->banks = 1;
    if (c->sc)                     c->ram_size = 128;
    if (c->type == A26_CART_E7)    c->ram_size = 2048;
    if (c->type == A26_CART_FA)    c->ram_size = 256;
    if (c->type == A26_CART_CV)    c->ram_size = 1024;
    if (c->type == A26_CART_3E)    c->ram_size = A26_CART_RAM_MAX;
    if (c->type == A26_CART_AR)    c->ram_size = 6144;
    if (c->type == A26_CART_AR)    build_ar_bios(c->ar.bios);

    printf("atari2600: cartridge '%s' (%uK, %s%s)\n", path,
           (unsigned)((c->rom_size + 1023) / 1024), a26_cart_type_name(c->type),
           c->sc ? " + Superchip" : "");
    a26_cart_reset(c);
    return true;
}

void a26_cart_free(A2600Cart *c) {
    free(c->rom);
    c->rom = NULL;
    c->rom_size = 0;
}

static void ar_configure(A2600Cart *c, uint8_t config);

void a26_cart_reset(A2600Cart *c) {
    memset(c->bank, 0, sizeof(c->bank));
    c->fe_pending = false;
    switch (c->type) {
    case A26_CART_F8: case A26_CART_F6: case A26_CART_F4: case A26_CART_EF:
    case A26_CART_DF: case A26_CART_BF: case A26_CART_FA: case A26_CART_F0:
    case A26_CART_DPC:
        c->bank[0] = (uint16_t)(c->banks - 1);   /* start in the last bank */
        break;
    case A26_CART_E0:
        c->bank[0] = 4; c->bank[1] = 5; c->bank[2] = 6;
        break;
    case A26_CART_E7:
        c->bank[0] = 0; c->bank[1] = 0;
        break;
    case A26_CART_3F: case A26_CART_3E:
        c->bank[0] = 0; c->bank[1] = 0;   /* bank[1]: 0 = ROM, 1 = RAM */
        break;
    case A26_CART_AR:
        c->ar.data_pending = false;
        ar_configure(c, 0);
        break;
    default:
        break;
    }
    if (c->type == A26_CART_DPC) {
        memset(&c->dpc, 0, sizeof(c->dpc));
        c->dpc.random = 1;
    }
}

/* ── Hotspot helpers ─────────────────────────────────────────────────────── */

/* Atari-style hotspots: an access to base+n selects 4K bank n. */
static void hotspot_access(A2600Cart *c, uint16_t a) {
    uint16_t base;
    switch (c->type) {
    case A26_CART_F8: base = 0x1FF8; break;
    case A26_CART_F6: base = 0x1FF6; break;
    case A26_CART_F4: base = 0x1FF4; break;
    case A26_CART_EF: base = 0x1FE0; break;
    case A26_CART_DF: base = 0x1FC0; break;
    case A26_CART_BF: base = 0x1F80; break;
    case A26_CART_FA: base = 0x1FF8; break;
    case A26_CART_DPC: base = 0x1FF8; break;
    default: return;
    }
    if (a >= base && a < base + c->banks) c->bank[0] = (uint16_t)(a - base);
}

static void e0_access(A2600Cart *c, uint16_t a) {
    if (a >= 0x1FE0 && a <= 0x1FF7) c->bank[(a - 0x1FE0) >> 3] = a & 7u;
}

static void e7_access(A2600Cart *c, uint16_t a) {
    if (a >= 0x1FE0 && a <= 0x1FE7) c->bank[0] = a & 7u;
    else if (a >= 0x1FE8 && a <= 0x1FEB) c->bank[1] = a & 3u;
}

/* ── DPC ─────────────────────────────────────────────────────────────────── */

static void dpc_clock_random(A26Dpc *d) {
    uint8_t r = d->random;
    uint8_t parity = (uint8_t)(((r >> 7) ^ (r >> 5) ^ (r >> 4) ^ (r >> 3)) & 1u);
    d->random = (uint8_t)((r << 1) | (parity ^ 1u));
}

#define DPC_OSC_HZ 20000.0

static void dpc_update_music(A2600Cart *c, uint64_t cycle) {
    A26Dpc *d = &c->dpc;
    double clocks = (double)(cycle - d->music_cycle) * DPC_OSC_HZ / c->cpu_hz + d->music_frac;
    d->music_cycle = cycle;
    uint32_t whole = (uint32_t)clocks;
    d->music_frac = clocks - whole;
    if (!whole) return;
    for (int x = 5; x <= 7; x++) {
        if (!d->music[x - 5]) continue;
        int top = d->tops[x] + 1;
        int low = d->counters[x] & 0xFF;
        if (d->tops[x]) {
            low -= (int)(whole % (uint32_t)top);
            if (low < 0) low += top;
        } else {
            low = 0;
        }
        if (low <= d->bottoms[x])   d->flags[x] = 0x00;
        else if (low <= d->tops[x]) d->flags[x] = 0xFF;
        d->counters[x] = (uint16_t)((d->counters[x] & 0x0700u) | (uint16_t)low);
    }
}

static uint8_t dpc_read(A2600Cart *c, uint16_t a, uint64_t cycle) {
    A26Dpc *d = &c->dpc;
    int index = a & 7, function = (a >> 3) & 7;
    const uint8_t *display = c->rom + 8192;
    uint8_t result = 0;

    if ((d->counters[index] & 0xFF) == d->tops[index])         d->flags[index] = 0xFF;
    else if ((d->counters[index] & 0xFF) == d->bottoms[index]) d->flags[index] = 0x00;

    switch (function) {
    case 0:
        if (index < 4) {
            result = d->random;
        } else {
            static const uint8_t amplitude[8] = { 0x00, 0x04, 0x05, 0x09, 0x06, 0x0A, 0x0B, 0x0F };
            dpc_update_music(c, cycle);
            int i = 0;
            if (d->music[0] && d->flags[5]) i |= 1;
            if (d->music[1] && d->flags[6]) i |= 2;
            if (d->music[2] && d->flags[7]) i |= 4;
            result = amplitude[i];
        }
        break;
    case 1: result = display[2047 - d->counters[index]]; break;
    case 2: result = display[2047 - d->counters[index]] & d->flags[index]; break;
    case 7: result = d->flags[index]; break;
    default: break;
    }
    if (index < 5 || !d->music[index - 5])
        d->counters[index] = (uint16_t)((d->counters[index] - 1u) & 0x07FFu);
    return result;
}

static void dpc_write(A2600Cart *c, uint16_t a, uint8_t val, uint64_t cycle) {
    A26Dpc *d = &c->dpc;
    int index = a & 7, function = (a >> 3) & 7;
    dpc_update_music(c, cycle);
    switch (function) {
    case 0: d->tops[index] = val; d->flags[index] = 0; break;
    case 1: d->bottoms[index] = val; break;
    case 2:
        if (index >= 5 && d->music[index - 5])
            d->counters[index] = (uint16_t)((d->counters[index] & 0x0700u) | d->tops[index]);
        else
            d->counters[index] = (uint16_t)((d->counters[index] & 0x0700u) | val);
        break;
    case 3:
        d->counters[index] = (uint16_t)(((val & 7u) << 8) | (d->counters[index] & 0xFFu));
        if (index >= 5) d->music[index - 5] = val & 0x10u;
        break;
    case 6: d->random = 1; break;
    default: break;
    }
}

/* ── Supercharger ────────────────────────────────────────────────────────── */

static void ar_configure(A2600Cart *c, uint8_t config) {
    static const uint8_t map[8][2] = {   /* banks at $1000 and $1800; 3 = BIOS */
        { 2, 3 }, { 0, 3 }, { 2, 0 }, { 0, 2 }, { 2, 3 }, { 1, 3 }, { 2, 1 }, { 1, 2 },
    };
    A26Supercharger *ar = &c->ar;
    ar->control = config;
    ar->write_enabled = config & 0x02u;
    for (int i = 0; i < 2; i++) {
        uint8_t b = map[(config >> 2) & 7u][i];
        ar->page_rom[i] = b == 3;
        ar->page[i] = b == 3 ? 0 : b * 2048u;
    }
}

static uint8_t checksum(const uint8_t *p, int n) {
    uint8_t s = 0;
    for (int i = 0; i < n; i++) s = (uint8_t)(s + p[i]);
    return s;
}

/* Copy multiload number `load` into RAM; returns false if it's missing.
 * hdr receives the load's start address and bank configuration. */
static bool ar_load(A2600Cart *c, uint8_t load, uint8_t hdr[3]) {
    for (uint32_t i = 0; i < c->ar.load_count; i++) {
        const uint8_t *img = c->rom + i * 8448u;
        const uint8_t *h = img + 8192;
        if (h[5] != load) continue;
        if (checksum(h, 8) != 0x55)
            fprintf(stderr, "atari2600: Supercharger load %u header checksum is bad\n", load);
        for (uint32_t j = 0; j < h[3] && j < 32; j++) {
            uint32_t bank = h[16 + j] & 3u, page = (h[16 + j] >> 2) & 7u;
            if (bank < 3) memcpy(c->ram + bank * 2048u + page * 256u, img + j * 256u, 256);
        }
        hdr[0] = h[0]; hdr[1] = h[1]; hdr[2] = h[2];
        return true;
    }
    fprintf(stderr, "atari2600: Supercharger load %u is not in the image\n", load);
    return false;
}

/* The machine supplies RAM access for the BIOS hotspot (load number in
 * $80; config and start address written back to $80/$FE/$FF). */
static uint8_t (*ar_ram_peek)(void *, uint8_t);
static void    (*ar_ram_poke)(void *, uint8_t, uint8_t);
static void     *ar_ram_ud;

void a26_cart_set_ram_access(uint8_t (*peek)(void *, uint8_t),
                             void (*poke)(void *, uint8_t, uint8_t), void *ud) {
    ar_ram_peek = peek; ar_ram_poke = poke; ar_ram_ud = ud;
}

/* One cartridge access (read or write) to the Supercharger.  Writes reach
 * its RAM indirectly: an access to $1000-$10FF latches the low address
 * byte, and the fifth distinct bus access after it stores that byte at
 * the address being accessed, if writing is enabled. */
static uint8_t ar_access(A2600Cart *c, uint16_t a, uint64_t distinct) {
    A26Supercharger *ar = &c->ar;
    int half = (a & 0x0800u) ? 1 : 0;

    if ((a & 0x1FFFu) == 0x1850 && ar->page_rom[1] && ar_ram_peek) {
        uint8_t hdr[3];
        if (ar_load(c, ar_ram_peek(ar_ram_ud, 0x80), hdr)) {
            ar_ram_poke(ar_ram_ud, 0xFE, hdr[0]);
            ar_ram_poke(ar_ram_ud, 0xFF, hdr[1]);
            ar_ram_poke(ar_ram_ud, 0x80, hdr[2]);
        }
        return ar->bios[a & 0x07FFu];
    }

    if (ar->data_pending && distinct > ar->data_cycle + 5) ar->data_pending = false;

    if (!(a & 0x0F00u) && (!ar->write_enabled || !ar->data_pending)) {
        ar->data_hold = (uint8_t)a;
        ar->data_cycle = distinct;
        ar->data_pending = true;
    } else if ((a & 0x1FFFu) == 0x1FF8) {
        ar->data_pending = false;
        ar_configure(c, ar->data_hold);
    } else if (ar->write_enabled && ar->data_pending && distinct == ar->data_cycle + 5) {
        if (!ar->page_rom[half]) c->ram[ar->page[half] + (a & 0x07FFu)] = ar->data_hold;
        ar->data_pending = false;
    }
    return ar->page_rom[half] ? ar->bios[a & 0x07FFu] : c->ram[ar->page[half] + (a & 0x07FFu)];
}

/* ── Access ──────────────────────────────────────────────────────────────── */

/* Read through the current mapping, applying hotspots first. */
uint8_t a26_cart_read(A2600Cart *c, uint16_t a, uint8_t bus, uint64_t cycle) {
    a &= 0x1FFFu;
    if (!c->rom) return bus;
    uint16_t off = a & 0x0FFFu;
    switch (c->type) {
    case A26_CART_2K:  return c->rom[off % c->rom_size];
    case A26_CART_4K:  return c->rom[off % c->rom_size];
    case A26_CART_F8: case A26_CART_F6: case A26_CART_F4:
    case A26_CART_EF: case A26_CART_DF: case A26_CART_BF:
        if (c->sc && off < 0x100) {
            if (off < 0x80) { c->ram[off] = bus; return bus; }   /* read of the write port */
            return c->ram[off & 0x7Fu];
        }
        hotspot_access(c, a);
        return c->rom[c->bank[0] * 4096u + off];
    case A26_CART_FA:
        if (off < 0x100) { c->ram[off] = bus; return bus; }
        if (off < 0x200) return c->ram[off & 0xFFu];
        hotspot_access(c, a);
        return c->rom[c->bank[0] * 4096u + off];
    case A26_CART_F0:
        if (a == 0x1FF0) c->bank[0] = (uint16_t)((c->bank[0] + 1u) % c->banks);
        return c->rom[c->bank[0] * 4096u + off];
    case A26_CART_FE: case A26_CART_UA: case A26_CART_0840: case A26_CART_SB:
        return c->rom[(c->bank[0] % c->banks) * 4096u + off];
    case A26_CART_E0:
        e0_access(c, a);
        return c->rom[(off < 0xC00 ? c->bank[off >> 10] : 7u) * 1024u % c->rom_size + (off & 0x3FFu)];
    case A26_CART_3F: case A26_CART_3E:
        if (off >= 0x800) return c->rom[(c->banks - 1u) * 2048u + (off & 0x7FFu)];
        if (c->type == A26_CART_3E && c->bank[1]) {   /* RAM: read $1000-$13FF */
            uint32_t r = (c->bank[0] & 31u) * 1024u + (off & 0x3FFu);
            if (off >= 0x400) { c->ram[r] = bus; return bus; }
            return c->ram[r];
        }
        return c->rom[(c->bank[0] % c->banks) * 2048u + (off & 0x7FFu)];
    case A26_CART_E7:
        e7_access(c, a);
        if (off < 0x800) {
            if (c->bank[0] == 7) {   /* 1K RAM: write $1000-$13FF, read $1400-$17FF */
                if (off < 0x400) { c->ram[off] = bus; return bus; }
                return c->ram[off & 0x3FFu];
            }
            return c->rom[(c->bank[0] % c->banks) * 2048u + off];
        }
        if (off < 0x900) { c->ram[1024u + c->bank[1] * 256u + (off & 0xFFu)] = bus; return bus; }
        if (off < 0xA00) return c->ram[1024u + c->bank[1] * 256u + (off & 0xFFu)];
        return c->rom[(c->banks - 1u) * 2048u + (off & 0x7FFu)];
    case A26_CART_CV:
        if (off >= 0x800) return c->rom[(c->rom_size - 2048u) + (off & 0x7FFu)];
        if (off < 0x400) return c->ram[off];
        c->ram[off & 0x3FFu] = bus;
        return bus;
    case A26_CART_DPC:
        dpc_clock_random(&c->dpc);
        if (off < 0x40) return dpc_read(c, off, cycle);
        hotspot_access(c, a);
        return c->rom[c->bank[0] * 4096u + off];
    case A26_CART_AR:
        return ar_access(c, a, c->distinct);
    default:
        return bus;
    }
}

void a26_cart_write(A2600Cart *c, uint16_t a, uint8_t val, uint64_t cycle) {
    a &= 0x1FFFu;
    if (!c->rom) return;
    uint16_t off = a & 0x0FFFu;
    switch (c->type) {
    case A26_CART_F8: case A26_CART_F6: case A26_CART_F4:
    case A26_CART_EF: case A26_CART_DF: case A26_CART_BF:
        if (c->sc && off < 0x80) { c->ram[off] = val; return; }
        hotspot_access(c, a);
        break;
    case A26_CART_FA:
        if (off < 0x100) { c->ram[off] = val; return; }
        hotspot_access(c, a);
        break;
    case A26_CART_F0:
        if (a == 0x1FF0) c->bank[0] = (uint16_t)((c->bank[0] + 1u) % c->banks);
        break;
    case A26_CART_E0: e0_access(c, a); break;
    case A26_CART_3E:
        if (c->bank[1] && off >= 0x400 && off < 0x800)
            c->ram[(c->bank[0] & 31u) * 1024u + (off & 0x3FFu)] = val;
        break;
    case A26_CART_E7:
        e7_access(c, a);
        if (off < 0x400 && c->bank[0] == 7) c->ram[off] = val;
        else if (off >= 0x800 && off < 0x900) c->ram[1024u + c->bank[1] * 256u + (off & 0xFFu)] = val;
        break;
    case A26_CART_CV:
        if (off >= 0x400 && off < 0x800) c->ram[off & 0x3FFu] = val;
        break;
    case A26_CART_DPC:
        if (off >= 0x40 && off < 0x80) { dpc_write(c, off, val, cycle); return; }
        hotspot_access(c, a);
        break;
    case A26_CART_AR:
        (void)ar_access(c, a, c->distinct);
        break;
    default:
        break;
    }
}

void a26_cart_snoop(A2600Cart *c, uint16_t a, uint8_t val, bool write) {
    a &= 0x1FFFu;
    switch (c->type) {
    case A26_CART_FE:
        /* The byte after an access to $01FE is the high byte of a JSR/RTS
         * address: $Fxxx (bit 5 set) selects bank 0, $Dxxx bank 1. */
        if (c->fe_pending) {
            c->bank[0] = (val & 0x20u) ? 0 : 1;
            c->fe_pending = false;
        } else {
            c->fe_pending = a == 0x01FE;
        }
        break;
    case A26_CART_3F:
        if (write && a < 0x40) c->bank[0] = val;
        break;
    case A26_CART_3E:
        if (write && a == 0x3F) { c->bank[0] = val; c->bank[1] = 0; }
        else if (write && a == 0x3E) { c->bank[0] = val; c->bank[1] = 1; }
        break;
    case A26_CART_UA:
        if ((a & 0x1260u) == 0x0220u) c->bank[0] = 0;
        else if ((a & 0x1260u) == 0x0240u) c->bank[0] = 1;
        break;
    case A26_CART_0840:
        if ((a & 0x1840u) == 0x0800u) c->bank[0] = 0;
        else if ((a & 0x1840u) == 0x0840u) c->bank[0] = 1;
        break;
    case A26_CART_SB:
        if ((a & 0x1800u) == 0x0800u) c->bank[0] = (uint16_t)(a & (c->banks - 1u));
        break;
    default:
        break;
    }
}

uint8_t a26_cart_peek(const A2600Cart *c, uint16_t a) {
    if (!c->rom) return 0;
    uint16_t off = a & 0x0FFFu;
    switch (c->type) {
    case A26_CART_2K: case A26_CART_4K: return c->rom[off % c->rom_size];
    case A26_CART_E0:
        return c->rom[(off < 0xC00 ? c->bank[off >> 10] : 7u) * 1024u % c->rom_size + (off & 0x3FFu)];
    case A26_CART_3F: case A26_CART_3E:
        if (off >= 0x800) return c->rom[(c->banks - 1u) * 2048u + (off & 0x7FFu)];
        if (c->type == A26_CART_3E && c->bank[1])
            return c->ram[(c->bank[0] & 31u) * 1024u + (off & 0x3FFu)];
        return c->rom[(c->bank[0] % c->banks) * 2048u + (off & 0x7FFu)];
    case A26_CART_E7:
        if (off < 0x800) return c->bank[0] == 7 ? c->ram[off & 0x3FFu]
                                                : c->rom[(c->bank[0] % c->banks) * 2048u + off];
        if (off < 0xA00) return c->ram[1024u + c->bank[1] * 256u + (off & 0xFFu)];
        return c->rom[(c->banks - 1u) * 2048u + (off & 0x7FFu)];
    case A26_CART_CV:
        if (off >= 0x800) return c->rom[(c->rom_size - 2048u) + (off & 0x7FFu)];
        return c->ram[off & 0x3FFu];
    case A26_CART_AR: {
        int half = (off & 0x800u) ? 1 : 0;
        return c->ar.page_rom[half] ? c->ar.bios[off & 0x7FFu]
                                    : c->ram[c->ar.page[half] + (off & 0x7FFu)];
    }
    default:
        if ((c->sc && off < 0x100) || (c->type == A26_CART_FA && off < 0x200))
            return c->ram[off & (c->type == A26_CART_FA ? 0xFFu : 0x7Fu)];
        return c->rom[(c->bank[0] % c->banks) * 4096u + off];
    }
}

void a26_cart_describe(const A2600Cart *c, char *buf, int len) {
    if (!c->rom) { snprintf(buf, (size_t)len, "no cartridge"); return; }
    int n = snprintf(buf, (size_t)len, "type=%s%s size=%uK", a26_cart_type_name(c->type),
                     c->sc ? "SC" : "", (unsigned)((c->rom_size + 1023) / 1024));
    switch (c->type) {
    case A26_CART_E0:
        snprintf(buf + n, (size_t)(len - n), " slices=%u,%u,%u,7", c->bank[0], c->bank[1], c->bank[2]);
        break;
    case A26_CART_E7:
        snprintf(buf + n, (size_t)(len - n), " bank=%u%s ram256=%u", c->bank[0],
                 c->bank[0] == 7 ? "(RAM)" : "", c->bank[1]);
        break;
    case A26_CART_3E:
        snprintf(buf + n, (size_t)(len - n), " bank=%u %s", c->bank[0], c->bank[1] ? "RAM" : "ROM");
        break;
    case A26_CART_AR:
        snprintf(buf + n, (size_t)(len - n), " control=%02X loads=%u write=%d", c->ar.control,
                 c->ar.load_count, c->ar.write_enabled);
        break;
    case A26_CART_2K: case A26_CART_4K: case A26_CART_CV:
        break;
    default:
        snprintf(buf + n, (size_t)(len - n), " bank=%u/%u", c->bank[0], c->banks);
        break;
    }
}
