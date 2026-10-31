#pragma once
/* Debug > Hex Editor.  Three implementations, chosen by the display in use
 * (hex_editor_select.c), as the Game Genie window chooses its own:
 *   hardware/device/hex_editor.c        GTK, with -display gtk (GTK builds)
 *   hardware/device/hex_editor_sdl.c    an SDL window, with -display sdl
 *   hardware/device/hex_editor_win32.c  native Win32, with -display sdl on Windows
 * Every build compiles the ones its platform can run. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "gemu/display.h"

/* One memory region shown as a tab in the hex editor. */
typedef struct {
    const char    *label;      /* tab title, e.g. "CPU RAM ($0000-$07FF)" */
    uint8_t       *data;       /* live pointer (may be NULL → "(no data)") */
    size_t         size;       /* bytes */
    bool           read_only;
    uint32_t       base_addr;  /* address shown at offset 0 */
} HexRegion;

typedef struct HexEditor HexEditor;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/* Create a hidden hex editor window with one tab per region, in the style
 * of `display`.  n_regions must be > 0 and <= HEX_MAX_TABS (8).
 * Returns NULL on failure, or for a display with no hex editor (none,
 * curses). */
HexEditor *hex_editor_create(GemuDisplayType display, const HexRegion *regions, int n_regions);

/* Destroy the window and free all resources. */
void hex_editor_destroy(HexEditor *he);

/* ── Display ─────────────────────────────────────────────────────────────── */

/* Show / hide the window. */
void hex_editor_show(HexEditor *he);
void hex_editor_hide(HexEditor *he);
bool hex_editor_is_visible(const HexEditor *he);

/* Refresh the hex display with current data.  Safe to call from the run
 * loop every frame - it will only redraw when the window is visible. */
void hex_editor_refresh(HexEditor *he);

/* ── The implementations (for hex_editor_select.c) ──────────────────────── */

#define HEX_EDITOR_IMPL(prefix, Type)                                             \
    typedef struct Type Type;                                                     \
    Type *hex_editor_##prefix##_create(const HexRegion *regions, int n_regions); \
    void  hex_editor_##prefix##_destroy(Type *he);                                \
    void  hex_editor_##prefix##_show(Type *he);                                   \
    void  hex_editor_##prefix##_hide(Type *he);                                   \
    bool  hex_editor_##prefix##_is_visible(const Type *he);                       \
    void  hex_editor_##prefix##_refresh(Type *he);

#ifdef GEMU_GTK
HEX_EDITOR_IMPL(gtk, HexEditorGtk)
#endif
#ifdef _WIN32
HEX_EDITOR_IMPL(win32, HexEditorWin32)
#else
HEX_EDITOR_IMPL(sdl, HexEditorSdl)
#endif
