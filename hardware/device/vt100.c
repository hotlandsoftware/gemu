/*
 * vt100.c - DEC VT100 terminal emulator (SDL2 window)
 *
 * Translated from uconsole.pas (Hans Otten / Eduardo Casino, MIT).
 *
 * Public API: vt100_create / vt100_destroy / vt100_write / vt100_read_key /
 *             vt100_key_available / vt100_poll / vt100_should_quit
 */

#include "vt100.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef HAVE_TTF
#include "vt100_font.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

/* ── Terminal geometry ───────────────────────────────────────────────────── */

#define VT100_COLS       80
#define VT100_LINES      24
#define CELL_W           12
#define CELL_H           24
#define BORDER           10
#define WIN_W            (VT100_COLS  * CELL_W + 2 * BORDER)   /* 980 */
#define WIN_H            (VT100_LINES * CELL_H + 2 * BORDER)   /* 596 */
#define FONT_SIZE_PT     15
#define BLINK_MS         500u

/* ── Style bits ──────────────────────────────────────────────────────────── */

#define VT_BOLD      0x01u
#define VT_UNDERLINE 0x02u
#define VT_BLINK     0x04u
#define VT_REVERSE   0x08u
#define VT_STRIKE    0x10u

/* ── Charset codes ───────────────────────────────────────────────────────── */

typedef enum { CS_ASCII = 0, CS_GRAPHICS, CS_UK } VtCharset;

/* ── Parser states ───────────────────────────────────────────────────────── */

typedef enum {
    STATE_NORMAL = 0,
    STATE_ESCAPE,
    STATE_CSI,
    STATE_CSI_PARAM,
    STATE_CHARSET_DESIGNATE,
} VtState;

/* ── ANSI colour table ───────────────────────────────────────────────────── */

static const SDL_Color ansi_colors[16] = {
    {  0,   0,   0, 255},   /* 0 black         */
    {187,   0,   0, 255},   /* 1 dark red       */
    {  0, 187,   0, 255},   /* 2 dark green     */
    {229, 229,  16, 255},   /* 3 dark yellow    */
    {  0,   0, 187, 255},   /* 4 dark blue      */
    {187,   0, 187, 255},   /* 5 dark magenta   */
    {  0, 187, 187, 255},   /* 6 dark cyan      */
    {229, 229, 229, 255},   /* 7 light gray     */
    { 85,  85,  85, 255},   /* 8 dark gray      */
    {255,  85,  85, 255},   /* 9 bright red     */
    { 85, 255,  85, 255},   /* 10 bright green  */
    {255, 255,  85, 255},   /* 11 bright yellow */
    { 85,  85, 255, 255},   /* 12 bright blue   */
    {255,  85, 255, 255},   /* 13 bright magenta*/
    { 85, 255, 255, 255},   /* 14 bright cyan   */
    {255, 255, 255, 255},   /* 15 white         */
};

/* Default colors: green-on-black matching reference InitFColor = $89FDAF */
#define DEFAULT_FG ((SDL_Color){0xAF, 0xFD, 0x89, 0xFF})
#define DEFAULT_BG ((SDL_Color){0x00, 0x00, 0x00, 0xFF})

/* ── VT100 Special Graphics map ──────────────────────────────────────────── */

/* Maps ASCII '_'..'~' to UTF-8 strings when in graphics charset mode */
static const char *graphics_map[64]; /* indexed by (ch - '_') for ch in '_'..'~' */

static void build_graphics_map(void) {
    for (int i = 0; i < 64; i++) {
        static char pass[64][2];
        pass[i][0] = (char)('_' + i);
        pass[i][1] = '\0';
        graphics_map[i] = pass[i];
    }
    graphics_map['_' - '_'] = "\xC2\xA0";          /* NBSP     U+00A0 */
    graphics_map['`' - '_'] = "\xE2\x97\x86";       /* ◆        U+25C6 */
    graphics_map['a' - '_'] = "\xE2\x96\x92";       /* ▒        U+2592 */
    graphics_map['b' - '_'] = "\xE2\x90\x89";       /* ␉        U+2409 */
    graphics_map['c' - '_'] = "\xE2\x90\x8C";       /* ␌        U+240C */
    graphics_map['d' - '_'] = "\xE2\x90\x8D";       /* ␍        U+240D */
    graphics_map['e' - '_'] = "\xE2\x90\x8A";       /* ␊        U+240A */
    graphics_map['f' - '_'] = "\xC2\xB0";           /* °        U+00B0 */
    graphics_map['g' - '_'] = "\xC2\xB1";           /* ±        U+00B1 */
    graphics_map['h' - '_'] = "\xE2\x90\xA4";       /* ␤        U+2424 */
    graphics_map['i' - '_'] = "\xE2\x90\x8B";       /* ␋        U+240B */
    graphics_map['j' - '_'] = "\xE2\x94\x98";       /* ┘        U+2518 */
    graphics_map['k' - '_'] = "\xE2\x94\x90";       /* ┐        U+2510 */
    graphics_map['l' - '_'] = "\xE2\x94\x8C";       /* ┌        U+250C */
    graphics_map['m' - '_'] = "\xE2\x94\x94";       /* └        U+2514 */
    graphics_map['n' - '_'] = "\xE2\x94\xBC";       /* ┼        U+253C */
    graphics_map['o' - '_'] = "\xE2\x8E\xBA";       /* ⎺        U+23BA */
    graphics_map['p' - '_'] = "\xE2\x8E\xBB";       /* ⎻        U+23BB */
    graphics_map['q' - '_'] = "\xE2\x94\x80";       /* ─        U+2500 */
    graphics_map['r' - '_'] = "\xE2\x8E\xBC";       /* ⎼        U+23BC */
    graphics_map['s' - '_'] = "\xE2\x8E\xBD";       /* ⎽        U+23BD */
    graphics_map['t' - '_'] = "\xE2\x94\x9C";       /* ├        U+251C */
    graphics_map['u' - '_'] = "\xE2\x94\xA4";       /* ┤        U+2524 */
    graphics_map['v' - '_'] = "\xE2\x94\xB4";       /* ┴        U+2534 */
    graphics_map['w' - '_'] = "\xE2\x94\xAC";       /* ┬        U+252C */
    graphics_map['x' - '_'] = "\xE2\x94\x82";       /* │        U+2502 */
    graphics_map['y' - '_'] = "\xE2\x89\xA4";       /* ≤        U+2264 */
    graphics_map['z' - '_'] = "\xE2\x89\xA5";       /* ≥        U+2265 */
    graphics_map['{' - '_'] = "\xCF\x80";           /* π        U+03C0 */
    graphics_map['|' - '_'] = "\xE2\x89\xA0";       /* ≠        U+2260 */
    graphics_map['}' - '_'] = "\xC2\xA3";           /* £        U+00A3 */
    graphics_map['~' - '_'] = "\xC2\xB7";           /* ·        U+00B7 */
}

