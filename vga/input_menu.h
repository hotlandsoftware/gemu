#pragma once
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* Shared 6×8 bitmap font (ASCII 32–122), reused by other overlay renderers
 * (e.g. the NES Lua scripting gui.text binding). Each byte is one glyph
 * column, LSB = top row; out-of-range characters fall back to '?'. */
#define GEMU_FONT_W 6
#define GEMU_FONT_H 8
const uint8_t *gemu_font_glyph(char ch);

typedef struct InputMenu InputMenu;

/* One named group of buttons shown as a separate page in the Input menu.
 * Pages partition the flat button array into consecutive slices. */
typedef struct {
    const char *name;      /* page heading, e.g. "nes-controller", "famicom-mic" */
    int         n_buttons; /* number of consecutive buttons on this page */
} InputMenuPage;

/* Create an input menu.
 * n_buttons / button_names / default_bindings define remappable keyboard keys.
 * default_controller_bindings defines a parallel controller binding set.
 * ini_section is the INI section name (e.g. "nes-controller").
 * n_pages / pages partition the buttons into named pages; pass 0/NULL for a
 * single page titled with ini_section (original behaviour). */
InputMenu *input_menu_create(SDL_Renderer *renderer,
                             int n_buttons,
                             const char **button_names,
                             const SDL_Keycode *default_bindings,
                             const char **default_controller_bindings,
                             const char *ini_section,
                             int n_pages,
                             const InputMenuPage *pages);

void input_menu_destroy(InputMenu *menu);

/* Toggle the menu open/closed.  Returns true if now open. */
bool input_menu_toggle(InputMenu *menu);
bool input_menu_event_opens(const InputMenu *menu, const SDL_Event *ev);
void input_menu_reset_keys(InputMenu *menu);

/* Process an SDL event.  Returns true if the event was consumed
 * (caller should NOT process it further). */
bool input_menu_handle_event(InputMenu *menu, const SDL_Event *ev);

/* Render the menu overlay on top of the current frame.
 * pixel_scale: integer window/fb scale (1 = 1:1, 2 = 2x window, etc.).
 * Call after the emulator frame is drawn but before SDL_RenderPresent. */
void input_menu_render(InputMenu *menu, SDL_Renderer *r, int pixel_scale);

/* Menu state queries */
bool input_menu_is_open(const InputMenu *menu);
bool input_menu_quit_requested(const InputMenu *menu);
bool input_menu_reset_requested(const InputMenu *menu);
void input_menu_clear_actions(InputMenu *menu);

/* Translate a physical key to a button index (0..n_buttons-1) using current
 * bindings.  Returns -1 if the key is not bound. */
int  input_menu_key_to_btn_index(const InputMenu *menu, SDL_Keycode key);
