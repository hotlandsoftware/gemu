/*
 * Hex editor drawn with SDL, for builds with neither GTK nor Win32 (the
 * plain Linux SDL build) - see hex_editor.h.  Its own window, in the same
 * Windows 3.1 look and IBM 8x16 font as the window menus:
 *
 *   tabs        one per memory region (click, or Tab / Shift+Tab)
 *   hex view    16 bytes per line with ASCII; click a byte to select it
 *   status      selected address and value, and the keys
 *
 * Keys: arrows / PgUp / PgDn / Home / End move the selection, 0-9 A-F type
 * over the selected byte a nibble at a time (unless the region is
 * read-only), G (or Ctrl+G) goes to an address, Esc closes the window.
 *
 * Its events are picked out of SDL's queue with an event watch; the display
 * backend leaves events for windows other than its own alone.
 */
#if !defined(GEMU_GTK) && !defined(_WIN32)
#include "hex_editor.h"
#include "vgafont16.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEX_MAX_TABS   8
#define GLYPH_W        8
#define GLYPH_H        16
#define LINE_BYTES     16
#define TAB_H          (GLYPH_H + 8)
#define STATUS_H       (GLYPH_H + 8)
#define PAD            6
#define COLS           78      /* address, 16 hex bytes, 16 ASCII characters */
#define ROWS_DEFAULT   24

/* Windows 3.1 colours, as in gemu_menubar_sdl.c */
#define C_BG       0xFFFFFF
#define C_FACE     0xC0C0C0
#define C_TEXT     0x000000
#define C_DIM      0x808080
#define C_ADDR     0x000080
#define C_HI       0x000080
#define C_HITEXT   0xFFFFFF
#define C_RO       0x606060

struct HexEditor {
    HexRegion   tabs[HEX_MAX_TABS];
    char        labels[HEX_MAX_TABS][64];
    int         n_tabs, cur;
    size_t      sel[HEX_MAX_TABS];    /* selected offset per tab */
    size_t      top[HEX_MAX_TABS];    /* first line shown per tab */
    bool        low_nibble;           /* next hex digit edits the low nibble */

    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *font;
    bool          visible, sized;
    bool          close_requested;  /* hide at the next refresh (see hex_event_watch) */
    Uint32        last_draw;
    bool          dirty;

    bool        goto_open;            /* "Go to address" prompt */
    char        goto_buf[12];
    char        message[80];          /* status line note */

    SDL_Rect    tab_rect[HEX_MAX_TABS];
};

/* ── Drawing ─────────────────────────────────────────────────────────────── */

static void fill(SDL_Renderer *r, int x, int y, int w, int h, uint32_t rgb) {
    SDL_SetRenderDrawColor(r, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 255);
    SDL_RenderFillRect(r, &(SDL_Rect){ x, y, w, h });
}

static void text(HexEditor *he, int x, int y, const char *s, uint32_t rgb) {
    SDL_SetTextureColorMod(he->font, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb);
    for (; *s; s++, x += GLYPH_W) {
        unsigned ch = (unsigned char)*s;
        SDL_Rect src = { (int)(ch % 16) * GLYPH_W, (int)(ch / 16) * GLYPH_H, GLYPH_W, GLYPH_H };
        SDL_RenderCopy(he->ren, he->font, &src, &(SDL_Rect){ x, y, GLYPH_W, GLYPH_H });
    }
}

static bool make_font(HexEditor *he) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 16 * GLYPH_W, 16 * GLYPH_H, 32,
                                                    SDL_PIXELFORMAT_ARGB8888);
    if (!s) return false;
    uint32_t *px = s->pixels;
    int pitch = s->pitch / 4;
    for (int ch = 0; ch < 256; ch++)
        for (int y = 0; y < GLYPH_H; y++)
            for (int x = 0; x < GLYPH_W; x++)
                px[((ch / 16) * GLYPH_H + y) * pitch + (ch % 16) * GLYPH_W + x] =
                    (vgafont16[ch * 16 + y] & (0x80 >> x)) ? 0xFFFFFFFFu : 0;
    he->font = SDL_CreateTextureFromSurface(he->ren, s);
    SDL_FreeSurface(s);
    if (he->font) SDL_SetTextureBlendMode(he->font, SDL_BLENDMODE_BLEND);
    return he->font != NULL;
}

