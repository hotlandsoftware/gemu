#include "ibm5150.h"
#include "romdb.h"
#include "gemu/args.h"
#include "gemu/monitor.h"
#include "gemu/drive.h"
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
    .vga_options      = true,
    .block_devices    = true,
    .block_default_if = GEMU_DRIVE_IF_IDE,   /* QEMU's PC default */
    .display_mask = GEMU_DISP_F(GEMU_DISPLAY_SDL) | GEMU_DISP_F(GEMU_DISPLAY_NONE),
    .extra_help =
        "\nx86 options:\n"
        "  -rom FILE|DIR   Load a BIOS/BASIC image, or scan a directory for\n"
        "                  known ROMs by SHA-256 (e.g. -rom roms/ibm5150)\n"
        "  -m SIZE         RAM size, K/M suffix (default 64K; the 19OCT81 BIOS\n"
        "                  uses up to 544K, and POST takes ~50 s to test that)\n"
        "\nDiskettes: -fda/-fdb FILE or -drive file=FILE,if=floppy[,index=N]\n"
        "                  (160K/180K/320K/360K images; up to 4 drives).\n"
        "                  With a drive in A: POST boots from it, otherwise\n"
        "                  into Cassette BASIC.\n"
        "\nHard disks: -device ibm-fixed-disk -hda FILE [-hdb FILE]\n"
        "                  The IBM Fixed Disk Adapter (needs the 10/27/82 BIOS and\n"
        "                  the adapter ROM, found by -rom roms/ibm5150).  Each\n"
        "                  image's size picks its drive type from the ROM's table\n"
        "                  (5000059: 5, 10, 15 or 26 MB); type0=/type1=N choose\n"
        "                  one, romfile=FILE another ROM, interleave=N (default 6)\n"
        "                  how unformatted images are laid out.\n"
        "\nVideo: -vga cga (default) or -vga mda[,phosphor=green|amber|white]\n"
        "                  (the MDA's monitor: IBM 5151 green, amber or white).\n"
        "                  Add the other card with -device mda[,phosphor=...] or\n"
        "                  -device cga for a second monitor; the View menu (or\n"
        "                  Ctrl+Alt+1/2) switches between them.\n"
        "Not yet emulated: cassette, printer and serial ports.  Monitor:\n"
        "'sendkey' types into the keyboard (sendkey PRINT spc 2+2 ret,\n"
        "sendkey ctrl-alt-delete); 'change floppy0 FILE' swaps diskettes.\n"
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
        .ram_size = 64u << 10,   /* Cassette BASIC uses at most 64K, and more
                                    * RAM only lengthens POST's memory test */
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
    if (args.vga && strcmp(args.vga, "mda") == 0) {
        cfg.vga = X86_VGA_MDA;
        cfg.vga_opts = args.vga_opts;
    } else if (args.vga_opts) {
        fprintf(stderr, "gemu: -vga %s takes no options\n", args.vga);
        return 1;
    } else if (args.vga && strcmp(args.vga, "cga") != 0) {
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
        } else if (strcmp(rem[i], "-device") == 0) {
            /* A second display adapter on its own monitor: -device mda[,opts]
             * or -device cga, the one -vga didn't pick. */
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -device requires an argument\n"); return 1; }
            const char *dev = rem[++i];
            const char *comma = strchr(dev, ',');
            size_t len = comma ? (size_t)(comma - dev) : strlen(dev);
            if (len == 14 && strncmp(dev, "ibm-fixed-disk", 14) == 0) {
                if (cfg.hdc != X86_HDC_NONE) {
                    fprintf(stderr, "gemu: -device ibm-fixed-disk: only one hard disk controller fits\n");
                    return 1;
                }
                cfg.hdc = X86_HDC_IBM;
                cfg.hdc_opts = comma ? comma + 1 : NULL;
                continue;
            }
            X86VgaType type;
            if (len == 3 && strncmp(dev, "mda", 3) == 0)      type = X86_VGA_MDA;
            else if (len == 3 && strncmp(dev, "cga", 3) == 0) type = X86_VGA_CGA;
            else {
                fprintf(stderr, "gemu: unknown x86 device '%.*s' (use -device ? to list)\n",
                        (int)len, dev);
                return 1;
            }
            if (comma && type != X86_VGA_MDA) {
                fprintf(stderr, "gemu: -device cga takes no options\n");
                return 1;
            }
            cfg.has_vga2 = true;
            cfg.vga2 = type;
            cfg.vga2_opts = comma ? comma + 1 : NULL;
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

    if (cfg.has_vga2 && cfg.vga2 == cfg.vga) {
        fprintf(stderr, "gemu: -device %s: that card is already the -vga adapter "
                        "(the second card must be the other of cga/mda)\n",
                cfg.vga2 == X86_VGA_MDA ? "mda" : "cga");
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
    if (!s || !gemu_drive_check_unclaimed(alias)) {
        ibm5150_destroy(s);
        if (sdl_up) SDL_Quit();
        return 1;
    }
    ibm5150_run(s, &cfg);
    ibm5150_destroy(s);
    if (sdl_up) SDL_Quit();
    return 0;
}
