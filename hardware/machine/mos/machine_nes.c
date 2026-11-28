/*
 * The NES / Famicom console: CPU bus, PPU, APU and their timing, DMA, the
 * controller ports, the cartridge connector (device/cart/nes_cart.h) and
 * its faults, the FDS RAM adapter, the VS. System's inputs, reset and save
 * states.  Everything the player sees and touches (the window, input
 * bindings, dialogs, save files, netplay, frame pacing) is the front end's:
 * nes_frontend.c and nes_netplay.c, through the API in nes.h.
 */
#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif
#include "nes.h"
#include "storage/fds.h"
#include "storage/fds_hle.h"
#include "romdb.h"
#include "apu2a03.h"
#include "apu2a03_state.h"
#include "rp2c02_state.h"
#include "mos6502_state.h"
#include "cart/nes_cart_state.h"
#include "storage/fds_state.h"
#include "gemu/dev/device.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/screen.h"
#include "gemu/dev/stream.h"
#include "gemu/dev/space.h"
#include "gemu/dev/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#ifdef _WIN32
#  define strncasecmp _strnicmp
#else
#  include <strings.h>
#endif

#define VS_DEBUG(s, ...) do { \
    if ((s)->cfg->is_arcade && nes_vs_debug_enabled()) { \
        fprintf(stderr, "vs: " __VA_ARGS__); \
    } \
} while (0)

static bool nes_vs_debug_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GEMU_VS_DEBUG");
        enabled = env && env[0] && strcmp(env, "0") != 0;
    }
    return enabled != 0;
}

static uint8_t nes_pal3bit(uint8_t v) {
    v &= 7u;
    return (uint8_t)((v << 5) | (v << 2) | (v >> 1));
}

/* VS wiring: bit 2 is the cabinet Select/Start button, not NES Start (bit 3). */
#define VS_BTN_START NES_BTN_SELECT

/* ── VS. SMB DIP switch table ────────────────────────────────────────────── */

typedef struct { const char *value; const char *desc; uint8_t bits; } VsDipOption;
typedef struct {
    const char       *name, *desc;
    uint8_t           mask;
    const VsDipOption *options;
    size_t             n_options;
} VsDip;

static const VsDipOption vs_coinage_opts[] = {
    {"1c-1c",     "1 Coin / 1 Credit",  0x00},
    {"3c-1c",     "3 Coins / 1 Credit", 0x02},
    {"2c-1c",     "2 Coins / 1 Credit", 0x04},
    {"1c-2c",     "1 Coin / 2 Credits", 0x06},
    {"1c-3c",     "1 Coin / 3 Credits", 0x01},
    {"1c-4c",     "1 Coin / 4 Credits", 0x05},
    {"1c-5c",     "1 Coin / 5 Credits", 0x03},
    {"free-play", "Free Play",          0x07},
};
static const VsDipOption vs_lives_opts[] = {
    {"3", "3 lives", 0x00},
    {"2", "2 lives", 0x08},
};
static const VsDipOption vs_bonus_opts[] = {
    {"200", "200 coins",  0x10},
    {"150", "150 coins",  0x20},
    {"100", "100 coins",  0x00},
    {"250", "250 coins",  0x30},
};
static const VsDipOption vs_speed_opts[] = {
    {"slow", "Slow", 0x00},
    {"fast", "Fast", 0x40},
};
static const VsDipOption vs_continue_opts[] = {
    {"4", "4 lives on continue", 0x00},
    {"3", "3 lives on continue", 0x80},
};

static const VsDip vs_dips[] = {
    {"coinage",  "Coinage",        0x07, vs_coinage_opts,  8},
    {"lives",    "Lives",          0x08, vs_lives_opts,    2},
    {"bonus",    "Bonus coins",    0x30, vs_bonus_opts,    4},
    {"timer",    "Timer speed",    0x40, vs_speed_opts,    2},
    {"continue", "Continue lives", 0x80, vs_continue_opts, 2},
};
/* FCEUX initializes VS. SMB with all DIP switches off. */

static const VsDip *vs_find_dip(const char *name) {
    for (size_t i = 0; i < sizeof(vs_dips)/sizeof(vs_dips[0]); i++)
        if (strcasecmp(name, vs_dips[i].name) == 0) return &vs_dips[i];
    return NULL;
}
static const VsDipOption *vs_current_opt(const NesState *s, const VsDip *dip) {
    uint8_t bits = s->vs_dip & dip->mask;
    for (size_t i = 0; i < dip->n_options; i++)
        if (dip->options[i].bits == bits) return &dip->options[i];
    return NULL;
}
static void vs_list_dips(const NesState *s) {
    gemu_monitor_printf(s->monitor, "VS. SMB DIP switches (raw=0x%02X):\n", s->vs_dip);
    for (size_t i = 0; i < sizeof(vs_dips)/sizeof(vs_dips[0]); i++) {
        const VsDip *dip = &vs_dips[i];
        const VsDipOption *cur = vs_current_opt(s, dip);
        gemu_monitor_printf(s->monitor, "  %-10s  %-16s  current: %s\n",
               dip->name, dip->desc, cur ? cur->value : "?");
        gemu_monitor_printf(s->monitor, "             values:");
        for (size_t j = 0; j < dip->n_options; j++) gemu_monitor_printf(s->monitor, " %s", dip->options[j].value);
        gemu_monitor_printf(s->monitor, "\n");
    }
}
static bool vs_set_dip(NesState *s, const char *name, const char *value) {
    const VsDip *dip = vs_find_dip(name);
    if (!dip) { gemu_monitor_printf(s->monitor, "unknown DIP '%s' (try 'dipswitch list')\n", name); return true; }
    uint8_t bits = 0; bool found = false;
    for (size_t i = 0; i < dip->n_options; i++)
        if (strcasecmp(value, dip->options[i].value) == 0)
            { bits = dip->options[i].bits; found = true; break; }
    if (!found) {
        char *end; unsigned long v = strtoul(value, &end, 0);
        if (end && *end == '\0') { bits = (uint8_t)v & dip->mask; found = true; }
    }
    if (!found) { gemu_monitor_printf(s->monitor, "invalid value '%s' for DIP '%s'\n", value, name); return true; }
    s->vs_dip = (uint8_t)((s->vs_dip & ~dip->mask) | (bits & dip->mask));
    const VsDipOption *cur = vs_current_opt(s, dip);
    gemu_monitor_printf(s->monitor, "DIP %s = %s\n", dip->name, cur ? cur->value : value);
    return true;
}
static bool vs_dip_cmd(NesState *s, const char *args) {
    while (*args == ' ' || *args == '\t') args++;
    char buf[256]; snprintf(buf, sizeof(buf), "%s", args);
    char *name = strtok(buf, " \t"), *val = strtok(NULL, " \t");
    if (!name || strcasecmp(name, "list") == 0) { vs_list_dips(s); return true; }
    if (!val) { gemu_monitor_printf(s->monitor, "usage: dipswitch list | dipswitch <name> <value>\n"); return true; }
    return vs_set_dip(s, name, val);
}

static bool vs_coin_cmd(NesState *s, const char *args) {
    while (*args == ' ' || *args == '\t') args++;
    int slot = (*args == '2') ? 1 : 0;
    s->vs_coin_latch[slot] = 15;
    gemu_monitor_printf(s->monitor, "VS coin %d\n", slot + 1);
    return true;
}

static bool vs_service_cmd(NesState *s) {
    s->vs_service = true;
    gemu_monitor_printf(s->monitor, "VS service\n");
    return true;
}


/* ── The cartridge slot ────────────────────────────────────────────────── */

/* The cartridge's /IRQ changed (the FDS sets cart_irq itself). */
static void nes_cart_irq_changed(void *ud) {
    NesState *s = ud;
    s->cart_irq = s->cart.irq;
    nes_update_irq(s);
}

static void nes_cart_wire(NesState *s) {
    s->cart.ppu = &s->ppu;
    s->cart.irq_changed = nes_cart_irq_changed;
    s->cart.ud = s;
}

/* Load a cartridge into the (empty) slot. */
static bool nes_cart_insert(NesState *s, const char *path) {
    if (!nes_cart_load(&s->cart, path)) return false;
    snprintf(s->cart_path_buf, sizeof(s->cart_path_buf), "%s", path);
    return true;
}

/* ── CPU memory map ──────────────────────────────────────────────────────── */


static uint8_t nes_cpu_read_bus(uint16_t addr, void *ud);
static void    nes_cpu_write_bus(uint16_t addr, uint8_t val, void *ud);
static GemuSpace *nes_space_cpu(const NesState *s);
static inline uint8_t *nes_page(NesState *s, uint16_t addr, bool write);
static void nes_maps_sync(NesState *s);
static GemuSpace *nes_space_ppu(const NesState *s);
static void nes_lines_irq(NesState *s, bool apu, bool cart);
static void nes_lines_nmi(NesState *s);
static void nes_lines_resync(NesState *s);
static GemuMachine *nes_machine(const NesState *s);
static void nes_sound_drain(NesState *s);
static void nes_exec_clock(NesState *s, uint32_t hz);

/* ── Cartridge connector (Debug > Cartridge Pins, nes_pins.h) ─────────────── */

void nes_update_irq(NesState *s) {
    bool apu = s->apu.fc_irq || s->apu.dmc.irq_flag;
    bool cart = s->cart_irq && !(nes_pins_sample(&s->pins) & NES_PIN_BIT(NP_IRQ));
    nes_lines_irq(s, apu, cart);     /* wired-OR into the CPU's /IRQ */
    if (apu) s->pins.tp_apu_irq = true;
    if (s->cart_irq) s->pins.tp_map_irq = true;   /* the test point is on the cartridge side */
}

static uint64_t nes_ppu_cart_open(void *ud) {
    return nes_pins_sample(&((NesState *)ud)->pins);
}

/* Bring the PPU hook, the sound loop and the clock in line with the pins. */
static void nes_pins_apply(NesState *s) {
    NesPins *p = &s->pins;
    if (s->pins_seen != p->version) {
        s->pins_seen = p->version;
        bool any = nes_pins_any(p) && !s->cfg->is_arcade;
        s->ppu.cart_open = any ? nes_ppu_cart_open : NULL;
        s->ppu.cart_open_ud = s;
        uint64_t open = nes_pins_open_mask(p);
        s->apu.no_sound_in  = any && (open & NES_PIN_BIT(NP_SOUND_IN)) != 0;
        s->apu.no_sound_out = any && ((open & NES_PIN_BIT(NP_SOUND_OUT)) || nes_pins_dead(open));
        nes_update_irq(s);
    }
    if (s->clock_seen != p->clock_hz) {
        s->clock_seen = p->clock_hz;
        nes_exec_clock(s, p->clock_hz);
        /* The APU resamples from the CPU clock: a slower clock makes more
         * output samples per cycle, so pacing to the audio slows everything
         * (and lowers the pitch) as on a real underclocked console. */
        s->apu.clock_pps = (double)p->clock_hz / (double)APU2A03_SAMPLE_RATE;
    }
}

/* CPU access to cartridge space ($4020-$FFFF) through faulty contacts. */
static uint8_t nes_cart_read_faulty(NesState *s, uint16_t addr) {
    uint64_t open = nes_pins_sample(&s->pins);
    if (!open) return nes_cpu_read_bus(addr, s);
    uint8_t bus = s->cpu_bus;
    if (nes_pins_dead(open)) return bus;
    /* /ROMSEL (A15 and M2, made in the console) selects $8000-$FFFF; the
     * cartridge decodes $4020-$7FFF from A13, A14 and its M2 input. */
    if (open & NES_PIN_BIT(addr >= 0x8000u ? NP_ROMSEL : NP_M2)) return bus;
    uint16_t a = (uint16_t)((addr & 0x8000u) | ((addr | nes_pins_cpu_addr_open(open)) & 0x7FFFu));
    uint8_t d = nes_cpu_read_bus(a, s);
    uint8_t dopen = nes_pins_cpu_data_open(open);
    return (uint8_t)((d & ~dopen) | (bus & dopen));
}

static void nes_cart_write_faulty(NesState *s, uint16_t addr, uint8_t val) {
    uint64_t open = nes_pins_sample(&s->pins);
    if (!open) { nes_cpu_write_bus(addr, val, s); return; }
    if (nes_pins_dead(open) || (open & NES_PIN_BIT(NP_CPU_RW))) return;
    if (open & NES_PIN_BIT(addr >= 0x8000u ? NP_ROMSEL : NP_M2)) return;
    uint16_t a = (uint16_t)((addr & 0x8000u) | ((addr | nes_pins_cpu_addr_open(open)) & 0x7FFFu));
    nes_cpu_write_bus(a, (uint8_t)(val | nes_pins_cpu_data_open(open)), s);
}

static bool nes_cart_faulty(const NesState *s, uint16_t addr) {
    return addr >= 0x4020u && nes_pins_any(&s->pins) && !s->cfg->is_arcade;
}

