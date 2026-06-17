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

/* ── Keyboard state builder ──────────────────────────────────────────────── */

static void kim1_update_keys(Kim1State *s, GemuDisplay *d) {
    if (!d) return;

    bool shift = gemu_display_is_key_held(d, "Left Shift")
              || gemu_display_is_key_held(d, "Right Shift");

    uint32_t held = 0;

    /* Hex digits 0–9 */
    if (gemu_display_is_key_held(d, "0")) held |= KIM1_ACT_0;
    if (gemu_display_is_key_held(d, "1")) held |= KIM1_ACT_1;
    if (gemu_display_is_key_held(d, "2")) held |= KIM1_ACT_2;
    if (gemu_display_is_key_held(d, "3")) held |= KIM1_ACT_3;
    if (gemu_display_is_key_held(d, "4")) held |= KIM1_ACT_4;
    if (gemu_display_is_key_held(d, "5")) held |= KIM1_ACT_5;
    if (gemu_display_is_key_held(d, "6")) held |= KIM1_ACT_6;
    if (gemu_display_is_key_held(d, "7")) held |= KIM1_ACT_7;
    if (gemu_display_is_key_held(d, "8")) held |= KIM1_ACT_8;
    if (gemu_display_is_key_held(d, "9")) held |= KIM1_ACT_9;

    /* Hex A–F: lowercase a-f → hex digit; Shift+a → AD, Shift+d → DA */
    if (gemu_display_is_key_held(d, "a")) held |= shift ? KIM1_ACT_AD : KIM1_ACT_A;
    if (gemu_display_is_key_held(d, "b")) held |= KIM1_ACT_B;
    if (gemu_display_is_key_held(d, "c")) held |= KIM1_ACT_C;
    if (gemu_display_is_key_held(d, "d")) held |= shift ? KIM1_ACT_DA : KIM1_ACT_D;
    if (gemu_display_is_key_held(d, "e")) held |= KIM1_ACT_E;
    if (gemu_display_is_key_held(d, "f")) held |= KIM1_ACT_F;

    /* Function keys — p/g/s/r (both cases hit the same physical key) */
    if (gemu_display_is_key_held(d, "p")) held |= KIM1_ACT_PC;
    if (gemu_display_is_key_held(d, "g")) held |= KIM1_ACT_GO;
    if (gemu_display_is_key_held(d, "s")) held |= KIM1_ACT_ST;
    if (gemu_display_is_key_held(d, "r")) held |= KIM1_ACT_RS;

    /* + key: the = physical key (SDL maps + to the same scancode) or Keypad + */
    if (gemu_display_is_key_held(d, "+") || gemu_display_is_key_held(d, "Keypad +"))
        held |= KIM1_ACT_PLUS;

    s->keypad_held = held;
}

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
    { KIM1_ACT_0,   KIM1_ACT_1,   KIM1_ACT_2,   KIM1_ACT_3   },
    { KIM1_ACT_4,   KIM1_ACT_5,   KIM1_ACT_6,   KIM1_ACT_7   },
    { KIM1_ACT_8,   KIM1_ACT_9,   KIM1_ACT_A,   KIM1_ACT_B   },
    { KIM1_ACT_C,   KIM1_ACT_D,   KIM1_ACT_E,   KIM1_ACT_F   },
    { KIM1_ACT_AD,  KIM1_ACT_DA,  KIM1_ACT_PC,  KIM1_ACT_PLUS },
    { KIM1_ACT_GO,  KIM1_ACT_ST,  KIM1_ACT_RS,  0            },
};

/* ── Key labels (for visual rendering) ──────────────────────────────────── */

