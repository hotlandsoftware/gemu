/*
 * SDL window menu bar (see gemu_menubar_sdl.h): bar, drop-down menus with
 * submenus, an About box and a file browser, in the style of DOSBox-X's
 * built-in menus.  The menu contents come from gemu_display_build_menubar().
 */
#include "gemu_menubar_sdl.h"
#include "gemu_display_priv.h"
#include "gemu/menu.h"
#include "vgafont16.h"
#include <ctype.h>
#include <dirent.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#  include <unistd.h>
#else
#  include <direct.h>
#  define getcwd _getcwd
#endif

#define GLYPH_W   8
#define GLYPH_H   16
#define MAX_DEPTH 4                 /* bar menu + 3 submenu levels */
#define MAX_FILES 4096

/* Colours (0xRRGGBB) */
#define C_BAR_BG    0x1E2A40
#define C_BAR_FG    0xFFFFFF
#define C_BAR_HI    0x3C5A8C        /* open menu's title */
#define C_BAR_HOVER 0x34486C        /* title under the mouse */
#define C_MENU_BG   0xF4F4EE
#define C_MENU_FG   0x000000
#define C_MENU_DIS  0x9A9A9A
#define C_MENU_HI   0x22407A
#define C_MENU_HIFG 0xFFFFFF
#define C_BORDER    0x404048
#define C_SHADOW    0x000000
#define C_DLG_TITLE 0xD6D8E6
#define C_DLG_BG    0xFFFBF0
#define C_BTN_FACE  0xCACACA
#define C_BTN_LIGHT 0xF4F4F4
#define C_BTN_DARK  0x707070

typedef struct {
    GemuMenu *menu;
    int       sel;                  /* highlighted item, -1 = none */
    SDL_Rect  rect;                 /* where it was drawn */
} Level;

typedef enum { DLG_NONE, DLG_ABOUT, DLG_FILE } DialogType;

typedef struct { char name[256]; bool dir; } FileEntry;

struct GemuMenubar {
    GemuDisplay  *d;
    GemuMenu     *bar;              /* current model */
    Uint32        bar_built;        /* SDL_GetTicks() when built */
    int           open;             /* open bar menu, -1 = none */
    Level         levels[MAX_DEPTH];
    int           depth;            /* open levels (1 = just the drop-down) */
    SDL_Rect      title_rect[16];   /* bar titles, for hit testing */
    int           n_titles;
    int           hover;            /* title under the mouse, -1 = none */
    int           scale;            /* UI scale used for the last frame */
    int           out_w, out_h;

    SDL_Texture  *font;
    SDL_Renderer *font_owner;

    DialogType    dialog;
    char          dlg_title[128];
    char          dlg_text[1024];
    SDL_Rect      dlg_rect, btn_rect[2];
    /* file browser */
    char          dir[1024];
    FileEntry    *files;
    int           n_files, file_sel, file_top, file_rows;
    SDL_Rect      list_rect;
    Uint32        last_click;
    int           last_click_row;
    void        (*file_done)(void *ud, const char *path);
    void         *file_ud;
};

/* ── Drawing helpers ─────────────────────────────────────────────────────── */

static void set_color(SDL_Renderer *r, uint32_t rgb) {
    SDL_SetRenderDrawColor(r, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 255);
}

static void fill(SDL_Renderer *r, int x, int y, int w, int h, uint32_t rgb) {
    set_color(r, rgb);
    SDL_RenderFillRect(r, &(SDL_Rect){ x, y, w, h });
}

static void frame(SDL_Renderer *r, SDL_Rect rc, uint32_t rgb, int t) {
    fill(r, rc.x, rc.y, rc.w, t, rgb);
    fill(r, rc.x, rc.y + rc.h - t, rc.w, t, rgb);
    fill(r, rc.x, rc.y, t, rc.h, rgb);
    fill(r, rc.x + rc.w - t, rc.y, t, rc.h, rgb);
}

