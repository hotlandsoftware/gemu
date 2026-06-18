#ifndef _WIN32
#  define _POSIX_C_SOURCE 199309L
#endif
#include "kim1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static inline void kim1_sleep_ms(unsigned ms) { Sleep(ms); }
#else
static inline void kim1_sleep_ms(unsigned ms) {
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

static void kim1_reset_rriots(Kim1State *s);
static uint8_t kim1_mem_read(uint16_t addr, void *ud);
static void kim1_mem_write(uint16_t addr, uint8_t val, void *ud);
static void kim1_update_monitor_display(Kim1State *s);
static void kim1_panel_command(Kim1State *s, uint8_t key);

/* ── KIM-1 keypad action table ──────────────────────────────────────────── */

static const GemuActionDef kim1_actions[KIM1_NUM_ACTIONS] = {
    { "0",    KIM1_ACT_0,    "0" },
    { "1",    KIM1_ACT_1,    "1" },
    { "2",    KIM1_ACT_2,    "2" },
    { "3",    KIM1_ACT_3,    "3" },
    { "4",    KIM1_ACT_4,    "4" },
    { "5",    KIM1_ACT_5,    "5" },
    { "6",    KIM1_ACT_6,    "6" },
    { "7",    KIM1_ACT_7,    "7" },
    { "8",    KIM1_ACT_8,    "8" },
    { "9",    KIM1_ACT_9,    "9" },
    { "A",    KIM1_ACT_A,    "a" },
    { "B",    KIM1_ACT_B,    "b" },
    { "C",    KIM1_ACT_C,    "c" },
    { "D",    KIM1_ACT_D,    "d" },
    { "E",    KIM1_ACT_E,    "e" },
    { "F",    KIM1_ACT_F,    "f" },
    { "AD",   KIM1_ACT_AD,   "<" },
    { "DA",   KIM1_ACT_DA,   ">" },
    { "PC",   KIM1_ACT_PC,   "p" },
    { "+",    KIM1_ACT_PLUS, "+" },
    { "GO",   KIM1_ACT_GO,   "g" },
    { "ST",   KIM1_ACT_ST,   "s" },
    { "RS",   KIM1_ACT_RS,   "r" },
};

/* ── Hardware keyboard matrix (4 rows × 6 columns) ──────────────────────── *
 * Row selected by 74145 decoder (PB1-PB4 of u3): decoder values 0-3 = rows.
 * Columns read on PA0-PA5 (active-low when key pressed).
 *
 *        PA0      PA1      PA2      PA3      PA4       PA5
 * Row 0:  0        1        2        3        C         D
 * Row 1:  4        5        6        7        E         F
 * Row 2:  8        9        A        B        +        (nc)
 * Row 3: AD       DA       GO       ST       PC        RS
 */
static const uint32_t hw_keymap[4][6] = {
    { KIM1_ACT_0,  KIM1_ACT_1,  KIM1_ACT_2,  KIM1_ACT_3,  KIM1_ACT_C,    KIM1_ACT_D  },
    { KIM1_ACT_4,  KIM1_ACT_5,  KIM1_ACT_6,  KIM1_ACT_7,  KIM1_ACT_E,    KIM1_ACT_F  },
    { KIM1_ACT_8,  KIM1_ACT_9,  KIM1_ACT_A,  KIM1_ACT_B,  KIM1_ACT_PLUS, 0           },
    { KIM1_ACT_AD, KIM1_ACT_DA, KIM1_ACT_GO, KIM1_ACT_ST, KIM1_ACT_PC,   KIM1_ACT_RS },
};

/* ── Visual keypad layout (6 rows × 4 cols) for on-screen rendering ──────── */
static const uint32_t keypad_matrix[6][4] = {
    { KIM1_ACT_GO,  KIM1_ACT_ST,  KIM1_ACT_RS,  0             },
    { KIM1_ACT_AD,  KIM1_ACT_DA,  KIM1_ACT_PC,  KIM1_ACT_PLUS },
    { KIM1_ACT_C,   KIM1_ACT_D,   KIM1_ACT_E,   KIM1_ACT_F    },
    { KIM1_ACT_8,   KIM1_ACT_9,   KIM1_ACT_A,   KIM1_ACT_B    },
    { KIM1_ACT_4,   KIM1_ACT_5,   KIM1_ACT_6,   KIM1_ACT_7    },
    { KIM1_ACT_0,   KIM1_ACT_1,   KIM1_ACT_2,   KIM1_ACT_3    },
};

/* ── Key labels (for visual rendering) ──────────────────────────────────── */

static const char *key_labels[6][4] = {
    {"GO", "ST", "RS", "SST"},
    {"AD", "DA", "PC", "+" },
    {"C",  "D",  "E",  "F" },
    {"8",  "9",  "A",  "B" },
    {"4",  "5",  "6",  "7" },
    {"0",  "1",  "2",  "3" },
};

/* ── 5×7 bitmap font (7 bytes per char, MSB = leftmost pixel) ─────────────
 *
 * Characters: 0-9, A-F, +, and the letters in AD/DA/PC/GO/ST/RS.
 */

#define FONT_W  5
#define FONT_H  7

static const uint8_t font_data[128][FONT_H] = {
    ['0'] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    ['1'] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    ['2'] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    ['3'] = {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E},
    ['4'] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    ['5'] = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    ['6'] = {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    ['7'] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    ['8'] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    ['9'] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    ['A'] = {0x04, 0x0A, 0x11, 0x11, 0x1F, 0x11, 0x11},
    ['B'] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    ['C'] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    ['D'] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    ['E'] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    ['F'] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    ['+'] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},
    ['G'] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E},
    ['O'] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    ['P'] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    ['R'] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    ['S'] = {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E},
    ['T'] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
};