/* ── Key ring buffer ────────────────────────────────────────────────────── */

#define KEY_BUF_SIZE 256

/* ── Main state struct ───────────────────────────────────────────────────── */

struct Vt100State {
    /* SDL */
    SDL_Window   *window;
    SDL_Renderer *renderer;
    TTF_Font     *font;
    uint32_t      window_id;

    /* Screen buffers [line][col] (0-indexed) */
    char      text   [VT100_LINES][VT100_COLS];
    SDL_Color fcolor [VT100_LINES][VT100_COLS];
    SDL_Color bcolor [VT100_LINES][VT100_COLS];
    uint8_t   style  [VT100_LINES][VT100_COLS];
    VtCharset charset[VT100_LINES][VT100_COLS];
    bool      dirty  [VT100_LINES][VT100_COLS];
    bool      full_redraw;

    /* Cursor */
    int  cur_line, cur_col;   /* 0-indexed */
    bool cursor_visible;
    bool cursor_on;           /* current blink state */
    uint32_t last_blink_ms;

    /* Parser */
    VtState state;
    int     csi_params[16];
    int     csi_param_count;
    bool    dec_private;
    bool    designating_g0;

    /* Character sets */
    VtCharset charset_g0, charset_g1;
    bool      invoking_g1;
    bool      use_graphics;

    /* Scroll region (0-indexed, inclusive) */
    int scroll_top, scroll_bottom;

    /* Origin mode */
    bool dec_origin_mode;

    /* Saved cursor */
    int saved_line, saved_col;

    /* Current render attributes */
    SDL_Color cur_fg, cur_bg;
    uint8_t   cur_style;

    /* Default colors */
    SDL_Color def_fg, def_bg;

    /* Key buffer */
    uint8_t  key_buf[KEY_BUF_SIZE];
    int      key_head, key_tail;  /* circular: head=write, tail=read */

    /* Forwarded chars: text typed in the *host* (KIM-1) window that we
     * intercepted from SDL_TEXTINPUT but cannot push back via SDL_PushEvent
     * (crashes on SDL3-compat).  The machine drains this after each poll. */
    uint8_t  fwd_buf[KEY_BUF_SIZE];
    int      fwd_head, fwd_tail;

    /* DSR response queue */
    char     resp_buf[64];
    int      resp_len, resp_pos;

    /* Flags */
    bool should_quit;

    /* Font ascent for glyph baseline */
    int font_ascent;
};

/* ── Key buffer helpers ──────────────────────────────────────────────────── */

static void key_push(Vt100State *t, uint8_t ch) {
    int next = (t->key_head + 1) & (KEY_BUF_SIZE - 1);
    if (next != t->key_tail) {
        t->key_buf[t->key_head] = ch;
        t->key_head = next;
    }
}

static void key_push_str(Vt100State *t, const char *s) {
    while (*s) key_push(t, (uint8_t)*s++);
}

static void fwd_push(Vt100State *t, uint8_t ch) {
    int next = (t->fwd_head + 1) & (KEY_BUF_SIZE - 1);
    if (next != t->fwd_tail) {
        t->fwd_buf[t->fwd_head] = ch;
        t->fwd_head = next;
    }
}

/* ── Screen helpers ──────────────────────────────────────────────────────── */

static void clear_line_range(Vt100State *t, int line, int c0, int c1) {
    for (int c = c0; c <= c1 && c < VT100_COLS; c++) {
        t->text   [line][c] = ' ';
        t->fcolor [line][c] = t->cur_fg;
        t->bcolor [line][c] = t->cur_bg;
        t->style  [line][c] = t->cur_style;
        t->charset[line][c] = CS_ASCII;
        t->dirty  [line][c] = true;
    }
}

static void clear_screen_range(Vt100State *t, int l0, int l1) {
    for (int l = l0; l <= l1 && l < VT100_LINES; l++)
        clear_line_range(t, l, 0, VT100_COLS - 1);
}

static void scroll_up_region(Vt100State *t) {
    int top = t->scroll_top, bot = t->scroll_bottom;
    for (int l = top; l < bot; l++) {
        memcpy(t->text   [l], t->text   [l+1], VT100_COLS);
        memcpy(t->fcolor [l], t->fcolor [l+1], VT100_COLS * sizeof(SDL_Color));
        memcpy(t->bcolor [l], t->bcolor [l+1], VT100_COLS * sizeof(SDL_Color));
        memcpy(t->style  [l], t->style  [l+1], VT100_COLS);
        memcpy(t->charset[l], t->charset[l+1], VT100_COLS * sizeof(VtCharset));
    }
    clear_line_range(t, bot, 0, VT100_COLS - 1);
    t->full_redraw = true;
}

static void scroll_down_region(Vt100State *t) {
    int top = t->scroll_top, bot = t->scroll_bottom;
    for (int l = bot; l > top; l--) {
        memcpy(t->text   [l], t->text   [l-1], VT100_COLS);
        memcpy(t->fcolor [l], t->fcolor [l-1], VT100_COLS * sizeof(SDL_Color));
        memcpy(t->bcolor [l], t->bcolor [l-1], VT100_COLS * sizeof(SDL_Color));
        memcpy(t->style  [l], t->style  [l-1], VT100_COLS);
        memcpy(t->charset[l], t->charset[l-1], VT100_COLS * sizeof(VtCharset));
    }
    clear_line_range(t, top, 0, VT100_COLS - 1);
    t->full_redraw = true;
}

/* ── Cursor movement helpers ─────────────────────────────────────────────── */

static void clamp_cursor(Vt100State *t) {
    if (t->cur_line < 0) t->cur_line = 0;
    if (t->cur_line >= VT100_LINES) t->cur_line = VT100_LINES - 1;
    if (t->cur_col  < 0) t->cur_col  = 0;
    if (t->cur_col  >= VT100_COLS)  t->cur_col  = VT100_COLS  - 1;
}

static void cursor_home(Vt100State *t) {
    t->cur_line = t->dec_origin_mode ? t->scroll_top : 0;
    t->cur_col  = 0;
}

static void linefeed(Vt100State *t) {
    if (t->cur_line >= t->scroll_top && t->cur_line <= t->scroll_bottom) {
        if (t->cur_line == t->scroll_bottom)
            scroll_up_region(t);
        else
            t->cur_line++;
    } else if (t->cur_line < VT100_LINES - 1) {
        t->cur_line++;
    }
}

/* ── Erase helpers ───────────────────────────────────────────────────────── */

static void erase_to_eol(Vt100State *t) {
    clear_line_range(t, t->cur_line, t->cur_col, VT100_COLS - 1);
}

