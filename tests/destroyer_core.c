/* Destroyer on the core (stage R5), with the stand-in from
 * tests/data/rca/standin.py.  Built with -DR_HEAD against HEAD's driver it
 * runs HEAD's own frame.  Run: ninja check-destroyer_core */
#include "../hardware/machine/rca/machine_destroyer.c"
#include "rca_harness.h"

#ifdef R_HEAD
#define CPU(s) (&(s)->cpu)
#define VIS(s) (&(s)->vis)
static void frame(RcaDestroyerState *s) {     /* HEAD's rca_destroyer_run, one frame */
    for (unsigned i = 0; i < DESTROYER_MCYCLES_PER_FRAME; i++) {
        destroyer_video_timing(s, i);
        cdp1802_step(&s->cpu);
    }
    s->vis.non_display = true;
    s->cpu.EF[0] = true;
    if (s->coin1_latch > 0) s->coin1_latch--;
    if (s->coin2_latch > 0) s->coin2_latch--;
}
#else
#define CPU(s) ((s)->cpu)
#define VIS(s) ((s)->vis)
static void frame(RcaDestroyerState *s) { destroyer_frame(s); }
void destroyer_core_checks(void);
#endif

static void report(RcaDestroyerState *s, int frames) {
    printf("%4d", frames);
    hash("ram", s->mem + 0x2000, 0x100);
    hash("page", VIS(s)->page_ram, sizeof VIS(s)->page_ram);
    hash("char", VIS(s)->char_ram, sizeof VIS(s)->char_ram);
    cdp1869_render(VIS(s));
    hash("bitmap", VIS(s)->bitmap, sizeof VIS(s)->bitmap);
    cpu_report(CPU(s));
}

int main(int argc, char **argv) {
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    write_file("destroyer.rom", rca_standin_destroyer, sizeof rca_standin_destroyer);
    RcaConfig c = rca_cfg(RCA_MACHINE_DESTROYER, RCA_VGA_CDP1869, "destroyer.rom", 0);
    RcaDestroyerState *s = rca_destroyer_create(&c);
    CHECK(s);
    static const int at[] = { 1, 10, 100, 500 };
    int done = 0;
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
        while (done < at[i]) { frame(s); done++; }
        if (argc > 1) report(s, done);
    }
    CHECK(s->mem[0x2080] == (uint8_t)500);   /* one IRQ a frame */
    rca_destroyer_destroy(s);
#ifndef R_HEAD
    if (argc < 2) destroyer_core_checks();
#endif
    printf("destroyer_core: %u checks passed\n", checks);
    return 0;
}

