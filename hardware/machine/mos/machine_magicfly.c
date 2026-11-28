#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif
#include "magicfly.h"
#include "gemu/util.h"
#include "gemu/screendump.h"
#include "mos_core.h"
#include "mos6502_dev.h"
#include "gemu/dev/device.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/screen.h"
#include "gemu/dev/space.h"
#include "gemu/dev/state.h"
#include "gemu/dev/stream.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define strncasecmp _strnicmp
#else
#  include <strings.h>
#  include <sys/stat.h>
#endif

/* ── Input action tables (per game - different games, different controls) ── */

static const GemuActionDef magicfly_actions_mf[] = {
    { "coin",     GEMU_ACTION(MAGICFLY_ACT_COIN),      "6" },
    { "payout",   GEMU_ACTION(MAGICFLY_ACT_PAYOUT),    "Q" },
    { "joyright", GEMU_ACTION(MAGICFLY_ACT_JOY_RIGHT), "Right" },
    { "joyleft",  GEMU_ACTION(MAGICFLY_ACT_JOY_LEFT),  "Left" },
    { "deal",     GEMU_ACTION(MAGICFLY_ACT_DEAL),      "C" },
    { "cancel",   GEMU_ACTION(MAGICFLY_ACT_CANCEL),    "V" },
    { "bet10",    GEMU_ACTION(MAGICFLY_ACT_BET10),     "X" },
    { "select",   GEMU_ACTION(MAGICFLY_ACT_SELECT),    "Z" },
    { "service",  GEMU_ACTION(MAGICFLY_ACT_SERVICE),   "F2" },
};

static const GemuActionDef magicfly_actions_7m[] = {
    { "coin",    GEMU_ACTION(MAGICFLY_ACT_COIN),    "6" },
    { "big",     GEMU_ACTION(MAGICFLY_ACT_BIG),     "Right" },
    { "small",   GEMU_ACTION(MAGICFLY_ACT_SMALL),   "Left" },
    { "payout",  GEMU_ACTION(MAGICFLY_ACT_PAYOUT),  "P" },
    { "take",    GEMU_ACTION(MAGICFLY_ACT_TAKE),    "T" },
    { "deal",    GEMU_ACTION(MAGICFLY_ACT_DEAL),    "Return" },
    { "stand",   GEMU_ACTION(MAGICFLY_ACT_STAND),   "S" },
    { "service", GEMU_ACTION(MAGICFLY_ACT_SERVICE), "F2" },
    { "dup",     GEMU_ACTION(MAGICFLY_ACT_DUP),     "U" },
    { "bet",     GEMU_ACTION(MAGICFLY_ACT_BET),     "B" },
};

/* ── ROM loading helper ─────────────────────────────────────────────────── */

static uint8_t *load_file(const char *path, size_t expect_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "magicfly: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0 || (size_t)sz != expect_size) {
        fprintf(stderr, "magicfly: '%s' is %ld bytes, expected %zu\n", path, sz, expect_size);
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc(expect_size);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, expect_size, f) != expect_size) {
        fprintf(stderr, "magicfly: read error on '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

/* ── CPU bus ─────────────────────────────────────────────────────────────── */

static uint8_t mux_port_r(MagicflyState *s) {
    bool mf = !s->cfg->is_7mezzo;
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
        if (mf) {
            if (h & GEMU_ACTION(MAGICFLY_ACT_PAYOUT)) v &= ~0x04u;
        } else {
            if (h & GEMU_ACTION(MAGICFLY_ACT_BIG))    v &= ~0x01u;
            if (h & GEMU_ACTION(MAGICFLY_ACT_SMALL))  v &= ~0x02u;
            if (h & GEMU_ACTION(MAGICFLY_ACT_PAYOUT)) v &= ~0x04u;
            if (h & GEMU_ACTION(MAGICFLY_ACT_TAKE))   v &= ~0x08u;
        }
        return v;
    }
    case 0x04: {
        if (!mf) return 0xFF; /* no named inputs on this row for 7mezzo */
        uint32_t h = s->held_actions;
        uint8_t v = 0xFF;
        if (h & GEMU_ACTION(MAGICFLY_ACT_JOY_RIGHT)) v &= ~0x04u;
        if (h & GEMU_ACTION(MAGICFLY_ACT_JOY_LEFT))  v &= ~0x20u;
        return v;
    }
    case 0x08: {
        uint32_t h = s->held_actions;
        uint8_t v = 0xFF;
        if (mf) {
            if (h & GEMU_ACTION(MAGICFLY_ACT_DEAL))    v &= ~0x01u;
            if (h & GEMU_ACTION(MAGICFLY_ACT_CANCEL))  v &= ~0x02u;
            if (h & GEMU_ACTION(MAGICFLY_ACT_SERVICE)) v &= ~0x04u;
            if (h & GEMU_ACTION(MAGICFLY_ACT_BET10))   v &= ~0x08u;
            if (h & GEMU_ACTION(MAGICFLY_ACT_SELECT))  v &= ~0x20u;
        } else {
            if (h & GEMU_ACTION(MAGICFLY_ACT_DEAL))    v &= ~0x01u;
            if (h & GEMU_ACTION(MAGICFLY_ACT_STAND))   v &= ~0x02u;
            if (h & GEMU_ACTION(MAGICFLY_ACT_SERVICE)) v &= ~0x04u;
            if (h & GEMU_ACTION(MAGICFLY_ACT_DUP))     v &= ~0x08u;
            if (h & GEMU_ACTION(MAGICFLY_ACT_BET))     v &= ~0x20u;
        }
        return v;
    }
    case 0x00:
        return s->dsw0;
    }
    return 0xFF;
}

