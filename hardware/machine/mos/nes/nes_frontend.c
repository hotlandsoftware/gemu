/*
 * The NES front end: the window and its input bindings, the -device crt
 * picture, the Debug and Netplay menus (hex editor, Game Genie, cartridge
 * pins; nes_netplay.c), R.O.B.'s window, Lua, battery and FDS save files,
 * media changes, savevm/loadvm, and the run loop with its pacing.  The
 * console itself is machine_nes.c, used through nes.h.
 */
#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L
#endif
#include "nes_frontend.h"
#include "gemu/util.h"
#include "storage/fds.h"
#include "nes_ntsc_encode.h"
#include "ntsc_decode.h"
#include "crt_tube.h"
#include "gemu/screendump.h"
#include "gemu/gemu_display.h"
#include "gemu/gemu.h"
#include "hex_editor.h"
#include "cheat_dialog.h"
#include "nes_pin_window.h"
#include "nes_cheats.h"
#ifdef HAVE_ROB
#  include "rob_window.h"
#endif
#ifdef HAVE_LUA
#  include "nes_lua.h"
#endif
#include <SDL2/SDL.h>  /* frame pacing to the audio queue, the battery prompt */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#ifdef _WIN32
#  include <direct.h>
#  define strcasecmp      _stricmp
#  define strncasecmp     _strnicmp
#else
#  include <sys/stat.h>
#  include <strings.h>
#endif
#ifdef GEMU_GTK
#  include <gtk/gtk.h>
#endif

/* -device crt: decoded output resolution. Matches the NES's native
 * 256x240 exactly -- SDL_RenderSetLogicalSize (core/src/video_sdl.c)
 * always preserves *this* aspect ratio via letterboxing regardless of
 * window shape, so any width other than RP2C02_WIDTH here distorts the
 * displayed picture into that ratio (an earlier attempt at 4x width for
 * "more decode detail" did exactly that -- stretched the whole image
 * widescreen). The extra horizontal detail from oversampled decoding
 * (see NES_NTSC_OVERSAMPLE in crt/nes_ntsc_encode.h) still improves the
 * signal recovery quality even when resampled back down to native width
 * here; it just isn't shown as literal extra pixels. */
#define NES_CRT_WIDTH  RP2C02_WIDTH
#define NES_CRT_HEIGHT RP2C02_HEIGHT

/* One NES per process: the front end's state is file-scope. */
static GemuDisplay *g_display;      /* display+input handle (NULL if headless) */
static HexEditor   *g_hex_editor;   /* Debug > Hex Editor */
static bool         g_battery_autosave;   /* user opted in to save SRAM on exit */
static char         g_sav_path[512];      /* ~/.gemu/<game>.sav or AppData equivalent */
#ifdef HAVE_ROB
static RobWindow   *g_rob_window;
#endif
#ifdef HAVE_LUA
static NesLua      *g_lua;          /* -lua FILE: FCEUX-Lua-subset script, NULL if none */
#endif
/* -device crt: NTSC composite encode+decode (see crt/nes_ntsc_encode.h,
 * crt/ntsc_decode.h). g_crt_signal is the oversampled composite scratch
 * buffer; g_crt_decoded_argb is the clean decoded signal; g_crt_argb is
 * the tube/display-profile output, presented instead of ppu.pixels_argb
 * when active; g_crt_dot_counter is the running count of visible dots
 * encoded so far, carried across frames so chroma phase (and the dot
 * crawl it produces) stays continuous.  Allocated in nes_create() only
 * when the device is attached. */
static float       *g_crt_signal;
static uint32_t    *g_crt_decoded_argb;
static uint32_t    *g_crt_argb;
static uint64_t     g_crt_dot_counter;

static void nes_hex_cart(NesState *s);

/* ── NES controller action table ─────────────────────────────────────────── */
/* GEMU_ACTION(n) = 1u<<n; NES_BTN_* happen to match, so ctrl_state[0] = held & 0xFF */
#define NES_N_ACTIONS 8
#define NES_2P_N_ACTIONS 16

static const GemuActionDef nes_actions[NES_N_ACTIONS] = {
    { "A",      GEMU_ACTION(0), "z"           },  /* NES_BTN_A      = 0x01 */
    { "B",      GEMU_ACTION(1), "x"           },  /* NES_BTN_B      = 0x02 */
    { "Select", GEMU_ACTION(2), "Right Shift" },  /* NES_BTN_SELECT = 0x04 */
    { "Start",  GEMU_ACTION(3), "Return"      },  /* NES_BTN_START  = 0x08 */
    { "Up",     GEMU_ACTION(4), "Up"          },  /* NES_BTN_UP     = 0x10 */
    { "Down",   GEMU_ACTION(5), "Down"        },  /* NES_BTN_DOWN   = 0x20 */
    { "Left",   GEMU_ACTION(6), "Left"        },  /* NES_BTN_LEFT   = 0x40 */
    { "Right",  GEMU_ACTION(7), "Right"       },  /* NES_BTN_RIGHT  = 0x80 */
};

#define NES_FC2MIC_N_ACTIONS 15
static const GemuActionDef nes_fc2mic_actions[NES_FC2MIC_N_ACTIONS] = {
    /* nes-controller page (P1: full controller with Select + Start) */
    { "A",         GEMU_ACTION(0),  "z"           },
    { "B",         GEMU_ACTION(1),  "x"           },
    { "Select",    GEMU_ACTION(2),  "Right Shift" },
    { "Start",     GEMU_ACTION(3),  "Return"      },
    { "Up",        GEMU_ACTION(4),  "Up"          },
    { "Down",      GEMU_ACTION(5),  "Down"        },
    { "Left",      GEMU_ACTION(6),  "Left"        },
    { "Right",     GEMU_ACTION(7),  "Right"       },
    /* famicom-mic page (P2: no Select/Start on FC2 hardware) */
    { "P2 A",      GEMU_ACTION(8),  "a"           },
    { "P2 B",      GEMU_ACTION(9),  "s"           },
    { "P2 Up",     GEMU_ACTION(12), "i"           },
    { "P2 Down",   GEMU_ACTION(13), "k"           },
    { "P2 Left",   GEMU_ACTION(14), "j"           },
    { "P2 Right",  GEMU_ACTION(15), "l"           },
    { "Send Mic",  GEMU_ACTION(16), "m"           },
};

