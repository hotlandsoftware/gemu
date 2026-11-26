/* The monitor and core machines (the gemu/dev/ headers): info qtree / mtree / <path>,
 * "space:addr" for the memory commands, device types' own commands.
 * See gemu_monitor_attach_machine() in gemu/monitor.h. */
#include "monitor_priv.h"
#include "gemu/dev/device.h"
#include "gemu/dev/option.h"
#include "gemu/dev/space.h"
#include "gemu/dev/state.h"
#include "gemu/dev/stream.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* ── Attaching ────────────────────────────────────────────────────────────── */

static bool warn_shadowed(GemuDevice *d, void *ud) {
    (void)ud;
    if (!d->type || !d->type->commands)
        return true;
    for (const GemuDeviceCmd *c = d->type->commands; c->name; c++)
        if (mon_is_table_command(c->name))
            fprintf(stderr, "monitor: %s's command '%s' is a monitor command already; "
                            "it is ignored (rename it)\n", d->type->name, c->name);
    return true;
}

void gemu_monitor_attach_machine(GemuMonitor *mon, GemuMachine *m, const char *default_space) {
    if (!mon)
        return;
    mon->machine = m;
    snprintf(mon->default_space, sizeof mon->default_space, "%s",
             m && default_space ? default_space : "");
    if (m)
        gemu_device_walk(gemu_machine_root(m), warn_shadowed, NULL);
}

/* ── info qtree / mtree / <path> ──────────────────────────────────────────── */

static void print_clock(GemuMonitor *mon, uint64_t hz) {
    if (hz >= 1000000)
        mon_printf(mon, "  %" PRIu64 ".%06" PRIu64 " MHz", hz / 1000000, hz % 1000000);
    else if (hz)
        mon_printf(mon, "  %" PRIu64 " Hz", hz);
}

static void qtree(GemuMonitor *mon, GemuDevice *d, int depth) {
    if (depth == 0) {
        mon_printf(mon, "machine %s\n", gemu_machine_name(d->machine));
    } else {
        mon_printf(mon, "%*s%s  (%s)", depth * 2, "", d->name, d->type->name);
        if (d->clock_hz)
            print_clock(mon, d->clock_hz);
        if (d->opts && d->opts[0])
            mon_printf(mon, "  %s", d->opts);
        if (!d->started)
            mon_printf(mon, "  [not started]");
        mon_printf(mon, "\n");
    }
    for (GemuDevice *c = d->first_child; c; c = c->next_sibling)
        qtree(mon, c, depth + 1);
}

static int digits_for(unsigned bits) { return (int)((bits + 3) / 4); }

static int cmp_map(const void *a, const void *b) {
    const GemuMapping *x = *(GemuMapping *const *)a, *y = *(GemuMapping *const *)b;
    if (x->base != y->base) return x->base < y->base ? -1 : 1;
    if (x->priority != y->priority) return x->priority > y->priority ? -1 : 1;
    return x->seq < y->seq ? -1 : 1;
}

static void mtree(GemuMonitor *mon) {
    GemuSpace *s = gemu_space_first(mon->machine);
    if (!s) {
        mon_printf(mon, "(no address spaces)\n");
        return;
    }
    for (; s; s = gemu_space_next(s)) {
        unsigned bits = gemu_space_addr_bits(s);
        int w = digits_for(bits);
        mon_printf(mon, "%s: %u-bit addresses, %u-bit data, %s-endian\n", gemu_space_name(s),
                   bits, gemu_space_data_bits(s),
                   gemu_space_endian(s) == GEMU_BIG_ENDIAN ? "big" : "little");
        size_t n = 0;
        for (GemuMapping *m = gemu_space_mappings(s); m; m = m->next)
            n++;
        GemuMapping **v = n ? malloc(n * sizeof *v) : NULL;
        if (n && !v)
            return;
        n = 0;
        for (GemuMapping *m = gemu_space_mappings(s); m; m = m->next)
            v[n++] = m;
        qsort(v, n, sizeof *v, cmp_map);
        if (!n)
            mon_printf(mon, "  (nothing mapped)\n");
        for (size_t i = 0; i < n; i++) {
            GemuMapping *m = v[i];
            const char *kind;
            if (m->ram)
                kind = (m->flags & GEMU_MAP_READONLY) ? "rom" :
                       (m->flags & GEMU_MAP_WRITEONLY) ? "ram (write-only)" : "ram";
            else {
                bool r = m->h.r8 || m->h.r16 || m->h.r32 || m->h.r64;
                bool wr = m->h.w8 || m->h.w16 || m->h.w32 || m->h.w64;
                kind = r && wr ? "i/o" : r ? "i/o (read-only)" : "i/o (write-only)";
            }
            mon_printf(mon, "  %0*" PRIx64 "-%0*" PRIx64 "  %-16s prio %d%s%s%s%s\n",
                       w, m->base, w, m->base + m->size - 1, kind, m->priority,
                       (m->flags & GEMU_MAP_SHARED) ? " shared" : "",
                       m->enabled ? "" : " disabled",
                       m->name[0] ? "  " : "", m->name);
        }
        free(v);
        uint64_t base, size, mask;
        for (unsigned i = 0; gemu_space_mirror_get(s, i, &base, &size, &mask); i++)
            mon_printf(mon, "  %0*" PRIx64 "-%0*" PRIx64 "  mirror (address & %0*" PRIx64 ")\n",
                       w, base, w, base + size - 1, w, mask);
    }
}

