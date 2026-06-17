#include "mos6502cfg.h"
#include "hardware/generic.h"
#include "hardware/nes.h"
#include "hardware/nes_devices.h"
#include "hardware/romdb.h"
#include "gemu/gemu.h"
#include "gemu/args.h"
#include "gemu/monitor.h"
#include <SDL2/SDL.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Device registry ─────────────────────────────────────────────────────── */

static const GemuDevDesc machines[] = {
    {"famicom", "Nintendo Family Computer (Ricoh 2A03 + RP2C02, alias for nes)"},
    {"generic", "Generic MOS-compatible machine (flat 64 KB, ROM at user-specified address)"},
    {"nes",     "Nintendo Entertainment System (Ricoh 2A03 + RP2C02, NTSC)"},
    {"nespal",  "Nintendo Entertainment System (Ricoh 2A03 + RP2C02, PAL)"},
};
static const GemuDevDesc cpus[] = {
    {"6501", "MOS Technology 6501"},
    {"6502", "MOS Technology 6502"},
    {"2a03", "Ricoh 2A03 (6502-like, no decimal mode)"},
};
static const GemuDevDesc vgas[] = {
    {"2c02", "Ricoh RP2C02 (256x240 px, NES PPU)"},
};

static const GemuArgsDef def = {
    .prog       = "gemu-mos",
    .machines   = machines, .n_machines = (int)GEMU_ARRAY_LEN(machines),
    .cpus       = cpus,     .n_cpus     = (int)GEMU_ARRAY_LEN(cpus),
    .vgas       = vgas,     .n_vgas     = (int)GEMU_ARRAY_LEN(vgas),
    .display_mask = GEMU_DISP_F(GEMU_DISPLAY_SDL)
#ifdef GEMU_GTK
                  | GEMU_DISP_F(GEMU_DISPLAY_GTK)
#endif
#ifdef HAVE_CACA
                  | GEMU_DISP_F(GEMU_DISPLAY_CURSES)
#endif
                  | GEMU_DISP_F(GEMU_DISPLAY_NONE),
    .vnc_support  = true,
    .extra_help =
        "\nArguments:\n"
        "  -m SIZE            Amount of memory i.e. 4K, 32K, 64K, 1M, etc.\n"
        "                     Bare number (64) is treated as kilobytes. Default: 64K.\n"
        "  -rom ADDR:FILE     Load a ROM image at CPU address ADDR\n"
        "  -rom FILE          Load a ROM image (or optional FDS BIOS with -device fds)\n"
        "  -rom DIR           Directory containing ROM files (identified by SHA256)\n"
        "  -start ADDR        Override reset vector and start execution at ADDR\n"
        "  -cartridge FILE    Insert a cartridge\n"
        "  -fda FILE          Insert a floppy disk image\n"
        "  -renderer MODE     SDL renderer: auto | software | accelerated (default: auto)\n"
        "  -soundhw CHIP      Sound hardware: none | 2a03"
#ifdef HAVE_ALSA
        " | 2a03,output=midi"
#endif
        "  (default: 2a03 for NES)\n"
        "  -device NAME       Attach a device (use -device ? to list)\n"
        "\nExample commands:\n"
        "  ./bin/gemu-mos -M generic -rom 0xE000:rom.bin\n"
        "  ./bin/gemu-mos -rom 0x0000:6502_functional_test.bin -start 0x0400\n"
        "  ./bin/gemu-mos -M nes -cartridge game.nes -vnc :1\n"
        "  ./bin/gemu-mos -M nes -device fds -fda game.fds -device nes-controller\n",
};

/* ── Memory size parsing ─────────────────────────────────────────────────── */

