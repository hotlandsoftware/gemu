#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif
#include "chip8.h"
#include "chip8_dev.h"
#include "gemu/dev/device.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/screen.h"
#include "gemu/dev/state.h"
#include "gemu/memory.h"
#include "gemu/screendump.h"
#include "gemu/gemu_display.h"
#include "octo.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static inline void chip8_sleep_frame(void) { Sleep(1000 / CHIP8_TIMER_HZ); }
#else
#  include <time.h>
static inline void chip8_sleep_frame(void) {
    struct timespec ts = {0, 1000000000L / CHIP8_TIMER_HZ};
    nanosleep(&ts, NULL);
}
#endif

static const uint8_t chip8_font[CHIP8_FONT_BYTES] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, /* 0 */
    0x20, 0x60, 0x20, 0x20, 0x70, /* 1 */
    0xF0, 0x10, 0xF0, 0x80, 0xF0, /* 2 */
    0xF0, 0x10, 0xF0, 0x10, 0xF0, /* 3 */
    0x90, 0x90, 0xF0, 0x10, 0x10, /* 4 */
    0xF0, 0x80, 0xF0, 0x10, 0xF0, /* 5 */
    0xF0, 0x80, 0xF0, 0x90, 0xF0, /* 6 */
    0xF0, 0x10, 0x20, 0x40, 0x40, /* 7 */
    0xF0, 0x90, 0xF0, 0x90, 0xF0, /* 8 */
    0xF0, 0x90, 0xF0, 0x10, 0xF0, /* 9 */
    0xF0, 0x90, 0xF0, 0x90, 0x90, /* A */
    0xE0, 0x90, 0xE0, 0x90, 0xE0, /* B */
    0xF0, 0x80, 0x80, 0x80, 0xF0, /* C */
    0xE0, 0x90, 0x90, 0x90, 0xE0, /* D */
    0xF0, 0x80, 0xF0, 0x80, 0xF0, /* E */
    0xF0, 0x80, 0xF0, 0x80, 0x80, /* F */
};

/* ── The core machine (stage K1) ─────────────────────────────────────────────
 * cpu (chip8): the interpreter, the machine's executor at the frame's
 * instructions times 60 a second (-hz is instructions a frame); its delay
 * and sound timers count down on a 60 Hz core timer, at each frame's end;
 * the board's "screen" is the 64x32 display.
 * Before stage K the run loop ran blocks for the frame's instructions,
 * counted the timers down and slept; it didn't check breakpoints, and
 * while a program waited for a key (Fx0A) nothing ran, the timers
 * included.  Now the executor idles out a key wait and the timers keep
 * counting; breakpoints stop it (an instruction at a time while some are
 * set). */
struct Chip8Machine {
    GemuMachine  *machine;
    GemuDevice   *board, *cpu_dev;
    GemuTimer    *timers;
    GemuScreen   *screen;
    Chip8State   *cpu;          /* the device's */
    const Chip8Config *cfg;
    GemuMonitor  *monitor;
};

static bool chip8_screendump(void *ud, const char *path) {
    Chip8Machine *s = ud;
    return gemu_screendump_mono(path, s->cpu->vram, CHIP8_DISPLAY_W, CHIP8_DISPLAY_H);
}

/* ── ROM loading (raw .ch8 or compiled .8o) ──────────────────────────────── */

/* Monitor memory view over the 4 KB RAM array (opcodes are big-endian). */
static bool chip8_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    const Chip8Machine *s = ud;
    if (addr >= CHIP8_MEM_SIZE) return false;
    *val = s->cpu->mem[addr];
    return true;
}

static bool chip8_mon_write(void *ud, uint64_t addr, uint8_t val) {
    Chip8Machine *s = ud;
    if (addr >= CHIP8_MEM_SIZE) return false;
    s->cpu->mem[addr] = val;
    gemu_tb_cache_flush(&s->cpu->tb_cache);   /* poked code must be re-translated */
    return true;
}

static void chip8_mon_cpu_state(void *ud, char *buf, size_t len) {
    const Chip8State *c = ((const Chip8Machine *)ud)->cpu;
    int n = snprintf(buf, len, "PC=%03X I=%03X SP=%X DT=%02X ST=%02X\n",
                     c->PC, c->I, c->SP, c->delay, c->sound);
    for (int r = 0; r < CHIP8_NUM_REGS && n >= 0 && (size_t)n < len; r++)
        n += snprintf(buf + n, len - (size_t)n, "V%X=%02X%s", r, c->V[r],
                      r == CHIP8_NUM_REGS - 1 ? "" : " ");
}

