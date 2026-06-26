#include "rca_keyboard.h"
#include <stdbool.h>

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
