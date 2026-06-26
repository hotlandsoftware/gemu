#include "input_menu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#  include <direct.h>
#  define gemu_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define gemu_mkdir(p) mkdir((p), 0755)
#endif

/* ── 6×8 bitmap font (ASCII 32–122, 91 glyphs, 6 bytes each) ─────────── */
/* Each byte is one column, LSB top.  0 = background, 1 = foreground. */

static const uint8_t font_data[] = {
    /* 32 space */ 0x00,0x00,0x00,0x00,0x00,0x00,
    /* 33 !     */ 0x00,0x00,0x00,0x5F,0x00,0x00,
    /* 34 "     */ 0x00,0x00,0x07,0x00,0x07,0x00,
    /* 35 #     */ 0x00,0x14,0x7F,0x14,0x7F,0x14,
    /* 36 $     */ 0x00,0x24,0x2A,0x7F,0x2A,0x12,
    /* 37 %     */ 0x00,0x23,0x13,0x08,0x64,0x62,
    /* 38 &     */ 0x00,0x36,0x49,0x55,0x22,0x50,
    /* 39 '     */ 0x00,0x00,0x00,0x07,0x00,0x00,
    /* 40 (     */ 0x00,0x00,0x1C,0x22,0x41,0x00,
    /* 41 )     */ 0x00,0x00,0x41,0x22,0x1C,0x00,
    /* 42 *     */ 0x00,0x2A,0x1C,0x7F,0x1C,0x2A,
    /* 43 +     */ 0x00,0x08,0x08,0x3E,0x08,0x08,
    /* 44 ,     */ 0x00,0x00,0x80,0x60,0x20,0x00,
    /* 45 -     */ 0x00,0x08,0x08,0x08,0x08,0x08,
    /* 46 .     */ 0x00,0x00,0x00,0x60,0x60,0x00,
    /* 47 /     */ 0x00,0x20,0x10,0x08,0x04,0x02,
    /* 48 0     */ 0x00,0x3E,0x51,0x49,0x45,0x3E,
    /* 49 1     */ 0x00,0x00,0x42,0x7F,0x40,0x00,
    /* 50 2     */ 0x00,0x42,0x61,0x51,0x49,0x46,
    /* 51 3     */ 0x00,0x21,0x41,0x45,0x4B,0x31,
    /* 52 4     */ 0x00,0x18,0x14,0x12,0x7F,0x10,
    /* 53 5     */ 0x00,0x27,0x45,0x45,0x45,0x39,
    /* 54 6     */ 0x00,0x3C,0x4A,0x49,0x49,0x30,
    /* 55 7     */ 0x00,0x01,0x71,0x09,0x05,0x03,
    /* 56 8     */ 0x00,0x36,0x49,0x49,0x49,0x36,
    /* 57 9     */ 0x00,0x06,0x49,0x49,0x29,0x1E,
    /* 58 :     */ 0x00,0x00,0x00,0x36,0x36,0x00,
    /* 59 ;     */ 0x00,0x00,0x80,0x6C,0x2C,0x00,
    /* 60 <     */ 0x00,0x08,0x14,0x22,0x41,0x00,
    /* 61 =     */ 0x00,0x14,0x14,0x14,0x14,0x14,
    /* 62 >     */ 0x00,0x00,0x41,0x22,0x14,0x08,
    /* 63 ?     */ 0x00,0x02,0x01,0x51,0x09,0x06,
    /* 64 @     */ 0x00,0x32,0x49,0x79,0x41,0x3E,
    /* 65 A     */ 0x00,0x7E,0x11,0x11,0x11,0x7E,
    /* 66 B     */ 0x00,0x7F,0x49,0x49,0x49,0x36,
    /* 67 C     */ 0x00,0x3E,0x41,0x41,0x41,0x22,
    /* 68 D     */ 0x00,0x7F,0x41,0x41,0x22,0x1C,
    /* 69 E     */ 0x00,0x7F,0x49,0x49,0x49,0x41,
    /* 70 F     */ 0x00,0x7F,0x09,0x09,0x09,0x01,
    /* 71 G     */ 0x00,0x3E,0x41,0x49,0x49,0x7A,
    /* 72 H     */ 0x00,0x7F,0x08,0x08,0x08,0x7F,
    /* 73 I     */ 0x00,0x00,0x41,0x7F,0x41,0x00,
    /* 74 J     */ 0x00,0x20,0x40,0x41,0x3F,0x01,
    /* 75 K     */ 0x00,0x7F,0x08,0x14,0x22,0x41,
    /* 76 L     */ 0x00,0x7F,0x40,0x40,0x40,0x40,
    /* 77 M     */ 0x00,0x7F,0x02,0x0C,0x02,0x7F,
    /* 78 N     */ 0x00,0x7F,0x04,0x08,0x10,0x7F,
    /* 79 O     */ 0x00,0x3E,0x41,0x41,0x41,0x3E,
    /* 80 P     */ 0x00,0x7F,0x09,0x09,0x09,0x06,
    /* 81 Q     */ 0x00,0x3E,0x41,0x51,0x21,0x5E,
    /* 82 R     */ 0x00,0x7F,0x09,0x19,0x29,0x46,
    /* 83 S     */ 0x00,0x46,0x49,0x49,0x49,0x31,
    /* 84 T     */ 0x00,0x01,0x01,0x7F,0x01,0x01,
    /* 85 U     */ 0x00,0x3F,0x40,0x40,0x40,0x3F,
    /* 86 V     */ 0x00,0x1F,0x20,0x40,0x20,0x1F,
    /* 87 W     */ 0x00,0x3F,0x40,0x38,0x40,0x3F,
    /* 88 X     */ 0x00,0x63,0x14,0x08,0x14,0x63,
    /* 89 Y     */ 0x00,0x07,0x08,0x70,0x08,0x07,
    /* 90 Z     */ 0x00,0x61,0x51,0x49,0x45,0x43,
    /* 91 [     */ 0x00,0x00,0x7F,0x41,0x41,0x00,
    /* 92 \     */ 0x00,0x02,0x04,0x08,0x10,0x20,
    /* 93 ]     */ 0x00,0x00,0x41,0x41,0x7F,0x00,
    /* 94 ^     */ 0x00,0x04,0x02,0x01,0x02,0x04,
    /* 95 _     */ 0x00,0x80,0x80,0x80,0x80,0x80,
    /* 96 `     */ 0x00,0x00,0x01,0x02,0x04,0x00,
    /* 97 a     */ 0x00,0x20,0x54,0x54,0x54,0x78,
    /* 98 b     */ 0x00,0x7F,0x48,0x44,0x44,0x38,
    /* 99 c     */ 0x00,0x38,0x44,0x44,0x44,0x20,
    /*100 d     */ 0x00,0x38,0x44,0x44,0x48,0x7F,
    /*101 e     */ 0x00,0x38,0x54,0x54,0x54,0x18,
    /*102 f     */ 0x00,0x08,0x7E,0x09,0x01,0x02,
    /*103 g     */ 0x00,0x0C,0x52,0x52,0x52,0x3E,
    /*104 h     */ 0x00,0x7F,0x08,0x04,0x04,0x78,
    /*105 i     */ 0x00,0x00,0x44,0x7D,0x40,0x00,
    /*106 j     */ 0x00,0x00,0x40,0x44,0x3D,0x00,
    /*107 k     */ 0x00,0x7F,0x10,0x28,0x44,0x00,
    /*108 l     */ 0x00,0x00,0x41,0x7F,0x40,0x00,
    /*109 m     */ 0x00,0x7C,0x04,0x18,0x04,0x78,
    /*110 n     */ 0x00,0x7C,0x08,0x04,0x04,0x78,
    /*111 o     */ 0x00,0x38,0x44,0x44,0x44,0x38,
    /*112 p     */ 0x00,0x7C,0x14,0x14,0x14,0x08,
    /*113 q     */ 0x00,0x08,0x14,0x14,0x18,0x7C,
    /*114 r     */ 0x00,0x7C,0x08,0x04,0x04,0x08,
    /*115 s     */ 0x00,0x48,0x54,0x54,0x54,0x20,
    /*116 t     */ 0x00,0x04,0x3F,0x44,0x40,0x20,
    /*117 u     */ 0x00,0x3C,0x40,0x40,0x20,0x7C,
    /*118 v     */ 0x00,0x1C,0x20,0x40,0x20,0x1C,
    /*119 w     */ 0x00,0x3C,0x40,0x30,0x40,0x3C,
    /*120 x     */ 0x00,0x44,0x28,0x10,0x28,0x44,
    /*121 y     */ 0x00,0x0C,0x50,0x50,0x50,0x3C,
    /*122 z     */ 0x00,0x44,0x64,0x54,0x4C,0x44,
};
#define FONT_W  6
#define FONT_H  8
#define FONT_GLYPH(c)  (&font_data[((unsigned)(c) - 32u) * FONT_W])

