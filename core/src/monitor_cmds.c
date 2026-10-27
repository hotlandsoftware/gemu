/*
 * GEMU monitor - command table, dispatch and built-in commands.
 *
 * Every monitor command is declared once in mon_cmds[] below.  Machines
 * never parse command lines themselves: they bind handlers to names from
 * this table with gemu_monitor_bind(), and 'help' is generated from it, so
 * the command set and its documentation are the same on every machine.
 *
 * Adding a command:
 *   - universal, works everywhere:   give it a builtin
 *   - universal, needs the machine:  builtin = NULL, machines = NULL, and
 *                                    bind it in each machine that can
 *   - machine-specific:              builtin = NULL, machines = "a b c",
 *                                    and bind it in those machines
 * Keep the table sorted by name; 'help' prints it in order.
 */
#include "monitor_priv.h"
#include "gemu/gemu_display.h"
#include "gemu/vnc.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define strcasecmp   _stricmp
#  define strncasecmp  _strnicmp
#else
#  include <strings.h>
#endif

typedef GemuMonCmd (*MonBuiltin)(GemuMonitor *mon, const char *args);

/* Facilities a built-in needs from the machine; the command is reported as
 * unsupported when the machine hasn't provided them. */
enum {
    NEED_MEM       = 1 << 0,
    NEED_MEM_WRITE = 1 << 1,
    NEED_SCREEN    = 1 << 2,
    NEED_FLIP      = 1 << 3,   /* some media device can flip sides */
};

typedef struct {
    const char *name;
    const char *aliases;   /* space-separated, or NULL */
    const char *params;
    const char *help;
    const char *machines;  /* NULL = every machine; else space-separated -M names */
    MonBuiltin  builtin;   /* NULL = the machine must bind it */
    unsigned    needs;
} MonCmdDef;

/* Machine groups for restricted commands (names as given to -M). */
#define M_NES       "nes nespal famicom dendy"
#define M_VS        "vssmb vsskatekids"
#define M_NES_ALL   M_NES " " M_VS
#define M_MAGICFLY  "magicfly 7mezzo"
#define M_DESTROYER "destroyer altair2"
#define M_UM6578    "um6578 asobou czdf1"
#define M_IA64      "i2000 generic-ia64"
#define M_A2600     "atari2600 atari2600pal atari2600secam"

static GemuMonCmd cmd_help(GemuMonitor *, const char *);
static GemuMonCmd cmd_info(GemuMonitor *, const char *);
static GemuMonCmd cmd_cpu(GemuMonitor *, const char *);
static GemuMonCmd cmd_cont(GemuMonitor *, const char *);
static GemuMonCmd cmd_stop(GemuMonitor *, const char *);
static GemuMonCmd cmd_step(GemuMonitor *, const char *);
static GemuMonCmd cmd_reset(GemuMonitor *, const char *);
static GemuMonCmd cmd_quit(GemuMonitor *, const char *);
static GemuMonCmd cmd_break(GemuMonitor *, const char *);
static GemuMonCmd cmd_rwatch(GemuMonitor *, const char *);
static GemuMonCmd cmd_wwatch(GemuMonitor *, const char *);
static GemuMonCmd cmd_watch(GemuMonitor *, const char *);
static GemuMonCmd cmd_delete(GemuMonitor *, const char *);
static GemuMonCmd cmd_change(GemuMonitor *, const char *);
static GemuMonCmd cmd_eject(GemuMonitor *, const char *);
static GemuMonCmd cmd_flip(GemuMonitor *, const char *);
static GemuMonCmd cmd_screendump(GemuMonitor *, const char *);
static GemuMonCmd cmd_x(GemuMonitor *, const char *);
static GemuMonCmd cmd_peek(GemuMonitor *, const char *);
static GemuMonCmd cmd_poke(GemuMonitor *, const char *);
static GemuMonCmd cmd_memsave(GemuMonitor *, const char *);
static GemuMonCmd cmd_set_password(GemuMonitor *, const char *);
static GemuMonCmd cmd_expire_password(GemuMonitor *, const char *);

