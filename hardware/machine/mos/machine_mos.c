#include "machine_mos.h"
#include "gemu/dev/space_cache.h"
#include "mos6502_dev.h"
#include "gemu/memory.h"
#include "gemu/dev/device.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/space.h"
#include "gemu/dev/state.h"
#include <inttypes.h>
#include <SDL2/SDL.h>
#ifndef _WIN32
#  include <sys/select.h>
#  include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GENERIC_HZ 1000000u   /* the CPU's clock */
#define GENERIC_FRAME_HZ 60u  /* host frames: serial polled, monitor served */

/* ── IRQ aggregator ──────────────────────────────────────────────────────── */

static void generic_irq_line(MosGenericState *s, bool level);
void mos_generic_destroy(MosGenericState *s);
static void generic_update_irq(MosGenericState *s) {
    generic_irq_line(s,
        (s->timer_pending  && (s->timer_ctrl  & 0x02u)) ||
        (s->serial_rxq_cnt && (s->serial_ctrl & 0x01u)));
}

/* ── Timer ───────────────────────────────────────────────────────────────── */

static uint32_t timer_period(MosGenericState *s) {
    return s->timer_reload ? (uint32_t)s->timer_reload : 0x10000u;
}

/* Call once per instruction in the run loop. */
static void generic_timer_tick(MosGenericState *s) {
    if (!(s->timer_ctrl & 0x01u)) return;
    if (s->cpu->cycle_count < s->timer_next_fire) return;

    s->timer_pending = true;

    if (s->timer_ctrl & 0x04u)        /* auto-reload: advance from last fire */
        s->timer_next_fire += timer_period(s);
    else
        s->timer_ctrl &= ~0x01u;      /* one-shot: disable */

    generic_update_irq(s);
}

/* ── Serial ──────────────────────────────────────────────────────────────── */

/* Poll stdin (non-blocking) and fill the RX FIFO. Call once per frame. */
static void generic_poll_serial(MosGenericState *s) {
#ifndef _WIN32
    while (s->serial_rxq_cnt < 16u) {
        struct timeval tv = {0, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) break;

        uint8_t b;
        if (read(STDIN_FILENO, &b, 1) != 1) break;

        s->serial_rxq[s->serial_rxq_w] = b;
        s->serial_rxq_w = (s->serial_rxq_w + 1u) & 15u;
        s->serial_rxq_cnt++;
    }
    if (s->serial_rxq_cnt) generic_update_irq(s);
#else
    /* Non-blocking stdin polling via select() isn't available on Windows;
     * serial RX is a no-op there (TX to stdout still works). */
    (void)s;
#endif
}

/* ── Memory callbacks ────────────────────────────────────────────────────── */

/* The I/O page ($DF00-$DFFF) as a handler: the timer and the serial port. */
static uint8_t io_r8(void *ud, uint64_t off) {
    MosGenericState *s = ud;
    switch (off) {
    case 0x00u:  /* $DF00 timer reload lo */
        return (uint8_t)(s->timer_reload);
    case 0x01u:  /* $DF01 timer reload hi */
        return (uint8_t)(s->timer_reload >> 8);
    case 0x02u:  /* $DF02 timer control + pending */
        return s->timer_ctrl | (s->timer_pending ? 0x80u : 0u);
    case 0x10u:  /* $DF10 serial RX data */
        if (s->serial_rxq_cnt) {
            uint8_t b = s->serial_rxq[s->serial_rxq_r];
            s->serial_rxq_r   = (s->serial_rxq_r + 1u) & 15u;
            s->serial_rxq_cnt--;
            generic_update_irq(s);
            return b;
        }
        return 0;
    case 0x11u:  /* $DF11 serial status */
        return 0x02u | (s->serial_rxq_cnt ? 0x01u : 0u);  /* TX always ready */
    case 0x12u:  /* $DF12 serial control */
        return s->serial_ctrl;
    }
    return 0;
}

