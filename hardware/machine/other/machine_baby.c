#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif

/*
 * Manchester Baby (SSEM, 1948).
 *
 * A simulator of the machine as its operators saw it: the monitor tube and
 * the switch panel.  The processor is cpu/other/ssem.c.
 *
 * The Baby kept its store, accumulator and control on three Williams
 * tubes; the monitor showed any one of them, as a raster of 32 lines of 32
 * bits - a dash for a 1, a dot for a 0, least significant bit on the left.
 * Here the display switches choose which:
 *   Store        line n is store line n
 *   Accumulator  A, repeated on every line
 *   Control      CI and PI on alternate lines (CI on 0, 2, 4..., PI on
 *                1, 3, 5...); PI only exists while the machine runs, so its
 *                lines are dark when it's stopped
 *
 * The panel (click it, or use the keys in baby_actions):
 *   Typewriter 0-31   while stopped, write (or erase) that bit of the line
 *                     the line switches select
 *   L-stat 0-4        line switches: a line number
 *   F-stat 13-15      function switches: a function number
 *   KCC KAC KLC KSC   clear the control, the accumulator, the selected
 *                     line, the whole store
 *   Write/Erase       whether the typewriter sets or clears bits
 *   Man/Auto          what KC does: Auto obeys the next instruction of the
 *                     program (one step); Manual obeys the instruction set
 *                     up on the F-stat and L-stat switches
 *   Stop/Run          run the program from CI, about 1,100 instructions a
 *                     second, until STP or Stop
 *   KC                the single-shot key, while stopped
 *   Stop lamp         lit once STP has been obeyed; Stop then Run (or KC)
 *                     carries on
 */
#include "baby.h"
#include "ssem.h"
#include "ssem_dev.h"
#include "gemu/dev/device.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/screen.h"
#include "gemu/dev/state.h"
#include "vgafont16.h"
#include "gemu/gemu_display.h"
#include "gemu/monitor.h"
#include "gemu/screendump.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static void baby_sleep_frame(void) { Sleep(1000 / 60); }
#else
#  include <time.h>
static void baby_sleep_frame(void) {
    struct timespec ts = {0, 1000000000L / 60};
    nanosleep(&ts, NULL);
}
#endif

#define FB_W 800
#define FB_H 500
#define FPS  60
/* About 1.1 thousand instructions a second, the figure usually given for
 * the Baby. */
#define BABY_IPS 1100

/* ── Layout ──────────────────────────────────────────────────────────────── */

#define TUBE_X 12            /* monitor bezel */
#define TUBE_Y 12
#define TUBE_W 400
#define TUBE_H 440
#define SCREEN_X 32          /* phosphor */
#define SCREEN_Y 32
#define SCREEN_W 360
#define SCREEN_H 360
#define RASTER_X 52          /* bit 0 of line 0 */
#define RASTER_Y 52
#define PITCH 10             /* between bits and between lines */

#define PANEL_X 432

typedef enum {
    TUBE_STORE, TUBE_ACC, TUBE_CONTROL
} BabyTube;

typedef enum {
    C_TYPE,                  /* typewriter key, index = bit */
    C_LSTAT,                 /* line switch, index = bit 0-4 */
    C_FSTAT,                 /* function switch, index = 0-2 (bits 13-15) */
    C_KCC, C_KAC, C_KLC, C_KSC,
    C_WRITE, C_MANUAL, C_RUN,
    C_KC,
    C_TUBE,                  /* display switch, index = BabyTube */
} CtlKind;

typedef struct { int x, y, w, h; CtlKind kind; int index; } Ctl;

#define MAX_CTLS 64

struct BabyState {
    GemuMachine *machine;    /* the core machine (stage O1) */
    GemuDevice  *board, *cpu;
    GemuScreen  *screen;
    GemuDisplay *display;
    GemuMonitor *monitor;
    uint32_t     fb[FB_W * FB_H];

    Ssem        *m;          /* the cpu device's */
    BabyTube     tube;       /* what the monitor shows */
    unsigned     lstat;      /* line switches, 5 bits */
    unsigned     fstat;      /* function switches, 3 bits */
    bool         erase;      /* Write/Erase at Erase */
    bool         manual;     /* Man/Auto at Manual */
    bool         run;        /* Stop/Run at Run (the cpu device runs while it is) */