static void mux_port_w(MagicflyState *s, uint8_t val) {
    s->input_selector = val & 0x0F;
    s->dac_bit = (val & 0x80) != 0;
    /* Bits 4-6 (Coin2/Payout/Coin1) drive physical counters - nothing to
     * drive in GEMU, so just ignored, matching 5clown's convention. */
}

/* ── The bus (stage M6) ──────────────────────────────────────────────────────
 * "program": NVRAM $0000, video RAM $1000, colour RAM $1800 and the program
 * ROM ($C000, read-only) as memory; handlers for the CRTC ($0800-$0801),
 * the input mux ($2800, read) and the mux select / DAC ($3000, write).
 * Anything else reads $FF. */
static uint8_t crtc_r(void *ud, uint64_t off) {
    MagicflyState *s = ud;
    return off == 1 ? s->crtc_regs[s->crtc_addr] : 0xFF;
}
static void crtc_w(void *ud, uint64_t off, uint8_t val) {
    MagicflyState *s = ud;
    if (off == 0) s->crtc_addr = (uint8_t)(val & 0x1F);
    else          s->crtc_regs[s->crtc_addr] = val;
}
static uint8_t mux_r(void *ud, uint64_t off) { (void)off; return mux_port_r(ud); }
static void mux_w(void *ud, uint64_t off, uint8_t val) { (void)off; mux_port_w(ud, val); }
static const GemuHandlers crtc_handlers = { .r8 = crtc_r, .w8 = crtc_w };
static const GemuHandlers mux_r_handlers = { .r8 = mux_r };
static const GemuHandlers mux_w_handlers = { .w8 = mux_w };

static uint8_t magicfly_read(uint16_t addr, void *ud);
static void    magicfly_write(uint16_t addr, uint8_t val, void *ud);
static bool    magicfly_core_build(MagicflyState *s);
static void    magicfly_core_free(MagicflyState *s);

/* ── Video: whole-frame tilemap render ───────────────────────────────────── */

