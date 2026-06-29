#ifndef _WIN32
#  define _POSIX_C_SOURCE 199309L
#endif
#include "studio2.h"
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
static inline void studio2_sleep_ms(unsigned ms) { Sleep(ms); }
#else
#  include <time.h>
static inline void studio2_sleep_ms(unsigned ms) {
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

#define STUDIO2_FRAME_HZ       60u

/* 300 Hz beeper as used in MAME's Studio II driver */
#define STUDIO2_BEEP_HZ  300u

/* ST2 cartridge file format */
#define ST2_HEADER_SIZE  0x100u
#define ST2_MAGIC        "RCA2"

/* ── Memory callbacks ────────────────────────────────────────────────────── */

static uint8_t studio2_mem_read(uint16_t addr, void *ud) {
    RcaStudio2State *s = ud;
    if (addr < 0x0400)
        return s->rom[addr];
    if (addr < 0x0800)
        return s->cart_loaded ? s->cart[addr - 0x0400] : s->rom[addr];
    if (addr >= 0x0C00 && addr < 0x1000 && s->cart_c00)
        return s->cart[0x0800 + (addr - 0x0C00)];
    return s->ram[addr & STUDIO2_RAM_MASK];
}

static void studio2_mem_write(uint16_t addr, uint8_t val, void *ud) {
    RcaStudio2State *s = ud;
    if (addr >= 0x0800)
        s->ram[addr & STUDIO2_RAM_MASK] = val;
}

/* ── DMA: CDP1861 → vram ─────────────────────────────────────────────────── */

static void studio2_dma_out(uint8_t *data, void *ud) {
    RcaStudio2State *s = ud;
    Cdp1861 *vdc = &s->vdc;

    int row = vdc->line_counter - CDP1861_FIRST_LINE;
    if (row < 0 || row >= CDP1861_DISPLAY_H) return;

    int logical_row = row / 4;
    int byte_col = vdc->display_addr % STUDIO2_BYTES_PER_LINE;
    uint8_t b = *data;
    for (int bit = 0; bit < 8; bit++)
        s->vram[logical_row * STUDIO2_DISPLAY_W + byte_col * 8 + bit] =
            (b >> (7 - bit)) & 1u;

    vdc->display_addr++;
    if (vdc->display_addr >= (CDP1861_DISPLAY_W * CDP1861_DISPLAY_H / 8))
        vdc->display_addr = 0;

    s->draw_flag = true;
}

/* ── CPU I/O callbacks ───────────────────────────────────────────────────── */

static uint8_t studio2_io_in(uint8_t port, void *ud) {
    RcaStudio2State *s = ud;
    if (port == 1) {
        cdp1861_set_display(&s->vdc, true);
        return 0xFF;
    }
    return 0xFF;
}

static void studio2_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaStudio2State *s = ud;
    if (port == 1) {
        cdp1861_set_display(&s->vdc, false);
    } else if (port == 2) {
        s->keylatch = val & 0x0Fu;
    }
}

static void studio2_sync(void *ud) {
    RcaStudio2State *s = ud;
    cdp1861_sync(&s->vdc, &s->cpu);
}

static void studio2_q_out(uint8_t q, void *ud) {
    RcaStudio2State *s = ud;
    rca_pcspk_set_gate(s->speaker, q);
}

/* ── Cartridge loading ───────────────────────────────────────────────────── */