static void describe(GemuMonitor *mon, GemuDevice *d) {
    char *buf = malloc(8192);
    if (!buf)
        return;
    gemu_device_describe(d, buf, 8192);
    if (!d->type) {
        mon_printf(mon, "%s\n", buf);
        free(buf);
        return;
    }
    mon_printf(mon, "%s: %s%s%s\n", d->path, d->type->name, d->type->desc ? " - " : "",
               d->type->desc ? d->type->desc : "");
    uint64_t hz = gemu_device_clock(d);
    if (hz) {
        mon_printf(mon, "  clock");
        print_clock(mon, hz);
        mon_printf(mon, "%s\n", d->clock_hz ? "" : " (inherited)");
    }
    for (const GemuOptionDef *o = d->type->options; o && o->name; o++) {
        mon_printf(mon, "  %s=", o->name);
        switch (o->kind) {
        case GEMU_OPT_INT:  mon_printf(mon, "%" PRId64, gemu_opt_int(d, o->name)); break;
        case GEMU_OPT_BOOL: mon_printf(mon, "%s", gemu_opt_bool(d, o->name) ? "on" : "off"); break;
        case GEMU_OPT_SIZE: mon_printf(mon, "%" PRIu64, gemu_opt_size(d, o->name)); break;
        default:            mon_printf(mon, "%s", gemu_opt_str(d, o->name)); break;
        }
        mon_printf(mon, "%s\n", gemu_opt_given(d, o->name) ? "" : " (default)");
    }
    /* the hook's text, indented (it may span lines) */
    if (d->type->describe && buf[0]) {
        for (char *line = buf, *nl; line && *line; line = nl) {
            nl = strchr(line, '\n');
            if (nl)
                *nl++ = 0;
            mon_printf(mon, "  %s\n", line);
        }
    }
    free(buf);
}

bool mon_dev_info(GemuMonitor *mon, const char *sub, const char *rest) {
    if (!mon->machine)
        return false;
    if (!strcmp(sub, "qtree")) {
        qtree(mon, gemu_machine_root(mon->machine), 0);
        return true;
    }
    if (!strcmp(sub, "mtree")) {
        mtree(mon);
        return true;
    }
    (void)rest;
    GemuDevice *d = gemu_device_find(mon->machine, sub);
    if (!d || !d->type)
        return false;
    describe(mon, d);
    return true;
}

void mon_dev_info_help(GemuMonitor *mon, void (*row)(GemuMonitor *, const char *, const char *)) {
    if (!mon->machine)
        return;
    row(mon, "info qtree", "show the device tree");
    row(mon, "info mtree", "show each address space's mappings");
    row(mon, "info <device path>", "describe one device (paths from 'info qtree')");
}

/* ── Memory views on spaces ───────────────────────────────────────────────── */

static bool space_read(void *ud, uint64_t addr, uint8_t *v) {
    return gemu_space_peek8(ud, addr, v);
}
static bool space_write(void *ud, uint64_t addr, uint8_t v) {
    gemu_space_write8(ud, addr, v);
    return true;
}

static void view_of(GemuSpace *s, GemuMonMemory *v) {
    v->read = space_read;
    v->write = space_write;
    v->ud = s;
    v->addr_bits = gemu_space_addr_bits(s);
    v->big_endian = gemu_space_endian(s) == GEMU_BIG_ENDIAN;
}

bool mon_dev_has_spaces(const GemuMonitor *mon) {
    return mon->machine && gemu_space_first(mon->machine);
}

static void list_spaces(GemuMonitor *mon, char *buf, size_t len) {
    buf[0] = 0;
    for (GemuSpace *s = mon->machine ? gemu_space_first(mon->machine) : NULL; s;
         s = gemu_space_next(s)) {
        size_t n = strlen(buf);
        snprintf(buf + n, len - n, "%s%s", n ? ", " : "", gemu_space_name(s));
    }
}