static const GemuInputPage nes_fc2mic_pages[] = {
    { "nes-controller", 8 },   /* P1: A, B, Select, Start, Up, Down, Left, Right */
    { "famicom-mic",    7 },   /* P2: A, B, Up, Down, Left, Right, Send Mic */
};

static const GemuInputPage nes_2p_pages[] = {
    { "nes-controller", 8 },   /* P1 */
    { "nes-controller", 8 },   /* P2 */
};

static const GemuActionDef nes_2p_actions[NES_2P_N_ACTIONS] = {
    { "A",        GEMU_ACTION(0),  "z"           },
    { "B",        GEMU_ACTION(1),  "x"           },
    { "Select",   GEMU_ACTION(2),  "Right Shift" },
    { "Start",    GEMU_ACTION(3),  "Return"      },
    { "Up",       GEMU_ACTION(4),  "Up"          },
    { "Down",     GEMU_ACTION(5),  "Down"        },
    { "Left",     GEMU_ACTION(6),  "Left"        },
    { "Right",    GEMU_ACTION(7),  "Right"       },
    { "P2 A",      GEMU_ACTION(8),  "" },
    { "P2 B",      GEMU_ACTION(9),  "" },
    { "P2 Select", GEMU_ACTION(10), "" },
    { "P2 Start",  GEMU_ACTION(11), "" },
    { "P2 Up",     GEMU_ACTION(12), "" },
    { "P2 Down",   GEMU_ACTION(13), "" },
    { "P2 Left",   GEMU_ACTION(14), "" },
    { "P2 Right",  GEMU_ACTION(15), "" },
};

/* ── VS. System arcade actions ────────────────────────────────────────────── */
/* VS wiring: bit 2 is the cabinet Select/Start button, not NES Start (bit 3). */
#define VS_BTN_START NES_BTN_SELECT

/* Bit layout: 0-7 = P1 VS_UNI_JOYSTICK bits, 8-15 = P2, 16 = Coin1, 17 = Coin2, 18 = Svc */
#define VS_NES_N_ACTIONS 19
static const GemuActionDef vs_nes_actions[VS_NES_N_ACTIONS] = {
    { "P1 A",     GEMU_ACTION(8),  "z"      },
    { "P1 B",     GEMU_ACTION(9),  "x"      },
    { "P1 Start", GEMU_ACTION(10), "Return" },
    { "P1 Up",    GEMU_ACTION(12), "Up"     },
    { "P1 Down",  GEMU_ACTION(13), "Down"   },
    { "P1 Left",  GEMU_ACTION(14), "Left"   },
    { "P1 Right", GEMU_ACTION(15), "Right"  },
    { "P2 A",     GEMU_ACTION(0),  "d"      },
    { "P2 B",     GEMU_ACTION(1),  "f"      },
    { "P2 Start", GEMU_ACTION(2),  "g"      },
    { "P2 Up",    GEMU_ACTION(4),  "r"      },
    { "P2 Down",  GEMU_ACTION(5),  "v"      },
    { "P2 Left",  GEMU_ACTION(6),  "c"      },
    { "P2 Right", GEMU_ACTION(7),  "b"      },
    { "Coin 1",   GEMU_ACTION(16), "5"      },
    { "Coin 2",   GEMU_ACTION(17), "6"      },
    { "Service",  GEMU_ACTION(18), "F2"     },
};


/* Debug > Hex Editor menu callback. */
static void nes_hex_toggle(void *ud) {
    (void)ud;
    if (!g_hex_editor) return;
    if (hex_editor_is_visible(g_hex_editor))
        hex_editor_hide(g_hex_editor);
    else
        hex_editor_show(g_hex_editor);
}


/* ── Battery-backed SRAM persistence ────────────────────────────────────── */

void nes_ui_game_name(const char *path, char *out, size_t len) {
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    const char *dot = strrchr(name, '.');
    size_t n = dot ? (size_t)(dot - name) : strlen(name);
    if (n >= len) n = len - 1;
    memcpy(out, name, n);
    out[n] = '\0';
}

static void nes_build_sav_path(const char *game, char *out, size_t len) {
    char file[300];
    snprintf(file, sizeof(file), "%s.sav", game);
    gemu_config_path(out, len, file);
}

static void nes_build_fds_sav_path(const char *game, char *out, size_t len) {
    char file[300];
    snprintf(file, sizeof(file), "%s.fds.sav", game);
    gemu_config_path(out, len, file);
}

static bool nes_battery_prompt(void) {
#ifdef GEMU_GTK
    GtkWidget *dlg = gtk_message_dialog_new(
        NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "This game has a battery.\n"
        "Do you want GEMU to save data automatically?");
    gtk_window_set_title(GTK_WINDOW(dlg), "Battery-backed Save");
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    while (gtk_events_pending()) gtk_main_iteration_do(FALSE);
    return resp == GTK_RESPONSE_YES;
#else
    SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"  },
    };
    SDL_MessageBoxData data = {
        .flags       = SDL_MESSAGEBOX_INFORMATION,
        .window      = NULL,
        .title       = "Battery-backed Save",
        .message     = "This game has a battery.\n"
                       "Do you want GEMU to save data automatically?",
        .numbuttons  = 2,
        .buttons     = buttons,
        .colorScheme = NULL,
    };
    int choice = 0;
    SDL_ShowMessageBox(&data, &choice);
    return choice == 1;
#endif
}

static void nes_sav_load(NesState *s) {
    FILE *f = fopen(g_sav_path, "rb");
    if (!f) return;
    fread(s->cart.prg_ram, 1, sizeof(s->cart.prg_ram), f);
    fclose(f);
    printf("nes: loaded save '%s'\n", g_sav_path);
}

static void nes_sav_save(NesState *s) {
    if (!g_battery_autosave || !g_sav_path[0]) return;
    gemu_ensure_parent_dir(g_sav_path);
    FILE *f = fopen(g_sav_path, "wb");
    if (!f) { fprintf(stderr, "nes: cannot write save '%s'\n", g_sav_path); return; }
    fwrite(s->cart.prg_ram, 1, sizeof(s->cart.prg_ram), f);
    fclose(f);
    printf("nes: saved to '%s'\n", g_sav_path);
}

static void nes_fds_save(NesState *s) {
    if (!s->fds_enabled || !s->fds.dirty || !g_sav_path[0])
        return;
    gemu_ensure_parent_dir(g_sav_path);
    if (!fds_disk_save(&s->fds, g_sav_path))
        fprintf(stderr, "fds: cannot write save '%s'\n", g_sav_path);
}