static void erase_to_bol(Vt100State *t) {
    clear_line_range(t, t->cur_line, 0, t->cur_col - 1);
}

static void erase_line(Vt100State *t) {
    clear_line_range(t, t->cur_line, 0, VT100_COLS - 1);
}

static void erase_to_eos(Vt100State *t) {
    erase_to_eol(t);
    clear_screen_range(t, t->cur_line + 1, t->scroll_bottom);
}

static void erase_to_bos(Vt100State *t) {
    erase_to_bol(t);
    clear_screen_range(t, t->scroll_top, t->cur_line - 1);
}

static void clear_screen(Vt100State *t) {
    clear_screen_range(t, 0, VT100_LINES - 1);
    t->cur_line = 0;
    t->cur_col  = 0;
    t->full_redraw = true;
}

/* ── Insert/Delete helpers ───────────────────────────────────────────────── */

static void delete_char(Vt100State *t) {
    int l = t->cur_line, c = t->cur_col;
    for (int i = c; i < VT100_COLS - 1; i++) {
        t->text   [l][i] = t->text   [l][i+1];
        t->fcolor [l][i] = t->fcolor [l][i+1];
        t->bcolor [l][i] = t->bcolor [l][i+1];
        t->style  [l][i] = t->style  [l][i+1];
        t->charset[l][i] = t->charset[l][i+1];
        t->dirty  [l][i] = true;
    }
    t->text   [l][VT100_COLS-1] = ' ';
    t->fcolor [l][VT100_COLS-1] = t->cur_fg;
    t->bcolor [l][VT100_COLS-1] = t->cur_bg;
    t->style  [l][VT100_COLS-1] = 0;
    t->charset[l][VT100_COLS-1] = CS_ASCII;
    t->dirty  [l][VT100_COLS-1] = true;
}

static void insert_char(Vt100State *t) {
    int l = t->cur_line, c = t->cur_col;
    for (int i = VT100_COLS - 1; i > c; i--) {
        t->text   [l][i] = t->text   [l][i-1];
        t->fcolor [l][i] = t->fcolor [l][i-1];
        t->bcolor [l][i] = t->bcolor [l][i-1];
        t->style  [l][i] = t->style  [l][i-1];
        t->charset[l][i] = t->charset[l][i-1];
        t->dirty  [l][i] = true;
    }
    t->text   [l][c] = ' ';
    t->fcolor [l][c] = t->cur_fg;
    t->bcolor [l][c] = t->cur_bg;
    t->style  [l][c] = 0;
    t->charset[l][c] = CS_ASCII;
    t->dirty  [l][c] = true;
}

static void insert_line(Vt100State *t) {
    /* Shift lines from cursor down to scroll_bottom down by 1 */
    int bot = t->scroll_bottom;
    for (int l = bot; l > t->cur_line; l--) {
        memcpy(t->text   [l], t->text   [l-1], VT100_COLS);
        memcpy(t->fcolor [l], t->fcolor [l-1], VT100_COLS * sizeof(SDL_Color));
        memcpy(t->bcolor [l], t->bcolor [l-1], VT100_COLS * sizeof(SDL_Color));
        memcpy(t->style  [l], t->style  [l-1], VT100_COLS);
        memcpy(t->charset[l], t->charset[l-1], VT100_COLS * sizeof(VtCharset));
    }
    clear_line_range(t, t->cur_line, 0, VT100_COLS - 1);
    t->full_redraw = true;
}

static void delete_line(Vt100State *t) {
    /* Scroll from cursor to scroll_bottom up by 1 */
    int bot = t->scroll_bottom;
    for (int l = t->cur_line; l < bot; l++) {
        memcpy(t->text   [l], t->text   [l+1], VT100_COLS);
        memcpy(t->fcolor [l], t->fcolor [l+1], VT100_COLS * sizeof(SDL_Color));
        memcpy(t->bcolor [l], t->bcolor [l+1], VT100_COLS * sizeof(SDL_Color));
        memcpy(t->style  [l], t->style  [l+1], VT100_COLS);
        memcpy(t->charset[l], t->charset[l+1], VT100_COLS * sizeof(VtCharset));
    }
    clear_line_range(t, bot, 0, VT100_COLS - 1);
    t->full_redraw = true;
}

/* ── Escape parser ───────────────────────────────────────────────────────── */

static void back_to_normal(Vt100State *t) {
    t->state           = STATE_NORMAL;
    t->csi_param_count = 0;
    t->dec_private     = false;
    t->designating_g0  = false;
}

/* Apply SGR parameters (m command) */
static void apply_sgr(Vt100State *t) {
    for (int i = 0; i < t->csi_param_count; i++) {
        int p = t->csi_params[i];
        switch (p) {
        case 0:
            t->cur_fg    = t->def_fg;
            t->cur_bg    = t->def_bg;
            t->cur_style = 0;
            break;
        case 1:  t->cur_style |=  VT_BOLD;      break;
        case 4:  t->cur_style |=  VT_UNDERLINE;  break;
        case 5:  t->cur_style |=  VT_BLINK;      break;
        case 7:  t->cur_style |=  VT_REVERSE;    break;
        case 9:  t->cur_style |=  VT_STRIKE;     break;
        case 22: t->cur_style &= ~VT_BOLD;       break;
        case 24: t->cur_style &= ~VT_UNDERLINE;  break;
        case 25: t->cur_style &= ~VT_BLINK;      break;
        case 27: t->cur_style &= ~VT_REVERSE;    break;
        case 29: t->cur_style &= ~VT_STRIKE;     break;
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            t->cur_fg = ansi_colors[p - 30]; break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            t->cur_bg = ansi_colors[p - 40]; break;
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            t->cur_fg = ansi_colors[8 + (p - 90)]; break;
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            t->cur_bg = ansi_colors[8 + (p - 100)]; break;
        default: break;
        }
    }
}

/* Dispatch DSR response into key buffer */
static void push_dsr(Vt100State *t, const char *s) {
    key_push_str(t, s);
}

