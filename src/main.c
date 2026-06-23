#include "gemu/gemu.h"
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
#ifdef HAVE_RCA
    {"altair2",   rca_setup,   "Cidelsa Altair II (alias for destroyer)"},
    {"apollo80",  rca_setup,   "Academy Apollo 80 (alias for studio2)"},
#endif
#ifdef HAVE_CHIP8
    {"chip8",     chip8_setup, "Generic CHIP-8 interpreter"},
#endif
#ifdef HAVE_RCA
    {"cm1200",    rca_setup,   "Conic M-1200 (alias for studio2)"},
    {"destroyer", rca_setup,   "Cidelsa Destroyer arcade board"},
#endif
#ifdef HAVE_MOS
    {"famicom",   mos_setup,   "Nintendo Family Computer (alias for nes)"},
    {"kim1",      mos_setup,   "MOS KIM-1 single-board computer"},
    {"mos",       mos_setup,   "Generic MOS 6502 machine (flat 64 KB)"},
#endif
#ifdef HAVE_RCA
    {"mpt02",     rca_setup,   "Victory MPT-02 (alias for studio2)"},
    {"mpt02j",    rca_setup,   "Hanimex MPT-02 (alias for studio2)"},
    {"mtc9016",   rca_setup,   "Mustang 9016 (alias for studio2)"},
#endif
#ifdef HAVE_MOS
    {"nes",       mos_setup,   "Nintendo Entertainment System (NTSC)"},
    {"nespal",    mos_setup,   "Nintendo Entertainment System (PAL)"},
#endif
#ifdef HAVE_RCA
    {"pecom32",   rca_setup,   "Pecom 32"},
    {"pecom64",   rca_setup,   "Pecom 64 (alias for pecom32)"},
    {"rca",       rca_setup,   "Generic RCA COSMAC machine"},
    {"sm1200",    rca_setup,   "Sheen M1200 (alias for studio2)"},
    {"studio2",   rca_setup,   "RCA Studio II"},
    {"vip",       rca_setup,   "RCA COSMAC VIP"},
    {"visicom",   rca_setup,   "Visicom COM-100 (alias for studio2)"},
#endif
};

#define N_MACHINES ((int)(sizeof MACHINES / sizeof *MACHINES))

static void print_machines(FILE *f) {
    fprintf(f, "Available machines (-M <machine>):\n");
    int maxw = 0;
    for (int i = 0; i < N_MACHINES; i++) {
        int w = (int)strlen(MACHINES[i].name);
        if (w > maxw) maxw = w;
    }
    for (int i = 0; i < N_MACHINES; i++)
        fprintf(f, "  %-*s  %s\n", maxw, MACHINES[i].name, MACHINES[i].desc);
}

static void print_usage(const char *prog) {
    FILE *f = stderr;
    fprintf(f, "GEMU v" GEMU_VERSION_STR " — Generic EMUlator\n"
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
    OPT("-m SIZE",            "RAM size — plain number = KB, or suffix K/M (e.g. 32K, 1M)");
    OPT("-device NAME",       "Attach a device (use -device ? to list)");
    OPT("-soundhw NAME",      "Sound hardware (use -soundhw ? to list)");
    OPT("-renderer MODE",     "SDL renderer: auto | software | accelerated");
    fprintf(f, "\n");

    fprintf(f, "Debug options:\n");
    OPT("-monitor <spec>",    "Monitor: stdio | none | telnet:HOST:PORT,server,nowait");
    fprintf(f, "\n");

#undef OPT

    fprintf(f, "Use '-M ?' to list all available machines.\n"
               "Use '-M <machine> -h' for machine-specific options and examples.\n");
}

int main(int argc, char *argv[]) {
    const char *prog = argv[0];

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
        print_machines(stdout);
        return 0;
    }

    /* Dispatch via -M */
    if (machine_name) {
        for (int i = 0; i < N_MACHINES; i++) {
            if (strcmp(machine_name, MACHINES[i].name) == 0)
                return MACHINES[i].fn(argc, argv);
        }
        fprintf(stderr, "gemu: unknown machine '%s' (use -M ? to list)\n", machine_name);
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