static void nes_fds_save_setup(NesState *s, const char *disk_path) {
    if (!disk_path || !disk_path[0]) return;
    char game[256];
    nes_ui_game_name(disk_path, game, sizeof(game));
    nes_build_fds_sav_path(game, g_sav_path, sizeof(g_sav_path));
    fds_disk_load_save(&s->fds, g_sav_path);
}

void nes_ui_save_persistent(NesState *s) {
    if (s->fds_enabled)
        nes_fds_save(s);
    else
        nes_sav_save(s);
}

static void nes_battery_setup(NesState *s) {
    if (!s->cart.info.has_battery) return;
    char game[256];
    nes_ui_game_name(s->cart_path_buf, game, sizeof(game));
    nes_build_sav_path(game, g_sav_path, sizeof(g_sav_path));
    FILE *existing = fopen(g_sav_path, "rb");
    if (existing) {
        fclose(existing);
        g_battery_autosave = true;
        nes_sav_load(s);
    } else {
        g_battery_autosave = nes_battery_prompt();
    }
}


/* ── Game Genie ──────────────────────────────────────────────────────────── *
 *
 * The cheat list (Debug > Game Genie and the monitor's "gamegenie") lives
 * outside NesState - snapshots store NesState raw, and cheats are the
 * player's settings rather than machine state, as in FCE Ultra.  The ROM
 * patches the CPU applies (gg_patches) are rebuilt from its enabled codes.
 * One NES per process, so file-scope state is fine. */

static NesCheats    g_cheats;
static CheatDialog *g_cheat_dialog;


static void nes_cheats_apply(void *ud) {
    NesState *s = ud;
    s->gg_count = 0;
    nes_gg_changed(s);
    if (nes_np_active()) return;   /* codes would change one player's game only */
    for (int i = 0; i < g_cheats.n && s->gg_count < NES_GG_MAX; i++) {
        if (!g_cheats.list[i].enabled) continue;
        NesGgPatch *g = &s->gg_patches[s->gg_count];
        if (!nes_gg_decode(g_cheats.list[i].code, &g->addr, &g->val, &g->cmp, &g->has_cmp)) continue;
        snprintf(g->code, sizeof(g->code), "%s", g_cheats.list[i].code);
        s->gg_count++;
    }
    nes_gg_changed(s);
}

/* This cartridge's codes, from <config>/<game>.cht. */
static void nes_cheats_load(NesState *s) {
    char path[512] = "";
    if (s->cart_path_buf[0] && !s->cfg->is_arcade) {
        char game[256], file[300];
        nes_ui_game_name(s->cart_path_buf, game, sizeof(game));
        snprintf(file, sizeof(file), "%s.cht", game);
        gemu_config_path(path, sizeof(path), file);
    }
    nes_cheats_init(&g_cheats, path, nes_cheats_apply, s);
    nes_cheats_apply(s);
    if (g_cheats.n)
        printf("nes: %d Game Genie code%s from %s\n", g_cheats.n, g_cheats.n == 1 ? "" : "s", path);
}

static NesPinWindow *g_pin_window;   /* Debug > Cartridge Pins */

static void nes_pin_window_toggle_cb(void *ud) {
    (void)ud;
    nes_pin_window_toggle(g_pin_window);
}

static void nes_cheat_dialog_toggle(void *ud) {
    (void)ud;
    cheat_dialog_toggle(g_cheat_dialog);
}


static void gg_print(NesState *s, const NesCheat *e) {
    uint16_t a; uint8_t v, k; bool h;
    if (!nes_gg_decode(e->code, &a, &v, &k, &h)) return;
    uint8_t rom = s->cart.prg && a >= 0x8000 ? nes_cart_prg(&s->cart, a) : 0;
    const char *state = !e->enabled ? "off" : (h && rom != k) ? "on, compare mismatch" : "on";
    if (h)
        gemu_monitor_printf(s->monitor, "  %-8s  $%04X = $%02X  (if $%02X)  ROM=$%02X [%s]  %s\n",
                            e->code, a, v, k, rom, state, e->name);
    else
        gemu_monitor_printf(s->monitor, "  %-8s  $%04X = $%02X  ROM=$%02X [%s]  %s\n",
                            e->code, a, v, rom, state, e->name);
}

/* line = arguments after "gamegenie":
 *   list [code] | add <code> [name] | delete <code> | on <code> | off <code> */
static void nes_gamegenie_cmd(NesState *s, const char *line) {
    while (*line && isspace((unsigned char)*line)) line++;
    char sub[32] = {0}, code[32] = {0};
    int si = 0;
    while (*line && !isspace((unsigned char)*line) && si < 31)
        sub[si++] = (char)tolower((unsigned char)*line++);
    while (*line && isspace((unsigned char)*line)) line++;
    si = 0;
    while (*line && !isspace((unsigned char)*line) && si < 31) code[si++] = *line++;
    while (*line && isspace((unsigned char)*line)) line++;
    char name[64];
    snprintf(name, sizeof(name), "%s", line);
    for (int i = (int)strlen(name) - 1; i >= 0 && isspace((unsigned char)name[i]); i--) name[i] = '\0';
    char err[160];

    if (strcmp(sub, "list") == 0) {
        if (code[0]) {
            int i = nes_cheats_find(&g_cheats, code);
            if (i < 0) gemu_monitor_printf(s->monitor, "gamegenie: code '%s' not found\n", code);
            else gg_print(s, &g_cheats.list[i]);
            return;
        }
        if (g_cheats.n == 0) { gemu_monitor_printf(s->monitor, "gamegenie: no codes\n"); return; }
        gemu_monitor_printf(s->monitor, "Game Genie codes:\n");
        for (int i = 0; i < g_cheats.n; i++) gg_print(s, &g_cheats.list[i]);
        return;
    }
    if (strcmp(sub, "add") == 0) {
        if (!code[0]) { gemu_monitor_printf(s->monitor, "usage: gamegenie add <code> [name]\n"); return; }
        if (!nes_cheats_add(&g_cheats, code, name, true, err, sizeof(err))) {
            gemu_monitor_printf(s->monitor, "gamegenie: %s\n", err);
            return;
        }
        gemu_monitor_printf(s->monitor, "gamegenie: added\n");
        gg_print(s, &g_cheats.list[g_cheats.n - 1]);
        return;
    }
    int i = code[0] ? nes_cheats_find(&g_cheats, code) : -1;
    if (strcmp(sub, "delete") == 0 || strcmp(sub, "remove") == 0 ||
        strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0) {
        if (i < 0) {
            gemu_monitor_printf(s->monitor, code[0] ? "gamegenie: code '%s' not found\n"
                                                    : "usage: gamegenie %s <code>\n", code[0] ? code : sub);
            return;
        }
        if (sub[0] == 'o') {
            nes_cheats_set_enabled(&g_cheats, i, sub[1] == 'n');
            gemu_monitor_printf(s->monitor, "gamegenie: %s %s\n", g_cheats.list[i].code, sub[1] == 'n' ? "on" : "off");
        } else {
            gemu_monitor_printf(s->monitor, "gamegenie: removed %s\n", g_cheats.list[i].code);
            nes_cheats_remove(&g_cheats, i);
        }
        return;
    }
    gemu_monitor_printf(s->monitor, "gamegenie list [code] | add <code> [name] | delete <code> | on <code> | off <code>\n"
                                    "(Debug > Game Genie in the window does the same)\n");
}


