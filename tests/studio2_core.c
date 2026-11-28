/* The Studio II on the core (stage R3), with the stand-in from
 * tests/data/rca/standin.py.  Built with -DR_HEAD against HEAD's driver it
 * runs HEAD's own frame.  Run: ninja check-studio2_core */
#include "../hardware/machine/rca/machine_studio2.c"
#include "rca_harness.h"

#ifdef R_HEAD
#define CPU(s) (&(s)->cpu)
static void frame(RcaStudio2State *s) {       /* HEAD's rca_studio2_run, one frame */
    studio2_update_ef(s);
    const unsigned n = s->vdc.lines_total * CDP1861_MCYCLES_PER_LINE;
    for (unsigned i = 0; i < n; i++) {
        studio2_update_ef(s);
        cdp1802_step(&s->cpu);
    }
}
#else
#define CPU(s) ((s)->cpu)
static void frame(RcaStudio2State *s) { studio2_frame(s); }
void studio2_core_checks(void);
#endif

static void report(RcaStudio2State *s, int frames) {
    printf("%4d", frames);
    hash("ram", s->ram, sizeof s->ram);
    hash("vram", s->vram, sizeof s->vram);
    cpu_report(CPU(s));
}

int main(int argc, char **argv) {
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    write_file("studio2.rom", rca_standin_studio2, sizeof rca_standin_studio2);
    for (int pal = 0; pal < 2; pal++) {
        RcaConfig c = rca_cfg(RCA_MACHINE_STUDIO2, RCA_VGA_CDP1861, "studio2.rom", 0);
        c.tv_mode = pal ? RCA_TV_PAL : RCA_TV_NTSC;
        RcaStudio2State *s = rca_studio2_create(&c);
        CHECK(s);
        if (argc > 1) printf("%s\n", pal ? "PAL" : "NTSC");
        static const int at[] = { 1, 10, 100, 500 };
        int done = 0;
        for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
            while (done < at[i]) { frame(s); done++; }
            if (argc > 1) report(s, done);
        }
        CHECK(s->ram[0x080] == (uint8_t)500);      /* $0880: the frame count */
        rca_studio2_destroy(s);
    }
#ifndef R_HEAD
    if (argc < 2) studio2_core_checks();
#endif
    printf("studio2_core: %u checks passed\n", checks);
    return 0;
}

#ifndef R_HEAD
/* The core machine's own checks (stage R3). */
void studio2_core_checks(void) {
    RcaConfig c = rca_cfg(RCA_MACHINE_STUDIO2, RCA_VGA_CDP1861, "studio2.rom", 0);
    c.sound_hw = RCA_SOUND_PCSPK;
    RcaStudio2State *s = rca_studio2_create(&c), *t = rca_studio2_create(&c);
    CHECK(s && t && s->core->tone);
    GemuTime t0 = gemu_machine_time(s->core->machine);
    uint64_t c0 = s->cpu->cycle_count;
    for (int i = 0; i < 60; i++) studio2_frame(s);
    CHECK(s->cpu->cycle_count - c0 == 60u * CDP1861_MCYCLES_PER_FRAME);
    CHECK(gemu_machine_time(s->core->machine) - t0 == 60 * gemu_clocks(STUDIO2_MCYCLE_HZ, CDP1861_MCYCLES_PER_FRAME));
    const GemuBitmap *b = gemu_screen_render(s->core->screen);
    CHECK(b->width == STUDIO2_DISPLAY_W && b->height == STUDIO2_DISPLAY_H);
    /* the bus: ROM, the RAM repeated, writes below $0800 dropped */
    CHECK(gemu_space_read8(s->core->bus, 0x0000) == rca_standin_studio2[0]);
    gemu_space_write8(s->core->bus, 0x0812, 0x5A);
    CHECK(s->ram[0x012] == 0x5A && gemu_space_read8(s->core->bus, 0xFE12) == 0x5A);
    gemu_space_write8(s->core->bus, 0x0000, 0x00);
    CHECK(gemu_space_read8(s->core->bus, 0x0000) == rca_standin_studio2[0]);
    /* state: round trip; another console runs on alike */
    uint8_t *a, *bb;
    size_t na, nb;
    CHECK(gemu_machine_save_state(s->core->machine, &a, &na));
    for (int i = 0; i < 20; i++) studio2_frame(s);
    CHECK(gemu_machine_load_state(s->core->machine, a, na));
    CHECK(gemu_machine_save_state(s->core->machine, &bb, &nb));
    CHECK(na == nb && !memcmp(a, bb, na));
    free(bb);
    CHECK(gemu_machine_load_state(t->core->machine, a, na));
    for (int i = 0; i < 30; i++) { studio2_frame(s); studio2_frame(t); }
    CHECK(!memcmp(s->ram, t->ram, sizeof s->ram) && !memcmp(s->vram, t->vram, sizeof s->vram) &&
          s->cpu->cycle_count == t->cpu->cycle_count);
    free(a);
    rca_studio2_destroy(t);
    rca_studio2_destroy(s);
}
#endif
