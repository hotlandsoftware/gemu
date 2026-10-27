#include "machine_z80.h"
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
    s->cpu.int_line =
        (s->timer_pending  && (s->timer_ctrl  & 0x02u)) ||
        (s->serial_rxq_cnt && (s->serial_ctrl & 0x01u));
}

static uint32_t timer_period(const Z80GenericState *s) {
    return s->timer_reload ? (uint32_t)s->timer_reload : 0x10000u;
}

static void generic_timer_tick(Z80GenericState *s) {
    if (!(s->timer_ctrl & 0x01u)) return;
    if (s->cpu.cycles < s->timer_next_fire) return;
    s->timer_pending = true;
    if (s->timer_ctrl & 0x04u)
        s->timer_next_fire += timer_period(s);
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

static uint8_t generic_read(void *ud, uint16_t addr) {
    Z80GenericState *s = ud;
    gemu_monitor_check_read(s->monitor, addr);
    return s->mem[addr];
}

static void generic_write(void *ud, uint16_t addr, uint8_t val) {
    Z80GenericState *s = ud;
    gemu_monitor_check_write(s->monitor, addr);
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
    Z80 *c = &s->cpu;
    switch (c->c) {
    case 0: s->exit_requested = true; break;
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
        if (!was_enabled && (val & 0x01u))
            s->timer_next_fire = s->cpu.cycles + timer_period(s);
        generic_update_int(s);
        break;
    }
    case 0x10: putchar(val); fflush(stdout); break;
    case 0x12: s->serial_ctrl = val; generic_update_int(s); break;
    case Z80GEN_PORT_BDOS: if (s->cfg->cpm_path) bdos_call(s); break;
    case Z80GEN_PORT_EXIT: if (s->cfg->cpm_path) s->exit_requested = true; break;
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
    z80_format_state(&s->cpu, buf, len);
}

static uint8_t dis_peek(void *ud, uint16_t addr) {
    return ((const Z80GenericState *)ud)->mem[addr];
}

static GemuMonCmd cmd_disas(GemuMonitor *mon, const char *args, void *ud) {
    Z80GenericState *s = ud;
    char a1[64] = "", a2[32] = "";
    sscanf(args, "%63s %31s", a1, a2);
    uint64_t addr = s->cpu.pc;
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
        gemu_monitor_printf(mon, "%s%04X  %-12s %s\n", pc == s->cpu.pc ? ">" : " ", pc, bytes, text);
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
    s->timer_ctrl = 0;
    s->timer_pending = false;
    s->serial_ctrl = 0;
    s->serial_rxq_r = s->serial_rxq_w = s->serial_rxq_cnt = 0;
    s->exit_requested = false;
    s->cpu.int_line = s->cpu.nmi_line = false;
    z80_reset(&s->cpu);
    if (cfg->cpm_path) { s->cpu.pc = 0x0100; s->cpu.sp = 0xFE00; }
    if (cfg->has_start_addr) s->cpu.pc = cfg->start_addr;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

Z80GenericState *z80_generic_create(const Z80Config *cfg) {
    Z80GenericState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;

    z80_init(&s->cpu);
    s->cpu.read  = generic_read;
    s->cpu.write = generic_write;
    s->cpu.in    = generic_in;
    s->cpu.out   = generic_out;
    s->cpu.ud    = s;

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = generic_mon_read, .write = generic_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = false });
    gemu_monitor_set_cpu_state_cb(s->monitor, generic_mon_cpu_state, s);
    gemu_monitor_bind(s->monitor, "disas", cmd_disas, s);

    if (!load_all(s, cfg)) {
        z80_generic_destroy(s);
        return NULL;
    }
    generic_reset(s, cfg);
    return s;
}

void z80_generic_destroy(Z80GenericState *s) {
    if (!s) return;
    gemu_monitor_destroy(s->monitor);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

#define GENERIC_FRAME_HZ 60u
#define GENERIC_FRAME_MS (1000u / GENERIC_FRAME_HZ)

static void step_one(Z80GenericState *s) {
    z80_step(&s->cpu);
    s->insn_count++;
    generic_timer_tick(s);
}

void z80_generic_run(Z80GenericState *s, const Z80Config *cfg) {
    gemu_monitor_start(s->monitor);
    /* A CP/M test program runs as fast as the host allows. */
    bool throttle = !cfg->cpm_path;
    uint64_t per_frame = cfg->clock_hz / GENERIC_FRAME_HZ;
    if (!throttle) per_frame *= 50;

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

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
                for (uint32_t n = gemu_monitor_step_count(s->monitor); n; n--) step_one(s);
            }
        }
        if (quit) break;

        if (!gemu_monitor_is_paused(s->monitor)) {
            generic_poll_serial(s);
            uint64_t target = s->cpu.cycles + per_frame;
            while (s->cpu.cycles < target && !s->exit_requested) {
                if (gemu_monitor_check_exec(s->monitor, s->cpu.pc)) break;
                step_one(s);
                if (gemu_monitor_is_paused(s->monitor)) break;
            }
        }
        if (s->exit_requested) {
            s->exit_requested = false;
            if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
            else quit = true;
        }

        Uint32 elapsed = SDL_GetTicks() - t0;
        if (throttle && elapsed < GENERIC_FRAME_MS)
            SDL_Delay(GENERIC_FRAME_MS - elapsed);
    }

    printf("gemu-z80: %llu T-states, %llu instructions\n",
           (unsigned long long)s->cpu.cycles, (unsigned long long)s->insn_count);
    gemu_monitor_stop(s->monitor);
}