/* ── 7-segment segment geometry (scaled for KIM1_DIGIT_W=72, KIM1_DIGIT_H=110) */

static const struct { int x, y, w, h; } seg_rects[8] = {
    {12,   4, 48, 10},  /* a — top horizontal       */
    {58,  12,  8, 42},  /* b — top right vertical    */
    {58,  60,  8, 42},  /* c — bottom right vertical */
    {12, 100, 48, 10},  /* d — bottom horizontal     */
    { 6,  60,  8, 42},  /* e — bottom left vertical  */
    { 6,  12,  8, 42},  /* f — top left vertical     */
    {12,  52, 48, 10},  /* g — middle horizontal     */
    {66, 100,  8,  8},  /* dp — decimal point        */
};

/* ── Colours (0xAARRGGBB) ───────────────────────────────────────────────── */

#define COLOUR_LIT        0xFFFF2020u  /* bright red LED on      */
#define COLOUR_OFF        0xFF301010u  /* dim red LED off (visible) */
#define COLOUR_BG         0xFF000000u  /* black front panel       */
#define COLOUR_KEY_BG     0xFF6A6A68u  /* key background          */
#define COLOUR_KEY_BD     0xFF3F3F3Fu  /* key border              */
#define COLOUR_KEY_LIT    0xFF8A7A50u  /* key pressed highlight   */
#define COLOUR_KEY_TEXT   0xFFFFFFFFu  /* key label text          */

static const uint32_t kim1_vnc_palette[] = {
    0x000000u, /* background */
    0x301010u, /* LED off    */
    0xFF2020u, /* LED lit    */
    0x3F3F3Fu, /* key border */
    0x6A6A68u, /* key body   */
    0x8A7A50u, /* key active */
    0xFFFFFFu, /* key text   */
};

/* ── Framebuffer helpers ────────────────────────────────────────────────── */

static void fill_rect(uint32_t *fb, int fb_w, int x, int y,
                      int w, int h, uint32_t colour) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > KIM1_FB_HEIGHT) h = KIM1_FB_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            fb[(y + row) * fb_w + (x + col)] = colour;
}

static void draw_rect_border(uint32_t *fb, int fb_w, int x, int y,
                             int w, int h, int t, uint32_t c) {
    fill_rect(fb, fb_w, x, y, w, t, c);             /* top    */
    fill_rect(fb, fb_w, x, y + h - t, w, t, c);     /* bottom */
    fill_rect(fb, fb_w, x, y + t, t, h - 2 * t, c); /* left   */
    fill_rect(fb, fb_w, x + w - t, y + t, t, h - 2 * t, c); /* right */
}

static uint8_t kim1_vnc_index(uint32_t argb) {
    uint32_t rgb = argb & 0x00FFFFFFu;
    switch (rgb) {
    case 0x000000u: return 0;
    case 0x301010u: return 1;
    case 0xFF2020u: return 2;
    case 0x3F3F3Fu: return 3;
    case 0x6A6A68u: return 4;
    case 0x8A7A50u: return 5;
    case 0xFFFFFFu: return 6;
    default:
        if (((rgb >> 16) & 0xFFu) > 0xC0u && (rgb & 0xFFFFu) < 0x8080u)
            return 2;
        if ((rgb & 0xFFu) > 0xC0u && ((rgb >> 8) & 0xFFu) > 0xC0u)
            return 6;
        return 0;
    }
}

