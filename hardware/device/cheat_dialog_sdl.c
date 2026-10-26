/*
 * Debug > Game Genie, drawn with SDL (see cheat_dialog.h): its own window in
 * the Windows 3.1 look and IBM 8x16 font of the window menus.
 *
 *   +--------------------------------------------------------------+
 *   | On  Code      Name                                           |
 *   | [x] SXIOPO    Infinite lives                                 |
 *   | [ ] ...                                                      |
 *   +--------------------------------------------------------------+
 *   Name:  [______________________________]
 *   Code:  [________]  Address: [____]  Value: [__]  Compare: [__]
 *   [ Add ] [ Update ] [ Delete ]                          [ Close ]
 *   status line
 *
 * Typing a code fills in Address/Value/Compare; typing those fills in the
 * code.  Keys: Tab moves between the list and the fields, Up/Down pick a
 * code, Space turns it on/off, Delete removes it, Enter adds (or updates
 * the picked code), Esc closes.  Events come from an SDL event watch, as
 * in hex_editor_sdl.c.
 */
#include "cheat_dialog.h"
#include "vgafont16.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLYPH_W   8
#define GLYPH_H   16
#define ROW_H     (GLYPH_H + 2)
#define LIST_ROWS 10
#define WIN_W     (70 * GLYPH_W)
#define PAD       8

#define C_BG      0xFFFFFF
#define C_FACE    0xC0C0C0
#define C_TEXT    0x000000
#define C_DIM     0x808080
#define C_LIGHT   0xFFFFFF
#define C_DARK    0x808080
#define C_HI      0x000080
#define C_HITEXT  0xFFFFFF
#define C_ERR     0x800000

enum { F_NAME, F_CODE, F_ADDR, F_VAL, F_CMP, N_FIELDS };
enum { B_ADD, B_UPDATE, B_DELETE, B_CLOSE, N_BUTTONS };
#define FOCUS_LIST (-1)

typedef struct {
    CheatDialog   base;
    NesCheats    *cheats;
    unsigned      seen_version;

    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *font;
    bool          visible, dirty;
    Uint32        last_draw;

    int           sel, top;             /* picked code, first row shown */
    int           focus;                /* FOCUS_LIST or a field */
    char          field[N_FIELDS][64];
    int           field_max[N_FIELDS];
    char          status[160];
    bool          status_err;
    Uint32        last_click;
    int           last_click_row;
    bool          text_seen;            /* SDL_TEXTINPUT reaches this window */
    bool          close_requested;      /* hide at the next refresh (see event_watch) */
    char          fallback_char;        /* last key typed without it */

    SDL_Rect      list_rect, field_rect[N_FIELDS], button_rect[N_BUTTONS];
} SdlCheatDialog;

static const char *button_label[N_BUTTONS] = { "Add", "Update", "Delete", "Close" };

/* ── Drawing ─────────────────────────────────────────────────────────────── */

static void fill(SDL_Renderer *r, int x, int y, int w, int h, uint32_t rgb) {
    SDL_SetRenderDrawColor(r, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 255);
    SDL_RenderFillRect(r, &(SDL_Rect){ x, y, w, h });
}

static void frame(SDL_Renderer *r, SDL_Rect rc, uint32_t rgb) {
    fill(r, rc.x, rc.y, rc.w, 1, rgb);
    fill(r, rc.x, rc.y + rc.h - 1, rc.w, 1, rgb);
    fill(r, rc.x, rc.y, 1, rc.h, rgb);
    fill(r, rc.x + rc.w - 1, rc.y, 1, rc.h, rgb);
}

static void text(SdlCheatDialog *d, int x, int y, const char *s, uint32_t rgb) {
    SDL_SetTextureColorMod(d->font, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb);
    for (; *s; s++, x += GLYPH_W) {
        unsigned ch = (unsigned char)*s;
        SDL_Rect src = { (int)(ch % 16) * GLYPH_W, (int)(ch / 16) * GLYPH_H, GLYPH_W, GLYPH_H };
        SDL_RenderCopy(d->ren, d->font, &src, &(SDL_Rect){ x, y, GLYPH_W, GLYPH_H });
    }
}

static bool make_font(SdlCheatDialog *d) {
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
    d->font = SDL_CreateTextureFromSurface(d->ren, s);
    SDL_FreeSurface(s);
    if (d->font) SDL_SetTextureBlendMode(d->font, SDL_BLENDMODE_BLEND);
    return d->font != NULL;
}

