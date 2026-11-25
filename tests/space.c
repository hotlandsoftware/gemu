/* Address spaces (core/src/dev/space.c).  Run: ninja check-space [-- seed]
 *
 * Fixed cases for each feature, then random maps checked byte by byte
 * against a brute-force model of the decode rules in gemu/dev/space.h. */
#include "gemu/dev/space.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

/* ── A test device: registers that log their accesses ── */

typedef struct {
    uint8_t  id;
    unsigned reads, writes;
    uint64_t last_off, last_v;
    unsigned last_bytes;
} Dev;

static uint8_t  d_r8 (void *ud, uint64_t off) { Dev *d = ud; d->reads++; d->last_off = off; d->last_bytes = 1; return (uint8_t)(d->id + off); }
static uint16_t d_r16(void *ud, uint64_t off) { Dev *d = ud; d->reads++; d->last_off = off; d->last_bytes = 2; return (uint16_t)(0x1100 * d->id + off); }
static uint32_t d_r32(void *ud, uint64_t off) { Dev *d = ud; d->reads++; d->last_off = off; d->last_bytes = 4; return 0xA0B0C0D0u + (uint32_t)off; }
static void d_w8 (void *ud, uint64_t off, uint8_t v)  { Dev *d = ud; d->writes++; d->last_off = off; d->last_v = v; d->last_bytes = 1; }
static void d_w16(void *ud, uint64_t off, uint16_t v) { Dev *d = ud; d->writes++; d->last_off = off; d->last_v = v; d->last_bytes = 2; }
static bool d_peek(void *ud, uint64_t off, uint8_t *v) { *v = (uint8_t)(((Dev *)ud)->id + off); return true; }

static const GemuHandlers H8   = { .r8 = d_r8, .w8 = d_w8, .peek8 = d_peek };
static const GemuHandlers H8NP = { .r8 = d_r8, .w8 = d_w8 };
static const GemuHandlers H16  = { .r16 = d_r16, .w16 = d_w16 };
static const GemuHandlers H32R = { .r32 = d_r32 };
static const GemuHandlers HW8  = { .w8 = d_w8 };

static unsigned unmapped_reads, unmapped_writes;
static uint64_t unmapped_addr;
static void on_ur(void *ud, uint64_t a, unsigned n) { (void)ud; (void)n; unmapped_reads++; unmapped_addr = a; }
static void on_uw(void *ud, uint64_t a, unsigned n, uint64_t v) { (void)ud; (void)n; (void)v; unmapped_writes++; unmapped_addr = a; }

