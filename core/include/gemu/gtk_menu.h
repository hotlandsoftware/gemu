#pragma once
#ifdef GEMU_GTK
#include <gtk/gtk.h>
#include "gemu/monitor.h"

/* Append the standard "Action" menu bar (Reset, Quit) to vbox.
 * Both items enqueue commands into mon so the emulator's run loop handles them.
 *
 * If hex_toggle_cb is non-NULL, a "Debug > Hex Editor" menu item is also
 * added.  The callback receives hex_toggle_ud. */
void gemu_gtk_add_action_menu(GtkWidget *vbox, GemuMonitor *mon,
                               void (*hex_toggle_cb)(void *),
                               void *hex_toggle_ud);

#endif /* GEMU_GTK */
