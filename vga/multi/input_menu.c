#include "input_menu.h"
#include "vgafont16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "gemu/util.h"

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

const uint8_t *gemu_font_glyph(char ch) {
    if (ch < 32 || ch > 122) ch = '?';
    return FONT_GLYPH(ch);
}

/* ── INI path ──────────────────────────────────────────────────────────── */

static void build_ini_path(char *out, size_t len) {
    gemu_config_path(out, len, "gemu.ini");
}

/* ── Menu state ─────────────────────────────────────────────────────────── */

typedef enum {
    MENU_CLOSED,
    MENU_MAIN,
    MENU_BINDING_OPTIONS,
    MENU_KEY_CAPTURE,
} MenuPage;

#define MENU_ITEM_MAX  64
#define INPUT_BINDING_NAME_MAX 64
#define MENU_CONTROL_INDEX_BASE (-100)

typedef enum {
    MENU_LIST_MAIN,
    MENU_LIST_INPUT,
    MENU_LIST_CONTROLS,
} MenuList;

typedef enum {
    MENU_CTL_OPEN,
    MENU_CTL_ACCEPT,
    MENU_CTL_BACK,
    MENU_CTL_UP,
    MENU_CTL_DOWN,
    MENU_CTL_LEFT,
    MENU_CTL_RIGHT,
    MENU_CTL_COUNT,
} MenuControl;

static const char *menu_control_names[MENU_CTL_COUNT] = {
    "Open",
    "Accept",
    "Back",
    "Up",
    "Down",
    "Left",
    "Right",
};

static const SDL_Keycode menu_control_default_keys[MENU_CTL_COUNT] = {
    SDLK_TAB,
    SDLK_RETURN,
    SDLK_ESCAPE,
    SDLK_UP,
    SDLK_DOWN,
    SDLK_LEFT,
    SDLK_RIGHT,
};

static const char *menu_control_default_controllers[MENU_CTL_COUNT] = {
    "Controller leftshoulder",
    "Controller a",
    "Controller b",
    "Controller dpup",
    "Controller dpdown",
    "Controller dpleft",
    "Controller dpright",
};

typedef struct {
    SDL_Keycode key;
    char        name[INPUT_BINDING_NAME_MAX];
} InputBinding;

typedef struct {
    const char *label;
    InputBinding *binding_ptr;
    InputBinding *controller_ptr;
    int binding_index; /* -1 = global menu binding */
    void (*action)(struct InputMenu *);
} MenuItem;

struct InputMenu {
    char           section[64];
    int            n_buttons;
    const char   **button_names;
    char          (*controller_button_names)[INPUT_BINDING_NAME_MAX];
    SDL_Keycode   *default_bindings;
    char          (*default_controller_bindings)[INPUT_BINDING_NAME_MAX];
    InputBinding  *key_bindings;        /* malloc'd, n_buttons entries */
    InputBinding  *controller_bindings; /* malloc'd, n_buttons entries */
    InputBinding   menu_key_bindings[MENU_CTL_COUNT];
    InputBinding   menu_controller_bindings[MENU_CTL_COUNT];

    InputMenuPage *pages;      /* malloc'd copy; NULL when n_pages==0 */
    int            n_pages;

