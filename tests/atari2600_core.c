/* The Atari 2600's core machine (stage T): its devices' state.
 * Run: ninja check-atari2600_core */
#include "../hardware/machine/mos/machine_atari2600.c"
#include "data/atari/standin.h"

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static const char *write_file(const char *name, const unsigned char *data, size_t len) {
    FILE *f = fopen(name, "wb");
    CHECK(f && fwrite(data, 1, len, f) == len);
    fclose(f);
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

static MosConfig cfg_for(const char *cart) {
    MosConfig c;
    memset(&c, 0, sizeof c);
    c.machine = MOS_MACHINE_ATARI2600;
    c.cpu = MOS_CPU_6507;
    c.display_type = GEMU_DISPLAY_NONE;
    c.sound = MOS_SOUND_NONE;
    c.cart_path = cart;
    c.a2600_ports[0] = A2600_CTRL_CX40;
    c.a2600_ports[1] = A2600_CTRL_NONE;
    return c;
}

static void steps(Atari2600State *s, int n) {
    for (int i = 0; i < n; i++) {
        a26_bus_step(s);
        if (s->tia.frame_ready) a26_end_frame(s);
    }
    a26_bus_sync(s);
}

static bool same_bytes(const uint8_t *a, size_t na, const uint8_t *b, size_t nb) {
    return na == nb && !memcmp(a, b, na);
}

/* Everything a run shows: CPU, RAM, the TIA's frame and beam, the cart. */
static bool same_console(const Atari2600State *a, const Atari2600State *b) {
    return a->cpu.PC == b->cpu.PC && a->cpu.A == b->cpu.A && a->cpu.X == b->cpu.X &&
           a->cpu.Y == b->cpu.Y && a->cpu.SP == b->cpu.SP && a->cpu.P == b->cpu.P &&
           a->cpu.cycle_count == b->cpu.cycle_count &&
           !memcmp(a->riot.ram, b->riot.ram, sizeof a->riot.ram) &&
           a->tia.clock == b->tia.clock && a->tia.frame_count == b->tia.frame_count &&
           !memcmp(a->tia.fb, b->tia.fb, sizeof a->tia.fb) &&
           !memcmp(&a->tia.sound.ch, &b->tia.sound.ch, sizeof a->tia.sound.ch) &&
           !memcmp(a->cart.bank, b->cart.bank, sizeof a->cart.bank) &&
           !memcmp(a->fb, b->fb, sizeof a->fb);
}

int main(void) {
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    const char *f8 = write_file("core-f8.a26", a2600_test_f8, sizeof a2600_test_f8);
    const char *k4 = write_file("core-4k.a26", a2600_test_4k, sizeof a2600_test_4k);
    MosConfig c1 = cfg_for(f8), c2 = cfg_for(f8), c3 = cfg_for(k4);

    Atari2600State *s = atari2600_create(&c1);
    CHECK(s && s->core && s->core->machine);
    CHECK(s->cart.type == A26_CART_F8);
    steps(s, 300000);
    CHECK(s->riot.ram[0] != 0 && s->tia.frame_count > 10);

    /* save, run on, load: back where it was; save again: the same bytes */
    uint8_t *a, *b;
    size_t na, nb;
    CHECK(gemu_machine_save_state(s->core->machine, &a, &na));
    uint64_t cyc = s->cpu.cycle_count, frames = s->tia.frame_count;
    uint8_t ram0 = s->riot.ram[0];
    steps(s, 50000);
    CHECK(s->cpu.cycle_count != cyc && s->riot.ram[0] != ram0);
    CHECK(gemu_machine_load_state(s->core->machine, a, na));
    CHECK(s->cpu.cycle_count == cyc && s->tia.frame_count == frames && s->riot.ram[0] == ram0);
    CHECK(gemu_machine_save_state(s->core->machine, &b, &nb));
    CHECK(same_bytes(a, na, b, nb));
    free(b);

    /* another console, the same cartridge: loads it and runs on exactly as
     * the first (a picture made by the post_load included) */
    Atari2600State *t = atari2600_create(&c2);
    CHECK(t);
    steps(t, 1234);
    CHECK(gemu_machine_load_state(t->core->machine, a, na));
    CHECK(same_console(s, t));
    steps(s, 200000);
    steps(t, 200000);
    CHECK(same_console(s, t));
    build_frame(s); build_frame(t);
    CHECK(!memcmp(s->fb, t->fb, sizeof s->fb));
    CHECK(gemu_machine_save_state(t->core->machine, &b, &nb));
    uint8_t *c;
    size_t nc;
    CHECK(gemu_machine_save_state(s->core->machine, &c, &nc));
    CHECK(same_bytes(b, nb, c, nc));
    free(b); free(c);

    /* a 4K cartridge refuses the F8's state, untouched */
    Atari2600State *u = atari2600_create(&c3);
    CHECK(u && u->cart.type == A26_CART_4K);
    steps(u, 1000);
    uint64_t ucyc = u->cpu.cycle_count;
    CHECK(!gemu_machine_load_state(u->core->machine, a, na));
    CHECK(strstr(gemu_machine_error(u->core->machine), "2600 cartridge"));
    CHECK(u->cpu.cycle_count == ucyc);
    free(a);

    /* T3: frames through the scheduler are the instructions a second
     * console steps by hand; the machine's time is the CPU's cycles at the
     * colour clock, three to a cycle */
    MosConfig c4 = cfg_for(f8), c5 = cfg_for(f8);
    Atari2600State *f = atari2600_create(&c4), *g = atari2600_create(&c5);
    CHECK(f && g);
    GemuTime t0 = gemu_machine_time(f->core->machine);
    uint64_t c0 = f->cpu.cycle_count, fr0 = f->tia.frame_count;
    uint64_t sf0 = gemu_screen_frame_number(a26_screen(f));
    bool each = true;
    for (int i = 0; i < 60; i++) {
        a26_run_frame(f);
        each &= f->tia.frame_ready && !f->core->stopped;
        a26_end_frame(f);
    }
    CHECK(each);
    a26_bus_sync(f);
    CHECK(f->tia.frame_count == fr0 + 60);
    /* T4: the TIA's screen began each of those frames, and shows the window */
    CHECK(gemu_screen_frame_number(a26_screen(f)) == sf0 + 60);
    update_window(f);
    build_frame(f);
    const GemuBitmap *bmp = gemu_screen_render(a26_screen(f));
    CHECK(bmp->width == A2600_FB_W && bmp->height == A2600_FB_H);
    bool same = true;
    for (int y = 0; y < A2600_FB_H; y++)
        same &= !memcmp(bmp->pix + y * bmp->stride, f->fb + y * A2600_FB_W, A2600_FB_W * sizeof(uint32_t));
    CHECK(same);
    int vpos = gemu_screen_vpos(a26_screen(f));
    CHECK(vpos >= 0 && vpos < 262);
    while (g->cpu.insn_count < f->cpu.insn_count) steps(g, 1);
    update_window(g);
    build_frame(g);
    CHECK(g->cpu.cycle_count == f->cpu.cycle_count && same_console(f, g));
    uint64_t ran = f->cpu.cycle_count - c0;
    GemuTime took = gemu_machine_time(f->core->machine) - t0;
    GemuTime want = gemu_clocks((uint64_t)A2600_COLOR_HZ_NTSC, ran * 3u);
    CHECK(took + 1000 >= want && took <= want + 1000);
    CHECK(ran / 60 > 19000 && ran / 60 < 20500);     /* 262 lines of 76 cycles */
    atari2600_destroy(g);
    atari2600_destroy(f);

    /* T5: the TIA's samples reach the mixer's output unchanged and in order:
     * one console runs frames with the mixer's output captured, another is
     * stepped by hand and its TIA's samples taken straight from its buffer
     * (what the console queued to the host before T5) */
    MosConfig ct = cfg_for(f8), cr = cfg_for(f8);
    ct.sound = cr.sound = MOS_SOUND_TIA;      /* a dummy host device */
    Atari2600State *A = atari2600_create(&ct), *B = atari2600_create(&cr);
    CHECK(A && B && A->tia.sound.dev);
    gemu_mixer_set_output(A->core->machine, TIA_SOUND_RATE, 1, capture, NULL);
    for (int i = 0; i < 120; i++) a26_frame(A);
    size_t raw_cap = 1u << 20, raw_n = 0;
    float *raw = malloc(raw_cap * sizeof *raw);
    while (B->cpu.insn_count < A->cpu.insn_count) {
        steps(B, 1);                      /* the TIA caught up after each */
        size_t n = (size_t)B->tia.sound.buf_n;
        if (raw_n + n > raw_cap) n = raw_cap - raw_n;
        memcpy(raw + raw_n, B->tia.sound.buf, n * sizeof *raw);
        raw_n += n;
        B->tia.sound.buf_n = 0;
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
    printf("atari2600_core: T5: %zu samples captured, %zu raw, %zu differ, %zu non-zero\n",
           cap_n, raw_n, diff, nonzero);
    CHECK(diff == 0 && nonzero > cap_n / 4);
    free(raw);
    free(cap);
    atari2600_destroy(A);
    atari2600_destroy(B);

    /* T6: savevm/loadvm through the file; an old-format file is refused,
     * the console untouched */
    Atari2600State *w = atari2600_create(&c2);
    CHECK(w);
    steps(w, 30000);
    a26_cmd_savevm(w->monitor, "core-state.bin", w);
    uint64_t wcyc = w->cpu.cycle_count;
    uint8_t wram[128];
    memcpy(wram, w->riot.ram, sizeof wram);
    steps(w, 30000);
    CHECK(w->cpu.cycle_count != wcyc);
    a26_cmd_loadvm(w->monitor, "core-state.bin", w);
    CHECK(w->cpu.cycle_count == wcyc && !memcmp(w->riot.ram, wram, sizeof wram));
    FILE *of = fopen("core-old.bin", "wb");
    uint64_t om = A26_OLD_SNAPSHOT_MAGIC;
    uint32_t ov = 2;
    CHECK(of && fwrite(&om, sizeof om, 1, of) == 1 && fwrite(&ov, sizeof ov, 1, of) == 1);
    fclose(of);
    steps(w, 1000);
    wcyc = w->cpu.cycle_count;
    a26_cmd_loadvm(w->monitor, "core-old.bin", w);
    CHECK(w->cpu.cycle_count == wcyc);
    remove("core-state.bin");
    remove("core-old.bin");
    atari2600_destroy(w);

    /* T7: the switches' power-on positions as settings; a setting no
     * device has, or a bad value, stops the console being made */
    MosConfig cs = cfg_for(k4);
    cs.machine_opts = "left_difficulty=a,-color";
    Atari2600State *sw = atari2600_create(&cs);
    CHECK(sw && !sw->color && sw->diff_a[0] && !sw->diff_a[1]);
    CHECK(!(a26_read_pb(sw) & 0x08u) && (a26_read_pb(sw) & 0x40u) && !(a26_read_pb(sw) & 0x80u));
    atari2600_destroy(sw);
    cs.machine_opts = "bogus";
    CHECK(!atari2600_create(&cs));
    cs.machine_opts = "right_difficulty=c";
    CHECK(!atari2600_create(&cs));

    atari2600_destroy(u);
    atari2600_destroy(t);
    atari2600_destroy(s);
    remove(f8);
    remove(k4);
    printf("atari2600_core: %u checks passed\n", checks);
    return 0;
}
