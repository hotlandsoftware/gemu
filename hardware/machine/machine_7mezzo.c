#ifndef _WIN32
#  define _POSIX_C_SOURCE 199309L
#endif
#include "7mezzo.h"
#include "gemu/screendump.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <direct.h>
#  define gemu_mkdir(p) _mkdir(p)
#  define strncasecmp _strnicmp
#else
#  include <strings.h>
#  include <sys/stat.h>
#  define gemu_mkdir(p) mkdir((p), 0755)
#endif

/* ── Input action table ─────────────────────────────────────────────────── */

static const GemuActionDef mezzo7_actions[MEZZO7_NUM_ACTIONS] = {
    { "coin",    GEMU_ACTION(MEZZO7_ACT_COIN),    "6" },
    { "big",     GEMU_ACTION(MEZZO7_ACT_BIG),     "Right" },
    { "small",   GEMU_ACTION(MEZZO7_ACT_SMALL),   "Left" },
    { "payout",  GEMU_ACTION(MEZZO7_ACT_PAYOUT),  "P" },
    { "take",    GEMU_ACTION(MEZZO7_ACT_TAKE),    "T" },
    { "deal",    GEMU_ACTION(MEZZO7_ACT_DEAL),    "Return" },
    { "stand",   GEMU_ACTION(MEZZO7_ACT_STAND),   "S" },
    { "service", GEMU_ACTION(MEZZO7_ACT_SERVICE), "F2" },
    { "dup",     GEMU_ACTION(MEZZO7_ACT_DUP),     "U" },
    { "bet",     GEMU_ACTION(MEZZO7_ACT_BET),     "B" },
};

/* ── ROM loading helper ─────────────────────────────────────────────────── */