static void nes_sync_ppu_to_cpu(NesState *s, uint64_t cpu_cycle);

/* The DMC asking for a byte, or its channel disabled while it waited. */
static void nes_dmc_dma_check(NesState *s) {
    if (s->apu.dmc.dma_pending && !s->dmc_dma) {
        s->dmc_dma = s->dmc_dummy = s->dma_halt = true;
    }
    if (s->apu.dmc.dma_abort) {
        s->apu.dmc.dma_abort = false;
        if (s->dmc_dma) {
            if (s->dma_halt && !s->in_dma) s->dmc_dma = s->dmc_dummy = s->dma_halt = false;
            else s->dmc_abort = true;   /* already under way: dropped at the next cycle */
        }
    }
}

/* The APU, the cartridge's expansion audio and its mapper's cycle counters,
 * one tick per CPU cycle up to `cpu_cycle`.  The cartridge's chips count
 * M2, so with that contact open (or no power) they stop. */
/* Whether the cartridge side must tick with every CPU cycle: chips that
 * count M2 (VRC IRQs and audio, FME-7 and friends, the FDS), or connector
 * faults that can stop M2.  Otherwise the APU alone runs in batches. */
static bool nes_apu_eager(const NesState *s) {
    return nes_cart_ticks(&s->cart) || s->fds_enabled ||
           (nes_pins_any(&s->pins) && !s->cfg->is_arcade);
}

static void nes_sync_apu_to_cpu(NesState *s, uint64_t cpu_cycle) {
    if (s->apu_synced_cpu_cycle >= cpu_cycle || s->apu_syncing) return;
    s->apu_syncing = true;   /* the DMC fetches through nes_cpu_read */
    if (!nes_apu_eager(s)) {
        while (s->apu_synced_cpu_cycle < cpu_cycle) {
            uint64_t to = cpu_cycle - s->apu_synced_cpu_cycle > 256u ? s->apu_synced_cpu_cycle + 256u
                                                                     : cpu_cycle;
            s->apu.cycle = s->apu_synced_cpu_cycle;
            apu2a03_run(&s->apu, to);
            s->apu_synced_cpu_cycle = to;
            /* a slow clock makes a frame's audio outgrow the buffer */
            if (s->apu.frame_n > 900) nes_sound_drain(s);
        }
        nes_dmc_dma_check(s);
        s->apu_deadline = s->apu_synced_cpu_cycle + apu2a03_until_event(&s->apu);
        s->apu_syncing = false;
        nes_update_irq(s);
        return;
    }
    bool cart_ticks = nes_cart_ticks(&s->cart);
    bool m2 = true;
    if (nes_pins_any(&s->pins) && !s->cfg->is_arcade) {
        uint64_t open = nes_pins_sample(&s->pins);
        m2 = !(open & NES_PIN_BIT(NP_M2)) && !nes_pins_dead(open);
    }
    while (s->apu_synced_cpu_cycle < cpu_cycle) {
        s->apu_synced_cpu_cycle++;
        if (!m2)
            s->apu.fds_in = 0.0f;
        else if (cart_ticks)   /* the board's counters, and its audio */
            s->apu.fds_in = nes_cart_tick(&s->cart);
        else if (s->apu.audio_dev && s->fds_enabled)
            s->apu.fds_in = fds_audio_tick(&s->fds);
        s->apu.cycle = s->apu_synced_cpu_cycle - 1u;   /* tick numbers the cycle */
        apu2a03_tick(&s->apu);
        nes_dmc_dma_check(s);
    }
    if (s->apu.frame_n > 900) nes_sound_drain(s);
    s->apu_syncing = false;
    nes_update_irq(s);
}

/* Before each bus access the rest of the console catches up with the CPU,
 * so registers, IRQ and NMI are exact to the cycle: the PPU through this
 * cycle (as before), the APU side up to its start. */
static void nes_sync_all(NesState *s) {
    uint64_t c = s->cpu.cycle_count - 1;
    if (c >= s->apu_deadline || nes_apu_eager(s)) nes_sync_apu_to_cpu(s, c);
    nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
}

/* ── DMA ─────────────────────────────────────────────────────────────────────
 * A DMA halts the CPU (RDY) on its next read cycle - a write can't be
 * halted - and the halted CPU keeps repeating that read.  The DMC's fetch
 * needs a halt and a dummy cycle, then a get (even) cycle; OAM DMA reads on
 * get cycles and writes $2004 on put cycles, 256 times.  Running together,
 * any DMA cycle counts as the DMC's halt or dummy cycle.
 *
 * The 2A03 reads its own registers ($4015-$4017, mirrored every $20) only
 * while the 6502's address bus is in $4000-$401F: a DMA read there sees
 * open bus unless the CPU was halted on such an address, and then reads the
 * registers too, whatever the DMA's own address (bus conflicts). */

static uint8_t dma_bus_read(NesState *s, uint16_t a) {
    uint8_t v = nes_cart_faulty(s, a) ? nes_cart_read_faulty(s, a) : nes_cpu_read_bus(a, s);
    if (a != 0x4015) s->cpu_bus = v;
    return v;
}

static uint8_t dma_read(NesState *s, uint16_t a, bool regs, uint16_t *prev) {
    uint8_t v;
    if (!regs) {
        v = (a & 0xFFE0u) == 0x4000u ? s->cpu_bus : dma_bus_read(s, a);
        *prev = a;
        return v;
    }
    uint16_t reg = (uint16_t)(0x4000u | (a & 0x1Fu));
    if (reg == 0x4015) {
        v = dma_bus_read(s, 0x4015);
        if (a != reg) (void)dma_bus_read(s, a);
    } else if (reg == 0x4016 || reg == 0x4017) {
        /* the controller sees back-to-back reads as one */
        v = *prev == reg ? s->cpu_bus : dma_bus_read(s, reg);
        if (a != reg) {
            uint8_t ext = dma_bus_read(s, a);
            v = (uint8_t)((ext & 0xE0u) | (v & ext & 0x1Fu));
        }
    } else {
        v = dma_bus_read(s, a);
    }
    *prev = reg;
    return v;
}

static void dma_cycle(NesState *s) {
    s->cpu.cycle_count++;
    nes_sync_all(s);
}

static void nes_run_dma(NesState *s, uint16_t cpu_addr) {
    s->in_dma = true;
    s->cpu.cycle_count--;          /* the DMA's cycles come before this read */
    bool regs = (cpu_addr & 0xFFE0u) == 0x4000u;
    bool skip_dummy = cpu_addr == 0x4016 || cpu_addr == 0x4017;
    uint16_t prev = cpu_addr;
    uint8_t oam_val = 0;
    unsigned oam_n = 0;

    /* halt: the CPU's read happens and is repeated later */
    dma_cycle(s);
    if (s->dmc_abort) s->dmc_dma = s->dmc_dummy = s->dmc_abort = false;
    else (void)dma_bus_read(s, cpu_addr);
    s->dma_halt = false;

    while (s->dmc_dma || s->oam_dma) {
        if (s->dmc_abort) s->dmc_dma = s->dmc_dummy = s->dmc_abort = s->dma_halt = false;
        bool get = ((s->cpu.cycle_count + 1u) & 1u) == 0;
        bool dmc_ready = s->dmc_dma && !s->dma_halt && !s->dmc_dummy;
        if (s->dma_halt) s->dma_halt = false;
        else if (s->dmc_dummy) s->dmc_dummy = false;
        dma_cycle(s);
        if (get && dmc_ready) {
            uint8_t v = dma_read(s, s->apu.dmc.cur_addr, regs, &prev);
            s->dmc_dma = false;
            apu2a03_dmc_fill(&s->apu, v);
            s->apu_deadline = 0;   /* its state moved on: look again */
        } else if (get && s->oam_dma && !(oam_n & 1u)) {
            oam_val = dma_read(s, (uint16_t)(s->oam_page << 8 | oam_n >> 1), regs, &prev);
            oam_n++;
        } else if (!get && s->oam_dma && (oam_n & 1u)) {
            nes_cpu_write_bus(0x2004, oam_val, s);
            s->cpu_bus = oam_val;
            if (++oam_n == 512) s->oam_dma = false;
        } else if (!skip_dummy) {
            (void)dma_bus_read(s, cpu_addr);   /* halted: the read repeats */
        }
    }
    s->cpu.cycle_count++;          /* back to the CPU's own read */
    s->cpu.rdy_halted = true;
    s->in_dma = false;
}

uint8_t nes_cpu_read(uint16_t addr, void *ud) {
    NesState *s = ud;
    nes_sync_all(s);
    if (s->dma_halt && !s->in_dma) {
        nes_run_dma(s, addr);
        nes_sync_all(s);
    }
    uint8_t v = nes_cart_faulty(s, addr) ? nes_cart_read_faulty(s, addr) : nes_cpu_read_bus(addr, ud);
    if (addr != 0x4015) s->cpu_bus = v;   /* $4015 is read over the internal bus only */
    return v;
}

void nes_cpu_write(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;
    nes_sync_all(s);
    s->cpu_bus = val;
    if (nes_cart_faulty(s, addr)) nes_cart_write_faulty(s, addr, val);
    else nes_cpu_write_bus(addr, val, ud);
}

static void nes_screen_frame(NesState *s);
static void nes_sync_ppu_to_cpu(NesState *s, uint64_t cpu_cycle) {
    uint64_t frame = s->ppu.frame;
    while (s->ppu_synced_cpu_cycle < cpu_cycle) {
        s->ppu_synced_cpu_cycle++;
        if (s->fds_enabled)
            { s->cart_irq = fds_tick(&s->fds); nes_update_irq(s); }
        for (int i = 0; i < 3; i++) {
            rp2c02_tick(&s->ppu);
            if (s->ppu.nmi_pending) {
                nes_lines_nmi(s);
                s->pins.tp_nmi = true;
                s->ppu.nmi_pending = false;
            }
        }
    }
    if (s->ppu.frame != frame) nes_screen_frame(s);
}


/* ── The CPU's bus (stage N2) ────────────────────────────────────────────────
 *
 * A 16-bit core space, "program" (nes_core_build()):
 *   $0000-$1FFF  work RAM, 2 KB mirrored
 *   $2000-$3FFF  the PPU's registers, mirrored every 8 bytes
 *   $4000-$401F  the 2A03's own: APU, OAM DMA, the controller ports
 *   $4020-$FFFF  the cartridge slot; above it, at higher priority:
 *                  the VS. System's coin counter ($4020-$5FFF)
 *                  the Disk System's registers ($4020-$408F) and its
 *                  RAM and BIOS ($6000-$FFFF)
 *                  the Game Genie ($8000-$FFFF, while codes are on)
 * Each handler gets its offset from its mapping's base. */

/* Plain memory (work RAM, a board's PRG ROM windows, the Disk System's
 * RAM...) is read and written straight from host memory: each 256-byte page
 * the space serves from one RAM mapping is cached (nes_page()), until the
 * space's map changes.  Everything else is a space access. */
static uint8_t nes_cpu_read_bus(uint16_t addr, void *ud) {
    NesState *s = ud;
    gemu_monitor_check_read(s->monitor, addr);
    uint8_t *p = nes_page(s, addr, false);
    return p ? p[addr & 0xFFu] : gemu_space_read8(nes_space_cpu(s), addr);
}

static void nes_cpu_write_bus(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;
    gemu_monitor_check_write(s->monitor, addr);
    uint8_t *p = nes_page(s, addr, true);
    if (p) p[addr & 0xFFu] = val;
    else gemu_space_write8(nes_space_cpu(s), addr, val);
}

/* $2000-$2007 */
static uint8_t bus_ppu_r(void *ud, uint64_t off) {
    NesState *s = ud;
    nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
    return rp2c02_read(&s->ppu, (uint8_t)(off & 7));
}

static void bus_ppu_w(void *ud, uint64_t off, uint8_t val) {
    NesState *s = ud;
    if (s->cfg->is_arcade) {
        uint8_t r = (uint8_t)(off & 7);
        if (r == 0 || r == 1 || r == 5 || r == 6)
            VS_DEBUG(s, "write 200%u <- %02X pc=%04X frame=%llu sl=%d dot=%d\n",
                     r, val, s->cpu.PC,
                     (unsigned long long)s->ppu.frame,
                     s->ppu.scanline, s->ppu.dot);
    }
    nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
    rp2c02_write(&s->ppu, (uint8_t)(off & 7), val);
}