static void fixed_cases(void) {
    /* ── RAM: the fast path, byte order, page crossing ── */
    GemuSpace *s = gemu_space_new(NULL, "mem", 32, 32, GEMU_LITTLE_ENDIAN);
    CHECK(s && gemu_space_addr_bits(s) == 32 && !strcmp(gemu_space_name(s), "mem"));
    static uint8_t ram[0x10000];
    GemuMapping *rm = gemu_space_map_ram(s, 0x10000, sizeof ram, ram, 0, 0);
    CHECK(rm);
    gemu_space_write32(s, 0x10010, 0x11223344);
    CHECK(ram[0x10] == 0x44 && ram[0x13] == 0x11);
    CHECK(gemu_space_read16(s, 0x10011) == 0x2233);
    gemu_space_write64(s, 0x10FFC, 0x8877665544332211ull);    /* across a page */
    CHECK(ram[0xFFC] == 0x11 && ram[0x1003] == 0x88);
    CHECK(gemu_space_read64(s, 0x10FFC) == 0x8877665544332211ull);
    uint64_t avail;
    CHECK(gemu_space_ptr(s, 0x10FFC, false, &avail) == ram + 0xFFC && avail == 4);
    CHECK(gemu_space_ptr(s, 0x10FFC, true, NULL) == ram + 0xFFC);
    CHECK(!gemu_space_ptr(s, 0x20000, false, NULL));
    /* Unmapped: all ones, hooks called; reads straddling RAM and a hole. */
    CHECK(gemu_space_read32(s, 0x40000) == 0xFFFFFFFF);
    gemu_space_set_unmapped(s, 0x5A5A5A5A5A5A5A5Aull, on_ur, on_uw, NULL);
    CHECK(gemu_space_read8(s, 0x40000) == 0x5A && unmapped_reads == 1 && unmapped_addr == 0x40000);
    gemu_space_write16(s, 0x40002, 1);
    CHECK(unmapped_writes == 1);
    ram[0xFFFF] = 0x77;
    CHECK(gemu_space_read16(s, 0x1FFFF) == 0x5A77);           /* RAM byte, hole byte */
    CHECK(unmapped_reads == 2 && unmapped_addr == 0x20000);
    uint8_t b;
    CHECK(gemu_space_peek8(s, 0x10010, &b) && b == 0x44);
    CHECK(!gemu_space_peek8(s, 0x40000, &b) && unmapped_reads == 2);
    /* Addresses wrap at the space's width. */
    CHECK(gemu_space_read8(s, 0x100010010ull) == 0x44);

    /* Big-endian. */
    GemuSpace *be = gemu_space_new(NULL, "be", 24, 16, GEMU_BIG_ENDIAN);
    static uint8_t bram[0x100];
    gemu_space_map_ram(be, 0, sizeof bram, bram, 0, 0);
    gemu_space_write32(be, 4, 0x11223344);
    CHECK(bram[4] == 0x11 && bram[7] == 0x44 && gemu_space_read16(be, 6) == 0x3344);
    Dev bd = { .id = 0x40 };
    gemu_space_map(be, 0x1000, 0x100, &H8, &bd, 0, 0);
    CHECK(gemu_space_read16(be, 0x1002) == 0x4243);          /* two r8, big-endian */
    gemu_space_write16(be, 0x1004, 0xABCD);
    CHECK(bd.writes == 2 && bd.last_off == 5 && bd.last_v == 0xCD);
    gemu_space_free(be);

    /* ── ROM over RAM: reads from the ROM, writes to the RAM (shadowing) ── */
    static uint8_t rom[0x1000];
    memset(rom, 0xEA, sizeof rom);
    GemuMapping *romm = gemu_space_map_ram(s, 0x18000, sizeof rom, rom, GEMU_MAP_READONLY, 1);
    gemu_space_write8(s, 0x18000, 0x12);
    CHECK(ram[0x8000] == 0x12 && rom[0] == 0xEA && gemu_space_read8(s, 0x18000) == 0xEA);
    CHECK(gemu_space_ptr(s, 0x18000, false, NULL) == rom);
    CHECK(gemu_space_ptr(s, 0x18000, true, NULL) == ram + 0x8000);
    uint64_t gen = gemu_space_generation(s);
    gemu_mapping_enable(romm, false);
    CHECK(gemu_space_read8(s, 0x18000) == 0x12 && gemu_space_generation(s) > gen);
    gemu_mapping_enable(romm, true);
    CHECK(gemu_space_read8(s, 0x18000) == 0xEA);
    /* Bank switching: a new pointer, no rebuild. */
    static uint8_t rom2[0x1000];
    memset(rom2, 0x60, sizeof rom2);
    gen = gemu_space_generation(s);
    gemu_mapping_set_ptr(romm, rom2);
    CHECK(gemu_space_read8(s, 0x18123) == 0x60 && gemu_space_ptr(s, 0x18000, false, NULL) == rom2);
    CHECK(gemu_space_generation(s) == gen + 1);
    /* Move it (a PCI BAR, a shadow region). */
    CHECK(gemu_mapping_move(romm, 0x30000, 0x1000));
    CHECK(gemu_space_read8(s, 0x18000) == 0x12 && gemu_space_read8(s, 0x30010) == 0x60);
    CHECK(!gemu_mapping_move(romm, 0xFFFFF800, 0x1000));      /* past the end */
    CHECK(romm->base == 0x30000);

    /* ── Handlers: a register block in the middle of a RAM page ── */
    Dev d = { .id = 0x10 };
    GemuMapping *hm = gemu_space_map(s, 0x12040, 0x10, &H8, &d, 5, 0);
    gemu_mapping_set_name(hm, "regs");
    CHECK(!strcmp(hm->name, "regs"));
    CHECK(gemu_space_read8(s, 0x12043) == 0x13 && d.last_off == 3);
    CHECK(!gemu_space_ptr(s, 0x12000, false, NULL));          /* page no longer direct */
    ram[0x203F] = 0x99;
    CHECK(gemu_space_read8(s, 0x1203F) == 0x99);
    /* r8 only: a 32-bit read is four, in order; straddling RAM and regs too */
    d.reads = 0;
    CHECK(gemu_space_read32(s, 0x12040) == 0x13121110 && d.reads == 4);
    CHECK(gemu_space_read16(s, 0x1203F) == 0x1099);
    gemu_space_write32(s, 0x1204C, 0xDDCCBBAA);
    CHECK(d.last_off == 0xF && d.last_v == 0xDD);
    CHECK(gemu_space_peek8(s, 0x12041, &b) && b == 0x11);
    unsigned r0 = d.reads;
    CHECK(gemu_space_peek8(s, 0x12041, &b) && d.reads == r0);  /* no side effects */
    gemu_mapping_remove(hm);
    CHECK(gemu_space_read8(s, 0x12043) == ram[0x2043]);
    CHECK(gemu_space_ptr(s, 0x12000, false, NULL) == ram + 0x2000);  /* direct again */

    /* r16 only: an 8-bit read takes its half; an 8-bit write is dropped */
    Dev d16 = { .id = 2 };
    gemu_space_map(s, 0x50000, 0x10, &H16, &d16, 0, 0);
    CHECK(gemu_space_read8(s, 0x50005) == 0x22 && d16.last_off == 4);   /* 0x2204 >> 8 */
    CHECK(gemu_space_read8(s, 0x50004) == 0x04);
    CHECK(gemu_space_read32(s, 0x50004) == 0x22062204);
    d16.writes = 0;
    gemu_space_write8(s, 0x50004, 1);
    CHECK(d16.writes == 0);
    gemu_space_write32(s, 0x50008, 0x12345678);
    CHECK(d16.writes == 2 && d16.last_off == 10 && d16.last_v == 0x1234);
    /* r32 only, read as bytes */
    Dev d32 = { 0 };
    gemu_space_map(s, 0x50100, 0x10, &H32R, &d32, 0, 0);
    CHECK(gemu_space_read8(s, 0x50106) == 0xB0 && d32.last_off == 4);
    CHECK(!gemu_space_peek8(s, 0x50106, &b));                 /* no peek8 hook */
    gemu_space_write8(s, 0x50106, 1);                         /* no writes: unmapped */
    CHECK(unmapped_writes == 2);

    /* ── Write-only registers over ROM (an NES mapper over PRG) ── */
    Dev mapper = { 0 };
    static uint8_t prg[0x100];
    memset(prg, 0x4C, sizeof prg);
    gemu_space_map_ram(s, 0x60000, sizeof prg, prg, GEMU_MAP_READONLY, 0);
    gemu_space_map(s, 0x60000, 0x100, &HW8, &mapper, 0, 0);
    CHECK(gemu_space_read8(s, 0x60080) == 0x4C);
    gemu_space_write8(s, 0x60080, 7);
    CHECK(mapper.writes == 1 && mapper.last_off == 0x80 && prg[0x80] == 0x4C);
    CHECK(gemu_space_ptr(s, 0x60000, false, NULL) == NULL);   /* page is 4K, prg 256 */

    /* ── Priority and equal-priority replacement ── */
    Dev lo = { .id = 0x20 }, hi = { .id = 0x30 }, nw = { .id = 0x40 };
    gemu_space_map(s, 0x70000, 0x100, &H8, &lo, 0, 0);
    GemuMapping *him = gemu_space_map(s, 0x70080, 0x10, &H8, &hi, 1, 0);
    CHECK(gemu_space_read8(s, 0x70081) == 0x31 && gemu_space_read8(s, 0x7007F) == 0x9F);
    GemuMapping *nwm = gemu_space_map(s, 0x70000, 0x100, &H8, &nw, 0, 0);
    CHECK(gemu_space_read8(s, 0x70001) == 0x41);              /* newest wins the tie */
    CHECK(gemu_space_read8(s, 0x70081) == 0x31);              /* priority still wins */
    gemu_mapping_remove(nwm);
    CHECK(gemu_space_read8(s, 0x70001) == 0x21);
    gemu_mapping_remove(him);
    CHECK(gemu_space_read8(s, 0x70081) == 0xA1);

    /* ── Stacked (SHARED) handlers on one port: 86Box io_sethandler ── */
    GemuSpace *io = gemu_space_new(NULL, "io", 16, 8, GEMU_LITTLE_ENDIAN);
    Dev a = { .id = 0xF0 }, c = { .id = 0x3C };
    gemu_space_map(io, 0x388, 2, &H8, &a, 0, GEMU_MAP_SHARED);
    gemu_space_map(io, 0x388, 2, &H8, &c, 0, GEMU_MAP_SHARED);
    CHECK(gemu_space_read8(io, 0x388) == (0xF0 & 0x3C));      /* ANDed */
    gemu_space_write8(io, 0x389, 0x55);
    CHECK(a.writes == 1 && c.writes == 1 && a.last_v == 0x55 && c.last_off == 1);
    CHECK(gemu_space_peek8(io, 0x388, &b) && b == 0x30);
    /* removed by identity: same range, handlers and ud */
    GemuHandlers copy = H8;
    CHECK(gemu_space_unmap(io, 0x388, 2, &copy, &a) == 1);
    CHECK(gemu_space_unmap(io, 0x388, 2, &copy, &a) == 0);
    CHECK(gemu_space_unmap(io, 0x388, 1, &H8, &c) == 0);      /* other size */
    CHECK(gemu_space_read8(io, 0x388) == 0x3C);
    /* a non-shared mapping at that priority joins the stack */
    Dev e = { .id = 0x0F };
    GemuMapping *em = gemu_space_map(io, 0x388, 1, &H8, &e, 0, 0);
    CHECK(gemu_space_read8(io, 0x388) == (0x3C & 0x0F));
    gemu_mapping_remove(em);

    /* ── Mirrors: the 5150's 10-bit decode, the NES's 2K of RAM ── */
    Dev pic = { .id = 0x80 };
    gemu_space_map(io, 0x20, 2, &H8NP, &pic, 0, 0);
    CHECK(gemu_space_read8(io, 0x421) == 0xFF);
    CHECK(gemu_space_mirror(io, 0, 0x10000, 0x3FF));
    CHECK(gemu_space_read8(io, 0x421) == 0x81 && pic.last_off == 1);
    CHECK(gemu_space_read8(io, 0xFC20) == 0x80);
    CHECK(gemu_space_read8(io, 0x788) == 0x3C);               /* the stack, mirrored */
    /* mappings added after the mirror show through it as well */
    Dev late = { .id = 0x50 };
    gemu_space_map(io, 0x60, 1, &H8NP, &late, 0, 0);
    CHECK(gemu_space_read8(io, 0x8460) == 0x50);
    gemu_space_free(io);

    GemuSpace *nes = gemu_space_new(NULL, "nes", 16, 8, GEMU_LITTLE_ENDIAN);
    static uint8_t wram[0x800];
    GemuMapping *wm = gemu_space_map_ram(nes, 0, 0x800, wram, 0, 0);
    gemu_space_mirror(nes, 0, 0x2000, 0x7FF);
    gemu_space_write8(nes, 0x1803, 0x42);
    CHECK(wram[3] == 0x42 && gemu_space_read8(nes, 0x0803) == 0x42);
    CHECK(gemu_space_read16(nes, 0x07FF) == (wram[0x7FF] | (wram[0] << 8)));   /* wraps to 0 */
    CHECK(gemu_space_ptr(nes, 0x1800, false, &avail) == wram && avail == 256);
    static uint8_t wram2[0x800];
    wram2[3] = 0x24;
    gemu_mapping_set_ptr(wm, wram2);
    CHECK(gemu_space_read8(nes, 0x1803) == 0x24 && gemu_space_ptr(nes, 0x1800, false, NULL) == wram2);
    CHECK(gemu_space_read8(nes, 0x2000) == 0xFF);
    gemu_space_free(nes);

    /* ── Bad arguments ── */
    CHECK(!gemu_space_map_ram(s, 0, 0, ram, 0, 0));
    CHECK(!gemu_space_map_ram(s, 0xFFFFFFFF, 2, ram, 0, 0));
    CHECK(!gemu_space_map_ram(s, 0x100000000ull, 1, ram, 0, 0));
    CHECK(!gemu_space_map_ram(s, 0, 1, NULL, 0, 0));
    CHECK(!gemu_space_map(s, 0, 1, NULL, NULL, 0, 0));
    CHECK(!gemu_space_new(NULL, "x", 0, 8, GEMU_LITTLE_ENDIAN));
    CHECK(!gemu_space_new(NULL, "x", 65, 8, GEMU_LITTLE_ENDIAN));
    CHECK(!gemu_space_new(NULL, "x", 16, 12, GEMU_LITTLE_ENDIAN));
    /* the last byte of the space */
    CHECK(gemu_space_map_ram(s, 0xFFFFFFFF, 1, ram, 0, 0));
    CHECK(gemu_space_read8(s, 0xFFFFFFFF) == ram[0]);
    /* a read wrapping off the top: the top byte, then address 0 */
    CHECK(gemu_space_read16(s, 0xFFFFFFFF) == (0x5A00 | ram[0]));
    gemu_space_free(s);

    /* ── 64-bit addresses ── */
    GemuSpace *big = gemu_space_new(NULL, "phys", 64, 64, GEMU_LITTLE_ENDIAN);
    static uint8_t hiram[0x2000];
    gemu_space_map_ram(big, 0xFFFFFFFFFFFFE000ull, sizeof hiram, hiram, 0, 0);
    gemu_space_write64(big, 0xFFFFFFFFFFFFFFF8ull, 0x0102030405060708ull);
    CHECK(hiram[0x1FF8] == 8 && gemu_space_read64(big, 0xFFFFFFFFFFFFFFF8ull) == 0x0102030405060708ull);
    Dev far = { .id = 1 };
    gemu_space_map(big, 0x00000FED00000000ull, 0x1000, &H8, &far, 0, 0);
    CHECK(gemu_space_read8(big, 0x00000FED00000010ull) == 0x11);
    CHECK(gemu_space_read8(big, 0x10000000000ull) == 0xFF);
    gemu_space_free(big);

    /* ── Spaces belong to a machine ── */
    GemuMachine *m = gemu_machine_new("t");
    GemuSpace *ms = gemu_space_new(m, "program", 16, 8, GEMU_LITTLE_ENDIAN);
    CHECK(ms && gemu_space_find(m, "program") == ms && !gemu_space_find(m, "io"));
    CHECK(!gemu_space_new(m, "program", 16, 8, GEMU_LITTLE_ENDIAN));   /* duplicate */
    GemuSpace *mio = gemu_space_new(m, "io", 16, 8, GEMU_LITTLE_ENDIAN);
    gemu_space_free(mio);
    CHECK(!gemu_space_find(m, "io") && gemu_space_find(m, "program") == ms);
    gemu_space_map_ram(ms, 0, 0x100, ram, 0, 0);
    gemu_machine_free(m);                                   /* frees ms */
}

