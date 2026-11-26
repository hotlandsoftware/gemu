/*
 * GEMU's 86Box layer: a port's device (gemu_86box.h), the device_get_config
 * calls (<86box/device.h>) and logging (<86box/86box.h>).
 */
#include "compat_priv.h"
#include <86box/86box.h>
#include <86box/device.h>
#include "gemu/dev/state.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Gemu86BoxDev *gemu_86box_cur;
uint64_t      gemu_86box_hz;
static unsigned live;              /* devices; with none, the clock is free again */
extern uint64_t tsc;
extern uint64_t TIMER_USEC;

/* ── Logging ──────────────────────────────────────────────────────────────── */

void pclog_ex(const char *fmt, va_list ap) {
    vfprintf(stderr, fmt, ap);
}

void pclog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pclog_ex(fmt, ap);
    va_end(ap);
}

void fatal(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "gemu: 86Box layer%s%s: fatal: ", gemu_86box_cur ? ": " : "",
            gemu_86box_cur ? gemu_86box_cur->dev->path : "");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (!*fmt || fmt[strlen(fmt) - 1] != '\n')
        fputc('\n', stderr);
    exit(1);
}

/* ── Calling into drivers ─────────────────────────────────────────────────── */

Gemu86BoxDev *gemu_86box_enter(Gemu86BoxDev *b) {
    Gemu86BoxDev *prev = gemu_86box_cur;
    gemu_86box_cur = b;
    tsc = gemu_cycles_in(gemu_86box_hz, gemu_now(b->dev->machine));
    return prev;
}

void gemu_86box_leave(Gemu86BoxDev *prev) {
    gemu_86box_cur = prev;
    if (prev)
        tsc = gemu_cycles_in(gemu_86box_hz, gemu_now(prev->dev->machine));
}

Gemu86BoxDev *gemu_86box_need(const char *what) {
    if (!gemu_86box_cur)
        fatal("%s called outside a device's code\n", what);
    return gemu_86box_cur;
}

/* ── Options from the config table ────────────────────────────────────────── */

static char *dup_printf(const char *fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char *s = strdup(buf);
    if (!s) {
        fprintf(stderr, "gemu: 86Box layer: out of memory\n");
        exit(1);
    }
    return s;
}

/* The values of a selection, as text: hex for addresses. */
static const char *const *selection_choices(const device_config_t *c, bool hex, const char **def) {
    unsigned n = 0;
    while (n < 64 && c->selection[n].description && c->selection[n].description[0])
        n++;
    if (!n)
        return NULL;
    const char **ch = calloc(n + 1, sizeof *ch);
    if (!ch) {
        fprintf(stderr, "gemu: 86Box layer: out of memory\n");
        exit(1);
    }
    *def = NULL;
    for (unsigned i = 0; i < n; i++) {
        int v = c->selection[i].value;
        ch[i] = hex ? dup_printf("0x%X", (unsigned)v) : dup_printf("%d", v);
        if (v == c->default_int && !*def)
            *def = ch[i];
    }
    return ch;
}