/* Windows 3.1 push button (as in gemu_menubar_sdl.c, at 1x). */
static void button(SdlCheatDialog *d, SDL_Rect rc, const char *label, bool dflt) {
    SDL_Renderer *r = d->ren;
    int b = dflt ? 2 : 1;
    fill(r, rc.x + 1, rc.y, rc.w - 2, b, C_TEXT);
    fill(r, rc.x + 1, rc.y + rc.h - b, rc.w - 2, b, C_TEXT);
    fill(r, rc.x, rc.y + 1, b, rc.h - 2, C_TEXT);
    fill(r, rc.x + rc.w - b, rc.y + 1, b, rc.h - 2, C_TEXT);
    SDL_Rect in = { rc.x + b, rc.y + b, rc.w - 2 * b, rc.h - 2 * b };
    fill(r, in.x, in.y, in.w, in.h, C_FACE);
    fill(r, in.x, in.y, in.w - 1, 2, C_LIGHT);
    fill(r, in.x, in.y, 2, in.h - 1, C_LIGHT);
    fill(r, in.x + 1, in.y + in.h - 2, in.w - 1, 2, C_DARK);
    fill(r, in.x + in.w - 2, in.y + 1, 2, in.h - 1, C_DARK);
    int tw = (int)strlen(label) * GLYPH_W;
    text(d, rc.x + (rc.w - tw) / 2, rc.y + (rc.h - GLYPH_H) / 2, label, C_TEXT);
}

static int list_y0(void) { return PAD + ROW_H + 2; }   /* below the header row */

static void layout(SdlCheatDialog *d) {
    d->list_rect = (SDL_Rect){ PAD, PAD, WIN_W - 2 * PAD, ROW_H * (LIST_ROWS + 1) + 4 };
    int y = d->list_rect.y + d->list_rect.h + 10;
    int lx = PAD + 6 * GLYPH_W;
    d->field_rect[F_NAME] = (SDL_Rect){ lx, y, 40 * GLYPH_W + 8, ROW_H + 4 };
    y += ROW_H + 10;
    d->field_rect[F_CODE] = (SDL_Rect){ lx, y, 8 * GLYPH_W + 8, ROW_H + 4 };
    int x = lx + 8 * GLYPH_W + 8 + 11 * GLYPH_W;
    d->field_rect[F_ADDR] = (SDL_Rect){ x, y, 4 * GLYPH_W + 8, ROW_H + 4 };
    x += 4 * GLYPH_W + 8 + 9 * GLYPH_W;
    d->field_rect[F_VAL]  = (SDL_Rect){ x, y, 2 * GLYPH_W + 8, ROW_H + 4 };
    x += 2 * GLYPH_W + 8 + 11 * GLYPH_W;
    d->field_rect[F_CMP]  = (SDL_Rect){ x, y, 2 * GLYPH_W + 8, ROW_H + 4 };
    y += ROW_H + 14;
    int bw = 10 * GLYPH_W, bh = GLYPH_H + 10;
    for (int i = 0; i < B_CLOSE; i++)
        d->button_rect[i] = (SDL_Rect){ PAD + i * (bw + 8), y, bw, bh };
    d->button_rect[B_CLOSE] = (SDL_Rect){ WIN_W - PAD - bw, y, bw, bh };
}

static int win_h(SdlCheatDialog *d) {
    layout(d);
    return d->button_rect[0].y + d->button_rect[0].h + 10 + GLYPH_H + 8;
}

