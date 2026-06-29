#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "pecom.h"
#include "gemu/gemu.h"
#include "gemu/gemu_display.h"
#include "gemu/memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#if defined(_WIN32)
#  include <windows.h>
#endif

static void pecom_sleep_ms(unsigned ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* ── Palette ─────────────────────────────────────────────────────────────── */

/*
 * CDP1869 8-color palette: bits [2]=R, [1]=B, [0]=G.
 * Pecom 32 BASIC uses white background (bkg=7) with black text (color=0).
 */
static uint32_t pecom_palette[8];

static void pecom_init_palette(void) {
    static bool done = false;
    if (done) return;
    static const uint32_t rgb[8] = {
        0xFF000000u, /* 0 = black   */
        0xFF00FF00u, /* 1 = green   */
        0xFF0000FFu, /* 2 = blue    */
        0xFF00FFFFu, /* 3 = cyan    */
        0xFFFF0000u, /* 4 = red     */
        0xFFFFFF00u, /* 5 = yellow  */
        0xFFFF00FFu, /* 6 = magenta */
        0xFFFFFFFFu, /* 7 = white   */
    };
    for (int i = 0; i < 8; i++)
        pecom_palette[i] = rgb[i];
    done = true;
}

/* ── Memory map ──────────────────────────────────────────────────────────── */

static uint8_t pecom_mem_read(uint16_t addr, void *ud) {
    RcaPecom32State *s = ud;

    /* ROM at 0x8000–0xBFFF */
    if (addr >= 0x8000u && addr < 0xC000u)
        return s->rom[addr - 0x8000u];

    /* Bootstrap: ROM mirrored at 0x0000–0x3FFF until first OUT 1 */
    if (s->boot_mirror && addr < 0x4000u)
        return s->rom[addr];

    /* VIS-1870 char RAM: 0xF400–0xF7FF */
    if (addr >= PECOM32_CRAM_BASE && addr < PECOM32_PRAM_BASE)
        return cdp1869_char_read(&s->vis, addr - PECOM32_CRAM_BASE);

    /* VIS-1870 page RAM: 0xF800–0xFFFF */
    if (addr >= PECOM32_PRAM_BASE)
        return cdp1869_page_read(&s->vis, addr - PECOM32_PRAM_BASE);

    /* 0xC000–0xF3FF: ROM chip 2 (Pecom 64) or RAM bank 2 (Pecom 32) */
    if (addr >= 0xC000u) {
        if (s->rom_size > PECOM32_ROM_SIZE)
            return s->rom[addr - 0x8000u];
        return s->ram2[addr - 0xC000u];
    }

    /* RAM bank 1: 0x0000–0x7FFF */
    return s->ram1[addr];
}

static void pecom_mem_write(uint16_t addr, uint8_t val, void *ud) {
    RcaPecom32State *s = ud;

    /* ROM and ROM mirror: read-only */
    if (addr >= 0x8000u && addr < 0xC000u) return;
    if (s->boot_mirror && addr < 0x4000u)  return;

    /* VIS-1870 char RAM: 0xF400–0xF7FF */
    if (addr >= PECOM32_CRAM_BASE && addr < PECOM32_PRAM_BASE) {
        cdp1869_char_write(&s->vis, addr - PECOM32_CRAM_BASE, val);
        return;
    }

    /* VIS-1870 page RAM: 0xF800–0xFFFF */
    if (addr >= PECOM32_PRAM_BASE) {
        cdp1869_page_write(&s->vis, addr - PECOM32_PRAM_BASE, val);
        return;
    }

    /* 0xC000–0xF3FF: read-only ROM2 (Pecom 64) or writable RAM2 (Pecom 32) */
    if (addr >= 0xC000u) {
        if (s->rom_size <= PECOM32_ROM_SIZE)
            s->ram2[addr - 0xC000u] = val;
        return;
    }

    /* RAM bank 1: 0x0000–0x7FFF */
    s->ram1[addr] = val;
}

/* ── I/O ─────────────────────────────────────────────────────────────────── */

/*
 * Pecom 32 keyboard matrix layout (from bare.xml):
 * IN 3 uses R[X] & 0x3F as the row address.
 * Each row has two keys at bit 0 and bit 1 (active-high: 1 = pressed).
 * Shift/Ctrl/Caps are handled via EF pins, not the matrix bits.
 */
static const struct {
    const char *name;  /* SDL key name (same vocabulary as GemuActionDef.default_key) */
    uint8_t     row;   /* matrix row: IN 3 address & 0x3F */
    uint8_t     bit;   /* 0 or 1 within that row */
} pecom_keymap[] = {
    /* Row 0x0A */
    {"Return",      0x0A, 0},
    {"Home",        0x0A, 1},
    /* Row 0x0B */
    {"End",         0x0B, 0},
    /* Row 0x0C: 0 1 */
    {"0",           0x0C, 0},
    {"1",           0x0C, 1},
    /* Row 0x0D: 2 3 */
    {"2",           0x0D, 0},
    {"3",           0x0D, 1},
    /* Row 0x0E: 4 5 */
    {"4",           0x0E, 0},
    {"5",           0x0E, 1},
    /* Row 0x0F: 6 7 */
    {"6",           0x0F, 0},
    {"7",           0x0F, 1},
    /* Row 0x10: 8 9 */
    {"8",           0x10, 0},
    {"9",           0x10, 1},
    /* Row 0x11: : ; */
    {":",           0x11, 0},
    {";",           0x11, 1},
    /* Row 0x12: , = */
    {",",           0x12, 0},
    {"=",           0x12, 1},
    /* Row 0x13: . / */
    {".",           0x13, 0},
    {"/",           0x13, 1},
    /* Row 0x14: space a */
    {"Space",       0x14, 0},
    {"a",           0x14, 1},
    /* Row 0x15: b c */
    {"b",           0x15, 0},
    {"c",           0x15, 1},
    /* Row 0x16: d e */
    {"d",           0x16, 0},
    {"e",           0x16, 1},
    /* Row 0x17: f g */
    {"f",           0x17, 0},
    {"g",           0x17, 1},
    /* Row 0x18: h i */
    {"h",           0x18, 0},
    {"i",           0x18, 1},
    /* Row 0x19: j k */
    {"j",           0x19, 0},
    {"k",           0x19, 1},
    /* Row 0x1A: l m */
    {"l",           0x1A, 0},
    {"m",           0x1A, 1},
    /* Row 0x1B: n o */
    {"n",           0x1B, 0},
    {"o",           0x1B, 1},
    /* Row 0x1C: p q */
    {"p",           0x1C, 0},
    {"q",           0x1C, 1},
    /* Row 0x1D: r s */
    {"r",           0x1D, 0},
    {"s",           0x1D, 1},
    /* Row 0x1E: t u */
    {"t",           0x1E, 0},
    {"u",           0x1E, 1},
    /* Row 0x1F: v w */
    {"v",           0x1F, 0},
    {"w",           0x1F, 1},
    /* Row 0x20: x y */
    {"x",           0x20, 0},
    {"y",           0x20, 1},
    /* Row 0x21: z down */
    {"z",           0x21, 0},
    {"Down",        0x21, 1},
    /* Row 0x22: left right */
    {"Left",        0x22, 0},
    {"Right",       0x22, 1},
    /* Row 0x23: up */
    {"Up",          0x23, 0},
};
#define PECOM_N_KEYS (sizeof(pecom_keymap) / sizeof(pecom_keymap[0]))

static uint8_t pecom_io_in(uint8_t port, void *ud) {
    RcaPecom32State *s = ud;

    if (s->iogroup == 0 && port == 3) {
        /* IN 3 in iogroup 0: matrix keyboard row read.
         * R[X] (the memory address register at IN time) selects the row
         * via the lower 6 bits.  Active-high: bit set = key pressed. */
        uint8_t row = (uint8_t)(s->cpu.R[s->cpu.X] & 0x3Fu);
        return s->keys[row];
    }
    return 0xFFu;
}

static void pecom_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaPecom32State *s = ud;

    if (port == 1) {
        /* OUT 1: iogroup selector (bit 1 = 1 → iogroup 2 / VIS-1870) */
        s->iogroup = (val & 0x02u) ? 2 : 0;
        s->boot_mirror = false;
        return;
    }

    /* Iogroup 0: no OUT ports used by the keyboard (matrix uses IN 3 address bus) */
    if (s->iogroup == 0)
        return;

    if (s->iogroup == 2 && port >= 3 && port <= 7) {
        cdp1869_out(&s->vis, port, s->cpu.memory_addr, val);
        return;
    }
}

