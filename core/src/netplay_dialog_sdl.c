/*
 * Netplay > Connect... / Lobby..., drawn with SDL (see gemu/netplay_dialog.h):
 * its own window in the Windows 3.1 look and IBM 8x16 font of the window
 * menus, like the Game Genie window.
 *
 * Offline:                               Connected:
 *   Mode:       [ Kaillera ]               +- Games ----------------+ +- Users -+
 *   Name:       [__________]               | smb [e447318b]  1/2 ..| | alice   |
 *   Server:     [____________________]     +-----------------------+ +---------+
 *   Connection: [<] Good [>]               [Create Game] [Join] [Leave] [Start]  [Disconnect]
 *   Recent servers: (list)                 +- chat ------------------------------------+
 *   [ Connect ]               [ Close ]    +-------------------------------------------+
 *   (connection log)                       [ type here                        ] [Send]
 *
 * Keys: Tab moves between fields, Enter connects or sends, Esc closes,
 * Up/Down pick a game, PgUp/PgDn scroll the chat; double-click a game to
 * join it.  Events come from an SDL event watch, as in the Game Genie
 * window.
 */
#include "gemu/netplay_dialog.h"
#include "vgafont16.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define GLYPH_W   8
#define GLYPH_H   16
#define ROW_H     (GLYPH_H + 2)
#define WIN_W     (112 * GLYPH_W)
#define PAD       8
#define FIELD_H   (ROW_H + 4)
#define BTN_H     (GLYPH_H + 10)

#define GAME_ROWS 11
#define CHAT_ROWS 12
#define SERVER_ROWS 10
#define MAX_SERVERS 400
#define CLOG_ROWS 8
#define MAX_GAMES 512
#define MAX_USERS 512
#define MAX_LOG   300

#define C_BG      0xFFFFFF
#define C_FACE    0xC0C0C0
#define C_TEXT    0x000000
#define C_DIM     0x808080
#define C_LIGHT   0xFFFFFF
#define C_DARK    0x808080
#define C_HI      0x000080
#define C_HITEXT  0xFFFFFF
#define C_NOTE    0x000080
#define C_ERR     0x800000

enum { F_NAME, F_SERVER, F_CHAT, N_FIELDS };
enum { B_CONNECT, B_CLOSE, B_CONN_DOWN, B_CONN_UP, B_REFRESH,            /* connect page */
       B_CREATE, B_JOIN, B_LEAVE, B_START, B_DISCONNECT, B_SEND, N_BUTTONS };
#define FOCUS_GAMES (-1)

/* Lists with column headers: drag a header's edge to resize the column,
 * click a header to sort by it (again to reverse), like a Windows list
 * view.  The last column takes what's left. */
enum { T_GAMES, T_USERS, T_SERVERS, N_TABLES };
#define MAX_COLS  6
#define MIN_COL_W (4 * GLYPH_W)
typedef struct {
    const char *title;
    int         w;                /* pixels */
    bool        numeric, desc_first;
} TCol;
typedef struct {
    TCol     col[MAX_COLS];
    int      n, sort;
    bool     desc;
    int      top;                 /* first row shown */
    int      rows;                /* rows that fit */
    SDL_Rect rect;
} Table;

typedef struct {
    NetplayDialog base;
    Netplay      *np;
    uint32_t      seen_version;

    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *font;
    bool          visible, dirty, close_requested;
    Uint32        last_draw;
    int           shown_h;

    /* the session as last drawn */
    NetplayInfo   info;
    NetplayGame   games[MAX_GAMES];
    int           n_games;
    NetplayUser   users[MAX_USERS];
    int           n_users;
    char          log[MAX_LOG][NETPLAY_LINE_LEN];
    int           n_log;

    char          field[N_FIELDS][128];
    int           field_max[N_FIELDS];
    int           focus;
    int           conn;
    char          recent[8][128];
    int           n_recent;
    NetplayServer servers[8 + MAX_SERVERS];   /* the recent ones first, then the public lists */
    int           n_servers, n_recent_rows;
    char          sel_server[64];         /* address picked */
    bool          servers_busy, fetched;
    Uint32        last_ping;
    uint32_t      sel_game;               /* game id picked (0 = none) */
    int           chat_back;              /* chat lines scrolled back */
    Table         tab[N_TABLES];
    char          users_title[32];
    int           drag_t, drag_c, drag_x0, drag_w0;   /* a column edge being dragged (drag_t -1: none) */
    SDL_Cursor   *cur_size, *cur_arrow;
    bool          cur_sizing;
    Uint32        last_click;
    uint32_t      last_click_game;
    bool          text_seen;
    char          fallback_char;

    SDL_Rect      field_rect[N_FIELDS], button_rect[N_BUTTONS];
    SDL_Rect      games_rect, users_rect, chat_rect, servers_rect, clog_rect;
} SdlNetplayDialog;

static const char *button_label[N_BUTTONS] = {
    "Connect", "Close", "<", ">", "Refresh list", "Create Game", "Join", "Leave", "Start", "Disconnect", "Send",
};

static bool lobby(const SdlNetplayDialog *d) {
    return d->info.state != NETPLAY_OFFLINE && d->info.state != NETPLAY_CONNECTING;
}

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

/* Text, cut to max characters (0 = no limit). */
static void text(SdlNetplayDialog *d, int x, int y, const char *s, int max, uint32_t rgb) {
    SDL_SetTextureColorMod(d->font, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb);
    for (int i = 0; *s && (!max || i < max); s++, i++, x += GLYPH_W) {
        unsigned ch = (unsigned char)*s;
        SDL_Rect src = { (int)(ch % 16) * GLYPH_W, (int)(ch / 16) * GLYPH_H, GLYPH_W, GLYPH_H };
        SDL_RenderCopy(d->ren, d->font, &src, &(SDL_Rect){ x, y, GLYPH_W, GLYPH_H });
    }
}