/* ── Random maps against a model ─────────────────────────────────────────── */

#define SPACE_BITS 16
#define NADDR (1u << SPACE_BITS)
#define MAXMAP 24

typedef struct {
    bool     used, ram, enabled;
    unsigned flags;
    int      prio;
    uint64_t seq;
    uint32_t base, size;
    uint8_t *mem;                 /* RAM: own buffer of `size` */
    GemuHandlers h;
    Dev      dev;
    GemuMapping *mp;
} MMap;

static MMap mm[MAXMAP];
static uint64_t mseq;
typedef struct { uint32_t lo, hi, mask; } MMirror;
static MMirror mirrors[4];
static int nmirrors;

static uint32_t rnd_state;
static uint32_t rnd(void) {
    rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5;
    return rnd_state;
}

/* Handlers in the random test: one byte wide (so a model byte = an access) */
static uint8_t m_r8(void *ud, uint64_t off) { Dev *d = ud; d->reads++; return (uint8_t)(d->id * 7 + off * 13); }
static void m_w8(void *ud, uint64_t off, uint8_t v) { Dev *d = ud; d->writes++; d->last_off = off; d->last_v = v; }
static bool m_peek(void *ud, uint64_t off, uint8_t *v) { Dev *d = ud; *v = (uint8_t)(d->id * 7 + off * 13); return true; }
static const GemuHandlers MH_RW = { .r8 = m_r8, .w8 = m_w8, .peek8 = m_peek };
static const GemuHandlers MH_R  = { .r8 = m_r8, .peek8 = m_peek };
static const GemuHandlers MH_W  = { .w8 = m_w8 };
static const GemuHandlers MH_RNP = { .r8 = m_r8, .w8 = m_w8 };   /* no peek */

