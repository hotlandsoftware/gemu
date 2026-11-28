/* 5 Clown on the core (stage M5), with the stand-in from
 * tests/data/mos/standin.py.  Built with -DM_HEAD against HEAD's driver it
 * runs HEAD's own frame (main CPU, then sound CPU, a frame's cycles each)
 * and prints what stage M compares.  Run: ninja check-fiveclown_core */
#include "../hardware/machine/mos/machine_5clown.c"
#include "data/mos/standin.h"
#include "gemu/sha256.h"

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

#ifdef M_HEAD
#define CPU(s)  (&(s)->cpu)
#define ACPU(s) (&(s)->audiocpu)
static void frame(FiveClownState *s) {      /* HEAD's fiveclown_run, one frame, no display */
    s->held_actions = 0;
    if (s->coin_latch > 0) s->coin_latch--;
    s->cpu.nmi = true;
    uint64_t target = s->cpu.cycle_count + FIVECLOWN_CYCLES_PER_FRAME;
    while (s->cpu.cycle_count < target) mos6502_step(&s->cpu);
    uint64_t audio_target = s->audiocpu.cycle_count + FIVECLOWN_CYCLES_PER_FRAME;
    while (s->audiocpu.cycle_count < audio_target) {
        uint64_t prev = s->audiocpu.cycle_count;
        mos6502_step(&s->audiocpu);
        if (s->oki.audio_dev) {
            uint64_t delta = s->audiocpu.cycle_count - prev;
            for (uint64_t i = 0; i < delta; i++) oki6295_tick(&s->oki);
        }
    }
    if (s->oki.audio_dev) oki6295_flush(&s->oki);
    fiveclown_render(s);
    s->frame++;
}
#else
#define CPU(s)  ((s)->cpu)
#define ACPU(s) ((s)->audiocpu)
static void frame(FiveClownState *s) { fiveclown_frame(s); }
#endif

static const char *write_file(const char *name, const unsigned char *data, size_t len) {
    FILE *f = fopen(name, "wb");
    CHECK(f && fwrite(data, 1, len, f) == len);
    fclose(f);
    return name;
}

static void hash(const char *label, const void *p, size_t n) {
    uint8_t d[GEMU_SHA256_DIGEST_LEN];
    GemuSha256Ctx c;
    gemu_sha256_init(&c);
    gemu_sha256_update(&c, p, n);
    gemu_sha256_final(&c, d);
    char hex[65];
    gemu_sha256_hex(d, hex);
    printf(" %s %.12s", label, hex);
}

static void report(FiveClownState *s, int frames) {
    printf("%4d frame=%llu", frames, (unsigned long long)s->frame);
    hash("nvram", s->nvram, sizeof s->nvram);
    hash("video", s->videoram, sizeof s->videoram);
    hash("color", s->colorram, sizeof s->colorram);
    hash("audio", s->audio_ram, sizeof s->audio_ram);
    hash("pixels", s->pixels_argb, sizeof s->pixels_argb);
    printf(" main A%02X X%02X Y%02X SP%02X PC%04X audio A%02X X%02X Y%02X SP%02X PC%04X\n",
           CPU(s)->A, CPU(s)->X, CPU(s)->Y, CPU(s)->SP, CPU(s)->PC,
           ACPU(s)->A, ACPU(s)->X, ACPU(s)->Y, ACPU(s)->SP, ACPU(s)->PC);
}

static MosConfig cfg_for(void) {
    MosConfig c;
    memset(&c, 0, sizeof c);
    c.machine = MOS_MACHINE_5CLOWN;
    c.cpu = MOS_CPU_6502;
    c.display_type = GEMU_DISPLAY_NONE;
    static const char *const regions[] = { "prg", "audiocpu", "gfxbanks", "proms" };
    static const char *const files[] = { "fc-prg.rom", "fc-audio.rom", "fc-gfx.rom", "fc-proms.rom" };
    for (int i = 0; i < 4; i++) {
        c.roms[i].path = files[i];
        c.roms[i].region = regions[i];
    }
    c.n_roms = 4;
    return c;
}

#ifndef M_HEAD
void fiveclown_core_checks(void);   /* the core's own checks (below main) */
#endif

