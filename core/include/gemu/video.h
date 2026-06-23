#pragma once
#include "gemu/display.h"
#include "gemu/monitor.h"
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct GemuVideoSdl GemuVideoSdl;

typedef struct {
    const char       *title;
    int               width;
    int               height;
    int               window_width;
    int               window_height;
    const uint32_t   *palette;
    int               n_colors;
    GemuRendererType  renderer;
    const char       *log_prefix;
} GemuVideoSdlSpec;

GemuVideoSdl *gemu_video_sdl_create(const GemuVideoSdlSpec *spec);
void          gemu_video_sdl_destroy(GemuVideoSdl *v);

void gemu_video_sdl_present_argb(GemuVideoSdl *v, const uint32_t *pixels,
                                 int w, int h);
void gemu_video_sdl_present_indexed(GemuVideoSdl *v, const uint8_t *pixels,
                                    int w, int h);
void gemu_video_sdl_present_mono(GemuVideoSdl *v, const uint8_t *pixels,
                                 int w, int h, uint32_t on, uint32_t off);
void gemu_video_sdl_clear(GemuVideoSdl *v);

bool gemu_video_sdl_is_software(const GemuVideoSdl *v);
void gemu_video_sdl_mouse_logical(GemuVideoSdl *v, int *x, int *y);
/* Snap window to nearest integer scale on resize (no-op for fixed-size windows). */
void gemu_video_sdl_snap_resize(GemuVideoSdl *v, int new_w, int new_h);

/* Optional overlay: called after emulator frame is drawn but before
 * SDL_RenderPresent.  Use for in-emulator menus, OSD, etc. */
/* pixel_scale: integer window/fb scale passed to the callback so overlays
 * can render at the right size (e.g. font 6px × scale). */
void gemu_video_sdl_set_overlay(GemuVideoSdl *v,
                                void (*cb)(void *ud, SDL_Renderer *r, int pixel_scale),
                                void *ud);

SDL_Window *gemu_video_sdl_get_window(const GemuVideoSdl *v);

#ifdef GEMU_GTK
#include <gtk/gtk.h>

typedef struct GemuVideoGtk GemuVideoGtk;

typedef struct {
    const char     *title;
    int             width;
    int             height;
    int             scale;
    int             window_width;
    int             window_height;
    const uint32_t *palette;
    int             n_colors;
    GemuMonitor    *monitor;
    void          (*hex_toggle_cb)(void *);  /* optional: Debug > Hex Editor */
    void           *hex_toggle_ud;
} GemuVideoGtkSpec;

GemuVideoGtk *gemu_video_gtk_create(const GemuVideoGtkSpec *spec);
void          gemu_video_gtk_destroy(GemuVideoGtk *v);

void gemu_video_gtk_present_argb(GemuVideoGtk *v, const uint32_t *pixels,
                                 int w, int h);
void gemu_video_gtk_present_indexed(GemuVideoGtk *v, const uint8_t *pixels,
                                    int w, int h);
void gemu_video_gtk_present_mono(GemuVideoGtk *v, const uint8_t *pixels,
                                 int w, int h, uint32_t on, uint32_t off);
void gemu_video_gtk_poll(void);

GtkWidget *gemu_video_gtk_window(GemuVideoGtk *v);
GtkWidget *gemu_video_gtk_drawing_area(GemuVideoGtk *v);

#endif /* GEMU_GTK */
