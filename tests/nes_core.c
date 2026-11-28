/* The NES console's core machine (stage N): its devices' state.
 * Run: ninja check-nes_core */
#include "../hardware/machine/mos/machine_nes.c"
#include <assert.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

/* An iNES file: PRG of NOPs ending in a loop at the reset vector's target
 * that writes the PPU and the MMC3's registers. */
static const char *rom_file(const char *name, unsigned mapper, unsigned prg16, unsigned chr8) {
    size_t prg = prg16 * 16384u, chr = chr8 * 8192u, len = 16 + prg + chr;
    uint8_t *d = calloc(1, len);
    memcpy(d, "NES\x1a", 4);
    d[4] = (uint8_t)prg16; d[5] = (uint8_t)chr8;
    d[6] = (uint8_t)((mapper & 15) << 4); d[7] = (uint8_t)(mapper & 0xF0);
    uint8_t *p = d + 16;
    memset(p, 0xEA, prg);
    static const uint8_t loop[] = {
        0xA9, 0x80, 0x8D, 0x00, 0x20,     /* LDA #$80; STA $2000 (NMI on) */
        0xA9, 0x06, 0x8D, 0x00, 0x80,     /* LDA #6; STA $8000 (MMC3: R6) */
        0xEE, 0x00, 0x03,                 /* INC $0300 */
        0xAD, 0x00, 0x03, 0x8D, 0x01, 0x80, /* LDA $0300; STA $8001 */
        0x4C, 0x00, 0xC0,                 /* JMP $C000 */
    };
    size_t last = prg - 16384;            /* the last 16 KB, at $C000 */
    memcpy(p + last, loop, sizeof loop);
    p[prg - 4] = 0x00; p[prg - 3] = 0xC0; /* reset vector: $C000 */
    p[prg - 6] = 0x13; p[prg - 5] = 0xC0; /* NMI: the JMP (keeps looping) */
    for (size_t i = 0; i < chr; i++) d[16 + prg + i] = (uint8_t)(i * 7);
    FILE *f = fopen(name, "wb");
    CHECK(f && fwrite(d, 1, len, f) == len);
    fclose(f);
    free(d);
    return name;
}

/* An NROM that plays pulse 1 with a period that keeps moving. */
static const char *tone_file(const char *name) {
    size_t prg = 16384, chr = 8192, len = 16 + prg + chr;
    uint8_t *d = calloc(1, len);
    memcpy(d, "NES\x1a", 4);
    d[4] = 1; d[5] = 1;
    uint8_t *p = d + 16;
    memset(p, 0xEA, prg);
    static const uint8_t code[] = {
        0xA9, 0x01, 0x8D, 0x15, 0x40,     /* LDA #1; STA $4015 (pulse 1 on) */
        0xA9, 0xBF, 0x8D, 0x00, 0x40,     /* LDA #$BF; STA $4000 (duty, constant volume 15) */
        0xA9, 0x00, 0x8D, 0x01, 0x40,     /* LDA #0; STA $4001 (sweep off) */
        0xA9, 0x01, 0x8D, 0x03, 0x40,     /* LDA #1; STA $4003 (period high, length) */
        0xEE, 0x00, 0x03,                 /* loop ($C014): INC $0300 */
        0xAD, 0x00, 0x03, 0x8D, 0x02, 0x40, /* LDA $0300; STA $4002 (period low) */
        0xA2, 0x00, 0xCA, 0xD0, 0xFD,     /* LDX #0; DEX; BNE (a pause) */
        0x4C, 0x14, 0xC0,                 /* JMP loop */
    };
    memcpy(p, code, sizeof code);
    p[prg - 4] = 0x00; p[prg - 3] = 0xC0;
    p[prg - 6] = 0x00; p[prg - 5] = 0xC0;
    FILE *f = fopen(name, "wb");
    CHECK(f && fwrite(d, 1, len, f) == len);
    fclose(f);
    free(d);
    return name;
}

static float *cap;
static size_t cap_n, cap_cap;
static void capture(void *ud, const float *samples, size_t frames) {
    (void)ud;
    if (cap_n + frames > cap_cap) {
        cap_cap = (cap_n + frames) * 2;
        cap = realloc(cap, cap_cap * sizeof *cap);
    }
    memcpy(cap + cap_n, samples, frames * sizeof *cap);
    cap_n += frames;
}

static MosConfig cfg_for(const char *rom) {
    MosConfig c;
    memset(&c, 0, sizeof c);
    c.machine = MOS_MACHINE_NES;
    c.cpu = MOS_CPU_2A03;
    c.display_type = GEMU_DISPLAY_NONE;
    c.cart_path = rom;
    c.ports[0] = NES_DEVICE_CONTROLLER;
    return c;
}