static bool studio2_load_cart(RcaStudio2State *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "studio2: cannot open cartridge '%s'\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    memset(s->cart, 0xFF, sizeof(s->cart));
    s->cart_c00 = false;

    /* Detect ST2 format by magic header */
    char magic[4];
    bool is_st2 = (file_size >= (long)ST2_HEADER_SIZE) &&
                  fread(magic, 1, 4, f) == 4 &&
                  memcmp(magic, ST2_MAGIC, 4) == 0;

    if (is_st2) {
        uint8_t header[ST2_HEADER_SIZE];
        rewind(f);
        if (fread(header, 1, ST2_HEADER_SIZE, f) < ST2_HEADER_SIZE) {
            fprintf(stderr, "studio2: truncated ST2 header in '%s'\n", path);
            fclose(f);
            return false;
        }
        uint8_t blocks = header[4];
        if (blocks < 2 || blocks > 11) {
            fprintf(stderr, "studio2: invalid ST2 block count %u in '%s'\n", blocks, path);
            fclose(f);
            return false;
        }
        uint8_t *pages = &header[64];
        for (int blk = 0; blk < blocks - 1; blk++) {
            uint16_t target = (uint16_t)pages[blk] << 8;
            if (target < 0x0400 || target > 0x0F00) {
                fprintf(stderr, "studio2: ST2 block %d has invalid target 0x%04X\n", blk, target);
                continue;
            }
            uint16_t offset;
            if (target >= 0x0C00)
                offset = (uint16_t)(0x0800 + (target - 0x0C00));
            else
                offset = (uint16_t)(target - 0x0400);
            if ((unsigned)offset + 0x100u > STUDIO2_CART_SIZE) continue;
            if (fread(&s->cart[offset], 1, 0x100, f) < 0x100)
                fprintf(stderr, "studio2: short ST2 block %d\n", blk);
            if (target >= 0x0C00)
                s->cart_c00 = true;
        }
    } else {
        /* Raw binary: load at 0x0400 */
        rewind(f);
        size_t limit = (file_size > 0x0400) ? 0x0400 : (size_t)file_size;
        size_t n = fread(s->cart, 1, limit, f);
        if (n == 0) {
            fprintf(stderr, "studio2: empty cartridge '%s'\n", path);
            fclose(f);
            return false;
        }
        /* Some homebrews extend into 0x0C00-0x0FFF */
        if (file_size > 0x0400) {
            size_t extra = fread(&s->cart[0x0800], 1, 0x0400, f);
            if (extra > 0) s->cart_c00 = true;
        }
    }

    fclose(f);
    s->cart_loaded = true;
    snprintf(s->cart_path, sizeof(s->cart_path), "%s", path);
    printf("studio2: cartridge '%s' loaded%s\n", path, s->cart_c00 ? " (+0xC00 bank)" : "");
    return true;
}

static void studio2_eject_cart(RcaStudio2State *s) {
    memset(s->cart, 0xFF, sizeof(s->cart));
    s->cart_loaded = false;
    s->cart_c00    = false;
    s->cart_path[0] = '\0';
    printf("studio2: cartridge ejected\n");
}

static GemuMediaResult studio2_media_change_cart(void *ud, const char *arg,
                                                 char *err, size_t err_len) {
    RcaStudio2State *s = ud;
    if (!arg || !arg[0]) {
        snprintf(err, err_len, "missing cartridge path");
        return GEMU_MEDIA_ERR;
    }
    if (!studio2_load_cart(s, arg)) {
        snprintf(err, err_len, "failed to load cartridge '%s'", arg);
        return GEMU_MEDIA_ERR;
    }
    return GEMU_MEDIA_OK_RESET;
}

static GemuMediaResult studio2_media_eject_cart(void *ud,
                                                char *err, size_t err_len) {
    (void)err;
    (void)err_len;
    studio2_eject_cart((RcaStudio2State *)ud);
    return GEMU_MEDIA_OK;
}