/* $4000-$401F */
static uint8_t bus_io_r(void *ud, uint64_t off) {
    NesState *s = ud;
    uint16_t addr = (uint16_t)(0x4000u + off);
    if (addr == 0x4015) {
        nes_sync_apu_to_cpu(s, s->cpu.cycle_count - 1);
        s->apu_deadline = 0;
        /* bit 5 isn't driven: it reads the CPU's internal bus (and, unlike
         * every other read, this one doesn't reach the external bus) */
        uint8_t v = (uint8_t)((apu2a03_read(&s->apu, 0x4015) & ~0x20u) | (s->cpu_bus & 0x20u));
        nes_update_irq(s);
        return v;
    }

    /* VS. System: DIP switches and coin signals layered onto $4016/$4017.
     * Hardware wiring: left stick → $4017, right stick → $4016.
     * VS. SMB P1 uses left stick ($4017) and P2 uses right stick ($4016),
     * so ctrl_state[0] (P1 keys) feeds $4017 and ctrl_state[1] (P2 keys) feeds $4016.
     * $4016 D0 = P2 shift reg | D2 = coin1 | D3 = coin2 | D4-D5 = DIP[0:1] | D6 = service
     * $4017 D0 = P1 shift reg | D2-D7 = DIP[2:7] */
    if (s->cfg->is_arcade && addr == 0x4016) {
        uint8_t bit;
        if (s->ctrl_strobe)
            s->ctrl_shift[1] = s->ctrl_state[1];
        bit = s->ctrl_shift[1] & 1;
        s->ctrl_shift[1] >>= 1;
        uint8_t v = bit
             | (s->vs_coin_latch[0] > 0 ? 0x04u : 0u)         /* D2: coin 1 */
             | (s->vs_coin_latch[1] > 0 ? 0x08u : 0u)         /* D3: coin 2 */
             | (uint8_t)((s->vs_dip & 0x03u) << 4)             /* D4-D5: DIP bits 0-1 */
             | (s->vs_service           ? 0x40u : 0u);         /* D6: service */
        if (s->vs_service || s->vs_coin_latch[0] > 0 || s->vs_coin_latch[1] > 0)
            VS_DEBUG(s, "read 4016 -> %02X pc=%04X strobe=%d dip=%02X coin=%d/%d\n",
                     v, s->cpu.PC, s->ctrl_strobe ? 1 : 0, s->vs_dip,
                     s->vs_coin_latch[0], s->vs_coin_latch[1]);
        return v;
    }
    if (s->cfg->is_arcade && addr == 0x4017) {
        uint8_t bit;
        if (s->ctrl_strobe)
            s->ctrl_shift[0] = s->ctrl_state[0];
        bit = s->ctrl_shift[0] & 1;
        s->ctrl_shift[0] >>= 1;
        uint8_t v = bit
             | (uint8_t)(s->vs_dip & ~0x03u);                 /* D2-D7: DIP bits 2-7 */
        return v;
    }

    /* The controller ports drive D0-D4 (whatever is plugged in uses D0, the
     * Famicom's microphone D2, the Zapper D3-D4); D5-D7 are open bus. */
    uint8_t ctrl_open = s->cpu_bus & 0xE0u;
    if (addr == 0x4016) {
        if (s->cfg->ports[0] != NES_DEVICE_CONTROLLER) return ctrl_open;
        if (s->ctrl_strobe) return ctrl_open | ((s->ctrl_state[0] & NES_BTN_A) ? 1u : 0u);
        uint8_t bit = s->ctrl_shift[0] & 1;
        s->ctrl_shift[0] = (s->ctrl_shift[0] >> 1) | 0x80u;
        /* D2: Famicom hardwired controller 2 microphone */
        uint8_t mic = (s->cfg->ports[1] == NES_DEVICE_FC2_MIC && s->mic_noise) ? 0x04u : 0u;
        return ctrl_open | bit | mic;
    }
    if (addr == 0x4017) {
        if (s->cfg->ports[1] == NES_DEVICE_ZAPPER) {
            /* Bit 4: trigger (1 = half-pulled); bit 3: light sense (0 = detected) */
            uint8_t trigger = (s->zapper_trigger_ttl > 0) ? 0x10u : 0u;
            uint8_t light   = 0x08u; /* default: not detected */
            int sl = s->ppu.scanline;
            if (sl >= 0 && sl < 240 &&
                s->zapper_x >= 0 && s->zapper_x < 256 &&
                s->zapper_y >= 0 && s->zapper_y < 240 &&
                abs(sl - s->zapper_y) <= 8) {
                uint32_t argb = s->ppu.pixels_argb[s->zapper_y * RP2C02_WIDTH + s->zapper_x];
                uint8_t r = (uint8_t)(argb >> 16);
                uint8_t g = (uint8_t)(argb >>  8);
                uint8_t b = (uint8_t)(argb);
                uint8_t luma = (uint8_t)((r * 77u + g * 150u + b * 29u) >> 8);
                if (luma >= 85) light = 0; /* bright pixel → light detected */
            }
            return ctrl_open | trigger | light;
        }
        if (s->cfg->ports[1] != NES_DEVICE_CONTROLLER &&
            s->cfg->ports[1] != NES_DEVICE_FC2_MIC &&
            !nes_device_is_rob(s->cfg->ports[1]) && !s->port2_controller) return ctrl_open;
        if (s->ctrl_strobe) return ctrl_open | ((s->ctrl_state[1] & NES_BTN_A) ? 1u : 0u);
        uint8_t bit = s->ctrl_shift[1] & 1;
        s->ctrl_shift[1] = (s->ctrl_shift[1] >> 1) | 0x80u;
        return ctrl_open | bit;
    }
    return s->cpu_bus;            /* the APU's write-only registers, $4018-$401F */
}

static void bus_io_w(void *ud, uint64_t off, uint8_t val) {
    NesState *s = ud;
    uint16_t addr = (uint16_t)(0x4000u + off);
    if (addr == 0x4014) {
        VS_DEBUG(s, "write 4014 <- %02X pc=%04X dma\n", val, s->cpu.PC);
        /* OAM DMA: starts on the CPU's next read (nes_run_dma) */
        s->oam_page = val;
        s->oam_dma = s->dma_halt = true;
        return;
    }

    if (addr == 0x4016) {
        bool new_strobe = (val & 1) != 0;
        if (s->cfg->is_arcade &&
            (new_strobe != s->ctrl_strobe ||
             ((val & 0x04u) ? 0x2000u : 0u) != s->cart.chr_off[0]))
            VS_DEBUG(s, "write 4016 <- %02X pc=%04X strobe %d->%d chr=%05X\n",
                     val, s->cpu.PC, s->ctrl_strobe ? 1 : 0,
                     new_strobe ? 1 : 0, (val & 0x04u) ? 0x2000u : 0u);
        if (s->cfg->is_arcade) {
            if (new_strobe) {
                s->ctrl_shift[0] = s->ctrl_state[0];
                s->ctrl_shift[1] = s->ctrl_state[1];
            }
        } else if (s->ctrl_strobe && !new_strobe) {
            s->ctrl_shift[0] = s->ctrl_state[0];
            s->ctrl_shift[1] = s->ctrl_state[1];
        }
        s->ctrl_strobe = new_strobe;
        nes_cart_out(&s->cart, val);   /* OUT0-2: the VS. System's CHR bank */
        nes_maps_sync(s);
        return;
    }

    /* APU registers */
    if (addr <= 0x4013 || addr == 0x4015 || addr == 0x4017) {
        nes_sync_apu_to_cpu(s, s->cpu.cycle_count - 1);
        apu2a03_write(&s->apu, addr, val);
        nes_dmc_dma_check(s);
        s->apu_deadline = 0;   /* the write may bring the next event closer */
        /* $4010/$4017 can clear APU IRQ flags */
        nes_update_irq(s);
    }
}

/* $4020-$FFFF: the cartridge slot */
static uint8_t cart_read(NesState *s, uint16_t addr) {
    if (addr < 0x8000) return nes_cart_read(&s->cart, addr, s->cpu_bus);
    if (!s->cart.prg) return s->cpu_bus;
    return nes_cart_prg(&s->cart, addr);
}
static uint8_t bus_cart_r(void *ud, uint64_t off) {
    return cart_read(ud, (uint16_t)(0x4020u + off));
}
static void bus_cart_w(void *ud, uint64_t off, uint8_t val) {
    NesState *s = ud;
    nes_cart_write(&s->cart, (uint16_t)(0x4020u + off), val);
    nes_maps_sync(s);                   /* a bank switch moves the windows */
}
static bool bus_cart_peek(void *ud, uint64_t off, uint8_t *v) {
    NesState *s = ud;
    uint16_t addr = (uint16_t)(0x4020u + off);
    if (addr < 0x6000 || !s->cart.prg) return false;   /* board registers */
    if (addr < 0x8000) { *v = s->cart.prg_ram[addr - 0x6000u]; return true; }
    *v = nes_cart_prg(&s->cart, addr);
    return true;
}

/* $8000-$FFFF over the slot, while Game Genie codes are on (nes_gg_changed()) */
static uint8_t bus_gg_r(void *ud, uint64_t off) {
    NesState *s = ud;
    uint16_t addr = (uint16_t)(0x8000u + off);
    uint8_t rom = cart_read(s, addr);
    for (int i = 0; i < s->gg_count; i++) {
        if (s->gg_patches[i].addr == addr &&
            (!s->gg_patches[i].has_cmp || rom == s->gg_patches[i].cmp))
            return s->gg_patches[i].val;
    }
    return rom;
}
static void bus_gg_w(void *ud, uint64_t off, uint8_t val) {
    NesState *s = ud;
    nes_cart_write(&s->cart, (uint16_t)(0x8000u + off), val);
    nes_maps_sync(s);
}

/* $4020-$5FFF on the VS. System: the coin counter */
static uint8_t bus_vs_r(void *ud, uint64_t off) {
    NesState *s = ud;
    VS_DEBUG(s, "read %04X -> %02X pc=%04X coin_counter\n",
             (unsigned)(0x4020u + off), s->vs_coin_counter, s->cpu.PC);
    return s->vs_coin_counter;
}
static void bus_vs_w(void *ud, uint64_t off, uint8_t val) {
    NesState *s = ud;
    s->vs_coin_counter = val;
    VS_DEBUG(s, "write %04X <- %02X pc=%04X coin_counter\n",
             (unsigned)(0x4020u + off), val, s->cpu.PC);
}

/* $4020-$408F with the Disk System: its registers (writes to $408B up reach
 * the slot) */
static uint8_t bus_fds_reg_r(void *ud, uint64_t off) {
    NesState *s = ud;
    uint8_t v = fds_reg_read(&s->fds, (uint16_t)(0x4020u + off));
    s->cart_irq = fds_tick(&s->fds);  /* recalculate IRQ after flag clear */
    nes_update_irq(s);
    return v;
}
static void bus_fds_reg_w(void *ud, uint64_t off, uint8_t val) {
    NesState *s = ud;
    uint16_t addr = (uint16_t)(0x4020u + off);
    if (addr > 0x408Au) { nes_cart_write(&s->cart, addr, val); nes_maps_sync(s); return; }
    fds_reg_write(&s->fds, addr, val);
    /* $4025 bit 3 controls mirroring (0=vertical, 1=horizontal) */
    if (addr == 0x4025) {
        s->ppu.mirror = (val & 0x08) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
        nes_maps_sync(s);
    }
    s->cart_irq = fds_tick(&s->fds);
    nes_update_irq(s);
}

/* $6000-$FFFF with the Disk System: its RAM, then the BIOS */
static uint8_t bus_fds_mem_r(void *ud, uint64_t off) {
    NesState *s = ud;
    uint16_t addr = (uint16_t)(0x6000u + off);
    if (addr >= 0xE000) return s->fds.bios[addr - 0xE000u];
    return s->fds.ram[addr - 0x6000u];
}
static void bus_fds_mem_w(void *ud, uint64_t off, uint8_t val) {
    NesState *s = ud;
    uint16_t addr = (uint16_t)(0x6000u + off);
    if (addr < 0xE000) s->fds.ram[addr - 0x6000u] = val;
}
static bool bus_fds_mem_peek(void *ud, uint64_t off, uint8_t *v) {
    *v = bus_fds_mem_r(ud, off);
    return true;
}

static const GemuHandlers nes_bus_ppu = { .r8 = bus_ppu_r, .w8 = bus_ppu_w };
static const GemuHandlers nes_bus_io = { .r8 = bus_io_r, .w8 = bus_io_w };
static const GemuHandlers nes_bus_cart = { .r8 = bus_cart_r, .w8 = bus_cart_w,
                                           .peek8 = bus_cart_peek };
static const GemuHandlers nes_bus_gg = { .r8 = bus_gg_r, .w8 = bus_gg_w };
static const GemuHandlers nes_bus_vs = { .r8 = bus_vs_r, .w8 = bus_vs_w };
static const GemuHandlers nes_bus_fds_reg = { .r8 = bus_fds_reg_r, .w8 = bus_fds_reg_w };
static const GemuHandlers nes_bus_fds_mem = { .r8 = bus_fds_mem_r, .w8 = bus_fds_mem_w,
                                              .peek8 = bus_fds_mem_peek };

/* ── The PPU's bus (stage N2) ────────────────────────────────────────────────
 * A 14-bit core space, "ppu": the pattern tables ($0000-$1FFF) through the
 * board's CHR windows, the nametables ($2000-$2FFF, mirrored to $3EFF)
 * through the console's 2 KB CIRAM as the board's mirroring arranges it, or
 * through the board itself when it maps its own (MMC5, four-screen).  The
 * palette is inside the PPU.  The PPU reaches it through its chr and nt
 * hooks (nes_ppu_space_*). */