    MenuPage       page;
    MenuList       list;
    int            input_page;
    int            input_page_count;
    int            input_chunk;        /* part of the page shown, when it doesn't fit */
    int            input_chunks;
    int            rows_fit;           /* list rows the window has room for; 0 = unknown */
    int            selected;
    int            option_selected;
    int            capture_index;
    bool           capture_controller;
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
static void menu_action_menu_controls(InputMenu *m);
static void start_binding_options(InputMenu *m, int index, bool controller);
static void start_capture(InputMenu *m, int index, bool controller);
static bool binding_matches_controller(const InputBinding *binding,
                                       const SDL_Event *ev);

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

static void binding_clear(InputBinding *binding) {
    binding->key = SDLK_UNKNOWN;
    binding->name[0] = '\0';
}

static void binding_set_controller(InputBinding *binding, const char *name) {
    binding->key = SDLK_UNKNOWN;
    snprintf(binding->name, sizeof(binding->name), "%s", name ? name : "");
}

static const char *binding_display_name(const InputBinding *binding) {
    return (binding && binding->name[0]) ? binding->name : "(none)";
}

static int page_for_button(const InputMenu *m, int i) {
    int start = 0;
    for (int p = 0; p < m->n_pages; p++) {
        if (i < start + m->pages[p].n_buttons) return p;
        start += m->pages[p].n_buttons;
    }
    return m->n_pages > 0 ? m->n_pages - 1 : 0;
}

static const char *page_title(const InputMenu *m, int p) {
    if (m->n_pages > 0 && p < m->n_pages) return m->pages[p].name;
    return m->section;
}

static const char *display_button_name(const char *name) {
    if (!name) return "";
    if (name[0] == 'P' && name[1] >= '2' && name[1] <= '9' && name[2] == ' ')
        return name + 3;
    return name;
}

static bool is_menu_control_index(int index) {
    return index <= MENU_CONTROL_INDEX_BASE;
}

static int menu_control_from_index(int index) {
    return MENU_CONTROL_INDEX_BASE - index;
}

static int menu_control_to_index(int control) {
    return MENU_CONTROL_INDEX_BASE - control;
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

        bool controller = false;
        char *dot = strrchr(key, '.');
        if (dot && strcasecmp(dot, ".controller") == 0) {
            *dot = '\0';
            controller = true;
        }

        for (int i = 0; i < m->n_buttons; i++) {
            if (strcasecmp(key, m->button_names[i]) != 0) continue;
            if (!*val) {
                binding_clear(controller ? &m->controller_bindings[i]
                                         : &m->key_bindings[i]);
                break;
            }
            SDL_Keycode k = name_to_key(val);
            if (controller) {
                if (strncasecmp(val, "Controller ", 11) == 0)
                    binding_set_controller(&m->controller_bindings[i], val);
            } else if (k != SDLK_UNKNOWN) {
                binding_set_key(&m->key_bindings[i], k);
            } else if (strncasecmp(val, "Controller ", 11) == 0) {
                binding_set_controller(&m->controller_bindings[i], val);
            }
            break;
        }
    }
    fclose(f);
}

static void load_menu_binding(InputMenu *m) {
    for (int i = 0; i < MENU_CTL_COUNT; i++) {
        binding_set_key(&m->menu_key_bindings[i], menu_control_default_keys[i]);
        binding_set_controller(&m->menu_controller_bindings[i],
                               menu_control_default_controllers[i]);
    }

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
        bool controller = false;
        char *dot = strrchr(key, '.');
        if (dot && strcasecmp(dot, ".controller") == 0) {
            *dot = '\0';
            controller = true;
        }
        int control = -1;
        for (int i = 0; i < MENU_CTL_COUNT; i++) {
            if (strcasecmp(key, i == MENU_CTL_OPEN ? "Menu" : menu_control_names[i]) == 0) {
                control = i;
                break;
            }
        }
        if (control < 0) continue;

        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        if (!*val) {
            binding_clear(controller ? &m->menu_controller_bindings[control]
                                     : &m->menu_key_bindings[control]);
            continue;
        }
        SDL_Keycode k = name_to_key(val);
        if (controller) {
            if (strncasecmp(val, "Controller ", 11) == 0)
                binding_set_controller(&m->menu_controller_bindings[control], val);
        } else if (k != SDLK_UNKNOWN) {
            binding_set_key(&m->menu_key_bindings[control], k);
        } else if (strncasecmp(val, "Controller ", 11) == 0) {
            binding_set_controller(&m->menu_controller_bindings[control], val);
        }
    }
    fclose(f);
}

static void write_ini_section(FILE *fw, const InputMenu *m) {
    fprintf(fw, "[%s]\n", m->section);
    for (int i = 0; i < m->n_buttons; i++) {
        fprintf(fw, "%s = %s\n", m->button_names[i],
                m->key_bindings[i].name);
        fprintf(fw, "%s.controller = %s\n", m->button_names[i],
                m->controller_bindings[i].name);
    }
    fprintf(fw, "\n");
}

static void write_menu_ini_section(FILE *fw, const InputMenu *m) {
    fprintf(fw, "[input]\n");
    for (int i = 0; i < MENU_CTL_COUNT; i++) {
        const char *name = i == MENU_CTL_OPEN ? "Menu" : menu_control_names[i];
        fprintf(fw, "%s = %s\n", name, m->menu_key_bindings[i].name);
        fprintf(fw, "%s.controller = %s\n", name,
                m->menu_controller_bindings[i].name);
    }
    fprintf(fw, "\n");
}