/* Process CSI final byte */
static void do_csi_final(Vt100State *t, uint8_t ch) {
    /* Ensure at least one parameter slot */
    if (t->csi_param_count == 0) {
        t->csi_params[0] = 0;
        t->csi_param_count = 1;
    }
    int p1 = t->csi_params[0];
    int p2 = (t->csi_param_count >= 2) ? t->csi_params[1] : 0;

    switch (ch) {
    case 'H': case 'f': {
        /* CUP / HVP */
        int row  = (p1 < 1 ? 1 : p1) - 1;
        int col  = (p2 < 1 ? 1 : p2) - 1;
        if (t->dec_origin_mode)
            row += t->scroll_top;
        t->cur_line = row; t->cur_col = col;
        clamp_cursor(t);
        break;
    }
    case 'A': {  /* CUU */
        int n = p1 < 1 ? 1 : p1;
        t->cur_line -= n;
        if (t->cur_line < 0) t->cur_line = 0;
        break;
    }
    case 'B': {  /* CUD */
        int n = p1 < 1 ? 1 : p1;
        t->cur_line += n;
        if (t->cur_line >= VT100_LINES) t->cur_line = VT100_LINES - 1;
        break;
    }
    case 'C': {  /* CUF */
        int n = p1 < 1 ? 1 : p1;
        t->cur_col += n;
        if (t->cur_col >= VT100_COLS) t->cur_col = VT100_COLS - 1;
        break;
    }
    case 'D': {  /* CUB */
        int n = p1 < 1 ? 1 : p1;
        t->cur_col -= n;
        if (t->cur_col < 0) t->cur_col = 0;
        break;
    }
    case 'J': {  /* ED */
        switch (p1) {
        case 0: erase_to_eos(t);                          break;
        case 1: erase_to_bos(t);                          break;
        case 2: clear_screen(t); cursor_home(t);          break;
        }
        break;
    }
    case 'K': {  /* EL */
        switch (p1) {
        case 0: erase_to_eol(t); break;
        case 1: erase_to_bol(t); break;
        case 2: erase_line(t);   break;
        }
        break;
    }
    case 'm': {  /* SGR */
        apply_sgr(t);
        break;
    }
    case 'r': {  /* DECSTBM */
        if (t->csi_param_count >= 2 && p1 > 0 && p2 > 0 && p1 < p2) {
            t->scroll_top    = p1 - 1;
            t->scroll_bottom = p2 - 1;
        } else {
            t->scroll_top    = 0;
            t->scroll_bottom = VT100_LINES - 1;
        }
        if (t->scroll_top    < 0)              t->scroll_top    = 0;
        if (t->scroll_bottom >= VT100_LINES)   t->scroll_bottom = VT100_LINES - 1;
        if (t->scroll_top    > t->scroll_bottom) {
            t->scroll_top    = 0;
            t->scroll_bottom = VT100_LINES - 1;
        }
        cursor_home(t);
        break;
    }
    case 'L': {  /* IL: insert n lines */
        int n = p1 < 1 ? 1 : p1;
        for (int i = 0; i < n; i++) insert_line(t);
        break;
    }
    case 'M': {  /* DL: delete n lines */
        int n = p1 < 1 ? 1 : p1;
        for (int i = 0; i < n; i++) delete_line(t);
        break;
    }
    case 'P': {  /* DCH: delete n chars */
        int n = p1 < 1 ? 1 : p1;
        for (int i = 0; i < n; i++) delete_char(t);
        break;
    }
    case '@': {  /* ICH: insert n blank chars */
        int n = p1 < 1 ? 1 : p1;
        for (int i = 0; i < n; i++) insert_char(t);
        break;
    }
    case 'n': {  /* DSR */
        if (!t->dec_private) {
            if (p1 == 5) {
                push_dsr(t, "\x1B[0n");
            } else if (p1 == 6) {
                char buf[32];
                int report_line = t->dec_origin_mode
                    ? (t->cur_line - t->scroll_top + 1)
                    : (t->cur_line + 1);
                snprintf(buf, sizeof(buf), "\x1B[%d;%dR", report_line, t->cur_col + 1);
                push_dsr(t, buf);
            }
        }
        break;
    }
    case 's': {  /* save cursor */
        t->saved_line = t->cur_line;
        t->saved_col  = t->cur_col;
        break;
    }
    case 'u': {  /* restore cursor */
        t->cur_line = t->saved_line;
        t->cur_col  = t->saved_col;
        clamp_cursor(t);
        break;
    }
    case 'c': {  /* DA: device attributes */
        push_dsr(t, "\x1B[?1;0c");
        break;
    }
    case 'h': {  /* DEC private mode set */
        if (t->dec_private) {
            switch (p1) {
            case  6: t->dec_origin_mode  = true;  break;
            case 25: t->cursor_visible   = true;  break;
            }
        }
        break;
    }
    case 'l': {  /* DEC private mode reset */
        if (t->dec_private) {
            switch (p1) {
            case  6: t->dec_origin_mode  = false; break;
            case 25: t->cursor_visible   = false; break;
            }
        }
        break;
    }
    default:
        break;
    }
    back_to_normal(t);
}

/* ── Main character processor ────────────────────────────────────────────── */

