/* The core device model: see gemu/dev/device.h. */
#include "dev_priv.h"
#include "gemu/dev/registry.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gemu_machine_set_error(GemuMachine *m, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->error, sizeof m->error, fmt, ap);
    va_end(ap);
}

/* ── Machine ──────────────────────────────────────────────────────────────── */

GemuMachine *gemu_machine_new(const char *name) {
    GemuMachine *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->name = strdup(name ? name : "");
    if (!m->name) {
        free(m);
        return NULL;
    }
    m->root.machine = m;
    m->root.name = m->root.path;
    m->root.started = true;
    if (!(m->root.core = calloc(1, sizeof *m->root.core))) {
        free(m->name);
        free(m);
        return NULL;
    }
    return m;
}

void gemu_machine_free(GemuMachine *m) {
    if (!m)
        return;
    while (m->root.last_child)
        gemu_device_destroy(m->root.last_child);
    gemu_screens_free_all(m);
    gemu_lines_free_all(m);
    gemu_sched_free_all(m);
    gemu_space_free_all(m);
    gemu_state_free_items(&m->root);
    free(m->root.core);
    free(m->name);
    free(m);
}

const char *gemu_machine_name(const GemuMachine *m) { return m->name; }
GemuDevice *gemu_machine_root(GemuMachine *m) { return &m->root; }
bool gemu_machine_started(const GemuMachine *m) { return m->started; }
const char *gemu_machine_error(const GemuMachine *m) { return m->error; }

/* Run d's init and check it registered its save state (question 7: every
 * device saves).  On failure the destroy hook undoes the init. */
static bool init_device(GemuDevice *d) {
    GemuMachine *m = d->machine;
    if (d->type->init && !d->type->init(d)) {
        if (!m->error[0])
            gemu_machine_set_error(m, "%s: init failed", d->path);
        return false;
    }
    if (d->core->state_error[0])
        gemu_machine_set_error(m, "%s", d->core->state_error);
    else if (!d->type->stateless && !d->core->nitems)
        gemu_machine_set_error(m, "%s: %s registers no save state (and isn't marked stateless)",
                               d->path, d->type->name);
    else {
        d->started = true;
        return true;
    }
    if (d->type->destroy)
        d->type->destroy(d);
    return false;
}

/* Init d and then its children, in order. */
static bool start_tree(GemuDevice *d) {
    if (!d->started && !init_device(d))
        return false;
    for (GemuDevice *c = d->first_child; c; c = c->next_sibling)
        if (!start_tree(c))
            return false;
    return true;
}

bool gemu_machine_init(GemuMachine *m) {
    if (m->inited)
        return true;
    m->error[0] = 0;
    if (!start_tree(&m->root))
        return false;
    m->inited = true;
    return true;
}

bool gemu_machine_start(GemuMachine *m) {
    if (m->started)
        return true;
    if (!gemu_machine_init(m))
        return false;
    m->started = true;
    gemu_device_reset(&m->root, GEMU_RESET_POWER_ON);
    return true;
}

void gemu_machine_reset(GemuMachine *m, GemuResetKind kind) {
    gemu_device_reset(&m->root, kind);
}

/* ── Devices ──────────────────────────────────────────────────────────────── */

static bool valid_name(const char *s) {
    if (!*s || !strcmp(s, ".") || !strcmp(s, ".."))
        return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.'))
            return false;
    return true;
}

static GemuDevice *child_named(GemuDevice *parent, const char *name, size_t len) {
    for (GemuDevice *c = parent->first_child; c; c = c->next_sibling)
        if (strlen(c->name) == len && !memcmp(c->name, name, len))
            return c;
    return NULL;
}

static void unlink_device(GemuDevice *d) {
    GemuDevice *p = d->parent;
    if (d->prev_sibling) d->prev_sibling->next_sibling = d->next_sibling;
    else                 p->first_child = d->next_sibling;
    if (d->next_sibling) d->next_sibling->prev_sibling = d->prev_sibling;
    else                 p->last_child = d->prev_sibling;
    d->next_sibling = d->prev_sibling = NULL;
}

