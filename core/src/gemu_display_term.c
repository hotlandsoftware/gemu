#ifndef _WIN32
/*
 * ANSI terminal display backend — pixterm-style graphics without libcaca.
 *
 * Each character cell shows two vertically stacked framebuffer samples via
 * U+2580 UPPER HALF BLOCK: foreground color = top pixel, background color =
 * bottom pixel.  Colors are 24-bit SGR sequences when the terminal
 * advertises truecolor (COLORTERM=truecolor/24bit, overridable with
 * GEMU_TERM_COLOR=truecolor|256), otherwise the xterm 256-color cube.
 *
 * The framebuffer is box-filtered down to whatever fits the terminal and
 * re-drawn with per-cell diffing, so static screens cost almost nothing
 * and SSH sessions stay usable.
 *
 * Input: raw non-canonical stdin.  On terminals that support the kitty
 * keyboard protocol (kitty, foot, WezTerm, Ghostty, Alacritty, ...) we get
 * real key press/release events — input quality matches SDL.  Elsewhere,
 * terminals only report key *presses* plus autorepeat, so a bound action
 * stays "held" for GEMU_TERM_KEY_MS milliseconds (default 250) after its
 * last repeat.  Lower values make taps release promptly; higher values can
 * smooth long holds on terminals without release events.
 *
 * Output is capped at ~30 fps and adaptively decimated when the terminal
 * can't drain frames fast enough, so a slow terminal degrades the frame
 * rate instead of stalling the emulation (and its input polling).
 */
#include "gemu_display_priv.h"
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TERM_MAX_BINDINGS GEMU_DISPLAY_MAX_ACTIONS

/* Special-key tokens (printable keys use their lowercased ASCII value) */
enum {
    TK_UP = 0x1000, TK_DOWN, TK_LEFT, TK_RIGHT,
    TK_RETURN, TK_TAB, TK_BACKSPACE, TK_DELETE, TK_ESCAPE,
    TK_F1, TK_F2, TK_F3, TK_F4, TK_F5, TK_F6,
    TK_F7, TK_F8, TK_F9, TK_F10, TK_F11, TK_F12,
    /* Modifier keys — only observable with the kitty protocol */
    TK_LSHIFT, TK_LCTRL, TK_LALT, TK_LSUPER, TK_LHYPER, TK_LMETA,
    TK_RSHIFT, TK_RCTRL, TK_RALT, TK_RSUPER, TK_RHYPER, TK_RMETA,
};

typedef struct {
    struct termios old_termios;
    bool termios_active;
    int  old_fl;
    bool screen_active;

    int  fb_w, fb_h;
    bool truecolor;

    /* Current terminal-fit geometry (cells) and cell colors of the frame
     * on screen; top/bottom packed RGB per cell. */
    int tw, th;
    int off_x, off_y;             /* centering offset in cells */
    uint32_t *shown;              /* 2 * tw * th entries, 0xFFFFFFFF = dirty */

    char  *out;                   /* per-frame output buffer */
    size_t out_len, out_cap;

    uint64_t next_render_ms;      /* frame pacing / adaptive decimation */

    /* Input */
    struct {
        int      key;
        uint32_t bit;
        uint64_t held_until;      /* legacy autorepeat synthesis */
        bool     held_now;        /* kitty protocol: exact key level */
    } bindings[TERM_MAX_BINDINGS];
    int      n_bindings;
    unsigned hold_ms;
    bool     kitty_active;        /* kitty keyboard protocol enabled */
    unsigned char pend[16];       /* partial escape sequence across reads */
    int      n_pend;
} TermBackend;

/* ── Small helpers ───────────────────────────────────────────────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void out_reserve(TermBackend *b, size_t extra) {
    if (b->out_len + extra <= b->out_cap) return;
    size_t cap = b->out_cap ? b->out_cap : 65536;
    while (cap < b->out_len + extra) cap *= 2;
    char *next = realloc(b->out, cap);
    if (!next) return;
    b->out = next;
    b->out_cap = cap;
}

static void outs(TermBackend *b, const char *s) {
    size_t n = strlen(s);
    out_reserve(b, n);
    if (b->out_len + n <= b->out_cap) {
        memcpy(b->out + b->out_len, s, n);
        b->out_len += n;
    }
}

static void outf(TermBackend *b, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) outs(b, tmp);
}

static void term_flush(TermBackend *b) {
    size_t off = 0;
    while (off < b->out_len) {
        ssize_t n = write(STDOUT_FILENO, b->out + off, b->out_len - off);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        off += (size_t)n;
    }
    b->out_len = 0;
}

/* ── Color emission ──────────────────────────────────────────────────────── */

