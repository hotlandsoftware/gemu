#pragma once
#ifdef GEMU_GTK
#include <gtk/gtk.h>
#include "gemu/menu.h"

/* A GtkMenuBar that shows a GemuMenu model (gemu/menu.h).  build() returns
 * a fresh model (the caller's to keep; freed here after use).  A menu is
 * rebuilt each time it opens, so checkmarks and drive names are current. */
GtkWidget *gemu_gtk_menubar_new(GemuMenu *(*build)(void *ud), void *ud);

/* Re-read the bar's titles (e.g. a Blocks menu that appears once the machine
 * has registered drives).  Does nothing while a menu is open. */
void gemu_gtk_menubar_refresh(GtkWidget *bar);

#endif /* GEMU_GTK */
