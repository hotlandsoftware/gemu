#pragma once
/* Netplay > Connect... / Lobby...: the window for a Netplay session.
 * Offline it asks for the mode, a name, the server and the connection
 * type; connected it's the lobby - the server's games and users, Create /
 * Join / Leave / Start, and the chat.
 *
 * Two implementations, like the Game Genie window: netplay_dialog_sdl.c
 * (drawn with SDL; Linux and Windows) and netplay_dialog_gtk.c (GTK builds,
 * used with -display gtk). */
#include "gemu/netplay.h"
#include <stdbool.h>

typedef struct NetplayDialog {
    void (*show)(struct NetplayDialog *);
    void (*hide)(struct NetplayDialog *);
    bool (*visible)(const struct NetplayDialog *);
    void (*refresh)(struct NetplayDialog *);     /* once per frame */
    void (*destroy)(struct NetplayDialog *);
} NetplayDialog;

NetplayDialog *netplay_dialog_sdl_create(Netplay *np);
#ifdef GEMU_GTK
NetplayDialog *netplay_dialog_gtk_create(Netplay *np);
#endif