#ifdef HAVE_LUA
/* Only installed as cpu.mem_read/mem_write when -lua is active, so a script
 * with no memory.register{read,write}() hooks costs nothing on the hot path. */
static void nes_lua_exec(void *ud, uint16_t pc) { nes_lua_notify_exec(ud, pc); }
static uint8_t nes_cpu_read_lua(uint16_t addr, void *ud) {
    uint8_t val = nes_cpu_read(addr, ud);
    nes_lua_notify_read(g_lua, addr, val);
    return val;
}
static void nes_cpu_write_lua(uint16_t addr, uint8_t val, void *ud) {
    nes_cpu_write(addr, val, ud);
    nes_lua_notify_write(g_lua, addr, val);
}
static uint64_t nes_lua_get_framecount(void *ud) {
    NesState *s = ud;
    return s->ppu.frame;
}
static void nes_lua_get_zapper(void *ud, int *x, int *y, int *click) {
    NesState *s = ud;
    *x = s->zapper_x;
    *y = s->zapper_y;
    /* Report the live mouse-button state, not the Zapper's decaying 10-frame
     * hardware trigger pulse (nes_handle_keys()) - a Lua drag-and-drop
     * script needs "still held" to stay true for as long as the button is
     * actually down, not just the first 10 frames of the click. */
    GemuPointerState ptr = gemu_display_get_pointer(g_display);
    *click = ptr.button ? 1 : 0;
}
#endif


/* ── FDS media device ────────────────────────────────────────────────────── */

static GemuMediaResult fds_media_change(void *ud, const char *arg,
                                         char *err, size_t err_len) {
    NesState *s = ud;
    if (!arg || !arg[0]) { snprintf(err, err_len, "missing disk path"); return GEMU_MEDIA_ERR; }
    nes_fds_save(s);
    g_sav_path[0] = '\0';
    fds_disk_eject(&s->fds);
    if (!fds_disk_load(&s->fds, arg)) {
        snprintf(err, err_len, "failed to load '%s'", arg);
        return GEMU_MEDIA_ERR;
    }
    nes_fds_save_setup(s, arg);
    nes_fds_disk_inserted(s);
    return GEMU_MEDIA_OK;
}

static GemuMediaResult fds_media_eject(void *ud, char *err, size_t err_len) {
    (void)err; (void)err_len;
    NesState *s = ud;
    nes_fds_save(s);
    g_sav_path[0] = '\0';
    fds_disk_eject(&s->fds);
    return GEMU_MEDIA_OK;
}

static GemuMediaResult fds_media_flip(void *ud, char *err, size_t err_len) {
    NesState *s = ud;
    if (!s->fds.disk_inserted) {
        snprintf(err, err_len, "no disk inserted");
        return GEMU_MEDIA_ERR;
    }
    if (s->fds.disk_sides < 2) {
        snprintf(err, err_len, "disk has only one side");
        return GEMU_MEDIA_ERR;
    }
    return fds_disk_flip(&s->fds) ? GEMU_MEDIA_OK : GEMU_MEDIA_ERR;
}

static void fds_media_status(void *ud, char *buf, size_t buf_len) {
    const NesState *s = ud;
    if (!s->fds.disk_inserted)
        snprintf(buf, buf_len, "no disk");
    else if (s->fds.disk_change_cycles > 0)
        snprintf(buf, buf_len, "changing disk, side %c (%u/%u)",
                 (char)('A' + s->fds.cur_side),
                 (unsigned)s->fds.cur_side + 1u,
                 (unsigned)s->fds.disk_sides);
    else
        snprintf(buf, buf_len, "disk inserted, side %c (%u/%u)",
                 (char)('A' + s->fds.cur_side),
                 (unsigned)s->fds.cur_side + 1u,
                 (unsigned)s->fds.disk_sides);
}


static GemuMediaResult nes_media_change(void *ud, const char *arg,
                                         char *err, size_t err_len) {
    NesState *s = ud;
    if (!arg || !arg[0]) {
        snprintf(err, err_len, "missing cartridge path");
        return GEMU_MEDIA_ERR;
    }
    if (nes_np_active()) {
        snprintf(err, err_len, "can't change the cartridge during a netplay game");
        return GEMU_MEDIA_ERR;
    }
    nes_sav_save(s);
    g_battery_autosave = false;
    g_sav_path[0] = '\0';
    nes_eject_cartridge(s);
    if (!nes_insert_cartridge(s, arg)) {
        snprintf(err, err_len, "failed to load '%s'", arg);
        return GEMU_MEDIA_ERR;
    }
    nes_battery_setup(s);
    nes_cheats_load(s);         /* this cartridge's Game Genie codes */
    nes_hex_cart(s);
    nes_np_cart_changed(s);     /* listed under its own name; not a fresh power-on any more */
    return GEMU_MEDIA_OK_RESET;
}

static GemuMediaResult nes_media_eject(void *ud, char *err, size_t err_len) {
    (void)err; (void)err_len;
    NesState *s = ud;
    nes_sav_save(s);
    g_battery_autosave = false;
    g_sav_path[0] = '\0';
    nes_eject_cartridge(s);
    nes_hex_cart(s);
    printf("cartridge: ejected\n");
    return GEMU_MEDIA_OK;
}

static void nes_media_status(void *ud, char *buf, size_t buf_len) {
    const NesState *s = ud;
    snprintf(buf, buf_len, "%s",
             s->cart_path_buf[0] ? s->cart_path_buf : "no cartridge");
}