static uint8_t ppu_chr_r(void *ud, uint64_t off) {
    return nes_cart_chr_read((uint16_t)off, &((NesState *)ud)->cart);
}
static void ppu_chr_w(void *ud, uint64_t off, uint8_t val) {
    nes_cart_chr_write((uint16_t)off, val, &((NesState *)ud)->cart);
}
static bool ppu_chr_peek(void *ud, uint64_t off, uint8_t *v) {
    *v = ppu_chr_r(ud, off);
    return true;
}
static uint8_t ppu_nt_r(void *ud, uint64_t off) {
    NesState *s = ud;
    uint16_t a = (uint16_t)(0x2000u + off);
    if (nes_cart_wants_nt(&s->cart)) return nes_cart_nt_read(a, &s->cart);
    return s->ppu.vram[rp2c02_ciram_index(&s->ppu, a)];
}
static void ppu_nt_w(void *ud, uint64_t off, uint8_t val) {
    NesState *s = ud;
    uint16_t a = (uint16_t)(0x2000u + off);
    if (nes_cart_wants_nt(&s->cart)) nes_cart_nt_write(a, val, &s->cart);
    else s->ppu.vram[rp2c02_ciram_index(&s->ppu, a)] = val;
}
static bool ppu_nt_peek(void *ud, uint64_t off, uint8_t *v) {
    NesState *s = ud;
    if (nes_cart_wants_nt(&s->cart)) return false;   /* MMC5's ExRAM modes have effects */
    *v = ppu_nt_r(ud, off);
    return true;
}
static const GemuHandlers nes_ppu_chr = { .r8 = ppu_chr_r, .w8 = ppu_chr_w,
                                          .peek8 = ppu_chr_peek };
static const GemuHandlers nes_ppu_nt = { .r8 = ppu_nt_r, .w8 = ppu_nt_w, .peek8 = ppu_nt_peek };

/* The PPU's hooks: its bus is the space. */
static inline uint8_t *nes_ppu_page(NesState *s, uint16_t addr, bool write);
static uint8_t nes_ppu_space_r(uint16_t addr, void *ud) {
    uint8_t *p = nes_ppu_page(ud, addr, false);
    return p ? p[addr & 0xFFu] : gemu_space_read8(nes_space_ppu(ud), addr);
}
static void nes_ppu_space_w(uint16_t addr, uint8_t val, void *ud) {
    uint8_t *p = nes_ppu_page(ud, addr, true);
    if (p) p[addr & 0xFFu] = val;
    else gemu_space_write8(nes_space_ppu(ud), addr, val);
}

/* ── Cartridge media device ──────────────────────────────────────────────── */

/* PPU hooks for the cartridge's mapper: scanline IRQ counters and
 * nametable mapping.  Called again when the cartridge changes. */
static void nes_install_mapper_hooks(NesState *s) {
    NesCart *c = &s->cart;
    s->ppu.irq_scanline = nes_cart_wants_scanline(c) ? nes_cart_scanline : NULL;
    s->ppu.irq_ud = c;
    if (s->core) {
        /* the PPU's bus is the "ppu" space (its handlers ask the board) */
        s->ppu.chr_read  = nes_ppu_space_r;
        s->ppu.chr_write = nes_ppu_space_w;
        s->ppu.nt_read   = nes_ppu_space_r;
        s->ppu.nt_write  = nes_ppu_space_w;
        s->ppu.chr_ud = s->ppu.nt_ud = s;
    } else {
        s->ppu.nt_read  = nes_cart_wants_nt(c) ? nes_cart_nt_read : NULL;
        s->ppu.nt_write = nes_cart_wants_nt(c) ? nes_cart_nt_write : NULL;
        s->ppu.nt_ud = c;
    }
}


/* ── Machine lifecycle ───────────────────────────────────────────────────── */

void nes_reset(NesState *s) {
    rp2c02_reset(&s->ppu);
    if (s->fds_enabled) {
        /* FDS: horizontal mirroring until $4025 says otherwise */
        s->ppu.mirror = RP2C02_MIRROR_HORIZONTAL;
        s->ppu_synced_cpu_cycle = s->apu_synced_cpu_cycle = s->cpu.cycle_count;
        s->apu_deadline = 0;
        mos6502_reset(&s->cpu);
        apu2a03_reset(&s->apu);
        nes_maps_sync(s);
        return;
    }
    s->ppu.mirror = s->cart.info.mirror;
    s->ppu_synced_cpu_cycle = s->apu_synced_cpu_cycle = s->cpu.cycle_count;
    s->apu_deadline = 0;

    /* The board's banks before mos6502_reset reads the reset vector */
    nes_cart_reset(&s->cart);
    nes_maps_sync(s);
    s->cart_irq = s->cart.irq;
    nes_update_irq(s);

    mos6502_reset(&s->cpu);
    VS_DEBUG(s, "reset pc=%04X nmi=%04X reset=%04X irq=%04X prgfffc=%02X%02X dip=%02X\n",
             s->cpu.PC,
             (uint16_t)(s->cart.prg[0x7FFAu] | ((uint16_t)s->cart.prg[0x7FFBu] << 8)),
             (uint16_t)(s->cart.prg[0x7FFCu] | ((uint16_t)s->cart.prg[0x7FFDu] << 8)),
             (uint16_t)(s->cart.prg[0x7FFEu] | ((uint16_t)s->cart.prg[0x7FFFu] << 8)),
             s->cart.prg[0x7FFDu], s->cart.prg[0x7FFCu], s->vs_dip);
    apu2a03_reset(&s->apu);
}

/* ── Arcade ROM chip assembler ───────────────────────────────────────────── */
/* Builds a NROM-256 cartridge in memory from individual chip files:
 *   region "prg"  = PRG chips at MAME region offsets 0x0000-0x7FFF
 *   region "gfx1" = CHR chips at MAME region offsets 0x0000-0x3FFF
 *   region "ppu1:palette" = 2C04 palette LUT.
 * Legacy manual loads without a region still accept PRG CPU addresses
 * 0x8000-0xFFFF, CHR at 0x10000-0x1FFFF, and metadata >= 0x20000. */
