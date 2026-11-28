/* The Lagonda's instrument computer on the core (stage Z3), on its real
 * EPROMs (roms/lagonda, or LAGONDA_ROMS=DIR; skipped when they're not
 * here).  The clock is set to a fixed time; at frame 60 the car is driven
 * (120 mph, 3,000 rpm, drive) and the oil pressure warning comes on (the
 * speech card says it).  Built with -DZ_HEAD against HEAD's driver it runs
 * HEAD's frame.  Run: ninja check-lagonda_core */
#include "../hardware/machine/z80/machine_lagonda.c"
#include "z80_harness.h"

#ifdef Z_HEAD
#define CPU(s)    (&(s)->cpu)
#define VDP(s, i) (&(s)->vdp[i])
#define RTC(s)    (&(s)->rtc)
#define MSM(s)    (&(s)->speech.msm)
#define INSNS(s)  ((s)->insn_count)
static void frame(LagondaState *s) {     /* HEAD's lagonda_run, one frame */
    handle_input(s, 1.0 / 59.94);
    while (!lag_step(s)) {}
}
#else
#define CPU(s)    ((s)->cpu)
#define VDP(s, i) ((s)->vdp[i])
#define RTC(s)    ((s)->rtc)
#define MSM(s)    ((s)->msm)
#define INSNS(s)  lag_insns(s)
static void frame(LagondaState *s) { lagonda_frame(s); }
void lagonda_core_checks(Z80Config *c);
#endif

#define OUT_FILE "lagonda-out.txt"

static void report(LagondaState *s, int frames) {
    fprintf(rep, "%4d", frames);
    hash("ram", s->ram, sizeof s->ram);
    for (int i = 0; i < LAG_VDP_N; i++) {
        char l[8];
        snprintf(l, sizeof l, "v%d", i);
        hash(l, VDP(s, i)->vram, sizeof VDP(s, i)->vram);
        snprintf(l, sizeof l, "fb%d", i);
        hash(l, VDP(s, i)->fb, sizeof VDP(s, i)->fb);
    }
    fprintf(rep, " sp %02X %04X %d msm %d %d trip %04X", s->speech.latch, s->speech.counter,
            s->speech.playing, MSM(s)->signal, MSM(s)->step, s->trip_bcd);
    z80_report(CPU(s), INSNS(s));
}

static const struct { const char *file, *region; } roms[] = {
    { "lagonda_code_2_center_of_board.bin", "code" },
    { "lagonda_code_1_edge_of_board.bin", "graphics" },
    { "lagonda_sound_1.bin", "speech1" }, { "lagonda_sound_2.bin", "speech2" },
    { "lagonda_sound_3.bin", "speech3" }, { "lagonda_sound_4.bin", "speech4" },
    { "lagonda_sound_5.bin", "speech5" },
};

static char paths[7][512];

static bool lagonda_cfg(Z80Config *c) {
    /* LAGONDA_ROMS=DIR, or roms/ from the run directory (ninja check runs
     * each test in build/tests/run/NAME-XXXX) */
    const char *dir = getenv("LAGONDA_ROMS");
    if (!dir) {
        FILE *f = fopen("../../../../roms/lagonda/lagonda_code_1_edge_of_board.bin", "rb");
        dir = f ? "../../../../roms/lagonda" : "../../../roms/lagonda";
        if (f) fclose(f);
    }
    memset(c, 0, sizeof *c);
    c->machine = Z80_MACHINE_LAGONDA;
    c->clock_hz = 4000000u;
    c->display_type = GEMU_DISPLAY_NONE;
    for (int i = 0; i < 7; i++) {
        snprintf(paths[i], sizeof paths[i], "%s/%s", dir, roms[i].file);
        FILE *f = fopen(paths[i], "rb");
        if (!f) return false;
        fclose(f);
        c->roms[i] = (Z80RomLoad){ .addr = 0, .path = paths[i], .region = roms[i].region };
    }
    c->n_roms = 7;
    return true;
}

/* The car driven and the warning on, at frame 60. */
static void drive(LagondaState *s) {
    s->car.speed_mph = 120;
    s->car.rpm = 3000;
    s->car.gear = LAG_GEAR_D;
    switch_set(s, find_switch("oil_pressure"), true);
    update_adc_inputs(s);
}

int main(int argc, char **argv) {
    (void)argv;
    harness_start(OUT_FILE);
    Z80Config c;
    if (!lagonda_cfg(&c)) {
        fprintf(rep, "lagonda_core: skipped (no EPROMs; LAGONDA_ROMS=DIR)\n");
        return 0;
    }
    LagondaState *s = lagonda_create(&c);
    CHECK(s);
    mm58167_set_time(RTC(s), 6, 15, 3, 12, 34, 56);
    static const int at[] = { 1, 10, 60, 100, 200, 400 };
    int done = 0;
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
        while (done < at[i]) {
            if (done == 60) drive(s);
            frame(s);
            done++;
        }
        if (argc > 1) report(s, done);
    }
    CHECK(s->trip_bcd != 0);   /* it drove: the trip counter counted */
    lagonda_destroy(s);
