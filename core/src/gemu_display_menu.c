/*
 * The standard window menu bar, the same for every machine:
 *
 *   Machine  Pause, Reset, Screenshot, Quit
 *   Blocks   one submenu per monitor media device: Change..., Eject, Flip
 *   Scale    window scale 1x-4x
 *   Input    one entry per input device (its bindings), reset bindings
 *   Debug    Hex Editor (machines that have one)
 *   Help     About GEMU
 *
 * Actions go through the machine's monitor where possible, so the menus do
 * exactly what the equivalent monitor command does.
 */
#include "gemu_display_priv.h"
#include "gemu/menu.h"
#include "gemu/gemu.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── Machine ─────────────────────────────────────────────────────────────── */

static bool is_paused(void *ud, int arg) {
    (void)arg;
    GemuDisplay *d = ud;
    return gemu_monitor_is_paused(d->monitor);
}

static void toggle_pause(void *ud, int arg) {
    (void)arg;
    GemuDisplay *d = ud;
    gemu_monitor_enqueue_command(d->monitor, gemu_monitor_is_paused(d->monitor) ? "cont" : "stop");
}

static void do_reset(void *ud, int arg) {
    (void)arg;
    GemuDisplay *d = ud;
    if (d->monitor) gemu_monitor_enqueue_command(d->monitor, "system_reset");
    else            d->reset = true;
}

static void do_screenshot(void *ud, int arg) {
    (void)arg;
    GemuDisplay *d = ud;
    char cmd[96];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(cmd, sizeof(cmd), "screendump gemu-%Y%m%d-%H%M%S.png", tm);
    gemu_monitor_enqueue_command(d->monitor, cmd);
}

static void do_quit(void *ud, int arg) {
    (void)arg;
    GemuDisplay *d = ud;
    d->quit = true;
}

/* ── Blocks ──────────────────────────────────────────────────────────────── */

static const GemuMediaDevice *media(GemuDisplay *d, int i) {
    return gemu_monitor_media_get(d->monitor, i);
}

/* Change... picks a file, then runs "change NAME FILE". */
typedef struct { GemuDisplay *d; int index; } ChangeCtx;
static ChangeCtx change_ctx;

static void change_chosen(void *ud, const char *path) {
    ChangeCtx *c = ud;
    const GemuMediaDevice *dev = media(c->d, c->index);
    if (!dev || !path || !*path) return;
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "change %s \"%s\"", dev->name, path);
    gemu_monitor_enqueue_command(c->d->monitor, cmd);
}

static void drive_change(void *ud, int i) {
    GemuDisplay *d = ud;
    const GemuMediaDevice *dev = media(d, i);
    if (!dev || !d->ui_choose_file) return;
    change_ctx = (ChangeCtx){ d, i };
    char title[96];
    snprintf(title, sizeof(title), "Insert media into %s", dev->name);
    d->ui_choose_file(d, title, change_chosen, &change_ctx);
}

static void drive_eject(void *ud, int i) {
    GemuDisplay *d = ud;
    const GemuMediaDevice *dev = media(d, i);
    char cmd[96];
    if (!dev) return;
    snprintf(cmd, sizeof(cmd), "eject %s", dev->name);
    gemu_monitor_enqueue_command(d->monitor, cmd);
}

static void drive_flip(void *ud, int i) {
    GemuDisplay *d = ud;
    const GemuMediaDevice *dev = media(d, i);
    char cmd[96];
    if (!dev) return;
    snprintf(cmd, sizeof(cmd), "flip %s", dev->name);
    gemu_monitor_enqueue_command(d->monitor, cmd);
}

static bool drive_has_media(void *ud, int i) {
    const GemuMediaDevice *dev = media(ud, i);
    return dev && dev->file[0];
}

static bool can_choose_file(void *ud, int i) {
    (void)i;
    GemuDisplay *d = ud;
    return d->ui_choose_file != NULL;
}

static bool never(void *ud, int arg) {
    (void)ud; (void)arg;
    return false;
}

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* ── Scale / Input / Help ────────────────────────────────────────────────── */

static bool scale_is(void *ud, int scale) {
    GemuDisplay *d = ud;
    return d->ui_get_scale && d->ui_get_scale(d) == scale;
}

static void set_scale(void *ud, int scale) {
    GemuDisplay *d = ud;
    if (d->ui_set_scale) d->ui_set_scale(d, scale);
}

static void input_config(void *ud, int page) {
    GemuDisplay *d = ud;
    if (d->ui_input_config) d->ui_input_config(d, page);
}

static void reset_bindings(void *ud, int arg) {
    (void)arg;
    gemu_display_reset_input_bindings(ud);
}