static bool same_bytes(const uint8_t *a, size_t na, const uint8_t *b, size_t nb) {
    return na == nb && !memcmp(a, b, na);
}

int main(void) {
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    const char *mmc3 = rom_file("core-mmc3.nes", 4, 8, 8);
    const char *nrom = rom_file("core-nrom.nes", 0, 2, 1);
    MosConfig c1 = cfg_for(mmc3), c2 = cfg_for(mmc3), c3 = cfg_for(nrom);

    NesState *s = nes_core_create(&c1);
    CHECK(s && s->core && s->core->machine);
    nes_reset(s);
    for (int i = 0; i < 200000; i++) nes_step_insn(s);
    nes_sync(s);
    CHECK(s->ram[0x300] != 0 && s->ppu.frame > 0);

    /* save, load, save: the same bytes */
    uint8_t *a, *b;
    size_t na, nb;
    CHECK(gemu_machine_save_state(s->core->machine, &a, &na));
    uint8_t ram300 = s->ram[0x300];
    uint64_t cyc = s->cpu.cycle_count;
    for (int i = 0; i < 50000; i++) nes_step_insn(s);
    CHECK(s->cpu.cycle_count != cyc);
    CHECK(gemu_machine_load_state(s->core->machine, a, na));
    CHECK(s->ram[0x300] == ram300 && s->cpu.cycle_count == cyc);
    CHECK(gemu_machine_save_state(s->core->machine, &b, &nb));
    CHECK(same_bytes(a, na, b, nb));
    free(b);

    /* into a second console with the same cartridge: the same bytes again,
     * and it runs on exactly as the first */
    NesState *t = nes_core_create(&c2);
    CHECK(t);
    nes_reset(t);
    CHECK(gemu_machine_load_state(t->core->machine, a, na));
    CHECK(gemu_machine_save_state(t->core->machine, &b, &nb));
    CHECK(same_bytes(a, na, b, nb));
    free(b);
    for (int i = 0; i < 30000; i++) { nes_step_insn(s); nes_step_insn(t); }
    nes_sync(s); nes_sync(t);
    CHECK(s->cpu.PC == t->cpu.PC && s->cpu.cycle_count == t->cpu.cycle_count);
    CHECK(!memcmp(s->ram, t->ram, sizeof s->ram));
    CHECK(!memcmp(s->ppu.pixels, t->ppu.pixels, sizeof s->ppu.pixels));
    CHECK(!memcmp(s->cart.prg_off, t->cart.prg_off, sizeof s->cart.prg_off));

    /* another cartridge: refused, untouched */
    NesState *u = nes_core_create(&c3);
    CHECK(u);
    nes_reset(u);
    u->ram[5] = 0x5a;
    CHECK(!gemu_machine_load_state(u->core->machine, a, na));
    CHECK(strstr(gemu_machine_error(u->core->machine), "mapper"));
    CHECK(u->ram[5] == 0x5a);

    /* N6: the front end's save states are the core state */
    uint8_t *r;
    size_t nr;
    CHECK(nes_snapshot_save(s, &r, &nr));
    CHECK(nr == na || nr > 0);
    uint64_t pc = s->cpu.PC;
    uint32_t px = s->ppu.pixels_argb[1000];
    for (int i = 0; i < 1000; i++) nes_step_insn(s);
    memset(s->ppu.pixels_argb, 0, sizeof s->ppu.pixels_argb);
    CHECK(nes_snapshot_load(s, r, nr));
    CHECK(s->cpu.PC == pc && s->core && s->core->machine);
    CHECK(s->ppu.pixels_argb[1000] == px);           /* the frame worked out again */
    /* into another console */
    CHECK(nes_snapshot_load(t, r, nr) && t->cpu.PC == pc);
    free(r);
    /* the format before N6: refused */
    uint8_t old[64] = { 0 };
    uint64_t old_magic = NES_OLD_SNAPSHOT_MAGIC;
    memcpy(old, &old_magic, 8);
    old[8] = 6;
    uint64_t pc2 = s->cpu.PC;
    CHECK(!nes_snapshot_load(s, old, sizeof old));
    CHECK(s->cpu.PC == pc2);

    /* N3: frames through the scheduler (the executor) are exactly the
     * instructions a console stepping by hand runs */
    NesState *f = nes_core_create(&c1), *g = nes_core_create(&c2);
    CHECK(f && g);
    nes_reset(f);
    nes_reset(g);
    uint64_t f0 = f->ppu.frame;
    uint64_t sf0 = gemu_screen_frame_number(nes_screen(f));
    GemuTime t0 = gemu_machine_time(f->core->machine);
    uint64_t c0 = f->cpu.cycle_count;
    for (int i = 0; i < 100; i++) nes_run_frame(f);
    CHECK(f->ppu.frame == f0 + 100);
    /* N4: the PPU's screen began each of those frames, and shows its picture */
    CHECK(gemu_screen_frame_number(nes_screen(f)) == sf0 + 100);
    const GemuBitmap *bmp = gemu_screen_render(nes_screen(f));
    CHECK(bmp->width == RP2C02_WIDTH && bmp->height == RP2C02_HEIGHT);
    bool same = true;
    for (int y = 0; y < RP2C02_HEIGHT; y++)
        same &= !memcmp(bmp->pix + y * bmp->stride, f->ppu.pixels_argb + y * RP2C02_WIDTH,
                        RP2C02_WIDTH * sizeof(uint32_t));
    CHECK(same);
    int vpos = gemu_screen_vpos(nes_screen(f));
    CHECK(vpos >= 0 && vpos < f->ppu.lines_total);
    while (g->cpu.insn_count < f->cpu.insn_count) nes_step_insn(g);
    nes_sync(g);
    CHECK(g->cpu.cycle_count == f->cpu.cycle_count && g->cpu.PC == f->cpu.PC);
    CHECK(!memcmp(f->ram, g->ram, sizeof f->ram));
    CHECK(!memcmp(f->ppu.pixels, g->ppu.pixels, sizeof f->ppu.pixels));
    /* the machine's time is the CPU's cycles at its clock (to within the
     * picosecond rounding of each run's end) */
    uint64_t ran = f->cpu.cycle_count - c0;
    GemuTime took = gemu_machine_time(f->core->machine) - t0;
    GemuTime want = gemu_clocks(f->pins.clock_hz, ran);
    CHECK(took + 1000 >= want && took <= want + 1000);
    /* the pin window's clock re-clocks the executor: half the clock, a
     * frame's cycles take twice the time */
    GemuTime t1 = gemu_machine_time(f->core->machine);
    uint64_t cy1 = f->cpu.cycle_count;
    f->pins.clock_hz /= 2;
    nes_apply_pins(f);
    nes_run_frame(f);
    GemuTime slow = gemu_machine_time(f->core->machine) - t1;
    GemuTime at_half = gemu_clocks(f->pins.clock_hz, f->cpu.cycle_count - cy1);
    /* (within one cycle: the new clock starts from the machine's time,
     * which a run that ended on its target can have just passed) */
    GemuTime cycle = gemu_clocks(f->pins.clock_hz, 1);
    CHECK(slow + cycle + 1000 >= at_half && slow <= at_half + cycle + 1000);
    nes_core_destroy(f);
    nes_core_destroy(g);

    /* N5: the APU's samples reach the mixer's output unchanged and in order:
     * one console runs frames with the mixer's output captured, another is
     * stepped by hand and its APU's samples taken straight from its buffer
     * (what the console queued to the host before N5) */
    const char *tone = tone_file("core-tone.nes");
    MosConfig ct = cfg_for(tone), cr = cfg_for(tone);
    ct.sound = cr.sound = MOS_SOUND_2A03;     /* the APU at its clock (a dummy host device) */
    NesState *A = nes_core_create(&ct), *B = nes_core_create(&cr);
    CHECK(A && B);
    nes_reset(A);
    nes_reset(B);
    gemu_mixer_set_output(A->core->machine, APU2A03_SAMPLE_RATE, 1, capture, NULL);
    for (int i = 0; i < 120; i++) nes_run_frame(A);
    size_t raw_cap = 1u << 20, raw_n = 0;
    float *raw = malloc(raw_cap * sizeof *raw);
    while (B->cpu.insn_count < A->cpu.insn_count) {
        nes_step_insn(B);
        nes_sync(B);
        size_t n = (size_t)B->apu.frame_n;
        if (raw_n + n > raw_cap) n = raw_cap - raw_n;
        memcpy(raw + raw_n, B->apu.frame_buf, n * sizeof *raw);
        raw_n += n;
        B->apu.frame_n = 0;
    }
    CHECK(raw_n < raw_cap);
    CHECK(cap_n > 100 * 700 && raw_n >= cap_n);
    /* the mixer's first run syncs to the machine's time and drops what came
     * before it: the capture is the raw samples from where it began */
    size_t skip = raw_n - cap_n, diff = 0, nonzero = 0;
    for (size_t i = 0; i < cap_n; i++) {
        diff += cap[i] != raw[skip + i];
        nonzero += cap[i] != 0.0f;
    }
    CHECK(diff == 0 && nonzero > cap_n / 4);
    free(raw);
    free(cap);
    nes_core_destroy(A);
    nes_core_destroy(B);

    free(a);
    nes_core_destroy(s);
    nes_core_destroy(t);
    nes_core_destroy(u);
    printf("nes_core: %u checks passed\n", checks);
    return 0;
}