static void magicfly_render(MagicflyState *s) {
    bool mf = !s->cfg->is_7mezzo;

    /* "Boot check" hardware quirk (verified against the driver's
     * get_magicfly_tile_info / get_7mezzo_tile_info): bit7 of colorram[0]
     * mirrors bit3 (Magic Fly) or bit2 (7 e Mezzo), or the boot code spins
     * forever thinking a ROM-swap protection check failed. */
    if (mf)
        s->colorram[0] = (uint8_t)(s->colorram[0] | ((s->colorram[0] & 0x08) << 4));
    else
        s->colorram[0] = (uint8_t)(s->colorram[0] | ((s->colorram[0] & 0x04) << 5));

    for (int ty = 0; ty < MAGICFLY_TILE_ROWS; ty++) {
        for (int tx = 0; tx < MAGICFLY_TILE_COLS; tx++) {
            int tile_index = ty * MAGICFLY_TILE_COLS + tx;
            uint8_t attr = s->colorram[tile_index];
            uint8_t code = s->videoram[tile_index];
            bool chars = (attr & 0x10) != 0; /* 0 = tiles (3bpp), 1 = chars (1bpp) */
            int color = mf ? (attr & 0x0F) : (attr & 0x07);

            for (int ra = 0; ra < 8; ra++) {
                uint8_t plane0, plane1, plane2;
                if (chars) {
                    plane0 = s->gfxbnk0[(code * 8 + ra) & (MAGICFLY_GFXBNK0_SIZE - 1)];
                    plane1 = plane2 = 0;
                } else {
                    uint32_t base = (uint32_t)(code * 8 + ra) & 0x7FFu; /* 0x800-byte planes */
                    plane0 = s->gfxbnk1[base];
                    plane1 = s->gfxbnk1[base + 0x800];
                    plane2 = s->gfxbnk1[base + 0x1000];
                }

                int py = ty * 8 + ra;
                uint32_t *row = &s->pixels_argb[py * MAGICFLY_FB_WIDTH + tx * 8];
                for (int n = 7; n >= 0; n--) {
                    int bit0 = (plane0 >> n) & 1;
                    int pen;
                    if (chars) {
                        /* charlayout_1bpp: GFXDECODE_ENTRY(..., 0, 8) -> colorbase 0,
                         * 8 color groups of granularity 2 -> color really matters here. */
                        pen = (color % 8) * 2 + bit0;
                    } else {
                        /* tilelayout: GFXDECODE_ENTRY(..., 16, 1) -> colorbase 16, only
                         * ONE color group (granularity 8). MAME's tilemap_t::pen_data()
                         * computes colorbase() + granularity()*(color % colors()), and
                         * colors()==1 here, so the "color" attribute bits are always
                         * folded to group 0 - verified against emu/tilemap.h. The tile
                         * bank therefore always renders through pens 16-23 regardless
                         * of the attribute's color nibble (those pens are never assigned
                         * by magicfly_palette()/_7mezzo_palette(), so this layer is
                         * black on real hardware too). */
                        int bit1 = (plane1 >> n) & 1, bit2 = (plane2 >> n) & 1;
                        pen = 16 + ((bit2 << 2) | (bit1 << 1) | bit0);
                    }
                    pen &= (MAGICFLY_N_PALETTE - 1);
                    *row++ = s->palette[pen];
                }
            }
        }
    }
}

/* ── 1-bit bitstream DAC ─────────────────────────────────────────────────── */

static void magicfly_dac_tick(MagicflyState *s) {
    if (!s->audio_dev) return;
    s->dac_sample_acc += 1.0;
    if (s->dac_sample_acc >= s->dac_clock_pps) {
        s->dac_sample_acc -= s->dac_clock_pps;
        if (s->dac_frame_n < 1024)
            s->dac_frame_buf[s->dac_frame_n++] = s->dac_bit ? 0.5f : -0.5f;
    }
}

/* ── Input: local display actions (VNC input is merged in by the display) ── */

static void magicfly_handle_keys(MagicflyState *s, uint32_t held) {
    if (s->display) {
        s->held_actions = held;
        uint32_t newly = gemu_display_last_pressed(s->display);
        if (newly & GEMU_ACTION(MAGICFLY_ACT_COIN)) s->coin_latch = 15;
    } else {
        s->held_actions = 0;
    }
}

/* ── Screendump ──────────────────────────────────────────────────────────── */

static bool magicfly_screendump(void *ud, const char *path) {
    MagicflyState *s = ud;
    return gemu_screendump_argb(path, s->pixels_argb,
                                MAGICFLY_FB_WIDTH, MAGICFLY_FB_HEIGHT);
}

/* ── NVRAM persistence ───────────────────────────────────────────────────── */

/* ── Monitor ─────────────────────────────────────────────────────────────── */

/* Memory view: NVRAM, video/colour RAM and program ROM.  The CRTC and the
 * multiplexed input port are not exposed. */
static bool magicfly_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    MagicflyState *s = ud;
    if (addr > 0xFFFF) return false;
    if (addr < 0x0800) { *val = s->nvram[addr]; return true; }
    if (addr >= 0x1000 && addr <= 0x13FF) { *val = s->videoram[addr - 0x1000]; return true; }
    if (addr >= 0x1800 && addr <= 0x1BFF) { *val = s->colorram[addr - 0x1800]; return true; }
    if (addr >= 0xC000) { *val = s->prg[addr - 0xC000]; return true; }
    return false;
}

static bool magicfly_mon_write(void *ud, uint64_t addr, uint8_t val) {
    MagicflyState *s = ud;
    if (addr < 0x0800) { s->nvram[addr] = val; return true; }
    if (addr >= 0x1000 && addr <= 0x13FF) { s->videoram[addr - 0x1000] = val; return true; }
    if (addr >= 0x1800 && addr <= 0x1BFF) { s->colorram[addr - 0x1800] = val; return true; }
    return false;
}

static void magicfly_mon_cpu_state(void *ud, char *buf, size_t len) {
    const MagicflyState *s = ud;
    mos6502_format_state(s->cpu, buf, len);
}

