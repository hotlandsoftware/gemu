#include "machine_z80.h"
#include "z80_dev.h"
#include "gemu/dev/device.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/space.h"
#include "gemu/dev/state.h"
#include "gemu/memory.h"
#include <SDL2/SDL.h>
#ifndef _WIN32
#  include <sys/select.h>
#  include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Interrupts and timer ────────────────────────────────────────────────── */

static void generic_update_int(Z80GenericState *s) {
    s->cpu->int_line =
        (s->timer_pending  && (s->timer_ctrl  & 0x02u)) ||
        (s->serial_rxq_cnt && (s->serial_ctrl & 0x01u));
}

static uint32_t timer_period(const Z80GenericState *s) {
    return s->timer_reload ? (uint32_t)s->timer_reload : 0x10000u;
}

static void timer_arm(Z80GenericState *s);
static void timer_disarm(Z80GenericState *s);

/* The timer's expiry (a core timer, one period at a time, so a reload or
 * auto-reload written meanwhile counts from the next expiry, as before). */
static void generic_timer_fire(void *ud, int param) {
    Z80GenericState *s = ud;
    (void)param;
    if (!(s->timer_ctrl & 0x01u)) return;
    s->timer_pending = true;
    if (s->timer_ctrl & 0x04u)
        timer_arm(s);
    else
        s->timer_ctrl &= ~0x01u;
    generic_update_int(s);
}

/* ── Serial ──────────────────────────────────────────────────────────────── */

static void generic_poll_serial(Z80GenericState *s) {
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
    if (s->serial_rxq_cnt) generic_update_int(s);
#else
    (void)s;   /* no non-blocking stdin on Windows: TX only */
#endif
}

/* ── Bus callbacks ───────────────────────────────────────────────────────── */

static void generic_end_run(Z80GenericState *s, bool done);

/* A watchpoint hit pauses the monitor and ends the run after the
 * instruction (as HEAD's loop stopped once the monitor was paused). */
static uint8_t generic_read(void *ud, uint16_t addr) {
    Z80GenericState *s = ud;
    if (gemu_monitor_check_read(s->monitor, addr)) generic_end_run(s, false);
    return s->mem[addr];
}

static void generic_write(void *ud, uint16_t addr, uint8_t val) {
    Z80GenericState *s = ud;
    if (gemu_monitor_check_write(s->monitor, addr)) generic_end_run(s, false);
    if (!s->rom_map[addr]) s->mem[addr] = val;
}

static uint8_t generic_in(void *ud, uint16_t port) {
    Z80GenericState *s = ud;
    switch (port & 0xFFu) {
    case 0x00: return (uint8_t)s->timer_reload;
    case 0x01: return (uint8_t)(s->timer_reload >> 8);
    case 0x02: return s->timer_ctrl | (s->timer_pending ? 0x80u : 0u);
    case 0x10:
        if (s->serial_rxq_cnt) {
            uint8_t b = s->serial_rxq[s->serial_rxq_r];
            s->serial_rxq_r = (s->serial_rxq_r + 1u) & 15u;
            s->serial_rxq_cnt--;
            generic_update_int(s);
            return b;
        }
        return 0;
    case 0x11: return 0x02u | (s->serial_rxq_cnt ? 0x01u : 0u);
    case 0x12: return s->serial_ctrl;
    default:   return 0xFF;
    }
}

/* -cpm: the BDOS functions a test program needs. */
static void bdos_call(Z80GenericState *s) {
    Z80 *c = s->cpu;
    switch (c->c) {
    case 0: s->exit_requested = true; generic_end_run(s, true); break;
    case 2: putchar(c->e); break;
    case 9:
        for (uint16_t a = (uint16_t)(c->d << 8 | c->e), n = 0; s->mem[a] != '$' && n < 0xFFFF; a++, n++)
            putchar(s->mem[a]);
        break;
    default:
        fprintf(stderr, "gemu-z80: BDOS function %u not supported\n", c->c);
        break;
    }
    fflush(stdout);
}

