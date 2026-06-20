#pragma once
#include <SDL2/SDL.h>
#include <stdbool.h>

typedef struct InputMenu InputMenu;

/* Create an input menu.
 * n_buttons / button_names / default_bindings define the remappable keys
 * (e.g. NES controller: 8 buttons named A,B,START,... with defaults z,x,return,...).
 * ini_section is the INI section name (e.g. "nes-controller"). */
InputMenu *input_menu_create(SDL_Renderer *renderer,
                             int n_buttons,
                             const char **button_names,
                             const SDL_Keycode *default_bindings,
                             const char *ini_section);

void input_menu_destroy(InputMenu *menu);

/* Toggle the menu open/closed.  Returns true if now open. */
bool input_menu_toggle(InputMenu *menu);

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