static GemuMonCmd magicfly_cmd_coin(GemuMonitor *mon, const char *args, void *ud) {
    MagicflyState *s = ud;
    (void)args;
    s->coin_latch = 15;
    gemu_monitor_printf(mon, "%s: coin in\n", s->cfg->is_7mezzo ? "7mezzo" : "magicfly");
    return GEMU_MON_NONE;
}

static GemuMonCmd magicfly_cmd_dipswitch(GemuMonitor *mon, const char *args, void *ud) {
    MagicflyState *s = ud;
    bool mf = !s->cfg->is_7mezzo;
    const char *name = mf ? "magicfly" : "7mezzo";
    unsigned val = 0;
    if (sscanf(args, "%x", &val) == 1) {
        s->dsw0 = (uint8_t)val;
        gemu_monitor_printf(mon, "%s: DSW0 = 0x%02X\n", name, s->dsw0);
    } else {
        if (mf)
            gemu_monitor_printf(mon, "%s: DSW0=%02X (bit4: 0=Maximum Bet 100, 1=Maximum Bet 20; "
                                "bits 5-7 undocumented even in MAME)\n", name, s->dsw0);
        else
            gemu_monitor_printf(mon, "%s: DSW0=%02X (all 4 physical switches are undocumented "
                                "even in MAME)\n", name, s->dsw0);
        gemu_monitor_printf(mon, "usage: dipswitch <hex-value>\n");
    }
    return GEMU_MON_NONE;
}

static void magicfly_build_sav_path(char *out, size_t len, const char *name) {
    char file[64];
    snprintf(file, sizeof(file), "%s.nvram", name);
    gemu_config_path(out, len, file);
}

static void magicfly_nvram_load(MagicflyState *s) {
    FILE *f = fopen(s->sav_path, "rb");
    if (!f) return;
    size_t got = fread(s->nvram, 1, sizeof(s->nvram), f);
    fclose(f);
    if (got) printf("magicfly: loaded NVRAM '%s'\n", s->sav_path);
}

static void magicfly_nvram_save(MagicflyState *s) {
    gemu_ensure_parent_dir(s->sav_path);
    FILE *f = fopen(s->sav_path, "wb");
    if (!f) { fprintf(stderr, "magicfly: cannot write NVRAM '%s'\n", s->sav_path); return; }
    fwrite(s->nvram, 1, sizeof(s->nvram), f);
    fclose(f);
}

/* ── Create / destroy ────────────────────────────────────────────────────── */