    Ctl          ctls[MAX_CTLS];
    int          n_ctls;
    int          held;       /* control under a held mouse button, -1 none */
    uint32_t     prev_actions;
};

/* ── Keys ──────────────────────────────────────────────────────────────── */

enum {
    A_RUN, A_KC, A_STORE, A_ACC, A_CONTROL, A_MANUAL, A_WRITE,
    A_KCC, A_KAC, A_KLC, A_KSC,
};

static const GemuActionDef baby_actions[] = {
    { "stop_run",          GEMU_ACTION(A_RUN),     "Space" },
    { "kc",                GEMU_ACTION(A_KC),      "Return" },
    { "display_store",     GEMU_ACTION(A_STORE),   "1" },
    { "display_acc",       GEMU_ACTION(A_ACC),     "2" },
    { "display_control",   GEMU_ACTION(A_CONTROL), "3" },
    { "manual_auto",       GEMU_ACTION(A_MANUAL),  "m" },
    { "write_erase",       GEMU_ACTION(A_WRITE),   "w" },
    { "clear_control",     GEMU_ACTION(A_KCC),     "c" },
    { "clear_accumulator", GEMU_ACTION(A_KAC),     "a" },
    { "clear_line",        GEMU_ACTION(A_KLC),     "l" },
    { "clear_store",       GEMU_ACTION(A_KSC),     "k" },
};

/* ── Drawing ───────────────────────────────────────────────────────────── */

#define COL_BG       0xFF34383Eu
#define COL_BEZEL    0xFF1E1F22u
#define COL_PHOSPHOR 0xFF030B06u
#define COL_TRACE    0xFF6BFF94u
#define COL_GLOW     0xFF1F6A37u
#define COL_TEXT     0xFFE6E6E0u
#define COL_DIM      0xFF9AA0A6u
#define COL_KEY      0xFFE8E0C8u
#define COL_KEY_DOWN 0xFFB8AE92u
#define COL_KEY_TEXT 0xFF202020u
#define COL_SWITCH   0xFF15171Au
#define COL_LEVER    0xFFC9CDD2u
#define COL_SEL      0xFF4E8DD8u

static void fill(BabyState *s, int x, int y, int w, int h, uint32_t c) {
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < FB_H; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < FB_W; xx++)
            s->fb[yy * FB_W + xx] = c;
}

static void frame(BabyState *s, int x, int y, int w, int h, uint32_t c) {
    fill(s, x, y, w, 1, c);
    fill(s, x, y + h - 1, w, 1, c);
    fill(s, x, y, 1, h, c);
    fill(s, x + w - 1, y, 1, h, c);
}

static void text(BabyState *s, int x, int y, const char *str, uint32_t c) {
    for (; *str; str++, x += 8) {
        const uint8_t *g = &vgafont16[(uint8_t)*str * FONT_HEIGHT];
        for (int r = 0; r < FONT_HEIGHT; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] & (0x80 >> b) && x + b < FB_W && y + r < FB_H && x + b >= 0 && y + r >= 0)
                    s->fb[(y + r) * FB_W + x + b] = c;
    }
}

static void text_centered(BabyState *s, int x, int w, int y, const char *str, uint32_t c) {
    text(s, x + (w - 8 * (int)strlen(str)) / 2, y, str, c);
}

static void disc(BabyState *s, int cx, int cy, int r, uint32_t c) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) fill(s, cx + x, cy + y, 1, 1, c);
}

/* ── The monitor tube ──────────────────────────────────────────────────── */

/* One line of the raster: 32 bits, bit 0 on the left. */
static void raster_line(BabyState *s, int line, uint32_t word) {
    int y = RASTER_Y + line * PITCH + PITCH / 2;
    for (int bit = 0; bit < 32; bit++) {
        int x = RASTER_X + bit * PITCH + PITCH / 2;
        if (word >> bit & 1) {               /* a dash */
            fill(s, x - 5, y - 2, 10, 4, COL_GLOW);
            fill(s, x - 4, y - 1, 8, 2, COL_TRACE);
        } else {                             /* a dot */
            fill(s, x - 2, y - 2, 4, 4, COL_GLOW);
            fill(s, x - 1, y - 1, 2, 2, COL_TRACE);
        }
    }
}

