#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "pecom.h"
#include "cdp1802_dev.h"
#include "cdp1869_dev.h"
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

static void pecom_maps_sync(RcaPecom32State *s);

/* ── Memory map ──────────────────────────────────────────────────────────── */

/* The decode, for the monitor's view (the CPU goes through the bus). */
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
        return cdp1869_char_read(s->vis, addr - PECOM32_CRAM_BASE);

    /* VIS-1870 page RAM: 0xF800–0xFFFF */
    if (addr >= PECOM32_PRAM_BASE)
        return cdp1869_page_read(s->vis, addr - PECOM32_PRAM_BASE);

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
        cdp1869_char_write(s->vis, addr - PECOM32_CRAM_BASE, val);
        return;
    }

    /* VIS-1870 page RAM: 0xF800–0xFFFF */
    if (addr >= PECOM32_PRAM_BASE) {
        cdp1869_page_write(s->vis, addr - PECOM32_PRAM_BASE, val);
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

/* Monitor memory view: side-effect-free.  Character RAM reads through the
 * VIS-1870 latch the PCB bit, so that window is not inspectable; every other
 * region of pecom_mem_read() is a plain array lookup. */
static bool pecom_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    RcaPecom32State *s = ud;
    if (addr > 0xffffu) return false;
    if (addr >= PECOM32_CRAM_BASE && addr < PECOM32_PRAM_BASE) return false;
    *val = pecom_mem_read((uint16_t)addr, s);
    return true;
}

static bool pecom_mon_write(void *ud, uint64_t addr, uint8_t val) {
    RcaPecom32State *s = ud;
    if (addr > 0xffffu) return false;
    if (addr >= 0x8000u && addr < 0xC000u) return false;            /* ROM */
    if (s->boot_mirror && addr < 0x4000u) return false;             /* ROM mirror */
    if (addr >= 0xC000u && addr < PECOM32_CRAM_BASE &&
        s->rom_size > PECOM32_ROM_SIZE) return false;               /* Pecom 64 ROM 2 */
    pecom_mem_write((uint16_t)addr, val, s);
    return true;
}

static void pecom_mon_cpu_state(void *ud, char *buf, size_t len) {
    const RcaPecom32State *s = ud;
    cdp1802_format_state(s->cpu, buf, len);
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
        uint8_t row = (uint8_t)(s->cpu->R[s->cpu->X] & 0x3Fu);
        return s->keys[row];
    }
    return 0xFFu;
}

static void pecom_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaPecom32State *s = ud;

    if (port == 1) {
        /* OUT 1: iogroup selector (bit 1 = 1 → iogroup 2 / VIS-1870) */
        s->iogroup = (val & 0x02u) ? 2 : 0;
        if (s->boot_mirror) {
            s->boot_mirror = false;
            pecom_maps_sync(s);
        }
        return;
    }

    /* Iogroup 0: no OUT ports used by the keyboard (matrix uses IN 3 address bus) */
    if (s->iogroup == 0)
        return;

    if (s->iogroup == 2 && port >= 3 && port <= 7) {
        cdp1869_out(s->vis, port, s->cpu->memory_addr, val);
        return;
    }
}

static void pecom_tone_gate(RcaPecom32State *s, uint8_t q);
static void pecom_maps_sync(RcaPecom32State *s);

static void pecom_q_out(uint8_t q, void *ud) {
    RcaPecom32State *s = ud;
    s->vis->q = q & 1u;
    pecom_tone_gate(s, q);
}

/* ── Per-frame video timing ──────────────────────────────────────────────── */

