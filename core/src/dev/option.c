/* Device options: see gemu/dev/option.h. */
#include "dev_priv.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static unsigned count_defs(const GemuOptionDef *defs) {
    unsigned n = 0;
    while (defs && defs[n].name)
        n++;
    return n;
}

static bool parse_int(const char *s, int64_t *v) {
    if (!*s)
        return false;
    errno = 0;
    char *end;
    long long x = strtoll(s, &end, 0);
    if (errno || *end)
        return false;
    *v = x;
    return true;
}

static bool parse_size(const char *s, uint64_t *v) {
    if (!*s || *s == '-')
        return false;
    errno = 0;
    char *end;
    unsigned long long x = strtoull(s, &end, 0);
    if (errno || end == s)
        return false;
    unsigned shift = 0;
    if (*end) {
        switch (*end) {
        case 'k': case 'K': shift = 10; break;
        case 'm': case 'M': shift = 20; break;
        case 'g': case 'G': shift = 30; break;
        default: return false;
        }
        if (end[1])
            return false;
        if (x > (UINT64_MAX >> shift))
            return false;
    }
    *v = (uint64_t)x << shift;
    return true;
}

static bool parse_bool(const char *s, bool *v) {
    static const char *yes[] = { "on", "yes", "true", "1" }, *no[] = { "off", "no", "false", "0" };
    for (unsigned i = 0; i < 4; i++) {
        if (!strcasecmp(s, yes[i])) { *v = true; return true; }
        if (!strcasecmp(s, no[i]))  { *v = false; return true; }
    }
    return false;
}

static void list_choices(char *buf, size_t len, const char *const *choices) {
    buf[0] = 0;
    for (unsigned i = 0; choices && choices[i]; i++) {
        size_t n = strlen(buf);
        snprintf(buf + n, len - n, "%s%s", i ? ", " : "", choices[i]);
    }
}

static void list_options(char *buf, size_t len, const GemuOptionDef *defs) {
    buf[0] = 0;
    for (unsigned i = 0; defs && defs[i].name; i++) {
        size_t n = strlen(buf);
        snprintf(buf + n, len - n, "%s%s", i ? ", " : "", defs[i].name);
    }
}

/* Set option i of d from text (NULL: a bare key).  `what` names it in errors. */
static bool set_value(GemuDevice *d, unsigned i, const char *text, bool is_default) {
    const GemuOptionDef *o = &d->type->options[i];
    GemuOptVal *v = &d->core->opt[i];
    GemuMachine *m = d->machine;
    const char *who = is_default ? "default of " : "";
    if (!text && o->kind != GEMU_OPT_BOOL) {
        gemu_machine_set_error(m, "%s: option %s needs a value", d->path, o->name);
        return false;
    }
    switch (o->kind) {
    case GEMU_OPT_INT: {
        int64_t x;
        if (!parse_int(text, &x)) {
            gemu_machine_set_error(m, "%s: %s%s=%s: not a number", d->path, who, o->name, text);
            return false;
        }
        if ((o->min || o->max) && (x < o->min || x > o->max)) {
            gemu_machine_set_error(m, "%s: %s%s=%s: out of range (%" PRId64 " to %" PRId64 ")",
                                   d->path, who, o->name, text, o->min, o->max);
            return false;
        }
        v->i = x;
        return true;
    }
    case GEMU_OPT_SIZE: {
        uint64_t x;
        if (!parse_size(text, &x)) {
            gemu_machine_set_error(m, "%s: %s%s=%s: not a size (a number, optionally K, M or G)",
                                   d->path, who, o->name, text);
            return false;
        }
        if ((o->min || o->max) && (x < (uint64_t)o->min || x > (uint64_t)o->max)) {
            gemu_machine_set_error(m, "%s: %s%s=%s: out of range (%" PRId64 " to %" PRId64 ")",
                                   d->path, who, o->name, text, o->min, o->max);
            return false;
        }
        v->size = x;
        return true;
    }
    case GEMU_OPT_BOOL:
        if (!text) {
            v->b = true;
            return true;
        }
        if (!parse_bool(text, &v->b)) {
            gemu_machine_set_error(m, "%s: %s%s=%s: not on/off", d->path, who, o->name, text);
            return false;
        }
        return true;
    case GEMU_OPT_CHOICE:
        for (int c = 0; o->choices && o->choices[c]; c++)
            if (!strcasecmp(text, o->choices[c])) {
                v->choice = c;
                return true;
            }
        {
            char list[256];
            list_choices(list, sizeof list, o->choices);
            gemu_machine_set_error(m, "%s: %s%s=%s: not one of %s", d->path, who, o->name, text, list);
        }
        return false;
    case GEMU_OPT_STRING:
    case GEMU_OPT_FILE: {
        char *c = strdup(text);
        if (!c) {
            gemu_machine_set_error(m, "%s: out of memory", d->path);
            return false;
        }
        free(v->str);
        v->str = c;
        return true;
    }
    }
    return false;
}