MagicflyState *magicfly_create(const MosConfig *cfg) {
    MagicflyState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;
    bool mf = !cfg->is_7mezzo;
    const char *name = mf ? "magicfly" : "7mezzo";

    const char *prg_path = NULL, *gfx2_path = NULL, *gfx1_path = NULL, *gfx0_path = NULL;
    for (int i = 0; i < cfg->n_roms; i++) {
        const char *region = cfg->roms[i].region;
        if (!region) continue;
        if      (!strcmp(region, "maincpu")) prg_path = cfg->roms[i].path;
        else if (!strcmp(region, "gfx2"))    gfx2_path = cfg->roms[i].path;
        else if (!strcmp(region, "gfx1"))    gfx1_path = cfg->roms[i].path;
        else if (!strcmp(region, "gfx0"))    gfx0_path = cfg->roms[i].path;
    }
    if (!prg_path || !gfx2_path || !gfx1_path || !gfx0_path) {
        fprintf(stderr, "%s: missing ROM(s) - need maincpu, gfx0, gfx1, gfx2; "
                        "use -rom roms/%s\n", name, name);
        free(s);
        return NULL;
    }

    uint8_t *prg  = load_file(prg_path, MAGICFLY_PRG_SIZE);
    uint8_t *gfx2 = load_file(gfx2_path, MAGICFLY_GFX_FILE_SIZE);
    uint8_t *gfx1 = load_file(gfx1_path, MAGICFLY_GFX_FILE_SIZE);
    uint8_t *gfx0 = load_file(gfx0_path, MAGICFLY_GFX_FILE_SIZE);
    if (!prg || !gfx2 || !gfx1 || !gfx0) {
        free(prg); free(gfx2); free(gfx1); free(gfx0);
        free(s);
        return NULL;
    }
    s->prg = prg;

    /* Combined gfx blob is gfx[0x0000:0x2000]=gfx2, [0x2000:0x4000]=gfx1,
     * [0x4000:0x6000]=gfx0 (per ROM_LOAD order in the driver). Only specific
     * 2KB slices of each file actually feed the tile/char decoders - this
     * is a verified real-hardware quirk (see hardware/mos/magicfly.h), not a
     * bug, and is identical between Magic Fly and 7 e Mezzo:
     *   gfxbnk0 (chars, 1bpp)   = gfx[0x1800:0x2000] = gfx2[0x1800:0x2000]
     *   gfxbnk1 plane0 (tiles)  = gfx[0x1000:0x1800] = gfx2[0x1000:0x1800]
     *   gfxbnk1 plane1          = gfx[0x3800:0x4000] = gfx1[0x1800:0x2000]
     *   gfxbnk1 plane2          = gfx[0x5800:0x6000] = gfx0[0x1800:0x2000]
     */
    s->gfxbnk0 = malloc(MAGICFLY_GFXBNK0_SIZE);
    s->gfxbnk1 = malloc(MAGICFLY_GFXBNK1_SIZE);
    if (!s->gfxbnk0 || !s->gfxbnk1) {
        free(prg); free(gfx2); free(gfx1); free(gfx0);
        free(s->gfxbnk0); free(s->gfxbnk1);
        free(s);
        return NULL;
    }
    memcpy(s->gfxbnk0,          gfx2 + 0x1800, 0x800);
    memcpy(s->gfxbnk1,          gfx2 + 0x1000, 0x800);
    memcpy(s->gfxbnk1 + 0x800,  gfx1 + 0x1800, 0x800);
    memcpy(s->gfxbnk1 + 0x1000, gfx0 + 0x1800, 0x800);
    free(gfx2); free(gfx1); free(gfx0);

    /* Palette - hand-coded in the driver, no PROM on this board (only 16 of
     * 32 pens are ever set; the rest default to black, matching upstream).
     * Magic Fly and 7 e Mezzo use the exact same table except pens 3 and 7
     * are swapped (verified against magicfly_palette()/_7mezzo_palette()). */
    static const struct { int pen; uint32_t rgb; } pal_mf[] = {
        {3, 0xFF0000}, {5, 0x00FF00}, {7, 0xFFFF00}, {9, 0x0000FF},
        {11, 0xFF00FF}, {13, 0x00FFFF}, {15, 0xFFFFFF},
    };
    static const struct { int pen; uint32_t rgb; } pal_7m[] = {
        {3, 0xFFFF00}, {5, 0x00FF00}, {7, 0xFF0000}, {9, 0x0000FF},
        {11, 0xFF00FF}, {13, 0x00FFFF}, {15, 0xFFFFFF},
    };
    if (mf) {
        for (size_t i = 0; i < sizeof(pal_mf)/sizeof(pal_mf[0]); i++)
            s->palette[pal_mf[i].pen] = pal_mf[i].rgb;
    } else {
        for (size_t i = 0; i < sizeof(pal_7m)/sizeof(pal_7m[0]); i++)
            s->palette[pal_7m[i].pen] = pal_7m[i].rgb;
    }

    /* DSW0 default (the board's dsw option's, stage M6): all 4 physical switches (bits 4/6/7 = unknown, bit 4 =
     * Maximum Bet on Magic Fly only) default to their MAME "off" position,
     * i.e. all bits set. */
    s->dsw0 = 0xFF;

    s->monitor = gemu_monitor_create();
    if (!s->monitor) { free(s->prg); free(s->gfxbnk0); free(s->gfxbnk1); free(s); return NULL; }
    gemu_monitor_set_screendump_cb(s->monitor, magicfly_screendump, s);
    gemu_monitor_bind(s->monitor, "coin", magicfly_cmd_coin, s);
    gemu_monitor_bind(s->monitor, "dipswitch", magicfly_cmd_dipswitch, s);
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = magicfly_mon_read, .write = magicfly_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = false });
    gemu_monitor_set_cpu_state_cb(s->monitor, magicfly_mon_cpu_state, s);

    magicfly_build_sav_path(s->sav_path, sizeof(s->sav_path), name);
    magicfly_nvram_load(s);

    if (cfg->sound == MOS_SOUND_PCSPK) {
        s->dac_clock_pps = (double)MAGICFLY_CPU_HZ / 44100.0;
        s->audio_dev = gemu_audio_open(name, 44100);
    }

    if (!magicfly_core_build(s)) {
        magicfly_destroy(s);
        return NULL;
    }

    if (gemu_display_wanted(cfg->display_type)) {
        const GemuActionDef *actions = mf ? magicfly_actions_mf : magicfly_actions_7m;
        int n_actions = mf ? (int)(sizeof(magicfly_actions_mf)/sizeof(magicfly_actions_mf[0]))
                           : (int)(sizeof(magicfly_actions_7m)/sizeof(magicfly_actions_7m[0]));
        s->display = gemu_display_create(cfg->display_type, &(GemuDisplayConfig){
            .fb_width    = MAGICFLY_FB_WIDTH,
            .monitor     = s->monitor,
            .fb_height   = MAGICFLY_FB_HEIGHT,
            .scale       = cfg->display_scale,
            .renderer    = cfg->display_renderer,
            .actions     = actions,
            .n_actions   = n_actions,
            .ini_section = name,
        });
    }

    mos6502_reset(s->cpu);
    return s;
}