static bool make_font(SdlNetplayDialog *d) {
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

/* Windows 3.1 push button; greyed out when it does nothing now. */
static void button(SdlNetplayDialog *d, SDL_Rect rc, const char *label, bool dflt, bool enabled) {
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
    text(d, rc.x + (rc.w - tw) / 2, rc.y + (rc.h - GLYPH_H) / 2, label, 0, enabled ? C_TEXT : C_DIM);
}

static void field_box(SdlNetplayDialog *d, int f, const char *label) {
    SDL_Rect R = d->field_rect[f];
    if (label) text(d, PAD, R.y + 3, label, 0, C_TEXT);
    fill(d->ren, R.x, R.y, R.w, R.h, C_BG);
    frame(d->ren, R, d->focus == f ? C_HI : C_TEXT);
    if (d->focus == f) frame(d->ren, (SDL_Rect){ R.x + 1, R.y + 1, R.w - 2, R.h - 2 }, C_HI);
    int cols = (R.w - 8) / GLYPH_W, len = (int)strlen(d->field[f]);
    const char *s = d->field[f] + (len > cols - 1 ? len - (cols - 1) : 0);   /* show the end */
    text(d, R.x + 4, R.y + 3, s, cols, C_TEXT);
    if (d->focus == f && (SDL_GetTicks() / 500) % 2 == 0)
        fill(d->ren, R.x + 4 + (int)strlen(s) * GLYPH_W, R.y + 3, 1, GLYPH_H, C_TEXT);
}

static void list_box(SdlNetplayDialog *d, SDL_Rect L, const char *header, bool focused) {
    fill(d->ren, L.x, L.y, L.w, L.h, C_BG);
    frame(d->ren, L, focused ? C_HI : C_TEXT);
    if (header) {
        fill(d->ren, L.x + 1, L.y + 1, L.w - 2, ROW_H, C_FACE);
        text(d, L.x + 6, L.y + 2, header, (L.w - 12) / GLYPH_W, C_TEXT);
        fill(d->ren, L.x + 1, L.y + ROW_H + 1, L.w - 2, 1, C_TEXT);
    }
}

static bool in_rect_xy(SDL_Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static uint32_t line_color(const char *s) {
    if (!strncmp(s, "***", 3)) return C_ERR;
    if (s[0] == '*') return C_NOTE;
    return C_TEXT;
}

/* Log lines, wrapped to cols, the last `rows` of them (minus back). */
static void draw_log(SdlNetplayDialog *d, SDL_Rect L, int rows, int back) {
    int cols = (L.w - 12) / GLYPH_W;
    static char wrapped[MAX_LOG * 3][NETPLAY_LINE_LEN];
    static uint32_t colors[MAX_LOG * 3];
    int n = 0;
    for (int i = 0; i < d->n_log && n < MAX_LOG * 3; i++) {
        const char *s = d->log[i];
        uint32_t c = line_color(s);
        int len = (int)strlen(s), o = 0;
        do {                                   /* continuation lines indent by 2 */
            int take = o ? cols - 2 : cols;
            snprintf(wrapped[n], sizeof(wrapped[n]), "%s%.*s", o ? "  " : "", take, s + o);
            colors[n++] = c;
            o += take;
        } while (o < len && n < MAX_LOG * 3);
    }
    int end = n - back;
    if (end < 0) end = 0;
    int start = end - rows < 0 ? 0 : end - rows;
    for (int i = start; i < end; i++)
        text(d, L.x + 6, L.y + 3 + (i - start) * ROW_H, wrapped[i], cols, colors[i]);
}

/* ── Tables ──────────────────────────────────────────────────────────────── */

static int col_x(const Table *t, int c) {
    int x = t->rect.x + 1;
    for (int i = 0; i < c; i++) x += t->col[i].w;
    return x;
}

static int col_w(const Table *t, int c) {
    if (c < t->n - 1) return t->col[c].w;
    int w = t->rect.x + t->rect.w - 1 - col_x(t, c);
    return w > 0 ? w : 0;
}

static int row_y(const Table *t, int row) { return t->rect.y + ROW_H + 3 + row * ROW_H; }

typedef void (*CellFn)(SdlNetplayDialog *d, int row, int col, char *buf, size_t len);
/* The row's colour; *hi when it's the picked one. */
typedef uint32_t (*RowFn)(SdlNetplayDialog *d, int row, bool *hi);

static void draw_table(SdlNetplayDialog *d, Table *t, int n_rows, CellFn cell, RowFn color,
                       const char *empty, bool focused) {
    SDL_Renderer *r = d->ren;
    SDL_Rect L = t->rect;
    int right = L.x + L.w - 1;
    fill(r, L.x, L.y, L.w, L.h, C_BG);
    frame(r, L, focused ? C_HI : C_TEXT);
    fill(r, L.x + 1, L.y + 1, L.w - 2, ROW_H, C_FACE);
    for (int c = 0; c < t->n; c++) {
        int x = col_x(t, c), w = col_w(t, c);
        if (x >= right) break;
        if (w > right - x) w = right - x;
        char title[64];
        snprintf(title, sizeof(title), "%s%s", t->col[c].title,
                 c == t->sort ? (t->desc ? " \x1F" : " \x1E") : "");   /* the font's down / up triangles */
        text(d, x + 5, L.y + 2, title, (w - 8) / GLYPH_W, C_TEXT);
        if (c < t->n - 1) {                       /* the edge you drag */
            fill(r, x + w - 2, L.y + 1, 1, ROW_H, C_DARK);
            fill(r, x + w - 1, L.y + 1, 1, ROW_H, C_LIGHT);
        }
    }
    fill(r, L.x + 1, L.y + ROW_H + 1, L.w - 2, 1, C_TEXT);
    t->rows = (L.h - ROW_H - 5) / ROW_H;
    if (t->top > n_rows - t->rows) t->top = n_rows - t->rows;
    if (t->top < 0) t->top = 0;
    if (!n_rows && empty) text(d, L.x + 6, row_y(t, 0) + 1, empty, (L.w - 12) / GLYPH_W, C_DIM);
    for (int row = 0; row < t->rows && t->top + row < n_rows; row++) {
        int y = row_y(t, row);
        bool hi = false;
        uint32_t fg = color(d, t->top + row, &hi);
        if (hi) { fill(r, L.x + 1, y, L.w - 2, ROW_H, C_HI); fg = C_HITEXT; }
        for (int c = 0; c < t->n; c++) {
            int x = col_x(t, c), w = col_w(t, c);
            if (x >= right) break;
            if (w > right - x) w = right - x;
            char buf[160];
            cell(d, t->top + row, c, buf, sizeof(buf));
            text(d, x + 5, y + 1, buf, (w - 8) / GLYPH_W, fg);
        }
    }
}

/* The header column at x (-1: none); *edge is the column whose right edge
 * is under x (-1: none). */
static int header_hit(const Table *t, int x, int y, int *edge) {
    *edge = -1;
    if (!in_rect_xy(t->rect, x, y) || y > t->rect.y + ROW_H + 1) return -1;
    for (int c = 0; c < t->n - 1; c++) {
        int e = col_x(t, c) + t->col[c].w;
        if (x >= e - 4 && x <= e + 3) { *edge = c; return c; }
    }
    for (int c = t->n - 1; c >= 0; c--)
        if (x >= col_x(t, c)) return c;
    return -1;
}

/* The row under y in t's list (-1: none). */
static int row_hit(const Table *t, int x, int y) {
    if (!in_rect_xy(t->rect, x, y) || y < row_y(t, 0)) return -1;
    int row = (y - row_y(t, 0)) / ROW_H;
    return row < t->rows ? t->top + row : -1;
}

static void layout(SdlNetplayDialog *d) {
    int bw = 13 * GLYPH_W;
    if (!lobby(d)) {
        int lx = PAD + 13 * GLYPH_W, y = PAD + FIELD_H + 8;   /* row 0: the mode */
        d->field_rect[F_NAME]   = (SDL_Rect){ lx, y, 26 * GLYPH_W + 8, FIELD_H };
        y += FIELD_H + 8;
        d->field_rect[F_SERVER] = (SDL_Rect){ lx, y, 52 * GLYPH_W + 8, FIELD_H };
        y += FIELD_H + 8;
        d->button_rect[B_CONN_DOWN] = (SDL_Rect){ lx, y, 3 * GLYPH_W, FIELD_H };
        d->button_rect[B_CONN_UP]   = (SDL_Rect){ lx + 3 * GLYPH_W + 30 * GLYPH_W, y, 3 * GLYPH_W, FIELD_H };
        y += FIELD_H + 12;
        d->servers_rect = (SDL_Rect){ PAD, y, WIN_W - 2 * PAD, ROW_H * (SERVER_ROWS + 1) + 4 };
        y += d->servers_rect.h + 12;
        d->button_rect[B_REFRESH] = (SDL_Rect){ lx + bw + 8, y, bw, BTN_H };
        d->button_rect[B_CONNECT] = (SDL_Rect){ lx, y, bw, BTN_H };
        d->button_rect[B_CLOSE]   = (SDL_Rect){ WIN_W - PAD - bw, y, bw, BTN_H };
        y += BTN_H + 12;
        d->clog_rect = (SDL_Rect){ PAD, y, WIN_W - 2 * PAD, ROW_H * CLOG_ROWS + 6 };
        d->tab[T_SERVERS].rect = d->servers_rect;
    } else {
        int gw = 78 * GLYPH_W;
        d->games_rect = (SDL_Rect){ PAD, PAD, gw, ROW_H * (GAME_ROWS + 1) + 4 };
        d->users_rect = (SDL_Rect){ PAD + gw + 8, PAD, WIN_W - 2 * PAD - gw - 8, d->games_rect.h };
        int y = PAD + d->games_rect.h + 10;
        for (int b = B_CREATE; b <= B_START; b++)
            d->button_rect[b] = (SDL_Rect){ PAD + (b - B_CREATE) * (bw + 8), y, bw, BTN_H };
        d->button_rect[B_DISCONNECT] = (SDL_Rect){ WIN_W - PAD - bw, y, bw, BTN_H };
        y += BTN_H + 10;
        d->chat_rect = (SDL_Rect){ PAD, y, WIN_W - 2 * PAD, ROW_H * CHAT_ROWS + 6 };
        y += d->chat_rect.h + 8;
        d->field_rect[F_CHAT] = (SDL_Rect){ PAD, y, WIN_W - 3 * PAD - 8 * GLYPH_W, FIELD_H };
        d->button_rect[B_SEND] = (SDL_Rect){ WIN_W - PAD - 8 * GLYPH_W, y, 8 * GLYPH_W, FIELD_H };
        d->tab[T_GAMES].rect = d->games_rect;
        d->tab[T_USERS].rect = d->users_rect;
    }
}

static int win_h(SdlNetplayDialog *d) {
    layout(d);
    int bottom = lobby(d) ? d->field_rect[F_CHAT].y + FIELD_H
                          : d->clog_rect.y + d->clog_rect.h;
    return bottom + 10 + GLYPH_H + 8;
}

static const char *status_text(SdlNetplayDialog *d, char *buf, size_t len) {
    const NetplayInfo *in = &d->info;
    switch (in->state) {
    case NETPLAY_OFFLINE:    snprintf(buf, len, "Not connected.  This game: %s", netplay_game_name(d->np)); break;
    case NETPLAY_CONNECTING: snprintf(buf, len, "Connecting to %s...", in->server); break;
    case NETPLAY_LOBBY:      snprintf(buf, len, "%s on %s - pick a game and Join, or Create Game", in->user, in->server); break;
    case NETPLAY_ROOM:       snprintf(buf, len, "In game %u%s - %s", in->game_id, in->owner ? " (yours)" : "",
                                      in->owner ? "press Start when everyone's in" : "waiting for the owner to start"); break;
    case NETPLAY_STARTING:   snprintf(buf, len, "Starting - waiting for every player to be ready..."); break;
    case NETPLAY_PLAYING:    snprintf(buf, len, "Playing: player %d of %d, delay %d frames, frame %llu",
                                      in->player, in->players, in->delay, (unsigned long long)in->frame); break;
    }
    return buf;
}

static bool button_enabled(SdlNetplayDialog *d, int b) {
    NetplayState st = d->info.state;
    switch (b) {
    case B_CONNECT:    return st == NETPLAY_OFFLINE;
    case B_REFRESH:    return !d->servers_busy;
    case B_CREATE:     return st == NETPLAY_LOBBY;
    case B_JOIN:       return st == NETPLAY_LOBBY && d->sel_game;
    case B_LEAVE:      return st >= NETPLAY_ROOM;
    case B_START:      return st == NETPLAY_ROOM && d->info.owner;
    default:           return true;
    }
}

static void server_cell(SdlNetplayDialog *d, int row, int col, char *buf, size_t len) {
    const NetplayServer *sv = &d->servers[row];
    bool listed = sv->users[0] != 0;
    switch (col) {
    case 0: snprintf(buf, len, "%s", sv->name); break;
    case 1: snprintf(buf, len, "%s", sv->address); break;
    case 2: snprintf(buf, len, "%s", sv->users); break;
    case 3: if (listed) snprintf(buf, len, "%d", sv->games); else buf[0] = 0; break;
    case 4:
        if (sv->ping >= 0) snprintf(buf, len, "%dms", sv->ping);
        else snprintf(buf, len, "%s", listed ? "-" : "");
        break;
    default: snprintf(buf, len, "%s", sv->location); break;
    }
}

static uint32_t server_color(SdlNetplayDialog *d, int row, bool *hi) {
    *hi = d->sel_server[0] && !strcmp(d->servers[row].address, d->sel_server);
    return row < d->n_recent_rows ? C_NOTE : C_TEXT;
}

static const char *const game_status[] = { "waiting", "playing", "syncing" };

static void game_cell(SdlNetplayDialog *d, int row, int col, char *buf, size_t len) {
    const NetplayGame *g = &d->games[row];
    switch (col) {
    case 0: snprintf(buf, len, "%s", g->name); break;
    case 1: snprintf(buf, len, "%s", g->emulator); break;
    case 2: snprintf(buf, len, "%s", g->players); break;
    case 3: snprintf(buf, len, "%s", g->status < 3 ? game_status[g->status] : "?"); break;
    default: snprintf(buf, len, "%s", g->owner); break;
    }
}

static uint32_t game_color(SdlNetplayDialog *d, int row, bool *hi) {
    const NetplayGame *g = &d->games[row];
    *hi = g->id == d->sel_game;
    if (d->info.state >= NETPLAY_ROOM && g->id == d->info.game_id) return C_NOTE;
    return !strcmp(g->name, netplay_game_name(d->np)) ? C_TEXT : C_DIM;   /* games you can't join: grey */
}

static void user_cell(SdlNetplayDialog *d, int row, int col, char *buf, size_t len) {
    const NetplayUser *u = &d->users[row];
    if (col == 0 && d->info.state >= NETPLAY_ROOM) snprintf(buf, len, "P%d %s", row + 1, u->name);
    else if (col == 0) snprintf(buf, len, "%s", u->name);
    else snprintf(buf, len, "%ums", u->ping);
}

static uint32_t user_color(SdlNetplayDialog *d, int row, bool *hi) {
    (void)d; (void)row;
    *hi = false;
    return C_TEXT;
}

static void draw_connect(SdlNetplayDialog *d) {
    SDL_Renderer *r = d->ren;
    (void)r;
    text(d, PAD, PAD + 3, "Mode:", 0, C_TEXT);
    SDL_Rect mode = { d->field_rect[F_NAME].x, PAD, 26 * GLYPH_W + 8, FIELD_H };
    fill(r, mode.x, mode.y, mode.w, mode.h, C_FACE);
    frame(r, mode, C_TEXT);
    text(d, mode.x + 4, mode.y + 3, netplay_mode_name(NETPLAY_MODE_KAILLERA), 0, C_TEXT);
    field_box(d, F_NAME, "Name:");
    field_box(d, F_SERVER, "Server:");
    text(d, d->field_rect[F_SERVER].x + d->field_rect[F_SERVER].w + 8, d->field_rect[F_SERVER].y + 3,
         "host[:port]", 0, C_DIM);
    SDL_Rect cd = d->button_rect[B_CONN_DOWN];
    text(d, PAD, cd.y + 3, "Connection:", 0, C_TEXT);
    button(d, cd, "<", false, d->conn > NETPLAY_CONN_MIN);
    button(d, d->button_rect[B_CONN_UP], ">", false, d->conn < NETPLAY_CONN_MAX);
    char conn[64];
    snprintf(conn, sizeof(conn), "%s (%d frame%s a packet)", netplay_conn_name(d->conn), d->conn,
             d->conn == 1 ? "" : "s");
    text(d, cd.x + cd.w + 8, cd.y + 3, conn, 0, C_TEXT);

    draw_table(d, &d->tab[T_SERVERS], d->n_servers, server_cell, server_color,
               d->servers_busy ? "(fetching the server lists...)" : "(no servers - type one above)", false);
    button(d, d->button_rect[B_REFRESH], "Refresh list", false, !d->servers_busy);
    button(d, d->button_rect[B_CONNECT], d->info.state == NETPLAY_CONNECTING ? "Connecting" : "Connect",
           true, button_enabled(d, B_CONNECT));
    button(d, d->button_rect[B_CLOSE], "Close", false, true);
    list_box(d, d->clog_rect, NULL, false);
    draw_log(d, d->clog_rect, CLOG_ROWS, 0);
}

static void draw_lobby(SdlNetplayDialog *d) {
    draw_table(d, &d->tab[T_GAMES], d->n_games, game_cell, game_color,
               "(no games yet)", d->focus == FOCUS_GAMES);
    snprintf(d->users_title, sizeof(d->users_title),
             d->info.state >= NETPLAY_ROOM ? "In this game (%d)" : "Users (%d)", d->n_users);
    draw_table(d, &d->tab[T_USERS], d->n_users, user_cell, user_color, NULL, false);

    for (int b = B_CREATE; b <= B_DISCONNECT; b++)
        button(d, d->button_rect[b], button_label[b], false, button_enabled(d, b));
    list_box(d, d->chat_rect, NULL, false);
    draw_log(d, d->chat_rect, CHAT_ROWS, d->chat_back);
    if (d->chat_back)
        text(d, d->chat_rect.x + d->chat_rect.w - 22 * GLYPH_W, d->chat_rect.y + 3,
             "(scrolled back - PgDn)", 0, C_DIM);
    field_box(d, F_CHAT, NULL);
    button(d, d->button_rect[B_SEND], "Send", d->focus == F_CHAT, true);
}

static void draw(SdlNetplayDialog *d) {
    int H = win_h(d);
    if (H != d->shown_h) {          /* the page changed: the window's height follows */
        d->shown_h = H;
        SDL_SetWindowSize(d->win, WIN_W, H);
        SDL_RenderSetLogicalSize(d->ren, WIN_W, H);
    }
    fill(d->ren, 0, 0, WIN_W, H, C_FACE);
    if (lobby(d)) draw_lobby(d);
    else draw_connect(d);
    int sy = H - GLYPH_H - 6;
    fill(d->ren, 0, sy - 4, WIN_W, 1, C_DARK);
    char status[200];
    text(d, PAD, sy, status_text(d, status, sizeof(status)), (WIN_W - 2 * PAD) / GLYPH_W, C_TEXT);
    SDL_RenderPresent(d->ren);
    d->dirty = false;
}

/* ── The session ─────────────────────────────────────────────────────────── */

/* Sorting, by the tables' columns (qsort has no context: one window). */
static const Table *sort_by;

static int num_before_slash(const char *s) { return atoi(s); }

static int dir(int c) { return sort_by->desc ? -c : c; }

static int cmp_games(const void *pa, const void *pb) {
    const NetplayGame *a = pa, *b = pb;
    int c = 0;
    switch (sort_by->sort) {
    case 0: c = strcasecmp(a->name, b->name); break;
    case 1: c = strcasecmp(a->emulator, b->emulator); break;
    case 2: c = num_before_slash(a->players) - num_before_slash(b->players); break;
    case 3: c = a->status - b->status; break;
    default: c = strcasecmp(a->owner, b->owner); break;
    }
    return c ? dir(c) : strcasecmp(a->name, b->name);
}

static int cmp_users(const void *pa, const void *pb) {
    const NetplayUser *a = pa, *b = pb;
    int c = sort_by->sort == 0 ? strcasecmp(a->name, b->name) : (int)a->ping - (int)b->ping;
    return c ? dir(c) : strcasecmp(a->name, b->name);
}

static int cmp_servers(const void *pa, const void *pb) {
    const NetplayServer *a = pa, *b = pb;
    int c = 0;
    switch (sort_by->sort) {
    case 0: c = strcasecmp(a->name, b->name); break;
    case 1: c = strcasecmp(a->address, b->address); break;
    case 2: c = num_before_slash(a->users) - num_before_slash(b->users); break;
    case 3: c = a->games - b->games; break;
    case 4:                                       /* no answer: last, either way */
        if ((a->ping < 0) != (b->ping < 0)) return a->ping < 0 ? 1 : -1;
        c = a->ping - b->ping;
        break;
    default: c = strcasecmp(a->location, b->location); break;
    }
    return c ? dir(c) : strcasecmp(a->name, b->name);
}

static void sort_lists(SdlNetplayDialog *d) {
    sort_by = &d->tab[T_GAMES];
    qsort(d->games, (size_t)d->n_games, sizeof(d->games[0]), cmp_games);
    if (d->info.state < NETPLAY_ROOM) {           /* in a game: player order */
        sort_by = &d->tab[T_USERS];
        qsort(d->users, (size_t)d->n_users, sizeof(d->users[0]), cmp_users);
    }
    sort_by = &d->tab[T_SERVERS];                 /* recent ones stay on top */
    qsort(d->servers + d->n_recent_rows, (size_t)(d->n_servers - d->n_recent_rows),
          sizeof(d->servers[0]), cmp_servers);
}

static void fetch(SdlNetplayDialog *d) {
    NetplayState was = d->info.state;
    netplay_info(d->np, &d->info);
    d->n_games = netplay_games(d->np, d->games, MAX_GAMES);
    d->n_users = d->info.state >= NETPLAY_ROOM ? netplay_room(d->np, d->users, NETPLAY_MAX_PLAYERS)
                                               : netplay_users(d->np, d->users, MAX_USERS);
    d->n_log = netplay_log(d->np, d->log, MAX_LOG);
    d->servers_busy = netplay_servers_busy(d->np);
    d->n_servers = 0;
    for (int i = 0; i < d->n_recent; i++) {
        NetplayServer *sv = &d->servers[d->n_servers++];
        memset(sv, 0, sizeof(*sv));
        snprintf(sv->name, sizeof(sv->name), "(recent)");
        snprintf(sv->address, sizeof(sv->address), "%.63s", d->recent[i]);
        sv->ping = -1;
    }
    d->n_recent_rows = d->n_servers;
    static NetplayServer pub[MAX_SERVERS];
    int np_n = netplay_servers(d->np, pub, MAX_SERVERS);
    for (int i = 0; i < np_n; i++) {
        bool dup = false;
        for (int j = 0; j < d->n_servers && !dup; j++) dup = !strcmp(d->servers[j].address, pub[i].address);
        if (dup) {   /* a recent one that's listed: show its details */
            for (int j = 0; j < d->n_servers; j++)
                if (!strcmp(d->servers[j].address, pub[i].address)) d->servers[j] = pub[i];
        } else {
            d->servers[d->n_servers++] = pub[i];
        }
    }
    sort_lists(d);
    bool found = false;
    for (int i = 0; i < d->n_games; i++) found |= d->games[i].id == d->sel_game;
    if (!found) d->sel_game = 0;
    if (lobby(d) != (was != NETPLAY_OFFLINE && was != NETPLAY_CONNECTING))
        d->focus = lobby(d) ? F_CHAT : F_SERVER;
}

static void press(SdlNetplayDialog *d, int b) {
    if (!button_enabled(d, b)) return;
    char err[160];
    switch (b) {
    case B_CONNECT:
        if (!netplay_connect(d->np, NETPLAY_MODE_KAILLERA, d->field[F_SERVER], d->field[F_NAME],
                             d->conn, err, sizeof(err)))
            netplay_note(d->np, "%s", err);
        break;
    case B_CLOSE:     d->base.hide(&d->base); break;
    case B_CONN_DOWN: if (d->conn > NETPLAY_CONN_MIN) d->conn--; break;
    case B_CONN_UP:   if (d->conn < NETPLAY_CONN_MAX) d->conn++; break;
    case B_REFRESH:   netplay_refresh_servers(d->np); break;
    case B_CREATE:    netplay_create_game(d->np); break;
    case B_JOIN:      netplay_join_game(d->np, d->sel_game); break;
    case B_LEAVE:     netplay_leave_game(d->np); break;
    case B_START:     netplay_start_game(d->np); break;
    case B_DISCONNECT: netplay_disconnect(d->np); break;
    case B_SEND:
        netplay_chat(d->np, d->field[F_CHAT]);
        d->field[F_CHAT][0] = '\0';
        d->chat_back = 0;
        break;
    }
    fetch(d);
}

/* ── Events ──────────────────────────────────────────────────────────────── */

static bool in_rect(SDL_Rect r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void type_char(SdlNetplayDialog *d, char ch) {
    if (d->focus < 0 || (unsigned char)ch < 0x20 || (unsigned char)ch > 0x7E) return;
    char *f = d->field[d->focus];
    size_t n = strlen(f);
    if ((int)n >= d->field_max[d->focus]) return;
    f[n] = ch;
    f[n + 1] = '\0';
}

static void pick_game(SdlNetplayDialog *d, int delta) {
    if (!d->n_games) return;
    int at = -1;
    for (int i = 0; i < d->n_games; i++) if (d->games[i].id == d->sel_game) at = i;
    at = at < 0 ? 0 : at + delta;
    if (at < 0) at = 0;
    if (at >= d->n_games) at = d->n_games - 1;
    d->sel_game = d->games[at].id;
    Table *t = &d->tab[T_GAMES];
    if (at < t->top) t->top = at;
    if (at >= t->top + t->rows) t->top = at - t->rows + 1;
}

/* A click on a header: start dragging its edge, or sort by it. */
static bool header_click(SdlNetplayDialog *d, int x, int y) {
    for (int t = 0; t < N_TABLES; t++) {
        bool shown = lobby(d) ? t != T_SERVERS : t == T_SERVERS;
        int edge, c = shown ? header_hit(&d->tab[t], x, y, &edge) : -1;
        if (c < 0) continue;
        Table *tb = &d->tab[t];
        if (edge >= 0) {
            d->drag_t = t;
            d->drag_c = edge;
            d->drag_x0 = x;
            d->drag_w0 = tb->col[edge].w;
        } else if (!(t == T_USERS && d->info.state >= NETPLAY_ROOM)) {
            if (tb->sort == c) tb->desc = !tb->desc;
            else { tb->sort = c; tb->desc = tb->col[c].desc_first; }
            fetch(d);
        }
        return true;
    }
    return false;
}

static void drag_to(SdlNetplayDialog *d, int x) {
    Table *t = &d->tab[d->drag_t];
    int w = d->drag_w0 + x - d->drag_x0;
    int others = 0;
    for (int c = 0; c < t->n - 1; c++) if (c != d->drag_c) others += t->col[c].w;
    int max = t->rect.w - 2 - others - MIN_COL_W;      /* the last column keeps a little */
    if (w > max) w = max;
    if (w < MIN_COL_W) w = MIN_COL_W;
    t->col[d->drag_c].w = w;
}

static void on_click(SdlNetplayDialog *d, int x, int y) {
    if (header_click(d, x, y)) return;
    for (int b = 0; b < N_BUTTONS; b++) {
        bool on_page = lobby(d) ? b >= B_CREATE : b < B_CREATE;
        if (on_page && in_rect(d->button_rect[b], x, y)) { press(d, b); return; }
    }
    if (!lobby(d)) {
        if (in_rect(d->field_rect[F_NAME], x, y)) d->focus = F_NAME;
        else if (in_rect(d->field_rect[F_SERVER], x, y)) d->focus = F_SERVER;
        else {
            int i = row_hit(&d->tab[T_SERVERS], x, y);
            if (i >= 0 && i < d->n_servers) {
                Uint32 now = SDL_GetTicks();
                bool dbl = !strcmp(d->sel_server, d->servers[i].address) && now - d->last_click < 400;
                d->last_click = now;
                snprintf(d->sel_server, sizeof(d->sel_server), "%s", d->servers[i].address);
                snprintf(d->field[F_SERVER], sizeof(d->field[F_SERVER]), "%s", d->servers[i].address);
                if (dbl) press(d, B_CONNECT);
            }
        }
        return;
    }
    if (in_rect(d->field_rect[F_CHAT], x, y)) { d->focus = F_CHAT; return; }
    if (in_rect(d->games_rect, x, y) && y >= row_y(&d->tab[T_GAMES], 0)) {
        d->focus = FOCUS_GAMES;
        int i = row_hit(&d->tab[T_GAMES], x, y);
        if (i < 0 || i >= d->n_games) { d->sel_game = 0; return; }
        uint32_t id = d->games[i].id;
        Uint32 now = SDL_GetTicks();
        bool dbl = id == d->last_click_game && now - d->last_click < 400;
        d->last_click = now;
        d->last_click_game = id;
        d->sel_game = id;
        if (dbl) press(d, B_JOIN);
    }
}

static void on_key(SdlNetplayDialog *d, const SDL_KeyboardEvent *k) {
    SDL_Keycode key = k->keysym.sym;
    bool shift = (k->keysym.mod & KMOD_SHIFT) != 0;
    switch (key) {
    case SDLK_ESCAPE: d->base.hide(&d->base); return;
    case SDLK_TAB:
        if (lobby(d)) d->focus = d->focus == F_CHAT ? FOCUS_GAMES : F_CHAT;
        else d->focus = d->focus == F_NAME ? F_SERVER : F_NAME;
        return;
    case SDLK_RETURN: case SDLK_KP_ENTER:
        if (!lobby(d)) press(d, B_CONNECT);
        else if (d->focus == F_CHAT) press(d, B_SEND);
        else press(d, B_JOIN);
        return;
    case SDLK_BACKSPACE:
        if (d->focus >= 0) {
            size_t n = strlen(d->field[d->focus]);
            if (n) d->field[d->focus][n - 1] = '\0';
        }
        return;
    case SDLK_PAGEUP:   d->chat_back += CHAT_ROWS - 1; return;
    case SDLK_PAGEDOWN: d->chat_back -= CHAT_ROWS - 1; if (d->chat_back < 0) d->chat_back = 0; return;
    case SDLK_UP:       if (lobby(d) && d->focus == FOCUS_GAMES) pick_game(d, -1); return;
    case SDLK_DOWN:     if (lobby(d) && d->focus == FOCUS_GAMES) pick_game(d, 1); return;
    default: break;
    }
    /* Characters normally arrive as SDL_TEXTINPUT.  Some desktops never
     * send it to a second window; until it shows up, type from keys. */
    if (d->focus >= 0 && !d->text_seen && key >= SDLK_SPACE && key <= SDLK_z &&
        !(k->keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))) {
        char ch = (char)key;
        if (shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        d->fallback_char = ch;
        type_char(d, ch);
    }
}

static int SDLCALL event_watch(void *ud, SDL_Event *ev) {
    SdlNetplayDialog *d = ud;
    Uint32 id = d->win ? SDL_GetWindowID(d->win) : 0, ev_id;
    switch (ev->type) {
    case SDL_WINDOWEVENT:     ev_id = ev->window.windowID; break;
    case SDL_KEYDOWN:         ev_id = ev->key.windowID;    break;
    case SDL_TEXTINPUT:       ev_id = ev->text.windowID;   break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:   ev_id = ev->button.windowID; break;
    case SDL_MOUSEMOTION:     ev_id = ev->motion.windowID; break;
    case SDL_MOUSEWHEEL:      ev_id = ev->wheel.windowID;  break;
    default:                  return 1;
    }
    if (!id || ev_id != id) return 1;
    switch (ev->type) {
    case SDL_WINDOWEVENT:
        /* Not hidden here: SDL counts visible windows right after this,
         * and with this one hidden it would think the last window closed. */
        if (ev->window.event == SDL_WINDOWEVENT_CLOSE) d->close_requested = true;
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) SDL_StartTextInput();
        if (ev->window.event == SDL_WINDOWEVENT_LEAVE && d->cur_sizing && d->cur_arrow) {
            SDL_SetCursor(d->cur_arrow);
            d->cur_sizing = false;
        }
        break;
    case SDL_KEYDOWN: on_key(d, &ev->key); break;
    case SDL_TEXTINPUT: {
        const char *p = ev->text.text;
        if (!d->text_seen && d->fallback_char && !p[1] &&
            tolower((unsigned char)p[0]) == tolower((unsigned char)d->fallback_char)) p++;
        d->text_seen = true;
        for (; *p; p++) type_char(d, *p);
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (ev->button.button == SDL_BUTTON_LEFT) on_click(d, ev->button.x, ev->button.y);
        break;
    case SDL_MOUSEBUTTONUP:
        if (ev->button.button == SDL_BUTTON_LEFT) d->drag_t = -1;
        break;
    case SDL_MOUSEMOTION: {
        int x = ev->motion.x, y = ev->motion.y;
        if (d->drag_t >= 0) {
            if (!(ev->motion.state & SDL_BUTTON_LMASK)) d->drag_t = -1;   /* released outside */
            else drag_to(d, x);
            break;
        }
        bool over_edge = false;
        for (int t = 0; t < N_TABLES && !over_edge; t++) {
            bool shown = lobby(d) ? t != T_SERVERS : t == T_SERVERS;
            int edge;
            if (shown && header_hit(&d->tab[t], x, y, &edge) >= 0 && edge >= 0) over_edge = true;
        }
        if (over_edge != d->cur_sizing && d->cur_size && d->cur_arrow) {
            SDL_SetCursor(over_edge ? d->cur_size : d->cur_arrow);
            d->cur_sizing = over_edge;
        }
        return 1;                       /* nothing to redraw */
    }
    case SDL_MOUSEWHEEL: {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        (void)mx;
        if (!lobby(d)) {
            d->tab[T_SERVERS].top -= ev->wheel.y * 3;
        } else if (my < d->games_rect.y + d->games_rect.h) {
            Table *t = in_rect_xy(d->users_rect, mx, my) ? &d->tab[T_USERS] : &d->tab[T_GAMES];
            t->top -= ev->wheel.y * 3;
        } else {
            d->chat_back += ev->wheel.y * 3;
            if (d->chat_back < 0) d->chat_back = 0;
        }
        break;
    }
    }
    d->dirty = true;
    return 1;
}

/* ── NetplayDialog ───────────────────────────────────────────────────────── */

static void sdl_show(NetplayDialog *base) {
    SdlNetplayDialog *d = (SdlNetplayDialog *)base;
    d->n_recent = netplay_recent_servers(d->recent, 8);
    fetch(d);
    if (!lobby(d) && !d->fetched) {     /* the public lists, once */
        d->fetched = true;
        netplay_refresh_servers(d->np);
        fetch(d);
    }
    d->focus = lobby(d) ? F_CHAT : (d->field[F_SERVER][0] ? F_SERVER : F_NAME);
    d->shown_h = 0;
    SDL_ShowWindow(d->win);
    SDL_SetWindowSize(d->win, WIN_W, win_h(d));
    SDL_RaiseWindow(d->win);
    SDL_StartTextInput();
    d->visible = d->dirty = true;
}

static void sdl_hide(NetplayDialog *base) {
    SdlNetplayDialog *d = (SdlNetplayDialog *)base;
    SDL_HideWindow(d->win);
    d->visible = false;
}

static bool sdl_visible(const NetplayDialog *base) {
    return ((const SdlNetplayDialog *)base)->visible;
}

static void sdl_refresh(NetplayDialog *base) {
    SdlNetplayDialog *d = (SdlNetplayDialog *)base;
    if (d->close_requested) {
        d->close_requested = false;
        sdl_hide(base);
    }
    if (!d->visible) return;
    uint32_t v = netplay_version(d->np);
    if (v != d->seen_version) {
        d->seen_version = v;
        fetch(d);
        d->dirty = true;
    }
    Uint32 now = SDL_GetTicks();
    /* the public servers' pings, every few seconds while they're on show */
    if (!lobby(d) && d->n_servers > d->n_recent_rows && now - d->last_ping > 5000) {
        d->last_ping = now;
        netplay_ping_servers(d->np);
    }
    if (!d->dirty && now - d->last_draw < 250) return;   /* caret blink */
    d->last_draw = now;
    draw(d);
}

static void sdl_destroy(NetplayDialog *base) {
    SdlNetplayDialog *d = (SdlNetplayDialog *)base;
    if (!d) return;
    SDL_DelEventWatch(event_watch, d);
    if (d->cur_sizing && d->cur_arrow) SDL_SetCursor(d->cur_arrow);
    if (d->cur_size) SDL_FreeCursor(d->cur_size);
    if (d->cur_arrow) SDL_FreeCursor(d->cur_arrow);
    if (d->font) SDL_DestroyTexture(d->font);
    if (d->ren) SDL_DestroyRenderer(d->ren);
    if (d->win) SDL_DestroyWindow(d->win);
    free(d);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

NetplayDialog *netplay_dialog_sdl_create(Netplay *np) {
    if (!np || SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL;
    SdlNetplayDialog *d = calloc(1, sizeof(*d));
    if (!d) { SDL_QuitSubSystem(SDL_INIT_VIDEO); return NULL; }
    d->base = (NetplayDialog){ sdl_show, sdl_hide, sdl_visible, sdl_refresh, sdl_destroy };
    d->np = np;
    d->drag_t = -1;
    static const TCol games[] = {
        { "Game", 29 * GLYPH_W, false, false }, { "Emulator", 19 * GLYPH_W, false, false },
        { "Players", 9 * GLYPH_W, true, true }, { "Status", 9 * GLYPH_W, false, false },
        { "Owner", 0, false, false },
    };
    static const TCol servers[] = {
        { "Server", 29 * GLYPH_W, false, false }, { "Address", 23 * GLYPH_W, false, false },
        { "Users", 9 * GLYPH_W, true, true }, { "Games", 7 * GLYPH_W, true, true },
        { "Ping", 8 * GLYPH_W, true, false }, { "Location", 0, false, false },
    };
    memcpy(d->tab[T_GAMES].col, games, sizeof(games));
    d->tab[T_GAMES].n = 5;
    d->tab[T_GAMES].sort = 2;                    /* most players first */
    d->tab[T_GAMES].desc = true;
    memcpy(d->tab[T_SERVERS].col, servers, sizeof(servers));
    d->tab[T_SERVERS].n = 6;
    d->tab[T_SERVERS].sort = 2;                  /* most users first */
    d->tab[T_SERVERS].desc = true;
    d->tab[T_USERS].col[0] = (TCol){ d->users_title, 21 * GLYPH_W, false, false };
    d->tab[T_USERS].col[1] = (TCol){ "Ping", 0, true, false };
    d->tab[T_USERS].n = 2;
    snprintf(d->users_title, sizeof(d->users_title), "Users");
    d->cur_size = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_SIZEWE);
    d->cur_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    d->field_max[F_NAME] = NETPLAY_NAME_LEN - 1;
    d->field_max[F_SERVER] = 120;
    d->field_max[F_CHAT] = 120;
    netplay_last_settings(d->field[F_NAME], sizeof(d->field[F_NAME]), &d->conn);
    d->n_recent = netplay_recent_servers(d->recent, 8);
    if (d->n_recent) snprintf(d->field[F_SERVER], sizeof(d->field[F_SERVER]), "%s", d->recent[0]);
    fetch(d);
    d->win = SDL_CreateWindow("GEMU - Netplay", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              WIN_W, win_h(d), SDL_WINDOW_HIDDEN);
    d->ren = d->win ? SDL_CreateRenderer(d->win, -1, 0) : NULL;
    if (!d->ren || !make_font(d)) {
        sdl_destroy(&d->base);
        return NULL;
    }
    SDL_RenderSetLogicalSize(d->ren, WIN_W, win_h(d));
    SDL_AddEventWatch(event_watch, d);
    return &d->base;
}
