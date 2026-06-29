#pragma once
#include "display.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Universal argument parser shared by all gemu binaries.
 *
 * Each binary registers its supported devices and flags via GemuArgsDef.
 * gemu_args_parse() handles -M, -cpu, -vga, -display, -scale, -vnc,
 * -monitor, -gmp/-qmp, and -h.
 * Binary-specific flags (e.g. chip8's -hz) are returned unmodified in
 * rem_argv[0..(*rem_argc)-1]; rem_argv must be pre-allocated to argc entries.
 * Pass NULL/NULL for rem_argc/rem_argv if no binary-specific flags exist.
 */

typedef struct {
    const char *name;
    const char *desc;
} GemuDevDesc;

/* Bitmask helper: GEMU_DISP_F(GEMU_DISPLAY_SDL) | GEMU_DISP_F(...) */
#define GEMU_DISP_F(t) (1u << (unsigned)(t))

typedef struct {
    const char        *prog;            /* binary name shown in usage */
    const GemuDevDesc *machines;
    int                n_machines;
    const GemuDevDesc *cpus;            /* NULL / n_cpus=0 → -cpu not listed */
    int                n_cpus;
    const GemuDevDesc *vgas;            /* NULL / n_vgas=0 → -vga not listed */
    int                n_vgas;
    unsigned           display_mask;    /* bitfield of supported GemuDisplayType */
    bool               vnc_support;     /* whether -vnc is a valid option */
    const char        *extra_help;      /* optional binary-specific usage text */
} GemuArgsDef;

/*
 * Caller initialises GemuArgs with its own defaults before calling.
 * gemu_args_parse only writes fields that were explicitly given on the
 * command line.
 */
typedef struct {
    const char     *rom_path;
    const char     *machine;       /* NULL until -M given */
    const char     *cpu;           /* NULL until -cpu given */
    const char     *vga;           /* NULL until -vga given */
    GemuDisplayType display_type;
    int             display_scale;
    const char     *vnc_addr;      /* NULL = no VNC */
    const char     *monitor_spec;  /* NULL = stdio */
    bool            display_explicit; /* true if -display was given */
} GemuArgs;

/*
 * Parse argc/argv.
 * -h/-help/--help, -M ?, -cpu ?, -vga ?, -display ? all print and exit(0).
 * Returns false and prints an error message on invalid input.
 */
bool gemu_args_parse(int argc, char **argv,
                     const GemuArgsDef *def, GemuArgs *out,
                     int *rem_argc, char **rem_argv);

/*
 * Parse an "ADDR:FILE" or plain "FILE" argument.
 *
 * Returns  1: colon syntax found — *addr set from the hex/decimal prefix,
 *             *path points to the character after the colon.
 * Returns  0: no colon — *addr is unchanged, *path == arg.
 * Returns -1: malformed input (empty addr or empty path after colon);
 *             an error is printed to stderr using prog as the program name.
 */
int gemu_parse_addr_arg(const char *prog, const char *arg,
                        uint32_t *addr, const char **path);