void magicfly_destroy(MagicflyState *s) {
    if (!s) return;
    if (s->core) magicfly_nvram_save(s);
    magicfly_core_free(s);
    gemu_audio_close(s->audio_dev);
    gemu_monitor_destroy(s->monitor);
    gemu_display_destroy(s->display);
    free(s->prg);
    free(s->gfxbnk0);
    free(s->gfxbnk1);
    free(s);
}

/* ── The core machine (stage M6) ─────────────────────────────────────────────
 * board (magicfly-board): NVRAM, video and colour RAM, the CRTC's
 * registers, the input mux and DIP switches, the DAC; cpu (mos6502): the
 * machine's executor at 625 kHz, the DAC clocked for each instruction's
 * cycles.  The board's "vsync" line pulses the CPU's nmi once a frame (a
 * timer, every 10,417 cycles).  Its "screen" (256x232) is the tilemap,
 * drawn at each frame's end; its "sound" the DAC's samples, through the
 * mixer. */
typedef struct MagicflyCore {
    GemuMachine *machine;
    GemuDevice  *board, *cpu;
    GemuSpace   *bus;
    MosBus       cache;
    GemuLine    *vsync;
    GemuTimer   *vsync_timer;
    GemuTime     frame_end;
    GemuScreen  *screen;
    GemuStream  *sound;
} MagicflyCore;

#define MAGICFLY_FRAME gemu_clocks(MAGICFLY_CPU_HZ, MAGICFLY_CYCLES_PER_FRAME)

static uint8_t magicfly_read(uint16_t addr, void *ud) {
    MagicflyState *s = ud;
    return mos_bus_read(&s->core->cache, addr);
}
static void magicfly_write(uint16_t addr, uint8_t val, void *ud) {
    MagicflyState *s = ud;
    mos_bus_write(&s->core->cache, addr, val);
}

static bool magicfly_bus_build(MagicflyState *s) {
    MagicflyCore *c = s->core;
    GemuSpace *b = c->bus = gemu_space_new(c->machine, "program", 16, 8, GEMU_LITTLE_ENDIAN);
    if (!b) return false;
    c->cache.space = b;
    return gemu_space_map_ram(b, 0x0000, MAGICFLY_NVRAM_SIZE, s->nvram, 0, 0) &&
           gemu_space_map(b, 0x0800, 2, &crtc_handlers, s, 0, 0) &&
           gemu_space_map_ram(b, 0x1000, MAGICFLY_VRAM_SIZE, s->videoram, 0, 0) &&
           gemu_space_map_ram(b, 0x1800, MAGICFLY_VRAM_SIZE, s->colorram, 0, 0) &&
           gemu_space_map(b, 0x2800, 1, &mux_r_handlers, s, 0, 0) &&
           gemu_space_map(b, 0x3000, 1, &mux_w_handlers, s, 0, 0) &&
           gemu_space_map_ram(b, 0xC000, MAGICFLY_PRG_SIZE, s->prg, GEMU_MAP_READONLY, 0);
}

static void magicfly_board_describe(GemuDevice *d, char *buf, size_t len) {
    const MagicflyState *s = *(MagicflyState **)d->priv;
    snprintf(buf, len, "%s, frame %llu; input select %X, DIP %02X, DAC %d; CRTC R%u",
             s->cfg->is_7mezzo ? "7 e Mezzo" : "Magic Fly", (unsigned long long)s->frame,
             s->input_selector, s->dsw0, s->dac_bit, s->crtc_addr);
}

