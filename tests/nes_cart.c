/* The NES cartridge slot and some of its boards.  Run: ninja check-nes_cart */
#include "cart/nes_cart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static Rp2c02 ppu;
static unsigned irq_calls;
static void on_irq(void *ud) { (void)ud; irq_calls++; }

/* An iNES image: every 8 KB PRG bank filled with its number, every 1 KB
 * CHR bank with its number.  The caller frees it. */
static uint8_t *image(unsigned mapper, unsigned prg16, unsigned chr8, uint8_t flags6, size_t *len) {
    *len = 16 + prg16 * 0x4000u + chr8 * 0x2000u;
    uint8_t *d = calloc(1, *len);
    memcpy(d, "NES\x1a", 4);
    d[4] = (uint8_t)prg16; d[5] = (uint8_t)chr8;
    d[6] = (uint8_t)(((mapper & 0x0F) << 4) | flags6);
    d[7] = (uint8_t)(mapper & 0xF0);
    for (unsigned i = 0; i < prg16 * 2; i++) memset(d + 16 + i * 0x2000u, (int)i, 0x2000);
    for (unsigned i = 0; i < chr8 * 8; i++) memset(d + 16 + prg16 * 0x4000u + i * 0x400u, (int)i, 0x400);
    return d;
}

static NesCart *insert(unsigned mapper, unsigned prg16, unsigned chr8, uint8_t flags6) {
    NesCart *c = calloc(1, sizeof(*c));
    c->ppu = &ppu;
    c->irq_changed = on_irq;
    size_t len;
    uint8_t *d = image(mapper, prg16, chr8, flags6, &len);
    CHECK(nes_cart_load_mem(c, d, len, "test"));
    free(d);
    ppu.mirror = c->info.mirror;
    nes_cart_reset(c);
    return c;
}

static void eject(NesCart *c) { nes_cart_free(c); free(c); }

static void test_load(void) {
    NesCart c = {0};
    size_t len;
    uint8_t *d = image(0, 1, 1, 0x01, &len);
    d[0] = 'X';
    CHECK(!nes_cart_load_mem(&c, d, len, "bad magic"));
    d[0] = 'N';
    d[6] = 0xF0; d[7] = 0xF0;                       /* mapper 255 */
    CHECK(!nes_cart_load_mem(&c, d, len, "mapper 255"));
    d[6] = 0x01; d[7] = 0;
    CHECK(!nes_cart_load_mem(&c, d, len - 1, "short"));
    CHECK(c.prg == NULL);
    free(d);

    /* NROM-128: the 16 KB is at both $8000 and $C000; vertical pads */
    NesCart *n = insert(0, 1, 1, 0x01);
    CHECK(n->info.mirror == RP2C02_MIRROR_VERTICAL);
    CHECK(nes_cart_prg(n, 0x8000) == 0 && nes_cart_prg(n, 0xA000) == 1);
    CHECK(nes_cart_prg(n, 0xC000) == 0 && nes_cart_prg(n, 0xE000) == 1);
    CHECK(nes_cart_chr_read(0x1C00, n) == 7);
    nes_cart_chr_write(0x0000, 0x55, n);           /* CHR ROM: ignored */
    CHECK(nes_cart_chr_read(0x0000, n) == 0);
    CHECK(nes_cart_read(n, 0x6000, 0xAB) == 0xAB); /* no PRG-RAM: open bus */
    uint8_t v;
    CHECK(!nes_cart_peek(n, 0x6000, &v) && nes_cart_peek(n, 0xFFFC, &v) && v == 1);
    eject(n);

    /* No CHR ROM: 8 KB of CHR RAM */
    n = insert(2, 8, 0, 0);
    CHECK(n->chr_is_ram && n->info.chr_banks == 1);
    nes_cart_chr_write(0x1234, 0x5A, n);
    CHECK(nes_cart_chr_read(0x1234, n) == 0x5A);
    size_t ram_len;
    CHECK(nes_cart_chr_ram(n, &ram_len) == n->chr && ram_len == 0x2000);
    eject(n);
}

