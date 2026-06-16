#include "nes_display.h"
#include "../hardware/nes.h"
#include "../hardware/nes_devices.h"
#include "input_menu.h"
#include "gemu/video.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

#define NES_SDL_DEFAULT_SCALE 2

/* NES controller button names and default key bindings (referenced by
 * the device descriptor in nes_devices.c via extern). */
const char *nes_ctrl_names[] = {
    "A", "B", "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
};
const SDL_Keycode nes_ctrl_defaults[] = {
    SDLK_z, SDLK_x, SDLK_RETURN, SDLK_RSHIFT,
    SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
};

/* NES_BTN_* bitmask per button index (same order as nes_ctrl_names) */
static const uint8_t nes_btn_masks[] = {
    NES_BTN_A, NES_BTN_B, NES_BTN_START, NES_BTN_SELECT,
    NES_BTN_UP, NES_BTN_DOWN, NES_BTN_LEFT, NES_BTN_RIGHT,
};

typedef struct {
    GemuVideoSdl   *video;
    InputMenu      *menu;
    bool            quit;
    bool            menu_reset_requested;
    uint8_t         ctrl1;
    int             zapper_x, zapper_y;
    bool            zapper_btn;
    NesDeviceType   port_devices[NES_PORTS];
} NesDisplaySdlCtx;

/* ── Overlay callback for the input menu ────────────────────────────────── */

static void menu_overlay_cb(void *ud, SDL_Renderer *r) {
    input_menu_render((InputMenu *)ud, r);
}

/* ── Render ──────────────────────────────────────────────────────────────── */

static void sdl_render(void *vctx, const uint8_t *pixels, int w, int h) {
    NesDisplaySdlCtx *c = vctx;
    gemu_video_sdl_present_indexed(c->video, pixels, w, h);
}

static void sdl_render_argb(void *vctx, const uint32_t *pixels, int w, int h) {
    NesDisplaySdlCtx *c = vctx;
    gemu_video_sdl_present_argb(c->video, pixels, w, h);
}

/* ── Poll ────────────────────────────────────────────────────────────────── */

static void sdl_poll(void *vctx) {
    NesDisplaySdlCtx *c = vctx;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        /* Menu consumes events when open */
        if (input_menu_is_open(c->menu)) {
            input_menu_handle_event(c->menu, &ev);
            continue;
        }

        if (ev.type == SDL_QUIT) { c->quit = true; continue; }
        if (ev.type == SDL_MOUSEMOTION ||
            ev.type == SDL_MOUSEBUTTONDOWN ||
            ev.type == SDL_MOUSEBUTTONUP) {
            gemu_video_sdl_mouse_logical(c->video, &c->zapper_x, &c->zapper_y);
            if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT)
                c->zapper_btn = true;
            if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT)
                c->zapper_btn = false;
            continue;
        }
        if (ev.type != SDL_KEYDOWN && ev.type != SDL_KEYUP) continue;

        SDL_Keycode key = ev.key.keysym.sym;

        /* Tab toggles the input menu (skip if keyboard device on port 0) */
        if (ev.type == SDL_KEYDOWN && key == SDLK_TAB) {
            if (c->port_devices[0] != NES_DEVICE_KEYBOARD)
                input_menu_toggle(c->menu);
            continue;
        }

        if (ev.type == SDL_KEYDOWN && key == SDLK_ESCAPE) { c->quit = true; continue; }
        uint8_t btn = 0;
        int bidx = input_menu_key_to_btn_index(c->menu, key);
        if (bidx >= 0 && bidx < (int)(sizeof(nes_btn_masks) / sizeof(nes_btn_masks[0])))
            btn = nes_btn_masks[bidx];
        if (!btn) continue;
        if (ev.type == SDL_KEYDOWN) c->ctrl1 |=  btn;
        else                        c->ctrl1 &= ~btn;
    }

    /* Check menu actions after poll */
    if (input_menu_quit_requested(c->menu)) {
        c->quit = true;
        input_menu_clear_actions(c->menu);
    }
    if (input_menu_reset_requested(c->menu)) {
        c->menu_reset_requested = true;
        input_menu_clear_actions(c->menu);
    }
}

