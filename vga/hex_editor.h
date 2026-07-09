#pragma once
/* Two independent implementations share this API: vga/hex_editor.c (GTK,
 * used when GEMU_GTK) and vga/hex_editor_win32.c (plain Win32, used when
 * _WIN32 && !GEMU_GTK). Exactly one is compiled into any given build. */
#if defined(GEMU_GTK) || defined(_WIN32)
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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

/* Create a hidden hex editor window with one tab per region.
 * n_regions must be > 0 and <= HEX_MAX_TABS (8).
 * Returns NULL on failure. */
HexEditor *hex_editor_create(const HexRegion *regions, int n_regions);

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

#endif /* GEMU_GTK || _WIN32 */
