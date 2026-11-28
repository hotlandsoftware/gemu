#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif
#include "destroyer.h"
#include "gemu/util.h"
#include "cdp1802_dev.h"
#include "cdp1869_dev.h"
#include "audio/tone.h"
#include "gemu/audio_out.h"
#include "gemu/dev/device.h"
#include "gemu/dev/screen.h"
#include "gemu/dev/space.h"
#include "gemu/dev/space_cache.h"
#include "gemu/dev/state.h"
#include "gemu/dev/stream.h"
#include "gemu/memory.h"
#include "gemu/screendump.h"
#include "gemu/gemu_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <time.h>
#endif

#define DESTROYER_N_ACTIONS 8

static const GemuActionDef destroyer_actions[DESTROYER_N_ACTIONS] = {
    { "Start1",  GEMU_ACTION(0), "1"     },
    { "Start2",  GEMU_ACTION(1), "2"     },
    { "Left",    GEMU_ACTION(2), "Left"  },
    { "Right",   GEMU_ACTION(3), "Right" },
    { "Fire",    GEMU_ACTION(4), "Space" },
    { "Service", GEMU_ACTION(5), "F2"    },
    { "Coin1",   GEMU_ACTION(6), "5"     },
    { "Coin2",   GEMU_ACTION(7), "6"     },
};

#define DESTROYER_CPU_HZ 3579000u
#define DESTROYER_FRAME_HZ 50u
#define DESTROYER_MCYCLES_PER_FRAME ((DESTROYER_CPU_HZ / CDP1802_CLOCKS_PER_MCYCLE) / DESTROYER_FRAME_HZ)
#define DESTROYER_PAL_LINES 312u
/* Lines-from-end constants matching emma02's nonDisplayPeriodEndLine_/preDisplayPeriodLine_ */
#define DESTROYER_PREDISPLAY_LINE     269u   /* EF1 asserts 1 line before display */
#define DESTROYER_NONDISPLAY_END_LINE 268u   /* non-display ends; active display begins */
#define DESTROYER_ROTATED_W           CDP1869_VISIBLE_H
#define DESTROYER_ROTATED_H           CDP1869_VISIBLE_W

static uint32_t destroyer_palette[72] = {
    0xFF000000u, 0xFF00FF00u, 0xFF0000FFu, 0xFF00FFFFu,
    0xFFFF0000u, 0xFFFFFF00u, 0xFFFF00FFu, 0xFFFFFFFFu,
};

typedef struct {
    const char *value;
    const char *desc;
    uint8_t bits;
} DestroyerDipOption;

typedef struct {
    const char *name;
    const char *desc;
    uint8_t mask;
    const DestroyerDipOption *options;
    size_t n_options;
} DestroyerDip;

static const DestroyerDipOption destroyer_difficulty_opts[] = {
    {"very-hard", "Very hard", 0x00},
    {"hard",      "Hard",      0x01},
    {"easy",      "Easy",      0x02},
    {"very-easy", "Very easy", 0x03},
};

static const DestroyerDipOption destroyer_bonus_opts[] = {
    {"5000",  "5000",  0x0c},
    {"7000",  "7000",  0x08},
    {"10000", "10000", 0x04},
    {"14000", "14000", 0x00},
};

static const DestroyerDipOption destroyer_lives_opts[] = {
    {"1", "1", 0x30},
    {"2", "2", 0x20},
    {"3", "3", 0x10},
    {"4", "4", 0x00},
};

static const DestroyerDipOption destroyer_coinage_opts[] = {
    {"a1-b2",     "Slot A: 1, Slot B: 2",     0xc0},
    {"a1.5-b3",   "Slot A: 1.5, Slot B: 3",   0x80},
    {"a2-b4",     "Slot A: 2, Slot B: 4",     0x40},
    {"a2.5-b5",   "Slot A: 2.5, Slot B: 5",   0x00},
};

static const DestroyerDip destroyer_dips[] = {
    {"difficulty", "Difficulty", 0x03, destroyer_difficulty_opts,
     sizeof(destroyer_difficulty_opts) / sizeof(destroyer_difficulty_opts[0])},
    {"bonus", "Bonus life", 0x0c, destroyer_bonus_opts,
     sizeof(destroyer_bonus_opts) / sizeof(destroyer_bonus_opts[0])},
    {"lives", "Lives", 0x30, destroyer_lives_opts,
     sizeof(destroyer_lives_opts) / sizeof(destroyer_lives_opts[0])},
    {"coinage", "Coinage", 0xc0, destroyer_coinage_opts,
     sizeof(destroyer_coinage_opts) / sizeof(destroyer_coinage_opts[0])},
};