static void draw(SdlCheatDialog *d) {
    SDL_Renderer *r = d->ren;
    NesCheats *c = d->cheats;
    layout(d);
    int H = win_h(d);
    fill(r, 0, 0, WIN_W, H, C_FACE);

    /* the list */
    SDL_Rect L = d->list_rect;
    fill(r, L.x, L.y, L.w, L.h, C_BG);
    frame(r, L, d->focus == FOCUS_LIST ? C_HI : C_TEXT);
    fill(r, L.x + 1, L.y + 1, L.w - 2, ROW_H, C_FACE);
    text(d, L.x + 6, L.y + 2, "On  Code      Name", C_TEXT);
    fill(r, L.x + 1, L.y + ROW_H + 1, L.w - 2, 1, C_TEXT);
    if (d->sel >= c->n) d->sel = c->n - 1;
    if (d->sel >= 0 && d->sel < d->top) d->top = d->sel;
    if (d->sel >= d->top + LIST_ROWS) d->top = d->sel - LIST_ROWS + 1;
    if (c->n == 0)
        text(d, L.x + 6, list_y0() + 2, "(no codes yet - type one below and press Add)", C_DIM);
    for (int row = 0; row < LIST_ROWS && d->top + row < c->n; row++) {
        int i = d->top + row, y = list_y0() + row * ROW_H;
        bool hi = i == d->sel;
        if (hi) fill(r, L.x + 1, y, L.w - 2, ROW_H, C_HI);
        uint32_t fg = hi ? C_HITEXT : C_TEXT;
        SDL_Rect box = { L.x + 8, y + 3, 12, 12 };          /* checkbox */
        fill(r, box.x, box.y, box.w, box.h, C_BG);
        frame(r, box, C_TEXT);
        if (c->list[i].enabled) {                            /* the 3.1 "X" check */
            for (int k = 2; k < 10; k++) {
                fill(r, box.x + k, box.y + k, 1, 1, C_TEXT);
                fill(r, box.x + 11 - k, box.y + k, 1, 1, C_TEXT);
            }
        }
        text(d, L.x + 6 + 4 * GLYPH_W, y + 1, c->list[i].code, fg);
        char name[64];
        snprintf(name, sizeof(name), "%.52s", c->list[i].name);
        text(d, L.x + 6 + 14 * GLYPH_W, y + 1, name, fg);
    }

    /* fields */
    static const char *labels[N_FIELDS] = { "Name:", "Code:", "Address:", "Value:", "Compare:" };
    for (int f = 0; f < N_FIELDS; f++) {
        SDL_Rect R = d->field_rect[f];
        text(d, R.x - ((int)strlen(labels[f]) + 1) * GLYPH_W, R.y + 3, labels[f], C_TEXT);
        fill(r, R.x, R.y, R.w, R.h, C_BG);
        frame(r, R, d->focus == f ? C_HI : C_TEXT);
        if (d->focus == f) frame(r, (SDL_Rect){ R.x + 1, R.y + 1, R.w - 2, R.h - 2 }, C_HI);
        text(d, R.x + 4, R.y + 3, d->field[f], C_TEXT);
        if (d->focus == f && (SDL_GetTicks() / 500) % 2 == 0)       /* caret */
            fill(r, R.x + 4 + (int)strlen(d->field[f]) * GLYPH_W, R.y + 3, 1, GLYPH_H, C_TEXT);
    }

    /* buttons */
    bool fields_focused = d->focus != FOCUS_LIST;
    for (int b = 0; b < N_BUTTONS; b++)
        button(d, d->button_rect[b], button_label[b],
               fields_focused && b == (d->sel >= 0 ? B_UPDATE : B_ADD));

    /* status */
    int sy = H - GLYPH_H - 6;
    fill(r, 0, sy - 4, WIN_W, 1, C_DARK);
    char status[200];
    if (d->status[0]) {
        snprintf(status, sizeof(status), "%s", d->status);
    } else {
        int on = 0;
        for (int i = 0; i < c->n; i++) on += c->list[i].enabled;
        snprintf(status, sizeof(status), "%d code%s, %d on.  Space: on/off   Del: delete   Enter: add/update",
                 c->n, c->n == 1 ? "" : "s", on);
    }
    text(d, PAD, sy, status, d->status_err ? C_ERR : C_TEXT);
    SDL_RenderPresent(r);
    d->dirty = false;
}

/* ── Fields ──────────────────────────────────────────────────────────────── */

static void set_status(SdlCheatDialog *d, bool err, const char *msg) {
    snprintf(d->status, sizeof(d->status), "%s", msg);
    d->status_err = err;
}

/* Code -> Address/Value/Compare. */
static void decode_fields(SdlCheatDialog *d) {
    uint16_t a; uint8_t v, k; bool h;
    if (!nes_gg_decode(d->field[F_CODE], &a, &v, &k, &h)) return;
    snprintf(d->field[F_ADDR], sizeof(d->field[F_ADDR]), "%04X", a);
    snprintf(d->field[F_VAL], sizeof(d->field[F_VAL]), "%02X", v);
    if (h) snprintf(d->field[F_CMP], sizeof(d->field[F_CMP]), "%02X", k);
    else d->field[F_CMP][0] = '\0';
}