static bool path_ends_8o(const char *p)
{
    size_t n = strlen(p);
    return n >= 3 && strcmp(p + n - 3, ".8o") == 0;
}

/* Returns bytes loaded, or 0 on error (fills errbuf). */
static size_t chip8_load_rom(uint8_t *mem, uint32_t mem_size,
                              const char *path, char *errbuf, size_t ebsz)
{
    if (path_ends_8o(path)) {
        /* Read source text */
        FILE *fp = fopen(path, "rb");
        if (!fp) { snprintf(errbuf, ebsz, "cannot open '%s'", path); return 0; }
        fseek(fp, 0, SEEK_END); long fsz = ftell(fp); rewind(fp);
        if (fsz <= 0) { fclose(fp); snprintf(errbuf, ebsz, "empty file"); return 0; }
        char *src = malloc((size_t)fsz + 1);
        if (!src) { fclose(fp); snprintf(errbuf, ebsz, "OOM"); return 0; }
        if (fread(src, 1, (size_t)fsz, fp) != (size_t)fsz)
            { free(src); fclose(fp); snprintf(errbuf, ebsz, "read error"); return 0; }
        src[fsz] = '\0'; fclose(fp);

        int max = (int)(mem_size - CHIP8_ROM_BASE);
        int n = octo_compile(src, mem + CHIP8_ROM_BASE, max, errbuf, (int)ebsz);
        free(src);
        return (n >= 0) ? (size_t)n : 0;
    }

    GemuMemory tmp = {.data = mem, .size = mem_size};
    size_t rom_len = 0;
    if (!gemu_mem_load_file(&tmp, CHIP8_ROM_BASE, path, &rom_len))
        { snprintf(errbuf, ebsz, "failed to load '%s'", path); return 0; }
    return rom_len;
}

/* The 60 Hz timers: at each frame's end. */
static void chip8_timers_tick(void *ud, int param) {
    Chip8State *c = ((Chip8Machine *)ud)->cpu;
    (void)param;
    if (c->delay > 0) c->delay--;
    if (c->sound > 0) c->sound--;
}

static bool chip8_stop_before(void *ud) {
    Chip8Machine *s = ud;
    return gemu_monitor_check_exec(s->monitor, s->cpu->PC);
}

static void chip8_screen_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    Chip8Machine *s = ud;
    for (int y = clip->y0; y < clip->y1; y++)
        for (int x = 0; x < CHIP8_DISPLAY_W; x++)
            bmp->pix[(size_t)y * (size_t)bmp->stride + (size_t)x] =
                s->cpu->vram[y * CHIP8_DISPLAY_W + x] ? s->cfg->fg_argb : s->cfg->bg_argb;
}

static const GemuDeviceType chip8_board_type = {
    .name = "chip8-board", .desc = "CHIP-8 board (the display, the keypad)",
    .size = 0, .stateless = true,
};

static bool chip8_core_build(Chip8Machine *s) {
    if (!(s->machine = gemu_machine_new("chip8"))) return false;
    uint64_t hz = (uint64_t)s->cfg->cpu_hz * CHIP8_TIMER_HZ;
    bool ok = (s->board = gemu_device_add(s->machine, &chip8_board_type, "board", NULL, NULL)) &&
              (s->cpu_dev = gemu_device_add(s->machine, &gemu_chip8_type, "cpu", NULL, NULL)) &&
              (s->timers = gemu_timer_new(s->cpu_dev, chip8_timers_tick, s)) &&
              gemu_machine_init(s->machine);
    if (ok) {
        s->cpu = gemu_chip8(s->cpu_dev);
        s->cpu->monitor = s->monitor;
        ok = gemu_chip8_exec(s->cpu_dev, hz) &&
             (s->screen = gemu_screen_new(s->board, "screen")) &&
             gemu_screen_set_size(s->screen, CHIP8_DISPLAY_W, CHIP8_DISPLAY_H);
    }
    if (ok) {
        gemu_screen_set_update(s->screen, chip8_screen_update, s);
        ok = gemu_machine_start(s->machine);
    }
    if (ok) {
        gemu_timer_start_clocks(s->timers, CHIP8_TIMER_HZ, 1, 1, 0);
        gemu_monitor_attach_machine(s->monitor, s->machine, NULL);
    } else {
        fprintf(stderr, "gemu-chip8: core machine: %s\n", gemu_machine_error(s->machine)[0]
                ? gemu_machine_error(s->machine) : "a device couldn't be added");
    }
    return ok;
}

