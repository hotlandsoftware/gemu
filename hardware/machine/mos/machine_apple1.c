#include "machine_apple1.h"
#include "gemu/args.h"
#include "gemu/gemu_display.h"
#include "gemu/memory.h"
#include "gemu/screendump.h"
#include "gemu/sha256.h"
#include "gemu/vnc.h"
#include "romdb.h"
#include "gemu/dev/space_cache.h"
#include "mos6502_dev.h"
#include "controller/pia6821_dev.h"
#include "gemu/dev/device.h"
#include "gemu/dev/option.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/screen.h"
#include "gemu/dev/space.h"
#include "gemu/dev/state.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef _WIN32
#include <sys/select.h>
#include <unistd.h>
#endif

#define APPLE1_KBD     0xD010u
#define APPLE1_KBDCR   0xD011u
#define APPLE1_DSP     0xD012u
#define APPLE1_DSPCR   0xD013u
#define APPLE1_ROM     0xFF00u
#define APPLE1_WOZ_ECHO 0xFFEFu
#define APPLE1_MAX_RAM 0xD000u
#define APPLE1_HZ      1000000u
#define APPLE1_FPS     60u
#define APPLE1_CPF     (APPLE1_HZ / APPLE1_FPS)
#define APPLE1_ECHO_CYCLES (APPLE1_HZ / 30u) /* 300 baud, 10 bits/char */

#define A1_COLS        40
#define A1_ROWS        24
#define A1_CELL_W      7
#define A1_CELL_H      8
#define A1_SCALE       3
#define A1_BORDER      14
#define A1_WIN_W       (A1_COLS * A1_CELL_W * A1_SCALE + A1_BORDER * 2)
#define A1_WIN_H       (A1_ROWS * A1_CELL_H * A1_SCALE + A1_BORDER * 2)
#define A1_FB_W        (A1_COLS * A1_CELL_W)
#define A1_FB_H        (A1_ROWS * A1_CELL_H)
#define A1_CHARGEN_SIZE 512u

struct Apple1Display {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    uint32_t      window_id;
    uint8_t       screen[A1_ROWS][A1_COLS];
    int           row;
    int           col;
    bool          dirty;
    bool          cursor_phase;
    bool          title_paused;
    bool          quit;
    uint8_t       kbuf[64];
    int           khead;
    int           ktail;
    uint8_t       chargen[A1_CHARGEN_SIZE];
    bool          have_chargen;
    MosCharGenType cg;
};

static void apple1_monitor_prompt(Apple1State *s);
static uint8_t apple1_read(uint16_t addr, void *ud);

static void apple1_display_kpush(Apple1Display *d, uint8_t ch) {
    if (!d) return;
    int n = (d->khead + 1) & 63;
    if (n != d->ktail) {
        d->kbuf[d->khead] = ch;
        d->khead = n;
    }
}

static bool apple1_display_key_available(Apple1Display *d) {
    return d && d->khead != d->ktail;
}

static uint8_t apple1_display_read_key(Apple1Display *d) {
    if (!apple1_display_key_available(d)) return 0;
    uint8_t ch = d->kbuf[d->ktail];
    d->ktail = (d->ktail + 1) & 63;
    return ch;
}

static const uint8_t cm2140_fallback[A1_CHARGEN_SIZE] = {
    /* @ */
    0x00,0x0E,0x11,0x15,0x17,0x16,0x10,0x0F,
    /* A-Z */
    0x00,0x04,0x0A,0x11,0x11,0x1F,0x11,0x11, 0x00,0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E,
    0x00,0x0E,0x11,0x10,0x10,0x10,0x11,0x0E, 0x00,0x1E,0x11,0x11,0x11,0x11,0x11,0x1E,
    0x00,0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F, 0x00,0x1F,0x10,0x10,0x1E,0x10,0x10,0x10,
    0x00,0x0F,0x10,0x10,0x10,0x13,0x11,0x0F, 0x00,0x11,0x11,0x11,0x1F,0x11,0x11,0x11,
    0x00,0x0E,0x04,0x04,0x04,0x04,0x04,0x0E, 0x00,0x01,0x01,0x01,0x01,0x01,0x11,0x0E,
    0x00,0x11,0x12,0x14,0x18,0x14,0x12,0x11, 0x00,0x10,0x10,0x10,0x10,0x10,0x10,0x1F,
    0x00,0x11,0x1B,0x15,0x15,0x11,0x11,0x11, 0x00,0x11,0x11,0x19,0x15,0x13,0x11,0x11,
    0x00,0x0E,0x11,0x11,0x11,0x11,0x11,0x0E, 0x00,0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,
    0x00,0x0E,0x11,0x11,0x11,0x15,0x12,0x0D, 0x00,0x1E,0x11,0x11,0x1E,0x14,0x12,0x11,
    0x00,0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E, 0x00,0x1F,0x04,0x04,0x04,0x04,0x04,0x04,
    0x00,0x11,0x11,0x11,0x11,0x11,0x11,0x0E, 0x00,0x11,0x11,0x11,0x11,0x0A,0x0A,0x04,
    0x00,0x11,0x11,0x11,0x15,0x15,0x1B,0x11, 0x00,0x11,0x11,0x0A,0x04,0x0A,0x11,0x11,
    0x00,0x11,0x11,0x0A,0x04,0x04,0x04,0x04, 0x00,0x1F,0x01,0x02,0x04,0x08,0x10,0x1F,
    /* [ \ ] ^ _ */
    0x00,0x0E,0x08,0x08,0x08,0x08,0x08,0x0E, 0x00,0x10,0x08,0x04,0x02,0x01,0x00,0x00,
    0x00,0x0E,0x02,0x02,0x02,0x02,0x02,0x0E, 0x00,0x04,0x0A,0x11,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,
    /* space ! " # $ % & ' */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x04,0x04,0x04,0x04,0x00,0x04,0x00,
    0x00,0x0A,0x0A,0x00,0x00,0x00,0x00,0x00, 0x00,0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A,
    0x00,0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04, 0x00,0x18,0x19,0x02,0x04,0x08,0x13,0x03,
    0x00,0x0C,0x12,0x14,0x08,0x15,0x12,0x0D, 0x00,0x04,0x04,0x00,0x00,0x00,0x00,0x00,
    /* ( ) * + , - . / */
    0x00,0x02,0x04,0x08,0x08,0x08,0x04,0x02, 0x00,0x08,0x04,0x02,0x02,0x02,0x04,0x08,
    0x00,0x00,0x04,0x15,0x0E,0x15,0x04,0x00, 0x00,0x00,0x04,0x04,0x1F,0x04,0x04,0x00,
    0x00,0x00,0x00,0x00,0x00,0x04,0x04,0x08, 0x00,0x00,0x00,0x00,0x1F,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00, 0x00,0x01,0x02,0x04,0x08,0x10,0x00,0x00,
    /* 0-9 : ; < = > ? */
    0x00,0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, 0x00,0x04,0x0C,0x04,0x04,0x04,0x04,0x0E,
    0x00,0x0E,0x11,0x01,0x02,0x04,0x08,0x1F, 0x00,0x1F,0x02,0x04,0x02,0x01,0x11,0x0E,
    0x00,0x02,0x06,0x0A,0x12,0x1F,0x02,0x02, 0x00,0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,
    0x00,0x06,0x08,0x10,0x1E,0x11,0x11,0x0E, 0x00,0x1F,0x01,0x02,0x04,0x08,0x08,0x08,
    0x00,0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E, 0x00,0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C,
    0x00,0x00,0x04,0x00,0x00,0x04,0x00,0x00, 0x00,0x00,0x04,0x00,0x00,0x04,0x04,0x08,
    0x00,0x02,0x04,0x08,0x10,0x08,0x04,0x02, 0x00,0x00,0x00,0x1F,0x00,0x1F,0x00,0x00,
    0x00,0x08,0x04,0x02,0x01,0x02,0x04,0x08, 0x00,0x0E,0x11,0x01,0x02,0x04,0x00,0x04,
};

