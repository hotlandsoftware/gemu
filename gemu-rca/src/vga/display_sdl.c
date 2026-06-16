#include "rca_display.h"
#include "cdp1802.h"
#include "input_menu.h"
#include "gemu/video.h"
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdlib.h>

#define VIP_DEFAULT_SCALE 4
#define RCA_SDL_MAX_KEYS 128

typedef struct {
    GemuVideoSdl   *video;
    InputMenu      *menu;
    bool            indexed;
    uint32_t        pixel_on;
    uint32_t        pixel_off;
    bool            quit;
    bool            menu_reset_requested;
    struct {
        uint32_t keysym;
        bool     down;
    } keys[RCA_SDL_MAX_KEYS];
    int             n_keys;
    uint32_t        queued_keysym;
    RcaKeyboardType keyboard_type;
} VipSdlCtx;

/* Match Emma02's COSMAC VIP presentation: white pixels on deep blue. */
#define PIXEL_ON  0xFFFFFFFFu
#define PIXEL_OFF 0xFF100080u

/* ── Overlay callback for the input menu ────────────────────────────────── */

static void menu_overlay_cb(void *ud, SDL_Renderer *r) {
    input_menu_render((InputMenu *)ud, r);
}

static void sdl_render(void *ctx, const uint8_t *vram, int w, int h) {
    VipSdlCtx *c = ctx;
    if (c->indexed)
        gemu_video_sdl_present_indexed(c->video, vram, w, h);
    else
        gemu_video_sdl_present_mono(c->video, vram, w, h,
                                    c->pixel_on, c->pixel_off);
}

static void sdl_destroy(void *ctx) {
    VipSdlCtx *c = ctx;
    input_menu_destroy(c->menu);
    gemu_video_sdl_destroy(c->video);
    free(c);
}

static uint32_t sdl_key_to_rca(SDL_Keycode key) {
    if (key >= 0x20 && key <= 0x7e)
        return (uint32_t)key;
    switch (key) {
    case SDLK_ESCAPE: return RCA_KEY_ESCAPE;
    case SDLK_TAB:    return RCA_KEY_TAB;
    case SDLK_F2:     return RCA_KEY_F2;
    case SDLK_F3:     return RCA_KEY_F3;
    case SDLK_LEFT:   return RCA_KEY_LEFT;
    case SDLK_RIGHT:  return RCA_KEY_RIGHT;
    case SDLK_LALT:   return RCA_KEY_LALT;
    case SDLK_KP_0:   return RCA_KEY_KP_0;
    case SDLK_KP_1:   return RCA_KEY_KP_1;
    case SDLK_KP_2:   return RCA_KEY_KP_2;
    case SDLK_KP_3:   return RCA_KEY_KP_3;
    case SDLK_KP_4:   return RCA_KEY_KP_4;
    case SDLK_KP_5:   return RCA_KEY_KP_5;
    case SDLK_KP_6:   return RCA_KEY_KP_6;
    case SDLK_KP_7:   return RCA_KEY_KP_7;
    case SDLK_KP_8:   return RCA_KEY_KP_8;
    case SDLK_KP_9:   return RCA_KEY_KP_9;
    case SDLK_RETURN: return '\r';
    case SDLK_BACKSPACE: return '\b';
    default:          return 0;
    }
}

static int find_key(VipSdlCtx *c, uint32_t keysym) {
    for (int i = 0; i < c->n_keys; i++)
        if (c->keys[i].keysym == keysym)
            return i;
    return -1;
}

static void set_key(VipSdlCtx *c, uint32_t keysym, bool down) {
    if (keysym >= 'A' && keysym <= 'Z')
        keysym += 'a' - 'A';
    int idx = find_key(c, keysym);
    if (idx < 0) {
        if (c->n_keys >= RCA_SDL_MAX_KEYS)
            return;
        idx = c->n_keys++;
        c->keys[idx].keysym = keysym;
    }
    c->keys[idx].down = down;
    if (down)
        c->queued_keysym = keysym;
}

