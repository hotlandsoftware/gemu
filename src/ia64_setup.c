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

static const GemuArgsDef def = {
    .prog         = "gemu",
    .machines     = machines,
    .n_machines   = (int)(sizeof machines / sizeof *machines),
    .cpus         = cpus,
    .n_cpus       = (int)(sizeof cpus / sizeof *cpus),
    .vgas         = vgas,
    .n_vgas       = (int)(sizeof vgas / sizeof *vgas),
    .display_mask = GEMU_DISP_F(GEMU_DISPLAY_SDL) | GEMU_DISP_F(GEMU_DISPLAY_NONE)
#ifdef GEMU_GTK
                  | GEMU_DISP_F(GEMU_DISPLAY_GTK)
#endif
    ,
    .vnc_support  = true,
    .extra_help =
        "\nIA64 options:\n"
        "  -rom FILE       Load firmware flash image (top-aligned, max 4 MiB)\n"
        "  -rom DIR|ZIP    Scan for known firmware by SHA-256\n"
        "  -m SIZE         RAM size, K/M/G suffix (default 512M)\n"
        "  -cdrom FILE     Attach a read-only ISO image as an ATAPI CD-ROM\n"
        "                  (alias for -drive file=FILE,if=ide,media=cdrom)\n"
        "  -drive file=FILE,if=ide,media=disk|cdrom[,index=0][,readonly=on|off]\n"
        "                  QEMU-style drive attach. i2000 exposes one IDE\n"
        "                  drive per channel (index=0 only): media=disk\n"
        "                  goes to the primary ATA HDD (like -hda), media=\n"
        "                  cdrom goes to the secondary ATAPI CD-ROM (like\n"
        "                  -cdrom)\n"
        "  -net nic,model=i82559\n"
        "                  Attach the i2000 Intel 82559 NIC (baset alias)\n"
        "  -microprogram F Run an architectural microprogram on a bare core\n"
        "                  and print the resulting state (conformance testing;\n"
        "                  see tools/ia64_conformance.py)\n"
        "  -hda FILE       Attach a raw read-write disk image (512-byte\n"
        "                  sectors), e.g. one made with\n"
        "                  'qemu-img create -f raw winxp.img 5G'\n"
        "  -serial stdio   generic machine only: attach COM1 to host stdio\n"
        "                  (casual use only - shares stdout with the VGA\n"
        "                  text mirror, not a clean debugger transport)\n"
        "  -serial tcp:HOST:PORT\n"
        "                  generic machine only: attach COM1 to a listening\n"
        "                  TCP socket (a clean full-duplex byte stream, for\n"
        "                  a real Windows kernel debugger to connect to)\n"
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

    const char *rom_arg = args.rom_path;
    const char *serial_spec = NULL;
    /* Backing storage for -drive's parsed file= path: cfg.hda_path/
     * cdrom_path just hold a pointer, and GemuDriveSpec is stack-local to
     * the branch below, so the string needs a buffer that outlives the
     * loop. */
    char drive_hda_file[512] = {0};
    char drive_cdrom_file[512] = {0};
    for (int i = 0; i < nrem; i++) {
        if (strcmp(rem[i], "-rom") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -rom requires an argument\n"); return 1; }
            rom_arg = rem[++i];
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
        } else if (strcmp(rem[i], "-hda") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -hda requires an argument\n"); return 1; }
            if (cfg.hda_path) {
                fprintf(stderr, "gemu: -hda/-drive: hard disk already attached\n");
                return 1;
            }
            cfg.hda_path = rem[++i];
        } else if (strcmp(rem[i], "-cdrom") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -cdrom requires an argument\n"); return 1; }
            if (cfg.cdrom_path) {
                fprintf(stderr, "gemu: -cdrom/-drive: CD-ROM drive already attached\n");
                return 1;
            }
            cfg.cdrom_path = rem[++i];
        } else if (strcmp(rem[i], "-drive") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -drive requires an argument\n"); return 1; }
            GemuDriveSpec spec;
            if (!gemu_parse_drive_spec(rem[++i], &spec))
                return 1;
            if (spec.if_type != GEMU_DRIVE_IF_IDE) {
                fprintf(stderr, "gemu: -drive: i2000 only exposes an IDE bus (if=ide)\n");
                return 1;
            }
            if (spec.index != 0) {
                fprintf(stderr, "gemu: -drive: i2000 emulates a single drive per "
                                "IDE channel (index=0 only) - no slave/secondary "
                                "devices\n");
                return 1;
            }
            if (spec.media == GEMU_DRIVE_MEDIA_CDROM) {
                if (cfg.cdrom_path) {
                    fprintf(stderr, "gemu: -drive/-cdrom: CD-ROM drive already attached\n");
                    return 1;
                }
                snprintf(drive_cdrom_file, sizeof(drive_cdrom_file), "%s", spec.file);
                cfg.cdrom_path = drive_cdrom_file;
            } else {
                if (spec.readonly) {
                    fprintf(stderr, "gemu: -drive: media=disk with readonly=on is not "
                                    "supported (i2000's ATA HDD is always read-write)\n");
                    return 1;
                }
                if (cfg.hda_path) {
                    fprintf(stderr, "gemu: -drive/-hda: hard disk already attached\n");
                    return 1;
                }
                snprintf(drive_hda_file, sizeof(drive_hda_file), "%s", spec.file);
                cfg.hda_path = drive_hda_file;
            }
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
    cfg.vnc_addr = args.vnc_addr;
    cfg.vga = args.vga ? args.vga : "std";
    cfg.no_shutdown = args.no_shutdown;

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
            .cdrom_path = cfg.cdrom_path,
            .hda_path = cfg.hda_path,
            .cpu = args.cpu,
            .serial_spec = serial_spec,
            .vnc_addr = args.vnc_addr,
            .menu_disabled = cfg.menu_disabled,
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