static void pecom_q_out(uint8_t q, void *ud) {
    RcaPecom32State *s = ud;
    s->vis.q = q & 1u;
    rca_pcspk_set_gate(s->speaker, q);
}

/* ── Per-frame video timing ──────────────────────────────────────────────── */

static void pecom_video_timing(RcaPecom32State *s, unsigned frame_cycle) {
    /* Map CPU frame cycle → VIS-1870 scan line */
    unsigned vis_line =
        (frame_cycle * CDP1869_LINES_TOTAL) / PECOM32_MCYCLES_PER_FRAME;

    /* non_display: true during VBlank (lines 0–47 and 264–311) */
    bool nd = (vis_line < CDP1869_DISPLAY_START ||
               vis_line >= CDP1869_DISPLAY_END);
    s->vis.non_display = nd || s->vis.dispoff;

    /* EF1: open-drain shared between VIS-1870 display timing and CTRL key.
     * High only when NOT in active display AND CTRL is not pressed. */
    s->cpu.EF[0] = s->vis.non_display && !s->key_ctrl;

    /* EF2 = SHIFT key, active-low: 0 when pressed, 1 when released. */
    s->cpu.EF[1] = !s->key_shift;

    /* EF3 = CAPS LOCK (pol=rev): 1 = not locked, 0 = locked.
     * BN3 fires when EF3=1 (caps not active), routing to lowercase path. */
    s->cpu.EF[2] = !s->caps_locked;

    /* EF4 = ESC key, active-low. */
    s->cpu.EF[3] = !s->key_esc;

    /* Single interrupt per frame at "line 2" (start of VBlank) */
    unsigned int_cycle =
        (2u * PECOM32_MCYCLES_PER_FRAME) / CDP1869_LINES_TOTAL;
    if (frame_cycle == int_cycle)
        cdp1802_request_irq(&s->cpu);
}