static void sdl_poll(void *ctx) {
    VipSdlCtx *c = ctx;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        /* Menu consumes events when open */
        if (input_menu_is_open(c->menu)) {
            input_menu_handle_event(c->menu, &ev);
            continue;
        }

        if (ev.type == SDL_QUIT) {
            c->quit = true;
            continue;
        }
        if (ev.type == SDL_TEXTINPUT) {
            unsigned char ch = (unsigned char)ev.text.text[0];
            if (ch >= 0x20 && ch <= 0x7e)
                c->queued_keysym = ch;
            continue;
        }
        if (ev.type != SDL_KEYDOWN && ev.type != SDL_KEYUP)
            continue;

        bool down = ev.type == SDL_KEYDOWN;
        SDL_Keycode key = ev.key.keysym.sym;

        /* Tab toggles menu (skip when a keyboard device is attached) */
        if (down && key == SDLK_TAB) {
            if (c->keyboard_type == RCA_KEYBOARD_NONE)
                input_menu_toggle(c->menu);
            continue;
        }

        uint32_t sym = sdl_key_to_rca(key);
        if (!sym)
            continue;
        if (down && sym == RCA_KEY_ESCAPE)
            c->quit = true;
        set_key(c, sym, down);
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

static bool sdl_should_quit(void *ctx) {
    return ((VipSdlCtx *)ctx)->quit;
}

static bool sdl_menu_reset_requested(void *ctx) {
    return ((VipSdlCtx *)ctx)->menu_reset_requested;
}

static void sdl_menu_clear_reset(void *ctx) {
    ((VipSdlCtx *)ctx)->menu_reset_requested = false;
}

static bool sdl_key_down(void *ctx, uint32_t keysym) {
    VipSdlCtx *c = ctx;
    if (keysym >= 'A' && keysym <= 'Z')
        keysym += 'a' - 'A';
    int idx = find_key(c, keysym);
    return idx >= 0 && c->keys[idx].down;
}

static uint32_t sdl_pop_keysym(void *ctx) {
    VipSdlCtx *c = ctx;
    uint32_t keysym = c->queued_keysym;
    c->queued_keysym = 0;
    return keysym;
}

RcaDisplay *rca_display_sdl_create(int scale) {
    return rca_display_sdl_create_mono("GEMU", CDP1861_DISPLAY_W,
                                       CDP1861_DISPLAY_H, scale,
                                       PIXEL_ON, PIXEL_OFF,
                                       RCA_KEYBOARD_NONE);
}

static RcaDisplay *sdl_create_common(const char *title, int w, int h,
                                     int scale, const uint32_t *palette,
                                     int n_colors, uint32_t on,
                                     uint32_t off,
                                     RcaKeyboardType keyboard_type) {
    if (scale <= 0) scale = VIP_DEFAULT_SCALE;

    VipSdlCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->indexed = palette && n_colors > 0;
    c->pixel_on = on;
    c->pixel_off = off;
    c->keyboard_type = keyboard_type;
    c->video = gemu_video_sdl_create(&(GemuVideoSdlSpec){
        .title         = title ? title : "GEMU",
        .width         = w,
        .height        = h,
        .window_width  = w * scale,
        .window_height = h * scale,
        .palette       = palette,
        .n_colors      = n_colors,
        .renderer      = GEMU_RENDERER_AUTO,
        .log_prefix    = "rca",
    });
    if (!c->video) { free(c); return NULL; }

    RcaDisplay *d = calloc(1, sizeof(*d));
    d->render = sdl_render;
    d->destroy = sdl_destroy;
    d->run = NULL;
    d->poll = sdl_poll;
    d->should_quit = sdl_should_quit;
    d->key_down = sdl_key_down;
    d->pop_keysym = sdl_pop_keysym;
    d->menu_reset_requested = sdl_menu_reset_requested;
    d->menu_clear_reset     = sdl_menu_clear_reset;
    d->ctx = c;
    d->keyboard_type = keyboard_type;

    /* Create input menu — no remappable buttons (keyboard devices have none).
     * The overlay is set once and render is a no-op when closed. */
    c->menu = input_menu_create(NULL, 0, NULL, NULL, "rca-input");
    gemu_video_sdl_set_overlay(c->video, menu_overlay_cb, c->menu);

    return d;
}

RcaDisplay *rca_display_sdl_create_mono(const char *title, int w, int h,
                                        int scale, uint32_t on,
                                        uint32_t off,
                                        RcaKeyboardType keyboard_type) {
    return sdl_create_common(title, w, h, scale, NULL, 0, on, off,
                             keyboard_type);
}

RcaDisplay *rca_display_sdl_create_indexed(const char *title, int w, int h,
                                           int scale,
                                           const uint32_t *palette,
                                           int n_colors,
                                           RcaKeyboardType keyboard_type) {
    return sdl_create_common(title, w, h, scale, palette, n_colors,
                             PIXEL_ON, PIXEL_OFF, keyboard_type);
}

RcaDisplay *rca_display_create_mono(GemuDisplayType type, const char *title,
                                    int w, int h, int scale,
                                    uint32_t on, uint32_t off,
                                    GemuMonitor *mon,
                                    RcaKeyboardType keyboard_type) {
    switch (type) {
    case GEMU_DISPLAY_SDL:
        (void)mon;
        return rca_display_sdl_create_mono(title, w, h, scale, on, off,
                                           keyboard_type);
#ifdef GEMU_GTK
    case GEMU_DISPLAY_GTK:
        return rca_display_gtk_create_mono(title, w, h, scale, on, off, mon,
                                           keyboard_type);
#endif
#ifndef GEMU_NO_CURSES
    case GEMU_DISPLAY_CURSES:
        return rca_display_curses_create();
#endif
    default:
        return rca_display_none_create();
    }
}

RcaDisplay *rca_display_create_indexed(GemuDisplayType type, const char *title,
                                       int w, int h, int scale,
                                       const uint32_t *palette, int n_colors,
                                       GemuMonitor *mon,
                                       RcaKeyboardType keyboard_type) {
    switch (type) {
    case GEMU_DISPLAY_SDL:
        (void)mon;
        return rca_display_sdl_create_indexed(title, w, h, scale,
                                              palette, n_colors,
                                              keyboard_type);
#ifdef GEMU_GTK
    case GEMU_DISPLAY_GTK:
        return rca_display_gtk_create_indexed(title, w, h, scale,
                                              palette, n_colors, mon,
                                              keyboard_type);
#endif
#ifndef GEMU_NO_CURSES
    case GEMU_DISPLAY_CURSES:
        return rca_display_curses_create();
#endif
    default:
        return rca_display_none_create();
    }
}

static void none_render(void *ctx, const uint8_t *v, int w, int h) {
    (void)ctx; (void)v; (void)w; (void)h;
}
static void none_destroy(void *ctx) { free(ctx); }

RcaDisplay *rca_display_none_create(void) {
    RcaDisplay *d = calloc(1, sizeof(*d));
    d->render  = none_render;
    d->destroy = none_destroy;
    d->run     = NULL;
    d->ctx     = calloc(1, 1);
    return d;
}