static int rgb_to_256(uint32_t rgb) {
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
    /* Prefer the gray ramp for near-gray colors: finer steps than the cube */
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx - mn < 24) {
        int lum = (r + g + b) / 3;
        if (lum < 8)   return 16;    /* cube black */
        if (lum > 238) return 231;   /* cube white */
        return 232 + (lum - 8) * 24 / 240;
    }
    return 16 + 36 * (r * 6 / 256) + 6 * (g * 6 / 256) + (b * 6 / 256);
}

static void emit_colors(TermBackend *b, uint32_t top, uint32_t bot,
                        uint32_t *cur_top, uint32_t *cur_bot) {
    if (b->truecolor) {
        if (top != *cur_top)
            outf(b, "\033[38;2;%u;%u;%um",
                 (top >> 16) & 0xFF, (top >> 8) & 0xFF, top & 0xFF);
        if (bot != *cur_bot)
            outf(b, "\033[48;2;%u;%u;%um",
                 (bot >> 16) & 0xFF, (bot >> 8) & 0xFF, bot & 0xFF);
    } else {
        if (top != *cur_top) outf(b, "\033[38;5;%dm", rgb_to_256(top));
        if (bot != *cur_bot) outf(b, "\033[48;5;%dm", rgb_to_256(bot));
    }
    *cur_top = top;
    *cur_bot = bot;
}

/* ── Rendering ───────────────────────────────────────────────────────────── */

/* Box-average the fb rect covering sample row `sy` of `total_sy` rows and
 * sample column `sx` of `total_sx` — integer arithmetic, no libm. */
static uint32_t sample_box(const uint32_t *argb, int fw, int fh,
                           int sx, int total_sx, int sy, int total_sy) {
    int x0 = sx * fw / total_sx, x1 = (sx + 1) * fw / total_sx;
    int y0 = sy * fh / total_sy, y1 = (sy + 1) * fh / total_sy;
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    if (x1 > fw) x1 = fw;
    if (y1 > fh) y1 = fh;
    uint32_t r = 0, g = 0, bl = 0, n = 0;
    for (int y = y0; y < y1; y++) {
        const uint32_t *row = argb + (size_t)y * (size_t)fw;
        for (int x = x0; x < x1; x++) {
            uint32_t p = row[x];
            r  += (p >> 16) & 0xFF;
            g  += (p >> 8) & 0xFF;
            bl += p & 0xFF;
            n++;
        }
    }
    if (!n) return 0;
    return ((r / n) << 16) | ((g / n) << 8) | (bl / n);
}

static void term_geometry(TermBackend *b, GemuDisplay *d) {
    struct winsize ws = { 0 };
    int cols = 80, rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    /* Fit fb into cols x (rows*2) samples, preserving aspect (cell ≈ 1x2 px). */
    int max_w = cols, max_h = rows * 2;
    int tw = max_w, th_px = b->fb_h * max_w / b->fb_w;
    if (th_px > max_h) {
        th_px = max_h;
        tw = b->fb_w * max_h / b->fb_h;
        if (tw < 1) tw = 1;
    }
    int th = (th_px + 1) / 2;
    if (th < 1) th = 1;

    if (tw != b->tw || th != b->th || !b->shown) {
        free(b->shown);
        b->tw = tw;
        b->th = th;
        b->off_x = (cols - tw) / 2;
        b->off_y = (rows - th) / 2;
        if (b->off_x < 0) b->off_x = 0;
        if (b->off_y < 0) b->off_y = 0;
        size_t n = (size_t)tw * (size_t)th * 2;
        b->shown = malloc(n * sizeof(uint32_t));
        if (b->shown) memset(b->shown, 0xFF, n * sizeof(uint32_t));  /* all dirty */
        outs(b, "\033[2J");
    }
    (void)d;
}

