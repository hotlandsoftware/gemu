#define _POSIX_C_SOURCE 200809L
#include "gemu_display_priv.h"
#include "gemu/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── INI reader ──────────────────────────────────────────────────────────── */

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
    gemu_config_path(path, sizeof(path), "gemu.ini");
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
    case GEMU_DISPLAY_CURSES: {
#ifndef _WIN32
        /* Default: built-in ANSI truecolor half-block renderer (pixterm
         * style).  GEMU_CURSES=caca selects the legacy libcaca dither, and
         * text-terminal machines keep their plain selectable-text path. */
        const char *env = getenv("GEMU_CURSES");
        bool want_caca = env && strcmp(env, "caca") == 0;
        if (!cfg->terminal_text && !want_caca)
            return gemu_display_term_create(cfg);
#endif
#ifdef HAVE_CACA
        return gemu_display_caca_create(cfg);
#else
        return NULL;
#endif
    }
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

void gemu_display_render_text(GemuDisplay *d, const char *text, int rows, int cols) {
    if (d && text && d->do_render_text) d->do_render_text(d, text, rows, cols);
}

void gemu_display_set_paused(GemuDisplay *d, bool paused) {
    if (!d || !d->do_set_title || d->paused_title == paused) return;
    char title[160];
    snprintf(title, sizeof(title), "%s%s",
             d->title[0] ? d->title : "GEMU",
             paused ? " [Paused]" : "");
    d->do_set_title(d, title);
    d->paused_title = paused;
}

uint32_t gemu_display_poll(GemuDisplay *d) {
    if (!d) return 0;
    d->held_prev   = d->held;
    d->pointer.rel_x = 0;
    d->pointer.rel_y = 0;
    d->pointer.pressed = false;
    d->pointer.right_pressed = false;
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

bool gemu_display_is_key_held(GemuDisplay *d, const char *name) {
    if (!d || !d->do_is_key_held) return false;
    return d->do_is_key_held(d, name);
}


void gemu_display_reset_input_bindings(GemuDisplay *d) {
    if (d && d->do_reset_input_bindings) d->do_reset_input_bindings(d);
}

void gemu_display_reset_input_bindings_ud(void *d) {
    gemu_display_reset_input_bindings((GemuDisplay *)d);
}