static void vt100_write(Vt100State *t, uint8_t ch) {
    if (!t) return;

    switch (t->state) {

    case STATE_NORMAL:
        if (ch > 0x1F && ch != 0x7F) {
            /* Printable character */
            const char *glyph = NULL;
            if (t->use_graphics && ch >= '_' && ch <= '~')
                glyph = graphics_map[(int)(ch - '_')];

            int l = t->cur_line, c = t->cur_col;
            t->text   [l][c] = (char)ch;
            t->fcolor [l][c] = t->cur_fg;
            t->bcolor [l][c] = t->cur_bg;
            t->style  [l][c] = t->cur_style;
            t->charset[l][c] = t->use_graphics ? CS_GRAPHICS : CS_ASCII;
            t->dirty  [l][c] = true;
            (void)glyph; /* charset stored; rendering uses graphics_map lookup */

            t->cur_col++;
            if (t->cur_col >= VT100_COLS) {
                t->cur_col = 0;
                linefeed(t);
            }
        } else {
            switch (ch) {
            case 0x01: cursor_home(t); break;
            case 0x04: /* Ctrl-D: cursor right */
                if (t->cur_col < VT100_COLS - 1) t->cur_col++;
                break;
            case 0x05: /* Ctrl-E: cursor up */
                if (t->cur_line > 0) t->cur_line--;
                break;
            case 0x08: /* BS */
                if (t->cur_col > 0) {
                    t->cur_col--;
                    t->dirty[t->cur_line][t->cur_col] = true;
                }
                break;
            case 0x09: /* HT (tab) */
                t->cur_col = 8 * ((t->cur_col / 8) + 1);
                if (t->cur_col >= VT100_COLS) t->cur_col = VT100_COLS - 1;
                break;
            case 0x0A: case 0x0B: case 0x0C: /* LF/VT/FF */
                if (ch == 0x0C) { clear_screen(t); } else { linefeed(t); }
                break;
            case 0x0D: /* CR */
                t->cur_col = 0;
                break;
            case 0x0E: /* SO: invoke G1 */
                t->invoking_g1  = true;
                t->use_graphics = (t->charset_g1 == CS_GRAPHICS);
                break;
            case 0x0F: /* SI: invoke G0 */
                t->invoking_g1  = false;
                t->use_graphics = (t->charset_g0 == CS_GRAPHICS);
                break;
            case 0x13: /* Ctrl-S: cursor left */
                if (t->cur_col > 0) t->cur_col--;
                break;
            case 0x16: /* Ctrl-V: erase to EOL */
                erase_to_eol(t);
                break;
            case 0x18: /* Ctrl-X: cursor down */
                if (t->cur_line < VT100_LINES - 1) t->cur_line++;
                break;
            case 0x1B: /* ESC */
                t->state           = STATE_ESCAPE;
                t->csi_param_count = 0;
                t->dec_private     = false;
                break;
            case 0x7F: /* DEL: treat as BS */
                if (t->cur_col > 0) {
                    t->cur_col--;
                    t->dirty[t->cur_line][t->cur_col] = true;
                }
                break;
            default: break;
            }
        }
        break;

    case STATE_ESCAPE:
        switch (ch) {
        case '[':
            t->state = STATE_CSI;
            break;
        case '(':
            t->designating_g0 = true;
            t->state = STATE_CHARSET_DESIGNATE;
            break;
        case ')':
            t->designating_g0 = false;
            t->state = STATE_CHARSET_DESIGNATE;
            break;
        case 'D': /* IND: cursor down with scroll */
            if (t->cur_line == t->scroll_bottom)
                scroll_up_region(t);
            else if (t->cur_line < VT100_LINES - 1)
                t->cur_line++;
            back_to_normal(t);
            break;
        case 'E': /* NEL: next line */
            if (t->cur_line == t->scroll_bottom)
                scroll_up_region(t);
            else if (t->cur_line < VT100_LINES - 1)
                t->cur_line++;
            t->cur_col = 0;
            back_to_normal(t);
            break;
        case 'M': /* RI: cursor up with scroll */
            if (t->cur_line == t->scroll_top)
                scroll_down_region(t);
            else if (t->cur_line > 0)
                t->cur_line--;
            back_to_normal(t);
            break;
        case 'c': /* RIS: full reset */
            /* Reset parser state and screen */
            back_to_normal(t);
            t->cur_fg    = t->def_fg;
            t->cur_bg    = t->def_bg;
            t->cur_style = 0;
            t->scroll_top    = 0;
            t->scroll_bottom = VT100_LINES - 1;
            t->dec_origin_mode = false;
            t->cursor_visible  = true;
            t->charset_g0 = CS_ASCII;
            t->charset_g1 = CS_ASCII;
            t->invoking_g1 = false;
            t->use_graphics = false;
            clear_screen(t);
            cursor_home(t);
            break;
        default:
            back_to_normal(t);
            break;
        }
        break;

    case STATE_CSI:
        if (ch >= '0' && ch <= '9') {
            t->csi_param_count = 1;
            t->csi_params[0]   = ch - '0';
            t->state = STATE_CSI_PARAM;
        } else if (ch == '?') {
            t->dec_private     = true;
            t->csi_param_count = 1;
            t->csi_params[0]   = 0;
            t->state = STATE_CSI_PARAM;
        } else {
            /* Dispatch with no params */
            switch (ch) {
            case 'H': case 'f': cursor_home(t);                   back_to_normal(t); break;
            case 'J': erase_to_eos(t);                            back_to_normal(t); break;
            case 'K': erase_to_eol(t);                            back_to_normal(t); break;
            case 'M': delete_line(t);                             back_to_normal(t); break;
            case 'c': push_dsr(t, "\x1B[?1;0c");                 back_to_normal(t); break;
            case 's': t->saved_line = t->cur_line;
                      t->saved_col  = t->cur_col;                 back_to_normal(t); break;
            case 'u': t->cur_line = t->saved_line;
                      t->cur_col  = t->saved_col;
                      clamp_cursor(t);                            back_to_normal(t); break;
            case 'm': /* SGR reset */
                t->cur_fg    = t->def_fg;
                t->cur_bg    = t->def_bg;
                t->cur_style = 0;                                 back_to_normal(t); break;
            case 'r': /* DECSTBM reset */
                t->scroll_top    = 0;
                t->scroll_bottom = VT100_LINES - 1;
                cursor_home(t);                                   back_to_normal(t); break;
            default:                                              back_to_normal(t); break;
            }
        }
        break;

    case STATE_CSI_PARAM:
        if (ch >= '0' && ch <= '9') {
            t->csi_params[t->csi_param_count - 1] =
                t->csi_params[t->csi_param_count - 1] * 10 + (ch - '0');
            if (t->csi_params[t->csi_param_count - 1] > 9999)
                back_to_normal(t);
            /* else stay in STATE_CSI_PARAM */
        } else if (ch == ';') {
            t->csi_param_count++;
            if (t->csi_param_count > 16) { back_to_normal(t); break; }
            t->csi_params[t->csi_param_count - 1] = 0;
            /* stay in STATE_CSI_PARAM */
        } else {
            /* Final byte */
            do_csi_final(t, ch);
        }
        break;

    case STATE_CHARSET_DESIGNATE:
        switch (ch) {
        case 'B': if (t->designating_g0) t->charset_g0 = CS_ASCII;    else t->charset_g1 = CS_ASCII;    break;
        case '0': if (t->designating_g0) t->charset_g0 = CS_GRAPHICS; else t->charset_g1 = CS_GRAPHICS; break;
        case 'A': if (t->designating_g0) t->charset_g0 = CS_UK;       else t->charset_g1 = CS_UK;       break;
        default:  break;
        }
        t->use_graphics = (t->invoking_g1 ? (t->charset_g1 == CS_GRAPHICS)
                                           : (t->charset_g0 == CS_GRAPHICS));
        back_to_normal(t);
        break;
    }
}

/* ── Rendering ───────────────────────────────────────────────────────────── */

