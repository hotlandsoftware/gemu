#pragma once
#include <stdbool.h>

/*
 * Window menus: one description of the menu bar that every front end draws -
 * the SDL backend draws it inside the window (above the picture, never on
 * VNC), the GTK backend turns it into a GtkMenuBar.
 *
 * The display layer builds the standard bar for each window
 * (gemu_display_build_menubar() in gemu_display_menu.c): Machine, Blocks,
 * Scale, Input, Debug, Help.  It's rebuilt whenever a menu opens, so checkmarks
 * and drive names are always current.
 */

typedef enum {
    GEMU_MENU_ACTION,
    GEMU_MENU_CHECK,       /* shows a checkmark when checked() is true */
    GEMU_MENU_RADIO,       /* shows a bullet when checked() is true */
    GEMU_MENU_SEPARATOR,
    GEMU_MENU_SUBMENU,
} GemuMenuItemType;

typedef struct GemuMenu GemuMenu;

typedef struct {
    GemuMenuItemType type;
    char      label[96];
    char      shortcut[24];                 /* shown right-aligned, e.g. "Ctrl+Shift+Tab" */
    void    (*activate)(void *ud, int arg);
    bool    (*checked)(void *ud, int arg);  /* CHECK/RADIO */
    bool    (*enabled)(void *ud, int arg);  /* NULL = always enabled */
    void     *ud;
    int       arg;
    GemuMenu *submenu;                      /* SUBMENU */
} GemuMenuItem;

struct GemuMenu {
    char          title[48];
    GemuMenuItem *items;
    int           n, cap;
};

GemuMenu     *gemu_menu_new(const char *title);
void          gemu_menu_free(GemuMenu *m);

/* Append an item; returns it so callers can set shortcut/checked/enabled. */
GemuMenuItem *gemu_menu_add(GemuMenu *m, GemuMenuItemType type, const char *label,
                            void (*activate)(void *ud, int arg), void *ud, int arg);
void          gemu_menu_add_separator(GemuMenu *m);
GemuMenu     *gemu_menu_add_submenu(GemuMenu *m, const char *label);

bool          gemu_menu_item_enabled(const GemuMenuItem *it);
bool          gemu_menu_item_checked(const GemuMenuItem *it);
void          gemu_menu_item_activate(const GemuMenuItem *it);