static bool nes_arcade_assemble(NesState *s, const MosConfig *cfg) {
    uint8_t *prg = calloc(1, 32768);
    uint8_t *chr = calloc(1, 16384); /* 2 × 8 KB VROM banks */
    if (!prg || !chr) { free(prg); free(chr); return false; }

    bool has_prg = false, has_chr = false;

    /* Load PRG chips directly at their declared CPU offset.  VS. sockets are
     * 8 KB; keeping that boundary exact catches missing/swapped chips early. */
    for (int i = 0; i < cfg->n_roms; i++) {
        uint32_t a = cfg->roms[i].addr;
        bool is_prg = cfg->roms[i].region
                   ? strcmp(cfg->roms[i].region, "prg") == 0
                   : (a >= 0x8000 && a < 0x10000);
        if (!is_prg) continue;
        size_t off = cfg->roms[i].region ? a : a - 0x8000u;
        if (off > 32768u - 0x2000u) {
            fprintf(stderr, "arcade: PRG chip '%s' offset 0x%zX is out of range\n",
                    cfg->roms[i].path, off);
            free(prg); free(chr); return false;
        }
        FILE *f = fopen(cfg->roms[i].path, "rb");
        if (!f) continue;
        size_t n = fread(prg + off, 1, 0x2000u, f);
        fclose(f);
        if (n != 0x2000u) {
            fprintf(stderr, "arcade: PRG chip '%s' is %zu bytes, expected 8192\n",
                    cfg->roms[i].path, n);
            free(prg); free(chr); return false;
        }
        has_prg = true;
        VS_DEBUG(s, "loaded PRG %s -> prg:%04zX\n", cfg->roms[i].path, off);
    }

    /* Collect CHR chips, sort by addr, concatenate into the two 8KB VROM banks. */
    struct ChrChip { uint32_t addr; const char *path; } cc[16];
    int ncc = 0;
    for (int i = 0; i < cfg->n_roms && ncc < 16; i++) {
        uint32_t a = cfg->roms[i].addr;
        bool is_chr = cfg->roms[i].region
                   ? strcmp(cfg->roms[i].region, "gfx1") == 0
                   : (a >= 0x10000 && a < 0x20000);
        if (is_chr) {
            cc[ncc].addr = cfg->roms[i].region ? a : a - 0x10000u;
            cc[ncc].path = cfg->roms[i].path;
            ncc++;
        }
    }
    /* insertion sort by addr (tiny n) */
    for (int i = 1; i < ncc; i++) {
        struct ChrChip t = cc[i]; int j = i - 1;
        for (; j >= 0 && cc[j].addr > t.addr; j--) cc[j+1] = cc[j];
        cc[j+1] = t;
    }
    size_t chr_fill = 0;
    for (int i = 0; i < ncc && chr_fill < 16384; i++) {
        FILE *f = fopen(cc[i].path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
        size_t to = (size_t)sz < 16384 - chr_fill ? (size_t)sz : 16384 - chr_fill;
        chr_fill += fread(chr + chr_fill, 1, to, f);
        fclose(f);
        if (to != 0x2000u) {
            fprintf(stderr, "arcade: CHR chip '%s' is %zu bytes, expected 8192\n",
                    cc[i].path, to);
            free(prg); free(chr); return false;
        }
        has_chr = true;
        VS_DEBUG(s, "loaded CHR %s -> gfx1:%04zX\n", cc[i].path, chr_fill - to);
    }

    if (!has_prg) {
        fprintf(stderr, "arcade: no PRG ROM chips found in ROM set\n");
        romdb_print_needed("vssmb");
        free(prg); free(chr); return false;
    }

    if (!has_chr) { free(chr); chr = NULL; }
    if (!nes_cart_init_vs(&s->cart, prg, chr)) { free(prg); free(chr); return false; }

    /* Load 2C04 palette file if present.  MAME .pal files are 64 palette
     * indexes stored as RGB-looking triples; each first byte selects one of
     * the normal NES RGB entries. */
    for (int i = 0; i < cfg->n_roms; i++) {
        bool is_palette = cfg->roms[i].region
                       ? strcmp(cfg->roms[i].region, "ppu1:palette") == 0
                       : cfg->roms[i].addr >= 0x20000;
        if (!is_palette) continue;
        FILE *pf = fopen(cfg->roms[i].path, "rb");
        if (!pf) continue;
        uint8_t rgb[192];
        size_t n = fread(rgb, 1, sizeof(rgb), pf);
        fclose(pf);
        if (n == 192) {
            for (int j = 0; j < 64; j++)
                s->vs_palette_rgb[j] = 0xFF000000u
                    | ((uint32_t)nes_pal3bit(rgb[j * 3 + 0]) << 16)
                    | ((uint32_t)nes_pal3bit(rgb[j * 3 + 1]) <<  8)
                    |  (uint32_t)nes_pal3bit(rgb[j * 3 + 2]);
            /* ppu not initialised yet; pointer set after rp2c02_init in nes_create */
            VS_DEBUG(s, "loaded 2C04 palette %s\n", cfg->roms[i].path);
        }
        break;
    }

    snprintf(s->cart_path_buf, sizeof(s->cart_path_buf), "vssmb");
    return true;
}


/* ── Snapshots (monitor "savevm"/"loadvm") ──────────────────────────────────
 *
 * Dumps the whole NesState verbatim (CPU/PPU/APU/mapper registers, work RAM,
 * OAM, etc. are all plain embedded arrays/scalars) plus the CHR banks when
 * they're RAM rather than ROM. Pointer/handle fields (the monitor, the
 * PRG/CHR ROM buffers, callback wiring set up once in nes_core_create(), the live
 * SDL audio device, ...) are never meaningful to persist - they're captured
 * before the struct is overwritten and spliced back in afterward, the same
 * pattern machine_i2000.c uses for its savevm/loadvm. */

/* Save states (stage N6) are the core machine's state: every part saves
 * itself (nes_core_build()), with the core's timers, the CPU's place in time
 * and the lines.  They are built in memory; the front end's savevm/loadvm
 * and its slots write and read them as files, and netplay keeps its
 * power-on state there.
 *
 * States from before N6 ("NEMSEVM1", the raw console struct) are refused,
 * as question 9 settled for the i2000's: no converter. */
#define NES_OLD_SNAPSHOT_MAGIC 0x314D5645534d454eULL /* "NEMSEVM1" */

bool nes_snapshot_save(NesState *s, uint8_t **data, size_t *len) {
    if (!s->core || !gemu_machine_save_state(nes_machine(s), data, len)) {
        fprintf(stderr, "nes: savevm: %s\n",
                s->core ? gemu_machine_error(nes_machine(s)) : "no core machine");
        return false;
    }
    return true;
}

bool nes_snapshot_load(NesState *s, const uint8_t *data, size_t len) {
    uint64_t magic = 0;
    if (len >= sizeof magic) memcpy(&magic, data, sizeof magic);
    if (magic == NES_OLD_SNAPSHOT_MAGIC) {
        uint32_t version = 0;
        if (len >= sizeof magic + sizeof version) memcpy(&version, data + sizeof magic, sizeof version);
        fprintf(stderr, "nes: loadvm: this is a save state in the old format (version %u), "
                "which this GEMU no longer reads; save a new one\n", version);
        return false;
    }
    if (!s->core || !gemu_machine_load_state(nes_machine(s), data, len)) {
        fprintf(stderr, "nes: loadvm: %s\n",
                s->core ? gemu_machine_error(nes_machine(s)) : "no core machine");
        return false;
    }
    /* the board's post_load did the windows, the Game Genie layer and the
     * APU's cache; the rest belongs to this run */
    s->pins_seen = s->pins.version - 1u;   /* the pin window's settings: reapply */
    s->clock_seen = 0;
    nes_pins_apply(s);
    rp2c02_refresh_argb(&s->ppu);
    return true;
}

/* ── Monitor commands and memory view ────────────────────────────────────── */

/* Side-effect-free CPU-space read for x/peek/memsave.  PPU, APU and
 * controller registers ($2000-$401F) and FDS I/O return false because
 * reading them on the bus clears flags or clocks shift registers. */
static bool nes_mon_read(void *ud, uint64_t addr64, uint8_t *val) {
    const NesState *s = ud;
    if (addr64 > 0xFFFFu) return false;
    uint16_t addr = (uint16_t)addr64;

    if (addr < 0x2000) { *val = s->ram[addr & 0x07FF]; return true; }
    if (addr < 0x4020) return false;

    if (s->fds_enabled) {
        if (addr < 0x6000) return false;
        *val = addr >= 0xE000 ? s->fds.bios[addr - 0xE000u]
                              : s->fds.ram[addr - 0x6000u];
        return true;
    }

    if (addr < 0x6000 && s->cfg->is_arcade) { *val = s->vs_coin_counter; return true; }
    uint8_t rom;
    if (!nes_cart_peek(&s->cart, addr, &rom)) return false;
    if (addr < 0x8000) { *val = rom; return true; }
    for (int i = 0; i < s->gg_count; i++) {
        if (s->gg_patches[i].addr == addr &&
            (!s->gg_patches[i].has_cmp || rom == s->gg_patches[i].cmp)) {
            rom = s->gg_patches[i].val;
            break;
        }
    }
    *val = rom;
    return true;
}

/* Writes reach internal RAM, mapped PRG-RAM and FDS RAM only. */
static bool nes_mon_write(void *ud, uint64_t addr64, uint8_t val) {
    NesState *s = ud;
    if (addr64 > 0xFFFFu) return false;
    uint16_t addr = (uint16_t)addr64;

    if (addr < 0x2000) { s->ram[addr & 0x07FF] = val; return true; }
    if (addr < 0x6000) return false;
    if (s->fds_enabled) {
        if (addr >= 0xE000) return false;
        s->fds.ram[addr - 0x6000u] = val;
        return true;
    }
    if (addr < 0x8000 && nes_cart_prg_ram_mapped(&s->cart)) {
        s->cart.prg_ram[addr & 0x1FFFu] = val;
        return true;
    }
    return false;
}

static void nes_cpu_state(void *ud, char *buf, size_t buf_len) {
    const NesState *s = ud;
    mos6502_format_state(&s->cpu, buf, buf_len);
}


static GemuMonCmd nes_cmd_mapperdump(GemuMonitor *mon, const char *args, void *ud) {
    NesState *s = ud;
    (void)args;
    char buf[1024];
    nes_cart_dump(&s->cart, buf, sizeof(buf));
    gemu_monitor_printf(mon, "%s", buf);
    return GEMU_MON_NONE;
}

static GemuMonCmd nes_cmd_dipswitch(GemuMonitor *mon, const char *args, void *ud) {
    (void)mon;
    vs_dip_cmd(ud, args);
    return GEMU_MON_NONE;
}

static GemuMonCmd nes_cmd_coin(GemuMonitor *mon, const char *args, void *ud) {
    (void)mon;
    vs_coin_cmd(ud, args);
    return GEMU_MON_NONE;
}

static GemuMonCmd nes_cmd_service(GemuMonitor *mon, const char *args, void *ud) {
    (void)mon; (void)args;
    vs_service_cmd(ud);
    return GEMU_MON_NONE;
}


/* sendkey <button>[-<button>...] [frames]: hold the buttons (e.g. "right-b-a")
 * on controller 1 for the given number of frames (default 5). */
static GemuMonCmd nes_cmd_sendkey(GemuMonitor *mon, const char *args, void *ud) {
    NesState *s = ud;
    static const struct { const char *name; uint8_t bit; } buttons[] = {
        { "a", NES_BTN_A }, { "b", NES_BTN_B }, { "select", NES_BTN_SELECT },
        { "start", NES_BTN_START }, { "up", NES_BTN_UP }, { "down", NES_BTN_DOWN },
        { "left", NES_BTN_LEFT }, { "right", NES_BTN_RIGHT },
    };
    uint8_t mask = 0;
    const char *p = args;
    while (*p && *p != ' ' && *p != '\t') {
        size_t len = strcspn(p, "-+ \t");
        uint8_t bit = 0;
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
            if (strlen(buttons[i].name) == len && strncasecmp(p, buttons[i].name, len) == 0)
                bit = buttons[i].bit;
        if (bit == NES_BTN_START && s->cfg->is_arcade) bit = VS_BTN_START;
        if (!bit) {
            gemu_monitor_printf(mon, "sendkey: unknown button '%.*s' "
                                "(a b start select up down left right, combine with -)\n",
                                (int)len, p);
            return GEMU_MON_NONE;
        }
        mask |= bit;
        p += len;
        if (*p == '-' || *p == '+') p++;
    }
    if (!mask) {
        gemu_monitor_printf(mon, "usage: sendkey <button>[-<button>...] [frames]\n");
        return GEMU_MON_NONE;
    }
    while (*p == ' ' || *p == '\t') p++;
    int frames = (*p >= '1' && *p <= '9') ? (int)strtol(p, NULL, 10) : 5;
    s->ctrl_inject_mask   = mask;
    s->ctrl_inject_frames = frames;
    gemu_monitor_printf(mon, "sendkey: holding 0x%02X for %d frame(s)\n", mask, frames);
    return GEMU_MON_NONE;
}

static GemuMonCmd nes_cmd_ppudump(GemuMonitor *mon, const char *args, void *ud) {
    NesState *s = ud;
    const Rp2c02 *ppu = &s->ppu;
    (void)args;
    gemu_monitor_printf(mon, "PPU: ctrl=%02X mask=%02X status=%02X v=%04X t=%04X x=%d w=%d\n",
                        ppu->ppuctrl, ppu->ppumask, ppu->ppustatus,
                        ppu->v, ppu->t, ppu->x, ppu->w);
    gemu_monitor_printf(mon, "Palette BG:  ");
    for (int i = 0; i < 16; i++) gemu_monitor_printf(mon, "%02X ", ppu->palette[i]);
    gemu_monitor_printf(mon, "\nPalette SPR: ");
    for (int i = 0; i < 16; i++) gemu_monitor_printf(mon, "%02X ", ppu->palette[i + 16]);
    gemu_monitor_printf(mon, "\nSL0 pixels[0..31]: ");
    for (int i = 0; i < 32; i++) gemu_monitor_printf(mon, "%02X ", ppu->pixels[i]);
    gemu_monitor_printf(mon, "\nSL8 pixels[0..31]: ");
    for (int i = 0; i < 32; i++) gemu_monitor_printf(mon, "%02X ", ppu->pixels[256*8 + i]);
    gemu_monitor_printf(mon, "\nNT row0 tiles: ");
    for (int i = 0; i < 32; i++) gemu_monitor_printf(mon, "%02X ", ppu->vram[i]);
    gemu_monitor_printf(mon, "\nNT row1 tiles: ");
    for (int i = 0; i < 32; i++) gemu_monitor_printf(mon, "%02X ", ppu->vram[32 + i]);
    gemu_monitor_printf(mon, "\nNT attr[0..7]: ");
    for (int i = 0; i < 8; i++) gemu_monitor_printf(mon, "%02X ", ppu->vram[0x3C0 + i]);
    gemu_monitor_printf(mon, "\nOAM[0..3]:     ");
    for (int i = 0; i < 16; i++) gemu_monitor_printf(mon, "%02X ", ppu->oam[i]);
    gemu_monitor_printf(mon, "\nOAM visible:   ");
    int shown = 0;
    for (int i = 0; i < 64; i++) {
        uint8_t y = ppu->oam[i * 4 + 0];
        if (y >= 0xEF) continue;
        gemu_monitor_printf(mon, "#%02d:%02X %02X %02X %02X ",
                            i, y, ppu->oam[i * 4 + 1],
                            ppu->oam[i * 4 + 2], ppu->oam[i * 4 + 3]);
        if (++shown == 12) break;
    }
    if (!shown) gemu_monitor_printf(mon, "(none)");
    gemu_monitor_printf(mon, "\n");
    return GEMU_MON_NONE;
}


static GemuMonCmd nes_cmd_cartpin(GemuMonitor *mon, const char *args, void *ud) {
    NesState *s = ud;
    NesPins *p = &s->pins;
    char a[32] = "", b[32] = "";
    int n = sscanf(args, "%31s %31s", a, b);
    if (n <= 0 || !strcmp(a, "list")) {
        static const char *const st[3] = { "good", "OPEN", "flaky" };
        gemu_monitor_printf(mon, "M2 clock %u Hz (console: %u), tilt %+.1f deg%s\n", p->clock_hz,
                            p->clock_default, p->tilt_deg,
                            nes_pins_dead(nes_pins_open_mask(p)) ? "   cartridge has no power" : "");
        for (int i = 1; i <= NES_PINS_N; i++) {
            NesPinState ps = nes_pins_get(p, i);
            if (n > 0 || ps != NES_PIN_GOOD)
                gemu_monitor_printf(mon, "  %2d %-10s %-6s%s %s\n", i, nes_pin_info[i].label, st[ps],
                                    ps != nes_pins_get_user(p, i) ? "(tilt)" : "      ",
                                    nes_pin_info[i].what);
        }
        if (n <= 0 && !nes_pins_any(p))
            gemu_monitor_printf(mon, "  every contact good ('cartpin list' shows all 60)\n");
    } else if (!strcmp(a, "reinsert")) {
        nes_pins_reinsert(p);
    } else if (!strcmp(a, "tilt")) {
        char *end;
        double d = strtod(b, &end);
        if (!b[0] || *end || fabs(d) > NES_PIN_TILT_MAX) {
            gemu_monitor_printf(mon, "cartpin: tilt -%.0f..%.0f degrees (+ lifts the pin 30 end)\n",
                                NES_PIN_TILT_MAX, NES_PIN_TILT_MAX);
            return GEMU_MON_NONE;
        }
        nes_pins_set_tilt(p, d);
    } else if (!strcmp(a, "clock")) {
        if (!strcmp(b, "default")) p->clock_hz = p->clock_default;
        else {
            unsigned long hz = strtoul(b, NULL, 0);
            if (hz < NES_PIN_CLOCK_MIN || hz > NES_PIN_CLOCK_MAX) {
                gemu_monitor_printf(mon, "cartpin: clock %u-%u Hz, or default\n",
                                    NES_PIN_CLOCK_MIN, NES_PIN_CLOCK_MAX);
                return GEMU_MON_NONE;
            }
            p->clock_hz = (uint32_t)hz;
        }
    } else {
        int pin = nes_pins_find(a);
        NesPinState ps;
        if (!pin) { gemu_monitor_printf(mon, "cartpin: no pin '%s' (1-60 or a label, e.g. m2, cpu_a3)\n", a); return GEMU_MON_NONE; }
        if (!strcmp(b, "good")) ps = NES_PIN_GOOD;
        else if (!strcmp(b, "open") || !strcmp(b, "bad")) ps = NES_PIN_BROKEN;
        else if (!strcmp(b, "flaky")) ps = NES_PIN_FLAKY;
        else { gemu_monitor_printf(mon, "usage: cartpin <pin> good|open|flaky\n"); return GEMU_MON_NONE; }
        nes_pins_set(p, pin, ps);
    }
    nes_pins_apply(s);
    return GEMU_MON_NONE;
}


/* ── Run loop ────────────────────────────────────────────────────────────── */

/* Execute one CPU instruction and bring the APU, expansion audio and PPU up
 * to the same cycle. */
void nes_step_insn(NesState *s) {
    if (s->exec_hook) s->exec_hook(s->exec_hook_ud, s->cpu.PC);
    mos6502_step(&s->cpu);
    nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
}


/* ── The console's front-end interface (nes.h) ───────────────────────────── */

static void nes_monitor_setup(NesState *s) {
    GemuMonitor *mon = s->monitor;
    gemu_monitor_set_memory(mon, &(GemuMonMemory){
        .read = nes_mon_read, .write = nes_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = false });
    gemu_monitor_set_cpu_state_cb(mon, nes_cpu_state, s);
    gemu_monitor_bind(mon, "mapperdump", nes_cmd_mapperdump, s);
    gemu_monitor_bind(mon, "sendkey",    nes_cmd_sendkey,    s);
    gemu_monitor_bind(mon, "ppudump",    nes_cmd_ppudump,    s);
    if (!s->cfg->is_arcade)   /* a VS. board has no cartridge connector */
        gemu_monitor_bind(mon, "cartpin", nes_cmd_cartpin, s);
    if (s->cfg->is_arcade) {
        gemu_monitor_bind(mon, "dipswitch", nes_cmd_dipswitch, s);
        gemu_monitor_bind(mon, "coin",      nes_cmd_coin,      s);
        gemu_monitor_bind(mon, "service",   nes_cmd_service,   s);
    }
}

/* ── The console on the core (stage N) ──────────────────────────────────────
 *
 *   board    the console's glue: work RAM, the 2A03's DMA unit and its
 *            catch-up counters, the controller ports, the VS. System's
 *            inputs, the data bus
 *   cpu      the 2A03's 6502
 *   ppu      the 2C02 (2C07, 2C04 ...)
 *   apu      the 2A03's sound and frame counter
 *   cart     the cartridge slot (the board inside it)
 *   fds      the Famicom Disk System's RAM adapter, when there is one
 *
 * N1: the parts stay where NesState keeps them - the front end, Lua and
 * netplay read them there - and each device describes (`info`) and saves
 * its part.  The console still runs itself; N2 moves its decode into spaces
 * and lines, N3 its CPU onto the scheduler. */
typedef struct NesCore {
    GemuMachine *machine;
    GemuDevice  *board, *cpu, *ppu, *apu, *cart, *fds;
    /* N2: the buses and the interrupt lines */
    GemuSpace   *bus, *ppu_bus;
    GemuMapping *gg;                     /* the Game Genie's layer */
    GemuMapping *prg_map[4];             /* the board's PRG windows, read-only */
    uint8_t     *prg_ptr[4];
    GemuMapping *chr_map[8], *chr_wmap[8]; /* its CHR windows: reads, RAM writes */
    uint8_t     *chr_ptr[8], *chr_wptr[8];
    GemuMapping *nt_map[4];              /* CIRAM, as the board mirrors it */
    uint8_t     *nt_ptr[4];
    uint8_t     *ppage[2][64];           /* nes_ppu_page(): as page[] for the PPU's bus */
    uint64_t     ppage_gen;
    /* N3: the 2A03's 6502 runs the machine (nes_exec_run()) */
    GemuExecutor *exec;
    uint64_t     run_start;              /* cycle_count when this run began */
    bool         stopped;                /* the last run ended at a breakpoint or pause */
    /* N5: the APU's sound (expansion audio mixed in) */
    GemuStream  *sound;
    /* N4: the PPU's picture */
    GemuScreen  *screen;
    uint64_t     frame_seen;             /* ppu.frame when the screen last began one */
    /* nes_page(): host memory per 256-byte page, [0] reads, [1] writes;
     * NES_PAGE_SLOW = a space access.  Good while the space's generation
     * is page_gen. */
    uint8_t     *page[2][256];
    uint64_t     page_gen;
    GemuLine    *nmi, *apu_irq, *cart_irq;
    GemuLineIn  *cpu_nmi, *cpu_irq;
} NesCore;

static GemuMachine *nes_machine(const NesState *s) { return s->core->machine; }
static GemuSpace *nes_space_cpu(const NesState *s) { return s->core->bus; }

static uint8_t nes_page_slow;
#define NES_PAGE_SLOW (&nes_page_slow)

static inline uint8_t *nes_page(NesState *s, uint16_t addr, bool write) {
    NesCore *c = s->core;
    uint64_t gen = gemu_space_generation(c->bus);
    if (c->page_gen != gen) {
        memset(c->page, 0, sizeof c->page);
        c->page_gen = gen;
    }
    uint8_t **slot = &c->page[write][addr >> 8];
    if (!*slot) {
        uint64_t avail;
        uint8_t *p = gemu_space_ptr(c->bus, addr & 0xFF00u, write, &avail);
        *slot = p && avail >= 256 ? p : NES_PAGE_SLOW;
    }
    return *slot == NES_PAGE_SLOW ? NULL : *slot;
}

static inline uint8_t *nes_ppu_page(NesState *s, uint16_t addr, bool write) {
    NesCore *c = s->core;
    uint64_t gen = gemu_space_generation(c->ppu_bus);
    if (c->ppage_gen != gen) {
        memset(c->ppage, 0, sizeof c->ppage);
        c->ppage_gen = gen;
    }
    addr &= 0x3FFFu;
    uint8_t **slot = &c->ppage[write][addr >> 8];
    if (!*slot) {
        uint64_t avail;
        uint8_t *p = gemu_space_ptr(c->ppu_bus, addr & 0x3F00u, write, &avail);
        *slot = p && avail >= 256 ? p : NES_PAGE_SLOW;
    }
    return *slot == NES_PAGE_SLOW ? NULL : *slot;
}

/* One window of host memory in a space: mapped at base (size bytes, with
 * flags) when ptr is set, re-pointed as ptr moves, disabled when it's NULL. */
static void nes_window(GemuSpace *sp, GemuMapping **map, uint8_t **cur, uint64_t base,
                       uint64_t size, unsigned flags, uint8_t *ptr, const char *name) {
    if (!ptr) {
        if (*map && *cur) gemu_mapping_enable(*map, false);
        *cur = NULL;
        return;
    }
    if (!*map) {
        *map = gemu_space_map_ram(sp, base, size, ptr, flags, 1);
        if (*map) gemu_mapping_set_name(*map, name);
    } else if (!*cur) {
        gemu_mapping_set_ptr(*map, ptr);
        gemu_mapping_enable(*map, true);
    } else if (*cur != ptr) {
        gemu_mapping_set_ptr(*map, ptr);
    }
    *cur = *map ? ptr : NULL;
}

/* The board's windows as plain memory in the spaces, where the board reads
 * them plainly: PRG ROM read-only (no prg_read hook, as MMC5's RAM windows
 * need; writes reach the board's handler underneath), CHR read-only plus
 * writes where it's RAM (no chr_read hook, as MMC2's latches need), and
 * CIRAM as the board's mirroring arranges it (unless it maps the nametables
 * itself).  After anything that can move them: a write to the board, $4016
 * (the VS. System's CHR bank), the Disk System's mirroring, a reset, a load,
 * another cartridge. */
static void nes_maps_sync(NesState *s) {
    NesCore *c = s->core;
    if (!c || !c->bus) return;
    NesCart *k = &s->cart;
    bool board = k->ops != NULL;
    bool prg = board && k->prg && !k->ops->prg_read && !s->fds_enabled;
    for (int i = 0; i < 4; i++)
        nes_window(c->bus, &c->prg_map[i], &c->prg_ptr[i], 0x8000u + 0x2000u * (unsigned)i,
                   0x2000, GEMU_MAP_READONLY, prg ? k->prg + k->prg_off[i] : NULL, "PRG window");
    bool chr = board && k->chr && !k->ops->chr_read;
    for (int i = 0; i < 8; i++) {
        uint8_t *p = chr ? k->chr + k->chr_off[i] : NULL;
        nes_window(c->ppu_bus, &c->chr_map[i], &c->chr_ptr[i], 0x400u * (unsigned)i, 0x400,
                   GEMU_MAP_READONLY, p, "CHR window");
        nes_window(c->ppu_bus, &c->chr_wmap[i], &c->chr_wptr[i], 0x400u * (unsigned)i, 0x400,
                   GEMU_MAP_WRITEONLY, (k->chr_wmask >> i) & 1u ? p : NULL, "CHR RAM window");
    }
    bool ciram = !(board && nes_cart_wants_nt(k));
    for (int i = 0; i < 4; i++)
        nes_window(c->ppu_bus, &c->nt_map[i], &c->nt_ptr[i], 0x2000u + 0x400u * (unsigned)i, 0x400,
                   0, ciram ? s->ppu.vram + rp2c02_ciram_index(&s->ppu, (uint16_t)(0x2000u + 0x400u * (unsigned)i))
                            : NULL, "CIRAM");
}
static GemuSpace *nes_space_ppu(const NesState *s) { return s->core->ppu_bus; }

/* The 2A03's /NMI (an edge: the PPU's vblank) and /IRQ (level, wired-OR). */
static void cpu_nmi_in(void *ud, int idx, bool level) {
    (void)idx;
    if (level) ((NesState *)ud)->cpu.nmi = true;
}
static void cpu_irq_in(void *ud, int idx, bool level) {
    (void)idx;
    ((NesState *)ud)->cpu.irq = level;
}
static void nes_lines_irq(NesState *s, bool apu, bool cart) {
    if (!s->core) { s->cpu.irq = apu || cart; return; }
    gemu_line_set(s->core->apu_irq, apu);
    gemu_line_set(s->core->cart_irq, cart);
}
static void nes_lines_nmi(NesState *s) {
    if (s->core) gemu_line_pulse(s->core->nmi);
    else s->cpu.nmi = true;
}

/* The Game Genie's codes changed (the front end rebuilt gg_patches). */
void nes_gg_changed(NesState *s) {
    if (s->core && s->core->gg)
        gemu_mapping_enable(s->core->gg, s->gg_count > 0);
}

/* The spaces' mappings for this console (see "The CPU's bus"). */
static bool nes_bus_build(NesState *s) {
    NesCore *c = s->core;
    GemuMachine *m = c->machine;
    if (!(c->bus = gemu_space_new(m, "program", 16, 8, GEMU_LITTLE_ENDIAN)) ||
        !(c->ppu_bus = gemu_space_new(m, "ppu", 14, 8, GEMU_LITTLE_ENDIAN)))
        return false;
    bool ok = gemu_space_map_ram(c->bus, 0x0000, 0x800, s->ram, 0, 0) &&
              gemu_space_mirror(c->bus, 0x0000, 0x2000, 0x07FF) &&
              gemu_space_map(c->bus, 0x2000, 8, &nes_bus_ppu, s, 0, 0) &&
              gemu_space_mirror(c->bus, 0x2000, 0x2000, 0x2007) &&
              gemu_space_map(c->bus, 0x4000, 0x20, &nes_bus_io, s, 0, 0) &&
              gemu_space_map(c->bus, 0x4020, 0x10000 - 0x4020, &nes_bus_cart, s, 0, 0);
    if (ok && s->cfg->is_arcade)
        ok = gemu_space_map(c->bus, 0x4020, 0x6000 - 0x4020, &nes_bus_vs, s, 2, 0);
    if (ok && s->fds_enabled)
        ok = gemu_space_map(c->bus, 0x4020, 0x70, &nes_bus_fds_reg, s, 2, 0) &&
             gemu_space_map(c->bus, 0x6000, 0xA000, &nes_bus_fds_mem, s, 2, 0);
    if (ok && (c->gg = gemu_space_map(c->bus, 0x8000, 0x8000, &nes_bus_gg, s, 3, 0)))
        gemu_mapping_enable(c->gg, false);
    ok = ok && c->gg &&
         gemu_space_map(c->ppu_bus, 0x0000, 0x2000, &nes_ppu_chr, s, 0, 0) &&
         gemu_space_map(c->ppu_bus, 0x2000, 0x1000, &nes_ppu_nt, s, 0, 0) &&
         gemu_space_mirror(c->ppu_bus, 0x2000, 0x2000, 0x2FFF);
    return ok;
}

/* NMI from the PPU, /IRQ from the APU and the cartridge (wired-OR). */
static bool nes_lines_build(NesState *s) {
    NesCore *c = s->core;
    return (c->cpu_nmi = gemu_line_in(c->cpu, "nmi", cpu_nmi_in, s, 0)) &&
           (c->cpu_irq = gemu_line_in(c->cpu, "irq", cpu_irq_in, s, 0)) &&
           (c->nmi = gemu_line_out(c->ppu, "nmi")) &&
           (c->apu_irq = gemu_line_out(c->apu, "irq")) &&
           (c->cart_irq = gemu_line_out(c->cart, "irq")) &&
           gemu_line_connect_in(c->nmi, c->cpu_nmi, 0) &&
           gemu_line_connect_in(c->apu_irq, c->cpu_irq, 0) &&
           gemu_line_connect_in(c->cart_irq, c->cpu_irq, 0);
}

/* The 6502 as the machine's executor, at the console's CPU clock (the pin
 * window's clock).  A run executes instructions until its budget is spent,
 * the PPU finishes a frame, or the monitor stops it; the PPU and APU still
 * catch up to the CPU's cycle - the executor's time - at a register access,
 * a DMA or the end of the frame. */
static uint64_t nes_exec_run(void *ud, uint64_t budget) {
    NesState *s = ud;
    NesCore *c = s->core;
    uint64_t start = c->run_start = s->cpu.cycle_count;
    (void)budget;
    while (s->cpu.cycle_count - start < gemu_executor_budget(c->exec)) {
        if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) { c->stopped = true; break; }
        nes_step_insn(s);
        if (gemu_monitor_is_paused(s->monitor)) { c->stopped = true; break; }
        if (s->ppu.dirty) break;
    }
    if (c->stopped || s->ppu.dirty)
        gemu_machine_stop(c->machine);
    return s->cpu.cycle_count - start;
}