static const MonCmdDef mon_cmds[] = {
    /* name          aliases         params                        help */
    { "atapistate",  NULL,           "",
      "show ATAPI controller state", "i2000", NULL, 0 },
    { "autoreload",  NULL,           "[on | off]",
      "reload the cartridge (and symbols) whenever the file changes", M_A2600, NULL, 0 },
    { "break",       "b",            "<addr>",
      "set exec breakpoint", NULL, cmd_break, 0 },
    { "breakline",   NULL,           "[<line> | off]",
      "stop when the beam reaches a scanline", M_A2600, NULL, 0 },
    { "calls",       NULL,           "[count]",
      "dump the recent call history to stderr", M_IA64, NULL, 0 },
    { "change",      NULL,           "<device> <file> | vnc password",
      "insert/change media, or set the VNC password", NULL, cmd_change, 0 },
    { "coin",        NULL,           "[slot]",
      "insert a coin", M_VS " 5clown " M_MAGICFLY, NULL, 0 },
    { "cont",        "c continue resume", "",
      "resume emulation", NULL, cmd_cont, 0 },
    { "cpu",         NULL,           "",
      "show CPU state (same as 'info cpu')", NULL, cmd_cpu, 0 },
    { "debugcolors", NULL,           "[on | off]",
      "draw each TIA object in a fixed colour; show blanking", M_A2600, NULL, 0 },
    { "delete",      "del",          "[N]",
      "delete breakpoint N (or all if N omitted)", NULL, cmd_delete, 0 },
    { "dipswitch",   "dip",          "[list | <switch> <value>]",
      "list or set DIP switches", M_VS " 5clown " M_MAGICFLY " " M_DESTROYER " " M_A2600, NULL, 0 },
    { "disas",       "u",            "[addr] [count]",
      "disassemble (default: from the PC)", M_A2600, NULL, 0 },
    { "dump",        NULL,           "<addr> <len> <file>",
      "write len bytes (both hex) of the system bus to file", M_IA64, NULL, 0 },
    { "eject",       NULL,           "<device>",
      "eject media", NULL, cmd_eject, 0 },
    { "expire_password", NULL,       "vnc now|never",
      "expire the VNC password now (or keep it, 'never')", NULL, cmd_expire_password, 0 },
    { "faultstats",  NULL,           "",
      "show CPU fault counters", M_IA64, NULL, 0 },
    { "flip",        NULL,           "<device>",
      "flip disk media to the next side", NULL, cmd_flip, NEED_FLIP },
    { "frame",       NULL,           "[count]",
      "run count frames (default 1), then stop", M_A2600, NULL, 0 },
    { "frameinfo",   NULL,           "[watch on | off]",
      "scanline counts and VSYNC/VBLANK of recent frames", M_A2600, NULL, 0 },
    { "gamegenie",   NULL,           "add <code> | list | delete <code>",
      "manage Game Genie codes", M_NES, NULL, 0 },
    { "help",        "?",            "[command | all]",
      "list commands, or describe one", NULL, cmd_help, 0 },
    { "hexeditor",   NULL,           "",
      "toggle the hex editor window", NULL, NULL, 0 },
    { "hide",        NULL,           "[p0 p1 m0 m1 bl pf | none]",
      "hide TIA objects from the picture (collisions still happen)", M_A2600, NULL, 0 },
    { "history",     NULL,           "[count]",
      "dump the most recently executed slots to stderr", M_IA64, NULL, 0 },
    { "info",        NULL,           "<subcommand>",
      "show machine information ('help info' lists subcommands)", NULL, cmd_info, 0 },
    { "input",       "input-state",  "",
      "show keyboard controller and PIT state", "i2000", NULL, 0 },
    { "key",         NULL,           "<key|text>",
      "inject a key (up, down, enter, ...) or type text", M_IA64, NULL, 0 },
    { "loadvm",      NULL,           "<file>",
      "restore machine state from file", NULL, NULL, 0 },
    { "mapperdump",  NULL,           "",
      "show cartridge mapper registers", M_NES_ALL " " M_A2600, NULL, 0 },
    { "memsave",     "pmemsave",     "<addr> <size> <file>",
      "save size bytes of memory at addr to file", NULL, cmd_memsave, NEED_MEM },
    { "mouse",       NULL,           "<x> <y> [left] [right] [none]",
      "move the synthetic mouse to an absolute position", M_UM6578, NULL, 0 },
    { "mouseclear",  NULL,           "",
      "return mouse control to the host pointer", M_UM6578, NULL, 0 },
    { "mousekick",   NULL,           "",
      "re-raise a stuck mouse interrupt", "i2000", NULL, 0 },
    { "mousemove",   NULL,           "<dx> <dy> [left] [right]",
      "move the mouse relative to its position", M_UM6578 " i2000", NULL, 0 },
    { "mousestate",  NULL,           "",
      "show mouse and controller state", M_UM6578 " i2000", NULL, 0 },
    { "panel",       NULL,           "<file>",
      "write the CPU/MMIO debug front panel image to file", "i2000", NULL, 0 },
    { "peek",        NULL,           "<addr> [len]",
      "hex dump len bytes of memory (default 16)", NULL, cmd_peek, NEED_MEM },
    { "poke",        NULL,           "<addr> <byte>...",
      "write bytes (hex) to memory", NULL, cmd_poke, NEED_MEM | NEED_MEM_WRITE },
    { "ppudump",     NULL,           "",
      "show PPU registers, palette, nametable and OAM", M_NES_ALL, NULL, 0 },
    { "quit",        "q",            "",
      "quit the emulator", NULL, cmd_quit, 0 },
    { "regpoke",     NULL,           "<reg> <value>",
      "set general register r<reg> to a hex value", "generic-ia64", NULL, 0 },
    { "reload",      NULL,           "",
      "reload the cartridge file and power-cycle", M_A2600, NULL, 0 },
    { "reset",       "system_reset", "[keys]",
      "reset the machine ('reset keys' restores default input bindings)",
      NULL, cmd_reset, 0 },
    { "riot",        NULL,           "",
      "show the 6532 RIOT timer and ports", M_A2600, NULL, 0 },
    { "rwatch",      NULL,           "<addr>",
      "set read watchpoint", NULL, cmd_rwatch, 0 },
    { "savevm",      NULL,           "<file>",
      "save machine state to file", NULL, NULL, 0 },
    { "scanline",    NULL,           "[count]",
      "run count scanlines (default 1), then stop", M_A2600, NULL, 0 },
    { "screendump",  NULL,           "<file>[.png]",
      "save screenshot (PPM or PNG)", NULL, cmd_screendump, NEED_SCREEN },
    { "sendkey",     NULL,           "<keys>",
      "press keys/buttons on the machine's input device", NULL, NULL, 0 },
    { "service",     NULL,           "",
      "press the service button", M_VS, NULL, 0 },
    { "set_password", NULL,          "vnc <password>",
      "set the VNC password (needs -vnc ...,password=on)", NULL, cmd_set_password, 0 },
    { "step",        "s",            "[count]",
      "execute count instructions (default 1), then stop", NULL, cmd_step, 0 },
    { "stop",        "halt",         "",
      "halt emulation", NULL, cmd_stop, 0 },
    { "symbols",     "sym",          "[load <file> | reload | list [text] | clear]",
      "load assembler labels; addresses then accept names", NULL, mon_cmd_symbols, 0 },
    { "tia",         NULL,           "",
      "show TIA registers, object positions and collisions", M_A2600, NULL, 0 },
    { "trace",       NULL,           "<count> | <file> | off",
      "trace execution (ia64: next count slots to stderr; atari2600: every instruction to file)",
      M_IA64 " " M_A2600, NULL, 0 },
    { "view",        NULL,           "[tv | full]",
      "show the TV picture or the full-frame debug view", M_A2600, NULL, 0 },
    { "vgapeek",     NULL,           "<plane> <offset> <count>",
      "dump VGA plane memory", "generic-ia64", NULL, 0 },
    { "vgareg",      NULL,           "",
      "show VGA registers", "generic-ia64", NULL, 0 },
    { "vhptstats",   NULL,           "",
      "show VHPT walker statistics", M_IA64, NULL, 0 },
    { "watch",       "awatch",       "<addr>",
      "set read+write watchpoint", NULL, cmd_watch, 0 },
    { "wwatch",      NULL,           "<addr>",
      "set write watchpoint", NULL, cmd_wwatch, 0 },
    { "x",           NULL,           "[/fmt] <addr> [count]",
      "examine memory; fmt is [count][x|d|u|o|c][b|h|w|g]", NULL, cmd_x, NEED_MEM },
    { "xp",          NULL,           "[/fmt] <addr> [count]",
      "examine physical memory (same as x on machines without an MMU)",
      NULL, cmd_x, NEED_MEM },
};

#define N_CMDS (sizeof(mon_cmds) / sizeof(mon_cmds[0]))

/* ── Parsing helpers ─────────────────────────────────────────────────────── */

static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Copy the next whitespace-delimited token into out; returns the text after
 * it, or NULL if there is no token. */