/* Font atlas: 16x16 glyphs of 8x16, white on transparent. */
static void ensure_font(GemuMenubar *mb, SDL_Renderer *r) {
    if (mb->font && mb->font_owner == r) return;
    if (mb->font) SDL_DestroyTexture(mb->font);
    mb->font = NULL;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 16 * GLYPH_W, 16 * GLYPH_H, 32,
                                                    SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    uint32_t *px = s->pixels;
    int pitch = s->pitch / 4;
    for (int ch = 0; ch < 256; ch++)
        for (int y = 0; y < GLYPH_H; y++)
            for (int x = 0; x < GLYPH_W; x++)
                px[((ch / 16) * GLYPH_H + y) * pitch + (ch % 16) * GLYPH_W + x] =
                    (vgafont16[ch * 16 + y] & (0x80 >> x)) ? 0xFFFFFFFFu : 0;
    mb->font = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    if (mb->font) {
        SDL_SetTextureBlendMode(mb->font, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(mb->font, SDL_ScaleModeNearest);
    }
    mb->font_owner = r;
}

static int text_w(const GemuMenubar *mb, const char *s) {
    return (int)strlen(s) * GLYPH_W * mb->scale;
}

static void text(GemuMenubar *mb, SDL_Renderer *r, int x, int y, const char *s, uint32_t rgb) {
    if (!mb->font) return;
    SDL_SetTextureColorMod(mb->font, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb);
    int sc = mb->scale;
    for (; *s; s++, x += GLYPH_W * sc) {
        unsigned ch = (unsigned char)*s;
        SDL_Rect src = { (int)(ch % 16) * GLYPH_W, (int)(ch / 16) * GLYPH_H, GLYPH_W, GLYPH_H };
        SDL_Rect dst = { x, y, GLYPH_W * sc, GLYPH_H * sc };
        SDL_RenderCopy(r, mb->font, &src, &dst);
    }
}

/* 3D button in the Windows 3.x style. */
static void button(GemuMenubar *mb, SDL_Renderer *r, SDL_Rect rc, const char *label, bool focus) {
    int t = mb->scale;
    fill(r, rc.x, rc.y, rc.w, rc.h, C_BTN_FACE);
    fill(r, rc.x, rc.y, rc.w, t, C_BTN_LIGHT);
    fill(r, rc.x, rc.y, t, rc.h, C_BTN_LIGHT);
    fill(r, rc.x, rc.y + rc.h - t, rc.w, t, C_BTN_DARK);
    fill(r, rc.x + rc.w - t, rc.y, t, rc.h, C_BTN_DARK);
    frame(r, (SDL_Rect){ rc.x - t, rc.y - t, rc.w + 2 * t, rc.h + 2 * t }, C_BORDER, t);
    if (focus) frame(r, (SDL_Rect){ rc.x + 3 * t, rc.y + 3 * t, rc.w - 6 * t, rc.h - 6 * t }, C_BTN_DARK, 1);
    text(mb, r, rc.x + (rc.w - text_w(mb, label)) / 2, rc.y + (rc.h - GLYPH_H * mb->scale) / 2,
         label, C_MENU_FG);
}

/* ── Model / state ───────────────────────────────────────────────────────── */

static void rebuild(GemuMenubar *mb) {
    gemu_menu_free(mb->bar);
    mb->bar = gemu_display_build_menubar(mb->d);
    mb->bar_built = SDL_GetTicks();
}

static void close_menus(GemuMenubar *mb) {
    mb->open = -1;
    mb->depth = 0;
}

static void open_menu(GemuMenubar *mb, int index, bool select_first) {
    if (!mb->bar || index < 0 || index >= mb->bar->n) return;
    if (mb->open < 0) rebuild(mb);          /* fresh checkmarks / drive names */
    mb->open = index;
    mb->depth = 1;
    mb->levels[0].menu = mb->bar->items[index].submenu;
    mb->levels[0].sel = -1;
    if (select_first && mb->levels[0].menu) {
        for (int i = 0; i < mb->levels[0].menu->n; i++)
            if (gemu_menu_item_enabled(&mb->levels[0].menu->items[i])) { mb->levels[0].sel = i; break; }
    }
}

GemuMenubar *gemu_menubar_create(GemuDisplay *d) {
    GemuMenubar *mb = calloc(1, sizeof(*mb));
    if (!mb) return NULL;
    mb->d = d;
    mb->open = -1;
    mb->scale = 1;
    mb->hover = -1;
    rebuild(mb);
    mb->bar_built = 0;      /* rebuild at the first frame: the monitor is attached later */
    return mb;
}

void gemu_menubar_destroy(GemuMenubar *mb) {
    if (!mb) return;
    gemu_menu_free(mb->bar);
    if (mb->font) SDL_DestroyTexture(mb->font);
    free(mb->files);
    free(mb);
}

static int ui_scale(int out_h) {
    return out_h >= 1100 ? 2 : 1;
}

int gemu_menubar_height(int out_h) {
    return (GLYPH_H + 4) * ui_scale(out_h);
}

bool gemu_menubar_active(const GemuMenubar *mb) {
    return mb && (mb->open >= 0 || mb->dialog != DLG_NONE);
}

void gemu_menubar_focus(GemuMenubar *mb) {
    if (mb && mb->dialog == DLG_NONE) open_menu(mb, 0, true);
}

/* ── Rendering ───────────────────────────────────────────────────────────── */

static int item_h(const GemuMenubar *mb, const GemuMenuItem *it) {
    return it->type == GEMU_MENU_SEPARATOR ? 6 * mb->scale : (GLYPH_H + 4) * mb->scale;
}

/* Lay out and draw one drop-down at (x, y). */
static void draw_level(GemuMenubar *mb, SDL_Renderer *r, Level *lv, int x, int y) {
    GemuMenu *m = lv->menu;
    int sc = mb->scale, pad = 8 * sc;
    int label_w = 0, short_w = 0, h = 2 * sc;
    for (int i = 0; i < m->n; i++) {
        int lw = text_w(mb, m->items[i].label), sw = text_w(mb, m->items[i].shortcut);
        if (m->items[i].type == GEMU_MENU_SUBMENU) sw = text_w(mb, ">");
        if (lw > label_w) label_w = lw;
        if (sw > short_w) short_w = sw;
        h += item_h(mb, &m->items[i]);
    }
    int w = pad + 2 * GLYPH_W * sc + label_w + (short_w ? 3 * GLYPH_W * sc + short_w : 0) + pad;
    if (x + w > mb->out_w) x = mb->out_w - w;
    if (y + h > mb->out_h) y = mb->out_h - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    lv->rect = (SDL_Rect){ x, y, w, h };

    fill(r, x + 3 * sc, y + 3 * sc, w, h, C_SHADOW);            /* drop shadow */
    fill(r, x, y, w, h, C_MENU_BG);
    frame(r, lv->rect, C_BORDER, sc);
    int iy = y + sc;
    for (int i = 0; i < m->n; i++) {
        const GemuMenuItem *it = &m->items[i];
        int ih = item_h(mb, it);
        if (it->type == GEMU_MENU_SEPARATOR) {
            fill(r, x + 4 * sc, iy + ih / 2, w - 8 * sc, sc, C_MENU_DIS);
        } else {
            bool en = gemu_menu_item_enabled(it), hi = i == lv->sel && en;
            if (hi) fill(r, x + sc, iy, w - 2 * sc, ih, C_MENU_HI);
            uint32_t fg = !en ? C_MENU_DIS : hi ? C_MENU_HIFG : C_MENU_FG;
            int ty = iy + 2 * sc;
            if (gemu_menu_item_checked(it))
                text(mb, r, x + pad, ty, it->type == GEMU_MENU_RADIO ? "\x07" : "\xFB", fg);
            text(mb, r, x + pad + 2 * GLYPH_W * sc, ty, it->label, fg);
            const char *right = it->type == GEMU_MENU_SUBMENU ? "\x10" : it->shortcut;
            if (right[0]) text(mb, r, x + w - pad - text_w(mb, right), ty, right, fg);
        }
        iy += ih;
    }
}

/* Top of item i in a level's rect. */
static int item_top(const GemuMenubar *mb, const Level *lv, int i) {
    int y = lv->rect.y + mb->scale;
    for (int k = 0; k < i; k++) y += item_h(mb, &lv->menu->items[k]);
    return y;
}

static void draw_dialog_frame(GemuMenubar *mb, SDL_Renderer *r, int w, int h) {
    int sc = mb->scale;
    int x = (mb->out_w - w) / 2, y = (mb->out_h - h) / 2;
    if (y < gemu_menubar_height(mb->out_h)) y = gemu_menubar_height(mb->out_h);
    mb->dlg_rect = (SDL_Rect){ x, y, w, h };
    fill(r, x + 4 * sc, y + 4 * sc, w, h, C_SHADOW);
    fill(r, x, y, w, h, C_DLG_BG);
    int th = (GLYPH_H + 6) * sc;
    fill(r, x, y, w, th, C_DLG_TITLE);
    fill(r, x, y + th, w, sc, C_BORDER);
    frame(r, mb->dlg_rect, C_BORDER, sc);
    /* the little close box, as in the screenshots this is modelled on */
    frame(r, (SDL_Rect){ x + 4 * sc, y + 4 * sc, th - 8 * sc, th - 8 * sc }, C_BORDER, sc);
    text(mb, r, x + (w - text_w(mb, mb->dlg_title)) / 2, y + 3 * sc, mb->dlg_title, C_MENU_FG);
}

static void draw_about(GemuMenubar *mb, SDL_Renderer *r) {
    int sc = mb->scale, lh = (GLYPH_H + 2) * sc;
    int lines = 1, longest = 0, cur = 0;
    for (const char *p = mb->dlg_text; ; p++) {
        if (*p == '\n' || !*p) { if (cur > longest) longest = cur; cur = 0; if (!*p) break; lines++; }
        else cur++;
    }
    int w = longest * GLYPH_W * sc + 48 * sc, h = (GLYPH_H + 6) * sc + 16 * sc + lines * lh + 48 * sc;
    if (w < 240 * sc) w = 240 * sc;
    draw_dialog_frame(mb, r, w, h);
    int x = mb->dlg_rect.x + 24 * sc, y = mb->dlg_rect.y + (GLYPH_H + 6) * sc + 16 * sc;
    char line[256];
    const char *p = mb->dlg_text;
    while (*p) {
        size_t n = strcspn(p, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)n, p);
        text(mb, r, x, y, line, C_MENU_FG);
        y += lh;
        p += n + (p[n] == '\n');
    }
    int bw = 10 * GLYPH_W * sc, bh = (GLYPH_H + 8) * sc;
    mb->btn_rect[0] = (SDL_Rect){ mb->dlg_rect.x + (w - bw) / 2, mb->dlg_rect.y + h - bh - 12 * sc, bw, bh };
    button(mb, r, mb->btn_rect[0], "Close", true);
}