static void kim1_update_vnc(Kim1State *s) {
    if (!s->vnc) return;

    for (int y = 0; y < KIM1_PRESENT_HEIGHT; y++) {
        int sy = y * KIM1_FB_HEIGHT / KIM1_PRESENT_HEIGHT;
        for (int x = 0; x < KIM1_PRESENT_WIDTH; x++) {
            int sx = x * KIM1_FB_WIDTH / KIM1_PRESENT_WIDTH;
            s->vnc_fb[y * KIM1_PRESENT_WIDTH + x] =
                kim1_vnc_index(s->fb[sy * KIM1_FB_WIDTH + sx]);
        }
    }
    gemu_vnc_update(s->vnc, s->vnc_fb, KIM1_PRESENT_WIDTH, KIM1_PRESENT_HEIGHT);
}

/* ── Font rendering ─────────────────────────────────────────────────────── */

static void draw_char_scaled(uint32_t *fb, int fb_w, int x, int y,
                             char ch, int scale, uint32_t colour) {
    int c = (int)(unsigned char)ch;
    if (c < 0 || c > 127 || scale <= 0) return;
    const uint8_t *bits = font_data[c];
    if (!bits[0] && !bits[1] && !bits[2] && !bits[3] &&
        !bits[4] && !bits[5] && !bits[6]) return;

    for (int row = 0; row < FONT_H; row++) {
        uint8_t b = bits[row];
        for (int col = 0; col < FONT_W; col++) {
            if (b & (0x10u >> col))
                fill_rect(fb, fb_w, x + col * scale, y + row * scale,
                          scale, scale, colour);
        }
    }
}

static void draw_text_scaled(uint32_t *fb, int fb_w, int x, int y,
                             const char *s, int scale, uint32_t colour) {
    while (*s) {
        draw_char_scaled(fb, fb_w, x, y, *s, scale, colour);
        x += (FONT_W + 1) * scale;
        s++;
    }
}

/* ── Keypad rendering ───────────────────────────────────────────────────── */

static void draw_keypad(Kim1State *s) {
    uint32_t *fb = s->fb;
    int fw = KIM1_FB_WIDTH;

    int kw = KIM1_KEY_W, kh = KIM1_KEY_H, kg = KIM1_KEY_GAP;
    int total_w = kw * KIM1_KEYPAD_COLS + kg * (KIM1_KEYPAD_COLS - 1);
    int ox = (KIM1_FB_WIDTH - total_w) / 2;
    int oy = KIM1_KEYPAD_Y;

    for (int row = 0; row < KIM1_KEYPAD_ROWS; row++) {
        for (int col = 0; col < KIM1_KEYPAD_COLS; col++) {
            const char *label = key_labels[row][col];
            if (!label) continue;

            int kx = ox + col * (kw + kg);
            int ky = oy + row * (kh + kg);

            uint32_t bit = keypad_matrix[row][col];
            bool pressed = bit && (s->keypad_held & bit);

            bool inactive = strcmp(label, "SST") == 0;
            uint32_t key_bg = inactive ? COLOUR_BG :
                              (pressed ? COLOUR_KEY_LIT : COLOUR_KEY_BG);

            fill_rect(fb, fw, kx + 5, ky + 5, kw - 10, kh - 10, key_bg);
            draw_rect_border(fb, fw, kx, ky, kw, kh, 5,
                             pressed ? COLOUR_KEY_LIT : COLOUR_KEY_BD);

            int scale = (strlen(label) >= 3) ? 4 : ((strlen(label) == 2) ? 5 : 6);
            int text_w = ((int)strlen(label) * FONT_W +
                         ((int)strlen(label) - 1)) * scale;
            int text_h = FONT_H * scale;
            int tx = kx + (kw - text_w) / 2;
            int ty = ky + (kh - text_h) / 2;
            draw_text_scaled(fb, fw, tx, ty, label, scale, COLOUR_KEY_TEXT);
        }
    }
}

static uint32_t kim1_pointer_key(Kim1State *s) {
    if (!s->display) return 0;

    GemuPointerState ptr = gemu_display_get_pointer(s->display);
    if ((!ptr.button && !ptr.pressed) || ptr.x < 0 || ptr.y < 0)
        return 0;

    int kw = KIM1_KEY_W, kh = KIM1_KEY_H, kg = KIM1_KEY_GAP;
    int total_w = kw * KIM1_KEYPAD_COLS + kg * (KIM1_KEYPAD_COLS - 1);
    int ox = (KIM1_FB_WIDTH - total_w) / 2;
    int oy = KIM1_KEYPAD_Y;

    for (int row = 0; row < KIM1_KEYPAD_ROWS; row++) {
        for (int col = 0; col < KIM1_KEYPAD_COLS; col++) {
            uint32_t bit = keypad_matrix[row][col];
            if (!bit) continue;

            int kx = ox + col * (kw + kg);
            int ky = oy + row * (kh + kg);
            if (ptr.x >= kx && ptr.x < kx + kw &&
                ptr.y >= ky && ptr.y < ky + kh)
                return bit;
        }
    }

    return 0;
}

