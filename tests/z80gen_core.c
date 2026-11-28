/* The generic Z80 on the core (stage Z2).  A stand-in program (the timer
 * interrupting at IM 1, the serial port printing, a write to ROM) and, when
 * roms/tests/zex/zexdoc.com is here, ZEXDOC under -cpm.  Built with
 * -DZ_HEAD against HEAD's driver it runs HEAD's machine to the same cycle
 * targets (HEAD's run loop sliced frames from wherever the last one ended;
 * the targets here are absolute, which the core's frames are too).
 * Run: ninja check-z80gen_core */
#include "../hardware/machine/z80/machine_z80.c"
#include "z80_harness.h"

/* The stand-in ROM, at 0000h:
 *   0000  DI; LD SP,FF00h; IM 1; timer reload 1000 (OUT 00h/01h);
 *         control 07h (on, interrupt, auto-reload); EI; JP 0100h
 *   0038  the interrupt: PUSH AF; (8000h)++; OUT (02h),87h (clear
 *         pending, keep counting); POP AF; EI; RETI
 *   0100  the loop: (8001h)++ and every 64th pass print 'A' + the
 *         interrupt count's low nibble (OUT 10h), store it at 8002h and
 *         at 0050h (ROM: dropped) */
static const unsigned char standin[0x200] = {
    [0x000] = 0xF3, 0x31, 0x00, 0xFF, 0xED, 0x56, 0x3E, 0xE8, 0xD3, 0x00, 0x3E, 0x03,
              0xD3, 0x01, 0x3E, 0x07, 0xD3, 0x02, 0xFB, 0xC3, 0x00, 0x01,
    [0x038] = 0xF5, 0x3A, 0x00, 0x80, 0x3C, 0x32, 0x00, 0x80, 0x3E, 0x87, 0xD3, 0x02,
              0xF1, 0xFB, 0xED, 0x4D,
    [0x100] = 0x21, 0x01, 0x80, 0x34, 0x7E, 0xE6, 0x3F, 0x20, 0xF7, 0x3A, 0x00, 0x80,
              0xE6, 0x0F, 0xC6, 0x41, 0xD3, 0x10, 0x32, 0x02, 0x80, 0x32, 0x50, 0x00,
              0x18, 0xE6,
};

#define OUT_FILE "z80gen-out.txt"

#ifdef Z_HEAD
#define CPU(s) (&(s)->cpu)
#define INSNS(s) ((s)->insn_count)
static uint64_t per_frame(const Z80Config *cfg) {
    uint64_t n = cfg->clock_hz / 60u;
    return cfg->cpm_path ? n * 50 : n;
}
static uint64_t frames_run;
static void frame(Z80GenericState *s, const Z80Config *cfg) {   /* HEAD's machine, one frame */
    uint64_t target = ++frames_run * per_frame(cfg);
    while (s->cpu.cycles < target && !s->exit_requested) step_one(s);
}
#else
#define CPU(s) ((s)->cpu)
#define INSNS(s) generic_insns(s)
static void frame(Z80GenericState *s, const Z80Config *cfg) { (void)cfg; generic_frame(s); }
void z80gen_core_checks(void);
#endif

static void report(Z80GenericState *s, int frames) {
    fprintf(rep, "%4d", frames);
    hash("ram", s->mem, sizeof s->mem);
    hash_output(OUT_FILE);
    fprintf(rep, " tmr %04X %02X %d", s->timer_reload, s->timer_ctrl, s->timer_pending);
    z80_report(CPU(s), INSNS(s));
}

static Z80Config cfg_for(const char *rom, const char *cpm) {
    Z80Config c;
    memset(&c, 0, sizeof c);
    c.machine = Z80_MACHINE_GENERIC;
    c.clock_hz = 4000000u;
    c.display_type = GEMU_DISPLAY_NONE;
    if (rom) { c.roms[0].path = rom; c.roms[0].addr = 0; c.n_roms = 1; }
    c.cpm_path = cpm;
    return c;
}

static void run(Z80Config *c, const int *at, size_t n, bool verbose) {
    Z80GenericState *s = z80_generic_create(c);
    CHECK(s);
#ifdef Z_HEAD
    frames_run = 0;
#endif
    int done = 0;
    for (size_t i = 0; i < n; i++) {
        while (done < at[i] && !s->exit_requested) { frame(s, c); done++; }
        if (verbose) report(s, done);
    }
    if (!c->cpm_path) {
        CHECK(s->mem[0x8000] != 0 && s->mem[0x0050] == 0);   /* interrupts; ROM kept */
    }
    z80_generic_destroy(s);
}

