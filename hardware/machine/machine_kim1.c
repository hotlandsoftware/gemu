#ifndef _WIN32
#  define _POSIX_C_SOURCE 199309L
#endif
#include "kim1.h"
#include "wozmon_rom.h"
#ifdef GEMU_GTK
#  include "../vga/hex_editor.h"
#endif
#include <SDL2/SDL.h>
#include <errno.h>
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

#ifdef GEMU_GTK
static void kim1_hex_toggle(void *ud) {
    Kim1State *s = ud;
    if (!s->hex_editor) return;
    if (hex_editor_is_visible(s->hex_editor))
        hex_editor_hide(s->hex_editor);
    else
        hex_editor_show(s->hex_editor);
}
#endif

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

/* ── Cassette tape I/O ───────────────────────────────────────────────────── *
 *
 * KIM-1 binary tape format (used by DUMPT $1800 / LOADT $1873 routines):
 *   2A  ID  SAL SAH  EAL EAH  <data bytes>  2F  CHKL CHKH  04 04
 *
 * Checksum = SAL+SAH+EAL+EAH + sum(data) masked to 16 bits.
 * Parameters are read/written from RRIOT RAM at $17F5–$17F9.
 * On success: ram[$FA]=$00, ram[$FB]=$00.  On error: $FF, $FF.
 * PC is redirected to $1C4F (MONITR entry) after either op.
 */

#define TAPE_SAL  (s->rriot_ram[0x75])
#define TAPE_SAH  (s->rriot_ram[0x76])
#define TAPE_EAL  (s->rriot_ram[0x77])
#define TAPE_EAH  (s->rriot_ram[0x78])
#define TAPE_ID   (s->rriot_ram[0x79])

static void tape_ok(Kim1State *s) {
    s->ram[0xFA] = 0x00;
    s->ram[0xFB] = 0x00;
}
static void tape_fail(Kim1State *s, const char *msg) {
    fprintf(stderr, "gemu-kim1: tape: %s\n", msg);
    s->ram[0xFA] = 0xFF;
    s->ram[0xFB] = 0xFF;
}

/* ── WAV cassette encode / decode ──────────────────────────────────────────
 *
 * KIM-1 FSK timing recovered from 6530-003 ROM ($199E short, $19C4 long):
 *   Short burst : 9 cycles,  126 µs half-period  → ~3968 Hz
 *   Long  burst : 6 cycles,  195 µs half-period  → ~2564 Hz
 *   Bit 0 = SSL  |  Bit 1 = SLL  (LSB-first)
 *   Sync leader : 100 × 0x16  then start marker 0x2A
 */

/* ---- encode helpers ---- */
static void kwav_w16(FILE *f, uint16_t v) { fputc(v&0xFF,f); fputc(v>>8,f); }
static void kwav_w32(FILE *f, uint32_t v) { kwav_w16(f,v&0xFFFFu); kwav_w16(f,v>>16); }

#define KWAV_SRATE  44100
#define KWAV_AMP    28000
#define KWAV_HP_S   6       /* short half-period (samples at 44100) */
#define KWAV_HP_L   9       /* long  half-period */
#define KWAV_CY_S   9       /* short burst cycles */
#define KWAV_CY_L   6       /* long  burst cycles */

static void kwav_burst_enc(FILE *f, int ncyc, int hp, int *pol)
{
    for (int h = 0; h < ncyc*2; h++) {
        int16_t s = (int16_t)(*pol * KWAV_AMP);
        for (int i = 0; i < hp; i++) kwav_w16(f, (uint16_t)s);
        *pol = -(*pol);
    }
}
static void kwav_enc_byte(FILE *f, uint8_t byte, int *pol)
{
    for (int b = 0; b < 8; b++) {
        kwav_burst_enc(f, KWAV_CY_S, KWAV_HP_S, pol);
        if ((byte >> b) & 1) {
            kwav_burst_enc(f, KWAV_CY_L, KWAV_HP_L, pol);
            kwav_burst_enc(f, KWAV_CY_L, KWAV_HP_L, pol);
        } else {
            kwav_burst_enc(f, KWAV_CY_S, KWAV_HP_S, pol);
            kwav_burst_enc(f, KWAV_CY_L, KWAV_HP_L, pol);
        }
    }
}

