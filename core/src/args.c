#include "gemu/args.h"
#include "gemu/gemu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

/* ── Display backend table (fixed set, same for all binaries) ────────────── */

typedef struct { GemuDisplayType type; const char *name; const char *desc; } DispEntry;

static const DispEntry display_table[] = {
    { GEMU_DISPLAY_SDL,    "sdl",    "SDL2 windowed display (hardware-accelerated)" },
    { GEMU_DISPLAY_GTK,    "gtk",    "GTK3 windowed display with menu bar" },
    { GEMU_DISPLAY_CURSES, "curses", "ncurses terminal (half-block Unicode characters)" },
    { GEMU_DISPLAY_NONE,   "none",   "Headless - no display output (pair with -vnc)" },
};
#define N_DISPLAYS (int)(sizeof(display_table) / sizeof(display_table[0]))

/* ── Help / listing helpers ──────────────────────────────────────────────── */

static void print_usage(const GemuArgsDef *def) {
    printf("GEMU v" GEMU_VERSION_STR " - Generic EMUlator\n"
           "Usage: %s -M <machine> [options] [rom]\n\n"
           "Options:\n", def->prog);
    if (def->n_machines > 0)
        printf("  %-14s Machine type      (use -M ? to list)\n", "-M TYPE");
    if (def->n_machines > 0)
        printf("  %-14s Machine feature toggles, e.g. nes,-feature\n", "-M M,OPTS");
    if (def->n_cpus > 0)
        printf("  %-14s CPU model         (use -cpu ? to list)\n", "-cpu TYPE");
    if (def->n_vgas > 0)
        printf("  %-14s Video chip        (use -vga ? to list)\n", "-vga TYPE");
    printf("  %-14s Display backend   (use -display ? to list)\n", "-display TYPE");
    printf("  %-14s Window scale factor\n", "-scale N");
    if (def->vnc_support)
        printf("  %-14s VNC server (use -vnc ? for address format)\n", "-vnc ADDR");
    printf("  %-14s Pause/halt instead of exiting on monitor shutdown\n", "-no-shutdown");
    printf("  %-14s Monitor: stdio | none | telnet:HOST:PORT,server,nowait\n",
           "-monitor SPEC");
    printf("  %-14s GMP/QMP-compatible monitor over TCP or stdio (alias: -qmp)\n", "-gmp ADDR");
    printf("  %-14s Show this help\n", "-h, -help");
    if (def->extra_help && def->extra_help[0])
        printf("%s", def->extra_help);
}

enum { TABLE_PAGER_MIN_ROWS = 15 };

static bool text_contains_ci(const char *text, const char *needle) {
    if (!needle[0]) return true;
    for (; *text; text++) {
        const char *a = text, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++; b++;
        }
        if (!*b) return true;
    }
    return false;
}

static void table_row(char *buf, size_t len, const GemuDevDesc *devs,
                      const GemuDevDesc3 *devs3, int i, int maxw) {
    if (devs3)
        snprintf(buf, len, "  %-*s  %s [%s]", maxw, devs3[i].name,
                 devs3[i].desc, devs3[i].machines);
    else
        snprintf(buf, len, "  %-*s  %s", maxw, devs[i].name, devs[i].desc);
}

#ifndef _WIN32
static struct termios pager_saved_termios;
static bool pager_termios_active;

static void pager_restore_terminal(void) {
    if (pager_termios_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &pager_saved_termios);
        pager_termios_active = false;
    }
}

static bool pager_begin(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &pager_saved_termios) != 0) return false;
    struct termios raw = pager_saved_termios;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;
    pager_termios_active = true;
    static bool registered;
    if (!registered) { atexit(pager_restore_terminal); registered = true; }
    return true;
}

static void pager_size(int *columns, int *rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *columns = ws.ws_col ? ws.ws_col : 80;
        *rows = ws.ws_row ? ws.ws_row : 24;
    } else {
        *columns = 80;
        *rows = 24;
    }
}

static int pager_read_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return 'q';
    if (c != 0x1b) return c;
    unsigned char seq[2];
    if (read(STDIN_FILENO, &seq[0], 1) != 1 || seq[0] != '[') return 0x1b;
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return 0x1b;
    if (seq[1] == 'C' || seq[1] == 'B') return 'n';
    if (seq[1] == 'D' || seq[1] == 'A') return 'p';
    return 0x1b;
}

