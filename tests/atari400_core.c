/* The Atari 400's core machine (stage T): its devices' state, with the
 * stand-in OS from tests/data/atari/standin.py.
 * Run: ninja check-atari400_core */
#include "../hardware/machine/mos/machine_atari400.c"
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

static const char *os_path, *cart_path, *atr_path;

static MosConfig cfg_for(bool cart, bool axlon) {
    MosConfig c;
    memset(&c, 0, sizeof c);
    c.machine = MOS_MACHINE_ATARI400;
    c.cpu = MOS_CPU_6502;
    c.display_type = GEMU_DISPLAY_NONE;
    c.mem_size = 0x4000;
    c.roms[0].path = os_path;
    c.roms[0].addr = 0xD800;
    c.n_roms = 1;
    c.cart_path = cart ? cart_path : NULL;
    c.atari810 = true;
    c.fda_path = atr_path;
    c.axlon1056 = axlon;
    c.generic_keyboard = true;
    return c;
}

static void steps(Atari400State *s, int n) {
    for (int i = 0; i < n; i++) a400_insn(s);
}

static bool same_bytes(const uint8_t *a, size_t na, const uint8_t *b, size_t nb) {
    return na == nb && !memcmp(a, b, na);
}

static bool same_machine(const Atari400State *a, const Atari400State *b) {
    return a->cpu.PC == b->cpu.PC && a->cpu.A == b->cpu.A && a->cpu.X == b->cpu.X &&
           a->cpu.Y == b->cpu.Y && a->cpu.SP == b->cpu.SP && a->cpu.P == b->cpu.P &&
           a->cpu.cycle_count == b->cpu.cycle_count &&
           !memcmp(a->ram, b->ram, sizeof a->ram) &&
           !memcmp(&a->pokey, &b->pokey, sizeof a->pokey) &&
           a->antic.nmien == b->antic.nmien && a->antic.dlist == b->antic.dlist &&
           !memcmp(a->antic.pixels, b->antic.pixels, sizeof a->antic.pixels) &&
           !memcmp(a->antic.pixels_argb, b->antic.pixels_argb, sizeof a->antic.pixels_argb) &&
           a->axlon_bank == b->axlon_bank && a->cart_bank == b->cart_bank &&
           (!a->axlon_ram || !memcmp(a->axlon_ram, b->axlon_ram,
                                     (size_t)A400_AXLON_EXT_BANKS * A400_AXLON_BANK_SIZE));
}

