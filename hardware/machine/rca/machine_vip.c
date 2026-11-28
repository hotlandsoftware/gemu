#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif
#include "vip.h"
#include "cdp1802_dev.h"
#include "cdp1861_dev.h"
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
#include "input/rca_keyboard.h"
#include "gemu/gemu.h"
#include "gemu/memory.h"
#include "gemu/screendump.h"
#include "gemu/gemu_display.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#endif

static void rca_vip_reset(RcaVipState *s, const RcaConfig *cfg);

static RcaVipState *crash_state;

static const uint32_t vip_vis_palette[8] = {
    0xFF000000u, 0xFF00FF00u, 0xFF0000FFu, 0xFF00FFFFu,
    0xFFFF0000u, 0xFFFFFF00u, 0xFFFF00FFu, 0xFFFFFFFFu,
};

static const char *cpu_state_name(Cdp1802CycleState state) {
    switch (state) {
    case CDP1802_S_EXECUTE:   return "EXECUTE";
    case CDP1802_S_FETCH:     return "FETCH";
    case CDP1802_S_DMA:       return "DMA";
    case CDP1802_S_INTERRUPT: return "INTERRUPT";
    default:                  return "INVALID";
    }
}

static uint8_t mem8(const RcaVipState *s, uint16_t addr) {
    return s->mem[addr & (VIP_MEM_SIZE - 1u)];
}

static bool vip_ascii_pending(const RcaVipState *s) {
    return rca_vip_has_vp601(s->cfg) && s->ascii_key >= 0;
}

static void vip_dump_state(const RcaVipState *s, const char *reason) {
    if (!s) return;

    const Cdp1802 *c = s->cpu;   /* (s->vdc may be NULL) */
    uint16_t rp = c->R[c->P];
    uint16_t rx = c->R[c->X];
    uint16_t op_addr = (c->state == CDP1802_S_EXECUTE) ? (uint16_t)(rp - 1u) : rp;
    uint8_t op = mem8(s, op_addr);
    unsigned vram_on = 0;
    for (unsigned i = 0; i < sizeof(s->vram); i++)
        vram_on += s->vram[i] != 0;

    fprintf(stderr, "\n==== GEMU RCA fatal dump ====\n");
    fprintf(stderr, "reason: %s\n", reason ? reason : "unknown");
    fprintf(stderr, "cycles=%llu instructions=%llu state=%s idle=%d exec_left=%d\n",
            (unsigned long long)c->cycle_count,
            (unsigned long long)c->insn_count,
            cpu_state_name(c->state), c->idle, c->exec_left);
    fprintf(stderr, "P=%X X=%X I=%X N=%X opcode=%02X @ %04X D=%02X DF=%u B=%02X T=%02X Q=%u IE=%u\n",
            c->P, c->X, c->I, c->N, op, op_addr, c->D, c->DF, c->B, c->T, c->Q, c->IE);
    fprintf(stderr, "EF1=%u EF2=%u EF3=%u EF4=%u RP=%04X RX=%04X\n",
            c->EF[0], c->EF[1], c->EF[2], c->EF[3], rp, rx);

    for (int i = 0; i < 16; i += 4) {
        fprintf(stderr, "R%X=%04X R%X=%04X R%X=%04X R%X=%04X\n",
                i, c->R[i], i + 1, c->R[i + 1],
                i + 2, c->R[i + 2], i + 3, c->R[i + 3]);
    }

    fprintf(stderr, "pending: dma_out=%d irq=%d dma_count=%u dma_head=%u\n",
            c->dma_out_pending, c->irq_pending,
            c->dma_count, c->dma_head);
    if (s->vdc)
        fprintf(stderr, "pixie: display_on=%d line=%u mcycle=%u display_addr=%u int_fired=%d draw=%d vram_on=%u\n",
                s->vdc->display_on, s->vdc->line_counter, s->vdc->mcycle,
                s->vdc->display_addr, s->vdc->int_fired, s->draw_flag, vram_on);
    fprintf(stderr, "input: keyboard=%s key_down=%d ascii_key=%d ef2_down=%d\n",
            s->cfg ? rca_vip_keyboard_name(s->cfg->keyboard) : "unknown",
            s->key_down, s->ascii_key, s->ef2_down);

    fprintf(stderr, "roms:\n");
    if (s->cfg) {
        for (int i = 0; i < s->cfg->n_roms; i++)
            fprintf(stderr, "  %04X  %s\n",
                    (unsigned)s->cfg->roms[i].addr, s->cfg->roms[i].path);
    }

    fprintf(stderr, "memory around opcode:\n");
    uint16_t base = (uint16_t)(op_addr - 8u);
    for (int row = 0; row < 2; row++) {
        uint16_t addr = (uint16_t)(base + row * 16);
        fprintf(stderr, "  %04X:", addr);
        for (int col = 0; col < 16; col++)
            fprintf(stderr, " %02X", mem8(s, (uint16_t)(addr + col)));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "=============================\n");
}

