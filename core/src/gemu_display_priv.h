#pragma once
#include "gemu/gemu_display.h"
#include <string.h>

#define GEMU_DISPLAY_MAX_ACTIONS 32
#define GEMU_DISPLAY_RAW_QUEUE   16

struct GemuDisplay {
    /* Per-frame input state (written by do_poll, read by public API) */
    uint32_t held;          /* currently-held action bits                    */
    uint32_t held_prev;     /* held bits at start of last poll()             */
    uint32_t last_pressed;  /* bits that went up→down this frame             */

    /* Raw key FIFO (VP-601 / full-keyboard mode) */
    uint32_t raw_queue[GEMU_DISPLAY_RAW_QUEUE];
    int      raw_head, raw_tail;

    /* Pointer (light gun / mouse) */
    GemuPointerState pointer;

    /* Lifecycle flags */
    bool quit;
    bool reset;
    bool paused_title;

    /* Monitors (gemu_display_set_heads); n_heads 0 = just the one */
    GemuDisplayHead heads[GEMU_DISPLAY_MAX_HEADS];
    char            head_names[GEMU_DISPLAY_MAX_HEADS][48];
    bool            head_detached[GEMU_DISPLAY_MAX_HEADS];  /* in a window of its own */
    int             n_heads, head;
    char title[128];

    /* Window menus (gemu_display_menu.c) */
    GemuMonitor *monitor;                   /* for Pause, Reset, Blocks... */
    void       (*hex_toggle_cb)(void *ud);  /* Debug > Hex Editor, if any */
    void        *hex_toggle_ud;
    struct { char label[48]; void (*activate)(void *ud); void *ud; }
                 debug_items[GEMU_DISPLAY_MAX_DEBUG_ITEMS];
    int          n_debug_items;
    /* UI a backend can offer the menus (NULL = not available there) */
    int  (*ui_get_scale)(struct GemuDisplay *);
    void (*ui_set_scale)(struct GemuDisplay *, int scale);
    void (*ui_choose_file)(struct GemuDisplay *, const char *title,
                           void (*done)(void *ud, const char *path), void *ud);
    void (*ui_about)(struct GemuDisplay *);
    /* Input devices (binding pages): count, menu label, open one */
    int  (*ui_input_pages)(struct GemuDisplay *);
    void (*ui_input_page_label)(struct GemuDisplay *, int page, char *buf, size_t len);
    void (*ui_input_config)(struct GemuDisplay *, int page);

    /* VNC mirroring/input (gemu_display_vnc.c); NULL without -vnc */
    struct GemuDisplayVnc *vnc;
    bool headless;          /* no local output: -display none with -vnc */

    /* Backend vtable */
    void     (*do_render)     (struct GemuDisplay *, const uint32_t *, int, int);
    void     (*do_render_text)(struct GemuDisplay *, const char *, int, int);
    uint32_t (*do_poll)       (struct GemuDisplay *);  /* returns new held mask */
    bool     (*do_is_key_held)(struct GemuDisplay *, const char *);
    void     (*do_reset_input_bindings)(struct GemuDisplay *);
    void     (*do_set_title)   (struct GemuDisplay *, const char *);
    /* New framebuffer size/shape (switching monitors); NULL = takes any size */
    void     (*do_set_geometry)(struct GemuDisplay *, int w, int h, int aspect_w, int aspect_h);
    /* Detached monitor windows (NULL = backend can't): open one for a head,
     * draw a frame into it, bring it to the front.  When the user closes it
     * the backend calls gemu_display_head_closed(). */
    bool     (*do_detach_head) (struct GemuDisplay *, int head);
    void     (*do_present_head)(struct GemuDisplay *, int head, const uint32_t *argb, int w, int h);
    void     (*do_raise_head)  (struct GemuDisplay *, int head);
    void     (*do_close_head)  (struct GemuDisplay *, int head);  /* then head_closed */
    void     (*do_destroy)    (struct GemuDisplay *);
    void      *backend;
};

/* ── Backend constructors (implemented in each gemu_display_*.c) ─────────── */
GemuDisplay *gemu_display_sdl_create (const GemuDisplayConfig *cfg);
#ifdef GEMU_GTK
GemuDisplay *gemu_display_gtk_create (const GemuDisplayConfig *cfg);
#endif
#ifdef HAVE_CACA
GemuDisplay *gemu_display_caca_create(const GemuDisplayConfig *cfg);
#endif
#ifndef _WIN32
GemuDisplay *gemu_display_term_create(const GemuDisplayConfig *cfg);
#endif

/* ── VNC (gemu_display_vnc.c) ────────────────────────────────────────────── */
typedef struct GemuVncServer GemuVncServer;
GemuDisplay *gemu_display_headless_create(const GemuDisplayConfig *cfg);
void     gemu_display_vnc_attach(GemuDisplay *d, GemuVncServer *server,
                                 const GemuDisplayConfig *cfg);
void     gemu_display_vnc_detach(GemuDisplay *d);
void     gemu_display_vnc_frame(GemuDisplay *d, const uint32_t *argb, int w, int h);
uint32_t gemu_display_vnc_poll(GemuDisplay *d);
bool     gemu_display_vnc_key_held(GemuDisplay *d, const char *name);

/* ── Menus (gemu_display_menu.c) ─────────────────────────────────────────── */
typedef struct GemuMenu GemuMenu;
/* The standard menu bar for this window; free with gemu_menu_free(). */
GemuMenu *gemu_display_build_menubar(GemuDisplay *d);
/* A detached monitor's window was closed: the head is back in View. */
void      gemu_display_head_closed(GemuDisplay *d, int head);
/* Text for the About box (multi-line). */
const char *gemu_display_about_text(void);

/* ── INI helper (implemented in gemu_display.c) ──────────────────────────── */

/* Read a single key from a section of ~/.gemu/gemu.ini.
 * Returns true and writes the trimmed value into buf[bufsz] on success. */
bool gemu_ini_read(const char *section, const char *key,
                   char *buf, int bufsz);

/* ── Raw-key FIFO helpers (inline, used by backends) ─────────────────────── */

static inline void gemu_display_push_raw(struct GemuDisplay *d, uint32_t cp) {
    int next = (d->raw_tail + 1) % GEMU_DISPLAY_RAW_QUEUE;
    if (next != d->raw_head) {         /* drop silently when full */
        d->raw_queue[d->raw_tail] = cp;
        d->raw_tail = next;
    }
}