/* Everything the core holds for d: its lines, timers, executors, state
 * items and options; then d. */
static void free_device(GemuDevice *d) {
    if (d->core) {
        gemu_screens_remove_device(d);
        gemu_lines_remove_device(d);
        gemu_sched_remove_device(d);
        gemu_state_free_items(d);
        gemu_options_free(d);
        free(d->core);
    }
    free(d->priv);
    free(d->opts);
    free(d);
}

GemuDevice *gemu_device_add(GemuMachine *m, const GemuDeviceType *t,
                            const char *name, GemuDevice *parent, const char *opts) {
    m->error[0] = 0;
    if (!parent)
        parent = &m->root;
    if (!t) {
        gemu_machine_set_error(m, "%s: no device type", name ? name : "(null)");
        return NULL;
    }
    if (!name || !valid_name(name)) {
        gemu_machine_set_error(m, "\"%s\": not a valid device name", name ? name : "(null)");
        return NULL;
    }
    if (parent->machine != m) {
        gemu_machine_set_error(m, "%s: the parent belongs to another machine", name);
        return NULL;
    }
    if (child_named(parent, name, strlen(name))) {
        gemu_machine_set_error(m, "%s%s%s: already exists", parent->path,
                  parent->path[0] ? "/" : "", name);
        return NULL;
    }
    size_t plen = strlen(parent->path), nlen = strlen(name);
    if (plen + (plen ? 1 : 0) + nlen >= GEMU_DEVICE_PATH_MAX) {
        gemu_machine_set_error(m, "%s/%s: path longer than %d characters", parent->path, name,
                  GEMU_DEVICE_PATH_MAX - 1);
        return NULL;
    }

    GemuDevice *d = calloc(1, sizeof *d);
    if (!d) {
        gemu_machine_set_error(m, "%s: out of memory", name);
        return NULL;
    }
    d->type = t;
    d->machine = m;
    d->parent = parent;
    memcpy(d->path, parent->path, plen);          /* the length was checked above */
    if (plen)
        d->path[plen] = '/';
    memcpy(d->path + plen + (plen ? 1 : 0), name, nlen + 1);
    d->name = d->path + (plen ? plen + 1 : 0);
    if (t->size && !(d->priv = calloc(1, t->size))) {
        gemu_machine_set_error(m, "%s: out of memory", d->path);
        free(d);
        return NULL;
    }
    if ((opts && !(d->opts = strdup(opts))) || !(d->core = calloc(1, sizeof *d->core))) {
        gemu_machine_set_error(m, "%s: out of memory", d->path);
        free_device(d);
        return NULL;
    }
    if (!gemu_options_parse(d, opts)) {
        free_device(d);
        return NULL;
    }

    d->prev_sibling = parent->last_child;
    if (parent->last_child) parent->last_child->next_sibling = d;
    else                    parent->first_child = d;
    parent->last_child = d;

    /* Hot-plug: a device added to a running machine (under a started
     * parent) starts now.  Under a parent that isn't started yet, it waits
     * for the parent. */
    if (m->inited && parent->started) {
        if (!init_device(d)) {
            unlink_device(d);
            free_device(d);
            return NULL;
        }
        if (m->started)
            gemu_device_reset(d, GEMU_RESET_POWER_ON);
    }
    return d;
}

GemuDevice *gemu_device_add_type(GemuMachine *m, const char *type_name,
                                 const char *name, GemuDevice *parent, const char *opts) {
    const GemuDeviceType *t = gemu_device_type_find(type_name);
    if (!t) {
        m->error[0] = 0;
        gemu_machine_set_error(m, "%s: no device type \"%s\"", name ? name : "(null)", type_name);
        return NULL;
    }
    return gemu_device_add(m, t, name, parent, opts);
}