bool mon_dev_view(GemuMonitor *mon, const char *cmd, const char *tok, GemuMonMemory *view,
                  const char **addr_text) {
    *addr_text = tok;
    const char *colon = strchr(tok, ':');
    if (colon && mon->machine) {
        char name[32];
        size_t n = (size_t)(colon - tok);
        if (n && n < sizeof name) {
            memcpy(name, tok, n);
            name[n] = 0;
            GemuSpace *s = gemu_space_find(mon->machine, name);
            if (s) {
                view_of(s, view);
                *addr_text = colon + 1;
                return true;
            }
            char list[256];
            list_spaces(mon, list, sizeof list);
            mon_printf(mon, "%s: no address space '%s' (spaces: %s)\n", cmd, name,
                       list[0] ? list : "none");
            return false;
        }
    }
    if (mon->has_mem) {
        *view = mon->mem;
        return true;
    }
    if (mon->machine && mon->default_space[0]) {
        GemuSpace *s = gemu_space_find(mon->machine, mon->default_space);
        if (s) {
            view_of(s, view);
            return true;
        }
    }
    char list[256];
    list_spaces(mon, list, sizeof list);
    mon_printf(mon, "%s: no default memory; give a space, e.g. %s:%s (spaces: %s)\n", cmd,
               mon->machine && gemu_space_first(mon->machine)
                   ? gemu_space_name(gemu_space_first(mon->machine)) : "space",
               tok, list[0] ? list : "none");
    return false;
}

/* ── Device commands ──────────────────────────────────────────────────────── */

typedef struct {
    const char *verb;
    GemuDevice *found[8];
    const GemuDeviceCmd *cmd[8];
    unsigned n;
} CmdSearch;

static const GemuDeviceCmd *type_cmd(const GemuDevice *d, const char *verb) {
    if (!d->type || !d->type->commands || mon_is_table_command(verb))
        return NULL;
    for (const GemuDeviceCmd *c = d->type->commands; c->name; c++)
        if (!strcmp(c->name, verb))
            return c;
    return NULL;
}

static bool search_cb(GemuDevice *d, void *ud) {
    CmdSearch *cs = ud;
    const GemuDeviceCmd *c = type_cmd(d, cs->verb);
    if (c) {
        if (cs->n < 8) {
            cs->found[cs->n] = d;
            cs->cmd[cs->n] = c;
        }
        cs->n++;
    }
    return true;
}

bool mon_dev_dispatch(GemuMonitor *mon, const char *verb, const char *args) {
    if (!mon->machine)
        return false;
    CmdSearch cs = { verb, { 0 }, { 0 }, 0 };
    gemu_device_walk(gemu_machine_root(mon->machine), search_cb, &cs);
    if (!cs.n)
        return false;
    GemuDevice *d = NULL;
    const GemuDeviceCmd *c = NULL;
    if (args[0] == '@') {
        char path[GEMU_DEVICE_PATH_MAX];
        const char *p = args + 1;
        size_t n = 0;
        while (p[n] && !isspace((unsigned char)p[n]))
            n++;
        if (n >= sizeof path) n = sizeof path - 1;
        memcpy(path, p, n);
        path[n] = 0;
        args = p + n;
        while (isspace((unsigned char)*args))
            args++;
        d = gemu_device_find(mon->machine, path);
        if (!d || !(c = type_cmd(d, verb))) {
            mon_printf(mon, "%s: %s %s\n", verb, path,
                       d ? "has no such command" : "is not a device here");
            return true;
        }
    } else if (cs.n == 1) {
        d = cs.found[0];
        c = cs.cmd[0];
    } else {
        mon_printf(mon, "%s: %u devices have it; pick one with @path:", verb, cs.n);
        for (unsigned i = 0; i < cs.n && i < 8; i++)
            mon_printf(mon, " %s", cs.found[i]->path);
        mon_printf(mon, "%s\n", cs.n > 8 ? " ..." : "");
        return true;
    }
    c->fn(d, mon, args);
    return true;
}

typedef struct {
    GemuMonitor *mon;
    void (*row)(GemuMonitor *, const char *, const char *);
    const GemuDeviceType *seen[64];
    unsigned nseen;
    bool header;
} HelpWalk;