/* ── INI path ──────────────────────────────────────────────────────────── */

static void build_ini_path(char *out, size_t len) {
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("APPDATA");
    if (!base || !base[0]) base = "C:\\Users\\Default\\AppData\\Local";
    snprintf(out, len, "%s\\gemu\\gemu.ini", base);
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(out, len, "%s/.gemu/gemu.ini", home);
#endif
}

static void ensure_ini_dir(const char *ini_path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", ini_path);
    char *sep = strrchr(dir, '/');
#ifdef _WIN32
    char *sep2 = strrchr(dir, '\\');
    if (sep2 > sep) sep = sep2;
#endif
    if (sep) { *sep = '\0'; gemu_mkdir(dir); }
}

/* ── Menu state ─────────────────────────────────────────────────────────── */

typedef enum {
    MENU_CLOSED,
    MENU_MAIN,
    MENU_KEY_CAPTURE,
} MenuPage;

#define MENU_ITEM_MAX  16
#define INPUT_BINDING_NAME_MAX 64

typedef struct {
    SDL_Keycode key;
    char        name[INPUT_BINDING_NAME_MAX];
} InputBinding;

typedef struct {
    const char *label;
    InputBinding *binding_ptr;
    bool is_menu_binding;
    void (*action)(struct InputMenu *);
} MenuItem;

