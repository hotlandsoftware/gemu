/* The COSMAC VIP on the core (stage R2), with the stand-in from
 * tests/data/rca/standin.py.  Built with -DR_HEAD against HEAD's driver it
 * runs HEAD's own frame.  Run: ninja check-vip_core */
#include "../hardware/machine/rca/machine_vip.c"
#include "rca_harness.h"

#ifdef R_HEAD
#define CPU(s) (&(s)->cpu)
static void frame(RcaVipState *s) {           /* HEAD's rca_machine_run, one frame */
    const RcaConfig *cfg = s->cfg;
    s->key_down = -1;
    if (rca_vip_has_keypad(cfg))
        for (int k = 0; k < 16; k++) if (s->keys[k]) { s->key_down = k; break; }
    s->cpu.EF[3] = !vip_ascii_pending(s) && (!rca_vip_has_keypad(cfg) || s->key_down < 0);
    const int n = cfg->vga == RCA_VGA_CDP1869 ? CDP1869_MCYCLES_PER_FRAME : CDP1861_MCYCLES_PER_FRAME;
    for (int i = 0; i < n; i++) {
        s->cpu.EF[1] = !s->ef2_down;
        if (s->tape.playing) s->cpu.EF[2] = (bool)vip_tape_ef3(&s->tape, s->cpu.cycle_count);
        else s->cpu.EF[2] = !rca_vip_has_vp601(cfg) || s->ascii_key < 0;
        s->cpu.EF[3] = !vip_ascii_pending(s) && (!rca_vip_has_keypad(cfg) || s->key_down < 0);
        cdp1802_step(&s->cpu);
    }
}
#else
#define CPU(s) ((s)->cpu)
static void frame(RcaVipState *s) { vip_frame(s); }
void vip_core_checks(void);
#endif

static void report(RcaVipState *s, int frames) {
    printf("%4d", frames);
    hash("ram", s->mem, 0x8000);
    hash("vram", s->vram, sizeof s->vram);
    cpu_report(CPU(s));
}

int main(int argc, char **argv) {
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    write_file("vip.rom", rca_standin_vip, sizeof rca_standin_vip);
    RcaConfig c = rca_cfg(RCA_MACHINE_COSMAC_VIP, RCA_VGA_CDP1861, "vip.rom", 0);
    c.keyboard = RCA_KEYBOARD_KEYPAD;
    RcaVipState *s = rca_vip_create(&c);
    CHECK(s);
    static const int at[] = { 1, 10, 100, 500 };
    int done = 0;
    for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
        while (done < at[i]) { frame(s); done++; }
        if (argc > 1) report(s, done);
    }
    CHECK(s->mem[0x600] == (uint8_t)500 && s->mem[0x601] == 0xFF);   /* frames; no key */
    rca_vip_destroy(s);
#ifndef R_HEAD
    if (argc < 2) vip_core_checks();
#endif
    printf("vip_core: %u checks passed\n", checks);
    return 0;
}

#ifndef R_HEAD
static float *cap;
static size_t cap_n;
static void capture(void *ud, const float *s, size_t n) {
    (void)ud;
    cap = realloc(cap, (cap_n + n) * sizeof *cap);
    memcpy(cap + cap_n, s, n * sizeof *cap);
    cap_n += n;
}

/* The core machine's own checks (stage R2). */
void vip_core_checks(void) {
    RcaConfig c = rca_cfg(RCA_MACHINE_COSMAC_VIP, RCA_VGA_CDP1861, "vip.rom", 0);
    c.keyboard = RCA_KEYBOARD_KEYPAD;
    c.sound_hw = RCA_SOUND_PCSPK;
    RcaVipState *s = rca_vip_create(&c), *t = rca_vip_create(&c);
    CHECK(s && t && s->core->vdc && !s->core->vis && s->core->tone);
    /* frames are exactly their machine cycles and time */
    GemuTime t0 = gemu_machine_time(s->core->machine);
    uint64_t c0 = s->cpu->cycle_count;
    for (int i = 0; i < 60; i++) vip_frame(s);
    CHECK(s->cpu->cycle_count - c0 == 60u * CDP1861_MCYCLES_PER_FRAME);
    CHECK(gemu_machine_time(s->core->machine) - t0 == 60 * gemu_clocks(VIP_MCYCLE_HZ, CDP1861_MCYCLES_PER_FRAME));
    /* the screen is the CDP1861's frame */
    const GemuBitmap *b = gemu_screen_render(s->core->screen);
    CHECK(b->width == CDP1861_DISPLAY_W && b->height == CDP1861_DISPLAY_H);
    bool same = true;
    for (int i = 0; i < CDP1861_DISPLAY_W * CDP1861_DISPLAY_H; i++)
        same &= b->pix[(i / CDP1861_DISPLAY_W) * b->stride + i % CDP1861_DISPLAY_W] ==
                (s->vram[i] ? 0xFFFFFFFFu : 0xFF100080u);
    CHECK(same);
    /* a pause stops a running frame at an instruction's fetch */
    gemu_monitor_pause(s->monitor);
    vip_frame(s);
    CHECK(gemu_cdp1802_ended(s->core->cpu) == GEMU_CDP1802_STOPPED && s->cpu->state == CDP1802_S_FETCH);
    /* state: round trip; another VIP runs on alike */
    uint8_t *a, *bb;
    size_t na, nb;
    RcaVipState *u = rca_vip_create(&c);
    CHECK(u);
    for (int i = 0; i < 61; i++) vip_frame(u);
    CHECK(gemu_machine_save_state(u->core->machine, &a, &na));
    for (int i = 0; i < 20; i++) vip_frame(u);
    CHECK(gemu_machine_load_state(u->core->machine, a, na));
    CHECK(gemu_machine_save_state(u->core->machine, &bb, &nb));
    CHECK(na == nb && !memcmp(a, bb, na));
    free(bb);
    CHECK(gemu_machine_load_state(t->core->machine, a, na));
    for (int i = 0; i < 30; i++) { vip_frame(u); vip_frame(t); }
    CHECK(!memcmp(u->mem, t->mem, VIP_MEM_SIZE) && !memcmp(u->vram, t->vram, sizeof u->vram) &&
          u->cpu->cycle_count == t->cpu->cycle_count && u->cpu->R[3] == t->cpu->R[3]);
    free(a);
    /* the beeper: Q's tone, through the mixer, on for part of the time */
    gemu_mixer_set_output(u->core->machine, 44100, 1, capture, NULL);
    for (int i = 0; i < 120; i++) vip_frame(u);
    size_t on = 0;
    for (size_t i = 0; i < cap_n; i++) on += cap[i] != 0.0f;
    printf("vip_core: tone: %zu samples, %zu sounding\n", cap_n, on);
    CHECK(cap_n > 80000 && on > cap_n / 8 && on < cap_n);
    free(cap);
    rca_vip_destroy(u);
    rca_vip_destroy(t);
    rca_vip_destroy(s);
}
#endif
