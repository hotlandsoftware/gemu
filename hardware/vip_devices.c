#include "vip_devices.h"
#include <stdio.h>
#include <string.h>

#define RCA_MACHINE_F(machine) (1u << (machine))
#define RCA_DEVICE_VIP_ONLY RCA_MACHINE_F(RCA_MACHINE_COSMAC_VIP)

static const RcaDeviceDesc devices[] = {
    {"vp601",      "RCA COSMAC VIP VP601 ASCII/QWERTY keyboard", RCA_KEYBOARD_VP601, RCA_DEVICE_VIP_ONLY},
    {"vip-keypad", "COSMAC VIP 16-key hexadecimal keypad",       RCA_KEYBOARD_KEYPAD, RCA_DEVICE_VIP_ONLY},
    {"keypad",     "Alias for vip-keypad",                       RCA_KEYBOARD_KEYPAD, RCA_DEVICE_VIP_ONLY},
    {"keyboard",   "Generic ASCII/QWERTY keyboard",              RCA_KEYBOARD_GENERIC, RCA_DEVICE_VIP_ONLY},
};

static const char *rca_machine_type_name(RcaMachineType machine) {
    switch (machine) {
    case RCA_MACHINE_GENERIC:    return "generic";
    case RCA_MACHINE_COSMAC_VIP: return "cosmac-vip";
    case RCA_MACHINE_DESTROYER:  return "destroyer";
    default:                     return "unknown";
    }
}

const RcaDeviceDesc *rca_device_list(int *count) {
    if (count)
        *count = (int)(sizeof(devices) / sizeof(devices[0]));
    return devices;
}

void rca_device_list_print(void) {
    int n = 0;
    const RcaDeviceDesc *devs = rca_device_list(&n);
    int maxw = 0;
    for (int i = 0; i < n; i++) {
        int w = (int)strlen(devs[i].name);
        if (w > maxw) maxw = w;
    }

    printf("Available devices:\n");
    for (int i = 0; i < n; i++) {
        char machines[96];
        rca_device_supported_machines(&devs[i], machines, sizeof(machines));
        printf("  %-*s  %s [%s]\n", maxw, devs[i].name, devs[i].desc, machines);
    }
}

const RcaDeviceDesc *rca_device_find(const char *name) {
    int n = 0;
    const RcaDeviceDesc *devs = rca_device_list(&n);
    for (int i = 0; i < n; i++) {
        if (strcmp(name, devs[i].name) == 0)
            return &devs[i];
    }
    return NULL;
}

bool rca_device_parse(const char *name, RcaKeyboardType *out) {
    const RcaDeviceDesc *dev = rca_device_find(name);
    if (dev) {
        if (out) *out = dev->keyboard;
        return true;
    }
    return false;
}

bool rca_device_supports_machine(const RcaDeviceDesc *dev, RcaMachineType machine) {
    return dev && (dev->supported_machines & RCA_MACHINE_F(machine)) != 0;
}

void rca_device_supported_machines(const RcaDeviceDesc *dev, char *buf, size_t len) {
    if (!buf || len == 0)
        return;
    buf[0] = '\0';
    if (!dev)
        return;

    bool first = true;
    for (int machine = RCA_MACHINE_GENERIC; machine <= RCA_MACHINE_DESTROYER; machine++) {
        if ((dev->supported_machines & RCA_MACHINE_F(machine)) == 0)
            continue;
        size_t used = strlen(buf);
        int n = snprintf(buf + used, len - used, "%s%s",
                         first ? "" : ", ", rca_machine_type_name((RcaMachineType)machine));
        if (n < 0 || (size_t)n >= len - used)
            break;
        first = false;
    }
}

