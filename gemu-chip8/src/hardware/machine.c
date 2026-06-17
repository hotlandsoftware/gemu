#ifndef _WIN32
#  define _POSIX_C_SOURCE 199309L
#endif
#include "chip8.h"
#include "gemu/memory.h"
#include "gemu/screendump.h"
#include "gemu/gemu_display.h"
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

static bool chip8_screendump(void *ud, const char *path) {
    Chip8State *s = ud;
    int w = CHIP8_DISPLAY_W, h = CHIP8_DISPLAY_H;
    uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return false;
    for (int i = 0; i < w * h; i++) {
        uint8_t v  = s->vram[i] ? 0xFF : 0x00;
        rgb[i*3+0] = v; rgb[i*3+1] = v; rgb[i*3+2] = v;
    }
    bool ok = gemu_screendump(path, rgb, w, h);
    free(rgb);
    return ok;
}

Chip8State *chip8_machine_create(const Chip8Config *cfg) {
    Chip8State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->PC      = CHIP8_ROM_BASE;
    s->monitor = gemu_monitor_create();
    gemu_monitor_set_screendump_cb(s->monitor, chip8_screendump, s);
    gemu_tb_cache_init(&s->tb_cache, free);
    if (cfg->vnc_addr)
        s->vnc = gemu_vnc_create(cfg->vnc_addr,
                                 CHIP8_DISPLAY_W * cfg->display_scale,
                                 CHIP8_DISPLAY_H * cfg->display_scale);

    memcpy(s->mem + CHIP8_FONT_BASE, chip8_font, CHIP8_FONT_BYTES);

    GemuMemory tmp = {.data = s->mem, .size = cfg->mem_size};
    size_t rom_len = 0;
    if (!gemu_mem_load_file(&tmp, CHIP8_ROM_BASE, cfg->rom_path, &rom_len)) {
        fprintf(stderr, "gemu-chip8: failed to load ROM '%s'\n", cfg->rom_path);
        free(s);
        return NULL;
    }
    printf("gemu-chip8: loaded %zu bytes from '%s'\n", rom_len, cfg->rom_path);
    gemu_monitor_register_rom(s->monitor, CHIP8_ROM_BASE, (uint32_t)rom_len, cfg->rom_path);
    return s;
}

void chip8_machine_reset(Chip8State *s, const Chip8Config *cfg) {
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
    GemuMemory tmp = {.data = s->mem, .size = cfg->mem_size};
    gemu_mem_load_file(&tmp, CHIP8_ROM_BASE, cfg->rom_path, NULL);

    printf("gemu-chip8: reset\n");
}