static int kim1_action_to_key_number(uint32_t bit) {
    switch (bit) {
    case KIM1_ACT_0:    return 0x00;
    case KIM1_ACT_1:    return 0x01;
    case KIM1_ACT_2:    return 0x02;
    case KIM1_ACT_3:    return 0x03;
    case KIM1_ACT_4:    return 0x04;
    case KIM1_ACT_5:    return 0x05;
    case KIM1_ACT_6:    return 0x06;
    case KIM1_ACT_7:    return 0x07;
    case KIM1_ACT_8:    return 0x08;
    case KIM1_ACT_9:    return 0x09;
    case KIM1_ACT_A:    return 0x0A;
    case KIM1_ACT_B:    return 0x0B;
    case KIM1_ACT_C:    return 0x0C;
    case KIM1_ACT_D:    return 0x0D;
    case KIM1_ACT_E:    return 0x0E;
    case KIM1_ACT_F:    return 0x0F;
    case KIM1_ACT_AD:   return 0x10;
    case KIM1_ACT_DA:   return 0x11;
    case KIM1_ACT_PLUS: return 0x12;
    case KIM1_ACT_GO:   return 0x13;
    case KIM1_ACT_PC:   return 0x14;
    default:            return -1;
    }
}

static void kim1_queue_keypress(Kim1State *s, uint32_t newly_pressed) {
    if (!newly_pressed || s->kim_key_pending)
        return;

    for (int i = 0; i < KIM1_NUM_ACTIONS; i++) {
        uint32_t bit = 1u << i;
        if (!(newly_pressed & bit))
            continue;

        int key = kim1_action_to_key_number(bit);
        if (key >= 0) {
            kim1_panel_command(s, (uint8_t)key);
        } else if (bit == KIM1_ACT_RS) {
            mos6502_reset(&s->cpu);
            kim1_reset_rriots(s);
        } else if (bit == KIM1_ACT_ST) {
            if (s->kim_running_user)
                s->kim_saved_pc = s->cpu.PC;
            s->kim_running_user = false;
            s->cpu.nmi = true;
        }
        return;
    }
}

static uint32_t kim1_raw_key_to_action(uint32_t cp) {
    if (cp >= '0' && cp <= '9')
        return 1u << (cp - '0');
    if (cp >= 'a' && cp <= 'f')
        return 1u << (10 + cp - 'a');
    if (cp >= 'A' && cp <= 'F')
        return 1u << (10 + cp - 'A');
    switch (cp) {
    case '<': return KIM1_ACT_AD;
    case '>': return KIM1_ACT_DA;
    case 'p': case 'P': return KIM1_ACT_PC;
    case '+': return KIM1_ACT_PLUS;
    case 'g': case 'G': return KIM1_ACT_GO;
    case 's': case 'S': return KIM1_ACT_ST;
    case 'r': case 'R': return KIM1_ACT_RS;
    default: return 0;
    }
}

static void kim1_poll_vnc(Kim1State *s) {
    if (!s->vnc) return;

    GemuVncKeyEvent ev;
    while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
        if (!ev.down)
            continue;
        uint32_t action = kim1_raw_key_to_action(ev.keysym);
        kim1_queue_keypress(s, action);
    }
}

static void kim1_set_a_nz(Kim1State *s, uint8_t v) {
    s->cpu.A = v;
    s->cpu.P = (uint8_t)((s->cpu.P & ~(MOS6502_P_N | MOS6502_P_Z))
             | (v == 0 ? MOS6502_P_Z : 0)
             | (v & 0x80u ? MOS6502_P_N : 0));
}

static void kim1_panel_refresh(Kim1State *s) {
    uint8_t data = kim1_mem_read(s->kim_panel_addr, s);
    s->ram[0xFB] = (uint8_t)(s->kim_panel_addr >> 8);
    s->ram[0xFA] = (uint8_t)s->kim_panel_addr;
    s->ram[0xF9] = data;
    kim1_update_monitor_display(s);
}

static void kim1_display_blank(Kim1State *s) {
    for (int i = 0; i < KIM1_NUM_DIGITS; i++)
        s->seg_cache[i] = 0;
}