static bool path_ends_wav(const char *p)
{
    size_t n = strlen(p);
    if (n < 4) return false;
    const char *e = p + n - 4;
    return e[0]=='.' &&
           (e[1]=='w'||e[1]=='W') &&
           (e[2]=='a'||e[2]=='A') &&
           (e[3]=='v'||e[3]=='V');
}

static bool kim1_tape_save_wav(Kim1State *s)
{
    FILE *fp = fopen(s->cfg->tape_path, "wb");
    if (!fp) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot write '%s': %s",
                 s->cfg->tape_path, strerror(errno));
        tape_fail(s, msg); return false;
    }
    uint16_t start = (uint16_t)(TAPE_SAL | ((uint16_t)TAPE_SAH << 8));
    uint16_t end   = (uint16_t)(TAPE_EAL | ((uint16_t)TAPE_EAH << 8));
    uint8_t  id    = TAPE_ID;
    uint16_t cksum = (uint16_t)(TAPE_SAL + TAPE_SAH + TAPE_EAL + TAPE_EAH);

    fwrite("RIFF",1,4,fp); long riff_pos = ftell(fp); kwav_w32(fp,0);
    fwrite("WAVE",1,4,fp);
    fwrite("fmt ",1,4,fp); kwav_w32(fp,16);
    kwav_w16(fp,1); kwav_w16(fp,1);               /* PCM, mono */
    kwav_w32(fp,KWAV_SRATE); kwav_w32(fp,KWAV_SRATE*2);
    kwav_w16(fp,2); kwav_w16(fp,16);              /* block align, bps */
    fwrite("data",1,4,fp); long data_pos = ftell(fp); kwav_w32(fp,0);
    long data_start = ftell(fp);

    int pol = 1;
    for (int i = 0; i < 100; i++) kwav_enc_byte(fp, 0x16, &pol);
    kwav_enc_byte(fp, 0x2A,      &pol);
    kwav_enc_byte(fp, id,        &pol);
    kwav_enc_byte(fp, TAPE_SAL,  &pol);
    kwav_enc_byte(fp, TAPE_SAH,  &pol);
    kwav_enc_byte(fp, TAPE_EAL,  &pol);
    kwav_enc_byte(fp, TAPE_EAH,  &pol);
    if (end >= start) {
        for (uint32_t a = start; a <= (uint32_t)end; a++) {
            uint8_t b = kim1_mem_read((uint16_t)a, s);
            cksum = (uint16_t)(cksum + b);
            kwav_enc_byte(fp, b, &pol);
        }
    }
    kwav_enc_byte(fp, 0x2F,         &pol);
    kwav_enc_byte(fp, cksum & 0xFF, &pol);
    kwav_enc_byte(fp, cksum >> 8,   &pol);
    kwav_enc_byte(fp, 0x04, &pol);
    kwav_enc_byte(fp, 0x04, &pol);

    uint32_t dsz = (uint32_t)(ftell(fp) - data_start);
    fseek(fp, data_pos,  SEEK_SET); kwav_w32(fp, dsz);
    fseek(fp, riff_pos,  SEEK_SET); kwav_w32(fp, 36 + dsz);
    fclose(fp);

    printf("gemu-kim1: tape SAVE $%04X–$%04X → %s (WAV, %.1fs)\n",
           start, end, s->cfg->tape_path, (double)dsz / (KWAV_SRATE*2));
    tape_ok(s);
    return true;
}

/* ---- decode helpers ---- */
typedef struct {
    FILE    *fp;
    uint32_t sr;
    int      ch, bps;
    float    thresh;         /* S/L half-cycle threshold (samples) */
    float    hp_x, hp_y;    /* IIR DC-removal state */
    int      zx_sign;
    int      zx_n;
} KWavDec;