void chip8_machine_destroy(Chip8State *s) {
    if (!s) return;
    gemu_tb_cache_flush(&s->tb_cache);
    gemu_monitor_destroy(s->monitor);
    gemu_vnc_destroy(s->vnc);
    free(s);
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

void chip8_machine_run(Chip8State *s, const Chip8Config *cfg) {
    /* Headless: no display, VNC-only or silent bench */
    if (cfg->display_type == GEMU_DISPLAY_NONE) {
        const int insns_frame = cfg->cpu_hz / CHIP8_TIMER_HZ;
        gemu_monitor_start(s->monitor);
        bool running = true;
        while (running) {
            GemuMonCmd cmd;
            while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
                if      (cmd == GEMU_MON_QUIT)  running = false;
                else if (cmd == GEMU_MON_RESET) chip8_machine_reset(s, cfg);
                else if (cmd == GEMU_MON_STEP) {
                    uint32_t n = gemu_monitor_step_count(s->monitor);
                    for (uint32_t i = 0; i < n; i++) chip8_exec_single(s);
                } else if (cmd == GEMU_MON_CUSTOM) gemu_monitor_unknown_command(s->monitor);
            }
            if (!running || gemu_monitor_is_paused(s->monitor)) {
                chip8_sleep_frame();
                continue;
            }
            memcpy(s->keys_prev, s->keys, CHIP8_NUM_KEYS);
            gemu_vnc_get_keys(s->vnc, s->keys);
            if (s->wait_key) {
                for (int k = 0; k < CHIP8_NUM_KEYS; k++) {
                    if (s->keys[k] && !s->keys_prev[k]) {
                        s->V[s->wait_reg] = (uint8_t)k;
                        s->wait_key = false;
                        s->PC += 2;
                        break;
                    }
                }
                chip8_sleep_frame();
                continue;
            }
            int budget = insns_frame;
            while (budget > 0 && !s->wait_key) {
                GemuTb *tb = gemu_tb_lookup(&s->tb_cache, s->PC);
                if (!tb) tb = chip8_translate_block(s, s->PC);
                if (!tb) break;
                chip8_execute_tb(s, tb);
                budget -= (int)tb->n_insns;
            }
            if (s->delay > 0) s->delay--;
            if (s->sound > 0) s->sound--;
            if (s->draw_flag) {
                gemu_vnc_update(s->vnc, s->vram, CHIP8_DISPLAY_W, CHIP8_DISPLAY_H);
                s->draw_flag = false;
            }
            chip8_sleep_frame();
        }
        printf("gemu-chip8: %llu instructions executed, %llu TB hits, %llu misses\n",
               (unsigned long long)s->insn_count,
               (unsigned long long)s->tb_hits,
               (unsigned long long)s->tb_misses);
        gemu_monitor_stop(s->monitor);
        return;
    }

    /* Display-backed run loop (SDL, GTK, caca) */
    GemuDisplayGtkExtras gtk_extras = { .monitor = s->monitor };
    GemuDisplay *display = gemu_display_create(cfg->display_type,
        &(GemuDisplayConfig){
            .title       = "gemu-chip8",
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
    gemu_monitor_start(s->monitor);

    const int insns_frame = cfg->cpu_hz / CHIP8_TIMER_HZ;
    uint32_t  argb[CHIP8_DISPLAY_W * CHIP8_DISPLAY_H];

    while (true) {
        /* Input */
        uint32_t held = gemu_display_poll(display);
        if (gemu_display_should_quit(display)) break;
        if (gemu_display_reset_requested(display)) {
            chip8_machine_reset(s, cfg);
            gemu_display_clear_flags(display);
        }

        /* Update key arrays for CPU (SKP/SKNP instructions) */
        memcpy(s->keys_prev, s->keys, CHIP8_NUM_KEYS);
        for (int k = 0; k < CHIP8_NUM_KEYS; k++)
            s->keys[k] = (held >> k) & 1;

        /* Monitor commands */
        bool quit = false;
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)  { quit = true; break; }
            else if (cmd == GEMU_MON_RESET) chip8_machine_reset(s, cfg);
            else if (cmd == GEMU_MON_STEP) {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                for (uint32_t i = 0; i < n; i++) chip8_exec_single(s);
            } else if (cmd == GEMU_MON_CUSTOM) gemu_monitor_unknown_command(s->monitor);
        }
        if (quit) break;

        if (gemu_monitor_is_paused(s->monitor)) {
            chip8_sleep_frame();
            continue;
        }

        /* Wait for keypress (LD Vx, K) */
        if (s->wait_key) {
            uint32_t pressed = gemu_display_last_pressed(display);
            if (pressed) {
                s->V[s->wait_reg] = (uint8_t)__builtin_ctz(pressed);
                s->wait_key = false;
                s->PC += 2;
            } else {
                chip8_sleep_frame();
                continue;
            }
        }

        /* Execute */
        int budget = insns_frame;
        while (budget > 0 && !s->wait_key) {
            GemuTb *tb = gemu_tb_lookup(&s->tb_cache, s->PC);
            if (!tb) tb = chip8_translate_block(s, s->PC);
            if (!tb) break;
            chip8_execute_tb(s, tb);
            budget -= (int)tb->n_insns;
        }

        if (s->delay > 0) s->delay--;
        if (s->sound > 0) s->sound--;

        /* Render */
        if (s->draw_flag) {
            for (int i = 0; i < CHIP8_DISPLAY_W * CHIP8_DISPLAY_H; i++)
                argb[i] = s->vram[i] ? 0xFFFFFFFFu : 0xFF000000u;
            gemu_display_render(display, argb, CHIP8_DISPLAY_W, CHIP8_DISPLAY_H);
            gemu_vnc_update(s->vnc, s->vram, CHIP8_DISPLAY_W, CHIP8_DISPLAY_H);
            s->draw_flag = false;
        }

        chip8_sleep_frame();
    }

    printf("gemu-chip8: %llu instructions executed, %llu TB hits, %llu misses\n",
           (unsigned long long)s->insn_count,
           (unsigned long long)s->tb_hits,
           (unsigned long long)s->tb_misses);
    gemu_monitor_stop(s->monitor);
    gemu_display_destroy(display);
}
