#include "i2000.h"
#include "generic.h"
#include "romdb.h"
#include "gemu/args.h"
#include "gemu/monitor.h"
#include <SDL2/SDL.h>
#include <inttypes.h>
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
} MachineDef;

static const MachineDef machine_defs[] = {
#include "generated/machine_defaults.inc"
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

static const GemuDevDesc machines[] = {
#include "generated/machines_ia64.inc"
};

static const GemuDevDesc cpus[] = {
    { "merced", "Intel Itanium (Merced) [skeleton]" },
};

static const GemuArgsDef def = {
    .prog         = "gemu",
    .machines     = machines,
    .n_machines   = (int)(sizeof machines / sizeof *machines),
    .cpus         = cpus,
    .n_cpus       = (int)(sizeof cpus / sizeof *cpus),
    .display_mask = GEMU_DISP_F(GEMU_DISPLAY_SDL) | GEMU_DISP_F(GEMU_DISPLAY_NONE)
#ifdef GEMU_GTK
                  | GEMU_DISP_F(GEMU_DISPLAY_GTK)
#endif
    ,
    .vnc_support  = false,
    .extra_help =
        "\nHP i2000 (IA-64) options:\n"
        "  -rom FILE       Load firmware flash image (top-aligned, max 4 MiB)\n"
        "  -rom DIR|ZIP    Scan for known firmware by SHA-256\n"
        "  -m SIZE         RAM size, K/M/G suffix (default 512M, i2000 max 2G)\n"
        "  -cdrom FILE     Attach a read-only ISO image as an ATAPI CD-ROM\n"
        "\nThe SDL display is a front panel: CPU state, POST code, unhandled\n"
        "MMIO log, and the COM1 serial console (also echoed to stdout).\n"
        "Monitor: 'info cpu', 'step [N]', 'x ADDR [COUNT]' (phys hexdump).\n"
        "\nExample commands:\n"
        "  ./bin/gemu -M i2000 -rom roms/bios130.BIN\n"
        "  ./bin/gemu -M i2000 -rom roms/bios130.BIN -cdrom disc.iso\n"
        "  ./bin/gemu -M i2000 -rom roms/ -m 1G -display sdl\n",
};

/* ── Memory size parsing ─────────────────────────────────────────────────── */

static uint64_t parse_size(const char *s) {
    char *end;
    uint64_t v = strtoull(s, &end, 0);
    if      (*end == 'K' || *end == 'k') v <<= 10;
    else if (*end == 'M' || *end == 'm') v <<= 20;
    else if (*end == 'G' || *end == 'g') v <<= 30;
    else if (*end == '\0')               v <<= 10;   /* plain number = KB */
    if (v < (16ull << 20) || v > I2000_RAM_MAX) {
        fprintf(stderr, "gemu: -m: invalid size '%s' (16M-2G, with K/M/G suffix)\n", s);
        return 0;
    }
    return v;
}

/* ── Setup ───────────────────────────────────────────────────────────────── */

typedef struct {
    Ia64I2000State *machine;
    int             loaded;
} RomScanCtx;

static bool romdb_add_ia64(const char *path, const char *region, uint32_t addr, void *ud) {
    (void)region; (void)addr;   /* one flash image, always top-aligned */
    RomScanCtx *ctx = ud;
    if (ia64_i2000_load_firmware(ctx->machine, path))
        ctx->loaded++;
    return true;
}

int ia64_setup(int argc, char *argv[]) {
    Ia64Config cfg = {
        .ram_size = 512ull << 20,
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
        fprintf(stderr, "gemu: -M machine feature options are not supported by ia64 machines\n");
        return 1;
    }

    const char *alias = "i2000";
    if (args.machine) {
        const MachineDef *md = NULL;
        for (int i = 0; machine_defs[i].name; i++) {
            if (strcmp(args.machine, machine_defs[i].name) == 0) {
                md = &machine_defs[i];
                break;
            }
        }
        if (!md) {
            fprintf(stderr, "gemu: unknown ia64 machine '%s'\n", args.machine);
            return 1;
        }
        alias = md->canonical;
        if (md->ram) {
            uint64_t sz = parse_size(md->ram);
            if (!sz) return 1;
            cfg.ram_size = sz;
        }
    }

    if (args.cpu && strcmp(args.cpu, "merced") != 0) {
        fprintf(stderr, "gemu: unknown ia64 CPU '%s' (use -cpu ? to list)\n", args.cpu);
        return 1;
    }

    const char *rom_arg = args.rom_path;
    for (int i = 0; i < nrem; i++) {
        if (strcmp(rem[i], "-rom") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -rom requires an argument\n"); return 1; }
            rom_arg = rem[++i];
        } else if (strcmp(rem[i], "-m") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -m requires an argument\n"); return 1; }
            uint64_t sz = parse_size(rem[++i]);
            if (!sz) return 1;
            cfg.ram_size = sz;
        } else if (strcmp(rem[i], "-cdrom") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -cdrom requires an argument\n"); return 1; }
            cfg.cdrom_path = rem[++i];
        } else {
            fprintf(stderr, "gemu: unknown option '%s' (try -h)\n", rem[i]);
            return 1;
        }
    }

    cfg.display_type = args.display_type;
    cfg.display_scale = args.display_scale;
    cfg.no_shutdown = args.no_shutdown;

    bool sdl_up = (cfg.display_type == GEMU_DISPLAY_SDL);
    if (sdl_up && SDL_Init(0) < 0) {
        fprintf(stderr, "gemu: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (strcmp(alias, "generic") == 0) {
        GenericConfig gcfg = {
            .ram_size = cfg.ram_size,
            .display_type = cfg.display_type,
            .display_scale = cfg.display_scale,
            .no_shutdown = cfg.no_shutdown,
        };
        Ia64GenericState *g = ia64_generic_create(&gcfg);
        if (!g)
            return 1;
        if (!rom_arg || !ia64_generic_load_firmware(g, rom_arg)) {
            fprintf(stderr, "gemu: generic needs a firmware image (-rom FILE)\n");
            ia64_generic_destroy(g);
            return 1;
        }
        ia64_generic_run(g, &gcfg);
        ia64_generic_destroy(g);
        if (sdl_up)
            SDL_Quit();
        return 0;
    }

    Ia64I2000State *s = ia64_i2000_create(&cfg);
    if (!s)
        return 1;

    bool have_fw = false;
    if (rom_arg) {
        struct stat st;
        if ((stat(rom_arg, &st) == 0 && S_ISDIR(st.st_mode)) || romdb_is_zip(rom_arg)) {
            RomScanCtx ctx = { .machine = s };
            int n = romdb_load_dir(rom_arg, alias, romdb_add_ia64, &ctx);
            if (n < 0) {
                fprintf(stderr, "gemu: cannot scan '%s'\n", rom_arg);
                ia64_i2000_destroy(s);
                return 1;
            }
            have_fw = ctx.loaded > 0;
        } else {
            have_fw = ia64_i2000_load_firmware(s, rom_arg);
            if (!have_fw) {
                ia64_i2000_destroy(s);
                return 1;
            }
        }
    }

    if (!have_fw) {
        fprintf(stderr, "gemu: %s needs a firmware image (-rom FILE, or -rom DIR to scan)\n", alias);
        romdb_print_needed(alias);
        ia64_i2000_destroy(s);
        return 1;
    }

    ia64_i2000_run(s, &cfg);
    ia64_i2000_destroy(s);
    if (sdl_up)
        SDL_Quit();
    return 0;
}