/* ── Controller input ────────────────────────────────────────────────────── */

/* held: bitmask returned by gemu_display_poll() this frame (0 if headless) */
static void nes_handle_keys(NesState *s, uint32_t held) {
    if (s->cfg->is_arcade) {
        /* VS. arcade: bits 0-7 = P1, 8-15 = P2, 16 = Coin1, 17 = Coin2, 18 = Service */
        if (g_display) {
            s->ctrl_state[0] = (uint8_t)(held & 0xFFu);
            s->ctrl_state[1] = (uint8_t)((held >> 8) & 0xFFu);
            bool coin1 = (held >> 16) & 1;
            bool coin2 = (held >> 17) & 1;
            s->vs_service = (held >> 18) & 1;
            if (coin1 && !s->vs_coin_prev[0]) s->vs_coin_latch[0] = 15;
            if (coin2 && !s->vs_coin_prev[1]) s->vs_coin_latch[1] = 15;
            s->vs_coin_prev[0] = coin1;
            s->vs_coin_prev[1] = coin2;
        }
        if (s->ctrl_inject_frames > 0) {
            s->ctrl_state[0] |= s->ctrl_inject_mask;
            s->ctrl_inject_frames--;
        }
        return;
    }

    if (!g_display) {
        /* Headless: no input source */
        s->ctrl_state[0] = 0; s->ctrl_state[1] = 0;
    } else {
        /* Action bits 0-7 and 8-15 map directly to NES_BTN_* for ports 1/2. */
        if (s->cfg->ports[0] == NES_DEVICE_CONTROLLER)
            s->ctrl_state[0] = (uint8_t)(held & 0xFFu);
        else
            s->ctrl_state[0] = 0;
        if (s->cfg->ports[1] == NES_DEVICE_CONTROLLER ||
            s->cfg->ports[1] == NES_DEVICE_FC2_MIC)
            s->ctrl_state[1] = (uint8_t)((held >> 8) & 0xFFu);
        else if (!nes_device_is_rob(s->cfg->ports[1]))
            s->ctrl_state[1] = 0;
        if (s->cfg->ports[1] == NES_DEVICE_FC2_MIC)
            s->mic_noise = (held & GEMU_ACTION(16)) != 0;
        /* Track host pointer position unconditionally (not just when a real
         * NES Zapper is attached) so Lua scripts using input.get()/
         * zapper.read() for mouse-driven GUIs (drag-and-drop tools, etc.)
         * see live coordinates even on a plain controller-only setup. */
        GemuPointerState ptr = gemu_display_get_pointer(g_display);
        s->zapper_x = ptr.x;
        s->zapper_y = ptr.y;
        if (s->cfg->ports[1] == NES_DEVICE_ZAPPER) {
            /* Real light-gun games need the trigger held "on" for a short
             * pulse for the CRT light-sensing to register - a single-frame
             * click would be invisible to the game's polling loop. */
            if (ptr.button && s->zapper_trigger_ttl == 0)
                s->zapper_trigger_ttl = 10; /* 10-frame trigger pulse */
            if (s->zapper_trigger_ttl > 0)
                s->zapper_trigger_ttl--;
        }
    }

    /* Monitor key injection */
    if (s->ctrl_inject_frames > 0) {
        s->ctrl_state[0] |= s->ctrl_inject_mask;
        s->ctrl_inject_frames--;
    }
}

/* ── Screendump ──────────────────────────────────────────────────────────── */

static bool nes_screendump(void *ud, const char *path) {
    NesState *s = ud;
    if (s->cfg->crt_type != MOS_CRT_NONE && g_crt_argb)
        return gemu_screendump_argb(path, g_crt_argb, NES_CRT_WIDTH, NES_CRT_HEIGHT);
    int w = RP2C02_WIDTH, h = RP2C02_HEIGHT;
    uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return false;
    const uint32_t *palette = s->ppu.alt_palette_rgb ? s->ppu.alt_palette_rgb
                                                     : rp2c02_palette_rgb;
    for (int i = 0; i < w * h; i++) {
        uint32_t c    = palette[s->ppu.pixels[i] & 0x3F];
        rgb[i*3+0]    = (uint8_t)(c >> 16);
        rgb[i*3+1]    = (uint8_t)(c >>  8);
        rgb[i*3+2]    = (uint8_t)(c      );
    }
    bool ok = gemu_screendump(path, rgb, w, h);
    free(rgb);
    return ok;
}


static void nes_monitor_setup(NesState *s);

/* Debug > Hex Editor: tab 0 the CPU's RAM, 1 and 2 the cartridge's PRG
 * and CHR (nes_hex_cart after a change). */
static void nes_hex_create(NesState *s) {
    HexRegion regions[] = {
        { "CPU RAM ($0000-$07FF)", s->ram, sizeof(s->ram), false, 0x0000 },
        { "PRG ROM ($8000+)", s->cart.prg, nes_cart_prg_size(&s->cart), true, 0x8000 },
        { "CHR ROM/RAM", s->cart.chr, nes_cart_chr_size(&s->cart), !s->cart.chr_is_ram, 0x0000 },
    };
    /* NULL for the display in use (none with -display none or curses) */
    g_hex_editor = hex_editor_create(s->cfg->display_type, regions, 3);
}

/* Another cartridge in the slot (or none): the PRG and CHR tabs follow. */
static void nes_hex_cart(NesState *s) {
    hex_editor_set_region(g_hex_editor, 1, s->cart.prg, nes_cart_prg_size(&s->cart), true);
    hex_editor_set_region(g_hex_editor, 2, s->cart.chr, nes_cart_chr_size(&s->cart),
                          !s->cart.chr_is_ram);
}