static bool pi_exists(const BabyState *s) {
    return s->run && !s->m->stopped;
}

static void draw_tube(BabyState *s) {
    fill(s, TUBE_X, TUBE_Y, TUBE_W, TUBE_H, COL_BEZEL);
    fill(s, SCREEN_X, SCREEN_Y, SCREEN_W, SCREEN_H, COL_PHOSPHOR);
    for (int line = 0; line < SSEM_LINES; line++) {
        switch (s->tube) {
        case TUBE_STORE:   raster_line(s, line, s->m->store[line]); break;
        case TUBE_ACC:     raster_line(s, line, s->m->a); break;
        case TUBE_CONTROL:
            if (!(line & 1)) raster_line(s, line, s->m->ci);
            else if (pi_exists(s)) raster_line(s, line, s->m->pi);
            break;
        }
    }
    static const char *const names[] = { "STORE", "ACCUMULATOR", "CONTROL" };
    text_centered(s, TUBE_X, TUBE_W, SCREEN_Y + SCREEN_H + 14, names[s->tube], COL_DIM);
}

/* ── The panel ─────────────────────────────────────────────────────────── */

static void add_ctl(BabyState *s, int x, int y, int w, int h, CtlKind k, int i) {
    if (s->n_ctls < MAX_CTLS) s->ctls[s->n_ctls++] = (Ctl){ x, y, w, h, k, i };
}

static void layout(BabyState *s) {
    s->n_ctls = 0;
    for (int b = 0; b < 32; b++)                       /* typewriter: 4 rows of 8 */
        add_ctl(s, PANEL_X + (b % 8) * 44, 32 + (b / 8) * 30, 40, 26, C_TYPE, b);
    for (int b = 0; b < 5; b++)                        /* line switches */
        add_ctl(s, PANEL_X + b * 36, 184, 28, 44, C_LSTAT, b);
    for (int b = 0; b < 3; b++)                        /* function switches */
        add_ctl(s, PANEL_X + 212 + b * 36, 184, 28, 44, C_FSTAT, b);
    static const CtlKind clears[] = { C_KCC, C_KAC, C_KLC, C_KSC };
    for (int i = 0; i < 4; i++)
        add_ctl(s, PANEL_X + i * 88, 274, 80, 28, clears[i], 0);
    add_ctl(s, PANEL_X + 20,  344, 40, 56, C_WRITE, 0);
    add_ctl(s, PANEL_X + 108, 344, 40, 56, C_MANUAL, 0);
    add_ctl(s, PANEL_X + 196, 344, 40, 56, C_RUN, 0);
    add_ctl(s, PANEL_X + 264, 344, 88, 34, C_KC, 0);
    for (int t = 0; t < 3; t++)                        /* display switches */
        add_ctl(s, TUBE_X + t * 136, TUBE_Y + TUBE_H + 10, 128, 26, C_TUBE, t);
}

static bool ctl_down(const BabyState *s, int i) { return s->held == i; }

static void draw_key(BabyState *s, int i, const char *label) {
    const Ctl *c = &s->ctls[i];
    fill(s, c->x, c->y, c->w, c->h, ctl_down(s, i) ? COL_KEY_DOWN : COL_KEY);
    frame(s, c->x, c->y, c->w, c->h, 0xFF000000u);
    text_centered(s, c->x, c->w, c->y + (c->h - 16) / 2, label, COL_KEY_TEXT);
}

/* A toggle: lever up (on) or down. */
static void draw_toggle(BabyState *s, const Ctl *c, bool up) {
    fill(s, c->x, c->y, c->w, c->h, COL_SWITCH);
    frame(s, c->x, c->y, c->w, c->h, 0xFF5A5E64u);
    int lx = c->x + c->w / 2 - 4, lh = c->h / 2 - 4;
    fill(s, lx, up ? c->y + 4 : c->y + c->h / 2, 8, lh, COL_LEVER);
}