static void pecom_video_timing(RcaPecom32State *s, unsigned frame_cycle) {
    /* Map CPU frame cycle → VIS-1870 scan line */
    unsigned vis_line =
        (frame_cycle * CDP1869_LINES_TOTAL) / PECOM32_MCYCLES_PER_FRAME;

    /* non_display: true during VBlank (lines 0–47 and 264–311) */
    bool nd = (vis_line < CDP1869_DISPLAY_START ||
               vis_line >= CDP1869_DISPLAY_END);
    s->vis->non_display = nd || s->vis->dispoff;

    /* EF1: open-drain shared between VIS-1870 display timing and CTRL key.
     * High only when NOT in active display AND CTRL is not pressed. */
    s->cpu->EF[0] = s->vis->non_display && !s->key_ctrl;

    /* EF2 = SHIFT key, active-low: 0 when pressed, 1 when released. */
    s->cpu->EF[1] = !s->key_shift;

    /* EF3 = CAPS LOCK (pol=rev): 1 = not locked, 0 = locked.
     * BN3 fires when EF3=1 (caps not active), routing to lowercase path. */
    s->cpu->EF[2] = !s->caps_locked;

    /* EF4 = ESC key, active-low. */
    s->cpu->EF[3] = !s->key_esc;

    /* Single interrupt per frame at "line 2" (start of VBlank) */
    unsigned int_cycle =
        (2u * PECOM32_MCYCLES_PER_FRAME) / CDP1869_LINES_TOTAL;
    if (frame_cycle == int_cycle)
        cdp1802_request_irq(s->cpu);
}

/* ── Machine lifecycle ───────────────────────────────────────────────────── */

static bool pecom_core_build(RcaPecom32State *s);
static void pecom_core_free(RcaPecom32State *s);

/* ── The core machine (stage R4) ─────────────────────────────────────────────
 * board (pecom-board): RAM, the boot mirror, the I/O group, CAPS LOCK, the
 * beam (frame_cycle); cpu (cdp1802): the machine's executor at 220,000
 * machine cycles a second (pecom_cycle: the VIS's timing for the cycle -
 * non-display, EF1-EF4, the frame interrupt - then the CPU's cycle); vis
 * (cdp1869); tone (with -soundhw pcspk): Q's 250 Hz beeper.
 * The bus, "program": RAM bank 1 at $0000, under the ROM's boot mirror
 * until the first OUT 1; the ROM at $8000; at $C000 RAM bank 2 (Pecom 32)
 * or the second ROM (Pecom 64); the VIS's character RAM ($F400) and page
 * RAM ($F800).  The board's "screen" is the VIS's 240x216 bitmap.
 * Before stage R the beam was the run loop's loop variable, restarted each
 * frame, and step didn't clock it; now both run and step go through
 * pecom_cycle and a stopped frame carries on where it stopped. */
typedef struct RcaPecomCore {
    GemuMachine   *machine;
    GemuDevice    *board, *cpu, *vis, *tone;
    GemuSpace     *bus;
    GemuSpaceCache cache;
    GemuMapping   *mirror, *mirror_drop;
    GemuScreen    *screen;
    GemuAudioOut  *audio;
} RcaPecomCore;

#define PECOM_MCYCLE_HZ CDP1802_MCYCLES_PER_SEC

static void pecom_tone_gate(RcaPecom32State *s, uint8_t q) {
    if (s->core && s->core->tone) gemu_tone_set_gate(s->core->tone, q);
}

static uint8_t pecom_bus_read(uint16_t addr, void *ud) {
    return gemu_space_cache_read8(&((RcaPecom32State *)ud)->core->cache, addr);
}
static void pecom_bus_write(uint16_t addr, uint8_t val, void *ud) {
    gemu_space_cache_write8(&((RcaPecom32State *)ud)->core->cache, addr, val);
}
static uint8_t vis_char_r(void *ud, uint64_t off) {
    return cdp1869_char_read(((RcaPecom32State *)ud)->vis, (uint16_t)off);
}
static void vis_char_w(void *ud, uint64_t off, uint8_t v) {
    cdp1869_char_write(((RcaPecom32State *)ud)->vis, (uint16_t)off, v);
}
static uint8_t vis_page_r(void *ud, uint64_t off) {
    return cdp1869_page_read(((RcaPecom32State *)ud)->vis, (uint16_t)off);
}
static void vis_page_w(void *ud, uint64_t off, uint8_t v) {
    cdp1869_page_write(((RcaPecom32State *)ud)->vis, (uint16_t)off, v);
}
static void drop_w(void *ud, uint64_t off, uint8_t v) { (void)ud; (void)off; (void)v; }
static const GemuHandlers vis_char_handlers = { .r8 = vis_char_r, .w8 = vis_char_w };
static const GemuHandlers vis_page_handlers = { .r8 = vis_page_r, .w8 = vis_page_w };
static const GemuHandlers drop_handlers     = { .w8 = drop_w };