/* Hex lines that fit between the tabs and the status line. */
static int view_rows(const HexEditor *he) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(he->ren, &w, &h);
    int rows = (h - TAB_H - STATUS_H - 2 * PAD) / GLYPH_H;
    return rows > 1 ? rows : 1;
}

static size_t region_lines(const HexRegion *r) {
    return (r->size + LINE_BYTES - 1) / LINE_BYTES;
}

/* Keep the selection on screen. */
static void scroll_to_sel(HexEditor *he) {
    const HexRegion *r = &he->tabs[he->cur];
    size_t line = he->sel[he->cur] / LINE_BYTES, rows = (size_t)view_rows(he);
    if (line < he->top[he->cur]) he->top[he->cur] = line;
    if (line >= he->top[he->cur] + rows) he->top[he->cur] = line - rows + 1;
    size_t lines = region_lines(r);
    if (he->top[he->cur] + rows > lines) he->top[he->cur] = lines > rows ? lines - rows : 0;
}

static void draw(HexEditor *he) {
    SDL_Renderer *r = he->ren;
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(r, &ow, &oh);
    fill(r, 0, 0, ow, oh, C_BG);

    /* tabs */
    fill(r, 0, 0, ow, TAB_H, C_FACE);
    int x = PAD;
    for (int i = 0; i < he->n_tabs; i++) {
        int w = (int)strlen(he->labels[i]) * GLYPH_W + 16;
        he->tab_rect[i] = (SDL_Rect){ x, 2, w, TAB_H - 2 };
        bool cur = i == he->cur;
        fill(r, x, cur ? 2 : 4, w, TAB_H - (cur ? 2 : 4), cur ? C_BG : C_FACE);
        fill(r, x, cur ? 2 : 4, w, 1, C_TEXT);                    /* outline */
        fill(r, x, cur ? 2 : 4, 1, TAB_H, C_TEXT);
        fill(r, x + w - 1, cur ? 2 : 4, 1, TAB_H, C_TEXT);
        text(he, x + 8, cur ? 6 : 7, he->labels[i], C_TEXT);
        x += w + 2;
    }
    fill(r, 0, TAB_H - 1, ow, 1, C_TEXT);

    /* hex view */
    const HexRegion *reg = &he->tabs[he->cur];
    int rows = view_rows(he), y = TAB_H + PAD;
    if (!reg->data || !reg->size) {
        text(he, PAD, y, "(no data)", C_DIM);
    } else {
        scroll_to_sel(he);
        size_t sel = he->sel[he->cur];
        for (int row = 0; row < rows; row++, y += GLYPH_H) {
            size_t off = (he->top[he->cur] + (size_t)row) * LINE_BYTES;
            if (off >= reg->size) break;
            char buf[16];
            snprintf(buf, sizeof(buf), "%05X:", (unsigned)(reg->base_addr + off) & 0xFFFFFu);
            text(he, PAD, y, buf, C_ADDR);
            for (int b = 0; b < LINE_BYTES && off + (size_t)b < reg->size; b++) {
                uint8_t v = reg->data[off + (size_t)b];
                int hx = PAD + (8 + b * 3 + (b >= 8)) * GLYPH_W;
                int ax = PAD + (8 + LINE_BYTES * 3 + 2 + b) * GLYPH_W;
                bool hi = off + (size_t)b == sel;
                snprintf(buf, sizeof(buf), "%02X", v);
                char asc[2] = { (char)(v >= 0x20 && v < 0x7F ? v : '.'), 0 };
                if (hi) {
                    fill(r, hx - 1, y, 2 * GLYPH_W + 2, GLYPH_H, C_HI);
                    fill(r, ax, y, GLYPH_W, GLYPH_H, C_HI);
                    if (!reg->read_only)   /* which nibble the next digit replaces */
                        fill(r, hx + (he->low_nibble ? GLYPH_W : 0), y + GLYPH_H - 2, GLYPH_W, 2, C_HITEXT);
                }
                text(he, hx, y, buf, hi ? C_HITEXT : (reg->read_only ? C_RO : C_TEXT));
                text(he, ax, y, asc, hi ? C_HITEXT : C_DIM);
            }
        }
    }

    /* status line */
    int sy = oh - STATUS_H;
    fill(r, 0, sy, ow, STATUS_H, C_FACE);
    fill(r, 0, sy, ow, 1, C_TEXT);
    char status[200];
    if (he->goto_open) {
        snprintf(status, sizeof(status), "Go to address (hex): %s_   Enter = go, Esc = cancel", he->goto_buf);
    } else if (reg->data && reg->size) {
        size_t sel = he->sel[he->cur];
        snprintf(status, sizeof(status), "%05X = %02X (%u)%s   %s",
                 (unsigned)(reg->base_addr + sel) & 0xFFFFFu, reg->data[sel], reg->data[sel],
                 reg->read_only ? "  read-only" : "",
                 he->message[0] ? he->message
                 : reg->read_only ? "G: go to   Tab: next region"
                                  : "0-F: edit   G: go to   Tab: next region");
    } else {
        snprintf(status, sizeof(status), "Tab: next region");
    }
    text(he, PAD, sy + 4, status, C_TEXT);
    SDL_RenderPresent(r);
    he->dirty = false;
}