static void draw_panel(BabyState *s) {
    char buf[64];
    text(s, PANEL_X, 12, "TYPEWRITER", COL_TEXT);
    snprintf(buf, sizeof buf, "%s line %u", s->erase ? "erases" : "writes", s->lstat);
    text(s, PANEL_X + 96, 12, buf, COL_DIM);
    snprintf(buf, sizeof buf, "LINE  %2u", s->lstat);
    text(s, PANEL_X, 164, buf, COL_TEXT);
    snprintf(buf, sizeof buf, "FUNCTION  %u %s", s->fstat, ssem_mnemonic(s->fstat));
    text(s, PANEL_X + 212, 164, buf, COL_TEXT);
    text(s, PANEL_X, 254, "CLEAR", COL_TEXT);

    for (int i = 0; i < s->n_ctls; i++) {
        const Ctl *c = &s->ctls[i];
        switch (c->kind) {
        case C_TYPE:
            snprintf(buf, sizeof buf, "%d", c->index);
            draw_key(s, i, buf);
            break;
        case C_LSTAT:
            draw_toggle(s, c, s->lstat >> c->index & 1);
            snprintf(buf, sizeof buf, "%d", c->index);
            text_centered(s, c->x, c->w, c->y + c->h + 2, buf, COL_DIM);
            break;
        case C_FSTAT:
            draw_toggle(s, c, s->fstat >> c->index & 1);
            snprintf(buf, sizeof buf, "%d", 13 + c->index);
            text_centered(s, c->x, c->w, c->y + c->h + 2, buf, COL_DIM);
            break;
        case C_KCC: draw_key(s, i, "KCC"); break;
        case C_KAC: draw_key(s, i, "KAC"); break;
        case C_KLC: draw_key(s, i, "KLC"); break;
        case C_KSC: draw_key(s, i, "KSC"); break;
        case C_WRITE: case C_MANUAL: case C_RUN: {
            static const char *const top[] = { "WRITE", "MAN", "RUN" };
            static const char *const bot[] = { "ERASE", "AUTO", "STOP" };
            int k = c->kind - C_WRITE;
            bool up = c->kind == C_WRITE ? !s->erase : c->kind == C_MANUAL ? s->manual : s->run;
            draw_toggle(s, c, up);
            text_centered(s, c->x - 20, c->w + 40, c->y - 18, top[k], up ? COL_TEXT : COL_DIM);
            text_centered(s, c->x - 20, c->w + 40, c->y + c->h + 2, bot[k], up ? COL_DIM : COL_TEXT);
            break;
        }
        case C_KC: draw_key(s, i, "KC"); break;
        case C_TUBE: {
            static const char *const names[] = { "STORE", "ACCUM", "CONTROL" };
            bool sel = (int)s->tube == c->index;
            fill(s, c->x, c->y, c->w, c->h, sel ? COL_SEL : COL_SWITCH);
            frame(s, c->x, c->y, c->w, c->h, 0xFF5A5E64u);
            text_centered(s, c->x, c->w, c->y + 5, names[c->index], sel ? 0xFFFFFFFFu : COL_DIM);
            break;
        }
        }
    }

    /* the stop lamp */
    disc(s, PANEL_X + 308, 412, 13, 0xFF101010u);
    disc(s, PANEL_X + 308, 412, 11, s->m->stopped ? 0xFFFF5A3Cu : 0xFF4A2A24u);
    text_centered(s, PANEL_X + 264, 88, 428, "STOP", s->m->stopped ? COL_TEXT : COL_DIM);

    text(s, PANEL_X, 456, "Space Run/Stop  Enter KC  M Man/Auto", COL_DIM);
    text(s, PANEL_X, 474, "W Wr/Erase  1-3 Display  C A L K Clear", COL_DIM);
}

static void baby_render(BabyState *s) {
    for (int i = 0; i < FB_W * FB_H; i++) s->fb[i] = COL_BG;
    draw_tube(s);
    draw_panel(s);
}

/* ── Operating the panel ───────────────────────────────────────────────── */