struct InputMenu {
    char           section[64];
    int            n_buttons;
    const char   **button_names;
    SDL_Keycode   *default_bindings;
    InputBinding  *bindings;          /* malloc'd, n_buttons entries */
    InputBinding   menu_binding;

    MenuPage       page;
    int            selected;
    int            capture_index;
    bool           capture_backspace_armed;
    bool           quit_requested;
    bool           reset_requested;

    MenuItem       items[MENU_ITEM_MAX];
    int            n_items;
};

/* ── Forward declarations ───────────────────────────────────────────────── */

static void menu_action_reset(InputMenu *m);
static void menu_action_close(InputMenu *m);
static void menu_action_quit(InputMenu *m);
static void menu_action_input(InputMenu *m);
static void menu_action_menu_key(InputMenu *m);

/* ── INI load / save (uses m->n_buttons / m->button_names) ─────────────── */

static SDL_Keycode name_to_key(const char *name) {
    if (!name || !*name) return SDLK_UNKNOWN;
    SDL_Keycode k = SDL_GetKeyFromName(name);
    if (k != SDLK_UNKNOWN) return k;
    char cap[32];
    if (strlen(name) < sizeof(cap)) {
        for (size_t i = 0; name[i]; i++)
            cap[i] = (char)toupper((unsigned char)name[i]);
        cap[strlen(name)] = '\0';
        k = SDL_GetKeyFromName(cap);
        if (k != SDLK_UNKNOWN) return k;
    }
    return SDLK_UNKNOWN;
}

static void binding_set_key(InputBinding *binding, SDL_Keycode key) {
    binding->key = key;
    snprintf(binding->name, sizeof(binding->name), "%s", SDL_GetKeyName(key));
}