/* ── Editing ─────────────────────────────────────────────────────────────── */

static void move_sel(HexEditor *he, long delta) {
    const HexRegion *reg = &he->tabs[he->cur];
    if (!reg->size) return;
    long s = (long)he->sel[he->cur] + delta;
    if (s < 0) s = 0;
    if ((size_t)s >= reg->size) s = (long)reg->size - 1;
    he->sel[he->cur] = (size_t)s;
    he->low_nibble = false;
    he->message[0] = '\0';
}

static void type_nibble(HexEditor *he, int v) {
    HexRegion *reg = &he->tabs[he->cur];
    if (!reg->data || !reg->size) return;
    if (reg->read_only) {
        snprintf(he->message, sizeof(he->message), "This region is read-only");
        return;
    }
    uint8_t *p = &reg->data[he->sel[he->cur]];
    if (he->low_nibble) {
        *p = (uint8_t)((*p & 0xF0) | v);
        move_sel(he, 1);               /* clears low_nibble */
    } else {
        *p = (uint8_t)((*p & 0x0F) | (v << 4));
        he->low_nibble = true;
    }
}

static void do_goto(HexEditor *he) {
    HexRegion *reg = &he->tabs[he->cur];
    he->goto_open = false;
    if (!he->goto_buf[0]) return;
    unsigned long a = strtoul(he->goto_buf, NULL, 16);
    if (a < reg->base_addr || a >= reg->base_addr + reg->size) {
        snprintf(he->message, sizeof(he->message), "%lX is outside this region", a);
        return;
    }
    he->sel[he->cur] = a - reg->base_addr;
    he->low_nibble = false;
    he->message[0] = '\0';
}

static int hex_value(SDL_Keycode k) {
    if (k >= SDLK_0 && k <= SDLK_9) return (int)(k - SDLK_0);
    if (k >= SDLK_a && k <= SDLK_f) return (int)(k - SDLK_a) + 10;
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return (int)(k - SDLK_KP_1) + 1;
    if (k == SDLK_KP_0) return 0;
    return -1;
}