static void pager_search(char *query, size_t query_len) {
    size_t used = 0;
    query[0] = '\0';
    printf("\033[2K\r/Search: ");
    fflush(stdout);
    for (;;) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;
        if (c == '\r' || c == '\n') break;
        if (c == 0x1b) { query[0] = '\0'; break; }
        if (c == 0x7f || c == '\b') {
            if (used) { used--; query[used] = '\0'; printf("\b \b"); fflush(stdout); }
        } else if (c >= 0x20 && c < 0x7f && used + 1 < query_len) {
            query[used++] = (char)c;
            query[used] = '\0';
            putchar(c);
            fflush(stdout);
        }
    }
}

static bool table_pager(const char *heading, const GemuDevDesc *devs,
                        const GemuDevDesc3 *devs3, int n, int maxw) {
    if (n < TABLE_PAGER_MIN_ROWS || !pager_begin()) return false;

    int *matches = malloc((size_t)n * sizeof(*matches));
    if (!matches) { pager_restore_terminal(); return false; }
    char query[128] = "";
    int page = 0;

    for (;;) {
        int cols, terminal_rows;
        pager_size(&cols, &terminal_rows);
        /* One row for the heading and one for the status bar.  Rows are
         * truncated to the terminal width below, so every item consumes
         * exactly one terminal line. */
        int page_rows = terminal_rows - 2;
        if (page_rows < 1) page_rows = 1;

        int count = 0;
        for (int i = 0; i < n; i++) {
            char row[1024];
            table_row(row, sizeof(row), devs, devs3, i, maxw);
            if (text_contains_ci(row, query)) matches[count++] = i;
        }
        int pages = count ? (count + page_rows - 1) / page_rows : 1;
        if (page >= pages) page = pages - 1;

        printf("\033[H\033[2J%s%s:\n", heading,
               query[0] ? " (filtered)" : "");
        int first = page * page_rows;
        int last = first + page_rows;
        if (last > count) last = count;
        for (int i = first; i < last; i++) {
            char row[1024];
            table_row(row, sizeof(row), devs, devs3, matches[i], maxw);
            printf("%.*s\n", cols, row);
        }
        if (!count) printf("  No matches for \"%s\".\n", query);

        char status[256];
        snprintf(status, sizeof(status), " Page %d / %d   <-/-> page   / search   q quit%s%s ",
                 page + 1, pages, query[0] ? "   filter: " : "", query);
        printf("\033[30;42m%-*.*s\033[0m", cols, cols, status);
        fflush(stdout);

        int key = pager_read_key();
        if (key == 'q' || key == 'Q' || key == 0x1b || key == 0x03 || key == 0x04) break;
        if (key == '/' ) { pager_search(query, sizeof(query)); page = 0; }
        else if (key == 'n' || key == ' ' || key == '\r' || key == '\n') {
            if (page + 1 < pages) page++;
        } else if (key == 'p' || key == 'b') {
            if (page > 0) page--;
        }
    }
    putchar('\n');
    free(matches);
    pager_restore_terminal();
    return true;
}
#endif

void gemu_print_table(const char *heading, const GemuDevDesc *devs, int n) {
    int maxw = 0;
    for (int i = 0; i < n; i++) {
        int w = (int)strlen(devs[i].name);
        if (w > maxw) maxw = w;
    }
#ifndef _WIN32
    if (table_pager(heading, devs, NULL, n, maxw)) return;
#endif
    printf("%s:\n", heading);
    for (int i = 0; i < n; i++) {
        char row[1024];
        table_row(row, sizeof(row), devs, NULL, i, maxw);
        printf("%s\n", row);
    }
}

void gemu_print_table3(const char *heading, const GemuDevDesc3 *devs, int n) {
    int maxw = 0;
    for (int i = 0; i < n; i++) {
        int w = (int)strlen(devs[i].name);
        if (w > maxw) maxw = w;
    }
#ifndef _WIN32
    if (table_pager(heading, NULL, devs, n, maxw)) return;
#endif
    printf("%s:\n", heading);
    for (int i = 0; i < n; i++) {
        char row[1024];
        table_row(row, sizeof(row), NULL, devs, i, maxw);
        printf("%s\n", row);
    }
}

static void list_devices(const char *kind, const GemuDevDesc *devs, int n) {
    char heading[64];
    snprintf(heading, sizeof(heading), "Available %s", kind);
    gemu_print_table(heading, devs, n);
}