/* ── Zapper ──────────────────────────────────────────────────────────────── */

static void sdl_zapper(void *vctx, int *x, int *y, bool *trigger) {
    NesDisplaySdlCtx *c = vctx;
    if (x)       *x       = c->zapper_x;
    if (y)       *y       = c->zapper_y;
    if (trigger) *trigger = c->zapper_btn;
}

/* ── Destroy ─────────────────────────────────────────────────────────────── */

static void sdl_destroy(void *vctx) {
    NesDisplaySdlCtx *c = vctx;
    if (!c) return;
    input_menu_destroy(c->menu);
    gemu_video_sdl_destroy(c->video);
    free(c);
}

/* ── Accessors ───────────────────────────────────────────────────────────── */

static bool    sdl_should_quit(void *vctx)        { return ((NesDisplaySdlCtx *)vctx)->quit; }
static uint8_t sdl_ctrl1(void *vctx)              { return ((NesDisplaySdlCtx *)vctx)->ctrl1; }
static bool    sdl_menu_reset_requested(void *vctx) { return ((NesDisplaySdlCtx *)vctx)->menu_reset_requested; }
static void    sdl_menu_clear_reset(void *vctx)     { ((NesDisplaySdlCtx *)vctx)->menu_reset_requested = false; }

/* ── Create ──────────────────────────────────────────────────────────────── */

NesDisplay *nes_display_sdl_create(const char *title,
                                   const uint32_t *palette, int scale,
                                   GemuRendererType renderer,
                                   const NesDeviceType *port_devices) {
    if (scale <= 0) scale = NES_SDL_DEFAULT_SCALE;
    NesDisplaySdlCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (port_devices)
        memcpy(c->port_devices, port_devices, sizeof(c->port_devices));
    c->video = gemu_video_sdl_create(&(GemuVideoSdlSpec){
        .title         = title ? title : "gemu-6502 NES",
        .width         = 256,
        .height        = 240,
        .window_width  = 320 * scale,
        .window_height = 240 * scale,
        .palette       = palette,
        .n_colors      = 64,
        .renderer      = renderer,
        .log_prefix    = "nes",
    });
    if (!c->video) goto fail;

    /* Create input menu using device descriptor button info. */
    const NesDeviceDesc *dev = nes_device_find("nes-controller");
    if (dev && dev->n_buttons > 0) {
        c->menu = input_menu_create(NULL,
                                    dev->n_buttons,
                                    dev->button_names,
                                    dev->default_bindings,
                                    dev->ini_section);
        gemu_video_sdl_set_overlay(c->video, menu_overlay_cb, c->menu);
    }

    NesDisplay *d = calloc(1, sizeof(*d));
    if (!d) goto fail;
    d->render               = sdl_render;
    d->render_argb          = sdl_render_argb;
    d->poll                 = sdl_poll;
    d->destroy              = sdl_destroy;
    d->should_quit          = sdl_should_quit;
    d->ctrl1                = sdl_ctrl1;
    d->zapper               = sdl_zapper;
    d->menu_reset_requested = sdl_menu_reset_requested;
    d->menu_clear_reset     = sdl_menu_clear_reset;
    d->ctx                  = c;
    if (port_devices)
        memcpy(d->port_devices, port_devices, sizeof(d->port_devices));
    return d;

fail:
    sdl_destroy(c);
    return NULL;
}

NesDisplay *nes_display_create(GemuDisplayType type, const char *title,
                               const uint32_t *palette, int scale,
                               GemuRendererType renderer, GemuMonitor *mon,
                               void (*hex_toggle_cb)(void *),
                               void *hex_toggle_ud,
                               const NesDeviceType *port_devices) {
    switch (type) {
    case GEMU_DISPLAY_SDL:
        (void)mon; (void)hex_toggle_cb; (void)hex_toggle_ud;
        return nes_display_sdl_create(title, palette, scale, renderer,
                                      port_devices);
#ifdef GEMU_GTK
    case GEMU_DISPLAY_GTK:
        (void)renderer;
        return nes_display_gtk_create(title, palette, scale, mon,
                                      hex_toggle_cb, hex_toggle_ud,
                                      port_devices);
#endif
    default:
        return NULL;
    }
}