static bool kwav_open(KWavDec *d, FILE *fp)
{
    memset(d, 0, sizeof(*d));
    d->fp = fp;
    char tag[4]; unsigned char sb[4];

    if (fread(tag,1,4,fp)!=4 || memcmp(tag,"RIFF",4)) return false;
    fread(sb,1,4,fp); /* skip RIFF size */
    if (fread(tag,1,4,fp)!=4 || memcmp(tag,"WAVE",4)) return false;

    bool got_fmt = false;
    for (;;) {
        if (fread(tag,1,4,fp)!=4 || fread(sb,1,4,fp)!=4) break;
        uint32_t csz = (uint32_t)sb[0] | ((uint32_t)sb[1]<<8) |
                       ((uint32_t)sb[2]<<16) | ((uint32_t)sb[3]<<24);
        long cend = ftell(fp) + (long)csz;

        if (memcmp(tag,"fmt ",4)==0) {
            unsigned char fb[16];
            if (fread(fb,1,16,fp)!=16) return false;
            if ((fb[0]|(fb[1]<<8)) != 1) return false; /* PCM only */
            d->ch  = fb[2] | (fb[3]<<8);
            d->sr  = (uint32_t)fb[4] | ((uint32_t)fb[5]<<8) |
                     ((uint32_t)fb[6]<<16) | ((uint32_t)fb[7]<<24);
            d->bps = fb[14] | (fb[15]<<8);
            d->thresh = (float)(160.0 * (double)d->sr * 1e-6);
            got_fmt = true;
            fseek(fp, cend, SEEK_SET);
        } else if (memcmp(tag,"data",4)==0) {
            break;   /* fp now at first PCM byte */
        } else {
            fseek(fp, cend, SEEK_SET);
        }
    }
    return got_fmt && d->sr >= 11000 && (d->bps == 8 || d->bps == 16);
}

static bool kwav_sample(KWavDec *d, float *out)
{
    float raw;
    if (d->bps == 16) {
        int lo = fgetc(d->fp), hi = fgetc(d->fp);
        if (lo == EOF || hi == EOF) return false;
        raw = (float)(int16_t)((unsigned)lo | ((unsigned)(uint8_t)hi << 8)) / 32768.0f;
        for (int c = 1; c < d->ch; c++) { fgetc(d->fp); fgetc(d->fp); }
    } else {
        int v = fgetc(d->fp);
        if (v == EOF) return false;
        raw = ((float)v - 128.0f) / 128.0f;
        for (int c = 1; c < d->ch; c++) fgetc(d->fp);
    }
    float y = 0.9999f * (d->hp_y + raw - d->hp_x);
    d->hp_x = raw; d->hp_y = y; *out = y;
    return true;
}

/* Length of next half-cycle in samples, or -1 for EOF */
static int kwav_half(KWavDec *d)
{
    float s;
    for (;;) {
        if (!kwav_sample(d, &s)) return -1;
        int sign = (s >= 0.0f) ? 1 : -1;
        d->zx_n++;
        if (!d->zx_sign) { d->zx_sign = sign; continue; }
        if (sign != d->zx_sign) {
            int len = d->zx_n;
            d->zx_n = 1; d->zx_sign = sign;
            return len;
        }
    }
}

/* Returns 'S', 'L', or 0 for EOF.
 * Burst ends when 3+ consecutive half-cycles of the opposite type appear. */
static int kwav_next_burst(KWavDec *d)
{
    int cur = 0, noise = 0;
    for (;;) {
        int hlen = kwav_half(d);
        if (hlen < 0) return cur ? cur : 0;
        int t = ((float)hlen < d->thresh) ? 'S' : 'L';
        if (!cur) { cur = t; noise = 0; }
        else if (t == cur) { noise = 0; }
        else if (++noise >= 3) return cur;
    }
}

/* Decode one byte from the burst stream. Returns 0–255 or -1 for EOF. */
static int kwav_read_byte(KWavDec *d)
{
    uint8_t val = 0;
    for (int b = 0; b < 8; b++) {
        int b1 = kwav_next_burst(d); if (b1 != 'S') return -1;
        int b2 = kwav_next_burst(d); if (!b2)        return -1;
        int b3 = kwav_next_burst(d); if (!b3)        return -1;
        (void)b3;
        if (b2 == 'L') val |= (uint8_t)(1u << b);
    }
    return val;
}

