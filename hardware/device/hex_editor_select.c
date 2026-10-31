/*
 * Debug > Hex Editor: the implementation for the display in use (see
 * hex_editor.h) behind the one API the machines call.
 */
#include "hex_editor.h"
#include <stdlib.h>

struct HexEditor {
    void  *impl;
    void (*destroy)(void *);
    void (*show)(void *);
    void (*hide)(void *);
    bool (*is_visible)(const void *);
    void (*refresh)(void *);
};

/* typed wrappers, so no function pointer is called through the wrong type */
#define WRAP(prefix, Type)                                                                 \
    static void prefix##_destroy(void *p)          { hex_editor_##prefix##_destroy(p); }    \
    static void prefix##_show(void *p)             { hex_editor_##prefix##_show(p); }       \
    static void prefix##_hide(void *p)             { hex_editor_##prefix##_hide(p); }       \
    static bool prefix##_is_visible(const void *p) { return hex_editor_##prefix##_is_visible(p); } \
    static void prefix##_refresh(void *p)          { hex_editor_##prefix##_refresh(p); }

#define BIND(he, prefix) do {                                  \
        (he)->destroy = prefix##_destroy; (he)->show = prefix##_show;   \
        (he)->hide = prefix##_hide; (he)->is_visible = prefix##_is_visible; \
        (he)->refresh = prefix##_refresh;                          \
    } while (0)

#ifdef GEMU_GTK
WRAP(gtk, HexEditorGtk)
#endif
#ifdef _WIN32
WRAP(win32, HexEditorWin32)
#else
WRAP(sdl, HexEditorSdl)
#endif

HexEditor *hex_editor_create(GemuDisplayType display, const HexRegion *regions, int n_regions) {
    HexEditor *he = calloc(1, sizeof(*he));
    if (!he) return NULL;
#ifdef GEMU_GTK
    if (display == GEMU_DISPLAY_GTK) {
        he->impl = hex_editor_gtk_create(regions, n_regions);
        BIND(he, gtk);
    }
#endif
    if (display == GEMU_DISPLAY_SDL) {
#ifdef _WIN32
        he->impl = hex_editor_win32_create(regions, n_regions);
        BIND(he, win32);
#else
        he->impl = hex_editor_sdl_create(regions, n_regions);
        BIND(he, sdl);
#endif
    }
    if (!he->impl) { free(he); return NULL; }
    return he;
}

void hex_editor_destroy(HexEditor *he) {
    if (!he) return;
    he->destroy(he->impl);
    free(he);
}

void hex_editor_show(HexEditor *he)             { if (he) he->show(he->impl); }
void hex_editor_hide(HexEditor *he)             { if (he) he->hide(he->impl); }
bool hex_editor_is_visible(const HexEditor *he) { return he && he->is_visible(he->impl); }
void hex_editor_refresh(HexEditor *he)          { if (he) he->refresh(he->impl); }
