#ifndef _WIN32
#  define _POSIX_C_SOURCE 199309L
#endif
#include "5clown.h"
#include "gemu/screendump.h"
#include <SDL2/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <direct.h>
#  define gemu_mkdir(p) _mkdir(p)
#  define strcasecmp _stricmp
#  define strncasecmp _strnicmp
#else
#  include <strings.h>
#  include <sys/stat.h>
#  define gemu_mkdir(p) mkdir((p), 0755)
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

/* ── Main CPU bus ────────────────────────────────────────────────────────── */

static uint8_t main_read(uint16_t addr, void *ud) {
    FiveClownState *s = ud;
    if (addr < 0x0800) return s->nvram[addr];
    if (addr == 0x0801) return s->crtc_regs[s->crtc_addr];
    if (addr >= 0x0844 && addr <= 0x0847) return pia6821_read(&s->pia0, addr);
    if (addr >= 0x0848 && addr <= 0x084B) return pia6821_read(&s->pia1, addr);
    if (addr >= 0x1000 && addr <= 0x13FF) return s->videoram[addr - 0x1000];
    if (addr >= 0x1800 && addr <= 0x1BFF) return s->colorram[addr - 0x1800];
    if (addr >= 0x2000 && addr <= 0x7FFF) return s->prg[addr - 0x2000];
    if (addr == 0xC400) return s->dip1;
    if (addr == 0xCC00) return s->dip2;
    if (addr == 0xD400) return s->dip3;
    if (addr >= 0xE000) return s->prg[0x6000 + (addr - 0xE000)];
    return 0xFF; /* open bus */
}

static void main_write(uint16_t addr, uint8_t val, void *ud) {
    FiveClownState *s = ud;
    if (addr < 0x0800) { s->nvram[addr] = val; return; }
    if (addr == 0x0800) { s->crtc_addr = (uint8_t)(val & 0x1F); return; }
    if (addr == 0x0801) { s->crtc_regs[s->crtc_addr] = val; return; }
    if (addr >= 0x0844 && addr <= 0x0847) { pia6821_write(&s->pia0, addr, val); return; }
    if (addr >= 0x0848 && addr <= 0x084B) { pia6821_write(&s->pia1, addr, val); return; }
    if (addr >= 0x1000 && addr <= 0x13FF) { s->videoram[addr - 0x1000] = val; return; }
    if (addr >= 0x1800 && addr <= 0x1BFF) { s->colorram[addr - 0x1800] = val; return; }
    if (addr == 0xD800) { s->main_latch_d800 = val; return; }
    /* $C048 (unknown sound-related write, per driver notes) and the ROM
     * windows: no effect, matching real hardware. */
}

/* ── Audio CPU bus ───────────────────────────────────────────────────────── */

static uint8_t audio_read(uint16_t addr, void *ud) {
    FiveClownState *s = ud;
    if (addr < 0x0800) return s->audio_ram[addr];
    if (addr == 0x0C06) /* OKI6295 busy status */
        return s->oki.audio_dev ? oki6295_read(&s->oki) : 0xFF;
    if (addr == 0x0E06) return s->main_latch_d800;
    if (addr >= 0xE000) return s->audio_prg[addr - 0xE000];
    return 0xFF;
}

static void audio_write(uint16_t addr, uint8_t val, void *ud) {
    FiveClownState *s = ud;
    if (addr < 0x0800) { s->audio_ram[addr] = val; return; }
    if (addr == 0x0800) {
        s->snd_latch_0800 = val;
        if (s->cfg->sound_hw_mask & MOS_SOUNDHW_AY8910) {
            if (s->snd_latch_0a02 == 0xC0) s->ay8910_addr = val;
            /* 0x00 mode would latch (ay8910_addr, val) into the real chip;
             * stubbed for now, so nothing further happens. */
        }
        return;
    }
    if (addr == 0x0A02) { s->snd_latch_0a02 = val; return; }
    if (addr == 0x0C04) { if (s->oki.audio_dev) oki6295_write(&s->oki, val); return; }
    /* ROM region: no effect */
}

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
    (void)ud; (void)val; /* coin counters — nothing physical to drive */
}

/* ── PIA1: SW4 + audio-CPU NMI trigger + mux select ─────────────────────── */

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
    if (assert_line && !s->audio_nmi_line) s->audiocpu.nmi = true;
    s->audio_nmi_line = assert_line;
}

static void pia1_write_pb(void *ud, uint8_t val) {
    FiveClownState *s = ud;
    s->mux_data = (uint8_t)(val ^ 0xFFu); /* inverted, per hardware */
}