static void draw_file(GemuMenubar *mb, SDL_Renderer *r) {
    int sc = mb->scale, lh = (GLYPH_H + 2) * sc;
    int w = 64 * GLYPH_W * sc, rows = 16;
    if (w > mb->out_w - 16 * sc) w = mb->out_w - 16 * sc;
    while (rows > 4 && (GLYPH_H + 6) * sc + lh * (rows + 4) + 40 * sc > mb->out_h - gemu_menubar_height(mb->out_h))
        rows--;
    mb->file_rows = rows;
    int h = (GLYPH_H + 6) * sc + 12 * sc + lh + 6 * sc + rows * lh + 8 * sc + (GLYPH_H + 8) * sc + 16 * sc;
    draw_dialog_frame(mb, r, w, h);
    int x = mb->dlg_rect.x + 12 * sc, y = mb->dlg_rect.y + (GLYPH_H + 6) * sc + 12 * sc;
    int cw = w - 24 * sc, maxc = cw / (GLYPH_W * sc);

    char path[1100];
    size_t dl = strlen(mb->dir);
    snprintf(path, sizeof(path), "%s", dl > (size_t)maxc ? mb->dir + dl - (size_t)maxc : mb->dir);
    text(mb, r, x, y, path, C_MENU_FG);
    y += lh + 6 * sc;

    mb->list_rect = (SDL_Rect){ x, y, cw, rows * lh };
    fill(r, x, y, cw, rows * lh, 0xFFFFFF);
    frame(r, (SDL_Rect){ x - sc, y - sc, cw + 2 * sc, rows * lh + 2 * sc }, C_BTN_DARK, sc);
    if (mb->file_sel < mb->file_top) mb->file_top = mb->file_sel;
    if (mb->file_sel >= mb->file_top + rows) mb->file_top = mb->file_sel - rows + 1;
    for (int i = 0; i < rows && mb->file_top + i < mb->n_files; i++) {
        const FileEntry *f = &mb->files[mb->file_top + i];
        bool hi = mb->file_top + i == mb->file_sel;
        if (hi) fill(r, x, y + i * lh, cw, lh, C_MENU_HI);
        char name[300];
        snprintf(name, sizeof(name), "%s%s", f->name, f->dir ? "/" : "");
        if ((int)strlen(name) > maxc - 1) name[maxc - 1] = '\0';
        text(mb, r, x + 4 * sc, y + i * lh + sc, name, hi ? C_MENU_HIFG : (f->dir ? 0x203080 : C_MENU_FG));
    }
    int bw = 10 * GLYPH_W * sc, bh = (GLYPH_H + 8) * sc, by = mb->dlg_rect.y + h - bh - 12 * sc;
    mb->btn_rect[0] = (SDL_Rect){ mb->dlg_rect.x + w - 2 * bw - 28 * sc, by, bw, bh };
    mb->btn_rect[1] = (SDL_Rect){ mb->dlg_rect.x + w - bw - 14 * sc, by, bw, bh };
    button(mb, r, mb->btn_rect[0], "Open", true);
    button(mb, r, mb->btn_rect[1], "Cancel", false);
}