static bool kim1_tape_load_wav(Kim1State *s, FILE *fp)
{
    KWavDec d;
    if (!kwav_open(&d, fp)) {
        tape_fail(s, "WAV: not PCM or unsupported format");
        return false;
    }
    /* Scan for start marker 0x2A (discard sync/preamble) */
    for (int tries = 0; ; tries++) {
        if (tries > 20000) { tape_fail(s, "WAV: start marker not found"); return false; }
        int b = kwav_read_byte(&d);
        if (b < 0) { tape_fail(s, "WAV: start marker not found"); return false; }
        if (b == 0x2A) break;
    }

    int r_id = kwav_read_byte(&d); if (r_id < 0) goto trunc;
    int r_sal= kwav_read_byte(&d); if (r_sal< 0) goto trunc;
    int r_sah= kwav_read_byte(&d); if (r_sah< 0) goto trunc;
    int r_eal= kwav_read_byte(&d); if (r_eal< 0) goto trunc;
    int r_eah= kwav_read_byte(&d); if (r_eah< 0) goto trunc;

    {
        uint8_t req_id = TAPE_ID;
        if (req_id!=0x00 && req_id!=0xFF && (uint8_t)r_id!=req_id) {
            tape_fail(s,"WAV: ID mismatch"); return false;
        }
        uint16_t fstart = (uint16_t)(r_sal | (r_sah<<8));
        uint16_t fend   = (uint16_t)(r_eal | (r_eah<<8));
        uint16_t load_at = (req_id==0xFF)
            ? (uint16_t)(TAPE_SAL | ((uint16_t)TAPE_SAH<<8)) : fstart;
        if (fend < fstart) { tape_fail(s,"WAV: bad address range"); return false; }

        uint16_t cksum = (uint16_t)(r_sal + r_sah + r_eal + r_eah);
        uint16_t size  = (uint16_t)(fend - fstart + 1);
        for (uint16_t i = 0; i < size; i++) {
            int b = kwav_read_byte(&d); if (b<0) goto trunc;
            cksum = (uint16_t)(cksum + b);
            kim1_mem_write((uint16_t)(load_at + i), (uint8_t)b, s);
        }
        int em = kwav_read_byte(&d);
        if (em != 0x2F) { tape_fail(s,"WAV: bad end marker"); return false; }
        int cl = kwav_read_byte(&d); if (cl<0) goto trunc;
        int ch = kwav_read_byte(&d); if (ch<0) goto trunc;
        if (cksum != (uint16_t)(cl|(ch<<8))) { tape_fail(s,"WAV: checksum mismatch"); return false; }

        printf("gemu-kim1: tape LOAD $%04X–$%04X ID=$%02X ← %s (WAV)\n",
               fstart, fend, (uint8_t)r_id, s->cfg->tape_path);
        tape_ok(s);
        return true;
    }
trunc:
    tape_fail(s,"WAV: file truncated"); return false;
}

static void kim1_tape_save(Kim1State *s) {
    if (!s->cfg->tape_path) {
        tape_fail(s, "no -tape FILE specified");
        return;
    }
    if (path_ends_wav(s->cfg->tape_path)) {
        (void)kim1_tape_save_wav(s);
        return;
    }

    uint16_t start = (uint16_t)(TAPE_SAL | ((uint16_t)TAPE_SAH << 8));
    uint16_t end   = (uint16_t)(TAPE_EAL | ((uint16_t)TAPE_EAH << 8));
    uint8_t  id    = TAPE_ID;

    if (end < start) {
        tape_fail(s, "end address < start address");
        return;
    }

    FILE *fp = fopen(s->cfg->tape_path, "wb");
    if (!fp) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot write '%s': %s", s->cfg->tape_path, strerror(errno));
        tape_fail(s, msg);
        return;
    }

    uint16_t cksum = (uint16_t)(TAPE_SAL + TAPE_SAH + TAPE_EAL + TAPE_EAH);

    fputc(0x2A, fp);
    fputc(id,   fp);
    fputc(TAPE_SAL, fp);
    fputc(TAPE_SAH, fp);
    fputc(TAPE_EAL, fp);
    fputc(TAPE_EAH, fp);

    for (uint32_t a = start; a <= (uint32_t)end; a++) {
        uint8_t b = kim1_mem_read((uint16_t)a, s);
        fputc(b, fp);
        cksum = (uint16_t)(cksum + b);
    }

    fputc(0x2F,           fp);
    fputc(cksum & 0xFF,   fp);
    fputc(cksum >> 8,     fp);
    fputc(0x04, fp);
    fputc(0x04, fp);
    fclose(fp);

    printf("gemu-kim1: tape SAVE $%04X–$%04X ID=$%02X → %s\n",
           start, end, id, s->cfg->tape_path);
    tape_ok(s);
}

