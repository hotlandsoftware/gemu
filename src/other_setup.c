/*
 * gemu-other: machines that aren't a family of their own - for now the
 * Manchester Baby (SSEM, 1948).
 */
#include "baby.h"
#include "gemu/args.h"
#include "gemu/monitor.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

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
#include "generated/machines_other.inc"
};

static const GemuArgsDef def = {
    .prog         = "gemu",
    .machines     = machines,
    .n_machines   = (int)(sizeof machines / sizeof *machines),
    .display_mask = GEMU_DISP_F(GEMU_DISPLAY_SDL) | GEMU_DISP_F(GEMU_DISPLAY_NONE)
#ifdef GEMU_GTK
                  | GEMU_DISP_F(GEMU_DISPLAY_GTK)
#endif
    ,
    .extra_help =
        "\nOther machines:\n"
        "  -M baby         Manchester Baby (SSEM, 1948)\n"
        "\nBaby panel (or click it):\n"
        "  1 / 2 / 3       Display the store / accumulator / control\n"
        "  Space           Stop/Run switch\n"
        "  Return          KC: one instruction (Auto) or the F-stat/L-stat\n"
        "                  instruction (Manual), while stopped\n"
        "  M / W           Man/Auto, Write/Erase switches\n"
        "  C A L K         KCC KAC KLC KSC: clear control, accumulator,\n"
        "                  the selected line, the store\n",
};

int other_setup(int argc, char *argv[]) {
    OtherConfig cfg = {
        .machine       = OTHER_MACHINE_BABY,
        .display_type  = GEMU_DISPLAY_SDL,
        .display_scale = 0,          /* 0 = 1x unless -scale */
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
        fprintf(stderr, "gemu: -M machine feature options are not supported by other machines\n");
        return 1;
    }
    for (int i = 0; i < nrem; i++) {
        fprintf(stderr, "gemu: unknown option '%s' (try -h)\n", rem[i]);
        return 1;
    }

    if (args.machine) {
        bool found = false;
        for (int i = 0; machine_defs[i].name; i++) {
            if (strcmp(args.machine, machine_defs[i].name) != 0)
                continue;
            if (strcmp(machine_defs[i].canonical, "baby") == 0)
                cfg.machine = OTHER_MACHINE_BABY;
            found = true;
            break;
        }
        if (!found) {
            fprintf(stderr, "gemu: unknown other machine '%s'\n", args.machine);
            return 1;
        }
    }

    cfg.display_type  = args.display_type;
    cfg.display_scale = args.display_scale;
    cfg.no_shutdown   = args.no_shutdown;

    bool sdl_up = (cfg.display_type == GEMU_DISPLAY_SDL ||
                   cfg.display_type == GEMU_DISPLAY_NONE);
    if (sdl_up && SDL_Init(0) < 0) {
        fprintf(stderr, "gemu: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int rc = 1;
    if (cfg.machine == OTHER_MACHINE_BABY) {
        BabyState *s = other_baby_create(&cfg);
        if (s) {
            other_baby_run(s, &cfg);
            other_baby_destroy(s);
            rc = 0;
        }
    }

    if (sdl_up)
        SDL_Quit();
    return rc;
}
