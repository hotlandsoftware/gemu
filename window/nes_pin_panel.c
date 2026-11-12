/*
 * Debug > Cartridge Pins, drawn and driven the same for every backend
 * (see nes_pin_window.h), in the Windows 3.1 look of GEMU's other tool
 * windows (cheat_dialog_sdl.c):
 *
 *   +---------------------------------+
 *   |   labels 31-60 (rotated)        |
 *   | [31][32][33] ... [60]           |  gold = contact, black = open,
 *   | ===================== PCB edge  |  blinking = flaky
 *   | [ 1][ 2][ 3] ... [30]           |
 *   |   labels 1-30                   |
 *   +---------------------------------+
 *   Clock:        [-------[]-------]  [ 1789773] Hz
 *   Tilt:         [<] [ +1.5 deg] [>]  right end up 1.96 mm
 *   Test points:  (o) /NMI  (o) APU /IRQ  (o) MAP /IRQ   [Reinsert] [Clock reset]
 *   ----------------------------------------------------------------
 *   status: what the pin or control under the mouse does
 *
 * Left click opens or closes a contact, right click makes it flaky; the
 * slider (or the wheel over it) sets M2, the tilt buttons (or the wheel
 * there) tip the cartridge, and each column of contacts is drawn raised
 * by how far the tilt lifted it.
 */
#include "nes_pin_window.h"
#include "vgafont16.h"
#include <stdio.h>
#include <string.h>

#define W        NES_PIN_PANEL_W
#define H        NES_PIN_PANEL_H
#define PITCH    20
#define COLS     30
#define X0       ((W - COLS * PITCH) / 2)
#define LABEL_H  80
#define TOP_LBL  10
#define TOP_PIN  (TOP_LBL + LABEL_H + 4)
#define PIN_W    16
#define PIN_H    30
#define BAND_Y   (TOP_PIN + PIN_H + 2)
#define BAND_H   12
#define BOT_PIN  (BAND_Y + BAND_H + 2)
#define BOT_LBL  (BOT_PIN + PIN_H + 4)
#define BOX_X    (X0 - 12)
#define BOX_Y    4
#define BOX_W    (COLS * PITCH + 24)
#define BOX_H    (BOT_LBL + LABEL_H + 4 - BOX_Y)
#define LABEL_X  BOX_X
#define CLOCK_Y  (BOX_Y + BOX_H + 12)
#define TILT_Y   (CLOCK_Y + 30)
#define TP_Y     (TILT_Y + 30)
#define STATUS_Y (TP_Y + 34)
#define TILT_STEP 0.5
#define LIFT_PX_PER_MM 4.0
#define LIFT_PX_MAX    10
#define CTL_X    120                    /* the controls' column, after the labels */
#define SLIDER_X (CTL_X + 6)
#define SLIDER_W 348
#define READOUT_X (SLIDER_X + SLIDER_W + 14)

/* the 3.1 palette of cheat_dialog_sdl.c */
#define C_FACE    0xFFC0C0C0u
#define C_BG      0xFFFFFFFFu
#define C_TEXT    0xFF000000u
#define C_DIM     0xFF808080u
#define C_LIGHT   0xFFFFFFFFu
#define C_DARK    0xFF808080u
#define C_HI      0xFF000080u
#define C_ERR     0xFF800000u
#define C_FLAKY_TX 0xFFA05000u
/* the connector picture */
#define C_GOLD    0xFFE0B040u
#define C_GOLD_HI 0xFFF6D782u
#define C_GOLD_LO 0xFFA07818u
#define C_OPEN    0xFF151517u
#define C_OPEN_ED 0xFF3C3C40u
#define C_FLAKY   0xFFFF8C1Au
#define C_PCB     0xFF2050B0u
#define C_PCB_LO  0xFF163A80u
#define C_BOARD   0xFF2A3A2Eu
#define C_LED_ON  0xFF00FF00u
#define C_LED_OFF 0xFF005000u

enum { CTL_SLIDER, CTL_TILT_LEFT, CTL_TILT_RIGHT, CTL_REINSERT, CTL_CLOCK_RESET, N_CTL };