static void vip_cpu_panic(Cdp1802 *cpu, const char *reason, void *ud) {
    (void)cpu;
    vip_dump_state((const RcaVipState *)ud, reason);
    exit(1);
}

static void vip_signal_handler(int sig) {
    char reason[64];
    snprintf(reason, sizeof(reason), "host signal %d", sig);
    vip_dump_state(crash_state, reason);
    fflush(stderr);
    _Exit(128 + sig);
}

static void vip_install_crash_handlers(RcaVipState *s) {
    crash_state = s;
    signal(SIGSEGV, vip_signal_handler);
    signal(SIGABRT, vip_signal_handler);
    signal(SIGFPE,  vip_signal_handler);
    signal(SIGILL,  vip_signal_handler);
#ifdef SIGBUS
    signal(SIGBUS,  vip_signal_handler);
#endif
}

/* ── DMA callback: CDP1861 → vram ────────────────────────────────────────── */

static void vip_dma_out(uint8_t *data, void *ud) {
    RcaVipState *s = ud;
    Cdp1861     *vdc = s->vdc;

    int row = vdc->line_counter - CDP1861_FIRST_LINE;
    if (row < 0 || row >= CDP1861_DISPLAY_H) return;

    int byte_col = vdc->display_addr % CDP1861_BYTES_PER_LINE;
    uint8_t b = *data;
    for (int bit = 0; bit < 8; bit++)
        s->vram[row * CDP1861_DISPLAY_W + byte_col * 8 + bit] = (b >> (7 - bit)) & 1u;

    vdc->display_addr++;
    if (vdc->display_addr >= (CDP1861_DISPLAY_W * CDP1861_DISPLAY_H / 8))
        vdc->display_addr = 0;

    s->draw_flag = true;
}

/* ── CPU I/O callbacks ───────────────────────────────────────────────────── */

/* Monitor memory view: side-effect-free.  With a CDP1869, character RAM
 * reads latch the PCB bit, so that window is not inspectable. */
static bool vip_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    RcaVipState *s = ud;
    if (addr > 0xffffu) return false;
    if (s->cfg->vga == RCA_VGA_CDP1869) {
        if (addr >= 0xf400 && addr <= 0xf7ff)
            return false;
        if (addr >= 0xf800) {
            *val = cdp1869_page_read(s->vis, (uint16_t)(addr - 0xf800));
            return true;
        }
    }
    *val = s->mem[addr];
    return true;
}

static void vip_bus_poke(RcaVipState *s, uint16_t addr, uint8_t val);

static bool vip_mon_write(void *ud, uint64_t addr, uint8_t val) {
    RcaVipState *s = ud;
    if (addr > 0xffffu) return false;
    vip_bus_poke(s, (uint16_t)addr, val);   /* same as a CPU write */
    return true;
}

static void vip_mon_cpu_state(void *ud, char *buf, size_t len) {
    const RcaVipState *s = ud;
    cdp1802_format_state(s->cpu, buf, len);
}

static void vip_sync(void *ud) {
    RcaVipState *s = ud;
    if (s->cfg->vga == RCA_VGA_CDP1861)
        cdp1861_sync(s->vdc, s->cpu);
    else if (s->cfg->vga == RCA_VGA_CDP1869)
        cdp1869_sync(s->vis, s->cpu);
}

static void vip_tone_gate(RcaVipState *s, uint8_t q);

static void vip_q_out(uint8_t q, void *ud) {
    RcaVipState *s = ud;
    vip_tone_gate(s, q);
    if (s->cfg->vga == RCA_VGA_CDP1869)
        s->vis->q = q & 1u;
}

