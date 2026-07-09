#include "gemu/video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif
#ifdef HAVE_SDL_IMAGE
#include <SDL2/SDL_image.h>
#endif

struct GemuVideoSdl {
    SDL_Window     *window;
    SDL_Renderer   *renderer;
    SDL_Texture    *texture;
    int             width;
    int             height;
    bool            software;
    bool            resizable;  /* true for scale-based windows (window_width==0) */
#ifndef _WIN32
    bool            termios_active;
    struct termios  old_termios;
#endif
    void          (*overlay_cb)(void *ud, SDL_Renderer *r, int pixel_scale);
    void           *overlay_ud;
};

static void gemu_video_sdl_clear(GemuVideoSdl *v);

static bool video_sdl_is_kmsdrm(void) {
    const char *driver = SDL_GetCurrentVideoDriver();
    return driver && strcmp(driver, "KMSDRM") == 0;
}

static void video_sdl_mute_console_echo(GemuVideoSdl *v) {
#ifndef _WIN32
    if (!v || !isatty(STDIN_FILENO)) return;
    struct termios tio;
    if (tcgetattr(STDIN_FILENO, &v->old_termios) != 0) return;
    tio = v->old_termios;
    tio.c_lflag &= (tcflag_t)~ECHO;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) == 0)
        v->termios_active = true;
#else
    (void)v;
#endif
}

static void video_sdl_restore_console(GemuVideoSdl *v) {
#ifndef _WIN32
    if (v && v->termios_active)
        tcsetattr(STDIN_FILENO, TCSANOW, &v->old_termios);
#else
    (void)v;
#endif
}

static void video_sdl_free(GemuVideoSdl *v) {
    if (!v) return;
    video_sdl_restore_console(v);
    if (v->texture)  SDL_DestroyTexture(v->texture);
    if (v->renderer) SDL_DestroyRenderer(v->renderer);
    if (v->window)   SDL_DestroyWindow(v->window);
    free(v);
}

GemuVideoSdl *gemu_video_sdl_create(const GemuVideoSdlSpec *spec) {
    if (!spec || spec->width <= 0 || spec->height <= 0) return NULL;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL;

    GemuVideoSdl *v = calloc(1, sizeof(*v));
    if (!v) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return NULL;
    }

    v->width    = spec->width;
    v->height   = spec->height;

    /* Windows with an explicit fixed size (e.g. KIM-1 keypad) are not
     * resizable.  Scale-based windows (window_width == 0) are resizable and
     * will snap to integer scale on SDL_WINDOWEVENT_RESIZED. */
    v->resizable = (spec->window_width == 0);
    int ww = spec->window_width  > 0 ? spec->window_width  : spec->width;
    int wh = spec->window_height > 0 ? spec->window_height : spec->height;
    Uint32 win_flags = SDL_WINDOW_SHOWN | (v->resizable ? SDL_WINDOW_RESIZABLE : 0);
    bool kmsdrm = video_sdl_is_kmsdrm();
    int window_pos = kmsdrm ? SDL_WINDOWPOS_CENTERED_DISPLAY(0)
                            : SDL_WINDOWPOS_CENTERED;
    v->window = SDL_CreateWindow(
        spec->title ? spec->title : "GEMU",
        window_pos, window_pos,
        ww, wh, win_flags);
    if (!v->window) goto fail;
    if (kmsdrm) {
        video_sdl_mute_console_echo(v);
        SDL_RaiseWindow(v->window);
        SDL_SetWindowGrab(v->window, SDL_TRUE);
#if SDL_VERSION_ATLEAST(2, 0, 16)
        SDL_SetWindowKeyboardGrab(v->window, SDL_TRUE);
#endif
#if SDL_VERSION_ATLEAST(2, 0, 5)
        SDL_SetWindowInputFocus(v->window);
#endif
    }

#ifdef HAVE_SDL_IMAGE
    /* Best-effort: gemu.png lives at the repo root, same as every other
     * relative path this project uses (ROMs, gemu.ini via $HOME aside).
     * Missing/unreadable icon is not fatal - just falls back to the
     * platform default window icon. */
    SDL_Surface *icon = IMG_Load("gemu.png");
    if (icon) {
        SDL_SetWindowIcon(v->window, icon);
        SDL_FreeSurface(icon);
    }