static void term_do_render(GemuDisplay *d, const uint32_t *argb, int w, int h) {
    TermBackend *b = d->backend;
    if (w <= 0 || h <= 0) return;
    uint64_t start = now_ms();
    if (start < b->next_render_ms) return;   /* pace: skip, diff catches up */
    b->fb_w = w;
    b->fb_h = h;
    term_geometry(b, d);
    if (!b->shown) return;

    uint32_t cur_top = 0xFFFFFFFFu, cur_bot = 0xFFFFFFFFu;
    int pen_x = -1, pen_y = -1;

    for (int cy = 0; cy < b->th; cy++) {
        for (int cx = 0; cx < b->tw; cx++) {
            uint32_t top = sample_box(argb, w, h, cx, b->tw, cy * 2,     b->th * 2);
            uint32_t bot = sample_box(argb, w, h, cx, b->tw, cy * 2 + 1, b->th * 2);
            uint32_t *sh = &b->shown[((size_t)cy * (size_t)b->tw + (size_t)cx) * 2];
            if (sh[0] == top && sh[1] == bot) continue;
            sh[0] = top;
            sh[1] = bot;
            if (pen_y != cy || pen_x != cx) {
                outf(b, "\033[%d;%dH", b->off_y + cy + 1, b->off_x + cx + 1);
                pen_y = cy;
                pen_x = cx;
            }
            if (top == bot) {
                /* Flat cell: a space needs only the background color */
                if (bot != cur_bot) {
                    if (b->truecolor)
                        outf(b, "\033[48;2;%u;%u;%um",
                             (bot >> 16) & 0xFF, (bot >> 8) & 0xFF, bot & 0xFF);
                    else
                        outf(b, "\033[48;5;%dm", rgb_to_256(bot));
                    cur_bot = bot;
                }
                outs(b, " ");
            } else {
                emit_colors(b, top, bot, &cur_top, &cur_bot);
                outs(b, "\xE2\x96\x80");      /* U+2580 UPPER HALF BLOCK */
            }
            pen_x++;
        }
    }
    if (b->out_len) {
        outs(b, "\033[0m\033[H");
        term_flush(b);
    }
    /* Adaptive pacing: never faster than ~30 fps, and keep the time spent
     * writing under ~half the wall clock so slow terminals (SSH) drop frames
     * instead of stalling the emulation loop. */
    uint64_t cost = now_ms() - start;
    uint64_t min_iv = 33;
    if (cost * 2 > min_iv) min_iv = cost * 2;
    b->next_render_ms = start + min_iv;
}

/* ── Input ───────────────────────────────────────────────────────────────── */

static int name_to_token(const char *name) {
    if (name[0] && !name[1]) {
        unsigned char c = (unsigned char)name[0];
        if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
        if (c >= 0x20 && c <= 0x7E) return c;
    }
    if (!strcasecmp(name, "Return"))    return TK_RETURN;
    if (!strcasecmp(name, "Tab"))       return TK_TAB;
    if (!strcasecmp(name, "Space"))     return ' ';
    if (!strcasecmp(name, "Backspace")) return TK_BACKSPACE;
    if (!strcasecmp(name, "Delete"))    return TK_DELETE;
    if (!strcasecmp(name, "Up"))        return TK_UP;
    if (!strcasecmp(name, "Down"))      return TK_DOWN;
    if (!strcasecmp(name, "Left"))      return TK_LEFT;
    if (!strcasecmp(name, "Right"))     return TK_RIGHT;
    if (!strcasecmp(name, "Escape"))      return TK_ESCAPE;
    if (!strcasecmp(name, "Left Shift"))  return TK_LSHIFT;
    if (!strcasecmp(name, "Left Ctrl"))   return TK_LCTRL;
    if (!strcasecmp(name, "Left Alt"))    return TK_LALT;
    if (!strcasecmp(name, "Right Shift")) return TK_RSHIFT;
    if (!strcasecmp(name, "Right Ctrl"))  return TK_RCTRL;
    if (!strcasecmp(name, "Right Alt"))   return TK_RALT;
    if (name[0] == 'F' && name[1]) {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12) return TK_F1 + n - 1;
    }
    return 0;   /* not representable in a terminal */
}

