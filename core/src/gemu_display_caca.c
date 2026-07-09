#ifdef HAVE_CACA
#include "gemu_display_priv.h"
#include <caca.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define CACA_MAX_BINDINGS GEMU_DISPLAY_MAX_ACTIONS

/* Map an SDL key name string to a caca key code.
 * For single printable ASCII characters the caca key is the character itself.
 * Special keys are mapped to CACA_KEY_* constants. */
static int sdl_name_to_caca(const char *name) {
    /* Single printable character */
    if (name[0] && !name[1]) {
        unsigned char c = (unsigned char)name[0];
        if (c >= 0x20 && c <= 0x7e) return c;
    }
    /* Named specials (subset of what SDL uses) */
    if (strcmp(name, "Return")    == 0) return CACA_KEY_RETURN;
    if (strcmp(name, "Escape")    == 0) return CACA_KEY_ESCAPE;
    if (strcmp(name, "Backspace") == 0) return CACA_KEY_BACKSPACE;
    if (strcmp(name, "Tab")       == 0) return '\t';
    if (strcmp(name, "Delete")    == 0) return CACA_KEY_DELETE;
    if (strcmp(name, "Up")        == 0) return CACA_KEY_UP;
    if (strcmp(name, "Down")      == 0) return CACA_KEY_DOWN;
    if (strcmp(name, "Left")      == 0) return CACA_KEY_LEFT;
    if (strcmp(name, "Right")     == 0) return CACA_KEY_RIGHT;
    if (strcmp(name, "F1")        == 0) return CACA_KEY_F1;
    if (strcmp(name, "F2")        == 0) return CACA_KEY_F2;
    if (strcmp(name, "F3")        == 0) return CACA_KEY_F3;
    if (strcmp(name, "F4")        == 0) return CACA_KEY_F4;
    if (strcmp(name, "F5")        == 0) return CACA_KEY_F5;
    if (strcmp(name, "F6")        == 0) return CACA_KEY_F6;
    if (strcmp(name, "F7")        == 0) return CACA_KEY_F7;
    if (strcmp(name, "F8")        == 0) return CACA_KEY_F8;
    if (strcmp(name, "F9")        == 0) return CACA_KEY_F9;
    if (strcmp(name, "F10")       == 0) return CACA_KEY_F10;
    if (strcmp(name, "F11")       == 0) return CACA_KEY_F11;
    if (strcmp(name, "F12")       == 0) return CACA_KEY_F12;
    if (strcmp(name, "Space")     == 0) return ' ';
    /* Modifier keys and others are not representable in caca terminal events */
    return 0;
}

/* NES pixel format for caca dithering: ARGB8888 */
#define GEMU_CACA_RMASK 0x00ff0000u
#define GEMU_CACA_GMASK 0x0000ff00u
#define GEMU_CACA_BMASK 0x000000ffu
#define GEMU_CACA_AMASK 0xff000000u

/* Half-block rendering for monochrome/small displays */
static const uint32_t half_blk[4] = { ' ', 0x2584, 0x2580, 0x2588 };

typedef struct {
    caca_canvas_t  *cv;
    caca_display_t *dp;
    caca_dither_t  *dither;   /* for ARGB32 framebuffers; NULL for half-block */

    int  fb_w, fb_h;
    bool use_halfblock;       /* true for mono/small framebuffers */
    bool plain_text;          /* direct ANSI terminal, no alternate screen */
    bool termios_active;
    bool plain_started;
    int  old_fl;
    struct termios old_termios;
    char *last_text;
    int  last_text_rows, last_text_cols;

    struct { int key; uint32_t bit; } bindings[CACA_MAX_BINDINGS];
    int n_bindings;
    bool held_bits_arr[CACA_MAX_BINDINGS]; /* per-binding hold state */
} CacaBackend;

static void build_bindings(CacaBackend *b, const GemuDisplayConfig *cfg) {
    b->n_bindings = 0;
    for (int i = 0; i < cfg->n_actions && b->n_bindings < CACA_MAX_BINDINGS; i++) {
        const GemuActionDef *def = &cfg->actions[i];
        char val[64] = "";
        bool has_binding = false;
        if (cfg->ini_section)
            has_binding = gemu_ini_read(cfg->ini_section, def->name, val, sizeof(val));
        const char *key_name = has_binding ? val : def->default_key;
        int ck = sdl_name_to_caca(key_name);
        if (!ck) continue;
        b->bindings[b->n_bindings].key = ck;
        b->bindings[b->n_bindings].bit = def->bit;
        b->n_bindings++;
    }
}

/* ── Backend callbacks ───────────────────────────────────────────────────── */