static bool parse_given(GemuDevice *d, const char *opts);

bool gemu_options_parse(GemuDevice *d, const char *opts) {
    const GemuOptionDef *defs = d->type->options;
    GemuDeviceCore *c = d->core;
    GemuMachine *m = d->machine;
    c->nopt = count_defs(defs);
    if (c->nopt) {
        c->opt = calloc(c->nopt, sizeof *c->opt);
        c->given = calloc(c->nopt, sizeof *c->given);
        if (!c->opt || !c->given) {
            gemu_machine_set_error(m, "%s: out of memory", d->path);
            return false;
        }
    }
    /* defaults */
    for (unsigned i = 0; i < c->nopt; i++) {
        const char *def = defs[i].def;
        if (!def) {
            if (defs[i].kind == GEMU_OPT_CHOICE && !(defs[i].choices && defs[i].choices[0])) {
                gemu_machine_set_error(m, "%s: option %s has no choices", d->path, defs[i].name);
                return false;
            }
            def = defs[i].kind == GEMU_OPT_CHOICE ? defs[i].choices[0]
                : defs[i].kind == GEMU_OPT_BOOL ? "off"
                : defs[i].kind == GEMU_OPT_STRING || defs[i].kind == GEMU_OPT_FILE ? "" : NULL;
            if (!def) {                            /* int, size: 0 unless 0 is out of range, */
                def = "0";                         /* when it must be given */
                if ((defs[i].min || defs[i].max) && (defs[i].min > 0 || defs[i].max < 0))
                    continue;
            }
        }
        if (!set_value(d, i, def, true))
            return false;
    }
    if (opts && !parse_given(d, opts))
        return false;
    for (unsigned i = 0; i < c->nopt; i++)
        if (!defs[i].def && !c->given[i] && (defs[i].kind == GEMU_OPT_INT || defs[i].kind == GEMU_OPT_SIZE) &&
            (defs[i].min || defs[i].max) && (defs[i].min > 0 || defs[i].max < 0)) {
            gemu_machine_set_error(m, "%s: option %s must be given (%s)", d->path, defs[i].name,
                                   defs[i].desc ? defs[i].desc : "no default");
            return false;
        }
    return true;
}

/* "key=val,key=val", ",," = a comma */
static bool parse_given(GemuDevice *d, const char *opts) {
    const GemuOptionDef *defs = d->type->options;
    GemuDeviceCore *c = d->core;
    GemuMachine *m = d->machine;
    size_t len = strlen(opts);
    char *buf = malloc(len + 1);
    if (!buf) {
        gemu_machine_set_error(m, "%s: out of memory", d->path);
        return false;
    }
    bool ok = true;
    const char *p = opts;
    while (ok && *p) {
        /* one key[=value] into buf */
        size_t n = 0;
        while (*p) {
            if (*p == ',') {
                if (p[1] == ',') { buf[n++] = ','; p += 2; continue; }
                break;
            }
            buf[n++] = *p++;
        }
        buf[n] = 0;
        if (*p == ',')
            p++;
        if (!n)
            continue;                              /* an empty item: ",a=1" */
        char *eq = strchr(buf, '=');
        const char *val = NULL;
        if (eq) {
            *eq = 0;
            val = eq + 1;
        }
        unsigned i;
        for (i = 0; i < c->nopt; i++)
            if (!strcmp(defs[i].name, buf))
                break;
        if (i == c->nopt) {
            char list[256];
            list_options(list, sizeof list, defs);
            if (c->nopt)
                gemu_machine_set_error(m, "%s: %s has no option \"%s\" (it has: %s)",
                                       d->path, d->type->name, buf, list);
            else
                gemu_machine_set_error(m, "%s: %s has no options (\"%s\" given)",
                                       d->path, d->type->name, buf);
            ok = false;
        } else if (c->given[i]) {
            gemu_machine_set_error(m, "%s: option %s given twice", d->path, buf);
            ok = false;
        } else {
            ok = set_value(d, i, val, false);
            c->given[i] = true;
        }
    }
    free(buf);
    return ok;
}

void gemu_options_free(GemuDevice *d) {
    GemuDeviceCore *c = d->core;
    for (unsigned i = 0; c->opt && i < c->nopt; i++)
        if (d->type->options[i].kind == GEMU_OPT_STRING || d->type->options[i].kind == GEMU_OPT_FILE)
            free(c->opt[i].str);
    free(c->opt);
    free(c->given);
    c->opt = NULL;
    c->given = NULL;
}

