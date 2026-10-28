/*
 * Debug > Cartridge Pins, drawn and driven the same for every backend
 * (see nes_pin_window.h):
 *
 *     labels 31-60 (rotated)
 *   [31][32][33] ... [60]      gold = contact, black = open, blinking = flaky
 *   ===================== PCB edge
 *   [ 1][ 2][ 3] ... [30]
 *     labels 1-30
 *   what the pin under the mouse carries
 *   CLOCK 1.79 MHz  [======o-----]  1789773 Hz
 *   TILT            [<] +1.5 deg [>]  right end up
 *   TP (o)/NMI (o)APU /IRQ (o)MAP /IRQ          [Reinsert] [Clock reset]
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
#define TOP_LBL  8
#define TOP_PIN  (TOP_LBL + LABEL_H + 4)
#define PIN_W    16
#define PIN_H    30
#define BAND_Y   (TOP_PIN + PIN_H + 2)
#define BAND_H   12
#define BOT_PIN  (BAND_Y + BAND_H + 2)
#define BOT_LBL  (BOT_PIN + PIN_H + 4)
#define STATUS_Y (BOT_LBL + LABEL_H + 6)
#define CLOCK_Y  (STATUS_Y + 40)
#define TILT_Y   (CLOCK_Y + 28)
#define TP_Y     (TILT_Y + 28)
#define TILT_STEP 0.5
#define LIFT_PX_PER_MM 4.0
#define LIFT_PX_MAX    10
#define SLIDER_X 176
#define SLIDER_W 300
#define READOUT_X (SLIDER_X + SLIDER_W + 16)

#define C_BG      0xFF1C1F24u
#define C_TEXT    0xFFD8D8D8u
#define C_DIM     0xFF8890A0u
#define C_GOLD    0xFFE0B040u
#define C_GOLD_HI 0xFFF6D782u
#define C_GOLD_LO 0xFFA07818u
#define C_OPEN    0xFF151517u
#define C_OPEN_ED 0xFF3C3C40u
#define C_FLAKY   0xFFFF8C1Au
#define C_PCB     0xFF2050B0u
#define C_PCB_LO  0xFF163A80u
#define C_GREEN   0xFF40C060u
#define C_LED_ON  0xFF70FF70u
#define C_LED_OFF 0xFF24402Au
#define C_BTN     0xFF3A414Au
#define C_BTN_HI  0xFF4E5764u

enum { CTL_SLIDER, CTL_TILT_LEFT, CTL_TILT_RIGHT, CTL_REINSERT, CTL_CLOCK_RESET, N_CTL };

typedef struct { int x, y, w, h; } Rect;

static const Rect ctl_rect[N_CTL] = {
    [CTL_SLIDER]      = { SLIDER_X, CLOCK_Y - 2, SLIDER_W, 20 },
    [CTL_TILT_LEFT]   = { SLIDER_X, TILT_Y - 3, 28, 22 },
    [CTL_TILT_RIGHT]  = { SLIDER_X + 112, TILT_Y - 3, 28, 22 },
    [CTL_REINSERT]    = { 420, TP_Y - 3, 92, 22 },
    [CTL_CLOCK_RESET] = { 520, TP_Y - 3, 100, 22 },
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
    if (pin == p->hover_pin) {
        fill(fb, r.x - 2, r.y - 2, r.w + 4, 1, 0xFFFFFFFFu);
        fill(fb, r.x - 2, r.y + r.h + 1, r.w + 4, 1, 0xFFFFFFFFu);
        fill(fb, r.x - 2, r.y - 2, 1, r.h + 4, 0xFFFFFFFFu);
        fill(fb, r.x + r.w + 1, r.y - 2, 1, r.h + 4, 0xFFFFFFFFu);
    }
    const char *label = nes_pin_info[pin].label;
    uint32_t lc = st == NES_PIN_GOOD ? C_DIM : st == NES_PIN_BROKEN ? 0xFFFF6060u : C_FLAKY;
    if (pin == p->hover_pin) lc = 0xFFFFFFFFu;
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

static void button(NesPinPanel *p, uint32_t *fb, int ctl, const char *label) {
    Rect r = ctl_rect[ctl];
    fill_round(fb, r.x, r.y, r.w, r.h, 4, p->hover_ctl == ctl ? C_BTN_HI : C_BTN);
    text(fb, r.x + (r.w - (int)strlen(label) * 8) / 2, r.y + (r.h - 16) / 2, label, C_TEXT);
}

static void led(uint32_t *fb, int x, const char *label, bool on) {
    int w = 24 + (int)strlen(label) * 8;
    fill_round(fb, x, TP_Y - 3, w, 22, 10, 0xFF1F3324u);
    fill_round(fb, x + 1, TP_Y - 2, w - 2, 20, 9, 0xFF16261Au);
    circle(fb, x + 11, TP_Y + 8, 4, on ? C_LED_ON : C_LED_OFF);
    text(fb, x + 20, TP_Y, label, on ? C_TEXT : C_DIM);
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
}

void nes_pin_panel_draw(NesPinPanel *p, uint32_t *fb, uint32_t now_ms) {
    p->now = now_ms;
    /* A test point stays lit for a moment after its line was asserted. */
    bool *tp[3] = { &p->pins->tp_nmi, &p->pins->tp_apu_irq, &p->pins->tp_map_irq };
    for (int i = 0; i < 3; i++) {
        if (*tp[i]) p->tp_lit_until[i] = now_ms + 120u;
        *tp[i] = false;
    }

    fill(fb, 0, 0, W, H, C_BG);
    /* the cartridge's edge, column by column so a tilt can raise it:
     * board, then the two rows of contacts */
    for (int col = -1; col <= COLS; col++) {
        int c = col < 0 ? 0 : col >= COLS ? COLS - 1 : col, dy = col_dy(p, c);
        int x = col < 0 ? X0 - 8 : col >= COLS ? X0 + COLS * PITCH : X0 + col * PITCH;
        int w = col < 0 || col >= COLS ? 8 : PITCH;
        fill(fb, x, TOP_PIN + 6 + dy, w, BOT_PIN + PIN_H - 6 - (TOP_PIN + 6), 0xFF2A3A2Eu);
        fill(fb, x, BAND_Y + dy, w, BAND_H, C_PCB);
        fill(fb, x, BAND_Y + BAND_H - 2 + dy, w, 2, C_PCB_LO);
    }
    circle(fb, X0 - 4, BAND_Y + BAND_H / 2 + col_dy(p, 0), 4, C_PCB_LO);
    circle(fb, X0 + COLS * PITCH + 3, BAND_Y + BAND_H / 2 + col_dy(p, COLS - 1), 4, C_PCB_LO);
    for (int pin = 1; pin <= NES_PINS_N; pin++) draw_pin(p, fb, pin);

    char buf[160], buf2[160];
    status_lines(p, buf, buf2, sizeof(buf));
    buf[(W - 2 * X0) / 8] = buf2[(W - 2 * X0) / 8] = '\0';
    text(fb, X0, STATUS_Y, buf, p->hover_pin ? C_TEXT : C_DIM);
    text(fb, X0, STATUS_Y + 16, buf2, C_DIM);

    /* clock */
    uint32_t hz = p->pins->clock_hz;
    snprintf(buf, sizeof(buf), "CLOCK %.2f MHz", hz / 1e6);
    text(fb, X0, CLOCK_Y, buf, C_TEXT);
    int ty = CLOCK_Y + 6, kx = slider_x_for(hz);
    fill_round(fb, SLIDER_X, ty, SLIDER_W, 5, 2, 0xFF404850u);
    fill_round(fb, SLIDER_X, ty, kx - SLIDER_X + 1, 5, 2, C_GREEN);
    int home = slider_x_for(p->pins->clock_default);
    fill(fb, home, ty - 4, 1, 3, C_DIM);
    circle(fb, kx, ty + 2, 8, p->dragging || p->hover_ctl == CTL_SLIDER ? 0xFFFFFFFFu : 0xFFD0D4DAu);
    fill_round(fb, READOUT_X, CLOCK_Y - 3, 96, 22, 3, 0xFF101214u);
    snprintf(buf, sizeof(buf), "%u", hz);
    text(fb, READOUT_X + 88 - (int)strlen(buf) * 8, CLOCK_Y, buf, hz == p->pins->clock_default ? C_TEXT : C_FLAKY);
    text(fb, READOUT_X + 102, CLOCK_Y, "Hz", C_DIM);

    /* tilt */
    double t = p->pins->tilt_deg;
    text(fb, X0, TILT_Y, "TILT", C_TEXT);
    button(p, fb, CTL_TILT_LEFT, "\x11");
    fill_round(fb, SLIDER_X + 34, TILT_Y - 3, 72, 22, 3, 0xFF101214u);
    snprintf(buf, sizeof(buf), "%+.1f\xF8", t);
    text(fb, SLIDER_X + 34 + 36 - (int)strlen(buf) * 4, TILT_Y, buf, t == 0.0 ? C_TEXT : C_FLAKY);
    button(p, fb, CTL_TILT_RIGHT, "\x10");
    if (t == 0.0) snprintf(buf, sizeof(buf), "seated straight");
    else snprintf(buf, sizeof(buf), "%s end up %.2f mm", t > 0 ? "right" : "left",
                  nes_pins_lift_mm(p->pins, t > 0 ? 30 : 1));
    text(fb, SLIDER_X + 150, TILT_Y, buf, C_DIM);

    /* test points */
    text(fb, X0, TP_Y, "TP", C_DIM);
    led(fb, X0 + 24, "/NMI", now_ms < p->tp_lit_until[0]);
    led(fb, X0 + 88, "APU /IRQ", now_ms < p->tp_lit_until[1]);
    led(fb, X0 + 184, "MAP /IRQ", now_ms < p->tp_lit_until[2]);
    button(p, fb, CTL_REINSERT, "Reinsert");
    button(p, fb, CTL_CLOCK_RESET, "Clock reset");
}

void nes_pin_panel_mouse(NesPinPanel *p, int x, int y, NesPinMouse ev) {
    if (ev == NES_PIN_MOUSE_LEAVE) {
        if (!p->dragging) { p->hover_pin = 0; p->hover_ctl = -1; }
        return;
    }
    if (ev == NES_PIN_MOUSE_UP) { p->dragging = false; return; }
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