static void render_cell(Vt100State *t, int line, int col) {
    int x = BORDER + col  * CELL_W;
    int y = BORDER + line * CELL_H;

    uint8_t st  = t->style  [line][col];
    SDL_Color fg = t->fcolor [line][col];
    SDL_Color bg = t->bcolor [line][col];

    if (st & VT_REVERSE) {
        SDL_Color tmp = fg; fg = bg; bg = tmp;
    }

    /* Blink-off: render blank */
    bool blink_off = (st & VT_BLINK) && !t->cursor_on;

    /* Background */
    SDL_SetRenderDrawColor(t->renderer, bg.r, bg.g, bg.b, 255);
    SDL_Rect rect = { x, y, CELL_W, CELL_H };
    SDL_RenderFillRect(t->renderer, &rect);

    char ch = t->text[line][col];
    if (ch == ' ' || ch == '\0' || blink_off) return;

    /* Determine output string */
    char single[2] = { ch, '\0' };
    const char *out = single;
    if (t->charset[line][col] == CS_GRAPHICS && ch >= '_' && ch <= '~')
        out = graphics_map[(int)(ch - '_')];

    /* Render glyph */
    SDL_Color render_fg = fg;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(t->font, out,
        (SDL_Color){render_fg.r, render_fg.g, render_fg.b, 255});
    if (surf) {
        SDL_Texture *tex = SDL_CreateTextureFromSurface(t->renderer, surf);
        if (tex) {
            SDL_Rect dst = { x, y + (CELL_H - surf->h) / 2, surf->w, surf->h };
            SDL_RenderCopy(t->renderer, tex, NULL, &dst);

            /* Bold: re-render shifted 1px */
            if (st & VT_BOLD) {
                dst.x++;
                SDL_RenderCopy(t->renderer, tex, NULL, &dst);
                dst.x--;
            }

            /* Underline */
            if (st & VT_UNDERLINE) {
                SDL_SetRenderDrawColor(t->renderer, fg.r, fg.g, fg.b, 255);
                int uy = y + t->font_ascent + 2;
                SDL_RenderDrawLine(t->renderer, x, uy, x + CELL_W - 1, uy);
            }

            /* Strikethrough */
            if (st & VT_STRIKE) {
                SDL_SetRenderDrawColor(t->renderer, fg.r, fg.g, fg.b, 255);
                int sy = y + CELL_H / 2;
                SDL_RenderDrawLine(t->renderer, x, sy, x + CELL_W - 1, sy);
            }

            SDL_DestroyTexture(tex);
        }
        SDL_FreeSurface(surf);
    }
}

static void render_cursor(Vt100State *t) {
    if (!t->cursor_visible || !t->cursor_on) return;
    int l = t->cur_line, c = t->cur_col;
    if (l < 0 || l >= VT100_LINES || c < 0 || c >= VT100_COLS) return;

    int x = BORDER + c * CELL_W;
    int y = BORDER + l * CELL_H;

    /* Draw a filled rectangle in inverse of current fg/bg */
    SDL_Color fg = t->fcolor[l][c];
    SDL_SetRenderDrawColor(t->renderer, fg.r, fg.g, fg.b, 200);
    SDL_Rect rect = { x, y + CELL_H - 3, CELL_W, 3 };
    SDL_RenderFillRect(t->renderer, &rect);
}

static void vt100_render(Vt100State *t) {
    bool needs_update = t->full_redraw;

    if (!needs_update) {
        for (int l = 0; l < VT100_LINES && !needs_update; l++)
            for (int c = 0; c < VT100_COLS; c++)
                if (t->dirty[l][c]) { needs_update = true; break; }
    }

    if (!needs_update) return;

    if (t->full_redraw) {
        SDL_Color bg = t->def_bg;
        SDL_SetRenderDrawColor(t->renderer, bg.r, bg.g, bg.b, 255);
        SDL_RenderClear(t->renderer);
    }

    for (int l = 0; l < VT100_LINES; l++) {
        for (int c = 0; c < VT100_COLS; c++) {
            if (t->full_redraw || t->dirty[l][c]) {
                render_cell(t, l, c);
                t->dirty[l][c] = false;
            }
        }
    }

    render_cursor(t);
    SDL_RenderPresent(t->renderer);
    t->full_redraw = false;
}

/* ── SDL event processing ────────────────────────────────────────────────── */