NesState *nes_create(const MosConfig *cfg) {
    NesState *s = nes_core_create(cfg);
    if (!s) return NULL;
    if (s->fds_enabled && cfg->fda_path) {
        nes_fds_save_setup(s, cfg->fda_path);   /* the disk's save data, then the boot */
        nes_fds_disk_inserted(s);
    }

    nes_monitor_setup(s);
    gemu_monitor_set_screendump_cb(s->monitor, nes_screendump, s);
    nes_hex_create(s);
    if (s->fds_enabled) {
        GemuMediaDevice floppy_dev = {
            .name   = "floppy",
            .kind   = "floppy",
            .ud     = s,
            .change = fds_media_change,
            .eject  = fds_media_eject,
            .flip   = fds_media_flip,
            .status = fds_media_status,
        };
        if (cfg->fda_path)
            snprintf(floppy_dev.file, sizeof(floppy_dev.file), "%s", cfg->fda_path);
        gemu_monitor_register_media(s->monitor, &floppy_dev);
    } else {
        GemuMediaDevice cart_dev = {
            .name   = "cartridge",
            .kind   = "cartridge",
            .ud     = s,
            .change = nes_media_change,
            .eject  = nes_media_eject,
            .status = nes_media_status,
        };
        if (s->cart_path_buf[0])
            snprintf(cart_dev.file, sizeof(cart_dev.file), "%s", s->cart_path_buf);
        gemu_monitor_register_media(s->monitor, &cart_dev);
    }

    if (gemu_display_wanted(cfg->display_type)) {
        bool two_controllers = cfg->ports[0] == NES_DEVICE_CONTROLLER &&
                               cfg->ports[1] == NES_DEVICE_CONTROLLER;
        bool has_fc2_mic     = cfg->ports[1] == NES_DEVICE_FC2_MIC;
        const GemuActionDef  *acts      = nes_actions;
        int                   n_acts    = NES_N_ACTIONS;
        const GemuInputPage  *act_pages = NULL;
        int                   n_pages   = 0;
        const char           *ini_sec   = "nes-controller";
        if (cfg->is_arcade) {
            acts    = vs_nes_actions;
            n_acts  = VS_NES_N_ACTIONS;
            ini_sec = "vssmb";
        } else if (two_controllers) {
            acts      = nes_2p_actions; n_acts  = NES_2P_N_ACTIONS;
            act_pages = nes_2p_pages;   n_pages = 2;
        } else if (has_fc2_mic) {
            acts      = nes_fc2mic_actions; n_acts  = NES_FC2MIC_N_ACTIONS;
            act_pages = nes_fc2mic_pages;   n_pages = 2;
        }
        g_display = gemu_display_create(cfg->display_type,
            &(GemuDisplayConfig){
                .fb_width    = NES_CRT_WIDTH,
                .fb_height   = NES_CRT_HEIGHT,
                .scale       = cfg->display_scale,
                .renderer    = cfg->display_renderer,
                .actions     = acts,
                .n_actions   = n_acts,
                .ini_section = ini_sec,
                .pages       = act_pages,
                .n_pages     = n_pages,
                .gtk         = &(GemuDisplayGtkExtras){
                    .monitor       = s->monitor,
                    .hex_toggle_cb = nes_hex_toggle,
                    .hex_toggle_ud = s,
                },
            });
        if (!g_display)
            fprintf(stderr, "nes: failed to create display window\n");
        /* Debug > Game Genie (a Game Genie plugs into a home console only) */
        if (g_display && !cfg->is_arcade) {
#ifdef GEMU_GTK
            if (cfg->display_type == GEMU_DISPLAY_GTK)
                g_cheat_dialog = cheat_dialog_gtk_create(&g_cheats);
#endif
            if (cfg->display_type == GEMU_DISPLAY_SDL)
                g_cheat_dialog = cheat_dialog_sdl_create(&g_cheats);
            if (g_cheat_dialog)
                gemu_display_add_debug_item(g_display, "Game Genie...", nes_cheat_dialog_toggle, s);
            /* Debug > Cartridge Pins (a VS. board has no cartridge connector) */
#ifdef GEMU_GTK
            if (cfg->display_type == GEMU_DISPLAY_GTK)
                g_pin_window = nes_pin_window_gtk_create(&s->pins);
#endif
            if (cfg->display_type == GEMU_DISPLAY_SDL)
                g_pin_window = nes_pin_window_sdl_create(&s->pins);
            if (g_pin_window)
                gemu_display_add_debug_item(g_display, "Cartridge Pins...", nes_pin_window_toggle_cb, s);
        }
        else
            gemu_monitor_set_input_reset_cb(s->monitor,
                                            gemu_display_reset_input_bindings_ud,
                                            g_display);
    }

    /* -device crt: independent of display_type -- also drives screendump
     * on headless "-display none" runs, so it must not be gated on a
     * display existing. */
    if (cfg->crt_type != MOS_CRT_NONE) {
        g_crt_signal = calloc((size_t)RP2C02_WIDTH * NES_NTSC_OVERSAMPLE *
                               RP2C02_HEIGHT, sizeof(float));
        g_crt_decoded_argb = calloc((size_t)NES_CRT_WIDTH * NES_CRT_HEIGHT,
                                     sizeof(uint32_t));
        g_crt_argb = calloc((size_t)NES_CRT_WIDTH * NES_CRT_HEIGHT,
                             sizeof(uint32_t));
        if (!g_crt_signal || !g_crt_decoded_argb || !g_crt_argb) {
            fprintf(stderr, "nes: -device crt: out of memory, disabling\n");
            free(g_crt_signal);       g_crt_signal = NULL;
            free(g_crt_decoded_argb); g_crt_decoded_argb = NULL;
            free(g_crt_argb);         g_crt_argb   = NULL;
        }
    }

    if (!s->fds_enabled)
        nes_battery_setup(s);
    nes_cheats_load(s);

#ifdef HAVE_ROB
    for (int p = 0; p < cfg->n_ports; p++) {
        if (nes_device_is_rob(cfg->ports[p])) {
            g_rob_window = rob_window_create("mdl/rob",
                                              cfg->ports[p] == NES_DEVICE_ROB_FAMICOM);
            break;
        }
    }
#else
    for (int p = 0; p < cfg->n_ports; p++)
        if (nes_device_is_rob(cfg->ports[p])) {
            fprintf(stderr, "gemu: R.O.B. needs the 3D window, which this build lacks "
                            "(install assimp, then re-run configure)\n");
            break;
        }
#endif

#ifdef HAVE_LUA
    if (cfg->lua_path) {
        NesLuaBus bus = {
            .mem_read       = nes_cpu_read,
            .mem_write      = nes_cpu_write,
            .get_framecount = nes_lua_get_framecount,
            .get_zapper     = nes_lua_get_zapper,
            .ud             = s,
            .rom_path       = s->cart_path_buf[0] ? s->cart_path_buf : NULL,
        };
        g_lua = nes_lua_create(cfg->lua_path, &bus);
        if (g_lua) {
            s->cpu.mem_read  = nes_cpu_read_lua;
            s->cpu.mem_write = nes_cpu_write_lua;
            s->exec_hook     = nes_lua_exec;
            s->exec_hook_ud  = g_lua;
        }
    }
#endif

    nes_reset(s);
    nes_np_setup(s, g_display);

    return s;
}

