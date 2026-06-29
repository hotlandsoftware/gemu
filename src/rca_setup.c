#include "rca.h"
#include "vip.h"
#include "destroyer.h"
#include "studio2.h"
#include "pecom.h"
#include "romdb.h"
#include "rca_keyboard.h"
#include "gemu/gemu.h"
#include "gemu/args.h"
#include "gemu/monitor.h"
#include <SDL2/SDL.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static bool romdb_add_rca(const char *path, const char *region, uint32_t addr, void *ud) {
    RcaConfig *cfg = ud;
    (void)region;
    if (cfg->n_roms >= RCA_MAX_ROM_LOADS) {
        fprintf(stderr, "gemu: too many ROMs (max %d)\n", RCA_MAX_ROM_LOADS);
        return false;
    }
    char *p = strdup(path);
    if (!p) return false;
    cfg->roms[cfg->n_roms].path = p;
    cfg->roms[cfg->n_roms].addr = addr;
    cfg->n_roms++;
    return true;
}

typedef struct {
    const char *name;
    const char *canonical;
    const char *cpu;
    const char *vga;
    const char *soundhw;
    const char *chargen;
    const char *tv;
    const char *ram;
} MachineDef;

static const MachineDef machine_defs[] = {
#include "generated/machine_defaults.inc"
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

/* ── Device registry ─────────────────────────────────────────────────────── */

static const GemuDevDesc machines[] = {
    {"altair2",    "Cidelsa Altair II arcade board (CDP1802 + CDP1869 VIS, alias for destroyer)"},
    {"apollo80",   "Academy Apollo 80 (alias for studio2)"},
    {"cm1200",     "Conic M-1200 (alias for studio2)"},
    {"vip",        "RCA COSMAC VIP (CDP1802 + CDP1861 Pixie, 2 KB RAM)"},
    {"destroyer",  "Cidelsa Destroyer arcade board (CDP1802 + CDP1869 VIS)"},
    {"rca",        "Generic RCA COSMAC (stub/not yet implemented)"},
    {"mpt02",      "Victory MPT-02 (alias for studio2)"},
    {"mpt02j",     "Hanimex MPT-02 (alias for studio2)"},
    {"mtc9016",    "Mustang 9016 (alias for studio2)"},
    {"pecom32",    "Pecom 32 (CDP1802 + CDP1869/1870 VIS-1870, 16 KB ROM, 32 KB RAM, PAL)"},
    {"pecom64",    "Pecom 64 (CDP1802 + CDP1869/1870 VIS-1870, 16 KB ROM, 32 KB RAM, PAL, alias for pecom32)"},
    {"studio2",    "RCA Studio II (CDP1802 + CDP1861 Pixie, cartridge-based)"},
    {"sm1200",     "Sheen M1200 (alias for studio2)"},
    {"visicom",    "Visicom COM-100 (alias for studio2)"},
};
static const GemuDevDesc cpus[] = {
    {"cdp1802", "RCA CDP1802 COSMAC"},
};
static const GemuDevDesc vgas[] = {
#include "generated/vgas_rca.inc"
};
static const GemuDevDesc soundhws[] = {
    {"pcspk", "Standard PC speaker / one-bit loudspeaker"},
    {"none",  "Disable sound output"},
};
static const GemuArgsDef def = {
    .prog         = "gemu",
    .machines     = machines, .n_machines = (int)(sizeof machines / sizeof *machines),
    .cpus         = cpus,     .n_cpus     = (int)(sizeof cpus     / sizeof *cpus),
    .vgas         = vgas,     .n_vgas     = (int)(sizeof vgas     / sizeof *vgas),
    .display_mask = GEMU_DISP_F(GEMU_DISPLAY_SDL)
#ifndef GEMU_NO_CURSES
                  | GEMU_DISP_F(GEMU_DISPLAY_CURSES)
#endif
                  | GEMU_DISP_F(GEMU_DISPLAY_NONE)
#ifdef GEMU_GTK
                  | GEMU_DISP_F(GEMU_DISPLAY_GTK)
#endif
    ,
    .vnc_support  = true,
    .extra_help =
        "\nArguments:\n"
        "  -rom ADDR:FILE   Load a ROM/blob at address ADDR; may be repeated\n"
        "  -rom FILE        Load a ROM/blob using machine/content address detection, or 0x0000\n"
        "  -load-addr ADDR  Load positional ROM at ADDR (default 0x0000)\n"
        "  -start ADDR      Start CDP1802 execution at ADDR\n"
        "  -device NAME     Attach device      (use -device ? to list)\n"
        "  -soundhw NAME    Sound hardware     (use -soundhw ? to list)\n"
        "  -tape FILE       Insert a cassette tape (raw binary, loaded at 0x0000)\n"
        "  -tape ADDR:FILE  Insert a cassette tape at the given load address\n"
        "  -cartridge FILE  Insert a cartridge (Studio II; raw binary or ST2 format)\n"
        "\nExample commands:\n"
        "  ./bin/gemu -M vip -device vip-keypad -rom roms/fpb_color.bin -rom roms/vip.32.rom\n"
        "  ./bin/gemu -M vip -rom 0x0000:roms/fpb_color.bin -rom 0x8000:roms/vip.32.rom\n"
        "  ./bin/gemu -M vip -start 0x1000 -rom 0x0000:roms/fpb_color.bin\n",
};

/* ── ROM load helper ─────────────────────────────────────────────────────── */

static bool add_rom(RcaConfig *cfg, uint32_t addr, const char *path) {
    if (cfg->n_roms >= RCA_MAX_ROM_LOADS) {
        fprintf(stderr, "gemu: too many -rom loads (max %d)\n", RCA_MAX_ROM_LOADS);
        return false;
    }
    cfg->roms[cfg->n_roms].addr = addr;
    cfg->roms[cfg->n_roms].path = path;
    cfg->n_roms++;
    return true;
}

static bool infer_rom_addr(const char *path, uint32_t *addr) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gemu: failed to open '%s'\n", path);
        return false;
    }

    uint8_t header[3] = {0};
    size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);

    if (n == sizeof(header) && header[0] == 0xF8 &&
        header[2] >= 0xB0 && header[2] <= 0xBF) {
        *addr = (uint32_t)header[1] << 8;
        return true;
    }

    return false;
}