static uint32_t apple1_renderer_flags(GemuRendererType renderer) {
    switch (renderer) {
    case GEMU_RENDERER_SOFTWARE: return SDL_RENDERER_SOFTWARE;
    case GEMU_RENDERER_ACCELERATED: return SDL_RENDERER_ACCELERATED;
    case GEMU_RENDERER_AUTO:
    default: return SDL_RENDERER_ACCELERATED;
    }
}

static const uint8_t *apple1_display_font(const Apple1Display *d) {
    return (d && d->cg == MOS_CG_CM2140 && d->have_chargen)
         ? d->chargen : cm2140_fallback;
}

static int apple1_glyph_index(uint8_t ch) {
    ch &= 0x7Fu;
    if (ch >= '@' && ch <= '_') return ch - '@';
    if (ch >= ' ' && ch <= '?') return 32 + (ch - ' ');
    return -1;
}

static void apple1_display_set_paused(Apple1Display *d, bool paused) {
    if (!d || !d->window || d->title_paused == paused) return;
    char title[200];
    gemu_window_title(title, sizeof(title), NULL, paused);
    SDL_SetWindowTitle(d->window, title);
    d->title_paused = paused;
}

static void apple1_display_render(Apple1Display *d, bool paused) {
    if (!d || !d->renderer) return;
    bool cursor_phase = paused ? d->cursor_phase
                               : ((SDL_GetTicks() / 500u) & 1u) != 0;
    if (!d->dirty && d->cursor_phase == cursor_phase) return;
    d->cursor_phase = cursor_phase;

    SDL_SetRenderDrawColor(d->renderer, 0, 0, 0, 255);
    SDL_RenderClear(d->renderer);
    SDL_SetRenderDrawColor(d->renderer, 255, 255, 255, 255);

    const uint8_t *font = apple1_display_font(d);
    for (int row = 0; row < A1_ROWS; row++) {
        for (int col = 0; col < A1_COLS; col++) {
            uint8_t ch = d->screen[row][col];
            if (cursor_phase && row == d->row && col == d->col)
                ch = '@';
            int idx = apple1_glyph_index(ch);
            if (idx < 0) continue;
            const uint8_t *glyph = &font[idx * 8];
            int x0 = A1_BORDER + col * A1_CELL_W * A1_SCALE;
            int y0 = A1_BORDER + row * A1_CELL_H * A1_SCALE;
            for (int gy = 0; gy < 8; gy++) {
                uint8_t bits = glyph[gy];
                for (int gx = 0; gx < 5; gx++) {
                    if (!(bits & (uint8_t)(0x10u >> gx))) continue;
                    SDL_Rect r = {
                        x0 + gx * A1_SCALE,
                        y0 + gy * A1_SCALE,
                        A1_SCALE,
                        A1_SCALE
                    };
                    SDL_RenderFillRect(d->renderer, &r);
                }
            }
        }
    }
    SDL_RenderPresent(d->renderer);
    d->dirty = false;
}

static Apple1Display *apple1_display_create(const MosConfig *cfg) {
    Apple1Display *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->cg = cfg->char_gen;
    if (d->cg == MOS_CG_AUTO)
        d->cg = MOS_CG_CM2140_COMPAT;
    memset(d->screen, ' ', sizeof(d->screen));
    d->dirty = true;

    if (cfg->display_type == GEMU_DISPLAY_NONE ||
        cfg->display_type == GEMU_DISPLAY_CURSES)
        return d;

    if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) &&
        SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "apple1: SDL video init failed: %s\n", SDL_GetError());
        return d;
    }

    char title[200];
    gemu_window_title(title, sizeof(title), NULL, false);
    d->window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        A1_WIN_W, A1_WIN_H, SDL_WINDOW_SHOWN);
    if (!d->window) {
        fprintf(stderr, "apple1: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return d;
    }
    d->window_id = SDL_GetWindowID(d->window);
    d->renderer = SDL_CreateRenderer(d->window, -1,
                                     apple1_renderer_flags(cfg->display_renderer));
    if (!d->renderer)
        d->renderer = SDL_CreateRenderer(d->window, -1, SDL_RENDERER_SOFTWARE);
    if (!d->renderer) {
        fprintf(stderr, "apple1: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(d->window);
        d->window = NULL;
        d->window_id = 0;
        return d;
    }
    SDL_StartTextInput();
    apple1_display_render(d, false);
    return d;
}

static void apple1_display_clear(Apple1Display *d) {
    if (!d) return;
    memset(d->screen, ' ', sizeof(d->screen));
    d->row = 0;
    d->col = 0;
    d->khead = d->ktail = 0;
    d->dirty = true;
}

static void apple1_pia_setup(Apple1State *s);

static void apple1_reset_machine(Apple1State *s) {
    mos6502_reset(s->cpu);
    apple1_pia_setup(s);
    s->native_monitor = !s->have_monitor_rom;
    s->quit_requested = false;
    apple1_display_clear(s->display);
    if (s->native_monitor)
        apple1_monitor_prompt(s);
}

static void apple1_display_destroy(Apple1Display *d) {
    if (!d) return;
    if (d->window) SDL_StopTextInput();
    if (d->renderer) SDL_DestroyRenderer(d->renderer);
    if (d->window) SDL_DestroyWindow(d->window);
    free(d);
}

static void apple1_display_scroll(Apple1Display *d) {
    memmove(d->screen[0], d->screen[1], (A1_ROWS - 1) * A1_COLS);
    memset(d->screen[A1_ROWS - 1], ' ', A1_COLS);
    d->row = A1_ROWS - 1;
    d->dirty = true;
}

static void apple1_display_putc(Apple1Display *d, uint8_t ch) {
    if (!d) return;
    ch &= 0x7Fu;
    if (ch == '\n') return;
    if (ch == '\r') {
        d->col = 0;
        d->row++;
        if (d->row >= A1_ROWS) apple1_display_scroll(d);
        d->dirty = true;
        return;
    }
    if (ch >= 'a' && ch <= 'z') ch = (uint8_t)toupper(ch);
    if (ch < ' ' || ch > '_') ch = ' ';
    d->screen[d->row][d->col] = ch;
    d->dirty = true;
    d->col++;
    if (d->col >= A1_COLS) {
        d->col = 0;
        d->row++;
        if (d->row >= A1_ROWS) apple1_display_scroll(d);
    }
}

static void apple1_display_poll(Apple1Display *d) {
    if (!d || !d->renderer) return;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            d->quit = true;
        } else if (ev.type == SDL_WINDOWEVENT &&
                   ev.window.windowID == d->window_id &&
                   ev.window.event == SDL_WINDOWEVENT_CLOSE) {
            d->quit = true;
        } else if (ev.type == SDL_TEXTINPUT) {
            for (int i = 0; ev.text.text[i]; i++)
                apple1_display_kpush(d, (uint8_t)ev.text.text[i]);
        } else if (ev.type == SDL_KEYDOWN) {
            if (ev.key.keysym.sym == SDLK_RETURN)
                apple1_display_kpush(d, '\r');
            else if (ev.key.keysym.sym == SDLK_BACKSPACE ||
                     ev.key.keysym.sym == SDLK_DELETE)
                apple1_display_kpush(d, 0x7F);
        }
    }
    apple1_display_render(d, false);
}

