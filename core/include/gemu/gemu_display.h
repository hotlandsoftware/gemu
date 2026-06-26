#pragma once
/*
 * GemuDisplay — unified display + input interface for all GEMU emulators.
 *
 * DESIGN
 * ──────
 * Each machine describes its inputs as a flat table of named actions
 * (GemuActionDef[]). The display core maps physical keys to those actions
 * (defaults in the table, overrides from gemu.ini) and returns a uint32_t
 * bitmask each frame.  The machine never sees SDL keycodes, caca keycodes,
 * or GDK keysyms — only its own action bits.
 *
 * The machine always owns the run loop. gemu_display_poll() is non-blocking;
 * it drains the backend's event queue and returns the current action bitmask.
 *
 * All framebuffers are passed as ARGB8888 (0xAARRGGBB). Machines with
 * palette or monochrome framebuffers convert before calling
 * gemu_display_render().  The extra memcpy is negligible for the display
 * sizes these machines use.
 */

#include <stdint.h>
#include <stdbool.h>
#include "display.h"    /* GemuDisplayType, GemuRendererType */
#include "monitor.h"    /* GemuMonitor */

typedef struct GemuDisplay GemuDisplay;

/* ── Action table ────────────────────────────────────────────────────────────
 *
 * Machines declare an array of these at startup.  Each entry names one
 * logical action, assigns it a single bitmask position, and provides a
 * default key binding expressed as an SDL key name string (the same
 * vocabulary used in gemu.ini: "Z", "Return", "Up", "Left Shift", …).
 * gemu.ini overrides the default if a matching [ini_section] / name entry
 * exists.  Backends that don't use SDL (caca, ncurses) map the key name
 * to their own keycode on init.
 *
 * Maximum 32 actions per machine (one uint32_t bitmask).
 */
typedef struct {
    const char *name;        /* INI key and human label, e.g. "A", "p1_up"  */
    uint32_t    bit;         /* single power-of-two bit in the action mask   */
    const char *default_key; /* SDL_GetKeyFromName()-compatible default       */
} GemuActionDef;

/* Convenience: define action bits as sequential powers of two */
#define GEMU_ACTION(n) (1u << (n))

/* ── Pointer state (light-gun / mouse) ───────────────────────────────────── */
typedef struct {
    int  x, y;      /* position in logical (framebuffer) pixels; -1 if outside */
    bool button;
    bool pressed;   /* true for one poll when the primary button went down */
} GemuPointerState;

/* ── GTK-specific extras ─────────────────────────────────────────────────────
 *
 * Fields only used by the GTK backend.  Other backends ignore them.
 * Pass NULL/0 when not building with GTK.
 */
typedef struct {
    GemuMonitor *monitor;
    void       (*hex_toggle_cb)(void *ud);
    void        *hex_toggle_ud;
} GemuDisplayGtkExtras;

/* ── Input page descriptor ───────────────────────────────────────────────────
 * Partitions actions[] into named pages shown in the input menu.
 * Pages are consecutive slices: pages[0] covers the first n_actions actions, etc. */
typedef struct {
    const char *name;      /* page heading, e.g. "nes-controller", "famicom-mic" */
    int         n_actions; /* count of consecutive actions belonging to this page */
} GemuInputPage;

/* ── Display config ──────────────────────────────────────────────────────── */
typedef struct {
    /* Window / framebuffer */
    const char          *title;
    int                  fb_width, fb_height; /* logical pixel dimensions */
    int                  scale;               /* ignored by terminal backends */
    int                  window_width;        /* override window width  (0 = fb_width*scale) */
    int                  window_height;       /* override window height (0 = fb_height*scale) */
    GemuRendererType     renderer;            /* SDL hint; ignored elsewhere */

    /* Action table */
    const GemuActionDef *actions;
    int                  n_actions;
    const char          *ini_section; /* gemu.ini section, e.g. "nes-controller" */
    bool                 no_rebind;   /* show menu but hide the Input/rebind page */

    /* Optional per-device pages; NULL means all actions on one page (ini_section). */
    const GemuInputPage *pages;
    int                  n_pages;

    /* GTK extras — NULL for non-GTK builds / backends */
    const GemuDisplayGtkExtras *gtk;
} GemuDisplayConfig;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/*
 * gemu_display_create() reads gemu.ini for the given ini_section, builds the
 * key → action map, and opens the window / terminal.  Returns NULL on failure.
 */
