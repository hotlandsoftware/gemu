#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct GemuVncServer GemuVncServer;

typedef struct {
    uint32_t keysym;
    bool     down;
} GemuVncKeyEvent;

typedef struct {
    int  x, y;
    int  rel_x, rel_y;
    bool button;
    bool pressed;
    bool right_button;
    bool right_pressed;
} GemuVncPointerState;

/* addr: "host:display" or ":display"  (port = 5900 + display)
 * fb_w/fb_h: framebuffer dimensions in pixels                */
GemuVncServer *gemu_vnc_create(const char *addr, int fb_w, int fb_h);
void           gemu_vnc_destroy(GemuVncServer *vnc);
void           gemu_vnc_set_password(GemuVncServer *vnc, const char *password);
void           gemu_vnc_set_colors(GemuVncServer *vnc,
                                   uint32_t fg_rgb, uint32_t bg_rgb);
void           gemu_vnc_set_palette(GemuVncServer *vnc,
                                    const uint32_t *palette, int n_colors);

/* Called whenever the guest's display changes.
 * vram: 1 byte/pixel. Without a palette: 0=background, non-zero=foreground.
 * With a palette: byte values index the configured RGB table. */
void           gemu_vnc_update(GemuVncServer *vnc,
                               const uint8_t *vram, int vw, int vh);

/* Copy the current VNC key state into keys[16] (CHIP-8 layout).
 * Safe to call from any thread.                               */
void           gemu_vnc_get_keys(GemuVncServer *vnc, uint8_t keys[16]);

/* Return one queued RFB keysym, or 0 if no key event is waiting. */
uint32_t       gemu_vnc_pop_keysym(GemuVncServer *vnc);

/* Return one queued RFB key press/release event. */
bool           gemu_vnc_pop_key_event(GemuVncServer *vnc,
                                      GemuVncKeyEvent *event);

/* Return current pointer state and clear one-frame relative/pressed fields. */
GemuVncPointerState gemu_vnc_get_pointer(GemuVncServer *vnc);