static uint8_t apple1_normalize_key(uint8_t ch) {
    if (ch == '\n') ch = '\r';
    if (ch >= 'a' && ch <= 'z') ch = (uint8_t)toupper(ch);
    return (uint8_t)(ch | 0x80u);
}

static void apple1_poll_curses_keyboard(Apple1State *s) {
#ifndef _WIN32
    if (!s->display || s->cfg->display_type != GEMU_DISPLAY_CURSES)
        return;

    fd_set rfds;
    struct timeval tv = {0, 0};
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) <= 0)
        return;

    unsigned char ch = 0;
    if (read(STDIN_FILENO, &ch, 1) == 1)
        apple1_display_kpush(s->display, ch);
#else
    (void)s;
#endif
}

static void apple1_poll_vnc_keyboard(Apple1State *s) {
    GemuVncServer *vnc = gemu_vnc_get();
    if (!vnc) return;
    GemuVncKeyEvent ev;
    while (gemu_vnc_pop_key_event(vnc, &ev)) {
        if (!ev.down || !s->display)
            continue;
        uint32_t sym = ev.keysym;
        if (sym == '\n' || sym == '\r' || sym == 0xFF0D || sym == 0xFF8D)
            apple1_display_kpush(s->display, '\r');
        else if (sym == 0xFF08 || sym == 0xFFFF)
            apple1_display_kpush(s->display, 0x7F);
        else if (sym >= ' ' && sym <= 0x7F)
            apple1_display_kpush(s->display, (uint8_t)sym);
    }
}

static void apple1_poll_input_devices(Apple1State *s) {
    if (s->display) {
        apple1_display_poll(s->display);
        apple1_poll_curses_keyboard(s);
        apple1_poll_vnc_keyboard(s);
    }

    GemuSerial *ser = s->cfg->serial;
    if (ser) ser->poll(ser->ud);
}

/* Mirror the custom SDL window to VNC (no-op without -vnc). */
/* The terminal as ARGB, 280x192 (40x24 cells of 7x8; the glyph's 5x8 at
 * the cell's left), white on black, the cursor an '@' when cursor_phase. */
static void apple1_render_argb(const Apple1State *s, uint32_t *argb, int stride, bool cursor_phase) {
    for (int y = 0; y < A1_FB_H; y++)
        for (int x = 0; x < A1_FB_W; x++)
            argb[(size_t)y * (size_t)stride + (size_t)x] = 0xFF000000u;
    if (!s->display) return;
    const uint8_t *font = apple1_display_font(s->display);
    for (int row = 0; row < A1_ROWS; row++) {
        for (int col = 0; col < A1_COLS; col++) {
            uint8_t ch = s->display->screen[row][col];
            if (cursor_phase && row == s->display->row && col == s->display->col)
                ch = '@';
            int idx = apple1_glyph_index(ch);
            if (idx < 0) continue;
            const uint8_t *glyph = &font[idx * 8];
            int x0 = col * A1_CELL_W;
            int y0 = row * A1_CELL_H;
            for (int gy = 0; gy < A1_CELL_H; gy++) {
                uint8_t bits = glyph[gy];
                for (int gx = 0; gx < 5; gx++) {
                    if (bits & (uint8_t)(0x10u >> gx))
                        argb[(size_t)(y0 + gy) * (size_t)stride + (size_t)(x0 + gx)] = 0xFFFFFFFFu;
                }
            }
        }
    }
}

static void apple1_update_vnc(Apple1State *s, bool paused) {
    static uint32_t argb[A1_FB_W * A1_FB_H];
    GemuVncServer *vnc = gemu_vnc_get();
    if (!vnc || !s || !s->display)
        return;
    bool cursor_phase = paused ? s->display->cursor_phase
                               : ((SDL_GetTicks() / 500u) & 1u) != 0;
    apple1_render_argb(s, argb, A1_FB_W, cursor_phase);
    gemu_vnc_update_argb(vnc, argb, A1_FB_W, A1_FB_H);
}

/* The core screen's picture (stage M3): the terminal, its cursor as the
 * window last drew it. */
static void apple1_screen_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    (void)clip;
    const Apple1State *s = ud;
    apple1_render_argb(s, bmp->pix, bmp->stride, s->display && s->display->cursor_phase);
}

static struct GemuScreen *apple1_screen(Apple1State *s);
/* What the core screen shows. */
static bool apple1_screendump(void *ud, const char *path) {
    Apple1State *s = ud;
    const GemuBitmap *b = gemu_screen_render(apple1_screen(s));
    uint32_t *argb = malloc((size_t)A1_FB_W * A1_FB_H * sizeof *argb);
    if (!argb) return false;
    for (int y = 0; y < A1_FB_H; y++)
        memcpy(argb + (size_t)y * A1_FB_W, b->pix + (size_t)y * (size_t)b->stride,
               A1_FB_W * sizeof *argb);
    bool ok = gemu_screendump_argb(path, argb, A1_FB_W, A1_FB_H);
    free(argb);
    return ok;
}