/* Monitor memory view: RAM/ROM directly; in the $DFxx I/O page only the
 * side-effect-free timer/serial status registers ($DF10 pops the RX FIFO). */
static bool generic_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    MosGenericState *s = ud;
    if (addr > 0xFFFF) return false;
    if (addr >= GENERIC_IO_BASE && addr <= GENERIC_IO_END) {
        switch (addr - GENERIC_IO_BASE) {
        case 0x00u: *val = (uint8_t)(s->timer_reload);      return true;
        case 0x01u: *val = (uint8_t)(s->timer_reload >> 8); return true;
        case 0x02u: *val = s->timer_ctrl | (s->timer_pending ? 0x80u : 0u); return true;
        case 0x11u: *val = 0x02u | (s->serial_rxq_cnt ? 0x01u : 0u); return true;
        case 0x12u: *val = s->serial_ctrl; return true;
        }
        return false;
    }
    *val = s->mem[addr % s->mem_size];
    return true;
}

/* Writes go to RAM only (not ROM, the I/O page or the $F001 debug port). */
static bool generic_mon_write(void *ud, uint64_t addr, uint8_t val) {
    MosGenericState *s = ud;
    if (addr > 0xFFFF || addr == 0xF001u) return false;
    if (addr >= GENERIC_IO_BASE && addr <= GENERIC_IO_END) return false;
    uint32_t off = (uint32_t)(addr % s->mem_size);
    if (s->rom_map[off]) return false;
    s->mem[off] = val;
    return true;
}

static void generic_mon_cpu_state(void *ud, char *buf, size_t len) {
    const MosGenericState *s = ud;
    mos6502_format_state(s->cpu, buf, len);
}

static void io_w8(void *ud, uint64_t off, uint8_t val) {
    MosGenericState *s = ud;
    switch (off) {
    case 0x00u:  /* $DF00 timer reload lo */
        s->timer_reload = (s->timer_reload & 0xFF00u) | val;
        break;
    case 0x01u:  /* $DF01 timer reload hi */
        s->timer_reload = (s->timer_reload & 0x00FFu) | ((uint16_t)val << 8);
        break;
    case 0x02u: {  /* $DF02 timer control */
        bool was_enabled = (s->timer_ctrl & 0x01u) != 0;
        bool now_enabled = (val           & 0x01u) != 0;
        if (val & 0x80u)                 /* write 1 to bit7 = clear pending */
            s->timer_pending = false;
        s->timer_ctrl = val & 0x7Fu;     /* bit7 is status-only, not stored */
        if (!was_enabled && now_enabled)  /* rising enable edge: arm timer */
            s->timer_next_fire = s->cpu->cycle_count + timer_period(s);
        generic_update_irq(s);
        break;
    }
    case 0x10u:  /* $DF10 serial TX data */
        putchar(val);
        fflush(stdout);
        break;
    case 0x12u:  /* $DF12 serial control */
        s->serial_ctrl = val;
        generic_update_irq(s);
        break;
    /* $DF11 serial status is read-only; $DF13-$DFFF reserved */
    }
}

/* Legacy debug port: a write to $F001 goes to stdout (test ROMs); reads
 * are the memory under it. */
static void debug_w8(void *ud, uint64_t off, uint8_t val) {
    (void)ud; (void)off;
    putchar(val);
    fflush(stdout);
}
/* ROM: writes dropped, reads the memory under it. */
static void rom_w8(void *ud, uint64_t off, uint8_t val) { (void)ud; (void)off; (void)val; }

static const GemuHandlers io_handlers    = { .r8 = io_r8, .w8 = io_w8 };
static const GemuHandlers debug_handlers = { .w8 = debug_w8 };
static const GemuHandlers rom_handlers   = { .w8 = rom_w8 };