/* The boot mirror, on until the first OUT 1. */
static void pecom_maps_sync(RcaPecom32State *s) {
    RcaPecomCore *c = s->core;
    if (!c || !c->mirror) return;
    gemu_mapping_enable(c->mirror, s->boot_mirror);
    gemu_mapping_enable(c->mirror_drop, s->boot_mirror);
}

static bool pecom_bus_build(RcaPecom32State *s) {
    RcaPecomCore *c = s->core;
    GemuSpace *b = c->bus = gemu_space_new(c->machine, "program", 16, 8, GEMU_BIG_ENDIAN);
    if (!b) return false;
    c->cache.space = b;
    bool p64 = s->rom_size > PECOM32_ROM_SIZE;
    bool ok = gemu_space_map_ram(b, 0x0000, PECOM32_RAM1_SIZE, s->ram1, 0, 0) &&
              (c->mirror = gemu_space_map_ram(b, 0x0000, 0x4000, s->rom, GEMU_MAP_READONLY, 1)) &&
              (c->mirror_drop = gemu_space_map(b, 0x0000, 0x4000, &drop_handlers, s, 1, 0)) &&
              gemu_space_map_ram(b, 0x8000, 0x4000, s->rom, GEMU_MAP_READONLY, 0) &&
              (p64 ? gemu_space_map_ram(b, 0xC000, 0x3400, s->rom + 0x4000, GEMU_MAP_READONLY, 0)
                   : gemu_space_map_ram(b, 0xC000, 0x3400, s->ram2, 0, 0)) &&
              gemu_space_map(b, PECOM32_CRAM_BASE, 0x400, &vis_char_handlers, s, 0, 0) &&
              gemu_space_map(b, PECOM32_PRAM_BASE, 0x800, &vis_page_handlers, s, 0, 0);
    if (ok) pecom_maps_sync(s);
    return ok;
}

static void pecom_board_describe(GemuDevice *d, char *buf, size_t len) {
    const RcaPecom32State *s = *(RcaPecom32State **)d->priv;
    snprintf(buf, len, "Pecom %s; %s; I/O group %u; CAPS %s; frame cycle %u of %u",
             s->rom_size > PECOM32_ROM_SIZE ? "64" : "32",
             s->boot_mirror ? "booting (ROM at $0000)" : "RAM at $0000", s->iogroup,
             s->caps_locked ? "locked" : "off", s->frame_cycle, (unsigned)PECOM32_MCYCLES_PER_FRAME);
}

static void pecom_board_post_load(GemuDevice *d);
static const GemuDeviceType pecom_board_type = {
    .name = "pecom-board", .desc = "Pecom 32/64 board (RAM, ROM, keyboard)",
    .size = sizeof(RcaPecom32State *), .describe = pecom_board_describe,
    .post_load = pecom_board_post_load,
};

#define B(field) ok &= GEMU_STATE_MEMBER(d, "", s, field)
/* The board's state: the RAM, the boot mirror, the I/O group, CAPS LOCK,
 * the beam.  Not saved: the ROM (loaded), the keys held (the host's). */
static bool pecom_board_state(RcaPecom32State *s, GemuDevice *d) {
    bool ok = true;
    B(ram1); B(ram2); B(boot_mirror); B(iogroup); B(caps_locked); B(caps_key_prev); B(frame_cycle);
    return ok;
}
#undef B

