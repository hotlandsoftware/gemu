#include "gemu/gemu.h"
#include "gemu/args.h"
#include <stdio.h>
#include <string.h>

#ifdef HAVE_CHIP8
int chip8_setup(int argc, char *argv[]);
#endif
#ifdef HAVE_MOS
int mos_setup(int argc, char *argv[]);
#endif
#ifdef HAVE_RCA
int rca_setup(int argc, char *argv[]);
#endif

typedef int (*SetupFn)(int argc, char *argv[]);

typedef struct {
    const char *name;
    SetupFn     fn;
    const char *desc;
} MachineEntry;

static const MachineEntry MACHINES[] = {
#include "generated/machines.inc"
};

#define N_MACHINES ((int)(sizeof MACHINES / sizeof *MACHINES))

/* ── Global device / soundhw listings ───────────────────────────────────── */

static void list_all_devices(void) {
    static const GemuDevDesc3 devs[] = {
#include "generated/devices.inc"
    };
    gemu_print_table3("Available devices", devs, (int)(sizeof devs / sizeof *devs));
}

static void list_all_soundhw(void) {
    static const GemuDevDesc3 hw[] = {
#include "generated/soundhw.inc"
    };
    gemu_print_table3("Available sound hardware", hw, (int)(sizeof hw / sizeof *hw));
}

/* ── Usage / machine listing ─────────────────────────────────────────────── */

static void print_machines(void) {
    GemuDevDesc tmp[N_MACHINES];
    for (int i = 0; i < N_MACHINES; i++)
        tmp[i] = (GemuDevDesc){ MACHINES[i].name, MACHINES[i].desc };
    gemu_print_table("Available machines (-M <machine>)", tmp, N_MACHINES);
}

static void print_usage(const char *prog) {
    FILE *f = stderr;
    fprintf(f, "GEMU v" GEMU_VERSION_STR " - Generic EMUlator\n"
               "Usage: %s -M <machine> [options] [rom]\n\n", prog);

#define OPT(name, desc) fprintf(f, "  %-26s %s\n", name, desc)

    fprintf(f, "Standard options:\n");
    OPT("-M <machine>",       "Select emulated machine (use -M ? to list all)");
    OPT("-cpu <type>",        "CPU model, if selectable (use -cpu ? to list)");
    OPT("-vga <type>",        "Video chip, if selectable (use -vga ? to list)");
    OPT("-h, -help",          "Show this help");
    fprintf(f, "\n");

    fprintf(f, "Display options:\n");
    OPT("-display <type>",    "Display backend: sdl | gtk | curses | none (use -display ? to list)");
    OPT("-scale N",           "Window scale factor");
    OPT("-vnc <addr>",        "Start VNC server on <addr> (use -vnc ? for address format)");
    fprintf(f, "\n");

    fprintf(f, "Media options:\n");
    OPT("-rom [ADDR:]FILE",   "Load ROM image at ADDR (auto-detected if omitted)");
    OPT("-rom DIR",           "Scan directory and load known ROMs by SHA-256");
    OPT("-cartridge FILE",    "Insert cartridge (NES .nes, Studio II)");
    OPT("-fda FILE",          "Insert floppy/disk image (Famicom Disk System)");
    OPT("-tape [ADDR:]FILE",  "Insert cassette tape (KIM-1, COSMAC VIP)");
    OPT("-start ADDR",        "Override reset vector / force start address");
    fprintf(f, "\n");

    fprintf(f, "Machine options:\n");
    OPT("-m SIZE",            "RAM size - plain number = KB, or suffix K/M (e.g. 32K, 1M)");
    OPT("-cg NAME",           "Character generator, if selectable (use -cg ? to list)");
    OPT("-device NAME",       "Attach a device (use -device ? to list all)");
    OPT("-soundhw NAME",      "Sound hardware (use -soundhw ? to list all)");
    OPT("-renderer MODE",     "SDL renderer: auto | software | accelerated");
    fprintf(f, "\n");

    fprintf(f, "Debug options:\n");
    OPT("-monitor <spec>",    "Monitor: stdio | none | telnet:HOST:PORT,server,nowait");
    fprintf(f, "\n");

#undef OPT

    fprintf(f, "Use '-M ?' to list all available machines.\n"
               "Use '-M <machine> -h' for machine-specific options and examples.\n");
}

static void machine_spec_name(const char *spec, char *out, size_t out_sz) {
    const char *comma = strchr(spec, ',');
    size_t len = comma ? (size_t)(comma - spec) : strlen(spec);
    if (out_sz == 0) return;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, spec, len);
    out[len] = '\0';
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *prog = argv[0];

    /* Global -device ? and -soundhw ? - list across all families without -M */
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-device") == 0 && strcmp(argv[i + 1], "?") == 0) {
            list_all_devices();
            return 0;
        }
        if (strcmp(argv[i], "-soundhw") == 0 && strcmp(argv[i + 1], "?") == 0) {
            list_all_soundhw();
            return 0;
        }
    }

    /* Scan for -M <machine> */
    const char *machine_name = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-M") == 0) {
            machine_name = argv[i + 1];
            break;
        }
    }

    /* -M ? lists all machines */
    if (machine_name && strcmp(machine_name, "?") == 0) {
        print_machines();
        return 0;
    }

    /* Dispatch via -M */
    if (machine_name) {
        char base_machine[128];
        machine_spec_name(machine_name, base_machine, sizeof(base_machine));
        for (int i = 0; i < N_MACHINES; i++) {
            if (strcmp(base_machine, MACHINES[i].name) == 0)
                return MACHINES[i].fn(argc, argv);
        }
        fprintf(stderr, "gemu: unknown machine '%s' (use -M ? to list)\n", base_machine);
        return 1;
    }

    /* Legacy argv[0] dispatch for backward compat with symlinks */
    const char *base = strrchr(prog, '/');
    base = base ? base + 1 : prog;
#ifdef HAVE_CHIP8
    if (strcmp(base, "gemu-chip8") == 0) return chip8_setup(argc, argv);
#endif
#ifdef HAVE_MOS
    if (strcmp(base, "gemu-mos") == 0)   return mos_setup(argc, argv);
#endif
#ifdef HAVE_RCA
    if (strcmp(base, "gemu-rca") == 0)   return rca_setup(argc, argv);
#endif

    print_usage(prog);
    return 1;
}