#endif

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    if (spec->renderer == GEMU_RENDERER_SOFTWARE)
        v->renderer = SDL_CreateRenderer(v->window, -1, SDL_RENDERER_SOFTWARE);
    else
        v->renderer = SDL_CreateRenderer(v->window, -1, SDL_RENDERER_ACCELERATED);
    if (!v->renderer && spec->renderer == GEMU_RENDERER_AUTO)
        v->renderer = SDL_CreateRenderer(v->window, -1, SDL_RENDERER_SOFTWARE);
    if (!v->renderer) goto fail;

    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(v->renderer, &info) == 0) {
        v->software = (info.flags & SDL_RENDERER_SOFTWARE) != 0;
        if (v->software) {
            const char *why = spec->renderer == GEMU_RENDERER_SOFTWARE
                ? "forced" : "auto-detected";
            fprintf(stderr, "%s: using SDL software renderer (%s: %s)\n",
                    spec->log_prefix ? spec->log_prefix : "gemu",
                    why, info.name ? info.name : "unknown");
        }
    }

    SDL_RenderSetLogicalSize(v->renderer, v->width, v->height);
    /* For resizable windows, render at the largest integer scale that fits -
     * avoids fighting the WM with SDL_SetWindowSize during a resize drag. */
    if (v->resizable)
        SDL_RenderSetIntegerScale(v->renderer, SDL_TRUE);

    v->texture = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   v->width, v->height);
    if (!v->texture) goto fail;

    gemu_video_sdl_clear(v);
    return v;

fail:
    video_sdl_free(v);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return NULL;
}

void gemu_video_sdl_destroy(GemuVideoSdl *v) {
    video_sdl_free(v);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void gemu_video_sdl_present_argb(GemuVideoSdl *v, const uint32_t *pixels,
                                 int w, int h) {
    if (!v || !pixels || w != v->width || h != v->height) return;
    SDL_UpdateTexture(v->texture, NULL, pixels, w * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(v->renderer, 0, 0, 0, 255);
    SDL_RenderClear(v->renderer);
    SDL_RenderCopy(v->renderer, v->texture, NULL, NULL);
    if (v->overlay_cb) {
        /* Draw overlay at native window pixels, not logical (fb) pixels.
         * Compute integer scale so the menu can size fonts/boxes proportionally. */
        int ow = v->width, oh = v->height;
        SDL_GetRendererOutputSize(v->renderer, &ow, &oh);
        int scale = (v->width > 0) ? (ow / v->width) : 1;
        if (scale < 1) scale = 1;
        /* Cap independent of the emulated framebuffer's pixel scale - low-res
         * cores (e.g. CHIP-8 at scale 10) would otherwise blow the menu font
         * up to the point of being unreadable/comically oversized. */
        if (scale > 4) scale = 4;
        SDL_RenderSetLogicalSize(v->renderer, 0, 0);
        v->overlay_cb(v->overlay_ud, v->renderer, scale);
        SDL_RenderSetLogicalSize(v->renderer, v->width, v->height);
    }
    SDL_RenderPresent(v->renderer);
}

static void gemu_video_sdl_clear(GemuVideoSdl *v) {
    if (!v) return;
    SDL_SetRenderDrawColor(v->renderer, 0, 0, 0, 255);
    SDL_RenderClear(v->renderer);
    SDL_RenderPresent(v->renderer);
}


void gemu_video_sdl_mouse_logical(GemuVideoSdl *v, int *x, int *y) {
    if (!v) { if (x) *x = -1; if (y) *y = -1; return; }
    int wx, wy;
    SDL_GetMouseState(&wx, &wy);
    float lx, ly;
    SDL_RenderWindowToLogical(v->renderer, wx, wy, &lx, &ly);
    if (x) *x = (int)lx;
    if (y) *y = (int)ly;
}

void gemu_video_sdl_snap_resize(GemuVideoSdl *v, int new_w, int new_h) {
    if (!v || !v->resizable || v->width <= 0 || v->height <= 0) return;
    int scale = (new_w + v->width / 2) / v->width;
    if (scale < 1) scale = 1;
    int snapped_w = v->width  * scale;
    int snapped_h = v->height * scale;
    if (new_w != snapped_w || new_h != snapped_h)
        SDL_SetWindowSize(v->window, snapped_w, snapped_h);
}

void gemu_video_sdl_set_overlay(GemuVideoSdl *v,
                                void (*cb)(void *ud, SDL_Renderer *r, int pixel_scale),
                                void *ud) {
    if (!v) return;
    v->overlay_cb = cb;
    v->overlay_ud = ud;
}

SDL_Window *gemu_video_sdl_get_window(const GemuVideoSdl *v) {
    return v ? v->window : NULL;
}