static uint8_t vip_io_in(uint8_t port, void *ud) {
    RcaVipState *s = ud;
    if (port == 1) {
        /*
         * IN 1 on the COSMAC VIP: the SC lines from the 1802 assert the CDP1861
         * display-enable pin.  The instruction simultaneously reads the hex keypad
         * value off the bus (0x0-0xF, or 0xFF if no key is pressed).
         */
        if (s->cfg->vga == RCA_VGA_CDP1861)
            cdp1861_set_display(s->vdc, true);
        if (!rca_vip_has_keypad(s->cfg))
            return 0xFF;
        return (s->key_down >= 0) ? (uint8_t)s->key_down : 0xFF;
    }
    if (port == 3) {
        /* IN 3: ASCII keyboard (for BASIC/text programs).
         * EF4 is the active-low strobe; reading the port consumes the key. */
        if (!rca_vip_has_vp601(s->cfg))
            return 0xFF;
        if (s->ascii_key >= 0) {
            uint8_t key = (uint8_t)s->ascii_key;
            s->ascii_key = -1;
            s->cpu->EF[3] = true;  /* EF4 deasserted: no more key pending */
            return key;
        }
        return 0xFF;
    }
    return 0xFF;
}

static void vip_io_out(uint8_t port, uint8_t val, void *ud) {
    RcaVipState *s = ud;
    (void)val;
    if (port == 1) {
        /* OUT 1 on the COSMAC VIP disables the CDP1861 display. */
        if (s->cfg->vga == RCA_VGA_CDP1861)
            cdp1861_set_display(s->vdc, false);
    } else if (s->cfg->vga == RCA_VGA_CDP1869 && port >= 3 && port <= 7) {
        cdp1869_out(s->vis, port, s->cpu->memory_addr, val);
    }
}

/* ── Machine lifecycle ───────────────────────────────────────────────────── */

static bool vip_fpb_basic_loaded(const RcaVipState *s) {
    static const uint8_t marker[] = "BASIC-REL V2.2";
    return memcmp(&s->mem[0x1025], marker, sizeof(marker) - 1) == 0;
}

static bool vip_start_addr(const RcaVipState *s, const RcaConfig *cfg,
                           uint16_t *addr) {
    if (cfg->has_start_addr) {
        *addr = cfg->start_addr;
        return true;
    }

    if (vip_fpb_basic_loaded(s)) {
        *addr = 0x1000;
        return true;
    }

    return false;
}

static void vip_apply_start_addr(RcaVipState *s, const RcaConfig *cfg) {
    uint16_t addr;
    if (!vip_start_addr(s, cfg, &addr))
        return;

    s->cpu->X = 0;
    s->cpu->P = 0;
    s->cpu->R[0] = addr;
    s->cpu->state = CDP1802_S_FETCH;
    s->cpu->exec_left = 0;
    s->cpu->idle = false;
    s->cpu->init_pending = false;
}

/* One machine cycle: the EF inputs (EF2 the front panel, EF3 the tape or
 * the VP-601's data-ready, EF4 a key), then the CPU's cycle. */
static void vip_cycle(RcaVipState *s) {
    Cdp1802 *cpu = s->cpu;
    cpu->EF[1] = !s->ef2_down;
    if (s->tape.playing)
        cpu->EF[2] = (bool)vip_tape_ef3(&s->tape, cpu->cycle_count);
    else
        cpu->EF[2] = !rca_vip_has_vp601(s->cfg) || s->ascii_key < 0;
    cpu->EF[3] = !vip_ascii_pending(s) && (!rca_vip_has_keypad(s->cfg) || s->key_down < 0);
    cdp1802_step(cpu);
}

static void vip_step_instruction(RcaVipState *s) {
    uint64_t start = s->cpu->insn_count;
    for (int i = 0; i < 1024 && s->cpu->insn_count == start; i++)
        vip_cycle(s);
}

static GemuMediaResult vip_media_change_tape(void *ud, const char *arg,
                                             char *err, size_t err_len) {
    RcaVipState *s = ud;
    if (!arg || !arg[0]) {
        snprintf(err, err_len, "missing tape path");
        return GEMU_MEDIA_ERR;
    }

    uint16_t addr = 0x0000;
    const char *file = arg;
    const char *colon = strchr(arg, ':');
    if (colon && colon != arg) {
        char addr_buf[32] = {0};
        size_t alen = (size_t)(colon - arg);
        if (alen < sizeof(addr_buf)) {
            memcpy(addr_buf, arg, alen);
            addr = (uint16_t)strtoul(addr_buf, NULL, 0);
            file = colon + 1;
        }
    }

    if (!vip_tape_load(&s->tape, file, addr, s->cpu->cycle_count)) {
        snprintf(err, err_len, "failed to load tape '%s'", file);
        return GEMU_MEDIA_ERR;
    }
    return GEMU_MEDIA_OK;
}

