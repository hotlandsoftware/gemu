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



int rca_vp601_vnc_keysym_to_ascii(uint32_t sym) {
    if (sym >= 'a' && sym <= 'z') return (int)(sym - 32u);
    if (sym >= 0x20 && sym <= 0x7E) return (int)sym;
    if (sym == 0x0D || sym == 0x0A || sym == 0xFF0D || sym == 0xFF8D) return '\r';
    if (sym == 0x08 || sym == 0x7F || sym == 0xFF08 || sym == 0xFFFF) return '\b';
    if (sym == 0xFF1B) return 0x1B;
    return -1;
}
