#include "gemu_display_priv.h"
#include "gemu/video.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

/* InputMenu lives in gemu-mos/src/vga/ but is included via the build's
 * include path (same hack that lets rca share it). */
#include "input_menu.h"

#define SDL_MAX_BINDINGS GEMU_DISPLAY_MAX_ACTIONS

typedef struct {
    GemuVideoSdl *video;
    InputMenu    *menu;

    /* key → action bit mapping, built from action table + INI overrides */
    struct { SDL_Keycode key; uint32_t bit; } bindings[SDL_MAX_BINDINGS];
    int n_bindings;

    /* SDL_SCANCODE_TAB is intercepted for the rebind menu when Tab is not
     * bound to any game action. */
    bool tab_is_action;

    /* Key-hold tracking: SDL gives us down/up events, not state queries.
     * We keep a compact list of currently-held SDL_Keycodes and their bits. */
    struct { SDL_Keycode key; uint32_t bits; } held[SDL_MAX_BINDINGS * 2];
    int n_held;

    bool menu_was_open;   /* detect menu close to reload INI */
    const GemuActionDef *action_defs;
    int                  n_actions;
    const char          *ini_section;
} SdlBackend;

/* ── Binding table ───────────────────────────────────────────────────────── */

static void build_bindings(SdlBackend *b) {
    b->n_bindings   = 0;
    b->tab_is_action = false;

    for (int i = 0; i < b->n_actions && b->n_bindings < SDL_MAX_BINDINGS; i++) {
        const GemuActionDef *def = &b->action_defs[i];
        char val[64] = "";
        if (b->ini_section)
            gemu_ini_read(b->ini_section, def->name, val, sizeof(val));
        const char *key_name = (val[0]) ? val : def->default_key;
        SDL_Keycode kc = SDL_GetKeyFromName(key_name);
        if (kc == SDLK_UNKNOWN) continue;

        b->bindings[b->n_bindings].key = kc;
        b->bindings[b->n_bindings].bit = def->bit;
        b->n_bindings++;

        if (kc == SDLK_TAB) b->tab_is_action = true;
    }
}

static uint32_t key_to_bits(const SdlBackend *b, SDL_Keycode kc) {
    for (int i = 0; i < b->n_bindings; i++)
        if (b->bindings[i].key == kc) return b->bindings[i].bit;
    return 0;
}

/* ── Held-key tracking ───────────────────────────────────────────────────── */

static void held_set(SdlBackend *b, SDL_Keycode kc, uint32_t bits, bool down) {
    for (int i = 0; i < b->n_held; i++) {
        if (b->held[i].key != kc) continue;
        if (down) b->held[i].bits = bits;
        else      b->held[i].bits = 0;
        return;
    }
    if (down && b->n_held < (int)(sizeof(b->held)/sizeof(b->held[0]))) {
        b->held[b->n_held].key  = kc;
        b->held[b->n_held].bits = bits;
        b->n_held++;
    }
}

static uint32_t held_mask(const SdlBackend *b) {
    uint32_t m = 0;
    for (int i = 0; i < b->n_held; i++) m |= b->held[i].bits;
    return m;
}

/* ── Raw key encoding: SDL keycode → UTF-32 codepoint (0 = ignore) ──────── */

static uint32_t sdl_to_raw(SDL_Keycode kc) {
    /* Printable ASCII: SDL keycodes for a-z, 0-9, symbols are their codepoints */
    if (kc >= 0x20 && kc <= 0x7e) return (uint32_t)kc;
    switch (kc) {
    case SDLK_RETURN:    return '\r';
    case SDLK_BACKSPACE: return '\b';
    case SDLK_TAB:       return '\t';
    case SDLK_ESCAPE:    return 0x1b;
    case SDLK_DELETE:    return 0x7f;
    default:             return 0;
    }
}

/* ── Backend callbacks ───────────────────────────────────────────────────── */

static void sdl_do_render(GemuDisplay *d, const uint32_t *argb, int w, int h) {
    SdlBackend *b = d->backend;
    gemu_video_sdl_present_argb(b->video, argb, w, h);
}