static void destroyer_mem_write(uint16_t addr, uint8_t val, void *ud) {
    RcaDestroyerState *s = ud;
    if (addr <= 0x1fff)
        return;
    if (addr >= 0x2000 && addr <= 0x20ff) {
        s->mem[addr] = val;
        return;
    }
    if (addr >= 0xf400 && addr <= 0xf7ff) {
        cdp1869_char_write(s->vis, (uint16_t)(addr - 0xf400), val);
        return;
    }
    if (addr >= 0xf800) {
        cdp1869_page_write(s->vis, (uint16_t)(addr - 0xf800), val);
        return;
    }
    s->mem[addr] = val;
}

static uint8_t destroyer_io_in(uint8_t port, void *ud) {
    RcaDestroyerState *s = ud;
    static int debug = -1;
    static uint8_t last_in0 = 0;
    static uint8_t last_in1 = 0;

    if (debug < 0)
        debug = getenv("GEMU_DESTROYER_DEBUG") != NULL;

    if (port == 1) {
        uint8_t v = (uint8_t)(s->in0 | (!s->vis->pcb ? 0x80u : 0x00u));
        if (debug && v != last_in0) {
            fprintf(stderr,
                    "destroyer port1: %02x pcb=%u start1=%u start2=%u left=%u right=%u fire=%u\n",
                    v, s->vis->pcb, s->start1, s->start2, s->left, s->right, s->fire);
            last_in0 = v;
        }
        return v;
    }
    if (port == 2) {
        if (debug && s->in1 != last_in1) {
            fprintf(stderr, "destroyer port2: %02x\n", s->in1);
            last_in1 = s->in1;
        }
        return s->in1;
    }
    return 0xff;
}

/* CDP1869 dot clock: 5.7143 MHz for Destroyer (PAL) */
#define DESTROYER_DOT_HZ 5714300u

static GemuDevice *destroyer_tone(RcaDestroyerState *s);
static void destroyer_update_sound(RcaDestroyerState *s) {
    GemuDevice *tone = destroyer_tone(s);
    if (!tone) return;
    const Cdp1869 *vis = s->vis;
    if (vis->tone_off || vis->tone_amp == 0) {
        gemu_tone_set_gate(tone, false);
        return;
    }
    /* f = (dot_hz / 2) / (512 >> tone_freq_sel) / (tone_div + 1) */
    unsigned prescaler = 512u >> vis->tone_freq_sel;
    unsigned freq = (DESTROYER_DOT_HZ / 2u) / prescaler / ((unsigned)vis->tone_div + 1u);
    if (freq < 20u)    freq = 20u;
    if (freq > 20000u) freq = 20000u;
    gemu_tone_set_freq(tone, freq);
    gemu_tone_set_gate(tone, true);
}

static void destroyer_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaDestroyerState *s = ud;
    if (port >= 3 && port <= 7) {
        cdp1869_out(s->vis, port, s->cpu->memory_addr, val);
        if (port == 4)
            destroyer_update_sound(s);
    }
}

static void destroyer_q_out(uint8_t q, void *ud) {
    RcaDestroyerState *s = ud;
    s->vis->q = q & 1u;
}

