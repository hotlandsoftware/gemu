/*
 * VNC as part of every display.
 *
 * When -vnc is given, gemu_display_create() attaches the process's VNC
 * server to the display it returns (creating a headless display for
 * -display none).  Rendered frames are mirrored to it, and VNC keyboard and
 * pointer input is merged into the display's own input state: action bits
 * through the same key bindings (gemu.ini overrides included) the window
 * uses, gemu_display_is_key_held(), the raw-character queue, and the
 * pointer.  Machines don't know VNC exists.
 */
#include "gemu_display_priv.h"
#include "gemu/vnc.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

#define VNC_HELD_MAX 32

struct GemuDisplayVnc {
    GemuVncServer *server;
    struct { SDL_Keycode key; uint32_t bit; } bindings[GEMU_DISPLAY_MAX_ACTIONS];
    int            n_bindings;
    SDL_Keycode    held[VNC_HELD_MAX];   /* keys VNC clients are holding */
    int            n_held;
    bool           pointer_active;       /* a client has moved the pointer */
};

/* ── X11 keysym -> SDL keycode ───────────────────────────────────────────── */

static const struct { uint32_t sym; SDL_Keycode key; } special_keys[] = {
    { 0xFF08, SDLK_BACKSPACE }, { 0xFF09, SDLK_TAB },       { 0xFF0D, SDLK_RETURN },
    { 0xFF13, SDLK_PAUSE },     { 0xFF14, SDLK_SCROLLLOCK }, { 0xFF1B, SDLK_ESCAPE },
    { 0xFF50, SDLK_HOME },      { 0xFF51, SDLK_LEFT },      { 0xFF52, SDLK_UP },
    { 0xFF53, SDLK_RIGHT },     { 0xFF54, SDLK_DOWN },      { 0xFF55, SDLK_PAGEUP },
    { 0xFF56, SDLK_PAGEDOWN },  { 0xFF57, SDLK_END },       { 0xFF61, SDLK_PRINTSCREEN },
    { 0xFF63, SDLK_INSERT },    { 0xFF7F, SDLK_NUMLOCKCLEAR }, { 0xFF8D, SDLK_KP_ENTER },
    { 0xFFAA, SDLK_KP_MULTIPLY }, { 0xFFAB, SDLK_KP_PLUS }, { 0xFFAD, SDLK_KP_MINUS },
    { 0xFFAE, SDLK_KP_PERIOD }, { 0xFFAF, SDLK_KP_DIVIDE },
    { 0xFFE1, SDLK_LSHIFT },    { 0xFFE2, SDLK_RSHIFT },    { 0xFFE3, SDLK_LCTRL },
    { 0xFFE4, SDLK_RCTRL },     { 0xFFE5, SDLK_CAPSLOCK },  { 0xFFE7, SDLK_LGUI },
    { 0xFFE8, SDLK_RGUI },      { 0xFFE9, SDLK_LALT },      { 0xFFEA, SDLK_RALT },
    { 0xFFEB, SDLK_LGUI },      { 0xFFEC, SDLK_RGUI },      { 0xFFFF, SDLK_DELETE },
};

/* VNC sends the shifted symbol ('!' for shift+1); key bindings and
 * is_key_held() name the physical key, so map US shifted symbols back. */
static uint32_t unshift(uint32_t ch) {
    static const char shifted[]   = "!@#$%^&*()_+{}|:\"~<>?";
    static const char unshifted[] = "1234567890-=[]\\;'`,./";
    const char *p = ch < 0x80 ? strchr(shifted, (int)ch) : NULL;
    if (p && ch) return (uint32_t)unshifted[p - shifted];
    if (ch >= 'A' && ch <= 'Z') return ch | 0x20;
    return ch;
}

static SDL_Keycode keysym_to_key(uint32_t sym) {
    if (sym >= 0x20 && sym <= 0x7E) return (SDL_Keycode)unshift(sym);
    if (sym >= 0xFFBE && sym <= 0xFFC9) return SDLK_F1 + (SDL_Keycode)(sym - 0xFFBE);   /* F1-F12 */
    if (sym >= 0xFFB0 && sym <= 0xFFB9)                                                 /* KP 0-9 */
        return sym == 0xFFB0 ? SDLK_KP_0 : SDLK_KP_1 + (SDL_Keycode)(sym - 0xFFB1);
    for (size_t i = 0; i < sizeof(special_keys) / sizeof(special_keys[0]); i++)
        if (special_keys[i].sym == sym) return special_keys[i].key;
    return SDLK_UNKNOWN;
}

/* Character for the raw key queue (see gemu_display_pop_raw_key()). */
static uint32_t keysym_to_char(uint32_t sym) {
    if (sym >= 0x20 && sym <= 0x7E) return sym;
    switch (sym) {
    case 0xFF0D: case 0xFF8D: return '\r';
    case 0xFF08: return '\b';
    case 0xFF09: return '\t';
    case 0xFF1B: return 0x1B;
    case 0xFFFF: return 0x7F;
    default:     return 0;
    }
}