static const char *take_token(const char *s, char *out, size_t out_len) {
    s = skip_ws(s);
    if (!*s) return NULL;
    size_t n = 0;
    while (*s && !isspace((unsigned char)*s)) {
        if (n + 1 < out_len) out[n++] = *s;
        s++;
    }
    out[n] = '\0';
    return s;
}

/* Strip surrounding whitespace and one pair of matching quotes. */
static void unquote(const char *in, char *out, size_t out_len) {
    in = skip_ws(in);
    snprintf(out, out_len, "%s", in);
    size_t len = strlen(out);
    while (len && isspace((unsigned char)out[len - 1])) out[--len] = '\0';
    if (len >= 2 && (out[0] == '"' || out[0] == '\'') && out[len - 1] == out[0]) {
        memmove(out, out + 1, len - 2);
        out[len - 2] = '\0';
    }
}

/* The monitor running the current command, for symbol lookups. */
static const GemuMonitor *cur_mon;

/* Addresses and bytes are hex by default ("1234", "0x1234", "$1234"), or
 * a loaded symbol ("Kernel", "Score+2"). */
static bool parse_hex(const char *s, uint64_t *out) {
    return gemu_monitor_parse_addr(cur_mon, s, out);
}

/* Counts and sizes are decimal unless prefixed ("16", "0x10"). */
static bool parse_count(const char *s, uint64_t *out) {
    if (*s == '$') return parse_hex(s, out);
    if (!*s) return false;
    char *end;
    *out = strtoull(s, &end, 0);
    return *end == '\0';
}

static bool word_in_list(const char *list, const char *word) {
    size_t wlen = strlen(word);
    const char *p = list;
    while (p && *p) {
        p = skip_ws(p);
        const char *e = p;
        while (*e && !isspace((unsigned char)*e)) e++;
        if ((size_t)(e - p) == wlen && strncasecmp(p, word, wlen) == 0)
            return true;
        p = e;
    }
    return false;
}

/* ── Table lookup and availability ───────────────────────────────────────── */

static const MonCmdDef *find_def(const char *word) {
    for (size_t i = 0; i < N_CMDS; i++) {
        if (strcasecmp(mon_cmds[i].name, word) == 0 ||
            (mon_cmds[i].aliases && word_in_list(mon_cmds[i].aliases, word)))
            return &mon_cmds[i];
    }
    return NULL;
}

static const MonBinding *find_binding(const GemuMonitor *mon, const MonCmdDef *def) {
    for (int i = 0; i < mon->n_bindings; i++)
        if (mon->bindings[i].name == def->name)
            return &mon->bindings[i];
    return NULL;
}

typedef enum {
    AVAIL_OK,
    AVAIL_WRONG_MACHINE,  /* restricted to machines other than this one */
    AVAIL_UNSUPPORTED,    /* allowed here, but the machine doesn't provide it */
} MonAvail;

static bool machine_allowed(const MonCmdDef *def) {
    const char *machine = mon_machine_name();
    return !def->machines || !machine || word_in_list(def->machines, machine);
}

static MonAvail availability(const GemuMonitor *mon, const MonCmdDef *def) {
    if (!machine_allowed(def))
        return AVAIL_WRONG_MACHINE;
    if (find_binding(mon, def))
        return AVAIL_OK;
    if (!def->builtin)
        return AVAIL_UNSUPPORTED;
    if ((def->needs & NEED_MEM) && !mon->has_mem)
        return AVAIL_UNSUPPORTED;
    if ((def->needs & NEED_MEM_WRITE) && !(mon->has_mem && mon->mem.write))
        return AVAIL_UNSUPPORTED;
    if ((def->needs & NEED_SCREEN) && !mon->screendump_cb)
        return AVAIL_UNSUPPORTED;
    if (def->needs & NEED_FLIP) {
        bool any = false;
        for (int i = 0; i < mon->n_media; i++) any |= mon->media[i].flip != NULL;
        if (!any) return AVAIL_UNSUPPORTED;
    }
    return AVAIL_OK;
}

/* "nes nespal famicom" -> "nes, nespal, famicom" */
static void format_machine_list(const char *list, char *out, size_t out_len) {
    out[0] = '\0';
    char word[64];
    const char *p = list;
    while ((p = take_token(p, word, sizeof(word))) != NULL) {
        size_t used = strlen(out);
        snprintf(out + used, out_len - used, "%s%s", used ? ", " : "", word);
    }
}