void rca_device_attach(RcaConfig *cfg, RcaKeyboardType dev) {
    if (dev == RCA_KEYBOARD_VP601) {
        cfg->keyboard = (cfg->keyboard == RCA_KEYBOARD_KEYPAD ||
                         cfg->keyboard == RCA_KEYBOARD_BOTH)
                      ? RCA_KEYBOARD_BOTH : RCA_KEYBOARD_VP601;
    } else if (dev == RCA_KEYBOARD_KEYPAD) {
        cfg->keyboard = (cfg->keyboard == RCA_KEYBOARD_VP601 ||
                         cfg->keyboard == RCA_KEYBOARD_BOTH)
                      ? RCA_KEYBOARD_BOTH : RCA_KEYBOARD_KEYPAD;
    } else {
        cfg->keyboard = dev;
    }
}

bool rca_vip_has_keypad(const RcaConfig *cfg) {
    return cfg->keyboard == RCA_KEYBOARD_KEYPAD ||
           cfg->keyboard == RCA_KEYBOARD_BOTH;
}

bool rca_vip_has_vp601(const RcaConfig *cfg) {
    return cfg->keyboard == RCA_KEYBOARD_VP601 ||
           cfg->keyboard == RCA_KEYBOARD_BOTH;
}

const char *rca_vip_keyboard_name(RcaKeyboardType keyboard) {
    switch (keyboard) {
    case RCA_KEYBOARD_NONE:    return "none";
    case RCA_KEYBOARD_KEYPAD:  return "keypad";
    case RCA_KEYBOARD_VP601:   return "vp601";
    case RCA_KEYBOARD_BOTH:    return "both";
    case RCA_KEYBOARD_GENERIC: return "generic";
    default:                   return "unknown";
    }
}

int rca_vip_keypad_keycode_to_hex(SDL_Keycode sym) {
    static const SDL_Keycode key_map[16] = {
        SDLK_x, SDLK_1, SDLK_2, SDLK_3,
        SDLK_q, SDLK_w, SDLK_e, SDLK_a,
        SDLK_s, SDLK_d, SDLK_z, SDLK_c,
        SDLK_4, SDLK_r, SDLK_f, SDLK_v,
    };

    for (int k = 0; k < 16; k++)
        if (sym == key_map[k])
            return k;
    return -1;
}

int rca_vp601_sdl_key_to_ascii(SDL_Keycode sym, SDL_Keymod mod) {
    bool shift = (mod & KMOD_SHIFT) != 0;

    if (sym >= SDLK_a && sym <= SDLK_z)
        return (int)('A' + (sym - SDLK_a));
    if (sym >= SDLK_0 && sym <= SDLK_9) {
        static const char shifted[] = ")!@#$%^&*(";
        return shift ? shifted[sym - SDLK_0] : (int)('0' + (sym - SDLK_0));
    }
    if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) return '\r';
    if (sym == SDLK_BACKSPACE) return '\b';
    if (sym == SDLK_ESCAPE) return 0x1B;
    if (sym == SDLK_SPACE) return ' ';

    switch (sym) {
    case SDLK_MINUS:        return shift ? '_' : '-';
    case SDLK_EQUALS:       return shift ? '+' : '=';
    case SDLK_LEFTBRACKET:  return shift ? '{' : '[';
    case SDLK_RIGHTBRACKET: return shift ? '}' : ']';
    case SDLK_BACKSLASH:    return shift ? '|' : '\\';
    case SDLK_SEMICOLON:    return shift ? ':' : ';';
    case SDLK_QUOTE:        return shift ? '"' : '\'';
    case SDLK_COMMA:        return shift ? '<' : ',';
    case SDLK_PERIOD:       return shift ? '>' : '.';
    case SDLK_SLASH:        return shift ? '?' : '/';
    default:                break;
    }

    return -1;
}

int rca_vp601_vnc_keysym_to_ascii(uint32_t sym) {
    if (sym >= 'a' && sym <= 'z') return (int)(sym - 32u);
    if (sym >= 0x20 && sym <= 0x7E) return (int)sym;
    if (sym == 0x0D || sym == 0x0A || sym == 0xFF0D || sym == 0xFF8D) return '\r';
    if (sym == 0x08 || sym == 0x7F || sym == 0xFF08 || sym == 0xFFFF) return '\b';
    if (sym == 0xFF1B) return 0x1B;
    return -1;
}