static uint32_t cdp1869_rgb(int color, int luma) {
    int y = 0;
    y += (luma & 4) ? 30 : 0;
    y += (luma & 1) ? 59 : 0;
    y += (luma & 2) ? 11 : 0;
    y = (y * 255) / 100;

    uint32_t r = (color & 4) ? (uint32_t)y : 0;
    uint32_t g = (color & 1) ? (uint32_t)y : 0;
    uint32_t b = (color & 2) ? (uint32_t)y : 0;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

static void destroyer_init_palette(void) {
    static bool init = false;
    if (init)
        return;

    for (int i = 0; i < 8; i++)
        destroyer_palette[i] = cdp1869_rgb(i, 15);
    for (int c = 0, i = 8; c < 8; c++)
        for (int l = 0; l < 8; l++, i++)
            destroyer_palette[i] = cdp1869_rgb(c, l);
    init = true;
}

static const DestroyerDip *destroyer_find_dip(const char *name) {
    for (size_t i = 0; i < sizeof(destroyer_dips) / sizeof(destroyer_dips[0]); i++)
        if (strcasecmp(name, destroyer_dips[i].name) == 0)
            return &destroyer_dips[i];
    return NULL;
}

static const DestroyerDipOption *destroyer_current_dip_option(
    const RcaDestroyerState *s, const DestroyerDip *dip) {
    uint8_t bits = s->in1 & dip->mask;
    for (size_t i = 0; i < dip->n_options; i++)
        if (dip->options[i].bits == bits)
            return &dip->options[i];
    return NULL;
}

static void destroyer_list_dips(const RcaDestroyerState *s) {
    GemuMonitor *mon = s->monitor;
    gemu_monitor_printf(mon, "Destroyer DIP switches:\n");
    for (size_t i = 0; i < sizeof(destroyer_dips) / sizeof(destroyer_dips[0]); i++) {
        const DestroyerDip *dip = &destroyer_dips[i];
        const DestroyerDipOption *cur = destroyer_current_dip_option(s, dip);
        gemu_monitor_printf(mon, "  %-10s %-12s current: %s\n", dip->name, dip->desc,
               cur ? cur->value : "unknown");
        gemu_monitor_printf(mon, "             values:");
        for (size_t j = 0; j < dip->n_options; j++)
            gemu_monitor_printf(mon, " %s", dip->options[j].value);
        gemu_monitor_printf(mon, "\n");
    }
}

static bool destroyer_parse_raw_dip_value(const char *text, uint8_t mask,
                                          uint8_t *bits) {
    if (!text || !*text)
        return false;
    char *end = NULL;
    unsigned long v = strtoul(text, &end, 0);
    if (!end || *end != '\0' || v > 0xffu)
        return false;
    *bits = (uint8_t)v & mask;
    return true;
}

static bool destroyer_set_dip(RcaDestroyerState *s, const char *name,
                              const char *value) {
    const DestroyerDip *dip = destroyer_find_dip(name);
    if (!dip) {
        gemu_monitor_printf(s->monitor, "unknown DIP switch '%s' (try 'dipswitch list')\n", name);
        return true;
    }

    uint8_t bits = 0;
    bool found = false;
    for (size_t i = 0; i < dip->n_options; i++) {
        if (strcasecmp(value, dip->options[i].value) == 0) {
            bits = dip->options[i].bits;
            found = true;
            break;
        }
    }
    if (!found)
        found = destroyer_parse_raw_dip_value(value, dip->mask, &bits);
    if (!found) {
        gemu_monitor_printf(s->monitor, "invalid value '%s' for DIP '%s' (try 'dipswitch list')\n",
               value, name);
        return true;
    }

    s->in1 = (uint8_t)((s->in1 & ~dip->mask) | (bits & dip->mask));
    const DestroyerDipOption *cur = destroyer_current_dip_option(s, dip);
    gemu_monitor_printf(s->monitor, "DIP %s = %s\n", dip->name, cur ? cur->value : value);
    return true;
}

static GemuMonCmd destroyer_cmd_dipswitch(GemuMonitor *mon, const char *args,
                                          void *ud) {
    RcaDestroyerState *s = ud;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", args);

    char *name = strtok(buf, " \t");
    char *value = strtok(NULL, " \t");
    char *extra = strtok(NULL, " \t");

    if (!name || strcasecmp(name, "list") == 0) {
        destroyer_list_dips(s);
        return GEMU_MON_NONE;
    }
    if (!value || extra) {
        gemu_monitor_printf(mon, "usage: dipswitch list | dipswitch <name> <value>\n");
        return GEMU_MON_NONE;
    }
    destroyer_set_dip(s, name, value);
    return GEMU_MON_NONE;
}

/* Monitor memory view: side-effect-free.  Character RAM reads through the
 * CDP1869 latch the PCB bit, so that window is not inspectable. */
static bool destroyer_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    RcaDestroyerState *s = ud;
    if (addr > 0xffffu) return false;
    if (addr >= 0xf400 && addr <= 0xf7ff) return false;
    if (addr >= 0xf800) {
        *val = cdp1869_page_read(s->vis, (uint16_t)(addr - 0xf800));
        return true;
    }
    *val = s->mem[addr];
    return true;
}

static bool destroyer_mon_write(void *ud, uint64_t addr, uint8_t val) {
    RcaDestroyerState *s = ud;
    if (addr > 0xffffu || addr <= 0x1fffu) return false;   /* ROM */
    destroyer_mem_write((uint16_t)addr, val, s);
    return true;
}

static void destroyer_mon_cpu_state(void *ud, char *buf, size_t len) {
    const RcaDestroyerState *s = ud;
    cdp1802_format_state(s->cpu, buf, len);
}

/*
 * Return the cycle (counting from frame start) at which the active display ends.
 * Accounts for 2× pixel scaling (fresvert/freshorz): the CDP1869 doubles each
 * character row/column when those flags are clear, halving the number of unique
 * scan lines that need to be rendered and therefore the display window in cycles.
 */
static unsigned destroyer_display_end_cycle(const Cdp1869 *vis) {
    unsigned pixel_w = vis->freshorz ? 1u : 2u;
    unsigned pixel_h = vis->fresvert ? 1u : 2u;
    unsigned non_display_end =
        (DESTROYER_MCYCLES_PER_FRAME * DESTROYER_NONDISPLAY_END_LINE) /
        DESTROYER_PAL_LINES;
    unsigned display_period =
        (DESTROYER_MCYCLES_PER_FRAME * CDP1869_VISIBLE_H) /
        DESTROYER_PAL_LINES;
    return non_display_end - (display_period / (pixel_w * pixel_h));
}

