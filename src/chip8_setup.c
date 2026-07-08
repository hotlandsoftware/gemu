#include "chip8.h"
#include "gemu/gemu.h"
#include "gemu/args.h"
#include "gemu/monitor.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Device registry ─────────────────────────────────────────────────────── */

static const GemuDevDesc machines[] = {
#include "generated/machines_chip8.inc"
};
static const GemuDevDesc cpus[] = {
    {"chip8", "CHIP-8 bytecode interpreter (TCG-accelerated)"},
};
static const GemuArgsDef def = {
    .prog       = "gemu",
    .machines   = machines,  .n_machines = (int)(sizeof machines / sizeof *machines),
    .cpus       = cpus,      .n_cpus     = 1,
    .vgas       = NULL,      .n_vgas     = 0,
    .display_mask = GEMU_DISP_F(GEMU_DISPLAY_SDL)
#ifndef GEMU_NO_CURSES
                  | GEMU_DISP_F(GEMU_DISPLAY_CURSES)
#endif
                  | GEMU_DISP_F(GEMU_DISPLAY_NONE)
#ifdef GEMU_GTK
                  | GEMU_DISP_F(GEMU_DISPLAY_GTK)
#endif
    ,
    .vnc_support = true,
};

/* ── Chip8-specific extra flags ──────────────────────────────────────────── */

static uint32_t parse_size(const char *s) {
    char *end;
    uint32_t v = (uint32_t)strtoul(s, &end, 0);
    if      (*end == 'K' || *end == 'k') v *= 1024;
    else if (*end == 'M' || *end == 'm') v *= 1024 * 1024;
    return v ? v : CHIP8_MEM_SIZE;
}

int chip8_setup(int argc, char *argv[]) {
    if (argc < 2) {
        char *fake[] = {argv[0], "-h"};
        int rem = 0;
        gemu_args_parse(2, fake, &def, &(GemuArgs){}, &rem, NULL);
        return 1;
    }

    Chip8Config cfg = {
        .mem_size      = CHIP8_MEM_SIZE,
        .cpu_hz        = 0,
        .display_type  = GEMU_DISPLAY_SDL,
        .display_scale = 10,
        .quirk_shift   = false,
        .quirk_jump    = false,
        .fg_argb       = 0,
        .bg_argb       = 0xFF000000u,
    };

    GemuArgs args = {
        .display_type  = cfg.display_type,
        .display_scale = cfg.display_scale,
    };

    char *rem[32]; int nrem = 0;
    if (!gemu_args_parse(argc, argv, &def, &args, &nrem, rem))
        return 1;
    gemu_monitor_set_default(args.monitor_spec);

    cfg.rom_path     = args.rom_path;
    cfg.display_type = args.display_type;
    cfg.display_scale = args.display_scale;
    cfg.vnc_addr     = args.vnc_addr;
    cfg.no_shutdown  = args.no_shutdown;

    for (int i = 0; i < nrem; i++) {
        if ((strcmp(rem[i], "-hz") == 0 || strcmp(rem[i], "-speed") == 0) && i + 1 < nrem) {
            int cpf = atoi(rem[++i]);
            cfg.cpu_hz = (cpf > 0) ? cpf : -1;
        } else if (strcmp(rem[i], "-m") == 0 && i + 1 < nrem) {
            cfg.mem_size = parse_size(rem[++i]);
        } else if (strcmp(rem[i], "-quirk-shift") == 0) {
            cfg.quirk_shift = true;
        } else if (strcmp(rem[i], "-quirk-jump") == 0) {
            cfg.quirk_jump = true;
        } else if (strcmp(rem[i], "-fg") == 0 && i + 1 < nrem) {
            cfg.fg_argb = 0xFF000000u | (uint32_t)strtoul(rem[++i], NULL, 16);
        } else if (strcmp(rem[i], "-bg") == 0 && i + 1 < nrem) {
            cfg.bg_argb = 0xFF000000u | (uint32_t)strtoul(rem[++i], NULL, 16);
        } else {
            fprintf(stderr, "gemu: unknown option '%s' (try -h)\n", rem[i]);
            return 1;
        }
    }

    {
        size_t rlen = cfg.rom_path ? strlen(cfg.rom_path) : 0;
        bool is_8o  = rlen >= 3 && strcmp(cfg.rom_path + rlen - 3, ".8o") == 0;
        if (cfg.cpu_hz <= 0)
            cfg.cpu_hz = is_8o ? CHIP8_DEFAULT_CPF_8O : CHIP8_DEFAULT_CPF;
        if (!cfg.fg_argb)
            cfg.fg_argb = is_8o ? 0xFFFFCC00u : 0xFFFFFFFFu;
    }

    if (!cfg.rom_path) {
        fprintf(stderr, "gemu: no ROM specified\n");
        return 1;
    }

    bool sdl_up = (cfg.display_type == GEMU_DISPLAY_SDL);
    if (sdl_up && SDL_Init(0) < 0) {
        fprintf(stderr, "gemu: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    Chip8State *s = chip8_machine_create(&cfg);
    if (!s) { if (sdl_up) SDL_Quit(); return 1; }

    chip8_machine_run(s, &cfg);
    chip8_machine_destroy(s);

    if (sdl_up) SDL_Quit();
    return 0;
}