static void kim1_panel_command(Kim1State *s, uint8_t key) {
    if (s->kim_running_user && key != 0x15 && key != 0x16)
        return;

    if (key <= 0x0F) {
        if (s->kim_address_mode) {
            s->kim_panel_addr = (uint16_t)((s->kim_panel_addr << 4) | key);
        } else {
            uint8_t data = (uint8_t)((kim1_mem_read(s->kim_panel_addr, s) << 4) | key);
            kim1_mem_write(s->kim_panel_addr, data, s);
        }
        kim1_panel_refresh(s);
        return;
    }

    switch (key) {
    case 0x10: /* AD */
        s->kim_address_mode = true;
        kim1_panel_refresh(s);
        break;
    case 0x11: /* DA */
        s->kim_address_mode = false;
        kim1_panel_refresh(s);
        break;
    case 0x12: /* + */
        s->kim_panel_addr++;
        kim1_panel_refresh(s);
        break;
    case 0x13: /* GO */
        s->kim_saved_pc = s->kim_panel_addr;
        s->cpu.PC = s->kim_panel_addr;
        s->kim_running_user = true;
        kim1_display_blank(s);
        break;
    case 0x14: /* PC */
        s->kim_panel_addr = s->kim_saved_pc;
        s->kim_address_mode = true;
        kim1_panel_refresh(s);
        break;
    default:
        break;
    }
}

/* ── Framebuffer rendering ──────────────────────────────────────────────── */

void kim1_render_fb(Kim1State *s) {
    uint32_t *fb = s->fb;
    int fw = KIM1_FB_WIDTH;

    /* Clear background */
    for (int i = 0; i < KIM1_FB_WIDTH * KIM1_FB_HEIGHT; i++)
        fb[i] = COLOUR_BG;

    /* Segment patterns are cached from Port A/Port B writes in kim1_mem_write. */
    uint8_t segs[KIM1_NUM_DIGITS];
    for (int d = 0; d < KIM1_NUM_DIGITS; d++)
        segs[d] = s->seg_cache[d];

    /* ── 7-segment display ──────────────────────────────────────────── */

    int display_middle_gap = 24;
    int total_dw = (KIM1_DIGIT_W + KIM1_DIGIT_GAP) * KIM1_NUM_DIGITS -
                   KIM1_DIGIT_GAP + display_middle_gap;
    int dox = (KIM1_FB_WIDTH - total_dw) / 2;
    int doy = 10;

    for (int d = 0; d < KIM1_NUM_DIGITS; d++) {
        uint8_t pat = segs[d];
        int dx = dox + d * (KIM1_DIGIT_W + KIM1_DIGIT_GAP);
        if (d >= 4)
            dx += display_middle_gap;

        for (int i = 0; i < 8; i++) {
            bool lit = (pat >> i) & 1u;
            fill_rect(fb, fw, dx + seg_rects[i].x, doy + seg_rects[i].y,
                      seg_rects[i].w, seg_rects[i].h,
                      lit ? COLOUR_LIT : COLOUR_OFF);
        }
    }

    /* ── Visual keypad ──────────────────────────────────────────────── */

    if (s->has_keypad)
        draw_keypad(s);
}

/* ── 6530 timer helpers ─────────────────────────────────────────────────── */

static inline uint32_t rriot_period(Kim1Rriot *r) {
    return r->reload ? (uint32_t)r->reload : 0x10000u;
}

static void rriot_update_irq(Kim1State *s) {
    s->cpu.irq = s->u2.irq_flag || s->u3.irq_flag;
}

static void rriot_timer_tick(Kim1Rriot *r, Kim1State *s) {
    if (!r->running) return;
    if (s->cpu.cycle_count < r->next_fire) return;

    r->irq_flag = true;
    r->count    = 0;
    rriot_update_irq(s);
    r->next_fire = s->cpu.cycle_count + (uint64_t)rriot_period(r);
}

static void rriot_timer_start(Kim1Rriot *r, Kim1State *s) {
    r->count    = r->reload;
    r->running  = true;
    r->irq_flag = false;
    rriot_update_irq(s);
    r->next_fire = s->cpu.cycle_count + (uint64_t)rriot_period(r);
}

/* ── 6530 RRIOT reset state ─────────────────────────────────────────────── */

static void kim1_reset_rriots(Kim1State *s) {
    s->keypad_prev = 0;
    s->kim_key_pending = false;
    s->kim_key_number = 0x15u;
    s->kim_address_mode = true;
    s->kim_panel_addr = 0x0000u;
    s->kim_saved_pc = 0x0000u;
    s->kim_running_user = false;

    /* 6530-003 (u2, addr 0x1700): application I/O */
    s->u2 = (Kim1Rriot){0};
    s->u2.irq_flag = true;

    /* 6530-002 (u3, addr 0x1740): LED display / keyboard / tape.
     * PB0=1 (TTY idle-high), DDRB=$3F (PB0-PB5 outputs).
     * irq_flag=true so the monitor's first BIT $1747 spin exits immediately
     * rather than looping forever waiting for a timer that hasn't started yet. */
    s->u3 = (Kim1Rriot){0};
    s->u3.pb      = 0x01u;
    s->u3.ddrb    = 0x3Fu;
    s->u3.irq_flag = true;
    rriot_update_irq(s);
    kim1_panel_refresh(s);
}

