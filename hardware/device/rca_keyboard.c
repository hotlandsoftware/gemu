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