static bool controller_binding_name(const SDL_Event *ev, char *out, size_t out_len) {
    if (ev->type == SDL_CONTROLLERBUTTONDOWN) {
        const char *name = SDL_GameControllerGetStringForButton(
            (SDL_GameControllerButton)ev->cbutton.button);
        if (!name || !*name) return false;
        snprintf(out, out_len, "Controller %s", name);
        return true;
    }

    if (ev->type == SDL_CONTROLLERAXISMOTION) {
        if (ev->caxis.value > -16000 && ev->caxis.value < 16000)
            return false;
        const char *name = SDL_GameControllerGetStringForAxis(
            (SDL_GameControllerAxis)ev->caxis.axis);
        if (!name || !*name) return false;
        snprintf(out, out_len, "Controller %s%c", name,
                 ev->caxis.value < 0 ? '-' : '+');
        return true;
    }

    return false;
}

static void load_ini(InputMenu *m) {
    char path[512];
    build_ini_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    bool in_section = false;
    while (fgets(line, (int)sizeof(line), f)) {
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r' ||
                          line[ll-1] == ' '  || line[ll-1] == '\t'))
            line[--ll] = '\0';

        if (!line[0] || line[0] == '#' || line[0] == ';')
            continue;

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) continue;
            *end = '\0';
            in_section = (strcasecmp(line + 1, m->section) == 0);
            continue;
        }

        if (!in_section) continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key = line;
        while (*key == ' ' || *key == '\t') key++;
        char *ke = key + strlen(key) - 1;
        while (ke >= key && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';

        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;

        for (int i = 0; i < m->n_buttons; i++) {
            if (strcasecmp(key, m->button_names[i]) != 0) continue;
            SDL_Keycode k = name_to_key(val);
            if (k != SDLK_UNKNOWN)
                binding_set_key(&m->bindings[i], k);
            else if (strncasecmp(val, "Controller ", 11) == 0) {
                m->bindings[i].key = SDLK_UNKNOWN;
                snprintf(m->bindings[i].name, sizeof(m->bindings[i].name), "%s", val);
            }
            break;
        }
    }
    fclose(f);
}

static void load_menu_binding(InputMenu *m) {
    binding_set_key(&m->menu_binding, SDLK_TAB);

    char path[512];
    build_ini_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    bool in_section = false;
    while (fgets(line, (int)sizeof(line), f)) {
        size_t ll = strlen(line);
        while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r' ||
                          line[ll-1] == ' '  || line[ll-1] == '\t'))
            line[--ll] = '\0';
        if (!line[0] || line[0] == '#' || line[0] == ';')
            continue;
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) continue;
            *end = '\0';
            in_section = (strcasecmp(line + 1, "input") == 0);
            continue;
        }
        if (!in_section) continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key = line;
        while (*key == ' ' || *key == '\t') key++;
        char *ke = key + strlen(key) - 1;
        while (ke >= key && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';
        if (strcasecmp(key, "Menu") != 0) continue;

        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        SDL_Keycode k = name_to_key(val);
        if (k != SDLK_UNKNOWN)
            binding_set_key(&m->menu_binding, k);
        else if (strncasecmp(val, "Controller ", 11) == 0) {
            m->menu_binding.key = SDLK_UNKNOWN;
            snprintf(m->menu_binding.name, sizeof(m->menu_binding.name), "%s", val);
        }
        break;
    }
    fclose(f);
}

static void write_ini_section(FILE *fw, const InputMenu *m) {
    fprintf(fw, "[%s]\n", m->section);
    for (int i = 0; i < m->n_buttons; i++)
        fprintf(fw, "%s = %s\n", m->button_names[i],
                m->bindings[i].name);
    fprintf(fw, "\n");
}

static void write_menu_ini_section(FILE *fw, const InputMenu *m) {
    fprintf(fw, "[input]\n");
    fprintf(fw, "Menu = %s\n\n", m->menu_binding.name);
}