static GemuMediaResult vip_media_eject_tape(void *ud,
                                            char *err, size_t err_len) {
    (void)err;
    (void)err_len;
    RcaVipState *s = ud;
    vip_tape_eject(&s->tape);
    printf("tape: ejected\n");
    return GEMU_MEDIA_OK;
}

static void vip_media_status_tape(void *ud, char *buf, size_t buf_len) {
    const RcaVipState *s = ud;
    if (s->tape.path[0])
        snprintf(buf, buf_len, "%s%s", s->tape.path,
                 s->tape.playing ? " (playing)" : "");
    else
        snprintf(buf, buf_len, "empty");
}

/* ── VIP action table ────────────────────────────────────────────────────── */
/*
 * Bits 0-15: hex keypad keys 0-F
 * Bit 16: EF2 (active-high; mapped to Tab)
 * Default key layout matches CHIP-8 convention.
 */
static const GemuActionDef vip_actions[] = {
    { "0",   GEMU_ACTION(0),  "x"   }, { "1",   GEMU_ACTION(1),  "1"   },
    { "2",   GEMU_ACTION(2),  "2"   }, { "3",   GEMU_ACTION(3),  "3"   },
    { "4",   GEMU_ACTION(4),  "q"   }, { "5",   GEMU_ACTION(5),  "w"   },
    { "6",   GEMU_ACTION(6),  "e"   }, { "7",   GEMU_ACTION(7),  "a"   },
    { "8",   GEMU_ACTION(8),  "s"   }, { "9",   GEMU_ACTION(9),  "d"   },
    { "A",   GEMU_ACTION(10), "z"   }, { "B",   GEMU_ACTION(11), "c"   },
    { "C",   GEMU_ACTION(12), "4"   }, { "D",   GEMU_ACTION(13), "r"   },
    { "E",   GEMU_ACTION(14), "f"   }, { "F",   GEMU_ACTION(15), "v"   },
    { "EF2", GEMU_ACTION(16), "Tab" },
};
#define VIP_N_ACTIONS ((int)(sizeof(vip_actions)/sizeof(vip_actions[0])))

static void vip_poll_display(RcaVipState *s, const RcaConfig *cfg,
                             GemuDisplay *display, bool *quit) {
    uint32_t held = gemu_display_poll(display);
    if (gemu_display_should_quit(display)) {
        *quit = true;
        return;
    }
    if (gemu_display_reset_requested(display)) {
        gemu_display_clear_flags(display);
        rca_vip_reset(s, cfg);
    }

    s->ef2_down = (held >> 16) & 1;

    /* VP-601 ASCII keyboard: drain raw key queue */
    if (rca_vip_has_vp601(cfg)) {
        uint32_t raw;
        while ((raw = gemu_display_pop_raw_key(display)) != 0) {
            int ascii = (int)raw;
            if (ascii >= 'a' && ascii <= 'z') ascii -= 32;  /* uppercase */
            if (ascii == 0x7f) ascii = '\b';                /* Del → BS  */
            if ((ascii >= 0x20 && ascii <= 0x7e) ||
                ascii == '\r' || ascii == '\b' || ascii == 0x1B)
                s->ascii_key = ascii;
        }
    }

    if (rca_vip_has_keypad(cfg)) {
        for (int k = 0; k < 16; k++)
            s->keys[k] = (held >> k) & 1;
    }
}

static bool vip_screendump(void *ud, const char *path) {
    RcaVipState *s = ud;
    const uint8_t *buf;
    int w, h;
    if (s->cfg->vga == RCA_VGA_CDP1869) {
        buf = s->vis->bitmap; w = CDP1869_VISIBLE_W; h = CDP1869_VISIBLE_H;
    } else {
        buf = s->vram;       w = CDP1861_DISPLAY_W; h = CDP1861_DISPLAY_H;
    }
    return gemu_screendump_mono(path, buf, w, h);
}

static bool vip_core_build(RcaVipState *s);
static void vip_core_free(RcaVipState *s);

