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

    /* Backend vtable */
    void     (*do_render)     (struct GemuDisplay *, const uint32_t *, int, int);
    uint32_t (*do_poll)       (struct GemuDisplay *);  /* returns new held mask */
    bool     (*do_is_key_held)(struct GemuDisplay *, const char *);
    void     (*do_open_rebind)(struct GemuDisplay *);
    void     (*do_reset_input_bindings)(struct GemuDisplay *);
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