/* A key reaches the PIA: its code on port A and a strobe on CA1, which sets
 * CRA bit 7 until the program reads the key.  The next key waits until then. */
static bool apple1_key_waiting(const Apple1State *s) {
    return (s->pia->cra & 0x80u) != 0;
}
static void apple1_key_strobe(Apple1State *s, uint8_t key) {
    s->key_data = key;
    pia6821_set_ca1(s->pia, true);
    pia6821_set_ca1(s->pia, false);
}

static void apple1_poll_keyboard(Apple1State *s) {
    apple1_poll_input_devices(s);
    if (s->display && !apple1_key_waiting(s) && apple1_display_key_available(s->display))
        apple1_key_strobe(s, apple1_normalize_key(apple1_display_read_key(s->display)));

    GemuSerial *ser = s->cfg->serial;
    if (ser && !apple1_key_waiting(s) && ser->key_available(ser->ud))
        apple1_key_strobe(s, apple1_normalize_key(ser->read_byte(ser->ud)));
}

static void apple1_write_display(Apple1State *s, uint8_t val) {
    if (s->display) {
        apple1_display_putc(s->display, val);
        if (s->cfg->display_type != GEMU_DISPLAY_CURSES)
            return;
    }

    GemuSerial *ser = s->cfg->serial;
    val &= 0x7Fu;
    if (val == '\r') {
        if (ser) {
            ser->write_byte(ser->ud, '\r');
            ser->write_byte(ser->ud, '\n');
        } else {
            putchar('\n');
            fflush(stdout);
        }
        return;
    }
    if (ser) ser->write_byte(ser->ud, val);
    else {
        putchar(val);
        fflush(stdout);
    }
}

/* The PIA's wiring: port A reads the keyboard latch; port B's outputs are
 * the display's character; PB7, the display's busy line, reads 0 (the
 * display takes characters as fast as they come). */
static uint8_t apple1_pia_read_pa(void *ud) {
    return ((Apple1State *)ud)->key_data;
}
static uint8_t apple1_pia_read_pb(void *ud) {
    (void)ud;
    return 0;
}
static void apple1_pia_write_pb(void *ud, uint8_t val) {
    apple1_write_display(ud, val);
}

/* Reset the PIA.  Without a monitor ROM the native monitor stands in for
 * the Woz monitor, so leave the PIA as its reset code does: PB0-6 outputs
 * (DDRB $7F), and both control registers $A7 (output registers selected,
 * CA1/CB1 active on the rising edge). */
static void apple1_pia_setup(Apple1State *s) {
    s->pia->read_pa = apple1_pia_read_pa;
    s->pia->read_pb = apple1_pia_read_pb;
    s->pia->write_pb = apple1_pia_write_pb;
    s->pia->ud = s;
    pia6821_init(s->pia);
    if (!s->have_monitor_rom) {
        pia6821_write(s->pia, 2, 0x7F);
        pia6821_write(s->pia, 1, 0xA7);
        pia6821_write(s->pia, 3, 0xA7);
    }
}

static bool apple1_romless_woz_trap(Apple1State *s) {
    if (!s || s->have_monitor_rom || s->cpu->PC != APPLE1_WOZ_ECHO)
        return false;

    apple1_write_display(s, s->cpu->A);
    s->cpu->SP++;
    uint8_t lo = apple1_read((uint16_t)(0x0100u | s->cpu->SP), s);
    s->cpu->SP++;
    uint8_t hi = apple1_read((uint16_t)(0x0100u | s->cpu->SP), s);
    s->cpu->PC = (uint16_t)(((uint16_t)hi << 8) | lo);
    s->cpu->PC++;
    s->cpu->cycle_count += APPLE1_ECHO_CYCLES;
    s->cpu->insn_count++;
    return true;
}

static void apple1_monitor_puts(Apple1State *s, const char *text) {
    while (*text)
        apple1_write_display(s, (uint8_t)*text++);
}

static void apple1_monitor_hex8(Apple1State *s, uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    apple1_write_display(s, (uint8_t)hex[v >> 4]);
    apple1_write_display(s, (uint8_t)hex[v & 0x0F]);
}

static void apple1_monitor_hex16(Apple1State *s, uint16_t v) {
    apple1_monitor_hex8(s, (uint8_t)(v >> 8));
    apple1_monitor_hex8(s, (uint8_t)v);
}

static void apple1_monitor_prompt(Apple1State *s) {
    apple1_monitor_puts(s, "\\\r");
}

static const char *apple1_monitor_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int apple1_monitor_hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static bool apple1_monitor_parse_hex(const char **pp, uint16_t *out) {
    const char *p = apple1_monitor_skip_ws(*pp);
    uint32_t v = 0;
    int n = 0;
    for (;;) {
        int d = apple1_monitor_hex_digit(*p);
        if (d < 0) break;
        v = ((v << 4) | (uint32_t)d) & 0xFFFFu;
        p++;
        n++;
    }
    if (!n) return false;
    *pp = p;
    *out = (uint16_t)v;
    return true;
}

static uint8_t apple1_peek_mem(const Apple1State *s, uint16_t addr) {
    return (s->rom_map[addr] || addr < s->ram_size) ? s->mem[addr] : 0x00u;
}

static void apple1_monitor_store(Apple1State *s, uint16_t addr, const char *p) {
    uint16_t value = 0;
    bool any = false;
    while (apple1_monitor_parse_hex(&p, &value)) {
        if (!s->rom_map[addr] && addr < s->ram_size)
            s->mem[addr] = (uint8_t)value;
        s->mon_last_addr = addr++;
        any = true;
        p = apple1_monitor_skip_ws(p);
    }
    if (!any)
        apple1_monitor_puts(s, "?\r");
}

static void apple1_monitor_dump(Apple1State *s, uint16_t start, uint16_t end) {
    uint16_t addr = start;
    bool first = true;
    for (;;) {
        if (!first)
            apple1_write_display(s, '\r');
        first = false;
        apple1_monitor_hex16(s, addr);
        apple1_monitor_puts(s, ": ");
        for (int i = 0; i < 8; i++) {
            apple1_monitor_hex8(s, apple1_peek_mem(s, addr));
            s->mon_last_addr = addr;
            if (addr == end) return;
            addr++;
            apple1_write_display(s, ' ');
        }
    }
}

