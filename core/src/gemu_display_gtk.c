#ifdef GEMU_GTK
#include "gemu_display_priv.h"
#include "gemu/video.h"
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <stdlib.h>
#include <string.h>

#define GTK_MAX_BINDINGS GEMU_DISPLAY_MAX_ACTIONS

/* SDL key name strings → GDK keyval for names that differ between the two
 * naming conventions.  Everything not in this table is passed to
 * gdk_keyval_from_name() directly (works for single letters, digits,
 * "Return", "Escape", "Tab", "Delete", arrow keys, F-keys, …). */
static const struct { const char *sdl; guint gdk; } sdl_gdk_map[] = {
    { "Space",        GDK_KEY_space      },
    { "Left Shift",   GDK_KEY_Shift_L    },
    { "Right Shift",  GDK_KEY_Shift_R    },
    { "Left Ctrl",    GDK_KEY_Control_L  },
    { "Right Ctrl",   GDK_KEY_Control_R  },
    { "Left Alt",     GDK_KEY_Alt_L      },
    { "Right Alt",    GDK_KEY_Alt_R      },
    { "Left GUI",     GDK_KEY_Super_L    },
    { "Right GUI",    GDK_KEY_Super_R    },
    { "Caps Lock",    GDK_KEY_Caps_Lock  },
    { "Backspace",    GDK_KEY_BackSpace  },
    { "Page Up",      GDK_KEY_Page_Up    },
    { "Page Down",    GDK_KEY_Page_Down  },
    { "Num Lock",     GDK_KEY_Num_Lock   },
    { "Scroll Lock",  GDK_KEY_Scroll_Lock},
    { "Print Screen", GDK_KEY_Print      },
    { "Keypad 0",     GDK_KEY_KP_0       },
    { "Keypad 1",     GDK_KEY_KP_1       },
    { "Keypad 2",     GDK_KEY_KP_2       },
    { "Keypad 3",     GDK_KEY_KP_3       },
    { "Keypad 4",     GDK_KEY_KP_4       },
    { "Keypad 5",     GDK_KEY_KP_5       },
    { "Keypad 6",     GDK_KEY_KP_6       },
    { "Keypad 7",     GDK_KEY_KP_7       },
    { "Keypad 8",     GDK_KEY_KP_8       },
    { "Keypad 9",     GDK_KEY_KP_9       },
};

static guint sdl_name_to_gdk(const char *name) {
    for (size_t i = 0; i < sizeof(sdl_gdk_map)/sizeof(sdl_gdk_map[0]); i++)
        if (strcmp(name, sdl_gdk_map[i].sdl) == 0)
            return sdl_gdk_map[i].gdk;
    guint kv = gdk_keyval_from_name(name);
    return (kv != GDK_KEY_VoidSymbol) ? kv : GDK_KEY_VoidSymbol;
}

typedef struct {
    GemuVideoGtk *video;
    GemuDisplay  *parent; /* back-pointer so signal handlers can update state */

    struct { guint keyval; uint32_t bit; } bindings[GTK_MAX_BINDINGS];
    int n_bindings;
    int fb_width;
    int fb_height;

    const GemuActionDef *action_defs;
    int                  n_actions;
    const char          *ini_section;
} GtkBackend;

static void build_bindings(GtkBackend *b) {
    b->n_bindings = 0;
    for (int i = 0; i < b->n_actions && b->n_bindings < GTK_MAX_BINDINGS; i++) {
        const GemuActionDef *def = &b->action_defs[i];
        char val[64] = "";
        bool has_binding = false;
        if (b->ini_section)
            has_binding = gemu_ini_read(b->ini_section, def->name, val, sizeof(val));
        const char *key_name = has_binding ? val : def->default_key;
        guint kv = sdl_name_to_gdk(key_name);
        if (kv == GDK_KEY_VoidSymbol) continue;
        b->bindings[b->n_bindings].keyval = gdk_keyval_to_lower(kv);
        b->bindings[b->n_bindings].bit    = def->bit;
        b->n_bindings++;
    }
}

static uint32_t keyval_to_bits(const GtkBackend *b, guint kv) {
    guint lo = gdk_keyval_to_lower(kv);  /* bindings are pre-lowercased */
    for (int i = 0; i < b->n_bindings; i++) {
        if (b->bindings[i].keyval == lo) return b->bindings[i].bit;
    }
    return 0;
}

/* ── Signal callbacks ────────────────────────────────────────────────────── */

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    (void)w;
    GtkBackend  *b    = ud;
    GemuDisplay *d    = b->parent;
    bool         down = (ev->type == GDK_KEY_PRESS);

    if (down && ev->keyval == GDK_KEY_Escape) {
        d->quit = true;
        return TRUE;
    }

    uint32_t bits = keyval_to_bits(b, ev->keyval);
    if (bits) {
        if (down) d->held |=  bits;
        else      d->held &= ~bits;

        /* Raw queue for held printable single-char keys */
        if (down) {
            guint32 ucp = gdk_keyval_to_unicode(ev->keyval);
            if (ucp >= 0x20 && ucp != 0x7f)
                gemu_display_push_raw(d, ucp);
            else if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter)
                gemu_display_push_raw(d, '\r');
            else if (ev->keyval == GDK_KEY_BackSpace)
                gemu_display_push_raw(d, '\b');
            else if (ev->keyval == GDK_KEY_Tab)
                gemu_display_push_raw(d, '\t');
            else if (ev->keyval == GDK_KEY_Delete)
                gemu_display_push_raw(d, 0x7f);
        }
        return TRUE;
    }

    /* Key not in action table — still update raw queue for VP-601 mode */
    if (down) {
        guint32 ucp = gdk_keyval_to_unicode(ev->keyval);
        if (ucp >= 0x20 && ucp != 0x7f)
            gemu_display_push_raw(d, ucp);
        else if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter)
            gemu_display_push_raw(d, '\r');
        else if (ev->keyval == GDK_KEY_BackSpace)
            gemu_display_push_raw(d, '\b');
        else if (ev->keyval == GDK_KEY_Tab)
            gemu_display_push_raw(d, '\t');
        else if (ev->keyval == GDK_KEY_Delete)
            gemu_display_push_raw(d, 0x7f);
    }
    return FALSE;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer ud) {
    (void)w; (void)ev;
    ((GtkBackend *)ud)->parent->quit = true;
    return TRUE;
}