static void generic_out(void *ud, uint16_t port, uint8_t val) {
    Z80GenericState *s = ud;
    switch (port & 0xFFu) {
    case 0x00: s->timer_reload = (uint16_t)((s->timer_reload & 0xFF00u) | val); break;
    case 0x01: s->timer_reload = (uint16_t)((s->timer_reload & 0x00FFu) | (val << 8)); break;
    case 0x02: {
        bool was_enabled = (s->timer_ctrl & 0x01u) != 0;
        if (val & 0x80u) s->timer_pending = false;
        s->timer_ctrl = val & 0x7Fu;
        if (!was_enabled && (val & 0x01u)) timer_arm(s);
        else if (!(val & 0x01u)) timer_disarm(s);
        generic_update_int(s);
        break;
    }
    case 0x10: putchar(val); fflush(stdout); break;
    case 0x12: s->serial_ctrl = val; generic_update_int(s); break;
    case Z80GEN_PORT_BDOS: if (s->cfg->cpm_path) bdos_call(s); break;
    case Z80GEN_PORT_EXIT:
        if (s->cfg->cpm_path) { s->exit_requested = true; generic_end_run(s, true); }
        break;
    default: break;
    }
}

/* ── Monitor ─────────────────────────────────────────────────────────────── */

static bool generic_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    Z80GenericState *s = ud;
    if (addr > 0xFFFF) return false;
    *val = s->mem[addr];
    return true;
}

static bool generic_mon_write(void *ud, uint64_t addr, uint8_t val) {
    Z80GenericState *s = ud;
    if (addr > 0xFFFF || s->rom_map[addr]) return false;
    s->mem[addr] = val;
    return true;
}

static void generic_mon_cpu_state(void *ud, char *buf, size_t len) {
    const Z80GenericState *s = ud;
    z80_format_state(s->cpu, buf, len);
}

static uint8_t dis_peek(void *ud, uint16_t addr) {
    return ((const Z80GenericState *)ud)->mem[addr];
}

static GemuMonCmd cmd_disas(GemuMonitor *mon, const char *args, void *ud) {
    Z80GenericState *s = ud;
    char a1[64] = "", a2[32] = "";
    sscanf(args, "%63s %31s", a1, a2);
    uint64_t addr = s->cpu->pc;
    if (a1[0] && !gemu_monitor_parse_addr(mon, a1, &addr)) {
        gemu_monitor_printf(mon, "disas: bad address '%s'\n", a1);
        return GEMU_MON_NONE;
    }
    int count = a2[0] ? atoi(a2) : 16;
    if (count <= 0 || count > 1000) count = 16;

    uint16_t pc = (uint16_t)addr;
    for (int i = 0; i < count; i++) {
        const char *label = gemu_monitor_symbol_name(mon, pc);
        if (label) gemu_monitor_printf(mon, "        %s:\n", label);
        char text[64], bytes[16] = "";
        int len = z80_disasm(dis_peek, s, pc, text, sizeof(text));
        for (int b = 0; b < len; b++)
            snprintf(bytes + strlen(bytes), sizeof(bytes) - strlen(bytes), "%02X ",
                     s->mem[(uint16_t)(pc + b)]);
        gemu_monitor_printf(mon, "%s%04X  %-12s %s\n", pc == s->cpu->pc ? ">" : " ", pc, bytes, text);
        pc = (uint16_t)(pc + len);
    }
    return GEMU_MON_NONE;
}

/* ── Loading ─────────────────────────────────────────────────────────────── */

static bool load_file_at(Z80GenericState *s, uint32_t addr, const char *path, bool rom) {
    uint32_t off = addr & 0xFFFFu;
    GemuMemory tmp = {.data = s->mem + off, .size = 0x10000u - off};
    size_t len = 0;
    if (!gemu_mem_load_file(&tmp, 0, path, &len)) {
        fprintf(stderr, "gemu-z80: failed to load '%s'\n", path);
        return false;
    }
    if (rom) {
        memset(s->rom_map + off, 1, len);
        gemu_monitor_register_rom(s->monitor, off, (uint32_t)len, path);
    }
    printf("gemu-z80: %zu bytes @ %04Xh  <- %s\n", len, (unsigned)off, path);
    return true;
}