static bool m_part(const MMap *x, int dir) {
    if (!x->used || !x->enabled)
        return false;
    if (x->ram)
        return dir == 0 ? !(x->flags & GEMU_MAP_WRITEONLY) : !(x->flags & GEMU_MAP_READONLY);
    return dir == 0 ? x->h.r8 != NULL : x->h.w8 != NULL;
}

/* The model: which mappings serve addr in dir, oldest first. */
static int model_set(uint32_t addr, int dir, int *out, uint32_t *eff_out) {
    uint32_t eff = addr;
    for (int i = nmirrors - 1; i >= 0; i--)
        if (mirrors[i].lo <= addr && addr <= mirrors[i].hi) {
            eff = addr & mirrors[i].mask;
            break;
        }
    *eff_out = eff;
    int top = -1000;
    for (int i = 0; i < MAXMAP; i++)
        if (m_part(&mm[i], dir) && mm[i].base <= eff && eff - mm[i].base < mm[i].size && mm[i].prio > top)
            top = mm[i].prio;
    int n = 0, newest = -1;
    for (int i = 0; i < MAXMAP; i++) {
        if (!m_part(&mm[i], dir) || mm[i].prio != top || mm[i].base > eff || eff - mm[i].base >= mm[i].size)
            continue;
        if (mm[i].flags & GEMU_MAP_SHARED)
            out[n++] = i;
        else if (newest < 0 || mm[i].seq > mm[newest].seq)
            newest = i;
    }
    if (newest >= 0)
        out[n++] = newest;
    /* oldest first */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && mm[out[j]].seq < mm[out[j - 1]].seq; j--) {
            int t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return n;
}