static uint8_t kim1_hex_to_segments(uint8_t v) {
    static const uint8_t segs[16] = {
        0x3Fu, /* 0: a b c d e f     */
        0x06u, /* 1: b c             */
        0x5Bu, /* 2: a b d e g       */
        0x4Fu, /* 3: a b c d g       */
        0x66u, /* 4: b c f g         */
        0x6Du, /* 5: a c d f g       */
        0x7Du, /* 6: a c d e f g     */
        0x07u, /* 7: a b c           */
        0x7Fu, /* 8: a b c d e f g   */
        0x6Fu, /* 9: a b c d f g     */
        0x77u, /* A: a b c e f g     */
        0x7Cu, /* b: c d e f g       */
        0x39u, /* C: a d e f         */
        0x5Eu, /* d: b c d e g       */
        0x79u, /* E: a d e f g       */
        0x71u, /* F: a e f g         */
    };
    return segs[v & 0x0Fu];
}

static void kim1_update_monitor_display(Kim1State *s) {
    uint8_t fb = s->ram[0xFB];
    uint8_t fa = s->ram[0xFA];
    uint8_t f9 = s->ram[0xF9];

    s->seg_cache[0] = kim1_hex_to_segments(fb >> 4);
    s->seg_cache[1] = kim1_hex_to_segments(fb);
    s->seg_cache[2] = kim1_hex_to_segments(fa >> 4);
    s->seg_cache[3] = kim1_hex_to_segments(fa);
    s->seg_cache[4] = kim1_hex_to_segments(f9 >> 4);
    s->seg_cache[5] = kim1_hex_to_segments(f9);
}

/* ── Memory callbacks ────────────────────────────────────────────────────── */

static uint8_t kim1_mem_read(uint16_t addr, void *ud) {
    Kim1State *s = ud;
    gemu_monitor_check_read(s->monitor, addr);

    uint16_t a = addr & 0x1FFFu;

    if (a < 0x0400u)
        return s->ram[a];

    if (a >= KIM1_U2_BASE && a < KIM1_U2_BASE + 0x0080u) {
        uint8_t off = a & 0x0Fu;
        Kim1Rriot *r = (a & 0x0040u) ? &s->u3 : &s->u2;

        switch (off) {
        case KIM1_RA: {
            uint8_t out = r->pa & r->ddra;
            /* PA0-PA5 keyboard columns, active-low; PA7 = TTY break (high = no break) */
            uint8_t in = 0xFFu;
            if (r == &s->u3) {
                /* 74145 decoder value from PB1-PB4 selects keyboard row (0-3) */
                int dec = (s->u3.pb >> 1) & 0x0F;
                if (dec <= 3) {
                    for (int col = 0; col < 6; col++) {
                        uint32_t bit = hw_keymap[dec][col];
                        if (bit && (s->keypad_held & bit))
                            in &= (uint8_t)~(1u << col);
                    }
                }
            }
            return (out & r->ddra) | (in & ~r->ddra);
        }
        case KIM1_DDRA:  return r->ddra;
        case KIM1_RB: {
            uint8_t out = r->pb & r->ddrb;
            return out | (0u & ~r->ddrb);
        }
        case KIM1_DDRB:  return r->ddrb;
        case KIM1_TL:    return (uint8_t)(r->count & 0xFFu);
        case KIM1_TH:    return (uint8_t)((r->count >> 8) & 0xFFu);
        case KIM1_TW:    return 0u;
        case KIM1_TIF: {
            uint8_t v = r->irq_flag ? 0x80u : 0u;
            r->irq_flag = false;
            rriot_update_irq(s);
            return v;
        }
        default:         return 0u;
        }
    }

    /* RRIOT internal RAM: 6530-003 @ 0x1780-0x17BF, 6530-002 @ 0x17C0-0x17FF */
    if (a >= 0x1780u && a < 0x1800u)
        return s->rriot_ram[a - 0x1780u];

    if (a >= 0x1800u) {
        if (a == 0x1F1Fu) {
            if (!s->kim_running_user)
                kim1_update_monitor_display(s);
            s->cpu.PC = 0x1F44u;
            return 0xEAu;
        }
        if (a == 0x1EFEu) {
            kim1_set_a_nz(s, s->kim_key_pending ? 0xFFu : 0x00u);
            s->cpu.PC = 0x1F17u;
            return 0xEAu;
        }
        if (a == 0x1F6Au) {
            kim1_set_a_nz(s, s->kim_key_pending ? s->kim_key_number : 0x15u);
            s->kim_key_pending = false;
            s->cpu.PC = 0x1F8Fu;
            return 0xEAu;
        }
        if (a < 0x1C00u) return s->rom_002[a - 0x1800u];
        return s->rom_003[a - 0x1C00u];
    }

    return 0u;
}

