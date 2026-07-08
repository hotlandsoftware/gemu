#pragma once

#include "rca.h"
#include <SDL2/SDL.h>
#include <stdbool.h>

void        rca_device_attach     (RcaConfig *cfg, RcaKeyboardType dev);
bool        rca_vip_has_keypad    (const RcaConfig *cfg);
bool        rca_vip_has_vp601     (const RcaConfig *cfg);
const char *rca_vip_keyboard_name (RcaKeyboardType keyboard);

int rca_vp601_vnc_keysym_to_ascii (uint32_t sym);