/* After a load: the boot mirror as the state has it. */
static void pecom_board_post_load(GemuDevice *d) {
    pecom_maps_sync(*(RcaPecom32State **)d->priv);
}

static void pecom_video_timing(RcaPecom32State *s, unsigned frame_cycle);
/* One machine cycle on the beam. */
static void pecom_cycle(RcaPecom32State *s) {
    pecom_video_timing(s, s->frame_cycle);
    cdp1802_step(s->cpu);
    if (++s->frame_cycle == PECOM32_MCYCLES_PER_FRAME) s->frame_cycle = 0;
}
static bool pecom_stop_before(void *ud) {
    RcaPecom32State *s = ud;
    return gemu_monitor_check_exec(s->monitor, s->cpu->R[s->cpu->P]);
}
static void pecom_hook_cycle(void *ud) { pecom_cycle(ud); }
static bool pecom_stop_after(void *ud) { return gemu_monitor_is_paused(((RcaPecom32State *)ud)->monitor); }

static void pecom_screen_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    RcaPecom32State *s = ud;
    cdp1869_render(s->vis);
    for (int y = clip->y0; y < clip->y1; y++)
        for (int x = 0; x < CDP1869_VISIBLE_W; x++)
            bmp->pix[(size_t)y * (size_t)bmp->stride + (size_t)x] =
                pecom_palette[s->vis->bitmap[y * CDP1869_VISIBLE_W + x] & 7u];
}

static void pecom_sound_out(void *ud, const float *samples, size_t frames) {
    gemu_audio_queue(ud, samples, frames);
}