static void save_section(const InputMenu *m, const char *section,
                         void (*write_section)(FILE *, const InputMenu *)) {
    char path[512];
    build_ini_path(path, sizeof(path));
    gemu_ensure_parent_dir(path);

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
    m->list = MENU_LIST_MAIN;
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

/* A device page with more buttons than the window has rows for is shown
 * in parts; Left/Right step through them before moving to the next page. */
static void build_input_menu(InputMenu *m) {
    m->list = MENU_LIST_INPUT;
    m->input_page_count = m->n_pages > 0 ? m->n_pages : 1;
    if (m->input_page < 0) m->input_page = 0;
    if (m->input_page >= m->input_page_count)
        m->input_page = m->input_page_count - 1;

    int n_page = 0;
    for (int i = 0; i < m->n_buttons; i++)
        if (page_for_button(m, i) == m->input_page) n_page++;

    /* Each part ends with "(close)"; the first page starts with "Menu keys >". */
    bool menu_keys = m->input_page == 0;
    int fit = m->rows_fit >= 3 ? m->rows_fit : MENU_ITEM_MAX;
    if (fit > MENU_ITEM_MAX) fit = MENU_ITEM_MAX;
    int first_cap = fit - 1 - (menu_keys ? 1 : 0), cap = fit - 1;
    if (first_cap < 1) first_cap = 1;
    m->input_chunks = 1;
    for (int left = n_page - first_cap; left > 0; left -= cap) m->input_chunks++;
    if (m->input_chunk < 0) m->input_chunk = 0;
    if (m->input_chunk >= m->input_chunks) m->input_chunk = m->input_chunks - 1;
    int from = m->input_chunk ? first_cap + (m->input_chunk - 1) * cap : 0;
    int to = from + (m->input_chunk ? cap : first_cap);

    m->n_items = 0;
    if (menu_keys && m->input_chunk == 0) {
        m->items[m->n_items++] = (MenuItem){
            .label = "Menu keys >",
            .action = menu_action_menu_controls,
        };
    }
    for (int i = 0, k = 0; i < m->n_buttons; i++) {
        if (page_for_button(m, i) != m->input_page)
            continue;
        if (k++ < from || k > to)
            continue;
        m->items[m->n_items++] = (MenuItem){
            .label = display_button_name(m->button_names[i]),
            .binding_ptr = &m->key_bindings[i],
            .controller_ptr = &m->controller_bindings[i],
            .binding_index = i,
        };
    }
    m->items[m->n_items++] = (MenuItem){
        .label  = "(close)",
        .action = NULL,
    };
}

/* Left/Right on the Input list: the next part of this page, then the
 * next page (entering the previous one at its last part). */
static void input_menu_step(InputMenu *m, int dir) {
    if (m->input_chunks <= 1 && m->input_page_count <= 1) return;
    int chunk = m->input_chunk + dir;
    if (chunk >= 0 && chunk < m->input_chunks) {
        m->input_chunk = chunk;
    } else {
        m->input_page = (m->input_page + dir + m->input_page_count) % m->input_page_count;
        m->input_chunk = dir > 0 ? 0 : MENU_ITEM_MAX;   /* clamped to the last part */
    }
    build_input_menu(m);
    m->selected = 0;
}

static void build_menu_controls_menu(InputMenu *m) {
    m->list = MENU_LIST_CONTROLS;
    m->n_items = 0;
    for (int i = 0; i < MENU_CTL_COUNT; i++) {
        m->items[m->n_items++] = (MenuItem){
            .label = i == MENU_CTL_OPEN ? "Open" : menu_control_names[i],
            .binding_ptr = &m->menu_key_bindings[i],
            .controller_ptr = &m->menu_controller_bindings[i],
            .binding_index = menu_control_to_index(i),
        };
    }
    m->items[m->n_items++] = (MenuItem){
        .label = "(back)",
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
    m->input_page = 0;
    m->input_chunk = 0;
    build_input_menu(m);
    m->selected = 0;
}

static void menu_action_menu_controls(InputMenu *m) {
    m->page = MENU_MAIN;
    build_menu_controls_menu(m);
    m->selected = 0;
}

static void start_binding_options(InputMenu *m, int index, bool controller) {
    m->capture_index = index;
    m->capture_controller = controller;
    m->capture_backspace_armed = false;
    m->option_selected = 0;
    m->page = MENU_BINDING_OPTIONS;
}

static void start_capture(InputMenu *m, int index, bool controller) {
    m->capture_index = index;
    m->capture_controller = controller;
    m->capture_backspace_armed = false;
    m->page = MENU_KEY_CAPTURE;
}

static InputBinding *menu_capture_binding(InputMenu *m, bool controller) {
    if (!is_menu_control_index(m->capture_index))
        return NULL;
    int control = menu_control_from_index(m->capture_index);
    if (control < 0 || control >= MENU_CTL_COUNT)
        return NULL;
    return controller ? &m->menu_controller_bindings[control]
                      : &m->menu_key_bindings[control];
}

void input_menu_reset_keys(InputMenu *m) {
    if (!m) return;
    for (int i = 0; i < MENU_CTL_COUNT; i++) {
        binding_set_key(&m->menu_key_bindings[i], menu_control_default_keys[i]);
        binding_set_controller(&m->menu_controller_bindings[i],
                               menu_control_default_controllers[i]);
    }
    save_menu_binding(m);

    for (int i = 0; i < m->n_buttons; i++) {
        SDL_Keycode key = m->default_bindings ? m->default_bindings[i] : SDLK_UNKNOWN;
        if (key != SDLK_UNKNOWN)
            binding_set_key(&m->key_bindings[i], key);
        else
            binding_clear(&m->key_bindings[i]);
        binding_set_controller(&m->controller_bindings[i],
                               m->default_controller_bindings ? m->default_controller_bindings[i] : "");
    }
    if (m->n_buttons > 0)
        save_ini(m);

    /* Refresh the binding screen if it's showing; otherwise stay closed
     * (this is also the menu bar's "Reset bindings to defaults"). */
    if (m->page != MENU_CLOSED)
        input_menu_open_input(m);
}

/* ── Rendering ──────────────────────────────────────────────────────────── */

/* Drawn as a dialog matching the window menu bar's (gemu_menubar_sdl.c):
 * Windows 3.1 frame and navy title bar, white body, IBM 8x16 font. */
#define DLG_GLYPH_W 8
#define DLG_GLYPH_H 16
#define DLG_BG      0xFFFFFF
#define DLG_TITLE   0x000080
#define DLG_TITLEFG 0xFFFFFF
#define DLG_FRAME   0xC0C0C0
#define DLG_BORDER  0x000000
#define DLG_TEXT    0x000000
#define DLG_HI      0x000080
#define DLG_HITEXT  0xFFFFFF
#define DLG_ACCENT  0x000080
#define DLG_HINT    0x8A2020

static void dlg_fill(SDL_Renderer *r, int x, int y, int w, int h, uint32_t rgb) {
    SDL_SetRenderDrawColor(r, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 255);
    SDL_RenderFillRect(r, &(SDL_Rect){ x, y, w, h });
}

static int dlg_text(SDL_Renderer *r, int x, int y, const char *str, uint32_t rgb, int s) {
    SDL_SetRenderDrawColor(r, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 255);
    int start = x;
    for (; *str; str++, x += DLG_GLYPH_W * s) {
        const uint8_t *g = vgafont16 + (unsigned char)*str * 16;
        for (int row = 0; row < DLG_GLYPH_H; row++)
            for (int col = 0; col < DLG_GLYPH_W; col++)
                if (g[row] & (0x80 >> col))
                    SDL_RenderFillRect(r, &(SDL_Rect){ x + col * s, y + row * s, s, s });
    }
    return x - start;
}

void input_menu_render(InputMenu *m, SDL_Renderer *r, int pixel_scale) {
    (void)pixel_scale;
    if (!m || m->page == MENU_CLOSED) return;

    int w = 256, h = 240;
    SDL_GetRendererOutputSize(r, &w, &h);
    int s = h >= 1100 ? 2 : 1;                  /* same UI scale as the menu bar */
    int fw = DLG_GLYPH_W * s, fh = DLG_GLYPH_H * s, lh = fh + 4 * s;

    /* How many list rows fit: the window less the frame, title and padding.
     * The Input list is split into parts of that size. */
    int fit = (h - 2 * 5 * s - 8 * s - (fh + 6 * s) - 20 * s) / lh;
    if (fit != m->rows_fit) {
        m->rows_fit = fit;
        if (m->page == MENU_MAIN && m->list == MENU_LIST_INPUT) {
            int sel = m->selected;
            build_input_menu(m);
            m->selected = sel < m->n_items ? sel : m->n_items - 1;
        }
    }

    /* What the dialog shows */
    const char *title;
    char title_buf[160];
    int rows;
    if (m->page == MENU_BINDING_OPTIONS || m->page == MENU_KEY_CAPTURE) {
        snprintf(title_buf, sizeof(title_buf), "%s%s", m->items[m->selected].label,
                 m->capture_controller ? " (controller)" : "");
        title = title_buf;
        rows = m->page == MENU_BINDING_OPTIONS ? 3 : 2;
    } else {
        if (m->list == MENU_LIST_INPUT) {
            char pname[96];
            input_menu_page_label(m, m->input_page, pname, sizeof(pname));
            char part[32] = "";
            if (m->input_chunks > 1)
                snprintf(part, sizeof(part), " %d/%d", m->input_chunk + 1, m->input_chunks);
            if (m->input_page_count > 1 || m->input_chunks > 1)
                snprintf(title_buf, sizeof(title_buf), "Input: %s%s  (Left/Right: %s)", pname, part,
                         m->input_page_count > 1 ? "other devices" : "more");
            else
                snprintf(title_buf, sizeof(title_buf), "Input: %s", pname);
            title = title_buf;
        } else {
            title = m->list == MENU_LIST_CONTROLS ? "Menu navigation keys" : "Menu";
        }
        rows = m->n_items;
    }

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 96);
    SDL_RenderFillRect(r, &(SDL_Rect){ 0, 0, w, h });
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    int th = fh + 6 * s;
    int mw = 60 * fw;
    if (mw > w - 16 * s) mw = w - 16 * s;
    int mh = th + 10 * s + rows * lh + 10 * s;
    int mx = (w - mw) / 2, my = (h - mh) / 2;
    int fb = 5 * s;                                                   /* 3.1 frame */
    dlg_fill(r, mx - fb, my - fb, mw + 2 * fb, mh + 2 * fb, DLG_BORDER);
    dlg_fill(r, mx - fb + s, my - fb + s, mw + 2 * fb - 2 * s, mh + 2 * fb - 2 * s, DLG_FRAME);
    dlg_fill(r, mx - s, my - s, mw + 2 * s, mh + 2 * s, DLG_BORDER);
    dlg_fill(r, mx, my, mw, mh, DLG_BG);
    dlg_fill(r, mx, my, mw, th, DLG_TITLE);
    dlg_fill(r, mx, my + th, mw, s, DLG_BORDER);
    int tw = (int)strlen(title) * fw;
    dlg_text(r, mx + (mw - tw > 0 ? (mw - tw) / 2 : 4 * s), my + 3 * s, title, DLG_TITLEFG, s);

    int ty = my + th + 10 * s, tx = mx + 12 * s;

    if (m->page == MENU_BINDING_OPTIONS) {
        static const char *options[] = { "< Back", "Clear binding", "Set new binding..." };
        for (int i = 0; i < 3; i++) {
            bool hi = i == m->option_selected;
            if (hi) dlg_fill(r, mx + 4 * s, ty - 2 * s, mw - 8 * s, lh, DLG_HI);
            dlg_text(r, tx, ty, options[i], hi ? DLG_HITEXT : DLG_TEXT, s);
            ty += lh;
        }
        return;
    }
    if (m->page == MENU_KEY_CAPTURE) {
        dlg_text(r, tx, ty, m->capture_controller ? "Press a controller button or stick direction..."
                                                  : "Press the key to use...", DLG_ACCENT, s);
        dlg_text(r, tx, ty + lh, m->capture_backspace_armed ? "(Backspace again to cancel)"
                                                            : "(Backspace to cancel)", DLG_HINT, s);
        return;
    }

    bool is_binding_menu = (m->list == MENU_LIST_INPUT || m->list == MENU_LIST_CONTROLS);
    for (int i = 0; i < m->n_items; i++) {
        bool hi = i == m->selected;
        uint32_t fg = hi ? DLG_HITEXT : DLG_TEXT;
        if (hi) dlg_fill(r, mx + 4 * s, ty - 2 * s, mw - 8 * s, lh, DLG_HI);
        dlg_text(r, tx, ty, m->items[i].label, fg, s);
        if (is_binding_menu && m->items[i].binding_ptr) {
            const char *kname = binding_display_name(m->items[i].binding_ptr);
            const char *cname = m->items[i].controller_ptr
                              ? binding_display_name(m->items[i].controller_ptr) : "(none)";
            char pair[192];
            snprintf(pair, sizeof(pair), "%s / %s", kname, cname);
            int x = mx + mw - 12 * s - (int)strlen(pair) * fw;
            if (x < tx + 14 * fw) x = tx + 14 * fw;
            dlg_text(r, x, ty, pair, hi ? DLG_HITEXT : DLG_ACCENT, s);
        }
        ty += lh;
    }
}

/* ── Event handling ─────────────────────────────────────────────────────── */

bool input_menu_handle_event(InputMenu *m, const SDL_Event *ev) {
    if (!m || m->page == MENU_CLOSED) return false;

    if (m->page == MENU_BINDING_OPTIONS) {
        bool go_up = false, go_down = false, accept = false, back = false;
        if (ev->type == SDL_KEYDOWN) {
            SDL_Keycode sym = ev->key.keysym.sym;
            go_up = sym == m->menu_key_bindings[MENU_CTL_UP].key;
            go_down = sym == m->menu_key_bindings[MENU_CTL_DOWN].key;
            accept = sym == m->menu_key_bindings[MENU_CTL_ACCEPT].key;
            back = sym == m->menu_key_bindings[MENU_CTL_BACK].key ||
                   sym == SDLK_BACKSPACE;
        } else if (binding_matches_controller(&m->menu_controller_bindings[MENU_CTL_UP], ev)) {
            go_up = true;
        } else if (binding_matches_controller(&m->menu_controller_bindings[MENU_CTL_DOWN], ev)) {
            go_down = true;
        } else if (binding_matches_controller(&m->menu_controller_bindings[MENU_CTL_ACCEPT], ev)) {
            accept = true;
        } else if (binding_matches_controller(&m->menu_controller_bindings[MENU_CTL_BACK], ev)) {
            back = true;
        } else {
            return true;
        }

        if (go_up) {
            m->option_selected = (m->option_selected + 2) % 3;
            return true;
        }
        if (go_down) {
            m->option_selected = (m->option_selected + 1) % 3;
            return true;
        }
        if (back) {
            m->page = MENU_MAIN;
            return true;
        }
        if (accept) {
            if (m->option_selected == 0) {
                m->page = MENU_MAIN;
            } else if (m->option_selected == 1) {
                if (is_menu_control_index(m->capture_index)) {
                    InputBinding *key = menu_capture_binding(m, false);
                    InputBinding *controller = menu_capture_binding(m, true);
                    if (key) binding_clear(key);
                    if (controller) binding_clear(controller);
                    save_menu_binding(m);
                } else {
                    binding_clear(&m->key_bindings[m->capture_index]);
                    binding_clear(&m->controller_bindings[m->capture_index]);
                    save_ini(m);
                }
                m->page = MENU_MAIN;
            } else {
                start_capture(m, m->capture_index, m->capture_controller);
            }
            return true;
        }
        return true;
    }

    /* Key capture mode: record the next non-modifier key */
    if (m->page == MENU_KEY_CAPTURE) {
        if (ev->type == SDL_CONTROLLERBUTTONDOWN ||
            ev->type == SDL_CONTROLLERAXISMOTION) {
            char name[INPUT_BINDING_NAME_MAX];
            if (!controller_binding_name(ev, name, sizeof(name)))
                return true;
            InputBinding *binding = is_menu_control_index(m->capture_index)
                                  ? menu_capture_binding(m, true)
                                  : &m->controller_bindings[m->capture_index];
            if (!binding) return true;
            binding_set_controller(binding, name);
            if (is_menu_control_index(m->capture_index))
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
        if (is_menu_control_index(m->capture_index)) {
            InputBinding *binding = menu_capture_binding(m, false);
            if (!binding) return true;
            binding_set_key(binding, sym);
            save_menu_binding(m);
        } else {
            binding_set_key(&m->key_bindings[m->capture_index], sym);
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

    bool go_up = false, go_down = false, go_left = false, go_right = false;
    bool accept = false, back = false;
    if (ev->type == SDL_KEYDOWN) {
        SDL_Keycode sym = ev->key.keysym.sym;
        go_up = sym == m->menu_key_bindings[MENU_CTL_UP].key;
        go_down = sym == m->menu_key_bindings[MENU_CTL_DOWN].key;
        go_left = sym == m->menu_key_bindings[MENU_CTL_LEFT].key;
        go_right = sym == m->menu_key_bindings[MENU_CTL_RIGHT].key;
        accept = sym == m->menu_key_bindings[MENU_CTL_ACCEPT].key;
        back = sym == m->menu_key_bindings[MENU_CTL_BACK].key;
    } else if (binding_matches_controller(&m->menu_controller_bindings[MENU_CTL_UP], ev)) {
        go_up = true;
    } else if (binding_matches_controller(&m->menu_controller_bindings[MENU_CTL_DOWN], ev)) {
        go_down = true;
    } else if (binding_matches_controller(&m->menu_controller_bindings[MENU_CTL_LEFT], ev)) {
        go_left = true;
    } else if (binding_matches_controller(&m->menu_controller_bindings[MENU_CTL_RIGHT], ev)) {
        go_right = true;
    } else if (binding_matches_controller(&m->menu_controller_bindings[MENU_CTL_ACCEPT], ev)) {
        accept = true;
    } else if (binding_matches_controller(&m->menu_controller_bindings[MENU_CTL_BACK], ev)) {
        back = true;
    } else {
        return m->page != MENU_CLOSED;
    }

    bool is_binding_menu = (m->list == MENU_LIST_INPUT ||
                            m->list == MENU_LIST_CONTROLS);

    if (go_up) {
        if (m->selected > 0)
            m->selected--;
        else
            m->selected = m->n_items - 1;
        return true;
    }
    if (go_down) {
        if (m->selected < m->n_items - 1)
            m->selected++;
        else
            m->selected = 0;
        return true;
    }
    if (go_left) {
        if (m->list == MENU_LIST_INPUT) input_menu_step(m, -1);
        return true;
    }
    if (go_right) {
        if (m->list == MENU_LIST_INPUT) input_menu_step(m, 1);
        return true;
    }
    if (accept) {
        if (is_binding_menu) {
            if (m->items[m->selected].binding_ptr) {
                start_binding_options(m, m->items[m->selected].binding_index,
                                      false);
            } else if (m->items[m->selected].action) {
                m->items[m->selected].action(m);
            } else if (m->list == MENU_LIST_CONTROLS) {
                build_input_menu(m);
                m->page = MENU_MAIN;
                m->selected = 0;
            } else {
                /* "(close)" on the Input page: the window menu bar opened
                 * this screen, so leaving it closes it. */
                m->page = MENU_CLOSED;
            }
        } else if (m->items[m->selected].action) {
            m->items[m->selected].action(m);
        }
        return true;
    }
    if (back) {
        if (m->list == MENU_LIST_CONTROLS) {
            build_input_menu(m);
            m->page = MENU_MAIN;
            m->selected = 0;
        } else if (m->list == MENU_LIST_INPUT) {
            m->page = MENU_CLOSED;
        } else {
            m->page = MENU_CLOSED;
        }
        return true;
    }

    return m->page != MENU_CLOSED;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

InputMenu *input_menu_create(SDL_Renderer *renderer,
                             int n_buttons,
                             const char **button_names,
                             const SDL_Keycode *default_bindings,
                             const char **default_controller_bindings,
                             const char *ini_section,
                             int n_pages,
                             const InputMenuPage *pages) {
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
    m->controller_button_names = calloc(n_buttons ? (size_t)n_buttons : 1,
                                        sizeof(*m->controller_button_names));
    if (!m->controller_button_names) { free(m->button_names); free(m); return NULL; }
    for (int i = 0; i < n_buttons; i++)
        snprintf(m->controller_button_names[i], INPUT_BINDING_NAME_MAX,
                 "%s Controller", m->button_names[i]);
    m->default_bindings = calloc(n_buttons ? (size_t)n_buttons : 1,
                                 sizeof(*m->default_bindings));
    if (!m->default_bindings) {
        free(m->controller_button_names); free(m->button_names); free(m); return NULL;
    }
    if (n_buttons > 0 && default_bindings)
        memcpy(m->default_bindings, default_bindings,
               (size_t)n_buttons * sizeof(*m->default_bindings));
    m->default_controller_bindings = calloc(n_buttons ? (size_t)n_buttons : 1,
                                            sizeof(*m->default_controller_bindings));
    if (!m->default_controller_bindings) {
        free(m->default_bindings); free(m->controller_button_names);
        free(m->button_names); free(m); return NULL;
    }
    if (n_buttons > 0 && default_controller_bindings) {
        for (int i = 0; i < n_buttons; i++)
            snprintf(m->default_controller_bindings[i], INPUT_BINDING_NAME_MAX,
                     "%s", default_controller_bindings[i] ? default_controller_bindings[i] : "");
    }
    /* allocate at least 1 to avoid malloc(0) portability issues */
    m->key_bindings = calloc(n_buttons ? (size_t)n_buttons : 1, sizeof(*m->key_bindings));
    m->controller_bindings = calloc(n_buttons ? (size_t)n_buttons : 1, sizeof(*m->controller_bindings));
    if (!m->key_bindings || !m->controller_bindings) {
        free(m->controller_bindings); free(m->key_bindings);
        free(m->default_controller_bindings); free(m->default_bindings);
        free(m->controller_button_names); free(m->button_names); free(m); return NULL;
    }
    if (n_buttons > 0 && default_bindings) {
        for (int i = 0; i < n_buttons; i++) {
            if (default_bindings[i] != SDLK_UNKNOWN)
                binding_set_key(&m->key_bindings[i], default_bindings[i]);
            else
                binding_clear(&m->key_bindings[i]);
        }
    }
    if (n_buttons > 0 && default_controller_bindings) {
        for (int i = 0; i < n_buttons; i++)
            binding_set_controller(&m->controller_bindings[i], default_controller_bindings[i]);
    }
    if (n_pages > 0 && pages) {
        m->pages = malloc((size_t)n_pages * sizeof(*m->pages));
        if (!m->pages) {
            free(m->controller_bindings); free(m->key_bindings);
            free(m->default_controller_bindings); free(m->default_bindings);
            free(m->controller_button_names); free(m->button_names); free(m);
            return NULL;
        }
        memcpy(m->pages, pages, (size_t)n_pages * sizeof(*m->pages));
        m->n_pages = n_pages;
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
    free(m->pages);
    free(m->button_names);
    free(m->controller_button_names);
    free(m->default_bindings);
    free(m->default_controller_bindings);
    free(m->key_bindings);
    free(m->controller_bindings);
    free(m);
}

void input_menu_open_input(InputMenu *m) {
    input_menu_open_page(m, 0);
}

int input_menu_page_count(const InputMenu *m) {
    if (!m || m->n_buttons == 0) return 0;
    return m->n_pages > 0 ? m->n_pages : 1;
}

void input_menu_page_label(const InputMenu *m, int page, char *buf, size_t len) {
    const char *name = page_title(m, page);
    int nth = 1;                    /* which occurrence of this name it is */
    for (int p = 0; p < page; p++)
        if (strcmp(page_title(m, p), name) == 0) nth++;
    if (nth > 1) snprintf(buf, len, "%s (%d)", name, nth);
    else         snprintf(buf, len, "%s", name);
}

void input_menu_open_page(InputMenu *m, int page) {
    if (!m) return;
    m->input_page = page;
    m->input_chunk = 0;
    build_input_menu(m);
    m->page = MENU_MAIN;
    m->selected = 0;
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
                                       const SDL_Event *ev) {
    if (ev->type != SDL_CONTROLLERBUTTONDOWN &&
        ev->type != SDL_CONTROLLERAXISMOTION)
        return false;

    char name[INPUT_BINDING_NAME_MAX];
    if (!controller_binding_name(ev, name, sizeof(name)))
        return false;
    if (strcasecmp(binding->name, name) == 0)
        return true;
    return false;
}

bool input_menu_event_opens(const InputMenu *m, const SDL_Event *ev) {
    if (!m || !ev) return false;
    if (ev->type == SDL_KEYDOWN)
        return m->menu_key_bindings[MENU_CTL_OPEN].key != SDLK_UNKNOWN &&
               ev->key.keysym.sym == m->menu_key_bindings[MENU_CTL_OPEN].key;
    return binding_matches_controller(&m->menu_controller_bindings[MENU_CTL_OPEN], ev);
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