static void clear_control(BabyState *s) {
    s->m->ci = 0;
    s->m->pi = 0;
}

static void set_run(BabyState *s, bool run) {
    if (run && !s->run) s->m->stopped = false;    /* Stop -> Run carries on after STP */
    s->run = run;
    gemu_ssem_set_running(s->cpu, run);
}

/* KC: one instruction, while stopped. */
static void press_kc(BabyState *s) {
    if (s->run) return;
    s->m->stopped = false;
    if (s->manual) ssem_obey(s->m, SSEM_INSN(s->fstat, s->lstat));
    else ssem_step(s->m);
}

static void operate(BabyState *s, const Ctl *c) {
    switch (c->kind) {
    case C_TYPE:
        if (s->run) break;                         /* the typewriter works while stopped */
        if (s->erase) s->m->store[s->lstat] &= ~(1u << c->index);
        else          s->m->store[s->lstat] |= 1u << c->index;
        break;
    case C_LSTAT:  s->lstat ^= 1u << c->index; break;
    case C_FSTAT:  s->fstat ^= 1u << c->index; break;
    case C_KCC:    clear_control(s); break;
    case C_KAC:    s->m->a = 0; break;
    case C_KLC:    s->m->store[s->lstat] = 0; break;
    case C_KSC:    memset(s->m->store, 0, sizeof s->m->store); break;
    case C_WRITE:  s->erase = !s->erase; break;
    case C_MANUAL: s->manual = !s->manual; break;
    case C_RUN:    set_run(s, !s->run); break;
    case C_KC:     press_kc(s); break;
    case C_TUBE:   s->tube = (BabyTube)c->index; break;
    }
}

static int ctl_at(const BabyState *s, int x, int y) {
    for (int i = 0; i < s->n_ctls; i++) {
        const Ctl *c = &s->ctls[i];
        if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h) return i;
    }
    return -1;
}

static void handle_input(BabyState *s, uint32_t actions) {
    GemuPointerState p = gemu_display_get_pointer(s->display);
    if (p.pressed) {
        int i = ctl_at(s, p.x, p.y);
        if (i >= 0) operate(s, &s->ctls[i]);
        s->held = i;
    }
    if (!p.button) s->held = -1;

    uint32_t down = actions & ~s->prev_actions;
    s->prev_actions = actions;
    if (down & GEMU_ACTION(A_RUN))     set_run(s, !s->run);
    if (down & GEMU_ACTION(A_KC))      press_kc(s);
    if (down & GEMU_ACTION(A_STORE))   s->tube = TUBE_STORE;
    if (down & GEMU_ACTION(A_ACC))     s->tube = TUBE_ACC;
    if (down & GEMU_ACTION(A_CONTROL)) s->tube = TUBE_CONTROL;
    if (down & GEMU_ACTION(A_MANUAL))  s->manual = !s->manual;
    if (down & GEMU_ACTION(A_WRITE))   s->erase = !s->erase;
    if (down & GEMU_ACTION(A_KCC))     clear_control(s);
    if (down & GEMU_ACTION(A_KAC))     s->m->a = 0;
    if (down & GEMU_ACTION(A_KLC))     s->m->store[s->lstat] = 0;
    if (down & GEMU_ACTION(A_KSC))     memset(s->m->store, 0, sizeof s->m->store);
}

/* A breakpoint at the next instruction's line (byte address 4 x line in
 * the monitor's view of the store). */
static bool baby_stop_before(void *ud) {
    BabyState *s = ud;
    return gemu_monitor_check_exec(s->monitor, SSEM_LINE(s->m->ci + 1) * 4u);
}

/* A frame's worth of running: the machine for 1/60 s (the cpu device runs
 * at 1,100 instructions a second while the switch is at Run). */
static void baby_frame(BabyState *s) {
    gemu_ssem_set_stop_before(s->cpu, gemu_monitor_has_exec_breakpoints(s->monitor)
                              ? baby_stop_before : NULL, s);
    gemu_machine_run_until(s->machine, gemu_machine_time(s->machine) + gemu_clocks(FPS, 1));
}

/* ── Monitor ───────────────────────────────────────────────────────────── */

