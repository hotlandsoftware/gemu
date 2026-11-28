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
/* States bigger than 2 GiB (RAM) on hosts with a 32-bit off_t (MinGW). */
#define _FILE_OFFSET_BITS 64
#include "dev_priv.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifndef _WIN32
#  include <sys/mman.h>
#endif

#define STATE_MAGIC   "GEMUSTAT"
#define STATE_VERSION 1u

/* ── Streams ──────────────────────────────────────────────────────────────── */

void gemu_state_write(GemuStateWriter *w, const void *p, size_t n) {
    if (w->failed || !n)
        return;
    if (w->f) {
        if (fwrite(p, 1, n, w->f) != n)
            w->failed = true;
        w->len += n;
        return;
    }
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

bool gemu_state_add_named(GemuDevice *d, const char *prefix, const char *name, void *ptr,
                          size_t elem, size_t count) {
    char full[GEMU_STATE_NAME_MAX + 1];
    int n = snprintf(full, sizeof full, "%s%s", prefix ? prefix : "", name ? name : "");
    if (n < 0 || (size_t)n >= GEMU_STATE_NAME_MAX) {
        if (d && d->core && !d->core->state_error[0])
            snprintf(d->core->state_error, sizeof d->core->state_error,
                     "%s: state item name \"%s%s\" is too long", d->path,
                     prefix ? prefix : "", name ? name : "");
        return false;
    }
    return gemu_state_add(d, full, ptr, elem, count);
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

/* ── Tables ───────────────────────────────────────────────────────────────── */

static size_t table_row(const GemuStateItem *it) {
    size_t n = 0;
    for (size_t f = 0; f < it->nfields; f++)
        n += it->fields[f].size;
    return n;
}

static void table_save(void *ud, GemuStateWriter *w) {
    const GemuStateItem *it = ud;
    gemu_state_write_u32(w, (uint32_t)it->nfields);
    for (size_t f = 0; f < it->nfields; f++)
        gemu_state_write_u32(w, (uint32_t)it->fields[f].size);
    for (size_t k = 0; k < it->count; k++)
        for (size_t f = 0; f < it->nfields; f++)
            gemu_state_write(w, it->base + k * it->stride + it->fields[f].offset,
                             it->fields[f].size);
}

static bool table_check(void *ud, GemuStateReader *r) {
    const GemuStateItem *it = ud;
    uint32_t n, size;
    if (!gemu_state_read_u32(r, &n) || n != it->nfields) {
        gemu_state_refuse(r, "table %s: other fields", it->name);
        return false;
    }
    for (size_t f = 0; f < it->nfields; f++)
        if (!gemu_state_read_u32(r, &size) || size != it->fields[f].size) {
            gemu_state_refuse(r, "table %s: field %zu is %u bytes here, saved otherwise",
                              it->name, f, (unsigned)it->fields[f].size);
            return false;
        }
    if (gemu_state_left(r) != it->count * table_row(it)) {
        gemu_state_refuse(r, "table %s: other number of rows", it->name);
        return false;
    }
    return true;
}

static bool table_load(void *ud, GemuStateReader *r) {
    GemuStateItem *it = ud;
    if (!table_check(ud, r))
        return false;
    /* table_check() read the header; the rows follow */
    for (size_t k = 0; k < it->count; k++)
        for (size_t f = 0; f < it->nfields; f++)
            gemu_state_read(r, it->base + k * it->stride + it->fields[f].offset,
                            it->fields[f].size);
    return true;
}

bool gemu_state_add_table(GemuDevice *d, const char *name, void *base, size_t stride,
                          size_t count, const GemuStateField *fields, size_t nfields) {
    if (!d || !d->core)
        return false;
    bool bad = !base || !count || !fields || !nfields;
    for (size_t f = 0; !bad && f < nfields; f++)
        bad = !fields[f].size || fields[f].offset + fields[f].size > stride;
    if (bad) {
        if (!d->core->state_error[0])
            snprintf(d->core->state_error, sizeof d->core->state_error,
                     "%s: state table \"%s\": bad arguments", d->path, name ? name : "(null)");
        return false;
    }
    GemuStateItem *it = new_item(d, name);
    if (!it)
        return false;
    it->base = base;
    it->stride = stride;
    it->count = count;
    it->fields = fields;
    it->nfields = nfields;
    it->save = table_save;
    it->check = table_check;
    it->load = table_load;
    it->ud = it;
    return true;
}

/* ── Saving ───────────────────────────────────────────────────────────────── */

/* Fill in a length written earlier as 0, at offset `at`. */
static void patch_u64(GemuStateWriter *w, size_t at, uint64_t v) {
    if (w->failed)
        return;
    if (!w->f) {
        memcpy(w->buf + at, &v, 8);
        return;
    }
    if (fseeko(w->f, (off_t)at, SEEK_SET) || fwrite(&v, 8, 1, w->f) != 1 ||
        fseeko(w->f, 0, SEEK_END))
        w->failed = true;
}

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
    patch_u64(w, at, w->len - at - 8);
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
        patch_u64(w, len_at, w->len - len_at - 8);
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

static void save_to(GemuMachine *m, GemuStateWriter *w) {
    gemu_device_walk(&m->root, pre_save_cb, NULL);
    gemu_state_write(w, STATE_MAGIC, 8);
    gemu_sw_u64(w, STATE_VERSION);
    gemu_sw_str(w, m->name);
    gemu_device_walk(&m->root, save_device, w);
    size_t at = begin_section(w, 'S');
    gemu_sched_state_save(m, w);
    end_section(w, at);
    at = begin_section(w, 'L');
    gemu_lines_state_save(m, w);
    end_section(w, at);
    gemu_sw_u8(w, 'E');
}

bool gemu_machine_save_state(GemuMachine *m, uint8_t **buf, size_t *len) {
    m->error[0] = 0;
    if (!can_snapshot(m, "save"))
        return false;
    GemuStateWriter w = { 0 };
    save_to(m, &w);
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
        char name[GEMU_STATE_NAME_MAX];
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
    gemu_machine_pace_reset(m);            /* and real time starts over from the loaded time */
    gemu_device_walk(&m->root, post_load_cb, NULL);
    return true;
}

/* Straight to the file: a machine with a lot of RAM isn't held twice. */
bool gemu_machine_save_file(GemuMachine *m, const char *path) {
    m->error[0] = 0;
    if (!can_snapshot(m, "save"))
        return false;
    FILE *f = fopen(path, "wb");
    if (!f) {
        gemu_machine_set_error(m, "save: can't write %s", path);
        return false;
    }
    GemuStateWriter w = { .f = f };
    save_to(m, &w);
    bool ok = !w.failed;
    if (fclose(f))
        ok = false;
    if (!ok)
        gemu_machine_set_error(m, "save: can't write %s", path);
    return ok;
}

/* The file mapped where the host can (read lazily, not copied), else read
 * whole into one allocation of its size. */
bool gemu_machine_load_file(GemuMachine *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        gemu_machine_set_error(m, "load: can't open %s", path);
        return false;
    }
    off_t size = -1;
    if (!fseeko(f, 0, SEEK_END))
        size = ftello(f);
    if (size < 0 || (uint64_t)size > SIZE_MAX || fseeko(f, 0, SEEK_SET)) {
        fclose(f);
        gemu_machine_set_error(m, "load: can't read %s", path);
        return false;
    }
    size_t len = (size_t)size;
#ifndef _WIN32
    if (len) {
        void *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fileno(f), 0);
        if (map != MAP_FAILED) {
            fclose(f);
            bool ok = gemu_machine_load_state(m, map, len);
            munmap(map, len);
            return ok;
        }
    }
#endif
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) {
        fclose(f);
        gemu_machine_set_error(m, "load: out of memory");
        return false;
    }
    bool err = fread(buf, 1, len, f) != len;
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