/* ── Machine lifecycle ───────────────────────────────────────────────────── */

RcaPecom32State *rca_pecom32_create(const RcaConfig *cfg) {
    pecom_init_palette();

    RcaPecom32State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg         = cfg;
    s->boot_mirror = true;
    s->iogroup     = 0;

    memset(s->keys,       0, sizeof(s->keys));
    memset(s->keys_live,  0, sizeof(s->keys_live));
    memset(s->keys_latch, 0, sizeof(s->keys_latch));

    cdp1802_init(&s->cpu, NULL, 0);
    s->cpu.mem_read  = pecom_mem_read;
    s->cpu.mem_write = pecom_mem_write;
    s->cpu.io_in     = pecom_io_in;
    s->cpu.io_out    = pecom_io_out;
    s->cpu.q_out     = pecom_q_out;
    s->cpu.io_ud     = s;
    /* on_sync not used — timing driven by pecom_video_timing in the run loop */

    cdp1869_init(&s->vis);
    cdp1869_set_page_ram_mask(&s->vis, 0x3FFu);  /* 1 KB page RAM */
    cdp1869_set_char_stride(&s->vis, 16u);        /* 16 scan lines / char */
    cdp1869_set_block_cpu_access(&s->vis, true);  /* block during active display */

    s->monitor = gemu_monitor_create();

    if (cfg->sound_hw == RCA_SOUND_PCSPK && !cfg->vnc_addr) {
        s->speaker = rca_pcspk_create(250u);
        if (!s->speaker)
            fprintf(stderr, "gemu-rca: pecom32: failed to init audio\n");
    }

    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr,
                                 CDP1869_VISIBLE_W * cfg->display_scale,
                                 CDP1869_VISIBLE_H * cfg->display_scale);
        gemu_vnc_set_palette(s->vnc, pecom_palette,
                             (int)(sizeof(pecom_palette) / sizeof(pecom_palette[0])));
    }
    gemu_monitor_set_vnc(s->monitor, s->vnc);

    /* Load ROM (16 KB for Pecom 32, 32 KB across two chips for Pecom 64) */
    s->rom_size = 0;
    for (int i = 0; i < cfg->n_roms; i++) {
        uint32_t off = cfg->roms[i].addr;
        GemuMemory tmp = {.data = s->rom, .size = sizeof(s->rom)};
        size_t len = 0;
        if (!gemu_mem_load_file(&tmp, off, cfg->roms[i].path, &len)) {
            fprintf(stderr, "gemu-rca: pecom32: failed to load '%s'\n",
                    cfg->roms[i].path);
            free(s);
            return NULL;
        }
        uint32_t end = off + (uint32_t)len;
        if (end > s->rom_size) s->rom_size = end;
        printf("gemu-rca: %zu bytes @ ROM+0x%04X  ← %s\n",
               len, off, cfg->roms[i].path);
        gemu_monitor_register_rom(s->monitor, off, (uint32_t)len, cfg->roms[i].path);
    }

    return s;
}