GemuDevice *gemu_device_lookup(GemuDevice *from, const char *rel) {
    GemuDevice *d = from;
    const char *s = rel;
    while (d && *s) {
        const char *end = strchr(s, '/');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        if (len == 2 && s[0] == '.' && s[1] == '.')
            d = d->parent;
        else if (len == 1 && s[0] == '.')
            ;
        else if (len == 0)
            return NULL;          /* "a//b", a leading or trailing '/' */
        else
            d = child_named(d, s, len);
        if (!end)
            break;
        s = end + 1;
        if (!*s)
            return NULL;
    }
    return d;
}

GemuDevice *gemu_device_find(GemuMachine *m, const char *path) {
    return gemu_device_lookup(&m->root, path);
}

static void reset_tree(GemuDevice *d, GemuResetKind kind) {
    if (!d->started)
        return;
    if (d->type && d->type->reset)
        d->type->reset(d, kind);
    for (GemuDevice *c = d->first_child; c; c = c->next_sibling)
        reset_tree(c, kind);
    if (d->type && d->type->reset_after_children)
        d->type->reset_after_children(d, kind);
}

void gemu_device_reset(GemuDevice *d, GemuResetKind kind) {
    if (d->machine->started)
        reset_tree(d, kind);
}

void gemu_device_destroy(GemuDevice *d) {
    if (!d || !d->parent)
        return;               /* the root goes with gemu_machine_free */
    while (d->last_child)
        gemu_device_destroy(d->last_child);
    if (d->started && d->type->destroy)
        d->type->destroy(d);
    unlink_device(d);
    free_device(d);
}

uint64_t gemu_device_clock(const GemuDevice *d) {
    for (; d; d = d->parent)
        if (d->clock_hz)
            return d->clock_hz;
    return 0;
}

void gemu_device_set_clock(GemuDevice *d, uint64_t hz) { d->clock_hz = hz; }

void gemu_device_describe(GemuDevice *d, char *buf, size_t len) {
    if (!len)
        return;
    buf[0] = 0;
    if (!d->type)
        snprintf(buf, len, "machine %s", d->machine->name);
    else if (d->type->describe)
        d->type->describe(d, buf, len);
    else
        snprintf(buf, len, "%s: %s", d->type->name, d->type->desc ? d->type->desc : "");
}

bool gemu_device_walk(GemuDevice *d, bool (*cb)(GemuDevice *d, void *ud), void *ud) {
    if (!cb(d, ud))
        return false;
    for (GemuDevice *c = d->first_child, *next; c; c = next) {
        next = c->next_sibling;
        if (!gemu_device_walk(c, cb, ud))
            return false;
    }
    return true;
}

void gemu_device_error(GemuDevice *d, const char *fmt, ...) {
    GemuMachine *m = d->machine;
    int n = snprintf(m->error, sizeof m->error, "%s: ", d->path[0] ? d->path : m->name);
    if (n < 0 || (size_t)n >= sizeof m->error)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->error + n, sizeof m->error - n, fmt, ap);
    va_end(ap);
}

/* ── Type registry ────────────────────────────────────────────────────────── */

static size_t table_len(void) {
    static size_t n = (size_t)-1;
    if (n == (size_t)-1)
        for (n = 0; gemu_device_type_table[n].name; n++)
            ;
    return n;
}

size_t gemu_device_type_count(void) { return table_len(); }

const GemuDeviceType *gemu_device_type_at(size_t i) {
    return i < table_len() ? gemu_device_type_table[i].type : NULL;
}

const GemuDeviceType *gemu_device_type_find(const char *name) {
    if (!name)
        return NULL;
    size_t lo = 0, hi = table_len();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(name, gemu_device_type_table[mid].name);
        if (!c)
            return gemu_device_type_table[mid].type;
        if (c < 0) hi = mid;
        else       lo = mid + 1;
    }
    return NULL;
}
