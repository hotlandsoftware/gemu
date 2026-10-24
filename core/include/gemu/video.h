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
    int               aspect_w, aspect_h;   /* display shape; 0 = width:height */
    GemuRendererType  renderer;
    const char       *log_prefix;
} GemuVideoSdlSpec;

GemuVideoSdl *gemu_video_sdl_create(const GemuVideoSdlSpec *spec);
void          gemu_video_sdl_destroy(GemuVideoSdl *v);

void gemu_video_sdl_present_argb(GemuVideoSdl *v, const uint32_t *pixels,
                                 int w, int h);
void gemu_video_sdl_mouse_logical(GemuVideoSdl *v, int *x, int *y);
/* Reserve fn(output height) pixels above the picture (the menu bar). */
void gemu_video_sdl_set_top_margin(GemuVideoSdl *v, int (*fn)(int out_h));
SDL_Renderer *gemu_video_sdl_get_renderer(const GemuVideoSdl *v);
int  gemu_video_sdl_width(const GemuVideoSdl *v);
int  gemu_video_sdl_height(const GemuVideoSdl *v);
int  gemu_video_sdl_shown_height(const GemuVideoSdl *v, int w);
/* Change the framebuffer size and shape (aspect 0 = square pixels). */
bool gemu_video_sdl_set_geometry(GemuVideoSdl *v, int w, int h, int aspect_w, int aspect_h);

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
    int             aspect_w, aspect_h;     /* display shape; 0 = width:height */
} GemuVideoGtkSpec;

GemuVideoGtk *gemu_video_gtk_create(const GemuVideoGtkSpec *spec);
void          gemu_video_gtk_destroy(GemuVideoGtk *v);

void gemu_video_gtk_present_argb(GemuVideoGtk *v, const uint32_t *pixels,
                                 int w, int h);
void gemu_video_gtk_poll(void);

GtkWidget *gemu_video_gtk_window(GemuVideoGtk *v);
GtkWidget *gemu_video_gtk_drawing_area(GemuVideoGtk *v);
/* Where the picture is drawn inside the drawing area (logical pixels): the
 * window resizes freely and the picture is fitted with its aspect ratio. */
void gemu_video_gtk_picture_rect(GemuVideoGtk *v, int *x, int *y, int *w, int *h);

/* Put the window menu bar (gemu/gtk_menu.h) above the picture. */
void gemu_video_gtk_set_menubar(GemuVideoGtk *v, GtkWidget *bar);
/* Window scale: picture width / framebuffer width; setting resizes. */
int  gemu_video_gtk_get_scale(GemuVideoGtk *v);
void gemu_video_gtk_set_scale(GemuVideoGtk *v, int scale);
/* Change the framebuffer size and shape (switching monitors). */
void gemu_video_gtk_set_geometry(GemuVideoGtk *v, int w, int h, int aspect_w, int aspect_h);

#endif /* GEMU_GTK */
