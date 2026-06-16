#pragma once
#ifdef GEMU_GTK
#include <gtk/gtk.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Forward declaration — the full NesState is in nes.h */
typedef struct NesState NesState;

typedef struct HexEditor HexEditor;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/* Create a hidden hex editor window.  Returns NULL on failure. */
HexEditor *hex_editor_create(NesState *s);

/* Destroy the window and free all resources. */
void hex_editor_destroy(HexEditor *he);

/* ── Display ─────────────────────────────────────────────────────────────── */

/* Show / hide the window. */
void hex_editor_show(HexEditor *he);
void hex_editor_hide(HexEditor *he);
bool hex_editor_is_visible(const HexEditor *he);

/* Refresh the hex display with current data.  Safe to call from the run
 * loop every frame — it will only redraw when the window is visible. */
void hex_editor_refresh(HexEditor *he);

#endif /* GEMU_GTK */