static void nes_screen_timing(NesState *s, uint32_t hz);
static void nes_exec_clock(NesState *s, uint32_t hz) {
    if (s->core && s->core->exec) {
        gemu_executor_set_clock(s->core->exec, hz);
        nes_screen_timing(s, hz);
    }
}

/* ── The PPU's screen (stage N4) ─────────────────────────────────────────────
 * Device-timed: a frame begins when the PPU starts line 0 (ppu.frame moves,
 * nes_sync_ppu_to_cpu()); the raw timing describes its beam (341 dots a
 * line, the region's lines, three dots per CPU cycle) for vpos/hpos.  The
 * bitmap is the PPU's ARGB frame, copied when the front end renders it.
 * The CRT filters read the PPU's palette-index frame, not this: they encode
 * a composite signal from the indices. */
static void nes_screen_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    const NesState *s = ud;
    for (int y = clip->y0; y < clip->y1; y++)
        memcpy(bmp->pix + (size_t)y * (size_t)bmp->stride,
               s->ppu.pixels_argb + (size_t)y * RP2C02_WIDTH, RP2C02_WIDTH * sizeof(uint32_t));
}

static void nes_screen_timing(NesState *s, uint32_t hz) {
    if (s->core->screen)
        gemu_screen_set_raw(s->core->screen, (uint64_t)hz * 3u, RP2C02_DOTS_PER_LINE, 0,
                            RP2C02_WIDTH, s->ppu.lines_total, 0, RP2C02_HEIGHT, false);
}