bool gemu_monitor_bind(GemuMonitor *mon, const char *name,
                       GemuMonHandler fn, void *ud) {
    if (!mon || !name || !fn) return false;
    const MonCmdDef *def = find_def(name);
    if (!def || strcasecmp(def->name, name) != 0) {
        fprintf(stderr, "monitor: cannot bind '%s': not in the command table "
                        "(add it to mon_cmds[] in core/src/monitor_cmds.c)\n", name);
        return false;
    }
    if (!machine_allowed(def))
        fprintf(stderr, "monitor: '%s' is bound but not enabled for machine '%s' "
                        "(update its machine list in core/src/monitor_cmds.c)\n",
                name, mon_machine_name());
    MonBinding *b = (MonBinding *)find_binding(mon, def);
    if (!b) {
        if (mon->n_bindings >= MON_BIND_MAX) {
            fprintf(stderr, "monitor: too many bound commands\n");
            return false;
        }
        b = &mon->bindings[mon->n_bindings++];
    }
    b->name = def->name;
    b->fn = fn;
    b->ud = ud;
    return true;
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */

GemuMonCmd mon_dispatch(GemuMonitor *mon, const char *line) {
    char verb[64];
    const char *args = take_token(line, verb, sizeof(verb));
    if (!args) return GEMU_MON_NONE;

    char argbuf[MON_LINE_MAX];
    snprintf(argbuf, sizeof(argbuf), "%s", skip_ws(args));
    size_t len = strlen(argbuf);
    while (len && isspace((unsigned char)argbuf[len - 1])) argbuf[--len] = '\0';

    cur_mon = mon;
    const MonCmdDef *def = find_def(verb);
    if (!def) {
        mon_printf(mon, "unknown command '%s' (try 'help')\n", verb);
        return GEMU_MON_NONE;
    }

    char machines[512];
    switch (availability(mon, def)) {
    case AVAIL_WRONG_MACHINE:
        format_machine_list(def->machines, machines, sizeof(machines));
        mon_printf(mon, "%s: only available on %s\n", def->name, machines);
        return GEMU_MON_NONE;
    case AVAIL_UNSUPPORTED:
        mon_printf(mon, "%s: not supported on this machine\n", def->name);
        return GEMU_MON_NONE;
    case AVAIL_OK:
        break;
    }

    const MonBinding *b = find_binding(mon, def);
    if (b) return b->fn(mon, argbuf, b->ud);
    return def->builtin(mon, argbuf);
}

bool mon_line_is_quit(const char *line) {
    char verb[64];
    if (!take_token(line, verb, sizeof(verb))) return false;
    const MonCmdDef *def = find_def(verb);
    return def && def->builtin == cmd_quit;
}

bool mon_line_is_vnc_password(const char *line) {
    char a[16], b[16], c[16], extra[2];
    const char *p = line;
    if (!(p = take_token(p, a, sizeof(a))) || strcasecmp(a, "change") != 0) return false;
    if (!(p = take_token(p, b, sizeof(b))) || strcasecmp(b, "vnc") != 0) return false;
    if (!(p = take_token(p, c, sizeof(c))) || strcasecmp(c, "password") != 0) return false;
    return take_token(p, extra, sizeof(extra)) == NULL;
}

/* ── info ────────────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *aliases;
    const char *help;
    void      (*fn)(GemuMonitor *mon);
} MonInfoDef;

static void help_command(GemuMonitor *mon, const MonCmdDef *def);

static void info_block(GemuMonitor *mon) {
    if (mon->n_media == 0) {
        mon_printf(mon, "No block devices.\n");
        return;
    }
    /* QEMU's layout: "name: file (format...)" or "[not inserted]". */
    for (int i = 0; i < mon->n_media; i++) {
        const GemuMediaDevice *dev = &mon->media[i];
        const MonTray *tray = &mon->tray[i];
        char status[600];
        if (tray->open) {   /* the device itself is empty */
            snprintf(status, sizeof(status), "%s", tray->file[0] ? tray->file : "[not inserted]");
        } else if (dev->status) {
            status[0] = '\0';
            dev->status(dev->ud, status, sizeof(status));
        } else {
            snprintf(status, sizeof(status), "%s", dev->file[0] ? dev->file : "[not inserted]");
        }
        mon_printf(mon, "%s: %s\n", dev->name, status);
        if (dev->eject)
            mon_printf(mon, "    Removable device: not locked, tray %s\n",
                       tray->open ? "open" : "closed");
        if (dev->flip)
            mon_printf(mon, "    Flippable: yes ('flip %s')\n", dev->name);
    }
}

static void info_roms(GemuMonitor *mon) {
    if (mon->n_rom_entries == 0) {
        mon_printf(mon, "No ROMs loaded.\n");
        return;
    }
    for (int i = 0; i < mon->n_rom_entries; i++) {
        const MonRomEntry *e = &mon->rom_entries[i];
        mon_printf(mon, "addr=0x%04x size=0x%04x file=%s\n",
                   e->addr, e->size, e->file);
    }
}

static void info_breakpoints(GemuMonitor *mon) {
    if (mon->n_bps == 0) {
        mon_printf(mon, "No breakpoints set.\n");
        return;
    }
    for (int i = 0; i < mon->n_bps; i++) {
        const MonBpEntry *e = &mon->bp_entries[i];
        const char *type;
        if      (e->type == GEMU_BP_EXEC)  type = "exec ";
        else if (e->type == GEMU_BP_READ)  type = "read ";
        else if (e->type == GEMU_BP_WRITE) type = "write";
        else                               type = "rw   ";
        mon_printf(mon, "  %2d  %s  0x%04x\n", e->id, type, e->addr);
    }
}