static void apple1_monitor_execute(Apple1State *s, const char *line) {
    const char *p = apple1_monitor_skip_ws(line);
    uint16_t addr = s->mon_last_addr;
    bool have_addr = apple1_monitor_parse_hex(&p, &addr);
    p = apple1_monitor_skip_ws(p);

    if (!have_addr) {
        if (*p == '\0') return;
        if (toupper((unsigned char)*p) == 'R') {
            s->cpu->PC = s->mon_last_addr;
            s->native_monitor = false;
            return;
        }
        return;
    }

    s->mon_last_addr = addr;
    if (*p == ':') {
        apple1_monitor_store(s, addr, p + 1);
    } else if (*p == '.') {
        p++;
        uint16_t end = addr;
        if (!apple1_monitor_parse_hex(&p, &end)) {
            apple1_monitor_puts(s, "?\r");
            return;
        }
        apple1_monitor_dump(s, addr, end);
    } else if (toupper((unsigned char)*p) == 'R') {
        s->cpu->PC = addr;
        s->native_monitor = false;
    } else if (*p == '\0') {
        apple1_monitor_dump(s, addr, addr);
    } else {
        apple1_monitor_dump(s, addr, addr);
    }
}

static void apple1_monitor_key(Apple1State *s, uint8_t ch) {
    ch &= 0x7Fu;
    if (ch == '\n') ch = '\r';
    if (ch >= 'a' && ch <= 'z') ch = (uint8_t)toupper(ch);
    if (ch == 0x7F || ch == '\b') {
        if (s->mon_len > 0)
            s->mon_len--;
        return;
    }
    if (ch == '\r') {
        apple1_write_display(s, '\r');
        s->mon_line[s->mon_len] = '\0';
        apple1_monitor_execute(s, s->mon_line);
        s->mon_len = 0;
        if (s->native_monitor && !s->quit_requested)
            apple1_monitor_prompt(s);
        return;
    }
    if (ch < ' ' || ch > '_')
        return;
    if (s->mon_len < (int)sizeof(s->mon_line) - 1) {
        s->mon_line[s->mon_len++] = (char)ch;
        apple1_write_display(s, ch);
    }
}

static void apple1_monitor_poll(Apple1State *s) {
    apple1_poll_input_devices(s);
    while (s->display && apple1_display_key_available(s->display))
        apple1_monitor_key(s, apple1_display_read_key(s->display));

    GemuSerial *ser = s->cfg->serial;
    while (ser && ser->key_available(ser->ud))
        apple1_monitor_key(s, ser->read_byte(ser->ud));
}

static bool token_eq(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

static bool apple1_sendkey_token(Apple1State *s, const char *tok) {
    if (!tok || !*tok) return true;
    if (token_eq(tok, "reset")) {
        apple1_reset_machine(s);
        return true;
    }
    if (token_eq(tok, "cls") || token_eq(tok, "clear")) {
        apple1_display_clear(s->display);
        return true;
    }
    if (token_eq(tok, "enter") || token_eq(tok, "return")) {
        apple1_display_kpush(s->display, '\r');
        return true;
    }
    if (token_eq(tok, "space")) {
        apple1_display_kpush(s->display, ' ');
        return true;
    }
    if (token_eq(tok, "tab")) {
        apple1_display_kpush(s->display, '\t');
        return true;
    }
    if (token_eq(tok, "backspace") || token_eq(tok, "bs") ||
        token_eq(tok, "delete") || token_eq(tok, "del")) {
        apple1_display_kpush(s->display, 0x7F);
        return true;
    }
    if (token_eq(tok, "dash") || token_eq(tok, "minus")) {
        apple1_display_kpush(s->display, '-');
        return true;
    }

    size_t len = strlen(tok);
    if (len == 1) {
        apple1_display_kpush(s->display, (uint8_t)tok[0]);
        return true;
    }
    if (len >= 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        char *end = NULL;
        long v = strtol(tok, &end, 16);
        if (end && *end == '\0' && v >= 0 && v <= 0x7F) {
            apple1_display_kpush(s->display, (uint8_t)v);
            return true;
        }
    }
    return false;
}

static GemuMonCmd apple1_cmd_sendkey(GemuMonitor *mon, const char *args, void *ud) {
    Apple1State *s = ud;
    if (!*args) {
        gemu_monitor_printf(mon, "sendkey: usage: sendkey a-b-c-enter | reset | cls\n");
        return GEMU_MON_NONE;
    }

    char seq[256];
    snprintf(seq, sizeof(seq), "%s", args);
    char *tok = seq;
    while (tok && *tok) {
        char *next = strchr(tok, '-');
        if (next) *next++ = '\0';
        if (!apple1_sendkey_token(s, tok)) {
            gemu_monitor_printf(mon, "sendkey: unknown key '%s'\n", tok);
            return GEMU_MON_NONE;
        }
        tok = next;
    }
    return GEMU_MON_NONE;
}

static GemuMediaResult apple1_tape_change(void *ud, const char *arg,
                                          char *err, size_t err_len) {
    Apple1State *s = ud;
    uint32_t addr = 0x0280u;
    bool explicit_addr = strchr(arg ? arg : "", ':') != NULL;
    const char *path = NULL;
    if (explicit_addr) {
        if (gemu_parse_addr_arg("apple1", arg, &addr, &path) < 0) {
            snprintf(err, err_len, "expected [ADDR:]FILE for raw Apple I tape");
            return GEMU_MEDIA_ERR;
        }
    } else {
        path = arg;
    }
    if (!path || !*path) {
        snprintf(err, err_len, "expected [ADDR:]FILE for raw Apple I tape");
        return GEMU_MEDIA_ERR;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(err, err_len, "cannot open '%s': %s", path, strerror(errno));
        return GEMU_MEDIA_ERR;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        snprintf(err, err_len, "cannot seek '%s'", path);
        return GEMU_MEDIA_ERR;
    }
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        snprintf(err, err_len, "empty tape '%s'", path);
        return GEMU_MEDIA_ERR;
    }
    rewind(fp);

    uint8_t *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        snprintf(err, err_len, "out of memory reading '%s'", path);
        return GEMU_MEDIA_ERR;
    }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        free(buf);
        snprintf(err, err_len, "read error on '%s'", path);
        return GEMU_MEDIA_ERR;
    }

    /* Real Apple I cassettes carry no address header -- it was typed into the
     * Woz Monitor by hand before pressing play. Recognise known dumps by
     * content hash (same romdb used for ROM images) so common tapes don't
     * need an explicit ADDR: prefix; fall back to the $0280 BASIC default
     * otherwise. */
    const RomDbEntry *match = NULL;
    if (!explicit_addr) {
        uint8_t digest[GEMU_SHA256_DIGEST_LEN];
        GemuSha256Ctx ctx;
        gemu_sha256_init(&ctx);
        gemu_sha256_update(&ctx, buf, (size_t)sz);
        gemu_sha256_final(&ctx, digest);
        char hex[65];
        gemu_sha256_hex(digest, hex);
        const RomDbEntry *e = romdb_lookup(hex);
        if (e && e->region && strcmp(e->region, "tape") == 0) {
            addr  = e->addr;
            match = e;
        }
    }

    if (addr >= s->ram_size || (uint64_t)addr + (uint64_t)sz > s->ram_size) {
        free(buf);
        snprintf(err, err_len, "tape does not fit in RAM at $%04X", (unsigned)addr);
        return GEMU_MEDIA_ERR;
    }
    memcpy(s->mem + addr, buf, (size_t)sz);
    free(buf);

    snprintf(s->tape_path, sizeof(s->tape_path), "%s", path);
    s->tape_addr = (uint16_t)addr;
    s->tape_size = (uint32_t)sz;
    if (match)
        printf("apple1: tape '%s' matched known dump '%s' -> $%04X\n",
               path, match->label, (unsigned)addr);
    printf("apple1: tape loaded $%04X-$%04X <- %s\n",
           (unsigned)addr, (unsigned)(addr + (uint32_t)sz - 1u), path);
    return GEMU_MEDIA_OK;
}