int main(int argc, char **argv) {
    (void)argv;
    harness_start(OUT_FILE);
    write_file("z80gen.rom", standin, sizeof standin);
    Z80Config c = cfg_for("z80gen.rom", NULL);
    static const int at[] = { 1, 10, 100, 300 };
    run(&c, at, sizeof at / sizeof at[0], argc > 1);

    /* ZEXDOC=FILE, or roms/ from the run directory (ninja check runs each
     * test in build/tests/run/NAME-XXXX) */
    const char *zex = getenv("ZEXDOC");
    FILE *f = NULL;
    static const char *const tries[] = { "../../../../roms/tests/zex/zexdoc.com",
                                         "../../../roms/tests/zex/zexdoc.com" };
    for (size_t i = 0; !zex && i < 2; i++)
        if ((f = fopen(tries[i], "rb"))) { fclose(f); zex = tries[i]; }
    if (!zex) zex = tries[0];
    f = fopen(zex, "rb");
    if (f) {
        fclose(f);
        if (argc > 1) fprintf(rep, "zexdoc\n");
        freopen(OUT_FILE, "w", stdout);
        Z80Config z = cfg_for(NULL, zex);
        static const int zat[] = { 1, 20, 200 };
        run(&z, zat, sizeof zat / sizeof zat[0], argc > 1);
    } else if (argc > 1) {
        fprintf(rep, "zexdoc: not here (%s)\n", zex);
    }
#ifndef Z_HEAD
    if (argc < 2) z80gen_core_checks();
#endif
    fprintf(rep, "z80gen_core: %u checks passed\n", checks);
    return 0;
}

#ifndef Z_HEAD
/* The core machine's own checks (stage Z2). */
void z80gen_core_checks(void) {
    Z80Config c = cfg_for("z80gen.rom", NULL);
    Z80GenericState *s = z80_generic_create(&c), *t = z80_generic_create(&c);
    CHECK(s && t);
    /* the bus: the ROM read-only in the monitor's view, RAM above */
    GemuSpace *bus = s->core->bus;
    CHECK(gemu_space_read8(bus, 0x0000) == standin[0]);
    gemu_space_write8(bus, 0x0000, 0x00);
    CHECK(s->mem[0] == standin[0]);
    gemu_space_write8(bus, 0x9000, 0x5A);
    CHECK(s->mem[0x9000] == 0x5A);
    /* frames are machine time: 60 of them a second */
    GemuTime t0 = gemu_machine_time(s->core->machine);
    for (int i = 0; i < 60; i++) generic_frame(s);
    CHECK(gemu_machine_time(s->core->machine) - t0 == 60 * gemu_clocks(c.clock_hz, c.clock_hz / 60));
    CHECK(s->cpu->cycles >= 60u * (c.clock_hz / 60) && s->cpu->cycles < 60u * (c.clock_hz / 60) + 30);
    /* the timer: 1,000 T-states a period */
    CHECK(s->mem[0x8000] == (uint8_t)(s->cpu->cycles / 1000));
    CHECK(gemu_timer_running(s->core->timer));
    /* state: round trip; another machine runs on alike */
    uint8_t *a, *b;
    size_t na, nb;
    CHECK(gemu_machine_save_state(s->core->machine, &a, &na));
    for (int i = 0; i < 20; i++) generic_frame(s);
    CHECK(gemu_machine_load_state(s->core->machine, a, na));
    CHECK(gemu_machine_save_state(s->core->machine, &b, &nb));
    CHECK(na == nb && !memcmp(a, b, na));
    free(b);
    CHECK(gemu_machine_load_state(t->core->machine, a, na));
    for (int i = 0; i < 30; i++) { generic_frame(s); generic_frame(t); }
    CHECK(!memcmp(s->mem, t->mem, sizeof s->mem) && s->cpu->cycles == t->cpu->cycles &&
          s->cpu->pc == t->cpu->pc && generic_insns(s) == generic_insns(t));
    free(a);
    /* a monitor reset stops the timer; the program starts it again */
    generic_reset(t, &c);
    CHECK(!gemu_timer_running(t->core->timer) && t->cpu->pc == 0);
    generic_frame(t);
    CHECK(gemu_timer_running(t->core->timer));
    /* step runs instructions through the machine, the timer's interrupts
     * included (breakpoints and watchpoints: through the monitor, by hand) */
    uint64_t n = generic_insns(s);
    uint8_t ints = s->mem[0x8000];
    generic_step(s, 2000);
    CHECK(generic_insns(s) == n + 2000 && s->mem[0x8000] != ints);
    CHECK(gemu_z80_ended(s->core->cpu) == GEMU_Z80_DONE);
    z80_generic_destroy(t);
    z80_generic_destroy(s);
}
#endif