static void save_section(const InputMenu *m, const char *section,
                         void (*write_section)(FILE *, const InputMenu *)) {
    char path[512];
    build_ini_path(path, sizeof(path));
    ensure_ini_dir(path);

    char *existing = NULL;
    long existing_len = 0;
    FILE *fr = fopen(path, "r");
    if (fr) {
        fseek(fr, 0, SEEK_END);
        existing_len = ftell(fr);
        if (existing_len > 0) {
            existing = malloc((size_t)(existing_len + 1));
            if (existing) {
                rewind(fr);
                size_t n = fread(existing, 1, (size_t)existing_len, fr);
                existing[n] = '\0';
            }
        }
        fclose(fr);
    }

    FILE *fw = fopen(path, "w");
    if (!fw) { free(existing); return; }

    bool section_written = false;
    if (existing) {
        char *p = existing;
        bool in_target = false;
        while (*p) {
            char *line_start = p;
            char *nl = strchr(p, '\n');
            char *end = nl ? nl : p + strlen(p);
            char saved = *end;
            *end = '\0';

            if (line_start[0] == '[') {
                if (in_target && !section_written) {
                    write_section(fw, m);
                    section_written = true;
                }
                in_target = (strncmp(line_start + 1, section, strlen(section)) == 0 &&
                            line_start[1 + strlen(section)] == ']');
                if (in_target) {
                    write_section(fw, m);
                    section_written = true;
                    p = nl ? nl + 1 : end;
                    if (saved) *end = saved;
                    while (*p && *p != '[') {
                        char *nn = strchr(p, '\n');
                        p = nn ? nn + 1 : p + strlen(p);
                    }
                    if (saved) *end = saved;
                    continue;
                }
            }

            fputs(line_start, fw);
            if (nl) fputc('\n', fw);
            *end = saved;
            p = nl ? nl + 1 : end;
            if (!*p && !nl) break;
        }
        if (!section_written)
            write_section(fw, m);
        free(existing);
    } else {
        write_section(fw, m);
    }

    fclose(fw);
}

static void save_ini(const InputMenu *m) {
    save_section(m, m->section, write_ini_section);
}

static void save_menu_binding(const InputMenu *m) {
    save_section(m, "input", write_menu_ini_section);
}

/* ── Menu item tables ───────────────────────────────────────────────────── */

static void build_main_menu(InputMenu *m) {
    m->n_items = 0;
    m->items[m->n_items++] = (MenuItem){
        .label  = "Input",
        .action = menu_action_input,
    };
    m->items[m->n_items++] = (MenuItem){
        .label  = "Reset",
        .action = menu_action_reset,
    };
    m->items[m->n_items++] = (MenuItem){
        .label  = "Close Menu",
        .action = menu_action_close,
    };
    m->items[m->n_items++] = (MenuItem){
        .label  = "Quit",
        .action = menu_action_quit,
    };
}

static void build_input_menu(InputMenu *m) {
    m->n_items = 0;
    m->items[m->n_items++] = (MenuItem){
        .label = "Menu Key",
        .binding_ptr = &m->menu_binding,
        .is_menu_binding = true,
        .action = menu_action_menu_key,
    };
    for (int i = 0; i < m->n_buttons; i++) {
        m->items[m->n_items++] = (MenuItem){
            .label   = m->button_names[i],
            .binding_ptr = &m->bindings[i],
        };
    }
    m->items[m->n_items++] = (MenuItem){
        .label  = "(back)",
        .action = NULL,
    };
}

/* ── Actions ────────────────────────────────────────────────────────────── */

static void menu_action_reset(InputMenu *m) {
    m->reset_requested = true;
    m->page = MENU_CLOSED;
}

static void menu_action_quit(InputMenu *m) {
    m->quit_requested = true;
    m->page = MENU_CLOSED;
}

static void menu_action_close(InputMenu *m) {
    m->page = MENU_CLOSED;
}

static void menu_action_input(InputMenu *m) {
    m->page = MENU_MAIN;
    build_input_menu(m);
    m->selected = 0;
}

static void menu_action_menu_key(InputMenu *m) {
    m->capture_index = -1;
    m->capture_backspace_armed = false;
    m->page = MENU_KEY_CAPTURE;
}

void input_menu_reset_keys(InputMenu *m) {
    if (!m) return;
    binding_set_key(&m->menu_binding, SDLK_TAB);
    save_menu_binding(m);

    for (int i = 0; i < m->n_buttons; i++)
        binding_set_key(&m->bindings[i],
                        m->default_bindings ? m->default_bindings[i] : SDLK_UNKNOWN);
    if (m->n_buttons > 0)
        save_ini(m);

    build_main_menu(m);
    m->page = MENU_MAIN;
    m->selected = 0;
}

/* ── Font rendering helpers ─────────────────────────────────────────────── */