/* Address/Value/Compare -> Code. */
static void encode_fields(SdlCheatDialog *d) {
    if (!d->field[F_ADDR][0] || !d->field[F_VAL][0]) return;
    unsigned long a = strtoul(d->field[F_ADDR], NULL, 16);
    unsigned long v = strtoul(d->field[F_VAL], NULL, 16);
    int cmp = d->field[F_CMP][0] ? (int)strtoul(d->field[F_CMP], NULL, 16) : -1;
    char code[9];
    if (a < 0x8000 || a > 0xFFFF) {
        set_status(d, true, "Game Genie codes patch $8000-$FFFF");
        return;
    }
    if (nes_gg_encode((uint16_t)a, (uint8_t)v, cmp, code))
        snprintf(d->field[F_CODE], sizeof(d->field[F_CODE]), "%s", code);
}

static void type_char(SdlCheatDialog *d, char ch) {
    if (d->focus == FOCUS_LIST) return;
    char *f = d->field[d->focus];
    size_t n = strlen(f);
    if ((int)n >= d->field_max[d->focus]) return;
    if (d->focus == F_CODE) {
        ch = (char)toupper((unsigned char)ch);
        if (!strchr("APZLGITYEOXUKSVN", ch)) return;
    } else if (d->focus != F_NAME) {
        if (!isxdigit((unsigned char)ch)) return;
        ch = (char)toupper((unsigned char)ch);
    } else if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7E) {
        return;
    }
    f[n] = ch;
    f[n + 1] = '\0';
    d->status[0] = '\0';
    if (d->focus == F_CODE) decode_fields(d);
    else if (d->focus != F_NAME) encode_fields(d);
}

static void backspace(SdlCheatDialog *d) {
    if (d->focus == FOCUS_LIST) return;
    size_t n = strlen(d->field[d->focus]);
    if (!n) return;
    d->field[d->focus][n - 1] = '\0';
    if (d->focus == F_ADDR || d->focus == F_VAL || d->focus == F_CMP) encode_fields(d);
}

static void pick(SdlCheatDialog *d, int i) {
    NesCheats *c = d->cheats;
    d->sel = i;
    d->status[0] = '\0';
    if (i < 0 || i >= c->n) return;
    snprintf(d->field[F_NAME], sizeof(d->field[F_NAME]), "%s", c->list[i].name);
    snprintf(d->field[F_CODE], sizeof(d->field[F_CODE]), "%s", c->list[i].code);
    decode_fields(d);
}

static void clear_fields(SdlCheatDialog *d) {
    for (int f = 0; f < N_FIELDS; f++) d->field[f][0] = '\0';
}

/* The code to add/update: the Code field, or a code typed into Name. */
static bool code_to_use(SdlCheatDialog *d) {
    uint16_t a; uint8_t v, k; bool h;
    if (d->field[F_CODE][0]) return true;
    if (nes_gg_decode(d->field[F_NAME], &a, &v, &k, &h)) {
        for (char *p = d->field[F_NAME]; *p; p++) *p = (char)toupper((unsigned char)*p);
        snprintf(d->field[F_CODE], sizeof(d->field[F_CODE]), "%s", d->field[F_NAME]);
        d->field[F_NAME][0] = '\0';
        decode_fields(d);
        return true;
    }
    set_status(d, true, "Type a Game Genie code in the Code field");
    d->focus = F_CODE;
    return false;
}

static void press(SdlCheatDialog *d, int b) {
    NesCheats *c = d->cheats;
    char err[160];
    if ((b == B_ADD || b == B_UPDATE) && !code_to_use(d)) return;
    switch (b) {
    case B_ADD:
        if (nes_cheats_add(c, d->field[F_CODE], d->field[F_NAME], true, err, sizeof(err))) {
            set_status(d, false, "Added and turned on");
            d->sel = -1;              /* ready for the next one */
            clear_fields(d);
            d->focus = F_CODE;
        } else {
            set_status(d, true, err);
        }
        break;
    case B_UPDATE:
        if (nes_cheats_update(c, d->sel, d->field[F_CODE], d->field[F_NAME], err, sizeof(err)))
            set_status(d, false, "Updated");
        else
            set_status(d, true, err);
        break;
    case B_DELETE:
        if (d->sel < 0) { set_status(d, true, "Pick a code to delete"); break; }
        nes_cheats_remove(c, d->sel);
        if (d->sel >= c->n) d->sel = c->n - 1;
        pick(d, d->sel);
        if (d->sel < 0) clear_fields(d);
        set_status(d, false, "Deleted");
        break;
    case B_CLOSE:
        d->base.hide(&d->base);
        break;
    }
}