static GemuMediaResult apple1_tape_eject(void *ud, char *err, size_t err_len) {
    (void)err; (void)err_len;
    Apple1State *s = ud;
    s->tape_path[0] = '\0';
    s->tape_addr = 0;
    s->tape_size = 0;
    printf("apple1: tape ejected\n");
    return GEMU_MEDIA_OK;
}

static void apple1_tape_status(void *ud, char *buf, size_t buf_len) {
    Apple1State *s = ud;
    if (!s->tape_path[0])
        snprintf(buf, buf_len, "empty");
    else
        snprintf(buf, buf_len, "$%04X-$%04X",
                 (unsigned)s->tape_addr,
                 (unsigned)(s->tape_addr + s->tape_size - 1u));
}

/* ── The core machine (stage M3) ─────────────────────────────────────────────
 * board (apple1-board): the memory, the keyboard latch, the built-in
 * monitor's line and the terminal; cpu (mos6502): the CPU and the machine's
 * executor at 1 MHz; pia (pia6821): the keyboard and display PIA, its IRQs
 * not wired (as on the board).
 * The bus, "program" (16 bits): RAM from $0000 (ram_size); each run of ROM
 * (rom_map) read-only over it, with a handler that drops writes; the PIA at
 * $D010-$D013.  Anything else reads $00. */
typedef struct Apple1Core {
    GemuMachine *machine;
    GemuDevice  *board, *cpu, *pia;
    GemuSpace   *bus;
    GemuSpaceCache       cache;
    GemuScreen  *screen;
} Apple1Core;

static struct GemuScreen *apple1_screen(Apple1State *s) { return s->core->screen; }

static uint8_t apple1_read(uint16_t addr, void *ud) {
    Apple1State *s = ud;
    gemu_monitor_check_read(s->monitor, addr);
    return gemu_space_cache_read8(&s->core->cache, addr);
}
static void apple1_write(uint16_t addr, uint8_t val, void *ud) {
    Apple1State *s = ud;
    gemu_monitor_check_write(s->monitor, addr);
    gemu_space_cache_write8(&s->core->cache, addr, val);
}

static void rom_w8(void *ud, uint64_t off, uint8_t val) { (void)ud; (void)off; (void)val; }
static const GemuHandlers rom_drop = { .w8 = rom_w8 };

/* The ROMs' runs as read-only memory, after loading them. */
static void apple1_rom_maps(Apple1State *s) {
    Apple1Core *c = s->core;
    for (uint32_t a = 0; a < 0x10000u; ) {
        if (!s->rom_map[a]) { a++; continue; }
        uint32_t end = a;
        while (end < 0x10000u && s->rom_map[end]) end++;
        GemuMapping *mp = gemu_space_map_ram(c->bus, a, end - a, s->mem + a, GEMU_MAP_READONLY, 1);
        if (mp) gemu_mapping_set_name(mp, "ROM");
        gemu_space_map(c->bus, a, end - a, &rom_drop, s, 1, 0);
        a = end;
    }
}

static bool apple1_bus_build(Apple1State *s) {
    Apple1Core *c = s->core;
    if (!(c->bus = gemu_space_new(c->machine, "program", 16, 8, GEMU_LITTLE_ENDIAN)))
        return false;
    c->cache.space = c->bus;
    gemu_space_set_unmapped(c->bus, 0, NULL, NULL, NULL);
    GemuMapping *ram = gemu_space_map_ram(c->bus, 0, s->ram_size, s->mem, 0, 0);
    if (!ram) return false;
    gemu_mapping_set_name(ram, "RAM");
    return gemu_space_map(c->bus, APPLE1_KBD, 4, &gemu_pia6821_bus, c->pia, 2, 0) != NULL;
}

/* Monitor memory view: RAM and ROM as the CPU sees them.  The PIA keyboard
 * register is reported without clearing the key-ready flag. */
static bool apple1_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    Apple1State *s = ud;
    if (addr > 0xFFFF) return false;
    if (addr >= APPLE1_KBD && addr <= APPLE1_DSPCR) {
        *val = pia6821_peek(s->pia, (unsigned)addr & 3u);
        return true;
    }
    *val = apple1_peek_mem(s, (uint16_t)addr);
    return true;
}

/* Writes go to RAM only (not ROM or the PIA). */
static bool apple1_mon_write(void *ud, uint64_t addr, uint8_t val) {
    Apple1State *s = ud;
    if (addr > 0xFFFF) return false;
    if (addr >= APPLE1_KBD && addr <= APPLE1_DSPCR) return false;
    if (s->rom_map[addr] || addr >= s->ram_size) return false;
    s->mem[addr] = val;
    return true;
}

static void apple1_mon_cpu_state(void *ud, char *buf, size_t len) {
    const Apple1State *s = ud;
    mos6502_format_state(s->cpu, buf, len);
}

static void apple1_board_describe(GemuDevice *d, char *buf, size_t len) {
    const Apple1State *s = *(Apple1State **)d->priv;
    snprintf(buf, len, "%u KB RAM, %s monitor%s, keyboard latch %02X\n"
             "terminal: cursor at row %d, column %d",
             (unsigned)(s->ram_size / 1024), s->have_monitor_rom ? "ROM" : "built-in",
             s->native_monitor ? " (in it)" : "", s->key_data,
             s->display ? s->display->row : 0, s->display ? s->display->col : 0);
}

/* The terminal: its 24 rows of 40 characters and the cursor (the display's
 * own; zeros without one). */
