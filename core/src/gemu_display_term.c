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
 * Input: raw non-canonical stdin.  Terminals only report key *presses*
 * (plus autorepeat), never releases, so a bound action stays "held" for
 * GEMU_TERM_KEY_MS milliseconds (default 250) after its last repeat.
 * Expect a brief dropout on long holds if your key-repeat delay is longer
 * than that — a fundamental terminal limitation, not a bug.
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
    TK_RETURN, TK_TAB, TK_BACKSPACE, TK_DELETE,
    TK_F1, TK_F2, TK_F3, TK_F4, TK_F5, TK_F6,
    TK_F7, TK_F8, TK_F9, TK_F10, TK_F11, TK_F12,
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

    /* Input */
    struct { int key; uint32_t bit; uint64_t held_until; } bindings[TERM_MAX_BINDINGS];
    int      n_bindings;
    unsigned hold_ms;
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
            emit_colors(b, top, bot, &cur_top, &cur_bot);
            outs(b, "\xE2\x96\x80");          /* U+2580 UPPER HALF BLOCK */
            pen_x++;
        }
    }
    if (b->out_len) {
        outs(b, "\033[0m\033[H");
        term_flush(b);
    }
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
    if (name[0] == 'F' && name[1]) {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12) return TK_F1 + n - 1;
    }
    return 0;   /* modifiers etc. — not representable in a terminal */
}

static void term_token(GemuDisplay *d, int token, uint32_t raw_cp) {
    TermBackend *b = d->backend;
    if (raw_cp) gemu_display_push_raw(d, raw_cp);
    uint64_t until = now_ms() + b->hold_ms;
    for (int i = 0; i < b->n_bindings; i++)
        if (b->bindings[i].key == token)
            b->bindings[i].held_until = until;
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
    if (buf[1] == '[') {
        int i = 2;
        int num = 0;
        bool has_num = false;
        while (i < n && buf[i] >= '0' && buf[i] <= '9') {
            num = num * 10 + (buf[i] - '0');
            has_num = true;
            i++;
        }
        if (i >= n) return 0;
        switch (buf[i]) {
        case 'A': term_token(d, TK_UP, 0);    return i + 1;
        case 'B': term_token(d, TK_DOWN, 0);  return i + 1;
        case 'C': term_token(d, TK_RIGHT, 0); return i + 1;
        case 'D': term_token(d, TK_LEFT, 0);  return i + 1;
        case '~':
            if (has_num) {
                static const struct { int num, tk; } fn[] = {
                    {11,TK_F1},{12,TK_F2},{13,TK_F3},{14,TK_F4},
                    {15,TK_F5},{17,TK_F6},{18,TK_F7},{19,TK_F8},
                    {20,TK_F9},{21,TK_F10},{23,TK_F11},{24,TK_F12},
                    {3,TK_DELETE},
                };
                for (size_t k = 0; k < sizeof(fn)/sizeof(fn[0]); k++)
                    if (fn[k].num == num) { term_token(d, fn[k].tk, 0); break; }
            }
            return i + 1;
        default:
            return i + 1;                     /* unknown CSI — swallow */
        }
    }
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
    for (int i = 0; i < b->n_bindings; i++)
        if (b->bindings[i].held_until > now) mask |= b->bindings[i].bit;
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
        const char *bye = "\033[0m\033[?25h\033[?1049l";
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

    /* Alternate screen, hide cursor, no autowrap, clear */
    const char *hello = "\033[?1049h\033[?25l\033[?7l\033[2J";
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