/* event: 1 = press, 2 = repeat, 3 = release (kitty); legacy input is
 * always a press. */
static void term_key(GemuDisplay *d, int token, int event, uint32_t raw_cp) {
    TermBackend *b = d->backend;
    if (!token && !raw_cp) return;
    if (event != 3 && raw_cp)
        gemu_display_push_raw(d, raw_cp);
    if (token == TK_ESCAPE) {
        if (event == 1) d->quit = true;
        return;
    }
    uint64_t until = now_ms() + b->hold_ms;
    for (int i = 0; i < b->n_bindings; i++) {
        if (b->bindings[i].key != token) continue;
        if (b->kitty_active) b->bindings[i].held_now = (event != 3);
        else                 b->bindings[i].held_until = until;
    }
}

static void term_token(GemuDisplay *d, int token, uint32_t raw_cp) {
    term_key(d, token, 1, raw_cp);
}

/* kitty functional keycodes (CSI <code> u) → tokens */
static int kitty_code_to_token(int code) {
    switch (code) {
    case 13:    return TK_RETURN;
    case 9:     return TK_TAB;
    case 8: case 127: return TK_BACKSPACE;
    case 27:    return TK_ESCAPE;
    case 57414: return TK_RETURN;    /* keypad Enter */
    default: break;
    }
    if (code >= 57441 && code <= 57452)      /* modifiers, L then R */
        return TK_LSHIFT + (code - 57441);
    if (code >= 57364 && code <= 57375)      /* F1..F12 */
        return TK_F1 + (code - 57364);
    if (code >= 'A' && code <= 'Z') return code - 'A' + 'a';
    if (code >= 0x20 && code <= 0x7E) return code;
    return 0;
}

/* Full CSI parser: handles kitty key events (CSI code[:alts];mods[:event] u
 * plus parametrised legacy finals), the kitty capability query reply, and
 * classic arrow/F-key sequences.  buf points at ESC; returns bytes
 * consumed, 0 = need more input. */
static int term_csi(GemuDisplay *d, const unsigned char *buf, int n) {
    TermBackend *b = d->backend;
    int i = 2;
    unsigned char prefix = 0;
    if (i < n && (buf[i] == '?' || buf[i] == '<' || buf[i] == '>' || buf[i] == '='))
        prefix = buf[i++];

    int  p[4][3] = { { 0 } };
    bool has[4][3] = { { false } };
    int  np = 0, ns = 0;
    while (i < n) {
        unsigned char c = buf[i];
        if (c >= '0' && c <= '9') {
            if (np < 4 && ns < 3) {
                p[np][ns] = p[np][ns] * 10 + (c - '0');
                has[np][ns] = true;
            }
            i++;
        } else if (c == ':') {
            if (ns < 2) ns++;
            i++;
        } else if (c == ';') {
            if (np < 3) np++;
            ns = 0;
            i++;
        } else {
            break;
        }
    }
    if (i >= n) return 0;
    unsigned char final = buf[i++];
    if (final < 0x40 || final > 0x7E) return i;

    if (prefix == '?') {
        if (final == 'u' && !b->kitty_active) {
            /* Terminal supports the kitty protocol: enable disambiguated
             * escape codes + event types + all-keys-as-escapes (1|2|8). */
            const char *en = "\033[>11u";
            ssize_t rc = write(STDOUT_FILENO, en, strlen(en));
            (void)rc;
            b->kitty_active = true;
        }
        return i;      /* also swallows the DA1 reply (CSI ? ... c) */
    }
    if (prefix) return i;

    int event = has[1][1] ? p[1][1] : 1;
    int mods  = has[1][0] ? p[1][0] - 1 : 0;

    switch (final) {
    case 'u': {
        int code    = p[0][0];
        int shifted = has[0][1] ? p[0][1] : 0;
        if (code == 'c' && (mods & 4) && event == 1) {   /* Ctrl-C */
            d->quit = true;
            return i;
        }
        uint32_t raw = 0;
        if (code == 13) raw = '\r';
        else if (code == 9) raw = '\t';
        else if (code == 8 || code == 127) raw = '\b';
        else if (code >= 0x20 && code <= 0x10FFFF && code < 57344) {
            raw = shifted ? (uint32_t)shifted : (uint32_t)code;
            if (!shifted && (mods & 1) && code >= 'a' && code <= 'z')
                raw = (uint32_t)(code - 'a' + 'A');
        }
        term_key(d, kitty_code_to_token(code), event, raw);
        return i;
    }
    case 'A': term_key(d, TK_UP, event, 0);    return i;
    case 'B': term_key(d, TK_DOWN, event, 0);  return i;
    case 'C': term_key(d, TK_RIGHT, event, 0); return i;
    case 'D': term_key(d, TK_LEFT, event, 0);  return i;
    case '~': {
        static const struct { int num, tk; } fn[] = {
            {11,TK_F1},{12,TK_F2},{13,TK_F3},{14,TK_F4},
            {15,TK_F5},{17,TK_F6},{18,TK_F7},{19,TK_F8},
            {20,TK_F9},{21,TK_F10},{23,TK_F11},{24,TK_F12},
            {3,TK_DELETE},
        };
        for (size_t k = 0; k < sizeof(fn)/sizeof(fn[0]); k++)
            if (fn[k].num == p[0][0]) { term_key(d, fn[k].tk, event, 0); break; }
        return i;
    }
    default:
        return i;      /* unknown CSI — swallow */
    }
}