static void caca_do_render(GemuDisplay *d, const uint32_t *argb, int w, int h) {
    CacaBackend *b = d->backend;
    if (b->plain_text) return;
    int cw = caca_get_canvas_width(b->cv);
    int ch = caca_get_canvas_height(b->cv);

    if (b->use_halfblock) {
        /* Half-block rendering: two FB rows → one terminal row.
         * Treats any non-zero pixel as "on" (luminance check). */
        for (int y = 0; y < h && y/2 < ch; y += 2) {
            for (int x = 0; x < w && x < cw; x++) {
                uint32_t pt = argb[y * w + x];
                uint32_t pb = (y+1 < h) ? argb[(y+1)*w + x] : 0u;
                int top = (pt & 0x00ffffffu) ? 1 : 0;
                int bot = (pb & 0x00ffffffu) ? 1 : 0;
                if (top || bot)
                    caca_set_color_ansi(b->cv, CACA_WHITE, CACA_BLACK);
                else
                    caca_set_color_ansi(b->cv, CACA_BLACK, CACA_BLACK);
                caca_put_char(b->cv, x, y/2, half_blk[(top<<1)|bot]);
            }
        }
    } else {
        caca_dither_bitmap(b->cv, 0, 0, cw, ch, b->dither, (const void *)argb);
    }
    (void)w; (void)h;
    caca_refresh_display(b->dp);
}

static void caca_do_render_text(GemuDisplay *d, const char *text, int rows, int cols) {
    CacaBackend *b = d->backend;
    if (rows <= 0 || cols <= 0) return;
    if (b->plain_text) {
        size_t len = (size_t)rows * (size_t)cols;
        if (b->last_text && b->last_text_rows == rows && b->last_text_cols == cols &&
            memcmp(b->last_text, text, len) == 0)
            return;
        if (!b->last_text || b->last_text_rows != rows || b->last_text_cols != cols) {
            char *next = realloc(b->last_text, len);
            if (!next) return;
            b->last_text = next;
            b->last_text_rows = rows;
            b->last_text_cols = cols;
        }
        memcpy(b->last_text, text, len);
        if (!b->plain_started) {
            fputs("\033[?25l\033[2J", stdout);
            b->plain_started = true;
        }
        fputs("\033[H", stdout);
        for (int y = 0; y < rows; y++) {
            fwrite(text + y * cols, 1, (size_t)cols, stdout);
            fputc('\n', stdout);
        }
        fflush(stdout);
        return;
    }
    caca_set_canvas_size(b->cv, cols, rows);
    caca_set_color_ansi(b->cv, CACA_LIGHTBLUE, CACA_BLACK);
    caca_clear_canvas(b->cv);
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            unsigned char ch = (unsigned char)text[y * cols + x];
            caca_put_char(b->cv, x, y, ch ? ch : ' ');
        }
    }
    caca_refresh_display(b->dp);
}

static uint32_t caca_do_poll(GemuDisplay *d) {
    CacaBackend *b = d->backend;
    if (b->plain_text) {
        unsigned char buf[64];
        for (;;) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n <= 0) break;
            for (ssize_t i = 0; i < n; i++) {
                unsigned char ch = buf[i];
                if (ch == 3) { d->quit = true; continue; }
                if (ch == 0x1b) continue;
                if (ch == '\n') ch = '\r';
                if (ch >= 0x20 || ch == '\r' || ch == '\b' || ch == '\t' || ch == 0x7f)
                    gemu_display_push_raw(d, ch);
            }
        }
        return 0;
    }
    caca_event_t ev;

    /* caca doesn't give us key-up events, so we treat each frame's key set
     * as fresh: clear hold state, then re-assert from this frame's events. */
    memset(b->held_bits_arr, 0, sizeof(bool) * (size_t)b->n_bindings);

    while (caca_get_event(b->dp, CACA_EVENT_KEY_PRESS, &ev, 0) != 0) {
        int key = caca_get_event_key_ch(&ev);

        if (key == CACA_KEY_ESCAPE || key == 3 /* Ctrl-C */) {
            d->quit = true;
            continue;
        }

        /* Raw key queue */
        uint32_t ucp = caca_get_event_key_utf32(&ev);
        if (ucp >= 0x20 && ucp != 0x7f)
            gemu_display_push_raw(d, ucp);
        else if (key == CACA_KEY_RETURN)
            gemu_display_push_raw(d, '\r');
        else if (key == CACA_KEY_BACKSPACE)
            gemu_display_push_raw(d, '\b');
        else if (key == '\t')
            gemu_display_push_raw(d, '\t');
        else if (key == CACA_KEY_DELETE)
            gemu_display_push_raw(d, 0x7f);

        for (int i = 0; i < b->n_bindings; i++) {
            if (key == b->bindings[i].key)
                b->held_bits_arr[i] = true;
        }
    }

    uint32_t mask = 0;
    for (int i = 0; i < b->n_bindings; i++)
        if (b->held_bits_arr[i]) mask |= b->bindings[i].bit;
    return mask;
}