static void term_save(void *ud, GemuStateWriter *w) {
    const Apple1State *s = ud;
    uint8_t screen[A1_ROWS][A1_COLS];
    uint32_t row = 0, col = 0;
    memset(screen, 0, sizeof screen);
    if (s->display) {
        memcpy(screen, s->display->screen, sizeof screen);
        row = (uint32_t)s->display->row;
        col = (uint32_t)s->display->col;
    }
    gemu_state_write(w, screen, sizeof screen);
    gemu_state_write_u32(w, row);
    gemu_state_write_u32(w, col);
}
static bool term_check(void *ud, GemuStateReader *r) {
    (void)ud;
    if (gemu_state_left(r) != A1_ROWS * A1_COLS + 8) {
        gemu_state_refuse(r, "apple1 terminal: bad item");
        return false;
    }
    return true;
}
static bool term_load(void *ud, GemuStateReader *r) {
    Apple1State *s = ud;
    uint8_t screen[A1_ROWS][A1_COLS];
    uint32_t row, col;
    if (!gemu_state_read(r, screen, sizeof screen) || !gemu_state_read_u32(r, &row) ||
        !gemu_state_read_u32(r, &col) || row >= A1_ROWS || col >= A1_COLS)
        return false;
    if (s->display) {
        memcpy(s->display->screen, screen, sizeof screen);
        s->display->row = (int)row;
        s->display->col = (int)col;
        s->display->dirty = true;
    }
    return true;
}

static const GemuDeviceType apple1_board_type = {
    .name = "apple1-board", .desc = "Apple I board (memory, keyboard, terminal)",
    .size = sizeof(Apple1State *), .describe = apple1_board_describe,
};

#define B(field) ok &= GEMU_STATE_MEMBER(d, "", s, field)
/* The board's state: the memory (the ROMs' bytes included), the keyboard
 * latch, the built-in monitor (whether it's in charge, its line), the
 * terminal.  Not saved: the configuration (ram_size, rom_map,
 * have_monitor_rom), the tape (media), the host's input queue. */
static bool apple1_board_state(Apple1State *s, GemuDevice *d) {
    bool ok = true;
    B(mem); B(key_data); B(native_monitor); B(mon_line); B(mon_len); B(mon_last_addr);
    ok &= gemu_state_add_custom(d, "terminal", term_save, term_check, term_load, s);
    return ok;
}
#undef B

static bool apple1_stop_before(void *ud) {
    Apple1State *s = ud;
    return gemu_monitor_check_exec(s->monitor, s->cpu->PC);
}
static void apple1_insn(void *ud) {
    Apple1State *s = ud;
    if (!apple1_romless_woz_trap(s))
        mos6502_step(s->cpu);
}
static bool apple1_stop_after(void *ud) {
    return gemu_monitor_is_paused(((Apple1State *)ud)->monitor);
}

