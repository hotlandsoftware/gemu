/* Lines: see gemu/dev/line.h. */
#include "dev_priv.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Conn {
    GemuLine    *out;
    GemuLineIn  *in;
    bool         invert;
    bool         counted;              /* what it adds to in->nasserted now */
    struct Conn *next_out, *next_in;   /* the output's and the input's lists */
} Conn;

struct GemuLine {
    GemuDevice *dev;
    char        name[32];
    bool        level;
    Conn       *conns;
    GemuLine   *next;
    unsigned    load_mark;
};

struct GemuLineIn {
    GemuDevice  *dev;
    char         name[32];
    void       (*cb)(void *ud, int idx, bool level);
    void        *ud;
    int          idx;
    GemuLineMode mode;
    bool         def, level;
    unsigned     ndrivers, nasserted;
    Conn        *conns;
    GemuLineIn  *next;
};

static bool conn_value(const Conn *c) { return c->out->level != c->invert; }

/* Bring c's share of its input's count up to date.  Kept per connection
 * so that a callback setting the same output again (nested) can't skew it. */
static void recount(Conn *c) {
    bool v = conn_value(c);
    if (v == c->counted)
        return;
    c->counted = v;
    if (v) c->in->nasserted++;
    else   c->in->nasserted--;
}

static bool combined(const GemuLineIn *in) {
    if (!in->ndrivers)
        return in->def;
    return in->mode == GEMU_LINE_WIRED_AND ? in->nasserted == in->ndrivers : in->nasserted > 0;
}

/* Recompute in's level; tell its device if it changed (once running). */
static void update(GemuLineIn *in) {
    bool v = combined(in);
    if (v == in->level)
        return;
    in->level = v;
    if (in->cb && in->dev->started && in->dev->machine->started)
        in->cb(in->ud, in->idx, v);
}

GemuLine *gemu_line_find_out(GemuDevice *d, const char *name) {
    for (GemuLine *l = d->machine->lines.outs; l; l = l->next)
        if (l->dev == d && !strcmp(l->name, name))
            return l;
    return NULL;
}

GemuLineIn *gemu_line_find_in(GemuDevice *d, const char *name) {
    for (GemuLineIn *l = d->machine->lines.ins; l; l = l->next)
        if (l->dev == d && !strcmp(l->name, name))
            return l;
    return NULL;
}

GemuLine *gemu_line_out(GemuDevice *d, const char *name) {
    if (!d || !name || !*name || strlen(name) >= sizeof ((GemuLine *)0)->name ||
        gemu_line_find_out(d, name))
        return NULL;
    GemuLine *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->dev = d;
    strcpy(l->name, name);
    l->next = d->machine->lines.outs;
    d->machine->lines.outs = l;
    return l;
}

GemuLineIn *gemu_line_in(GemuDevice *d, const char *name,
                         void (*cb)(void *ud, int idx, bool level), void *ud, int idx) {
    if (!d || !name || !*name || strlen(name) >= sizeof ((GemuLineIn *)0)->name ||
        gemu_line_find_in(d, name))
        return NULL;
    GemuLineIn *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->dev = d;
    strcpy(l->name, name);
    l->cb = cb;
    l->ud = ud;
    l->idx = idx;
    l->next = d->machine->lines.ins;
    d->machine->lines.ins = l;
    return l;
}

void gemu_line_in_set_mode(GemuLineIn *in, GemuLineMode mode) {
    in->mode = mode;
    update(in);
}

void gemu_line_in_set_default(GemuLineIn *in, bool level) {
    in->def = level;
    update(in);
}

bool gemu_line_connect_in(GemuLine *out, GemuLineIn *in, unsigned flags) {
    if (!out || !in || out->dev->machine != in->dev->machine)
        return false;
    Conn *c = calloc(1, sizeof *c);
    if (!c)
        return false;
    c->out = out;
    c->in = in;
    c->invert = flags & GEMU_LINE_INVERT;
    c->next_out = out->conns;
    out->conns = c;
    c->next_in = in->conns;
    in->conns = c;
    in->ndrivers++;
    recount(c);
    update(in);
    return true;
}

bool gemu_line_connect(GemuLine *out, GemuDevice *to, const char *in_name, unsigned flags) {
    if (!out || !to)
        return false;
    return gemu_line_connect_in(out, gemu_line_find_in(to, in_name), flags);
}

/* "dev/path:line" -> the device and the line name */
static GemuDevice *split_path(GemuMachine *m, const char *path, char *line, size_t len) {
    const char *colon = strrchr(path, ':');
    if (!colon || !colon[1] || (size_t)(colon - path) >= GEMU_DEVICE_PATH_MAX) {
        gemu_machine_set_error(m, "%s: not \"device:line\"", path);
        return NULL;
    }
    char dpath[GEMU_DEVICE_PATH_MAX];
    memcpy(dpath, path, (size_t)(colon - path));
    dpath[colon - path] = 0;
    snprintf(line, len, "%s", colon + 1);
    GemuDevice *d = gemu_device_find(m, dpath);
    if (!d)
        gemu_machine_set_error(m, "%s: no device \"%s\"", path, dpath);
    return d;
}

bool gemu_lines_connect(GemuMachine *m, const char *out_path, const char *in_path,
                        unsigned flags) {
    char oname[64], iname[64];
    GemuDevice *od = split_path(m, out_path, oname, sizeof oname);
    if (!od)
        return false;
    GemuDevice *id = split_path(m, in_path, iname, sizeof iname);
    if (!id)
        return false;
    GemuLine *out = gemu_line_find_out(od, oname);
    if (!out) {
        gemu_machine_set_error(m, "%s: no output \"%s\"", out_path, oname);
        return false;
    }
    GemuLineIn *in = gemu_line_find_in(id, iname);
    if (!in) {
        gemu_machine_set_error(m, "%s: no input \"%s\"", in_path, iname);
        return false;
    }
    return gemu_line_connect_in(out, in, flags);
}