static void list_displays(unsigned mask) {
    /* Build a temporary table of only the supported entries */
    GemuDevDesc tmp[N_DISPLAYS];
    int n = 0;
    for (int i = 0; i < N_DISPLAYS; i++)
        if (mask & GEMU_DISP_F(display_table[i].type))
            tmp[n++] = (GemuDevDesc){ display_table[i].name, display_table[i].desc };

    gemu_print_table("Available display backends", tmp, n);
}

static void print_vnc_help(void) {
    printf("VNC address format:\n"
           "  :N            listen on all interfaces, port 5900+N\n"
           "  host:N        listen on host, port 5900+N\n"
           "  unix:/path    listen on a Unix domain socket (POSIX only)\n"
           "Examples:  :0   127.0.0.1:0   0.0.0.0:1\n");
}

static void split_machine_spec(const char *spec, char *machine, size_t machine_sz,
                               const char **opts) {
    const char *comma = strchr(spec, ',');
    size_t len = comma ? (size_t)(comma - spec) : strlen(spec);
    if (machine_sz > 0) {
        if (len >= machine_sz) len = machine_sz - 1;
        memcpy(machine, spec, len);
        machine[len] = '\0';
    }
    if (opts) *opts = comma ? comma + 1 : NULL;
}

static void print_monitor_help(void) {
    printf("Monitor backends:\n"
           "  stdio                                      console monitor on stdin/stdout\n"
           "  none                                       disable monitor\n"
           "  telnet:127.0.0.1:4444,server,nowait       listen for one telnet client\n"
           "  gmp:127.0.0.1:4444                        listen for GMP/QMP JSON clients\n"
           "  gmp-stdio                                  GMP/QMP JSON on stdin/stdout\n");
}

/* ── Validation helpers ──────────────────────────────────────────────────── */

static bool is_help(const char *s) {
    return strcmp(s, "?") == 0 || strcmp(s, "help") == 0;
}

static bool dev_validate(const char *prog, const char *flag,
                          const GemuDevDesc *devs, int n, const char *val) {
    for (int i = 0; i < n; i++)
        if (strcmp(devs[i].name, val) == 0) return true;
    fprintf(stderr, "%s: unknown %s '%s' (try %s ?)\n", prog, flag, val, flag);
    return false;
}

/* ── Address:file argument helper ────────────────────────────────────────── */

int gemu_parse_addr_arg(const char *prog, const char *arg,
                        uint32_t *addr, const char **path) {
    const char *colon = strchr(arg, ':');
    if (!colon) {
        *path = arg;
        return 0;
    }
    if (colon == arg) {
        fprintf(stderr, "%s: expected ADDR:FILE, got '%s'\n", prog, arg);
        return -1;
    }
    char buf[32];
    size_t len = (size_t)(colon - arg);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, arg, len);
    buf[len] = '\0';
    *addr = (uint32_t)strtoul(buf, NULL, 0);
    *path = colon + 1;
    if (!**path) {
        fprintf(stderr, "%s: missing file path in '%s'\n", prog, arg);
        return -1;
    }
    return 1;
}

/* ── Main parser ─────────────────────────────────────────────────────────── */

