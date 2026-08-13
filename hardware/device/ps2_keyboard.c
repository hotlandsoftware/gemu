/* Generic PS/2 Keyboard Device */

#include "ps2_keyboard.h"

#include <string.h>

enum {
    PS2_ACK = 0xfa,
    PS2_RESEND = 0xfe,
};

static void queue_byte(Ps2Keyboard *kbd, uint8_t value) {
    if (kbd->queue_pos) {
        memmove(kbd->queue, kbd->queue + kbd->queue_pos,
                kbd->queue_len - kbd->queue_pos);
        kbd->queue_len -= kbd->queue_pos;
        kbd->queue_pos = 0;
    }
    if (kbd->queue_len < sizeof(kbd->queue)) {
        kbd->queue[kbd->queue_len++] = value;
        kbd->last_sent = value;
    }
}

static void defaults(Ps2Keyboard *kbd) {
    kbd->pending_command = 0;
    kbd->leds = 0;
    kbd->typematic = 0x2b;
    kbd->scan_set = 1;
    kbd->scanning = true;
}

void ps2_keyboard_init(Ps2Keyboard *kbd) {
    memset(kbd, 0, sizeof(*kbd));
    defaults(kbd);
}

bool ps2_keyboard_has_data(const Ps2Keyboard *kbd) {
    return kbd->queue_pos < kbd->queue_len;
}

uint8_t ps2_keyboard_read(Ps2Keyboard *kbd) {
    if (!ps2_keyboard_has_data(kbd))
        return 0xff;
    uint8_t value = kbd->queue[kbd->queue_pos++];
    if (kbd->queue_pos == kbd->queue_len)
        kbd->queue_pos = kbd->queue_len = 0;
    return value;
}

void ps2_keyboard_command(Ps2Keyboard *kbd, uint8_t command) {
    if (kbd->pending_command) {
        uint8_t pending = kbd->pending_command;
        kbd->pending_command = 0;
        queue_byte(kbd, PS2_ACK);
        if (pending == 0xed)
            kbd->leds = command & 7;
        else if (pending == 0xf3)
            kbd->typematic = command;
        else if (pending == 0xf0) {
            if (command == 0)
                queue_byte(kbd, kbd->scan_set);
            else if (command >= 1 && command <= 3)
                kbd->scan_set = command;
        }
        return;
    }
    switch (command) {
    case 0xff:                         /* reset */
        queue_byte(kbd, PS2_ACK);
        defaults(kbd);
        queue_byte(kbd, 0xaa);         /* BAT passed */
        break;
    case 0xf6:                         /* set defaults */
        defaults(kbd); queue_byte(kbd, PS2_ACK); break;
    case 0xf5:                         /* defaults + disable scanning */
        defaults(kbd); kbd->scanning = false; queue_byte(kbd, PS2_ACK); break;
    case 0xf4:                         /* enable scanning */
        kbd->scanning = true; queue_byte(kbd, PS2_ACK); break;
    case 0xf3: case 0xf0: case 0xed:   /* command has one parameter */
        kbd->pending_command = command; queue_byte(kbd, PS2_ACK); break;
    case 0xf2:                         /* identify MF2 keyboard */
        queue_byte(kbd, PS2_ACK); queue_byte(kbd, 0xab); queue_byte(kbd, 0x83); break;
    case 0xee:                         /* echo */
        queue_byte(kbd, 0xee); break;
    case 0xfe:                         /* resend */
        queue_byte(kbd, kbd->last_sent ? kbd->last_sent : PS2_RESEND); break;
    default:
        queue_byte(kbd, PS2_ACK); break;
    }
}

typedef struct KeyMap { uint32_t key; uint8_t scan; bool extended, shift; } KeyMap;

static bool keymap(uint32_t key, KeyMap *out) {
    static const uint8_t letters[26] = {
        0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
        0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c
    };
    static const struct { char normal, shifted; uint8_t scan; } punct[] = {
        {'1','!',0x02},{'2','@',0x03},{'3','#',0x04},{'4','$',0x05},
        {'5','%',0x06},{'6','^',0x07},{'7','&',0x08},{'8','*',0x09},
        {'9','(',0x0a},{'0',')',0x0b},{'-','_',0x0c},{'=','+',0x0d},
        {'[','{',0x1a},{']','}',0x1b},{'\\','|',0x2b},{';',':',0x27},
        {'\'','"',0x28},{'`','~',0x29},{',','<',0x33},{'.','>',0x34},
        {'/','?',0x35},{' ',' ',0x39}
    };
    *out = (KeyMap){ .key = key };
    if (key >= 'a' && key <= 'z') { out->scan = letters[key-'a']; return true; }
    if (key >= 'A' && key <= 'Z') {
        out->scan = letters[key-'A']; out->shift = true; return true;
    }
    for (unsigned i = 0; i < sizeof(punct)/sizeof(punct[0]); i++) {
        if (key == (uint8_t)punct[i].normal || key == (uint8_t)punct[i].shifted) {
            out->scan = punct[i].scan;
            out->shift = key == (uint8_t)punct[i].shifted &&
                         punct[i].shifted != punct[i].normal;
            return true;
        }
    }
    switch (key) {
    case '\r': case '\n': out->scan=0x1c; return true;
    case '\b': out->scan=0x0e; return true;
    case '\t': out->scan=0x0f; return true;
    case 0x1b: out->scan=0x01; return true;
    case 0x7f: case 0xffff: out->scan=0x53; out->extended=true; return true;
    case 0xff08: out->scan=0x0e; return true;
    case 0xff09: out->scan=0x0f; return true;
    case 0xff0d: out->scan=0x1c; return true;
    case 0xff1b: out->scan=0x01; return true;
    case 0xff50: out->scan=0x47; out->extended=true; return true; /* Home */
    case 0xff51: out->scan=0x4b; out->extended=true; return true; /* Left */
    case 0xff52: out->scan=0x48; out->extended=true; return true; /* Up */
    case 0xff53: out->scan=0x4d; out->extended=true; return true; /* Right */
    case 0xff54: out->scan=0x50; out->extended=true; return true; /* Down */
    case 0xff55: out->scan=0x49; out->extended=true; return true; /* PgUp */
    case 0xff56: out->scan=0x51; out->extended=true; return true; /* PgDn */
    case 0xff57: out->scan=0x4f; out->extended=true; return true; /* End */
    case 0xff63: out->scan=0x52; out->extended=true; return true; /* Insert */
    default: return false;
    }
}

void ps2_keyboard_key(Ps2Keyboard *kbd, uint32_t keysym, bool down) {
    KeyMap map;
    if (!kbd->scanning || !keymap(keysym, &map))
        return;
    if (map.shift && down) queue_byte(kbd, 0x2a);
    if (map.extended) queue_byte(kbd, 0xe0);
    queue_byte(kbd, down ? map.scan : (uint8_t)(map.scan | 0x80));
    if (map.shift && !down) queue_byte(kbd, 0xaa);
}

void ps2_keyboard_tap(Ps2Keyboard *kbd, uint32_t keysym) {
    ps2_keyboard_key(kbd, keysym, true);
    ps2_keyboard_key(kbd, keysym, false);
}