/* Unlink c from both lists and free it; the input's counts follow. */
static void drop_conn(Conn *c, bool notify) {
    Conn **pp;
    for (pp = &c->out->conns; *pp != c; pp = &(*pp)->next_out)
        ;
    *pp = c->next_out;
    for (pp = &c->in->conns; *pp != c; pp = &(*pp)->next_in)
        ;
    *pp = c->next_in;
    GemuLineIn *in = c->in;
    in->ndrivers--;
    if (c->counted)
        in->nasserted--;
    free(c);
    if (notify)
        update(in);
}

int gemu_line_disconnect(GemuLine *out, GemuLineIn *in) {
    int n = 0;
    for (Conn *c = out->conns, *next; c; c = next) {
        next = c->next_out;
        if (c->in == in) {
            drop_conn(c, true);
            n++;
        }
    }
    return n;
}

void gemu_line_set(GemuLine *l, bool level) {
    if (l->level == level)
        return;
    l->level = level;
    for (Conn *c = l->conns; c; c = c->next_out) {
        recount(c);
        update(c->in);
    }
}

void gemu_line_pulse(GemuLine *l) {
    gemu_line_set(l, true);
    gemu_line_set(l, false);
}

bool gemu_line_get(const GemuLine *l) { return l->level; }
bool gemu_line_in_level(const GemuLineIn *in) { return in->level; }
unsigned gemu_line_in_drivers(const GemuLineIn *in) { return in->ndrivers; }
const char *gemu_line_name(const GemuLine *l) { return l->name; }
const char *gemu_line_in_name(const GemuLineIn *in) { return in->name; }

void gemu_lines_remove_device(GemuDevice *d) {
    GemuLines *ls = &d->machine->lines;
    /* its inputs first: nothing is told about them */
    for (GemuLineIn **pp = &ls->ins; *pp;) {
        GemuLineIn *in = *pp;
        if (in->dev != d) {
            pp = &in->next;
            continue;
        }
        while (in->conns)
            drop_conn(in->conns, false);
        *pp = in->next;
        free(in);
    }
    /* then its outputs: the inputs they drove see them go */
    for (GemuLine **pp = &ls->outs; *pp;) {
        GemuLine *l = *pp;
        if (l->dev != d) {
            pp = &l->next;
            continue;
        }
        while (l->conns)
            drop_conn(l->conns, true);
        *pp = l->next;
        free(l);
    }
}

void gemu_lines_free_all(GemuMachine *m) {
    GemuLines *ls = &m->lines;
    while (ls->ins) {
        GemuLineIn *in = ls->ins;
        while (in->conns)
            drop_conn(in->conns, false);
        ls->ins = in->next;
        free(in);
    }
    while (ls->outs) {
        GemuLine *l = ls->outs;
        ls->outs = l->next;
        free(l);
    }
}

/* ── Save state: the outputs' levels; inputs follow from them ─────────────── */

void gemu_lines_state_save(GemuMachine *m, GemuStateWriter *w) {
    uint64_t n = 0;
    for (GemuLine *l = m->lines.outs; l; l = l->next)
        n++;
    gemu_sw_u64(w, n);
    for (GemuLine *l = m->lines.outs; l; l = l->next) {
        gemu_sw_str(w, l->dev->path);
        gemu_sw_str(w, l->name);
        gemu_sw_u8(w, l->level);
    }
}

bool gemu_lines_state_load(GemuMachine *m, GemuStateReader *r, bool apply, unsigned epoch) {
    uint64_t n, live = 0;
    for (GemuLine *l = m->lines.outs; l; l = l->next)
        live++;
    if (!gemu_sr_u64(r, &n))
        goto bad;
    if (n != live) {
        gemu_machine_set_error(m, "state: %" PRIu64 " line outputs saved, %" PRIu64 " in this machine",
                               n, live);
        return false;
    }
    for (uint64_t i = 0; i < n; i++) {
        char path[GEMU_DEVICE_PATH_MAX], name[32];
        uint8_t level;
        if (!gemu_sr_str(r, path, sizeof path) || !gemu_sr_str(r, name, sizeof name) ||
            !gemu_sr_u8(r, &level))
            goto bad;
        GemuLine *l = NULL;
        for (GemuLine *x = m->lines.outs; x; x = x->next)
            if (!strcmp(x->name, name) && !strcmp(x->dev->path, path)) {
                l = x;
                break;
            }
        if (!l) {
            gemu_machine_set_error(m, "state: %s has no line output %s here",
                                   path[0] ? path : "(machine)", name);
            return false;
        }
        if (!apply) {
            if (l->load_mark == epoch)
                goto bad;
            l->load_mark = epoch;
        } else {
            l->level = level;
        }
    }
    if (r->left)
        goto bad;
    if (apply) {
        /* the inputs, from the outputs: quietly (the devices' own state
         * already holds what they had latched) */
        for (GemuLineIn *in = m->lines.ins; in; in = in->next) {
            in->nasserted = 0;
            for (Conn *c = in->conns; c; c = c->next_in) {
                c->counted = conn_value(c);
                in->nasserted += c->counted;
            }
            in->level = combined(in);
        }
    }
    return true;
bad:
    gemu_machine_set_error(m, "state: the lines section is damaged");
    return false;
}