/* -cpm: warm boot at 0000h, the BDOS entry at 0005h jumping to an OUT the
 * machine traps, and (0006h) giving the top of the TPA as CP/M does. */
static void install_cpm(Z80GenericState *s) {
    static const uint8_t page0[] = {
        0xD3, Z80GEN_PORT_EXIT, 0x76,        /* 0000: OUT (FFh),A; HALT */
        0x00, 0x00,
        0xC3, 0x00, 0xFE,                    /* 0005: JP FE00h */
    };
    static const uint8_t bdos[] = { 0xD3, Z80GEN_PORT_BDOS, 0xC9 };   /* FE00: OUT (FEh),A; RET */
    memcpy(s->mem, page0, sizeof(page0));
    memcpy(s->mem + 0xFE00, bdos, sizeof(bdos));
}

static bool load_all(Z80GenericState *s, const Z80Config *cfg) {
    memset(s->mem, 0, sizeof(s->mem));
    memset(s->rom_map, 0, sizeof(s->rom_map));
    gemu_monitor_clear_roms(s->monitor);
    for (int i = 0; i < cfg->n_roms; i++)
        if (!load_file_at(s, cfg->roms[i].addr, cfg->roms[i].path, true)) return false;
    if (cfg->cpm_path) {
        install_cpm(s);
        if (!load_file_at(s, 0x0100, cfg->cpm_path, false)) return false;
    }
    return true;
}

static void generic_reset(Z80GenericState *s, const Z80Config *cfg) {
    timer_disarm(s);
    s->timer_ctrl = 0;
    s->timer_pending = false;
    s->serial_ctrl = 0;
    s->serial_rxq_r = s->serial_rxq_w = s->serial_rxq_cnt = 0;
    s->exit_requested = false;
    s->cpu->int_line = s->cpu->nmi_line = false;
    z80_reset(s->cpu);
    if (cfg->cpm_path) { s->cpu->pc = 0x0100; s->cpu->sp = 0xFE00; }
    if (cfg->has_start_addr) s->cpu->pc = cfg->start_addr;
}

/* ── The core machine (stage Z2) ─────────────────────────────────────────────
 * board (z80gen-board): the 64 KB (and which bytes are ROM), the timer's
 * and serial port's registers; cpu (z80): the machine's executor at
 * -clock T-states a second (default 4 MHz), an instruction at a time; the
 * timer is a core timer the board arms a period at a time.
 * The bus, "program": the memory, the ROM loads read-only over it (writes
 * there dropped).  The CPU reads and writes the memory directly (with the
 * monitor's watchpoints), as before; the space is the monitor's view.
 * The executor runs a plain loop of instructions: breakpoints are its
 * stop_before hook only while some are set; a watchpoint, or the CP/M
 * program's end, ends a run from inside the instruction.
 * Before stage Z the run loop sliced frames from wherever the last one
 * ended (a frame's overshoot was lost) and checked the timer after each
 * instruction; now frames are machine time and the timer is the core's,
 * expiring after the same instruction. */
typedef struct Z80GenCore {
    GemuMachine *machine;
    GemuDevice  *board, *cpu;
    GemuTimer   *timer;
    GemuSpace   *bus;
} Z80GenCore;

static void timer_arm(Z80GenericState *s) {
    if (s->core) gemu_timer_start_clocks(s->core->timer, s->cfg->clock_hz, timer_period(s), 0, 0);
}
static void timer_disarm(Z80GenericState *s) {
    if (s->core) gemu_timer_stop(s->core->timer);
}

static void drop_w(void *ud, uint64_t off, uint8_t v) { (void)ud; (void)off; (void)v; }
static const GemuHandlers drop_handlers = { .w8 = drop_w };