static void destroyer_video_timing(RcaDestroyerState *s, unsigned frame_cycle) {
    /*
     * Emma02 models VIS1870 timing with a countdown from cycleSize_ to 0.
     * Convert that coordinate to our frame_cycle value by subtracting each
     * VIS threshold from the frame length once.
     */
    unsigned predisplay =
        DESTROYER_MCYCLES_PER_FRAME -
        ((DESTROYER_MCYCLES_PER_FRAME * DESTROYER_PREDISPLAY_LINE) /
         DESTROYER_PAL_LINES);
    unsigned non_display_end =
        DESTROYER_MCYCLES_PER_FRAME -
        ((DESTROYER_MCYCLES_PER_FRAME * DESTROYER_NONDISPLAY_END_LINE) /
         DESTROYER_PAL_LINES);
    unsigned display_end = DESTROYER_MCYCLES_PER_FRAME -
                           destroyer_display_end_cycle(s->vis);

    /* non_display: true during VBlank so CPU can access page/char RAM */
    s->vis->non_display = frame_cycle < non_display_end ||
                         frame_cycle >= display_end ||
                         s->vis->dispoff;

    /* EF1 (active-low) goes asserted one line before display starts */
    s->cpu->EF[0] = !(frame_cycle >= predisplay && frame_cycle < display_end &&
                     !s->vis->dispoff);

    /* IRQ fires at the START of VBlank (= end of active display) */
    if (frame_cycle == display_end && !s->vis->dispoff)
        cdp1802_request_irq(s->cpu);
}

static void destroyer_rotate_bitmap(RcaDestroyerState *s) {
    for (int y = 0; y < CDP1869_VISIBLE_H; y++) {
        for (int x = 0; x < CDP1869_VISIBLE_W; x++) {
            int dx = DESTROYER_ROTATED_W - 1 - y;
            int dy = x;
            s->rotated_bitmap[dy * DESTROYER_ROTATED_W + dx] =
                s->vis->bitmap[y * CDP1869_VISIBLE_W + x];
        }
    }
}

static void destroyer_sync_inputs(RcaDestroyerState *s) {
    s->in0 = 0x7f; /* bit 7 is the active-high CDP1869 PCB callback */
    if (s->start1) s->in0 &= (uint8_t)~0x02u;
    if (s->start2) s->in0 &= (uint8_t)~0x04u;
    if (s->right)  s->in0 &= (uint8_t)~0x08u;
    if (s->left)   s->in0 &= (uint8_t)~0x10u;
    if (s->fire)   s->in0 &= (uint8_t)~0x20u;

    s->cpu->EF[1] = !s->service;
    s->cpu->EF[2] = !(s->coin2 || s->coin2_latch > 0);
    s->cpu->EF[3] = !(s->coin1 || s->coin1_latch > 0);
}

static void destroyer_default_inputs(RcaDestroyerState *s) {
    s->in0 = 0x7f;
    s->in1 = 0xd6;             /* easy, 10000 bonus, 3 lives, default coinage */
    s->service = false;
    s->coin1 = false;
    s->coin2 = false;
    s->start1 = false;
    s->start2 = false;
    s->left = false;
    s->right = false;
    s->fire = false;
    s->coin1_latch = 0;
    s->coin2_latch = 0;
}

static bool destroyer_screendump(void *ud, const char *path) {
    RcaDestroyerState *s = ud;
    bool dirty = s->vis->dirty;
    cdp1869_render(s->vis);
    s->vis->dirty = dirty;
    destroyer_rotate_bitmap(s);
    const uint8_t *buf = (s->cfg->vga == RCA_VGA_CDP1869)
                         ? s->rotated_bitmap : s->vis->bitmap;
    /* the turned picture is 216x240 (HEAD wrote it as 240x216) */
    bool turned = s->cfg->vga == RCA_VGA_CDP1869;
    return gemu_screendump_mono(path, buf, turned ? DESTROYER_ROTATED_W : CDP1869_VISIBLE_W,
                                turned ? DESTROYER_ROTATED_H : CDP1869_VISIBLE_H);
}

