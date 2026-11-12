#pragma once
/* Debug > Game Genie: the window that edits a NesCheats list, in the style
 * of FCE Ultra's Cheats dialog - the codes with on/off checkboxes, and
 * Name / Code / Address / Value / Compare fields that encode and decode as
 * you type, with Add / Update / Delete.
 *
 * Two implementations: cheat_dialog_sdl.c (drawn with SDL; Linux and
 * Windows) and cheat_dialog_gtk.c (GTK builds, used with -display gtk). */
#include "nes_cheats.h"
#include <stdbool.h>

typedef struct CheatDialog {
    void (*show)(struct CheatDialog *);
    void (*hide)(struct CheatDialog *);
    bool (*visible)(const struct CheatDialog *);
    void (*refresh)(struct CheatDialog *);     /* once per frame */
    void (*destroy)(struct CheatDialog *);
} CheatDialog;

CheatDialog *cheat_dialog_sdl_create(NesCheats *cheats);
#ifdef GEMU_GTK
CheatDialog *cheat_dialog_gtk_create(NesCheats *cheats);
#endif

static inline void cheat_dialog_toggle(CheatDialog *d) {
    if (!d) return;
    if (d->visible(d)) d->hide(d);
    else d->show(d);
}
