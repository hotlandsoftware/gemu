/* Save state: see gemu/dev/state.h.
 *
 * Format (little-endian):
 *   "GEMUSTAT" u64 version  str machine-name
 *   sections: u8 tag, u64 length, payload
 *     'D' a device: str path, str type ("" for the root), u64 items, then
 *         per item: str name, u8 kind (0 memory, 1 custom), u64 elem,
 *         u64 count, u64 bytes, the bytes
 *     'S' the scheduler (scheduler.c)    'L' the line outputs (line.c)
 *   u8 'E'
 * Strings: u64 length, the bytes. */
#include "dev_priv.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_MAGIC   "GEMUSTAT"
#define STATE_VERSION 1u

/* ── Streams ──────────────────────────────────────────────────────────────── */

void gemu_state_write(GemuStateWriter *w, const void *p, size_t n) {
    if (w->failed || !n)
        return;
    if (w->len + n > w->cap) {
        size_t cap = w->cap ? w->cap : 4096;
        while (cap < w->len + n)
            cap *= 2;
        uint8_t *b = realloc(w->buf, cap);
        if (!b) {
            w->failed = true;
            return;
        }
        w->buf = b;
        w->cap = cap;
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

void gemu_sw_u8(GemuStateWriter *w, uint8_t v) { gemu_state_write(w, &v, 1); }
void gemu_sw_u64(GemuStateWriter *w, uint64_t v) { gemu_state_write(w, &v, 8); }
void gemu_state_write_u32(GemuStateWriter *w, uint32_t v) { gemu_state_write(w, &v, 4); }
void gemu_sw_str(GemuStateWriter *w, const char *s) {
    size_t n = strlen(s);
    gemu_sw_u64(w, n);
    gemu_state_write(w, s, n);
}

bool gemu_state_read(GemuStateReader *r, void *p, size_t n) {
    if (n > r->left)
        return false;
    memcpy(p, r->p, n);
    r->p += n;
    r->left -= n;
    return true;
}

bool gemu_sr_u8(GemuStateReader *r, uint8_t *v) { return gemu_state_read(r, v, 1); }
bool gemu_sr_u64(GemuStateReader *r, uint64_t *v) { return gemu_state_read(r, v, 8); }
bool gemu_state_read_u32(GemuStateReader *r, uint32_t *v) { return gemu_state_read(r, v, 4); }
size_t gemu_state_left(const GemuStateReader *r) { return r->left; }

void gemu_state_refuse(GemuStateReader *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->why, sizeof r->why, fmt, ap);
    va_end(ap);
}

bool gemu_sr_str(GemuStateReader *r, char *buf, size_t cap) {
    uint64_t n;
    if (!gemu_sr_u64(r, &n) || n >= cap || n > r->left)
        return false;
    gemu_state_read(r, buf, (size_t)n);
    buf[n] = 0;
    return true;
}

/* ── Registering ──────────────────────────────────────────────────────────── */

static GemuStateItem *new_item(GemuDevice *d, const char *name) {
    GemuDeviceCore *c = d->core;
    if (!name || !*name || strlen(name) >= sizeof ((GemuStateItem *)0)->name) {
        if (!c->state_error[0])
            snprintf(c->state_error, sizeof c->state_error, "%s: bad state item name \"%s\"",
                     d->path, name ? name : "(null)");
        return NULL;
    }
    for (GemuStateItem *i = c->items; i; i = i->next)
        if (!strcmp(i->name, name)) {
            if (!c->state_error[0])
                snprintf(c->state_error, sizeof c->state_error,
                         "%s: state item \"%s\" registered twice", d->path, name);
            return NULL;
        }
    GemuStateItem *it = calloc(1, sizeof *it);
    if (!it) {
        snprintf(c->state_error, sizeof c->state_error, "%s: out of memory", d->path);
        return NULL;
    }
    strcpy(it->name, name);
    if (c->items_tail) c->items_tail->next = it;
    else               c->items = it;
    c->items_tail = it;
    c->nitems++;
    return it;
}

bool gemu_state_add(GemuDevice *d, const char *name, void *ptr, size_t elem, size_t count) {
    if (!d || !d->core)
        return false;
    if (!ptr || !elem || !count || count > SIZE_MAX / elem) {
        if (!d->core->state_error[0])
            snprintf(d->core->state_error, sizeof d->core->state_error,
                     "%s: state item \"%s\": no memory or no size", d->path, name ? name : "(null)");
        return false;
    }
    GemuStateItem *it = new_item(d, name);
    if (!it)
        return false;
    it->ptr = ptr;
    it->elem = elem;
    it->count = count;
    return true;
}

bool gemu_state_add_custom(GemuDevice *d, const char *name,
                           void (*save)(void *ud, GemuStateWriter *w),
                           bool (*check)(void *ud, GemuStateReader *r),
                           bool (*load)(void *ud, GemuStateReader *r), void *ud) {
    if (!d || !d->core)
        return false;
    if (!save || !load) {
        if (!d->core->state_error[0])
            snprintf(d->core->state_error, sizeof d->core->state_error,
                     "%s: custom state item \"%s\" needs save and load", d->path,
                     name ? name : "(null)");
        return false;
    }
    GemuStateItem *it = new_item(d, name);
    if (!it)
        return false;
    it->save = save;
    it->check = check;
    it->load = load;
    it->ud = ud;
    return true;
}

void gemu_state_free_items(GemuDevice *d) {
    GemuDeviceCore *c = d->core;
    if (!c)
        return;
    for (GemuStateItem *i = c->items, *n; i; i = n) {
        n = i->next;
        free(i);
    }
    c->items = c->items_tail = NULL;
    c->nitems = 0;
}

/* ── Saving ───────────────────────────────────────────────────────────────── */

/* A section: tag, length (patched), payload. */
static size_t begin_section(GemuStateWriter *w, uint8_t tag) {
    gemu_sw_u8(w, tag);
    size_t at = w->len;
    gemu_sw_u64(w, 0);
    return at;
}
static void end_section(GemuStateWriter *w, size_t at) {
    if (w->failed)
        return;
    uint64_t n = w->len - at - 8;
    memcpy(w->buf + at, &n, 8);
}

static bool pre_save_cb(GemuDevice *d, void *ud) {
    (void)ud;
    if (d->type && d->type->pre_save)
        d->type->pre_save(d);
    return true;
}

static bool save_device(GemuDevice *d, void *ud) {
    GemuStateWriter *w = ud;
    size_t at = begin_section(w, 'D');
    gemu_sw_str(w, d->path);
    gemu_sw_str(w, d->type ? d->type->name : "");
    gemu_sw_u64(w, d->core->nitems);
    for (GemuStateItem *i = d->core->items; i; i = i->next) {
        gemu_sw_str(w, i->name);
        gemu_sw_u8(w, i->save ? 1 : 0);
        gemu_sw_u64(w, i->elem);
        gemu_sw_u64(w, i->count);
        size_t len_at = w->len;
        gemu_sw_u64(w, 0);
        if (i->save)
            i->save(i->ud, w);
        else
            gemu_state_write(w, i->ptr, i->elem * i->count);
        if (!w->failed) {
            uint64_t n = w->len - len_at - 8;
            memcpy(w->buf + len_at, &n, 8);
        }
    }
    end_section(w, at);
    return true;
}

static bool can_snapshot(GemuMachine *m, const char *what) {
    if (!m->started) {
        gemu_machine_set_error(m, "%s: the machine isn't started", what);
        return false;
    }
    if (m->sched.in_run) {
        gemu_machine_set_error(m, "%s: not while the machine is running", what);
        return false;
    }
    return true;
}

bool gemu_machine_save_state(GemuMachine *m, uint8_t **buf, size_t *len) {
    m->error[0] = 0;
    if (!can_snapshot(m, "save"))
        return false;
    gemu_device_walk(&m->root, pre_save_cb, NULL);
    GemuStateWriter w = { 0 };
    gemu_state_write(&w, STATE_MAGIC, 8);
    gemu_sw_u64(&w, STATE_VERSION);
    gemu_sw_str(&w, m->name);
    gemu_device_walk(&m->root, save_device, &w);
    size_t at = begin_section(&w, 'S');
    gemu_sched_state_save(m, &w);
    end_section(&w, at);
    at = begin_section(&w, 'L');
    gemu_lines_state_save(m, &w);
    end_section(&w, at);
    gemu_sw_u8(&w, 'E');
    if (w.failed) {
        free(w.buf);
        gemu_machine_set_error(m, "save: out of memory");
        return false;
    }
    *buf = w.buf;
    *len = w.len;
    return true;
}

/* ── Loading ──────────────────────────────────────────────────────────────── */

static bool load_device(GemuMachine *m, GemuStateReader *r, bool apply, unsigned epoch) {
    char path[GEMU_DEVICE_PATH_MAX], type[64];
    uint64_t nitems;
    if (!gemu_sr_str(r, path, sizeof path) || !gemu_sr_str(r, type, sizeof type) ||
        !gemu_sr_u64(r, &nitems))
        goto bad;
    GemuDevice *d = gemu_device_find(m, path);
    const char *where = path[0] ? path : "(machine)";
    if (!d) {
        gemu_machine_set_error(m, "state: %s (%s) isn't in this machine", where, type);
        return false;
    }
    const char *have = d->type ? d->type->name : "";
    if (strcmp(have, type)) {
        gemu_machine_set_error(m, "state: %s is a %s here, a %s in the state", where, have, type);
        return false;
    }
    if (!apply) {
        if (d->core->load_mark == epoch)
            goto bad;
        d->core->load_mark = epoch;
        if (nitems != d->core->nitems) {
            gemu_machine_set_error(m, "state: %s has %u state items here, %" PRIu64 " saved",
                                   where, d->core->nitems, nitems);
            return false;
        }
    }
    for (uint64_t k = 0; k < nitems; k++) {
        char name[32];
        uint8_t kind;
        uint64_t elem, count, bytes;
        if (!gemu_sr_str(r, name, sizeof name) || !gemu_sr_u8(r, &kind) ||
            !gemu_sr_u64(r, &elem) || !gemu_sr_u64(r, &count) || !gemu_sr_u64(r, &bytes) ||
            bytes > r->left)
            goto bad;
        GemuStateItem *it = NULL;
        for (GemuStateItem *i = d->core->items; i; i = i->next)
            if (!strcmp(i->name, name)) {
                it = i;
                break;
            }
        if (!it) {
            gemu_machine_set_error(m, "state: %s has no state item %s here", where, name);
            return false;
        }
        GemuStateReader sub = { r->p, (size_t)bytes, "" };
        r->p += bytes;
        r->left -= bytes;
        if (!apply) {
            if (it->load_mark == epoch)
                goto bad;
            it->load_mark = epoch;
            if (kind != (it->save ? 1 : 0)) {
                gemu_machine_set_error(m, "state: %s item %s: custom on one side only", where, name);
                return false;
            }
            if (!it->save && (elem != it->elem || count != it->count || bytes != elem * count)) {
                gemu_machine_set_error(m, "state: %s item %s: %" PRIu64 " x %" PRIu64
                                       " bytes saved, %" PRIu64 " x %" PRIu64 " here",
                                       where, name, count, elem, (uint64_t)it->count,
                                       (uint64_t)it->elem);
                return false;
            }
            if (it->save && it->check && !it->check(it->ud, &sub)) {
                if (sub.why[0])
                    gemu_machine_set_error(m, "state: %s: %s", where, sub.why);
                else
                    gemu_machine_set_error(m, "state: %s item %s: refused by the device",
                                           where, name);
                return false;
            }
            continue;
        }
        if (it->save) {
            if (!it->load(it->ud, &sub)) {
                gemu_machine_set_error(m, "state: %s item %s failed to load (the machine "
                                       "may be inconsistent: reset it)", where, name);
                return false;
            }
        } else {
            memcpy(it->ptr, sub.p, (size_t)bytes);
        }
    }
    if (r->left)
        goto bad;
    return true;
bad:
    gemu_machine_set_error(m, "state: a device section is damaged");
    return false;
}

static bool post_load_cb(GemuDevice *d, void *ud) {
    (void)ud;
    if (d->type && d->type->post_load)
        d->type->post_load(d);
    return true;
}

static bool count_cb(GemuDevice *d, void *ud) {
    (void)d;
    ++*(unsigned *)ud;
    return true;
}

/* One pass over the state: check (apply false) or apply. */
static bool load_pass(GemuMachine *m, const uint8_t *buf, size_t len, bool apply, unsigned epoch) {
    GemuStateReader r = { buf, len, "" };
    char magic[8], name[256];
    uint64_t version;
    if (!gemu_state_read(&r, magic, 8) || memcmp(magic, STATE_MAGIC, 8)) {
        gemu_machine_set_error(m, "state: not a GEMU machine state");
        return false;
    }
    if (!gemu_sr_u64(&r, &version) || version != STATE_VERSION) {
        gemu_machine_set_error(m, "state: format %" PRIu64 ", this GEMU reads %u", version,
                               STATE_VERSION);
        return false;
    }
    if (!gemu_sr_str(&r, name, sizeof name))
        goto bad;
    if (strcmp(name, m->name)) {
        gemu_machine_set_error(m, "state: saved from %s, this is %s", name, m->name);
        return false;
    }
    unsigned devices = 0;
    bool sched = false, lines = false;
    for (;;) {
        uint8_t tag;
        if (!gemu_sr_u8(&r, &tag))
            goto bad;
        if (tag == 'E')
            break;
        uint64_t n;
        if (!gemu_sr_u64(&r, &n) || n > r.left)
            goto bad;
        GemuStateReader sec = { r.p, (size_t)n, "" };
        r.p += n;
        r.left -= n;
        bool ok;
        switch (tag) {
        case 'D': ok = load_device(m, &sec, apply, epoch); devices++; break;
        case 'S': ok = !sched && gemu_sched_state_load(m, &sec, apply, epoch); sched = true; break;
        case 'L': ok = !lines && gemu_lines_state_load(m, &sec, apply, epoch); lines = true; break;
        default: goto bad;
        }
        if (!ok) {
            if (!m->error[0])
                goto bad;
            return false;
        }
    }
    if (r.left || !sched || !lines)
        goto bad;
    if (!apply) {
        unsigned live = 0;
        gemu_device_walk(&m->root, count_cb, &live);
        if (devices != live) {
            gemu_machine_set_error(m, "state: %u devices saved, %u in this machine", devices, live);
            return false;
        }
    }
    return true;
bad:
    gemu_machine_set_error(m, "state: damaged or cut short");
    return false;
}

bool gemu_machine_load_state(GemuMachine *m, const uint8_t *buf, size_t len) {
    m->error[0] = 0;
    if (!can_snapshot(m, "load"))
        return false;
    unsigned epoch = ++m->load_epoch;
    if (!load_pass(m, buf, len, false, epoch))
        return false;                      /* nothing changed */
    if (!load_pass(m, buf, len, true, epoch))
        return false;
    gemu_streams_after_load(m);            /* the sound so far was another timeline's */
    gemu_device_walk(&m->root, post_load_cb, NULL);
    return true;
}

bool gemu_machine_save_file(GemuMachine *m, const char *path) {
    uint8_t *buf;
    size_t len;
    if (!gemu_machine_save_state(m, &buf, &len))
        return false;
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(buf, 1, len, f) == len;
    if (f && fclose(f))
        ok = false;
    free(buf);
    if (!ok)
        gemu_machine_set_error(m, "save: can't write %s", path);
    return ok;
}

bool gemu_machine_load_file(GemuMachine *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        gemu_machine_set_error(m, "load: can't open %s", path);
        return false;
    }
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            uint8_t *b = realloc(buf, cap);
            if (!b) {
                free(buf);
                fclose(f);
                gemu_machine_set_error(m, "load: out of memory");
                return false;
            }
            buf = b;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        if (!n)
            break;
        len += n;
    }
    bool err = ferror(f);
    fclose(f);
    if (err) {
        free(buf);
        gemu_machine_set_error(m, "load: can't read %s", path);
        return false;
    }
    bool ok = gemu_machine_load_state(m, buf, len);
    free(buf);
    return ok;
}
