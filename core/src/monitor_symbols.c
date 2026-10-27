/*
 * GEMU monitor - symbol tables.
 *
 * `symbols load FILE` reads the labels an assembler wrote, so the monitor
 * can take them anywhere it takes an address ("break Kernel", "x Score+2")
 * and print them back (breakpoint hits, disassembly).  Formats:
 *
 *   DASM -s:          NAME   f000   (R )         (hex value, sorted list)
 *   VICE / ca65 -Ln:  al C:F000 .NAME
 *   generic:          NAME = $F000   /   NAME equ $F000
 *
 * The file is remembered so `symbols reload` (or a machine's autoreload)
 * can pick up a fresh build.
 */
#include "monitor_priv.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static bool parse_value(const char *s, uint32_t *out, bool default_hex) {
    char *end;
    unsigned long v;
    if (*s == '$')                          v = strtoul(s + 1, &end, 16);
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = strtoul(s + 2, &end, 16);
    else if (*s == '%')                     v = strtoul(s + 1, &end, 2);
    else                                    v = strtoul(s, &end, default_hex ? 16 : 10);
    if (end == s || (*end && !isspace((unsigned char)*end))) return false;
    *out = (uint32_t)v;
    return true;
}

static bool is_name(const char *s) {
    if (!*s || isdigit((unsigned char)*s)) return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '.' && *s != '@') return false;
    return true;
}

static void add_symbol(GemuMonitor *mon, const char *name, uint32_t value) {
    if (mon->n_syms == mon->cap_syms) {
        int cap = mon->cap_syms ? mon->cap_syms * 2 : 256;
        MonSymbol *p = realloc(mon->syms, (size_t)cap * sizeof(*p));
        if (!p) return;
        mon->syms = p;
        mon->cap_syms = cap;
    }
    MonSymbol *s = &mon->syms[mon->n_syms++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->value = value;
}

void gemu_monitor_clear_symbols(GemuMonitor *mon) {
    if (!mon) return;
    free(mon->syms);
    mon->syms = NULL;
    mon->n_syms = mon->cap_syms = 0;
}

int gemu_monitor_load_symbols(GemuMonitor *mon, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    gemu_monitor_clear_symbols(mon);
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *tok[4] = { 0 };
        int n = 0;
        for (char *p = strtok(line, " \t\r\n"); p && n < 4; p = strtok(NULL, " \t\r\n"))
            tok[n++] = p;
        if (n < 2 || tok[0][0] == ';' || !strncmp(tok[0], "---", 3)) continue;
        uint32_t v;
        if (!strcmp(tok[0], "al") && n >= 3) {          /* VICE label file */
            char *a = tok[1];
            if (strchr(a, ':')) a = strchr(a, ':') + 1;
            const char *name = tok[2][0] == '.' ? tok[2] + 1 : tok[2];
            if (parse_value(a, &v, true) && is_name(name)) add_symbol(mon, name, v);
        } else if (n >= 3 && (!strcmp(tok[1], "=") || !strcasecmp(tok[1], "equ"))) {
            if (is_name(tok[0]) && parse_value(tok[2], &v, false)) add_symbol(mon, tok[0], v);
        } else if (is_name(tok[0]) && parse_value(tok[1], &v, true)) {   /* DASM */
            add_symbol(mon, tok[0], v);
        }
    }
    fclose(f);
    snprintf(mon->sym_path, sizeof(mon->sym_path), "%s", path);
    struct stat st;
    mon->sym_mtime = stat(path, &st) == 0 ? (int64_t)st.st_mtime : 0;
    return mon->n_syms;
}

bool gemu_monitor_reload_symbols_if_changed(GemuMonitor *mon) {
    if (!mon || !mon->sym_path[0]) return false;
    struct stat st;
    if (stat(mon->sym_path, &st) != 0 || (int64_t)st.st_mtime == mon->sym_mtime) return false;
    char path[sizeof(mon->sym_path)];
    snprintf(path, sizeof(path), "%s", mon->sym_path);
    return gemu_monitor_load_symbols(mon, path) >= 0;
}

static bool addr_match(const GemuMonitor *mon, uint32_t a, uint32_t b) {
    uint32_t m = mon->addr_mask ? mon->addr_mask : 0xFFFFFFFFu;
    return (a & m) == (b & m);
}