/* ── The core machine (stage R2) ─────────────────────────────────────────────
 * board (vip-board): the 64 KB of memory, the CDP1861's frame, the keys,
 * the tape's position; cpu (cdp1802): the machine's executor at 220,000
 * machine cycles a second, a machine cycle at a time (vip_cycle: the EF
 * inputs, then the CPU's cycle); vdc (cdp1861) or vis (cdp1869, with a
 * VP-590); tone (with -soundhw pcspk): Q's 250 Hz beeper, through the mixer.
 * The bus, "program": the memory, and with a CDP1869 its character RAM
 * ($F400) and page RAM ($F800) over it.  With a CDP1861 it is all memory,
 * so the CPU reads it directly.
 * The board's "screen" is the CDP1861's 64x128 frame, or the CDP1869's
 * 240x216 bitmap.  Breakpoints stop at an instruction's fetch (before
 * stage R they worked only under step). */
typedef struct RcaVipCore {
    GemuMachine   *machine;
    GemuDevice    *board, *cpu, *vdc, *vis, *tone;
    GemuSpace     *bus;
    GemuSpaceCache cache;
    GemuScreen    *screen;
    GemuAudioOut  *audio;
} RcaVipCore;

#define VIP_MCYCLE_HZ CDP1802_MCYCLES_PER_SEC

static uint8_t vip_bus_read(uint16_t addr, void *ud) {
    return gemu_space_cache_read8(&((RcaVipState *)ud)->core->cache, addr);
}
static void vip_bus_write(uint16_t addr, uint8_t val, void *ud) {
    gemu_space_cache_write8(&((RcaVipState *)ud)->core->cache, addr, val);
}
static uint8_t vis_char_r(void *ud, uint64_t off) {
    return cdp1869_char_read(((RcaVipState *)ud)->vis, (uint16_t)off);
}
static void vis_char_w(void *ud, uint64_t off, uint8_t v) {
    cdp1869_char_write(((RcaVipState *)ud)->vis, (uint16_t)off, v);
}
static uint8_t vis_page_r(void *ud, uint64_t off) {
    return cdp1869_page_read(((RcaVipState *)ud)->vis, (uint16_t)off);
}
static void vis_page_w(void *ud, uint64_t off, uint8_t v) {
    cdp1869_page_write(((RcaVipState *)ud)->vis, (uint16_t)off, v);
}
static const GemuHandlers vis_char_handlers = { .r8 = vis_char_r, .w8 = vis_char_w };
static const GemuHandlers vis_page_handlers = { .r8 = vis_page_r, .w8 = vis_page_w };

static void vip_board_describe(GemuDevice *d, char *buf, size_t len) {
    const RcaVipState *s = *(RcaVipState **)d->priv;
    snprintf(buf, len, "%s, keyboard %s; key %d, ASCII key %d%s; tape %s",
             s->vis ? "VP-590 colour (CDP1869)" : "CDP1861", rca_vip_keyboard_name(s->cfg->keyboard),
             s->key_down, s->ascii_key, s->ef2_down ? ", EF2 held" : "",
             s->tape.n_bits ? (s->tape.playing ? "playing" : "stopped") : "none");
}

static const GemuDeviceType vip_board_type = {
    .name = "vip-board", .desc = "COSMAC VIP board (memory, keypad, tape)",
    .size = sizeof(RcaVipState *), .describe = vip_board_describe,
};

#define B(field) ok &= GEMU_STATE_MEMBER(d, "", s, field)
/* The board's state: the memory, the CDP1861's frame, the pending ASCII
 * key, the tape's position.  Not saved: the host's keys (keys, key_down,
 * ef2_down: set from the input each frame), the tape's bits (media). */
static bool vip_board_state(RcaVipState *s, GemuDevice *d) {
    bool ok = true;
    B(mem); B(vram); B(ascii_key);
    ok &= GEMU_STATE_MEMBER(d, "tape.", &s->tape, bit_pos) &&
          GEMU_STATE_MEMBER(d, "tape.", &s->tape, phase) &&
          GEMU_STATE_MEMBER(d, "tape.", &s->tape, ef3) &&
          GEMU_STATE_MEMBER(d, "tape.", &s->tape, next_toggle) &&
          GEMU_STATE_MEMBER(d, "tape.", &s->tape, playing);
    return ok;
}
#undef B

static bool vip_stop_before(void *ud) {
    RcaVipState *s = ud;
    return gemu_monitor_check_exec(s->monitor, s->cpu->R[s->cpu->P]);
}
static void vip_hook_cycle(void *ud) { vip_cycle(ud); }
static bool vip_stop_after(void *ud) { return gemu_monitor_is_paused(((RcaVipState *)ud)->monitor); }

