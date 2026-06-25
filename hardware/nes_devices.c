#include "nes_devices.h"
#include <stdio.h>
#include <string.h>

static const char *nes_ctrl_names[] = {
    "A", "B", "Select", "Start", "Up", "Down", "Left", "Right",
};
static const SDL_Keycode nes_ctrl_defaults[] = {
    SDLK_z, SDLK_x, SDLK_RSHIFT, SDLK_RETURN,
    SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
};

static const NesDeviceDesc devices[] = {
    {
        .name            = "famicom-keyboard",
        .desc            = "72-key Famicom Computer Keyboard",
        .type            = NES_DEVICE_KEYBOARD,
        .n_buttons       = 0,
        .button_names    = NULL,
        .default_bindings= NULL,
        .ini_section     = NULL,
    },
    {
        .name            = "nes-controller",
        .desc            = "NES/Famicom Standard Controller",
        .type            = NES_DEVICE_CONTROLLER,
        .n_buttons       = 8,
        .button_names    = nes_ctrl_names,
        .default_bindings= nes_ctrl_defaults,
        .ini_section     = "nes-controller",
    },
    {
        .name            = "zapper",
        .desc            = "NES Zapper (light gun)",
        .type            = NES_DEVICE_ZAPPER,
        .n_buttons       = 0,
        .button_names    = NULL,
        .default_bindings= NULL,
        .ini_section     = NULL,
    },
    {
        .name            = "rob",
        .desc            = "R.O.B. (Robotic Operating Buddy)",
        .type            = NES_DEVICE_ROB,
        .n_buttons       = 0,
        .button_names    = NULL,
        .default_bindings= NULL,
        .ini_section     = NULL,
    },
};

const NesDeviceDesc *nes_device_list(int *count) {
    if (count) *count = (int)(sizeof(devices) / sizeof(devices[0]));
    return devices;
}

void nes_device_list_print(void) {
    int n = 0;
    const NesDeviceDesc *devs = nes_device_list(&n);
    int maxw = 0;
    for (int i = 0; i < n; i++) {
        int w = (int)strlen(devs[i].name);
        if (w > maxw) maxw = w;
    }
    printf("Available devices:\n");
    for (int i = 0; i < n; i++)
        printf("  %-*s  %s\n", maxw, devs[i].name, devs[i].desc);
}

const NesDeviceDesc *nes_device_find(const char *name) {
    int n = 0;
    const NesDeviceDesc *devs = nes_device_list(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(name, devs[i].name) == 0)
            return &devs[i];
    return NULL;
}