/* ── Events ──────────────────────────────────────────────────────────────── */

static bool in_rect(SDL_Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void on_click(SdlCheatDialog *d, int x, int y) {
    for (int b = 0; b < N_BUTTONS; b++)
        if (in_rect(d->button_rect[b], x, y)) { press(d, b); return; }
    for (int f = 0; f < N_FIELDS; f++)
        if (in_rect(d->field_rect[f], x, y)) { d->focus = f; return; }
    if (in_rect(d->list_rect, x, y) && y >= list_y0()) {
        d->focus = FOCUS_LIST;
        int row = (y - list_y0()) / ROW_H, i = d->top + row;
        if (row >= LIST_ROWS || i >= d->cheats->n) { d->sel = -1; clear_fields(d); return; }
        Uint32 now = SDL_GetTicks();
        bool dbl = row == d->last_click_row && now - d->last_click < 400;
        d->last_click = now;
        d->last_click_row = row;
        if (x < d->list_rect.x + 4 * GLYPH_W || dbl)         /* checkbox or double-click */
            nes_cheats_set_enabled(d->cheats, i, !d->cheats->list[i].enabled);
        pick(d, i);
    }
}

static void on_key(SdlCheatDialog *d, const SDL_KeyboardEvent *k) {
    NesCheats *c = d->cheats;
    SDL_Keycode key = k->keysym.sym;
    bool shift = (k->keysym.mod & KMOD_SHIFT) != 0;
    switch (key) {
    case SDLK_ESCAPE: d->base.hide(&d->base); return;
    case SDLK_TAB: {
        int order[] = { FOCUS_LIST, F_NAME, F_CODE, F_ADDR, F_VAL, F_CMP }, n = 6, at = 0;
        for (int i = 0; i < n; i++) if (order[i] == d->focus) at = i;
        d->focus = order[(at + (shift ? n - 1 : 1)) % n];
        return;
    }
    case SDLK_RETURN: case SDLK_KP_ENTER:
        if (d->focus != FOCUS_LIST) press(d, d->sel >= 0 ? B_UPDATE : B_ADD);
        else if (d->sel >= 0) nes_cheats_set_enabled(c, d->sel, !c->list[d->sel].enabled);
        return;
    case SDLK_BACKSPACE: backspace(d); return;
    default: break;
    }
    if (d->focus != FOCUS_LIST) {
        /* Characters normally arrive as SDL_TEXTINPUT.  Some desktops never
         * send it to a second window; until it shows up, type from keys. */
        if (!d->text_seen && key >= SDLK_SPACE && key <= SDLK_z &&
            !(k->keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))) {
            char ch = (char)key;
            if (shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            d->fallback_char = ch;
            type_char(d, ch);
        }
        return;
    }
    switch (key) {
    case SDLK_UP:     if (d->sel > 0) pick(d, d->sel - 1); else if (c->n) pick(d, 0); break;
    case SDLK_DOWN:   if (d->sel + 1 < c->n) pick(d, d->sel + 1); break;
    case SDLK_HOME:   if (c->n) pick(d, 0); break;
    case SDLK_END:    if (c->n) pick(d, c->n - 1); break;
    case SDLK_SPACE:  if (d->sel >= 0) nes_cheats_set_enabled(c, d->sel, !c->list[d->sel].enabled); break;
    case SDLK_DELETE: press(d, B_DELETE); break;
    default: break;
    }
}

static int SDLCALL event_watch(void *ud, SDL_Event *ev) {
    SdlCheatDialog *d = ud;
    Uint32 id = d->win ? SDL_GetWindowID(d->win) : 0, ev_id;
    switch (ev->type) {
    case SDL_WINDOWEVENT:     ev_id = ev->window.windowID; break;
    case SDL_KEYDOWN:         ev_id = ev->key.windowID;    break;
    case SDL_TEXTINPUT:       ev_id = ev->text.windowID;   break;
    case SDL_MOUSEBUTTONDOWN: ev_id = ev->button.windowID; break;
    case SDL_MOUSEWHEEL:      ev_id = ev->wheel.windowID;  break;
    default:                  return 1;
    }
    if (!id || ev_id != id) return 1;
    switch (ev->type) {
    case SDL_WINDOWEVENT:
        /* Not hidden here: SDL counts visible windows right after
         * delivering this, and with this one hidden it would think the
         * last window closed and quit the emulator. */
        if (ev->window.event == SDL_WINDOWEVENT_CLOSE) d->close_requested = true;
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) SDL_StartTextInput();
        break;
    case SDL_KEYDOWN:   on_key(d, &ev->key); break;
    case SDL_TEXTINPUT: {
        const char *p = ev->text.text;
        /* the key this text belongs to was already typed by the fallback */
        if (!d->text_seen && d->fallback_char && !p[1] &&
            tolower((unsigned char)p[0]) == tolower((unsigned char)d->fallback_char)) p++;
        d->text_seen = true;
        for (; *p; p++) type_char(d, *p);
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (ev->button.button == SDL_BUTTON_LEFT) on_click(d, ev->button.x, ev->button.y);
        break;
    case SDL_MOUSEWHEEL: {
        int top = d->top - ev->wheel.y;
        int max = d->cheats->n - LIST_ROWS;
        d->top = top < 0 ? 0 : top > max ? (max > 0 ? max : 0) : top;
        break;
    }
    }
    d->dirty = true;
    return 1;
}

/* ── CheatDialog ─────────────────────────────────────────────────────────── */

static void sdl_show(CheatDialog *base) {
    SdlCheatDialog *d = (SdlCheatDialog *)base;
    SDL_ShowWindow(d->win);
    /* Some desktops (Wayland with fractional scaling, via sdl2-compat)
     * shrink the create-time size; asking again once shown gets the real
     * size, and the logical size below scales the dialog to whatever the
     * window ends up as rather than clipping it. */
    SDL_SetWindowSize(d->win, WIN_W, win_h(d));
    SDL_RaiseWindow(d->win);
    SDL_StartTextInput();       /* covers windows that exist now, this one included */
    d->visible = d->dirty = true;
}

static void sdl_hide(CheatDialog *base) {
    SdlCheatDialog *d = (SdlCheatDialog *)base;
    SDL_HideWindow(d->win);
    d->visible = false;
}

static bool sdl_visible(const CheatDialog *base) {
    return ((const SdlCheatDialog *)base)->visible;
}

static void sdl_refresh(CheatDialog *base) {
    SdlCheatDialog *d = (SdlCheatDialog *)base;
    if (d->close_requested) {
        d->close_requested = false;
        sdl_hide(base);
    }
    if (!d->visible) return;
    if (d->seen_version != d->cheats->version) {      /* e.g. the monitor edited it */
        d->seen_version = d->cheats->version;
        d->dirty = true;
    }
    Uint32 now = SDL_GetTicks();
    if (!d->dirty && now - d->last_draw < 250) return;   /* caret blink */
    d->last_draw = now;
    draw(d);
}

static void sdl_destroy(CheatDialog *base) {
    SdlCheatDialog *d = (SdlCheatDialog *)base;
    if (!d) return;
    SDL_DelEventWatch(event_watch, d);
    if (d->font) SDL_DestroyTexture(d->font);
    if (d->ren) SDL_DestroyRenderer(d->ren);
    if (d->win) SDL_DestroyWindow(d->win);
    free(d);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

CheatDialog *cheat_dialog_sdl_create(NesCheats *cheats) {
    if (!cheats || SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL;
    SdlCheatDialog *d = calloc(1, sizeof(*d));
    if (!d) { SDL_QuitSubSystem(SDL_INIT_VIDEO); return NULL; }
    d->base = (CheatDialog){ sdl_show, sdl_hide, sdl_visible, sdl_refresh, sdl_destroy };
    d->cheats = cheats;
    d->sel = -1;
    d->focus = F_CODE;
    d->last_click_row = -1;
    d->field_max[F_NAME] = 40;
    d->field_max[F_CODE] = 8;
    d->field_max[F_ADDR] = 4;
    d->field_max[F_VAL] = 2;
    d->field_max[F_CMP] = 2;
    d->win = SDL_CreateWindow("GEMU - Game Genie", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              WIN_W, win_h(d), SDL_WINDOW_HIDDEN);
    d->ren = d->win ? SDL_CreateRenderer(d->win, -1, 0) : NULL;
    if (!d->ren || !make_font(d)) {
        sdl_destroy(&d->base);
        return NULL;
    }
    /* Draw in dialog coordinates whatever the window's real size; SDL maps
     * mouse positions back the same way. */
    SDL_RenderSetLogicalSize(d->ren, WIN_W, win_h(d));
    SDL_AddEventWatch(event_watch, d);
    return &d->base;
}