/* ── The core machine (stage R5) ─────────────────────────────────────────────
 * board (destroyer-board): the RAM, the inputs and DIP switches, the coin
 * latches, the beam (frame_cycle); cpu (cdp1802): the machine's executor at
 * 447,350 machine cycles a second (destroyer_cycle: the VIS's timing for
 * the cycle - non-display, EF1, the interrupt at the start of VBlank - then
 * the CPU's cycle); vis (cdp1869); tone (with -soundhw pcspk): the VIS's
 * tone generator, its pitch from OUT 4.
 * The bus, "program": the ROM at $0000-$1FFF (writes dropped), RAM from
 * $2000, the VIS's character RAM ($F400) and page RAM ($F800).  The board's
 * "screen" is the VIS's 240x216 bitmap turned a quarter (the cabinet's
 * monitor is on its side): 216x240.
 * Before stage R the beam was the run loop's loop variable, restarted each
 * frame, and step didn't clock it; now both run and step go through
 * destroyer_cycle and a stopped frame carries on where it stopped. */
typedef struct RcaDestroyerCore {
    GemuMachine   *machine;
    GemuDevice    *board, *cpu, *vis, *tone;
    GemuSpace     *bus;
    GemuSpaceCache cache;
    GemuScreen    *screen;
    GemuAudioOut  *audio;
} RcaDestroyerCore;

#define DESTROYER_MCYCLE_HZ (DESTROYER_MCYCLES_PER_FRAME * DESTROYER_FRAME_HZ)

static GemuDevice *destroyer_tone(RcaDestroyerState *s) {
    return s->core ? s->core->tone : NULL;
}

static uint8_t destroyer_bus_read(uint16_t addr, void *ud) {
    return gemu_space_cache_read8(&((RcaDestroyerState *)ud)->core->cache, addr);
}
static void destroyer_bus_write(uint16_t addr, uint8_t val, void *ud) {
    gemu_space_cache_write8(&((RcaDestroyerState *)ud)->core->cache, addr, val);
}
static uint8_t dvis_char_r(void *ud, uint64_t off) {
    return cdp1869_char_read(((RcaDestroyerState *)ud)->vis, (uint16_t)off);
}
static void dvis_char_w(void *ud, uint64_t off, uint8_t v) {
    cdp1869_char_write(((RcaDestroyerState *)ud)->vis, (uint16_t)off, v);
}
static uint8_t dvis_page_r(void *ud, uint64_t off) {
    return cdp1869_page_read(((RcaDestroyerState *)ud)->vis, (uint16_t)off);
}
static void dvis_page_w(void *ud, uint64_t off, uint8_t v) {
    cdp1869_page_write(((RcaDestroyerState *)ud)->vis, (uint16_t)off, v);
}
static const GemuHandlers dvis_char_handlers = { .r8 = dvis_char_r, .w8 = dvis_char_w };
static const GemuHandlers dvis_page_handlers = { .r8 = dvis_page_r, .w8 = dvis_page_w };

/* (Unmapped writes are dropped: the ROM's mapping is read-only.) */
static bool destroyer_bus_build(RcaDestroyerState *s) {
    RcaDestroyerCore *c = s->core;
    GemuSpace *b = c->bus = gemu_space_new(c->machine, "program", 16, 8, GEMU_BIG_ENDIAN);
    if (!b) return false;
    c->cache.space = b;
    return gemu_space_map_ram(b, 0x0000, 0x2000, s->mem, GEMU_MAP_READONLY, 0) &&
           gemu_space_map_ram(b, 0x2000, 0xF400 - 0x2000, s->mem + 0x2000, 0, 0) &&
           gemu_space_map(b, 0xF400, 0x400, &dvis_char_handlers, s, 0, 0) &&
           gemu_space_map(b, 0xF800, 0x800, &dvis_page_handlers, s, 0, 0);
}

static void destroyer_board_describe(GemuDevice *d, char *buf, size_t len) {
    const RcaDestroyerState *s = *(RcaDestroyerState **)d->priv;
    snprintf(buf, len, "Destroyer; inputs %02X, DIP switches %02X; coin latches %d/%d; "
             "frame cycle %u of %u", s->in0, s->in1, s->coin1_latch, s->coin2_latch,
             s->frame_cycle, (unsigned)DESTROYER_MCYCLES_PER_FRAME);
}

static const GemuDeviceType destroyer_board_type = {
    .name = "destroyer-board", .desc = "Destroyer board (RAM, inputs, DIP switches)",
    .size = sizeof(RcaDestroyerState *), .describe = destroyer_board_describe,
};

#define B(field) ok &= GEMU_STATE_MEMBER(d, "", s, field)
/* The board's state: the memory (the ROM part is loaded again anyway), the
 * DIP switches, the coin latches, the beam.  Not saved: the inputs held
 * (the host's). */
static bool destroyer_board_state(RcaDestroyerState *s, GemuDevice *d) {
    bool ok = true;
    B(mem); B(in1); B(coin1_latch); B(coin2_latch); B(frame_cycle);
    return ok;
}
#undef B

