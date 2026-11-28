#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif
#include "studio2.h"
#include "cdp1802_dev.h"
#include "cdp1861_dev.h"
#include "audio/tone.h"
#include "gemu/audio_out.h"
#include "gemu/dev/device.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/screen.h"
#include "gemu/dev/space.h"
#include "gemu/dev/space_cache.h"
#include "gemu/dev/state.h"
#include "gemu/dev/stream.h"
#include "gemu/util.h"
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

static void rca_studio2_reset(RcaStudio2State *s, const RcaConfig *cfg);

#define STUDIO2_FRAME_HZ       60u

/* 300 Hz beeper as used in MAME's Studio II driver */
#define STUDIO2_BEEP_HZ  300u

/* ST2 cartridge file format */
#define ST2_HEADER_SIZE  0x100u
#define ST2_MAGIC        "RCA2"

/* ── Memory callbacks ────────────────────────────────────────────────────── */

/* The decode, for the monitor's view (the CPU goes through the bus). */
static uint8_t studio2_decode(const RcaStudio2State *s, uint16_t addr) {
    if (addr < 0x0400)
        return s->rom[addr];
    if (addr < 0x0800)
        return s->cart_loaded ? s->cart[addr - 0x0400] : s->rom[addr];
    if (addr >= 0x0C00 && addr < 0x1000 && s->cart_c00)
        return s->cart[0x0800 + (addr - 0x0C00)];
    return s->ram[addr & STUDIO2_RAM_MASK];
}

/* Monitor memory view: the decode is plain array lookups. */
static bool studio2_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    if (addr > 0xffffu) return false;
    *val = studio2_decode(ud, (uint16_t)addr);
    return true;
}

static bool studio2_mon_write(void *ud, uint64_t addr, uint8_t val) {
    RcaStudio2State *s = ud;
    if (addr > 0xffffu || addr < 0x0800) return false;              /* ROM / cart */
    if (addr >= 0x0C00 && addr < 0x1000 && s->cart_c00) return false; /* cart bank 2 */
    s->ram[addr & STUDIO2_RAM_MASK] = val;
    return true;
}

static void studio2_mon_cpu_state(void *ud, char *buf, size_t len) {
    const RcaStudio2State *s = ud;
    cdp1802_format_state(s->cpu, buf, len);
}

/* ── DMA: CDP1861 → vram ─────────────────────────────────────────────────── */

static void studio2_dma_out(uint8_t *data, void *ud) {
    RcaStudio2State *s = ud;
    Cdp1861 *vdc = s->vdc;

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
        cdp1861_set_display(s->vdc, true);
        return 0xFF;
    }
    return 0xFF;
}

static void studio2_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaStudio2State *s = ud;
    if (port == 1) {
        cdp1861_set_display(s->vdc, false);
    } else if (port == 2) {
        s->keylatch = val & 0x0Fu;
    }
}

static void studio2_sync(void *ud) {
    RcaStudio2State *s = ud;
    cdp1861_sync(s->vdc, s->cpu);
}

static void studio2_tone_gate(RcaStudio2State *s, uint8_t q);
static void studio2_maps_sync(RcaStudio2State *s);

static void studio2_q_out(uint8_t q, void *ud) {
    RcaStudio2State *s = ud;
    studio2_tone_gate(s, q);
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
    studio2_maps_sync(s);
    return GEMU_MEDIA_OK_RESET;
}

static GemuMediaResult studio2_media_eject_cart(void *ud,
                                                char *err, size_t err_len) {
    (void)err;
    (void)err_len;
    studio2_eject_cart((RcaStudio2State *)ud);
    studio2_maps_sync((RcaStudio2State *)ud);
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
    return gemu_screendump_mono(path, s->vram,
                                STUDIO2_DISPLAY_W, STUDIO2_DISPLAY_H);
}

static bool studio2_core_build(RcaStudio2State *s);
static void studio2_core_free(RcaStudio2State *s);
static void studio2_maps_sync(RcaStudio2State *s);