void rca_pecom32_reset(RcaPecom32State *s, const RcaConfig *cfg) {
    cdp1802_reset(&s->cpu);
    cdp1869_reset(&s->vis);
    cdp1869_set_page_ram_mask(&s->vis, 0x3FFu);
    cdp1869_set_char_stride(&s->vis, 16u);
    cdp1869_set_block_cpu_access(&s->vis, true);
    rca_pcspk_set_gate(s->speaker, 0);

    s->boot_mirror = true;
    s->iogroup     = 0;
    s->key_shift   = false;
    s->key_ctrl    = false;
    s->key_esc     = false;
    s->caps_locked = false;

    memset(s->keys,       0, sizeof(s->keys));
    memset(s->keys_live,  0, sizeof(s->keys_live));
    memset(s->keys_latch, 0, sizeof(s->keys_latch));

    s->rom_size = 0;
    for (int i = 0; i < cfg->n_roms; i++) {
        GemuMemory tmp = {.data = s->rom, .size = sizeof(s->rom)};
        size_t len = 0;
        gemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, &len);
        uint32_t end = cfg->roms[i].addr + (uint32_t)len;
        if (end > s->rom_size) s->rom_size = end;
    }
}

void rca_pecom32_destroy(RcaPecom32State *s) {
    if (!s) return;
    rca_pcspk_destroy(s->speaker);
    gemu_monitor_destroy(s->monitor);
    gemu_vnc_destroy(s->vnc);
    free(s);
}

/* ── Input polling ───────────────────────────────────────────────────────── */

static void pecom_poll_keys(RcaPecom32State *s, GemuDisplay *display) {
    if (!display) return;

    memset(s->keys, 0, sizeof(s->keys));
    for (size_t i = 0; i < PECOM_N_KEYS; i++) {
        if (gemu_display_is_key_held(display, pecom_keymap[i].name))
            s->keys[pecom_keymap[i].row] |= (uint8_t)(1u << pecom_keymap[i].bit);
    }

    s->key_esc   = gemu_display_is_key_held(display, "Escape");
    s->key_shift = gemu_display_is_key_held(display, "Left Shift")
                || gemu_display_is_key_held(display, "Right Shift");
    s->key_ctrl  = gemu_display_is_key_held(display, "Left Ctrl")
                || gemu_display_is_key_held(display, "Right Ctrl");
}

static void pecom_vnc_apply_key(RcaPecom32State *s, uint32_t ks, bool down) {
    switch (ks) {
    case 0xffe1: case 0xffe2: s->key_shift = down; return;
    case 0xffe3: case 0xffe4: s->key_ctrl  = down; return;
    case 0xff1b:               s->key_esc   = down; return;
    case 0xffe5: if (down) s->caps_locked = !s->caps_locked; return;
    default: break;
    }

    /* Translate VNC special keysyms to keymap name strings */
    const char *name = NULL;
    char single[2] = {0, 0};
    switch (ks) {
    case 0xff08: name = "End";    break;  /* Backspace → BS/DEL key (row 0x0B) */
    case 0xff0d: name = "Return"; break;
    case 0xff50: name = "Home";   break;
    case 0xff57: name = "End";    break;
    case 0xff52: name = "Up";     break;
    case 0xff54: name = "Down";   break;
    case 0xff51: name = "Left";   break;
    case 0xff53: name = "Right";  break;
    case ' ':    name = "Space";  break;
    default:
        if (ks >= 'A' && ks <= 'Z') ks += 32u;
        if (ks >= 0x21 && ks < 0x7f) { single[0] = (char)ks; name = single; }
        break;
    }
    if (!name) return;

    for (size_t i = 0; i < PECOM_N_KEYS; i++) {
        if (strcmp(name, pecom_keymap[i].name) != 0) continue;
        uint8_t row = pecom_keymap[i].row;
        uint8_t bit = (uint8_t)(1u << pecom_keymap[i].bit);
        if (down) {
            s->keys_live[row]  |= bit;
            s->keys_latch[row] |= bit;
        } else {
            s->keys_live[row]  &= (uint8_t)~bit;
        }
        break;
    }
}

