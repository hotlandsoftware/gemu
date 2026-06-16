#include "gemu/video.h"
#include <stdio.h>
#include <stdlib.h>

struct GemuVideoSdl {
    SDL_Window     *window;
    SDL_Renderer   *renderer;
    SDL_Texture    *texture;
    uint32_t       *frame_argb;
    const uint32_t *palette;
    int             n_colors;
    int             width;
    int             height;
    bool            software;
    void          (*overlay_cb)(void *ud, SDL_Renderer *r);
    void           *overlay_ud;
};

static void video_sdl_free(GemuVideoSdl *v) {
    if (!v) return;
    if (v->texture)  SDL_DestroyTexture(v->texture);
    if (v->renderer) SDL_DestroyRenderer(v->renderer);
    if (v->window)   SDL_DestroyWindow(v->window);
    free(v->frame_argb);
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
    v->palette  = spec->palette;
    v->n_colors = spec->n_colors;

    v->frame_argb = malloc((size_t)v->width * (size_t)v->height *
                           sizeof(*v->frame_argb));
    if (!v->frame_argb) goto fail;

    int ww = spec->window_width  > 0 ? spec->window_width  : spec->width;
    int wh = spec->window_height > 0 ? spec->window_height : spec->height;
    v->window = SDL_CreateWindow(
        spec->title ? spec->title : "GEMU",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        ww, wh, SDL_WINDOW_SHOWN);
    if (!v->window) goto fail;

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
    if (v->overlay_cb)
        v->overlay_cb(v->overlay_ud, v->renderer);
    SDL_RenderPresent(v->renderer);
}

void gemu_video_sdl_present_indexed(GemuVideoSdl *v, const uint8_t *pixels,
                                    int w, int h) {
    if (!v || !pixels || w != v->width || h != v->height || !v->palette)
        return;
    for (int i = 0; i < w * h; i++) {
        uint8_t idx = pixels[i];
        if (v->n_colors > 0 && idx >= v->n_colors)
            idx = (uint8_t)(v->n_colors - 1);
        v->frame_argb[i] = v->palette[idx];
    }
    gemu_video_sdl_present_argb(v, v->frame_argb, w, h);
}

void gemu_video_sdl_present_mono(GemuVideoSdl *v, const uint8_t *pixels,
                                 int w, int h, uint32_t on, uint32_t off) {
    if (!v || !pixels || w != v->width || h != v->height) return;
    for (int i = 0; i < w * h; i++)
        v->frame_argb[i] = pixels[i] ? on : off;
    gemu_video_sdl_present_argb(v, v->frame_argb, w, h);
}

void gemu_video_sdl_clear(GemuVideoSdl *v) {
    if (!v) return;
    SDL_SetRenderDrawColor(v->renderer, 0, 0, 0, 255);
    SDL_RenderClear(v->renderer);
    SDL_RenderPresent(v->renderer);
}

bool gemu_video_sdl_is_software(const GemuVideoSdl *v) {
    return v && v->software;
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

void gemu_video_sdl_set_overlay(GemuVideoSdl *v,
                                void (*cb)(void *ud, SDL_Renderer *r),
                                void *ud) {
    if (!v) return;
    v->overlay_cb = cb;
    v->overlay_ud = ud;
}
