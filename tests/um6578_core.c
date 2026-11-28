/* The UM6578 on the core (stage M7), with the stand-in from
 * tests/data/mos/standin.py.  Built with -DM_HEAD against HEAD's driver it
 * runs HEAD's own frame (to the PPU's frame).  Run: ninja check-um6578_core */
#include "../hardware/machine/mos/machine_um6578.c"
#include "data/mos/standin.h"
#include "gemu/sha256.h"

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

#ifdef M_HEAD
#define CPU(s) (&(s)->cpu)
static void frame(Um6578State *s) {        /* HEAD's um6578_run, one frame, no display */
    um6578_handle_keys(s, 0);
    um6578_mouse_frame_tick(s);
    s->ppu.dirty = false;
    while (!s->ppu.dirty) um6578_step_insn(s);
    apu2a03_flush(&s->apu);
    s->frame++;
}
#else
#define CPU(s) ((s)->cpu)
static void frame(Um6578State *s) {        /* the run loop's frame, no display */
    um6578_handle_keys(s, 0);
    um6578_mouse_frame_tick(s);
    um6578_frame(s);
}
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

static void report(Um6578State *s, int frames) {
    printf("%4d frame=%llu", frames, (unsigned long long)s->frame);
    hash("ram", s->ram_lo, sizeof s->ram_lo);
    hash("vram", s->ppu.vram, sizeof s->ppu.vram);
    hash("pixels", s->ppu.pixels_argb, sizeof s->ppu.pixels_argb);
    printf(" PC%04X A%02X SP%02X cyc %llu line %d\n", CPU(s)->PC, CPU(s)->A, CPU(s)->SP,
           (unsigned long long)CPU(s)->cycle_count, s->ppu.scanline);
}

static MosConfig cfg_for(void) {
    MosConfig c;
    memset(&c, 0, sizeof c);
    c.machine = MOS_MACHINE_UM6578;
    c.cpu = MOS_CPU_6502;
    c.display_type = GEMU_DISPLAY_NONE;
    c.roms[0].path = "um-rom.rom";
    c.roms[0].region = "maincpu";
    c.n_roms = 1;
    c.n_ports = 1;
    c.ports[0] = NES_DEVICE_CONTROLLER;
    return c;
}

#ifndef M_HEAD
void um6578_core_checks(void);
#endif

int main(int argc, char **argv) {
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("HOME", ".", 1);
    write_file("um-rom.rom", mos_standin_um6578, sizeof mos_standin_um6578);
    MosConfig c = cfg_for();
    Um6578State *s = um6578_create(&c);
    CHECK(s);
    static const int at[] = { 1, 10, 100, 500 };
    int done = 0;
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
        while (done < at[i]) { frame(s); done++; }
        if (argc > 1) report(s, done);
    }
    CHECK(s->ram_lo[0] == (uint8_t)497);   /* NMIs on after 3 frames of VRAM filling */
    um6578_destroy(s);
#ifndef M_HEAD
    if (argc < 2) um6578_core_checks();
#endif
    printf("um6578_core: %u checks passed\n", checks);
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

/* The core machine's own checks (stage M7). */
void um6578_core_checks(void) {
    MosConfig c = cfg_for(), d = cfg_for();
    Um6578State *s = um6578_create(&c);
    CHECK(s && s->core);
    uint64_t f0 = s->ppu.frame;
    for (int i = 0; i < 60; i++) frame(s);
    CHECK(s->ppu.frame == f0 + 60 && s->frame == 60);
    CHECK(gemu_screen_frame_number(s->core->screen) == 60);
    const GemuBitmap *b = gemu_screen_render(s->core->screen);
    CHECK(b->width == SH6578_WIDTH && !memcmp(b->pix, s->ppu.pixels_argb, SH6578_WIDTH * 4));
    /* the bank windows follow the registers */
    CHECK(gemu_space_read8(s->core->bus, 0x9000) == mos_standin_um6578[0x1000]);
    gemu_space_write8(s->core->bus, 0x4041, 0x07);
    CHECK(gemu_space_read8(s->core->bus, 0x9000) == mos_standin_um6578[0x7000]);
    gemu_space_write8(s->core->bus, 0x4041, 0x40);              /* past the image: $FF */
    CHECK(gemu_space_read8(s->core->bus, 0x9000) == 0xFF);
    gemu_space_write8(s->core->bus, 0x4041, 0x01);
    /* state: round trip; another console runs on alike */
    uint8_t *a, *bb;
    size_t na, nb;
    CHECK(gemu_machine_save_state(s->core->machine, &a, &na));
    for (int i = 0; i < 20; i++) frame(s);
    CHECK(gemu_machine_load_state(s->core->machine, a, na));
    CHECK(gemu_machine_save_state(s->core->machine, &bb, &nb));
    CHECK(na == nb && !memcmp(a, bb, na) && s->frame == 60);
    free(bb);
    Um6578State *t = um6578_create(&d);
    CHECK(t && gemu_machine_load_state(t->core->machine, a, na));
    for (int i = 0; i < 30; i++) { frame(s); frame(t); }
    CHECK(!memcmp(s->ram_lo, t->ram_lo, sizeof s->ram_lo) &&
          !memcmp(s->ppu.pixels_argb, t->ppu.pixels_argb, sizeof s->ppu.pixels_argb) &&
          s->cpu->cycle_count == t->cpu->cycle_count && s->cpu->PC == t->cpu->PC);
    free(a);
    um6578_destroy(t);
    um6578_destroy(s);

    /* the APU's samples reach the mixer's output unchanged and in order */
    MosConfig ca = cfg_for(), cr = cfg_for();
    ca.sound = cr.sound = MOS_SOUND_2A03;
    Um6578State *A = um6578_create(&ca), *B = um6578_create(&cr);
    CHECK(A && B && A->apu.audio_dev);
    gemu_mixer_set_output(A->core->machine, APU2A03_SAMPLE_RATE, 1, capture, NULL);
    size_t raw_cap = 1u << 20, raw_n = 0;
    float *raw = malloc(raw_cap * sizeof *raw);
    for (int i = 0; i < 120; i++) {
        frame(A);
        um6578_handle_keys(B, 0);
        um6578_mouse_frame_tick(B);
        B->ppu.dirty = false;
        do {
            gemu_machine_run_until(B->core->machine, gemu_machine_time(B->core->machine) +
                                   gemu_clocks(UM6578_EXEC_HZ, UM6578_LINES_TOTAL * 114u + 1u));
        } while (!B->ppu.dirty);
        size_t n = (size_t)B->apu.frame_n;
        memcpy(raw + raw_n, B->apu.frame_buf, n * sizeof *raw);
        raw_n += n;
        B->apu.frame_n = 0;
    }
    CHECK(cap_n > 100 * 700 && raw_n >= cap_n);
    size_t skip = raw_n - cap_n, diff = 0, nonzero = 0;
    for (size_t i = 0; i < cap_n; i++) {
        diff += cap[i] != raw[skip + i];
        nonzero += cap[i] != 0.0f;
    }
    printf("um6578_core: APU: %zu samples captured, %zu raw, %zu differ, %zu non-zero\n",
           cap_n, raw_n, diff, nonzero);
    CHECK(diff == 0 && nonzero > cap_n / 4);
    free(raw);
    free(cap);
    um6578_destroy(A);
    um6578_destroy(B);

    MosConfig e = cfg_for();
    e.machine_opts = "dips=1";
    CHECK(!um6578_create(&e));
}
#endif