static void pecom_poll_vnc(RcaPecom32State *s) {
    if (!s->vnc) return;

    /* Clear the one-frame latch so fast taps don't linger across frames. */
    memset(s->keys_latch, 0, sizeof(s->keys_latch));

    GemuVncKeyEvent ev;
    while (gemu_vnc_pop_key_event(s->vnc, &ev))
        pecom_vnc_apply_key(s, (uint32_t)ev.keysym, ev.down);

    for (size_t i = 0; i < 64u; i++)
        s->keys[i] = s->keys_live[i] | s->keys_latch[i];
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

void rca_pecom32_run(RcaPecom32State *s, const RcaConfig *cfg) {
    GemuDisplay *display = NULL;
    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        display = gemu_display_create(cfg->display_type,
            &(GemuDisplayConfig){
                .title     = "GEMU",
                .fb_width  = CDP1869_VISIBLE_W,
                .fb_height = CDP1869_VISIBLE_H,
                .scale     = cfg->display_scale,
                .renderer  = GEMU_RENDERER_AUTO,
                .actions   = NULL,
                .n_actions = 0,
                .ini_section = "pecom",
                .gtk = &(GemuDisplayGtkExtras){ .monitor = s->monitor },
            });
        if (!display) {
            fprintf(stderr, "gemu-rca: pecom32: failed to create display\n");
            return;
        }
        gemu_monitor_set_input_reset_cb(s->monitor,
                                        gemu_display_reset_input_bindings_ud,
                                        display);
    }

    /* PAL 50 Hz */
    const unsigned frame_ms = 1000u / PECOM32_FRAME_HZ;

    /* ARGB framebuffer for gemu_display_render() */
    uint32_t argb[CDP1869_VISIBLE_W * CDP1869_VISIBLE_H];

    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        if (display) {
            gemu_display_poll(display);
            if (gemu_display_should_quit(display)) break;
            if (gemu_display_reset_requested(display)) {
                gemu_display_clear_flags(display);
                rca_pecom32_reset(s, cfg);
            }
            pecom_poll_keys(s, display);
        }
        pecom_poll_vnc(s);

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)  { if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true); else quit = true; }
            else if (cmd == GEMU_MON_RESET) rca_pecom32_reset(s, cfg);
            else if (cmd == GEMU_MON_CUSTOM) gemu_monitor_unknown_command(s->monitor);
        }
        if (quit) break;
        gemu_display_set_paused(display, gemu_monitor_is_paused(s->monitor));

        if (!gemu_monitor_is_paused(s->monitor)) {
            for (unsigned i = 0; i < PECOM32_MCYCLES_PER_FRAME; i++) {
                pecom_video_timing(s, i);
                cdp1802_step(&s->cpu);
            }

            /* Ensure non_display=true at frame boundary so page/char RAM is writable */
            s->vis.non_display = true;
            s->cpu.EF[0] = !s->key_ctrl;

            if (s->vis.dirty) {
                cdp1869_render(&s->vis);
                if (display) {
                    int n = CDP1869_VISIBLE_W * CDP1869_VISIBLE_H;
                    for (int i = 0; i < n; i++)
                        argb[i] = pecom_palette[s->vis.bitmap[i] & 7u];
                    gemu_display_render(display, argb,
                                        CDP1869_VISIBLE_W, CDP1869_VISIBLE_H);
                }
                gemu_vnc_update(s->vnc, s->vis.bitmap,
                                CDP1869_VISIBLE_W, CDP1869_VISIBLE_H);
            }
        }

        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        unsigned elapsed_ms = (unsigned)(
            (t1.tv_sec  - t0.tv_sec)  * 1000u +
            (t1.tv_nsec - t0.tv_nsec) / 1000000u);
        if (elapsed_ms < frame_ms)
            pecom_sleep_ms(frame_ms - elapsed_ms);
    }

    printf("gemu-rca: %llu machine cycles, %llu instructions\n",
           (unsigned long long)s->cpu.cycle_count,
           (unsigned long long)s->cpu.insn_count);

    gemu_monitor_stop(s->monitor);
    gemu_display_destroy(display);
}