static void on_key(HexEditor *he, const SDL_KeyboardEvent *k) {
    SDL_Keycode key = k->keysym.sym;
    bool ctrl = (k->keysym.mod & KMOD_CTRL) != 0, shift = (k->keysym.mod & KMOD_SHIFT) != 0;
    long rows = view_rows(he);

    if (he->goto_open) {
        size_t n = strlen(he->goto_buf);
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) do_goto(he);
        else if (key == SDLK_ESCAPE) he->goto_open = false;
        else if (key == SDLK_BACKSPACE && n) he->goto_buf[n - 1] = '\0';
        else if (hex_value(key) >= 0 && n + 1 < sizeof(he->goto_buf)) {
            he->goto_buf[n] = "0123456789ABCDEF"[hex_value(key)];
            he->goto_buf[n + 1] = '\0';
        }
        return;
    }
    switch (key) {
    case SDLK_ESCAPE:   hex_editor_hide(he); return;
    case SDLK_LEFT:     move_sel(he, -1); break;
    case SDLK_RIGHT:    move_sel(he, 1); break;
    case SDLK_UP:       move_sel(he, -LINE_BYTES); break;
    case SDLK_DOWN:     move_sel(he, LINE_BYTES); break;
    case SDLK_PAGEUP:   move_sel(he, -LINE_BYTES * rows); break;
    case SDLK_PAGEDOWN: move_sel(he, LINE_BYTES * rows); break;
    case SDLK_HOME:     move_sel(he, ctrl ? -(long)he->tabs[he->cur].size
                                          : -(long)(he->sel[he->cur] % LINE_BYTES)); break;
    case SDLK_END:      move_sel(he, ctrl ? (long)he->tabs[he->cur].size
                                          : LINE_BYTES - 1 - (long)(he->sel[he->cur] % LINE_BYTES)); break;
    case SDLK_TAB:
        he->cur = (he->cur + (shift ? he->n_tabs - 1 : 1)) % he->n_tabs;
        he->low_nibble = false;
        he->message[0] = '\0';
        break;
    case SDLK_g:
        he->goto_open = true;
        he->goto_buf[0] = '\0';
        break;
    default: {
        int v = hex_value(key);
        if (v >= 0 && !ctrl) type_nibble(he, v);
        break;
    }
    }
}

static void on_click(HexEditor *he, int x, int y) {
    for (int i = 0; i < he->n_tabs; i++) {
        SDL_Rect t = he->tab_rect[i];
        if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) {
            he->cur = i;
            he->low_nibble = false;
            return;
        }
    }
    const HexRegion *reg = &he->tabs[he->cur];
    int row = (y - TAB_H - PAD) / GLYPH_H;
    if (y < TAB_H + PAD || row >= view_rows(he) || !reg->size) return;
    int col = (x - PAD) / GLYPH_W, b = -1;
    if (col >= 8 && col < 8 + LINE_BYTES * 3 + 1) {           /* hex column */
        int c = col - 8;
        if (c >= 8 * 3) c--;                                  /* the gap after byte 8 */
        b = c / 3;
    } else if (col >= 8 + LINE_BYTES * 3 + 2 && col < 8 + LINE_BYTES * 4 + 2) {
        b = col - (8 + LINE_BYTES * 3 + 2);                   /* ASCII column */
    }
    if (b < 0 || b >= LINE_BYTES) return;
    size_t off = (he->top[he->cur] + (size_t)row) * LINE_BYTES + (size_t)b;
    if (off < reg->size) {
        he->sel[he->cur] = off;
        he->low_nibble = false;
        he->message[0] = '\0';
    }
}

/* Event watch: this window's events, as SDL queues them. */
static int SDLCALL hex_event_watch(void *ud, SDL_Event *ev) {
    HexEditor *he = ud;
    Uint32 id = he->win ? SDL_GetWindowID(he->win) : 0, ev_id;
    switch (ev->type) {
    case SDL_WINDOWEVENT:     ev_id = ev->window.windowID; break;
    case SDL_KEYDOWN:         ev_id = ev->key.windowID;    break;
    case SDL_MOUSEBUTTONDOWN: ev_id = ev->button.windowID; break;
    case SDL_MOUSEWHEEL:      ev_id = ev->wheel.windowID;  break;
    default:                  return 1;
    }
    if (!id || ev_id != id) return 1;
    if (ev->type == SDL_WINDOWEVENT) {
        /* Not hidden here: SDL counts visible windows right after
         * delivering this, and with this one hidden it would think the
         * last window closed and quit the emulator. */
        if (ev->window.event == SDL_WINDOWEVENT_CLOSE) he->close_requested = true;
    } else if (ev->type == SDL_KEYDOWN) {
        on_key(he, &ev->key);
    } else if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) {
        on_click(he, ev->button.x, ev->button.y);
    } else if (ev->type == SDL_MOUSEWHEEL) {
        size_t *top = &he->top[he->cur];
        long t = (long)*top - ev->wheel.y * 3;
        size_t lines = region_lines(&he->tabs[he->cur]);
        if (t < 0) t = 0;
        if ((size_t)t >= lines) t = lines ? (long)lines - 1 : 0;
        *top = (size_t)t;
        /* keep the selection inside the view */
        size_t first = *top * LINE_BYTES, last = (*top + (size_t)view_rows(he)) * LINE_BYTES;
        if (he->sel[he->cur] < first) he->sel[he->cur] = first;
        if (he->sel[he->cur] >= last && last) he->sel[he->cur] = last - 1;
        if (he->sel[he->cur] >= he->tabs[he->cur].size && he->tabs[he->cur].size)
            he->sel[he->cur] = he->tabs[he->cur].size - 1;
    }
    he->dirty = true;
    return 1;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

