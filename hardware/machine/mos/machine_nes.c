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
#define VS_DIP_DEFAULT 0x00u

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

/* ── Cartridge connector (Debug > Cartridge Pins, nes_pins.h) ─────────────── */

void nes_update_irq(NesState *s) {
    bool apu = s->apu.fc_irq || s->apu.dmc.irq_flag;
    bool cart = s->cart_irq && !(nes_pins_sample(&s->pins) & NES_PIN_BIT(NP_IRQ));
    s->cpu.irq = apu || cart;
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
            if (s->apu.audio_dev && s->apu.frame_n > 900) apu2a03_flush(&s->apu);
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
    if (s->apu.audio_dev && s->apu.frame_n > 900) apu2a03_flush(&s->apu);
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

static void nes_sync_ppu_to_cpu(NesState *s, uint64_t cpu_cycle) {
    while (s->ppu_synced_cpu_cycle < cpu_cycle) {
        s->ppu_synced_cpu_cycle++;
        if (s->fds_enabled)
            { s->cart_irq = fds_tick(&s->fds); nes_update_irq(s); }
        for (int i = 0; i < 3; i++) {
            rp2c02_tick(&s->ppu);
            if (s->ppu.nmi_pending) {
                s->cpu.nmi = true;
                s->pins.tp_nmi = true;
                s->ppu.nmi_pending = false;
            }
        }
    }
}


static uint8_t nes_cpu_read_bus(uint16_t addr, void *ud) {
    NesState *s = ud;
    gemu_monitor_check_read(s->monitor, addr);

    if (addr < 0x2000) return s->ram[addr & 0x07FF];

    if (addr < 0x4000) {
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        return rp2c02_read(&s->ppu, (uint8_t)(addr & 7));
    }

    /* FDS I/O registers ($4020-$408F covers disk regs + wavetable + sound) */
    if (s->fds_enabled && addr >= 0x4020 && addr <= 0x408Fu) {
        uint8_t v = fds_reg_read(&s->fds, addr);
        s->cart_irq = fds_tick(&s->fds);  /* recalculate IRQ after flag clear */
        nes_update_irq(s);
        return v;
    }

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

    if (s->cfg->is_arcade && addr >= 0x4020 && addr < 0x6000) {
        VS_DEBUG(s, "read %04X -> %02X pc=%04X coin_counter\n",
                 addr, s->vs_coin_counter, s->cpu.PC);
        return s->vs_coin_counter;
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

    if (addr < 0x4020) return s->cpu_bus;

    /* FDS: RAM at $6000-$DFFF, the BIOS above */
    if (s->fds_enabled && addr >= 0x6000) {
        if (addr >= 0xE000) return s->fds.bios[addr - 0xE000u];
        return s->fds.ram[addr - 0x6000u];
    }

    if (addr < 0x8000) return nes_cart_read(&s->cart, addr, s->cpu_bus);

    if (!s->cart.prg) return s->cpu_bus;
    uint8_t rom = nes_cart_prg(&s->cart, addr);
    for (int i = 0; i < s->gg_count; i++) {
        if (s->gg_patches[i].addr == addr &&
            (!s->gg_patches[i].has_cmp || rom == s->gg_patches[i].cmp))
            return s->gg_patches[i].val;
    }
    return rom;
}

static void nes_cpu_write_bus(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;
    gemu_monitor_check_write(s->monitor, addr);

    if (addr < 0x2000) { s->ram[addr & 0x07FF] = val; return; }

    if (addr < 0x4000) {
        if (s->cfg->is_arcade) {
            uint8_t r = (uint8_t)(addr & 7);
            if (r == 0 || r == 1 || r == 5 || r == 6)
                VS_DEBUG(s, "write 200%u <- %02X pc=%04X frame=%llu sl=%d dot=%d\n",
                         r, val, s->cpu.PC,
                         (unsigned long long)s->ppu.frame,
                         s->ppu.scanline, s->ppu.dot);
        }
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        rp2c02_write(&s->ppu, (uint8_t)(addr & 7), val);
        return;
    }

    /* FDS I/O registers ($4020-$408A covers disk regs, wavetable and sound) */
    if (s->fds_enabled && addr >= 0x4020 && addr <= 0x408Au) {
        fds_reg_write(&s->fds, addr, val);
        /* $4025 bit 3 controls mirroring (0=vertical, 1=horizontal) */
        if (addr == 0x4025)
            s->ppu.mirror = (val & 0x08) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
        s->cart_irq = fds_tick(&s->fds);
        nes_update_irq(s);
        return;
    }

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
        return;
    }

    if (s->cfg->is_arcade && addr >= 0x4020 && addr < 0x6000) {
        s->vs_coin_counter = val;
        VS_DEBUG(s, "write %04X <- %02X pc=%04X coin_counter\n", addr, val, s->cpu.PC);
        return;
    }

    /* APU registers */
    if ((addr >= 0x4000 && addr <= 0x4013) || addr == 0x4015 || addr == 0x4017) {
        nes_sync_apu_to_cpu(s, s->cpu.cycle_count - 1);
        apu2a03_write(&s->apu, addr, val);
        nes_dmc_dma_check(s);
        s->apu_deadline = 0;   /* the write may bring the next event closer */
        /* $4010/$4017 can clear APU IRQ flags */
        nes_update_irq(s);
        return;
    }

    if (addr < 0x4020) return;

    /* FDS: $6000-$DFFF is RAM */
    if (s->fds_enabled && addr >= 0x6000) {
        if (addr < 0xE000) s->fds.ram[addr - 0x6000u] = val;
        return;
    }

    nes_cart_write(&s->cart, addr, val);
}

/* ── Cartridge media device ──────────────────────────────────────────────── */

/* PPU hooks for the cartridge's mapper: scanline IRQ counters and
 * nametable mapping.  Called again when the cartridge changes. */
static void nes_install_mapper_hooks(NesState *s) {
    NesCart *c = &s->cart;
    s->ppu.irq_scanline = nes_cart_wants_scanline(c) ? nes_cart_scanline : NULL;
    s->ppu.nt_read      = nes_cart_wants_nt(c) ? nes_cart_nt_read : NULL;
    s->ppu.nt_write     = nes_cart_wants_nt(c) ? nes_cart_nt_write : NULL;
    s->ppu.irq_ud = s->ppu.nt_ud = c;
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
        return;
    }
    s->ppu.mirror = s->cart.info.mirror;
    s->ppu_synced_cpu_cycle = s->apu_synced_cpu_cycle = s->cpu.cycle_count;
    s->apu_deadline = 0;

    /* The board's banks before mos6502_reset reads the reset vector */
    nes_cart_reset(&s->cart);
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

#define NES_SNAPSHOT_MAGIC    0x314D5645534d454eULL /* "NEMSVE1" */
#define NES_SNAPSHOT_VERSION  5u   /* 2: board state; 3: dot-by-dot sprite evaluation;
                                       4: the cartridge as a NesCart;
                                       5: the front end's fields out of NesState */

/* Snapshots are built in memory (SnapBuf); the front end's savevm/loadvm
 * write and read them as files, and netplay keeps them there. */
typedef struct { uint8_t *p; size_t n, cap; bool ok; } SnapBuf;

static void sb_put(SnapBuf *b, const void *d, size_t n) {
    if (!b->ok) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 65536;
        while (cap < b->n + n) cap *= 2;
        uint8_t *p = realloc(b->p, cap);
        if (!p) { b->ok = false; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

typedef struct { const uint8_t *p; size_t n, i; } SnapRd;

static bool sr_get(SnapRd *r, void *d, size_t n) {
    if (r->n - r->i < n) return false;
    memcpy(d, r->p + r->i, n);
    r->i += n;
    return true;
}

static bool snapshot_save(NesState *s, SnapBuf *b) {
    if (s->fds_enabled) {
        fprintf(stderr, "nes: savevm not supported for FDS yet\n");
        return false;
    }
    *b = (SnapBuf){ .ok = true };
    uint64_t magic   = NES_SNAPSHOT_MAGIC;
    uint32_t version = NES_SNAPSHOT_VERSION;
    sb_put(b, &magic, sizeof(magic));
    sb_put(b, &version, sizeof(version));
    sb_put(b, &s->cart.info, sizeof(s->cart.info));

    size_t chr_ram_len;
    uint8_t *chr_ram = nes_cart_chr_ram(&s->cart, &chr_ram_len);
    uint64_t chr_len = chr_ram_len;
    sb_put(b, &chr_len, sizeof(chr_len));
    if (chr_len) sb_put(b, chr_ram, chr_len);

    NesState *snap = malloc(sizeof(*snap));
    if (!snap) { free(b->p); return false; }
    *snap = *s;
    snap->cfg     = NULL;
    nes_cart_strip(&snap->cart);
    snap->monitor = NULL;
    snap->exec_hook = NULL; snap->exec_hook_ud = NULL;
    snap->cpu.mem_read  = NULL; snap->cpu.mem_write  = NULL; snap->cpu.mem_ud  = NULL;
    snap->ppu.chr_read  = NULL; snap->ppu.chr_write  = NULL; snap->ppu.chr_ud  = NULL;
    snap->ppu.nt_read   = NULL; snap->ppu.nt_write   = NULL; snap->ppu.nt_ud   = NULL;
    snap->ppu.irq_scanline  = NULL; snap->ppu.irq_ud = NULL;
    snap->ppu.alt_palette_rgb = NULL;
    snap->apu.mem_read     = NULL; snap->apu.mem_ud       = NULL;
    snap->apu.write_tap    = NULL; snap->apu.write_tap_ud = NULL;
    snap->apu.audio_dev    = NULL;
    snap->fds.raw_disk = NULL;
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
    snap->apu_midi.out = NULL;
#endif
    sb_put(b, snap, sizeof(*snap));
    free(snap);
    if (!b->ok) { free(b->p); b->p = NULL; }
    return b->ok;
}

bool nes_snapshot_load(NesState *s, const uint8_t *data, size_t len) {
    if (s->fds_enabled) {
        fprintf(stderr, "nes: loadvm not supported for FDS yet\n");
        return false;
    }
    SnapRd r = { data, len, 0 };
    bool ok = true;
    uint64_t magic   = 0;
    uint32_t version = 0;
    NesCartInfo cart  = {0};
    ok &= sr_get(&r, &magic, sizeof(magic));
    ok &= sr_get(&r, &version, sizeof(version));
    ok &= sr_get(&r, &cart, sizeof(cart));
    if (!ok || magic != NES_SNAPSHOT_MAGIC || version != NES_SNAPSHOT_VERSION ||
        cart.mapper != s->cart.info.mapper || cart.prg_banks != s->cart.info.prg_banks ||
        cart.chr_banks != s->cart.info.chr_banks) {
        fprintf(stderr, "nes: loadvm: snapshot doesn't match the loaded cartridge\n");
        return false;
    }

    uint64_t chr_len = 0;
    size_t want_len;
    uint8_t *chr_ram = nes_cart_chr_ram(&s->cart, &want_len);
    ok &= sr_get(&r, &chr_len, sizeof(chr_len));
    if (!ok || chr_len != want_len) {
        fprintf(stderr, "nes: loadvm: CHR RAM size mismatch\n");
        return false;
    }
    if (chr_len && !sr_get(&r, chr_ram, chr_len)) return false;

    NesState *loaded = malloc(sizeof(*loaded));
    if (!loaded) return false;
    if (!sr_get(&r, loaded, sizeof(*loaded))) { free(loaded); return false; }
    const MosConfig *cfg     = s->cfg;
    NesCart          *live_cart = malloc(sizeof(*live_cart));   /* its wiring */
    if (!live_cart) { free(loaded); return false; }
    *live_cart = s->cart;
    GemuMonitor      *monitor = s->monitor;
    void (*exec_hook)(void *, uint16_t) = s->exec_hook;
    void             *exec_hook_ud = s->exec_hook_ud;
    void *cpu_mem_ud  = s->cpu.mem_ud;
    void *ppu_chr_ud  = s->ppu.chr_ud;
    void *ppu_nt_ud   = s->ppu.nt_ud;
    void *ppu_irq_ud  = s->ppu.irq_ud;
    const uint32_t *ppu_alt_palette = s->ppu.alt_palette_rgb;
    void *apu_mem_ud       = s->apu.mem_ud;
    void *apu_write_tap_ud = s->apu.write_tap_ud;
    GemuAudioOut *audio_dev = s->apu.audio_dev;
    uint8_t *fds_raw_disk = s->fds.raw_disk;
    typeof(s->cpu.mem_read)   cpu_mem_read   = s->cpu.mem_read;
    typeof(s->cpu.mem_write)  cpu_mem_write  = s->cpu.mem_write;
    typeof(s->ppu.chr_read)   ppu_chr_read   = s->ppu.chr_read;
    typeof(s->ppu.chr_write)  ppu_chr_write  = s->ppu.chr_write;
    typeof(s->ppu.nt_read)    ppu_nt_read    = s->ppu.nt_read;
    typeof(s->ppu.nt_write)   ppu_nt_write   = s->ppu.nt_write;
    typeof(s->ppu.irq_scanline) ppu_irq_scanline = s->ppu.irq_scanline;
    typeof(s->apu.mem_read)   apu_mem_read   = s->apu.mem_read;
    typeof(s->apu.write_tap)  apu_write_tap  = s->apu.write_tap;
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
    MidiOut *apu_midi_out = s->apu_midi.out;
#endif
    NesPins pins = s->pins;       /* a debugging setting, not machine state */
    *s = *loaded;
    free(loaded);
    s->pins = pins;
    s->pins_seen = pins.version - 1u;   /* reapply: the hook pointers came from the file */
    s->apu_deadline = 0;
    s->clock_seen = 0;

    s->cfg     = cfg;
    nes_cart_rewire(&s->cart, live_cart);
    free(live_cart);
    s->monitor = monitor;
    s->exec_hook = exec_hook;
    s->exec_hook_ud = exec_hook_ud;
    s->cpu.mem_read  = cpu_mem_read;  s->cpu.mem_write = cpu_mem_write; s->cpu.mem_ud = cpu_mem_ud;
    s->ppu.chr_read  = ppu_chr_read;  s->ppu.chr_write = ppu_chr_write; s->ppu.chr_ud = ppu_chr_ud;
    s->ppu.nt_read   = ppu_nt_read;   s->ppu.nt_write  = ppu_nt_write;  s->ppu.nt_ud  = ppu_nt_ud;
    s->ppu.irq_scanline = ppu_irq_scanline; s->ppu.irq_ud = ppu_irq_ud;
    s->ppu.alt_palette_rgb = ppu_alt_palette;
    s->apu.mem_read = apu_mem_read; s->apu.mem_ud = apu_mem_ud;
    s->apu.write_tap = apu_write_tap; s->apu.write_tap_ud = apu_write_tap_ud;
    s->apu.audio_dev = audio_dev;
    s->fds.raw_disk = fds_raw_disk;
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
    s->apu_midi.out = apu_midi_out;
#endif
    return true;
}

bool nes_snapshot_save(NesState *s, uint8_t **data, size_t *len) {
    SnapBuf b;
    if (!snapshot_save(s, &b)) return false;
    *data = b.p;
    *len = b.n;
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
    if (cfg->disabled_features & MOS_FEATURE_ODD_FRAME_SKIP)
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
    s->apu.sweep_enabled = (cfg->disabled_features & MOS_FEATURE_SWEEP_REGISTERS) == 0;
    s->apu.dmc_enabled = (cfg->disabled_features & MOS_FEATURE_DMC_CHANNEL) == 0;
    s->apu.dma_external = true;   /* the DMC's fetches are DMAs that halt the CPU */
    s->apu.frame_irq_enabled = (cfg->disabled_features & MOS_FEATURE_FRAME_IRQ) == 0;

    s->monitor = gemu_monitor_create();
    nes_monitor_setup(s);
    nes_install_mapper_hooks(s);
#ifdef HAVE_ROB
    for (int p = 0; p < cfg->n_ports; p++)
        if (nes_device_is_rob(cfg->ports[p])) { rob_init(&s->rob); break; }
#endif
    if (cfg->is_arcade)
        s->vs_dip = VS_DIP_DEFAULT;
    return s;
}

void nes_core_destroy(NesState *s) {
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
    return true;
}

void nes_eject_cartridge(NesState *s) {
    nes_cart_free(&s->cart);
    s->cart_path_buf[0] = '\0';
}

void nes_apply_pins(NesState *s) { nes_pins_apply(s); }

void nes_sync(NesState *s) {
    nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
    nes_sync_apu_to_cpu(s, s->cpu.cycle_count);
}

void nes_run_frame(NesState *s) {
    s->ppu.dirty = false;
    while (!s->ppu.dirty) {
        if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) break;
        nes_step_insn(s);
        if (gemu_monitor_is_paused(s->monitor)) break;
    }
    VS_DEBUG(s, "frame=%llu pc=%04X a=%02X x=%02X y=%02X p=%02X sp=%02X "
                "ctrl=%02X mask=%02X status=%02X chr=%05X/%05X nmi=%d irq=%d\n",
             (unsigned long long)s->ppu.frame, s->cpu.PC,
             s->cpu.A, s->cpu.X, s->cpu.Y, s->cpu.P, s->cpu.SP,
             s->ppu.ppuctrl, s->ppu.ppumask, s->ppu.ppustatus,
             s->cart.chr_off[0], s->cart.chr_off[4],
             s->cpu.nmi ? 1 : 0, s->cpu.irq ? 1 : 0);
    /* the frame's audio out to the host */
    nes_sync_apu_to_cpu(s, s->cpu.cycle_count);
    apu2a03_flush(&s->apu);
}