typedef struct { int x, y, w, h; } Rect;

static const Rect ctl_rect[N_CTL] = {
    [CTL_SLIDER]      = { SLIDER_X, CLOCK_Y - 2, SLIDER_W, 20 },
    [CTL_TILT_LEFT]   = { CTL_X, TILT_Y - 3, 24, 22 },
    [CTL_TILT_RIGHT]  = { CTL_X + 108, TILT_Y - 3, 24, 22 },
    [CTL_REINSERT]    = { W - 8 - 104 - 8 - 88, TP_Y - 4, 88, 24 },
    [CTL_CLOCK_RESET] = { W - 8 - 104, TP_Y - 4, 104, 24 },
};

static bool in_rect(Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* ── Drawing primitives ──────────────────────────────────────────────────── */

static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t c) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++)
            fb[j * W + i] = c;
}

/* A rectangle with its corners rounded off by r pixels. */
static void fill_round(uint32_t *fb, int x, int y, int w, int h, int r, uint32_t c) {
    for (int j = 0; j < h; j++) {
        int dy = j < r ? r - j : j >= h - r ? j - (h - r - 1) : 0;
        int cut = 0;
        while (cut < r && (r - cut) * (r - cut) + dy * dy > r * r + r) cut++;
        if (dy == 0) cut = 0;
        fill(fb, x + cut, y + j, w - 2 * cut, 1, c);
    }
}

static void circle(uint32_t *fb, int cx, int cy, int r, uint32_t c) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r + r && cx + i >= 0 && cx + i < W && cy + j >= 0 && cy + j < H)
                fb[(cy + j) * W + cx + i] = c;
}

static void frame(uint32_t *fb, int x, int y, int w, int h, uint32_t c) {
    fill(fb, x, y, w, 1, c);
    fill(fb, x, y + h - 1, w, 1, c);
    fill(fb, x, y, 1, h, c);
    fill(fb, x + w - 1, y, 1, h, c);
}

/* A white box with a black frame: the list and text fields of the 3.1 look. */
static void field(uint32_t *fb, int x, int y, int w, int h) {
    fill(fb, x, y, w, h, C_BG);
    frame(fb, x, y, w, h, C_TEXT);
}

/* The raised 3.1 face of a push button or slider thumb (no border). */
static void bevel(uint32_t *fb, int x, int y, int w, int h) {
    fill(fb, x, y, w, h, C_FACE);
    fill(fb, x, y, w - 1, 2, C_LIGHT);
    fill(fb, x, y, 2, h - 1, C_LIGHT);
    fill(fb, x + 1, y + h - 2, w - 1, 2, C_DARK);
    fill(fb, x + w - 2, y + 1, 2, h - 1, C_DARK);
}

static void text(uint32_t *fb, int x, int y, const char *s, uint32_t c) {
    for (; *s; s++, x += 8) {
        const uint8_t *g = &vgafont16[(unsigned char)*s * 16];
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 8; i++)
                if ((g[j] & (0x80u >> i)) && x + i >= 0 && x + i < W && y + j >= 0 && y + j < H)
                    fb[(y + j) * W + x + i] = c;
    }
}

/* Text turned a quarter left (reading bottom to top), its first
 * character's bottom edge at y. */
static void text_up(uint32_t *fb, int x, int y, const char *s, uint32_t c) {
    for (int k = 0; s[k]; k++) {
        const uint8_t *g = &vgafont16[(unsigned char)s[k] * 16];
        for (int gy = 0; gy < 16; gy++)
            for (int gx = 0; gx < 8; gx++) {
                int px = x + gy, py = y - (k * 8 + gx);
                if ((g[gy] & (0x80u >> gx)) && px >= 0 && px < W && py >= 0 && py < H)
                    fb[py * W + px] = c;
            }
    }
}

/* ── The connector ───────────────────────────────────────────────────────── */

/* How far up a column of contacts is drawn: the tilt's lift, exaggerated. */
static int col_dy(const NesPinPanel *p, int col) {
    double px = nes_pins_lift_mm(p->pins, col + 1) * LIFT_PX_PER_MM;
    return -(px > LIFT_PX_MAX ? LIFT_PX_MAX : (int)(px + 0.5));
}