void nes_destroy(NesState *s) {
    nes_np_destroy(s);          /* the player's own game back, then save it */
    nes_ui_save_persistent(s);
#ifdef HAVE_LUA
    nes_lua_destroy(g_lua);
#endif
    hex_editor_destroy(g_hex_editor);
    if (g_cheat_dialog) { g_cheat_dialog->destroy(g_cheat_dialog); g_cheat_dialog = NULL; }
    if (g_pin_window) { g_pin_window->destroy(g_pin_window); g_pin_window = NULL; }
    nes_core_destroy(s);
#ifdef HAVE_ROB
    rob_window_destroy(g_rob_window);  /* must happen before display destroy */
#endif
    gemu_display_destroy(g_display);
    free(g_crt_signal);
    free(g_crt_decoded_argb);
    free(g_crt_argb);
}


bool nes_ui_load_state(NesState *s, const uint8_t *data, size_t len) {
    if (!nes_snapshot_load(s, data, len)) return false;
    nes_cheats_apply(s);    /* the codes that are on now, not the snapshot's */
    return true;
}

static bool nes_save_snapshot(NesState *s, const char *path) {
    uint8_t *data;
    size_t len;
    if (!nes_snapshot_save(s, &data, &len)) return false;
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(data, 1, len, f) == len;
    if (f && fclose(f) != 0) ok = false;
    free(data);
    return ok;
}

static bool nes_load_snapshot(NesState *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *data = NULL;
    size_t n = 0, cap = 0, got;
    do {
        if (n == cap) {
            cap = cap ? cap * 2 : 1u << 20;
            uint8_t *p = realloc(data, cap);
            if (!p) { free(data); fclose(f); return false; }
            data = p;
        }
        got = fread(data + n, 1, cap - n, f);
        n += got;
    } while (got);
    fclose(f);
    bool ok = nes_ui_load_state(s, data, n);
    free(data);
    return ok;
}


static GemuMonCmd nes_cmd_savevm(GemuMonitor *mon, const char *args, void *ud) {
    NesState *s = ud;
    char vm_path[256];
    if (sscanf(args, "%255s", vm_path) != 1) {
        gemu_monitor_printf(mon, "usage: savevm <file>\n");
        return GEMU_MON_NONE;
    }
    if (nes_save_snapshot(s, vm_path))
        gemu_monitor_printf(mon, "saved snapshot to %s (frame=%" PRIu64 ")\n",
                            vm_path, s->ppu.frame);
    else
        gemu_monitor_printf(mon, "failed to save snapshot to %s\n", vm_path);
    return GEMU_MON_NONE;
}

static GemuMonCmd nes_cmd_loadvm(GemuMonitor *mon, const char *args, void *ud) {
    NesState *s = ud;
    char vm_path[256];
    if (sscanf(args, "%255s", vm_path) != 1) {
        gemu_monitor_printf(mon, "usage: loadvm <file>\n");
        return GEMU_MON_NONE;
    }
    if (nes_np_active()) {
        gemu_monitor_printf(mon, "loadvm: not during a netplay game\n");
        return GEMU_MON_NONE;
    }
    if (nes_load_snapshot(s, vm_path))
        gemu_monitor_printf(mon, "loaded snapshot from %s (frame=%" PRIu64 ")\n",
                            vm_path, s->ppu.frame);
    else
        gemu_monitor_printf(mon, "failed to load snapshot from %s\n", vm_path);
    return GEMU_MON_NONE;
}


static GemuMonCmd nes_cmd_gamegenie(GemuMonitor *mon, const char *args, void *ud) {
    (void)mon;
    nes_gamegenie_cmd(ud, args);
    return GEMU_MON_NONE;
}


static GemuMonCmd nes_cmd_hexeditor(GemuMonitor *mon, const char *args, void *ud) {
    (void)args; (void)ud;
    if (!g_hex_editor) {
        gemu_monitor_printf(mon, "hexeditor: not available with this display\n");
    } else if (hex_editor_is_visible(g_hex_editor)) {
        hex_editor_hide(g_hex_editor);
        gemu_monitor_printf(mon, "hexeditor: closed\n");
    } else {
        hex_editor_show(g_hex_editor);
        gemu_monitor_printf(mon, "hexeditor: opened\n");
    }
    return GEMU_MON_NONE;
}

/* cartpin [list | <pin> good|open|flaky | tilt <deg> | reinsert | clock <Hz>|default] */

/* The front end's monitor commands (the console's: machine_nes.c) */
static void nes_monitor_setup(NesState *s) {
    GemuMonitor *mon = s->monitor;
    gemu_monitor_bind(mon, "savevm",     nes_cmd_savevm,     s);
    gemu_monitor_bind(mon, "loadvm",     nes_cmd_loadvm,     s);
    /* A Game Genie plugs into a home console's cartridge slot, not a VS. cabinet. */
    if (!s->cfg->is_arcade)
        gemu_monitor_bind(mon, "gamegenie", nes_cmd_gamegenie, s);
    gemu_monitor_bind(mon, "hexeditor",  nes_cmd_hexeditor,  s);
}


/* Show the PPU's picture: at the end of every frame, and after a monitor
 * "step" (mid-frame: the lines drawn so far over the last frame's), so the
 * window and screendump follow single-stepping while paused.
 *
 * -device crt: re-derive the frame from the PPU's raw hue+luma values
 * (ppu.pixels[], not the already-quantized pixels_argb) through a genuine
 * NTSC composite encode+decode into crt_argb. Independent of g_display so
 * screendump still gets a decoded frame on headless "-display none" runs.
 * Note this means anything drawn as an emulator-only overlay straight into
 * pixels_argb (e.g. the Lua gui.* API) won't appear in CRT mode — correctly
 * so, since it was never part of the real signal.  end_of_frame advances
 * the subcarrier phase (dot crawl); a redraw after a step must not, or
 * stepping would change every later frame. */