static void magicfly_board_post_load(GemuDevice *d);
/* The machine's settings (stage M6): the DIP bank, -M magicfly,dsw=0x7F. */
static const GemuOptionDef magicfly_board_opts[] = {
    { "dsw", "DIP switches (input select 0)", GEMU_OPT_INT, "0xFF", 0, 255, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};
static const GemuDeviceType magicfly_board_type = {
    .name = "magicfly-board", .desc = "Magic Fly / 7 e Mezzo board", .size = sizeof(MagicflyState *),
    .describe = magicfly_board_describe, .post_load = magicfly_board_post_load,
    .options = magicfly_board_opts,
};

#define B(field) ok &= GEMU_STATE_MEMBER(d, "", s, field)
/* The board's state.  Not saved: the ROMs and palette, the host's input,
 * the picture (drawn again after a load), the DAC's unsent samples and its
 * rate (configuration). */
static bool magicfly_board_state(MagicflyState *s, GemuDevice *d) {
    bool ok = true;
    B(nvram); B(videoram); B(colorram); B(crtc_addr); B(crtc_regs); B(input_selector); B(dsw0);
    B(coin_latch); B(dac_bit); B(dac_sample_acc); B(frame);
    ok &= GEMU_STATE_VAR(d, "frame_end", s->core->frame_end);
    return ok;
}
#undef B

/* After a load: the picture drawn again, unsent samples dropped. */
static void magicfly_board_post_load(GemuDevice *d) {
    MagicflyState *s = *(MagicflyState **)d->priv;
    magicfly_render(s);
    s->dac_frame_n = 0;
}

static void magicfly_step_insn(MagicflyState *s);
static bool magicfly_stop_before(void *ud) {
    MagicflyState *s = ud;
    return gemu_monitor_check_exec(s->monitor, s->cpu->PC);
}
static void magicfly_insn(void *ud) { magicfly_step_insn(ud); }
static bool magicfly_stop_after(void *ud) {
    return gemu_monitor_is_paused(((MagicflyState *)ud)->monitor);
}

static void magicfly_vsync(void *ud, int param) {
    (void)param;
    MagicflyState *s = ud;
    gemu_line_pulse(s->core->vsync);
    gemu_screen_frame_begin(s->core->screen);
}

static void magicfly_screen_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    const MagicflyState *s = ud;
    for (int y = clip->y0; y < clip->y1; y++)
        memcpy(bmp->pix + (size_t)y * (size_t)bmp->stride, s->pixels_argb + (size_t)y * MAGICFLY_FB_WIDTH,
               MAGICFLY_FB_WIDTH * sizeof(uint32_t));
}

/* The DAC's samples as the mixer asks, in order, none lost (as the NES's
 * APU stream). */
static void magicfly_sound_update(void *ud, float *const *out, unsigned n) {
    MagicflyState *s = ud;
    unsigned have = (unsigned)s->dac_frame_n, take = n < have ? n : have;
    memcpy(out[0], s->dac_frame_buf, take * sizeof(float));
    float last = take ? out[0][take - 1] : (have ? s->dac_frame_buf[0] : 0.0f);
    for (unsigned i = take; i < n; i++) out[0][i] = last;
    if (take < have)
        memmove(s->dac_frame_buf, s->dac_frame_buf + take, (have - take) * sizeof(float));
    s->dac_frame_n = (int)(have - take);
}
static void magicfly_sound_out(void *ud, const float *samples, size_t frames) {
    gemu_audio_queue(ud, samples, frames);
}