static uint8_t *load_file(const char *path, size_t expect_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "7mezzo: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0 || (size_t)sz != expect_size) {
        fprintf(stderr, "7mezzo: '%s' is %ld bytes, expected %zu\n", path, sz, expect_size);
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc(expect_size);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, expect_size, f) != expect_size) {
        fprintf(stderr, "7mezzo: read error on '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

/* ── CPU bus ─────────────────────────────────────────────────────────────── */

static uint8_t mux_port_r(Mezzo7State *s) {
    switch (s->input_selector) {
    case 0x01: {
        uint8_t v = 0xFF;
        if (s->coin_latch > 0) v &= ~0x01u;
        /* bit1 = Coin2: this board only exposes one coin action in GEMU */
        return v;
    }
    case 0x02: {
        uint32_t h = s->held_actions;
        uint8_t v = 0xFF;
        if (h & GEMU_ACTION(MEZZO7_ACT_BIG))    v &= ~0x01u;
        if (h & GEMU_ACTION(MEZZO7_ACT_SMALL))  v &= ~0x02u;
        if (h & GEMU_ACTION(MEZZO7_ACT_PAYOUT)) v &= ~0x04u;
        if (h & GEMU_ACTION(MEZZO7_ACT_TAKE))   v &= ~0x08u;
        return v;
    }
    case 0x04:
        return 0xFF; /* no named inputs on this row */
    case 0x08: {
        uint32_t h = s->held_actions;
        uint8_t v = 0xFF;
        if (h & GEMU_ACTION(MEZZO7_ACT_DEAL))    v &= ~0x01u;
        if (h & GEMU_ACTION(MEZZO7_ACT_STAND))   v &= ~0x02u;
        if (h & GEMU_ACTION(MEZZO7_ACT_SERVICE)) v &= ~0x04u;
        if (h & GEMU_ACTION(MEZZO7_ACT_DUP))     v &= ~0x08u;
        if (h & GEMU_ACTION(MEZZO7_ACT_BET))     v &= ~0x20u;
        return v;
    }
    case 0x00:
        return s->dsw0;
    }
    return 0xFF;
}

static void mux_port_w(Mezzo7State *s, uint8_t val) {
    s->input_selector = val & 0x0F;
    s->dac_bit = (val & 0x80) != 0;
    /* Bits 4-6 (Coin2/Payout/Coin1) drive physical counters — nothing to
     * drive in GEMU, so just ignored, matching 5clown's convention. */
}

static uint8_t mezzo7_read(uint16_t addr, void *ud) {
    Mezzo7State *s = ud;
    if (addr < 0x0800) return s->nvram[addr];
    if (addr == 0x0801) return s->crtc_regs[s->crtc_addr];
    if (addr >= 0x1000 && addr <= 0x13FF) return s->videoram[addr - 0x1000];
    if (addr >= 0x1800 && addr <= 0x1BFF) return s->colorram[addr - 0x1800];
    if (addr == 0x2800) return mux_port_r(s);
    if (addr >= 0xC000) return s->prg[addr - 0xC000];
    return 0xFF;
}

static void mezzo7_write(uint16_t addr, uint8_t val, void *ud) {
    Mezzo7State *s = ud;
    if (addr < 0x0800) { s->nvram[addr] = val; return; }
    if (addr == 0x0800) { s->crtc_addr = (uint8_t)(val & 0x1F); return; }
    if (addr == 0x0801) { s->crtc_regs[s->crtc_addr] = val; return; }
    if (addr >= 0x1000 && addr <= 0x13FF) { s->videoram[addr - 0x1000] = val; return; }
    if (addr >= 0x1800 && addr <= 0x1BFF) { s->colorram[addr - 0x1800] = val; return; }
    if (addr == 0x3000) { mux_port_w(s, val); return; }
    /* ROM region: no effect */
}

/* ── Video: whole-frame tilemap render ───────────────────────────────────── */

static void mezzo7_render(Mezzo7State *s) {
    /* "Boot check" hardware quirk (verified against the driver's
     * get_7mezzo_tile_info): bit7 of colorram[0] mirrors bit2, or the boot
     * code spins forever thinking a ROM-swap protection check failed. */
    s->colorram[0] = (uint8_t)(s->colorram[0] | ((s->colorram[0] & 0x04) << 5));

    for (int ty = 0; ty < MEZZO7_TILE_ROWS; ty++) {
        for (int tx = 0; tx < MEZZO7_TILE_COLS; tx++) {
            int tile_index = ty * MEZZO7_TILE_COLS + tx;
            uint8_t attr = s->colorram[tile_index];
            uint8_t code = s->videoram[tile_index];
            bool chars = (attr & 0x10) != 0; /* 0 = tiles (3bpp), 1 = chars (1bpp) */
            int color = attr & 0x07;

            for (int ra = 0; ra < 8; ra++) {
                uint8_t plane0, plane1, plane2;
                if (chars) {
                    plane0 = s->gfxbnk0[(code * 8 + ra) & (MEZZO7_GFXBNK0_SIZE - 1)];
                    plane1 = plane2 = 0;
                } else {
                    uint32_t base = (uint32_t)(code * 8 + ra) & 0x7FFu; /* 0x800-byte planes */
                    plane0 = s->gfxbnk1[base];
                    plane1 = s->gfxbnk1[base + 0x800];
                    plane2 = s->gfxbnk1[base + 0x1000];
                }

                int py = ty * 8 + ra;
                uint32_t *row = &s->pixels_argb[py * MEZZO7_FB_WIDTH + tx * 8];
                uint8_t  *idxrow = &s->pixels_idx[py * MEZZO7_FB_WIDTH + tx * 8];
                for (int n = 7; n >= 0; n--) {
                    int bit0 = (plane0 >> n) & 1;
                    int pen;
                    if (chars) {
                        pen = color * 2 + bit0; /* colorbase 0, granularity 2 (1bpp) */
                    } else {
                        int bit1 = (plane1 >> n) & 1, bit2 = (plane2 >> n) & 1;
                        pen = 16 + color * 8 + ((bit2 << 2) | (bit1 << 1) | bit0); /* colorbase 16 */
                    }
                    pen &= (MEZZO7_N_PALETTE - 1);
                    *row++ = s->palette[pen];
                    *idxrow++ = (uint8_t)pen;
                }
            }
        }
    }
}

/* ── 1-bit bitstream DAC ─────────────────────────────────────────────────── */

static void mezzo7_dac_tick(Mezzo7State *s) {
    if (!s->audio_dev) return;
    s->dac_sample_acc += 1.0;
    if (s->dac_sample_acc >= s->dac_clock_pps) {
        s->dac_sample_acc -= s->dac_clock_pps;
        if (s->dac_frame_n < 1024)
            s->dac_frame_buf[s->dac_frame_n++] = s->dac_bit ? 0.5f : -0.5f;
    }
}

static void mezzo7_dac_flush(Mezzo7State *s) {
    if (!s->audio_dev || s->dac_frame_n == 0) { s->dac_frame_n = 0; return; }
    SDL_QueueAudio(s->audio_dev, s->dac_frame_buf, (Uint32)(s->dac_frame_n * (int)sizeof(float)));
    s->dac_frame_n = 0;
}

/* ── Input: local display actions + VNC key -> action mapping ──────────── */

#define XK_Left   0xFF51u
#define XK_Right  0xFF53u
#define XK_Return 0xFF0Du
#define XK_F2     0xFFBFu

static void mezzo7_handle_keys(Mezzo7State *s, uint32_t held) {
    if (s->display) {
        s->held_actions = held;
        uint32_t newly = gemu_display_last_pressed(s->display);
        if (newly & GEMU_ACTION(MEZZO7_ACT_COIN)) s->coin_latch = 15;
    } else if (!s->vnc) {
        s->held_actions = 0;
    }

    if (s->vnc) {
        GemuVncKeyEvent ev;
        while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
            uint32_t bit = 0;
            switch (ev.keysym) {
            case '6':           bit = GEMU_ACTION(MEZZO7_ACT_COIN);    break;
            case XK_Right:      bit = GEMU_ACTION(MEZZO7_ACT_BIG);     break;
            case XK_Left:       bit = GEMU_ACTION(MEZZO7_ACT_SMALL);   break;
            case 'p': case 'P': bit = GEMU_ACTION(MEZZO7_ACT_PAYOUT);  break;
            case 't': case 'T': bit = GEMU_ACTION(MEZZO7_ACT_TAKE);    break;
            case XK_Return:     bit = GEMU_ACTION(MEZZO7_ACT_DEAL);    break;
            case 's': case 'S': bit = GEMU_ACTION(MEZZO7_ACT_STAND);   break;
            case XK_F2:         bit = GEMU_ACTION(MEZZO7_ACT_SERVICE); break;
            case 'u': case 'U': bit = GEMU_ACTION(MEZZO7_ACT_DUP);     break;
            case 'b': case 'B': bit = GEMU_ACTION(MEZZO7_ACT_BET);     break;
            default: break;
            }
            if (!bit) continue;
            if (ev.down) {
                s->held_actions |= bit;
                if (bit == GEMU_ACTION(MEZZO7_ACT_COIN)) s->coin_latch = 15;
            } else {
                s->held_actions &= ~bit;
            }
        }
    }
}

/* ── Screendump ──────────────────────────────────────────────────────────── */

static bool mezzo7_screendump(void *ud, const char *path) {
    Mezzo7State *s = ud;
    int w = MEZZO7_FB_WIDTH, h = MEZZO7_FB_HEIGHT;
    uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return false;
    for (int i = 0; i < w * h; i++) {
        uint32_t c = s->pixels_argb[i];
        rgb[i * 3 + 0] = (uint8_t)(c >> 16);
        rgb[i * 3 + 1] = (uint8_t)(c >> 8);
        rgb[i * 3 + 2] = (uint8_t)(c);
    }
    bool ok = gemu_screendump(path, rgb, w, h);
    free(rgb);
    return ok;
}

/* ── NVRAM persistence ───────────────────────────────────────────────────── */

static void mezzo7_build_sav_path(char *out, size_t len) {
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("APPDATA");
    if (!base || !base[0]) base = "C:\\Users\\Default\\AppData\\Local";
    snprintf(out, len, "%s\\gemu\\7mezzo.nvram", base);
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(out, len, "%s/.gemu/7mezzo.nvram", home);
#endif
}

static void mezzo7_ensure_dir(const char *path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *sep = strrchr(dir, '/');
#ifdef _WIN32
    char *sep2 = strrchr(dir, '\\');
    if (sep2 > sep) sep = sep2;
#endif
    if (sep) { *sep = '\0'; gemu_mkdir(dir); }
}

static void mezzo7_nvram_load(Mezzo7State *s) {
    FILE *f = fopen(s->sav_path, "rb");
    if (!f) return;
    size_t got = fread(s->nvram, 1, sizeof(s->nvram), f);
    fclose(f);
    if (got) printf("7mezzo: loaded NVRAM '%s'\n", s->sav_path);
}

static void mezzo7_nvram_save(Mezzo7State *s) {
    mezzo7_ensure_dir(s->sav_path);
    FILE *f = fopen(s->sav_path, "wb");
    if (!f) { fprintf(stderr, "7mezzo: cannot write NVRAM '%s'\n", s->sav_path); return; }
    fwrite(s->nvram, 1, sizeof(s->nvram), f);
    fclose(f);
}

/* ── Create / destroy ────────────────────────────────────────────────────── */

Mezzo7State *mezzo7_create(const MosConfig *cfg) {
    Mezzo7State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;

    const char *prg_path = NULL, *ns2_path = NULL, *ns1_path = NULL, *ns0_path = NULL;
    for (int i = 0; i < cfg->n_roms; i++) {
        const char *region = cfg->roms[i].region;
        if (!region) continue;
        if      (!strcmp(region, "maincpu")) prg_path = cfg->roms[i].path;
        else if (!strcmp(region, "gfx2"))    ns2_path = cfg->roms[i].path;
        else if (!strcmp(region, "gfx1"))    ns1_path = cfg->roms[i].path;
        else if (!strcmp(region, "gfx0"))    ns0_path = cfg->roms[i].path;
    }
    if (!prg_path || !ns2_path || !ns1_path || !ns0_path) {
        fprintf(stderr, "7mezzo: missing ROM(s) — need maincpu, gfx0, gfx1, gfx2; "
                        "use -rom roms/7mezzo\n");
        free(s);
        return NULL;
    }

    uint8_t *prg = load_file(prg_path, MEZZO7_PRG_SIZE);
    uint8_t *ns2 = load_file(ns2_path, MEZZO7_GFX_FILE_SIZE);
    uint8_t *ns1 = load_file(ns1_path, MEZZO7_GFX_FILE_SIZE);
    uint8_t *ns0 = load_file(ns0_path, MEZZO7_GFX_FILE_SIZE);
    if (!prg || !ns2 || !ns1 || !ns0) {
        free(prg); free(ns2); free(ns1); free(ns0);
        free(s);
        return NULL;
    }
    s->prg = prg;

    /* Combined gfx blob is gfx[0x0000:0x2000]=ns2, [0x2000:0x4000]=ns1,
     * [0x4000:0x6000]=ns0 (per ROM_LOAD order in the driver). Only specific
     * 2KB slices of each file actually feed the tile/char decoders — this
     * is a verified real-hardware quirk (see hardware/7mezzo.h), not a bug:
     *   gfxbnk0 (chars, 1bpp)   = gfx[0x1800:0x2000] = ns2[0x1800:0x2000]
     *   gfxbnk1 plane0 (tiles)  = gfx[0x1000:0x1800] = ns2[0x1000:0x1800]
     *   gfxbnk1 plane1          = gfx[0x3800:0x4000] = ns1[0x1800:0x2000]
     *   gfxbnk1 plane2          = gfx[0x5800:0x6000] = ns0[0x1800:0x2000]
     */
    s->gfxbnk0 = malloc(MEZZO7_GFXBNK0_SIZE);
    s->gfxbnk1 = malloc(MEZZO7_GFXBNK1_SIZE);
    if (!s->gfxbnk0 || !s->gfxbnk1) {
        free(prg); free(ns2); free(ns1); free(ns0);
        free(s->gfxbnk0); free(s->gfxbnk1);
        free(s);
        return NULL;
    }
    memcpy(s->gfxbnk0,          ns2 + 0x1800, 0x800);
    memcpy(s->gfxbnk1,          ns2 + 0x1000, 0x800);
    memcpy(s->gfxbnk1 + 0x800,  ns1 + 0x1800, 0x800);
    memcpy(s->gfxbnk1 + 0x1000, ns0 + 0x1800, 0x800);
    free(ns2); free(ns1); free(ns0);

    /* Palette — hand-coded in the driver, no PROM on this board (only 16 of
     * 32 pens are ever set; the rest default to black, matching upstream). */
    static const struct { int pen; uint32_t rgb; } pal[] = {
        {3, 0xFFFF00}, {5, 0x00FF00}, {7, 0xFF0000}, {9, 0x0000FF},
        {11, 0xFF00FF}, {13, 0x00FFFF}, {15, 0xFFFFFF},
    };
    for (size_t i = 0; i < sizeof(pal) / sizeof(pal[0]); i++)
        s->palette[pal[i].pen] = pal[i].rgb;

    s->dsw0 = 0xFF; /* all DIPs undocumented/unknown on this board; default all-off */

    mos6502_init(&s->cpu);
    s->cpu.mem_read  = mezzo7_read;
    s->cpu.mem_write = mezzo7_write;
    s->cpu.mem_ud    = s;

    s->monitor = gemu_monitor_create();
    if (!s->monitor) { free(s->prg); free(s->gfxbnk0); free(s->gfxbnk1); free(s); return NULL; }
    gemu_monitor_set_screendump_cb(s->monitor, mezzo7_screendump, s);

    mezzo7_build_sav_path(s->sav_path, sizeof(s->sav_path));
    mezzo7_nvram_load(s);

    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr, MEZZO7_FB_WIDTH, MEZZO7_FB_HEIGHT);
        if (s->vnc)
            gemu_vnc_set_palette(s->vnc, s->palette, MEZZO7_N_PALETTE);
        else
            fprintf(stderr, "7mezzo: failed to start VNC at %s\n", cfg->vnc_addr);
    }
    gemu_monitor_set_vnc(s->monitor, s->vnc);

    if (cfg->sound == MOS_SOUND_PCSPK) {
        s->dac_clock_pps = (double)MEZZO7_CPU_HZ / 44100.0;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            SDL_AudioSpec want = { .freq = 44100, .format = AUDIO_F32SYS, .channels = 1, .samples = 512 };
            SDL_AudioSpec have;
            s->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
            if (s->audio_dev) SDL_PauseAudioDevice(s->audio_dev, 0);
            else fprintf(stderr, "7mezzo: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        }
    }

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        s->display = gemu_display_create(cfg->display_type, &(GemuDisplayConfig){
            .title       = "GEMU",
            .fb_width    = MEZZO7_FB_WIDTH,
            .fb_height   = MEZZO7_FB_HEIGHT,
            .scale       = cfg->display_scale,
            .renderer    = cfg->display_renderer,
            .actions     = mezzo7_actions,
            .n_actions   = MEZZO7_NUM_ACTIONS,
            .ini_section = "7mezzo",
        });
    }

    mos6502_reset(&s->cpu);
    return s;
}

