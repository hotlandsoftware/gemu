#include "generic.h"
#include "gemu/memory.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Memory callbacks ────────────────────────────────────────────────────── */

static uint8_t generic_mem_read(uint16_t addr, void *ud) {
    MosGenericState *s = ud;
    return s->mem[addr % s->mem_size];
}

static void generic_mem_write(uint16_t addr, uint8_t val, void *ud) {
    MosGenericState *s = ud;

    /* Debug output port: write a byte to stdout (useful for test ROMs) */
    if (addr == 0xF001u) {
        putchar(val);
        fflush(stdout);
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

    s->cpu.mem_read       = generic_mem_read;
    s->cpu.mem_write      = generic_mem_write;
    s->cpu.mem_ud         = s;
    s->cpu.decimal_disable = (cfg->cpu == MOS_CPU_2A03);

    s->monitor = gemu_monitor_create();

    if (cfg->vnc_addr) {
        /* Placeholder 320×200 framebuffer; machines with real video will override */
        s->vnc = gemu_vnc_create(cfg->vnc_addr, 320, 200);
        if (!s->vnc)
            fprintf(stderr, "gemu-6502: failed to start VNC server at %s\n",
                    cfg->vnc_addr);
    }

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
    memset(s->rom_map, 0, s->mem_size);
    load_roms(s, cfg);
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

        /* SDL quit (headless still needs the event pump for clean exit) */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;
        }

        /* Monitor commands */
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)  { quit = true; break; }
            else if (cmd == GEMU_MON_RESET) mos_generic_reset(s, cfg);
            else if (cmd == GEMU_MON_CUSTOM) gemu_monitor_unknown_command(s->monitor);
        }
        if (quit) break;

        /* VNC key events (machines that add keyboard hardware poll s->vnc here) */
        if (s->vnc) {
            GemuVncKeyEvent ev_key;
            while (gemu_vnc_pop_key_event(s->vnc, &ev_key)) { /* discard for now */ }
        }

        if (!gemu_monitor_is_paused(s->monitor)) {
            uint64_t target = s->cpu.cycle_count + GENERIC_CYCLES_PER_FRAME;
            while (s->cpu.cycle_count < target)
                mos6502_step(&s->cpu);
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