static bool pecom_core_build(RcaPecom32State *s) {
    const RcaConfig *cfg = s->cfg;
    RcaPecomCore *c = s->core = calloc(1, sizeof *c);
    if (!c || !(c->machine = gemu_machine_new("pecom")))
        return false;
    bool ok = (c->board = gemu_device_add(c->machine, &pecom_board_type, "board", NULL, NULL)) &&
              (c->cpu = gemu_device_add(c->machine, &gemu_cdp1802_type, "cpu", NULL, NULL)) &&
              (c->vis = gemu_device_add(c->machine, &gemu_cdp1869_type, "vis", NULL, NULL));
    if (ok && cfg->sound_hw == RCA_SOUND_PCSPK)
        ok = (c->tone = gemu_device_add(c->machine, &gemu_tone_type, "tone", NULL, "freq=250"));
    if (ok) {
        *(RcaPecom32State **)c->board->priv = s;
        ok = pecom_board_state(s, c->board);
    }
    if (!ok) {
        fprintf(stderr, "gemu-rca: pecom32: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    bool inited = gemu_machine_init(c->machine);
    if (inited) {
        s->cpu = gemu_cdp1802(c->cpu);
        s->vis = gemu_cdp1869(c->vis);
        Cdp1802 *cpu = s->cpu;
        cpu->mem_read  = pecom_bus_read;
        cpu->mem_write = pecom_bus_write;
        cpu->io_in     = pecom_io_in;
        cpu->io_out    = pecom_io_out;
        cpu->q_out     = pecom_q_out;
        cpu->io_ud     = s;
        /* on_sync not used: the timing is pecom_cycle's */
        cdp1869_set_page_ram_mask(s->vis, 0x3FFu);  /* 1 KB page RAM */
        cdp1869_set_char_stride(s->vis, 16u);        /* 16 scan lines / char */
        cdp1869_set_block_cpu_access(s->vis, true);  /* block during active display */
    }
    GemuCdp1802Hooks h = { .stop_before = pecom_stop_before, .cycle = pecom_hook_cycle,
                           .stop_after = pecom_stop_after, .ud = s };
    ok = inited && pecom_bus_build(s) && gemu_cdp1802_exec(c->cpu, PECOM_MCYCLE_HZ, &h) &&
         (c->screen = gemu_screen_new(c->board, "screen")) &&
         gemu_screen_set_size(c->screen, CDP1869_VISIBLE_W, CDP1869_VISIBLE_H);
    if (ok) {
        gemu_screen_set_update(c->screen, pecom_screen_update, s);
        if (c->tone) c->audio = gemu_audio_open("pecom", 44100);
        gemu_mixer_set_output(c->machine, 44100, 1, c->audio ? pecom_sound_out : NULL, c->audio);
    }
    if (!ok || !gemu_machine_start(c->machine)) {
        fprintf(stderr, "gemu-rca: pecom32: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    gemu_monitor_attach_machine(s->monitor, c->machine, "program");
    return true;
}

static void pecom_core_free(RcaPecom32State *s) {
    if (!s->core) return;
    gemu_machine_free(s->core->machine);
    gemu_audio_close(s->core->audio);
    free(s->core);
    s->core = NULL;
}

/* One frame: the machine run to the frame's end (from wherever the beam
 * is), then, at a frame's end, VBlank for the host's turn (page and
 * character RAM writable, EF1 the CTRL key alone); the frame's sound out. */
static void pecom_frame(RcaPecom32State *s) {
    RcaPecomCore *c = s->core;
    uint64_t left = PECOM32_MCYCLES_PER_FRAME - s->frame_cycle;
    gemu_machine_run_until(c->machine, gemu_machine_time(c->machine) + gemu_clocks(PECOM_MCYCLE_HZ, left));
    if (s->frame_cycle == 0) {
        s->vis->non_display = true;
        s->cpu->EF[0] = !s->key_ctrl;
        gemu_screen_frame_begin(c->screen);
    }
    gemu_mixer_run(c->machine);
}

RcaPecom32State *rca_pecom32_create(const RcaConfig *cfg) {
    pecom_init_palette();

    RcaPecom32State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg         = cfg;
    s->boot_mirror = true;
    s->iogroup     = 0;

    memset(s->keys,       0, sizeof(s->keys));

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = pecom_mon_read, .write = pecom_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = true });
    gemu_monitor_set_cpu_state_cb(s->monitor, pecom_mon_cpu_state, s);

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

    if (!pecom_core_build(s)) {
        rca_pecom32_destroy(s);
        return NULL;
    }
    return s;
}

static void rca_pecom32_reset(RcaPecom32State *s, const RcaConfig *cfg) {
    cdp1802_reset(s->cpu);
    cdp1869_reset(s->vis);
    cdp1869_set_page_ram_mask(s->vis, 0x3FFu);
    cdp1869_set_char_stride(s->vis, 16u);
    cdp1869_set_block_cpu_access(s->vis, true);
    pecom_tone_gate(s, 0);

    s->boot_mirror = true;
    s->iogroup     = 0;
    s->key_shift   = false;
    s->key_ctrl    = false;
    s->key_esc     = false;
    s->caps_locked = false;
    s->caps_key_prev = false;

    memset(s->keys,       0, sizeof(s->keys));

    s->rom_size = 0;
    for (int i = 0; i < cfg->n_roms; i++) {
        GemuMemory tmp = {.data = s->rom, .size = sizeof(s->rom)};
        size_t len = 0;
        gemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, &len);
        uint32_t end = cfg->roms[i].addr + (uint32_t)len;
        if (end > s->rom_size) s->rom_size = end;
    }
    pecom_maps_sync(s);
}

void rca_pecom32_destroy(RcaPecom32State *s) {
    if (!s) return;
    pecom_core_free(s);
    gemu_monitor_destroy(s->monitor);
    free(s);
}

/* ── Input polling ───────────────────────────────────────────────────────── */

static void pecom_press(uint8_t keys[64], const char *name) {
    for (size_t i = 0; i < PECOM_N_KEYS; i++) {
        if (strcmp(name, pecom_keymap[i].name) == 0) {
            keys[pecom_keymap[i].row] |= (uint8_t)(1u << pecom_keymap[i].bit);
            return;
        }
    }
}

static void pecom_poll_keys(RcaPecom32State *s, GemuDisplay *display) {
    if (!display) return;

    memset(s->keys, 0, sizeof(s->keys));
    for (size_t i = 0; i < PECOM_N_KEYS; i++) {
        if (gemu_display_is_key_held(display, pecom_keymap[i].name))
            s->keys[pecom_keymap[i].row] |= (uint8_t)(1u << pecom_keymap[i].bit);
    }
    /* Backspace is the Pecom's BS/DEL key (the "End" matrix position). */
    if (gemu_display_is_key_held(display, "Backspace"))
        pecom_press(s->keys, "End");

    /* A key pressed and released within one frame (fast typists, VNC
     * clients, paste) is no longer held when we poll, so also press each
     * character typed this frame for one frame - long enough for the ROM's
     * matrix scan to see it.  Only characters that are their own physical
     * key; shifted symbols are left to the held state plus Shift. */
    static const char us_shifted[] = "!@#$%^&*()_+{}|:\"~<>?";
    uint32_t c;
    while ((c = gemu_display_pop_raw_key(display)) != 0) {
        char single[2] = {0, 0};
        const char *name = NULL;
        if      (c == '\r') name = "Return";
        else if (c == '\b') name = "End";
        else if (c == ' ')  name = "Space";
        else if (c > 0x20 && c < 0x7f && !strchr(us_shifted, (int)c)) {
            single[0] = (char)((c >= 'A' && c <= 'Z') ? (c | 0x20u) : c);
            name = single;
        }
        if (name) pecom_press(s->keys, name);
    }

    s->key_esc   = gemu_display_is_key_held(display, "Escape");
    s->key_shift = gemu_display_is_key_held(display, "Left Shift")
                || gemu_display_is_key_held(display, "Right Shift");
    s->key_ctrl  = gemu_display_is_key_held(display, "Left Ctrl")
                || gemu_display_is_key_held(display, "Right Ctrl");

    /* Caps Lock toggles the CAPS latch (EF3) on each press. */
    bool caps = gemu_display_is_key_held(display, "CapsLock");
    if (caps && !s->caps_key_prev) s->caps_locked = !s->caps_locked;
    s->caps_key_prev = caps;
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

/* Draw the CDP1869 picture if video RAM or registers changed since the last
 * time (running, or stepped from the monitor while paused). */
/* The board's core screen to the window, when the VIS has changed. */
static void pecom_present(RcaPecom32State *s, GemuDisplay *display) {
    if (!s->vis->dirty) return;
    const GemuBitmap *b = gemu_screen_render(s->core->screen);
    if (display) gemu_display_render(display, b->pix, b->width, b->height);
}

void rca_pecom32_run(RcaPecom32State *s, const RcaConfig *cfg) {
    GemuDisplay *display = NULL;
    if (gemu_display_wanted(cfg->display_type)) {
        display = gemu_display_create(cfg->display_type,
            &(GemuDisplayConfig){
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

    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        if (display) {
            gemu_display_poll(display);
            if (gemu_display_should_quit(display)) break;
            if (gemu_display_reset_requested(display)) {
                gemu_display_clear_flags(display);
                rca_pecom32_reset(s, cfg);
            }
            pecom_poll_keys(s, display);
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)  { if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true); else quit = true; }
            else if (cmd == GEMU_MON_RESET) rca_pecom32_reset(s, cfg);
            else if (cmd == GEMU_MON_STEP) {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                for (uint32_t k = 0; k < n; k++) {
                    /* cdp1802_step advances one machine cycle; run until an
                     * instruction completes (bounded, in case of IDL). */
                    uint64_t start = s->cpu->insn_count;
                    for (int i = 0; i < 1024 && s->cpu->insn_count == start; i++)
                        pecom_cycle(s);
                }
            }
        }
        if (quit) break;
        gemu_display_set_paused(display, gemu_monitor_is_paused(s->monitor));

        bool paused = gemu_monitor_is_paused(s->monitor);
        if (!paused) pecom_frame(s);   /* the cpu device's executor */
        pecom_present(s, display);     /* (paused: shows monitor steps) */

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