void gemu_menubar_render(GemuMenubar *mb, SDL_Renderer *r) {
    if (!mb) return;
    SDL_GetRendererOutputSize(r, &mb->out_w, &mb->out_h);
    mb->scale = ui_scale(mb->out_h);
    ensure_font(mb, r);
    /* Rebuild now and then for the bar titles (e.g. a Blocks menu appears
     * once the machine registers drives); open menus are rebuilt on opening. */
    if (mb->open < 0 && (!mb->bar_built || SDL_GetTicks() - mb->bar_built > 1000)) rebuild(mb);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    int sc = mb->scale, bh = gemu_menubar_height(mb->out_h);
    fill(r, 0, 0, mb->out_w, bh, C_BAR_BG);
    int x = 4 * sc;
    mb->n_titles = 0;
    for (int i = 0; mb->bar && i < mb->bar->n && i < 16; i++) {
        const char *t = mb->bar->items[i].label;
        int w = text_w(mb, t) + 12 * sc;
        mb->title_rect[i] = (SDL_Rect){ x, 0, w, bh };
        if (i == mb->open)       fill(r, x, 0, w, bh, C_BAR_HI);
        else if (i == mb->hover) fill(r, x, 0, w, bh, C_BAR_HOVER);
        text(mb, r, x + 6 * sc, 2 * sc, t, C_BAR_FG);
        x += w;
        mb->n_titles++;
    }

    if (mb->open >= 0 && mb->open < mb->n_titles) {
        int lx = mb->title_rect[mb->open].x, ly = bh;
        for (int l = 0; l < mb->depth; l++) {
            draw_level(mb, r, &mb->levels[l], lx, ly);
            Level *lv = &mb->levels[l];
            if (l + 1 < mb->depth) {      /* submenu opens beside its item */
                lx = lv->rect.x + lv->rect.w - 2 * sc;
                ly = item_top(mb, lv, lv->sel) - sc;
            }
        }
    }

    if (mb->dialog != DLG_NONE) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 96);
        SDL_RenderFillRect(r, &(SDL_Rect){ 0, bh, mb->out_w, mb->out_h - bh });
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        if (mb->dialog == DLG_ABOUT) draw_about(mb, r);
        else draw_file(mb, r);
    }
}