static void baby_reset(BabyState *s) {
    clear_control(s);
    s->m->a = 0;
    s->m->stopped = false;
    s->m->steps = 0;
    set_run(s, false);
}

static void baby_cpu_state(void *ud, char *buf, size_t buf_len) {
    BabyState *s = ud;
    char pi[16] = "-";
    if (pi_exists(s)) ssem_disasm(s->m->pi, pi, sizeof pi);
    snprintf(buf, buf_len,
             "Manchester Baby (SSEM)\n"
             "  CI=%08X (line %u)  PI=%s  A=%08X (%d)\n"
             "  %s%s, %s, %s, L-stat=%u F-stat=%u (%s)  %llu instructions\n",
             (unsigned)s->m->ci, SSEM_LINE(s->m->ci), pi, (unsigned)s->m->a, (int32_t)s->m->a,
             s->run ? "run" : "stop", s->m->stopped ? " (stop lamp)" : "",
             s->manual ? "manual" : "auto", s->erase ? "erase" : "write",
             s->lstat, s->fstat, ssem_mnemonic(s->fstat), (unsigned long long)s->m->steps);
}

static bool baby_screendump(void *ud, const char *path) {
    BabyState *s = ud;
    return gemu_screendump_argb(path, s->fb, FB_W, FB_H);
}

/* The store for x/peek/poke: line n is bytes 4n..4n+3, least significant first. */
static bool baby_mem_read(void *ud, uint64_t addr, uint8_t *val) {
    BabyState *s = ud;
    if (addr >= SSEM_LINES * 4) return false;
    *val = (uint8_t)(s->m->store[addr / 4] >> (addr % 4 * 8));
    return true;
}

static bool baby_mem_write(void *ud, uint64_t addr, uint8_t val) {
    BabyState *s = ud;
    if (addr >= SSEM_LINES * 4) return false;
    unsigned sh = (unsigned)(addr % 4 * 8);
    s->m->store[addr / 4] = (s->m->store[addr / 4] & ~(0xFFu << sh)) | (uint32_t)val << sh;
    return true;
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/* ── The core machine (stage O1) ─────────────────────────────────────────────
 * board (baby-board): the panel's switches and the display switches; cpu
 * (ssem): the machine's executor at 1,100 instructions a second, running
 * while Stop/Run is at Run; the board's "screen" is the tube and the panel
 * as drawn.  Before stage O each frame added 1,100/60 instructions to a
 * floating-point sum and obeyed its whole part (the sum lost a little now
 * and then, so a frame sometimes ran one fewer); now the speed is the
 * executor's clock, exactly 1,100 a second.  Breakpoints are new: at a
 * line, as the monitor's byte address (4 x the line). */
static void baby_screen_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    BabyState *s = ud;
    baby_render(s);
    for (int y = clip->y0; y < clip->y1; y++)
        memcpy(bmp->pix + (size_t)y * (size_t)bmp->stride, s->fb + (size_t)y * FB_W,
               FB_W * sizeof(uint32_t));
}

static void baby_board_describe(GemuDevice *d, char *buf, size_t len) {
    const BabyState *s = *(BabyState **)d->priv;
    static const char *const tubes[] = { "store", "accumulator", "control" };
    snprintf(buf, len, "%s, %s, %s; line switches %u, function switches %u (%s); the tube shows the %s",
             s->run ? "run" : "stop", s->manual ? "manual" : "auto", s->erase ? "erase" : "write",
             s->lstat, s->fstat, ssem_mnemonic(s->fstat), tubes[s->tube]);
}

static const GemuDeviceType baby_board_type = {
    .name = "baby-board", .desc = "Manchester Baby's panel and monitor tube",
    .size = sizeof(BabyState *), .describe = baby_board_describe,
};

#define B(field) ok &= GEMU_STATE_MEMBER(d, "", s, field)
/* The panel's state: its switches.  Not saved: the controls' layout, the
 * mouse, the picture (drawn). */
static bool baby_board_state(BabyState *s, GemuDevice *d) {
    bool ok = true;
    B(tube); B(lstat); B(fstat); B(erase); B(manual); B(run);
    return ok;
}
#undef B