static bool magicfly_core_build(MagicflyState *s) {
    MagicflyCore *c = s->core = calloc(1, sizeof *c);
    if (!c || !(c->machine = gemu_machine_new(s->cfg->is_7mezzo ? "7mezzo" : "magicfly")))
        return false;
    static const GemuDeviceType *const set_types[] = { &magicfly_board_type };
    char set[1][GEMU_OPTS_ROUTE_LEN], why[512];
    if (!gemu_opts_route(s->cfg->machine_opts, set_types, 1, set, why, sizeof why)) {
        fprintf(stderr, "gemu: %s\n", why);
        return false;
    }
    static const char *const variant[] = { "6502", "2a03", "2a07", "6501", "6507" };
    char cpu_opts[32];
    snprintf(cpu_opts, sizeof cpu_opts, "variant=%s", variant[s->cfg->cpu]);
    bool ok = (c->board = gemu_device_add(c->machine, &magicfly_board_type, "board", NULL,
                                          set[0][0] ? set[0] : NULL)) &&
              (c->cpu = gemu_device_add(c->machine, &gemu_mos6502_type, "cpu", NULL, cpu_opts));
    if (ok) {
        *(MagicflyState **)c->board->priv = s;
        ok = magicfly_board_state(s, c->board);
    }
    if (!ok) {
        fprintf(stderr, "magicfly: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    s->dsw0 = (uint8_t)gemu_opt_int(c->board, "dsw");
    bool inited = gemu_machine_init(c->machine);
    if (inited) {
        s->cpu = gemu_mos6502(c->cpu);
        s->cpu->mem_read = magicfly_read;
        s->cpu->mem_write = magicfly_write;
        s->cpu->mem_ud = s;
    }
    GemuMos6502Hooks h = { .stop_before = magicfly_stop_before, .insn = magicfly_insn,
                           .stop_after = magicfly_stop_after, .ud = s };
    ok = inited && magicfly_bus_build(s) && gemu_mos6502_exec(c->cpu, MAGICFLY_CPU_HZ, &h) &&
         (c->vsync = gemu_line_out(c->board, "vsync")) && gemu_line_connect(c->vsync, c->cpu, "nmi", 0) &&
         (c->vsync_timer = gemu_timer_new(c->board, magicfly_vsync, s)) &&
         (c->screen = gemu_screen_new(c->board, "screen")) &&
         gemu_screen_set_size(c->screen, MAGICFLY_FB_WIDTH, MAGICFLY_FB_HEIGHT) &&
         (c->sound = gemu_stream_new(c->board, "sound", 1, 44100, magicfly_sound_update, s));
    if (ok) {
        gemu_screen_set_update(c->screen, magicfly_screen_update, s);
        gemu_mixer_set_output(c->machine, 44100, 1, s->audio_dev ? magicfly_sound_out : NULL,
                              s->audio_dev);
    }
    if (!ok || !gemu_machine_start(c->machine)) {
        fprintf(stderr, "magicfly: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    gemu_timer_start(c->vsync_timer, 0, MAGICFLY_FRAME, 0);
    c->frame_end = gemu_machine_time(c->machine) + MAGICFLY_FRAME;
    gemu_monitor_attach_machine(s->monitor, c->machine, "program");
    return true;
}

static void magicfly_core_free(MagicflyState *s) {
    if (!s->core) return;
    gemu_machine_free(s->core->machine);
    free(s->core);
    s->core = NULL;
}

/* One frame: the machine run to the frame's end (the vsync at its start),
 * the frame's sound out through the mixer, the picture drawn.  A stop
 * leaves the frame to finish next time. */
static void magicfly_frame(MagicflyState *s) {
    MagicflyCore *c = s->core;
    gemu_machine_run_until(c->machine, c->frame_end);
    bool done = gemu_machine_time(c->machine) >= c->frame_end;
    gemu_mixer_run(c->machine);
    magicfly_render(s);
    if (done) {
        c->frame_end += MAGICFLY_FRAME;
        s->frame++;
    }
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

/* Execute one CPU instruction and clock the DAC for its cycles. */
static void magicfly_step_insn(MagicflyState *s) {
    uint64_t prev = s->cpu->cycle_count;
    mos6502_step(s->cpu);
    uint64_t delta = s->cpu->cycle_count - prev;
    for (uint64_t i = 0; i < delta; i++) magicfly_dac_tick(s);
}

void magicfly_run(MagicflyState *s, const MosConfig *cfg) {
    bool mf = !cfg->is_7mezzo;
    const char *name = mf ? "magicfly" : "7mezzo";
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        uint32_t held = 0;
        if (s->display) {
            held = gemu_display_poll(s->display);
            if (gemu_display_should_quit(s->display)) break;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                magicfly_nvram_save(s);
                mos6502_reset(s->cpu);
            }
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { quit = true; break; }
            } else if (cmd == GEMU_MON_RESET) {
                magicfly_nvram_save(s);
                mos6502_reset(s->cpu);
            } else if (cmd == GEMU_MON_STEP) {
                for (uint32_t n = gemu_monitor_step_count(s->monitor); n; n--)
                    magicfly_step_insn(s);
                magicfly_render(s);    /* the window follows stepping */
            }
        }
        if (quit) break;
        if (s->display) gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));

        magicfly_handle_keys(s, held);
        if (s->coin_latch > 0) s->coin_latch--;

        bool paused = gemu_monitor_is_paused(s->monitor);
        if (!paused) magicfly_frame(s);

        if (s->display) {
            const GemuBitmap *b = gemu_screen_render(s->core->screen);
            gemu_display_render(s->display, b->pix, b->width, b->height);
        }

        /* Pace to the machine's time (60 frames a second); paused, a
         * frame's delay. */
        if (!paused) {
            gemu_machine_pace(s->core->machine, 0);
        } else {
            gemu_machine_pace_reset(s->core->machine);
            SDL_Delay(1000u / 60u);
        }
    }

    magicfly_nvram_save(s);
    gemu_monitor_stop(s->monitor);
    printf("%s: %llu frames, %llu cpu cycles\n",
           name, (unsigned long long)s->frame, (unsigned long long)s->cpu->cycle_count);
}
