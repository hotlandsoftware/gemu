#define _XOPEN_SOURCE_EXTENDED 1
#include "vga/rca_display.h"
#include "vip.h"
#include "studio2.h"
#include "devices/vip_devices.h"
#include <ncurses.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Same physical layout as SDL/VNC VIP keypad input. */
static const int vip_key_map[16] = {
    'x', '1', '2', '3', 'q', 'w', 'e', 'a',
    's', 'd', 'z', 'c', '4', 'r', 'f', 'v',
};

typedef struct {
    bool initialized;
    int last_w;
    int last_h;
} RcaCursesCtx;

static volatile sig_atomic_t curses_interrupted;

static void curses_sigint(int sig) {
    (void)sig;
    curses_interrupted = 1;
}

static void curses_init_once(RcaCursesCtx *c) {
    if (c->initialized)
        return;

    setlocale(LC_ALL, "");
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    signal(SIGINT, curses_sigint);
    c->initialized = true;
}

static void curses_render(void *ctx, const uint8_t *vram, int w, int h) {
    RcaCursesCtx *c = ctx;
    curses_init_once(c);

    if (c->last_w != w || c->last_h != h) {
        erase();
        c->last_w = w;
        c->last_h = h;
    }

    static const wchar_t blk[4] = { L' ', L'▄', L'▀', L'█' };
    for (int y = 0; y < h; y += 2) {
        move(y / 2, 0);
        for (int x = 0; x < w; x++) {
            int top = vram[y * w + x] ? 1 : 0;
            int bot = (y + 1 < h && vram[(y + 1) * w + x]) ? 1 : 0;
            wchar_t ws[2] = { blk[(top << 1) | bot], L'\0' };
            cchar_t cc;
            setcchar(&cc, ws, A_NORMAL, 0, NULL);
            add_wch(&cc);
        }
    }
    refresh();
}

static void curses_destroy(void *ctx) {
    RcaCursesCtx *c = ctx;
    if (c && c->initialized)
        endwin();
    signal(SIGINT, SIG_DFL);
    free(c);
}

RcaDisplay *rca_display_curses_create(void) {
    RcaDisplay *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->render = curses_render;
    d->destroy = curses_destroy;
    d->run = NULL;
    d->ctx = calloc(1, sizeof(RcaCursesCtx));
    if (!d->ctx) {
        free(d);
        return NULL;
    }
    return d;
}

void rca_display_curses_poll_vip(RcaDisplay *d, RcaVipState *s, bool *quit) {
    if (!d)
        return;
    RcaCursesCtx *c = d->ctx;
    curses_init_once(c);

    if (curses_interrupted) {
        *quit = true;
        return;
    }

    memset(s->keys, 0, sizeof(s->keys));
    s->key_down = -1;
    s->ef2_down = false;

    int ch;
    while ((ch = getch()) != ERR) {
        if (ch == 3 || ch == 27) {
            *quit = true;
            continue;
        }
        if (ch == '\t')
            s->ef2_down = true;

        if (rca_vip_has_vp601(s->cfg)) {
            int ascii = ch;
            if (ascii >= 'a' && ascii <= 'z')
                ascii -= 32;
            if ((ascii >= 0x20 && ascii <= 0x7e) ||
                ascii == '\r' || ascii == '\n' ||
                ascii == KEY_BACKSPACE || ascii == 0x7f) {
                if (ascii == '\n')
                    ascii = '\r';
                if (ascii == KEY_BACKSPACE || ascii == 0x7f)
                    ascii = '\b';
                s->ascii_key = ascii;
            }
        }

        if (rca_vip_has_keypad(s->cfg)) {
            for (int k = 0; k < 16; k++) {
                if (ch == vip_key_map[k]) {
                    s->keys[k] = 1;
                    s->key_down = k;
                    break;
                }
            }
        }
    }
}

/* Player A: space=0, q=1, w=2, e=3, a=4, s=5, d=6, z=7, x=8, c=9 */
static const int studio2_keys_a[10] = {
    ' ', 'q', 'w', 'e', 'a', 's', 'd', 'z', 'x', 'c',
};
/* Player B: number row mapped to Studio II numpad layout (0,7,8,9,4,5,6,1,2,3) */
static const int studio2_keys_b[10] = {
    '0', '7', '8', '9', '4', '5', '6', '1', '2', '3',
};

void rca_display_curses_poll_studio2(RcaDisplay *d, RcaStudio2State *s,
                                     bool *quit, bool *reset) {
    if (!d) return;
    RcaCursesCtx *c = d->ctx;
    curses_init_once(c);

    if (curses_interrupted) { *quit = true; return; }

    memset(s->keys_a, 0, sizeof(s->keys_a));
    memset(s->keys_b, 0, sizeof(s->keys_b));

    int ch;
    while ((ch = getch()) != ERR) {
        if (ch == 3 || ch == 27) { *quit = true; continue; }
        if (ch == KEY_F(3) || ch == 'r') { *reset = true; continue; }
        for (int i = 0; i < 10; i++) {
            if (ch == studio2_keys_a[i]) { s->keys_a[i] = true; break; }
            if (ch == studio2_keys_b[i]) { s->keys_b[i] = true; break; }
        }
    }
}