int main(int argc, char **argv) {
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("HOME", ".", 1);                 /* the NVRAM file goes here, not ~/.gemu */
    write_file("fc-prg.rom", mos_standin_5clown_prg, sizeof mos_standin_5clown_prg);
    write_file("fc-audio.rom", mos_standin_5clown_audio, sizeof mos_standin_5clown_audio);
    write_file("fc-gfx.rom", mos_standin_5clown_gfx, sizeof mos_standin_5clown_gfx);
    write_file("fc-proms.rom", mos_standin_5clown_proms, sizeof mos_standin_5clown_proms);
    remove(".gemu/5clown.nvram");
    MosConfig c = cfg_for();
    FiveClownState *s = fiveclown_create(&c);
    CHECK(s);
    static const int at[] = { 1, 10, 100, 500 };
    int done = 0;
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
        while (done < at[i]) { frame(s); done++; }
        if (argc > 1) report(s, done);
    }
    /* 500 NMIs each (the sound CPU's first comes before its reset code
     * clears the count) */
    CHECK(s->nvram[0] == (uint8_t)500 && s->audio_ram[0] == (uint8_t)499);
    fiveclown_destroy(s);
#ifndef M_HEAD
    if (argc < 2) fiveclown_core_checks();
#endif
    printf("fiveclown_core: %u checks passed\n", checks);
    return 0;
}

#ifndef M_HEAD
/* The core machine's own checks (stage M5). */
void fiveclown_core_checks(void) {
    remove(".gemu/5clown.nvram");
    MosConfig c = cfg_for(), d = cfg_for();
    FiveClownState *s = fiveclown_create(&c);
    CHECK(s && s->core && s->core->cpu && s->core->audiocpu);
    /* both CPUs are executors: a frame runs both for a frame's time */
    GemuTime t0 = gemu_machine_time(s->core->machine);
    uint64_t m0 = s->cpu->cycle_count, a0 = s->audiocpu->cycle_count;
    for (int i = 0; i < 50; i++) fiveclown_frame(s);
    CHECK(gemu_machine_time(s->core->machine) == t0 + 50 * FIVECLOWN_FRAME);
    uint64_t mr = s->cpu->cycle_count - m0, ar = s->audiocpu->cycle_count - a0;
    CHECK(mr >= 50u * FIVECLOWN_CYCLES_PER_FRAME && mr < 50u * FIVECLOWN_CYCLES_PER_FRAME + 10);
    CHECK(ar >= 50u * FIVECLOWN_CYCLES_PER_FRAME && ar < 50u * FIVECLOWN_CYCLES_PER_FRAME + 10);
    CHECK(s->frame == 50 && s->nvram[0] == 50 && s->audio_ram[0] == 49);
    /* the screen is the picture */
    const GemuBitmap *b = gemu_screen_render(s->core->screen);
    CHECK(b->width == FIVECLOWN_FB_WIDTH && !memcmp(b->pix, s->pixels_argb, FIVECLOWN_FB_WIDTH * 4));
    /* 51 vsyncs: the next frame's falls on this one's end (its NMI waits) */
    CHECK(gemu_screen_frame_number(s->core->screen) == 51);
    /* state: save, run on, load, the same bytes; another board runs on alike */
    uint8_t *a, *bb;
    size_t na, nb;
    CHECK(gemu_machine_save_state(s->core->machine, &a, &na));
    for (int i = 0; i < 20; i++) fiveclown_frame(s);
    CHECK(gemu_machine_load_state(s->core->machine, a, na));
    CHECK(s->frame == 50 && s->nvram[0] == 50);
    CHECK(gemu_machine_save_state(s->core->machine, &bb, &nb));
    CHECK(na == nb && !memcmp(a, bb, na));
    free(bb);
    FiveClownState *t = fiveclown_create(&d);
    CHECK(t);
    CHECK(gemu_machine_load_state(t->core->machine, a, na));
    for (int i = 0; i < 30; i++) { fiveclown_frame(s); fiveclown_frame(t); }
    CHECK(!memcmp(s->nvram, t->nvram, sizeof s->nvram) &&
          !memcmp(s->audio_ram, t->audio_ram, sizeof s->audio_ram) &&
          !memcmp(s->pixels_argb, t->pixels_argb, sizeof s->pixels_argb) &&
          s->cpu->cycle_count == t->cpu->cycle_count &&
          s->audiocpu->cycle_count == t->audiocpu->cycle_count);
    free(a);
    fiveclown_destroy(t);
    fiveclown_destroy(s);
    /* the DIP switches as settings; one no device has stops it */
    MosConfig e = cfg_for();
    e.machine_opts = "dip2=0xC7,dip4=1";
    FiveClownState *u = fiveclown_create(&e);
    CHECK(u && u->dip2 == 0xC7 && u->dip4 == 1 && u->dip1 == 0xFF);
    CHECK(gemu_space_read8(u->core->bus, 0xCC00) == 0xC7 && gemu_space_read8(u->core->bus, 0xC400) == 0xFF);
    fiveclown_destroy(u);
    e.machine_opts = "dip5=1";
    CHECK(!fiveclown_create(&e));
}
#endif
