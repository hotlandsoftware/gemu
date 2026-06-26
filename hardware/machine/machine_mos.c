#include "machine_mos.h"
#include "gemu/memory.h"
#include <SDL2/SDL.h>
#ifndef _WIN32
#  include <sys/select.h>
#  include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── IRQ aggregator ──────────────────────────────────────────────────────── */

static void generic_update_irq(MosGenericState *s) {
    s->cpu.irq =
        (s->timer_pending  && (s->timer_ctrl  & 0x02u)) ||
        (s->serial_rxq_cnt && (s->serial_ctrl & 0x01u));
}

/* ── Timer ───────────────────────────────────────────────────────────────── */

static uint32_t timer_period(MosGenericState *s) {
    return s->timer_reload ? (uint32_t)s->timer_reload : 0x10000u;
}

/* Call once per instruction in the run loop. */
static void generic_timer_tick(MosGenericState *s) {
    if (!(s->timer_ctrl & 0x01u)) return;
    if (s->cpu.cycle_count < s->timer_next_fire) return;

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

static uint8_t generic_mem_read(uint16_t addr, void *ud) {
    MosGenericState *s = ud;
    gemu_monitor_check_read(s->monitor, addr);

    if (addr >= GENERIC_IO_BASE && addr <= GENERIC_IO_END) {
        switch (addr - GENERIC_IO_BASE) {
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

    return s->mem[addr % s->mem_size];
}

static void generic_mem_write(uint16_t addr, uint8_t val, void *ud) {
    MosGenericState *s = ud;
    gemu_monitor_check_write(s->monitor, addr);

    /* Legacy debug port: write byte to stdout (used by test ROMs) */
    if (addr == 0xF001u) {
        putchar(val);
        fflush(stdout);
        return;
    }

    if (addr >= GENERIC_IO_BASE && addr <= GENERIC_IO_END) {
        switch (addr - GENERIC_IO_BASE) {
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
                s->timer_next_fire = s->cpu.cycle_count + timer_period(s);
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
        return;
    }

    uint32_t off = addr % s->mem_size;
    if (!s->rom_map[off])
        s->mem[off] = val;
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

    mos6502_init(&s->cpu);   /* zeroes the struct — must come before assigning callbacks */

    s->cpu.mem_read        = generic_mem_read;
    s->cpu.mem_write       = generic_mem_write;
    s->cpu.mem_ud          = s;
    s->cpu.decimal_disable = (cfg->cpu == MOS_CPU_2A03);

    s->monitor = gemu_monitor_create();

    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr, 320, 200);
        if (!s->vnc)
            fprintf(stderr, "gemu-6502: failed to start VNC server at %s\n",
                    cfg->vnc_addr);
    }
    gemu_monitor_set_vnc(s->monitor, s->vnc);

    if (!load_roms(s, cfg)) {
        gemu_vnc_destroy(s->vnc);
        gemu_monitor_destroy(s->monitor);
        free(s);
        return NULL;
    }

    mos6502_reset(&s->cpu);

    if (cfg->has_start_addr)
        s->cpu.PC = cfg->start_addr;

    return s;
}

void mos_generic_reset(MosGenericState *s, const MosConfig *cfg) {
    gemu_monitor_clear_roms(s->monitor);
    memset(s->rom_map, 0, s->mem_size);
    load_roms(s, cfg);

    s->timer_ctrl    = 0;
    s->timer_pending = false;
    s->serial_ctrl   = 0;
    s->serial_rxq_r  = 0;
    s->serial_rxq_w  = 0;
    s->serial_rxq_cnt = 0;

    mos6502_reset(&s->cpu);
    if (cfg->has_start_addr)
        s->cpu.PC = cfg->start_addr;
}

void mos_generic_destroy(MosGenericState *s) {
    gemu_monitor_destroy(s->monitor);
    gemu_vnc_destroy(s->vnc);
    free(s->mem);
    free(s->rom_map);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

#define GENERIC_HZ                1000000u
#define GENERIC_FRAME_HZ          60u
#define GENERIC_CYCLES_PER_FRAME  (GENERIC_HZ / GENERIC_FRAME_HZ)
#define GENERIC_FRAME_MS          (1000u / GENERIC_FRAME_HZ)

void mos_generic_run(MosGenericState *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)   { quit = true; break; }
            else if (cmd == GEMU_MON_RESET)  mos_generic_reset(s, cfg);
            else if (cmd == GEMU_MON_CUSTOM) gemu_monitor_unknown_command(s->monitor);
        }
        if (quit) break;

        if (s->vnc) {
            GemuVncKeyEvent ev_key;
            while (gemu_vnc_pop_key_event(s->vnc, &ev_key)) { /* discard for now */ }
        }

        if (!gemu_monitor_is_paused(s->monitor)) {
            generic_poll_serial(s);

            uint64_t target = s->cpu.cycle_count + GENERIC_CYCLES_PER_FRAME;
            while (s->cpu.cycle_count < target) {
                if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) break;
                mos6502_step(&s->cpu);
                generic_timer_tick(s);
                if (gemu_monitor_is_paused(s->monitor)) break;
            }
        }

        Uint32 elapsed = SDL_GetTicks() - t0;
        if (elapsed < GENERIC_FRAME_MS)
            SDL_Delay(GENERIC_FRAME_MS - elapsed);
    }

    printf("gemu-6502: %llu cycles, %llu instructions\n",
           (unsigned long long)s->cpu.cycle_count,
           (unsigned long long)s->cpu.insn_count);

    gemu_monitor_stop(s->monitor);
    (void)cfg;
}
