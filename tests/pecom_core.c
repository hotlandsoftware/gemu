/* The Pecom 32 on the core (stage R4), with the stand-in from
 * tests/data/rca/standin.py.  Built with -DR_HEAD against HEAD's driver it
 * runs HEAD's own frame.  Run: ninja check-pecom_core */
#include "../hardware/machine/rca/machine_pecom.c"
#include "rca_harness.h"

#ifdef R_HEAD
#define CPU(s) (&(s)->cpu)
#define VIS(s) (&(s)->vis)
static void frame(RcaPecom32State *s) {       /* HEAD's rca_pecom32_run, one frame */
    for (unsigned i = 0; i < PECOM32_MCYCLES_PER_FRAME; i++) {
        pecom_video_timing(s, i);
        cdp1802_step(&s->cpu);
    }
    s->vis.non_display = true;
    s->cpu.EF[0] = !s->key_ctrl;
}
#else
#define CPU(s) ((s)->cpu)
#define VIS(s) ((s)->vis)
static void frame(RcaPecom32State *s) { pecom_frame(s); }
void pecom_core_checks(void);
#endif

static void report(RcaPecom32State *s, int frames) {
    printf("%4d", frames);
    hash("ram", s->ram1, 0x1000);
    hash("page", VIS(s)->page_ram, sizeof VIS(s)->page_ram);
    hash("char", VIS(s)->char_ram, sizeof VIS(s)->char_ram);
    cdp1869_render(VIS(s));
    hash("bitmap", VIS(s)->bitmap, sizeof VIS(s)->bitmap);
    cpu_report(CPU(s));
}

int main(int argc, char **argv) {
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    write_file("pecom.rom", rca_standin_pecom, sizeof rca_standin_pecom);
    RcaConfig c = rca_cfg(RCA_MACHINE_PECOM32, RCA_VGA_CDP1869, "pecom.rom", 0);   /* an offset into the ROM: $8000 */
    RcaPecom32State *s = rca_pecom32_create(&c);
    CHECK(s);
    static const int at[] = { 1, 10, 100, 500 };
    int done = 0;
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
        while (done < at[i]) { frame(s); done++; }
        if (argc > 1) report(s, done);
    }
    CHECK(s->ram1[0x600] != 0 && !s->boot_mirror);
    rca_pecom32_destroy(s);
#ifndef R_HEAD
    if (argc < 2) pecom_core_checks();
#endif
    printf("pecom_core: %u checks passed\n", checks);
    return 0;
}

#ifndef R_HEAD
/* The core machine's own checks (stage R4). */
void pecom_core_checks(void) {
    RcaConfig c = rca_cfg(RCA_MACHINE_PECOM32, RCA_VGA_CDP1869, "pecom.rom", 0);
    c.sound_hw = RCA_SOUND_PCSPK;
    RcaPecom32State *s = rca_pecom32_create(&c), *t = rca_pecom32_create(&c);
    CHECK(s && t && s->core->tone && s->boot_mirror);
    /* the boot mirror: the ROM at $0000 until OUT 1, RAM after */
    CHECK(gemu_space_read8(s->core->bus, 0x0000) == rca_standin_pecom[0]);
    GemuTime t0 = gemu_machine_time(s->core->machine);
    uint64_t c0 = s->cpu->cycle_count;
    for (int i = 0; i < 60; i++) pecom_frame(s);
    CHECK(!s->boot_mirror && gemu_space_read8(s->core->bus, 0x0000) == s->ram1[0]);
    CHECK(s->cpu->cycle_count - c0 == 60u * PECOM32_MCYCLES_PER_FRAME && s->frame_cycle == 0);
    CHECK(gemu_machine_time(s->core->machine) - t0 == 60 * gemu_clocks(PECOM_MCYCLE_HZ, PECOM32_MCYCLES_PER_FRAME));
    /* a pause stops mid-frame; the next frame call finishes that frame */
    gemu_monitor_pause(s->monitor);
    pecom_frame(s);
    CHECK(gemu_cdp1802_ended(s->core->cpu) == GEMU_CDP1802_STOPPED && s->frame_cycle != 0);
    const GemuBitmap *b = gemu_screen_render(s->core->screen);
    CHECK(b->width == CDP1869_VISIBLE_W && b->height == CDP1869_VISIBLE_H);
    /* state: round trip; another Pecom runs on alike */
    uint8_t *a, *bb;
    size_t na, nb;
    RcaPecom32State *u = rca_pecom32_create(&c);
    for (int i = 0; i < 61; i++) pecom_frame(u);
    CHECK(gemu_machine_save_state(u->core->machine, &a, &na));
    for (int i = 0; i < 20; i++) pecom_frame(u);
    CHECK(gemu_machine_load_state(u->core->machine, a, na));
    CHECK(gemu_machine_save_state(u->core->machine, &bb, &nb));
    CHECK(na == nb && !memcmp(a, bb, na));
    free(bb);
    CHECK(gemu_machine_load_state(t->core->machine, a, na) && !t->boot_mirror);
    CHECK(gemu_space_read8(t->core->bus, 0x0000) == t->ram1[0]);   /* the mirror off after the load */
    for (int i = 0; i < 30; i++) { pecom_frame(u); pecom_frame(t); }
    CHECK(!memcmp(u->ram1, t->ram1, sizeof u->ram1) &&
          !memcmp(u->vis->page_ram, t->vis->page_ram, sizeof u->vis->page_ram) &&
          u->cpu->cycle_count == t->cpu->cycle_count);
    free(a);
    rca_pecom32_destroy(u);
    rca_pecom32_destroy(t);
    rca_pecom32_destroy(s);
}
#endif