/* One machine cycle on the beam. */
static void destroyer_cycle(RcaDestroyerState *s) {
    destroyer_video_timing(s, s->frame_cycle);
    cdp1802_step(s->cpu);
    if (++s->frame_cycle == DESTROYER_MCYCLES_PER_FRAME) s->frame_cycle = 0;
}
static bool destroyer_stop_before(void *ud) {
    RcaDestroyerState *s = ud;
    return gemu_monitor_check_exec(s->monitor, s->cpu->R[s->cpu->P]);
}
static void destroyer_hook_cycle(void *ud) { destroyer_cycle(ud); }
static bool destroyer_stop_after(void *ud) {
    return gemu_monitor_is_paused(((RcaDestroyerState *)ud)->monitor);
}

/* The VIS's settings on this board (cdp1869_reset puts some back). */
static void destroyer_vis_setup(RcaDestroyerState *s) {
    cdp1869_set_page_ram_mask(s->vis, 0x03ffu);
    cdp1869_set_char_stride(s->vis, 8u);
    cdp1869_set_block_cpu_access(s->vis, false);
}

static void destroyer_screen_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    RcaDestroyerState *s = ud;
    cdp1869_render(s->vis);
    destroyer_rotate_bitmap(s);
    for (int y = clip->y0; y < clip->y1; y++)
        for (int x = 0; x < DESTROYER_ROTATED_W; x++)
            bmp->pix[(size_t)y * (size_t)bmp->stride + (size_t)x] =
                destroyer_palette[s->rotated_bitmap[y * DESTROYER_ROTATED_W + x]];
}

static void destroyer_sound_out(void *ud, const float *samples, size_t frames) {
    gemu_audio_queue(ud, samples, frames);
}

static bool destroyer_core_fail(RcaDestroyerCore *c) {
    fprintf(stderr, "gemu-rca: destroyer: core machine: %s\n", gemu_machine_error(c->machine)[0]
            ? gemu_machine_error(c->machine) : "a device couldn't be added");
    return false;
}

static bool destroyer_core_build(RcaDestroyerState *s) {
    const RcaConfig *cfg = s->cfg;
    RcaDestroyerCore *c = s->core = calloc(1, sizeof *c);
    if (!c || !(c->machine = gemu_machine_new("destroyer")))
        return false;
    bool ok = (c->board = gemu_device_add(c->machine, &destroyer_board_type, "board", NULL, NULL)) &&
              (c->cpu = gemu_device_add(c->machine, &gemu_cdp1802_type, "cpu", NULL, NULL)) &&
              (c->vis = gemu_device_add(c->machine, &gemu_cdp1869_type, "vis", NULL, NULL));
    if (ok && cfg->sound_hw != RCA_SOUND_NONE)
        ok = (c->tone = gemu_device_add(c->machine, &gemu_tone_type, "tone", NULL, "freq=440"));
    if (ok) {
        *(RcaDestroyerState **)c->board->priv = s;
        ok = destroyer_board_state(s, c->board);
    }
    if (!ok || !gemu_machine_init(c->machine))
        return destroyer_core_fail(c);
    s->cpu = gemu_cdp1802(c->cpu);
    s->vis = gemu_cdp1869(c->vis);
    Cdp1802 *cpu = s->cpu;
    cpu->mem_read  = destroyer_bus_read;
    cpu->mem_write = destroyer_bus_write;
    cpu->io_in     = destroyer_io_in;
    cpu->io_out    = destroyer_io_out;
    cpu->q_out     = destroyer_q_out;
    cpu->io_ud     = s;
    destroyer_vis_setup(s);
    GemuCdp1802Hooks h = { .stop_before = destroyer_stop_before, .cycle = destroyer_hook_cycle,
                           .stop_after = destroyer_stop_after, .ud = s };
    ok = destroyer_bus_build(s) && gemu_cdp1802_exec(c->cpu, DESTROYER_MCYCLE_HZ, &h) &&
         (c->screen = gemu_screen_new(c->board, "screen")) &&
         gemu_screen_set_size(c->screen, DESTROYER_ROTATED_W, DESTROYER_ROTATED_H);
    if (ok) {
        gemu_screen_set_update(c->screen, destroyer_screen_update, s);
        if (c->tone) c->audio = gemu_audio_open("destroyer", 44100);
        gemu_mixer_set_output(c->machine, 44100, 1, c->audio ? destroyer_sound_out : NULL, c->audio);
    }
    if (!ok || !gemu_machine_start(c->machine))
        return destroyer_core_fail(c);
    gemu_monitor_attach_machine(s->monitor, c->machine, "program");
    return true;
}

