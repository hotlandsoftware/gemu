/* Magic Fly on the core (stage M6), with the stand-in from
 * tests/data/mos/standin.py.  Built with -DM_HEAD against HEAD's driver it
 * runs HEAD's own frame.  Run: ninja check-magicfly_core */
#include "../hardware/machine/mos/machine_magicfly.c"
#include "data/mos/standin.h"
#include "gemu/sha256.h"

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

#ifdef M_HEAD
#define CPU(s) (&(s)->cpu)
static void frame(MagicflyState *s) {      /* HEAD's magicfly_run, one frame, no display */
    s->held_actions = 0;
    if (s->coin_latch > 0) s->coin_latch--;
    s->cpu.nmi = true;
    uint64_t target = s->cpu.cycle_count + MAGICFLY_CYCLES_PER_FRAME;
    while (s->cpu.cycle_count < target) magicfly_step_insn(s);
    magicfly_dac_flush(s);
    magicfly_render(s);
    s->frame++;
}
#else
#define CPU(s) ((s)->cpu)
static void frame(MagicflyState *s) { magicfly_frame(s); }
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

static void report(MagicflyState *s, int frames) {
    printf("%4d frame=%llu", frames, (unsigned long long)s->frame);
    hash("nvram", s->nvram, sizeof s->nvram);
    {   /* without the stack page ($0100-$01FF, inside NVRAM on this board) */
        uint8_t n[MAGICFLY_NVRAM_SIZE];
        memcpy(n, s->nvram, sizeof n);
        memset(n + 0x100, 0, 0x100);
        hash("nvram-nostack", n, sizeof n);
    }
    hash("video", s->videoram, sizeof s->videoram);
    hash("color", s->colorram, sizeof s->colorram);
    hash("pixels", s->pixels_argb, sizeof s->pixels_argb);
    printf(" SP%02X Y%02X\n", CPU(s)->SP, CPU(s)->Y);
}

static MosConfig cfg_for(bool mezzo) {
    MosConfig c;
    memset(&c, 0, sizeof c);
    c.machine = MOS_MACHINE_MAGICFLY;
    c.cpu = MOS_CPU_6502;
    c.display_type = GEMU_DISPLAY_NONE;
    c.is_7mezzo = mezzo;
    static const char *const regions[] = { "maincpu", "gfx0", "gfx1", "gfx2" };
    static const char *const files[] = { "mf-prg.rom", "mf-gfx0.rom", "mf-gfx1.rom", "mf-gfx2.rom" };
    for (int i = 0; i < 4; i++) {
        c.roms[i].path = files[i];
        c.roms[i].region = regions[i];
    }
    c.n_roms = 4;
    return c;
}

#ifndef M_HEAD
void magicfly_core_checks(void);
#endif

int main(int argc, char **argv) {
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("HOME", ".", 1);                 /* the NVRAM file goes here, not ~/.gemu */
    write_file("mf-prg.rom", mos_standin_magicfly_prg, sizeof mos_standin_magicfly_prg);
    write_file("mf-gfx0.rom", mos_standin_magicfly_gfx0, sizeof mos_standin_magicfly_gfx0);
    write_file("mf-gfx1.rom", mos_standin_magicfly_gfx1, sizeof mos_standin_magicfly_gfx1);
    write_file("mf-gfx2.rom", mos_standin_magicfly_gfx2, sizeof mos_standin_magicfly_gfx2);
    for (int mezzo = 0; mezzo < 2; mezzo++) {
        remove(".gemu/magicfly.nvram");
        remove(".gemu/7mezzo.nvram");
        MosConfig c = cfg_for(mezzo);
        MagicflyState *s = magicfly_create(&c);
        CHECK(s);
        if (argc > 1) printf("%s\n", mezzo ? "7mezzo" : "magicfly");
        static const int at[] = { 1, 10, 100, 500 };
        int done = 0;
        for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
            while (done < at[i]) { frame(s); done++; }
            if (argc > 1) report(s, done);
        }
        CHECK(s->nvram[0] == (uint8_t)500);
        magicfly_destroy(s);
    }
#ifndef M_HEAD
    if (argc < 2) magicfly_core_checks();
#endif
    printf("magicfly_core: %u checks passed\n", checks);
    return 0;
}

#ifndef M_HEAD
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

