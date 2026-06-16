#pragma once
#include "mos6502cfg.h"
#include <SDL2/SDL.h>

typedef struct {
    const char      *name;
    const char      *desc;
    NesDeviceType    type;
    /* Input menu / key rebinding: non-NULL only for devices with remappable
     * buttons (e.g. controller).  n_buttons is the count; button_names and
     * default_bindings each have n_buttons entries.  ini_section is the
     * INI section name (e.g. "nes-controller"). */
    int              n_buttons;
    const char     **button_names;
    const SDL_Keycode *default_bindings;
    const char      *ini_section;
} NesDeviceDesc;

const NesDeviceDesc *nes_device_list(int *count);
void                 nes_device_list_print(void);
const NesDeviceDesc *nes_device_find(const char *name);
