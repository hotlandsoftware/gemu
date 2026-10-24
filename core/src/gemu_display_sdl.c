#include "gemu_display_priv.h"
#include "gemu/video.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* InputMenu lives in gemu-mos/src/vga/ but is included via the build's
 * include path (same hack that lets rca share it). */
#include "input_menu.h"
#include "gemu_menubar_sdl.h"

#define SDL_MAX_BINDINGS GEMU_DISPLAY_MAX_ACTIONS
#define SDL_CONTROLLER_DEADZONE 16000

typedef struct {
    SDL_GameControllerButton button;
    SDL_GameControllerAxis   axis;
    int                      axis_dir;
    uint32_t                 bit;
} SdlControllerBinding;

typedef struct {
    GemuVideoSdl *video;
    InputMenu    *menu;      /* key/controller binding screen (Input > Configure) */
    GemuMenubar  *menubar;   /* the window's menu bar */

    /* key → action bit mapping, built from action table + INI overrides */
    struct { SDL_Keycode key; uint32_t bit; } bindings[SDL_MAX_BINDINGS];
    int n_bindings;
    SdlControllerBinding controller_bindings[SDL_MAX_BINDINGS];
    int n_controller_bindings;
    SDL_GameController *controller;

    /* SDL_SCANCODE_TAB is intercepted for the rebind menu when Tab is not
     * bound to any game action. */
    bool tab_is_action;
    bool menu_plain_key;   /* the menu key (Tab) focuses the menu bar; false on
                              keyboard machines, which use Ctrl+Shift+Tab */

    /* Key-hold tracking: SDL gives us down/up events, not state queries.
     * We keep a compact list of currently-held SDL_Keycodes and their bits. */
    struct { SDL_Keycode key; uint32_t bits; } held[SDL_MAX_BINDINGS * 2];
    int n_held;

    bool menu_was_open;   /* detect menu close to reload INI */
    const GemuActionDef *action_defs;
    int                  n_actions;
    const char          *ini_section;

    /* Resize snap: record last RESIZED dimensions + time; snap 150ms after
     * the last event so we never call SDL_SetWindowSize mid-drag (which
     * conflicts with the WM's pointer grab on X11 and causes the window to
     * move instead of resize). */
    bool   mouse_captured;
    bool   capture_pointer;
    bool   saw_mouse_motion;
} SdlBackend;

/* ── Binding table ───────────────────────────────────────────────────────── */

static const char *default_controller_binding(const GemuActionDef *def);

static void build_bindings(SdlBackend *b) {
    b->n_bindings   = 0;
    b->n_controller_bindings = 0;
    b->tab_is_action = false;

    for (int i = 0; i < b->n_actions && b->n_bindings < SDL_MAX_BINDINGS; i++) {
        const GemuActionDef *def = &b->action_defs[i];
        char val[64] = "";
        bool has_key_binding = false;
        if (b->ini_section)
            has_key_binding = gemu_ini_read(b->ini_section, def->name, val, sizeof(val));
        const char *key_name = has_key_binding ? val : def->default_key;

        SDL_Keycode kc = SDL_GetKeyFromName(key_name);
        if (kc != SDLK_UNKNOWN) {
            b->bindings[b->n_bindings].key = kc;
            b->bindings[b->n_bindings].bit = def->bit;
            b->n_bindings++;

            if (kc == SDLK_TAB) b->tab_is_action = true;
        }

        char controller_val[64] = "";
        char controller_key[96];
        snprintf(controller_key, sizeof(controller_key), "%s.controller", def->name);
        bool has_controller_binding = false;
        if (b->ini_section)
            has_controller_binding = gemu_ini_read(b->ini_section, controller_key,
                                                   controller_val, sizeof(controller_val));
        const char *controller_name = has_controller_binding
                                    ? controller_val
                                    : default_controller_binding(def);

        if (controller_name && strncmp(controller_name, "Controller ", 11) == 0 &&
            b->n_controller_bindings < SDL_MAX_BINDINGS) {
            const char *name = controller_name + 11;
            size_t len = strlen(name);
            int axis_dir = 0;
            char axis_name[64];
            snprintf(axis_name, sizeof(axis_name), "%s", name);
            if (len > 1 && (axis_name[len - 1] == '+' || axis_name[len - 1] == '-')) {
                axis_dir = axis_name[len - 1] == '-' ? -1 : 1;
                axis_name[len - 1] = '\0';
            }

            if (axis_dir) {
                SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(axis_name);
                if (axis != SDL_CONTROLLER_AXIS_INVALID) {
                    b->controller_bindings[b->n_controller_bindings++] = (SdlControllerBinding){
                        .button = SDL_CONTROLLER_BUTTON_INVALID,
                        .axis = axis,
                        .axis_dir = axis_dir,
                        .bit = def->bit,
                    };
                }
            } else {
                SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(name);
                if (button != SDL_CONTROLLER_BUTTON_INVALID) {
                    b->controller_bindings[b->n_controller_bindings++] = (SdlControllerBinding){
                        .button = button,
                        .axis = SDL_CONTROLLER_AXIS_INVALID,
                        .axis_dir = 0,
                        .bit = def->bit,
                    };
                }
            }
        }
    }
}