/* ── File browser ────────────────────────────────────────────────────────── */

static int cmp_entry(const void *a, const void *b) {
    const FileEntry *x = a, *y = b;
    if (!strcmp(x->name, "..")) return -1;
    if (!strcmp(y->name, "..")) return 1;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void load_dir(GemuMenubar *mb) {
    mb->n_files = 0;
    mb->file_sel = mb->file_top = 0;
    if (!mb->files) mb->files = calloc(MAX_FILES, sizeof(*mb->files));
    if (!mb->files) return;
    DIR *dp = opendir(mb->dir);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp)) && mb->n_files < MAX_FILES) {
        if (!strcmp(e->d_name, ".") || (e->d_name[0] == '.' && strcmp(e->d_name, ".."))) continue;
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s", mb->dir, e->d_name);
        struct stat st;
        bool dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        FileEntry *f = &mb->files[mb->n_files++];
        snprintf(f->name, sizeof(f->name), "%s", e->d_name);
        f->dir = dir;
    }
    closedir(dp);
    qsort(mb->files, (size_t)mb->n_files, sizeof(*mb->files), cmp_entry);
}

static void file_enter(GemuMenubar *mb) {
    if (mb->file_sel < 0 || mb->file_sel >= mb->n_files) return;
    FileEntry *f = &mb->files[mb->file_sel];
    if (f->dir) {
        if (!strcmp(f->name, "..")) {
            char *slash = strrchr(mb->dir, '/');
            if (slash && slash != mb->dir) *slash = '\0';
            else snprintf(mb->dir, sizeof(mb->dir), "/");
        } else {
            size_t l = strlen(mb->dir);
            snprintf(mb->dir + l, sizeof(mb->dir) - l, "%s%s", l && mb->dir[l - 1] == '/' ? "" : "/", f->name);
        }
        load_dir(mb);
        return;
    }
    char path[1400];
    snprintf(path, sizeof(path), "%s/%s", mb->dir, f->name);
    mb->dialog = DLG_NONE;
    if (mb->file_done) mb->file_done(mb->file_ud, path);
}