/* ── The core machine (stage M2) ─────────────────────────────────────────────
 * board (mos-generic-board): the memory, the timer and the serial port;
 * cpu (mos6502): the CPU and the machine's executor at 1 MHz.
 * The bus, "program" (16 bits): the memory repeated every mem_size bytes
 * (an address decodes as addr % mem_size); over it, at priority 1, a
 * write-dropping handler on each ROM (the reads are the memory), and at
 * priority 2 the I/O page and the debug port.  The timer's and the serial
 * port's interrupt is the board's "irq" line to the CPU's. */
typedef struct MosGenericCore {
    GemuMachine *machine;
    GemuDevice  *board, *cpu;
    GemuSpace   *bus;
    GemuSpaceCache       cache;
    GemuLine    *irq;
} MosGenericCore;

static void generic_irq_line(MosGenericState *s, bool level) {
    gemu_line_set(s->core->irq, level);
}

static uint8_t generic_mem_read(uint16_t addr, void *ud) {
    MosGenericState *s = ud;
    gemu_monitor_check_read(s->monitor, addr);
    return gemu_space_cache_read8(&s->core->cache, addr);
}

static void generic_mem_write(uint16_t addr, uint8_t val, void *ud) {
    MosGenericState *s = ud;
    gemu_monitor_check_write(s->monitor, addr);
    gemu_space_cache_write8(&s->core->cache, addr, val);
}

static void generic_board_describe(GemuDevice *d, char *buf, size_t len) {
    const MosGenericState *s = *(MosGenericState **)d->priv;
    snprintf(buf, len, "%u KB memory\n"
             "timer: reload %04X, control %02X%s, next at cycle %" PRIu64 "\n"
             "serial: control %02X, %u byte(s) waiting",
             (unsigned)(s->mem_size / 1024), s->timer_reload, s->timer_ctrl,
             s->timer_pending ? ", IRQ pending" : "", s->timer_next_fire,
             s->serial_ctrl, s->serial_rxq_cnt);
}

static const GemuDeviceType generic_board_type = {
    .name = "mos-generic-board", .desc = "Generic 6502 board (memory, timer, serial port)",
    .size = sizeof(MosGenericState *), .describe = generic_board_describe,
};

#define B(field) ok &= GEMU_STATE_MEMBER(d, "", s, field)
/* The board's state: the memory (ROM included: it is in the same array),
 * the timer and the serial port.  Not saved: rom_map (from the ROMs). */
static bool generic_board_state(MosGenericState *s, GemuDevice *d) {
    bool ok = gemu_state_add(d, "mem", s->mem, 1, s->mem_size);
    B(timer_reload); B(timer_ctrl); B(timer_pending); B(timer_next_fire);
    B(serial_ctrl); B(serial_rxq); B(serial_rxq_r); B(serial_rxq_w); B(serial_rxq_cnt);
    return ok;
}
#undef B

static bool generic_bus_build(MosGenericState *s) {
    MosGenericCore *c = s->core;
    if (!(c->bus = gemu_space_new(c->machine, "program", 16, 8, GEMU_LITTLE_ENDIAN)))
        return false;
    c->cache.space = c->bus;
    for (uint32_t base = 0; base < 0x10000u; base += s->mem_size) {
        uint32_t n = 0x10000u - base < s->mem_size ? 0x10000u - base : s->mem_size;
        GemuMapping *mp = gemu_space_map_ram(c->bus, base, n, s->mem, 0, 0);
        if (!mp) return false;
        gemu_mapping_set_name(mp, "memory");
    }
    return gemu_space_map(c->bus, GENERIC_IO_BASE, 0x100, &io_handlers, s, 2, 0) &&
           gemu_space_map(c->bus, 0xF001, 1, &debug_handlers, s, 2, 0);
}

/* The ROMs' write protection: a handler that drops writes over each ROM,
 * wherever the memory repeats.  After loading them (create, reset). */