static void draw_char(SDL_Renderer *r, int x, int y, char ch,
                      uint32_t fg, uint32_t bg, int s) {
    if (ch < 32 || ch > 122) ch = '?';
    const uint8_t *glyph = FONT_GLYPH(ch);
    for (int col = 0; col < FONT_W; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT_H; row++) {
            SDL_Rect dot = { x + col * s, y + row * s, s, s };
            if (bits & (1u << (unsigned)row)) {
                SDL_SetRenderDrawColor(r, (fg >> 16) & 0xFF,
                                       (fg >> 8) & 0xFF, fg & 0xFF, 255);
            } else {
                SDL_SetRenderDrawColor(r, (bg >> 16) & 0xFF,
                                       (bg >> 8) & 0xFF, bg & 0xFF, 255);
            }
            SDL_RenderFillRect(r, &dot);
        }
    }
}

static int draw_text(SDL_Renderer *r, int x, int y, const char *str,
                     uint32_t fg, uint32_t bg, int s) {
    int start = x;
    for (; *str; str++) {
        draw_char(r, x, y, *str, fg, bg, s);
        x += FONT_W * s;
    }
    return x - start;
}

/* ── Rendering ──────────────────────────────────────────────────────────── */

void input_menu_render(InputMenu *m, SDL_Renderer *r, int pixel_scale) {
    if (!m || m->page == MENU_CLOSED) return;
    if (pixel_scale < 1) pixel_scale = 1;

    int s = pixel_scale;
    int fw = FONT_W * s, fh = FONT_H * s;

    int w = 256, h = 240;
    SDL_GetRendererOutputSize(r, &w, &h);
    if (w <= 0) w = 256;
    if (h <= 0) h = 240;

    /* Semi-transparent dark background */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
    SDL_Rect bg = { 0, 0, w, h };
    SDL_RenderFillRect(r, &bg);

    /* Menu box: centered, 75% width */
    int mw = (w * 3) / 4;
    int mh = (int)(m->n_items + 2) * (fh + 2 * s) + 8 * s;
    int mx = (w - mw) / 2;
    int my = (h - mh) / 2;

    /* Box background */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 80, 255);
    SDL_RenderFillRect(r, &(SDL_Rect){ mx, my, mw, mh });

    /* Border */
    SDL_SetRenderDrawColor(r, 60, 60, 180, 255);
    SDL_RenderDrawRect(r, &(SDL_Rect){ mx - 1, my - 1, mw + 2, mh + 2 });

    int ty = my + 6 * s;

    /* Title */
    if (m->page == MENU_KEY_CAPTURE) {
        draw_text(r, mx + 6 * s, ty, "Press key for: ", 0xFFFFFFFF, 0x000050FF, s);
        draw_text(r, mx + 6 * s + 15 * fw, ty,
                  m->capture_index < 0 ? "Menu" : m->button_names[m->capture_index],
                  0xFFFFFF00, 0x000050FF, s);
        draw_text(r, mx + 6 * s, ty + fh + 4 * s,
                  m->capture_backspace_armed ? "(Backspace again to cancel)"
                                             : "(Backspace to cancel)",
                  0xFF8888FF, 0x000050FF, s);
        return;
    }

    bool is_input_submenu = (m->n_items > 0 && m->items[0].binding_ptr != NULL);

    if (is_input_submenu)
        draw_text(r, mx + 6 * s, ty, m->section, 0xFFFFFFFF, 0x000050FF, s);
    else
        draw_text(r, mx + 6 * s, ty, "Main Menu", 0xFFFFFFFF, 0x000050FF, s);
    ty += fh + 4 * s;

    /* Separator */
    SDL_SetRenderDrawColor(r, 60, 60, 180, 255);
    SDL_RenderDrawLine(r, mx + 4 * s, ty, mx + mw - 4 * s, ty);
    ty += 4 * s;

    /* Items */
    for (int i = 0; i < m->n_items; i++) {
        uint32_t fg = 0xFFFFFFFF;
        uint32_t bg = 0x000050FF;

        if (i == m->selected) {
            SDL_SetRenderDrawColor(r, 60, 60, 180, 255);
            SDL_RenderFillRect(r, &(SDL_Rect){
                mx + 4 * s, ty - s, mw - 8 * s, fh + 2 * s
            });
            fg = 0xFF000000;
            bg = 0x3C3CB4FF;
        }

        draw_text(r, mx + 8 * s, ty, m->items[i].label, fg, bg, s);

        if (m->items[i].binding_ptr) {
            const char *kname = m->items[i].binding_ptr->name;
            int kw = (int)strlen(kname) * fw;
            draw_text(r, mx + mw - 8 * s - kw, ty, kname, fg, bg, s);
        }

        ty += fh + 2 * s;
    }
}