static void hex_editor(void *ud, int arg) {
    (void)arg;
    GemuDisplay *d = ud;
    if (d->hex_toggle_cb) d->hex_toggle_cb(d->hex_toggle_ud);
}

static void about(void *ud, int arg) {
    (void)arg;
    GemuDisplay *d = ud;
    if (d->ui_about) d->ui_about(d);
}

const char *gemu_display_about_text(void) {
    return "GEMU " GEMU_VERSION_STR " - Generic EMUlator\n"
           "\n"
           "Emulating everything in existence!\n"
           "\n"
           "Build date: " __DATE__ "\n"
           "Type 'help' in monitor for commands.";
}

/* ── The bar ─────────────────────────────────────────────────────────────── */

GemuMenu *gemu_display_build_menubar(GemuDisplay *d) {
    GemuMenu *bar = gemu_menu_new("");
    GemuMenuItem *it;

    GemuMenu *m = gemu_menu_add_submenu(bar, "Machine");
    if (d->monitor) {
        it = gemu_menu_add(m, GEMU_MENU_CHECK, "Pause", toggle_pause, d, 0);
        it->checked = is_paused;
    }
    gemu_menu_add(m, GEMU_MENU_ACTION, "Reset", do_reset, d, 0);
    if (d->monitor)
        gemu_menu_add(m, GEMU_MENU_ACTION, "Screenshot", do_screenshot, d, 0);
    gemu_menu_add_separator(m);
    gemu_menu_add(m, GEMU_MENU_ACTION, "Quit", do_quit, d, 0);

    int n_media = gemu_monitor_media_count(d->monitor);
    if (n_media > 0) {
        GemuMenu *drives = gemu_menu_add_submenu(bar, "Blocks");
        for (int i = 0; i < n_media; i++) {
            const GemuMediaDevice *dev = media(d, i);
            char label[160];
            snprintf(label, sizeof(label), "%s: %s", dev->name,
                     dev->file[0] ? base_name(dev->file) : "(empty)");
            GemuMenu *sub = gemu_menu_add_submenu(drives, label);
            if (dev->change) {
                it = gemu_menu_add(sub, GEMU_MENU_ACTION, "Change...", drive_change, d, i);
                it->enabled = can_choose_file;
            }
            if (dev->eject) {
                it = gemu_menu_add(sub, GEMU_MENU_ACTION, "Eject", drive_eject, d, i);
                it->enabled = drive_has_media;
            }
            if (dev->flip) {
                it = gemu_menu_add(sub, GEMU_MENU_ACTION, "Flip disk", drive_flip, d, i);
                it->enabled = drive_has_media;
            }
            if (sub->n == 0) {   /* fixed drive: listed, nothing to do */
                it = gemu_menu_add(sub, GEMU_MENU_ACTION, "(not removable)", NULL, d, i);
                it->enabled = never;
            }
        }
    }

    if (d->ui_set_scale) {
        GemuMenu *view = gemu_menu_add_submenu(bar, "Scale");
        for (int s = 1; s <= 4; s++) {
            char label[16];
            snprintf(label, sizeof(label), "%dx", s);
            it = gemu_menu_add(view, GEMU_MENU_RADIO, label, set_scale, d, s);
            it->checked = scale_is;
        }
    }

    /* Input: one entry per input device (its key/controller bindings);
     * no tab at all when the machine has no remappable input. */
    int n_inputs = d->ui_input_pages ? d->ui_input_pages(d) : 0;
    if (n_inputs > 0) {
        GemuMenu *input = gemu_menu_add_submenu(bar, "Input");
        for (int i = 0; i < n_inputs; i++) {
            char label[96];
            d->ui_input_page_label(d, i, label, sizeof(label));
            gemu_menu_add(input, GEMU_MENU_ACTION, label, input_config, d, i);
        }
        if (d->do_reset_input_bindings)
            gemu_menu_add_separator(input);
        if (d->do_reset_input_bindings)
            gemu_menu_add(input, GEMU_MENU_ACTION, "Reset bindings to defaults", reset_bindings, d, 0);
    }

    if (d->hex_toggle_cb) {
        GemuMenu *debug = gemu_menu_add_submenu(bar, "Debug");
        gemu_menu_add(debug, GEMU_MENU_ACTION, "Hex Editor", hex_editor, d, 0);
    }

    if (d->ui_about) {
        GemuMenu *help = gemu_menu_add_submenu(bar, "Help");
        gemu_menu_add(help, GEMU_MENU_ACTION, "About GEMU", about, d, 0);
    }
    return bar;
}
