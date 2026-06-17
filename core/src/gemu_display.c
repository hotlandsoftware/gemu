#define _POSIX_C_SOURCE 200809L
#include "gemu_display_priv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── INI reader ──────────────────────────────────────────────────────────── */

static void ini_path(char *out, size_t len) {
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !*base) base = getenv("APPDATA");
    if (!base || !*base) base = "C:\\Users\\Default\\AppData\\Local";
    snprintf(out, len, "%s\\gemu\\gemu.ini", base);
#else
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/tmp";
    snprintf(out, len, "%s/.gemu/gemu.ini", home);
#endif
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

bool gemu_ini_read(const char *section, const char *key,
                   char *buf, int bufsz) {
    char path[512];
    ini_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[256];
    bool in_section = false, found = false;
    while (fgets(line, (int)sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;

        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = '\0'; in_section = (strcasecmp(s + 1, section) == 0); }
            continue;
        }
        if (!in_section) continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(s);
        char *v = trim(eq + 1);
        if (strcasecmp(k, key) == 0) {
            snprintf(buf, (size_t)bufsz, "%s", v);
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

GemuDisplay *gemu_display_create(GemuDisplayType type,
                                 const GemuDisplayConfig *cfg) {
    switch (type) {
    case GEMU_DISPLAY_SDL:
        return gemu_display_sdl_create(cfg);
#ifdef GEMU_GTK
    case GEMU_DISPLAY_GTK:
        return gemu_display_gtk_create(cfg);
#endif
#ifdef HAVE_CACA
    case GEMU_DISPLAY_CURSES:
        return gemu_display_caca_create(cfg);
#endif
    default:
        return NULL;
    }
}

void gemu_display_destroy(GemuDisplay *d) {
    if (!d) return;
    d->do_destroy(d);
    free(d);
}

void gemu_display_render(GemuDisplay *d, const uint32_t *argb, int w, int h) {
    if (d && argb) d->do_render(d, argb, w, h);
}

uint32_t gemu_display_poll(GemuDisplay *d) {
    if (!d) return 0;
    d->held_prev   = d->held;
    d->held        = d->do_poll(d);
    d->last_pressed = d->held & ~d->held_prev;
    return d->held;
}

uint32_t gemu_display_last_pressed(const GemuDisplay *d) {
    return d ? d->last_pressed : 0;
}

GemuPointerState gemu_display_get_pointer(const GemuDisplay *d) {
    if (!d) return (GemuPointerState){ .x = -1, .y = -1 };
    return d->pointer;
}

bool gemu_display_should_quit(const GemuDisplay *d)     { return d && d->quit;  }
bool gemu_display_reset_requested(const GemuDisplay *d) { return d && d->reset; }

void gemu_display_clear_flags(GemuDisplay *d) {
    if (d) { d->quit = false; d->reset = false; }
}

uint32_t gemu_display_pop_raw_key(GemuDisplay *d) {
    if (!d || d->raw_head == d->raw_tail) return 0;
    uint32_t cp = d->raw_queue[d->raw_head];
    d->raw_head = (d->raw_head + 1) % GEMU_DISPLAY_RAW_QUEUE;
    return cp;
}

void gemu_display_open_rebind_menu(GemuDisplay *d) {
    if (d && d->do_open_rebind) d->do_open_rebind(d);
}