/* ── Event handling ─────────────────────────────────────────────────────── */

bool input_menu_handle_event(InputMenu *m, const SDL_Event *ev) {
    if (!m || m->page == MENU_CLOSED) return false;

    /* Key capture mode: record the next non-modifier key */
    if (m->page == MENU_KEY_CAPTURE) {
        if (ev->type == SDL_CONTROLLERBUTTONDOWN ||
            ev->type == SDL_CONTROLLERAXISMOTION) {
            char name[INPUT_BINDING_NAME_MAX];
            if (!controller_binding_name(ev, name, sizeof(name)))
                return true;
            InputBinding *binding = m->capture_index < 0
                                  ? &m->menu_binding
                                  : &m->bindings[m->capture_index];
            binding->key = SDLK_UNKNOWN;
            snprintf(binding->name, sizeof(binding->name), "%s", name);
            if (m->capture_index < 0)
                save_menu_binding(m);
            else
                save_ini(m);
            m->page = MENU_MAIN;
            m->capture_backspace_armed = false;
            return true;
        }

        if (ev->type != SDL_KEYDOWN) return true;
        SDL_Keycode sym = ev->key.keysym.sym;
        if (sym == SDLK_BACKSPACE) {
            if (m->capture_backspace_armed) {
                m->page = MENU_MAIN;
                m->capture_backspace_armed = false;
            } else {
                m->capture_backspace_armed = true;
            }
            return true;
        }
        if (sym == SDLK_LSHIFT || sym == SDLK_RSHIFT ||
            sym == SDLK_LCTRL  || sym == SDLK_RCTRL ||
            sym == SDLK_LALT   || sym == SDLK_RALT ||
            sym == SDLK_LGUI   || sym == SDLK_RGUI)
            return true;
        if (m->capture_index < 0) {
            binding_set_key(&m->menu_binding, sym);
            save_menu_binding(m);
        } else {
            binding_set_key(&m->bindings[m->capture_index], sym);
            save_ini(m);
        }
        m->page = MENU_MAIN;
        m->capture_backspace_armed = false;
        return true;
    }

    if (input_menu_event_opens(m, ev)) {
        m->page = MENU_CLOSED;
        return true;
    }

    if (ev->type != SDL_KEYDOWN) return m->page != MENU_CLOSED;
    SDL_Keycode sym = ev->key.keysym.sym;

    bool is_input_submenu = (m->items[0].binding_ptr != NULL);

    switch (sym) {
    case SDLK_TAB:
        m->page = MENU_CLOSED;
        return true;
    case SDLK_UP:
        if (m->selected > 0)
            m->selected--;
        else
            m->selected = m->n_items - 1;
        return true;
    case SDLK_DOWN:
        if (m->selected < m->n_items - 1)
            m->selected++;
        else
            m->selected = 0;
        return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (is_input_submenu) {
            if (m->items[m->selected].is_menu_binding) {
                menu_action_menu_key(m);
            } else if (m->items[m->selected].binding_ptr) {
                m->capture_index = 0;
                m->capture_backspace_armed = false;
                for (int i = 0; i < m->n_buttons; i++) {
                    if (m->items[m->selected].binding_ptr == &m->bindings[i]) {
                        m->capture_index = i;
                        break;
                    }
                }
                m->page = MENU_KEY_CAPTURE;
            } else {
                build_main_menu(m);
                m->page = MENU_MAIN;
                m->selected = 0;
            }
        } else {
            if (m->items[m->selected].is_menu_binding)
                menu_action_menu_key(m);
            else if (m->items[m->selected].action)
                m->items[m->selected].action(m);
        }
        return true;
    case SDLK_ESCAPE:
        if (is_input_submenu) {
            build_main_menu(m);
            m->page = MENU_MAIN;
            m->selected = 0;
        } else {
            m->page = MENU_CLOSED;
        }
        return true;
    default:
        break;
    }

    return m->page != MENU_CLOSED;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

InputMenu *input_menu_create(SDL_Renderer *renderer,
                             int n_buttons,
                             const char **button_names,
                             const SDL_Keycode *default_bindings,
                             const char *ini_section) {
    (void)renderer;
    InputMenu *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    snprintf(m->section, sizeof(m->section), "%s",
             ini_section ? ini_section : "default");

    m->n_buttons = n_buttons;
    if (n_buttons > 0 && button_names) {
        m->button_names = malloc((size_t)n_buttons * sizeof(*m->button_names));
        if (!m->button_names) { free(m); return NULL; }
        memcpy(m->button_names, button_names,
               (size_t)n_buttons * sizeof(*m->button_names));
    }
    m->default_bindings = calloc(n_buttons ? (size_t)n_buttons : 1,
                                 sizeof(*m->default_bindings));
    if (!m->default_bindings) { free(m->button_names); free(m); return NULL; }
    if (n_buttons > 0 && default_bindings)
        memcpy(m->default_bindings, default_bindings,
               (size_t)n_buttons * sizeof(*m->default_bindings));
    /* allocate at least 1 to avoid malloc(0) portability issues */
    m->bindings = calloc(n_buttons ? (size_t)n_buttons : 1, sizeof(*m->bindings));
    if (!m->bindings) { free(m->default_bindings); free(m->button_names); free(m); return NULL; }
    if (n_buttons > 0 && default_bindings) {
        for (int i = 0; i < n_buttons; i++)
            binding_set_key(&m->bindings[i], default_bindings[i]);
    }
    load_menu_binding(m);
    load_ini(m);

    build_main_menu(m);
    m->page     = MENU_CLOSED;
    m->selected = 0;

    return m;
}

void input_menu_destroy(InputMenu *m) {
    if (!m) return;
    free(m->button_names);
    free(m->default_bindings);
    free(m->bindings);
    free(m);
}

bool input_menu_toggle(InputMenu *m) {
    if (m->page != MENU_CLOSED) {
        m->page = MENU_CLOSED;
        return false;
    }
    build_main_menu(m);
    m->page     = MENU_MAIN;
    m->selected = 0;
    return true;
}

static bool binding_matches_controller(const InputBinding *binding,
                                       const SDL_Event *ev,
                                       bool allow_default_l1) {
    if (ev->type != SDL_CONTROLLERBUTTONDOWN &&
        ev->type != SDL_CONTROLLERAXISMOTION)
        return false;

    char name[INPUT_BINDING_NAME_MAX];
    if (!controller_binding_name(ev, name, sizeof(name)))
        return false;
    if (strcasecmp(binding->name, name) == 0)
        return true;
    return allow_default_l1 && strcasecmp(name, "Controller leftshoulder") == 0;
}

bool input_menu_event_opens(const InputMenu *m, const SDL_Event *ev) {
    if (!m || !ev) return false;
    if (ev->type == SDL_KEYDOWN)
        return m->menu_binding.key != SDLK_UNKNOWN &&
               ev->key.keysym.sym == m->menu_binding.key;
    return binding_matches_controller(&m->menu_binding, ev, true);
}

bool input_menu_is_open(const InputMenu *m) {
    return m && m->page != MENU_CLOSED;
}

bool input_menu_quit_requested(const InputMenu *m) {
    return m && m->quit_requested;
}

bool input_menu_reset_requested(const InputMenu *m) {
    return m && m->reset_requested;
}

void input_menu_clear_actions(InputMenu *m) {
    if (m) {
        m->quit_requested  = false;
        m->reset_requested = false;
    }
}

int input_menu_key_to_btn_index(const InputMenu *m, SDL_Keycode key) {
    if (!m) return -1;
    for (int i = 0; i < m->n_buttons; i++)
        if (key == m->bindings[i].key)
            return i;
    return -1;
}