static void gtk_pointer_update(GtkBackend *b, double x, double y, bool button, bool pressed) {
    GemuDisplay *d = b->parent;
    GtkWidget *area = gemu_video_gtk_drawing_area(b->video);
    int aw = gtk_widget_get_allocated_width(area);
    int ah = gtk_widget_get_allocated_height(area);
    if (aw <= 0 || ah <= 0 || x < 0.0 || y < 0.0 || x >= aw || y >= ah) {
        d->pointer = (GemuPointerState){ .x = -1, .y = -1, .button = button, .pressed = pressed };
        return;
    }

    d->pointer.x = (int)(x * (double)b->fb_width / (double)aw);
    d->pointer.y = (int)(y * (double)b->fb_height / (double)ah);
    d->pointer.button = button;
    d->pointer.pressed = d->pointer.pressed || pressed;
}

static gboolean on_pointer_motion(GtkWidget *w, GdkEventMotion *ev, gpointer ud) {
    GtkBackend *b = ud;
    bool button = (ev->state & GDK_BUTTON1_MASK) != 0;
    gtk_pointer_update(b, ev->x, ev->y, button, false);
    (void)w;
    return FALSE;
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    GtkBackend *b = ud;
    if (ev->button == 1)
        gtk_pointer_update(b, ev->x, ev->y,
                           ev->type == GDK_BUTTON_PRESS,
                           ev->type == GDK_BUTTON_PRESS);
    (void)w;
    return FALSE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer ud) {
    GtkBackend *b = ud;
    b->parent->pointer = (GemuPointerState){ .x = -1, .y = -1, .button = false };
    (void)w; (void)ev;
    return FALSE;
}

/* ── Backend callbacks ───────────────────────────────────────────────────── */

static void gtk_do_render(GemuDisplay *d, const uint32_t *argb, int w, int h) {
    GtkBackend *b = d->backend;
    gemu_video_gtk_present_argb(b->video, argb, w, h);
}

static uint32_t gtk_do_poll(GemuDisplay *d) {
    (void)d;
    gemu_video_gtk_poll();
    /* d->held is updated by on_key signal; return current value */
    return d->held;
}

static void gtk_do_destroy(GemuDisplay *d) {
    GtkBackend *b = d->backend;
    if (!b) return;
    gemu_video_gtk_destroy(b->video);
    free(b);
    d->backend = NULL;
}

/* ── Create ──────────────────────────────────────────────────────────────── */

GemuDisplay *gemu_display_gtk_create(const GemuDisplayConfig *cfg) {
    GtkBackend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->action_defs = cfg->actions;
    b->n_actions   = cfg->n_actions;
    b->ini_section = cfg->ini_section;
    b->fb_width    = cfg->fb_width;
    b->fb_height   = cfg->fb_height;

    GemuVideoGtkSpec spec = {
        .title    = cfg->title ? cfg->title : "GEMU",
        .width    = cfg->fb_width,
        .height   = cfg->fb_height,
        .scale    = cfg->scale ? cfg->scale : 2,
        .window_width  = cfg->window_width,
        .window_height = cfg->window_height,
    };
    if (cfg->gtk) {
        spec.monitor       = cfg->gtk->monitor;
        spec.hex_toggle_cb = cfg->gtk->hex_toggle_cb;
        spec.hex_toggle_ud = cfg->gtk->hex_toggle_ud;
    }

    b->video = gemu_video_gtk_create(&spec);
    if (!b->video) { free(b); return NULL; }

    build_bindings(b);

    GemuDisplay *d = calloc(1, sizeof(*d));
    if (!d) { gtk_do_destroy(&(GemuDisplay){ .backend = b }); return NULL; }
    d->backend   = b;
    d->do_render = gtk_do_render;
    d->do_poll   = gtk_do_poll;
    d->do_destroy = gtk_do_destroy;
    d->pointer.x = d->pointer.y = -1;
    b->parent = d;

    GtkWidget *win = gemu_video_gtk_window(b->video);
    GtkWidget *area = gemu_video_gtk_drawing_area(b->video);
    gtk_widget_add_events(win, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    gtk_widget_add_events(area, GDK_POINTER_MOTION_MASK |
                                GDK_BUTTON_PRESS_MASK |
                                GDK_BUTTON_RELEASE_MASK |
                                GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(win, "delete-event",      G_CALLBACK(on_delete), b);
    g_signal_connect(win, "key-press-event",   G_CALLBACK(on_key),    b);
    g_signal_connect(win, "key-release-event", G_CALLBACK(on_key),    b);
    g_signal_connect(area, "motion-notify-event", G_CALLBACK(on_pointer_motion), b);
    g_signal_connect(area, "button-press-event",  G_CALLBACK(on_button), b);
    g_signal_connect(area, "button-release-event", G_CALLBACK(on_button), b);
    g_signal_connect(area, "leave-notify-event",  G_CALLBACK(on_leave), b);

    return d;
}

#endif /* GEMU_GTK */