static void test_uxrom_cnrom(void) {
    NesCart *c = insert(2, 8, 0, 0);
    CHECK(nes_cart_prg(c, 0xC000) == 14 && nes_cart_prg(c, 0xE000) == 15);   /* last 16 KB fixed */
    nes_cart_write(c, 0x8000, 3);
    CHECK(nes_cart_prg(c, 0x8000) == 6 && nes_cart_prg(c, 0xA000) == 7);
    CHECK(nes_cart_prg(c, 0xC000) == 14);
    eject(c);

    /* CNROM: bus conflicts AND the value with the ROM byte (bank 1's
     * 8 KB PRG is 1s at $A000) */
    c = insert(3, 2, 4, 0);
    nes_cart_write(c, 0xA000, 3);                  /* 3 & 1 = 1 */
    CHECK(nes_cart_chr_read(0x0000, c) == 8);      /* 8 KB bank 1 = 1 KB bank 8 */
    nes_cart_write(c, 0x8000, 3);                  /* 3 & 0 = 0 */
    CHECK(nes_cart_chr_read(0x0000, c) == 0);
    eject(c);
}

static void mmc1_load(NesCart *c, uint16_t addr, uint8_t val) {
    for (int i = 0; i < 5; i++) nes_cart_write(c, addr, (uint8_t)((val >> i) & 1u));
}

static void test_mmc1(void) {
    NesCart *c = insert(1, 8, 4, 0);
    /* power-on: PRG mode 3, last 16 KB at $C000 */
    CHECK(nes_cart_prg(c, 0xC000) == 14 && nes_cart_prg(c, 0xE000) == 15);
    mmc1_load(c, 0xE000, 0x02);
    CHECK(nes_cart_prg(c, 0x8000) == 4);
    /* four writes don't load the register; a write with bit 7 resets the
     * shift register */
    for (int i = 0; i < 4; i++) nes_cart_write(c, 0xE000, 1);
    CHECK(nes_cart_prg(c, 0x8000) == 4);
    nes_cart_write(c, 0x8000, 0x80);
    mmc1_load(c, 0xE000, 0x03);
    CHECK(nes_cart_prg(c, 0x8000) == 6);
    /* control: vertical mirroring, 4 KB CHR; CHR bank 1 (4 KB) at $0000 */
    mmc1_load(c, 0x8000, 0x1E);
    CHECK(ppu.mirror == RP2C02_MIRROR_VERTICAL);
    mmc1_load(c, 0xA000, 0x01);
    CHECK(nes_cart_chr_read(0x0000, c) == 4);
    /* PRG-RAM, and bit 4 of the PRG register disabling it */
    nes_cart_write(c, 0x6000, 0x42);
    CHECK(nes_cart_read(c, 0x6000, 0) == 0x42);
    mmc1_load(c, 0xE000, 0x13);
    CHECK(nes_cart_read(c, 0x6000, 0xEE) == 0xEE);
    eject(c);
}

