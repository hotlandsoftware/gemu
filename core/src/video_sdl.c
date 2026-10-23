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
    SDL_Texture    *scaled;     /* integer-upscaled copy for sharp-bilinear scaling */
    int             scaled_k;   /* its scale factor (0 = none yet) */
    bool            targets;    /* renderer supports render-to-texture */
    SDL_Rect        dst;        /* where the picture went in the last frame */
    int             width;
    int             height;
    bool            software;
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
    if (v->scaled)   SDL_DestroyTexture(v->scaled);
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

    /* Every window can be resized freely by dragging its edges; the picture
     * is scaled to fit, keeping its aspect ratio (see present_argb()). */
    int ww = spec->window_width  > 0 ? spec->window_width  : spec->width;
    int wh = spec->window_height > 0 ? spec->window_height : spec->height;
    Uint32 win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    bool kmsdrm = video_sdl_is_kmsdrm();
    int window_pos = kmsdrm ? SDL_WINDOWPOS_CENTERED_DISPLAY(0)
                            : SDL_WINDOWPOS_CENTERED;
    v->window = SDL_CreateWindow(
        spec->title ? spec->title : "GEMU",
        window_pos, window_pos,
        ww, wh, win_flags);
    if (!v->window) goto fail;
    SDL_SetWindowMinimumSize(v->window, v->width < 160 ? v->width : 160,
                                        v->height < 100 ? v->height : 100);
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

    v->targets = SDL_RenderTargetSupported(v->renderer);

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

/* Largest rectangle with the framebuffer's aspect ratio that fits the
 * window's output, centered (letterboxed or pillarboxed). */
static SDL_Rect fit_rect(const GemuVideoSdl *v, int ow, int oh) {
    SDL_Rect r;
    if ((long)ow * v->height <= (long)oh * v->width) {
        r.w = ow;
        r.h = (int)((long)ow * v->height / v->width);
    } else {
        r.h = oh;
        r.w = (int)((long)oh * v->width / v->height);
    }
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    r.x = (ow - r.w) / 2;
    r.y = (oh - r.h) / 2;
    return r;
}

static void set_scale_mode(SDL_Texture *t, bool linear) {
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(t, linear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#else
    (void)t; (void)linear;
#endif
}

/* Draw the frame texture into dst.  Exact integer multiples are drawn with
 * nearest-neighbour scaling.  Anything else uses "sharp bilinear": scale up
 * by the next integer factor with nearest-neighbour, then filter that down
 * to dst - pixels stay crisp, without the uneven rows and columns that
 * plain nearest-neighbour gives at fractional sizes. */
static void draw_scaled(GemuVideoSdl *v, const SDL_Rect *dst) {
    bool exact = dst->w % v->width == 0 && dst->h % v->height == 0 &&
                 dst->w / v->width == dst->h / v->height;
    int k = (dst->w + v->width - 1) / v->width;
    if (exact || !v->targets || k < 1 || k > 16) {
        set_scale_mode(v->texture, !exact);
        SDL_RenderCopy(v->renderer, v->texture, NULL, dst);
        return;
    }
    if (k == 1) {                           /* shrinking: filter directly */
        set_scale_mode(v->texture, true);
        SDL_RenderCopy(v->renderer, v->texture, NULL, dst);
        return;
    }
    if (v->scaled_k != k) {
        if (v->scaled) SDL_DestroyTexture(v->scaled);
        v->scaled = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_TARGET,
                                      v->width * k, v->height * k);
        v->scaled_k = v->scaled ? k : 0;
        if (!v->scaled) { v->targets = false; draw_scaled(v, dst); return; }
        set_scale_mode(v->scaled, true);
    }
    set_scale_mode(v->texture, false);
    SDL_SetRenderTarget(v->renderer, v->scaled);
    SDL_RenderCopy(v->renderer, v->texture, NULL, NULL);
    SDL_SetRenderTarget(v->renderer, NULL);
    SDL_RenderCopy(v->renderer, v->scaled, NULL, dst);
}

void gemu_video_sdl_present_argb(GemuVideoSdl *v, const uint32_t *pixels,
                                 int w, int h) {
    if (!v || !pixels || w != v->width || h != v->height) return;
    SDL_UpdateTexture(v->texture, NULL, pixels, w * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(v->renderer, 0, 0, 0, 255);
    SDL_RenderClear(v->renderer);
    int ow = v->width, oh = v->height;
    SDL_GetRendererOutputSize(v->renderer, &ow, &oh);
    v->dst = fit_rect(v, ow, oh);
    draw_scaled(v, &v->dst);
    if (v->overlay_cb) {
        /* Draw overlay at native window pixels, not logical (fb) pixels.
         * Compute integer scale so the menu can size fonts/boxes proportionally. */
        int scale = v->dst.w / v->width;
        if (scale < 1) scale = 1;
        /* Cap independent of the emulated framebuffer's pixel scale - low-res
         * cores (e.g. CHIP-8 at scale 10) would otherwise blow the menu font
         * up to the point of being unreadable/comically oversized. */
        if (scale > 4) scale = 4;
        v->overlay_cb(v->overlay_ud, v->renderer, scale);
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
    if (!v || v->dst.w <= 0 || v->dst.h <= 0) { if (x) *x = -1; if (y) *y = -1; return; }
    int wx, wy, winw, winh, ow, oh;
    SDL_GetMouseState(&wx, &wy);
    SDL_GetWindowSize(v->window, &winw, &winh);
    SDL_GetRendererOutputSize(v->renderer, &ow, &oh);
    /* window coordinates -> output pixels (differ on HiDPI) -> framebuffer */
    double px = winw > 0 ? (double)wx * ow / winw : wx;
    double py = winh > 0 ? (double)wy * oh / winh : wy;
    if (x) *x = (int)((px - v->dst.x) * v->width  / v->dst.w);
    if (y) *y = (int)((py - v->dst.y) * v->height / v->dst.h);
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