/* ── Headless display (-display none -vnc ...) ───────────────────────────── */

static void headless_render(GemuDisplay *d, const uint32_t *argb, int w, int h) {
    (void)d; (void)argb; (void)w; (void)h;
}
static uint32_t headless_poll(GemuDisplay *d) { (void)d; return 0; }
static void headless_destroy(GemuDisplay *d) { (void)d; }

GemuDisplay *gemu_display_headless_create(const GemuDisplayConfig *cfg) {
    (void)cfg;
    GemuDisplay *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->do_render  = headless_render;
    d->do_poll    = headless_poll;
    d->do_destroy = headless_destroy;
    d->headless   = true;
    d->pointer.x = d->pointer.y = -1;
    gemu_window_title(d->title, sizeof(d->title), NULL, false);
    return d;
}

/* ── Attach / per-frame hooks ────────────────────────────────────────────── */

void gemu_display_vnc_attach(GemuDisplay *d, GemuVncServer *server,
                             const GemuDisplayConfig *cfg) {
    struct GemuDisplayVnc *v = calloc(1, sizeof(*v));
    if (!v) return;
    v->server = server;
    for (int i = 0; i < cfg->n_actions && v->n_bindings < GEMU_DISPLAY_MAX_ACTIONS; i++) {
        const GemuActionDef *def = &cfg->actions[i];
        char val[64] = "";
        const char *name = (cfg->ini_section &&
                            gemu_ini_read(cfg->ini_section, def->name, val, sizeof(val)))
                         ? val : def->default_key;
        SDL_Keycode key = name ? SDL_GetKeyFromName(name) : SDLK_UNKNOWN;
        if (key == SDLK_UNKNOWN) continue;
        v->bindings[v->n_bindings].key = key;
        v->bindings[v->n_bindings].bit = def->bit;
        v->n_bindings++;
    }
    d->vnc = v;
}

void gemu_display_vnc_detach(GemuDisplay *d) {
    free(d->vnc);
    d->vnc = NULL;
}

void gemu_display_vnc_frame(GemuDisplay *d, const uint32_t *argb, int w, int h) {
    if (d->vnc) gemu_vnc_update_argb(d->vnc->server, argb, w, h);
}

static void set_held(struct GemuDisplayVnc *v, SDL_Keycode key, bool down) {
    int i;
    for (i = 0; i < v->n_held && v->held[i] != key; i++) {}
    if (down && i == v->n_held && v->n_held < VNC_HELD_MAX) v->held[v->n_held++] = key;
    if (!down && i < v->n_held) v->held[i] = v->held[--v->n_held];
}

/* Drain VNC input; returns the action bits VNC clients are holding. */
uint32_t gemu_display_vnc_poll(GemuDisplay *d) {
    struct GemuDisplayVnc *v = d->vnc;
    if (!v) return 0;

    GemuVncKeyEvent ev;
    while (gemu_vnc_pop_key_event(v->server, &ev)) {
        SDL_Keycode key = keysym_to_key(ev.keysym);
        if (key != SDLK_UNKNOWN) set_held(v, key, ev.down);
        if (ev.down) {
            uint32_t ch = keysym_to_char(ev.keysym);
            if (ch) gemu_display_push_raw(d, ch);
        }
    }

    uint32_t mask = 0;
    for (int i = 0; i < v->n_bindings; i++)
        for (int k = 0; k < v->n_held; k++)
            if (v->held[k] == v->bindings[i].key) mask |= v->bindings[i].bit;

    /* The pointer follows whichever source moved it last. */
    GemuVncPointerState p = gemu_vnc_get_pointer(v->server);
    bool moved = p.rel_x || p.rel_y || p.pressed || p.right_pressed ||
                 (v->pointer_active && (p.button != d->pointer.button ||
                                        p.right_button != d->pointer.right_button));
    if (p.x >= 0 && (moved || d->headless)) {
        v->pointer_active = true;
        d->pointer = (GemuPointerState){
            .x = p.x, .y = p.y, .rel_x = p.rel_x, .rel_y = p.rel_y,
            .button = p.button, .pressed = p.pressed,
            .right_button = p.right_button, .right_pressed = p.right_pressed,
        };
    }
    return mask;
}

bool gemu_display_vnc_key_held(GemuDisplay *d, const char *name) {
    struct GemuDisplayVnc *v = d->vnc;
    if (!v) return false;
    SDL_Keycode key = SDL_GetKeyFromName(name);
    for (int i = 0; key != SDLK_UNKNOWN && i < v->n_held; i++)
        if (v->held[i] == key) return true;
    return false;
}
