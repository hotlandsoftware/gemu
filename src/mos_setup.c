#include "mos6502cfg.h"
#include "machine_mos.h"
#include "nes.h"
#include "kim1.h"
#include "nes_devices.h"
#include "kim_devices.h"
#include "vt100.h"
#include "mos_romdb.h"
#include "gemu/gemu.h"
#include "gemu/args.h"
#include "gemu/monitor.h"
#include <SDL2/SDL.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    const char *name;
    const char *canonical;
    const char *cpu;
    const char *vga;
    const char *tv;
    const char *ram;
} MachineDef;

static const MachineDef machine_defs[] = {
#include "generated/machine_defaults.inc"
    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/* ── Device registry ─────────────────────────────────────────────────────── */

static const GemuDevDesc machines[] = {
    {"famicom", "Nintendo Family Computer (Ricoh 2A03 + RP2C02, alias for nes)"},
    {"kim1",    "MOS KIM-1 single-board computer (6502, 1 KB RAM, 2x 6530 RRIOT)"},
    {"mos",     "Generic MOS-compatible machine (flat 64 KB, ROM at user-specified address)"},
    {"nes",     "Nintendo Entertainment System (Ricoh 2A03 + RP2C02, NTSC)"},
    {"nespal",  "Nintendo Entertainment System (Ricoh 2A03 + RP2C02, PAL)"},
};
static const GemuDevDesc cpus[] = {
    {"6501", "MOS Technology 6501"},
    {"6502", "MOS Technology 6502"},
    {"2a03", "Ricoh 2A03 (6502-like, no decimal mode)"},
};
static const GemuDevDesc vgas[] = {
#include "generated/vgas_mos.inc"
};

static const GemuArgsDef def = {
    .prog       = "gemu",
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
        "  -tape FILE         Insert a casette tape\n"
        "  -renderer MODE     Set SDL renderer: auto | software | accelerated (default: auto)\n"
        "  -soundhw CHIP      Insert a sound card"
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
        " | 2a03,output=midi"
#endif
        "  (default: 2a03 for NES)\n"
        "  -device NAME       Attach a device (use -device ? to list)\n"
        "\nExample commands:\n"
        "  ./bin/gemu -M mos -rom 0xE000:rom.bin\n"
        "  ./bin/gemu -M mos -rom 0x0000:6502_functional_test.bin -start 0x0400\n"
        "  ./bin/gemu -M nes -cartridge game.nes -vnc :1\n"
        "  ./bin/gemu -M nes -device fds -fda game.fds -device nes-controller\n",
};

/* ── Memory size parsing ─────────────────────────────────────────────────── */

static uint32_t parse_size(const char *s) {
    char *end;
    uint32_t v = (uint32_t)strtoul(s, &end, 0);
    if      (*end == 'K' || *end == 'k') v *= 1024u;
    else if (*end == 'M' || *end == 'm') v *= 1024u * 1024u;
    else if (*end == '\0')               v *= 1024u;
    if (v == 0 || v > 0x10000u) {
        fprintf(stderr, "gemu: -m: invalid size '%s' (must be 1–64, with optional K/M suffix)\n", s);
        return 0;
    }
    return v;
}

/* ── ROM argument parsing ────────────────────────────────────────────────── */

static bool add_rom(MosConfig *cfg, uint32_t addr, const char *path) {
    if (cfg->n_roms >= MOS_MAX_ROM_LOADS) {
        fprintf(stderr, "gemu: too many -rom loads (max %d)\n", MOS_MAX_ROM_LOADS);
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
    if (gemu_parse_addr_arg("gemu", arg, &addr, &path) < 0) return false;
    return add_rom(cfg, addr, path);
}

/* ── mos_setup ───────────────────────────────────────────────────────────── */

int mos_setup(int argc, char *argv[]) {
    if (argc < 2) {
        char *fake[] = {argv[0], "-h"};
        int rem = 0;
        gemu_args_parse(2, fake, &def, &(GemuArgs){}, &rem, NULL);
        return 1;
    }

    bool want_vt100 = false;

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
        for (int i = 0; machine_defs[i].name; i++) {
            if (strcmp(args.machine, machine_defs[i].name) != 0) continue;
            const MachineDef *md = &machine_defs[i];
            const char *canon = md->canonical;
            if      (strcmp(canon, "mos")  == 0) cfg.machine = MOS_MACHINE_GENERIC;
            else if (strcmp(canon, "kim1") == 0) cfg.machine = MOS_MACHINE_KIM1;
            else if (strcmp(canon, "nes")  == 0) cfg.machine = MOS_MACHINE_NES;
            cfg.is_pal = md->tv && strcmp(md->tv, "pal") == 0;
            if (!args.cpu && md->cpu) {
                if      (strcmp(md->cpu, "6501") == 0) cfg.cpu = MOS_CPU_6501;
                else if (strcmp(md->cpu, "6502") == 0) cfg.cpu = MOS_CPU_6502;
                else if (strcmp(md->cpu, "2a03") == 0) cfg.cpu = MOS_CPU_2A03;
            }
            if (!args.vga && md->vga) {
                if (strcmp(md->vga, "rp2c02") == 0) cfg.vga = MOS_VGA_RP2C02;
            }
            break;
        }
    }

    if (args.cpu) {
        if      (strcmp(args.cpu, "6501") == 0) cfg.cpu = MOS_CPU_6501;
        else if (strcmp(args.cpu, "6502") == 0) cfg.cpu = MOS_CPU_6502;
        else if (strcmp(args.cpu, "2a03") == 0) cfg.cpu = MOS_CPU_2A03;
    }

    if (args.vga) {
        if (strcmp(args.vga, "rp2c02") == 0) cfg.vga = MOS_VGA_RP2C02;
    }

    for (int i = 0; i < nrem; i++) {
        if (strcmp(rem[i], "-m") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -m requires an argument\n"); return 1; }
            uint32_t sz = parse_size(rem[++i]);
            if (!sz) return 1;
            cfg.mem_size = sz;
        } else if (strcmp(rem[i], "-rom") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -rom requires an argument\n"); return 1; }
            const char *val = rem[++i];
            struct stat st;
            if (stat(val, &st) == 0 && S_ISDIR(st.st_mode)) {
                const char *alias = args.machine ? args.machine : "mos";
                int n = mos_romdb_load_dir(&cfg, val, alias);
                if (n < 0) return 1;
                if (n == 0) {
                    fprintf(stderr, "gemu: no known ROMs in '%s' for machine '%s'\n",
                            val, alias);
                    return 1;
                }
            } else {
                if (!parse_rom_arg(&cfg, val)) return 1;
            }
        } else if (strcmp(rem[i], "-start") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -start requires an argument\n"); return 1; }
            cfg.start_addr     = (uint16_t)strtoul(rem[++i], NULL, 0);
            cfg.has_start_addr = true;
        } else if (strcmp(rem[i], "-cartridge") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -cartridge requires an argument\n"); return 1; }
            cfg.cart_path = rem[++i];
        } else if (strcmp(rem[i], "-fda") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -fda requires an argument\n"); return 1; }
            cfg.fda_path = rem[++i];
        } else if (strcmp(rem[i], "-tape") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -tape requires an argument\n"); return 1; }
            cfg.tape_path = rem[++i];
        } else if (strcmp(rem[i], "-renderer") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -renderer requires an argument\n"); return 1; }
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
                fprintf(stderr, "gemu: unknown -renderer '%s' (use -renderer ? to list)\n", mode);
                return 1;
            }
        } else if (strcmp(rem[i], "-device") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -device requires an argument\n"); return 1; }
            const char *name = rem[++i];
            if (strcmp(name, "?") == 0) {
                int n_nes = 0, n_kim = 0;
                const NesDeviceDesc *ndevs = nes_device_list(&n_nes);
                const KimDeviceDesc *kdevs = kim_device_list(&n_kim);

                typedef struct { const char *name; const char *desc; } DevEntry;
                DevEntry all[64];
                int n_all = 0;
                all[n_all++] = (DevEntry){"fds",    "Famicom Disk System"};
                all[n_all++] = (DevEntry){"vt100",  "DEC VT100 serial terminal (second window)"};
                all[n_all++] = (DevEntry){"wozmon", "Wozniak Monitor"};
                for (int d = 0; d < n_nes; d++)
                    all[n_all++] = (DevEntry){ndevs[d].name, ndevs[d].desc};
                for (int d = 0; d < n_kim; d++)
                    all[n_all++] = (DevEntry){kdevs[d].name, kdevs[d].desc};

                int maxw = 0;
                for (int d = 0; d < n_all; d++) {
                    int w = (int)strlen(all[d].name);
                    if (w > maxw) maxw = w;
                }

                for (int i2 = 1; i2 < n_all; i2++) {
                    DevEntry tmp = all[i2];
                    int j = i2 - 1;
                    while (j >= 0 && strcmp(all[j].name, tmp.name) > 0) {
                        all[j + 1] = all[j];
                        j--;
                    }
                    all[j + 1] = tmp;
                }

                printf("Available devices:\n");
                for (int d = 0; d < n_all; d++)
                    printf("  %-*s  %s\n", maxw, all[d].name, all[d].desc);
                SDL_Quit(); return 0;
            }
            if (strcmp(name, "fds") == 0) {
                cfg.fds_enabled = true;
            } else if (strcmp(name, "vt100") == 0) {
                want_vt100 = true;
            } else if (strcmp(name, "wozmon") == 0) {
                cfg.want_wozmon = true;
            } else if (kim_device_find(name)) {
                cfg.kim_keyboard = true;
            } else {
                const NesDeviceDesc *dev = nes_device_find(name);
                if (!dev) {
                    fprintf(stderr, "gemu: unknown device '%s' (try -device ?)\n", name);
                    return 1;
                }
                if (cfg.n_ports >= NES_PORTS) {
                    fprintf(stderr, "gemu: all %d controller ports are already occupied\n", NES_PORTS);
                    return 1;
                }
                cfg.ports[cfg.n_ports++] = dev->type;
            }
        } else if (strcmp(rem[i], "-soundhw") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -soundhw requires an argument\n"); return 1; }
            const char *hw = rem[++i];
            if (strcmp(hw, "?") == 0) {
                printf("Available sound hardware:\n");
                printf("  2a03               Ricoh 2A03 APU -> SDL audio (default)\n");
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
                printf("  2a03,output=midi   Ricoh 2A03 APU -> MIDI output\n");
#endif
                printf("  none               Disable sound output\n");
                SDL_Quit(); return 0;
            }
            if      (strcmp(hw, "none") == 0) { cfg.sound = MOS_SOUND_NONE; cfg.sound_explicit = true; }
            else if (strcmp(hw, "2a03") == 0) { cfg.sound = MOS_SOUND_2A03; cfg.sound_explicit = true; }
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
            else if (strcmp(hw, "2a03,output=midi") == 0) { cfg.sound = MOS_SOUND_2A03_MIDI; cfg.sound_explicit = true; }
#endif
            else {
                fprintf(stderr, "gemu: unknown -soundhw '%s' (use -soundhw ? to list)\n", hw);
                return 1;
            }
        } else if (strcmp(rem[i], "-ppu-debug") == 0) {
            cfg.ppu_debug = true;
        } else {
            fprintf(stderr, "gemu: unknown option '%s' (try -h)\n", rem[i]);
            return 1;
        }
    }

    if (args.rom_path) {
        if (!add_rom(&cfg, 0x0000u, args.rom_path)) return 1;
    }

    cfg.display_type  = args.display_type;
    cfg.display_scale = args.display_scale;
    cfg.vnc_addr      = args.vnc_addr;

    if ((cfg.machine == MOS_MACHINE_NES || cfg.machine == MOS_MACHINE_KIM1)
        && !cfg.vnc_addr && !args.display_explicit) {
#ifdef GEMU_GTK
        cfg.display_type = GEMU_DISPLAY_GTK;
#else
        cfg.display_type = GEMU_DISPLAY_SDL;
#endif
    }

    if (cfg.machine == MOS_MACHINE_NES) {
        if (cfg.n_ports == 0) {
            cfg.ports[cfg.n_ports++] = NES_DEVICE_CONTROLLER;
        } else if (cfg.n_ports == 1 && cfg.ports[0] == NES_DEVICE_ZAPPER) {
            cfg.ports[1] = NES_DEVICE_ZAPPER;
            cfg.ports[0] = NES_DEVICE_CONTROLLER;
            cfg.n_ports  = 2;
        }
    }

    if (cfg.machine == MOS_MACHINE_NES && !cfg.sound_explicit &&
        (cfg.cart_path || cfg.fds_enabled)) {
        bool has_output = (cfg.display_type == GEMU_DISPLAY_SDL ||
                           cfg.display_type == GEMU_DISPLAY_GTK);
        if (has_output)
            cfg.sound = MOS_SOUND_2A03;
    }

    if (cfg.machine == MOS_MACHINE_NES) {
        if (!cfg.cart_path && !cfg.fds_enabled) {
            fprintf(stderr, "gemu: NES requires -cartridge FILE.nes or -device fds\n");
            return 1;
        }
        if (cfg.fda_path && !cfg.fds_enabled) {
            fprintf(stderr, "gemu: -fda requires -device fds\n");
            return 1;
        }
    } else {
        if (cfg.n_roms == 0) {
            fprintf(stderr, "gemu: no ROM specified — use -rom ADDR:FILE\n");
            return 1;
        }
    }

    if (SDL_Init(0) < 0) {
        fprintf(stderr, "gemu: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (cfg.want_wozmon) {
        if (!cfg.has_start_addr) {
            cfg.start_addr     = 0x1AA0u;
            cfg.has_start_addr = true;
        }
        if (!want_vt100)
            want_vt100 = true; /* WozMon requires a serial terminal for I/O */
    }

    Vt100State *vt100 = NULL;
    GemuSerial  vt100_serial;
    if (want_vt100) {
        const char *vt_title = cfg.want_wozmon ? "GEMU (Wozniak Monitor)" : NULL;
        vt100 = vt100_create(cfg.display_type, vt_title);
        if (!vt100) { SDL_Quit(); return 1; }
        vt100_as_serial(vt100, &vt100_serial);
        cfg.serial = &vt100_serial;
    }

    int rc = 0;
    if (cfg.machine == MOS_MACHINE_NES) {
        NesState *s = nes_create(&cfg);
        if (!s) { vt100_destroy(vt100); SDL_Quit(); return 1; }
        s->ppu.debug = cfg.ppu_debug;
        nes_run(s, &cfg);
        nes_destroy(s);
    } else if (cfg.machine == MOS_MACHINE_KIM1) {
        Kim1State *s = kim1_create(&cfg);
        if (!s) { vt100_destroy(vt100); SDL_Quit(); return 1; }
        kim1_run(s, &cfg);
        kim1_destroy(s);
    } else {
        MosGenericState *s = mos_generic_create(&cfg);
        if (!s) { vt100_destroy(vt100); SDL_Quit(); return 1; }
        mos_generic_run(s, &cfg);
        mos_generic_destroy(s);
    }

    vt100_destroy(vt100);
    SDL_Quit();
    return rc;
}
