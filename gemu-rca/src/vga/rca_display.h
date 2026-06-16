#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include "gemu/display.h"
#include "gemu/monitor.h"
#include "rca.h"

typedef struct RcaDisplay RcaDisplay;

enum {
    RCA_KEY_ESCAPE = 0x10000,
    RCA_KEY_TAB,
    RCA_KEY_F2,
    RCA_KEY_F3,
    RCA_KEY_LEFT,
    RCA_KEY_RIGHT,
    RCA_KEY_LALT,
    RCA_KEY_KP_0,
    RCA_KEY_KP_1,
    RCA_KEY_KP_2,
    RCA_KEY_KP_3,
    RCA_KEY_KP_4,
    RCA_KEY_KP_5,
    RCA_KEY_KP_6,
    RCA_KEY_KP_7,
    RCA_KEY_KP_8,
    RCA_KEY_KP_9,
};

struct RcaDisplay {
    void (*render)(void *ctx, const uint8_t *vram, int w, int h);
    void (*destroy)(void *ctx);
    void (*run)(RcaDisplay *d, void *machine, const void *cfg);
    void (*poll)(void *ctx);
    bool (*should_quit)(void *ctx);
    bool (*key_down)(void *ctx, uint32_t keysym);
    uint32_t (*pop_keysym)(void *ctx);
    bool (*menu_reset_requested)(void *ctx);   /* SDL input menu */
    void (*menu_clear_reset)(void *ctx);       /* SDL input menu */
    void  *ctx;
    RcaKeyboardType keyboard_type;
};

RcaDisplay *rca_display_sdl_create(int scale);
RcaDisplay *rca_display_sdl_create_mono(const char *title, int w, int h,
                                        int scale, uint32_t on,
                                        uint32_t off,
                                        RcaKeyboardType keyboard_type);
RcaDisplay *rca_display_sdl_create_indexed(const char *title, int w, int h,
                                           int scale,
                                           const uint32_t *palette,
                                           int n_colors,
                                           RcaKeyboardType keyboard_type);
#ifdef GEMU_GTK
RcaDisplay *rca_display_gtk_create_mono(const char *title, int w, int h,
                                        int scale, uint32_t on,
                                        uint32_t off, GemuMonitor *mon,
                                        RcaKeyboardType keyboard_type);
RcaDisplay *rca_display_gtk_create_indexed(const char *title, int w, int h,
                                           int scale,
                                           const uint32_t *palette,
                                           int n_colors, GemuMonitor *mon,
                                           RcaKeyboardType keyboard_type);
#endif
#ifndef GEMU_NO_CURSES
RcaDisplay *rca_display_curses_create(void);
#endif
RcaDisplay *rca_display_none_create(void);
RcaDisplay *rca_display_create_mono(GemuDisplayType type, const char *title,
                                    int w, int h, int scale,
                                    uint32_t on, uint32_t off,
                                    GemuMonitor *mon,
                                    RcaKeyboardType keyboard_type);
RcaDisplay *rca_display_create_indexed(GemuDisplayType type, const char *title,
                                       int w, int h, int scale,
                                       const uint32_t *palette, int n_colors,
                                       GemuMonitor *mon,
                                       RcaKeyboardType keyboard_type);

static inline void rca_display_render(RcaDisplay *d, const uint8_t *vram,
                                      int w, int h) {
    d->render(d->ctx, vram, w, h);
}

static inline void rca_display_destroy(RcaDisplay *d) {
    if (!d) return;
    d->destroy(d->ctx);
    free(d);
}

static inline void rca_display_poll(RcaDisplay *d) {
    if (d && d->poll) d->poll(d->ctx);
}

static inline bool rca_display_should_quit(RcaDisplay *d) {
    return d && d->should_quit && d->should_quit(d->ctx);
}

static inline bool rca_display_key_down(RcaDisplay *d, uint32_t keysym) {
    return d && d->key_down && d->key_down(d->ctx, keysym);
}

static inline uint32_t rca_display_pop_keysym(RcaDisplay *d) {
    return (d && d->pop_keysym) ? d->pop_keysym(d->ctx) : 0;
}

static inline bool rca_display_menu_reset_requested(RcaDisplay *d) {
    return d && d->menu_reset_requested && d->menu_reset_requested(d->ctx);
}

static inline void rca_display_menu_clear_reset(RcaDisplay *d) {
    if (d && d->menu_clear_reset) d->menu_clear_reset(d->ctx);
}
