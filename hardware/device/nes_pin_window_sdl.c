/*
 * Debug > Cartridge Pins in its own SDL window (see nes_pin_window.h): the
 * shared panel is drawn into a streaming texture once per frame while the
 * window is shown.  Events arrive through an SDL event watch, as in
 * cheat_dialog_sdl.c, so the machine's own event loop needn't know.
 */
#include "nes_pin_window.h"
#include <SDL2/SDL.h>
#include <stdlib.h>

typedef struct {
    NesPinWindow  base;
    NesPinPanel   panel;
    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *tex;
    uint32_t     *fb;
    bool          visible;
    bool          close_requested;   /* hide at the next refresh (see event_watch) */
    int           mx, my;            /* last mouse position, for the wheel */
} SdlPinWindow;

static int SDLCALL event_watch(void *ud, SDL_Event *ev) {
    SdlPinWindow *w = ud;
    Uint32 id = w->win ? SDL_GetWindowID(w->win) : 0, ev_id;
    switch (ev->type) {
    case SDL_WINDOWEVENT:     ev_id = ev->window.windowID; break;
    case SDL_MOUSEMOTION:     ev_id = ev->motion.windowID; break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:   ev_id = ev->button.windowID; break;
    case SDL_MOUSEWHEEL:      ev_id = ev->wheel.windowID;  break;
    default:                  return 1;
    }
    if (!id || ev_id != id) return 1;
    switch (ev->type) {
    case SDL_WINDOWEVENT:
        /* Not hidden here: SDL counts visible windows right after
         * delivering this, and with this one hidden it would think the
         * last window closed and quit the emulator. */
        if (ev->window.event == SDL_WINDOWEVENT_CLOSE) w->close_requested = true;
        if (ev->window.event == SDL_WINDOWEVENT_LEAVE)
            nes_pin_panel_mouse(&w->panel, -1, -1, NES_PIN_MOUSE_LEAVE);
        break;
    case SDL_MOUSEMOTION:
        w->mx = ev->motion.x; w->my = ev->motion.y;
        nes_pin_panel_mouse(&w->panel, w->mx, w->my, NES_PIN_MOUSE_MOVE);
        break;
    case SDL_MOUSEBUTTONDOWN:
        w->mx = ev->button.x; w->my = ev->button.y;
        if (ev->button.button == SDL_BUTTON_LEFT)
            nes_pin_panel_mouse(&w->panel, w->mx, w->my, NES_PIN_MOUSE_LEFT);
        else if (ev->button.button == SDL_BUTTON_RIGHT)
            nes_pin_panel_mouse(&w->panel, w->mx, w->my, NES_PIN_MOUSE_RIGHT);
        break;
    case SDL_MOUSEBUTTONUP:
        nes_pin_panel_mouse(&w->panel, ev->button.x, ev->button.y, NES_PIN_MOUSE_UP);
        break;
    case SDL_MOUSEWHEEL:
        nes_pin_panel_wheel(&w->panel, w->mx, w->my,
                            ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -ev->wheel.y : ev->wheel.y);
        break;
    }
    return 1;
}

static void sdl_show(NesPinWindow *base) {
    SdlPinWindow *w = (SdlPinWindow *)base;
    SDL_ShowWindow(w->win);
    SDL_SetWindowSize(w->win, NES_PIN_PANEL_W, NES_PIN_PANEL_H);   /* see cheat_dialog_sdl.c */
    SDL_RaiseWindow(w->win);
    w->visible = true;
}

static void sdl_hide(NesPinWindow *base) {
    SdlPinWindow *w = (SdlPinWindow *)base;
    SDL_HideWindow(w->win);
    w->visible = false;
}

static bool sdl_visible(const NesPinWindow *base) {
    return ((const SdlPinWindow *)base)->visible;
}

static void sdl_refresh(NesPinWindow *base) {
    SdlPinWindow *w = (SdlPinWindow *)base;
    if (w->close_requested) {
        w->close_requested = false;
        sdl_hide(base);
    }
    if (!w->visible) return;
    nes_pin_panel_draw(&w->panel, w->fb, SDL_GetTicks());
    SDL_UpdateTexture(w->tex, NULL, w->fb, NES_PIN_PANEL_W * (int)sizeof(uint32_t));
    SDL_RenderClear(w->ren);
    SDL_RenderCopy(w->ren, w->tex, NULL, NULL);
    SDL_RenderPresent(w->ren);
}

static void sdl_destroy(NesPinWindow *base) {
    SdlPinWindow *w = (SdlPinWindow *)base;
    if (!w) return;
    SDL_DelEventWatch(event_watch, w);
    if (w->tex) SDL_DestroyTexture(w->tex);
    if (w->ren) SDL_DestroyRenderer(w->ren);
    if (w->win) SDL_DestroyWindow(w->win);
    free(w->fb);
    free(w);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

NesPinWindow *nes_pin_window_sdl_create(NesPins *pins) {
    if (!pins || SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL;
    SdlPinWindow *w = calloc(1, sizeof(*w));
    if (!w) { SDL_QuitSubSystem(SDL_INIT_VIDEO); return NULL; }
    w->base = (NesPinWindow){ sdl_show, sdl_hide, sdl_visible, sdl_refresh, sdl_destroy };
    nes_pin_panel_init(&w->panel, pins);
    w->fb = calloc((size_t)NES_PIN_PANEL_W * NES_PIN_PANEL_H, sizeof(uint32_t));
    w->win = SDL_CreateWindow("GEMU - Cartridge Pins", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              NES_PIN_PANEL_W, NES_PIN_PANEL_H, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    w->ren = w->win ? SDL_CreateRenderer(w->win, -1, 0) : NULL;
    w->tex = w->ren ? SDL_CreateTexture(w->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                        NES_PIN_PANEL_W, NES_PIN_PANEL_H) : NULL;
    if (!w->fb || !w->tex) {
        sdl_destroy(&w->base);
        return NULL;
    }
    /* Panel coordinates whatever the window's size; SDL maps the mouse back. */
    SDL_RenderSetLogicalSize(w->ren, NES_PIN_PANEL_W, NES_PIN_PANEL_H);
    SDL_AddEventWatch(event_watch, w);
    return &w->base;
}