bool gemu_args_parse(int argc, char **argv,
                     const GemuArgsDef *def, GemuArgs *out,
                     int *rem_argc, char **rem_argv) {
    if (rem_argc) *rem_argc = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        /* ── Help ── */
        if (strcmp(a, "-h") == 0 || strcmp(a, "-help") == 0 ||
            strcmp(a, "--help") == 0) {
            print_usage(def);
            exit(0);
        }

        if (strcmp(a, "-no-shutdown") == 0) {
            out->no_shutdown = true;
            continue;
        }

        /* ── -M TYPE ── */
        if (strcmp(a, "-M") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: -M requires an argument\n", def->prog);
                return false;
            }
            const char *v = argv[++i];
            if (is_help(v)) {
                list_devices("machines", def->machines, def->n_machines);
                exit(0);
            }
            static char machine_name[128];
            const char *machine_opts = NULL;
            split_machine_spec(v, machine_name, sizeof(machine_name), &machine_opts);
            if (!machine_name[0]) {
                fprintf(stderr, "%s: -M requires a machine name before options\n", def->prog);
                return false;
            }
            if (!dev_validate(def->prog, "-M", def->machines, def->n_machines, machine_name))
                return false;
            out->machine = machine_name;
            out->machine_opts = machine_opts;
            continue;
        }

        /* ── -cpu TYPE ── */
        if (strcmp(a, "-cpu") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: -cpu requires an argument\n", def->prog);
                return false;
            }
            const char *v = argv[++i];
            if (is_help(v)) {
                list_devices("CPUs", def->cpus, def->n_cpus);
                exit(0);
            }
            if (def->n_cpus == 0) {
                fprintf(stderr, "%s: -cpu not supported\n", def->prog);
                return false;
            }
            if (!dev_validate(def->prog, "-cpu", def->cpus, def->n_cpus, v))
                return false;
            out->cpu = v;
            continue;
        }

        /* ── -vga TYPE ── */
        if (strcmp(a, "-vga") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: -vga requires an argument\n", def->prog);
                return false;
            }
            const char *v = argv[++i];
            if (is_help(v)) {
                list_devices("video chips", def->vgas, def->n_vgas);
                exit(0);
            }
            if (def->n_vgas == 0) {
                fprintf(stderr, "%s: -vga not supported\n", def->prog);
                return false;
            }
            if (!dev_validate(def->prog, "-vga", def->vgas, def->n_vgas, v))
                return false;
            out->vga = v;
            continue;
        }

        /* ── -display TYPE ── */
        if (strcmp(a, "-display") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: -display requires an argument\n", def->prog);
                return false;
            }
            const char *v = argv[++i];
            if (is_help(v)) {
                list_displays(def->display_mask);
                exit(0);
            }
            bool found = false;
            for (int d = 0; d < N_DISPLAYS; d++) {
                if (strcmp(display_table[d].name, v) == 0) {
                    out->display_type     = display_table[d].type;
                    out->display_explicit = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                fprintf(stderr, "%s: unknown display '%s' (try -display ?)\n",
                        def->prog, v);
                return false;
            }
            continue;
        }

        /* ── -scale N ── */
        if (strcmp(a, "-scale") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: -scale requires an argument\n", def->prog);
                return false;
            }
            int s = atoi(argv[++i]);
            if (s < 1) s = 1;
            out->display_scale = s;
            continue;
        }

        /* ── -vnc ADDR ── */
        if (strcmp(a, "-vnc") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: -vnc requires an argument\n", def->prog);
                return false;
            }
            const char *v = argv[++i];
            if (is_help(v)) { print_vnc_help(); exit(0); }
            if (!def->vnc_support) {
                fprintf(stderr, "%s: -vnc not supported\n", def->prog);
                return false;
            }
            out->vnc_addr = v;
            continue;
        }

        /* ── -monitor SPEC ── */
        if (strcmp(a, "-monitor") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: -monitor requires an argument\n", def->prog);
                return false;
            }
            const char *v = argv[++i];
            if (is_help(v)) { print_monitor_help(); exit(0); }
            out->monitor_spec = v;
            continue;
        }

        /* ── -gmp ADDR / -qmp ADDR ── */
        if (strcmp(a, "-gmp") == 0 || strcmp(a, "-qmp") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s requires an argument\n", def->prog, a);
                return false;
            }
            const char *v = argv[++i];
            if (is_help(v)) { print_monitor_help(); exit(0); }
            static char gmp_spec[256];
            if (strcmp(v, "stdio") == 0)
                snprintf(gmp_spec, sizeof(gmp_spec), "gmp-stdio");
            else
                snprintf(gmp_spec, sizeof(gmp_spec), "gmp:%s", v);
            out->monitor_spec = gmp_spec;
            continue;
        }

        /* ── Positional: ROM path ── */
        if (a[0] != '-') {
            out->rom_path = a;
            continue;
        }

        /* ── Unknown flag: pass back to caller, or error ── */
        if (rem_argv && rem_argc) {
            rem_argv[(*rem_argc)++] = argv[i];
            /* if the next token looks like a value (no leading -), consume it too */
            if (i + 1 < argc && argv[i + 1][0] != '-')
                rem_argv[(*rem_argc)++] = argv[++i];
        } else {
            fprintf(stderr, "%s: unknown option '%s' (try -h)\n", def->prog, a);
            return false;
        }
    }

    /* Validate the final display choice against what this binary supports */
    if (out->display_explicit &&
        !(def->display_mask & GEMU_DISP_F(out->display_type))) {
        const char *name = "unknown";
        for (int d = 0; d < N_DISPLAYS; d++)
            if (display_table[d].type == out->display_type) { name = display_table[d].name; break; }
        fprintf(stderr, "%s: display '%s' not supported (try -display ?)\n",
                def->prog, name);
        return false;
    }

    /* VNC without an explicit -display → headless */
    if (out->vnc_addr && !out->display_explicit)
        out->display_type = GEMU_DISPLAY_NONE;

    return true;
}