/* ── Getters ──────────────────────────────────────────────────────────────── */

static unsigned find(const GemuDevice *d, const char *name, GemuOptKind k1, GemuOptKind k2,
                     GemuOptKind k3) {
    const GemuDeviceCore *c = d->core;
    for (unsigned i = 0; c && i < c->nopt; i++) {
        const GemuOptionDef *o = &d->type->options[i];
        if (!strcmp(o->name, name)) {
            if (o->kind != k1 && o->kind != k2 && o->kind != k3)
                break;
            return i;
        }
    }
    fprintf(stderr, "gemu: %s (%s) has no option \"%s\" of that kind\n",
            d->path, d->type ? d->type->name : "machine", name);
    abort();
}

int64_t gemu_opt_int(const GemuDevice *d, const char *name) {
    return d->core->opt[find(d, name, GEMU_OPT_INT, GEMU_OPT_INT, GEMU_OPT_INT)].i;
}
bool gemu_opt_bool(const GemuDevice *d, const char *name) {
    return d->core->opt[find(d, name, GEMU_OPT_BOOL, GEMU_OPT_BOOL, GEMU_OPT_BOOL)].b;
}
uint64_t gemu_opt_size(const GemuDevice *d, const char *name) {
    return d->core->opt[find(d, name, GEMU_OPT_SIZE, GEMU_OPT_SIZE, GEMU_OPT_SIZE)].size;
}
int gemu_opt_choice(const GemuDevice *d, const char *name) {
    return d->core->opt[find(d, name, GEMU_OPT_CHOICE, GEMU_OPT_CHOICE, GEMU_OPT_CHOICE)].choice;
}
const char *gemu_opt_str(const GemuDevice *d, const char *name) {
    unsigned i = find(d, name, GEMU_OPT_STRING, GEMU_OPT_FILE, GEMU_OPT_CHOICE);
    if (d->type->options[i].kind == GEMU_OPT_CHOICE)
        return d->type->options[i].choices[d->core->opt[i].choice];
    return d->core->opt[i].str;
}
bool gemu_opt_given(const GemuDevice *d, const char *name) {
    const GemuDeviceCore *c = d->core;
    for (unsigned i = 0; c && i < c->nopt; i++)
        if (!strcmp(d->type->options[i].name, name))
            return c->given[i];
    return false;
}

bool gemu_device_spec_split(const char *spec, char *type, unsigned type_len, const char **opts) {
    const char *comma = strchr(spec, ',');
    size_t n = comma ? (size_t)(comma - spec) : strlen(spec);
    if (!n || n >= type_len)
        return false;
    memcpy(type, spec, n);
    type[n] = 0;
    *opts = comma ? comma + 1 : NULL;
    return true;
}

bool gemu_opts_route(const char *spec, const GemuDeviceType *const *types, unsigned n,
                     char (*out)[GEMU_OPTS_ROUTE_LEN], char *err, size_t err_len) {
    if (err_len) err[0] = 0;
    for (unsigned i = 0; i < n; i++) out[i][0] = 0;
    const char *p = spec ? spec : "";
    while (*p) {
        size_t len = strcspn(p, ",");
        char tok[128];
        if (len && len < sizeof tok) {
            memcpy(tok, p, len);
            tok[len] = 0;
            const char *name = tok, *val = NULL;
            if (*name == '-' || *name == '+') { val = *name == '-' ? "off" : "on"; name++; }
            char *eq = strchr(tok, '=');
            if (eq) { *eq = 0; val = eq + 1; }
            int found = -1;
            for (unsigned i = 0; i < n && found < 0; i++)
                for (const GemuOptionDef *o = types[i]->options; o && o->name; o++)
                    if (!strcmp(o->name, name)) { found = (int)i; break; }
            if (found < 0) {
                int at = snprintf(err, err_len, "unknown -M setting '%s'; ", name);
                bool any = false;
                for (unsigned i = 0; i < n; i++)
                    for (const GemuOptionDef *o = types[i]->options; o && o->name; o++) {
                        if (at >= 0 && (size_t)at < err_len)
                            at += snprintf(err + at, err_len - (size_t)at, "%s %s",
                                           any ? "" : "this machine has:", o->name);
                        any = true;
                    }
                if (!any && at >= 0 && (size_t)at < err_len)
                    snprintf(err + at, err_len - (size_t)at, "this machine has no settings");
                return false;
            }
            size_t at = strlen(out[found]);
            snprintf(out[found] + at, GEMU_OPTS_ROUTE_LEN - at, "%s%s%s%s", at ? "," : "", name,
                     val ? "=" : "", val ? val : "");
        }
        p += len;
        if (*p == ',') p++;
    }
    return true;
}
