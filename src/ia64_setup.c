#include "i2000.h"
#include "generic.h"
#include "romdb.h"
#include "gemu/args.h"
#include "gemu/drive.h"
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
    const char *menu;
} MachineDef;

static const MachineDef machine_defs[] = {
#include "generated/machine_defaults.inc"
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

/* src/ia64_selftest.c - architectural microprogram conformance harness. */
int ia64_selftest_main(const char *path);

static const GemuDevDesc machines[] = {
#include "generated/machines_ia64.inc"
};

static const GemuDevDesc cpus[] = {
    { "merced",   "Intel Itanium (Merced) [preliminary]" },
};

static const GemuDevDesc vgas[] = {
#include "generated/vgas_ia64.inc"
};

/* The first entry is the default. */
static const GemuDevDesc accels[] = {
    { "jit",         "Translate IA-64 code to host code (x86-64 hosts) [default]" },
    { "interpreter", "Interpret one instruction slot at a time" },
};

static const GemuArgsDef def = {
    .prog         = "gemu",
    .machines     = machines,
    .n_machines   = (int)(sizeof machines / sizeof *machines),
    .cpus         = cpus,
    .n_cpus       = (int)(sizeof cpus / sizeof *cpus),
    .accels       = accels,
    .n_accels     = (int)(sizeof accels / sizeof *accels),
    .vgas         = vgas,
    .n_vgas       = (int)(sizeof vgas / sizeof *vgas),
    .display_mask = GEMU_DISP_F(GEMU_DISPLAY_SDL) | GEMU_DISP_F(GEMU_DISPLAY_NONE)
#ifdef GEMU_GTK
                  | GEMU_DISP_F(GEMU_DISPLAY_GTK)
#endif
    ,
    .block_devices    = true,
    .block_default_if = GEMU_DRIVE_IF_IDE,
    .extra_help =
        "\nIA64 options:\n"
        "  -rom FILE       Load firmware flash image (top-aligned, max 4 MiB)\n"
        "  -rom DIR|ZIP    Scan for known firmware by SHA-256\n"
        "  -m SIZE         RAM size, K/M/G suffix (default 512M)\n"
        "  -net nic,model=i82559\n"
        "                  Attach the i2000 Intel 82559 NIC (baset alias)\n"
        "  -microprogram F Run an architectural microprogram on a bare core\n"
        "                  and print the resulting state (conformance testing;\n"
        "                  see tools/ia64_conformance.py)\n"
        "  -serial stdio   generic machine only: attach COM1 to host stdio\n"
        "                  (casual use only - shares stdout with the VGA\n"
        "                  text mirror, not a clean debugger transport)\n"
        "  -serial tcp:HOST:PORT\n"
        "                  generic machine only: attach COM1 to a listening\n"
        "                  TCP socket (a clean full-duplex byte stream, for\n"
        "                  a real Windows kernel debugger to connect to)\n"
        "\nDrives (-drive/-hda/-cdrom, raw images only):\n"
        "  i2000         ide0-hd0 (-hda, index=0): ATA hard disk, 512-byte\n"
        "                sectors; ide1-cd0 (-cdrom, index=2): ATAPI CD-ROM,\n"
        "                ISO with 2048-byte sectors, may start empty\n"
        "                (-drive if=ide,index=2,media=cdrom). No slave\n"
        "                units (-hdb, -hdd) or other interfaces.\n"
        "  generic-ia64  the same two: ide0-hd0 is its HDD, ide1-cd0 its\n"
        "                CD-ROM.\n"
        "  Raw disk images can be made with\n"
        "  'qemu-img create -f raw winxp.img 5G'.\n"
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

/* What -rom DIR scans found, by region, resolved before the machine is
 * created (the LAN's EEPROM records whether it has a boot ROM). */
#define IA64_MAX_FIRMWARE 8
typedef struct {
    char *firmware[IA64_MAX_FIRMWARE];
    int   n_firmware;
    char *video_rom;
    char *nic_rom;
} RomScanCtx;

static bool romdb_add_ia64(const char *path, const char *region, uint32_t addr, void *ud) {
    (void)addr;   /* one flash image, always top-aligned */
    RomScanCtx *ctx = ud;
    if (region && strcmp(region, "vga") == 0) {
        free(ctx->video_rom);
        ctx->video_rom = strdup(path);
    } else if (region && strcmp(region, "nic") == 0) {
        free(ctx->nic_rom);
        ctx->nic_rom = strdup(path);
    } else if (ctx->n_firmware < IA64_MAX_FIRMWARE) {
        ctx->firmware[ctx->n_firmware++] = strdup(path);
    }
    return true;
}

static int cmp_path(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* -rom DIR (or .zip): scan by SHA-256; -rom FILE: the firmware image. */
static bool ia64_rom_arg(const char *arg, const char *alias, RomScanCtx *ctx,
                         const char **firmware_file) {
    struct stat st;
    if ((stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) || romdb_is_zip(arg)) {
        if (romdb_load_dir(arg, alias, romdb_add_ia64, ctx) < 0) {
            fprintf(stderr, "gemu: cannot scan '%s'\n", arg);
            return false;
        }
        return true;
    }
    *firmware_file = arg;
    return true;
}

int ia64_setup(int argc, char *argv[]) {
    Ia64Config cfg = {
        .ram_size = 512ull << 20,
        .display_type = GEMU_DISPLAY_SDL,
        .display_scale = 0,          /* 0 = 1x unless -scale */
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

    /* Re-resolved by alias (not the -M lookup above) so the legacy
     * gemu-ia64 argv[0] entry point, which never sets args.machine and
     * falls back to the hardcoded "i2000" alias, still picks up i2000's
     * menu="disabled" default. */
    for (int i = 0; machine_defs[i].name; i++) {
        if (strcmp(alias, machine_defs[i].canonical) == 0) {
            cfg.menu_disabled = machine_defs[i].menu &&
                                 strcmp(machine_defs[i].menu, "disabled") == 0;
            break;
        }
    }

    if (args.cpu && strcmp(args.cpu, "merced") != 0 &&
        strcmp(args.cpu, "mckinley") != 0) {
        fprintf(stderr, "gemu: unknown ia64 CPU '%s' (use -cpu ? to list)\n", args.cpu);
        return 1;
    }

    /* The i2000 has a PS/2 mouse port, so like a real machine it comes with
     * a mouse attached; -nodefaults leaves it off.  (-device mouse is still
     * accepted for existing command lines.) */
    cfg.mouse_enabled = strcmp(alias, "i2000") == 0 && gemu_drive_defaults();

    RomScanCtx roms = {0};
    const char *firmware_file = NULL;
    if (args.rom_path && !ia64_rom_arg(args.rom_path, alias, &roms, &firmware_file))
        return 1;
    const char *serial_spec = NULL;
    for (int i = 0; i < nrem; i++) {
        if (strcmp(rem[i], "-rom") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -rom requires an argument\n"); return 1; }
            if (!ia64_rom_arg(rem[++i], alias, &roms, &firmware_file))
                return 1;
        } else if (strcmp(rem[i], "-m") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -m requires an argument\n"); return 1; }
            uint64_t sz = parse_size(rem[++i]);
            if (!sz) return 1;
            cfg.ram_size = sz;
        } else if (strcmp(rem[i], "-microprogram") == 0) {
            /* Architectural conformance harness - see src/ia64_selftest.c.
             * Runs bundles on a bare core and exits; no machine is created. */
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -microprogram requires an argument\n"); return 1; }
            return ia64_selftest_main(rem[++i]);
        } else if (strcmp(rem[i], "-net") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -net requires an argument\n"); return 1; }
            const char *net = rem[++i];
            if (strcmp(alias, "i2000") != 0 ||
                (strcmp(net, "nic,model=i82559") != 0 &&
                 strcmp(net, "nic,model=baset") != 0)) {
                fprintf(stderr,
                        "gemu: -net currently supports only "
                        "'nic,model=i82559' (or 'baset') on i2000\n");
                return 1;
            }
            cfg.i82559_enabled = true;
        } else if (strcmp(rem[i], "-serial") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -serial requires an argument\n"); return 1; }
            const char *spec = rem[++i];
            if (strcmp(spec, "stdio") != 0 && strncmp(spec, "tcp:", 4) != 0) {
                fprintf(stderr, "gemu: -serial: use 'stdio' or 'tcp:HOST:PORT'\n");
                return 1;
            }
            serial_spec = spec;
        } else if (strcmp(rem[i], "-device") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -device requires an argument\n"); return 1; }
            const char *dev = rem[++i];
            if (strcmp(dev, "mouse") == 0) {
                if (strcmp(alias, "i2000") != 0) {
                    fprintf(stderr, "gemu: -device mouse is supported by i2000 only\n");
                    return 1;
                }
                cfg.mouse_enabled = true;
            } else {
                fprintf(stderr, "gemu: unknown device '%s' (use -device ? to list)\n", dev);
                return 1;
            }
        } else {
            fprintf(stderr, "gemu: unknown option '%s' (try -h)\n", rem[i]);
            return 1;
        }
    }

    cfg.display_type = args.display_type;
    cfg.display_scale = args.display_scale;
    bool is_i2000 = strcmp(alias, "generic-ia64") != 0;
    /* The i2000's display is the Rage 128 and its own video BIOS; "std" is
     * the generic machine's plain VGA. */
    cfg.vga = args.vga ? args.vga : (is_i2000 ? "rage128" : "std");
    if (is_i2000 && strcmp(cfg.vga, "std") == 0) {
        fprintf(stderr, "gemu: %s has a Rage 128 display (-vga rage128) or none (-vga none)\n", alias);
        return 1;
    }
    cfg.no_shutdown = args.no_shutdown;
    cfg.jit = !args.accel || strcmp(args.accel, "jit") == 0;

    /* An explicit -rom FILE picks the firmware; otherwise the scans' only
     * match, or the first by file name when a folder holds several. */
    const char *rom_arg = firmware_file;
    if (!rom_arg && roms.n_firmware) {
        qsort(roms.firmware, (size_t)roms.n_firmware, sizeof(char *), cmp_path);
        rom_arg = roms.firmware[0];
        for (int i = 1; i < roms.n_firmware; i++)
            fprintf(stderr, "gemu: %s: using firmware '%s', not '%s' "
                    "(-rom FILE selects one)\n", alias, rom_arg, roms.firmware[i]);
    }
    cfg.video_rom = roms.video_rom;
    cfg.nic_rom = roms.nic_rom;

    if (serial_spec && strcmp(alias, "generic-ia64") != 0) {
        fprintf(stderr, "gemu: -serial is only supported by the generic machine\n");
        return 1;
    }

    bool sdl_up = (cfg.display_type == GEMU_DISPLAY_SDL);
    if (sdl_up && SDL_Init(0) < 0) {
        fprintf(stderr, "gemu: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (strcmp(alias, "generic-ia64") == 0) {
        GenericConfig gcfg = {
            .ram_size = cfg.ram_size,
            .display_type = cfg.display_type,
            .display_scale = cfg.display_scale,
            .no_shutdown = cfg.no_shutdown,
            .jit = cfg.jit,
            .cpu = args.cpu,
            .serial_spec = serial_spec,
            .menu_disabled = cfg.menu_disabled,
        };
        Ia64GenericState *g = ia64_generic_create(&gcfg);
        if (!g)
            return 1;
        if (!gemu_drive_check_unclaimed(alias)) {
            ia64_generic_destroy(g);
            return 1;
        }
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
    if (!gemu_drive_check_unclaimed(alias)) {
        ia64_i2000_destroy(s);
        return 1;
    }

    bool have_fw = false;
    if (rom_arg) {
        have_fw = ia64_i2000_load_firmware(s, rom_arg);
        if (!have_fw) {
            ia64_i2000_destroy(s);
            return 1;
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