static bool apple1_core_build(Apple1State *s) {
    Apple1Core *c = s->core = calloc(1, sizeof *c);
    if (!c || !(c->machine = gemu_machine_new("apple1")))
        return false;
    static const GemuDeviceType *const set_types[] = { &apple1_board_type };
    char set[1][GEMU_OPTS_ROUTE_LEN], why[512];
    if (!gemu_opts_route(s->cfg->machine_opts, set_types, 1, set, why, sizeof why)) {
        fprintf(stderr, "gemu: %s\n", why);
        return false;
    }
    static const char *const variant[] = { "6502", "2a03", "2a07", "6501", "6507" };
    char cpu_opts[32];
    snprintf(cpu_opts, sizeof cpu_opts, "variant=%s", variant[s->cfg->cpu]);
    bool ok = (c->board = gemu_device_add(c->machine, &apple1_board_type, "board", NULL, NULL)) &&
              (c->cpu = gemu_device_add(c->machine, &gemu_mos6502_type, "cpu", NULL, cpu_opts)) &&
              (c->pia = gemu_device_add(c->machine, &gemu_pia6821_type, "pia", NULL, NULL));
    if (ok) {
        *(Apple1State **)c->board->priv = s;
        ok = apple1_board_state(s, c->board);
    }
    if (!ok) {
        fprintf(stderr, "apple1: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    bool inited = gemu_machine_init(c->machine);
    if (inited) {
        s->cpu = gemu_mos6502(c->cpu);
        s->pia = gemu_pia6821(c->pia);
        s->cpu->mem_read = apple1_read;
        s->cpu->mem_write = apple1_write;
        s->cpu->mem_ud = s;
    }
    GemuMos6502Hooks h = { .stop_before = apple1_stop_before, .insn = apple1_insn,
                           .stop_after = apple1_stop_after, .ud = s };
    ok = inited && apple1_bus_build(s) && gemu_mos6502_exec(c->cpu, APPLE1_HZ, &h) &&
         (c->screen = gemu_screen_new(c->board, "screen")) &&
         gemu_screen_set_size(c->screen, A1_FB_W, A1_FB_H);
    if (ok) {
        gemu_screen_set_aspect(c->screen, 4, 3);
        gemu_screen_set_update(c->screen, apple1_screen_update, s);
    }
    if (!ok || !gemu_machine_start(c->machine)) {
        fprintf(stderr, "apple1: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    gemu_monitor_attach_machine(s->monitor, c->machine, "program");
    return true;
}

static void apple1_core_free(Apple1State *s) {
    if (!s->core) return;
    gemu_machine_free(s->core->machine);
    free(s->core);
    s->core = NULL;
}

static bool apple1_load_roms(Apple1State *s, const MosConfig *cfg) {
    for (int i = 0; i < cfg->n_roms; i++) {
        const char *region = cfg->roms[i].region;
        if (region && strcmp(region, "chargen") == 0) {
            FILE *f = fopen(cfg->roms[i].path, "rb");
            if (!f) {
                fprintf(stderr, "apple1: failed to open chargen '%s'\n", cfg->roms[i].path);
                return false;
            }
            uint8_t buf[A1_CHARGEN_SIZE];
            size_t n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            if (n != sizeof(buf)) {
                fprintf(stderr, "apple1: chargen '%s' is %zu bytes, expected %u\n",
                        cfg->roms[i].path, n, (unsigned)A1_CHARGEN_SIZE);
                return false;
            }
            if (s->display) {
                memcpy(s->display->chargen, buf, sizeof(buf));
                s->display->have_chargen = true;
                if (s->display->cg == MOS_CG_CM2140_COMPAT && cfg->char_gen == MOS_CG_AUTO)
                    s->display->cg = MOS_CG_CM2140;
                s->display->dirty = true;
            }
            printf("apple1: loaded Signetics 2513 chargen <- %s\n",
                   cfg->roms[i].path);
            continue;
        }
        if (region && strcmp(region, "cffa") == 0) {
            uint32_t addr = cfg->roms[i].addr ? (cfg->roms[i].addr & 0xFFFFu) : 0x9000u;
            GemuMemory tmp = {.data = s->mem + addr, .size = 0x10000u - addr};
            size_t len = 0;
            if (!gemu_mem_load_file(&tmp, 0, cfg->roms[i].path, &len)) {
                fprintf(stderr, "apple1: failed to load CFFA ROM '%s'\n",
                        cfg->roms[i].path);
                return false;
            }
            if (addr == 0x9000u && len > 0x1FE0u)
                len = 0x1FE0u;
            memset(s->rom_map + addr, 1, len);
            printf("apple1: CFFA ROM %zu bytes @ 0x%04X <- %s\n",
                   len, (unsigned)addr, cfg->roms[i].path);
            gemu_monitor_register_rom(s->monitor, addr, (uint32_t)len, cfg->roms[i].path);
            continue;
        }
        if (region && region[0] && strcmp(region, "main") != 0) {
            printf("apple1: recognized auxiliary ROM %s (%s)\n",
                   cfg->roms[i].path, region);
            continue;
        }

        uint32_t addr = cfg->roms[i].addr & 0xFFFFu;
        GemuMemory tmp = {.data = s->mem + addr, .size = 0x10000u - addr};
        size_t len = 0;
        if (!gemu_mem_load_file(&tmp, 0, cfg->roms[i].path, &len)) {
            fprintf(stderr, "apple1: failed to load '%s'\n", cfg->roms[i].path);
            return false;
        }
        memset(s->rom_map + addr, 1, len);
        if (addr <= APPLE1_ROM && addr + len > APPLE1_ROM)
            s->have_monitor_rom = true;
        printf("apple1: %zu bytes @ 0x%04X <- %s\n",
               len, (unsigned)addr, cfg->roms[i].path);
        gemu_monitor_register_rom(s->monitor, addr, (uint32_t)len, cfg->roms[i].path);
    }
    return true;
}

Apple1State *apple1_create(const MosConfig *cfg) {
    Apple1State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg = cfg;
    s->ram_size = cfg->mem_size ? cfg->mem_size : (8u * 1024u);
    if (s->ram_size > APPLE1_MAX_RAM) {
        fprintf(stderr, "apple1: RAM size must be 1K-52K (got %uK)\n",
                s->ram_size / 1024u);
        free(s);
        return NULL;
    }
    s->monitor = gemu_monitor_create();
    if (!s->monitor) {
        free(s);
        return NULL;
    }
    gemu_monitor_set_screendump_cb(s->monitor, apple1_screendump, s);
    gemu_monitor_bind(s->monitor, "sendkey", apple1_cmd_sendkey, s);
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = apple1_mon_read, .write = apple1_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = false });
    gemu_monitor_set_cpu_state_cb(s->monitor, apple1_mon_cpu_state, s);

    s->display = apple1_display_create(cfg);
    if (cfg->display_type != GEMU_DISPLAY_NONE && !s->display)
        fprintf(stderr, "apple1: display unavailable, falling back to stdio\n");

    if (!apple1_core_build(s) || !apple1_load_roms(s, cfg)) {
        apple1_destroy(s);
        return NULL;
    }
    apple1_rom_maps(s);
    apple1_pia_setup(s);
    if (!s->have_monitor_rom) {
        s->native_monitor = true;
        fprintf(stderr,
                "apple1: no CPU monitor ROM at $FF00; using built-in ROMless monitor\n");
    }
    if (s->display && cfg->char_gen == MOS_CG_CM2140 && !s->display->have_chargen)
        fprintf(stderr, "apple1: -cg cm2140 requested but no chargen ROM was loaded; using built-in cm2140-compat fallback\n");

    if (cfg->a1ci) {
        GemuMediaDevice tape_dev = {
            .name = "tape",
            .kind = "a1ci",
            .ud = s,
            .change = apple1_tape_change,
            .eject = apple1_tape_eject,
            .status = apple1_tape_status,
        };
        if (cfg->tape_path)
            snprintf(tape_dev.file, sizeof(tape_dev.file), "%s", cfg->tape_path);
        gemu_monitor_register_media(s->monitor, &tape_dev);

        if (cfg->tape_path) {
            char arg[640], err[256] = "";
            if (cfg->tape_addr_explicit)
                snprintf(arg, sizeof(arg), "0x%04X:%s",
                         (unsigned)cfg->tape_addr, cfg->tape_path);
            else
                snprintf(arg, sizeof(arg), "%s", cfg->tape_path);
            if (apple1_tape_change(s, arg, err, sizeof(err)) != GEMU_MEDIA_OK) {
                fprintf(stderr, "apple1: tape: %s\n", err[0] ? err : "load failed");
                apple1_destroy(s);
                return NULL;
            }
        }
    } else if (cfg->tape_path) {
        fprintf(stderr, "apple1: -tape requires -device a1ci\n");
        apple1_destroy(s);
        return NULL;
    }

    mos6502_reset(s->cpu);
    if (cfg->has_start_addr)
        s->cpu->PC = cfg->start_addr;
    apple1_display_clear(s->display);
    if (s->native_monitor)
        apple1_monitor_prompt(s);
    apple1_update_vnc(s, false);

    return s;
}

void apple1_destroy(Apple1State *s) {
    if (!s) return;
    apple1_core_free(s);
    gemu_monitor_destroy(s->monitor);
    apple1_display_destroy(s->display);
    free(s);
}

void apple1_run(Apple1State *s, const MosConfig *cfg) {
    (void)cfg;
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        GemuSerial *ser = s->cfg->serial;
        if (ser && ser->should_quit(ser->ud))
            quit = true;
        if (s->display && s->display->quit)
            quit = true;
        if (s->quit_requested)
            quit = true;

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { quit = true; break; }
            } else if (cmd == GEMU_MON_RESET) {
                apple1_reset_machine(s);
            } else if (cmd == GEMU_MON_STEP) {
                for (uint32_t n = gemu_monitor_step_count(s->monitor); n; n--)
                    apple1_insn(s);
            }
        }

        bool paused = gemu_monitor_is_paused(s->monitor);
        apple1_display_set_paused(s->display, paused);

        if (s->native_monitor) {
            if (!paused)
                apple1_monitor_poll(s);
        } else if (!paused) {
            apple1_poll_keyboard(s);
            GemuMachine *m = s->core->machine;   /* 1/60 s of machine time */
            gemu_machine_run_until(m, gemu_machine_time(m) + gemu_clocks(APPLE1_HZ, APPLE1_CPF));
            gemu_screen_frame_begin(s->core->screen);
        }
        apple1_display_render(s->display, paused);
        apple1_update_vnc(s, paused);

        /* Pace to the machine's time; with the CPU idle (paused, or the
         * built-in monitor in charge), a frame's delay. */
        if (!paused && !s->native_monitor) {
            gemu_machine_pace(s->core->machine, 0);
        } else {
            gemu_machine_pace_reset(s->core->machine);
            SDL_Delay(1000u / APPLE1_FPS);
        }
    }

    printf("apple1: %llu cpu cycles\n",
           (unsigned long long)s->cpu->cycle_count);
}
