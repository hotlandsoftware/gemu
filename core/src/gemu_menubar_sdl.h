#pragma once
#include <SDL2/SDL.h>
#include <stdbool.h>

/* The SDL backend's window menu bar: drawn inside the window above the
 * picture (so VNC, which mirrors only the emulated frame, never shows it),
 * with drop-down menus and simple dialogs in the IBM 8x16 font. */

typedef struct GemuMenubar GemuMenubar;
typedef struct GemuDisplay GemuDisplay;

GemuMenubar *gemu_menubar_create(GemuDisplay *d);
void         gemu_menubar_destroy(GemuMenubar *mb);

/* Height of the bar in output pixels for a window of out_h output pixels. */
int          gemu_menubar_height(int out_h);
/* Width the bar needs to show every menu title (output pixels; 0 before
 * its first build). */
int          gemu_menubar_width(const GemuMenubar *mb, int out_h);

/* Draw the bar, any open menus and dialogs (output pixel coordinates). */
void         gemu_menubar_render(GemuMenubar *mb, SDL_Renderer *r);

/* Feed an event; returns true if the menus used it (don't pass it on).
 * win_w/win_h and out_w/out_h convert mouse coordinates on HiDPI. */
bool         gemu_menubar_handle_event(GemuMenubar *mb, const SDL_Event *ev,
                                       int win_w, int win_h, int out_w, int out_h);

/* A menu or dialog is open: keyboard/controller input belongs to it. */
bool         gemu_menubar_active(const GemuMenubar *mb);

/* Open the first menu for keyboard/controller navigation (the menu key). */
void         gemu_menubar_focus(GemuMenubar *mb);

/* Dialogs */
void         gemu_menubar_about(GemuMenubar *mb, const char *text);
void         gemu_menubar_choose_file(GemuMenubar *mb, const char *title,
                                      void (*done)(void *ud, const char *path), void *ud);