static void test_mmc3(void) {
    NesCart *c = insert(4, 8, 8, 0);
    CHECK(nes_cart_prg(c, 0xC000) == 14 && nes_cart_prg(c, 0xE000) == 15);
    nes_cart_write(c, 0x8000, 6);                  /* R6 */
    nes_cart_write(c, 0x8001, 5);
    CHECK(nes_cart_prg(c, 0x8000) == 5);
    nes_cart_write(c, 0x8000, 0x46);               /* PRG mode 1: R6 at $C000 */
    /* (the banks follow on the next $8001 write, not at once as on the chip) */
    nes_cart_write(c, 0x8001, 5);
    CHECK(nes_cart_prg(c, 0x8000) == 14 && nes_cart_prg(c, 0xC000) == 5);
    nes_cart_write(c, 0x8000, 2);                  /* R2: 1 KB at $1000 */
    nes_cart_write(c, 0x8001, 9);
    CHECK(nes_cart_chr_read(0x1000, c) == 9);
    nes_cart_write(c, 0xA000, 1);
    CHECK(ppu.mirror == RP2C02_MIRROR_HORIZONTAL);

    /* IRQ: latch 2, reload, enable; the counter reaches 0 on the third clock */
    CHECK(nes_cart_wants_scanline(c));
    nes_cart_write(c, 0xC000, 2);
    nes_cart_write(c, 0xC001, 0);
    nes_cart_write(c, 0xE001, 0);
    irq_calls = 0;
    nes_cart_scanline(c);
    nes_cart_scanline(c);
    CHECK(!c->irq);
    nes_cart_scanline(c);
    CHECK(c->irq && irq_calls == 1);
    nes_cart_write(c, 0xE000, 0);                  /* acknowledge and disable */
    CHECK(!c->irq && irq_calls == 2);
    eject(c);

    /* TQROM: bank values with bit 6 map the 8 KB of CHR RAM */
    c = insert(119, 8, 8, 0);
    nes_cart_write(c, 0x8000, 2);
    nes_cart_write(c, 0x8001, 0x41);
    nes_cart_chr_write(0x1000, 0x77, c);
    CHECK(nes_cart_chr_read(0x1000, c) == 0x77);
    nes_cart_chr_write(0x1400, 0x77, c);           /* R3 is ROM */
    CHECK(nes_cart_chr_read(0x1400, c) == 0);
    size_t ram_len;
    uint8_t *ram = nes_cart_chr_ram(c, &ram_len);
    CHECK(ram_len == 0x2000 && ram[0x400] == 0x77);
    eject(c);
}

static void test_vrc6(void) {
    NesCart *c = insert(24, 8, 8, 0);
    CHECK(nes_cart_ticks(c));
    nes_cart_write(c, 0xF000, 0xFD);               /* latch */
    nes_cart_write(c, 0xF001, 0x06);               /* enable, CPU-cycle mode */
    irq_calls = 0;
    for (int i = 0; i < 2; i++) nes_cart_tick(c);
    CHECK(!c->irq);
    nes_cart_tick(c);                              /* $FD -> $FE -> $FF -> fire */
    CHECK(c->irq);
    nes_cart_write(c, 0xF002, 0);                  /* acknowledge */
    CHECK(!c->irq);
    eject(c);

    /* VRC6b: A0 and A1 swapped, so $F002 is the control register */
    c = insert(26, 8, 8, 0);
    nes_cart_write(c, 0xF000, 0xFF);
    nes_cart_write(c, 0xF002, 0x06);
    nes_cart_tick(c);
    CHECK(c->irq);
    eject(c);
}

static void test_vs_and_snapshot(void) {
    NesCart c = { .ppu = &ppu };
    uint8_t *prg = calloc(1, 0x8000), *chr = calloc(1, 0x4000);
    memset(chr + 0x2000, 0x22, 0x2000);
    CHECK(nes_cart_init_vs(&c, prg, chr));
    CHECK(nes_cart_chr_read(0x0000, &c) == 0);
    nes_cart_out(&c, 0x04);                        /* OUT2: the second bank */
    CHECK(nes_cart_chr_read(0x0000, &c) == 0x22 && nes_cart_chr_read(0x1FFF, &c) == 0x22);
    nes_cart_write(&c, 0x6000, 0x99);
    CHECK(nes_cart_read(&c, 0x6000, 0) == 0x99);

    /* A raw snapshot: the copy loses the pointers, rewire brings them back */
    NesCart snap = c;
    nes_cart_strip(&snap);
    CHECK(snap.prg == NULL && snap.ops == NULL && snap.ppu == NULL);
    CHECK(snap.prg_ram[0] == 0x99 && snap.chr_off[0] == 0x2000);
    nes_cart_rewire(&snap, &c);
    CHECK(snap.prg == prg && snap.ops == c.ops && nes_cart_chr_read(0x0000, &snap) == 0x22);
    nes_cart_free(&c);
    CHECK(c.prg == NULL && nes_cart_chr_read(0x0000, &c) == 0);
}

int main(void) {
    test_load();
    test_uxrom_cnrom();
    test_mmc1();
    test_mmc3();
    test_vrc6();
    test_vs_and_snapshot();
    printf("nes_cart: %u checks passed\n", checks);
    return 0;
}