/* ── Input: local display actions + VNC key -> action mapping ──────────────
 *
 * VNC clients don't go through GemuDisplay's action-bitmask polling, so
 * (like machine_nes.c) we translate RFB keysyms directly onto the same
 * action bits by hand, using the same physical keys as the default SDL
 * bindings above so both input paths feel identical. */

#define XK_Left   0xFF51u
#define XK_Up     0xFF52u
#define XK_Right  0xFF53u
#define XK_Down   0xFF54u
#define XK_Return 0xFF0Du
#define XK_F2     0xFFBFu

static void fiveclown_handle_keys(FiveClownState *s, uint32_t held) {
    (void)XK_Up; (void)XK_Down; /* unused directions on this board */

    if (s->display) {
        s->held_actions = held;
        uint32_t newly = gemu_display_last_pressed(s->display);
        if (newly & GEMU_ACTION(FIVECLOWN_ACT_COIN)) s->coin_latch = 15;
    } else if (!s->vnc) {
        s->held_actions = 0;
    }

    if (s->vnc) {
        GemuVncKeyEvent ev;
        while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
            uint32_t bit = 0;
            switch (ev.keysym) {
            case 'b': case 'B': bit = GEMU_ACTION(FIVECLOWN_ACT_BET);     break;
            case 'r': case 'R': bit = GEMU_ACTION(FIVECLOWN_ACT_RECORD);  break;
            case 'u': case 'U': bit = GEMU_ACTION(FIVECLOWN_ACT_DUP);     break;
            case XK_Return:     bit = GEMU_ACTION(FIVECLOWN_ACT_START);   break;
            case 'k': case 'K': bit = GEMU_ACTION(FIVECLOWN_ACT_KEYOUT);  break;
            case 'p': case 'P': bit = GEMU_ACTION(FIVECLOWN_ACT_PAYOUT);  break;
            case 'c': case 'C': bit = GEMU_ACTION(FIVECLOWN_ACT_COLLECT); break;
            case XK_Right:      bit = GEMU_ACTION(FIVECLOWN_ACT_BIG);     break;
            case XK_Left:       bit = GEMU_ACTION(FIVECLOWN_ACT_SMALL);   break;
            case '1':           bit = GEMU_ACTION(FIVECLOWN_ACT_HOLD1);   break;
            case '2':           bit = GEMU_ACTION(FIVECLOWN_ACT_HOLD2);   break;
            case '3':           bit = GEMU_ACTION(FIVECLOWN_ACT_HOLD3);   break;
            case '4':           bit = GEMU_ACTION(FIVECLOWN_ACT_HOLD4);   break;
            case '5':           bit = GEMU_ACTION(FIVECLOWN_ACT_HOLD5);   break;
            case XK_F2:         bit = GEMU_ACTION(FIVECLOWN_ACT_SETTING); break;
            case '6':           bit = GEMU_ACTION(FIVECLOWN_ACT_COIN);    break;
            case '7':           bit = GEMU_ACTION(FIVECLOWN_ACT_KEYIN);   break;
            default: break;
            }
            if (!bit) continue;
            if (ev.down) {
                s->held_actions |= bit;
                if (bit == GEMU_ACTION(FIVECLOWN_ACT_COIN)) s->coin_latch = 15;
            } else {
                s->held_actions &= ~bit;
            }
        }
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
                uint8_t  *idxrow = &s->pixels_idx[py * FIVECLOWN_FB_WIDTH + tx * 8];
                for (int n = 7; n >= 0; n--) {
                    int idx3 = (((plane2 >> n) & 1) << 2) | (((plane1 >> n) & 1) << 1) | ((plane0 >> n) & 1);
                    uint8_t pen = (uint8_t)(color * 8 + idx3);
                    *row++ = s->palette[pen];
                    *idxrow++ = pen;
                }
            }
        }
    }
}

/* ── Screendump ──────────────────────────────────────────────────────────── */