static void kim1_mem_write(uint16_t addr, uint8_t val, void *ud) {
    Kim1State *s = ud;
    gemu_monitor_check_write(s->monitor, addr);

    uint16_t a = addr & 0x1FFFu;

    if (a < 0x0400u) {
        s->ram[a] = val;
        return;
    }

    if (a >= KIM1_U2_BASE && a < KIM1_U2_BASE + 0x0080u) {
        uint8_t off = a & 0x0Fu;
        Kim1Rriot *r = (a & 0x0040u) ? &s->u3 : &s->u2;

        switch (off) {
        case KIM1_RA:
            r->pa = val;
            if (r == &s->u3) {
                /* PA write during display refresh: latch segment data into the digit
                 * currently selected by PB1-PB4 (74145 decoder, values 4-9 = LEDs 1-6). */
                int dec = (s->u3.pb >> 1) & 0x0F;
                if (dec >= 4 && dec <= 9)
                    s->seg_cache[dec - 4] = val;
            }
            break;
        case KIM1_DDRA:
            r->ddra = val;
            break;
        case KIM1_RB:
            r->pb = val;
            if (r == &s->u3) {
                /* PB write changes 74145 decoder; latch current PA into the new digit slot.
                 * Decoder input = PB1-PB4; values 4-9 → LED digits 1-6 (seg_cache[0-5]). */
                int dec = (val >> 1) & 0x0F;
                if (dec >= 4 && dec <= 9)
                    s->seg_cache[dec - 4] = s->u3.pa;
            }
            break;
        case KIM1_DDRB:
            r->ddrb = val;
            break;
        case KIM1_TL:
            r->reload = (r->reload & 0xFF00u) | val;
            rriot_timer_start(r, s);
            break;
        case KIM1_TH:
            r->reload = (r->reload & 0x00FFu) | ((uint16_t)val << 8);
            rriot_timer_start(r, s);
            break;
        case KIM1_TW:
            r->reload = (r->reload & 0xFF00u) | val;
            break;
        case KIM1_TIF:
            r->irq_flag = false;
            rriot_update_irq(s);
            break;
        }
        return;
    }

    /* RRIOT internal RAM: 6530-003 @ 0x1780-0x17BF, 6530-002 @ 0x17C0-0x17FF */
    if (a >= 0x1780u && a < 0x1800u) {
        s->rriot_ram[a - 0x1780u] = val;
        return;
    }
}

/* ── ROM loading ─────────────────────────────────────────────────────────── */

static bool load_rom_file(const char *path, uint8_t *dest, size_t expected,
                           const char *label) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "gemu-kim1: cannot find '%s' (%s)\n", path, label);
        return false;
    }
    if ((size_t)st.st_size != expected) {
        fprintf(stderr, "gemu-kim1: '%s' is %ld bytes, expected %zu (%s)\n",
                path, (long)st.st_size, expected, label);
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gemu-kim1: cannot open '%s'\n", path);
        return false;
    }
    if (fread(dest, 1, expected, f) != expected) {
        fprintf(stderr, "gemu-kim1: short read on '%s'\n", path);
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static bool load_roms(Kim1State *s) {
    if (s->cfg->n_roms < 2) {
        fprintf(stderr, "gemu-kim1: need both 6530-002.bin and 6530-003.bin\n"
                        "  Use: -rom 0x1800:6530-002.bin -rom 0x1C00:6530-003.bin\n"
                        "  Or point at ROM directory: -rom /path/to/kim1/roms/\n");
        return false;
    }

    bool got_002 = false, got_003 = false;
    for (int i = 0; i < s->cfg->n_roms; i++) {
        uint32_t addr = s->cfg->roms[i].addr & 0xFFFFu;
        const char *path = s->cfg->roms[i].path;

        if (addr == 0x1800u) {
            if (!load_rom_file(path, s->rom_002, 1024, "6530-002"))
                return false;
            got_002 = true;
            printf("gemu-kim1: 6530-002 loaded @ $1800 <- %s\n", path);
            gemu_monitor_register_rom(s->monitor, 0x1800u, 1024, path);
        } else if (addr == 0x1C00u) {
            if (!load_rom_file(path, s->rom_003, 1024, "6530-003"))
                return false;
            got_003 = true;
            printf("gemu-kim1: 6530-003 loaded @ $1C00 <- %s\n", path);
            gemu_monitor_register_rom(s->monitor, 0x1C00u, 1024, path);
        }
    }

    if (!got_002) fprintf(stderr, "gemu-kim1: missing 6530-002.bin at $1800\n");
    if (!got_003) fprintf(stderr, "gemu-kim1: missing 6530-003.bin at $1C00\n");
    if (!got_002 || !got_003) return false;

    gemu_monitor_register_rom(s->monitor, 0xFC00u, 1024, "6530-003 (vector mirror)");
    return true;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

Kim1State *kim1_create(const MosConfig *cfg) {
    Kim1State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg        = cfg;
    s->has_keypad = true;  /* always show visual keypad; it's the primary UI */
    s->monitor    = gemu_monitor_create();

    mos6502_init(&s->cpu);
    s->cpu.mem_read  = kim1_mem_read;
    s->cpu.mem_write = kim1_mem_write;
    s->cpu.mem_ud    = s;
    s->cpu.decimal_disable = (cfg->cpu == MOS_CPU_2A03);

    if (!load_roms(s)) {
        gemu_monitor_destroy(s->monitor);
        free(s);
        return NULL;
    }

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        s->display = gemu_display_create(cfg->display_type,
            &(GemuDisplayConfig){
                .title       = "GEMU",
                .fb_width    = KIM1_FB_WIDTH,
                .fb_height   = KIM1_FB_HEIGHT,
                .scale       = cfg->display_scale,
                .window_width  = KIM1_PRESENT_WIDTH,
                .window_height = KIM1_PRESENT_HEIGHT,
                .renderer    = cfg->display_renderer,
                .actions     = kim1_actions,
                .n_actions   = KIM1_NUM_ACTIONS,
                .ini_section = "kim-keypad",
            });
        if (!s->display)
            fprintf(stderr, "gemu-kim1: failed to create display window\n");
    }

    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr, KIM1_PRESENT_WIDTH, KIM1_PRESENT_HEIGHT);
        if (s->vnc)
            gemu_vnc_set_palette(s->vnc, kim1_vnc_palette,
                                 (int)(sizeof(kim1_vnc_palette) / sizeof(kim1_vnc_palette[0])));
        else
            fprintf(stderr, "gemu-kim1: failed to start VNC at %s\n", cfg->vnc_addr);
    }

    kim1_reset_rriots(s);
    mos6502_reset(&s->cpu);
    return s;
}