static void kim1_tape_load(Kim1State *s) {
    if (!s->cfg->tape_path) {
        tape_fail(s, "no -tape FILE specified");
        return;
    }

    FILE *fp = fopen(s->cfg->tape_path, "rb");
    if (!fp) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot read '%s': %s", s->cfg->tape_path, strerror(errno));
        tape_fail(s, msg);
        return;
    }
    /* auto-detect WAV by RIFF magic */
    {
        unsigned char mg[4];
        bool is_wav = fread(mg,1,4,fp)==4 && memcmp(mg,"RIFF",4)==0;
        rewind(fp);
        if (is_wav) { (void)kim1_tape_load_wav(s, fp); fclose(fp); return; }
    }

    int b;
    if ((b = fgetc(fp)) != 0x2A) { tape_fail(s, "bad start marker"); fclose(fp); return; }

    uint8_t file_id  = (uint8_t)fgetc(fp);
    uint8_t sal      = (uint8_t)fgetc(fp);
    uint8_t sah      = (uint8_t)fgetc(fp);
    uint8_t eal      = (uint8_t)fgetc(fp);
    uint8_t eah      = (uint8_t)fgetc(fp);
    if (feof(fp)) { tape_fail(s, "truncated header"); fclose(fp); return; }

    uint8_t req_id = TAPE_ID;
    if (req_id != 0x00 && req_id != 0xFF && req_id != file_id) {
        tape_fail(s, "ID mismatch");
        fclose(fp);
        return;
    }

    uint16_t file_start = (uint16_t)(sal | ((uint16_t)sah << 8));
    uint16_t file_end   = (uint16_t)(eal | ((uint16_t)eah << 8));
    /* ID=$FF: load at stored start address rather than file's */
    uint16_t load_at = (req_id == 0xFF)
        ? (uint16_t)(TAPE_SAL | ((uint16_t)TAPE_SAH << 8))
        : file_start;

    if (file_end < file_start) { tape_fail(s, "bad address range"); fclose(fp); return; }

    uint16_t cksum = (uint16_t)(sal + sah + eal + eah);
    uint16_t size  = (uint16_t)(file_end - file_start + 1);

    for (uint16_t i = 0; i < size; i++) {
        if ((b = fgetc(fp)) == EOF) { tape_fail(s, "truncated data"); fclose(fp); return; }
        uint8_t byte = (uint8_t)b;
        cksum = (uint16_t)(cksum + byte);
        kim1_mem_write((uint16_t)(load_at + i), byte, s);
    }

    if ((b = fgetc(fp)) != 0x2F) { tape_fail(s, "bad end marker"); fclose(fp); return; }

    uint8_t  chkl     = (uint8_t)fgetc(fp);
    uint8_t  chkh     = (uint8_t)fgetc(fp);
    uint16_t file_chk = (uint16_t)(chkl | ((uint16_t)chkh << 8));
    fclose(fp);

    if (cksum != file_chk) { tape_fail(s, "checksum mismatch"); return; }

    printf("gemu-kim1: tape LOAD $%04X–$%04X ID=$%02X ← %s\n",
           file_start, file_end, file_id, s->cfg->tape_path);
    tape_ok(s);
}

#undef TAPE_SAL
#undef TAPE_SAH
#undef TAPE_EAL
#undef TAPE_EAH
#undef TAPE_ID

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

/* irq_en (set by $0C-$0F writes) controls whether irq_flag drives cpu.irq. */
static void rriot_update_irq(Kim1State *s) {
    s->cpu.irq = (s->u2.irq_flag && s->u2.irq_en)
              || (s->u3.irq_flag && s->u3.irq_en);
}

/* Live 8-bit count: counts down from reload to 0 (underflow), then wraps
 * to $FF and continues freely. */
static uint8_t rriot_current_count(const Kim1Rriot *r, uint64_t cycle) {
    if (!r->running) return r->reload;
    if (cycle < r->next_fire)
        return (uint8_t)((r->next_fire - cycle - 1u) / r->prescale);
    /* post-underflow: wraps $00 → $FF → $FE … */
    return (uint8_t)(-(uint8_t)((cycle - r->next_fire) / r->prescale));
}

static void rriot_timer_tick(Kim1Rriot *r, Kim1State *s) {
    if (!r->running || r->irq_flag) return;
    if (s->cpu.cycle_count < r->next_fire) return;
    r->irq_flag = true;
    rriot_update_irq(s);
}

