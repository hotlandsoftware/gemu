#include "ibm5150.h"
#include "romdb.h"
#include "gemu/args.h"
#include "gemu/monitor.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    const char *name;
    const char *canonical;
    const char *cpu;
    const char *vga;
    const char *soundhw;
    const char *chargen;
    const char *tv;
    const char *ram;
    const char *devices;
    const char *menu;
} MachineDef;

static const MachineDef machine_defs[] = {
#include "generated/machine_defaults.inc"
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

static const GemuDevDesc machines[] = {
#include "generated/machines_x86.inc"
};

static const GemuDevDesc cpus[] = {
    { "8086", "Intel 8086 (16-bit external bus)" },
    { "8088", "Intel 8088 (8-bit external bus, IBM 5150/5160)" },
};

static const GemuDevDesc vgas[] = {
#include "generated/vgas_x86.inc"
};

static const GemuArgsDef def = {
    .prog         = "gemu",
    .machines     = machines,
    .n_machines   = (int)(sizeof machines / sizeof *machines),
    .cpus         = cpus,
    .n_cpus       = (int)(sizeof cpus / sizeof *cpus),
    .vgas         = vgas,
    .n_vgas       = (int)(sizeof vgas / sizeof *vgas),
    .display_mask = GEMU_DISP_F(GEMU_DISPLAY_SDL) | GEMU_DISP_F(GEMU_DISPLAY_NONE),
    .vnc_support  = false,
    .extra_help =
        "\nx86 options:\n"
        "  -rom FILE|DIR   Load a BIOS/BASIC image, or scan a directory for\n"
        "                  known ROMs by SHA-256 (e.g. -rom roms/ibm5150)\n"
        "  -m SIZE         RAM size, K/M suffix (default 640K, ibm5150's ceiling)\n"
        "\nThis is an early skeleton: the 8086/8088 core covers the common\n"
        "real-mode instruction set but not BCD adjust or cycle-exact timing,\n"
        "and CGA only renders text mode - see cpu/x86/i8086.c and vga/x86/cga.c\n"
        "for the exact gaps.\n"
        "\nExample:\n"
        "  ./bin/gemu -M ibm5150 -rom roms/ibm5150 -display sdl\n",
};

static uint32_t parse_size(const char *s) {
    char *end;
    uint64_t v = strtoull(s, &end, 0);
    if      (*end == 'K' || *end == 'k') v <<= 10;
    else if (*end == 'M' || *end == 'm') v <<= 20;
    else if (*end == '\0')               v <<= 10; /* plain number = KB */
    if (v == 0 || v > IBM5150_RAM_MAX) {
        fprintf(stderr, "gemu: -m: invalid size '%s' (up to 640K)\n", s);
        return 0;
    }
    return (uint32_t)v;
}

typedef struct { X86Config *cfg; int loaded; } RomScanCtx;

static bool romdb_add_x86(const char *path, const char *region, uint32_t addr, void *ud) {
    (void)addr;
    RomScanCtx *ctx = ud;
    if (ctx->cfg->n_roms >= X86_MAX_ROM_LOADS) return false;
    X86RomLoad *r = &ctx->cfg->roms[ctx->cfg->n_roms++];
    r->path = strdup(path);
    r->region = region && region[0] ? strdup(region) : NULL;
    ctx->loaded++;
    return true;
}

int x86_setup(int argc, char *argv[]) {
    X86Config cfg = {
        .machine = X86_MACHINE_IBM5150,
        .cpu = X86_CPU_8088,
        .vga = X86_VGA_CGA,
        .ram_size = IBM5150_RAM_MAX,
        .display_type = GEMU_DISPLAY_SDL,
        .display_scale = 1,
    };
    GemuArgs args = {
        .display_type = cfg.display_type,
        .display_scale = cfg.display_scale,
    };

    char *rem[32];
    int nrem = 0;
    if (!gemu_args_parse(argc, argv, &def, &args, &nrem, rem))
        return 1;
    gemu_monitor_set_default(args.monitor_spec);
    if (args.machine_opts) {
        fprintf(stderr, "gemu: -M machine feature options are not supported by x86 machines\n");
        return 1;
    }

    const char *alias = "ibm5150";
    if (args.machine) {
        const MachineDef *md = NULL;
        for (int i = 0; machine_defs[i].name; i++) {
            if (strcmp(args.machine, machine_defs[i].name) == 0) { md = &machine_defs[i]; break; }
        }
        if (!md) {
            fprintf(stderr, "gemu: unknown x86 machine '%s'\n", args.machine);
            return 1;
        }
        alias = md->canonical;
    }
    if (strcmp(alias, "ibm5150") != 0) {
        fprintf(stderr, "gemu: '%s' is not implemented yet (only ibm5150 is)\n", alias);
        return 1;
    }

    if (args.cpu) {
        if (strcmp(args.cpu, "8086") == 0) cfg.cpu = X86_CPU_8086;
        else if (strcmp(args.cpu, "8088") == 0) cfg.cpu = X86_CPU_8088;
    }
    if (args.vga && strcmp(args.vga, "cga") != 0) {
        fprintf(stderr, "gemu: unknown x86 VGA '%s' (use -vga ? to list)\n", args.vga);
        return 1;
    }

    RomScanCtx ctx = { .cfg = &cfg, .loaded = 0 };
    const char *rom_arg = args.rom_path;
    for (int i = 0; i < nrem; i++) {
        if (strcmp(rem[i], "-m") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -m requires an argument\n"); return 1; }
            uint32_t sz = parse_size(rem[++i]);
            if (!sz) return 1;
            cfg.ram_size = sz;
        } else if (strcmp(rem[i], "-rom") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -rom requires an argument\n"); return 1; }
            rom_arg = rem[++i];
        } else {
            fprintf(stderr, "gemu: unknown option '%s' (try -h)\n", rem[i]);
            return 1;
        }
    }
    if (rom_arg) {
        struct stat st;
        if ((stat(rom_arg, &st) == 0 && S_ISDIR(st.st_mode)) || romdb_is_zip(rom_arg)) {
            int n = romdb_load_dir(rom_arg, alias, romdb_add_x86, &ctx);
            if (n < 0) { fprintf(stderr, "gemu: cannot scan '%s'\n", rom_arg); return 1; }
        } else if (cfg.n_roms < X86_MAX_ROM_LOADS) {
            X86RomLoad *r = &cfg.roms[cfg.n_roms++];
            r->path = rom_arg;
            r->region = NULL;
            ctx.loaded++;
        }
    }
    if (ctx.loaded == 0) {
        fprintf(stderr, "gemu: %s needs a BIOS ROM (-rom FILE, or -rom DIR to scan)\n", alias);
        romdb_print_needed(alias);
        return 1;
    }

    cfg.display_type = args.display_type;
    cfg.display_scale = args.display_scale;
    cfg.no_shutdown = args.no_shutdown;

    bool sdl_up = (cfg.display_type == GEMU_DISPLAY_SDL);
    if (sdl_up && SDL_Init(0) < 0) {
        fprintf(stderr, "gemu: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    Ibm5150State *s = ibm5150_create(&cfg);
    if (!s) {
        if (sdl_up) SDL_Quit();
        return 1;
    }
    ibm5150_run(s, &cfg);
    ibm5150_destroy(s);
    if (sdl_up) SDL_Quit();
    return 0;
}
