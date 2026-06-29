#include "machine_apple1.h"
#include "gemu/memory.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/select.h>
#include <unistd.h>
#endif

#define APPLE1_KBD     0xD010u
#define APPLE1_KBDCR   0xD011u
#define APPLE1_DSP     0xD012u
#define APPLE1_DSPCR   0xD013u
#define APPLE1_ROM     0xFF00u
#define APPLE1_HZ      1000000u
#define APPLE1_FPS     60u
#define APPLE1_CPF     (APPLE1_HZ / APPLE1_FPS)

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
    SDL_SetWindowTitle(d->window, paused ? "GEMU [Paused]" : "GEMU");
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

    d->window = SDL_CreateWindow("GEMU",
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
    GemuVncKeyEvent ev;
    while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
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

static void apple1_update_vnc(Apple1State *s, bool paused) {
    if (!s || !s->vnc || !s->display || !s->vnc_fb)
        return;

    memset(s->vnc_fb, 0, (size_t)A1_FB_W * A1_FB_H);
    const uint8_t *font = apple1_display_font(s->display);
    bool cursor_phase = paused ? s->display->cursor_phase
                               : ((SDL_GetTicks() / 500u) & 1u) != 0;
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
                        s->vnc_fb[(y0 + gy) * A1_FB_W + x0 + gx] = 1;
                }
            }
        }
    }
    gemu_vnc_update(s->vnc, s->vnc_fb, A1_FB_W, A1_FB_H);
}

static void apple1_poll_keyboard(Apple1State *s) {
    apple1_poll_input_devices(s);
    if (s->display && !s->key_ready && apple1_display_key_available(s->display)) {
        s->key_data = apple1_normalize_key(apple1_display_read_key(s->display));
        s->key_ready = true;
    }

    GemuSerial *ser = s->cfg->serial;
    if (ser && !s->key_ready && ser->key_available(ser->ud)) {
        s->key_data = apple1_normalize_key(ser->read_byte(ser->ud));
        s->key_ready = true;
    }
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

static void apple1_monitor_store(Apple1State *s, uint16_t addr, const char *p) {
    uint16_t value = 0;
    bool any = false;
    while (apple1_monitor_parse_hex(&p, &value)) {
        if (!s->rom_map[addr])
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
            apple1_monitor_hex8(s, s->mem[addr]);
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
        if (toupper((unsigned char)*p) == 'Q' && p[1] == '\0') {
            if (s->cfg->no_shutdown)
                gemu_monitor_shutdown_or_pause(s->monitor, true);
            else
                s->quit_requested = true;
            return;
        }
        if (toupper((unsigned char)*p) == 'R') {
            s->cpu.PC = s->mon_last_addr;
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
        s->cpu.PC = addr;
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

static uint8_t apple1_read(uint16_t addr, void *ud) {
    Apple1State *s = ud;
    gemu_monitor_check_read(s->monitor, addr);

    if (addr == APPLE1_KBD) {
        uint8_t v = s->key_ready ? s->key_data : 0;
        s->key_ready = false;
        return v;
    }
    if (addr == APPLE1_KBDCR)
        return s->key_ready ? 0x80u : 0x00u;
    if (addr == APPLE1_DSPCR)
        return 0x80u;

    return s->mem[addr];
}

static void apple1_write(uint16_t addr, uint8_t val, void *ud) {
    Apple1State *s = ud;
    gemu_monitor_check_write(s->monitor, addr);

    if (addr == APPLE1_DSP) {
        apple1_write_display(s, val);
        return;
    }
    if (addr >= APPLE1_KBD && addr <= APPLE1_DSPCR)
        return;

    if (!s->rom_map[addr])
        s->mem[addr] = val;
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
    s->monitor = gemu_monitor_create();
    if (!s->monitor) {
        free(s);
        return NULL;
    }

    s->display = apple1_display_create(cfg);
    if (cfg->display_type != GEMU_DISPLAY_NONE && !s->display)
        fprintf(stderr, "apple1: display unavailable, falling back to stdio\n");
    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr, A1_FB_W, A1_FB_H);
        if (s->vnc) {
            gemu_vnc_set_colors(s->vnc, 0xFFFFFFu, 0x000000u);
            s->vnc_fb = malloc((size_t)A1_FB_W * A1_FB_H);
            if (!s->vnc_fb)
                fprintf(stderr, "apple1: failed to allocate VNC framebuffer\n");
        }
    }
    gemu_monitor_set_vnc(s->monitor, s->vnc);

    if (!apple1_load_roms(s, cfg)) {
        apple1_destroy(s);
        return NULL;
    }
    if (!s->have_monitor_rom) {
        s->native_monitor = true;
        fprintf(stderr,
                "apple1: no CPU monitor ROM at $FF00; using built-in ROMless monitor\n");
    }
    if (s->display && cfg->char_gen == MOS_CG_CM2140 && !s->display->have_chargen)
        fprintf(stderr, "apple1: -cg cm2140 requested but no chargen ROM was loaded; using built-in cm2140-compat fallback\n");

    mos6502_init(&s->cpu);
    s->cpu.mem_read = apple1_read;
    s->cpu.mem_write = apple1_write;
    s->cpu.mem_ud = s;
    s->cpu.decimal_disable = false;
    mos6502_reset(&s->cpu);
    if (cfg->has_start_addr)
        s->cpu.PC = cfg->start_addr;
    apple1_display_clear(s->display);
    if (s->native_monitor)
        apple1_monitor_prompt(s);
    apple1_update_vnc(s, false);

    return s;
}

void apple1_destroy(Apple1State *s) {
    if (!s) return;
    gemu_monitor_destroy(s->monitor);
    gemu_vnc_destroy(s->vnc);
    apple1_display_destroy(s->display);
    free(s->vnc_fb);
    free(s);
}

void apple1_run(Apple1State *s, const MosConfig *cfg) {
    (void)cfg;
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();
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
                mos6502_reset(&s->cpu);
                s->native_monitor = !s->have_monitor_rom;
                s->quit_requested = false;
                apple1_display_clear(s->display);
                if (s->native_monitor)
                    apple1_monitor_prompt(s);
            } else if (cmd == GEMU_MON_CUSTOM) {
                gemu_monitor_unknown_command(s->monitor);
            }
        }

        bool paused = gemu_monitor_is_paused(s->monitor);
        apple1_display_set_paused(s->display, paused);

        if (s->native_monitor) {
            if (!paused)
                apple1_monitor_poll(s);
        } else if (!paused) {
            apple1_poll_keyboard(s);
            uint64_t target = s->cpu.cycle_count + APPLE1_CPF;
            while (!quit && s->cpu.cycle_count < target) {
                if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) break;
                mos6502_step(&s->cpu);
            }
        }
        apple1_display_render(s->display, paused);
        apple1_update_vnc(s, paused);

        Uint32 dt = SDL_GetTicks() - t0;
        Uint32 frame_ms = 1000u / APPLE1_FPS;
        if (dt < frame_ms) SDL_Delay(frame_ms - dt);
    }

    printf("apple1: %llu cpu cycles\n",
           (unsigned long long)s->cpu.cycle_count);
}
