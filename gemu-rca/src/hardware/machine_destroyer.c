#include "destroyer.h"
#include "devices/pcspk.h"
#include "gemu/memory.h"
#include "gemu/screendump.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

static uint8_t destroyer_mem_read(uint16_t addr, void *ud) {
    RcaDestroyerState *s = ud;
    if (addr >= 0xf400 && addr <= 0xf7ff)
        return cdp1869_char_read(&s->vis, (uint16_t)(addr - 0xf400));
    if (addr >= 0xf800)
        return cdp1869_page_read(&s->vis, (uint16_t)(addr - 0xf800));
    return s->mem[addr];
}

static void destroyer_mem_write(uint16_t addr, uint8_t val, void *ud) {
    RcaDestroyerState *s = ud;
    if (addr <= 0x1fff)
        return;
    if (addr >= 0x2000 && addr <= 0x20ff) {
        s->mem[addr] = val;
        return;
    }
    if (addr >= 0xf400 && addr <= 0xf7ff) {
        cdp1869_char_write(&s->vis, (uint16_t)(addr - 0xf400), val);
        return;
    }
    if (addr >= 0xf800) {
        cdp1869_page_write(&s->vis, (uint16_t)(addr - 0xf800), val);
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
        uint8_t v = (uint8_t)(s->in0 | (!s->vis.pcb ? 0x80u : 0x00u));
        if (debug && v != last_in0) {
            fprintf(stderr,
                    "destroyer port1: %02x pcb=%u start1=%u start2=%u left=%u right=%u fire=%u\n",
                    v, s->vis.pcb, s->start1, s->start2, s->left, s->right, s->fire);
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

static void destroyer_update_sound(RcaDestroyerState *s) {
    if (!s->speaker) return;
    const Cdp1869 *vis = &s->vis;
    if (vis->tone_off || vis->tone_amp == 0) {
        rca_pcspk_set_gate(s->speaker, 0);
        return;
    }
    /* f = (dot_hz / 2) / (512 >> tone_freq_sel) / (tone_div + 1) */
    unsigned prescaler = 512u >> vis->tone_freq_sel;
    unsigned freq = (DESTROYER_DOT_HZ / 2u) / prescaler / ((unsigned)vis->tone_div + 1u);
    if (freq < 20u)    freq = 20u;
    if (freq > 20000u) freq = 20000u;
    rca_pcspk_set_freq(s->speaker, freq);
    rca_pcspk_set_gate(s->speaker, 1);
}

static void destroyer_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaDestroyerState *s = ud;
    if (port >= 3 && port <= 7) {
        cdp1869_out(&s->vis, port, s->cpu.memory_addr, val);
        if (port == 4)
            destroyer_update_sound(s);
    }
}

static void destroyer_q_out(uint8_t q, void *ud) {
    RcaDestroyerState *s = ud;
    s->vis.q = q & 1u;
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
    printf("Destroyer DIP switches:\n");
    for (size_t i = 0; i < sizeof(destroyer_dips) / sizeof(destroyer_dips[0]); i++) {
        const DestroyerDip *dip = &destroyer_dips[i];
        const DestroyerDipOption *cur = destroyer_current_dip_option(s, dip);
        printf("  %-10s %-12s current: %s\n", dip->name, dip->desc,
               cur ? cur->value : "unknown");
        printf("             values:");
        for (size_t j = 0; j < dip->n_options; j++)
            printf(" %s", dip->options[j].value);
        printf("\n");
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
        printf("unknown DIP switch '%s' (try 'dipswitch list')\n", name);
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
        printf("invalid value '%s' for DIP '%s' (try 'dipswitch list')\n",
               value, name);
        return true;
    }

    s->in1 = (uint8_t)((s->in1 & ~dip->mask) | (bits & dip->mask));
    const DestroyerDipOption *cur = destroyer_current_dip_option(s, dip);
    printf("DIP %s = %s\n", dip->name, cur ? cur->value : value);
    return true;
}

static bool destroyer_monitor_command(RcaDestroyerState *s, const char *line) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", line);

    char *cmd = strtok(buf, " \t");
    if (!cmd)
        return true;
    if (strcasecmp(cmd, "dipswitch") != 0 && strcasecmp(cmd, "dip") != 0)
        return false;

    char *name = strtok(NULL, " \t");
    char *value = strtok(NULL, " \t");
    char *extra = strtok(NULL, " \t");

    if (!name || strcasecmp(name, "list") == 0) {
        destroyer_list_dips(s);
        return true;
    }
    if (!value || extra) {
        printf("usage: dipswitch list | dipswitch <name> <value>\n");
        return true;
    }
    return destroyer_set_dip(s, name, value);
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
                           destroyer_display_end_cycle(&s->vis);

    /* non_display: true during VBlank so CPU can access page/char RAM */
    s->vis.non_display = frame_cycle < non_display_end ||
                         frame_cycle >= display_end ||
                         s->vis.dispoff;

    /* EF1 (active-low) goes asserted one line before display starts */
    s->cpu.EF[0] = !(frame_cycle >= predisplay && frame_cycle < display_end &&
                     !s->vis.dispoff);

    /* IRQ fires at the START of VBlank (= end of active display) */
    if (frame_cycle == display_end && !s->vis.dispoff)
        cdp1802_request_irq(&s->cpu);
}

static void destroyer_rotate_bitmap(RcaDestroyerState *s) {
    for (int y = 0; y < CDP1869_VISIBLE_H; y++) {
        for (int x = 0; x < CDP1869_VISIBLE_W; x++) {
            int dx = DESTROYER_ROTATED_W - 1 - y;
            int dy = x;
            s->rotated_bitmap[dy * DESTROYER_ROTATED_W + dx] =
                s->vis.bitmap[y * CDP1869_VISIBLE_W + x];
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

    s->cpu.EF[1] = !s->service;
    s->cpu.EF[2] = !(s->coin2 || s->coin2_latch > 0);
    s->cpu.EF[3] = !(s->coin1 || s->coin1_latch > 0);
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
    const uint8_t *buf = (s->cfg->vga == RCA_VGA_CDP1869)
                         ? s->rotated_bitmap : s->vis.bitmap;
    int w = CDP1869_VISIBLE_W, h = CDP1869_VISIBLE_H;
    uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return false;
    for (int i = 0; i < w * h; i++) {
        uint8_t v = buf[i] ? 0xFF : 0x00;
        rgb[i*3+0] = v; rgb[i*3+1] = v; rgb[i*3+2] = v;
    }
    bool ok = gemu_screendump(path, rgb, w, h);
    free(rgb);
    return ok;
}

RcaDestroyerState *rca_destroyer_create(const RcaConfig *cfg) {
    RcaDestroyerState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg = cfg;
    destroyer_init_palette();
    cdp1802_init(&s->cpu, s->mem, DESTRYER_MEM_SIZE);
    s->cpu.io_in = destroyer_io_in;
    s->cpu.io_out = destroyer_io_out;
    s->cpu.mem_read = destroyer_mem_read;
    s->cpu.mem_write = destroyer_mem_write;
    s->cpu.q_out = destroyer_q_out;
    s->cpu.io_ud = s;
    cdp1869_init(&s->vis);
    cdp1869_set_page_ram_mask(&s->vis, 0x03ffu);
    cdp1869_set_char_stride(&s->vis, 8u);
    cdp1869_set_block_cpu_access(&s->vis, false);
    destroyer_default_inputs(s);
    destroyer_sync_inputs(s);

    if (cfg->sound_hw != RCA_SOUND_NONE)
        s->speaker = rca_pcspk_create(440u);

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_screendump_cb(s->monitor, destroyer_screendump, s);
    if (cfg->vnc_addr) {
        int w = (cfg->vga == RCA_VGA_CDP1869) ? DESTROYER_ROTATED_W : CDP1861_DISPLAY_W;
        int h = (cfg->vga == RCA_VGA_CDP1869) ? DESTROYER_ROTATED_H : CDP1861_DISPLAY_H;
        s->vnc = gemu_vnc_create(cfg->vnc_addr, w * cfg->display_scale,
                                 h * cfg->display_scale);
        if (cfg->vga == RCA_VGA_CDP1869)
            gemu_vnc_set_palette(s->vnc, destroyer_palette,
                                 (int)(sizeof(destroyer_palette) /
                                       sizeof(destroyer_palette[0])));
    }

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
    return s;
}

void rca_destroyer_reset(RcaDestroyerState *s, const RcaConfig *cfg) {
    memset(s->mem + 0x2000, 0, 0x100);
    cdp1802_reset(&s->cpu);
    cdp1869_reset(&s->vis);
    destroyer_default_inputs(s);
    destroyer_sync_inputs(s);

    GemuMemory tmp = {.data = s->mem, .size = DESTRYER_MEM_SIZE};
    for (int i = 0; i < cfg->n_roms; i++)
        gemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, NULL);
}

void rca_destroyer_destroy(RcaDestroyerState *s) {
    if (!s) return;
    rca_pcspk_destroy(s->speaker);
    gemu_monitor_destroy(s->monitor);
    gemu_vnc_destroy(s->vnc);
    free(s);
}

static void destroyer_poll_display(RcaDestroyerState *s, RcaDisplay *display,
                                   bool *quit) {
    static int debug = -1;
    if (debug < 0)
        debug = getenv("GEMU_INPUT_DEBUG") != NULL;

    rca_display_poll(display);
    if (rca_display_should_quit(display)) {
        *quit = true;
        return;
    }

    bool old_coin1 = s->coin1;
    bool old_coin2 = s->coin2;
    s->start1 = rca_display_key_down(display, '1');
    s->start2 = rca_display_key_down(display, '2');
    s->right = rca_display_key_down(display, RCA_KEY_RIGHT);
    s->left = rca_display_key_down(display, RCA_KEY_LEFT);
    s->fire = rca_display_key_down(display, ' ');
    s->service = rca_display_key_down(display, RCA_KEY_F2);
    s->coin1 = rca_display_key_down(display, '5') ||
               rca_display_key_down(display, 'a');
    s->coin2 = rca_display_key_down(display, '6') ||
               rca_display_key_down(display, 'b');
    if (s->coin1 && !old_coin1) s->coin1_latch = 15;
    if (s->coin2 && !old_coin2) s->coin2_latch = 15;

    if (debug) {
        if (s->start1) fprintf(stderr, "destroyer input: start1 down\n");
        if (s->start2) fprintf(stderr, "destroyer input: start2 down\n");
        if (s->left) fprintf(stderr, "destroyer input: left down\n");
        if (s->right) fprintf(stderr, "destroyer input: right down\n");
        if (s->fire) fprintf(stderr, "destroyer input: fire down\n");
        if (s->service) fprintf(stderr, "destroyer input: service down\n");
        if (s->coin1 && !old_coin1) fprintf(stderr, "destroyer input: coin1 down\n");
        if (s->coin2 && !old_coin2) fprintf(stderr, "destroyer input: coin2 down\n");
    }
}

static void destroyer_poll_vnc(RcaDestroyerState *s) {
    GemuVncKeyEvent ev;
    while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
        switch (ev.keysym) {
        case '1': s->start1 = ev.down; break;
        case '2': s->start2 = ev.down; break;
        case ' ': s->fire = ev.down; break;
        case '5':
        case 'a':
            if (ev.down) s->coin1_latch = 15;
            break;
        case '6':
        case 'b':
            if (ev.down) s->coin2_latch = 15;
            break;
        case 0xff51: s->left = ev.down; break;  /* Left */
        case 0xff53: s->right = ev.down; break; /* Right */
        default: break;
        }
    }
}

void rca_destroyer_run(RcaDestroyerState *s, const RcaConfig *cfg) {
    RcaDisplay *display = NULL;
    if (cfg->vga == RCA_VGA_CDP1869)
        display = rca_display_create_indexed(cfg->display_type, "GEMU",
                                             DESTROYER_ROTATED_W,
                                             DESTROYER_ROTATED_H,
                                             cfg->display_scale,
                                             destroyer_palette,
                                             (int)(sizeof(destroyer_palette) /
                                                   sizeof(destroyer_palette[0])),
                                             s->monitor);
    else if (cfg->vga == RCA_VGA_NONE)
        display = rca_display_none_create();
    else
        display = rca_display_create_mono(cfg->display_type, "GEMU",
                                          CDP1861_DISPLAY_W,
                                          CDP1861_DISPLAY_H,
                                          cfg->display_scale,
                                          0xFFFFFFFFu,
                                          0xFF100080u,
                                          s->monitor);
    if (!display) {
        fprintf(stderr, "gemu-rca: failed to create Destroyer display\n");
        return;
    }

    gemu_monitor_start(s->monitor);
    const Uint32 frame_ms = 1000 / DESTROYER_FRAME_HZ;
    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        if (cfg->display_type != GEMU_DISPLAY_NONE)
            destroyer_poll_display(s, display, &quit);
        destroyer_poll_vnc(s);
        destroyer_sync_inputs(s);

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) quit = true;
            else if (cmd == GEMU_MON_RESET) rca_destroyer_reset(s, cfg);
            else if (cmd == GEMU_MON_STEP) {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                for (uint32_t i = 0; i < n; i++) cdp1802_step(&s->cpu);
            }
            else if (cmd == GEMU_MON_CUSTOM) {
                if (!destroyer_monitor_command(s, gemu_monitor_command_text(s->monitor)))
                    gemu_monitor_unknown_command(s->monitor);
            }
        }
        if (quit) break;

        if (!gemu_monitor_is_paused(s->monitor)) {
            for (unsigned i = 0; i < DESTROYER_MCYCLES_PER_FRAME; i++) {
                destroyer_video_timing(s, i);
                cdp1802_step(&s->cpu);
            }

            s->vis.non_display = true;
            s->cpu.EF[0] = true;
            if (cfg->vga == RCA_VGA_CDP1869) {
                cdp1869_render(&s->vis);
                destroyer_rotate_bitmap(s);
                rca_display_render(display, s->rotated_bitmap,
                                   DESTROYER_ROTATED_W, DESTROYER_ROTATED_H);
                gemu_vnc_update(s->vnc, s->rotated_bitmap,
                                DESTROYER_ROTATED_W, DESTROYER_ROTATED_H);
            } else if (cfg->vga == RCA_VGA_CDP1861) {
                static const uint8_t blank[CDP1861_DISPLAY_W * CDP1861_DISPLAY_H];
                rca_display_render(display, blank,
                                   CDP1861_DISPLAY_W, CDP1861_DISPLAY_H);
                gemu_vnc_update(s->vnc, blank,
                                CDP1861_DISPLAY_W, CDP1861_DISPLAY_H);
            }
        }

        Uint32 elapsed = SDL_GetTicks() - t0;
        if (s->coin1_latch > 0) s->coin1_latch--;
        if (s->coin2_latch > 0) s->coin2_latch--;
        if (elapsed < frame_ms) SDL_Delay(frame_ms - elapsed);
    }

    printf("gemu-rca: %llu machine cycles, %llu instructions\n",
           (unsigned long long)s->cpu.cycle_count,
           (unsigned long long)s->cpu.insn_count);
    gemu_monitor_stop(s->monitor);
    rca_display_destroy(display);
}