static bool parse_rom_arg(RcaConfig *cfg, const char *arg) {
    uint32_t addr = 0;
    const char *path;
    int r = gemu_parse_addr_arg("gemu", arg, &addr, &path);
    if (r < 0) return false;
    if (r == 0) {
        if (!infer_rom_addr(arg, &addr) && cfg->machine == RCA_MACHINE_DESTROYER)
            addr = (uint32_t)cfg->n_roms * 0x0800u;
    }
    return add_rom(cfg, addr, path);
}

static void soundhw_list_print(void) {
    printf("Available RCA sound hardware:\n");
    int maxw = 0;
    for (int i = 0; i < (int)(sizeof(soundhws) / sizeof(soundhws[0])); i++) {
        int w = (int)strlen(soundhws[i].name);
        if (w > maxw) maxw = w;
    }
    for (int i = 0; i < (int)(sizeof(soundhws) / sizeof(soundhws[0])); i++)
        printf("  %-*s  %s\n", maxw, soundhws[i].name, soundhws[i].desc);
}

static bool parse_soundhw(const char *name, RcaSoundHwType *out) {
    if (strcmp(name, "pcspk") == 0) { *out = RCA_SOUND_PCSPK; return true; }
    if (strcmp(name, "none")  == 0) { *out = RCA_SOUND_NONE;  return true; }
    return false;
}

/* ── rca_setup ───────────────────────────────────────────────────────────── */