void gemu_86box_make_options(const Gemu86BoxPort *p) {
    const device_t *dev = p->dev;
    if (strcmp(p->type_name, dev->internal_name)) {
        fprintf(stderr, "gemu: 86Box port %s: the device_t is \"%s\"\n", p->type_name,
                dev->internal_name);
        exit(1);
    }
    unsigned n = 0;
    for (const device_config_t *c = dev->config; c && c->type != CONFIG_END; c++) {
        if (n == GEMU_86BOX_MAX_OPTIONS) {
            fprintf(stderr, "gemu: 86Box port %s: more than %d config entries\n",
                    p->type_name, GEMU_86BOX_MAX_OPTIONS);
            exit(1);
        }
        GemuOptionDef *o = &p->options[n++];
        o->name = c->name;
        o->desc = c->description;
        int type = c->type & CONFIG_TYPE_MASK;
        const char *def = NULL;
        switch (type) {
        case CONFIG_BINARY:
            o->kind = GEMU_OPT_BOOL;
            o->def = c->default_int ? "on" : "off";
            break;
        case CONFIG_SELECTION:
        case CONFIG_HEX16:
        case CONFIG_HEX20:
            o->choices = selection_choices(c, type != CONFIG_SELECTION, &def);
            if (o->choices) {
                o->kind = GEMU_OPT_CHOICE;
                o->def = def;
                break;
            }
            /* a bare number */
            /* fall through */
        case CONFIG_INT:
        case CONFIG_MEMORY:
        case CONFIG_MIDI_OUT:
        case CONFIG_MIDI_IN:
        case CONFIG_MAC:
            o->kind = GEMU_OPT_INT;
            o->def = dup_printf("%d", c->default_int);
            break;
        case CONFIG_SPINNER:
            o->kind = GEMU_OPT_INT;
            o->def = dup_printf("%d", c->default_int);
            o->min = c->spinner.min;
            o->max = c->spinner.max;
            break;
        case CONFIG_STRING:
        case CONFIG_SERPORT:
            o->kind = GEMU_OPT_STRING;
            o->def = c->default_string;
            break;
        case CONFIG_FNAME:
            o->kind = GEMU_OPT_FILE;
            o->def = c->default_string;
            break;
        case CONFIG_BIOS: {
            unsigned k = 0;
            while (k < 32 && c->bios[k].internal_name)
                k++;
            const char **ch = calloc(k + 1, sizeof *ch);
            if (!ch || !k) {
                fprintf(stderr, "gemu: 86Box port %s: %s: no BIOSes listed\n", p->type_name,
                        c->name);
                exit(1);
            }
            for (unsigned i = 0; i < k; i++)
                ch[i] = c->bios[i].internal_name;
            o->kind = GEMU_OPT_CHOICE;
            o->choices = ch;
            o->def = c->default_string;
            break;
        }
        default:
            fprintf(stderr, "gemu: 86Box port %s: %s: config type %d isn't supported\n",
                    p->type_name, c->name, c->type);
            exit(1);
        }
    }
}

/* The option behind config entry `name` of the current device, or NULL. */
static const GemuOptionDef *option(const char *name) {
    Gemu86BoxDev *b = gemu_86box_cur;
    if (!b || !name)
        return NULL;
    for (const GemuOptionDef *o = b->port->options; o->name; o++)
        if (!strcmp(o->name, name))
            return o;
    return NULL;
}

static int option_int(const GemuOptionDef *o) {
    GemuDevice *d = gemu_86box_cur->dev;
    switch (o->kind) {
    case GEMU_OPT_BOOL:   return gemu_opt_bool(d, o->name);
    case GEMU_OPT_INT:    return (int)gemu_opt_int(d, o->name);
    case GEMU_OPT_CHOICE: return (int)strtol(gemu_opt_str(d, o->name), NULL, 0);
    default:              return (int)strtol(gemu_opt_str(d, o->name), NULL, 0);
    }
}

int device_get_config_int(const char *name) {
    const GemuOptionDef *o = option(name);
    return o ? option_int(o) : 0;
}

int device_get_config_int_ex(const char *name, int def) {
    const GemuOptionDef *o = option(name);
    return o ? option_int(o) : def;
}

int device_get_config_hex16(const char *name) { return device_get_config_int(name); }
int device_get_config_hex20(const char *name) { return device_get_config_int(name); }

const char *device_get_config_string(const char *name) {
    const GemuOptionDef *o = option(name);
    if (!o || o->kind == GEMU_OPT_BOOL || o->kind == GEMU_OPT_INT)
        return "";
    const char *s = gemu_opt_str(gemu_86box_cur->dev, o->name);
    return s ? s : "";
}

/* ── The device ───────────────────────────────────────────────────────────── */