static void info_cpu(GemuMonitor *mon) {
    if (!mon->cpu_state_cb) {
        mon_printf(mon, "CPU state is not available for this machine.\n");
        return;
    }
    /* 64 KiB: 128 GRs plus IA-64's full TLB dump (dtr/itr/dtc/itc). */
    static char buf[65536];
    buf[0] = '\0';
    mon->cpu_state_cb(mon->cpu_state_ud, buf, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    mon_printf(mon, "%s\n", buf);
}

static void info_status(GemuMonitor *mon) {
    mon_printf(mon, "VM status: %s\n", mon->paused ? "paused" : "running");
}

static void info_machine(GemuMonitor *mon) {
    const char *machine = mon_machine_name();
    mon_printf(mon, "%s\n", machine ? machine : "(unknown)");
}

static void info_vnc(GemuMonitor *mon) {
    char buf[2048];
    gemu_vnc_info(gemu_vnc_get(), buf, sizeof(buf));
    mon_printf(mon, "%s", buf);
}

static void info_name(GemuMonitor *mon) {
    const char *name = gemu_vm_name();
    if (name) mon_printf(mon, "%s\n", name);
}

static void info_media_status(GemuMonitor *mon) {
    bool any = false;
    for (int i = 0; i < mon->n_media; i++) {
        GemuMediaDevice *dev = &mon->media[i];
        if (!dev->status) continue;
        char buf[512] = "";
        dev->status(dev->ud, buf, sizeof(buf));
        mon_printf(mon, "%s: %s\n", dev->name, buf);
        any = true;
    }
    if (!any) mon_printf(mon, "No media status available.\n");
}

static const MonInfoDef info_cmds[] = {
    { "block",       NULL,        "list block devices",          info_block },
    { "breakpoints", "bp",        "list breakpoints and watchpoints", info_breakpoints },
    { "cpu",         "registers", "show CPU state",              info_cpu },
    { "machine",     NULL,        "show the emulated machine",   info_machine },
    { "media",       NULL,        "show media device status",    info_media_status },
    { "name",        NULL,        "show the VM name given with -name", info_name },
    { "roms",        NULL,        "list loaded ROM images",      info_roms },
    { "status",      NULL,        "show whether emulation is running", info_status },
    { "vnc",         NULL,        "show the VNC server and its clients", info_vnc },
};
static const size_t n_info_cmds = sizeof(info_cmds) / sizeof(info_cmds[0]);

static GemuMonCmd cmd_info(GemuMonitor *mon, const char *args) {
    char sub[32];
    if (!take_token(args, sub, sizeof(sub))) {
        help_command(mon, find_def("info"));
        return GEMU_MON_NONE;
    }
    for (size_t i = 0; i < n_info_cmds; i++) {
        if (strcasecmp(sub, info_cmds[i].name) == 0 ||
            (info_cmds[i].aliases && word_in_list(info_cmds[i].aliases, sub))) {
            info_cmds[i].fn(mon);
            return GEMU_MON_NONE;
        }
    }
    mon_printf(mon, "info: unknown subcommand '%s' (try 'help info')\n", sub);
    return GEMU_MON_NONE;
}

static GemuMonCmd cmd_cpu(GemuMonitor *mon, const char *args) {
    if (*args) mon_printf(mon, "usage: cpu\n");
    else       info_cpu(mon);
    return GEMU_MON_NONE;
}

/* ── help ────────────────────────────────────────────────────────────────── */


static void format_synopsis(const MonCmdDef *def, char *out, size_t out_len) {
    snprintf(out, out_len, "%s", def->name);
    char word[32];
    const char *p = def->aliases;
    while (p && (p = take_token(p, word, sizeof(word))) != NULL) {
        size_t used = strlen(out);
        snprintf(out + used, out_len - used, " / %s", word);
    }
    if (def->params[0]) {
        size_t used = strlen(out);
        snprintf(out + used, out_len - used, " %s", def->params);
    }
}

#define HELP_COL 36

/* "<mark> <synopsis> -- <help><note>", moving the help to its own line when
 * the synopsis is too long to keep the column aligned. */
static void print_help_row(GemuMonitor *mon, char mark, const char *syn,
                           const char *help, const char *note) {
    if (strlen(syn) > HELP_COL)
        mon_printf(mon, "%c %s\n  %-*s -- %s%s\n", mark, syn, HELP_COL, "", help, note);
    else
        mon_printf(mon, "%c %-*s -- %s%s\n", mark, HELP_COL, syn, help, note);
}

static void print_help_line(GemuMonitor *mon, const MonCmdDef *def) {
    char syn[160];
    format_synopsis(def, syn, sizeof(syn));
    const char *note = "";
    if (availability(mon, def) == AVAIL_UNSUPPORTED && !def->machines)
        note = " (not supported on this machine)";
    print_help_row(mon, ' ', syn, def->help, note);
}

static void help_command(GemuMonitor *mon, const MonCmdDef *def) {
    char syn[160];
    format_synopsis(def, syn, sizeof(syn));
    mon_printf(mon, "%s\n    %s\n", syn, def->help);
    if (def->machines) {
        char machines[512];
        format_machine_list(def->machines, machines, sizeof(machines));
        mon_printf(mon, "    machines: %s\n", machines);
    }
    switch (availability(mon, def)) {
    case AVAIL_WRONG_MACHINE:
        mon_printf(mon, "    not available on this machine\n");
        break;
    case AVAIL_UNSUPPORTED:
        mon_printf(mon, "    not supported on this machine\n");
        break;
    case AVAIL_OK:
        break;
    }
    if (def->builtin == cmd_info) {
        mon_printf(mon, "subcommands:\n");
        for (size_t i = 0; i < n_info_cmds; i++) {
            char name[64];
            snprintf(name, sizeof(name), "info %s%s%s", info_cmds[i].name,
                     info_cmds[i].aliases ? " / " : "",
                     info_cmds[i].aliases ? info_cmds[i].aliases : "");
            print_help_row(mon, ' ', name, info_cmds[i].help, "");
        }
    }
}

static GemuMonCmd cmd_help(GemuMonitor *mon, const char *args) {
    char word[64];
    bool all = false;
    if (take_token(args, word, sizeof(word))) {
        if (strcasecmp(word, "all") != 0) {
            const MonCmdDef *def = find_def(word);
            if (def) help_command(mon, def);
            else     mon_printf(mon, "help: unknown command '%s'\n", word);
            return GEMU_MON_NONE;
        }
        all = true;
    }

    /* Only what this machine can run, unless "help all". */
    for (size_t i = 0; i < N_CMDS; i++)
        if (!mon_cmds[i].machines && (all || availability(mon, &mon_cmds[i]) == AVAIL_OK))
            print_help_line(mon, &mon_cmds[i]);

    const char *machine = mon_machine_name();
    bool header = false;
    for (size_t i = 0; i < N_CMDS; i++) {
        const MonCmdDef *def = &mon_cmds[i];
        if (!def->machines) continue;
        bool here = machine && availability(mon, def) == AVAIL_OK;
        if (!here && !all) continue;
        if (!header) {
            if (all)
                mon_printf(mon, "\nMachine-specific commands%s%s%s:\n",
                           machine ? " (* = available on " : "",
                           machine ? machine : "", machine ? ")" : "");
            else
                mon_printf(mon, "\n%s%s commands:\n", machine ? machine : "Machine",
                           machine ? "" : "-specific");
            header = true;
        }
        char syn[160];
        format_synopsis(def, syn, sizeof(syn));
        print_help_row(mon, all && here ? '*' : ' ', syn, def->help, "");
    }
    if (!all)
        mon_printf(mon, "\n'help <command>' describes one; 'help all' lists every machine's.\n");
    return GEMU_MON_NONE;
}

/* ── Execution control ───────────────────────────────────────────────────── */

static GemuMonCmd cmd_cont(GemuMonitor *mon, const char *args) {
    (void)mon; (void)args;
    return GEMU_MON_CONT;
}

static GemuMonCmd cmd_stop(GemuMonitor *mon, const char *args) {
    (void)mon; (void)args;
    return GEMU_MON_STOP;
}

static GemuMonCmd cmd_step(GemuMonitor *mon, const char *args) {
    uint64_t count = 1;
    if (*args && (!parse_count(args, &count) || count == 0 || count > UINT32_MAX)) {
        mon_printf(mon, "usage: step [count]\n");
        return GEMU_MON_NONE;
    }
    mon->last_step_count = (uint32_t)count;
    return GEMU_MON_STEP;
}

static GemuMonCmd cmd_reset(GemuMonitor *mon, const char *args) {
    if (!*args) return GEMU_MON_RESET;
    if (strcasecmp(args, "keys") != 0) {
        mon_printf(mon, "usage: reset [keys]\n");
    } else if (!mon->input_reset_cb) {
        mon_printf(mon, "reset keys: input reset is not available\n");
    } else {
        mon->input_reset_cb(mon->input_reset_ud);
        mon_printf(mon, "input bindings reset to defaults\n");
    }
    return GEMU_MON_NONE;
}

static GemuMonCmd cmd_quit(GemuMonitor *mon, const char *args) {
    (void)mon; (void)args;
    return GEMU_MON_QUIT;
}

/* ── Breakpoints ─────────────────────────────────────────────────────────── */

static void recompute_bp_flags(GemuMonitor *mon) {
    mon->has_exec_bp = false;
    mon->has_mem_bp  = false;
    for (int i = 0; i < mon->n_bps; i++) {
        if (mon->bp_entries[i].type & GEMU_BP_EXEC)
            mon->has_exec_bp = true;
        if (mon->bp_entries[i].type & (GEMU_BP_READ | GEMU_BP_WRITE))
            mon->has_mem_bp = true;
    }
}

static GemuMonCmd add_bp_cmd(GemuMonitor *mon, const char *args,
                             GemuBpType type, const char *verb,
                             const char *what) {
    uint64_t addr;
    if (!*args || !parse_hex(args, &addr) || addr > UINT32_MAX) {
        mon_printf(mon, "usage: %s <addr>\n", verb);
        return GEMU_MON_NONE;
    }
    if (mon->n_bps >= BP_MAX) {
        mon_printf(mon, "%s: too many breakpoints (max %d)\n", verb, BP_MAX);
        return GEMU_MON_NONE;
    }
    MonBpEntry *e = &mon->bp_entries[mon->n_bps++];
    e->id   = ++mon->next_bp_id;
    e->type = type;
    e->addr = (uint32_t)addr;
    recompute_bp_flags(mon);
    mon_printf(mon, "%s %d set at 0x%04x\n", what, e->id, e->addr);
    return GEMU_MON_NONE;
}

static GemuMonCmd cmd_break(GemuMonitor *mon, const char *args) {
    return add_bp_cmd(mon, args, GEMU_BP_EXEC, "break", "Exec breakpoint");
}

static GemuMonCmd cmd_rwatch(GemuMonitor *mon, const char *args) {
    return add_bp_cmd(mon, args, GEMU_BP_READ, "rwatch", "Read watchpoint");
}

static GemuMonCmd cmd_wwatch(GemuMonitor *mon, const char *args) {
    return add_bp_cmd(mon, args, GEMU_BP_WRITE, "wwatch", "Write watchpoint");
}

static GemuMonCmd cmd_watch(GemuMonitor *mon, const char *args) {
    return add_bp_cmd(mon, args, (GemuBpType)(GEMU_BP_READ | GEMU_BP_WRITE),
                      "watch", "Access watchpoint");
}

static GemuMonCmd cmd_delete(GemuMonitor *mon, const char *args) {
    if (!*args) {
        mon->n_bps = 0;
        recompute_bp_flags(mon);
        mon_printf(mon, "All breakpoints deleted.\n");
        return GEMU_MON_NONE;
    }
    uint64_t id;
    if (!parse_count(args, &id)) {
        mon_printf(mon, "usage: delete [N]\n");
        return GEMU_MON_NONE;
    }
    for (int i = 0; i < mon->n_bps; i++) {
        if ((uint64_t)mon->bp_entries[i].id != id) continue;
        memmove(&mon->bp_entries[i], &mon->bp_entries[i + 1],
                (size_t)(mon->n_bps - i - 1) * sizeof(MonBpEntry));
        mon->n_bps--;
        recompute_bp_flags(mon);
        mon_printf(mon, "Breakpoint %d deleted.\n", (int)id);
        return GEMU_MON_NONE;
    }
    mon_printf(mon, "No breakpoint with id %d.\n", (int)id);
    return GEMU_MON_NONE;
}

static bool check_bp(GemuMonitor *mon, GemuBpType type, uint32_t addr,
                     const char *what, const char *kind) {
    uint32_t m = mon->addr_mask ? mon->addr_mask : 0xFFFFFFFFu;
    for (int i = 0; i < mon->n_bps; i++) {
        if ((mon->bp_entries[i].type & type) && ((mon->bp_entries[i].addr ^ addr) & m) == 0) {
            const char *sym = gemu_monitor_symbol_name(mon, addr);
            mon_printf(mon, "%s %d hit: %s at 0x%04x%s%s\n",
                       what, mon->bp_entries[i].id, kind, addr,
                       sym ? " " : "", sym ? sym : "");
            mon->paused = true;
            return true;
        }
    }
    return false;
}

bool gemu_monitor_check_exec(GemuMonitor *mon, uint32_t addr) {
    if (!mon || !mon->has_exec_bp) return false;
    /* Machines check before executing, so resuming at a breakpoint would
     * stop on it again; let the instruction that was stopped on run. */
    if (mon->bp_resume_skip && addr == mon->bp_resume_addr) {
        mon->bp_resume_skip = false;
        return false;
    }
    mon->bp_resume_skip = false;
    if (!check_bp(mon, GEMU_BP_EXEC, addr, "Breakpoint", "exec")) return false;
    mon->bp_resume_skip = true;
    mon->bp_resume_addr = addr;
    return true;
}

bool gemu_monitor_check_read(GemuMonitor *mon, uint32_t addr) {
    if (!mon || !mon->has_mem_bp) return false;
    return check_bp(mon, GEMU_BP_READ, addr, "Watchpoint", "read");
}

bool gemu_monitor_check_write(GemuMonitor *mon, uint32_t addr) {
    if (!mon || !mon->has_mem_bp) return false;
    return check_bp(mon, GEMU_BP_WRITE, addr, "Watchpoint", "write");
}

bool gemu_monitor_has_exec_breakpoints(const GemuMonitor *mon) {
    return mon && mon->has_exec_bp;
}

bool gemu_monitor_has_mem_breakpoints(const GemuMonitor *mon) {
    return mon && mon->has_mem_bp;
}

/* ── Media ───────────────────────────────────────────────────────────────── */

static GemuMediaDevice *find_media(GemuMonitor *mon, const char *name) {
    for (int i = 0; i < mon->n_media; i++)
        if (strcasecmp(mon->media[i].name, name) == 0)
            return &mon->media[i];
    return NULL;
}

GemuMediaDevice *gemu_monitor_find_media(GemuMonitor *mon, const char *name) {
    return mon && name ? find_media(mon, name) : NULL;
}

/* Common prologue for change/eject/flip: look up the device or explain. */
static GemuMediaDevice *media_arg(GemuMonitor *mon, const char *verb,
                                  const char *args, const char **rest) {
    char name[64];
    const char *p = take_token(args, name, sizeof(name));
    if (!p) {
        mon_printf(mon, "usage: %s <device>%s\n", verb,
                   strcmp(verb, "change") == 0 ? " <file>" : "");
        return NULL;
    }
    GemuMediaDevice *dev = find_media(mon, name);
    if (!dev) {
        mon_printf(mon, "%s: no such media device '%s' (try 'info block')\n",
                   verb, name);
        return NULL;
    }
    *rest = skip_ws(p);
    return dev;
}

static GemuMonCmd media_result(GemuMonitor *mon, const char *verb,
                               GemuMediaDevice *dev, GemuMediaResult result,
                               const char *err) {
    if (result == GEMU_MEDIA_ERR) {
        if (err[0]) mon_printf(mon, "%s: %s\n", verb, err);
        else        mon_printf(mon, "%s: failed for media device '%s'\n", verb, dev->name);
        return GEMU_MON_NONE;
    }
    return (result == GEMU_MEDIA_OK_RESET) ? GEMU_MON_RESET : GEMU_MON_NONE;
}

static GemuMonCmd do_eject(GemuMonitor *mon, const char *verb, GemuMediaDevice *dev) {
    if (!dev->eject) {
        mon_printf(mon, "%s: device '%s' cannot be ejected\n", verb, dev->name);
        return GEMU_MON_NONE;
    }
    char err[256] = "";
    GemuMediaResult r = dev->eject(dev->ud, err, sizeof(err));
    if (r != GEMU_MEDIA_ERR) {
        dev->file[0] = '\0';
        mon_block_media_changed(mon, dev, true);
    }
    return media_result(mon, verb, dev, r, err);
}

static GemuMonCmd cmd_change(GemuMonitor *mon, const char *args) {
    /* Consoles intercept "change vnc password" in the reader thread so they
     * can prompt; this is only reached via QMP, which has nothing to read
     * a password from. */
    if (strcasecmp(args, "vnc password") == 0) {
        mon_printf(mon, "change vnc password: needs an interactive monitor\n");
        return GEMU_MON_NONE;
    }
    const char *rest;
    GemuMediaDevice *dev = media_arg(mon, "change", args, &rest);
    if (!dev) return GEMU_MON_NONE;

    char file[MON_LINE_MAX];
    unquote(rest, file, sizeof(file));
    if (!file[0]) {
        mon_printf(mon, "usage: change <device> <file>\n");
        return GEMU_MON_NONE;
    }
    if (strcasecmp(file, "eject") == 0)
        return do_eject(mon, "change", dev);
    if (!dev->change) {
        mon_printf(mon, "change: device '%s' cannot be changed\n", dev->name);
        return GEMU_MON_NONE;
    }
    char err[256] = "";
    GemuMediaResult r = dev->change(dev->ud, file, err, sizeof(err));
    if (r != GEMU_MEDIA_ERR) {
        snprintf(dev->file, sizeof(dev->file), "%s", file);
        mon_block_media_changed(mon, dev, false);
    }
    return media_result(mon, "change", dev, r, err);
}

static GemuMonCmd cmd_eject(GemuMonitor *mon, const char *args) {
    const char *rest;
    GemuMediaDevice *dev = media_arg(mon, "eject", args, &rest);
    if (!dev) return GEMU_MON_NONE;
    if (*rest) {
        mon_printf(mon, "usage: eject <device>\n");
        return GEMU_MON_NONE;
    }
    return do_eject(mon, "eject", dev);
}

static GemuMonCmd cmd_flip(GemuMonitor *mon, const char *args) {
    const char *rest;
    GemuMediaDevice *dev = media_arg(mon, "flip", args, &rest);
    if (!dev) return GEMU_MON_NONE;
    if (*rest) {
        mon_printf(mon, "usage: flip <device>\n");
        return GEMU_MON_NONE;
    }
    if (!dev->flip) {
        mon_printf(mon, "flip: device '%s' cannot be flipped\n", dev->name);
        return GEMU_MON_NONE;
    }
    char err[256] = "";
    GemuMediaResult r = dev->flip(dev->ud, err, sizeof(err));
    return media_result(mon, "flip", dev, r, err);
}

static GemuMonCmd cmd_screendump(GemuMonitor *mon, const char *args) {
    char path[MON_LINE_MAX];
    unquote(args, path, sizeof(path));
    if (!path[0]) {
        mon_printf(mon, "usage: screendump <filename>[.png]\n");
        return GEMU_MON_NONE;
    }
    if (!mon->screendump_cb(mon->screendump_ud, path))
        mon_printf(mon, "screendump: failed to write %s\n", path);
    return GEMU_MON_NONE;
}

/* ── Memory ──────────────────────────────────────────────────────────────── */

static uint64_t addr_mask(const GemuMonitor *mon) {
    unsigned bits = mon->mem.addr_bits;
    return bits >= 64 ? UINT64_MAX : ((UINT64_C(1) << bits) - 1);
}

static int addr_digits(const GemuMonitor *mon) {
    return (int)((mon->mem.addr_bits + 3) / 4);
}

static bool mem_read(GemuMonitor *mon, uint64_t addr, uint8_t *val) {
    return mon->mem.read(mon->mem.ud, addr & addr_mask(mon), val);
}

/* Read a size-byte unit in the machine's byte order. */
static bool mem_read_unit(GemuMonitor *mon, uint64_t addr, unsigned size,
                          uint64_t *out) {
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        uint8_t b;
        if (!mem_read(mon, addr + i, &b)) return false;
        unsigned shift = mon->mem.big_endian ? (size - 1 - i) * 8 : i * 8;
        v |= (uint64_t)b << shift;
    }
    *out = v;
    return true;
}

