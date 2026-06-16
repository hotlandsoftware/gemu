#include "nes_display.h"
#include "../hardware/nes.h"
#include "gemu/video.h"
#include <SDL2/SDL.h>
#include <stdlib.h>

#define NES_SDL_DEFAULT_SCALE 2

typedef struct {
    GemuVideoSdl   *video;
    bool            quit;
    uint8_t         ctrl1;
    int             zapper_x, zapper_y;
    bool            zapper_btn;
} NesDisplaySdlCtx;

static uint8_t sdl_key_to_btn(SDL_Keycode key) {
    switch (key) {
    case SDLK_z:           return NES_BTN_A;
    case SDLK_x:           return NES_BTN_B;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:    return NES_BTN_START;
    case SDLK_RSHIFT:      return NES_BTN_SELECT;
    case SDLK_UP:          return NES_BTN_UP;
    case SDLK_DOWN:        return NES_BTN_DOWN;
    case SDLK_LEFT:        return NES_BTN_LEFT;
    case SDLK_RIGHT:       return NES_BTN_RIGHT;
    default:               return 0;
    }
}

static void sdl_render(void *vctx, const uint8_t *pixels, int w, int h) {
    NesDisplaySdlCtx *c = vctx;
    gemu_video_sdl_present_indexed(c->video, pixels, w, h);
}

static void sdl_render_argb(void *vctx, const uint32_t *pixels, int w, int h) {
    NesDisplaySdlCtx *c = vctx;
    gemu_video_sdl_present_argb(c->video, pixels, w, h);
}

static void sdl_poll(void *vctx) {
    NesDisplaySdlCtx *c = vctx;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
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
        if (ev.type == SDL_KEYDOWN && key == SDLK_ESCAPE) { c->quit = true; continue; }
        uint8_t btn = sdl_key_to_btn(key);
        if (!btn) continue;
        if (ev.type == SDL_KEYDOWN) c->ctrl1 |=  btn;
        else                        c->ctrl1 &= ~btn;
    }
}

static void sdl_zapper(void *vctx, int *x, int *y, bool *trigger) {
    NesDisplaySdlCtx *c = vctx;
    if (x)       *x       = c->zapper_x;
    if (y)       *y       = c->zapper_y;
    if (trigger) *trigger = c->zapper_btn;
}

static void sdl_destroy(void *vctx) {
    NesDisplaySdlCtx *c = vctx;
    if (!c) return;
    gemu_video_sdl_destroy(c->video);
    free(c);
}

static bool    sdl_should_quit(void *vctx) { return ((NesDisplaySdlCtx *)vctx)->quit; }
static uint8_t sdl_ctrl1(void *vctx)       { return ((NesDisplaySdlCtx *)vctx)->ctrl1; }

NesDisplay *nes_display_sdl_create(const char *title,
                                   const uint32_t *palette, int scale,
                                   GemuRendererType renderer_mode) {
    if (scale <= 0) scale = NES_SDL_DEFAULT_SCALE;
    NesDisplaySdlCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->video = gemu_video_sdl_create(&(GemuVideoSdlSpec){
        .title         = title ? title : "gemu-6502 NES",
        .width         = 256,
        .height        = 240,
        .window_width  = 320 * scale,
        .window_height = 240 * scale,
        .palette       = palette,
        .n_colors      = 64,
        .renderer      = renderer_mode,
        .log_prefix    = "nes",
    });
    if (!c->video) goto fail;

    NesDisplay *d = calloc(1, sizeof(*d));
    if (!d) goto fail;
    d->render      = sdl_render;
    d->render_argb = sdl_render_argb;
    d->poll        = sdl_poll;
    d->destroy     = sdl_destroy;
    d->should_quit = sdl_should_quit;
    d->ctrl1       = sdl_ctrl1;
    d->zapper      = sdl_zapper;
    d->ctx         = c;
    return d;

fail:
    sdl_destroy(c);
    return NULL;
}

NesDisplay *nes_display_create(GemuDisplayType type, const char *title,
                               const uint32_t *palette, int scale,
                               GemuRendererType renderer, GemuMonitor *mon,
                               void (*hex_toggle_cb)(void *),
                               void *hex_toggle_ud) {
    switch (type) {
    case GEMU_DISPLAY_SDL:
        (void)mon; (void)hex_toggle_cb; (void)hex_toggle_ud;
        return nes_display_sdl_create(title, palette, scale, renderer);
#ifdef GEMU_GTK
    case GEMU_DISPLAY_GTK:
        (void)renderer;
        return nes_display_gtk_create(title, palette, scale, mon,
                                      hex_toggle_cb, hex_toggle_ud);
#endif
    default:
        return NULL;
    }
}