GemuDisplay *gemu_display_create(GemuDisplayType type,
                                 const GemuDisplayConfig *cfg);
void         gemu_display_destroy(GemuDisplay *d);

/* ── Per-frame I/O ───────────────────────────────────────────────────────── */

/*
 * Push an ARGB8888 frame.  May be called with a NULL pixels pointer to signal
 * "nothing changed" (the backend may choose to skip redraw).
 * w and h must match cfg->fb_width / cfg->fb_height; they are accepted as
 * parameters to support machines that switch resolution mid-run (e.g. SCHIP).
 */
void gemu_display_render(GemuDisplay *d, const uint32_t *argb, int w, int h);

/*
 * Drain the backend's event queue (non-blocking).  Must be called once per
 * emulated frame.  Returns the bitmask of all currently-held action bits.
 *
 * For GTK: pumps pending GLib events.
 * For SDL: processes the SDL event queue.
 * For caca: polls caca_get_event with timeout=0.
 */
uint32_t gemu_display_poll(GemuDisplay *d);

/*
 * Action bit of the most recently newly-pressed key this frame (up→down
 * transition since the previous poll).  Returns 0 if no key was newly
 * pressed.  Use for CHIP-8 LD Vx,K and RCA wait-for-key semantics.
 *
 * The value is valid only immediately after gemu_display_poll().
 */
uint32_t gemu_display_last_pressed(const GemuDisplay *d);

/* ── Pointer (light-gun / mouse) ─────────────────────────────────────────── */

/*
 * Returns mouse/pointer state in framebuffer pixel coordinates.
 * x/y are -1 when the pointer is outside the emulator window.
 * The state is updated by gemu_display_poll(); query after polling.
 */
GemuPointerState gemu_display_get_pointer(const GemuDisplay *d);

/* ── Flags ───────────────────────────────────────────────────────────────── */

bool gemu_display_should_quit(const GemuDisplay *d);
bool gemu_display_reset_requested(const GemuDisplay *d);

/*
 * Clear quit/reset flags after acting on them.  Call at the start of the
 * machine's reset handling so a re-triggered reset during reset is not lost.
 */
void gemu_display_clear_flags(GemuDisplay *d);

/* ── Raw key queue (VP-601 / full keyboard mode) ─────────────────────────── */

/*
 * Returns the next Unicode codepoint from the raw key queue, or 0 if empty.
 * Call in a loop to drain all keys pressed since the last poll().
 * Backends produce UTF-32 values: printable characters are their Unicode
 * codepoint; common specials map as follows:
 *   Return → '\r'   Backspace → '\b'   Escape → 0x1b
 *   Tab    → '\t'   Delete    → 0x7f
 * Arrow and function keys are not enqueued (use the action bitmask instead).
 * Only populated for machines that request raw key input; always empty
 * for machines that use the action-bitmask model exclusively.
 */
uint32_t gemu_display_pop_raw_key(GemuDisplay *d);

/* ── Key state query (full-keyboard machines) ───────────────────────────── */

/*
 * Returns true if the named key is currently held down.  The key_name uses
 * the same SDL key-name vocabulary as GemuActionDef.default_key ("a", "Return",
 * "Left Shift", "Home", "Escape", …).
 *
 * SDL backend: queries SDL_GetKeyboardState() — always accurate after poll().
 * Other backends: return false (they don't support arbitrary key queries).
 *
 * Intended for machines with large keyboards that don't fit in 32 action bits
 * (e.g. Pecom 32).  Machines using the standard action-bitmask model do not
 * need this.
 */
bool gemu_display_is_key_held(GemuDisplay *d, const char *key_name);

/* ── Key-rebinding overlay (SDL only) ────────────────────────────────────── */

/*
 * Opens the in-game key-rebinding overlay (wraps the existing InputMenu).
 * No-op on non-SDL backends.  On return, gemu.ini has been written.
 * The display's key map is reloaded from the updated INI automatically.
 */
void gemu_display_open_rebind_menu(GemuDisplay *d);
void gemu_display_reset_input_bindings(GemuDisplay *d);
void gemu_display_reset_input_bindings_ud(void *d);