static void destroyer_core_free(RcaDestroyerState *s) {
    if (!s->core) return;
    gemu_machine_free(s->core->machine);
    gemu_audio_close(s->core->audio);
    free(s->core);
    s->core = NULL;
}

/* One frame: the machine run to the frame's end (from wherever the beam
 * is), then, at a frame's end, VBlank for the host's turn (page and
 * character RAM writable, EF1 off) and the coin latches count down; the
 * frame's sound out. */
static void destroyer_frame(RcaDestroyerState *s) {
    RcaDestroyerCore *c = s->core;
    uint64_t left = DESTROYER_MCYCLES_PER_FRAME - s->frame_cycle;
    gemu_machine_run_until(c->machine,
                           gemu_machine_time(c->machine) + gemu_clocks(DESTROYER_MCYCLE_HZ, left));
    if (s->frame_cycle == 0) {
        s->vis->non_display = true;
        s->cpu->EF[0] = true;
        if (s->coin1_latch > 0) s->coin1_latch--;
        if (s->coin2_latch > 0) s->coin2_latch--;
        gemu_screen_frame_begin(c->screen);
    }
    gemu_mixer_run(c->machine);
}

RcaDestroyerState *rca_destroyer_create(const RcaConfig *cfg) {
    RcaDestroyerState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg = cfg;
    destroyer_init_palette();
    destroyer_default_inputs(s);

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_screendump_cb(s->monitor, destroyer_screendump, s);
    gemu_monitor_bind(s->monitor, "dipswitch", destroyer_cmd_dipswitch, s);
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = destroyer_mon_read, .write = destroyer_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = true });
    gemu_monitor_set_cpu_state_cb(s->monitor, destroyer_mon_cpu_state, s);

    GemuMemory tmp = {.data = s->mem, .size = DESTRYER_MEM_SIZE};
    for (int i = 0; i < cfg->n_roms; i++) {
        size_t len = 0;
        if (!gemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, &len)) {
            fprintf(stderr, "gemu-rca: failed to load '%s'\n", cfg->roms[i].path);
            rca_destroyer_destroy(s);
            return NULL;
        }
        printf("gemu-rca: %zu bytes @ 0x%04X  <- %s\n",
               len, cfg->roms[i].addr, cfg->roms[i].path);
        gemu_monitor_register_rom(s->monitor, cfg->roms[i].addr, (uint32_t)len,
                                  cfg->roms[i].path);
    }
    if (!destroyer_core_build(s)) {
        rca_destroyer_destroy(s);
        return NULL;
    }
    destroyer_sync_inputs(s);
    return s;
}

/* The DIP switches are kept; HEAD's reset put them back to the defaults
 * (and left the VIS blocking the CPU during the display). */
static void rca_destroyer_reset(RcaDestroyerState *s, const RcaConfig *cfg) {
    memset(s->mem + 0x2000, 0, 0x100);
    cdp1802_reset(s->cpu);
    cdp1869_reset(s->vis);
    destroyer_vis_setup(s);
    if (destroyer_tone(s)) gemu_tone_set_gate(destroyer_tone(s), false);
    uint8_t dips = s->in1;
    destroyer_default_inputs(s);
    s->in1 = dips;
    s->frame_cycle = 0;
    destroyer_sync_inputs(s);

    GemuMemory tmp = {.data = s->mem, .size = DESTRYER_MEM_SIZE};
    for (int i = 0; i < cfg->n_roms; i++)
        gemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, NULL);
}

void rca_destroyer_destroy(RcaDestroyerState *s) {
    if (!s) return;
    destroyer_core_free(s);
    gemu_monitor_destroy(s->monitor);
    free(s);
}

static void destroyer_apply_held(RcaDestroyerState *s, uint32_t held) {
    static int debug = -1;
    if (debug < 0) debug = getenv("GEMU_INPUT_DEBUG") != NULL;

    bool old_coin1 = s->coin1;
    bool old_coin2 = s->coin2;
    s->start1  = (held >> 0) & 1;
    s->start2  = (held >> 1) & 1;
    s->left    = (held >> 2) & 1;
    s->right   = (held >> 3) & 1;
    s->fire    = (held >> 4) & 1;
    s->service = (held >> 5) & 1;
    s->coin1   = (held >> 6) & 1;
    s->coin2   = (held >> 7) & 1;
    if (s->coin1 && !old_coin1) s->coin1_latch = 15;
    if (s->coin2 && !old_coin2) s->coin2_latch = 15;

    if (debug) {
        if (s->start1) fprintf(stderr, "destroyer input: start1 down\n");
        if (s->start2) fprintf(stderr, "destroyer input: start2 down\n");
        if (s->left)   fprintf(stderr, "destroyer input: left down\n");
        if (s->right)  fprintf(stderr, "destroyer input: right down\n");
        if (s->fire)   fprintf(stderr, "destroyer input: fire down\n");
        if (s->service) fprintf(stderr, "destroyer input: service down\n");
        if (s->coin1 && !old_coin1) fprintf(stderr, "destroyer input: coin1 down\n");
        if (s->coin2 && !old_coin2) fprintf(stderr, "destroyer input: coin2 down\n");
    }
}

