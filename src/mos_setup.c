#include "mos6502cfg.h"
#include "machine_mos.h"
#include "machine_apple1.h"
#include "nes.h"
#include "kim1.h"
#include "5clown.h"
#include "7mezzo.h"
#include "vt100.h"
#include "romdb.h"
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

/* ── Device registry ─────────────────────────────────────────────────────── */

static const GemuDevDesc machines[] = {
    {"5clown",  "Five Clown"},
    {"7mezzo",  "7 e Mezzo"},
    {"apple1",  "Apple I (MOS 6502, keyboard/display terminal)"},
    {"dendy",   "Dendy NES Famiclone (PAL 50 Hz, NTSC-compatible PPU/APU timing)"},
    {"vssmb",       "VS. Super Mario Bros. (coin-op arcade cabinet, DIP switches)"},
    {"vsskatekids", "VS. Skate Kid Bros. (arcade hack of VS. SMB)"},
    {"famicom", "Nintendo Family Computer (Ricoh 2A03 + RP2C02, alias for nes)"},
    {"kim1",    "MOS KIM-1 single-board computer (6502, 1 KB RAM, 2x 6530 RRIOT)"},
    {"mos",     "Generic MOS-compatible machine (flat 64 KB, ROM at user-specified address)"},
    {"nes",     "Nintendo Entertainment System (Ricoh 2A03 + RP2C02, NTSC)"},
    {"nespal",  "Nintendo Entertainment System (Ricoh 2A07 + RP2C02, PAL)"},
};
static const GemuDevDesc cpus[] = {
    {"6501", "MOS Technology 6501"},
    {"6502", "MOS Technology 6502"},
    {"2a03", "Ricoh 2A03 (6502-like, no decimal mode)"},
    {"2a07", "Ricoh 2A07 (PAL NES CPU/APU, no decimal mode)"},
};
static const GemuDevDesc vgas[] = {
#include "generated/vgas_mos.inc"
};
static const GemuDevDesc chargens[] = {
#include "generated/chargens_mos.inc"
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
        "  -tape [ADDR:]FILE  Insert a cassette tape\n"
#ifdef HAVE_LUA
        "  -lua FILE          Run an FCEUX-Lua-subset script (NES only)\n"
#endif
        "  -cg NAME           Character generator\n"
        "  -vga NAME          Graphics card\n"
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
        "  ./bin/gemu -M nes -device fds -fda game.fds -device nes-controller\n"
#ifdef HAVE_LUA
        "  ./bin/gemu -M nes -cartridge game.nes -lua script.lua\n"
#endif
        "  ./bin/gemu -M 5clown -rom roms/5clown\n"
        "  ./bin/gemu -M 7mezzo -rom roms/7mezzo\n"
        ,
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

static bool parse_soundhw(const char *hw, MosSoundType *out, uint32_t *mask) {
    if (strcmp(hw, "none") == 0) {
        *out = MOS_SOUND_NONE;
        *mask = 0;
        return true;
    }
    if (strcmp(hw, "2a03") == 0) {
        *out = MOS_SOUND_2A03;
        return true;
    }
    if (strcmp(hw, "pcspk") == 0) {
        *out = MOS_SOUND_PCSPK;
        return true;
    }
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
    if (strcmp(hw, "2a03,output=midi") == 0) {
        *out = MOS_SOUND_2A03_MIDI;
        return true;
    }
#endif
    /* Composable arcade sound cards (QEMU -soundhw-style: comma-separated,
     * independently present chips that OR together, e.g. "ay8910,msm6295"
     * for 5clown's onboard PSG + ADPCM). Unlike the exact-string cases
     * above, these don't replace *out — they only ever set *mask.
     * "msm6295" is the official chip name; the source/internal symbols
     * still say OKI6295/okim6295.c (OKI is just the manufacturer). */
    char buf[128];
    if (snprintf(buf, sizeof(buf), "%s", hw) >= (int)sizeof(buf)) return false;
    uint32_t new_mask = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if      (strcmp(tok, "ay8910")  == 0) new_mask |= MOS_SOUNDHW_AY8910;
        else if (strcmp(tok, "msm6295") == 0) new_mask |= MOS_SOUNDHW_OKI6295;
        else return false;
    }
    if (new_mask == 0) return false;
    *mask = new_mask;
    return true;
}

