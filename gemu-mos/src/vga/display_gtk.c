#include "nes_display.h"
#include "../hardware/nes.h"
#include "../vga/rp2c02.h"
#include "gemu/video.h"
#include <gtk/gtk.h>
#include <stdlib.h>

#define NES_GTK_DEFAULT_SCALE 2

typedef struct {
    GemuVideoGtk *video;
    bool          quit;
    uint8_t       ctrl1;
} NesGtkCtx;

static uint8_t gdk_key_to_btn(guint k) {
    switch (k) {
    case GDK_KEY_z: case GDK_KEY_Z:             return NES_BTN_A;
    case GDK_KEY_x: case GDK_KEY_X:             return NES_BTN_B;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: return NES_BTN_START;
    case GDK_KEY_Shift_R:                        return NES_BTN_SELECT;
    case GDK_KEY_Up:                             return NES_BTN_UP;
    case GDK_KEY_Down:                           return NES_BTN_DOWN;
    case GDK_KEY_Left:                           return NES_BTN_LEFT;
    case GDK_KEY_Right:                          return NES_BTN_RIGHT;
    default:                                     return 0;
    }
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data) {
    (void)w;
    NesGtkCtx *c = data;
    bool down = ev->type == GDK_KEY_PRESS;
    if (down && ev->keyval == GDK_KEY_Escape) { c->quit = true; return TRUE; }
    uint8_t btn = gdk_key_to_btn(ev->keyval);
    if (!btn) return FALSE;
    if (down) c->ctrl1 |=  btn;
    else      c->ctrl1 &= ~btn;
    return TRUE;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer data) {
    (void)w; (void)ev;
    ((NesGtkCtx *)data)->quit = true;
    return TRUE;
}

static void gtk_render(void *vctx, const uint8_t *pixels, int w, int h) {
    NesGtkCtx *c = vctx;
    gemu_video_gtk_present_indexed(c->video, pixels, w, h);
}

static void gtk_poll(void *vctx) {
    (void)vctx;
    gemu_video_gtk_poll();
}

static void gtk_destroy(void *vctx) {
    NesGtkCtx *c = vctx;
    if (!c) return;
    gemu_video_gtk_destroy(c->video);
    free(c);
}

static bool    gtk_should_quit(void *vctx) { return ((NesGtkCtx *)vctx)->quit; }
static uint8_t gtk_ctrl1(void *vctx)       { return ((NesGtkCtx *)vctx)->ctrl1; }

NesDisplay *nes_display_gtk_create(const char *title,
                                   const uint32_t *palette, int scale,
                                   GemuMonitor *mon,
                                   void (*hex_toggle_cb)(void *),
                                   void *hex_toggle_ud) {
    if (scale <= 0) scale = NES_GTK_DEFAULT_SCALE;

    NesGtkCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->video = gemu_video_gtk_create(&(GemuVideoGtkSpec){
        .title         = title ? title : "GEMU",
        .width         = RP2C02_WIDTH,
        .height        = RP2C02_HEIGHT,
        .scale         = scale,
        .palette       = palette,
        .n_colors      = 64,
        .monitor       = mon,
        .hex_toggle_cb = hex_toggle_cb,
        .hex_toggle_ud = hex_toggle_ud,
    });
    if (!c->video) { free(c); return NULL; }

    GtkWidget *window = gemu_video_gtk_window(c->video);
    gtk_widget_add_events(window, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    g_signal_connect(window, "delete-event",      G_CALLBACK(on_delete), c);
    g_signal_connect(window, "key-press-event",   G_CALLBACK(on_key),    c);
    g_signal_connect(window, "key-release-event", G_CALLBACK(on_key),    c);

    NesDisplay *d = calloc(1, sizeof(*d));
    if (!d) { gtk_destroy(c); return NULL; }
    d->render      = gtk_render;
    d->poll        = gtk_poll;
    d->destroy     = gtk_destroy;
    d->should_quit = gtk_should_quit;
    d->ctrl1       = gtk_ctrl1;
    d->ctx         = c;
    return d;
}