static void nes_screen_frame(NesState *s) {
    NesCore *c = s->core;
    if (!c || !c->screen) return;
    c->frame_seen = s->ppu.frame;
    gemu_screen_frame_begin(c->screen);
}

static bool nes_screen_build(NesState *s) {
    NesCore *c = s->core;
    if (!(c->screen = gemu_screen_new(c->ppu, "screen")) ||
        !gemu_screen_set_size(c->screen, RP2C02_WIDTH, RP2C02_HEIGHT))
        return false;
    gemu_screen_set_update(c->screen, nes_screen_update, s);
    nes_screen_timing(s, s->pins.clock_hz);
    return true;
}

GemuScreen *nes_screen(NesState *s) { return s->core ? s->core->screen : NULL; }

/* ── The APU's stream (stage N5) ─────────────────────────────────────────────
 * The APU makes its samples as it catches up (resampled to 44.1 kHz, the
 * cartridge's or Disk System's expansion audio already mixed in, the pin
 * window's cuts applied) into apu.frame_buf.  Its stream hands them on as
 * the mixer asks, in order, none lost: a request for more than there are
 * (the two clocks round differently by a sample now and then) repeats the
 * last; what's left over waits for the next.  The mixer runs at the end of
 * each frame (nes_run_frame()) and sends the result to the host audio
 * device; a slow clock that fills the APU's buffer mid-frame brings the
 * stream up to date then (nes_sound_drain()).  MIDI out (-soundhw
 * 2a03-midi) taps the APU's register writes and stays outside. */
static void nes_sound_update(void *ud, float *const *out, unsigned n) {
    NesState *s = ud;
    Apu2a03 *a = &s->apu;
    unsigned have = (unsigned)a->frame_n, take = n < have ? n : have;
    memcpy(out[0], a->frame_buf, take * sizeof(float));
    float last = take ? out[0][take - 1] : (have ? a->frame_buf[0] : 0.0f);
    for (unsigned i = take; i < n; i++) out[0][i] = last;
    if (take < have)
        memmove(a->frame_buf, a->frame_buf + take, (have - take) * sizeof(float));
    a->frame_n = (int)(have - take);
}

static void nes_sound_drain(NesState *s) {
    if (s->core && s->core->sound) gemu_stream_update(s->core->sound);
    else s->apu.frame_n = 0;
}

static void nes_sound_out(void *ud, const float *samples, size_t frames) {
    gemu_audio_queue(ud, samples, frames);
}

static bool nes_sound_build(NesState *s) {
    NesCore *c = s->core;
    if (!(c->sound = gemu_stream_new(c->apu, "sound", 1, APU2A03_SAMPLE_RATE,
                                     nes_sound_update, s)))
        return false;
    gemu_mixer_set_output(c->machine, APU2A03_SAMPLE_RATE, 1,
                          s->apu.audio_dev ? nes_sound_out : NULL, s->apu.audio_dev);
    return true;
}

static uint64_t nes_exec_elapsed(void *ud) {
    NesState *s = ud;
    return s->cpu.cycle_count - s->core->run_start;
}

/* After the raw snapshot put the CPU's pins behind the lines' backs. */
static void nes_lines_resync(NesState *s) {
    if (!s->core) return;
    nes_update_irq(s);
    s->cpu.irq = gemu_line_in_level(s->core->cpu_irq);
    nes_gg_changed(s);
}

static NesState *nes_of(GemuDevice *d) { return *(NesState **)d->priv; }

static void nes_board_describe(GemuDevice *d, char *buf, size_t len) {
    const NesState *s = nes_of(d);
    snprintf(buf, len,
             "data bus %02X, cartridge /IRQ %s\n"
             "PPU synced to cycle %" PRIu64 ", APU to %" PRIu64 "\n"
             "DMA: %s%s%s\n"
             "controller strobe %s, shift registers %02X %02X",
             s->cpu_bus, s->cart_irq ? "low" : "high",
             s->ppu_synced_cpu_cycle, s->apu_synced_cpu_cycle,
             s->dma_halt ? "waiting" : "idle", s->oam_dma ? ", OAM" : "",
             s->dmc_dma ? ", DMC" : "",
             s->ctrl_strobe ? "high" : "low", s->ctrl_shift[0], s->ctrl_shift[1]);
}

static void nes_cpu_describe(GemuDevice *d, char *buf, size_t len) {
    const Mos6502 *c = &nes_of(d)->cpu;
    snprintf(buf, len, "PC %04X  A %02X  X %02X  Y %02X  SP %02X  P %02X\n"
             "cycle %" PRIu64 ", instruction %" PRIu64 "%s%s",
             c->PC, c->A, c->X, c->Y, c->SP, c->P, c->cycle_count, c->insn_count,
             c->nmi ? ", NMI pending" : "", c->irq ? ", /IRQ low" : "");
}

static void nes_ppu_describe(GemuDevice *d, char *buf, size_t len) {
    const Rp2c02 *p = &nes_of(d)->ppu;
    snprintf(buf, len, "frame %" PRIu64 ", scanline %d, dot %d (%d lines)\n"
             "CTRL %02X  MASK %02X  STATUS %02X  OAMADDR %02X  v %04X  t %04X  x %u",
             p->frame, p->scanline, p->dot, p->lines_total,
             p->ppuctrl, p->ppumask, p->ppustatus, p->oamaddr, p->v, p->t, p->x);
}

static void nes_apu_describe(GemuDevice *d, char *buf, size_t len) {
    const Apu2a03 *a = &nes_of(d)->apu;
    snprintf(buf, len, "channels %s%s%s%s%s, frame counter %u-step step %u%s%s\n"
             "cycle %" PRIu64,
             a->ch_en[0] ? "P1 " : "", a->ch_en[1] ? "P2 " : "", a->ch_en[2] ? "T " : "",
             a->ch_en[3] ? "N " : "", a->ch_en[4] ? "DMC" : "",
             a->fc_mode ? 5 : 4, a->fc_step, a->fc_irq ? ", frame IRQ" : "",
             a->fc_irq_inhibit ? " (inhibited)" : "", a->cycle);
}

static void nes_cart_describe(GemuDevice *d, char *buf, size_t len) {
    const NesCart *c = &nes_of(d)->cart;
    if (!c->prg) {
        snprintf(buf, len, "empty");
        return;
    }
    snprintf(buf, len, "mapper %u, %u x 16 KB PRG, %u x 8 KB CHR%s\n"
             "PRG windows %05X %05X %05X %05X\n"
             "CHR windows %05X %05X %05X %05X %05X %05X %05X %05X%s",
             (unsigned)c->info.mapper, (unsigned)c->info.prg_banks, (unsigned)c->info.chr_banks,
             c->chr_is_ram ? " (RAM)" : "",
             c->prg_off[0], c->prg_off[1], c->prg_off[2], c->prg_off[3],
             c->chr_off[0], c->chr_off[1], c->chr_off[2], c->chr_off[3],
             c->chr_off[4], c->chr_off[5], c->chr_off[6], c->chr_off[7],
             c->irq ? "\n/IRQ low" : "");
}

static void nes_fds_describe(GemuDevice *d, char *buf, size_t len) {
    const FdsState *f = &nes_of(d)->fds;
    snprintf(buf, len, "%s BIOS, %s%s", f->hle_mode ? "built-in" : "ROM",
             f->raw_disk ? "a disk in the drive" : "no disk", f->dirty ? " (changed)" : "");
}

#define NES_TYPE(var, tname, tdesc, describe_fn) \
    static const GemuDeviceType var = { .name = tname, .desc = tdesc, \
        .size = sizeof(NesState *), .describe = describe_fn }
/* After a load: what follows from the board registers and the codes. */
static void nes_board_post_load(GemuDevice *d) {
    NesState *s = nes_of(d);
    nes_maps_sync(s);
    nes_gg_changed(s);
    s->apu_deadline = 0;
}
/* The machine's settings (stage N7): -M nes,-frame_irq and the like go to
 * whichever of these devices has the option (nes_machine_opts()). */