void gemu_menubar_choose_file(GemuMenubar *mb, const char *title,
                              void (*done)(void *ud, const char *path), void *ud) {
    if (!mb) return;
    close_menus(mb);
    mb->dialog = DLG_FILE;
    snprintf(mb->dlg_title, sizeof(mb->dlg_title), "%s", title);
    if (!mb->dir[0] && !getcwd(mb->dir, sizeof(mb->dir))) snprintf(mb->dir, sizeof(mb->dir), ".");
    for (char *p = mb->dir; *p; p++) if (*p == '\\') *p = '/';
    mb->file_done = done;
    mb->file_ud = ud;
    load_dir(mb);
}

void gemu_menubar_about(GemuMenubar *mb, const char *body) {
    if (!mb) return;
    close_menus(mb);
    mb->dialog = DLG_ABOUT;
    snprintf(mb->dlg_title, sizeof(mb->dlg_title), "About GEMU");
    snprintf(mb->dlg_text, sizeof(mb->dlg_text), "%s", body);
}

/* ── Input ───────────────────────────────────────────────────────────────── */

static bool in_rect(SDL_Rect rc, int x, int y) {
    return x >= rc.x && x < rc.x + rc.w && y >= rc.y && y < rc.y + rc.h;
}

/* Item index at (x, y) in level l, or -1. */
static int hit_item(GemuMenubar *mb, int l, int x, int y) {
    Level *lv = &mb->levels[l];
    if (!lv->menu || !in_rect(lv->rect, x, y)) return -1;
    int iy = lv->rect.y + mb->scale;
    for (int i = 0; i < lv->menu->n; i++) {
        int ih = item_h(mb, &lv->menu->items[i]);
        if (y >= iy && y < iy + ih) return i;
        iy += ih;
    }
    return -1;
}

/* Select item i in level l, opening its submenu if it has one. */
static void select_item(GemuMenubar *mb, int l, int i) {
    mb->depth = l + 1;
    mb->levels[l].sel = i;
    const GemuMenuItem *it = &mb->levels[l].menu->items[i];
    if (it->type == GEMU_MENU_SUBMENU && gemu_menu_item_enabled(it) && l + 1 < MAX_DEPTH) {
        mb->levels[l + 1] = (Level){ .menu = it->submenu, .sel = -1 };
        mb->depth = l + 2;
    }
}

/* Activate the highlighted item of the deepest level. */
static void activate(GemuMenubar *mb, int l) {
    Level *lv = &mb->levels[l];
    if (lv->sel < 0) return;
    GemuMenuItem it = lv->menu->items[lv->sel];   /* copy: the model may be rebuilt */
    if (it.type == GEMU_MENU_SUBMENU) {
        select_item(mb, l, lv->sel);
        if (mb->depth > l + 1) mb->levels[l + 1].sel = 0;
        return;
    }
    if (!gemu_menu_item_enabled(&it)) return;
    close_menus(mb);
    gemu_menu_item_activate(&it);
}

static void move_sel(GemuMenubar *mb, int dir) {
    Level *lv = &mb->levels[mb->depth - 1];
    int n = lv->menu ? lv->menu->n : 0;
    for (int k = 1; k <= n; k++) {
        int i = ((lv->sel < 0 ? (dir > 0 ? -1 : 0) : lv->sel) + dir * k + n * 4) % n;
        if (gemu_menu_item_enabled(&lv->menu->items[i])) {
            lv->sel = i;
            return;
        }
    }
}