void kim1_destroy(Kim1State *s) {
    gemu_display_destroy(s->display);
    gemu_vnc_destroy(s->vnc);
    gemu_monitor_destroy(s->monitor);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

#define KIM1_HZ                1000000u
#define KIM1_FRAME_HZ          60u
#define KIM1_CYCLES_PER_FRAME  (KIM1_HZ / KIM1_FRAME_HZ)
#define KIM1_FRAME_MS          (1000u / KIM1_FRAME_HZ)

void kim1_run(Kim1State *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {

        s->keypad_held = 0;
        if (s->display) {
            uint32_t held = gemu_display_poll(s->display) | kim1_pointer_key(s);
            uint32_t raw_pressed = 0;
            uint32_t cp;
            while ((cp = gemu_display_pop_raw_key(s->display)) != 0)
                raw_pressed |= kim1_raw_key_to_action(cp);
            s->keypad_held = held;
            kim1_queue_keypress(s, (held & ~s->keypad_prev) | raw_pressed);
            s->keypad_prev = held;
            if (gemu_display_should_quit(s->display))
                quit = true;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                mos6502_reset(&s->cpu);
                kim1_reset_rriots(s);
            }
        }
        kim1_poll_vnc(s);

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)   { quit = true; break; }
            else if (cmd == GEMU_MON_RESET) {
                mos6502_reset(&s->cpu);
                kim1_reset_rriots(s);
            }
            else if (cmd == GEMU_MON_CUSTOM) gemu_monitor_unknown_command(s->monitor);
        }
        if (quit) break;

        if (!gemu_monitor_is_paused(s->monitor)) {
            uint64_t target = s->cpu.cycle_count + KIM1_CYCLES_PER_FRAME;
            while (s->cpu.cycle_count < target) {
                if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) break;
                mos6502_step(&s->cpu);
                rriot_timer_tick(&s->u2, s);
                rriot_timer_tick(&s->u3, s);
                if (gemu_monitor_is_paused(s->monitor)) break;
            }
        }


        if (s->display || s->vnc) {
            kim1_render_fb(s);
            if (s->display)
                gemu_display_render(s->display, s->fb,
                                    KIM1_FB_WIDTH, KIM1_FB_HEIGHT);
            if (s->vnc)
                kim1_update_vnc(s);
        }

        kim1_sleep_ms(KIM1_FRAME_MS);
    }

    printf("gemu-kim1: %llu cycles, %llu instructions\n",
           (unsigned long long)s->cpu.cycle_count,
           (unsigned long long)s->cpu.insn_count);

    gemu_monitor_stop(s->monitor);
    (void)cfg;
}
