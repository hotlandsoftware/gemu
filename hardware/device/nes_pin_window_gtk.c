/*
 * Debug > Cartridge Pins for GTK builds (see nes_pin_window.h): a window
 * holding a drawing area that paints the shared panel's pixels with cairo
 * and passes the mouse on - the same window nes_pin_window_sdl.c shows.
 */
#ifdef GEMU_GTK
#include "nes_pin_window.h"
#include <gtk/gtk.h>
#include <stdlib.h>

typedef struct {
    NesPinWindow base;
    NesPinPanel  panel;
    GtkWidget   *window, *area;
    uint32_t    *fb;
    int          mx, my;
} GtkPinWindow;

static gboolean on_draw(GtkWidget *wd, cairo_t *cr, gpointer ud) {
    (void)wd;
    GtkPinWindow *w = ud;
    nes_pin_panel_draw(&w->panel, w->fb, (uint32_t)(g_get_monotonic_time() / 1000));
    cairo_surface_t *s = cairo_image_surface_create_for_data(
        (unsigned char *)w->fb, CAIRO_FORMAT_ARGB32, NES_PIN_PANEL_W, NES_PIN_PANEL_H,
        NES_PIN_PANEL_W * (int)sizeof(uint32_t));
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint(cr);
    cairo_surface_destroy(s);
    return TRUE;
}

static gboolean on_press(GtkWidget *wd, GdkEventButton *ev, gpointer ud) {
    (void)wd;
    GtkPinWindow *w = ud;
    if (ev->type != GDK_BUTTON_PRESS) return TRUE;   /* not the double-click repeat */
    w->mx = (int)ev->x; w->my = (int)ev->y;
    if (ev->button == 1) nes_pin_panel_mouse(&w->panel, w->mx, w->my, NES_PIN_MOUSE_LEFT);
    if (ev->button == 3) nes_pin_panel_mouse(&w->panel, w->mx, w->my, NES_PIN_MOUSE_RIGHT);
    gtk_widget_queue_draw(w->area);
    return TRUE;
}

static gboolean on_release(GtkWidget *wd, GdkEventButton *ev, gpointer ud) {
    (void)wd;
    GtkPinWindow *w = ud;
    nes_pin_panel_mouse(&w->panel, (int)ev->x, (int)ev->y, NES_PIN_MOUSE_UP);
    return TRUE;
}

static gboolean on_motion(GtkWidget *wd, GdkEventMotion *ev, gpointer ud) {
    (void)wd;
    GtkPinWindow *w = ud;
    w->mx = (int)ev->x; w->my = (int)ev->y;
    nes_pin_panel_mouse(&w->panel, w->mx, w->my, NES_PIN_MOUSE_MOVE);
    return TRUE;
}

static gboolean on_leave(GtkWidget *wd, GdkEventCrossing *ev, gpointer ud) {
    (void)wd; (void)ev;
    GtkPinWindow *w = ud;
    nes_pin_panel_mouse(&w->panel, -1, -1, NES_PIN_MOUSE_LEAVE);
    return TRUE;
}

static gboolean on_scroll(GtkWidget *wd, GdkEventScroll *ev, gpointer ud) {
    (void)wd;
    GtkPinWindow *w = ud;
    int n = 0;
    if (ev->direction == GDK_SCROLL_UP) n = 1;
    else if (ev->direction == GDK_SCROLL_DOWN) n = -1;
    else if (ev->direction == GDK_SCROLL_SMOOTH) n = ev->delta_y < 0 ? 1 : ev->delta_y > 0 ? -1 : 0;
    if (n) nes_pin_panel_wheel(&w->panel, (int)ev->x, (int)ev->y, n);
    return TRUE;
}

static gboolean on_delete_event(GtkWidget *wd, GdkEvent *ev, gpointer ud) {
    (void)ev; (void)ud;
    gtk_widget_hide(wd);
    return TRUE;   /* keep it for next time */
}

static void gtk_show(NesPinWindow *base) {
    GtkPinWindow *w = (GtkPinWindow *)base;
    gtk_widget_show_all(w->window);
    gtk_window_present(GTK_WINDOW(w->window));
}

static void gtk_hide(NesPinWindow *base) {
    gtk_widget_hide(((GtkPinWindow *)base)->window);
}

static bool gtk_visible(const NesPinWindow *base) {
    return gtk_widget_get_visible(((const GtkPinWindow *)base)->window);
}

static void gtk_refresh(NesPinWindow *base) {
    GtkPinWindow *w = (GtkPinWindow *)base;
    if (gtk_widget_get_visible(w->window)) gtk_widget_queue_draw(w->area);
}

static void gtk_destroy(NesPinWindow *base) {
    GtkPinWindow *w = (GtkPinWindow *)base;
    if (!w) return;
    gtk_widget_destroy(w->window);
    free(w->fb);
    free(w);
}

NesPinWindow *nes_pin_window_gtk_create(NesPins *pins) {
    if (!pins) return NULL;
    GtkPinWindow *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->fb = calloc((size_t)NES_PIN_PANEL_W * NES_PIN_PANEL_H, sizeof(uint32_t));
    if (!w->fb) { free(w); return NULL; }
    w->base = (NesPinWindow){ gtk_show, gtk_hide, gtk_visible, gtk_refresh, gtk_destroy };
    nes_pin_panel_init(&w->panel, pins);

    w->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w->window), "GEMU - Cartridge Pins");
    gtk_window_set_resizable(GTK_WINDOW(w->window), FALSE);
    g_signal_connect(w->window, "delete-event", G_CALLBACK(on_delete_event), w);

    w->area = gtk_drawing_area_new();
    gtk_widget_set_size_request(w->area, NES_PIN_PANEL_W, NES_PIN_PANEL_H);
    gtk_widget_add_events(w->area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                   GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK |
                                   GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(w->area, "draw", G_CALLBACK(on_draw), w);
    g_signal_connect(w->area, "button-press-event", G_CALLBACK(on_press), w);
    g_signal_connect(w->area, "button-release-event", G_CALLBACK(on_release), w);
    g_signal_connect(w->area, "motion-notify-event", G_CALLBACK(on_motion), w);
    g_signal_connect(w->area, "leave-notify-event", G_CALLBACK(on_leave), w);
    g_signal_connect(w->area, "scroll-event", G_CALLBACK(on_scroll), w);
    gtk_container_add(GTK_CONTAINER(w->window), w->area);
    return &w->base;
}
#endif /* GEMU_GTK */