static const char *default_controller_binding(const GemuActionDef *def) {
    if (!def || !def->name) return NULL;
    if (strcasecmp(def->name, "A") == 0)      return "Controller A";
    if (strcasecmp(def->name, "B") == 0)      return "Controller B";
    if (strcasecmp(def->name, "Select") == 0) return "Controller Back";
    if (strcasecmp(def->name, "Start") == 0)  return "Controller Start";
    if (strcasecmp(def->name, "Up") == 0)     return "Controller dpup";
    if (strcasecmp(def->name, "Down") == 0)   return "Controller dpdown";
    if (strcasecmp(def->name, "Left") == 0)   return "Controller dpleft";
    if (strcasecmp(def->name, "Right") == 0)  return "Controller dpright";
    return NULL;
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

static void open_first_controller(SdlBackend *b) {
    if (b->controller) return;
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) continue;
        b->controller = SDL_GameControllerOpen(i);
        if (b->controller) {
            SDL_Log("game controller: %s", SDL_GameControllerName(b->controller));
            break;
        }
    }
}

static uint32_t controller_mask(const SdlBackend *b) {
    if (!b->controller) return 0;
    uint32_t mask = 0;
    for (int i = 0; i < b->n_controller_bindings; i++) {
        if (b->controller_bindings[i].button != SDL_CONTROLLER_BUTTON_INVALID) {
            if (SDL_GameControllerGetButton(b->controller, b->controller_bindings[i].button))
                mask |= b->controller_bindings[i].bit;
        } else if (b->controller_bindings[i].axis != SDL_CONTROLLER_AXIS_INVALID) {
            Sint16 v = SDL_GameControllerGetAxis(b->controller, b->controller_bindings[i].axis);
            if ((b->controller_bindings[i].axis_dir < 0 && v < -SDL_CONTROLLER_DEADZONE) ||
                (b->controller_bindings[i].axis_dir > 0 && v >  SDL_CONTROLLER_DEADZONE))
                mask |= b->controller_bindings[i].bit;
        }
    }
    return mask;
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

static void sdl_do_set_title(GemuDisplay *d, const char *title);

/* Reflect mouse-capture state in the window title - capturing hides the OS
 * cursor and grabs it, which is confusing without a visible hint for how to
 * get it back. */
static void sdl_update_capture_title(GemuDisplay *d, SdlBackend *b) {
    char title[200];
    if (b->mouse_captured)
        snprintf(title, sizeof(title), "%s - Press Ctrl-Middle Click To Release Input",
                 d->title[0] ? d->title : "GEMU");
    else
        snprintf(title, sizeof(title), "%s", d->title[0] ? d->title : "GEMU");
    sdl_do_set_title(d, title);
}

static void sdl_set_mouse_captured(GemuDisplay *d, SdlBackend *b, bool captured) {
    SDL_SetRelativeMouseMode(captured ? SDL_TRUE : SDL_FALSE);
    SDL_CaptureMouse(captured ? SDL_TRUE : SDL_FALSE);
    b->mouse_captured = captured;
    if (!captured) {
        d->pointer.x = d->pointer.y = -1;
        d->pointer.rel_x = d->pointer.rel_y = 0;
        d->pointer.button = false;
        d->pointer.pressed = false;
        d->pointer.right_button = false;
        d->pointer.right_pressed = false;
        SDL_GetRelativeMouseState(NULL, NULL); /* drop any pending warp/grab delta */
    }
    sdl_update_capture_title(d, b);
}

static void sdl_do_render(GemuDisplay *d, const uint32_t *argb, int w, int h) {
    SdlBackend *b = d->backend;
    gemu_video_sdl_present_argb(b->video, argb, w, h);
}

static uint32_t sdl_do_poll(GemuDisplay *d) {
    SdlBackend *b = d->backend;
    SDL_Window *win = gemu_video_sdl_get_window(b->video);
    bool focused = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;

    /* Detect menu close → reload INI bindings */
    bool menu_open = b->menu && input_menu_is_open(b->menu);
    if (b->menu_was_open && !menu_open)
        build_bindings(b);
    b->menu_was_open = menu_open;

    int win_w = 0, win_h = 0, out_w = 0, out_h = 0;
    if (win) SDL_GetWindowSize(win, &win_w, &win_h);
    SDL_GetRendererOutputSize(gemu_video_sdl_get_renderer(b->video), &out_w, &out_h);

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        /* The binding screen swallows all events while open */
        if (menu_open) {
            input_menu_handle_event(b->menu, &ev);
            menu_open = input_menu_is_open(b->menu);
            continue;
        }

        if (b->menubar) {
            /* The menu key - Ctrl+Shift+Tab on every machine, the binding
             * screen's "Menu" key (Tab) or controller button where the guest
             * has no keyboard to type it on - puts focus on the menu bar. */
            bool combo = ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_TAB &&
                         (ev.key.keysym.mod & KMOD_CTRL) && (ev.key.keysym.mod & KMOD_SHIFT);
            bool own_key = b->menu && (b->menu_plain_key || ev.type != SDL_KEYDOWN) &&
                           input_menu_event_opens(b->menu, &ev);
            if (!gemu_menubar_active(b->menubar) && (combo || own_key)) {
                gemu_menubar_focus(b->menubar);
                continue;
            }
            if (gemu_menubar_handle_event(b->menubar, &ev, win_w, win_h, out_w, out_h)) {
                menu_open = b->menu && input_menu_is_open(b->menu);   /* Input > Configure */
                continue;
            }
        }

        switch (ev.type) {
        case SDL_CONTROLLERDEVICEADDED:
            open_first_controller(b);
            break;

        case SDL_CONTROLLERDEVICEREMOVED:
            if (b->controller) {
                SDL_GameControllerClose(b->controller);
                b->controller = NULL;
            }
            open_first_controller(b);
            break;

        case SDL_QUIT:
            d->quit = true;
            break;

        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
                /* Only quit if it's the main (NES) window being closed.
                 * Closing a secondary window (e.g. ROB display) must not
                 * terminate the emulator - that window handles itself. */
                SDL_Window *main_win = gemu_video_sdl_get_window(b->video);
                if (!main_win ||
                    ev.window.windowID == SDL_GetWindowID(main_win))
                    d->quit = true;
            }
            break;

        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (!b->capture_pointer || b->mouse_captured)
                gemu_video_sdl_mouse_logical(b->video,
                                             &d->pointer.x, &d->pointer.y);
            else
                d->pointer.x = d->pointer.y = -1;
            if (ev.type == SDL_MOUSEMOTION && focused &&
                (!b->capture_pointer || b->mouse_captured)) {
                d->pointer.rel_x += ev.motion.xrel;
                d->pointer.rel_y += ev.motion.yrel;
                b->saw_mouse_motion = true;
            }
            if (ev.type == SDL_MOUSEBUTTONDOWN &&
                ev.button.button == SDL_BUTTON_MIDDLE) {
                bool ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
                if (ctrl && b->mouse_captured) {
                    sdl_set_mouse_captured(d, b, false);
                } else if (!ctrl && !b->mouse_captured) {
                    sdl_set_mouse_captured(d, b, true);
                }
            }
            if (ev.type == SDL_MOUSEBUTTONDOWN &&
                ev.button.button == SDL_BUTTON_LEFT) {
                if (b->capture_pointer && !b->mouse_captured) {
                    sdl_set_mouse_captured(d, b, true);
                } else if (!b->capture_pointer) {
                    SDL_CaptureMouse(SDL_TRUE);
                }
                d->pointer.button = true;
                d->pointer.pressed = true;
            }
            if (ev.type == SDL_MOUSEBUTTONUP &&
                ev.button.button == SDL_BUTTON_LEFT) {
                d->pointer.button = false;
                if (!b->capture_pointer && !b->mouse_captured)
                    SDL_CaptureMouse(SDL_FALSE);
            }
            if (ev.type == SDL_MOUSEBUTTONDOWN &&
                ev.button.button == SDL_BUTTON_RIGHT) {
                if (b->capture_pointer && !b->mouse_captured) {
                    sdl_set_mouse_captured(d, b, true);
                }
                d->pointer.right_button = true;
                d->pointer.right_pressed = true;
            }
            if (ev.type == SDL_MOUSEBUTTONUP &&
                ev.button.button == SDL_BUTTON_RIGHT) {
                d->pointer.right_button = false;
            }
            break;

        case SDL_KEYDOWN: {
            SDL_Keycode kc = ev.key.keysym.sym;

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

    if (b->capture_pointer) {
        int rx = 0, ry = 0;
        SDL_GetRelativeMouseState(&rx, &ry); /* always drain, even unfocused,
                                              * so focus regaining doesn't
                                              * deliver a stale motion burst */
        if (!b->saw_mouse_motion && focused && b->mouse_captured) {
            d->pointer.rel_x += rx;
            d->pointer.rel_y += ry;
        }
    }
    b->saw_mouse_motion = false;

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


    return held_mask(b) | controller_mask(b);
}

static bool sdl_do_is_key_held(GemuDisplay *d, const char *name) {
    /* Keys pressed to drive the menu aren't the guest's. */
    SdlBackend *b = d->backend;
    if ((b->menu && input_menu_is_open(b->menu)) || gemu_menubar_active(b->menubar)) return false;
    SDL_Keycode kc = SDL_GetKeyFromName(name);
    if (kc == SDLK_UNKNOWN) return false;
    SDL_Scancode sc = SDL_GetScancodeFromKey(kc);
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    int numkeys = 0;
    const uint8_t *state = SDL_GetKeyboardState(&numkeys);
    return (int)sc < numkeys && state[sc];
}


static void sdl_do_reset_input_bindings(GemuDisplay *d) {
    SdlBackend *b = d->backend;
    if (!b->menu) return;
    input_menu_reset_keys(b->menu);
    build_bindings(b);
}

static void sdl_do_set_title(GemuDisplay *d, const char *title) {
    SdlBackend *b = d->backend;
    SDL_Window *win = b && b->video ? gemu_video_sdl_get_window(b->video) : NULL;
    if (win) SDL_SetWindowTitle(win, title ? title : "GEMU");
}

static void sdl_do_destroy(GemuDisplay *d) {
    SdlBackend *b = d->backend;
    if (!b) return;
    if (b->menu)  input_menu_destroy(b->menu);
    gemu_menubar_destroy(b->menubar);
    if (b->controller) SDL_GameControllerClose(b->controller);
    if (b->video) gemu_video_sdl_destroy(b->video);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    free(b);
    d->backend = NULL;
}

/* ── Menus: overlay drawing and the UI the menu bar can open ──────────────── */

static void menu_overlay_cb(void *ud, SDL_Renderer *r, int pixel_scale) {
    SdlBackend *b = ud;
    gemu_menubar_render(b->menubar, r);
    if (b->menu) input_menu_render(b->menu, r, pixel_scale);
}

static void ui_about(GemuDisplay *d) {
    SdlBackend *b = d->backend;
    gemu_menubar_about(b->menubar, gemu_display_about_text());
}

static void ui_choose_file(GemuDisplay *d, const char *title,
                           void (*done)(void *ud, const char *path), void *ud) {
    SdlBackend *b = d->backend;
    gemu_menubar_choose_file(b->menubar, title, done, ud);
}

static int ui_input_pages(GemuDisplay *d) {
    SdlBackend *b = d->backend;
    return input_menu_page_count(b->menu);
}

static void ui_input_page_label(GemuDisplay *d, int page, char *buf, size_t len) {
    SdlBackend *b = d->backend;
    input_menu_page_label(b->menu, page, buf, len);
}

static void ui_input_config(GemuDisplay *d, int page) {
    SdlBackend *b = d->backend;
    input_menu_open_page(b->menu, page);
}

/* Window scale = window width / framebuffer width (0 if not a whole
 * multiple, i.e. the user resized freely). */
static int ui_get_scale(GemuDisplay *d) {
    SdlBackend *b = d->backend;
    SDL_Window *win = gemu_video_sdl_get_window(b->video);
    int fw = gemu_video_sdl_width(b->video), w = 0, h = 0;
    if (!win || fw <= 0) return 0;
    SDL_GetWindowSize(win, &w, &h);
    return w % fw == 0 ? w / fw : 0;
}

static void ui_set_scale(GemuDisplay *d, int scale) {
    SdlBackend *b = d->backend;
    SDL_Window *win = gemu_video_sdl_get_window(b->video);
    if (!win || scale < 1) return;
    int w = gemu_video_sdl_width(b->video) * scale;
    int h = gemu_video_sdl_shown_height(b->video, w);
    SDL_SetWindowSize(win, w, h + gemu_menubar_height(h));
}

/* ── Create ──────────────────────────────────────────────────────────────── */

GemuDisplay *gemu_display_sdl_create(const GemuDisplayConfig *cfg) {
    char initial_title[200];
    gemu_window_title(initial_title, sizeof(initial_title), NULL, false);
    SdlBackend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->action_defs = cfg->actions;
    b->n_actions   = cfg->n_actions;
    b->ini_section = cfg->ini_section;
    b->capture_pointer = cfg->capture_pointer;

    int w = cfg->fb_width,  h = cfg->fb_height;
    int ww = cfg->window_width  ? cfg->window_width  : w * (cfg->scale ? cfg->scale : 2);
    int wh = cfg->window_height ? cfg->window_height : h * (cfg->scale ? cfg->scale : 2);
    if (!cfg->window_height && cfg->aspect_w > 0 && cfg->aspect_h > 0)
        wh = ww * cfg->aspect_h / cfg->aspect_w;    /* e.g. 720x350 -> 720x540 at 4:3 */
    wh += gemu_menubar_height(wh);                  /* the menu bar sits above the picture */

    b->video = gemu_video_sdl_create(&(GemuVideoSdlSpec){
        .title         = initial_title,
        .width         = w,
        .height        = h,
        .window_width  = ww,
        .window_height = wh,
        .aspect_w      = cfg->aspect_w,
        .aspect_h      = cfg->aspect_h,
        .renderer      = cfg->renderer,
        .log_prefix    = "gemu",
    });
    if (!b->video) { free(b); return NULL; }

    /* Printable keys are deliberately sourced from SDL_TEXTINPUT rather
     * than KEYDOWN so host layout/Shift handling stays correct.  Linux
     * backends commonly enable text events implicitly; Windows does not do
     * so reliably after focus/capture transitions. */
    SDL_StartTextInput();

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
        SDL_GameControllerEventState(SDL_ENABLE);
        open_first_controller(b);
    }

    /* Build action→key binding table */
    build_bindings(b);

    /* InputMenu: every window has one (at minimum: Reset / Close Menu /
     * Quit), opened with Tab, or Ctrl+Shift+Tab on keyboard machines
     * (no_menu) where Tab is the guest's.  Without an ini_section, with
     * no_rebind, or with no rebindable actions, pass 0 buttons so the Input
     * page is hidden. */
    b->menu_plain_key = !cfg->no_menu;
    {
        int nb = (!cfg->ini_section || cfg->no_rebind || !cfg->actions || cfg->n_actions <= 0)
                 ? 0 : cfg->n_actions;
        SDL_Keycode *defaults = nb ? calloc((size_t)nb, sizeof(SDL_Keycode)) : NULL;
        const char **names    = nb ? calloc((size_t)nb, sizeof(char *))      : NULL;
        const char **controller_defaults = nb ? calloc((size_t)nb, sizeof(char *)) : NULL;
        /* Translate GemuInputPage[] → InputMenuPage[] if provided */
        InputMenuPage *menu_pages = NULL;
        int n_menu_pages = 0;
        if (nb > 0 && cfg->n_pages > 0 && cfg->pages) {
            menu_pages = malloc((size_t)cfg->n_pages * sizeof(*menu_pages));
            if (menu_pages) {
                for (int p = 0; p < cfg->n_pages; p++) {
                    menu_pages[p].name      = cfg->pages[p].name;
                    menu_pages[p].n_buttons = cfg->pages[p].n_actions;
                }
                n_menu_pages = cfg->n_pages;
            }
        }

        if (nb == 0 || (defaults && names && controller_defaults)) {
            for (int i = 0; i < nb; i++) {
                names[i]               = cfg->actions[i].name;
                defaults[i]            = SDL_GetKeyFromName(cfg->actions[i].default_key);
                controller_defaults[i] = default_controller_binding(&cfg->actions[i]);
            }
            b->menu = input_menu_create(NULL, nb, names, defaults,
                                        controller_defaults, cfg->ini_section,
                                        n_menu_pages, menu_pages);
        }
        free(menu_pages);
        free(controller_defaults);
        free(names);
        free(defaults);
    }

    GemuDisplay *d = calloc(1, sizeof(*d));
    if (!d) { sdl_do_destroy(&(GemuDisplay){ .backend = b }); return NULL; }
    d->backend        = b;
    d->do_render      = sdl_do_render;
    d->do_poll        = sdl_do_poll;
    d->do_is_key_held = sdl_do_is_key_held;
    d->do_reset_input_bindings = sdl_do_reset_input_bindings;
    d->do_set_title   = sdl_do_set_title;
    d->do_destroy     = sdl_do_destroy;
    gemu_window_title(d->title, sizeof(d->title), NULL, false);
    d->pointer.x = d->pointer.y = -1;

    /* Window menu bar, drawn above the picture */
    d->ui_about       = ui_about;
    d->ui_choose_file = ui_choose_file;
    d->ui_get_scale   = ui_get_scale;
    d->ui_set_scale   = ui_set_scale;
    if (b->menu) {
        d->ui_input_pages      = ui_input_pages;
        d->ui_input_page_label = ui_input_page_label;
        d->ui_input_config     = ui_input_config;
    }
    b->menubar = gemu_menubar_create(d);
    gemu_video_sdl_set_top_margin(b->video, gemu_menubar_height);
    gemu_video_sdl_set_overlay(b->video, menu_overlay_cb, b);


    return d;
}