bool gemu_86box_init(GemuDevice *d, const Gemu86BoxPort *p) {
    Gemu86BoxDev *b = d->priv;
    uint64_t hz = gemu_device_clock(d);
    if (!hz || hz >= 1ull << 32) {
        gemu_device_error(d, "an 86Box device needs a clock (its bus's) below 4.3 GHz");
        return false;
    }
    if (!live)
        gemu_86box_hz = 0;
    if (gemu_86box_hz && gemu_86box_hz != hz) {
        gemu_device_error(d, "86Box devices share one clock: %" PRIu64 " Hz, not %" PRIu64,
                          gemu_86box_hz, hz);
        return false;
    }
    gemu_86box_hz = hz;
    TIMER_USEC = (hz << 32) / 1000000u;
    live++;
    b->port = p;                          /* from here destroy undoes it */
    b->dev = d;
    for (unsigned i = 0; i < 16; i++) {
        char name[8];
        snprintf(name, sizeof name, "irq%u", i);
        if (!(b->irq[i] = gemu_line_out(d, name)))
            return false;
    }
    const device_t *dev = p->dev;
    if (dev->available && !dev->available()) {
        gemu_device_error(d, "%s isn't available (are its ROMs there?)", dev->name);
        return false;
    }
    Gemu86BoxDev *prev = gemu_86box_enter(b);
    b->in_init = true;
    b->priv = dev->init ? dev->init(dev) : NULL;
    b->in_init = false;
    bool ok = b->priv != NULL || !dev->init;
    if (!ok)
        gemu_device_error(d, "%s: its init failed", dev->name);
    else if (p->state && !p->state(d, b->priv))
        ok = false;
    gemu_86box_leave(prev);
    return ok;
}

void gemu_86box_reset(GemuDevice *d, GemuResetKind kind) {
    Gemu86BoxDev *b = d->priv;
    const device_t *dev = b->port->dev;
    if (!dev->reset || (kind == GEMU_RESET_SOFT && !(dev->flags & DEVICE_SOFTRESET)))
        return;
    Gemu86BoxDev *prev = gemu_86box_enter(b);
    dev->reset(b->priv);
    gemu_86box_leave(prev);
}

void gemu_86box_destroy(GemuDevice *d) {
    Gemu86BoxDev *b = d->priv;
    if (!b->port || !b->dev)              /* its init never got going */
        return;
    if (live) live--;
    if (b->port->dev->close && b->priv) {
        Gemu86BoxDev *prev = gemu_86box_enter(b);
        b->port->dev->close(b->priv);
        gemu_86box_leave(prev);
    }
    b->priv = NULL;
    gemu_86box_io_free(b);
    gemu_86box_timers_free(b);
}

void gemu_86box_describe(GemuDevice *d, char *buf, size_t len) {
    Gemu86BoxDev *b = d->priv;
    if (!b->port) {
        snprintf(buf, len, "86Box device (not started)");
        return;
    }
    snprintf(buf, len, "%s (86Box v6.0: %s)", b->port->dev->name, b->port->dev->internal_name);
}

void gemu_86box_post_load(GemuDevice *d) {
    Gemu86BoxDev *b = d->priv;
    if (!b->port || !b->port->post_load)
        return;
    Gemu86BoxDev *prev = gemu_86box_enter(b);
    b->port->post_load(d, b->priv);
    gemu_86box_leave(prev);
}

void *gemu_86box_priv(GemuDevice *d) {
    return d ? ((Gemu86BoxDev *)d->priv)->priv : NULL;
}

bool gemu_86box_connect_irqs(GemuDevice *d, const char *in_fmt, unsigned count) {
    for (unsigned i = 0; i < count && i < 16; i++) {
        char out[GEMU_DEVICE_PATH_MAX + 8], in[128];
        snprintf(out, sizeof out, "%s:irq%u", d->path, i);
        snprintf(in, sizeof in, in_fmt, i);
        if (!gemu_lines_connect(d->machine, out, in, 0))
            return false;
    }
    return true;
}