/* Hex dump count units of size bytes from addr, in the given format. */
static void dump_memory(GemuMonitor *mon, uint64_t addr, uint64_t count,
                        unsigned size, char format) {
    unsigned per_line = (format == 'c') ? 16 : 16 / size;
    int width = (int)size * 2;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t a = (addr + i * size) & addr_mask(mon);
        if (i % per_line == 0)
            mon_printf(mon, "%s%0*" PRIx64 ":", i ? "\n" : "", addr_digits(mon), a);
        uint64_t v;
        if (!mem_read_unit(mon, a, size, &v)) {
            mon_printf(mon, " %.*s", format == 'c' ? 1 : width,
                       "----------------");
            continue;
        }
        switch (format) {
        case 'd': {
            int64_t sv = (size < 8 && (v >> (size * 8 - 1)) & 1)
                       ? (int64_t)(v | ~((UINT64_C(1) << (size * 8)) - 1))
                       : (int64_t)v;
            mon_printf(mon, " %" PRId64, sv);
            break;
        }
        case 'u': mon_printf(mon, " %" PRIu64, v); break;
        case 'o': mon_printf(mon, " %" PRIo64, v); break;
        case 'c': mon_printf(mon, "%c", isprint((int)(v & 0xFF)) ? (int)(v & 0xFF) : '.'); break;
        default:  mon_printf(mon, " %0*" PRIx64, width, v); break;
        }
    }
    mon_printf(mon, "\n");
}

