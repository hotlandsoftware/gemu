#pragma once

#include "rca.h"
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    const char *desc;
    RcaKeyboardType keyboard;
    uint32_t supported_machines;
} RcaDeviceDesc;

const RcaDeviceDesc *rca_device_list(int *count);
void rca_device_list_print(void);
const RcaDeviceDesc *rca_device_find(const char *name);
bool rca_device_parse(const char *name, RcaKeyboardType *out);
bool rca_device_supports_machine(const RcaDeviceDesc *dev, RcaMachineType machine);
void rca_device_supported_machines(const RcaDeviceDesc *dev, char *buf, size_t len);
void rca_device_attach(RcaConfig *cfg, RcaKeyboardType dev);

bool rca_vip_has_keypad(const RcaConfig *cfg);
bool rca_vip_has_vp601(const RcaConfig *cfg);
const char *rca_vip_keyboard_name(RcaKeyboardType keyboard);

int rca_vip_keypad_keycode_to_hex(SDL_Keycode sym);
int rca_vp601_sdl_key_to_ascii(SDL_Keycode sym, SDL_Keymod mod);
int rca_vp601_vnc_keysym_to_ascii(uint32_t sym);