static void nes_present_frame(NesState *s, const MosConfig *cfg, bool end_of_frame) {
    if (cfg->crt_type != MOS_CRT_NONE && g_crt_argb && g_crt_decoded_argb) {
        NesNtscEncodeSpec enc_spec = {
            .oversample = NES_NTSC_OVERSAMPLE,
            .emphasis   = (uint8_t)(s->ppu.ppumask & 0xE0u),
        };
        nes_ntsc_encode(&enc_spec, s->ppu.pixels, RP2C02_WIDTH, RP2C02_HEIGHT,
                        g_crt_dot_counter, g_crt_signal);
        if (end_of_frame)
            g_crt_dot_counter += (uint64_t)RP2C02_WIDTH * RP2C02_HEIGHT;

        NtscDecodeSpec dec_spec = {
            .samples_per_line = RP2C02_WIDTH * NES_NTSC_OVERSAMPLE,
            .lines            = RP2C02_HEIGHT,
            .subcarrier_cycles_per_sample =
                nes_ntsc_subcarrier_cycles_per_sample(NES_NTSC_OVERSAMPLE),
            .out_width        = NES_CRT_WIDTH,
            .comb_filter      = cfg->crt_type == MOS_CRT_COMB,
        };
        ntsc_decode(&dec_spec, g_crt_signal, g_crt_decoded_argb);
        crt_tube_apply(crt_tube_profile_generic(), g_crt_decoded_argb,
                       g_crt_argb, NES_CRT_WIDTH, NES_CRT_HEIGHT);
    }

    /* PPU already builds pixels_argb. */
    if (g_display && cfg->crt_type != MOS_CRT_NONE && g_crt_argb) {
        gemu_display_render(g_display, g_crt_argb,
                            NES_CRT_WIDTH, NES_CRT_HEIGHT);
    } else if (g_display) {
        gemu_display_render(g_display, s->ppu.pixels_argb,
                            RP2C02_WIDTH, RP2C02_HEIGHT);
    }
}

/* Fallback frame duration when audio is off (headless / -soundhw none). */

/* Audio queue threshold for sync: allow up to 3 frames of latency (~50 ms).
 * ~735 samples/frame * 3 frames * 4 bytes/float = 8820 bytes */
#define AUDIO_QUEUE_MAX  (3u * 735u * (unsigned)sizeof(float))

/* Reset: in a netplay game every machine resets together. */
static void nes_reset_or_np(NesState *s) {
    if (nes_np_take_reset()) return;
    nes_ui_save_persistent(s);
    nes_reset(s);
}

void nes_run(NesState *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        nes_apply_pins(s);

        /* Poll display: pump events, update held bitmask, check quit/reset */
        uint32_t held = 0;
        if (g_display) {
            held = gemu_display_poll(g_display);
            if (gemu_display_should_quit(g_display)) break;
            if (gemu_display_reset_requested(g_display)) {
                gemu_display_clear_flags(g_display);
                nes_reset_or_np(s);
            }
        }
        /* Monitor commands */
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)   { if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true); else { quit = true; break; } }
            else if (cmd == GEMU_MON_RESET)  nes_reset_or_np(s);
            else if (cmd == GEMU_MON_STEP) {
                for (uint32_t n = gemu_monitor_step_count(s->monitor); n; n--)
                    nes_step_insn(s);
                nes_sync(s);                                  /* show the monitor the whole state */
                nes_present_frame(s, cfg, false);              /* and the window */
            }
        }
        if (quit) break;
        if (g_display)
            gemu_display_set_paused(g_display, gemu_monitor_is_paused(s->monitor));

        /* Input: display action bits (includes VNC input) */
        nes_handle_keys(s, held);
#ifdef HAVE_LUA
        nes_lua_pre_frame(g_lua, s->ctrl_state);
#endif
        if (s->cfg->is_arcade) {
            if (s->vs_coin_latch[0] > 0) s->vs_coin_latch[0]--;
            if (s->vs_coin_latch[1] > 0) s->vs_coin_latch[1]--;
        }
        /* Netplay: everyone's input for this frame, or wait for it */
        bool np_waiting = !nes_np_frame(s);

        if (!gemu_monitor_is_paused(s->monitor) && !np_waiting) {
            nes_run_frame(s);
            nes_np_after_frame(s);

#ifdef HAVE_LUA
            /* Resume the script for this frame; gui.* draws straight into
             * pixels_argb so overlays show up in both the display and VNC. */
            nes_lua_run_frame(g_lua, s->ppu.pixels_argb, RP2C02_WIDTH, RP2C02_HEIGHT);
#endif

            nes_present_frame(s, cfg, true);
#ifdef HAVE_ROB
            if (s->cfg->n_ports > 1 && nes_device_is_rob(s->cfg->ports[1])) {
                rob_frame(&s->rob, s->ppu.pixels_argb, RP2C02_WIDTH, RP2C02_HEIGHT);
                s->ctrl_state[1] = 0;
                if (s->rob.btn_a) s->ctrl_state[1] |= NES_BTN_A;
                if (s->rob.btn_b) s->ctrl_state[1] |= NES_BTN_B;
            }
            if (g_rob_window) {
                if (rob_window_close_requested(g_rob_window)) {
                    rob_window_destroy(g_rob_window);
                    g_rob_window = NULL;
                } else {
                    rob_window_render(g_rob_window, &s->rob);
                }
            }
#endif
        }

        /* Frame sync:
         * - Audio enabled: block until SDL has consumed enough samples so the
         *   queue stays at ~3 frames. This locks emulation to the audio clock
         *   (exactly 60.099 fps) and eliminates drift entirely.
         * - Audio off / headless: fall back to a software timer. */
        if (np_waiting) {
            SDL_Delay(1);       /* the network, not the audio, sets the pace */
            nes_pace(s, true);
        } else if (s->apu.audio_dev) {
            while (gemu_audio_queued(s->apu.audio_dev) > AUDIO_QUEUE_MAX)
                SDL_Delay(1);
        } else if (gemu_monitor_is_paused(s->monitor)) {
            SDL_Delay(10);
            nes_pace(s, true);
        } else {
            /* the machine's time, at the CPU's clock (a changed M2 in
             * Debug > Cartridge Pins lengthens or shortens the frame) */
            nes_pace(s, false);
        }

        hex_editor_refresh(g_hex_editor);
        if (g_cheat_dialog) g_cheat_dialog->refresh(g_cheat_dialog);
        if (g_pin_window) g_pin_window->refresh(g_pin_window);
        nes_np_refresh();
        /* GTK event pump is done inside gemu_display_poll() via the backend */
    }

    printf("nes: %llu frames, %llu cpu cycles\n",
           (unsigned long long)s->ppu.frame,
           (unsigned long long)s->cpu.cycle_count);
    gemu_monitor_stop(s->monitor);
}