static GemuMonCmd cmd_x(GemuMonitor *mon, const char *args) {
    uint64_t count = 1;
    unsigned size = 1;
    char format = 'x';
    char tok[64];
    const char *p = take_token(args, tok, sizeof(tok));
    if (p && tok[0] == '/') {
        const char *f = tok + 1;
        if (isdigit((unsigned char)*f)) count = strtoull(f, (char **)&f, 10);
        for (; *f; f++) {
            switch (*f) {
            case 'x': case 'd': case 'u': case 'o': case 'c': format = *f; break;
            case 'b': size = 1; break;
            case 'h': size = 2; break;
            case 'w': size = 4; break;
            case 'g': size = 8; break;
            default:
                mon_printf(mon, "x: invalid format '%c' (use [count][x|d|u|o|c][b|h|w|g])\n", *f);
                return GEMU_MON_NONE;
            }
        }
        p = take_token(p, tok, sizeof(tok));
    }
    uint64_t addr;
    bool ok = p && parse_hex(tok, &addr);
    /* GEMU extension: "x <addr> <count>" as an alternative to /count. */
    char cnt[64];
    if (ok && (p = take_token(p, cnt, sizeof(cnt))) != NULL)
        ok = parse_count(cnt, &count) && !*skip_ws(p);
    if (!ok) {
        mon_printf(mon, "usage: x [/fmt] <addr> [count]  (fmt: [count][x|d|u|o|c][b|h|w|g])\n");
        return GEMU_MON_NONE;
    }
    if (format == 'c') size = 1;
    if (count == 0) count = 1;
    if (count * size > 65536) count = 65536 / size;
    dump_memory(mon, addr, count, size, format);
    return GEMU_MON_NONE;
}

