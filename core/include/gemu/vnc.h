#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * VNC server (RFB 3.3/3.7/3.8), configured like QEMU's -vnc:
 *
 *   -vnc host:N        listen on host (name or address), port 5900+N
 *   -vnc :N            listen on all interfaces
 *   -vnc unix:/path    listen on a Unix domain socket (POSIX)
 *   -vnc none          no server
 *   options (comma-separated, after the address):
 *     password=on      require VNC authentication; clients are refused until a
 *                      password is set from the monitor ("change vnc password"
 *                      or "set_password vnc PASSWORD")
 *     to=L             if port 5900+N is busy, try N+1 .. L
 *     share=allow-exclusive|force-shared|ignore
 *                      what a client asking for exclusive access does
 *                      (default allow-exclusive: it disconnects the others)
 *
 * There is one server per process.  Machines never talk to it: the display
 * layer (gemu_display.h) mirrors every rendered frame to it and merges VNC
 * keyboard/pointer input into its own input state, so -vnc works with every
 * machine, with or without a local window.
 */

typedef struct GemuVncServer GemuVncServer;

typedef struct {
    uint32_t keysym;   /* X11 keysym */
    bool     down;
} GemuVncKeyEvent;

typedef struct {
    int  x, y;         /* framebuffer pixels; -1 before the first event */
    int  rel_x, rel_y; /* motion since the last call */
    bool button, pressed;              /* left: held / went down */
    bool right_button, right_pressed;  /* right: held / went down */
} GemuVncPointerState;

/* Process-wide configuration from -vnc (validated; false on a bad spec). */
bool           gemu_vnc_configure(const char *spec);
bool           gemu_vnc_configured(void);

/* The process's server, started on first use; NULL if -vnc wasn't given or
 * the server couldn't listen. */
GemuVncServer *gemu_vnc_get(void);
void           gemu_vnc_shutdown(void);

/* Publish a frame (0xAARRGGBB).  Clients are sent only what changed; a
 * different size is announced to clients that support desktop resizing. */
void           gemu_vnc_update_argb(GemuVncServer *vnc, const uint32_t *argb,
                                    int w, int h);

bool           gemu_vnc_pop_key_event(GemuVncServer *vnc, GemuVncKeyEvent *ev);
GemuVncPointerState gemu_vnc_get_pointer(GemuVncServer *vnc);

/* Authentication (requires password=on, like QEMU). */
bool           gemu_vnc_set_password(GemuVncServer *vnc, const char *password,
                                     char *err, size_t err_len);
void           gemu_vnc_expire_password(GemuVncServer *vnc);

/* Human-readable status for the monitor's "info vnc". */
void           gemu_vnc_info(GemuVncServer *vnc, char *buf, size_t len);