Chip8Machine *chip8_machine_create(const Chip8Config *cfg) {
    Chip8Machine *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;
    s->monitor = gemu_monitor_create();
    gemu_monitor_set_screendump_cb(s->monitor, chip8_screendump, s);
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = chip8_mon_read, .write = chip8_mon_write, .ud = s,
        .addr_bits = 12, .big_endian = true });
    gemu_monitor_set_cpu_state_cb(s->monitor, chip8_mon_cpu_state, s);
    if (!chip8_core_build(s)) {
        chip8_machine_destroy(s);
        return NULL;
    }

    memcpy(s->cpu->mem + CHIP8_FONT_BASE, chip8_font, CHIP8_FONT_BYTES);

    char errbuf[256] = {0};
    size_t rom_len = chip8_load_rom(s->cpu->mem, cfg->mem_size, cfg->rom_path, errbuf, sizeof(errbuf));
    if (!rom_len) {
        fprintf(stderr, "gemu-chip8: %s\n", errbuf);
        chip8_machine_destroy(s);
        return NULL;
    }
    printf("gemu-chip8: loaded %zu bytes from '%s'\n", rom_len, cfg->rom_path);
    gemu_monitor_register_rom(s->monitor, CHIP8_ROM_BASE, (uint32_t)rom_len, cfg->rom_path);
    return s;
}

static void chip8_machine_reset(Chip8Machine *m, const Chip8Config *cfg) {
    Chip8State *s = m->cpu;
    memset(s->V,         0, sizeof(s->V));
    memset(s->stack,     0, sizeof(s->stack));
    memset(s->vram,      0, sizeof(s->vram));
    memset(s->keys,      0, sizeof(s->keys));
    memset(s->keys_prev, 0, sizeof(s->keys_prev));
    s->I = 0; s->PC = CHIP8_ROM_BASE; s->SP = 0;
    s->delay = 0; s->sound = 0;
    s->draw_flag = true; s->wait_key = false;
    s->insn_count = s->tb_hits = s->tb_misses = 0;

    gemu_tb_cache_flush(&s->tb_cache);

    memset(s->mem + CHIP8_ROM_BASE, 0, CHIP8_MEM_SIZE - CHIP8_ROM_BASE);
    char errbuf[256]; chip8_load_rom(s->mem, cfg->mem_size, cfg->rom_path, errbuf, sizeof(errbuf));

    printf("gemu-chip8: reset\n");
}

void chip8_machine_destroy(Chip8Machine *s) {
    if (!s) return;
    gemu_machine_free(s->machine);
    gemu_monitor_destroy(s->monitor);
    free(s);
}

/* The keypad's keys held (bit k = key k), for SKP/SKNP. */
static void chip8_set_keys(Chip8Machine *s, uint32_t held) {
    memcpy(s->cpu->keys_prev, s->cpu->keys, CHIP8_NUM_KEYS);
    for (int k = 0; k < CHIP8_NUM_KEYS; k++)
        s->cpu->keys[k] = (held >> k) & 1;
}

/* One frame: a key pressed this frame ends a key wait (the lowest one),
 * then the machine runs the frame's time (the timers count at its end). */
static void chip8_frame(Chip8Machine *s, uint32_t pressed) {
    Chip8State *c = s->cpu;
    if (c->wait_key && pressed) {
        c->V[c->wait_reg] = (uint8_t)__builtin_ctz(pressed);
        c->wait_key = false;
        c->PC += 2;
    }
    gemu_chip8_set_stop_before(s->cpu_dev,
        gemu_monitor_has_exec_breakpoints(s->monitor) ? chip8_stop_before : NULL, s);
    /* to the timers' tick, exactly (1/60 s added up frame by frame would
     * drift from it by rounding) */
    gemu_machine_run_until(s->machine, gemu_timer_expires(s->timers));
}

/* ── Hex-keypad action table ─────────────────────────────────────────────── */
/*
 * CHIP-8 hex keypad → keyboard:
 *   Keypad   1 2 3 C     Keyboard   1 2 3 4
 *            4 5 6 D                Q W E R
 *            7 8 9 E                A S D F
 *            A 0 B F                Z X C V
 */