static GemuMonCmd cmd_peek(GemuMonitor *mon, const char *args) {
    char a[64], l[64];
    const char *p = take_token(args, a, sizeof(a));
    uint64_t addr, len = 16;
    if (!p || !parse_hex(a, &addr) ||
        ((p = take_token(p, l, sizeof(l))) && (!parse_count(l, &len) || *skip_ws(p)))) {
        mon_printf(mon, "usage: peek <addr> [len]\n");
        return GEMU_MON_NONE;
    }
    if (len == 0) len = 1;
    if (len > 65536) len = 65536;
    dump_memory(mon, addr, len, 1, 'x');
    return GEMU_MON_NONE;
}

static GemuMonCmd cmd_poke(GemuMonitor *mon, const char *args) {
    char tok[64];
    const char *p = take_token(args, tok, sizeof(tok));
    uint64_t addr;
    if (!p || !parse_hex(tok, &addr)) {
        mon_printf(mon, "usage: poke <addr> <byte>...\n");
        return GEMU_MON_NONE;
    }
    uint8_t bytes[MON_LINE_MAX];
    size_t n = 0;
    while ((p = take_token(p, tok, sizeof(tok))) != NULL) {
        uint64_t v;
        if (!parse_hex(tok, &v) || v > 0xFF) {
            mon_printf(mon, "poke: '%s' is not a hex byte\n", tok);
            return GEMU_MON_NONE;
        }
        bytes[n++] = (uint8_t)v;
    }
    if (n == 0) {
        mon_printf(mon, "usage: poke <addr> <byte>...\n");
        return GEMU_MON_NONE;
    }
    for (size_t i = 0; i < n; i++) {
        uint64_t a = (addr + i) & addr_mask(mon);
        if (!mon->mem.write(mon->mem.ud, a, bytes[i])) {
            mon_printf(mon, "poke: 0x%0*" PRIx64 " is not writable\n",
                       addr_digits(mon), a);
            return GEMU_MON_NONE;
        }
    }
    mon_printf(mon, "wrote %zu byte%s at 0x%0*" PRIx64 "\n",
               n, n == 1 ? "" : "s", addr_digits(mon), addr & addr_mask(mon));
    return GEMU_MON_NONE;
}

static GemuMonCmd cmd_memsave(GemuMonitor *mon, const char *args) {
    char a[64], s[64], path[MON_LINE_MAX];
    const char *p = take_token(args, a, sizeof(a));
    if (p) p = take_token(p, s, sizeof(s));
    uint64_t addr, size;
    if (!p || !parse_hex(a, &addr) || !parse_count(s, &size)) {
        mon_printf(mon, "usage: memsave <addr> <size> <file>\n");
        return GEMU_MON_NONE;
    }
    unquote(p, path, sizeof(path));
    if (!path[0]) {
        mon_printf(mon, "usage: memsave <addr> <size> <file>\n");
        return GEMU_MON_NONE;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        mon_printf(mon, "memsave: cannot open %s\n", path);
        return GEMU_MON_NONE;
    }
    uint64_t unreadable = 0;
    for (uint64_t i = 0; i < size; i++) {
        uint8_t b = 0;
        if (!mem_read(mon, addr + i, &b)) unreadable++;
        fputc(b, f);
    }
    fclose(f);
    mon_printf(mon, "saved %" PRIu64 " bytes from 0x%0*" PRIx64 " to %s\n",
               size, addr_digits(mon), addr & addr_mask(mon), path);
    if (unreadable)
        mon_printf(mon, "memsave: %" PRIu64 " unreadable bytes were written as 00\n",
                   unreadable);
    return GEMU_MON_NONE;
}

/* ── VNC passwords (QEMU's set_password / expire_password) ───────────────── */

static GemuMonCmd cmd_set_password(GemuMonitor *mon, const char *args) {
    char proto[16], pw[64];
    const char *p = take_token(args, proto, sizeof(proto));
    if (!p || strcasecmp(proto, "vnc") != 0 || !take_token(p, pw, sizeof(pw))) {
        mon_printf(mon, "usage: set_password vnc <password>\n");
        return GEMU_MON_NONE;
    }
    char err[160];
    if (!gemu_vnc_set_password(gemu_vnc_get(), pw, err, sizeof(err)))
        mon_printf(mon, "set_password: %s\n", err);
    else if (strlen(pw) > 8)
        mon_printf(mon, "warning: VNC passwords are limited to 8 characters; only the first 8 are used\n");
    return GEMU_MON_NONE;
}

static GemuMonCmd cmd_expire_password(GemuMonitor *mon, const char *args) {
    char proto[16], when[32];
    const char *p = take_token(args, proto, sizeof(proto));
    if (!p || strcasecmp(proto, "vnc") != 0 || !take_token(p, when, sizeof(when)) ||
        (strcasecmp(when, "now") != 0 && strcasecmp(when, "never") != 0)) {
        mon_printf(mon, "usage: expire_password vnc now|never\n");
        return GEMU_MON_NONE;
    }
    GemuVncServer *vnc = gemu_vnc_get();
    if (!vnc) mon_printf(mon, "expire_password: VNC is not active\n");
    else if (strcasecmp(when, "now") == 0) gemu_vnc_expire_password(vnc);
    return GEMU_MON_NONE;
}