static uint8_t model_read(uint32_t addr, bool *mapped) {
    int set[MAXMAP];
    uint32_t eff;
    int n = model_set(addr, 0, set, &eff);
    *mapped = n > 0;
    uint8_t v = 0xFF;
    for (int i = 0; i < n; i++) {
        MMap *x = &mm[set[i]];
        v &= x->ram ? x->mem[eff - x->base] : (uint8_t)(x->dev.id * 7 + (eff - x->base) * 13);
    }
    return n ? v : 0xEE;
}

static void random_map(GemuSpace *s, int i) {
    MMap *x = &mm[i];
    memset(x, 0, sizeof *x);
    x->used = true;
    x->enabled = true;
    uint32_t sizes[] = { 1, 2, 3, 0x10, 0x80, 0x100, 0x1FF, 0x800, 0x1000, 0x4000 };
    x->size = sizes[rnd() % 10];
    x->base = rnd() % (NADDR - x->size + 1);
    if (rnd() % 3 == 0)
        x->base &= ~0xFFu;                              /* page-aligned often */
    x->prio = (int)(rnd() % 3);
    x->dev.id = (uint8_t)(i + 1);
    x->seq = ++mseq;
    if (rnd() % 2) {
        x->ram = true;
        x->mem = malloc(x->size);
        for (uint32_t k = 0; k < x->size; k++)
            x->mem[k] = (uint8_t)rnd();
        unsigned f[] = { 0, 0, GEMU_MAP_READONLY, GEMU_MAP_WRITEONLY };
        x->flags = f[rnd() % 4];
        x->mp = gemu_space_map_ram(s, x->base, x->size, x->mem, x->flags, x->prio);
    } else {
        const GemuHandlers *hs[] = { &MH_RW, &MH_RW, &MH_R, &MH_W, &MH_RNP };
        x->h = *hs[rnd() % 5];
        x->flags = rnd() % 3 == 0 ? GEMU_MAP_SHARED : 0;
        x->mp = gemu_space_map(s, x->base, x->size, &x->h, &x->dev, x->prio, x->flags);
    }
    CHECK(x->mp);
}