static void generic_rom_maps(MosGenericState *s) {
    MosGenericCore *c = s->core;
    if (!c || !c->bus) return;
    for (GemuMapping *mp = gemu_space_mappings(c->bus), *next; mp; mp = next) {
        next = mp->next;
        if (mp->ud == s && mp->h.w8 == rom_w8) gemu_mapping_remove(mp);
    }
    for (uint32_t off = 0; off < s->mem_size; ) {
        if (!s->rom_map[off]) { off++; continue; }
        uint32_t end = off;
        while (end < s->mem_size && s->rom_map[end]) end++;
        for (uint32_t base = 0; base + off < 0x10000u; base += s->mem_size) {
            uint32_t n = end - off;
            if (base + off + n > 0x10000u) n = 0x10000u - (base + off);
            gemu_space_map(c->bus, base + off, n, &rom_handlers, s, 1, 0);
        }
        off = end;
    }
}

static bool generic_stop_before(void *ud) {
    MosGenericState *s = ud;
    return gemu_monitor_check_exec(s->monitor, s->cpu->PC);
}
static void generic_insn(void *ud) {
    MosGenericState *s = ud;
    mos6502_step(s->cpu);
    generic_timer_tick(s);
}
static bool generic_stop_after(void *ud) {
    return gemu_monitor_is_paused(((MosGenericState *)ud)->monitor);
}

