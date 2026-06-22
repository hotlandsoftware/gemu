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

static void print_machines(void) {
    fprintf(stderr, "Available machines (-M <machine>):\n");
    int maxw = 0;
    for (int i = 0; i < N_MACHINES; i++) {
        int w = (int)strlen(MACHINES[i].name);
        if (w > maxw) maxw = w;
    }
    for (int i = 0; i < N_MACHINES; i++)
        fprintf(stderr, "  %-*s  %s\n", maxw, MACHINES[i].name, MACHINES[i].desc);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "GEMU " GEMU_VERSION_STR " — Generic Emulator\n\n");
    fprintf(stderr, "Usage: %s -M <machine> [options]\n\n", prog);
    print_machines();
    fprintf(stderr, "\nRun '%s -M <machine> -h' for machine-specific help.\n", prog);
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
        print_machines();
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