static void studio2_media_status_cart(void *ud, char *buf, size_t buf_len) {
    const RcaStudio2State *s = ud;
    if (s->cart_loaded)
        snprintf(buf, buf_len, "%s", s->cart_path);
    else
        snprintf(buf, buf_len, "empty");
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

static bool studio2_screendump(void *ud, const char *path) {
    RcaStudio2State *s = ud;
    int w = STUDIO2_DISPLAY_W, h = STUDIO2_DISPLAY_H;
    uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return false;
    for (int i = 0; i < w * h; i++) {
        uint8_t v = s->vram[i] ? 0xFF : 0x00;
        rgb[i*3+0] = v; rgb[i*3+1] = v; rgb[i*3+2] = v;
    }
    bool ok = gemu_screendump(path, rgb, w, h);
    free(rgb);
    return ok;
}

RcaStudio2State *rca_studio2_create(const RcaConfig *cfg) {
    RcaStudio2State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;

    cdp1802_init(&s->cpu, NULL, 0);
    s->cpu.io_in      = studio2_io_in;
    s->cpu.io_out     = studio2_io_out;
    s->cpu.mem_read   = studio2_mem_read;
    s->cpu.mem_write  = studio2_mem_write;
    s->cpu.on_sync    = studio2_sync;
    s->cpu.q_out      = studio2_q_out;
    s->cpu.io_ud      = s;

    cdp1861_init(&s->vdc, studio2_dma_out, s);
    s->vdc.lines_total = (cfg->tv_mode == RCA_TV_PAL)
                         ? CDP1861_PAL_LINES_TOTAL
                         : CDP1861_LINES_TOTAL;

    memset(s->rom,  0xFF, sizeof(s->rom));
    memset(s->cart, 0xFF, sizeof(s->cart));

    /* Load built-in ROM(s).
     * 0x0000–0x07FF → s->rom
     * 0x0C00–0x0FFF → s->cart[0x0800+] (clone machines with ROM in cart bank) */
    for (int i = 0; i < cfg->n_roms; i++) {
        uint16_t addr = cfg->roms[i].addr;
        size_t len = 0;
        bool ok;
        if (addr >= 0x0C00 && addr < 0x1000) {
            uint16_t offset = (uint16_t)(0x0800u + (addr - 0x0C00u));
            GemuMemory tmp = {.data = s->cart, .size = STUDIO2_CART_SIZE};
            ok = gemu_mem_load_file(&tmp, offset, cfg->roms[i].path, &len);
            if (ok) s->cart_c00 = true;
        } else {
            GemuMemory tmp = {.data = s->rom, .size = STUDIO2_ROM_SIZE};
            ok = gemu_mem_load_file(&tmp, addr, cfg->roms[i].path, &len);
        }
        if (!ok) {
            fprintf(stderr, "studio2: failed to load '%s'\n", cfg->roms[i].path);
            rca_studio2_destroy(s);
            return NULL;
        }
        printf("gemu-rca: %zu bytes @ 0x%04X  <- %s\n", len, addr, cfg->roms[i].path);
        gemu_monitor_register_rom(s->monitor, addr, (uint32_t)len, cfg->roms[i].path);
    }

    /* Insert cartridge if provided */
    if (cfg->cartridge_path)
        studio2_load_cart(s, cfg->cartridge_path);

    if (cfg->sound_hw != RCA_SOUND_NONE) {
        s->speaker = rca_pcspk_create(STUDIO2_BEEP_HZ);
        if (s->speaker)
            rca_pcspk_set_freq(s->speaker, STUDIO2_BEEP_HZ);
    }

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_screendump_cb(s->monitor, studio2_screendump, s);
    {
        GemuMediaDevice cart_dev = {
            .name   = "cartridge",
            .kind   = "cartridge",
            .ud     = s,
            .change = studio2_media_change_cart,
            .eject  = studio2_media_eject_cart,
            .status = studio2_media_status_cart,
        };
        if (s->cart_loaded)
            snprintf(cart_dev.file, sizeof(cart_dev.file), "%s", s->cart_path);
        gemu_monitor_register_media(s->monitor, &cart_dev);
    }
    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr,
                                 STUDIO2_DISPLAY_W * cfg->display_scale,
                                 STUDIO2_DISPLAY_H * cfg->display_scale);
    }
    gemu_monitor_set_vnc(s->monitor, s->vnc);

    rca_studio2_reset(s, cfg);
    return s;
}