static void drop_map(int i) {
    free(mm[i].mem);
    memset(&mm[i], 0, sizeof mm[i]);
}

static const char *last_op = "start";
static void dump_model(void) {
    fprintf(stderr, "after %s\n", last_op);
    for (int i = 0; i < MAXMAP; i++)
        if (mm[i].used)
            fprintf(stderr, "  map %2d seq %2" PRIu64 " %04X+%04X prio %d %s flags %u%s%s\n", i, mm[i].seq,
                    mm[i].base, mm[i].size, mm[i].prio, mm[i].ram ? "ram" : "hdl", mm[i].flags,
                    mm[i].enabled ? "" : " disabled",
                    mm[i].ram ? "" : mm[i].h.r8 ? (mm[i].h.w8 ? " rw" : " r") : " w");
    for (int i = 0; i < nmirrors; i++)
        fprintf(stderr, "  mirror %04X-%04X mask %04X\n", mirrors[i].lo, mirrors[i].hi, mirrors[i].mask);
}

static void compare_all(GemuSpace *s) {
    for (uint32_t a = 0; a < NADDR; a++) {
        bool mapped;
        uint8_t want = model_read(a, &mapped);
        uint8_t got = gemu_space_read8(s, a);
        if (got != want) {
            fprintf(stderr, "read %04X: got %02X want %02X\n", a, got, want);
            dump_model();
            CHECK(got == want);
        }
        /* peek: RAM, and handlers with a peek8 hook */
        int set[MAXMAP];
        uint32_t eff;
        int n = model_set(a, 0, set, &eff);
        bool peekable = n > 0;
        for (int i = 0; i < n; i++)
            if (!mm[set[i]].ram && !mm[set[i]].h.peek8)
                peekable = false;
        uint8_t pv;
        bool ok = gemu_space_peek8(s, a, &pv);
        CHECK(ok == peekable && (!ok || pv == want));
    }
    /* wide reads: the bytes joined little-endian (r8-only handlers and RAM) */
    for (int k = 0; k < 2000; k++) {
        uint32_t a = rnd() % NADDR;
        unsigned n = 1u << (rnd() % 4);
        uint64_t want = 0;
        bool mapped;
        for (unsigned j = 0; j < n; j++)
            want |= (uint64_t)model_read((a + j) & (NADDR - 1), &mapped) << (8 * j);
        uint64_t got = n == 1 ? gemu_space_read8(s, a) : n == 2 ? gemu_space_read16(s, a)
                     : n == 4 ? gemu_space_read32(s, a) : gemu_space_read64(s, a);
        if (got != want)
            fprintf(stderr, "read%u %04X: got %" PRIx64 " want %" PRIx64 "\n", 8 * n, a, got, want);
        CHECK(got == want);
    }
    /* writes: who gets them */
    for (int k = 0; k < 500; k++) {
        uint32_t a = rnd() % NADDR;
        uint8_t v = (uint8_t)rnd();
        int set[MAXMAP];
        uint32_t eff;
        int n = model_set(a, 1, set, &eff);
        unsigned before[MAXMAP];
        for (int i = 0; i < MAXMAP; i++)
            before[i] = mm[i].dev.writes;
        gemu_space_write8(s, a, v);
        for (int i = 0; i < n; i++) {
            MMap *x = &mm[set[i]];
            if (x->ram)
                CHECK(x->mem[eff - x->base] == v);
            else
                CHECK(x->dev.writes == before[set[i]] + 1 && x->dev.last_off == eff - x->base &&
                      x->dev.last_v == v);
        }
        int nw = 0;
        for (int i = 0; i < MAXMAP; i++)
            nw += mm[i].dev.writes - before[i];
        int nh = 0;
        for (int i = 0; i < n; i++)
            nh += !mm[set[i]].ram;
        CHECK(nw == nh);                                 /* nobody else */
    }
}