static const GemuOptionDef nes_board_opts[] = {
    { "dips", "VS. System DIP switches SW1 1-8, as a byte", GEMU_OPT_INT, "0", 0, 255, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};
static const GemuOptionDef nes_apu_opts[] = {
    { "sweep_registers", "APU sweep units", GEMU_OPT_BOOL, "on", 0, 0, NULL },
    { "dmc_channel", "APU DMC (sample) channel", GEMU_OPT_BOOL, "on", 0, 0, NULL },
    { "frame_irq", "APU frame counter IRQ", GEMU_OPT_BOOL, "on", 0, 0, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};
static const GemuOptionDef nes_ppu_opts[] = {
    { "odd_frame_skip", "PPU skips a dot on odd frames", GEMU_OPT_BOOL, "on", 0, 0, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};

static const GemuDeviceType nes_board_type = {
    .name = "nes-board", .desc = "NES/Famicom console board", .size = sizeof(NesState *),
    .describe = nes_board_describe, .post_load = nes_board_post_load, .options = nes_board_opts,
};
NES_TYPE(nes_cpu_type, "rp2a03-cpu", "Ricoh 2A03/2A07 CPU (6502)", nes_cpu_describe);
static const GemuDeviceType nes_ppu_type = {
    .name = "rp2c02", .desc = "Ricoh 2C02/2C07 PPU", .size = sizeof(NesState *),
    .describe = nes_ppu_describe, .options = nes_ppu_opts,
};
static const GemuDeviceType nes_apu_type = {
    .name = "rp2a03-apu", .desc = "Ricoh 2A03/2A07 APU", .size = sizeof(NesState *),
    .describe = nes_apu_describe, .options = nes_apu_opts,
};
NES_TYPE(nes_cart_type, "nes-cart-slot", "NES/Famicom cartridge slot", nes_cart_describe);
NES_TYPE(nes_fds_type, "fds-adapter", "Famicom Disk System RAM adapter", nes_fds_describe);

#define BOARD(field) ok &= GEMU_STATE_MEMBER(d, "", s, field)
/* The board's own state.  Not saved: the configuration (cfg, fds_enabled),
 * host input the front end sets each frame (ctrl_state, zapper_*, mic_noise,
 * ctrl_inject_*, port2_controller is netplay's), the Game Genie codes (the
 * front end applies its own after a load), the pin window's settings and
 * what follows from them (pins, pins_seen, clock_seen), the VS. palette
 * (from the ROM set), the monitor and hooks, and apu_deadline (a cache,
 * cleared by a load). */
static bool nes_board_state(NesState *s, GemuDevice *d) {
    bool ok = true;
    BOARD(ram);
    BOARD(dma_pending);
    BOARD(dma_page);
    BOARD(ctrl_shift);
    BOARD(ctrl_strobe);
    BOARD(ppu_synced_cpu_cycle);
    BOARD(apu_synced_cpu_cycle);
    BOARD(apu_syncing);
    BOARD(dma_halt);
    BOARD(dmc_dma);
    BOARD(dmc_dummy);
    BOARD(dmc_abort);
    BOARD(oam_dma);
    BOARD(oam_page);
    BOARD(in_dma);
    BOARD(vs_dip);
    BOARD(vs_coin_counter);
    BOARD(vs_coin_latch);
    BOARD(vs_coin_prev);
    BOARD(vs_service);
    BOARD(cart_irq);
    BOARD(cpu_bus);
    return ok;
}
#undef BOARD

/* -M's settings, "-name", "+name", "name" or "name=value", each to the
 * device whose type has the option: opts[i] for types[i]. */
static bool nes_machine_opts(const char *spec, const GemuDeviceType *const *types, char (*opts)[256],
                             int n) {
    for (int i = 0; i < n; i++) opts[i][0] = 0;
    const char *p = spec ? spec : "";
    while (*p) {
        size_t len = strcspn(p, ",");
        char tok[128];
        if (len && len < sizeof tok) {
            memcpy(tok, p, len);
            tok[len] = 0;
            const char *name = tok, *val = NULL;
            if (*name == '-' || *name == '+') { val = *name == '-' ? "off" : "on"; name++; }
            char *eq = strchr(tok, '=');
            if (eq) { *eq = 0; val = eq + 1; }
            int found = -1;
            for (int i = 0; i < n && found < 0; i++)
                for (const GemuOptionDef *o = types[i]->options; o && o->name; o++)
                    if (!strcmp(o->name, name)) { found = i; break; }
            if (found < 0) {
                fprintf(stderr, "gemu: unknown -M setting '%s'; this machine has:", name);
                for (int i = 0; i < n; i++)
                    for (const GemuOptionDef *o = types[i]->options; o && o->name; o++)
                        fprintf(stderr, " %s", o->name);
                fprintf(stderr, "\n");
                return false;
            }
            size_t at = strlen(opts[found]);
            snprintf(opts[found] + at, sizeof opts[found] - at, "%s%s%s%s", at ? "," : "", name,
                     val ? "=" : "", val ? val : "");
        }
        p += len;
        if (*p == ',') p++;
    }
    return true;
}

static GemuDevice *nes_part_opts(NesState *s, const GemuDeviceType *t, const char *name,
                                 const char *opts);
static GemuDevice *nes_part(NesState *s, const GemuDeviceType *t, const char *name) {
    return nes_part_opts(s, t, name, NULL);
}

static GemuDevice *nes_part_opts(NesState *s, const GemuDeviceType *t, const char *name,
                                 const char *opts) {
    GemuDevice *d = gemu_device_add(s->core->machine, t, name, NULL, opts && *opts ? opts : NULL);
    if (d)
        *(NesState **)d->priv = s;
    return d;
}

static bool nes_core_build(NesState *s) {
    NesCore *c = s->core = calloc(1, sizeof *c);
    if (!c || !(c->machine = gemu_machine_new("nes")))
        return false;
    static const GemuDeviceType *const set_types[] = { &nes_board_type, &nes_apu_type, &nes_ppu_type };
    char set[3][256];
    if (!nes_machine_opts(s->cfg->machine_opts, set_types, set, 3))
        return false;
    bool ok = (c->board = nes_part_opts(s, &nes_board_type, "board", set[0])) &&
              nes_board_state(s, c->board);
    ok = ok && (c->cpu = nes_part(s, &nes_cpu_type, "cpu")) &&
         mos6502_state_add(c->cpu, "", &s->cpu);
    ok = ok && (c->ppu = nes_part_opts(s, &nes_ppu_type, "ppu", set[2])) &&
         rp2c02_state_add(c->ppu, "", &s->ppu);
    ok = ok && (c->apu = nes_part_opts(s, &nes_apu_type, "apu", set[1])) &&
         apu2a03_state_add(c->apu, "", &s->apu);
    ok = ok && (c->cart = nes_part(s, &nes_cart_type, "cart")) &&
         nes_cart_state_add(c->cart, "", &s->cart);
    if (ok && s->fds_enabled)
        ok = (c->fds = nes_part(s, &nes_fds_type, "fds")) && fds_state_add(c->fds, "", &s->fds);
    if (ok) {
        /* the settings, into the chips */
        s->apu.sweep_enabled = gemu_opt_bool(c->apu, "sweep_registers");
        s->apu.dmc_enabled = gemu_opt_bool(c->apu, "dmc_channel");
        s->apu.frame_irq_enabled = gemu_opt_bool(c->apu, "frame_irq");
        if (!gemu_opt_bool(c->ppu, "odd_frame_skip")) s->ppu.no_odd_skip = true;
        if (s->cfg->is_arcade) s->vs_dip = (uint8_t)gemu_opt_int(c->board, "dips");
    }
    bool inited = gemu_machine_init(c->machine);
    static const GemuExecutorOps exec_ops = { .run = nes_exec_run, .elapsed = nes_exec_elapsed };
    ok = ok && inited && nes_bus_build(s) && nes_lines_build(s) &&
         (c->exec = gemu_executor_new(c->cpu, s->pins.clock_hz, &exec_ops, s)) &&
         nes_screen_build(s) && nes_sound_build(s);
    if (!inited || !ok || !gemu_machine_start(c->machine)) {
        fprintf(stderr, "nes: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    gemu_monitor_attach_machine(s->monitor, c->machine, "program");
    nes_install_mapper_hooks(s);       /* the PPU onto its space */
    nes_maps_sync(s);
    return true;
}

static void nes_core_free(NesState *s) {
    if (!s->core)
        return;
    gemu_machine_free(s->core->machine);
    free(s->core);
    s->core = NULL;
}

NesState *nes_core_create(const MosConfig *cfg) {
    NesState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg         = cfg;
    s->fds_enabled = cfg->fds_enabled;
    nes_cart_wire(s);

    if (s->fds_enabled) {
        /* 8 KB CHR RAM for PPU (needed before HLE boot so CHR files can load) */
        if (!nes_cart_init_chr_ram(&s->cart)) { free(s); return NULL; }

        /* BIOS: use real ROM if provided, otherwise install HLE stub */
        const char *bios_path = NULL;
        for (int i = 0; i < cfg->n_roms; i++) {
            if (cfg->roms[i].addr == 0xE000u) { bios_path = cfg->roms[i].path; break; }
        }
        if (!bios_path && cfg->n_roms > 0)
            bios_path = cfg->roms[0].path;

        if (bios_path) {
            if (!fds_bios_load(&s->fds, bios_path)) { nes_cart_free(&s->cart); free(s); return NULL; }
        } else {
            fds_hle_build_rom(&s->fds);
            s->fds.hle_mode = true;
        }

        /* the disk's save data and the HLE boot: the front end
         * (nes_fds_disk_inserted) */
        if (cfg->fda_path && !fds_disk_load(&s->fds, cfg->fda_path)) {
            nes_cart_free(&s->cart); free(s); return NULL;
        }
    } else {
        if (!cfg->cart_path) {
            if (cfg->is_arcade && cfg->n_roms > 0) {
                if (!nes_arcade_assemble(s, cfg)) { free(s); return NULL; }
            } else {
                fprintf(stderr, "nes: no cartridge specified - use -cartridge FILE.nes\n");
                free(s); return NULL;
            }
        } else {
            if (!nes_cart_insert(s, cfg->cart_path)) { free(s); return NULL; }
        }
    }

    nes_pins_init(&s->pins, cfg->cpu == MOS_CPU_2A07 ? 1662607u : 1789773u);
    s->clock_seen = s->pins.clock_hz;

    /* Wire up PPU CHR bus */
    rp2c02_init(&s->ppu, cfg->is_pal);
    if (cfg->is_dendy) s->ppu.vblank_line = 291;  /* Dendy: 50 extra post-render lines */
    if (cfg->vga == MOS_VGA_RP2C04_0004)
        s->ppu.no_odd_skip = true;
    if (cfg->is_arcade && s->vs_palette_rgb[0])    /* 2C04 palette present */
        s->ppu.alt_palette_rgb = s->vs_palette_rgb;
    s->ppu.chr_read  = nes_cart_chr_read;
    s->ppu.chr_write = nes_cart_chr_write;
    s->ppu.chr_ud    = &s->cart;
    s->ppu.mirror    = s->cart.info.mirror;

    mos6502_init(&s->cpu);
    s->cpu.mem_read        = nes_cpu_read;
    s->cpu.mem_write       = nes_cpu_write;
    s->cpu.mem_ud          = s;
    mos_cpu_apply(&s->cpu, cfg->cpu);
    s->cpu.dummy_cycles = true;   /* page-crossing and branch dummy reads hit registers */
    s->cpu.cycle_polling = true;  /* interrupts polled on each instruction's penultimate cycle */

    s->apu.sweep_enabled = true;
    s->apu.dmc_enabled = true;
    s->apu.frame_irq_enabled = true;

    /* APU - only initialise when sound is enabled */
    if (cfg->sound == MOS_SOUND_2A03) {
        uint32_t apu_clock = cfg->cpu == MOS_CPU_2A07 ? 1662607u : 1789773u;
        if (!apu2a03_init(&s->apu, apu_clock))
            fprintf(stderr, "nes: APU audio init failed (continuing silently)\n");
        s->apu.mem_read = nes_cpu_read;
        s->apu.mem_ud   = s;
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
    } else if (cfg->sound == MOS_SOUND_2A03_MIDI) {
        s->apu.mem_read    = nes_cpu_read;
        s->apu.mem_ud      = s;
        apu_midi_open(&s->apu_midi);
        s->apu.write_tap    = apu_midi_tap;
        s->apu.write_tap_ud = &s->apu_midi;
#endif
    }
    s->apu.dma_external = true;   /* the DMC's fetches are DMAs that halt the CPU */

    s->monitor = gemu_monitor_create();
    nes_monitor_setup(s);
    nes_install_mapper_hooks(s);
#ifdef HAVE_ROB
    for (int p = 0; p < cfg->n_ports; p++)
        if (nes_device_is_rob(cfg->ports[p])) { rob_init(&s->rob); break; }
#endif
    if (!nes_core_build(s)) {
        nes_core_destroy(s);
        return NULL;
    }
    return s;
}

void nes_core_destroy(NesState *s) {
    nes_core_free(s);
    if (s->fds_enabled) free(s->fds.raw_disk);
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
    apu_midi_close(&s->apu_midi);
#endif
    apu2a03_destroy(&s->apu);
    gemu_monitor_destroy(s->monitor);
    nes_cart_free(&s->cart);
    free(s);
}

/* A disk went in (or was there at power-on): the HLE BIOS loads its boot
 * files now. */
void nes_fds_disk_inserted(NesState *s) {
    if (s->fds.hle_mode)
        fds_hle_boot(&s->fds, s->cart.chr);
}

bool nes_insert_cartridge(NesState *s, const char *path) {
    if (!nes_cart_insert(s, path)) return false;
    nes_install_mapper_hooks(s);
    nes_maps_sync(s);
    return true;
}

void nes_eject_cartridge(NesState *s) {
    nes_cart_free(&s->cart);
    nes_maps_sync(s);
    s->cart_path_buf[0] = '\0';
}

void nes_apply_pins(NesState *s) { nes_pins_apply(s); }

/* Real time: wait until the host catches up with the machine's time (the
 * CPU's cycles at its clock); after a pause or a wait, start over. */
void nes_pace(NesState *s, bool hold) {
    if (hold) gemu_machine_pace_reset(s->core->machine);
    else gemu_machine_pace(s->core->machine, 0);
}

void nes_sync(NesState *s) {
    nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
    nes_sync_apu_to_cpu(s, s->cpu.cycle_count);
}

void nes_run_frame(NesState *s) {
    NesCore *c = s->core;
    s->ppu.dirty = false;
    c->stopped = false;
    /* a frame's worth at a time, until the PPU has finished one */
    uint64_t cycles = (uint64_t)s->ppu.lines_total * RP2C02_DOTS_PER_LINE / 3 + 1;
    while (!s->ppu.dirty && !c->stopped)
        gemu_machine_run_until(c->machine, gemu_machine_time(c->machine) +
                               gemu_clocks(s->pins.clock_hz, cycles));
    VS_DEBUG(s, "frame=%llu pc=%04X a=%02X x=%02X y=%02X p=%02X sp=%02X "
                "ctrl=%02X mask=%02X status=%02X chr=%05X/%05X nmi=%d irq=%d\n",
             (unsigned long long)s->ppu.frame, s->cpu.PC,
             s->cpu.A, s->cpu.X, s->cpu.Y, s->cpu.P, s->cpu.SP,
             s->ppu.ppuctrl, s->ppu.ppumask, s->ppu.ppustatus,
             s->cart.chr_off[0], s->cart.chr_off[4],
             s->cpu.nmi ? 1 : 0, s->cpu.irq ? 1 : 0);
    /* the frame's audio out to the host: the APU's stream, through the mixer */
    nes_sync_apu_to_cpu(s, s->cpu.cycle_count);
    gemu_mixer_run(s->core->machine);
}
