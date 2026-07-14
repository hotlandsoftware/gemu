#include "tvgame6.h"
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
} MachineDef;

static const MachineDef machine_defs[] = {
#include "generated/machine_defaults.inc"
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

static const GemuDevDesc machines[] = {
#include "generated/machines_mitsu.inc"
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
    .vnc_support  = false,
    .extra_help =
        "\nMitsubishi/Nintendo dedicated console options:\n"
        "  -M tvgame6      Nintendo Color TV-Game 6\n"
        "\nControls:\n"
        "  W/S             Left paddle up/down\n"
        "  Up/Down         Right paddle up/down\n"
        "  1/2/3           Select Tennis/Volley/Hockey\n"
        "  4               Toggle one/two paddles per side\n"
        "  5               Toggle ball speed (normal/turbo)\n"
        "  6/7             Resize left/right paddle\n"
        "  Space           Serve\n",
};

int mitsu_setup(int argc, char *argv[]) {
    MitsuConfig cfg = {
        .machine       = MITSU_MACHINE_TVGAME6,
        .display_type  = GEMU_DISPLAY_SDL,
        .display_scale = 3,
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
        fprintf(stderr, "gemu: -M machine feature options are not supported by mitsu machines\n");
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
            if (strcmp(machine_defs[i].canonical, "tvgame6") == 0)
                cfg.machine = MITSU_MACHINE_TVGAME6;
            found = true;
            break;
        }
        if (!found) {
            fprintf(stderr, "gemu: unknown mitsu machine '%s'\n", args.machine);
            return 1;
        }
    }

    cfg.display_type = args.display_type;
    cfg.display_scale = args.display_scale;
    cfg.no_shutdown = args.no_shutdown;

    bool sdl_up = (cfg.display_type == GEMU_DISPLAY_SDL ||
                   cfg.display_type == GEMU_DISPLAY_NONE);
    if (sdl_up && SDL_Init(0) < 0) {
        fprintf(stderr, "gemu: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int rc = 1;
    if (cfg.machine == MITSU_MACHINE_TVGAME6) {
        MitsuTvGame6State *s = mitsu_tvgame6_create(&cfg);
        if (s) {
            mitsu_tvgame6_run(s, &cfg);
            mitsu_tvgame6_destroy(s);
            rc = 0;
        }
    }

    if (sdl_up)
        SDL_Quit();
    return rc;
}