static void random_cases(uint32_t seed) {
    rnd_state = seed ? seed : 1;
    for (int round = 0; round < 6; round++) {
        GemuSpace *s = gemu_space_new(NULL, "r", SPACE_BITS, 8, GEMU_LITTLE_ENDIAN);
        gemu_space_set_unmapped(s, 0xEEEEEEEEEEEEEEEEull, NULL, NULL, NULL);
        memset(mm, 0, sizeof mm);
        nmirrors = 0;
        if (round % 3 == 1) {
            /* a mirror or two (they are permanent: set them first or later) */
            uint32_t masks[] = { 0x7FF, 0x3FF, 0xFF0F, 0x1FFF };
            uint32_t lo = (rnd() % NADDR) & ~0x3FFu, len = 0x400u << (rnd() % 5);
            if (lo + len > NADDR) len = NADDR - lo;
            mirrors[nmirrors++] = (MMirror){ lo, lo + len - 1, masks[rnd() % 4] };
            CHECK(gemu_space_mirror(s, lo, len, mirrors[0].mask));
        }
        for (int i = 0; i < MAXMAP / 2; i++)
            random_map(s, i);
        compare_all(s);
        for (int step = 0; step < 15; step++) {
            int i = (int)(rnd() % MAXMAP);
            MMap *x = &mm[i];
            switch (rnd() % 6) {
            case 0:                                      /* add or replace */ last_op = "add";
                if (x->used) { gemu_mapping_remove(x->mp); drop_map(i); }
                random_map(s, i);
                break;
            case 1:                                      /* remove */ last_op = "remove";
                if (x->used) { gemu_mapping_remove(x->mp); drop_map(i); }
                break;
            case 2:                                      /* enable/disable */ last_op = "enable";
                if (x->used) { x->enabled = !x->enabled; gemu_mapping_enable(x->mp, x->enabled); }
                break;
            case 3:                                      /* move */ last_op = "move";
                if (x->used) {
                    x->base = rnd() % (NADDR - x->size + 1);
                    CHECK(gemu_mapping_move(x->mp, x->base, x->size));
                }
                break;
            case 4:                                      /* bank switch */ last_op = "setptr";
                if (x->used && x->ram) {
                    uint8_t *nm = malloc(x->size);
                    for (uint32_t k = 0; k < x->size; k++)
                        nm[k] = (uint8_t)rnd();
                    gemu_mapping_set_ptr(x->mp, nm);
                    free(x->mem);
                    x->mem = nm;
                }
                break;
            case 5:                                      /* a mirror */ last_op = "mirror";
                if (nmirrors < 4 && rnd() % 2) {
                    uint32_t lo = (rnd() % NADDR) & ~0xFFu, len = 0x100u << (rnd() % 8);
                    if (lo + len > NADDR) len = NADDR - lo;
                    uint32_t mask = (rnd() % 2) ? (0xFFu << (rnd() % 6)) | 0xFF : rnd() & 0xFFFF;
                    mirrors[nmirrors++] = (MMirror){ lo, lo + len - 1, mask };
                    CHECK(gemu_space_mirror(s, lo, len, mask));
                }
                break;
            }
            compare_all(s);
        }
        for (int i = 0; i < MAXMAP; i++)
            drop_map(i);
        gemu_space_free(s);
    }
}

int main(int argc, char **argv) {
    fixed_cases();
    uint32_t seed = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 2026;
    random_cases(seed);
    printf("space: %u checks passed (seed %" PRIu32 ")\n", checks, seed);
    return 0;
}