static uint32_t sdl_do_poll(GemuDisplay *d) {
    SdlBackend *b = d->backend;

    /* Detect menu close → reload INI bindings */
    bool menu_open = b->menu && input_menu_is_open(b->menu);
    if (b->menu_was_open && !menu_open)
        build_bindings(b);
    b->menu_was_open = menu_open;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        /* Menu swallows all events while open */
        if (menu_open) {
            input_menu_handle_event(b->menu, &ev);
            continue;
        }

        switch (ev.type) {
        case SDL_QUIT:
            d->quit = true;
            break;

        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            gemu_video_sdl_mouse_logical(b->video,
                                         &d->pointer.x, &d->pointer.y);
            if (ev.type == SDL_MOUSEBUTTONDOWN &&
                ev.button.button == SDL_BUTTON_LEFT)
                d->pointer.button = true;
            if (ev.type == SDL_MOUSEBUTTONUP &&
                ev.button.button == SDL_BUTTON_LEFT)
                d->pointer.button = false;
            break;

        case SDL_KEYDOWN: {
            SDL_Keycode kc = ev.key.keysym.sym;
            if (kc == SDLK_ESCAPE) { d->quit = true; break; }

            /* Tab opens rebind menu unless Tab is a game action */
            if (kc == SDLK_TAB && !b->tab_is_action && b->menu) {
                input_menu_toggle(b->menu);
                break;
            }

            uint32_t bits = key_to_bits(b, kc);
            held_set(b, kc, bits, true);

            /* Raw queue: only push special keys here; printable characters
             * come from SDL_TEXTINPUT (which is layout-correct and avoids
             * double-pushing the same key press). */
            uint32_t cp = sdl_to_raw(kc);
            if (cp && (cp < 0x20 || cp == 0x7f))
                gemu_display_push_raw(d, cp);
            break;
        }

        case SDL_KEYUP: {
            SDL_Keycode kc = ev.key.keysym.sym;
            held_set(b, kc, key_to_bits(b, kc), false);
            break;
        }

        case SDL_TEXTINPUT:
            /* TEXTINPUT delivers layout-correct UTF-8 for printable keys.
             * Decode the first codepoint and push it as raw input. */
            {
                const unsigned char *u = (const unsigned char *)ev.text.text;
                uint32_t cp = 0;
                if      (u[0] < 0x80)  cp = u[0];
                else if (u[0] < 0xe0)  cp = ((u[0]&0x1f)<<6)|(u[1]&0x3f);
                else if (u[0] < 0xf0)  cp = ((u[0]&0x0f)<<12)|((u[1]&0x3f)<<6)|(u[2]&0x3f);
                else                   cp = ((u[0]&0x07)<<18)|((u[1]&0x3f)<<12)|((u[2]&0x3f)<<6)|(u[3]&0x3f);
                if (cp >= 0x20 && cp != 0x7f)
                    gemu_display_push_raw(d, cp);
            }
            break;

        default:
            break;
        }
    }

    /* Check InputMenu for quit / reset requests */
    if (b->menu) {
        if (input_menu_quit_requested(b->menu)) {
            d->quit = true;
            input_menu_clear_actions(b->menu);
        }
        if (input_menu_reset_requested(b->menu)) {
            d->reset = true;
            input_menu_clear_actions(b->menu);
        }
    }

    return held_mask(b);
}

static bool sdl_do_is_key_held(GemuDisplay *d, const char *name) {
    (void)d;
    SDL_Keycode kc = SDL_GetKeyFromName(name);
    if (kc == SDLK_UNKNOWN) return false;
    SDL_Scancode sc = SDL_GetScancodeFromKey(kc);
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    int numkeys = 0;
    const uint8_t *state = SDL_GetKeyboardState(&numkeys);
    return (int)sc < numkeys && state[sc];
}

static void sdl_do_open_rebind(GemuDisplay *d) {
    SdlBackend *b = d->backend;
    if (b->menu && !input_menu_is_open(b->menu))
        input_menu_toggle(b->menu);
}

static void sdl_do_destroy(GemuDisplay *d) {
    SdlBackend *b = d->backend;
    if (!b) return;
    if (b->menu)  input_menu_destroy(b->menu);
    if (b->video) gemu_video_sdl_destroy(b->video);
    free(b);
    d->backend = NULL;
}

/* ── Overlay callback for InputMenu ─────────────────────────────────────── */

static void menu_overlay_cb(void *ud, SDL_Renderer *r) {
    input_menu_render((InputMenu *)ud, r);
}

/* ── Create ──────────────────────────────────────────────────────────────── */

GemuDisplay *gemu_display_sdl_create(const GemuDisplayConfig *cfg) {
    SdlBackend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->action_defs = cfg->actions;
    b->n_actions   = cfg->n_actions;
    b->ini_section = cfg->ini_section;

    int w = cfg->fb_width,  h = cfg->fb_height;
    int ww = cfg->window_width  ? cfg->window_width  : w * (cfg->scale ? cfg->scale : 2);
    int wh = cfg->window_height ? cfg->window_height : h * (cfg->scale ? cfg->scale : 2);

    b->video = gemu_video_sdl_create(&(GemuVideoSdlSpec){
        .title         = cfg->title ? cfg->title : "GEMU",
        .width         = w,
        .height        = h,
        .window_width  = ww,
        .window_height = wh,
        .renderer      = cfg->renderer,
        .log_prefix    = "gemu",
    });
    if (!b->video) { free(b); return NULL; }

    /* Build action→key binding table */
    build_bindings(b);

    /* InputMenu: pass action names + defaults so user can rebind in-game.
     * We need SDL_Keycode defaults, so build them from the action table. */
    if (cfg->n_actions > 0 && cfg->ini_section) {
        SDL_Keycode *defaults = calloc((size_t)cfg->n_actions, sizeof(SDL_Keycode));
        if (defaults) {
            for (int i = 0; i < cfg->n_actions; i++) {
                defaults[i] = SDL_GetKeyFromName(cfg->actions[i].default_key);
                if (defaults[i] == SDLK_UNKNOWN) defaults[i] = SDLK_UNKNOWN;
            }
            /* Extract names into a separate array (InputMenu takes const char**) */
            const char **names = calloc((size_t)cfg->n_actions, sizeof(char *));
            if (names) {
                for (int i = 0; i < cfg->n_actions; i++)
                    names[i] = cfg->actions[i].name;
                b->menu = input_menu_create(NULL,
                                            cfg->n_actions,
                                            names,
                                            defaults,
                                            cfg->ini_section);
                free(names);
            }
            free(defaults);
        }
        if (b->menu)
            gemu_video_sdl_set_overlay(b->video, menu_overlay_cb, b->menu);
    }

    GemuDisplay *d = calloc(1, sizeof(*d));
    if (!d) { sdl_do_destroy(&(GemuDisplay){ .backend = b }); return NULL; }
    d->backend        = b;
    d->do_render      = sdl_do_render;
    d->do_poll        = sdl_do_poll;
    d->do_is_key_held = sdl_do_is_key_held;
    d->do_open_rebind = sdl_do_open_rebind;
    d->do_destroy     = sdl_do_destroy;
    d->pointer.x = d->pointer.y = -1;
    return d;
}