static void rriot_timer_start(Kim1Rriot *r, Kim1State *s) {
    r->running  = true;
    r->irq_flag = false;
    rriot_update_irq(s);
    uint64_t period = (uint64_t)(r->reload ? r->reload : 256u) * r->prescale;
    r->next_fire = s->cpu.cycle_count + period;
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
    s->u2.prescale = 1;
    s->u2.irq_flag = true;

    /* 6530-002 (u3, addr 0x1740): LED display / keyboard / tape.
     * PB0=1 (TTY idle-high), DDRB=$3F (PB0-PB5 outputs).
     * irq_flag=true so the monitor's first BIT $1747 spin exits immediately
     * rather than looping forever waiting for a timer that hasn't started yet. */
    s->u3 = (Kim1Rriot){0};
    s->u3.prescale = 1;
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
    s->display_dirty = true;
}

/* ── Memory callbacks ────────────────────────────────────────────────────── */

static uint8_t kim1_mem_read(uint16_t addr, void *ud) {
    Kim1State *s = ud;
    gemu_monitor_check_read(s->monitor, addr);

    /* RRIOT I/O and ROM use incomplete address decoding — present at every
     * $2000 mirror. Check these first via masked address so they shadow
     * expansion RAM at mirrored positions (including the CPU vectors at
     * $FFFA–$FFFF, which mirror to ROM at $1FFA–$1FFF). */
    uint16_t a = addr & 0x1FFFu;

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
        /* $04/$05 ($0C/$0D): read live count, no side-effects */
        case KIM1_TL: case KIM1_TH:
        case 0x0Cu:   case 0x0Du:
            return rriot_current_count(r, s->cpu.cycle_count);
        /* $06/$07 ($0E/$0F): read IRQ status + count, clears IRQ flag */
        case KIM1_TW: case KIM1_TIF:
        case 0x0Eu:   case 0x0Fu: {
            uint8_t cnt = rriot_current_count(r, s->cpu.cycle_count);
            uint8_t v = (r->irq_flag ? 0x80u : 0u) | (cnt & 0x7Fu);
            r->irq_flag = false;
            rriot_update_irq(s);
            return v;
        }
        default:         return 0u;
        }
    }

    /* RRIOT internal RAM: 6530-003 @ $1780–$17BF, 6530-002 @ $17C0–$17FF */
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
        /* DUMPT: CPU about to execute at $1800 — save memory block to tape */
        if (a == 0x1800u) {
            kim1_tape_save(s);
            s->cpu.PC = 0x1C4Fu;
            return 0xEAu;
        }
        /* LOADT: CPU about to execute at $1873 — load tape into memory */
        if (a == 0x1873u) {
            kim1_tape_load(s);
            s->cpu.PC = 0x1C4Fu;
            return 0xEAu;
        }
        /* OUTCH: CPU about to execute at $1EA0 — output char in A to serial terminal */
        if (s->serial && a == 0x1EA0u) {
            s->serial->write_byte(s->serial->ud, s->cpu.A);
            s->cpu.PC = 0x1ED4u;   /* skip to OUTCH's RTS */
            return 0xEAu;
        }
        /* GETCH: CPU about to execute at $1E5A — read char from serial terminal (blocking) */
        if (s->serial && a == 0x1E5Au) {
            while (!s->serial->key_available(s->serial->ud)) {
                s->serial->poll(s->serial->ud);
                if (s->serial->should_quit(s->serial->ud)) break;
                if (s->display) {
                    gemu_display_poll(s->display);
                    if (gemu_display_should_quit(s->display)) break;
                    kim1_render_fb(s);
                    gemu_display_render(s->display, s->fb,
                                        KIM1_FB_WIDTH, KIM1_FB_HEIGHT);
                }
                SDL_Delay(1);
            }
            kim1_set_a_nz(s, s->serial->read_byte(s->serial->ud));
            s->cpu.PC = 0x1E88u;   /* skip to GETCH's RTS */
            return 0xEAu;
        }
        if (a < 0x1C00u) return s->rom_002[a - 0x1800u];
        return s->rom_003[a - 0x1C00u];
    }

    /* On-board 1 KB RAM */
    if (addr < 0x0400u)
        return s->ram[addr];

    /* Expansion RAM: full address, no mirroring ($0400–ext_ram_top-1) */
    if (s->ext_ram && addr >= 0x0400u && addr < s->ext_ram_top)
        return s->ext_ram[addr - 0x0400u];

    /* KIM-1 mirror: on-board RAM mirrors at $2000, $4000, … */
    if (a < 0x0400u)
        return s->ram[a];

    return 0u;
}