void mezzo7_destroy(Mezzo7State *s) {
    if (!s) return;
    mezzo7_nvram_save(s);
    if (s->audio_dev) { SDL_CloseAudioDevice(s->audio_dev); SDL_QuitSubSystem(SDL_INIT_AUDIO); }
    gemu_monitor_destroy(s->monitor);
    gemu_display_destroy(s->display);
    gemu_vnc_destroy(s->vnc);
    free(s->prg);
    free(s->gfxbnk0);
    free(s->gfxbnk1);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

void mezzo7_run(Mezzo7State *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        uint32_t held = 0;
        if (s->display) {
            held = gemu_display_poll(s->display);
            if (gemu_display_should_quit(s->display)) break;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                mezzo7_nvram_save(s);
                mos6502_reset(&s->cpu);
            }
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { quit = true; break; }
            } else if (cmd == GEMU_MON_RESET) {
                mezzo7_nvram_save(s);
                mos6502_reset(&s->cpu);
            } else if (cmd == GEMU_MON_CUSTOM) {
                const char *text = gemu_monitor_command_text(s->monitor);
                while (*text == ' ' || *text == '\t') text++;
                if (strncasecmp(text, "coin", 4) == 0 && (text[4] == '\0' || text[4] == ' ')) {
                    s->coin_latch = 15;
                    printf("7mezzo: coin in\n");
                } else if (strncasecmp(text, "dipswitch", 9) == 0 &&
                           (text[9] == '\0' || text[9] == ' ' || text[9] == '\t')) {
                    const char *p = text + 9;
                    while (*p == ' ' || *p == '\t') p++;
                    unsigned val = 0;
                    if (sscanf(p, "%x", &val) == 1) {
                        s->dsw0 = (uint8_t)val;
                        printf("7mezzo: DSW0 = 0x%02X\n", s->dsw0);
                    } else {
                        printf("7mezzo: DSW0=%02X (all 4 physical switches are undocumented "
                               "even in MAME)\n", s->dsw0);
                        printf("usage: dipswitch <hex-value>\n");
                    }
                } else {
                    gemu_monitor_unknown_command(s->monitor);
                }
            }
        }
        if (quit) break;
        if (s->display) gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));

        mezzo7_handle_keys(s, held);
        if (s->coin_latch > 0) s->coin_latch--;

        if (!gemu_monitor_is_paused(s->monitor)) {
            s->cpu.nmi = true; /* CRTC vsync, once per frame */

            uint64_t target = s->cpu.cycle_count + MEZZO7_CYCLES_PER_FRAME;
            while (s->cpu.cycle_count < target) {
                if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) break;
                uint64_t prev = s->cpu.cycle_count;
                mos6502_step(&s->cpu);
                uint64_t delta = s->cpu.cycle_count - prev;
                for (uint64_t i = 0; i < delta; i++) mezzo7_dac_tick(s);
            }
            mezzo7_dac_flush(s);

            mezzo7_render(s);
            s->frame++;
        }

        if (s->display)
            gemu_display_render(s->display, s->pixels_argb, MEZZO7_FB_WIDTH, MEZZO7_FB_HEIGHT);
        if (s->vnc)
            gemu_vnc_update(s->vnc, s->pixels_idx, MEZZO7_FB_WIDTH, MEZZO7_FB_HEIGHT);

        Uint32 dt = SDL_GetTicks() - t0;
        Uint32 frame_ms = 1000u / 60u;
        if (dt < frame_ms) SDL_Delay(frame_ms - dt);
    }

    mezzo7_nvram_save(s);
    gemu_monitor_stop(s->monitor);
    printf("7mezzo: %llu frames\n", (unsigned long long)s->frame);
}