#ifndef Z_HEAD
    if (argc < 2) lagonda_core_checks(&c);
#endif
    fprintf(rep, "lagonda_core: %u checks passed\n", checks);
    return 0;
}

#ifndef Z_HEAD
/* The core machine's own checks (stage Z3). */
void lagonda_core_checks(Z80Config *c) {
    LagondaState *s = lagonda_create(c), *t = lagonda_create(c);
    CHECK(s && t);
    mm58167_set_time(s->rtc, 6, 15, 3, 12, 34, 56);
    mm58167_set_time(t->rtc, 6, 15, 3, 12, 34, 56);
    /* frames end at VDP 0's frame: 262 lines of 342 dots at 5.37 MHz */
    for (int i = 0; i < 5; i++) lagonda_frame(s);
    GemuTime t0 = gemu_machine_time(s->core->machine);
    uint64_t c0 = s->cpu->cycles;
    for (int i = 0; i < 60; i++) lagonda_frame(s);
    CHECK(gemu_z80_ended(s->core->cpu) == GEMU_Z80_DONE);
    double per = (double)(s->cpu->cycles - c0) / 60.0;
    CHECK(per > TMS9918_LINES * s->line_cycles - 20 && per < TMS9918_LINES * s->line_cycles + 20);
    CHECK(gemu_machine_time(s->core->machine) - t0 == gemu_clocks(4000000, s->cpu->cycles - c0));
    /* the program space: the RAM's mirrors; the EPROMs read-only */
    GemuSpace *p = s->core->prog, *io = s->core->io;
    gemu_space_write8(p, 0x2C10, 0x5A);
    CHECK(s->ram[0][0x10] == 0x5A && gemu_space_read8(p, 0x2010) == 0x5A);
    gemu_space_write8(p, 0x0000, (uint8_t)~s->code[0]);
    CHECK(gemu_space_read8(p, 0x0000) == s->code[0] && gemu_space_read8(p, 0x5000) == 0xFF);
    /* the io space: the trip counter, the clock, the lamps */
    s->trip_bcd = 0x1234;
    CHECK(gemu_space_read8(io, 0x10) == 0x34 && gemu_space_read8(io, 0x18) == 0x12);
    CHECK(gemu_space_read8(io, 0x47) == 0x06 && gemu_space_read8(io, 0x46) == 0x15);
    CHECK(gemu_space_read8(io, 0xFF) == port_ff(s) && gemu_space_read8(io, 0x30) == 0xFF);
    s->trip_bcd = 0;
    /* the oil pressure warning: the speech card plays, the stream follows */
    drive(s);
    bool played = false;
    for (int i = 0; i < 400 && !played; i++) { lagonda_frame(s); played = s->speech.playing; }
    CHECK(played);
    for (int i = 0; i < 20; i++) lagonda_frame(s);
    CHECK(s->lp2 != 0.0);
    /* state: round trip; another machine runs on alike */
    uint8_t *a, *b;
    size_t na, nb;
    CHECK(gemu_machine_save_state(s->core->machine, &a, &na));
    for (int i = 0; i < 20; i++) lagonda_frame(s);
    CHECK(gemu_machine_load_state(s->core->machine, a, na));
    CHECK(gemu_machine_save_state(s->core->machine, &b, &nb));
    CHECK(na == nb && !memcmp(a, b, na));
    free(b);
    CHECK(gemu_machine_load_state(t->core->machine, a, na));
    for (int i = 0; i < 30; i++) { lagonda_frame(s); lagonda_frame(t); }
    CHECK(!memcmp(s->ram, t->ram, sizeof s->ram) && s->cpu->cycles == t->cpu->cycles &&
          !memcmp(s->vdp[0]->vram, t->vdp[0]->vram, TMS9918_VRAM_SIZE) &&
          s->speech.counter == t->speech.counter && s->trip_bcd == t->trip_bcd);
    free(a);
    /* step: instructions through the machine */
    uint64_t n = lag_insns(s);
    lagonda_step(s, 1000);
    CHECK(lag_insns(s) == n + 1000);
    /* the panel screen: the three CRTs */
    const GemuBitmap *bmp = gemu_screen_render(s->core->screen);
    CHECK(bmp->width == PANEL_W && bmp->height == PANEL_H);
    CHECK(bmp->pix[10 * bmp->stride + TMS9918_FB_W + PANEL_GAP + 10] == s->vdp[0]->fb[10 * TMS9918_FB_W + 10]);
    lagonda_destroy(t);
    lagonda_destroy(s);
}
#endif