int rca_setup(int argc, char *argv[]) {
    if (argc < 2) {
        char *fake[] = {argv[0], "-h"};
        int rem = 0;
        gemu_args_parse(2, fake, &def, &(GemuArgs){}, &rem, NULL);
        return 1;
    }

    RcaConfig cfg = {
        .machine       = RCA_MACHINE_COSMAC_VIP,
        .cpu           = RCA_CPU_CDP1802,
        .vga           = RCA_VGA_CDP1861,
        .keyboard      = RCA_KEYBOARD_VP601,
        .sound_hw      = RCA_SOUND_NONE,
        .display_type  = GEMU_DISPLAY_SDL,
        .display_scale = 4,
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
            if      (strcmp(canon, "rca")       == 0) cfg.machine = RCA_MACHINE_GENERIC;
            else if (strcmp(canon, "vip")       == 0) cfg.machine = RCA_MACHINE_COSMAC_VIP;
            else if (strcmp(canon, "studio2")   == 0) cfg.machine = RCA_MACHINE_STUDIO2;
            else if (strcmp(canon, "destroyer") == 0) cfg.machine = RCA_MACHINE_DESTROYER;
            else if (strcmp(canon, "pecom32")   == 0) cfg.machine = RCA_MACHINE_PECOM32;
            cfg.tv_mode = (md->tv && strcmp(md->tv, "pal") == 0) ? RCA_TV_PAL : RCA_TV_NTSC;
            if (!args.cpu && md->cpu) {
                if (strcmp(md->cpu, "cdp1802") == 0) cfg.cpu = RCA_CPU_CDP1802;
            }
            if (!args.vga && md->vga) {
                if      (strcmp(md->vga, "cdp1861") == 0) cfg.vga = RCA_VGA_CDP1861;
                else if (strcmp(md->vga, "cdp1869") == 0) cfg.vga = RCA_VGA_CDP1869;
                else if (strcmp(md->vga, "none")    == 0) cfg.vga = RCA_VGA_NONE;
            }
            if (md->soundhw && !parse_soundhw(md->soundhw, &cfg.sound_hw)) {
                fprintf(stderr, "gemu: machine '%s' has unknown default soundhw '%s'\n",
                        md->name, md->soundhw);
                return 1;
            }
            break;
        }
    }
    if (args.vga) {
        if      (strcmp(args.vga, "cdp1861") == 0) cfg.vga = RCA_VGA_CDP1861;
        else if (strcmp(args.vga, "cdp1869") == 0) cfg.vga = RCA_VGA_CDP1869;
        else if (strcmp(args.vga, "none")    == 0) cfg.vga = RCA_VGA_NONE;
    }
    if (cfg.machine == RCA_MACHINE_DESTROYER)
        cfg.keyboard = RCA_KEYBOARD_NONE;

    uint32_t positional_addr = 0x0000;
    for (int i = 0; i < nrem; i++) {
        if (strcmp(rem[i], "-rom") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -rom requires an argument\n"); return 1; }
            const char *val = rem[++i];
            struct stat st;
            if (stat(val, &st) == 0 && S_ISDIR(st.st_mode)) {
                const char *alias = args.machine ? args.machine : "studio2";
                int n = romdb_load_dir(val, alias, romdb_add_rca, &cfg);
                if (n < 0) return 1;
                if (n == 0) {
                    fprintf(stderr, "gemu: no known ROMs found in '%s' for machine '%s'\n",
                            val, alias);
                    return 1;
                }
            } else {
                if (!parse_rom_arg(&cfg, val)) return 1;
            }
        } else if (strcmp(rem[i], "-load-addr") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -load-addr requires an argument\n"); return 1; }
            positional_addr = (uint32_t)strtoul(rem[++i], NULL, 0);
        } else if (strcmp(rem[i], "-start") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -start requires an argument\n"); return 1; }
            cfg.start_addr = (uint16_t)strtoul(rem[++i], NULL, 0);
            cfg.has_start_addr = true;
        } else if (strcmp(rem[i], "-mode") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -mode requires an argument (pal|ntsc)\n"); return 1; }
            const char *v = rem[++i];
            if      (strcmp(v, "pal")  == 0) cfg.tv_mode = RCA_TV_PAL;
            else if (strcmp(v, "ntsc") == 0) cfg.tv_mode = RCA_TV_NTSC;
            else { fprintf(stderr, "gemu: unknown mode '%s' (pal|ntsc)\n", v); return 1; }
        } else if (strcmp(rem[i], "-device") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -device requires an argument\n"); return 1; }
            const char *v = rem[++i];
            if (strcmp(v, "?") == 0 || strcmp(v, "help") == 0) {
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
                return 0;
            }
            static const struct { const char *name; RcaKeyboardType kb; } rca_devs[] = {
                {"vp601",      RCA_KEYBOARD_VP601},
                {"vip-keypad", RCA_KEYBOARD_KEYPAD},
                {"keypad",     RCA_KEYBOARD_KEYPAD},
                {"keyboard",   RCA_KEYBOARD_GENERIC},
            };
            RcaKeyboardType kb = RCA_KEYBOARD_NONE;
            for (int d = 0; d < (int)(sizeof(rca_devs)/sizeof(rca_devs[0])); d++)
                if (strcmp(v, rca_devs[d].name) == 0) { kb = rca_devs[d].kb; break; }
            if (kb == RCA_KEYBOARD_NONE) {
                fprintf(stderr, "gemu: unknown device '%s' (try -device ?)\n", v);
                return 1;
            }
            if (cfg.machine != RCA_MACHINE_COSMAC_VIP) {
                fprintf(stderr, "gemu: keyboard devices are only supported by vip\n");
                return 1;
            }
            rca_device_attach(&cfg, kb);
        } else if (strcmp(rem[i], "-soundhw") == 0) {
            if (i + 1 >= nrem) {
                fprintf(stderr, "gemu: -soundhw requires an argument\n");
                return 1;
            }
            const char *v = rem[++i];
            if (strcmp(v, "?") == 0 || strcmp(v, "help") == 0) {
                soundhw_list_print();
                return 0;
            }
            if (!parse_soundhw(v, &cfg.sound_hw)) {
                fprintf(stderr, "gemu: unknown sound hardware '%s' (try -soundhw ?)\n", v);
                return 1;
            }
        } else if (strcmp(rem[i], "-tape") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -tape requires an argument\n"); return 1; }
            const char *v = rem[++i];
            uint32_t tape_addr = 0;
            const char *tape_path;
            if (gemu_parse_addr_arg("gemu", v, &tape_addr, &tape_path) < 0) return 1;
            cfg.tape_addr = (uint16_t)tape_addr;
            cfg.tape_path = tape_path;
        } else if (strcmp(rem[i], "-cartridge") == 0) {
            if (i + 1 >= nrem) { fprintf(stderr, "gemu: -cartridge requires an argument\n"); return 1; }
            cfg.cartridge_path = rem[++i];
        } else {
            fprintf(stderr, "gemu: unknown option '%s' (try -h)\n", rem[i]);
            return 1;
        }
    }

    if (args.rom_path) {
        if (!add_rom(&cfg, positional_addr, args.rom_path)) return 1;
    }

    cfg.display_type  = args.display_type;
    cfg.display_scale = args.display_scale;
    cfg.vnc_addr      = args.vnc_addr;
    cfg.no_shutdown   = args.no_shutdown;
    if (cfg.vnc_addr)
        cfg.sound_hw = RCA_SOUND_NONE;
    if (cfg.n_roms == 0 && cfg.machine != RCA_MACHINE_GENERIC) {
        const char *alias = args.machine ? args.machine : "studio2";
        romdb_print_needed(alias);
        return 1;
    }

    if (cfg.machine == RCA_MACHINE_GENERIC) {
        fprintf(stderr, "gemu: rca generic machine not yet implemented\n");
        return 1;
    }
    if (cfg.machine == RCA_MACHINE_DESTROYER &&
        cfg.display_type == GEMU_DISPLAY_CURSES) {
        fprintf(stderr, "gemu: curses display is not yet supported by destroyer\n");
        return 1;
    }

    bool sdl_up = (cfg.display_type == GEMU_DISPLAY_SDL ||
#ifndef GEMU_NO_CURSES
                   cfg.display_type == GEMU_DISPLAY_CURSES ||
#endif
                   cfg.display_type == GEMU_DISPLAY_NONE);
    if (sdl_up && SDL_Init(0) < 0) {
        fprintf(stderr, "gemu: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int rc = 1;
    if (cfg.machine == RCA_MACHINE_PECOM32) {
        RcaPecom32State *s = rca_pecom32_create(&cfg);
        if (s) { rca_pecom32_run(s, &cfg); rca_pecom32_destroy(s); rc = 0; }
    } else if (cfg.machine == RCA_MACHINE_DESTROYER) {
        RcaDestroyerState *s = rca_destroyer_create(&cfg);
        if (s) { rca_destroyer_run(s, &cfg); rca_destroyer_destroy(s); rc = 0; }
    } else if (cfg.machine == RCA_MACHINE_STUDIO2) {
        RcaStudio2State *s = rca_studio2_create(&cfg);
        if (s) { rca_studio2_run(s, &cfg); rca_studio2_destroy(s); rc = 0; }
    } else {
        RcaVipState *s = rca_vip_create(&cfg);
        if (s) { rca_machine_run(s, &cfg); rca_vip_destroy(s); rc = 0; }
    }

    if (sdl_up) SDL_Quit();
    return rc;
}