static void kim1_mem_write(uint16_t addr, uint8_t val, void *ud) {
    Kim1State *s = ud;
    gemu_monitor_check_write(s->monitor, addr);

    uint16_t a = addr & 0x1FFFu;

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
                if (dec >= 4 && dec <= 9) {
                    s->seg_cache[dec - 4] = val;
                    s->display_dirty = true;
                }
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
                if (dec >= 4 && dec <= 9) {
                    s->seg_cache[dec - 4] = s->u3.pa;
                    s->display_dirty = true;
                }
            }
            break;
        case KIM1_DDRB:
            r->ddrb = val;
            break;
        /* $04-$07: load 8-bit timer, prescale ×1/×8/×64/×1024, no CPU IRQ */
        /* $0C-$0F: same prescales but assert CPU IRQ line on underflow      */
        case KIM1_TL: case KIM1_TH: case KIM1_TW: case KIM1_TIF:
        case 0x0Cu:   case 0x0Du:   case 0x0Eu:   case 0x0Fu: {
            static const uint16_t ps[4] = {1, 8, 64, 1024};
            r->reload   = val;
            r->prescale = ps[off & 3u];
            r->irq_en   = (off & 0x08u) != 0;
            rriot_timer_start(r, s);
            break;
        }
        }
        return;
    }

    /* RRIOT internal RAM: 6530-003 @ $1780–$17BF, 6530-002 @ $17C0–$17FF */
    if (a >= 0x1780u && a < 0x1800u) {
        s->rriot_ram[a - 0x1780u] = val;
        return;
    }

    /* ROM: writes silently ignored */
    if (a >= 0x1800u) return;

    /* On-board 1 KB RAM */
    if (addr < 0x0400u) {
        s->ram[addr] = val;
        return;
    }

    /* Expansion RAM */
    if (s->ext_ram && addr >= 0x0400u && addr < s->ext_ram_top) {
        s->ext_ram[addr - 0x0400u] = val;
        return;
    }

    /* KIM-1 mirror: on-board RAM mirrors at $2000, $4000, … */
    if (a < 0x0400u)
        s->ram[a] = val;
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
    s->cpu.decimal_disable = (cfg->cpu == MOS_CPU_2A03 ||
                              cfg->cpu == MOS_CPU_2A07);

    if (!load_roms(s)) {
        gemu_monitor_destroy(s->monitor);
        free(s);
        return NULL;
    }

    if (cfg->want_wozmon) {
        /* Patch EWoz KIM monitor (WozMon) into unused tail of 6530-002 ROM */
        const uint32_t off = WOZMON_LOAD_ADDR - 0x1800u; /* = 0x2A0 = 672 */
        if (off + WOZMON_SIZE <= sizeof(s->rom_002)) {
            memcpy(s->rom_002 + off, wozmon_rom, WOZMON_SIZE);
            printf("gemu-kim1: wozmon patched into 6530-002 @ $%04X\n",
                   WOZMON_LOAD_ADDR);
        } else {
            fprintf(stderr, "gemu-kim1: wozmon does not fit in 6530-002 ROM\n");
        }
    }

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
#ifdef GEMU_GTK
        GemuDisplayGtkExtras gtk_extras = {
            .monitor       = s->monitor,
            .hex_toggle_cb = kim1_hex_toggle,
            .hex_toggle_ud = s,
        };
#endif
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
                .no_rebind   = true,
#ifdef GEMU_GTK
                .gtk         = &gtk_extras,
#endif
            });
        if (!s->display)
            fprintf(stderr, "gemu-kim1: failed to create display window\n");
        else
            gemu_monitor_set_input_reset_cb(s->monitor,
                                            gemu_display_reset_input_bindings_ud,
                                            s->display);
    }

    /* Expansion RAM: covers $0400–(mem_size-1), $2000+ without KIM-1 mirroring */
    if (cfg->mem_size > 0x0400u) {
        uint32_t top = cfg->mem_size < 0x10000u ? cfg->mem_size : 0xFFFFu;
        s->ext_ram = calloc(1, top - 0x0400u);
        if (s->ext_ram) {
            s->ext_ram_top = top;
            printf("gemu-kim1: expansion RAM %u KB ($0400–$%04X)\n",
                   (top - 0x0400u) / 1024u, top - 1u);
        }
    }