static void caca_do_destroy(GemuDisplay *d) {
    CacaBackend *b = d->backend;
    if (!b) return;
    if (b->dither) caca_free_dither(b->dither);
    if (b->dp)     caca_free_display(b->dp);
    if (b->cv)     caca_free_canvas(b->cv);
    if (b->plain_text) {
        if (b->plain_started)
            fputs("\033[?25h\n", stdout);
        if (b->termios_active)
            tcsetattr(STDIN_FILENO, TCSANOW, &b->old_termios);
        if (b->old_fl >= 0)
            fcntl(STDIN_FILENO, F_SETFL, b->old_fl);
        free(b->last_text);
    }
    free(b);
    d->backend = NULL;
}

static void caca_do_set_title(GemuDisplay *d, const char *title) {
    CacaBackend *b = d->backend;
    if (b && b->dp)
        caca_set_display_title(b->dp, title ? title : "GEMU");
}

/* ── Create ──────────────────────────────────────────────────────────────── */

GemuDisplay *gemu_display_caca_create(const GemuDisplayConfig *cfg) {
    CacaBackend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->fb_w = cfg->fb_width;
    b->fb_h = cfg->fb_height;
    b->old_fl = -1;
    b->plain_text = cfg->terminal_text;

    /* Use half-block rendering for small/mono displays (≤128 wide). */
    b->use_halfblock = (cfg->fb_width <= 128);

    if (b->plain_text) {
        struct termios tio;
        if (tcgetattr(STDIN_FILENO, &b->old_termios) == 0) {
            tio = b->old_termios;
            tio.c_lflag &= (tcflag_t)~(ICANON | ECHO);
            tio.c_cc[VMIN] = 0;
            tio.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) == 0)
                b->termios_active = true;
        }
        b->old_fl = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (b->old_fl >= 0)
            fcntl(STDIN_FILENO, F_SETFL, b->old_fl | O_NONBLOCK);
        build_bindings(b, cfg);
        GemuDisplay *d = calloc(1, sizeof(*d));
        if (!d) { caca_do_destroy(&(GemuDisplay){.backend=b}); return NULL; }
        d->backend = b;
        d->do_render = caca_do_render;
        d->do_render_text = caca_do_render_text;
        d->do_poll = caca_do_poll;
        d->do_destroy = caca_do_destroy;
        snprintf(d->title, sizeof(d->title), "%s", cfg->title ? cfg->title : "GEMU");
        d->pointer.x = d->pointer.y = -1;
        return d;
    }

    b->cv = caca_create_canvas(0, 0);
    const char *driver = getenv("CACA_DRIVER");
    if (driver && driver[0]) {
        b->dp = caca_create_display_with_driver(b->cv, driver);
    } else {
        static const char *terminal_drivers[] = { "ncurses", "slang", "ansi" };
        for (size_t i = 0; i < sizeof(terminal_drivers) / sizeof(terminal_drivers[0]); i++) {
            b->dp = caca_create_display_with_driver(b->cv, terminal_drivers[i]);
            if (b->dp) break;
        }
    }
    if (!b->dp) { caca_free_canvas(b->cv); free(b); return NULL; }
    caca_set_display_title(b->dp, cfg->title ? cfg->title : "GEMU");

    if (!b->use_halfblock) {
        b->dither = caca_create_dither(32,
                                       cfg->fb_width, cfg->fb_height,
                                       cfg->fb_width * 4,
                                       GEMU_CACA_RMASK, GEMU_CACA_GMASK,
                                       GEMU_CACA_BMASK, GEMU_CACA_AMASK);
        if (!b->dither) { caca_do_destroy(&(GemuDisplay){.backend=b}); return NULL; }
    }

    build_bindings(b, cfg);

    GemuDisplay *d = calloc(1, sizeof(*d));
    if (!d) { caca_do_destroy(&(GemuDisplay){.backend=b}); return NULL; }
    d->backend   = b;
    d->do_render = caca_do_render;
    d->do_render_text = caca_do_render_text;
    d->do_poll   = caca_do_poll;
    d->do_set_title = caca_do_set_title;
    d->do_destroy = caca_do_destroy;
    snprintf(d->title, sizeof(d->title), "%s", cfg->title ? cfg->title : "GEMU");
    d->pointer.x = d->pointer.y = -1;
    return d;
}

#endif /* HAVE_CACA */