/* The board's core screen (the turned VIS picture) to the window, when the
 * VIS has changed (running, or stepped from the monitor while paused). */
static void destroyer_present(RcaDestroyerState *s, const RcaConfig *cfg,
                              GemuDisplay *display) {
    if (!display) return;
    if (cfg->vga == RCA_VGA_CDP1869) {
        if (!s->vis->dirty) return;
        const GemuBitmap *b = gemu_screen_render(s->core->screen);
        gemu_display_render(display, b->pix, b->width, b->height);
    } else if (cfg->vga == RCA_VGA_CDP1861) {
        static uint32_t black[CDP1861_DISPLAY_W * CDP1861_DISPLAY_H];
        gemu_display_render(display, black, CDP1861_DISPLAY_W, CDP1861_DISPLAY_H);
    }
}

void rca_destroyer_run(RcaDestroyerState *s, const RcaConfig *cfg) {
    int fw = (cfg->vga == RCA_VGA_CDP1869) ? DESTROYER_ROTATED_W : CDP1861_DISPLAY_W;
    int fh = (cfg->vga == RCA_VGA_CDP1869) ? DESTROYER_ROTATED_H : CDP1861_DISPLAY_H;

    GemuDisplay *display = NULL;
    if (cfg->vga != RCA_VGA_NONE && gemu_display_wanted(cfg->display_type)) {
        display = gemu_display_create(cfg->display_type,
            &(GemuDisplayConfig){
                .fb_width    = fw,
                .fb_height   = fh,
                .scale       = cfg->display_scale,
                .renderer    = GEMU_RENDERER_AUTO,
                .actions     = destroyer_actions,
                .n_actions   = DESTROYER_N_ACTIONS,
                .ini_section = "destroyer",
                .gtk         = &(GemuDisplayGtkExtras){ .monitor = s->monitor },
            });
        if (!display) {
            fprintf(stderr, "gemu-rca: failed to create Destroyer display\n");
            return;
        }
        gemu_monitor_set_input_reset_cb(s->monitor,
                                        gemu_display_reset_input_bindings_ud,
                                        display);
    }
    gemu_monitor_start(s->monitor);

    const unsigned frame_ms = 1000u / DESTROYER_FRAME_HZ;
    bool quit = false;

    while (!quit) {
        /* Input */
        uint32_t held = 0;
        if (display) {
            held = gemu_display_poll(display);
            if (gemu_display_should_quit(display)) break;
            if (gemu_display_reset_requested(display)) {
                gemu_display_clear_flags(display);
                rca_destroyer_reset(s, cfg);
            }
            /* A/B are alternate coin keys alongside the 5/6 bindings. */
            if (gemu_display_is_key_held(display, "a")) held |= GEMU_ACTION(6);
            if (gemu_display_is_key_held(display, "b")) held |= GEMU_ACTION(7);
        }
        destroyer_apply_held(s, held);
        destroyer_sync_inputs(s);

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) { if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true); else { quit = true; break; } }
            else if (cmd == GEMU_MON_RESET) rca_destroyer_reset(s, cfg);
            else if (cmd == GEMU_MON_STEP) {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                for (uint32_t k = 0; k < n; k++) {
                    /* a machine cycle at a time to the instruction's end
                     * (bounded, in case of IDL) */
                    uint64_t start = s->cpu->insn_count;
                    for (int i = 0; i < 1024 && s->cpu->insn_count == start; i++)
                        destroyer_cycle(s);
                }
            }
        }
        if (quit) break;
        gemu_display_set_paused(display, gemu_monitor_is_paused(s->monitor));

        bool paused = gemu_monitor_is_paused(s->monitor);
        if (!paused) destroyer_frame(s);       /* the cpu device's executor */
        destroyer_present(s, cfg, display);    /* (paused: shows monitor steps) */

        /* Pace to the machine's time; paused, a frame's delay. */
        if (!paused) {
            gemu_machine_pace(s->core->machine, 0);
        } else {
            gemu_machine_pace_reset(s->core->machine);
            gemu_sleep_ms(frame_ms);
        }
    }

    printf("gemu-rca: %llu machine cycles, %llu instructions\n",
           (unsigned long long)s->cpu->cycle_count,
           (unsigned long long)s->cpu->insn_count);
    gemu_monitor_stop(s->monitor);
    gemu_display_destroy(display);
}