/* Decode one input token from buf; returns bytes consumed, 0 = need more. */
static int term_decode(GemuDisplay *d, const unsigned char *buf, int n) {
    if (buf[0] != 0x1B) {
        unsigned char c = buf[0];
        if (c == 3) { d->quit = true; return 1; }             /* Ctrl-C */
        if (c == '\r' || c == '\n') term_token(d, TK_RETURN, '\r');
        else if (c == '\t')  term_token(d, TK_TAB, '\t');
        else if (c == 0x7F || c == '\b') term_token(d, TK_BACKSPACE, '\b');
        else if (c >= 0x20 && c <= 0x7E)
            term_token(d, (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c, c);
        return 1;
    }
    if (n < 2) return 0;                                       /* lone ESC? */
    if (buf[1] == 'O' && n >= 3) {                             /* SS3: F1-F4 */
        if (buf[2] >= 'P' && buf[2] <= 'S') term_token(d, TK_F1 + buf[2] - 'P', 0);
        return 3;
    }
    if (buf[1] == '[')
        return term_csi(d, buf, n);
    return 2;                                 /* ESC + other — swallow */
}

static uint32_t term_do_poll(GemuDisplay *d) {
    TermBackend *b = d->backend;

    unsigned char buf[128];
    for (;;) {
        int space = (int)sizeof(buf) - b->n_pend;
        memcpy(buf, b->pend, (size_t)b->n_pend);
        ssize_t r = read(STDIN_FILENO, buf + b->n_pend, (size_t)space);
        int n = b->n_pend + (r > 0 ? (int)r : 0);
        b->n_pend = 0;
        if (n == 0) break;

        int off = 0;
        while (off < n) {
            int used = term_decode(d, buf + off, n - off);
            if (used == 0) break;             /* partial escape at end */
            off += used;
        }
        if (off < n) {
            /* Partial sequence: stash and retry next poll.  A lone ESC with
             * nothing following is the Escape key → quit. */
            if (n - off == 1 && buf[off] == 0x1B && r <= 0) {
                d->quit = true;
            } else {
                int keep = n - off;
                if (keep > (int)sizeof(b->pend)) keep = (int)sizeof(b->pend);
                memcpy(b->pend, buf + off, (size_t)keep);
                b->n_pend = keep;
            }
        }
        if (r <= 0) break;
    }

    uint64_t now = now_ms();
    uint32_t mask = 0;
    for (int i = 0; i < b->n_bindings; i++) {
        if (b->kitty_active ? b->bindings[i].held_now
                            : b->bindings[i].held_until > now)
            mask |= b->bindings[i].bit;
    }
    return mask;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

static void term_do_set_title(GemuDisplay *d, const char *title) {
    (void)d;
    /* OSC 2 — window title; harmless on terminals that ignore it */
    fprintf(stdout, "\033]2;%s\033\\", title ? title : "GEMU");
    fflush(stdout);
}

static void term_do_destroy(GemuDisplay *d) {
    TermBackend *b = d->backend;
    if (!b) return;
    if (b->screen_active) {
        const char *bye = b->kitty_active
            ? "\033[<u\033[0m\033[?25h\033[?1049l"
            : "\033[0m\033[?25h\033[?1049l";
        ssize_t rc = write(STDOUT_FILENO, bye, strlen(bye));
        (void)rc;
    }
    if (b->termios_active)
        tcsetattr(STDIN_FILENO, TCSANOW, &b->old_termios);
    if (b->old_fl >= 0)
        fcntl(STDIN_FILENO, F_SETFL, b->old_fl);
    free(b->shown);
    free(b->out);
    free(b);
    d->backend = NULL;
}

static bool term_detect_truecolor(void) {
    const char *force = getenv("GEMU_TERM_COLOR");
    if (force) {
        if (!strcmp(force, "truecolor") || !strcmp(force, "24bit")) return true;
        if (!strcmp(force, "256")) return false;
    }
    const char *ct = getenv("COLORTERM");
    return ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"));
}

GemuDisplay *gemu_display_term_create(const GemuDisplayConfig *cfg) {
    TermBackend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->fb_w = cfg->fb_width;
    b->fb_h = cfg->fb_height;
    b->old_fl = -1;
    b->truecolor = term_detect_truecolor();

    const char *hold = getenv("GEMU_TERM_KEY_MS");
    b->hold_ms = hold ? (unsigned)atoi(hold) : 250u;
    if (b->hold_ms < 50u) b->hold_ms = 50u;

    struct termios tio;
    if (tcgetattr(STDIN_FILENO, &b->old_termios) == 0) {
        tio = b->old_termios;
        tio.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
        tio.c_iflag &= (tcflag_t)~(IXON);
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) == 0)
            b->termios_active = true;
    }
    b->old_fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (b->old_fl >= 0)
        fcntl(STDIN_FILENO, F_SETFL, b->old_fl | O_NONBLOCK);

    /* Alternate screen, hide cursor, no autowrap, clear; then probe for the
     * kitty keyboard protocol (CSI ? u).  A supporting terminal replies
     * CSI ? <flags> u, which the input parser turns into an enable. */
    const char *hello = "\033[?1049h\033[?25l\033[?7l\033[2J\033[?u";
    ssize_t rc = write(STDOUT_FILENO, hello, strlen(hello));
    (void)rc;
    b->screen_active = true;

    b->n_bindings = 0;
    for (int i = 0; i < cfg->n_actions && b->n_bindings < TERM_MAX_BINDINGS; i++) {
        const GemuActionDef *def = &cfg->actions[i];
        char val[64] = "";
        bool bound = false;
        if (cfg->ini_section)
            bound = gemu_ini_read(cfg->ini_section, def->name, val, sizeof(val));
        int tk = name_to_token(bound ? val : def->default_key);
        if (!tk) continue;
        b->bindings[b->n_bindings].key = tk;
        b->bindings[b->n_bindings].bit = def->bit;
        b->bindings[b->n_bindings].held_until = 0;
        b->n_bindings++;
    }

    GemuDisplay *d = calloc(1, sizeof(*d));
    if (!d) { term_do_destroy(&(GemuDisplay){ .backend = b }); return NULL; }
    d->backend      = b;
    d->do_render    = term_do_render;
    d->do_poll      = term_do_poll;
    d->do_set_title = term_do_set_title;
    d->do_destroy   = term_do_destroy;
    snprintf(d->title, sizeof(d->title), "%s", cfg->title ? cfg->title : "GEMU");
    d->pointer.x = d->pointer.y = -1;
    term_do_set_title(d, d->title);
    return d;
}

#endif /* !_WIN32 */