static bool help_cb(GemuDevice *d, void *ud) {
    HelpWalk *hw = ud;
    if (!d->type || !d->type->commands)
        return true;
    for (unsigned i = 0; i < hw->nseen; i++)
        if (hw->seen[i] == d->type)
            return true;
    if (hw->nseen < 64)
        hw->seen[hw->nseen++] = d->type;
    for (const GemuDeviceCmd *c = d->type->commands; c->name; c++) {
        if (mon_is_table_command(c->name))
            continue;
        if (!hw->header) {
            mon_printf(hw->mon, "\nDevice commands (\"@path\" first to pick a device):\n");
            hw->header = true;
        }
        char syn[160], help[256];
        snprintf(syn, sizeof syn, "%s%s%s", c->name, c->params && c->params[0] ? " " : "",
                 c->params ? c->params : "");
        snprintf(help, sizeof help, "%s (%s)", c->help ? c->help : "", d->type->name);
        hw->row(hw->mon, syn, help);
    }
    return true;
}

void mon_dev_help(GemuMonitor *mon, void (*row)(GemuMonitor *, const char *, const char *)) {
    if (!mon->machine)
        return;
    HelpWalk hw = { mon, row, { 0 }, 0, false };
    gemu_device_walk(gemu_machine_root(mon->machine), help_cb, &hw);
}

bool mon_dev_help_command(GemuMonitor *mon, const char *word) {
    if (!mon->machine)
        return false;
    CmdSearch cs = { word, { 0 }, { 0 }, 0 };
    gemu_device_walk(gemu_machine_root(mon->machine), search_cb, &cs);
    if (!cs.n)
        return false;
    const GemuDeviceCmd *c = cs.cmd[0];
    mon_printf(mon, "%s%s%s\n    %s\n", c->name, c->params && c->params[0] ? " " : "",
               c->params ? c->params : "", c->help ? c->help : "");
    mon_printf(mon, "    devices:");
    for (unsigned i = 0; i < cs.n && i < 8; i++)
        mon_printf(mon, " %s (%s)", cs.found[i]->path, cs.found[i]->type->name);
    mon_printf(mon, "%s\n", cs.n > 8 ? " ..." : "");
    return true;
}

/* ── Sound capture ────────────────────────────────────────────────────────── */

/* A command's file argument: "a file name" may be quoted. */
static void file_arg(const char *args, char path[MON_LINE_MAX]) {
    snprintf(path, MON_LINE_MAX, "%s", args);
    size_t n = strlen(path);
    while (n && isspace((unsigned char)path[n - 1]))
        path[--n] = 0;
    if (n >= 2 && path[0] == '"' && path[n - 1] == '"') {
        memmove(path, path + 1, n - 2);
        path[n - 2] = 0;
    }
}

GemuMonCmd mon_dev_wavcapture(GemuMonitor *mon, const char *args) {
    char path[MON_LINE_MAX];
    file_arg(args, path);
    if (!path[0]) {
        mon_printf(mon, "usage: wavcapture <file>\n");
    } else if (gemu_mixer_wav_running(mon->machine)) {
        mon_printf(mon, "wavcapture: already recording (stopcapture first)\n");
    } else if (!gemu_mixer_wav_start(mon->machine, path)) {
        mon_printf(mon, "wavcapture: can't write %s (or the machine has no sound)\n", path);
    } else {
        mon_printf(mon, "recording the machine's sound to %s\n", path);
    }
    return GEMU_MON_NONE;
}

GemuMonCmd mon_dev_stopcapture(GemuMonitor *mon, const char *args) {
    (void)args;                         /* QEMU's capture index: there is only one */
    if (!gemu_mixer_wav_stop(mon->machine))
        mon_printf(mon, "stopcapture: not recording\n");
    return GEMU_MON_NONE;
}

/* ── Save state ───────────────────────────────────────────────────────────── *
 *
 * The whole machine (gemu/dev/state.h) to a file and back.  A load checks
 * everything first and changes nothing when the file doesn't fit this
 * machine - another configuration, another disk - saying why. */

GemuMonCmd mon_dev_savevm(GemuMonitor *mon, const char *args) {
    char path[MON_LINE_MAX];
    file_arg(args, path);
    if (!path[0])
        mon_printf(mon, "usage: savevm <file>\n");
    else if (!gemu_machine_save_file(mon->machine, path))
        mon_printf(mon, "savevm: %s\n", gemu_machine_error(mon->machine));
    else
        mon_printf(mon, "saved the machine to %s\n", path);
    return GEMU_MON_NONE;
}

GemuMonCmd mon_dev_loadvm(GemuMonitor *mon, const char *args) {
    char path[MON_LINE_MAX];
    file_arg(args, path);
    if (!path[0])
        mon_printf(mon, "usage: loadvm <file>\n");
    else if (!gemu_machine_load_file(mon->machine, path))
        mon_printf(mon, "loadvm: %s\n", gemu_machine_error(mon->machine));
    else
        mon_printf(mon, "loaded the machine from %s\n", path);
    return GEMU_MON_NONE;
}