void rca_studio2_reset(RcaStudio2State *s, const RcaConfig *cfg) {
    (void)cfg;
    memset(s->ram, 0, sizeof(s->ram));
    cdp1802_reset(&s->cpu);
    cdp1861_reset(&s->vdc);
    s->keylatch = 0;
}

void rca_studio2_destroy(RcaStudio2State *s) {
    if (!s) return;
    rca_pcspk_destroy(s->speaker);
    gemu_monitor_destroy(s->monitor);
    gemu_vnc_destroy(s->vnc);
    free(s);
}

/* ── Action table ────────────────────────────────────────────────────────── */

/* X11 keysyms used by VNC for Studio II keys (separate from display input) */
static const uint32_t vnc_keys_a[10] = {
    0xffe9, /* Alt_L  */ 'q', 'w', 'e', 'a', 's', 'd', 'z', 'x', 'c',
};
static const uint32_t vnc_keys_b[10] = {
    0xffb0, /* KP_0 */ 0xffb7, 0xffb8, 0xffb9,   /* 7 8 9 */
    0xffb4, 0xffb5, 0xffb6,                         /* 4 5 6 */
    0xffb1, 0xffb2, 0xffb3,                         /* 1 2 3 */
};

/* Bits 0-9: P1 keys 0-9 | Bits 10-19: P2 keys 0-9 | Bit 20: RESET */
static const GemuActionDef studio2_actions[] = {
    { "P1_0",  GEMU_ACTION(0),  "Left Alt" },
    { "P1_1",  GEMU_ACTION(1),  "q"        },
    { "P1_2",  GEMU_ACTION(2),  "w"        },
    { "P1_3",  GEMU_ACTION(3),  "e"        },
    { "P1_4",  GEMU_ACTION(4),  "a"        },
    { "P1_5",  GEMU_ACTION(5),  "s"        },
    { "P1_6",  GEMU_ACTION(6),  "d"        },
    { "P1_7",  GEMU_ACTION(7),  "z"        },
    { "P1_8",  GEMU_ACTION(8),  "x"        },
    { "P1_9",  GEMU_ACTION(9),  "c"        },
    { "P2_0",  GEMU_ACTION(10), "Keypad 0" },
    { "P2_1",  GEMU_ACTION(11), "Keypad 7" },
    { "P2_2",  GEMU_ACTION(12), "Keypad 8" },
    { "P2_3",  GEMU_ACTION(13), "Keypad 9" },
    { "P2_4",  GEMU_ACTION(14), "Keypad 4" },
    { "P2_5",  GEMU_ACTION(15), "Keypad 5" },
    { "P2_6",  GEMU_ACTION(16), "Keypad 6" },
    { "P2_7",  GEMU_ACTION(17), "Keypad 1" },
    { "P2_8",  GEMU_ACTION(18), "Keypad 2" },
    { "P2_9",  GEMU_ACTION(19), "Keypad 3" },
    { "RESET", GEMU_ACTION(20), "F3"       },
};
#define STUDIO2_N_ACTIONS ((int)(sizeof(studio2_actions)/sizeof(studio2_actions[0])))

static void studio2_update_ef(RcaStudio2State *s) {
    uint8_t latch = s->keylatch;
    /* EF pins are active-low: TRUE = not pressed, FALSE = pressed */
    s->cpu.EF[2] = !(latch < 10 && s->keys_a[latch]); /* EF3 = Player A */
    s->cpu.EF[3] = !(latch < 10 && s->keys_b[latch]); /* EF4 = Player B */
}

/* ── Main loop ───────────────────────────────────────────────────────────── */

static void studio2_poll_vnc(RcaStudio2State *s) {
    if (!s->vnc) return;
    GemuVncKeyEvent ev;
    while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
        bool down = ev.down;
        uint32_t k = ev.keysym;
        for (int i = 0; i < 10; i++) {
            if (k == vnc_keys_a[i]) { s->keys_a[i] = down; break; }
            if (k == vnc_keys_b[i]) { s->keys_b[i] = down; break; }
        }
    }
}