#ifndef R_HEAD
/* The core machine's own checks (stage R5). */
void destroyer_core_checks(void) {
    RcaConfig c = rca_cfg(RCA_MACHINE_DESTROYER, RCA_VGA_CDP1869, "destroyer.rom", 0);
    c.sound_hw = RCA_SOUND_PCSPK;
    RcaDestroyerState *s = rca_destroyer_create(&c), *t = rca_destroyer_create(&c);
    CHECK(s && t && s->core->tone);
    /* the bus: the ROM read-only, RAM from $2000, the VIS's page RAM */
    GemuSpace *bus = s->core->bus;
    CHECK(gemu_space_read8(bus, 0x0000) == rca_standin_destroyer[0]);
    gemu_space_write8(bus, 0x0000, (uint8_t)~rca_standin_destroyer[0]);
    CHECK(gemu_space_read8(bus, 0x0000) == rca_standin_destroyer[0]);
    gemu_space_write8(bus, 0x3000, 0x5A);
    CHECK(s->mem[0x3000] == 0x5A && gemu_space_read8(bus, 0x3000) == 0x5A);
    gemu_space_write8(bus, 0xF805, 0xA5);
    CHECK(s->vis->page_ram[5] == 0xA5 && gemu_space_read8(bus, 0xF805) == 0xA5);
    /* frames: whole ones, one interrupt each */
    GemuTime t0 = gemu_machine_time(s->core->machine);
    uint64_t c0 = s->cpu->cycle_count;
    for (int i = 0; i < 60; i++) destroyer_frame(s);
    CHECK(s->cpu->cycle_count - c0 == 60u * DESTROYER_MCYCLES_PER_FRAME && s->frame_cycle == 0);
    CHECK(gemu_machine_time(s->core->machine) - t0 ==
          60 * gemu_clocks(DESTROYER_MCYCLE_HZ, DESTROYER_MCYCLES_PER_FRAME));
    CHECK(s->mem[0x2080] == 60);
    /* the input port, stored at $2081: fire held clears bit 5 */
    s->fire = true;
    destroyer_sync_inputs(s);
    destroyer_frame(s);
    CHECK(!(s->mem[0x2081] & 0x20));
    s->fire = false;
    destroyer_sync_inputs(s);
    /* a coin latches for 15 frames */
    destroyer_apply_held(s, GEMU_ACTION(6));
    destroyer_apply_held(s, 0);
    destroyer_sync_inputs(s);
    CHECK(s->coin1_latch == 15 && !s->cpu->EF[3]);
    for (int i = 0; i < 15; i++) destroyer_frame(s);
    destroyer_sync_inputs(s);
    CHECK(s->coin1_latch == 0 && s->cpu->EF[3]);
    /* the tone: OUT 4 sets the pitch, OUT 3's tone-off bit gates it */
    s->vis->tone_off = false; s->vis->tone_amp = 7; s->vis->tone_freq_sel = 0; s->vis->tone_div = 9;
    destroyer_update_sound(s);
    char buf[256];
    gemu_device_describe(s->core->tone, buf, sizeof buf);
    CHECK(strstr(buf, "558 Hz, sounding"));    /* 5,714,300 / 2 / 512 / 10 */
    s->vis->tone_off = true;
    destroyer_update_sound(s);
    gemu_device_describe(s->core->tone, buf, sizeof buf);
    CHECK(strstr(buf, "silent"));
    /* a reset keeps the DIP switches, and the VIS lets the CPU in */
    CHECK(destroyer_set_dip(s, "lives", "1") && (s->in1 & 0x30) == 0x30);
    rca_destroyer_reset(s, &c);
    CHECK((s->in1 & 0x30) == 0x30 && !s->vis->block_cpu_access && s->frame_cycle == 0);
    /* a pause stops mid-frame; the next frame call finishes that frame */
    gemu_monitor_pause(s->monitor);
    destroyer_frame(s);
    CHECK(gemu_cdp1802_ended(s->core->cpu) == GEMU_CDP1802_STOPPED && s->frame_cycle != 0);
    const GemuBitmap *b = gemu_screen_render(s->core->screen);
    CHECK(b->width == CDP1869_VISIBLE_H && b->height == CDP1869_VISIBLE_W);
    /* state: round trip; another Destroyer runs on alike */
    uint8_t *a, *bb;
    size_t na, nb;
    RcaDestroyerState *u = rca_destroyer_create(&c);
    for (int i = 0; i < 61; i++) destroyer_frame(u);
    CHECK(gemu_machine_save_state(u->core->machine, &a, &na));
    for (int i = 0; i < 20; i++) destroyer_frame(u);
    CHECK(gemu_machine_load_state(u->core->machine, a, na));
    CHECK(gemu_machine_save_state(u->core->machine, &bb, &nb));
    CHECK(na == nb && !memcmp(a, bb, na));
    free(bb);
    CHECK(gemu_machine_load_state(t->core->machine, a, na));
    for (int i = 0; i < 30; i++) { destroyer_frame(u); destroyer_frame(t); }
    CHECK(!memcmp(u->mem, t->mem, sizeof u->mem) &&
          !memcmp(u->vis->page_ram, t->vis->page_ram, sizeof u->vis->page_ram) &&
          u->cpu->cycle_count == t->cpu->cycle_count && u->mem[0x2080] == 91);
    free(a);
    rca_destroyer_destroy(u);
    rca_destroyer_destroy(t);
    rca_destroyer_destroy(s);
}
#endif