static uint32_t parse_size(const char *s) {
    char *end;
    uint32_t v = (uint32_t)strtoul(s, &end, 0);
    if      (*end == 'K' || *end == 'k') v *= 1024u;
    else if (*end == 'M' || *end == 'm') v *= 1024u * 1024u;
    else if (*end == '\0')               v *= 1024u; /* bare number = kilobytes */
    if (v == 0 || v > 0x10000u) {
        fprintf(stderr, "gemu-mos: -m: invalid size '%s' (must be 1–64, with optional K/M suffix)\n", s);
        return 0;
    }
    return v;
}

/* ── ROM argument parsing ────────────────────────────────────────────────── */

static bool add_rom(MosConfig *cfg, uint32_t addr, const char *path) {
    if (cfg->n_roms >= MOS_MAX_ROM_LOADS) {
        fprintf(stderr, "gemu-mos: too many -rom loads (max %d)\n", MOS_MAX_ROM_LOADS);
        return false;
    }
    cfg->roms[cfg->n_roms].addr = addr;
    cfg->roms[cfg->n_roms].path = path;
    cfg->n_roms++;
    return true;
}

static bool parse_rom_arg(MosConfig *cfg, const char *arg) {
    uint32_t addr = 0;
    const char *path;
    if (gemu_parse_addr_arg("gemu-mos", arg, &addr, &path) < 0) return false;
    return add_rom(cfg, addr, path);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        char *fake[] = {argv[0], "-h"};
        int rem = 0;
        gemu_args_parse(2, fake, &def, &(GemuArgs){}, &rem, NULL);
        return 1;
    }

    MosConfig cfg = {
        .machine      = MOS_MACHINE_GENERIC,
        .cpu          = MOS_CPU_6502,
        .vga          = MOS_VGA_NONE,
        .display_type = GEMU_DISPLAY_NONE,
        .display_renderer = GEMU_RENDERER_AUTO,
        .display_scale = 1,
    };

    GemuArgs args = {
        .display_type  = cfg.display_type,
        .display_scale = cfg.display_scale,
    };

    char *rem[32]; int nrem = 0;
    if (!gemu_args_parse(argc, argv, &def, &args, &nrem, rem))
        return 1;
    gemu_monitor_set_default(args.monitor_spec);

    if (args.machine) {
        if      (strcmp(args.machine, "generic") == 0) cfg.machine = MOS_MACHINE_GENERIC;
        else if (strcmp(args.machine, "nes")     == 0) cfg.machine = MOS_MACHINE_NES;
        else if (strcmp(args.machine, "nespal")  == 0) { cfg.machine = MOS_MACHINE_NES; cfg.is_pal = true; }
        else if (strcmp(args.machine, "famicom") == 0) cfg.machine = MOS_MACHINE_NES;
    }

    if (args.cpu) {
        if      (strcmp(args.cpu, "6501") == 0) cfg.cpu = MOS_CPU_6501;
        else if (strcmp(args.cpu, "6502") == 0) cfg.cpu = MOS_CPU_6502;
        else if (strcmp(args.cpu, "2a03") == 0) cfg.cpu = MOS_CPU_2A03;
    } else if (cfg.machine == MOS_MACHINE_NES) {
        cfg.cpu = MOS_CPU_2A03;
    }

    if (args.vga) {
        if (strcmp(args.vga, "2c02") == 0) cfg.vga = MOS_VGA_RP2C02;
    }

    /* 6502-specific flags */
    for (int i = 0; i < nrem; i++) {
        if (strcmp(rem[i], "-m") == 0 && i + 1 < nrem) {
            uint32_t sz = parse_size(rem[++i]);
            if (!sz) return 1;
            cfg.mem_size = sz;
        } else if (strcmp(rem[i], "-rom") == 0 && i + 1 < nrem) {
            const char *val = rem[++i];
            struct stat st;
            if (stat(val, &st) == 0 && S_ISDIR(st.st_mode)) {
                const char *alias = args.machine ? args.machine : "generic";
                int n = mos_romdb_load_dir(&cfg, val, alias);
                if (n < 0) return 1;
                if (n == 0) {
                    fprintf(stderr, "gemu-mos: no known ROMs in '%s' for machine '%s'\n",
                            val, alias);
                    return 1;
                }
            } else {
                if (!parse_rom_arg(&cfg, val)) return 1;
            }
        } else if (strcmp(rem[i], "-start") == 0 && i + 1 < nrem) {
            cfg.start_addr     = (uint16_t)strtoul(rem[++i], NULL, 0);
            cfg.has_start_addr = true;
        } else if (strcmp(rem[i], "-cartridge") == 0 && i + 1 < nrem) {
            cfg.cart_path = rem[++i];
        } else if (strcmp(rem[i], "-fda") == 0 && i + 1 < nrem) {
            cfg.fda_path = rem[++i];
        } else if (strcmp(rem[i], "-renderer") == 0 && i + 1 < nrem) {
            const char *mode = rem[++i];
            if (strcmp(mode, "?") == 0) {
                printf("Available SDL renderers:\n");
                printf("  auto         Try accelerated rendering, fall back to software\n");
                printf("  software     Force SDL software renderer\n");
                printf("  accelerated  Require SDL accelerated renderer\n");
                SDL_Quit(); return 0;
            }
            if      (strcmp(mode, "auto") == 0)        cfg.display_renderer = GEMU_RENDERER_AUTO;
            else if (strcmp(mode, "software") == 0)    cfg.display_renderer = GEMU_RENDERER_SOFTWARE;
            else if (strcmp(mode, "accelerated") == 0) cfg.display_renderer = GEMU_RENDERER_ACCELERATED;
            else {
                fprintf(stderr, "gemu-mos: unknown -renderer '%s' (use -renderer ? to list)\n", mode);
                return 1;
            }
        } else if (strcmp(rem[i], "-device") == 0 && i + 1 < nrem) {
            const char *name = rem[++i];
            if (strcmp(name, "?") == 0) {
                int n = 0;
                const NesDeviceDesc *devs = nes_device_list(&n);
                int maxw = (int)strlen("fds");
                for (int d = 0; d < n; d++) {
                    int w = (int)strlen(devs[d].name);
                    if (w > maxw) maxw = w;
                }
                printf("Available devices:\n");
                printf("  %-*s  Famicom Disk System\n", maxw, "fds");
                for (int d = 0; d < n; d++)
                    printf("  %-*s  %s\n", maxw, devs[d].name, devs[d].desc);
                SDL_Quit(); return 0;
            }
            if (strcmp(name, "fds") == 0) {
                cfg.fds_enabled = true;
            } else {
                const NesDeviceDesc *dev = nes_device_find(name);
                if (!dev) {
                    fprintf(stderr, "gemu-mos: unknown device '%s' (try -device ?)\n", name);
                    return 1;
                }
                if (cfg.n_ports >= NES_PORTS) {
                    fprintf(stderr, "gemu-mos: all %d controller ports are already occupied\n", NES_PORTS);
                    return 1;
                }
                cfg.ports[cfg.n_ports++] = dev->type;
            }
        } else if (strcmp(rem[i], "-soundhw") == 0 && i + 1 < nrem) {
            const char *hw = rem[++i];
            if (strcmp(hw, "?") == 0) {
                printf("Available sound hardware:\n");
                printf("  2a03               Ricoh 2A03 APU -> SDL audio (default)\n");
#ifdef HAVE_ALSA
                printf("  2a03,output=midi   Ricoh 2A03 APU -> ALSA MIDI port\n");
#endif
                printf("  none               Disable sound output\n");
                SDL_Quit(); return 0;
            }
            if      (strcmp(hw, "none") == 0) { cfg.sound = MOS_SOUND_NONE; cfg.sound_explicit = true; }
            else if (strcmp(hw, "2a03") == 0) { cfg.sound = MOS_SOUND_2A03; cfg.sound_explicit = true; }
#ifdef HAVE_ALSA
            else if (strcmp(hw, "2a03,output=midi") == 0) { cfg.sound = MOS_SOUND_2A03_MIDI; cfg.sound_explicit = true; }
#endif
            else {
                fprintf(stderr, "gemu-mos: unknown -soundhw '%s' (use -soundhw ? to list)\n", hw);
                return 1;
            }
        } else if (strcmp(rem[i], "-ppu-debug") == 0) {
            cfg.ppu_debug = true;
        } else {
            fprintf(stderr, "gemu-mos: unknown option '%s' (try -h)\n", rem[i]);
            return 1;
        }
    }

    /* Positional ROM (single-file usage: gemu-mos rom.bin) */
    if (args.rom_path) {
        if (!add_rom(&cfg, 0x0000u, args.rom_path)) return 1;
    }

    cfg.display_type  = args.display_type;
    cfg.display_scale = args.display_scale;
    cfg.vnc_addr      = args.vnc_addr;

    /* NES without any display preference → GTK if available, else SDL */
    if (cfg.machine == MOS_MACHINE_NES
        && !cfg.vnc_addr && !args.display_explicit) {
#ifdef GEMU_GTK
        cfg.display_type = GEMU_DISPLAY_GTK;
#else
        cfg.display_type = GEMU_DISPLAY_SDL;
#endif
    }

    /* NES defaults: controller on port 0 if nothing was specified;
       zapper always lives on port 1 (physical port 2, $4017) — auto-insert
       a controller on port 0 when the zapper was specified alone. */
    if (cfg.machine == MOS_MACHINE_NES) {
        if (cfg.n_ports == 0) {
            cfg.ports[cfg.n_ports++] = NES_DEVICE_CONTROLLER;
        } else if (cfg.n_ports == 1 && cfg.ports[0] == NES_DEVICE_ZAPPER) {
            cfg.ports[1] = NES_DEVICE_ZAPPER;
            cfg.ports[0] = NES_DEVICE_CONTROLLER;
            cfg.n_ports  = 2;
        }
    }

    /* NES default sound: 2A03 when any output is active; silent when headless.
     * Skipped if the user already chose -soundhw explicitly. */
    if (cfg.machine == MOS_MACHINE_NES && !cfg.sound_explicit &&
        (cfg.cart_path || cfg.fds_enabled)) {
        bool has_output = (cfg.display_type == GEMU_DISPLAY_SDL ||
                           cfg.display_type == GEMU_DISPLAY_GTK);
        if (has_output)
            cfg.sound = MOS_SOUND_2A03;
    }

    /* Validate machine-specific requirements */
    if (cfg.machine == MOS_MACHINE_NES) {
        if (!cfg.cart_path && !cfg.fds_enabled) {
            fprintf(stderr, "gemu-mos: NES requires -cartridge FILE.nes or -device fds\n");
            return 1;
        }
        /* No -rom is fine — machine_nes will install the HLE stub automatically */
        if (cfg.fda_path && !cfg.fds_enabled) {
            fprintf(stderr, "gemu-mos: -fda requires -device fds\n");
            return 1;
        }
    } else {
        if (cfg.n_roms == 0) {
            fprintf(stderr, "gemu-mos: no ROM specified — use -rom ADDR:FILE\n");
            return 1;
        }
    }

    if (SDL_Init(0) < 0) {
        fprintf(stderr, "gemu-mos: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int rc = 0;
    if (cfg.machine == MOS_MACHINE_NES) {
        NesState *s = nes_create(&cfg);
        if (!s) { SDL_Quit(); return 1; }
        s->ppu.debug = cfg.ppu_debug;
        nes_run(s, &cfg);
        nes_destroy(s);
    } else {
        MosGenericState *s = mos_generic_create(&cfg);
        if (!s) { SDL_Quit(); return 1; }
        mos_generic_run(s, &cfg);
        mos_generic_destroy(s);
    }

    SDL_Quit();
    return rc;
}
