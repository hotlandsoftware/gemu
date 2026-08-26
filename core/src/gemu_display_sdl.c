#include "gemu_display_priv.h"
#include "gemu/video.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define IDM_FILE_RESET    0x0101
#  define IDM_FILE_QUIT     0x0102
#  define IDM_DEBUG_HEXEDIT 0x0103
#endif

/* InputMenu lives in gemu-mos/src/vga/ but is included via the build's
 * include path (same hack that lets rca share it). */
#include "input_menu.h"

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
    InputMenu    *menu;

    /* key → action bit mapping, built from action table + INI overrides */
    struct { SDL_Keycode key; uint32_t bit; } bindings[SDL_MAX_BINDINGS];
    int n_bindings;
    SdlControllerBinding controller_bindings[SDL_MAX_BINDINGS];
    int n_controller_bindings;
    SDL_GameController *controller;

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

    /* Resize snap: record last RESIZED dimensions + time; snap 150ms after
     * the last event so we never call SDL_SetWindowSize mid-drag (which
     * conflicts with the WM's pointer grab on X11 and causes the window to
     * move instead of resize). */
    int    pending_snap_w;
    int    pending_snap_h;
    Uint32 pending_snap_t;
    bool   mouse_captured;
    bool   capture_pointer;
    bool   saw_mouse_motion;

#ifdef _WIN32
    void (*hex_toggle_cb)(void *ud);
    void  *hex_toggle_ud;
#endif
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

    /* Apply debounced resize snap: fire 150ms after the last RESIZED event.
     * We never snap mid-drag because calling SDL_SetWindowSize while the WM
     * holds the pointer grab causes the window to move on X11. */
    if (b->pending_snap_w > 0 &&
        SDL_TICKS_PASSED(SDL_GetTicks(), b->pending_snap_t + 150)) {
        gemu_video_sdl_snap_resize(b->video, b->pending_snap_w, b->pending_snap_h);
        b->pending_snap_w = 0;
    }

    /* Detect menu close → reload INI bindings */
    bool menu_open = b->menu && input_menu_is_open(b->menu);
    if (b->menu_was_open && !menu_open)
        build_bindings(b);
    b->menu_was_open = menu_open;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (!menu_open && b->menu && input_menu_event_opens(b->menu, &ev)) {
            input_menu_toggle(b->menu);
            menu_open = true;
            continue;
        }

        /* Menu swallows all events while open */
        if (menu_open) {
            input_menu_handle_event(b->menu, &ev);
            menu_open = input_menu_is_open(b->menu);
            continue;
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
            } else if (ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                b->pending_snap_w = ev.window.data1;
                b->pending_snap_h = ev.window.data2;
                b->pending_snap_t = SDL_GetTicks();
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

#ifdef _WIN32
        case SDL_SYSWMEVENT:
            if (ev.syswm.msg->subsystem == SDL_SYSWM_WINDOWS &&
                ev.syswm.msg->msg.win.msg == WM_COMMAND) {
                switch (LOWORD(ev.syswm.msg->msg.win.wParam)) {
                case IDM_FILE_RESET: d->reset = true; break;
                case IDM_FILE_QUIT:  d->quit  = true; break;
                case IDM_DEBUG_HEXEDIT:
                    if (b->hex_toggle_cb) b->hex_toggle_cb(b->hex_toggle_ud);
                    break;
                }
            }
            break;
#endif

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
    (void)d;
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
    if (b->controller) SDL_GameControllerClose(b->controller);
    if (b->video) gemu_video_sdl_destroy(b->video);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    free(b);
    d->backend = NULL;
}

/* ── Overlay callback for InputMenu ─────────────────────────────────────── */

static void menu_overlay_cb(void *ud, SDL_Renderer *r, int pixel_scale) {
    input_menu_render((InputMenu *)ud, r, pixel_scale);
}

/* ── Create ──────────────────────────────────────────────────────────────── */

GemuDisplay *gemu_display_sdl_create(const GemuDisplayConfig *cfg) {
    SdlBackend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->action_defs = cfg->actions;
    b->n_actions   = cfg->n_actions;
    b->ini_section = cfg->ini_section;
    b->capture_pointer = cfg->capture_pointer;

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

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
        SDL_GameControllerEventState(SDL_ENABLE);
        open_first_controller(b);
    }

    /* Build action→key binding table */
    build_bindings(b);

    /* InputMenu: create whenever ini_section is set so Tab always opens a menu
     * (at minimum: Reset / Close Menu / Quit).  When no_rebind is set or there
     * are no rebindable actions, pass 0 buttons so the Input page is hidden.
     * no_menu suppresses this entirely - Tab does nothing. */
    if (cfg->ini_section && !cfg->no_menu) {
        int nb = (cfg->no_rebind || !cfg->actions || cfg->n_actions <= 0)
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
        if (b->menu)
            gemu_video_sdl_set_overlay(b->video, menu_overlay_cb, b->menu);
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
    snprintf(d->title, sizeof(d->title), "%s", cfg->title ? cfg->title : "GEMU");
    d->pointer.x = d->pointer.y = -1;

#ifdef _WIN32
    /* Attach a native Win32 menu bar: File > Reset / Quit, and (when the
     * machine provides one - see GemuDisplayGtkExtras) Debug > Hex Editor.
     * The "gtk" name is historical: this struct predates the native Win32
     * menu and was originally GTK-only, but its fields are plain callbacks
     * that any backend can use. */
    if (cfg->gtk) {
        b->hex_toggle_cb = cfg->gtk->hex_toggle_cb;
        b->hex_toggle_ud = cfg->gtk->hex_toggle_ud;
    }
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    {
        SDL_SysWMinfo wm;
        SDL_VERSION(&wm.version);
        SDL_Window *win = gemu_video_sdl_get_window(b->video);
        if (win && SDL_GetWindowWMInfo(win, &wm) &&
            wm.subsystem == SDL_SYSWM_WINDOWS) {
            HWND hwnd  = wm.info.win.window;
            HMENU bar  = CreateMenu();
            HMENU file = CreatePopupMenu();
            AppendMenuA(file, MF_STRING,    IDM_FILE_RESET, "&Reset");
            AppendMenuA(file, MF_SEPARATOR, 0,              NULL);
            AppendMenuA(file, MF_STRING,    IDM_FILE_QUIT,  "&Quit\tEsc");
            AppendMenuA(bar,  MF_POPUP, (UINT_PTR)file, "&File");
            if (b->hex_toggle_cb) {
                HMENU debug = CreatePopupMenu();
                AppendMenuA(debug, MF_STRING, IDM_DEBUG_HEXEDIT, "&Hex Editor");
                AppendMenuA(bar,   MF_POPUP, (UINT_PTR)debug, "&Debug");
            }
            SetMenu(hwnd, bar);
            DrawMenuBar(hwnd);
        }
    }
#endif

    return d;
}