typedef enum { NAV_NONE, NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_OK, NAV_BACK } Nav;

static Nav nav_from_event(const SDL_Event *ev) {
    if (ev->type == SDL_KEYDOWN) {
        switch (ev->key.keysym.sym) {
        case SDLK_UP:     return NAV_UP;
        case SDLK_DOWN:   return NAV_DOWN;
        case SDLK_LEFT:   return NAV_LEFT;
        case SDLK_RIGHT:  return NAV_RIGHT;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return NAV_OK;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: return NAV_BACK;
        default: return NAV_NONE;
        }
    }
    if (ev->type == SDL_CONTROLLERBUTTONDOWN) {
        switch (ev->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    return NAV_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return NAV_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return NAV_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return NAV_RIGHT;
        case SDL_CONTROLLER_BUTTON_A:          return NAV_OK;
        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: return NAV_BACK;
        default: return NAV_NONE;
        }
    }
    return NAV_NONE;
}

static bool dialog_event(GemuMenubar *mb, const SDL_Event *ev, int mx, int my, bool mouse) {
    Nav nav = nav_from_event(ev);
    if (mb->dialog == DLG_ABOUT) {
        if (nav == NAV_OK || nav == NAV_BACK ||
            (ev->type == SDL_MOUSEBUTTONDOWN && mouse && in_rect(mb->btn_rect[0], mx, my)))
            mb->dialog = DLG_NONE;
        return true;
    }
    /* file browser */
    int rows = mb->file_rows > 0 ? mb->file_rows : 10;
    if (nav == NAV_BACK) { mb->dialog = DLG_NONE; return true; }
    if (nav == NAV_UP && mb->file_sel > 0) mb->file_sel--;
    if (nav == NAV_DOWN && mb->file_sel + 1 < mb->n_files) mb->file_sel++;
    if (nav == NAV_OK) file_enter(mb);
    if (ev->type == SDL_KEYDOWN) {
        SDL_Keycode k = ev->key.keysym.sym;
        if (k == SDLK_PAGEUP)   mb->file_sel = mb->file_sel > rows ? mb->file_sel - rows : 0;
        if (k == SDLK_PAGEDOWN) mb->file_sel = mb->file_sel + rows < mb->n_files ? mb->file_sel + rows : mb->n_files - 1;
        if (k == SDLK_HOME) mb->file_sel = 0;
        if (k == SDLK_END && mb->n_files) mb->file_sel = mb->n_files - 1;
        if (k >= 0x21 && k < 0x7F) {           /* jump to the next entry starting with k */
            for (int s = 1; s <= mb->n_files; s++) {
                int i = (mb->file_sel + s) % mb->n_files;
                if (tolower((unsigned char)mb->files[i].name[0]) == (int)k) { mb->file_sel = i; break; }
            }
        }
    }
    if (ev->type == SDL_MOUSEWHEEL) {
        mb->file_sel -= ev->wheel.y * 3;
        if (mb->file_sel < 0) mb->file_sel = 0;
        if (mb->file_sel >= mb->n_files) mb->file_sel = mb->n_files - 1;
    }
    if (ev->type == SDL_MOUSEBUTTONDOWN && mouse) {
        if (in_rect(mb->btn_rect[1], mx, my)) mb->dialog = DLG_NONE;
        else if (in_rect(mb->btn_rect[0], mx, my)) file_enter(mb);
        else if (in_rect(mb->list_rect, mx, my)) {
            int row = mb->file_top + (my - mb->list_rect.y) / ((GLYPH_H + 2) * mb->scale);
            if (row < mb->n_files) {
                Uint32 now = SDL_GetTicks();
                bool dbl = row == mb->last_click_row && now - mb->last_click < 400;
                mb->file_sel = row;
                mb->last_click = now;
                mb->last_click_row = row;
                if (dbl) file_enter(mb);
            }
        }
    }
    return true;
}

