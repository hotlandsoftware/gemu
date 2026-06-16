#ifdef GEMU_GTK
#include "gemu/gtk_menu.h"
#include <stdlib.h>

/* small context for the hex toggle callback */
typedef struct {
    void (*cb)(void *);
    void  *ud;
} HexMenuCtx;

static void on_menu_reset(GtkMenuItem *item, gpointer mon) {
    (void)item;
    gemu_monitor_enqueue_reset((GemuMonitor *)mon);
}

static void on_menu_quit(GtkMenuItem *item, gpointer mon) {
    (void)item;
    gemu_monitor_enqueue_quit((GemuMonitor *)mon);
}

static void on_menu_hex_editor(GtkMenuItem *item, gpointer data) {
    (void)item;
    HexMenuCtx *ctx = data;
    if (ctx->cb) ctx->cb(ctx->ud);
}

void gemu_gtk_add_action_menu(GtkWidget *vbox, GemuMonitor *mon,
                               void (*hex_toggle_cb)(void *),
                               void *hex_toggle_ud) {
    GtkWidget *menubar     = gtk_menu_bar_new();
    GtkWidget *action_menu = gtk_menu_new();
    GtkWidget *action_item = gtk_menu_item_new_with_label("Action");
    GtkWidget *reset_item  = gtk_menu_item_new_with_label("Reset");
    GtkWidget *quit_item   = gtk_menu_item_new_with_label("Quit");

    gtk_menu_item_set_submenu(GTK_MENU_ITEM(action_item), action_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(action_menu), reset_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(action_menu), quit_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), action_item);

    g_signal_connect(reset_item, "activate", G_CALLBACK(on_menu_reset), mon);
    g_signal_connect(quit_item,  "activate", G_CALLBACK(on_menu_quit),  mon);

    /* Debug > Hex Editor (only when machine supports it) */
    if (hex_toggle_cb) {
        HexMenuCtx *ctx = malloc(sizeof(*ctx));
        ctx->cb = hex_toggle_cb;
        ctx->ud = hex_toggle_ud;

        GtkWidget *debug_menu = gtk_menu_new();
        GtkWidget *debug_item = gtk_menu_item_new_with_label("Debug");
        GtkWidget *hex_item   = gtk_menu_item_new_with_label("Hex Editor");

        gtk_menu_item_set_submenu(GTK_MENU_ITEM(debug_item), debug_menu);
        gtk_menu_shell_append(GTK_MENU_SHELL(debug_menu), hex_item);
        gtk_menu_shell_append(GTK_MENU_SHELL(menubar), debug_item);

        g_signal_connect(hex_item, "activate",
                         G_CALLBACK(on_menu_hex_editor), ctx);
        /* ctx is leaked — small, lives as long as the menu does */
    }

    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
}
#endif /* GEMU_GTK */