static bool baby_core_build(BabyState *s) {
    if (!(s->machine = gemu_machine_new("baby"))) return false;
    bool ok = (s->board = gemu_device_add(s->machine, &baby_board_type, "board", NULL, NULL)) &&
              (s->cpu = gemu_device_add(s->machine, &gemu_ssem_type, "cpu", NULL, NULL));
    if (ok) {
        *(BabyState **)s->board->priv = s;
        ok = baby_board_state(s, s->board) && gemu_machine_init(s->machine);
    }
    if (ok) {
        s->m = gemu_ssem(s->cpu);
        ok = gemu_ssem_exec(s->cpu, BABY_IPS) &&
             (s->screen = gemu_screen_new(s->board, "screen")) &&
             gemu_screen_set_size(s->screen, FB_W, FB_H);
    }
    if (ok) {
        gemu_screen_set_update(s->screen, baby_screen_update, s);
        ok = gemu_machine_start(s->machine);
    }
    if (!ok)
        fprintf(stderr, "gemu-other: core machine: %s\n", gemu_machine_error(s->machine)[0]
                ? gemu_machine_error(s->machine) : "a device couldn't be added");
    return ok;
}

BabyState *other_baby_create(const OtherConfig *cfg) {
    BabyState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->held = -1;
    layout(s);
    if (!baby_core_build(s)) {
        gemu_machine_free(s->machine);
        free(s);
        return NULL;
    }
    baby_reset(s);

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_cpu_state_cb(s->monitor, baby_cpu_state, s);
    gemu_monitor_set_screendump_cb(s->monitor, baby_screendump, s);
    GemuMonMemory mem = { baby_mem_read, baby_mem_write, s, 7, false };
    gemu_monitor_set_memory(s->monitor, &mem);
    gemu_monitor_attach_machine(s->monitor, s->machine, NULL);

    if (gemu_display_wanted(cfg->display_type)) {
        GemuDisplayConfig dc = {
            .fb_width    = FB_W,
            .fb_height   = FB_H,
            .monitor     = s->monitor,
            .scale       = cfg->display_scale,
            .actions     = baby_actions,
            .n_actions   = (int)(sizeof baby_actions / sizeof *baby_actions),
            .ini_section = "baby",
        };
        s->display = gemu_display_create(cfg->display_type, &dc);
        if (!s->display) {
            other_baby_destroy(s);
            return NULL;
        }
    }
    return s;
}

void other_baby_destroy(BabyState *s) {
    if (!s) return;
    gemu_display_destroy(s->display);
    gemu_monitor_destroy(s->monitor);
    gemu_machine_free(s->machine);
    free(s);
}

void other_baby_run(BabyState *s, const OtherConfig *cfg) {
    baby_render(s);
    gemu_monitor_start(s->monitor);

    bool running = true;
    while (running) {
        if (s->display) {
            uint32_t actions = gemu_display_poll(s->display);
            if (gemu_display_should_quit(s->display) && !cfg->no_shutdown)
                running = false;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                baby_reset(s);
            }
            if (!gemu_monitor_is_paused(s->monitor))
                handle_input(s, actions);
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { running = false; break; }
            } else if (cmd == GEMU_MON_RESET) {
                baby_reset(s);
            } else if (cmd == GEMU_MON_STEP) {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                s->m->stopped = false;
                for (uint32_t i = 0; i < n && ssem_step(s->m); i++) {}
            }
        }
        if (!running) break;

        bool paused = gemu_monitor_is_paused(s->monitor);
        if (!paused) baby_frame(s);
        if (s->display) {
            gemu_display_set_paused(s->display, paused);
            const GemuBitmap *b = gemu_screen_render(s->screen);
            gemu_display_render(s->display, b->pix, b->width, b->height);
        }
        /* Pace to the machine's time; paused, a frame's delay. */
        if (!paused) {
            gemu_machine_pace(s->machine, 0);
        } else {
            gemu_machine_pace_reset(s->machine);
            baby_sleep_frame();
        }
    }
    gemu_monitor_stop(s->monitor);
}