/* ── The core machine (stage R3) ─────────────────────────────────────────────
 * board (studio2-board): the RAM, the CDP1861's frame, the key latch;
 * cpu (cdp1802): the machine's executor at 220,000 machine cycles a second
 * (studio2_cycle: the keypads on EF3/EF4, then the CPU's cycle); vdc
 * (cdp1861, NTSC or PAL); tone (with sound): Q's 300 Hz beeper.
 * The bus, "program": the ROM at $0000; at $0400 the cartridge if one is in,
 * else the ROM; the cartridge's second bank at $0C00 on the clones that
 * have one; the 512 bytes of RAM repeated everywhere else from $0800 (as
 * the board decodes it).  Writes below $0800 are dropped.
 * The board's "screen" is the 64x32 frame. */
typedef struct RcaStudio2Core {
    GemuMachine   *machine;
    GemuDevice    *board, *cpu, *vdc, *tone;
    GemuSpace     *bus;
    GemuSpaceCache cache;
    GemuMapping   *low, *c00;          /* $0400-$07FF, $0C00-$0FFF */
    GemuScreen    *screen;
    GemuAudioOut  *audio;
} RcaStudio2Core;

#define STUDIO2_MCYCLE_HZ CDP1802_MCYCLES_PER_SEC

static void studio2_tone_gate(RcaStudio2State *s, uint8_t q) {
    if (s->core && s->core->tone) gemu_tone_set_gate(s->core->tone, q);
}

static uint8_t studio2_bus_read(uint16_t addr, void *ud) {
    return gemu_space_cache_read8(&((RcaStudio2State *)ud)->core->cache, addr);
}
static void studio2_bus_write(uint16_t addr, uint8_t val, void *ud) {
    gemu_space_cache_write8(&((RcaStudio2State *)ud)->core->cache, addr, val);
}

/* The cartridge's windows, after it goes in or out. */
static void studio2_maps_sync(RcaStudio2State *s) {
    RcaStudio2Core *c = s->core;
    if (!c || !c->bus) return;
    gemu_mapping_set_ptr(c->low, s->cart_loaded ? s->cart : s->rom + 0x0400);
    gemu_mapping_enable(c->c00, s->cart_c00);
}

static bool studio2_bus_build(RcaStudio2State *s) {
    RcaStudio2Core *c = s->core;
    GemuSpace *b = c->bus = gemu_space_new(c->machine, "program", 16, 8, GEMU_BIG_ENDIAN);
    if (!b) return false;
    c->cache.space = b;
    for (uint32_t a = 0x0800; a < 0x10000u; a += STUDIO2_RAM_SIZE)
        if (!gemu_space_map_ram(b, a, STUDIO2_RAM_SIZE, s->ram, 0, 0)) return false;
    bool ok = gemu_space_map_ram(b, 0x0000, 0x0400, s->rom, GEMU_MAP_READONLY, 0) &&
              (c->low = gemu_space_map_ram(b, 0x0400, 0x0400, s->rom + 0x0400, GEMU_MAP_READONLY, 0)) &&
              (c->c00 = gemu_space_map_ram(b, 0x0C00, 0x0400, s->cart + 0x0800, GEMU_MAP_READONLY, 1));
    if (ok) studio2_maps_sync(s);
    return ok;
}

static void studio2_board_describe(GemuDevice *d, char *buf, size_t len) {
    const RcaStudio2State *s = *(RcaStudio2State **)d->priv;
    snprintf(buf, len, "%s; cartridge %s%s; key latch %u",
             s->vdc->lines_total == CDP1861_PAL_LINES_TOTAL ? "PAL" : "NTSC",
             s->cart_loaded ? "in" : "out", s->cart_c00 ? " (+$0C00 bank)" : "", s->keylatch);
}

static const GemuDeviceType studio2_board_type = {
    .name = "studio2-board", .desc = "RCA Studio II board (RAM, keypads, cartridge slot)",
    .size = sizeof(RcaStudio2State *), .describe = studio2_board_describe,
};

#define B(field) ok &= GEMU_STATE_MEMBER(d, "", s, field)
/* The board's state: the RAM, the frame, the key latch.  Not saved: the
 * ROM and cartridge (loaded), the keys (the host's, every frame). */