/* The monitor's view: the memory, each ROM load read-only over it. */
static bool generic_bus_build(Z80GenericState *s) {
    Z80GenCore *c = s->core;
    GemuSpace *b = c->bus = gemu_space_new(c->machine, "program", 16, 8, GEMU_LITTLE_ENDIAN);
    if (!b || !gemu_space_map_ram(b, 0, 0x10000, s->mem, 0, 0)) return false;
    for (uint32_t a = 0; a < 0x10000;) {
        if (!s->rom_map[a]) { a++; continue; }
        uint32_t e = a;
        while (e < 0x10000 && s->rom_map[e]) e++;
        if (!gemu_space_map_ram(b, a, e - a, s->mem + a, GEMU_MAP_READONLY, 1) ||
            !gemu_space_map(b, a, e - a, &drop_handlers, s, 1, 0))
            return false;
        a = e;
    }
    return true;
}

static void generic_board_describe(GemuDevice *d, char *buf, size_t len) {
    const Z80GenericState *s = *(Z80GenericState **)d->priv;
    snprintf(buf, len, "timer: reload %04X, control %02X%s; serial: control %02X, %u byte%s waiting%s",
             s->timer_reload, s->timer_ctrl, s->timer_pending ? ", pending" : "", s->serial_ctrl,
             s->serial_rxq_cnt, s->serial_rxq_cnt == 1 ? "" : "s", s->cfg->cpm_path ? "; CP/M" : "");
}

static const GemuDeviceType generic_board_type = {
    .name = "z80gen-board", .desc = "Generic Z80 board (memory, timer, serial)",
    .size = sizeof(Z80GenericState *), .describe = generic_board_describe,
};

#define B(field) ok &= GEMU_STATE_MEMBER(d, "", s, field)
/* The board's state: the memory, the timer and serial registers, the
 * serial input not yet read, the count.  Not saved: which bytes are ROM
 * (the loads). */
static bool generic_board_state(Z80GenericState *s, GemuDevice *d) {
    bool ok = true;
    B(mem); B(timer_reload); B(timer_ctrl); B(timer_pending);
    B(serial_ctrl); B(serial_rxq); B(serial_rxq_r); B(serial_rxq_w); B(serial_rxq_cnt);
    B(exit_requested);
    return ok;
}
#undef B

static uint64_t generic_insns(const Z80GenericState *s) { return gemu_z80_insns(s->core->cpu); }

static void generic_end_run(Z80GenericState *s, bool done) {
    if (s->core) gemu_z80_end_run(s->core->cpu, done ? GEMU_Z80_DONE : GEMU_Z80_STOPPED);
}
static bool hook_stop_before(void *ud) {
    Z80GenericState *s = ud;
    return gemu_monitor_check_exec(s->monitor, s->cpu->pc);
}
static bool hook_step_done(void *ud) {
    Z80GenericState *s = ud;
    return generic_insns(s) >= s->step_to;
}

/* The executor's hooks for a frame: none (the plain loop) unless
 * breakpoints are set; the program's end and watchpoints end a run from
 * inside the instruction. */
static void generic_set_hooks(Z80GenericState *s) {
    GemuZ80Hooks h = { .ud = s };
    if (s->step_to) h.done = hook_step_done;
    else if (gemu_monitor_has_exec_breakpoints(s->monitor)) h.stop_before = hook_stop_before;
    gemu_z80_set_hooks(s->core->cpu, &h);
}

