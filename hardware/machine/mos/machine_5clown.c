#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif
#include "5clown.h"
#include "gemu/util.h"
#include "gemu/screendump.h"
#include "mos_core.h"
#include "mos6502_dev.h"
#include "okim6295_state.h"
#include "controller/pia6821_dev.h"
#include "gemu/dev/device.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/screen.h"
#include "gemu/dev/space.h"
#include "gemu/dev/state.h"
#include "gemu/dev/stream.h"
#include <SDL2/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define strcasecmp _stricmp
#  define strncasecmp _strnicmp
#else
#  include <strings.h>
#  include <sys/stat.h>
#endif

/* ── Input action table ─────────────────────────────────────────────────── */

static const GemuActionDef fiveclown_actions[FIVECLOWN_NUM_ACTIONS] = {
    { "bet",     GEMU_ACTION(FIVECLOWN_ACT_BET),     "B" },
    { "record",  GEMU_ACTION(FIVECLOWN_ACT_RECORD),  "R" },
    { "dup",     GEMU_ACTION(FIVECLOWN_ACT_DUP),     "U" },
    { "start",   GEMU_ACTION(FIVECLOWN_ACT_START),   "Return" },
    { "keyout",  GEMU_ACTION(FIVECLOWN_ACT_KEYOUT),  "K" },
    { "payout",  GEMU_ACTION(FIVECLOWN_ACT_PAYOUT),  "P" },
    { "collect", GEMU_ACTION(FIVECLOWN_ACT_COLLECT), "C" },
    { "big",     GEMU_ACTION(FIVECLOWN_ACT_BIG),     "Right" },
    { "small",   GEMU_ACTION(FIVECLOWN_ACT_SMALL),   "Left" },
    { "hold1",   GEMU_ACTION(FIVECLOWN_ACT_HOLD1),   "1" },
    { "hold2",   GEMU_ACTION(FIVECLOWN_ACT_HOLD2),   "2" },
    { "hold3",   GEMU_ACTION(FIVECLOWN_ACT_HOLD3),   "3" },
    { "hold4",   GEMU_ACTION(FIVECLOWN_ACT_HOLD4),   "4" },
    { "hold5",   GEMU_ACTION(FIVECLOWN_ACT_HOLD5),   "5" },
    { "setting", GEMU_ACTION(FIVECLOWN_ACT_SETTING), "F2" },
    { "coin",    GEMU_ACTION(FIVECLOWN_ACT_COIN),    "6" },
    { "keyin",   GEMU_ACTION(FIVECLOWN_ACT_KEYIN),   "7" },
};

/* ── ROM loading helper ─────────────────────────────────────────────────── */