static bool studio2_board_state(RcaStudio2State *s, GemuDevice *d) {
    bool ok = true;
    B(ram); B(vram); B(keylatch);
    return ok;
}
#undef B

static void studio2_update_ef(RcaStudio2State *s);
static void studio2_cycle(RcaStudio2State *s) {
    studio2_update_ef(s);
    cdp1802_step(s->cpu);
}
static bool studio2_stop_before(void *ud) {
    RcaStudio2State *s = ud;
    return gemu_monitor_check_exec(s->monitor, s->cpu->R[s->cpu->P]);
}
static void studio2_hook_cycle(void *ud) { studio2_cycle(ud); }
static bool studio2_stop_after(void *ud) { return gemu_monitor_is_paused(((RcaStudio2State *)ud)->monitor); }

static void studio2_screen_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    const RcaStudio2State *s = ud;
    for (int y = clip->y0; y < clip->y1; y++)
        for (int x = 0; x < STUDIO2_DISPLAY_W; x++)
            bmp->pix[(size_t)y * (size_t)bmp->stride + (size_t)x] =
                s->vram[y * STUDIO2_DISPLAY_W + x] ? 0xFFFFFFFFu : 0xFF000000u;
}

static void studio2_sound_out(void *ud, const float *samples, size_t frames) {
    gemu_audio_queue(ud, samples, frames);
}