static bool generic_core_build(Z80GenericState *s) {
    Z80GenCore *c = s->core = calloc(1, sizeof *c);
    if (!c || !(c->machine = gemu_machine_new("z80"))) return false;
    bool ok = (c->board = gemu_device_add(c->machine, &generic_board_type, "board", NULL, NULL)) &&
              (c->cpu = gemu_device_add(c->machine, &gemu_z80_type, "cpu", NULL, NULL));
    if (ok) {
        *(Z80GenericState **)c->board->priv = s;
        ok = generic_board_state(s, c->board) &&
             (c->timer = gemu_timer_new(c->board, generic_timer_fire, s)) &&
             gemu_machine_init(c->machine);
    }
    if (ok) {
        s->cpu = gemu_z80(c->cpu);
        s->cpu->read  = generic_read;
        s->cpu->write = generic_write;
        s->cpu->in    = generic_in;
        s->cpu->out   = generic_out;
        s->cpu->ud    = s;
        ok = gemu_z80_exec(c->cpu, s->cfg->clock_hz, NULL) && gemu_machine_start(c->machine);
    }
    if (!ok) {
        fprintf(stderr, "gemu-z80: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    return true;
}

/* After the loads: the monitor's view of the bus (ROM where they put it). */
static bool generic_core_attach(Z80GenericState *s) {
    if (!generic_bus_build(s)) return false;
    gemu_monitor_attach_machine(s->monitor, s->core->machine, "program");
    return true;
}

static void generic_core_free(Z80GenericState *s) {
    if (!s->core) return;
    gemu_machine_free(s->core->machine);
    free(s->core);
    s->core = NULL;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

Z80GenericState *z80_generic_create(const Z80Config *cfg) {
    Z80GenericState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = generic_mon_read, .write = generic_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = false });
    gemu_monitor_set_cpu_state_cb(s->monitor, generic_mon_cpu_state, s);
    gemu_monitor_bind(s->monitor, "disas", cmd_disas, s);

    if (!generic_core_build(s) || !load_all(s, cfg) || !generic_core_attach(s)) {
        z80_generic_destroy(s);
        return NULL;
    }
    generic_reset(s, cfg);
    return s;
}

void z80_generic_destroy(Z80GenericState *s) {
    if (!s) return;
    generic_core_free(s);
    gemu_monitor_destroy(s->monitor);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

#define GENERIC_FRAME_HZ 60u

/* A CP/M test program runs 50 frames' worth at a time, unpaced. */
static uint64_t generic_frame_cycles(const Z80GenericState *s) {
    uint64_t n = s->cfg->clock_hz / GENERIC_FRAME_HZ;
    return s->cfg->cpm_path ? n * 50 : n;
}

/* One frame: the serial input, then the machine for the frame's time
 * (stopping early at the program's end, a breakpoint or a pause). */
static void generic_frame(Z80GenericState *s) {
    generic_poll_serial(s);
    generic_set_hooks(s);
    GemuMachine *m = s->core->machine;
    gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(s->cfg->clock_hz, generic_frame_cycles(s)));
}

/* The monitor's step: n instructions through the machine (the timer
 * expires on time), breakpoints and the pause aside. */
static void generic_step(Z80GenericState *s, uint32_t n) {
    if (!n) return;
    GemuMachine *m = s->core->machine;
    s->step_to = generic_insns(s) + n;
    generic_set_hooks(s);
    while (generic_insns(s) < s->step_to && !s->exit_requested)
        gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(s->cfg->clock_hz, 1000000));
    s->step_to = 0;
}

void z80_generic_run(Z80GenericState *s, const Z80Config *cfg) {
    gemu_monitor_start(s->monitor);
    bool throttle = !cfg->cpm_path;
    GemuMachine *m = s->core->machine;

    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT) quit = true;

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { quit = true; break; }
            } else if (cmd == GEMU_MON_RESET) {
                load_all(s, cfg);
                generic_reset(s, cfg);
            } else if (cmd == GEMU_MON_STEP) {
                generic_step(s, gemu_monitor_step_count(s->monitor));
            }
        }
        if (quit) break;

        bool paused = gemu_monitor_is_paused(s->monitor);
        if (!paused) generic_frame(s);
        if (s->exit_requested) {
            s->exit_requested = false;
            if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
            else quit = true;
        }

        /* Pace to the machine's time (a CP/M program runs flat out);
         * paused, a frame's delay. */
        if (paused) {
            gemu_machine_pace_reset(m);
            SDL_Delay(1000u / GENERIC_FRAME_HZ);
        } else if (throttle) {
            gemu_machine_pace(m, 0);
        }
    }

    printf("gemu-z80: %llu T-states, %llu instructions\n",
           (unsigned long long)s->cpu->cycles, (unsigned long long)generic_insns(s));
    gemu_monitor_stop(s->monitor);
}
