#include "machine_z80.h"
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
#include "generated/machines_z80.inc"
};

static const GemuDevDesc cpus[] = {
    { "z80", "Zilog Z80 (NMOS, undocumented behaviour included)" },
};

static const GemuArgsDef def = {
    .prog         = "gemu",
    .machines     = machines,
    .n_machines   = (int)(sizeof machines / sizeof *machines),
    .cpus         = cpus,
    .n_cpus       = (int)(sizeof cpus / sizeof *cpus),
    .display_mask = GEMU_DISP_F(GEMU_DISPLAY_SDL) | GEMU_DISP_F(GEMU_DISPLAY_NONE),
    .extra_help =
        "\nZ80 options:\n"
        "  -rom ADDR:FILE     Load a ROM image at ADDR (read-only)\n"
        "  -rom FILE          Load a ROM image at 0000h\n"
        "  -rom DIR|FILE.zip  Scan for the machine's ROMs by SHA-256\n"
        "  -cpm FILE          Run a CP/M .COM program (console BDOS calls;\n"
        "                     a warm boot ends the run), e.g. zexdoc.com\n"
        "  -start ADDR        Start execution at ADDR instead of 0000h\n"
        "  -clock HZ          CPU clock (default 4000000, a Z80A)\n"
        "\nThe generic machine (-M z80) has 64 KB of RAM, a timer at ports\n"
        "00h-02h and a serial console (stdin/stdout) at ports 10h-12h; see\n"
        "hardware/machine/z80/machine_z80.h.  Monitor: 'disas [addr] [n]'.\n"
        "\nExamples:\n"
        "  ./bin/gemu -M z80 -rom 0:prog.bin -display none\n"
        "  ./bin/gemu -M z80 -cpm zexdoc.com -display none\n",
};

static bool add_rom(Z80Config *cfg, uint32_t addr, const char *path, const char *region) {
    if (cfg->n_roms >= Z80_MAX_ROM_LOADS) {
        fprintf(stderr, "gemu: too many ROMs (max %d)\n", Z80_MAX_ROM_LOADS);
        return false;
    }
    cfg->roms[cfg->n_roms++] = (Z80RomLoad){ .addr = addr, .path = path, .region = region };
    return true;
}

static bool romdb_add_z80(const char *path, const char *region, uint32_t addr, void *ud) {
    char *p = strdup(path);
    return p && add_rom(ud, addr, p, region);
}

static bool load_rom_arg(Z80Config *cfg, const char *arg, const char *romdb_machine) {
    struct stat st;
    if ((stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) || romdb_is_zip(arg)) {
        int n = romdb_load_dir(arg, romdb_machine, romdb_add_z80, cfg);
        if (n < 0) { fprintf(stderr, "gemu: cannot scan '%s'\n", arg); return false; }
        if (n == 0) {
            fprintf(stderr, "gemu: no known ROMs in '%s' for machine '%s'\n", arg, romdb_machine);
            romdb_print_needed(romdb_machine);
            return false;
        }
        return true;
    }
    uint32_t addr = 0;
    const char *path;
    if (gemu_parse_addr_arg("gemu", arg, &addr, &path) < 0) return false;
    return add_rom(cfg, addr, path, NULL);
}

int z80_setup(int argc, char *argv[]) {
    Z80Config cfg = {
        .machine       = Z80_MACHINE_GENERIC,
        .clock_hz      = 4000000u,
        .display_type  = GEMU_DISPLAY_SDL,
        .display_scale = 0,
    };
    GemuArgs args = {
        .display_type  = cfg.display_type,
        .display_scale = cfg.display_scale,
    };

    char *rem[32];
    int nrem = 0;
    if (!gemu_args_parse(argc, argv, &def, &args, &nrem, rem))
        return 1;
    gemu_monitor_set_default(args.monitor_spec);
    if (args.machine_opts) {
        fprintf(stderr, "gemu: -M machine feature options are not supported by z80 machines\n");
        return 1;
    }

    const char *alias = "z80";
    if (args.machine) {
        const MachineDef *md = NULL;
        for (int i = 0; machine_defs[i].name; i++)
            if (strcmp(args.machine, machine_defs[i].name) == 0) { md = &machine_defs[i]; break; }
        if (!md) {
            fprintf(stderr, "gemu: unknown z80 machine '%s'\n", args.machine);
            return 1;
        }
        alias = md->canonical;
    }
    if (strcmp(alias, "lagonda") == 0) cfg.machine = Z80_MACHINE_LAGONDA;

    if (args.cpu && strcmp(args.cpu, "z80") != 0) {
        fprintf(stderr, "gemu: unknown z80 CPU '%s' (use -cpu ? to list)\n", args.cpu);
        return 1;
    }

    if (args.rom_path && !load_rom_arg(&cfg, args.rom_path, alias)) return 1;
    for (int i = 0; i < nrem; i++) {
        const char *opt = rem[i];
        bool has_arg = i + 1 < nrem;
        if (strcmp(opt, "-rom") == 0 || strcmp(opt, "-cpm") == 0 ||
            strcmp(opt, "-start") == 0 || strcmp(opt, "-clock") == 0) {
            if (!has_arg) { fprintf(stderr, "gemu: %s requires an argument\n", opt); return 1; }
            const char *val = rem[++i];
            if (strcmp(opt, "-rom") == 0) {
                if (!load_rom_arg(&cfg, val, alias)) return 1;
            } else if (strcmp(opt, "-cpm") == 0) {
                cfg.cpm_path = val;
            } else if (strcmp(opt, "-start") == 0) {
                cfg.start_addr = (uint16_t)strtoul(val, NULL, 0);
                cfg.has_start_addr = true;
            } else {
                cfg.clock_hz = (uint32_t)strtoul(val, NULL, 0);
                if (cfg.clock_hz < 1000u) {
                    fprintf(stderr, "gemu: -clock: invalid frequency '%s'\n", val);
                    return 1;
                }
            }
        } else {
            fprintf(stderr, "gemu: unknown option '%s' (try -h)\n", opt);
            return 1;
        }
    }

    if (cfg.machine == Z80_MACHINE_LAGONDA) {
        fprintf(stderr, "gemu: lagonda is not emulated yet (the Z80 core is in place; "
                        "the machine is next)\n");
        return 1;
    }
    if (cfg.n_roms == 0 && !cfg.cpm_path) {
        fprintf(stderr, "gemu: -M z80 needs a program: -rom ADDR:FILE or -cpm FILE\n");
        return 1;
    }

    cfg.display_type = args.display_type;
    cfg.display_scale = args.display_scale;
    cfg.no_shutdown = args.no_shutdown;

    if (SDL_Init(0) < 0) {
        fprintf(stderr, "gemu: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    int rc = 1;
    Z80GenericState *s = z80_generic_create(&cfg);
    if (s) {
        z80_generic_run(s, &cfg);
        z80_generic_destroy(s);
        rc = 0;
    }
    SDL_Quit();
    return rc;
}