static bool studio2_core_build(RcaStudio2State *s) {
    const RcaConfig *cfg = s->cfg;
    RcaStudio2Core *c = s->core = calloc(1, sizeof *c);
    if (!c || !(c->machine = gemu_machine_new("studio2")))
        return false;
    char tone_opts[32];
    snprintf(tone_opts, sizeof tone_opts, "freq=%u", STUDIO2_BEEP_HZ);
    bool ok = (c->board = gemu_device_add(c->machine, &studio2_board_type, "board", NULL, NULL)) &&
              (c->cpu = gemu_device_add(c->machine, &gemu_cdp1802_type, "cpu", NULL, NULL)) &&
              (c->vdc = gemu_device_add(c->machine, &gemu_cdp1861_type, "vdc", NULL, NULL));
    if (ok && cfg->sound_hw != RCA_SOUND_NONE)
        ok = (c->tone = gemu_device_add(c->machine, &gemu_tone_type, "tone", NULL, tone_opts));
    if (ok) {
        *(RcaStudio2State **)c->board->priv = s;
        ok = studio2_board_state(s, c->board);
    }
    if (!ok) {
        fprintf(stderr, "studio2: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    bool inited = gemu_machine_init(c->machine);
    if (inited) {
        s->cpu = gemu_cdp1802(c->cpu);
        s->vdc = gemu_cdp1861(c->vdc);
        Cdp1802 *cpu = s->cpu;
        cpu->io_in     = studio2_io_in;
        cpu->io_out    = studio2_io_out;
        cpu->mem_read  = studio2_bus_read;
        cpu->mem_write = studio2_bus_write;
        cpu->on_sync   = studio2_sync;
        cpu->q_out     = studio2_q_out;
        cpu->io_ud     = s;
        gemu_cdp1861_set_dma(c->vdc, studio2_dma_out, s);
        gemu_cdp1802_set_dma_target(c->cpu, studio2_dma_out, s);
        s->vdc->lines_total = cfg->tv_mode == RCA_TV_PAL ? CDP1861_PAL_LINES_TOTAL : CDP1861_LINES_TOTAL;
    }
    GemuCdp1802Hooks h = { .stop_before = studio2_stop_before, .cycle = studio2_hook_cycle,
                           .stop_after = studio2_stop_after, .ud = s };
    ok = inited && studio2_bus_build(s) && gemu_cdp1802_exec(c->cpu, STUDIO2_MCYCLE_HZ, &h) &&
         (c->screen = gemu_screen_new(c->board, "screen")) &&
         gemu_screen_set_size(c->screen, STUDIO2_DISPLAY_W, STUDIO2_DISPLAY_H);
    if (ok) {
        gemu_screen_set_update(c->screen, studio2_screen_update, s);
        if (c->tone) c->audio = gemu_audio_open("studio2", 44100);
        gemu_mixer_set_output(c->machine, 44100, 1, c->audio ? studio2_sound_out : NULL, c->audio);
    }
    if (!ok || !gemu_machine_start(c->machine)) {
        fprintf(stderr, "studio2: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    gemu_monitor_attach_machine(s->monitor, c->machine, "program");
    return true;
}

static void studio2_core_free(RcaStudio2State *s) {
    if (!s->core) return;
    gemu_machine_free(s->core->machine);
    gemu_audio_close(s->core->audio);
    free(s->core);
    s->core = NULL;
}

/* One frame (the CDP1861's lines of 14 machine cycles): the keys on EF3/
 * EF4, the machine run for the frame's time (a stop leaves it to finish
 * next time), the frame's sound out. */
static void studio2_frame(RcaStudio2State *s) {
    RcaStudio2Core *c = s->core;
    studio2_update_ef(s);
    uint64_t n = (uint64_t)s->vdc->lines_total * CDP1861_MCYCLES_PER_LINE;
    gemu_machine_run_until(c->machine, gemu_machine_time(c->machine) + gemu_clocks(STUDIO2_MCYCLE_HZ, n));
    gemu_mixer_run(c->machine);
    gemu_screen_frame_begin(c->screen);
}

RcaStudio2State *rca_studio2_create(const RcaConfig *cfg) {
    RcaStudio2State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;

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

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_screendump_cb(s->monitor, studio2_screendump, s);
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = studio2_mon_read, .write = studio2_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = true });
    gemu_monitor_set_cpu_state_cb(s->monitor, studio2_mon_cpu_state, s);
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

    if (!studio2_core_build(s)) {
        rca_studio2_destroy(s);
        return NULL;
    }
    rca_studio2_reset(s, cfg);
    return s;
}

static void rca_studio2_reset(RcaStudio2State *s, const RcaConfig *cfg) {
    (void)cfg;
    memset(s->ram, 0, sizeof(s->ram));
    cdp1802_reset(s->cpu);
    cdp1861_reset(s->vdc);
    s->keylatch = 0;
    if (s->core->tone) gemu_tone_set_gate(s->core->tone, false);
}

void rca_studio2_destroy(RcaStudio2State *s) {
    if (!s) return;
    studio2_core_free(s);
    gemu_monitor_destroy(s->monitor);
    free(s);
}

/* ── Action table ────────────────────────────────────────────────────────── */

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
    s->cpu->EF[2] = !(latch < 10 && s->keys_a[latch]); /* EF3 = Player A */
    s->cpu->EF[3] = !(latch < 10 && s->keys_b[latch]); /* EF4 = Player B */
}

/* ── Main loop ───────────────────────────────────────────────────────────── */

void rca_studio2_run(RcaStudio2State *s, const RcaConfig *cfg) {
    GemuDisplay *display = gemu_display_create(cfg->display_type,
        &(GemuDisplayConfig){
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

    const unsigned frame_ms = (s->vdc->lines_total == CDP1861_PAL_LINES_TOTAL) ? 20u : 16u;
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
        studio2_update_ef(s);

        /* Monitor commands */
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT)  { if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true); else quit = true; }
            else if (cmd == GEMU_MON_RESET) reset = true;
            else if (cmd == GEMU_MON_STEP) {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                for (uint32_t k = 0; k < n; k++) {
                    /* cdp1802_step advances one machine cycle; run until an
                     * instruction completes (bounded, in case of IDL). */
                    uint64_t start = s->cpu->insn_count;
                    for (int i = 0; i < 1024 && s->cpu->insn_count == start; i++)
                        studio2_cycle(s);
                }
            }
        }
        if (quit) continue;

        if (reset) { rca_studio2_reset(s, cfg); continue; }
        gemu_display_set_paused(display, gemu_monitor_is_paused(s->monitor));

        bool paused = gemu_monitor_is_paused(s->monitor);
        if (!paused) studio2_frame(s);   /* the cpu device's executor */

        /* Render */
        if (s->draw_flag) {
            s->draw_flag = false;
            const GemuBitmap *b = gemu_screen_render(s->core->screen);
            gemu_display_render(display, b->pix, b->width, b->height);
        }

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