static uint8_t *load_file(const char *path, size_t expect_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "5clown: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0 || (size_t)sz != expect_size) {
        fprintf(stderr, "5clown: '%s' is %ld bytes, expected %zu\n", path, sz, expect_size);
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc(expect_size);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, expect_size, f) != expect_size) {
        fprintf(stderr, "5clown: read error on '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

/* ── The buses (stage M5) ────────────────────────────────────────────────────
 * Main, "program": NVRAM $0000, video RAM $1000, colour RAM $1800 and the
 * program ROM ($2000-$7FFF, $E000-$FFFF, read-only) as memory; handlers for
 * the CRTC ($0800-$0801), the PIAs ($0844, $0848), the DIP banks ($C400,
 * $CC00, $D400) and the sound latch ($D800, write).  Sound, "audio": RAM
 * $0000 and ROM $E000 as memory; handlers for the AY latch ($0800, $0A02),
 * the OKI ($0C04 write, $0C06 read) and the sound latch ($0E06 read).
 * Anything else reads $FF. */

static uint8_t crtc_r(void *ud, uint64_t off) {
    FiveClownState *s = ud;
    return off == 1 ? s->crtc_regs[s->crtc_addr] : 0xFF;
}
static void crtc_w(void *ud, uint64_t off, uint8_t val) {
    FiveClownState *s = ud;
    if (off == 0) s->crtc_addr = (uint8_t)(val & 0x1F);
    else          s->crtc_regs[s->crtc_addr] = val;
}
static uint8_t dip1_r(void *ud, uint64_t off) { (void)off; return ((FiveClownState *)ud)->dip1; }
static uint8_t dip2_r(void *ud, uint64_t off) { (void)off; return ((FiveClownState *)ud)->dip2; }
static uint8_t dip3_r(void *ud, uint64_t off) { (void)off; return ((FiveClownState *)ud)->dip3; }
static void latch_w(void *ud, uint64_t off, uint8_t val) {
    (void)off;
    ((FiveClownState *)ud)->main_latch_d800 = val;
}

static void ay_w(void *ud, uint64_t off, uint8_t val) {      /* $0800 */
    FiveClownState *s = ud;
    (void)off;
    s->snd_latch_0800 = val;
    if (s->cfg->sound_hw_mask & MOS_SOUNDHW_AY8910) {
        if (s->snd_latch_0a02 == 0xC0) s->ay8910_addr = val;
        /* 0x00 mode would latch (ay8910_addr, val) into the real chip;
         * stubbed for now, so nothing further happens. */
    }
}
static void ay_mode_w(void *ud, uint64_t off, uint8_t val) { /* $0A02 */
    (void)off;
    ((FiveClownState *)ud)->snd_latch_0a02 = val;
}
static uint8_t oki_r(void *ud, uint64_t off) {
    FiveClownState *s = ud;
    (void)off;                                        /* $0C06: busy status */
    return s->oki.audio_dev ? oki6295_read(&s->oki) : 0xFF;
}
static void oki_w(void *ud, uint64_t off, uint8_t val) {
    FiveClownState *s = ud;
    (void)off;                                        /* $0C04 */
    if (s->oki.audio_dev) oki6295_write(&s->oki, val);
}
static uint8_t sndlatch_r(void *ud, uint64_t off) {
    (void)off;
    return ((FiveClownState *)ud)->main_latch_d800;   /* $0E06 */
}

static const GemuHandlers crtc_handlers     = { .r8 = crtc_r, .w8 = crtc_w };
static const GemuHandlers dip1_handlers     = { .r8 = dip1_r };
static const GemuHandlers dip2_handlers     = { .r8 = dip2_r };
static const GemuHandlers dip3_handlers     = { .r8 = dip3_r };
static const GemuHandlers latch_handlers    = { .w8 = latch_w };
static const GemuHandlers ay_handlers       = { .w8 = ay_w };
static const GemuHandlers ay_mode_handlers  = { .w8 = ay_mode_w };
static const GemuHandlers oki_r_handlers    = { .r8 = oki_r };
static const GemuHandlers oki_w_handlers    = { .w8 = oki_w };
static const GemuHandlers sndlatch_handlers = { .r8 = sndlatch_r };

static uint8_t main_read(uint16_t addr, void *ud);
static void    main_write(uint16_t addr, uint8_t val, void *ud);
static uint8_t audio_read(uint16_t addr, void *ud);
static void    audio_write(uint16_t addr, uint8_t val, void *ud);

/* ── PIA0: multiplexed inputs + coin counters ───────────────────────────── */

static uint8_t in_group(FiveClownState *s, int g) {
    uint32_t h = s->held_actions;
    uint8_t v = 0xFF;
    switch (g) {
    case 0:
        if (h & GEMU_ACTION(FIVECLOWN_ACT_BET))    v &= ~0x01u;
        if (h & GEMU_ACTION(FIVECLOWN_ACT_RECORD)) v &= ~0x02u;
        if (h & GEMU_ACTION(FIVECLOWN_ACT_DUP))    v &= ~0x04u;
        if (h & GEMU_ACTION(FIVECLOWN_ACT_START))  v &= ~0x08u;
        break;
    case 1:
        if (h & GEMU_ACTION(FIVECLOWN_ACT_KEYOUT))  v &= ~0x01u;
        if (h & GEMU_ACTION(FIVECLOWN_ACT_PAYOUT))  v &= ~0x02u;
        if (h & GEMU_ACTION(FIVECLOWN_ACT_COLLECT)) v &= ~0x04u;
        if (h & GEMU_ACTION(FIVECLOWN_ACT_BIG))     v &= ~0x08u;
        if (h & GEMU_ACTION(FIVECLOWN_ACT_SMALL))   v &= ~0x10u;
        break;
    case 2:
        if (h & GEMU_ACTION(FIVECLOWN_ACT_HOLD1)) v &= ~0x01u;
        if (h & GEMU_ACTION(FIVECLOWN_ACT_HOLD2)) v &= ~0x02u;
        if (h & GEMU_ACTION(FIVECLOWN_ACT_HOLD3)) v &= ~0x04u;
        if (h & GEMU_ACTION(FIVECLOWN_ACT_HOLD4)) v &= ~0x08u;
        if (h & GEMU_ACTION(FIVECLOWN_ACT_HOLD5)) v &= ~0x10u;
        break;
    default: /* group 3 */
        if (h & GEMU_ACTION(FIVECLOWN_ACT_SETTING)) v &= ~0x01u;
        if (s->coin_latch > 0)                      v &= ~0x08u;
        if (h & GEMU_ACTION(FIVECLOWN_ACT_KEYIN))   v &= ~0x10u;
        break;
    }
    return v;
}

static uint8_t pia0_read_pa(void *ud) {
    FiveClownState *s = ud;
    switch (s->mux_data & 0xF0u) {
    case 0x10: return in_group(s, 0);
    case 0x20: return in_group(s, 1);
    case 0x40: return in_group(s, 2);
    case 0x80: return in_group(s, 3);
    }
    return 0xFF;
}

static uint8_t pia0_read_pb(void *ud) { (void)ud; return 0x00; }

static void pia0_write_pb(void *ud, uint8_t val) {
    (void)ud; (void)val; /* coin counters - nothing physical to drive */
}

/* ── PIA1: SW4 + audio-CPU NMI trigger + mux select ─────────────────────── */

static void fiveclown_sound_nmi(FiveClownState *s, bool level);

static uint8_t pia1_read_pa(void *ud) {
    FiveClownState *s = ud;
    return s->dip4;
}

static uint8_t pia1_read_pb(void *ud) { (void)ud; return 0x00; }

static void pia1_write_pa(void *ud, uint8_t val) {
    FiveClownState *s = ud;
    /* Edge-triggered, matching real 6502 NMI hardware and MAME's
     * ASSERT_LINE/CLEAR_LINE handling: only the 0->1 transition fires it. */
    bool assert_line = (val & 0x0Fu) == 0x07u;
    s->audio_nmi_line = assert_line;
    fiveclown_sound_nmi(s, assert_line);   /* the line: a rise is the NMI */
}

static void pia1_write_pb(void *ud, uint8_t val) {
    FiveClownState *s = ud;
    s->mux_data = (uint8_t)(val ^ 0xFFu); /* inverted, per hardware */
}

/* ── Input: local display actions (VNC input is merged in by the display) ── */

static void fiveclown_handle_keys(FiveClownState *s, uint32_t held) {
    if (s->display) {
        s->held_actions = held;
        uint32_t newly = gemu_display_last_pressed(s->display);
        if (newly & GEMU_ACTION(FIVECLOWN_ACT_COIN)) s->coin_latch = 15;
    } else {
        s->held_actions = 0;
    }
}

/* ── Video: whole-frame tilemap render ───────────────────────────────────── */

static void fiveclown_render(FiveClownState *s) {
    for (int ty = 0; ty < FIVECLOWN_TILE_ROWS; ty++) {
        for (int tx = 0; tx < FIVECLOWN_TILE_COLS; tx++) {
            int tile_index = ty * FIVECLOWN_TILE_COLS + tx;
            uint8_t attr = s->colorram[tile_index];
            uint8_t code_byte = s->videoram[tile_index];
            int code  = ((attr & 0x01) << 8) | ((attr & 0x40) << 2) | code_byte;
            int bank  = (attr >> 1) & 1;
            int color = ((attr >> 2) & 0x0F) | ((attr >> 3) & 0x10);

            for (int ra = 0; ra < 8; ra++) {
                uint32_t base = (uint32_t)((code << 3) | ra);
                uint8_t plane0 = bank ? s->gfxbanks[base + 0x4000] : s->gfxbanks[base + 0x7000];
                uint8_t plane1 = bank ? s->gfxbanks[base + 0x5000] : 0;
                uint8_t plane2 = bank ? s->gfxbanks[base + 0x6000] : 0;

                int py = ty * 8 + ra;
                uint32_t *row = &s->pixels_argb[py * FIVECLOWN_FB_WIDTH + tx * 8];
                for (int n = 7; n >= 0; n--) {
                    int idx3 = (((plane2 >> n) & 1) << 2) | (((plane1 >> n) & 1) << 1) | ((plane0 >> n) & 1);
                    uint8_t pen = (uint8_t)(color * 8 + idx3);
                    *row++ = s->palette[pen];
                }
            }
        }
    }
}

/* ── Monitor ─────────────────────────────────────────────────────────────── */

/* Main-CPU memory view: NVRAM, video/colour RAM, program ROM and the DIP
 * switch ports.  CRTC and PIA registers are not exposed. */
static bool fiveclown_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    FiveClownState *s = ud;
    if (addr > 0xFFFF) return false;
    if (addr < 0x0800) { *val = s->nvram[addr]; return true; }
    if (addr >= 0x1000 && addr <= 0x13FF) { *val = s->videoram[addr - 0x1000]; return true; }
    if (addr >= 0x1800 && addr <= 0x1BFF) { *val = s->colorram[addr - 0x1800]; return true; }
    if (addr >= 0x2000 && addr <= 0x7FFF) { *val = s->prg[addr - 0x2000]; return true; }
    if (addr == 0xC400) { *val = s->dip1; return true; }
    if (addr == 0xCC00) { *val = s->dip2; return true; }
    if (addr == 0xD400) { *val = s->dip3; return true; }
    if (addr >= 0xE000) { *val = s->prg[0x6000 + (addr - 0xE000)]; return true; }
    return false;
}

static bool fiveclown_mon_write(void *ud, uint64_t addr, uint8_t val) {
    FiveClownState *s = ud;
    if (addr < 0x0800) { s->nvram[addr] = val; return true; }
    if (addr >= 0x1000 && addr <= 0x13FF) { s->videoram[addr - 0x1000] = val; return true; }
    if (addr >= 0x1800 && addr <= 0x1BFF) { s->colorram[addr - 0x1800] = val; return true; }
    return false;
}

static void fiveclown_mon_cpu_state(void *ud, char *buf, size_t len) {
    const FiveClownState *s = ud;
    int n = snprintf(buf, len, "main:  ");
    if (n > 0 && (size_t)n < len) mos6502_format_state(s->cpu, buf + n, len - (size_t)n);
    n = (int)strlen(buf);
    if ((size_t)n < len) n += snprintf(buf + n, len - (size_t)n, "\naudio: ");
    if (n > 0 && (size_t)n < len) mos6502_format_state(s->audiocpu, buf + n, len - (size_t)n);
}

static GemuMonCmd fiveclown_cmd_coin(GemuMonitor *mon, const char *args, void *ud) {
    FiveClownState *s = ud;
    (void)args;
    s->coin_latch = 15;
    gemu_monitor_printf(mon, "5clown: coin in\n");
    return GEMU_MON_NONE;
}

static GemuMonCmd fiveclown_cmd_dipswitch(GemuMonitor *mon, const char *args, void *ud) {
    FiveClownState *s = ud;
    int bank = 0; unsigned val = 0;
    if (sscanf(args, "%d %x", &bank, &val) == 2 && bank >= 1 && bank <= 4) {
        uint8_t *d = bank == 1 ? &s->dip1 : bank == 2 ? &s->dip2
                   : bank == 3 ? &s->dip3 : &s->dip4;
        *d = (uint8_t)val;
        gemu_monitor_printf(mon, "5clown: SW%d = 0x%02X\n", bank, *d);
    } else {
        gemu_monitor_printf(mon, "5clown: SW1=%02X SW2=%02X SW3=%02X SW4=%02X\n",
                            s->dip1, s->dip2, s->dip3, s->dip4);
        gemu_monitor_printf(mon, "usage: dipswitch <1-4> <hex-value>\n");
    }
    return GEMU_MON_NONE;
}

/* ── Screendump ──────────────────────────────────────────────────────────── */

static bool fiveclown_screendump(void *ud, const char *path) {
    FiveClownState *s = ud;
    return gemu_screendump_argb(path, s->pixels_argb,
                                FIVECLOWN_FB_WIDTH, FIVECLOWN_FB_HEIGHT);
}

/* ── NVRAM persistence ───────────────────────────────────────────────────── */

static void fiveclown_nvram_load(FiveClownState *s) {
    FILE *f = fopen(s->sav_path, "rb");
    if (!f) return;
    size_t got = fread(s->nvram, 1, sizeof(s->nvram), f);
    fclose(f);
    if (got) printf("5clown: loaded NVRAM '%s'\n", s->sav_path);
}

static void fiveclown_nvram_save(FiveClownState *s) {
    gemu_ensure_parent_dir(s->sav_path);
    FILE *f = fopen(s->sav_path, "wb");
    if (!f) { fprintf(stderr, "5clown: cannot write NVRAM '%s'\n", s->sav_path); return; }
    fwrite(s->nvram, 1, sizeof(s->nvram), f);
    fclose(f);
}

/* ── The core machine (stage M5) ─────────────────────────────────────────────
 * board (5clown-board): NVRAM, the sound CPU's RAM, video and colour RAM,
 * the CRTC's registers, the latches, the DIP switches, the input mux;
 * cpu, audiocpu (mos6502): both 6502s, each the machine's executor at
 * 1.25 MHz, interleaved by the scheduler (before, the main CPU ran a
 * frame's cycles, then the sound CPU); pia0, pia1 (pia6821), their IRQs not
 * wired; oki (okim6295-5clown): the OKI's state.
 * Lines: the board's "vsync" pulses the main CPU's nmi once a frame (a
 * timer, every 24,960 cycles: the CRTC's 320x312 raster at 5 MHz); its
 * "sound_nmi" follows PIA1's port A (bits 3-0 = 7) to the sound CPU's nmi.
 * The board's "screen" (256x256) is the tilemap, drawn at each frame's
 * end; the oki's "sound" is the OKI's samples, through the mixer. */
typedef struct FiveClownCore {
    GemuMachine *machine;
    GemuDevice  *board, *cpu, *audiocpu, *pia0, *pia1, *oki;
    GemuSpace   *bus, *audio_bus;
    MosBus       cache, audio_cache;
    GemuLine    *vsync, *sound_nmi;
    GemuTimer   *vsync_timer;
    GemuTime     frame_end;          /* the end of the frame in progress */
    GemuScreen  *screen;
    GemuStream  *sound;
} FiveClownCore;

#define FIVECLOWN_FRAME gemu_clocks(FIVECLOWN_CPU_HZ, FIVECLOWN_CYCLES_PER_FRAME)

static void fiveclown_sound_nmi(FiveClownState *s, bool level) {
    if (s->core) gemu_line_set(s->core->sound_nmi, level);
}

static uint8_t main_read(uint16_t addr, void *ud) {
    FiveClownState *s = ud;
    return mos_bus_read(&s->core->cache, addr);
}
static void main_write(uint16_t addr, uint8_t val, void *ud) {
    FiveClownState *s = ud;
    mos_bus_write(&s->core->cache, addr, val);
}
static uint8_t audio_read(uint16_t addr, void *ud) {
    FiveClownState *s = ud;
    return mos_bus_read(&s->core->audio_cache, addr);
}
static void audio_write(uint16_t addr, uint8_t val, void *ud) {
    FiveClownState *s = ud;
    mos_bus_write(&s->core->audio_cache, addr, val);
}

static bool fiveclown_bus_build(FiveClownState *s) {
    FiveClownCore *c = s->core;
    GemuSpace *b, *a;
    if (!(b = c->bus = gemu_space_new(c->machine, "program", 16, 8, GEMU_LITTLE_ENDIAN)) ||
        !(a = c->audio_bus = gemu_space_new(c->machine, "audio", 16, 8, GEMU_LITTLE_ENDIAN)))
        return false;
    c->cache.space = b;
    c->audio_cache.space = a;
    return gemu_space_map_ram(b, 0x0000, 0x0800, s->nvram, 0, 0) &&
           gemu_space_map(b, 0x0800, 2, &crtc_handlers, s, 0, 0) &&
           gemu_space_map(b, 0x0844, 4, &gemu_pia6821_bus, c->pia0, 0, 0) &&
           gemu_space_map(b, 0x0848, 4, &gemu_pia6821_bus, c->pia1, 0, 0) &&
           gemu_space_map_ram(b, 0x1000, FIVECLOWN_VRAM_SIZE, s->videoram, 0, 0) &&
           gemu_space_map_ram(b, 0x1800, FIVECLOWN_VRAM_SIZE, s->colorram, 0, 0) &&
           gemu_space_map_ram(b, 0x2000, 0x6000, s->prg, GEMU_MAP_READONLY, 0) &&
           gemu_space_map(b, 0xC400, 1, &dip1_handlers, s, 0, 0) &&
           gemu_space_map(b, 0xCC00, 1, &dip2_handlers, s, 0, 0) &&
           gemu_space_map(b, 0xD400, 1, &dip3_handlers, s, 0, 0) &&
           gemu_space_map(b, 0xD800, 1, &latch_handlers, s, 0, 0) &&
           gemu_space_map_ram(b, 0xE000, 0x2000, s->prg + 0x6000, GEMU_MAP_READONLY, 0) &&
           gemu_space_map_ram(a, 0x0000, 0x0800, s->audio_ram, 0, 0) &&
           gemu_space_map(a, 0x0800, 1, &ay_handlers, s, 0, 0) &&
           gemu_space_map(a, 0x0A02, 1, &ay_mode_handlers, s, 0, 0) &&
           gemu_space_map(a, 0x0C04, 1, &oki_w_handlers, s, 0, 0) &&
           gemu_space_map(a, 0x0C06, 1, &oki_r_handlers, s, 0, 0) &&
           gemu_space_map(a, 0x0E06, 1, &sndlatch_handlers, s, 0, 0) &&
           gemu_space_map_ram(a, 0xE000, 0x2000, s->audio_prg, GEMU_MAP_READONLY, 0);
}

static void fiveclown_board_describe(GemuDevice *d, char *buf, size_t len) {
    const FiveClownState *s = *(FiveClownState **)d->priv;
    snprintf(buf, len, "frame %llu; sound latch %02X, sound NMI %s\n"
             "DIP SW1 %02X SW2 %02X SW3 %02X SW4 %02X, input mux %02X; CRTC R%u",
             (unsigned long long)s->frame, s->main_latch_d800, s->audio_nmi_line ? "high" : "low",
             s->dip1, s->dip2, s->dip3, s->dip4, s->mux_data, s->crtc_addr);
}
static void fiveclown_oki_describe(GemuDevice *d, char *buf, size_t len) {
    const FiveClownState *s = *(FiveClownState **)d->priv;
    const Okim6295 *o = &s->oki;
    snprintf(buf, len, "%s; channels playing %d%d%d%d",
             o->audio_dev ? "sound on" : "silent (no sample ROM or no -soundhw msm6295)",
             o->ch[0].playing, o->ch[1].playing, o->ch[2].playing, o->ch[3].playing);
}

/* The machine's settings (stage M5): the DIP switches, -M 5clown,dip2=0xC7. */
static const GemuOptionDef fiveclown_board_opts[] = {
    { "dip1", "SW1 ($C400)", GEMU_OPT_INT, "0xFF", 0, 255, NULL },
    { "dip2", "SW2 ($CC00; bit 4, System Boot, must be on: 0)", GEMU_OPT_INT, "0xCF", 0, 255, NULL },
    { "dip3", "SW3 ($D400)", GEMU_OPT_INT, "0xD7", 0, 255, NULL },
    { "dip4", "SW4 (PIA1 port A)", GEMU_OPT_INT, "0xAF", 0, 255, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};
static void fiveclown_board_post_load(GemuDevice *d);
static const GemuDeviceType fiveclown_board_type = {
    .name = "5clown-board", .desc = "IGS 5 Clown board", .size = sizeof(FiveClownState *),
    .describe = fiveclown_board_describe, .options = fiveclown_board_opts,
    .post_load = fiveclown_board_post_load,
};
static const GemuDeviceType fiveclown_oki_type = {
    .name = "okim6295-5clown", .desc = "OKI MSM6295 ADPCM (5 Clown's)", .size = sizeof(FiveClownState *),
    .describe = fiveclown_oki_describe,
};

#define B(field) ok &= GEMU_STATE_MEMBER(d, "", s, field)
/* The board's state.  Not saved: the ROMs and palette (loaded), the
 * host's input (held_actions), the picture (drawn at a frame's end, or
 * after a load), the NVRAM file's path. */
static bool fiveclown_board_state(FiveClownState *s, GemuDevice *d) {
    bool ok = true;
    B(nvram); B(audio_ram); B(videoram); B(colorram); B(crtc_addr); B(crtc_regs);
    B(mux_data); B(main_latch_d800); B(snd_latch_0800); B(snd_latch_0a02); B(ay8910_addr);
    B(dip1); B(dip2); B(dip3); B(dip4); B(audio_nmi_line); B(coin_latch); B(frame);
    ok &= GEMU_STATE_VAR(d, "frame_end", s->core->frame_end);
    return ok;
}
#undef B

/* After a load: the picture drawn again, unsent samples dropped. */
static void fiveclown_board_post_load(GemuDevice *d) {
    FiveClownState *s = *(FiveClownState **)d->priv;
    fiveclown_render(s);
    s->oki.frame_n = 0;
}

static bool fiveclown_stop_before(void *ud) {
    FiveClownState *s = ud;
    return gemu_monitor_check_exec(s->monitor, s->cpu->PC);
}
static bool fiveclown_stop_after(void *ud) {
    return gemu_monitor_is_paused(((FiveClownState *)ud)->monitor);
}
/* The sound CPU's instruction, the OKI ticked for its cycles. */
static void fiveclown_audio_insn(void *ud) {
    FiveClownState *s = ud;
    uint64_t prev = s->audiocpu->cycle_count;
    mos6502_step(s->audiocpu);
    if (s->oki.audio_dev) {
        uint64_t delta = s->audiocpu->cycle_count - prev;
        for (uint64_t i = 0; i < delta; i++) oki6295_tick(&s->oki);
    }
}

static void fiveclown_vsync(void *ud, int param) {
    (void)param;
    FiveClownState *s = ud;
    gemu_line_pulse(s->core->vsync);
    gemu_screen_frame_begin(s->core->screen);
}

static void fiveclown_screen_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    const FiveClownState *s = ud;
    for (int y = clip->y0; y < clip->y1; y++)
        memcpy(bmp->pix + (size_t)y * (size_t)bmp->stride, s->pixels_argb + (size_t)y * FIVECLOWN_FB_WIDTH,
               FIVECLOWN_FB_WIDTH * sizeof(uint32_t));
}

/* The OKI's samples as the mixer asks, in order, none lost (as the NES's
 * APU stream: a short buffer repeats its last; what's left over waits). */
static void fiveclown_sound_update(void *ud, float *const *out, unsigned n) {
    FiveClownState *s = ud;
    Okim6295 *o = &s->oki;
    unsigned have = (unsigned)o->frame_n, take = n < have ? n : have;
    memcpy(out[0], o->frame_buf, take * sizeof(float));
    float last = take ? out[0][take - 1] : (have ? o->frame_buf[0] : 0.0f);
    for (unsigned i = take; i < n; i++) out[0][i] = last;
    if (take < have) memmove(o->frame_buf, o->frame_buf + take, (have - take) * sizeof(float));
    o->frame_n = (int)(have - take);
}
static void fiveclown_sound_out(void *ud, const float *samples, size_t frames) {
    gemu_audio_queue(ud, samples, frames);
}

static GemuDevice *fiveclown_part(FiveClownState *s, const GemuDeviceType *t, const char *name,
                                  const char *opts) {
    GemuDevice *d = gemu_device_add(s->core->machine, t, name, NULL, opts && *opts ? opts : NULL);
    if (d && (t == &fiveclown_board_type || t == &fiveclown_oki_type))
        *(FiveClownState **)d->priv = s;
    return d;
}

static bool fiveclown_core_build(FiveClownState *s) {
    FiveClownCore *c = s->core = calloc(1, sizeof *c);
    if (!c || !(c->machine = gemu_machine_new("5clown")))
        return false;
    static const GemuDeviceType *const set_types[] = { &fiveclown_board_type };
    char set[1][GEMU_OPTS_ROUTE_LEN], why[512];
    if (!gemu_opts_route(s->cfg->machine_opts, set_types, 1, set, why, sizeof why)) {
        fprintf(stderr, "gemu: %s\n", why);
        return false;
    }
    static const char *const variant[] = { "6502", "2a03", "2a07", "6501", "6507" };
    char cpu_opts[32];
    snprintf(cpu_opts, sizeof cpu_opts, "variant=%s", variant[s->cfg->cpu]);
    bool ok = (c->board = fiveclown_part(s, &fiveclown_board_type, "board", set[0])) &&
              (c->cpu = fiveclown_part(s, &gemu_mos6502_type, "cpu", cpu_opts)) &&
              (c->audiocpu = fiveclown_part(s, &gemu_mos6502_type, "audiocpu", NULL)) &&
              (c->pia0 = fiveclown_part(s, &gemu_pia6821_type, "pia0", NULL)) &&
              (c->pia1 = fiveclown_part(s, &gemu_pia6821_type, "pia1", NULL)) &&
              (c->oki = fiveclown_part(s, &fiveclown_oki_type, "oki", NULL)) &&
              fiveclown_board_state(s, c->board) && okim6295_state_add(c->oki, "", &s->oki);
    if (!ok) {
        fprintf(stderr, "5clown: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    s->dip1 = (uint8_t)gemu_opt_int(c->board, "dip1");
    s->dip2 = (uint8_t)gemu_opt_int(c->board, "dip2");
    s->dip3 = (uint8_t)gemu_opt_int(c->board, "dip3");
    s->dip4 = (uint8_t)gemu_opt_int(c->board, "dip4");
    bool inited = gemu_machine_init(c->machine);
    if (inited) {
        s->cpu = gemu_mos6502(c->cpu);
        s->audiocpu = gemu_mos6502(c->audiocpu);
        s->pia0 = gemu_pia6821(c->pia0);
        s->pia1 = gemu_pia6821(c->pia1);
        s->cpu->mem_read = main_read;
        s->cpu->mem_write = main_write;
        s->cpu->mem_ud = s;
        s->audiocpu->mem_read = audio_read;
        s->audiocpu->mem_write = audio_write;
        s->audiocpu->mem_ud = s;
    }
    GemuMos6502Hooks mh = { .stop_before = fiveclown_stop_before, .stop_after = fiveclown_stop_after,
                            .ud = s };
    GemuMos6502Hooks ah = { .insn = fiveclown_audio_insn, .ud = s };
    ok = inited && fiveclown_bus_build(s) &&
         gemu_mos6502_exec(c->cpu, FIVECLOWN_CPU_HZ, &mh) &&
         gemu_mos6502_exec(c->audiocpu, FIVECLOWN_CPU_HZ, &ah) &&
         (c->vsync = gemu_line_out(c->board, "vsync")) && gemu_line_connect(c->vsync, c->cpu, "nmi", 0) &&
         (c->sound_nmi = gemu_line_out(c->board, "sound_nmi")) &&
         gemu_line_connect(c->sound_nmi, c->audiocpu, "nmi", 0) &&
         (c->vsync_timer = gemu_timer_new(c->board, fiveclown_vsync, s)) &&
         (c->screen = gemu_screen_new(c->board, "screen")) &&
         gemu_screen_set_size(c->screen, FIVECLOWN_FB_WIDTH, FIVECLOWN_FB_HEIGHT) &&
         (c->sound = gemu_stream_new(c->oki, "sound", 1, OKIM6295_SAMPLE_RATE, fiveclown_sound_update, s));
    if (ok) {
        gemu_screen_set_update(c->screen, fiveclown_screen_update, s);
        gemu_mixer_set_output(c->machine, OKIM6295_SAMPLE_RATE, 1,
                              s->oki.audio_dev ? fiveclown_sound_out : NULL, s->oki.audio_dev);
    }
    if (!ok || !gemu_machine_start(c->machine)) {
        fprintf(stderr, "5clown: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    /* the first vsync now, then one a frame */
    gemu_timer_start(c->vsync_timer, 0, FIVECLOWN_FRAME, 0);
    c->frame_end = gemu_machine_time(c->machine) + FIVECLOWN_FRAME;
    gemu_monitor_attach_machine(s->monitor, c->machine, "program");
    return true;
}

static void fiveclown_core_free(FiveClownState *s) {
    if (!s->core) return;
    gemu_machine_free(s->core->machine);
    free(s->core);
    s->core = NULL;
}

/* One frame: the machine run to the frame's end (both CPUs, the vsync at
 * its start), the frame's sound out through the mixer, the picture drawn.
 * A stop (a breakpoint, a pause) leaves the frame to finish next time. */
static void fiveclown_frame(FiveClownState *s) {
    FiveClownCore *c = s->core;
    gemu_machine_run_until(c->machine, c->frame_end);
    bool done = gemu_machine_time(c->machine) >= c->frame_end;
    gemu_mixer_run(c->machine);
    fiveclown_render(s);
    if (done) {
        c->frame_end += FIVECLOWN_FRAME;
        s->frame++;
    }
}

/* ── Create / destroy ────────────────────────────────────────────────────── */

FiveClownState *fiveclown_create(const MosConfig *cfg) {
    FiveClownState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;

    const char *prg_path = NULL, *audiocpu_path = NULL;
    const char *gfxbanks_path = NULL, *oki_path = NULL, *proms_path = NULL;
    for (int i = 0; i < cfg->n_roms; i++) {
        const char *region = cfg->roms[i].region;
        if (!region) continue;
        if      (!strcmp(region, "prg"))       prg_path      = cfg->roms[i].path;
        else if (!strcmp(region, "audiocpu"))  audiocpu_path = cfg->roms[i].path;
        else if (!strcmp(region, "gfxbanks"))  gfxbanks_path = cfg->roms[i].path;
        else if (!strcmp(region, "oki6295"))   oki_path      = cfg->roms[i].path;
        else if (!strcmp(region, "proms"))     proms_path    = cfg->roms[i].path;
    }
    if (!prg_path || !audiocpu_path || !gfxbanks_path || !proms_path) {
        fprintf(stderr, "5clown: missing ROM(s) - need prg, audiocpu, gfxbanks, proms "
                        "(oki6295 optional); use -rom roms/5clown\n");
        free(s);
        return NULL;
    }

    uint8_t *prg_raw      = load_file(prg_path, 0x8000);
    uint8_t *audiocpu_raw = load_file(audiocpu_path, 0x2000);
    uint8_t *gfxbanks_raw = load_file(gfxbanks_path, FIVECLOWN_GFXBANKS_SIZE);
    uint8_t *proms_raw    = load_file(proms_path, FIVECLOWN_PROM_SIZE);
    uint8_t *oki_raw      = oki_path ? load_file(oki_path, FIVECLOWN_OKI_SIZE) : NULL;
    if (!prg_raw || !audiocpu_raw || !gfxbanks_raw || !proms_raw || (oki_path && !oki_raw)) {
        free(prg_raw); free(audiocpu_raw); free(gfxbanks_raw); free(proms_raw); free(oki_raw);
        free(s);
        return NULL;
    }

    /* Decrypt main program (whole-byte XOR) then split into the CPU's two
     * visible ROM windows - see hardware/mos/5clown.h for the layout. */
    for (int i = 0; i < 0x8000; i++) prg_raw[i] = (uint8_t)(prg_raw[i] ^ 0x20);
    s->prg = malloc(0x8000);
    memcpy(s->prg,          prg_raw,          0x6000); /* -> $2000-$7FFF */
    memcpy(s->prg + 0x6000, prg_raw + 0x6000, 0x2000); /* -> $E000-$FFFF */
    free(prg_raw);

    s->audio_prg = audiocpu_raw; /* unencrypted */

    /* Decrypt gfxbanks in place - 0x5000-0x5FFF is deliberately left alone
     * (verified against the driver's init_fclown; not a bug). */
    for (int i = 0x4000; i < 0x5000; i++) gfxbanks_raw[i] = (uint8_t)(gfxbanks_raw[i] ^ 0x22);
    for (int i = 0x6000; i < 0x7000; i++) gfxbanks_raw[i] = (uint8_t)(gfxbanks_raw[i] ^ 0x3F);
    for (int i = 0x7000; i < 0x8000; i++) gfxbanks_raw[i] = (uint8_t)(gfxbanks_raw[i] ^ 0x22);
    s->gfxbanks = gfxbanks_raw;

    if (oki_raw) {
        for (int i = 0; i < (int)FIVECLOWN_OKI_SIZE; i++) {
            uint8_t b = oki_raw[i];
            oki_raw[i] = (b & 0x02) ? (uint8_t)(b ^ 0x02) : (uint8_t)(b ^ 0x12);
        }
    }
    s->oki_rom = oki_raw;

    if (s->oki_rom && (cfg->sound_hw_mask & MOS_SOUNDHW_OKI6295)) {
        if (!oki6295_init(&s->oki, FIVECLOWN_CPU_HZ, FIVECLOWN_OKI_NATIVE_HZ,
                          s->oki_rom, FIVECLOWN_OKI_SIZE))
            fprintf(stderr, "5clown: OKI6295 audio init failed (continuing silently)\n");
    }

    for (int i = 0; i < 256; i++) {
        uint8_t v = proms_raw[i];
        int bk = (v >> 3) & 1;
        int r = (v & 1) ? 255 : 0;
        int g = (v & 2) ? 255 : 0;
        int b = (bk && (v & 4)) ? 255 : 0;
        s->palette[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    free(proms_raw);

    /* DIP switch defaults (the board's options' defaults, stage M5), computed bit-by-bit from the driver's
     * PORT_DIPNAME default arguments (deliberately not just "0xFF" -
     * several bits default "on"). SW2 bit 0x10 is "System Boot": the
     * driver comment says it must be On (=0 for that bit) to boot, which
     * is already reflected in 0xCF below - do not change this default
     * without re-checking that comment. */
    s->dip1 = 0xFF;
    s->dip2 = 0xCF;
    s->dip3 = 0xD7;
    s->dip4 = 0xAF;

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_screendump_cb(s->monitor, fiveclown_screendump, s);
    gemu_monitor_bind(s->monitor, "coin", fiveclown_cmd_coin, s);
    gemu_monitor_bind(s->monitor, "dipswitch", fiveclown_cmd_dipswitch, s);
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = fiveclown_mon_read, .write = fiveclown_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = false });
    gemu_monitor_set_cpu_state_cb(s->monitor, fiveclown_mon_cpu_state, s);

    if (!fiveclown_core_build(s)) {
        fiveclown_destroy(s);
        return NULL;
    }
    s->pia0->ud        = s;
    s->pia0->read_pa   = pia0_read_pa;
    s->pia0->read_pb   = pia0_read_pb;
    s->pia0->write_pb  = pia0_write_pb;
    s->pia1->ud        = s;
    s->pia1->read_pa   = pia1_read_pa;
    s->pia1->read_pb   = pia1_read_pb;
    s->pia1->write_pa  = pia1_write_pa;
    s->pia1->write_pb  = pia1_write_pb;

    gemu_config_path(s->sav_path, sizeof(s->sav_path), "5clown.nvram");
    fiveclown_nvram_load(s);

    if (gemu_display_wanted(cfg->display_type)) {
        s->display = gemu_display_create(cfg->display_type, &(GemuDisplayConfig){
            .fb_width    = FIVECLOWN_FB_WIDTH,
            .monitor     = s->monitor,
            .fb_height   = FIVECLOWN_FB_HEIGHT,
            .scale       = cfg->display_scale,
            .renderer    = cfg->display_renderer,
            .actions     = fiveclown_actions,
            .n_actions   = FIVECLOWN_NUM_ACTIONS,
            .ini_section = "5clown",
        });
    }

    mos6502_reset(s->cpu);
    mos6502_reset(s->audiocpu);

    return s;
}

void fiveclown_destroy(FiveClownState *s) {
    if (!s) return;
    if (s->core) fiveclown_nvram_save(s);
    fiveclown_core_free(s);
    oki6295_destroy(&s->oki);
    gemu_monitor_destroy(s->monitor);
    gemu_display_destroy(s->display);
    free(s->prg);
    free(s->audio_prg);
    free(s->gfxbanks);
    free(s->oki_rom);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

void fiveclown_run(FiveClownState *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        uint32_t held = 0;
        if (s->display) {
            held = gemu_display_poll(s->display);
            if (gemu_display_should_quit(s->display)) break;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                fiveclown_nvram_save(s);
                mos6502_reset(s->cpu);
                mos6502_reset(s->audiocpu);
            }
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { quit = true; break; }
            } else if (cmd == GEMU_MON_RESET) {
                fiveclown_nvram_save(s);
                mos6502_reset(s->cpu);
                mos6502_reset(s->audiocpu);
            } else if (cmd == GEMU_MON_STEP) {
                /* Steps the main CPU; the sound CPU only runs with the machine. */
                for (uint32_t n = gemu_monitor_step_count(s->monitor); n; n--)
                    mos6502_step(s->cpu);
                fiveclown_render(s);   /* the window follows stepping */
            }
        }
        if (quit) break;
        if (s->display) gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));

        fiveclown_handle_keys(s, held);
        if (s->coin_latch > 0) s->coin_latch--;

        bool paused = gemu_monitor_is_paused(s->monitor);
        if (!paused) fiveclown_frame(s);

        if (s->display) {
            const GemuBitmap *b = gemu_screen_render(s->core->screen);
            gemu_display_render(s->display, b->pix, b->width, b->height);
        }

        /* Pace to the machine's time (50.08 frames a second); paused, a
         * frame's delay. */
        if (!paused) {
            gemu_machine_pace(s->core->machine, 0);
        } else {
            gemu_machine_pace_reset(s->core->machine);
            SDL_Delay(20);
        }
    }

    fiveclown_nvram_save(s);
    gemu_monitor_stop(s->monitor);
    printf("5clown: %llu frames, %llu cpu cycles (main), %llu cpu cycles (audio)\n",
           (unsigned long long)s->frame, (unsigned long long)s->cpu->cycle_count,
           (unsigned long long)s->audiocpu->cycle_count);
}