static int pin_col(int pin) { return pin > 30 ? pin - 31 : pin - 1; }

static void pin_rect(const NesPinPanel *p, int pin, Rect *r) {
    r->x = X0 + pin_col(pin) * PITCH + (PITCH - PIN_W) / 2;
    r->y = (pin > 30 ? TOP_PIN : BOT_PIN) + col_dy(p, pin_col(pin));
    r->w = PIN_W;
    r->h = PIN_H;
}

static int pin_at(const NesPinPanel *p, int x, int y) {
    for (int pin = 1; pin <= NES_PINS_N; pin++) {
        Rect r;
        pin_rect(p, pin, &r);
        r.x -= 2; r.w += 4;              /* the whole pitch is clickable */
        if (in_rect(r, x, y)) return pin;
        /* and its label */
        Rect l = { r.x, (pin > 30 ? TOP_LBL : BOT_LBL) + col_dy(p, pin_col(pin)), r.w, LABEL_H };
        if (in_rect(l, x, y)) return pin;
    }
    return 0;
}

static void draw_pin(NesPinPanel *p, uint32_t *fb, int pin) {
    Rect r;
    pin_rect(p, pin, &r);
    NesPinState st = nes_pins_get(p->pins, pin);
    bool blink_open = st == NES_PIN_FLAKY && (p->now / 160u) % 3u == 0;
    bool gold = st == NES_PIN_GOOD || (st == NES_PIN_FLAKY && !blink_open);
    if (gold) {
        fill_round(fb, r.x, r.y, r.w, r.h, 3, C_GOLD_LO);
        fill_round(fb, r.x + 1, r.y + 1, r.w - 3, r.h - 3, 3, C_GOLD);
        fill(fb, r.x + 3, r.y + 3, r.w - 7, 2, C_GOLD_HI);
    } else {
        fill_round(fb, r.x, r.y, r.w, r.h, 3, C_OPEN_ED);
        fill_round(fb, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 3, C_OPEN);
    }
    if (st == NES_PIN_FLAKY) {
        fill(fb, r.x, r.y + r.h + 1, r.w, 2, C_FLAKY);
    }
    char num[4];
    snprintf(num, sizeof(num), "%d", pin);
    int tw = (int)strlen(num) * 8;
    text(fb, r.x + (r.w - tw) / 2, r.y + (r.h - 16) / 2, num, gold ? 0xFF3A2A08u : C_DIM);
    if (pin == p->hover_pin) frame(fb, r.x - 2, r.y - 2, r.w + 4, r.h + 4, C_LIGHT);
    const char *label = nes_pin_info[pin].label;
    uint32_t lc = st == NES_PIN_GOOD ? C_TEXT : st == NES_PIN_BROKEN ? C_ERR : C_FLAKY_TX;
    if (pin == p->hover_pin) lc = C_HI;
    int lx = X0 + pin_col(pin) * PITCH + (PITCH - 16) / 2, dy = col_dy(p, pin_col(pin));
    if (pin > 30) text_up(fb, lx, TOP_LBL + LABEL_H - 1 + dy, label, lc);                  /* ends at the contact */
    else          text_up(fb, lx, BOT_LBL + (int)strlen(label) * 8 - 1 + dy, label, lc);   /* starts at it */
}

/* ── Controls ────────────────────────────────────────────────────────────── */

static int slider_x_for(uint32_t hz) {
    double f = (double)(hz - NES_PIN_CLOCK_MIN) / (NES_PIN_CLOCK_MAX - NES_PIN_CLOCK_MIN);
    return SLIDER_X + (int)(f * (SLIDER_W - 1) + 0.5);
}

static void set_clock(NesPinPanel *p, uint32_t hz) {
    if (hz < NES_PIN_CLOCK_MIN) hz = NES_PIN_CLOCK_MIN;
    if (hz > NES_PIN_CLOCK_MAX) hz = NES_PIN_CLOCK_MAX;
    p->pins->clock_hz = hz;
}