static void vip_screen_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    RcaVipState *s = ud;
    if (s->vis) {
        cdp1869_render(s->vis);
        for (int y = clip->y0; y < clip->y1; y++)
            for (int x = 0; x < CDP1869_VISIBLE_W; x++)
                bmp->pix[(size_t)y * (size_t)bmp->stride + (size_t)x] =
                    vip_vis_palette[s->vis->bitmap[y * CDP1869_VISIBLE_W + x] & 7];
    } else {
        for (int y = clip->y0; y < clip->y1; y++)
            for (int x = 0; x < CDP1861_DISPLAY_W; x++)
                bmp->pix[(size_t)y * (size_t)bmp->stride + (size_t)x] =
                    s->vram[y * CDP1861_DISPLAY_W + x] ? 0xFFFFFFFFu : 0xFF100080u;
    }
}

static void vip_sound_out(void *ud, const float *samples, size_t frames) {
    gemu_audio_queue(ud, samples, frames);
}

static bool vip_core_build(RcaVipState *s) {
    const RcaConfig *cfg = s->cfg;
    RcaVipCore *c = s->core = calloc(1, sizeof *c);
    if (!c || !(c->machine = gemu_machine_new("vip")))
        return false;
    bool vis = cfg->vga == RCA_VGA_CDP1869, vdc = cfg->vga == RCA_VGA_CDP1861;
    bool ok = (c->board = gemu_device_add(c->machine, &vip_board_type, "board", NULL, NULL)) &&
              (c->cpu = gemu_device_add(c->machine, &gemu_cdp1802_type, "cpu", NULL, NULL));
    if (ok && vdc) ok = (c->vdc = gemu_device_add(c->machine, &gemu_cdp1861_type, "vdc", NULL, NULL));
    if (ok && vis) ok = (c->vis = gemu_device_add(c->machine, &gemu_cdp1869_type, "vis", NULL, NULL));
    if (ok && cfg->sound_hw == RCA_SOUND_PCSPK)
        ok = (c->tone = gemu_device_add(c->machine, &gemu_tone_type, "tone", NULL, "freq=250"));
    if (ok) {
        *(RcaVipState **)c->board->priv = s;
        ok = vip_board_state(s, c->board);
    }
    if (!ok) {
        fprintf(stderr, "gemu-rca: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    bool inited = gemu_machine_init(c->machine);
    if (inited) {
        s->cpu = gemu_cdp1802(c->cpu);
        s->vdc = c->vdc ? gemu_cdp1861(c->vdc) : NULL;
        s->vis = c->vis ? gemu_cdp1869(c->vis) : NULL;
        Cdp1802 *cpu = s->cpu;
        cpu->mem = s->mem;
        cpu->mem_mask = VIP_MEM_SIZE - 1u;
        cpu->on_sync = vip_sync;
        cpu->q_out   = vip_q_out;
        cpu->io_in   = vip_io_in;
        cpu->io_out  = vip_io_out;
        if (vis) {                 /* the VIS's RAM is on the bus; else plain memory */
            cpu->mem_read  = vip_bus_read;
            cpu->mem_write = vip_bus_write;
        }
        cpu->io_ud   = s;
        cpu->panic   = vip_cpu_panic;
        cpu->panic_ud = s;
        if (s->vdc) {
            gemu_cdp1861_set_dma(c->vdc, vip_dma_out, s);
            gemu_cdp1802_set_dma_target(c->cpu, vip_dma_out, s);
        }
    }
    GemuCdp1802Hooks h = { .stop_before = vip_stop_before, .cycle = vip_hook_cycle,
                           .stop_after = vip_stop_after, .ud = s };
    ok = inited && (c->bus = gemu_space_new(c->machine, "program", 16, 8, GEMU_BIG_ENDIAN)) &&
         gemu_space_map_ram(c->bus, 0, VIP_MEM_SIZE, s->mem, 0, 0) &&
         (!vis || (gemu_space_map(c->bus, 0xF400, 0x400, &vis_char_handlers, s, 1, 0) &&
                   gemu_space_map(c->bus, 0xF800, 0x800, &vis_page_handlers, s, 1, 0))) &&
         gemu_cdp1802_exec(c->cpu, VIP_MCYCLE_HZ, &h) &&
         (c->screen = gemu_screen_new(c->board, "screen")) &&
         gemu_screen_set_size(c->screen, vis ? CDP1869_VISIBLE_W : CDP1861_DISPLAY_W,
                              vis ? CDP1869_VISIBLE_H : CDP1861_DISPLAY_H);
    if (ok) {
        c->cache.space = c->bus;
        gemu_screen_set_update(c->screen, vip_screen_update, s);
        if (c->tone) c->audio = gemu_audio_open("rca-vip", 44100);
        gemu_mixer_set_output(c->machine, 44100, 1, c->audio ? vip_sound_out : NULL, c->audio);
    }
    if (!ok || !gemu_machine_start(c->machine)) {
        fprintf(stderr, "gemu-rca: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    gemu_monitor_attach_machine(s->monitor, c->machine, "program");
    return true;
}

static void vip_bus_poke(RcaVipState *s, uint16_t addr, uint8_t val) {
    gemu_space_write8(s->core->bus, addr, val);
}

static void vip_tone_gate(RcaVipState *s, uint8_t q) {
    if (s->core && s->core->tone) gemu_tone_set_gate(s->core->tone, q);
}

static void vip_core_free(RcaVipState *s) {
    if (!s->core) return;
    gemu_machine_free(s->core->machine);
    gemu_audio_close(s->core->audio);
    free(s->core);
    s->core = NULL;
}

/* One frame (the CDP1861's 262 lines, or the CDP1869's 312, of 14 machine
 * cycles): the key held now on EF4, the machine run for the frame's time
 * (a stop leaves it to finish next time), the frame's sound out. */
static void vip_frame(RcaVipState *s) {
    const RcaConfig *cfg = s->cfg;
    RcaVipCore *c = s->core;
    s->key_down = -1;
    if (rca_vip_has_keypad(cfg))
        for (int k = 0; k < 16; k++) if (s->keys[k]) { s->key_down = k; break; }
    s->cpu->EF[3] = !vip_ascii_pending(s) && (!rca_vip_has_keypad(cfg) || s->key_down < 0);
    uint64_t n = cfg->vga == RCA_VGA_CDP1869 ? CDP1869_MCYCLES_PER_FRAME : CDP1861_MCYCLES_PER_FRAME;
    gemu_machine_run_until(c->machine, gemu_machine_time(c->machine) + gemu_clocks(VIP_MCYCLE_HZ, n));
    gemu_mixer_run(c->machine);
    gemu_screen_frame_begin(c->screen);
}

RcaVipState *rca_vip_create(const RcaConfig *cfg) {
    RcaVipState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg = cfg;
    s->monitor = gemu_monitor_create();
    gemu_monitor_set_screendump_cb(s->monitor, vip_screendump, s);
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = vip_mon_read, .write = vip_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = true });
    gemu_monitor_set_cpu_state_cb(s->monitor, vip_mon_cpu_state, s);
    {
        GemuMediaDevice tape_dev = {
            .name   = "tape",
            .kind   = "tape",
            .ud     = s,
            .change = vip_media_change_tape,
            .eject  = vip_media_eject_tape,
            .status = vip_media_status_tape,
        };
        if (s->tape.path[0])
            snprintf(tape_dev.file, sizeof(tape_dev.file), "%s", s->tape.path);
        gemu_monitor_register_media(s->monitor, &tape_dev);
    }
    if (!vip_core_build(s)) {
        rca_vip_destroy(s);
        return NULL;
    }

    s->key_down = -1;
    s->ascii_key = -1;

    vip_tape_init(&s->tape);
    if (cfg->tape_path)
        vip_tape_load(&s->tape, cfg->tape_path, cfg->tape_addr, 0);

    GemuMemory tmp = {.data = s->mem, .size = VIP_MEM_SIZE};
    for (int i = 0; i < cfg->n_roms; i++) {
        size_t len = 0;
        if (!gemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, &len)) {
            fprintf(stderr, "gemu-rca: failed to load '%s'\n", cfg->roms[i].path);
            free(s);
            return NULL;
        }
        printf("gemu-rca: %zu bytes @ 0x%04X  ← %s\n",
               len, cfg->roms[i].addr, cfg->roms[i].path);
        gemu_monitor_register_rom(s->monitor, cfg->roms[i].addr, (uint32_t)len,
                                  cfg->roms[i].path);
    }
    vip_apply_start_addr(s, cfg);
    return s;
}

static void rca_vip_reset(RcaVipState *s, const RcaConfig *cfg) {
    cdp1802_reset(s->cpu);
    if (s->core->tone) gemu_tone_set_gate(s->core->tone, false);
    if (s->vdc) cdp1861_reset(s->vdc);
    if (s->vis) cdp1869_reset(s->vis);
    memset(s->vram, 0, sizeof(s->vram));
    s->draw_flag = true;
    s->key_down  = -1;
    s->ascii_key = -1;
    s->ef2_down  = false;
    memset(s->keys, 0, sizeof(s->keys));
    /* Rewind tape to the beginning on reset */
    if (s->tape.n_bits > 0) {
        s->tape.bit_pos     = 0;
        s->tape.phase       = 0;
        s->tape.ef3         = 0;
        s->tape.next_toggle = s->cpu->cycle_count + VIP_TAPE_HP_ZERO;
        s->tape.playing     = true;
    }

    GemuMemory tmp = {.data = s->mem, .size = VIP_MEM_SIZE};
    for (int i = 0; i < cfg->n_roms; i++)
        gemu_mem_load_file(&tmp, cfg->roms[i].addr, cfg->roms[i].path, NULL);

    vip_apply_start_addr(s, cfg);
}

void rca_vip_destroy(RcaVipState *s) {
    if (!s) return;
    if (crash_state == s) crash_state = NULL;
    vip_core_free(s);
    vip_tape_destroy(&s->tape);
    gemu_monitor_destroy(s->monitor);
    free(s);
}

/* ── Run loop ─────────────────────────────────────────────────────────────── */

/* Draw the VIP's screen (the board's core screen): the VP-590/CDP1869
 * picture every time, the CDP1861's when it has drawn since the last time. */
static void vip_present(RcaVipState *s, const RcaConfig *cfg, GemuDisplay *display) {
    if (!display) return;
    if (cfg->vga == RCA_VGA_CDP1869 || s->draw_flag) {
        const GemuBitmap *b = gemu_screen_render(s->core->screen);
        gemu_display_render(display, b->pix, b->width, b->height);
        s->draw_flag = false;
    }
}

void rca_machine_run(RcaVipState *s, const RcaConfig *cfg) {
    vip_install_crash_handlers(s);

    int fw = CDP1861_DISPLAY_W, fh = CDP1861_DISPLAY_H;
    if (cfg->vga == RCA_VGA_CDP1869) { fw = CDP1869_VISIBLE_W; fh = CDP1869_VISIBLE_H; }

    GemuDisplay *display = NULL;
    if (cfg->vga != RCA_VGA_NONE && gemu_display_wanted(cfg->display_type)) {
        display = gemu_display_create(cfg->display_type,
            &(GemuDisplayConfig){
                .fb_width    = fw,
                .fb_height   = fh,
                .scale       = cfg->display_scale,
                .renderer    = GEMU_RENDERER_AUTO,
                .actions     = vip_actions,
                .n_actions   = VIP_N_ACTIONS,
                .ini_section = "rca-vip",
                .gtk         = &(GemuDisplayGtkExtras){ .monitor = s->monitor },
            });
        if (!display) {
            fprintf(stderr, "gemu-rca: failed to create display\n");
            return;
        }
        gemu_monitor_set_input_reset_cb(s->monitor,
                                        gemu_display_reset_input_bindings_ud,
                                        display);
    }
    gemu_monitor_start(s->monitor);

    const bool is_pal = (cfg->vga == RCA_VGA_CDP1869);
    const unsigned frame_ms = is_pal ? 20u : 16u;
    GemuMachine *m = s->core->machine;
    bool quit = false;

    while (!quit) {
        /* ── Input ── */
        if (display) {
            vip_poll_display(s, cfg, display, &quit);
            if (quit) break;
        }

        /* Monitor */
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)  { if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true); else { quit = true; break; } }
            else if (cmd == GEMU_MON_RESET) rca_vip_reset(s, cfg);
            else if (cmd == GEMU_MON_STEP) {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                for (uint32_t i = 0; i < n; i++) vip_step_instruction(s);
            }
        }
        if (quit) break;
        gemu_display_set_paused(display, gemu_monitor_is_paused(s->monitor));

        if (gemu_monitor_is_paused(s->monitor)) {
            vip_present(s, cfg, display);   /* show monitor steps */
            gemu_machine_pace_reset(m);
            gemu_sleep_ms(frame_ms);
            continue;
        }

        /* ── One frame (the cpu device's executor), paced to its time ── */
        vip_frame(s);
        vip_present(s, cfg, display);
        gemu_machine_pace(m, 0);
    }

    printf("gemu-rca: %llu machine cycles, %llu instructions\n",
           (unsigned long long)s->cpu->cycle_count,
           (unsigned long long)s->cpu->insn_count);
    gemu_monitor_stop(s->monitor);
    gemu_display_destroy(display);
}