static void process_keydown(Vt100State *t, SDL_KeyboardEvent *ev) {
    SDL_Keycode sym  = ev->keysym.sym;
    SDL_Keymod  mod  = ev->keysym.mod;
    bool        ctrl = (mod & (KMOD_LCTRL | KMOD_RCTRL)) != 0;

    /* Arrow keys → VT100 cursor sequences  (also map to WordStar codes) */
    switch (sym) {
    case SDLK_UP:     key_push(t, 0x05); return;  /* Ctrl-E */
    case SDLK_DOWN:   key_push(t, 0x18); return;  /* Ctrl-X */
    case SDLK_RIGHT:  key_push(t, 0x04); return;  /* Ctrl-D */
    case SDLK_LEFT:   key_push(t, 0x13); return;  /* Ctrl-S */
    case SDLK_DELETE: key_push(t, 0x7F); return;
    case SDLK_F1:     key_push_str(t, "\x1BOP"); return;
    case SDLK_F2:     key_push_str(t, "\x1BOQ"); return;
    case SDLK_F3:     key_push_str(t, "\x1BOR"); return;
    case SDLK_F4:     key_push_str(t, "\x1BOS"); return;
    case SDLK_HOME:   key_push(t, 0x01); return;   /* Ctrl-A */
    case SDLK_END:    key_push_str(t, "\x1B[F"); return;
    case SDLK_PAGEUP: key_push_str(t, "\x1B[5~"); return;
    case SDLK_PAGEDOWN: key_push_str(t, "\x1B[6~"); return;
    case SDLK_INSERT: key_push_str(t, "\x1B[2~"); return;
    case SDLK_KP_ENTER: key_push(t, 0x0D); return;
    default: break;
    }

    if (ctrl) {
        /* Ctrl+letter → control code */
        if (sym >= SDLK_a && sym <= SDLK_z) {
            uint8_t code = (uint8_t)(sym - SDLK_a + 1);
            key_push(t, code);
            return;
        }
        if (sym == SDLK_LEFTBRACKET)  { key_push(t, 0x1B); return; }
        if (sym == SDLK_BACKSLASH)    { key_push(t, 0x1C); return; }
        if (sym == SDLK_RIGHTBRACKET) { key_push(t, 0x1D); return; }
        if (sym == SDLK_6)            { key_push(t, 0x1E); return; }
        if (sym == SDLK_MINUS)        { key_push(t, 0x1F); return; }
    }

    /* Escape key */
    if (sym == SDLK_ESCAPE) { key_push(t, 0x1B); return; }

    /* Backspace */
    if (sym == SDLK_BACKSPACE) { key_push(t, 0x08); return; }

    /* Tab */
    if (sym == SDLK_TAB) { key_push(t, 0x09); return; }

    /* Enter */
    if (sym == SDLK_RETURN) { key_push(t, 0x0D); return; }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

static void vt100_poll(Vt100State *t) {
    if (!t) return;

    /*
     * Batch-collect all pending SDL events, handle ours, push the rest back
     * so that gemu_display_poll (called after us) still sees KIM-1 events.
     * Using SDL_PeepEvents avoids the re-queue infinite-loop that would occur
     * if we pushed events back while still inside SDL_PollEvent.
     */
    SDL_PumpEvents();
    SDL_Event events[512];
    int n = SDL_PeepEvents(events, 512, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);

    for (int i = 0; i < n; i++) {
        SDL_Event *ev = &events[i];
        switch (ev->type) {
        case SDL_QUIT:
            t->should_quit = true;
            SDL_PushEvent(ev);   /* let gemu_display_poll quit too */
            break;
        case SDL_WINDOWEVENT:
            if (ev->window.windowID == t->window_id) {
                if (ev->window.event == SDL_WINDOWEVENT_CLOSE)
                    t->should_quit = true;
                if (ev->window.event == SDL_WINDOWEVENT_EXPOSED)
                    t->full_redraw = true;
            } else {
                SDL_PushEvent(ev);
            }
            break;
        case SDL_KEYDOWN:
            if (ev->key.windowID == t->window_id)
                process_keydown(t, &ev->key);
            else
                SDL_PushEvent(ev);
            break;
        case SDL_KEYUP:
            if (ev->key.windowID != t->window_id)
                SDL_PushEvent(ev);
            break;
        case SDL_TEXTINPUT:
            if (ev->text.windowID == t->window_id) {
                for (int j = 0; ev->text.text[j]; j++) {
                    unsigned char c = (unsigned char)ev->text.text[j];
                    if (c >= 0x20 && c < 0x7F)
                        key_push(t, c);
                }
            } else {
                /* SDL_PushEvent(SDL_TEXTINPUT) crashes on SDL3-compat.
                 * Buffer the text for the machine to consume via pop_forwarded. */
                for (int j = 0; ev->text.text[j]; j++) {
                    unsigned char c = (unsigned char)ev->text.text[j];
                    if (c >= 0x20)
                        fwd_push(t, c);
                }
            }
            break;
        default:
            SDL_PushEvent(ev);   /* other events back for display handler */
            break;
        }
    }

    /* Cursor blink */
    uint32_t now = SDL_GetTicks();
    if ((now - t->last_blink_ms) >= BLINK_MS) {
        t->last_blink_ms = now;
        t->cursor_on     = !t->cursor_on;
        t->full_redraw   = true;
    }

    vt100_render(t);
}

static bool vt100_key_available(Vt100State *t) {
    if (!t) return false;
    return t->key_head != t->key_tail;
}

static uint8_t vt100_read_key(Vt100State *t) {
    if (!t || t->key_head == t->key_tail) return 0;
    uint8_t ch = t->key_buf[t->key_tail];
    t->key_tail = (t->key_tail + 1) & (KEY_BUF_SIZE - 1);
    return ch;
}

static bool vt100_should_quit(Vt100State *t) {
    return t ? t->should_quit : true;
}

static uint32_t vt100_pop_fwd_key(Vt100State *t) {
    if (!t || t->fwd_head == t->fwd_tail) return 0;
    uint8_t ch = t->fwd_buf[t->fwd_tail];
    t->fwd_tail = (t->fwd_tail + 1) & (KEY_BUF_SIZE - 1);
    return ch;
}

void vt100_destroy(Vt100State *t) {
    if (!t) return;
    if (t->font)     { TTF_CloseFont(t->font); t->font = NULL; }
    if (t->renderer) { SDL_DestroyRenderer(t->renderer); t->renderer = NULL; }
    if (t->window)   { SDL_DestroyWindow(t->window); t->window = NULL; }
    TTF_Quit();
    free(t);
}

Vt100State *vt100_create(GemuDisplayType dtype, const char *title) {
    /* In GTK mode the frame cadence is driven by GLib; SDL VSync would
     * fight GLib's timing and break the cursor blink accumulator. */
    if (!title) title = "GEMU (VT100 Terminal)";
    bool use_vsync = (dtype != GEMU_DISPLAY_GTK);

    build_graphics_map();

    /* Ensure SDL video subsystem is up */
    if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
            fprintf(stderr, "vt100: SDL_InitSubSystem(VIDEO) failed: %s\n", SDL_GetError());
            return NULL;
        }
    }

    if (TTF_Init() < 0) {
        fprintf(stderr, "vt100: TTF_Init failed: %s\n", TTF_GetError());
        return NULL;
    }

    Vt100State *t = calloc(1, sizeof(*t));
    if (!t) { TTF_Quit(); return NULL; }

    /* Load font from embedded data */
    SDL_RWops *rw = SDL_RWFromConstMem(
        vt100_font_ttf,
        (int)vt100_font_ttf_len);
    if (!rw) {
        fprintf(stderr, "vt100: SDL_RWFromConstMem failed\n");
        free(t); TTF_Quit(); return NULL;
    }

    t->font = TTF_OpenFontRW(rw, 1 /* freesrc */, FONT_SIZE_PT);
    if (!t->font) {
        fprintf(stderr, "vt100: TTF_OpenFontRW failed: %s\n", TTF_GetError());
        free(t); TTF_Quit(); return NULL;
    }
    t->font_ascent = TTF_FontAscent(t->font);

    /* Create window */
    t->window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        WIN_W, WIN_H,
        SDL_WINDOW_SHOWN);
    if (!t->window) {
        fprintf(stderr, "vt100: SDL_CreateWindow failed: %s\n", SDL_GetError());
        vt100_destroy(t); return NULL;
    }
    t->window_id = SDL_GetWindowID(t->window);

    /* Create renderer.
     * In GTK mode use software rendering: the accelerated renderer uses OpenGL,
     * and every SDL_RenderPresent triggers glXSwapBuffers which causes the
     * compositor to re-composite all windows - making the GTK menu bar flicker. */
    Uint32 rflags = (dtype == GEMU_DISPLAY_GTK)
                    ? SDL_RENDERER_SOFTWARE
                    : (SDL_RENDERER_ACCELERATED | (use_vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    t->renderer = SDL_CreateRenderer(t->window, -1, rflags);
    if (!t->renderer)
        t->renderer = SDL_CreateRenderer(t->window, -1, SDL_RENDERER_SOFTWARE);
    if (!t->renderer) {
        fprintf(stderr, "vt100: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        vt100_destroy(t); return NULL;
    }

    /* Initialise state */
    t->def_fg   = DEFAULT_FG;
    t->def_bg   = DEFAULT_BG;
    t->cur_fg   = t->def_fg;
    t->cur_bg   = t->def_bg;
    t->cur_style     = 0;
    t->cursor_visible = true;
    t->cursor_on      = true;
    t->scroll_top     = 0;
    t->scroll_bottom  = VT100_LINES - 1;
    t->charset_g0     = CS_ASCII;
    t->charset_g1     = CS_ASCII;
    t->state          = STATE_NORMAL;
    t->last_blink_ms  = SDL_GetTicks();
    t->full_redraw    = true;

    /* Fill screen with spaces */
    for (int l = 0; l < VT100_LINES; l++) {
        for (int c = 0; c < VT100_COLS; c++) {
            t->text   [l][c] = ' ';
            t->fcolor [l][c] = t->def_fg;
            t->bcolor [l][c] = t->def_bg;
            t->style  [l][c] = 0;
            t->charset[l][c] = CS_ASCII;
        }
    }

    /* Initial render */
    vt100_render(t);

    printf("vt100: terminal window created (%dx%d)\n", WIN_W, WIN_H);
    return t;
}

#else /* !HAVE_TTF - spawn xterm (or fall back to stdin/stdout) */

#ifndef _WIN32
#  if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#    include <util.h>
#  else
#    include <pty.h>
#  endif
#  include <termios.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/wait.h>
#else
#  include <conio.h>
#endif

#define TTY_KBUF 256

struct Vt100State {
    uint8_t kbuf[TTY_KBUF];
    int     khead, ktail;
    bool    quit;
#ifndef _WIN32
    int     master_fd;      /* pty master when xterm is running; -1 otherwise */
    pid_t   xterm_pid;
    /* stdin/stdout fallback state */
    struct termios saved_term;
    bool           raw;
#endif
};

static void tty_kpush(Vt100State *t, uint8_t ch)
{
    int n = (t->khead + 1) & (TTY_KBUF - 1);
    if (n != t->ktail) { t->kbuf[t->khead] = ch; t->khead = n; }
}

Vt100State *vt100_create(GemuDisplayType dtype, const char *title)
{
    (void)dtype; (void)title;
    Vt100State *t = calloc(1, sizeof(*t));
    if (!t) return NULL;

#ifndef _WIN32
    t->master_fd = -1;

    /* ── Try to spawn xterm via pty ── */
    int master = -1, slave = -1;
    char slave_name[64];
    if (openpty(&master, &slave, slave_name, NULL, NULL) == 0) {
        /*
         * Use a CLOEXEC pipe to detect exec failure: if execlp succeeds,
         * the write end closes automatically and we read 0 bytes; if it
         * fails the child writes a byte before _exit.
         */
        int ep[2];
        if (pipe(ep) == 0) {
            fcntl(ep[1], F_SETFD, FD_CLOEXEC);

            pid_t pid = fork();
            if (pid == 0) {
                close(ep[0]); close(master);
                /* xterm -S<pty_rel_path>/<slave_fd>
                 * e.g. -Spts/4/5 for /dev/pts/4 with slave fd 5 */
                char sflag[128];
                const char *rel = slave_name;
                if (strncmp(rel, "/dev/", 5) == 0) rel += 5;
                snprintf(sflag, sizeof(sflag), "-S%s/%d", rel, slave);
                // TODO: check users terminal emulator?
                execlp("xterm", "xterm",
                       "-title", "GEMU (VT100 Terminal)",
                       "-geometry", "80x24",
                       sflag, NULL);
                /* exec failed */
                char err = 1; (void)write(ep[1], &err, 1);
                _exit(1);
            }
            close(ep[1]);
            char err = 0; ssize_t nr = read(ep[0], &err, 1);
            close(ep[0]);
            close(slave);

            if (pid > 0 && nr == 0) {
                /* exec succeeded (CLOEXEC closed the pipe) */
                fcntl(master, F_SETFL,
                      fcntl(master, F_GETFL, 0) | O_NONBLOCK);
                t->master_fd = master;
                t->xterm_pid = pid;
                fprintf(stderr, "vt100: terminal window created (xterm)\n");
                return t;
            }
            if (pid > 0) waitpid(pid, NULL, 0);
        }
        close(master); close(slave);
    }

    /* ── Fallback: use stdin/stdout of current terminal ── */
    if (isatty(STDIN_FILENO)) {
        struct termios raw;
        tcgetattr(STDIN_FILENO, &t->saved_term);
        raw = t->saved_term;
        raw.c_lflag &= (tcflag_t)~(ECHO | ICANON);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
            t->raw = true;
    }
    fprintf(stderr, "vt100: xterm not found - using host terminal (stdin/stdout)\n");
#endif
    return t;
}

void vt100_destroy(Vt100State *t)
{
    if (!t) return;
#ifndef _WIN32
    if (t->master_fd >= 0) {
        close(t->master_fd);
        if (t->xterm_pid > 0) {
            kill(t->xterm_pid, SIGTERM);
            waitpid(t->xterm_pid, NULL, 0);
        }
    } else if (t->raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &t->saved_term);
    }
#endif
    free(t);
}

static void vt100_write(Vt100State *t, uint8_t ch)
{
#ifndef _WIN32
    if (t->master_fd >= 0) { (void)write(t->master_fd, &ch, 1); return; }
#endif
    fputc((int)ch, stdout); fflush(stdout);
}

static void vt100_poll(Vt100State *t)
{
#ifndef _WIN32
    int fd = (t->master_fd >= 0) ? t->master_fd : STDIN_FILENO;
    unsigned char buf[64]; ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        for (ssize_t i = 0; i < n; i++) tty_kpush(t, buf[i]);
#else
    while (_kbhit()) tty_kpush(t, (uint8_t)_getch());
#endif
}

static bool vt100_key_available(Vt100State *t) { return t->khead != t->ktail; }

static uint8_t vt100_read_key(Vt100State *t)
{
    if (!vt100_key_available(t)) return 0;
    uint8_t ch = t->kbuf[t->ktail];
    t->ktail = (t->ktail + 1) & (TTY_KBUF - 1);
    return ch;
}

static bool vt100_should_quit(Vt100State *t)
{
#ifndef _WIN32
    if (t->master_fd >= 0 && t->xterm_pid > 0) {
        if (waitpid(t->xterm_pid, NULL, WNOHANG) == t->xterm_pid) {
            t->xterm_pid = -1;
            t->quit = true;
        }
    }
#endif
    return t->quit;
}

static uint32_t vt100_pop_fwd_key(Vt100State *t) { (void)t; return 0; }

#endif /* HAVE_TTF */

/* ── GemuSerial adapter (SDL and TTY modes) ──────────────────────────────── */

static void     _ser_write(void *ud, uint8_t ch) { vt100_write(ud, ch); }
static uint8_t  _ser_read (void *ud)             { return vt100_read_key(ud); }
static bool     _ser_avail(void *ud)             { return vt100_key_available(ud); }
static void     _ser_poll (void *ud)             { vt100_poll(ud); }
static bool     _ser_quit (void *ud)             { return vt100_should_quit(ud); }
static uint32_t _ser_fwd  (void *ud)             { return vt100_pop_fwd_key(ud); }

void vt100_as_serial(Vt100State *t, GemuSerial *out) {
    *out = (GemuSerial){
        .ud             = t,
        .write_byte     = _ser_write,
        .read_byte      = _ser_read,
        .key_available  = _ser_avail,
        .poll           = _ser_poll,
        .should_quit    = _ser_quit,
        .pop_forwarded  = _ser_fwd,
    };
}