static void slider_to(NesPinPanel *p, int x) {
    /* snap to the console's own clock near its mark */
    int home = slider_x_for(p->pins->clock_default);
    if (x >= home - 4 && x <= home + 4) { set_clock(p, p->pins->clock_default); return; }
    double f = (double)(x - SLIDER_X) / (SLIDER_W - 1);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    set_clock(p, NES_PIN_CLOCK_MIN + (uint32_t)(f * (NES_PIN_CLOCK_MAX - NES_PIN_CLOCK_MIN)));
}

/* A 3.1 push button (as cheat_dialog_sdl.c's), pressed while clicked. */
static void button(NesPinPanel *p, uint32_t *fb, int ctl, const char *label) {
    Rect r = ctl_rect[ctl];
    fill(fb, r.x + 1, r.y, r.w - 2, 1, C_TEXT);
    fill(fb, r.x + 1, r.y + r.h - 1, r.w - 2, 1, C_TEXT);
    fill(fb, r.x, r.y + 1, 1, r.h - 2, C_TEXT);
    fill(fb, r.x + r.w - 1, r.y + 1, 1, r.h - 2, C_TEXT);
    bool down = p->pressed_ctl == ctl && p->hover_ctl == ctl;
    if (down) fill(fb, r.x + 1, r.y + 1, r.w - 2, r.h - 2, C_FACE);
    else      bevel(fb, r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    int o = down ? 1 : 0;
    text(fb, r.x + (r.w - (int)strlen(label) * 8) / 2 + o, r.y + (r.h - 16) / 2 + o, label, C_TEXT);
}

/* returns the x after it */
static int led(uint32_t *fb, int x, const char *label, bool on) {
    circle(fb, x + 6, TP_Y + 8, 6, C_TEXT);
    circle(fb, x + 6, TP_Y + 8, 5, on ? C_LED_ON : C_LED_OFF);
    text(fb, x + 18, TP_Y, label, C_TEXT);
    return x + 18 + (int)strlen(label) * 8 + 18;
}

static void status_lines(NesPinPanel *p, char *l1, char *l2, size_t len) {
    l2[0] = '\0';
    if (p->hover_pin) {
        static const char *const st_name[3] = { "contact good", "OPEN", "FLAKY (1 access in 4)" };
        int pin = p->hover_pin;
        double lift = nes_pins_lift_mm(p->pins, pin);
        char by[48] = "";
        if (nes_pins_get(p->pins, pin) != nes_pins_get_user(p->pins, pin))
            snprintf(by, sizeof(by), " - tilt lifted it %.2f mm", lift);
        snprintf(l1, len, "Pin %d  %s  -  %s%s", pin, nes_pin_info[pin].label,
                 st_name[nes_pins_get(p->pins, pin)], by);
        snprintf(l2, len, "%s", nes_pin_info[pin].what);
    } else if (p->hover_ctl == CTL_SLIDER || p->dragging) {
        snprintf(l1, len, "M2, the CPU clock: the PPU and APU follow it");
        snprintf(l2, len, "%.1f-%.1f MHz; the wheel steps 5 kHz", NES_PIN_CLOCK_MIN / 1e6,
                 NES_PIN_CLOCK_MAX / 1e6);
    } else if (p->hover_ctl == CTL_TILT_LEFT || p->hover_ctl == CTL_TILT_RIGHT) {
        snprintf(l1, len, "Tilt the cartridge %.1f deg; it pivots on the end still seated", TILT_STEP);
        snprintf(l2, len, "Contacts go flaky past 0.75 mm of lift, open past 1.5 mm (wheel works)");
    } else if (p->hover_ctl == CTL_REINSERT) {
        snprintf(l1, len, "Reseat the cartridge straight: every contact good");
        snprintf(l2, len, "The game keeps running; press Reset if it crashed");
    } else if (p->hover_ctl == CTL_CLOCK_RESET) {
        snprintf(l1, len, "Back to the console's own clock, %u Hz", p->pins->clock_default);
    } else {
        snprintf(l1, len, "Click a contact to open it (black); right-click makes it flaky.");
        snprintf(l2, len, "Hover for what a pin carries. Reset the console to recover.");
    }
}

/* ── Public ──────────────────────────────────────────────────────────────── */

void nes_pin_panel_init(NesPinPanel *p, NesPins *pins) {
    memset(p, 0, sizeof(*p));
    p->pins = pins;
    p->hover_ctl = -1;
    p->pressed_ctl = -1;
}

void nes_pin_panel_draw(NesPinPanel *p, uint32_t *fb, uint32_t now_ms) {
    p->now = now_ms;
    /* A test point stays lit for a moment after its line was asserted. */
    bool *tp[3] = { &p->pins->tp_nmi, &p->pins->tp_apu_irq, &p->pins->tp_map_irq };
    for (int i = 0; i < 3; i++) {
        if (*tp[i]) p->tp_lit_until[i] = now_ms + 120u;
        *tp[i] = false;
    }

    fill(fb, 0, 0, W, H, C_FACE);
    /* the connector, in a box like the Game Genie's list; the cartridge's
     * edge column by column so a tilt can raise it: board, then contacts */
    field(fb, BOX_X, BOX_Y, BOX_W, BOX_H);
    for (int col = -1; col <= COLS; col++) {
        int c = col < 0 ? 0 : col >= COLS ? COLS - 1 : col, dy = col_dy(p, c);
        int x = col < 0 ? X0 - 8 : col >= COLS ? X0 + COLS * PITCH : X0 + col * PITCH;
        int w = col < 0 || col >= COLS ? 8 : PITCH;
        fill(fb, x, TOP_PIN + 6 + dy, w, BOT_PIN + PIN_H - 6 - (TOP_PIN + 6), C_BOARD);
        fill(fb, x, BAND_Y + dy, w, BAND_H, C_PCB);
        fill(fb, x, BAND_Y + BAND_H - 2 + dy, w, 2, C_PCB_LO);
    }
    circle(fb, X0 - 4, BAND_Y + BAND_H / 2 + col_dy(p, 0), 4, C_PCB_LO);
    circle(fb, X0 + COLS * PITCH + 3, BAND_Y + BAND_H / 2 + col_dy(p, COLS - 1), 4, C_PCB_LO);
    for (int pin = 1; pin <= NES_PINS_N; pin++) draw_pin(p, fb, pin);

    char buf[160], buf2[160];

    /* clock: a 3.1 trackbar and the value in a field */
    uint32_t hz = p->pins->clock_hz;
    text(fb, LABEL_X, CLOCK_Y, "Clock:", C_TEXT);
    int ty = CLOCK_Y + 5, kx = slider_x_for(hz);
    field(fb, SLIDER_X - 4, ty, SLIDER_W + 8, 6);
    int home = slider_x_for(p->pins->clock_default);
    fill(fb, home, ty + 9, 1, 5, C_TEXT);                 /* the console's own clock */
    frame(fb, kx - 6, CLOCK_Y - 3, 12, 22, C_TEXT);
    bevel(fb, kx - 5, CLOCK_Y - 2, 10, 20);
    field(fb, READOUT_X, CLOCK_Y - 3, 96, 22);
    snprintf(buf, sizeof(buf), "%u", hz);
    text(fb, READOUT_X + 90 - (int)strlen(buf) * 8, CLOCK_Y, buf, hz == p->pins->clock_default ? C_TEXT : C_ERR);
    text(fb, READOUT_X + 102, CLOCK_Y, "Hz", C_TEXT);

    /* tilt */
    double t = p->pins->tilt_deg;
    text(fb, LABEL_X, TILT_Y, "Tilt:", C_TEXT);
    button(p, fb, CTL_TILT_LEFT, "\x11");
    field(fb, CTL_X + 30, TILT_Y - 3, 72, 22);
    snprintf(buf, sizeof(buf), "%+.1f\xF8", t);
    text(fb, CTL_X + 30 + 36 - (int)strlen(buf) * 4, TILT_Y, buf, t == 0.0 ? C_TEXT : C_ERR);
    button(p, fb, CTL_TILT_RIGHT, "\x10");
    if (t == 0.0) snprintf(buf, sizeof(buf), "seated straight");
    else snprintf(buf, sizeof(buf), "%s end up %.2f mm", t > 0 ? "right" : "left",
                  nes_pins_lift_mm(p->pins, t > 0 ? 30 : 1));
    text(fb, CTL_X + 144, TILT_Y, buf, C_TEXT);

    /* test points and the buttons */
    text(fb, LABEL_X, TP_Y, "Test points:", C_TEXT);
    int lx = led(fb, CTL_X, "/NMI", now_ms < p->tp_lit_until[0]);
    lx = led(fb, lx, "APU /IRQ", now_ms < p->tp_lit_until[1]);
    led(fb, lx, "MAP /IRQ", now_ms < p->tp_lit_until[2]);
    button(p, fb, CTL_REINSERT, "Reinsert");
    button(p, fb, CTL_CLOCK_RESET, "Clock reset");

    /* status line, as the Game Genie's */
    fill(fb, 0, STATUS_Y - 6, W, 1, C_DARK);
    status_lines(p, buf, buf2, sizeof(buf));
    buf[(W - 2 * LABEL_X) / 8] = buf2[(W - 2 * LABEL_X) / 8] = '\0';
    text(fb, LABEL_X, STATUS_Y, buf, C_TEXT);
    text(fb, LABEL_X, STATUS_Y + 16, buf2, C_TEXT);
}

void nes_pin_panel_mouse(NesPinPanel *p, int x, int y, NesPinMouse ev) {
    if (ev == NES_PIN_MOUSE_LEAVE) {
        if (!p->dragging) { p->hover_pin = 0; p->hover_ctl = -1; }
        return;
    }
    if (ev == NES_PIN_MOUSE_UP) { p->dragging = false; p->pressed_ctl = -1; return; }
    if (p->dragging) { slider_to(p, x); return; }

    p->hover_pin = pin_at(p, x, y);
    p->hover_ctl = -1;
    for (int c = 0; c < N_CTL; c++) {
        Rect r = ctl_rect[c];
        if (c == CTL_SLIDER) { r.x -= 8; r.w += 16; }
        if (in_rect(r, x, y)) p->hover_ctl = c;
    }
    if (ev == NES_PIN_MOUSE_MOVE) return;

    if (p->hover_pin) {
        NesPinState st = nes_pins_get_user(p->pins, p->hover_pin);   /* the tilt's faults aren't clicked away */
        NesPinState want = ev == NES_PIN_MOUSE_LEFT ? (st == NES_PIN_GOOD ? NES_PIN_BROKEN : NES_PIN_GOOD)
                                                    : (st == NES_PIN_FLAKY ? NES_PIN_GOOD : NES_PIN_FLAKY);
        nes_pins_set(p->pins, p->hover_pin, want);
    } else if (ev == NES_PIN_MOUSE_LEFT) {
        p->pressed_ctl = p->hover_ctl;
        switch (p->hover_ctl) {
        case CTL_SLIDER:      p->dragging = true; slider_to(p, x); break;
        case CTL_TILT_LEFT:   nes_pins_set_tilt(p->pins, p->pins->tilt_deg - TILT_STEP); break;
        case CTL_TILT_RIGHT:  nes_pins_set_tilt(p->pins, p->pins->tilt_deg + TILT_STEP); break;
        case CTL_REINSERT:    nes_pins_reinsert(p->pins); break;
        case CTL_CLOCK_RESET: p->pins->clock_hz = p->pins->clock_default; break;
        default: break;
        }
    }
}

void nes_pin_panel_wheel(NesPinPanel *p, int x, int y, int notches) {
    Rect r = ctl_rect[CTL_SLIDER];
    r.x -= 8; r.w += 16;
    if (in_rect(r, x, y) || in_rect((Rect){ READOUT_X, CLOCK_Y - 3, 120, 22 }, x, y))
        set_clock(p, (uint32_t)((int64_t)p->pins->clock_hz + (int64_t)notches * 5000));
    else if (in_rect((Rect){ SLIDER_X, TILT_Y - 3, 140, 22 }, x, y))
        nes_pins_set_tilt(p->pins, p->pins->tilt_deg + notches * TILT_STEP);
}