bool gemu_menubar_handle_event(GemuMenubar *mb, const SDL_Event *ev,
                               int win_w, int win_h, int out_w, int out_h) {
    if (!mb) return false;
    if (ev->type == SDL_WINDOWEVENT && ev->window.event == SDL_WINDOWEVENT_LEAVE)
        mb->hover = -1;
    int mx = -1, my = -1;
    bool mouse = false;
    if (ev->type == SDL_MOUSEMOTION || ev->type == SDL_MOUSEBUTTONDOWN || ev->type == SDL_MOUSEBUTTONUP) {
        int wx = ev->type == SDL_MOUSEMOTION ? ev->motion.x : ev->button.x;
        int wy = ev->type == SDL_MOUSEMOTION ? ev->motion.y : ev->button.y;
        mx = win_w > 0 ? wx * out_w / win_w : wx;
        my = win_h > 0 ? wy * out_h / win_h : wy;
        mouse = true;
    }

    if (mb->dialog != DLG_NONE) {
        if (ev->type == SDL_KEYDOWN || ev->type == SDL_KEYUP || ev->type == SDL_TEXTINPUT ||
            ev->type == SDL_MOUSEBUTTONDOWN || ev->type == SDL_MOUSEBUTTONUP ||
            ev->type == SDL_MOUSEWHEEL || ev->type == SDL_MOUSEMOTION ||
            ev->type == SDL_CONTROLLERBUTTONDOWN || ev->type == SDL_CONTROLLERBUTTONUP) {
            if (ev->type != SDL_KEYUP && ev->type != SDL_MOUSEBUTTONUP && ev->type != SDL_MOUSEMOTION)
                dialog_event(mb, ev, mx, my, mouse);
            return true;
        }
        return false;
    }

    int bar_h = gemu_menubar_height(out_h);
    if (mouse) {
        int title = -1;
        for (int i = 0; i < mb->n_titles; i++)
            if (in_rect(mb->title_rect[i], mx, my)) title = i;
        mb->hover = title;
        if (ev->type == SDL_MOUSEMOTION) {
            if (mb->open >= 0) {
                if (title >= 0 && title != mb->open) open_menu(mb, title, false);
                for (int l = mb->depth - 1; l >= 0; l--) {
                    int i = hit_item(mb, l, mx, my);
                    if (i >= 0) { select_item(mb, l, i); break; }
                }
                return true;
            }
            return my < bar_h;
        }
        if (ev->type == SDL_MOUSEBUTTONDOWN) {
            if (title >= 0) {
                if (mb->open == title) close_menus(mb);
                else open_menu(mb, title, false);
                return true;
            }
            if (mb->open >= 0) {
                for (int l = mb->depth - 1; l >= 0; l--) {
                    int i = hit_item(mb, l, mx, my);
                    if (i >= 0) {
                        select_item(mb, l, i);
                        if (mb->levels[l].menu->items[i].type != GEMU_MENU_SUBMENU) activate(mb, l);
                        return true;
                    }
                }
                close_menus(mb);            /* click outside: just close */
                return true;
            }
            return my < bar_h;
        }
        return mb->open >= 0 || my < bar_h;
    }

    if (mb->open < 0) return false;

    /* Keyboard / controller navigation of open menus */
    if (ev->type == SDL_KEYUP || ev->type == SDL_TEXTINPUT || ev->type == SDL_CONTROLLERBUTTONUP)
        return true;
    Nav nav = nav_from_event(ev);
    int l = mb->depth - 1;
    switch (nav) {
    case NAV_UP:    move_sel(mb, -1); break;
    case NAV_DOWN:  move_sel(mb, +1); break;
    case NAV_OK:    activate(mb, l); break;
    case NAV_RIGHT: {
        Level *lv = &mb->levels[l];
        if (lv->sel >= 0 && lv->menu->items[lv->sel].type == GEMU_MENU_SUBMENU) activate(mb, l);
        else open_menu(mb, (mb->open + 1) % (mb->bar->n ? mb->bar->n : 1), true);
        break;
    }
    case NAV_LEFT:
        if (mb->depth > 1) mb->depth--;
        else open_menu(mb, (mb->open + mb->bar->n - 1) % (mb->bar->n ? mb->bar->n : 1), true);
        break;
    case NAV_BACK:
        if (mb->depth > 1) mb->depth--;
        else close_menus(mb);
        break;
    default: break;
    }
    return ev->type == SDL_KEYDOWN || ev->type == SDL_CONTROLLERBUTTONDOWN ||
           ev->type == SDL_MOUSEWHEEL;
}
