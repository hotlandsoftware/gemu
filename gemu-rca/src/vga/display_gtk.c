#include "rca_display.h"
#include "gemu/video.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#define RCA_GTK_MAX_KEYS 128
#define RCA_GTK_DEFAULT_SCALE 4

typedef struct {
    GemuVideoGtk    *video;
    bool             indexed;
    uint32_t         pixel_on;
    uint32_t         pixel_off;
    bool             quit;
    struct {
        uint32_t keysym;
        bool     down;
    } keys[RCA_GTK_MAX_KEYS];
    int              n_keys;
    uint32_t         queued_keysym;
    RcaKeyboardType  keyboard_type;
} RcaGtkCtx;

static int find_key(RcaGtkCtx *c, uint32_t keysym) {
    for (int i = 0; i < c->n_keys; i++)
        if (c->keys[i].keysym == keysym)
            return i;
    return -1;
}

static void set_key(RcaGtkCtx *c, uint32_t keysym, bool down) {
    if (keysym >= 'A' && keysym <= 'Z')
        keysym += 'a' - 'A';
    int idx = find_key(c, keysym);
    if (idx < 0) {
        if (c->n_keys >= RCA_GTK_MAX_KEYS)
            return;
        idx = c->n_keys++;
        c->keys[idx].keysym = keysym;
    }
    c->keys[idx].down = down;
    if (down)
        c->queued_keysym = keysym;
}

static uint32_t gtk_key_to_rca(guint keyval) {
    guint lower = gdk_keyval_to_lower(keyval);
    if (lower >= 0x20 && lower <= 0x7e)
        return (uint32_t)lower;
    switch (keyval) {
    case GDK_KEY_Escape: return RCA_KEY_ESCAPE;
    case GDK_KEY_Tab:    return RCA_KEY_TAB;
    case GDK_KEY_F2:     return RCA_KEY_F2;
    case GDK_KEY_F3:     return RCA_KEY_F3;
    case GDK_KEY_Left:   return RCA_KEY_LEFT;
    case GDK_KEY_Right:  return RCA_KEY_RIGHT;
    case GDK_KEY_Alt_L:  return RCA_KEY_LALT;
    case GDK_KEY_KP_0:   return RCA_KEY_KP_0;
    case GDK_KEY_KP_1:   return RCA_KEY_KP_1;
    case GDK_KEY_KP_2:   return RCA_KEY_KP_2;
    case GDK_KEY_KP_3:   return RCA_KEY_KP_3;
    case GDK_KEY_KP_4:   return RCA_KEY_KP_4;
    case GDK_KEY_KP_5:   return RCA_KEY_KP_5;
    case GDK_KEY_KP_6:   return RCA_KEY_KP_6;
    case GDK_KEY_KP_7:   return RCA_KEY_KP_7;
    case GDK_KEY_KP_8:   return RCA_KEY_KP_8;
    case GDK_KEY_KP_9:   return RCA_KEY_KP_9;
    case GDK_KEY_Return: return '\r';
    case GDK_KEY_BackSpace: return '\b';
    default:            return 0;
    }
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w;
    RcaGtkCtx *c = data;
    bool down = ev->type == GDK_KEY_PRESS;
    uint32_t sym = gtk_key_to_rca(ev->keyval);
    if (!sym)
        return TRUE;
    if (down && sym == RCA_KEY_ESCAPE) {
        c->quit = true;
        return TRUE;
    }
    set_key(c, sym, down);
    return TRUE;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer data) {
    (void)w;
    (void)ev;
    ((RcaGtkCtx *)data)->quit = true;
    return TRUE;
}

static void gtk_poll(void *ctx) {
    (void)ctx;
    gemu_video_gtk_poll();
}

static bool gtk_should_quit(void *ctx) {
    return ((RcaGtkCtx *)ctx)->quit;
}

static bool gtk_key_down(void *ctx, uint32_t keysym) {
    RcaGtkCtx *c = ctx;
    if (keysym >= 'A' && keysym <= 'Z')
        keysym += 'a' - 'A';
    int idx = find_key(c, keysym);
    return idx >= 0 && c->keys[idx].down;
}

static uint32_t gtk_pop_keysym(void *ctx) {
    RcaGtkCtx *c = ctx;
    uint32_t keysym = c->queued_keysym;
    c->queued_keysym = 0;
    return keysym;
}

static void gtk_render(void *ctx, const uint8_t *vram, int w, int h) {
    RcaGtkCtx *c = ctx;
    if (c->indexed)
        gemu_video_gtk_present_indexed(c->video, vram, w, h);
    else
        gemu_video_gtk_present_mono(c->video, vram, w, h,
                                    c->pixel_on, c->pixel_off);
}

static void gtk_destroy(void *ctx) {
    RcaGtkCtx *c = ctx;
    if (!c) return;
    gemu_video_gtk_destroy(c->video);
    free(c);
}

static RcaDisplay *gtk_create_common(const char *title, int w, int h,
                                     int scale, const uint32_t *palette,
                                     int n_colors, uint32_t on,
                                     uint32_t off, GemuMonitor *mon,
                                     RcaKeyboardType keyboard_type) {
    if (scale <= 0) scale = RCA_GTK_DEFAULT_SCALE;

    RcaGtkCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->indexed = palette && n_colors > 0;
    c->pixel_on = on;
    c->pixel_off = off;
    c->keyboard_type = keyboard_type;
    c->video = gemu_video_gtk_create(&(GemuVideoGtkSpec){
        .title    = title ? title : "GEMU",
        .width    = w,
        .height   = h,
        .scale    = scale,
        .palette  = palette,
        .n_colors = n_colors,
        .monitor  = mon,
    });
    if (!c->video) { free(c); return NULL; }

    GtkWidget *window = gemu_video_gtk_window(c->video);
    gtk_widget_add_events(window, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    g_signal_connect(window, "delete-event",      G_CALLBACK(on_delete), c);
    g_signal_connect(window, "key-press-event",   G_CALLBACK(on_key),    c);
    g_signal_connect(window, "key-release-event", G_CALLBACK(on_key),    c);

    RcaDisplay *d = calloc(1, sizeof(*d));
    if (!d) {
        gtk_destroy(c);
        return NULL;
    }
    d->render = gtk_render;
    d->destroy = gtk_destroy;
    d->poll = gtk_poll;
    d->should_quit = gtk_should_quit;
    d->key_down = gtk_key_down;
    d->pop_keysym = gtk_pop_keysym;
    d->ctx = c;
    d->keyboard_type = keyboard_type;
    return d;
}

RcaDisplay *rca_display_gtk_create_mono(const char *title, int w, int h,
                                        int scale, uint32_t on,
                                        uint32_t off, GemuMonitor *mon,
                                        RcaKeyboardType keyboard_type) {
    return gtk_create_common(title, w, h, scale, NULL, 0, on, off, mon,
                             keyboard_type);
}

RcaDisplay *rca_display_gtk_create_indexed(const char *title, int w, int h,
                                           int scale,
                                           const uint32_t *palette,
                                           int n_colors, GemuMonitor *mon,
                                           RcaKeyboardType keyboard_type) {
    return gtk_create_common(title, w, h, scale, palette, n_colors,
                             0xFFFFFFFFu, 0xFF000000u, mon, keyboard_type);
}