static bool parse_cpu(const char *name, MosCpuType *out) {
    if (strcmp(name, "6501") == 0) {
        *out = MOS_CPU_6501;
        return true;
    }
    if (strcmp(name, "6502") == 0) {
        *out = MOS_CPU_6502;
        return true;
    }
    if (strcmp(name, "2a03") == 0) {
        *out = MOS_CPU_2A03;
        return true;
    }
    if (strcmp(name, "2a07") == 0) {
        *out = MOS_CPU_2A07;
        return true;
    }
    return false;
}

static bool parse_cg(const char *name, MosCharGenType *out) {
    if (strcmp(name, "auto") == 0) {
        *out = MOS_CG_AUTO;
        return true;
    }
    if (strcmp(name, "cm2140") == 0 || strcmp(name, "s2513") == 0 ||
        strcmp(name, "signetics2513") == 0) {
        *out = MOS_CG_CM2140;
        return true;
    }
    if (strcmp(name, "cm2140-compat") == 0) {
        *out = MOS_CG_CM2140_COMPAT;
        return true;
    }
    return false;
}

static bool attach_device(MosConfig *cfg, bool *want_vt100, const char *name,
                          bool quiet) {
    if (strcmp(name, "fds") == 0) {
        cfg->fds_enabled = true;
        return true;
    }
    if (strcmp(name, "vt100") == 0) {
        *want_vt100 = true;
        return true;
    }
    if (strcmp(name, "wozmon") == 0) {
        cfg->want_wozmon = true;
        return true;
    }
    if (strcmp(name, "a1ci") == 0) {
        cfg->a1ci = true;
        return true;
    }
    if (strcmp(name, "kim-keypad") == 0 || strcmp(name, "keypad") == 0) {
        cfg->kim_keyboard = true;
        return true;
    }

    static const struct { const char *name; NesDeviceType type; } nes_devs[] = {
        {"nes-controller",   NES_DEVICE_CONTROLLER},
        {"zapper",           NES_DEVICE_ZAPPER},
        {"famicom-keyboard", NES_DEVICE_KEYBOARD},
        {"rob",              NES_DEVICE_ROB},
        {"rob-famicom",      NES_DEVICE_ROB_FAMICOM},
        {"famicom-mic",      NES_DEVICE_FC2_MIC},
    };
    NesDeviceType type = NES_DEVICE_NONE;
    for (int d = 0; d < (int)(sizeof(nes_devs)/sizeof(nes_devs[0])); d++)
        if (strcmp(name, nes_devs[d].name) == 0) { type = nes_devs[d].type; break; }
    if (type == NES_DEVICE_NONE) {
        if (!quiet) fprintf(stderr, "gemu: unknown device '%s' (try -device ?)\n", name);
        return false;
    }
    if (cfg->n_ports >= NES_PORTS) {
        if (!quiet) fprintf(stderr, "gemu: all %d controller ports are already occupied\n", NES_PORTS);
        return false;
    }
    cfg->ports[cfg->n_ports++] = type;
    return true;
}

static bool attach_default_devices(MosConfig *cfg, bool *want_vt100,
                                   const char *devices) {
    if (!devices || !devices[0]) return true;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", devices);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        if (*tok && !attach_device(cfg, want_vt100, tok, false))
            return false;
    }
    return true;
}

/* ── ROM argument parsing ────────────────────────────────────────────────── */