static bool fiveclown_screendump(void *ud, const char *path) {
    FiveClownState *s = ud;
    int w = FIVECLOWN_FB_WIDTH, h = FIVECLOWN_FB_HEIGHT;
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

static void fiveclown_build_sav_path(char *out, size_t len) {
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("APPDATA");
    if (!base || !base[0]) base = "C:\\Users\\Default\\AppData\\Local";
    snprintf(out, len, "%s\\gemu\\5clown.nvram", base);
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(out, len, "%s/.gemu/5clown.nvram", home);
#endif
}

static void fiveclown_ensure_dir(const char *path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *sep = strrchr(dir, '/');
#ifdef _WIN32
    char *sep2 = strrchr(dir, '\\');
    if (sep2 > sep) sep = sep2;
#endif
    if (sep) { *sep = '\0'; gemu_mkdir(dir); }
}

static void fiveclown_nvram_load(FiveClownState *s) {
    FILE *f = fopen(s->sav_path, "rb");
    if (!f) return;
    size_t got = fread(s->nvram, 1, sizeof(s->nvram), f);
    fclose(f);
    if (got) printf("5clown: loaded NVRAM '%s'\n", s->sav_path);
}

static void fiveclown_nvram_save(FiveClownState *s) {
    fiveclown_ensure_dir(s->sav_path);
    FILE *f = fopen(s->sav_path, "wb");
    if (!f) { fprintf(stderr, "5clown: cannot write NVRAM '%s'\n", s->sav_path); return; }
    fwrite(s->nvram, 1, sizeof(s->nvram), f);
    fclose(f);
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
        fprintf(stderr, "5clown: missing ROM(s) — need prg, audiocpu, gfxbanks, proms "
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
     * visible ROM windows — see hardware/5clown.h for the layout. */
    for (int i = 0; i < 0x8000; i++) prg_raw[i] = (uint8_t)(prg_raw[i] ^ 0x20);
    s->prg = malloc(0x8000);
    memcpy(s->prg,          prg_raw,          0x6000); /* -> $2000-$7FFF */
    memcpy(s->prg + 0x6000, prg_raw + 0x6000, 0x2000); /* -> $E000-$FFFF */
    free(prg_raw);

    s->audio_prg = audiocpu_raw; /* unencrypted */

    /* Decrypt gfxbanks in place — 0x5000-0x5FFF is deliberately left alone
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

    /* DIP switch defaults, computed bit-by-bit from the driver's
     * PORT_DIPNAME default arguments (deliberately not just "0xFF" —
     * several bits default "on"). SW2 bit 0x10 is "System Boot": the
     * driver comment says it must be On (=0 for that bit) to boot, which
     * is already reflected in 0xCF below — do not change this default
     * without re-checking that comment. */
    s->dip1 = 0xFF;
    s->dip2 = 0xCF;
    s->dip3 = 0xD7;
    s->dip4 = 0xAF;

    mos6502_init(&s->cpu);
    s->cpu.mem_read  = main_read;
    s->cpu.mem_write = main_write;
    s->cpu.mem_ud    = s;

    mos6502_init(&s->audiocpu);
    s->audiocpu.mem_read  = audio_read;
    s->audiocpu.mem_write = audio_write;
    s->audiocpu.mem_ud    = s;

    pia6821_init(&s->pia0);
    s->pia0.ud        = s;
    s->pia0.read_pa   = pia0_read_pa;
    s->pia0.read_pb   = pia0_read_pb;
    s->pia0.write_pb  = pia0_write_pb;

    pia6821_init(&s->pia1);
    s->pia1.ud        = s;
    s->pia1.read_pa   = pia1_read_pa;
    s->pia1.read_pb   = pia1_read_pb;
    s->pia1.write_pa  = pia1_write_pa;
    s->pia1.write_pb  = pia1_write_pb;

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_screendump_cb(s->monitor, fiveclown_screendump, s);

    fiveclown_build_sav_path(s->sav_path, sizeof(s->sav_path));
    fiveclown_nvram_load(s);

    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr, FIVECLOWN_FB_WIDTH, FIVECLOWN_FB_HEIGHT);
        if (s->vnc)
            gemu_vnc_set_palette(s->vnc, s->palette, 256);
        else
            fprintf(stderr, "5clown: failed to start VNC at %s\n", cfg->vnc_addr);
    }
    gemu_monitor_set_vnc(s->monitor, s->vnc);

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        s->display = gemu_display_create(cfg->display_type, &(GemuDisplayConfig){
            .title       = "GEMU",
            .fb_width    = FIVECLOWN_FB_WIDTH,
            .fb_height   = FIVECLOWN_FB_HEIGHT,
            .scale       = cfg->display_scale,
            .renderer    = cfg->display_renderer,
            .actions     = fiveclown_actions,
            .n_actions   = FIVECLOWN_NUM_ACTIONS,
            .ini_section = "5clown",
        });
    }

    mos6502_reset(&s->cpu);
    mos6502_reset(&s->audiocpu);

    return s;
}