void rca_studio2_run(RcaStudio2State *s, const RcaConfig *cfg) {
    GemuDisplay *display = gemu_display_create(cfg->display_type,
        &(GemuDisplayConfig){
            .title       = "GEMU",
            .fb_width    = STUDIO2_DISPLAY_W,
            .fb_height   = STUDIO2_DISPLAY_H,
            .scale       = cfg->display_scale,
            .renderer    = GEMU_RENDERER_AUTO,
            .actions     = studio2_actions,
            .n_actions   = STUDIO2_N_ACTIONS,
            .ini_section = "studio2",
            .gtk         = &(GemuDisplayGtkExtras){ .monitor = s->monitor },
        });
    if (!display) {
        fprintf(stderr, "studio2: failed to create display\n");
        return;
    }
    gemu_monitor_set_input_reset_cb(s->monitor, gemu_display_reset_input_bindings_ud, display);
    gemu_monitor_start(s->monitor);

    const unsigned mcycles_per_frame = s->vdc.lines_total * CDP1861_MCYCLES_PER_LINE;
    const unsigned frame_ms = (s->vdc.lines_total == CDP1861_PAL_LINES_TOTAL) ? 20u : 16u;
    uint32_t argb[STUDIO2_DISPLAY_W * STUDIO2_DISPLAY_H];
    bool quit = false;

    while (!quit) {
        bool reset = false;

        /* Input */
        uint32_t held = gemu_display_poll(display);
        if (gemu_display_should_quit(display)) break;
        if (gemu_display_reset_requested(display)) {
            gemu_display_clear_flags(display);
            reset = true;
        }
        if (held & GEMU_ACTION(20)) reset = true; /* F3 = RESET */

        for (int i = 0; i < 10; i++) {
            s->keys_a[i] = (held >> i) & 1;
            s->keys_b[i] = (held >> (i + 10)) & 1;
        }
        studio2_poll_vnc(s);
        studio2_update_ef(s);

        /* Monitor commands */
        GemuMonCmd cmd = gemu_monitor_poll(s->monitor);
        if (cmd == GEMU_MON_QUIT)   { if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true); else { quit = true; continue; } }
        if (cmd == GEMU_MON_RESET)  reset = true;
        if (cmd == GEMU_MON_CUSTOM) gemu_monitor_unknown_command(s->monitor);

        if (reset) { rca_studio2_reset(s, cfg); continue; }

        if (!gemu_monitor_is_paused(s->monitor)) {
            for (unsigned i = 0; i < mcycles_per_frame; i++) {
                studio2_update_ef(s);
                cdp1802_step(&s->cpu);
            }
        }

        /* Render */
        if (s->draw_flag) {
            s->draw_flag = false;
            for (int i = 0; i < STUDIO2_DISPLAY_W * STUDIO2_DISPLAY_H; i++)
                argb[i] = s->vram[i] ? 0xFFFFFFFFu : 0xFF000000u;
            gemu_display_render(display, argb, STUDIO2_DISPLAY_W, STUDIO2_DISPLAY_H);
            if (s->vnc) {
                static uint8_t px[STUDIO2_DISPLAY_W * STUDIO2_DISPLAY_H];
                for (int i = 0; i < STUDIO2_DISPLAY_W * STUDIO2_DISPLAY_H; i++)
                    px[i] = s->vram[i] ? 1u : 0u;
                gemu_vnc_update(s->vnc, px, STUDIO2_DISPLAY_W, STUDIO2_DISPLAY_H);
            }
        }

        studio2_sleep_ms(frame_ms);
    }

    printf("gemu-rca: %llu machine cycles, %llu instructions\n",
           (unsigned long long)s->cpu.cycle_count,
           (unsigned long long)s->cpu.insn_count);
    gemu_monitor_stop(s->monitor);
    gemu_display_destroy(display);
}