HexEditor *hex_editor_create(const HexRegion *regions, int n_regions) {
    if (!regions || n_regions <= 0) return NULL;
    if (n_regions > HEX_MAX_TABS) n_regions = HEX_MAX_TABS;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL;
    HexEditor *he = calloc(1, sizeof(*he));
    if (!he) { SDL_QuitSubSystem(SDL_INIT_VIDEO); return NULL; }
    for (int i = 0; i < n_regions; i++) {
        he->tabs[i] = regions[i];
        snprintf(he->labels[i], sizeof(he->labels[i]), "%s", regions[i].label ? regions[i].label : "?");
        he->tabs[i].label = he->labels[i];
    }
    he->n_tabs = n_regions;
    he->win = SDL_CreateWindow("GEMU - Hex Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               COLS * GLYPH_W + 2 * PAD,
                               TAB_H + ROWS_DEFAULT * GLYPH_H + STATUS_H + 2 * PAD,
                               SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    he->ren = he->win ? SDL_CreateRenderer(he->win, -1, 0) : NULL;
    if (!he->ren || !make_font(he)) {
        hex_editor_destroy(he);
        return NULL;
    }
    SDL_SetWindowMinimumSize(he->win, 40 * GLYPH_W, TAB_H + 4 * GLYPH_H + STATUS_H + 2 * PAD);
    SDL_AddEventWatch(hex_event_watch, he);
    return he;
}

void hex_editor_destroy(HexEditor *he) {
    if (!he) return;
    SDL_DelEventWatch(hex_event_watch, he);
    if (he->font) SDL_DestroyTexture(he->font);
    if (he->ren) SDL_DestroyRenderer(he->ren);
    if (he->win) SDL_DestroyWindow(he->win);
    free(he);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void hex_editor_show(HexEditor *he) {
    if (!he) return;
    SDL_ShowWindow(he->win);
    if (!he->sized) {   /* create-time sizes can come out shrunk (Wayland scaling) */
        SDL_SetWindowSize(he->win, COLS * GLYPH_W + 2 * PAD,
                          TAB_H + ROWS_DEFAULT * GLYPH_H + STATUS_H + 2 * PAD);
        he->sized = true;
    }
    SDL_RaiseWindow(he->win);
    he->visible = true;
    he->dirty = true;
}

void hex_editor_hide(HexEditor *he) {
    if (!he) return;
    SDL_HideWindow(he->win);
    he->visible = false;
    he->goto_open = false;
}

bool hex_editor_is_visible(const HexEditor *he) {
    return he && he->visible;
}

/* Called every emulated frame: memory changes under us, so redraw ~30 times
 * a second while shown (and at once after input). */
void hex_editor_refresh(HexEditor *he) {
    if (he && he->close_requested) {
        he->close_requested = false;
        hex_editor_hide(he);
    }
    if (!he || !he->visible) return;
    Uint32 now = SDL_GetTicks();
    if (!he->dirty && now - he->last_draw < 33) return;
    he->last_draw = now;
    draw(he);
}

#endif /* !GEMU_GTK && !_WIN32 */