static bool generic_core_build(MosGenericState *s) {
    MosGenericCore *c = s->core = calloc(1, sizeof *c);
    if (!c || !(c->machine = gemu_machine_new("mos")))
        return false;
    static const GemuDeviceType *const set_types[] = { &generic_board_type };
    char set[1][GEMU_OPTS_ROUTE_LEN], why[512];
    if (!gemu_opts_route(s->cfg->machine_opts, set_types, 1, set, why, sizeof why)) {
        fprintf(stderr, "gemu: %s\n", why);
        return false;
    }
    static const char *const variant[] = { "6502", "2a03", "2a07", "6501", "6507" };
    char cpu_opts[32];
    snprintf(cpu_opts, sizeof cpu_opts, "variant=%s", variant[s->cfg->cpu]);
    bool ok = (c->board = gemu_device_add(c->machine, &generic_board_type, "board", NULL, NULL)) &&
              (c->cpu = gemu_device_add(c->machine, &gemu_mos6502_type, "cpu", NULL, cpu_opts));
    if (ok) {
        *(MosGenericState **)c->board->priv = s;
        ok = generic_board_state(s, c->board);
    }
    if (!ok) {
        fprintf(stderr, "gemu-6502: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    bool inited = gemu_machine_init(c->machine);
    if (inited) {
        s->cpu = gemu_mos6502(c->cpu);
        s->cpu->mem_read  = generic_mem_read;
        s->cpu->mem_write = generic_mem_write;
        s->cpu->mem_ud    = s;
    }
    static const GemuMos6502Hooks hooks = { .stop_before = generic_stop_before, .insn = generic_insn,
                                            .stop_after = generic_stop_after };
    GemuMos6502Hooks h = hooks;
    h.ud = s;
    ok = inited && generic_bus_build(s) &&
         (c->irq = gemu_line_out(c->board, "irq")) && gemu_line_connect(c->irq, c->cpu, "irq", 0) &&
         gemu_mos6502_exec(c->cpu, GENERIC_HZ, &h);
    if (!ok || !gemu_machine_start(c->machine)) {
        fprintf(stderr, "gemu-6502: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    gemu_monitor_attach_machine(s->monitor, c->machine, "program");
    return true;
}

static void generic_core_free(MosGenericState *s) {
    if (!s->core) return;
    gemu_machine_free(s->core->machine);
    free(s->core);
    s->core = NULL;
}

/* ── ROM loading ─────────────────────────────────────────────────────────── */

static bool load_roms(MosGenericState *s, const MosConfig *cfg) {
    for (int i = 0; i < cfg->n_roms; i++) {
        uint32_t off = (cfg->roms[i].addr & 0xFFFFu) % s->mem_size;
        GemuMemory tmp = {.data = s->mem + off, .size = s->mem_size - off};
        size_t len = 0;
        if (!gemu_mem_load_file(&tmp, 0, cfg->roms[i].path, &len)) {
            fprintf(stderr, "gemu-6502: failed to load '%s'\n", cfg->roms[i].path);
            return false;
        }
        memset(s->rom_map + off, 1, len);
        printf("gemu-6502: %zu bytes @ 0x%04X  ← %s\n",
               len, (unsigned)off, cfg->roms[i].path);
        gemu_monitor_register_rom(s->monitor, off, (uint32_t)len, cfg->roms[i].path);
    }
    return true;
}

/* ── Machine lifecycle ───────────────────────────────────────────────────── */

MosGenericState *mos_generic_create(const MosConfig *cfg) {
    MosGenericState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg      = cfg;
    s->mem_size = cfg->mem_size ? cfg->mem_size : 0x10000u;

    s->mem     = calloc(1, s->mem_size);
    s->rom_map = calloc(1, s->mem_size);
    if (!s->mem || !s->rom_map) {
        free(s->mem); free(s->rom_map); free(s);
        return NULL;
    }

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = generic_mon_read, .write = generic_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = false });
    gemu_monitor_set_cpu_state_cb(s->monitor, generic_mon_cpu_state, s);

    if (!generic_core_build(s) || !load_roms(s, cfg)) {
        mos_generic_destroy(s);
        return NULL;
    }
    generic_rom_maps(s);

    mos6502_reset(s->cpu);

    if (cfg->has_start_addr)
        s->cpu->PC = cfg->start_addr;

    return s;
}

static void mos_generic_reset(MosGenericState *s, const MosConfig *cfg) {
    gemu_monitor_clear_roms(s->monitor);
    memset(s->rom_map, 0, s->mem_size);
    load_roms(s, cfg);
    generic_rom_maps(s);

    s->timer_ctrl    = 0;
    s->timer_pending = false;
    s->serial_ctrl   = 0;
    s->serial_rxq_r  = 0;
    s->serial_rxq_w  = 0;
    s->serial_rxq_cnt = 0;
    generic_update_irq(s);

    mos6502_reset(s->cpu);
    if (cfg->has_start_addr)
        s->cpu->PC = cfg->start_addr;
}

void mos_generic_destroy(MosGenericState *s) {
    generic_core_free(s);
    gemu_monitor_destroy(s->monitor);
    free(s->mem);
    free(s->rom_map);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── *
 * Per host frame (1/60 s): the serial port polled, the monitor served, the
 * machine run for 1/60 s of its time (the cpu device's executor), then
 * paced to real time.  The monitor's step runs instructions directly. */

void mos_generic_run(MosGenericState *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);
    GemuMachine *m = s->core->machine;

    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)   { if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true); else { quit = true; break; } }
            else if (cmd == GEMU_MON_RESET)  mos_generic_reset(s, cfg);
            else if (cmd == GEMU_MON_STEP) {
                for (uint32_t n = gemu_monitor_step_count(s->monitor); n; n--)
                    generic_insn(s);
            }
        }
        if (quit) break;

        if (!gemu_monitor_is_paused(s->monitor)) {
            generic_poll_serial(s);
            gemu_machine_run_until(m, gemu_machine_time(m) + GEMU_SEC(1) / GENERIC_FRAME_HZ);
            gemu_machine_pace(m, 0);
        } else {
            gemu_machine_pace_reset(m);
            SDL_Delay(1000u / GENERIC_FRAME_HZ);
        }
    }

    printf("gemu-6502: %llu cycles, %llu instructions\n",
           (unsigned long long)s->cpu->cycle_count,
           (unsigned long long)s->cpu->insn_count);

    gemu_monitor_stop(s->monitor);
    (void)cfg;
}