static const GemuActionDef chip8_actions[CHIP8_NUM_KEYS] = {
    { "0", GEMU_ACTION(0),  "x" }, { "1", GEMU_ACTION(1),  "1" },
    { "2", GEMU_ACTION(2),  "2" }, { "3", GEMU_ACTION(3),  "3" },
    { "4", GEMU_ACTION(4),  "q" }, { "5", GEMU_ACTION(5),  "w" },
    { "6", GEMU_ACTION(6),  "e" }, { "7", GEMU_ACTION(7),  "a" },
    { "8", GEMU_ACTION(8),  "s" }, { "9", GEMU_ACTION(9),  "d" },
    { "A", GEMU_ACTION(10), "z" }, { "B", GEMU_ACTION(11), "c" },
    { "C", GEMU_ACTION(12), "4" }, { "D", GEMU_ACTION(13), "r" },
    { "E", GEMU_ACTION(14), "f" }, { "F", GEMU_ACTION(15), "v" },
};

/* ── Run loop ─────────────────────────────────────────────────────────────── */

/* The board's screen to the window if the program drew since the last
 * time (running, or stepped from the monitor while paused). */
static void chip8_present(Chip8Machine *s, GemuDisplay *display) {
    if (!s->cpu->draw_flag) return;
    s->cpu->draw_flag = false;
    if (!display) return;
    const GemuBitmap *b = gemu_screen_render(s->screen);
    gemu_display_render(display, b->pix, b->width, b->height);
}

void chip8_machine_run(Chip8Machine *s, const Chip8Config *cfg) {
    /* Headless (-display none without -vnc): no window and no keypad, e.g.
     * a silent bench; with -vnc, gemu_display_create() returns a headless
     * display. */
    GemuDisplay *display = NULL;
    if (gemu_display_wanted(cfg->display_type)) {
        GemuDisplayGtkExtras gtk_extras = { .monitor = s->monitor };
        display = gemu_display_create(cfg->display_type,
            &(GemuDisplayConfig){
                .fb_width    = CHIP8_DISPLAY_W,
                .fb_height   = CHIP8_DISPLAY_H,
                .scale       = cfg->display_scale,
                .renderer    = GEMU_RENDERER_AUTO,
                .actions     = chip8_actions,
                .n_actions   = CHIP8_NUM_KEYS,
                .ini_section = "chip8-keypad",
                .gtk         = &gtk_extras,
            });
        if (!display) {
            fprintf(stderr, "gemu-chip8: failed to create display\n");
            return;
        }
        gemu_monitor_set_input_reset_cb(s->monitor, gemu_display_reset_input_bindings_ud, display);
    }
    gemu_monitor_start(s->monitor);

    while (true) {
        uint32_t pressed = 0;
        if (display) {
            uint32_t held = gemu_display_poll(display);
            if (gemu_display_should_quit(display)) break;
            if (gemu_display_reset_requested(display)) {
                chip8_machine_reset(s, cfg);
                gemu_display_clear_flags(display);
            }
            chip8_set_keys(s, held);
            if (s->cpu->wait_key) pressed = gemu_display_last_pressed(display);
        }

        bool quit = false;
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)  { if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true); else { quit = true; break; } }
            else if (cmd == GEMU_MON_RESET) chip8_machine_reset(s, cfg);
            else if (cmd == GEMU_MON_STEP) {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                for (uint32_t i = 0; i < n; i++) chip8_exec_single(s->cpu);
            }
        }
        if (quit) break;
        bool paused = gemu_monitor_is_paused(s->monitor);
        gemu_display_set_paused(display, paused);

        if (!paused) chip8_frame(s, pressed);
        chip8_present(s, display);

        /* Pace to the machine's time; paused, a frame's delay. */
        if (!paused) {
            gemu_machine_pace(s->machine, 0);
        } else {
            gemu_machine_pace_reset(s->machine);
            chip8_sleep_frame();
        }
    }

    printf("gemu-chip8: %llu instructions executed, %llu TB hits, %llu misses\n",
           (unsigned long long)s->cpu->insn_count,
           (unsigned long long)s->cpu->tb_hits,
           (unsigned long long)s->cpu->tb_misses);
    gemu_monitor_stop(s->monitor);
    gemu_display_destroy(display);
}