void fiveclown_destroy(FiveClownState *s) {
    if (!s) return;
    fiveclown_nvram_save(s);
    oki6295_destroy(&s->oki);
    gemu_monitor_destroy(s->monitor);
    gemu_display_destroy(s->display);
    gemu_vnc_destroy(s->vnc);
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
        Uint32 t0 = SDL_GetTicks();

        uint32_t held = 0;
        if (s->display) {
            held = gemu_display_poll(s->display);
            if (gemu_display_should_quit(s->display)) break;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                fiveclown_nvram_save(s);
                mos6502_reset(&s->cpu);
                mos6502_reset(&s->audiocpu);
            }
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { quit = true; break; }
            } else if (cmd == GEMU_MON_RESET) {
                fiveclown_nvram_save(s);
                mos6502_reset(&s->cpu);
                mos6502_reset(&s->audiocpu);
            } else if (cmd == GEMU_MON_CUSTOM) {
                const char *text = gemu_monitor_command_text(s->monitor);
                while (*text == ' ' || *text == '\t') text++;
                if (strncasecmp(text, "coin", 4) == 0 && (text[4] == '\0' || text[4] == ' ')) {
                    s->coin_latch = 15;
                    printf("5clown: coin in\n");
                } else if (strncasecmp(text, "dipswitch", 9) == 0 &&
                           (text[9] == '\0' || text[9] == ' ' || text[9] == '\t')) {
                    const char *p = text + 9;
                    while (*p == ' ' || *p == '\t') p++;
                    int bank = 0; unsigned val = 0;
                    if (sscanf(p, "%d %x", &bank, &val) == 2 && bank >= 1 && bank <= 4) {
                        uint8_t *d = bank == 1 ? &s->dip1 : bank == 2 ? &s->dip2
                                   : bank == 3 ? &s->dip3 : &s->dip4;
                        *d = (uint8_t)val;
                        printf("5clown: SW%d = 0x%02X\n", bank, *d);
                    } else {
                        printf("5clown: SW1=%02X SW2=%02X SW3=%02X SW4=%02X\n",
                               s->dip1, s->dip2, s->dip3, s->dip4);
                        printf("usage: dipswitch <1-4> <hex-value>\n");
                    }
                } else {
                    gemu_monitor_unknown_command(s->monitor);
                }
            }
        }
        if (quit) break;
        if (s->display) gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));

        fiveclown_handle_keys(s, held);
        if (s->coin_latch > 0) s->coin_latch--;

        if (!gemu_monitor_is_paused(s->monitor)) {
            s->cpu.nmi = true; /* CRTC vsync, once per frame */

            uint64_t target = s->cpu.cycle_count + FIVECLOWN_CYCLES_PER_FRAME;
            while (s->cpu.cycle_count < target) {
                if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) break;
                mos6502_step(&s->cpu);
            }

            uint64_t audio_target = s->audiocpu.cycle_count + FIVECLOWN_CYCLES_PER_FRAME;
            while (s->audiocpu.cycle_count < audio_target) {
                uint64_t prev = s->audiocpu.cycle_count;
                mos6502_step(&s->audiocpu);
                if (s->oki.audio_dev) {
                    uint64_t delta = s->audiocpu.cycle_count - prev;
                    for (uint64_t i = 0; i < delta; i++) oki6295_tick(&s->oki);
                }
            }
            if (s->oki.audio_dev) oki6295_flush(&s->oki);

            fiveclown_render(s);
            s->frame++;
        }

        if (s->display)
            gemu_display_render(s->display, s->pixels_argb, FIVECLOWN_FB_WIDTH, FIVECLOWN_FB_HEIGHT);
        if (s->vnc)
            gemu_vnc_update(s->vnc, s->pixels_idx, FIVECLOWN_FB_WIDTH, FIVECLOWN_FB_HEIGHT);

        Uint32 dt = SDL_GetTicks() - t0;
        Uint32 frame_ms = 1000u / 50u; /* ~50.08Hz; close enough for SDL_Delay granularity */
        if (dt < frame_ms) SDL_Delay(frame_ms - dt);
    }

    fiveclown_nvram_save(s);
    gemu_monitor_stop(s->monitor);
    printf("5clown: %llu frames, %llu cpu cycles (main), %llu cpu cycles (audio)\n",
           (unsigned long long)s->frame, (unsigned long long)s->cpu.cycle_count,
           (unsigned long long)s->audiocpu.cycle_count);
}
