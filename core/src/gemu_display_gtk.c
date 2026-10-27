#ifdef GEMU_GTK
#include "gemu_display_priv.h"
#include "gemu/video.h"
#include "gemu/gtk_menu.h"
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <SDL2/SDL.h>
#include <stdio.h>
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

typedef struct GtkBackendS {
    GemuVideoGtk *video;
    GemuDisplay  *parent; /* back-pointer so signal handlers can update state */
    GtkWidget    *menubar;
    gint64        menubar_refreshed;

    /* Monitors detached into windows of their own (View > Detach) */
    struct GtkHeadWin {
        struct GtkBackendS *b;
        int        head;
        GtkWidget *window, *area;
        uint32_t  *fb;
        int        w, h, aspect_w, aspect_h;
    } heads[GEMU_DISPLAY_MAX_HEADS];

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

    /* Ctrl+Alt+1..9 switches monitors, as in QEMU */
    if (down && d->n_heads > 1 &&
        (ev->state & GDK_CONTROL_MASK) && (ev->state & GDK_MOD1_MASK) &&
        ev->keyval >= GDK_KEY_1 && ev->keyval <= GDK_KEY_9) {
        gemu_display_select_head(d, (int)(ev->keyval - GDK_KEY_1));
        return TRUE;
    }

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

    /* Key not in action table - still update raw queue for VP-601 mode */
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

static void gtk_pointer_update(GtkBackend *b, double x, double y,
                               bool button, bool pressed,
                               bool right_button, bool right_pressed) {
    GemuDisplay *d = b->parent;
    int px, py, aw, ah;
    gemu_video_gtk_picture_rect(b->video, &px, &py, &aw, &ah);
    x -= px;
    y -= py;
    if (aw <= 0 || ah <= 0 || x < 0.0 || y < 0.0 || x >= aw || y >= ah) {
        d->pointer = (GemuPointerState){
            .x = -1, .y = -1,
            .button = button, .pressed = pressed,
            .right_button = right_button, .right_pressed = right_pressed,
        };
        return;
    }

    d->pointer.x = (int)(x * (double)b->fb_width / (double)aw);
    d->pointer.y = (int)(y * (double)b->fb_height / (double)ah);
    d->pointer.button = button;
    d->pointer.pressed = d->pointer.pressed || pressed;
    d->pointer.right_button = right_button;
    d->pointer.right_pressed = d->pointer.right_pressed || right_pressed;
}

static gboolean on_pointer_motion(GtkWidget *w, GdkEventMotion *ev, gpointer ud) {
    GtkBackend *b = ud;
    bool button = (ev->state & GDK_BUTTON1_MASK) != 0;
    bool right_button = (ev->state & GDK_BUTTON3_MASK) != 0;
    gtk_pointer_update(b, ev->x, ev->y, button, false, right_button, false);
    (void)w;
    return FALSE;
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    GtkBackend *b = ud;
    if (ev->button == 1)
        gtk_pointer_update(b, ev->x, ev->y,
                           ev->type == GDK_BUTTON_PRESS,
                           ev->type == GDK_BUTTON_PRESS,
                           b->parent->pointer.right_button, false);
    else if (ev->button == 3)
        gtk_pointer_update(b, ev->x, ev->y,
                           b->parent->pointer.button, false,
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
    GtkBackend *b = d->backend;
    gemu_video_gtk_poll();
    /* SDL windows opened alongside GTK (the R.O.B. view) handle their events
     * in event watches, which run during the pump; nothing else reads SDL's
     * queue here.  Without pumping, the compositor's pings go unanswered
     * and the window is marked "Not Responding". */
    if (SDL_GetCurrentVideoDriver()) {   /* SDL video is up (implicitly, too) */
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    }
    /* Pick up new top-level menus (the monitor is attached after creation). */
    gint64 now = g_get_monotonic_time();
    if (now - b->menubar_refreshed > G_USEC_PER_SEC) {
        gemu_gtk_menubar_refresh(b->menubar);
        b->menubar_refreshed = now;
    }
    /* d->held is updated by on_key signal; return current value */
    return d->held;
}

static void gtk_do_destroy(GemuDisplay *d) {
    GtkBackend *b = d->backend;
    if (!b) return;
    for (int i = 0; i < GEMU_DISPLAY_MAX_HEADS; i++) {
        if (b->heads[i].window) gtk_widget_destroy(b->heads[i].window);
        free(b->heads[i].fb);
    }
    gemu_video_gtk_destroy(b->video);
    free(b);
    d->backend = NULL;
}

static void gtk_do_set_title(GemuDisplay *d, const char *title) {
    GtkBackend *b = d->backend;
    GtkWidget *win = b && b->video ? gemu_video_gtk_window(b->video) : NULL;
    if (win) gtk_window_set_title(GTK_WINDOW(win), title ? title : "GEMU");
}

/* ── Detached monitor windows ────────────────────────────────────────────── */

static gboolean head_draw(GtkWidget *w, cairo_t *cr, gpointer ud) {
    struct GtkHeadWin *hw = ud;
    int aw = gtk_widget_get_allocated_width(w), ah = gtk_widget_get_allocated_height(w);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    if (!hw->fb || hw->w <= 0) return TRUE;
    /* fit the picture at its shape, centred */
    double shape = hw->aspect_w > 0 && hw->aspect_h > 0 ? (double)hw->aspect_w / hw->aspect_h
                                                        : (double)hw->w / hw->h;
    double pw = aw, ph = aw / shape;
    if (ph > ah) { ph = ah; pw = ah * shape; }
    cairo_surface_t *img = cairo_image_surface_create_for_data(
        (unsigned char *)hw->fb, CAIRO_FORMAT_RGB24, hw->w, hw->h, hw->w * 4);
    cairo_translate(cr, (aw - pw) / 2, (ah - ph) / 2);
    cairo_scale(cr, pw / hw->w, ph / hw->h);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_surface_destroy(img);
    return TRUE;
}

static void gtk_do_close_head(GemuDisplay *d, int head) {
    GtkBackend *b = d->backend;
    struct GtkHeadWin *hw = &b->heads[head];
    if (hw->window) gtk_widget_destroy(hw->window);
    free(hw->fb);
    *hw = (struct GtkHeadWin){ 0 };
    gemu_display_head_closed(d, head);
}

static gboolean head_delete(GtkWidget *w, GdkEvent *ev, gpointer ud) {
    (void)w; (void)ev;
    struct GtkHeadWin *hw = ud;
    gtk_do_close_head(hw->b->parent, hw->head);
    return TRUE;
}

static bool gtk_do_detach_head(GemuDisplay *d, int head) {
    GtkBackend *b = d->backend;
    const GemuDisplayHead *h = &d->heads[head];
    struct GtkHeadWin *hw = &b->heads[head];
    int scale = gemu_video_gtk_get_scale(b->video);
    if (scale < 1) scale = 1;
    char title[200];
    gemu_window_title(title, sizeof(title), h->name, false);
    *hw = (struct GtkHeadWin){ .b = b, .head = head, .aspect_w = h->aspect_w, .aspect_h = h->aspect_h };
    hw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(hw->window), title);
    hw->area = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(hw->window), hw->area);
    int ww = h->width * scale;
    int wh = h->aspect_w > 0 && h->aspect_h > 0 ? ww * h->aspect_h / h->aspect_w : h->height * scale;
    gtk_window_set_default_size(GTK_WINDOW(hw->window), ww, wh);
    g_signal_connect(hw->area, "draw", G_CALLBACK(head_draw), hw);
    g_signal_connect(hw->window, "delete-event", G_CALLBACK(head_delete), hw);
    /* keys typed into it go to the guest, like the main window's */
    gtk_widget_add_events(hw->window, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    g_signal_connect(hw->window, "key-press-event",   G_CALLBACK(on_key), b);
    g_signal_connect(hw->window, "key-release-event", G_CALLBACK(on_key), b);
    gtk_widget_show_all(hw->window);
    return true;
}

static void gtk_do_present_head(GemuDisplay *d, int head, const uint32_t *argb, int w, int h) {
    GtkBackend *b = d->backend;
    struct GtkHeadWin *hw = &b->heads[head];
    if (!hw->window) return;
    if (w != hw->w || h != hw->h) {
        uint32_t *fb = realloc(hw->fb, (size_t)w * h * sizeof(*fb));
        if (!fb) return;
        hw->fb = fb;
        hw->w = w;
        hw->h = h;
    }
    memcpy(hw->fb, argb, (size_t)w * h * sizeof(*argb));
    gtk_widget_queue_draw(hw->area);
}

static void gtk_do_raise_head(GemuDisplay *d, int head) {
    GtkBackend *b = d->backend;
    if (b->heads[head].window) gtk_window_present(GTK_WINDOW(b->heads[head].window));
}

/* ── Window menu hooks ───────────────────────────────────────────────────── */

static GemuMenu *build_menubar(void *ud) {
    return gemu_display_build_menubar(ud);
}

static GtkWindow *gtk_parent(GemuDisplay *d) {
    GtkBackend *b = d->backend;
    return GTK_WINDOW(gemu_video_gtk_window(b->video));
}

static void gtk_ui_about(GemuDisplay *d) {
    GtkWidget *dlg = gtk_message_dialog_new(gtk_parent(d), GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                            "%s", gemu_display_about_text());
    gtk_window_set_title(GTK_WINDOW(dlg), "About GEMU");
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void gtk_ui_choose_file(GemuDisplay *d, const char *title,
                               void (*done)(void *ud, const char *path), void *ud) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new(title, gtk_parent(d),
                                                 GTK_FILE_CHOOSER_ACTION_OPEN,
                                                 "_Cancel", GTK_RESPONSE_CANCEL,
                                                 "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) done(ud, path);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static int gtk_ui_get_scale(GemuDisplay *d) {
    GtkBackend *b = d->backend;
    return gemu_video_gtk_get_scale(b->video);
}

static void gtk_ui_set_scale(GemuDisplay *d, int scale) {
    GtkBackend *b = d->backend;
    gemu_video_gtk_set_scale(b->video, scale);
}

static void gtk_do_set_geometry(GemuDisplay *d, int w, int h, int aspect_w, int aspect_h) {
    GtkBackend *b = d->backend;
    b->fb_width = w;
    b->fb_height = h;
    gemu_video_gtk_set_geometry(b->video, w, h, aspect_w, aspect_h);
}

/* ── Create ──────────────────────────────────────────────────────────────── */

GemuDisplay *gemu_display_gtk_create(const GemuDisplayConfig *cfg) {
    char initial_title[200];
    gemu_window_title(initial_title, sizeof(initial_title), NULL, false);
    GtkBackend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->action_defs = cfg->actions;
    b->n_actions   = cfg->n_actions;
    b->ini_section = cfg->ini_section;
    b->fb_width    = cfg->fb_width;
    b->fb_height   = cfg->fb_height;

    GemuVideoGtkSpec spec = {
        .title    = initial_title,
        .width    = cfg->fb_width,
        .height   = cfg->fb_height,
        .scale    = cfg->scale ? cfg->scale : 1,
        .window_width  = cfg->window_width,
        .window_height = cfg->window_height,
        .aspect_w = cfg->aspect_w,
        .aspect_h = cfg->aspect_h,
    };
    b->video = gemu_video_gtk_create(&spec);
    if (!b->video) { free(b); return NULL; }

    build_bindings(b);

    GemuDisplay *d = calloc(1, sizeof(*d));
    if (!d) { gtk_do_destroy(&(GemuDisplay){ .backend = b }); return NULL; }
    d->backend   = b;
    d->do_render = gtk_do_render;
    d->do_poll   = gtk_do_poll;
    d->do_set_title = gtk_do_set_title;
    d->do_destroy = gtk_do_destroy;
    gemu_window_title(d->title, sizeof(d->title), NULL, false);
    d->pointer.x = d->pointer.y = -1;
    d->ui_about       = gtk_ui_about;
    d->ui_choose_file = gtk_ui_choose_file;
    d->ui_get_scale   = gtk_ui_get_scale;
    d->ui_set_scale   = gtk_ui_set_scale;
    d->do_set_geometry = gtk_do_set_geometry;
    d->do_detach_head  = gtk_do_detach_head;
    d->do_present_head = gtk_do_present_head;
    d->do_raise_head   = gtk_do_raise_head;
    d->do_close_head   = gtk_do_close_head;
    b->parent = d;

    b->menubar = gemu_gtk_menubar_new(build_menubar, d);
    gemu_video_gtk_set_menubar(b->video, b->menubar);

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