const char *gemu_monitor_symbol_name(const GemuMonitor *mon, uint32_t addr) {
    if (!mon) return NULL;
    for (int i = 0; i < mon->n_syms; i++)       /* exact value first... */
        if (mon->syms[i].value == addr) return mon->syms[i].name;
    for (int i = 0; i < mon->n_syms; i++)       /* ...then through mirrors */
        if (addr_match(mon, mon->syms[i].value, addr)) return mon->syms[i].name;
    return NULL;
}

bool gemu_monitor_symbol_value(const GemuMonitor *mon, const char *name, uint32_t *value) {
    if (!mon) return false;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < mon->n_syms; i++)
            if (pass ? !strcasecmp(mon->syms[i].name, name) : !strcmp(mon->syms[i].name, name)) {
                *value = mon->syms[i].value;
                return true;
            }
    return false;
}

/* An address: hex ("f000", "$F000", "0xF000") or a symbol, optionally
 * with an offset ("Score+2", "Table-1"). */
bool gemu_monitor_parse_addr(const GemuMonitor *mon, const char *s, uint64_t *out) {
    if (*s == '$') s++;
    if (!*s) return false;
    char *end;
    uint64_t v = strtoull(s, &end, 16);
    if (*end == '\0') { *out = v; return true; }

    char name[64];
    size_t n = strcspn(s, "+-");
    if (n == 0 || n >= sizeof(name)) return false;
    memcpy(name, s, n);
    name[n] = '\0';
    uint32_t base;
    if (!gemu_monitor_symbol_value(mon, name, &base)) return false;
    int64_t off = 0;
    if (s[n]) {
        uint32_t o;
        if (!parse_value(s + n + 1, &o, false)) return false;
        off = s[n] == '+' ? (int64_t)o : -(int64_t)o;
    }
    *out = (uint64_t)((int64_t)base + off);
    return true;
}

void gemu_monitor_set_addr_mask(GemuMonitor *mon, uint32_t mask) {
    if (mon) mon->addr_mask = mask;
}

static bool contains_nocase(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return true;
    return n == 0;
}

/* symbols [load FILE | reload | list [filter] | clear] */
GemuMonCmd mon_cmd_symbols(GemuMonitor *mon, const char *args) {
    char sub[16] = "", rest[MON_LINE_MAX] = "";
    sscanf(args, "%15s %[^\n]", sub, rest);
    if (!strcasecmp(sub, "load")) {
        if (!rest[0]) { mon_printf(mon, "usage: symbols load <file>\n"); return GEMU_MON_NONE; }
        int n = gemu_monitor_load_symbols(mon, rest);
        if (n < 0) mon_printf(mon, "symbols: cannot open '%s'\n", rest);
        else       mon_printf(mon, "symbols: %d loaded from %s\n", n, rest);
    } else if (!strcasecmp(sub, "reload")) {
        if (!mon->sym_path[0]) { mon_printf(mon, "symbols: no file loaded\n"); return GEMU_MON_NONE; }
        char path[sizeof(mon->sym_path)];
        snprintf(path, sizeof(path), "%s", mon->sym_path);
        int n = gemu_monitor_load_symbols(mon, path);
        mon_printf(mon, "symbols: %d loaded from %s\n", n < 0 ? 0 : n, path);
    } else if (!strcasecmp(sub, "clear")) {
        gemu_monitor_clear_symbols(mon);
        mon->sym_path[0] = '\0';
        mon_printf(mon, "symbols: cleared\n");
    } else if (!sub[0] || !strcasecmp(sub, "list")) {
        int shown = 0;
        for (int i = 0; i < mon->n_syms; i++) {
            if (rest[0] && !contains_nocase(mon->syms[i].name, rest)) continue;
            mon_printf(mon, "  %04X  %s\n", mon->syms[i].value, mon->syms[i].name);
            if (++shown >= 200) { mon_printf(mon, "  ... (narrow it down: symbols list <text>)\n"); break; }
        }
        mon_printf(mon, "%d symbol%s%s%s\n", mon->n_syms, mon->n_syms == 1 ? "" : "s",
                   mon->sym_path[0] ? " from " : "", mon->sym_path);
    } else {
        mon_printf(mon, "usage: symbols [load <file> | reload | list [text] | clear]\n");
    }
    return GEMU_MON_NONE;
}