/* The core machine's own checks (stage M6). */
void magicfly_core_checks(void) {
    remove(".gemu/magicfly.nvram");
    MosConfig c = cfg_for(false), d = cfg_for(false);
    MagicflyState *s = magicfly_create(&c);
    CHECK(s && s->core);
    GemuTime t0 = gemu_machine_time(s->core->machine);
    uint64_t c0 = s->cpu->cycle_count;
    for (int i = 0; i < 60; i++) magicfly_frame(s);
    CHECK(gemu_machine_time(s->core->machine) == t0 + 60 * MAGICFLY_FRAME);
    uint64_t ran = s->cpu->cycle_count - c0;
    CHECK(ran >= 60u * MAGICFLY_CYCLES_PER_FRAME && ran < 60u * MAGICFLY_CYCLES_PER_FRAME + 10);
    CHECK(s->frame == 60 && s->nvram[0] == 60);
    const GemuBitmap *b = gemu_screen_render(s->core->screen);
    CHECK(b->width == MAGICFLY_FB_WIDTH && !memcmp(b->pix, s->pixels_argb, MAGICFLY_FB_WIDTH * 4));
    /* state: round trip; another board runs on alike */
    uint8_t *a, *bb;
    size_t na, nb;
    CHECK(gemu_machine_save_state(s->core->machine, &a, &na));
    for (int i = 0; i < 20; i++) magicfly_frame(s);
    CHECK(gemu_machine_load_state(s->core->machine, a, na));
    CHECK(gemu_machine_save_state(s->core->machine, &bb, &nb));
    CHECK(na == nb && !memcmp(a, bb, na) && s->frame == 60);
    free(bb);
    MagicflyState *t = magicfly_create(&d);
    CHECK(t && gemu_machine_load_state(t->core->machine, a, na));
    for (int i = 0; i < 30; i++) { magicfly_frame(s); magicfly_frame(t); }
    CHECK(!memcmp(s->nvram, t->nvram, sizeof s->nvram) &&
          !memcmp(s->pixels_argb, t->pixels_argb, sizeof s->pixels_argb) &&
          s->cpu->cycle_count == t->cpu->cycle_count && s->cpu->PC == t->cpu->PC);
    free(a);
    magicfly_destroy(t);
    magicfly_destroy(s);

    /* the DAC's samples reach the mixer's output unchanged and in order:
     * one board runs frames with the output captured; another runs the same
     * time with its raw samples taken from the DAC's buffer */
    MosConfig ca = cfg_for(false), cr = cfg_for(false);
    ca.sound = cr.sound = MOS_SOUND_PCSPK;              /* a dummy host device */
    MagicflyState *A = magicfly_create(&ca), *B = magicfly_create(&cr);
    CHECK(A && B && A->audio_dev);
    gemu_mixer_set_output(A->core->machine, 44100, 1, capture, NULL);
    size_t raw_cap = 1u << 20, raw_n = 0;
    float *raw = malloc(raw_cap * sizeof *raw);
    for (int i = 0; i < 120; i++) {
        magicfly_frame(A);
        gemu_machine_run_until(B->core->machine, B->core->frame_end);
        B->core->frame_end += MAGICFLY_FRAME;
        size_t n = (size_t)B->dac_frame_n;
        memcpy(raw + raw_n, B->dac_frame_buf, n * sizeof *raw);
        raw_n += n;
        B->dac_frame_n = 0;
    }
    CHECK(cap_n > 100 * 700 && raw_n >= cap_n);
    size_t skip = raw_n - cap_n, diff = 0, nonzero = 0;
    for (size_t i = 0; i < cap_n; i++) {
        diff += cap[i] != raw[skip + i];
        nonzero += cap[i] != 0.0f;
    }
    printf("magicfly_core: DAC: %zu samples captured, %zu raw, %zu differ, %zu non-zero\n",
           cap_n, raw_n, diff, nonzero);
    CHECK(diff == 0 && nonzero > cap_n / 4);
    free(raw);
    free(cap);
    magicfly_destroy(A);
    magicfly_destroy(B);

    /* the DIP bank as a setting */
    MosConfig e = cfg_for(false);
    e.machine_opts = "dsw=0x7F";
    MagicflyState *u = magicfly_create(&e);
    CHECK(u && u->dsw0 == 0x7F);
    magicfly_destroy(u);
    e.machine_opts = "dip1=0";
    CHECK(!magicfly_create(&e));
}
#endif