#ifdef GEMU_GTK
    if (cfg->display_type == GEMU_DISPLAY_GTK) {
        HexRegion kim1_regions[5] = {
            { "RAM ($0000-$03FF)",      s->ram,      sizeof(s->ram),      false, 0x0000u },
            { "RRIOT RAM ($1780-$17FF)",s->rriot_ram,sizeof(s->rriot_ram),false, 0x1780u },
            { "ROM 6530-002 ($1800)",   s->rom_002,  sizeof(s->rom_002),  true,  0x1800u },
            { "ROM 6530-003 ($1C00)",   s->rom_003,  sizeof(s->rom_003),  true,  0x1C00u },
        };
        int n_regions = 4;
        if (s->ext_ram) {
            /* Cap hex editor region to 4 KB to avoid allocating large GTK
             * text buffers that exhaust GL resources before the display
             * context is ready. */
            size_t hex_sz = s->ext_ram_top - 0x0400u;
            if (hex_sz > 4096u) hex_sz = 4096u;
            kim1_regions[n_regions++] = (HexRegion){
                "Expansion RAM ($0400+)", s->ext_ram, hex_sz, false, 0x0400u
            };
        }
        s->hex_editor = hex_editor_create(kim1_regions, n_regions);
    }
#endif

    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr, KIM1_PRESENT_WIDTH, KIM1_PRESENT_HEIGHT);
        if (s->vnc)
            gemu_vnc_set_palette(s->vnc, kim1_vnc_palette,
                                 (int)(sizeof(kim1_vnc_palette) / sizeof(kim1_vnc_palette[0])));
        else
            fprintf(stderr, "gemu-kim1: failed to start VNC at %s\n", cfg->vnc_addr);
    }
    gemu_monitor_set_vnc(s->monitor, s->vnc);

    kim1_reset_rriots(s);
    s->serial = cfg->serial;
    s->display_dirty = true;

    mos6502_reset(&s->cpu);
    if (cfg->has_start_addr)
        s->cpu.PC = cfg->start_addr;
    return s;
}

void kim1_destroy(Kim1State *s) {
#ifdef GEMU_GTK
    hex_editor_destroy(s->hex_editor);
#endif
    gemu_display_destroy(s->display);
    gemu_vnc_destroy(s->vnc);
    gemu_monitor_destroy(s->monitor);
    free(s->ext_ram);
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

        /* Serial terminal must poll first: it batch-collects SDL events and
         * pushes non-terminal events back so gemu_display_poll sees them. */
        if (s->serial) {
            s->serial->poll(s->serial->ud);
            if (s->serial->should_quit(s->serial->ud))
                quit = true;
        }

        s->keypad_held = 0;
        if (s->display) {
            uint32_t held = gemu_display_poll(s->display) | kim1_pointer_key(s);
            uint32_t raw_pressed = 0;
            uint32_t cp;
            while ((cp = gemu_display_pop_raw_key(s->display)) != 0)
                raw_pressed |= kim1_raw_key_to_action(cp);
            /* Drain chars forwarded by the terminal (SDL_TEXTINPUT events that
             * could not be pushed back via SDL_PushEvent on SDL3-compat). */
            if (s->serial && s->serial->pop_forwarded)
                while ((cp = s->serial->pop_forwarded(s->serial->ud)) != 0)
                    raw_pressed |= kim1_raw_key_to_action(cp);
            if (held != s->keypad_held) s->display_dirty = true;
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


        if ((s->display || s->vnc) && s->display_dirty) {
            kim1_render_fb(s);
            s->display_dirty = false;
            if (s->display)
                gemu_display_render(s->display, s->fb,
                                    KIM1_FB_WIDTH, KIM1_FB_HEIGHT);
            if (s->vnc)
                kim1_update_vnc(s);
        }

#ifdef GEMU_GTK
        hex_editor_refresh(s->hex_editor);
#endif

        kim1_sleep_ms(KIM1_FRAME_MS);
    }

    printf("gemu-kim1: %llu cycles, %llu instructions\n",
           (unsigned long long)s->cpu.cycle_count,
           (unsigned long long)s->cpu.insn_count);

    gemu_monitor_stop(s->monitor);
    (void)cfg;
}