static bool add_rom(MosConfig *cfg, uint32_t addr, const char *path) {
    if (cfg->n_roms >= MOS_MAX_ROM_LOADS) {
        fprintf(stderr, "gemu: too many -rom loads (max %d)\n", MOS_MAX_ROM_LOADS);
        return false;
    }
    cfg->roms[cfg->n_roms].addr = addr;
    cfg->roms[cfg->n_roms].path = path;
    cfg->roms[cfg->n_roms].region = NULL;
    cfg->n_roms++;
    return true;
}

static bool romdb_add_mos(const char *path, const char *region, uint32_t addr, void *ud) {
    MosConfig *cfg = ud;
    if (cfg->n_roms >= MOS_MAX_ROM_LOADS) {
        fprintf(stderr, "gemu: too many ROMs (max %d)\n", MOS_MAX_ROM_LOADS);
        return false;
    }
    char *p = strdup(path);
    if (!p) return false;
    cfg->roms[cfg->n_roms].path = p;
    cfg->roms[cfg->n_roms].addr = addr;
    cfg->roms[cfg->n_roms].region = region;
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
    const char *romdb_machine = "mos";

    char *rem[32]; int nrem = 0;
    if (!gemu_args_parse(argc, argv, &def, &args, &nrem, rem))
        return 1;
    gemu_monitor_set_default(args.monitor_spec);

    if (args.machine) {
        for (int i = 0; machine_defs[i].name; i++) {
            if (strcmp(args.machine, machine_defs[i].name) != 0) continue;
            const MachineDef *md = &machine_defs[i];
            const char *canon = md->canonical;
            romdb_machine = canon;
            if      (strcmp(canon, "apple1") == 0) cfg.machine = MOS_MACHINE_APPLE1;
            else if (strcmp(canon, "mos")  == 0) cfg.machine = MOS_MACHINE_GENERIC;
            else if (strcmp(canon, "kim1") == 0) cfg.machine = MOS_MACHINE_KIM1;
            else if (strcmp(canon, "nes")  == 0) cfg.machine = MOS_MACHINE_NES;
            else if (strcmp(canon, "5clown") == 0) cfg.machine = MOS_MACHINE_5CLOWN;
            else if (strcmp(canon, "7mezzo") == 0) cfg.machine = MOS_MACHINE_7MEZZO;
            cfg.is_dendy = md->tv && strcmp(md->tv, "dendy") == 0;
            cfg.is_arcade = md->tv && strcmp(md->tv, "vs") == 0;
            cfg.is_pal   = cfg.is_dendy || (md->tv && strcmp(md->tv, "pal") == 0);
            if (!args.cpu && md->cpu) {
                if (!parse_cpu(md->cpu, &cfg.cpu)) {
                    fprintf(stderr, "gemu: machine '%s' has unknown default cpu '%s'\n",
                            md->name, md->cpu);
                    return 1;
                }
            }
            if (!args.vga && md->vga) {
                if      (strcmp(md->vga, "rp2c02")      == 0) cfg.vga = MOS_VGA_RP2C02;
                else if (strcmp(md->vga, "rp2c04-0004") == 0) cfg.vga = MOS_VGA_RP2C04_0004;
            }
            if (md->soundhw && !parse_soundhw(md->soundhw, &cfg.sound, &cfg.sound_hw_mask)) {
                fprintf(stderr, "gemu: machine '%s' has unknown default soundhw '%s'\n",
                        md->name, md->soundhw);
                return 1;
            }
            if (md->chargen && !parse_cg(md->chargen, &cfg.char_gen)) {
                fprintf(stderr, "gemu: machine '%s' has unknown default chargen '%s'\n",
                        md->name, md->chargen);
                return 1;
            }
            if (md->ram && md->ram[0]) {
                uint32_t sz = parse_size(md->ram);
                if (!sz) return 1;
                cfg.mem_size = sz;
            }
            if (!attach_default_devices(&cfg, &want_vt100, md->devices))
                return 1;
            break;
        }
    }

    if (args.cpu) {
        if (!parse_cpu(args.cpu, &cfg.cpu)) {
            fprintf(stderr, "gemu: unknown -cpu '%s' (use -cpu ? to list)\n", args.cpu);
            return 1;
        }
    }

    if (args.vga) {
        if      (strcmp(args.vga, "rp2c02")      == 0) cfg.vga = MOS_VGA_RP2C02;
        else if (strcmp(args.vga, "rp2c04-0004") == 0) cfg.vga = MOS_VGA_RP2C04_0004;
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
                int n = romdb_load_dir(val, romdb_machine, romdb_add_mos, &cfg);
                if (n < 0) return 1;
                if (n == 0) {
                    fprintf(stderr, "gemu: no known ROMs in '%s' for machine '%s'\n",
                            val, romdb_machine);
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
        } else if (strcmp(rem[i], "-lua") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -lua requires an argument\n"); return 1; }
            cfg.lua_path = rem[++i];
#ifndef HAVE_LUA
            fprintf(stderr, "gemu: -lua: this build was compiled without Lua 5.1 support\n");
            return 1;
#endif
        } else if (strcmp(rem[i], "-fda") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -fda requires an argument\n"); return 1; }
            cfg.fda_path = rem[++i];
        } else if (strcmp(rem[i], "-tape") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -tape requires an argument\n"); return 1; }
            const char *path = NULL;
            uint32_t addr = 0;
            bool addr_explicit = false;
            const char *arg = rem[++i];
            if (cfg.machine == MOS_MACHINE_APPLE1 && !strchr(arg, ':')) {
                addr = 0x0280u;
                path = arg;
            } else {
                int r = gemu_parse_addr_arg("gemu", arg, &addr, &path);
                if (r < 0) return 1;
                addr_explicit = (r == 1);
            }
            cfg.tape_addr = (uint16_t)addr;
            cfg.tape_path = path;
            cfg.tape_addr_explicit = addr_explicit;
        } else if (strcmp(rem[i], "-cg") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -cg requires an argument\n"); return 1; }
            const char *name = rem[++i];
            if (strcmp(name, "?") == 0) {
                printf("Available character generators:\n");
                int maxw = 0;
                for (int c = 0; c < (int)GEMU_ARRAY_LEN(chargens); c++) {
                    int w = (int)strlen(chargens[c].name);
                    if (w > maxw) maxw = w;
                }
                for (int c = 0; c < (int)GEMU_ARRAY_LEN(chargens); c++)
                    printf("  %-*s  %s\n", maxw, chargens[c].name, chargens[c].desc);
                SDL_Quit(); return 0;
            }
            if (!parse_cg(name, &cfg.char_gen)) {
                fprintf(stderr, "gemu: unknown -cg '%s' (use -cg ? to list)\n", name);
                return 1;
            }
            cfg.char_gen_explicit = true;
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
                static const struct { const char *name; const char *desc; const char *machines; } devs[] = {
#include "generated/devices.inc"
                };
                int maxw = 0;
                for (int d = 0; d < (int)(sizeof(devs)/sizeof(devs[0])); d++) {
                    int w = (int)strlen(devs[d].name);
                    if (w > maxw) maxw = w;
                }
                printf("Available devices:\n");
                for (int d = 0; d < (int)(sizeof(devs)/sizeof(devs[0])); d++)
                    printf("  %-*s  %s\n", maxw, devs[d].name, devs[d].desc);
                SDL_Quit(); return 0;
            }
            if (!attach_device(&cfg, &want_vt100, name, false)) return 1;
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
                printf("  ay8910             General Instrument AY-3-8910 PSG (5clown; stub, no synthesis yet)\n");
                printf("  msm6295            OKI MSM6295 ADPCM (5clown)\n");
                printf("                     ay8910/msm6295 are composable: -soundhw ay8910,msm6295\n");
                printf("  pcspk              Primitive 1-bit speaker (7mezzo)\n");
                SDL_Quit(); return 0;
            }
            if (!parse_soundhw(hw, &cfg.sound, &cfg.sound_hw_mask)) {
                fprintf(stderr, "gemu: unknown -soundhw '%s' (use -soundhw ? to list)\n", hw);
                return 1;
            }
            cfg.sound_explicit = true;
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
    cfg.no_shutdown   = args.no_shutdown;

    if ((cfg.machine == MOS_MACHINE_APPLE1 ||
         cfg.machine == MOS_MACHINE_NES ||
         cfg.machine == MOS_MACHINE_KIM1 ||
         cfg.machine == MOS_MACHINE_5CLOWN ||
         cfg.machine == MOS_MACHINE_7MEZZO)
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
        cfg.sound != MOS_SOUND_NONE) {
        bool has_output = (cfg.display_type == GEMU_DISPLAY_SDL ||
                           cfg.display_type == GEMU_DISPLAY_GTK);
        if (!has_output)
            cfg.sound = MOS_SOUND_NONE;
    }

    if (cfg.machine == MOS_MACHINE_NES) {
        if (!cfg.cart_path && !cfg.fds_enabled) {
            bool have_chips = cfg.is_arcade && cfg.n_roms > 0;
            if (!have_chips) {
                if (cfg.is_arcade)
                    fprintf(stderr, "gemu: arcade machine requires a ROM set: -rom /path/to/roms/\n");
                else
                    fprintf(stderr, "gemu: NES requires -cartridge FILE.nes or -device fds\n");
                return 1;
            }
        }
        if (cfg.fda_path && !cfg.fds_enabled) {
            fprintf(stderr, "gemu: -fda requires -device fds\n");
            return 1;
        }
    } else if (cfg.machine != MOS_MACHINE_APPLE1) {
        if (cfg.n_roms == 0) {
            fprintf(stderr, "gemu: no ROM specified\n"
                            "  Load a single file:  -rom ADDR:FILE\n"
                            "  Load from directory: -rom /path/to/roms/\n");
            return 1;
        }
    }

    if (SDL_Init(0) < 0) {
        fprintf(stderr, "gemu: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (cfg.want_wozmon && cfg.machine != MOS_MACHINE_KIM1) {
        fprintf(stderr, "gemu: -device wozmon is currently supported by KIM-1 only\n");
        return 1;
    }
    if (cfg.a1ci && cfg.machine != MOS_MACHINE_APPLE1) {
        fprintf(stderr, "gemu: -device a1ci is supported by Apple I only\n");
        return 1;
    }
    if (cfg.tape_path && cfg.machine == MOS_MACHINE_APPLE1 && !cfg.a1ci) {
        fprintf(stderr, "gemu: Apple I -tape requires -device a1ci\n");
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
        const char *vt_title = cfg.machine == MOS_MACHINE_APPLE1 ? "GEMU"
                             : cfg.want_wozmon ? "GEMU (Wozniak Monitor)"
                             : NULL;
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
    } else if (cfg.machine == MOS_MACHINE_APPLE1) {
        Apple1State *s = apple1_create(&cfg);
        if (!s) { vt100_destroy(vt100); SDL_Quit(); return 1; }
        apple1_run(s, &cfg);
        apple1_destroy(s);
    } else if (cfg.machine == MOS_MACHINE_KIM1) {
        Kim1State *s = kim1_create(&cfg);
        if (!s) { vt100_destroy(vt100); SDL_Quit(); return 1; }
        kim1_run(s, &cfg);
        kim1_destroy(s);
    } else if (cfg.machine == MOS_MACHINE_5CLOWN) {
        FiveClownState *s = fiveclown_create(&cfg);
        if (!s) { vt100_destroy(vt100); SDL_Quit(); return 1; }
        fiveclown_run(s, &cfg);
        fiveclown_destroy(s);
    } else if (cfg.machine == MOS_MACHINE_7MEZZO) {
        Mezzo7State *s = mezzo7_create(&cfg);
        if (!s) { vt100_destroy(vt100); SDL_Quit(); return 1; }
        mezzo7_run(s, &cfg);
        mezzo7_destroy(s);
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