static const char *key_labels[6][4] = {
    {"0",  "1",  "2",  "3" },
    {"4",  "5",  "6",  "7" },
    {"8",  "9",  "A",  "B" },
    {"C",  "D",  "E",  "F" },
    {"AD", "DA", "PC", "+" },
    {"GO", "ST", "RS", NULL},
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

/* ── 7-segment segment geometry (scaled for KIM1_DIGIT_W=50, KIM1_DIGIT_H=90) */

static const struct { int x, y, w, h; } seg_rects[8] = {
    {10,  2, 30,  8},  /* a — top horizontal       */
    {33,  5,  5, 34},  /* b — top right vertical    */
    {33, 44,  5, 34},  /* c — bottom right vertical */
    {10, 80, 30,  8},  /* d — bottom horizontal     */
    { 5, 44,  5, 34},  /* e — bottom left vertical  */
    { 5,  5,  5, 34},  /* f — top left vertical     */
    {10, 40, 30,  8},  /* g — middle horizontal     */
    {40, 78,  8,  8},  /* dp — decimal point        */
};

/* ── Colours (0xAARRGGBB) ───────────────────────────────────────────────── */

#define COLOUR_LIT        0xFFFF2020u  /* bright red LED on      */
#define COLOUR_OFF        0xFF301010u  /* dim red LED off (visible) */
#define COLOUR_BG         0xFF101010u  /* dark grey background   */
#define COLOUR_KEY_BG     0xFF282828u  /* key background          */
#define COLOUR_KEY_BD     0xFF444444u  /* key border              */
#define COLOUR_KEY_LIT    0xFF604020u  /* key pressed highlight   */
#define COLOUR_KEY_TEXT   0xFFCCCCCCu  /* key label text          */

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

/* ── Font rendering ─────────────────────────────────────────────────────── */

static void draw_char(uint32_t *fb, int fb_w, int x, int y,
                      char ch, uint32_t colour) {
    int c = (int)(unsigned char)ch;
    if (c < 0 || c > 127) return;
    const uint8_t *bits = font_data[c];
    if (!bits[0] && !bits[1] && !bits[2] && !bits[3] &&
        !bits[4] && !bits[5] && !bits[6]) return;  /* empty glyph */

    for (int row = 0; row < FONT_H; row++) {
        uint8_t b = bits[row];
        for (int col = 0; col < FONT_W; col++) {
            if (b & (0x10u >> col))
                fb[(y + row) * fb_w + (x + col)] = colour;
        }
    }
}

static void draw_text(uint32_t *fb, int fb_w, int x, int y,
                      const char *s, uint32_t colour) {
    while (*s) {
        draw_char(fb, fb_w, x, y, *s, colour);
        x += FONT_W + 1;
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

            /* Key body */
            fill_rect(fb, fw, kx + 2, ky + 2, kw - 4, kh - 4,
                      pressed ? COLOUR_KEY_LIT : COLOUR_KEY_BG);
            /* Border */
            draw_rect_border(fb, fw, kx, ky, kw, kh, 2,
                             pressed ? COLOUR_KEY_LIT : COLOUR_KEY_BD);

            /* Label — center in key */
            int text_w = (int)strlen(label) * (FONT_W + 1) - 1;
            int tx = kx + (kw - text_w) / 2;
            int ty = ky + (kh - FONT_H) / 2;
            draw_text(fb, fw, tx, ty, label, COLOUR_KEY_TEXT);
        }
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

    int total_dw = (KIM1_DIGIT_W + KIM1_DIGIT_GAP) * KIM1_NUM_DIGITS - KIM1_DIGIT_GAP;
    int dox = (KIM1_FB_WIDTH - total_dw) / 2;
    int doy = 10;

    for (int d = 0; d < KIM1_NUM_DIGITS; d++) {
        uint8_t pat = segs[d];
        int dx = dox + d * (KIM1_DIGIT_W + KIM1_DIGIT_GAP);

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
                .renderer    = cfg->display_renderer,
                .actions     = NULL,
                .n_actions   = 0,
                .ini_section = NULL,
            });
        if (!s->display)
            fprintf(stderr, "gemu-kim1: failed to create display window\n");
    }

    mos6502_reset(&s->cpu);
    return s;
}

void kim1_destroy(Kim1State *s) {
    gemu_display_destroy(s->display);
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
            gemu_display_poll(s->display);
            kim1_update_keys(s, s->display);
            if (gemu_display_should_quit(s->display))
                quit = true;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                mos6502_reset(&s->cpu);
                s->u2 = (Kim1Rriot){0};
                s->u3 = (Kim1Rriot){0};
            }
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)   { quit = true; break; }
            else if (cmd == GEMU_MON_RESET) {
                mos6502_reset(&s->cpu);
                s->u2 = (Kim1Rriot){0};
                s->u3 = (Kim1Rriot){0};
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


        if (s->display) {
            kim1_render_fb(s);
            gemu_display_render(s->display, s->fb,
                                KIM1_FB_WIDTH, KIM1_FB_HEIGHT);
        }

        kim1_sleep_ms(KIM1_FRAME_MS);
    }

    printf("gemu-kim1: %llu cycles, %llu instructions\n",
           (unsigned long long)s->cpu.cycle_count,
           (unsigned long long)s->cpu.insn_count);

    gemu_monitor_stop(s->monitor);
    (void)cfg;
}