int main(void) {
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    os_path = write_file("core-os.rom", a400_standin_os, sizeof a400_standin_os);
    cart_path = write_file("core-cart.rom", a400_standin_cart, sizeof a400_standin_cart);
    atr_path = write_file("core-disk.atr", a400_standin_atr, sizeof a400_standin_atr);
    MosConfig c1 = cfg_for(true, true), c2 = cfg_for(true, true), c3 = cfg_for(false, true),
              c4 = cfg_for(true, false);

    Atari400State *s = atari400_create(&c1);
    CHECK(s && s->core && s->core->machine && s->core->axlon && s->core->fdd);
    steps(s, 200000);
    antic_render_frame(&s->antic);
    /* the OS ran: the cartridge's line, Axlon's markers, the SIO read */
    CHECK(s->ram[0x2000 + 40] == 0x30 && s->ram[0x2000 + 86] == 0x5C);
    CHECK(s->ram[0x2050] == 0x11 && s->ram[0x2051] == 0xA5 && s->ram[0x2052] == 0x5A);
    CHECK(s->ram[0x2054] == 0x01 && s->ram[0x2000 + 120] == 1);

    /* save, run on, load: back where it was; save again: the same bytes */
    uint8_t *a, *b;
    size_t na, nb;
    CHECK(gemu_machine_save_state(s->core->machine, &a, &na));
    uint64_t cyc = s->cpu.cycle_count;
    uint8_t bank = s->axlon_bank;
    s->antic.colbk ^= 0x0E;
    steps(s, 30000);
    antic_render_frame(&s->antic);
    CHECK(s->cpu.cycle_count != cyc);
    CHECK(gemu_machine_load_state(s->core->machine, a, na));
    CHECK(s->cpu.cycle_count == cyc && s->axlon_bank == bank);
    CHECK(gemu_machine_save_state(s->core->machine, &b, &nb));
    CHECK(same_bytes(a, na, b, nb));
    free(b);

    /* another machine with the same cartridge and Axlon: loads it (the ARGB
     * frame worked out again) and runs on exactly as the first */
    Atari400State *t = atari400_create(&c2);
    CHECK(t);
    steps(t, 777);
    t->axlon_ram[12345] = 0x77;
    CHECK(gemu_machine_load_state(t->core->machine, a, na));
    CHECK(same_machine(s, t));
    steps(s, 100000);
    steps(t, 100000);
    CHECK(same_machine(s, t));

    /* no cartridge, or no Axlon: refused, untouched */
    Atari400State *u = atari400_create(&c3);
    CHECK(u);
    steps(u, 500);
    uint64_t ucyc = u->cpu.cycle_count;
    CHECK(!gemu_machine_load_state(u->core->machine, a, na));
    CHECK(strstr(gemu_machine_error(u->core->machine), "cartridge"));
    CHECK(u->cpu.cycle_count == ucyc);
    Atari400State *v = atari400_create(&c4);
    CHECK(v && !v->core->axlon);
    CHECK(!gemu_machine_load_state(v->core->machine, a, na));
    free(a);

    /* T3: frames through the scheduler are the instructions a second
     * machine steps by hand; the machine's time is the CPU's cycles at its
     * clock; a frame is 262 lines of 114 cycles */
    MosConfig c5 = cfg_for(true, false), c6 = cfg_for(true, false);
    Atari400State *f = atari400_create(&c5), *g = atari400_create(&c6);
    CHECK(f && g);
    GemuTime t0 = gemu_machine_time(f->core->machine);
    uint64_t c0 = f->cpu.cycle_count, fr0 = f->frame;
    uint64_t sf0 = gemu_screen_frame_number(a400_screen(f));
    for (int i = 0; i < 100; i++) a400_run_frame(f);
    CHECK(f->frame == fr0 + 100 && !f->core->stopped && f->line == 0);
    /* T4: ANTIC's screen began each of those frames, and shows its picture */
    CHECK(gemu_screen_frame_number(a400_screen(f)) == sf0 + 100);
    const GemuBitmap *bmp = gemu_screen_render(a400_screen(f));
    CHECK(bmp->width == ANTIC_FB_W && bmp->height == ANTIC_FB_H);
    bool same = true;
    for (int y = 0; y < ANTIC_FB_H; y++)
        same &= !memcmp(bmp->pix + y * bmp->stride, f->antic.pixels_argb + y * ANTIC_FB_W,
                        ANTIC_FB_W * sizeof(uint32_t));
    CHECK(same);
    int vpos = gemu_screen_vpos(a400_screen(f));
    CHECK(vpos >= 0 && vpos < f->antic.lines_total);
    CHECK(f->ram[0x80] >= 99);                           /* the stand-in OS's VBI count */
    while (g->cpu.insn_count < f->cpu.insn_count) a400_insn(g);
    CHECK(g->cpu.cycle_count == f->cpu.cycle_count && g->cpu.PC == f->cpu.PC && g->frame == f->frame);
    CHECK(same_machine(f, g));
    uint64_t ran = f->cpu.cycle_count - c0;
    GemuTime took = gemu_machine_time(f->core->machine) - t0;
    GemuTime want = gemu_clocks((uint64_t)ATARI400_CPU_HZ_NTSC, ran);
    CHECK(took + 1000 >= want && took <= want + 1000);
    CHECK(ran >= 100u * 262u * 114u && ran < 100u * 262u * 114u + 300u);
    /* a pause stops the run after an instruction, mid-frame; the beam is
     * where stepping the same instructions puts it (the next run carries on
     * from there, not from line 0) */
    uint64_t fr1 = f->frame;
    for (int i = 0; i < 40; i++) a400_insn(f);
    gemu_monitor_pause(f->monitor);
    a400_run_frame(f);
    CHECK(f->core->stopped && f->frame == fr1 && f->line > 0);
    while (g->cpu.insn_count < f->cpu.insn_count) a400_insn(g);
    CHECK(g->line == f->line && g->frame_start == f->frame_start && same_machine(f, g));
    atari400_destroy(g);
    atari400_destroy(f);

    /* T7: the 400 has no settings: any is refused */
    MosConfig cb = cfg_for(false, false);
    cb.machine_opts = "frame_irq";
    CHECK(!atari400_create(&cb));

    atari400_destroy(v);
    atari400_destroy(u);
    atari400_destroy(t);
    atari400_destroy(s);
    remove(os_path);
    remove(cart_path);
    remove(atr_path);
    printf("atari400_core: %u checks passed\n", checks);
    return 0;
}
