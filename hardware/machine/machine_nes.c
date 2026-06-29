#ifndef _WIN32
#  define _POSIX_C_SOURCE 199309L
#endif
#include "nes.h"
#include "fds.h"
#include "romdb.h"
#include "fds_hle.h"
#include "apu2a03.h"
#include "gemu/memory.h"
#include "gemu/screendump.h"
#include "gemu/gemu_display.h"
#include <SDL2/SDL.h>  /* needed for APU audio queue sync */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#  include <direct.h>
#  define gemu_mkdir(p)   _mkdir(p)
#  define strcasecmp      _stricmp
#  define strncasecmp     _strnicmp
#else
#  include <sys/stat.h>
#  include <strings.h>
#  define gemu_mkdir(p) mkdir((p), 0755)
#endif
#ifdef GEMU_GTK
#  include <gtk/gtk.h>
#endif
#include "hex_editor.h"

#define VS_DEBUG(s, ...) do { \
    if ((s)->cfg->is_arcade && nes_vs_debug_enabled()) { \
        fprintf(stderr, "vs: " __VA_ARGS__); \
    } \
} while (0)

static bool nes_vs_debug_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("GEMU_VS_DEBUG");
        enabled = env && env[0] && strcmp(env, "0") != 0;
    }
    return enabled != 0;
}

static uint8_t nes_pal3bit(uint8_t v) {
    v &= 7u;
    return (uint8_t)((v << 5) | (v << 2) | (v >> 1));
}

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

/* ── VS. SMB DIP switch table ────────────────────────────────────────────── */

typedef struct { const char *value; const char *desc; uint8_t bits; } VsDipOption;
typedef struct {
    const char       *name, *desc;
    uint8_t           mask;
    const VsDipOption *options;
    size_t             n_options;
} VsDip;

static const VsDipOption vs_coinage_opts[] = {
    {"1c-1c",     "1 Coin / 1 Credit",  0x00},
    {"3c-1c",     "3 Coins / 1 Credit", 0x02},
    {"2c-1c",     "2 Coins / 1 Credit", 0x04},
    {"1c-2c",     "1 Coin / 2 Credits", 0x06},
    {"1c-3c",     "1 Coin / 3 Credits", 0x01},
    {"1c-4c",     "1 Coin / 4 Credits", 0x05},
    {"1c-5c",     "1 Coin / 5 Credits", 0x03},
    {"free-play", "Free Play",          0x07},
};
static const VsDipOption vs_lives_opts[] = {
    {"3", "3 lives", 0x00},
    {"2", "2 lives", 0x08},
};
static const VsDipOption vs_bonus_opts[] = {
    {"200", "200 coins",  0x10},
    {"150", "150 coins",  0x20},
    {"100", "100 coins",  0x00},
    {"250", "250 coins",  0x30},
};
static const VsDipOption vs_speed_opts[] = {
    {"slow", "Slow", 0x00},
    {"fast", "Fast", 0x40},
};
static const VsDipOption vs_continue_opts[] = {
    {"4", "4 lives on continue", 0x00},
    {"3", "3 lives on continue", 0x80},
};

static const VsDip vs_dips[] = {
    {"coinage",  "Coinage",        0x07, vs_coinage_opts,  8},
    {"lives",    "Lives",          0x08, vs_lives_opts,    2},
    {"bonus",    "Bonus coins",    0x30, vs_bonus_opts,    4},
    {"timer",    "Timer speed",    0x40, vs_speed_opts,    2},
    {"continue", "Continue lives", 0x80, vs_continue_opts, 2},
};
/* FCEUX initializes VS. SMB with all DIP switches off. */
#define VS_DIP_DEFAULT 0x00u

static const VsDip *vs_find_dip(const char *name) {
    for (size_t i = 0; i < sizeof(vs_dips)/sizeof(vs_dips[0]); i++)
        if (strcasecmp(name, vs_dips[i].name) == 0) return &vs_dips[i];
    return NULL;
}
static const VsDipOption *vs_current_opt(const NesState *s, const VsDip *dip) {
    uint8_t bits = s->vs_dip & dip->mask;
    for (size_t i = 0; i < dip->n_options; i++)
        if (dip->options[i].bits == bits) return &dip->options[i];
    return NULL;
}
static void vs_list_dips(const NesState *s) {
    printf("VS. SMB DIP switches (raw=0x%02X):\n", s->vs_dip);
    for (size_t i = 0; i < sizeof(vs_dips)/sizeof(vs_dips[0]); i++) {
        const VsDip *dip = &vs_dips[i];
        const VsDipOption *cur = vs_current_opt(s, dip);
        printf("  %-10s  %-16s  current: %s\n",
               dip->name, dip->desc, cur ? cur->value : "?");
        printf("             values:");
        for (size_t j = 0; j < dip->n_options; j++) printf(" %s", dip->options[j].value);
        printf("\n");
    }
}
static bool vs_set_dip(NesState *s, const char *name, const char *value) {
    const VsDip *dip = vs_find_dip(name);
    if (!dip) { printf("unknown DIP '%s' (try 'dipswitch list')\n", name); return true; }
    uint8_t bits = 0; bool found = false;
    for (size_t i = 0; i < dip->n_options; i++)
        if (strcasecmp(value, dip->options[i].value) == 0)
            { bits = dip->options[i].bits; found = true; break; }
    if (!found) {
        char *end; unsigned long v = strtoul(value, &end, 0);
        if (end && *end == '\0') { bits = (uint8_t)v & dip->mask; found = true; }
    }
    if (!found) { printf("invalid value '%s' for DIP '%s'\n", value, name); return true; }
    s->vs_dip = (uint8_t)((s->vs_dip & ~dip->mask) | (bits & dip->mask));
    const VsDipOption *cur = vs_current_opt(s, dip);
    printf("DIP %s = %s\n", dip->name, cur ? cur->value : value);
    return true;
}
static bool vs_dip_cmd(NesState *s, const char *args) {
    while (*args == ' ' || *args == '\t') args++;
    char buf[256]; snprintf(buf, sizeof(buf), "%s", args);
    char *name = strtok(buf, " \t"), *val = strtok(NULL, " \t");
    if (!name || strcasecmp(name, "list") == 0) { vs_list_dips(s); return true; }
    if (!val) { printf("usage: dipswitch list | dipswitch <name> <value>\n"); return true; }
    return vs_set_dip(s, name, val);
}

static bool vs_coin_cmd(NesState *s, const char *args) {
    while (*args == ' ' || *args == '\t') args++;
    int slot = (*args == '2') ? 1 : 0;
    s->vs_coin_latch[slot] = 15;
    printf("VS coin %d\n", slot + 1);
    return true;
}

static bool vs_service_cmd(NesState *s) {
    s->vs_service = true;
    printf("VS service\n");
    return true;
}

static bool nes_device_is_rob(NesDeviceType dev) {
    return dev == NES_DEVICE_ROB || dev == NES_DEVICE_ROB_FAMICOM;
}

/* GTK Debug > Hex Editor menu callback (body is no-op when GTK disabled). */
static void nes_hex_toggle(void *ud) {
#ifdef GEMU_GTK
    NesState *s = ud;
    if (!s->hex_editor) return;
    if (hex_editor_is_visible(s->hex_editor))
        hex_editor_hide(s->hex_editor);
    else
        hex_editor_show(s->hex_editor);
#else
    (void)ud;
#endif
}

/* ── Battery-backed SRAM persistence ────────────────────────────────────── */

static void nes_game_basename(const char *path, char *out, size_t len) {
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
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("APPDATA");
    if (!base || !base[0]) base = "C:\\Users\\Default\\AppData\\Local";
    snprintf(out, len, "%s\\gemu\\%s.sav", base, game);
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(out, len, "%s/.gemu/%s.sav", home, game);
#endif
}

static void nes_build_fds_sav_path(const char *game, char *out, size_t len) {
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("APPDATA");
    if (!base || !base[0]) base = "C:\\Users\\Default\\AppData\\Local";
    snprintf(out, len, "%s\\gemu\\%s.fds.sav", base, game);
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(out, len, "%s/.gemu/%s.fds.sav", home, game);
#endif
}

static void nes_ensure_sav_dir(const char *sav_path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", sav_path);
    char *sep = strrchr(dir, '/');
#ifdef _WIN32
    char *sep2 = strrchr(dir, '\\');
    if (sep2 > sep) sep = sep2;
#endif
    if (sep) { *sep = '\0'; gemu_mkdir(dir); }
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
    FILE *f = fopen(s->sav_path, "rb");
    if (!f) return;
    fread(s->prg_ram, 1, sizeof(s->prg_ram), f);
    fclose(f);
    printf("nes: loaded save '%s'\n", s->sav_path);
}

static void nes_sav_save(NesState *s) {
    if (!s->battery_autosave || !s->sav_path[0]) return;
    nes_ensure_sav_dir(s->sav_path);
    FILE *f = fopen(s->sav_path, "wb");
    if (!f) { fprintf(stderr, "nes: cannot write save '%s'\n", s->sav_path); return; }
    fwrite(s->prg_ram, 1, sizeof(s->prg_ram), f);
    fclose(f);
    printf("nes: saved to '%s'\n", s->sav_path);
}

static void nes_fds_save(NesState *s) {
    if (!s->fds_enabled || !s->fds.dirty || !s->sav_path[0])
        return;
    nes_ensure_sav_dir(s->sav_path);
    if (!fds_disk_save(&s->fds, s->sav_path))
        fprintf(stderr, "fds: cannot write save '%s'\n", s->sav_path);
}

static void nes_fds_save_setup(NesState *s, const char *disk_path) {
    if (!disk_path || !disk_path[0]) return;
    char game[256];
    nes_game_basename(disk_path, game, sizeof(game));
    nes_build_fds_sav_path(game, s->sav_path, sizeof(s->sav_path));
    fds_disk_load_save(&s->fds, s->sav_path);
}

static void nes_save_persistent(NesState *s) {
    if (s->fds_enabled)
        nes_fds_save(s);
    else
        nes_sav_save(s);
}

static void nes_battery_setup(NesState *s) {
    if (!s->cart.has_battery) return;
    char game[256];
    nes_game_basename(s->cart_path_buf, game, sizeof(game));
    nes_build_sav_path(game, s->sav_path, sizeof(s->sav_path));
    FILE *existing = fopen(s->sav_path, "rb");
    if (existing) {
        fclose(existing);
        s->battery_autosave = true;
        nes_sav_load(s);
    } else {
        s->battery_autosave = nes_battery_prompt();
    }
}

/* ── iNES cartridge loading ──────────────────────────────────────────────── */

static bool ines_load(NesState *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nes: cannot open '%s'\n", path); return false; }

    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) != 16 ||
        hdr[0] != 'N' || hdr[1] != 'E' || hdr[2] != 'S' || hdr[3] != 0x1A) {
        fprintf(stderr, "nes: '%s' is not a valid iNES file\n", path);
        fclose(f); return false;
    }

    s->cart.prg_banks   = hdr[4];
    s->cart.chr_banks   = hdr[5];
    s->cart.mapper      = (hdr[7] & 0xF0) | (hdr[6] >> 4);
    s->cart.has_battery = (hdr[6] >> 1) & 1;

    bool four_screen = (hdr[6] >> 3) & 1;
    bool vertical    = hdr[6] & 1;
    if      (four_screen) s->cart.mirror = RP2C02_MIRROR_4SCREEN;
    else if (vertical)    s->cart.mirror = RP2C02_MIRROR_VERTICAL;
    else                  s->cart.mirror = RP2C02_MIRROR_HORIZONTAL;

    switch (s->cart.mapper) {
    case 0: case 1: case 2: case 4: case 5: case 7: case 9: case 24: case 26:
    case 66: case 178: case 228: break;
    default:
        fprintf(stderr, "nes: mapper %u not supported!\n",
                s->cart.mapper);
        fclose(f); return false;
    }
    if (s->cart.prg_banks == 0) {
        fprintf(stderr, "nes: invalid PRG bank count 0\n");
        fclose(f); return false;
    }
    if (s->cart.mapper == 0 && s->cart.prg_banks > 2) {
        fprintf(stderr, "nes: NROM requires 1 or 2 PRG banks, got %u\n", s->cart.prg_banks);
        fclose(f); return false;
    }

    /* Skip optional 512-byte trainer */
    if ((hdr[6] >> 2) & 1) fseek(f, 512, SEEK_CUR);

    uint32_t prg_bytes = (uint32_t)s->cart.prg_banks * 0x4000;
    s->prg = malloc(prg_bytes);
    if (!s->prg || fread(s->prg, 1, prg_bytes, f) != prg_bytes) {
        fprintf(stderr, "nes: failed to read PRG ROM\n");
        fclose(f); free(s->prg); s->prg = NULL; return false;
    }

    if (s->cart.chr_banks > 0) {
        uint32_t chr_bytes = (uint32_t)s->cart.chr_banks * 0x2000;
        s->chr = malloc(chr_bytes);
        if (!s->chr || fread(s->chr, 1, chr_bytes, f) != chr_bytes) {
            fprintf(stderr, "nes: failed to read CHR ROM\n");
            fclose(f); free(s->chr); s->chr = NULL; free(s->prg); return false;
        }
        s->chr_is_ram = false;
    } else {
        /* CHR RAM: 8 KB writable by PPU */
        s->chr = calloc(1, 0x2000);
        if (!s->chr) { fclose(f); free(s->prg); return false; }
        s->chr_is_ram = true;
        s->cart.chr_banks = 1;
    }

    fclose(f);
    snprintf(s->cart_path_buf, sizeof(s->cart_path_buf), "%s", path);
    printf("nes: loaded '%s' — mapper %u, %u×16KB PRG, %u×8KB CHR%s\n",
           path, s->cart.mapper,
           s->cart.prg_banks, s->cart.chr_banks,
           s->chr_is_ram ? " (RAM)" : "");
    return true;
}

/* ── Mapper 1 (MMC1/SxROM) ───────────────────────────────────────────────── */

static void mmc1_update_banks(NesState *s) {
    uint8_t  prg_mode  = (s->mmc1_ctrl >> 2) & 3;
    uint8_t  chr_mode  = (s->mmc1_ctrl >> 4) & 1;
    uint8_t  prg_bank  =  s->mmc1_prg  & 0x0F;
    uint32_t prg_size  = (uint32_t)s->cart.prg_banks * 0x4000u;
    uint32_t prg_last  = prg_size - 0x4000u;
    uint32_t chr_size  = (uint32_t)s->cart.chr_banks * 0x2000u;

    switch (prg_mode) {
    case 0: case 1:   /* 32 KB: switch both halves together */
        s->prg_offsets[0] = ((uint32_t)(prg_bank & ~1u) * 0x4000u) % prg_size;
        s->prg_offsets[1] =  s->prg_offsets[0] + 0x4000u;
        break;
    case 2:           /* fix first bank at $8000, switch $C000 */
        s->prg_offsets[0] = 0;
        s->prg_offsets[1] = ((uint32_t)prg_bank * 0x4000u) % prg_size;
        break;
    case 3:           /* switch $8000, fix last bank at $C000 */
        s->prg_offsets[0] = ((uint32_t)prg_bank * 0x4000u) % prg_size;
        s->prg_offsets[1] = prg_last;
        break;
    }

    if (chr_mode == 0) {   /* 8 KB: one window */
        s->chr_offsets[0] = ((uint32_t)(s->mmc1_chr0 & ~1u) * 0x1000u) % chr_size;
        s->chr_offsets[1] =  s->chr_offsets[0] + 0x1000u;
    } else {               /* 4 KB x 2: independent windows */
        s->chr_offsets[0] = ((uint32_t)s->mmc1_chr0 * 0x1000u) % chr_size;
        s->chr_offsets[1] = ((uint32_t)s->mmc1_chr1 * 0x1000u) % chr_size;
    }

    static const uint8_t mirror_map[] = {
        RP2C02_MIRROR_SINGLE_A, RP2C02_MIRROR_SINGLE_B,
        RP2C02_MIRROR_VERTICAL, RP2C02_MIRROR_HORIZONTAL
    };
    s->ppu.mirror = mirror_map[s->mmc1_ctrl & 3];
}

static void mmc1_serial_write(NesState *s, uint16_t addr, uint8_t val) {
    if (val & 0x80) {          /* reset: force PRG mode 3 */
        s->mmc1_shift       = 0;
        s->mmc1_shift_count = 0;
        s->mmc1_ctrl       |= 0x0C;
        mmc1_update_banks(s);
        return;
    }
    /* Shift bit 0 in from the MSB side; after 5 writes the register is full. */
    s->mmc1_shift = (s->mmc1_shift >> 1) | ((val & 1u) << 4);
    if (++s->mmc1_shift_count < 5) return;

    uint8_t reg = (addr >> 13) & 3;
    switch (reg) {
    case 0: s->mmc1_ctrl = s->mmc1_shift; break;
    case 1: s->mmc1_chr0 = s->mmc1_shift; break;
    case 2: s->mmc1_chr1 = s->mmc1_shift; break;
    case 3: s->mmc1_prg  = s->mmc1_shift; break;
    }
    s->mmc1_shift       = 0;
    s->mmc1_shift_count = 0;
    mmc1_update_banks(s);
}

/* ── Mapper 4 (MMC3/TxROM) ───────────────────────────────────────────────── */

static void mmc3_update_banks(NesState *s) {
    uint32_t prg_8k   = (uint32_t)s->cart.prg_banks * 2;   /* total 8 KB banks */
    uint32_t chr_1k   = (uint32_t)s->cart.chr_banks * 8;   /* total 1 KB banks */

    /* PRG: four 8 KB windows at $8000, $A000, $C000, $E000 */
    uint8_t  prg_mode = (s->mmc3_bank_sel >> 6) & 1;
    uint32_t r6 = (s->mmc3_regs[6] % prg_8k) * 0x2000u;
    uint32_t r7 = (s->mmc3_regs[7] % prg_8k) * 0x2000u;
    uint32_t f0 = (prg_8k - 2) * 0x2000u;   /* second-to-last fixed bank */
    uint32_t f1 = (prg_8k - 1) * 0x2000u;   /* last fixed bank */

    if (!prg_mode) {
        s->mmc3_prg_offsets[0] = r6;  s->mmc3_prg_offsets[1] = r7;
        s->mmc3_prg_offsets[2] = f0;  s->mmc3_prg_offsets[3] = f1;
    } else {
        s->mmc3_prg_offsets[0] = f0;  s->mmc3_prg_offsets[1] = r7;
        s->mmc3_prg_offsets[2] = r6;  s->mmc3_prg_offsets[3] = f1;
    }

    /* CHR: eight 1 KB windows at PPU $0000–$1FFF.
     * R0/R1 select 2 KB pairs (bit 0 ignored); R2–R5 select 1 KB banks.
     * CHR-invert bit swaps which half gets the 2 KB banks. */
    if (s->chr_is_ram || chr_1k == 0) return;

    uint8_t inv = (s->mmc3_bank_sel >> 7) & 1;
    uint32_t c[8];
    c[0] = ((s->mmc3_regs[0] & 0xFEu)      % chr_1k) * 0x400u;
    c[1] = (((s->mmc3_regs[0] & 0xFEu) + 1) % chr_1k) * 0x400u;
    c[2] = ((s->mmc3_regs[1] & 0xFEu)      % chr_1k) * 0x400u;
    c[3] = (((s->mmc3_regs[1] & 0xFEu) + 1) % chr_1k) * 0x400u;
    c[4] = (s->mmc3_regs[2] % chr_1k) * 0x400u;
    c[5] = (s->mmc3_regs[3] % chr_1k) * 0x400u;
    c[6] = (s->mmc3_regs[4] % chr_1k) * 0x400u;
    c[7] = (s->mmc3_regs[5] % chr_1k) * 0x400u;

    if (!inv) {
        /* 2 KB at $0000, 2 KB at $0800, 1 KB×4 at $1000–$1FFF */
        for (int i = 0; i < 4; i++) s->mmc3_chr_offsets[i]   = c[i];
        for (int i = 0; i < 4; i++) s->mmc3_chr_offsets[i+4] = c[i+4];
    } else {
        /* 1 KB×4 at $0000–$0FFF, 2 KB at $1000, 2 KB at $1800 */
        for (int i = 0; i < 4; i++) s->mmc3_chr_offsets[i]   = c[i+4];
        for (int i = 0; i < 4; i++) s->mmc3_chr_offsets[i+4] = c[i];
    }
}

static void mmc3_cpu_write(NesState *s, uint16_t addr, uint8_t val) {
    bool odd = (addr & 1) != 0;
    if (addr < 0xA000) {
        if (!odd) { s->mmc3_bank_sel = val; }
        else      { s->mmc3_regs[s->mmc3_bank_sel & 7] = val; mmc3_update_banks(s); }
    } else if (addr < 0xC000) {
        if (!odd && s->cart.mirror != RP2C02_MIRROR_4SCREEN)
            s->ppu.mirror = (val & 1) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
        /* $A001 PRG-RAM protect: ignored */
    } else if (addr < 0xE000) {
        if (!odd) { s->mmc3_irq_latch = val; }
        else      { s->mmc3_irq_reload = true; s->mmc3_irq_counter = 0; }
    } else {
        if (!odd) { s->mmc3_irq_enabled = false; s->cpu.irq = false; }
        else      { s->mmc3_irq_enabled = true; }
    }
}

static void mmc3_irq_scanline(void *ud) {
    NesState *s = ud;
    if (s->mmc3_irq_reload || s->mmc3_irq_counter == 0) {
        s->mmc3_irq_counter = s->mmc3_irq_latch;
        s->mmc3_irq_reload  = false;
    } else {
        s->mmc3_irq_counter--;
    }
    if (s->mmc3_irq_counter == 0 && s->mmc3_irq_enabled)
        s->cpu.irq = true;
}

/* ── Mapper 178 (WaixingFS) ──────────────────────────────────────────────── */

static void m178_update_banks(NesState *s) {
    uint8_t  prg_mode = (s->m178_mode >> 1) & 3u;
    uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
    uint32_t outer    = (uint32_t)s->m178_prg_hi * 0x20000u;
    uint32_t inner    = (uint32_t)(s->m178_prg_lo & 7u) * 0x4000u;

    switch (prg_mode) {
    case 0: /* NROM-256/BNROM: 32KB page, CPU A14 = PRG A14 */
        {
            uint32_t base = (outer + (uint32_t)(s->m178_prg_lo & 6u) * 0x4000u) % prg_size;
            s->prg_offsets[0] = base;
            s->prg_offsets[1] = (base + 0x4000u) % prg_size;
        }
        break;
    case 1: /* UNROM: switchable $8000–$BFFF, last inner bank fixed at $C000 */
        s->prg_offsets[0] = (outer + inner) % prg_size;
        s->prg_offsets[1] = (outer + 7u * 0x4000u) % prg_size;
        break;
    case 2: /* NROM-128: same 16KB bank mirrored at both windows */
        s->prg_offsets[0] = (outer + inner) % prg_size;
        s->prg_offsets[1] = (outer + inner) % prg_size;
        break;
    default: /* mode 3: UNROM variant — treat as mode 1 */
        s->prg_offsets[0] = (outer + inner) % prg_size;
        s->prg_offsets[1] = (outer + 7u * 0x4000u) % prg_size;
        break;
    }

    s->ppu.mirror = (s->m178_mode & 1u) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
}

/* ── Mapper 5 (MMC5) ─────────────────────────────────────────────────────── */

static void mmc5_update_prg_banks(NesState *s) {
    uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
    uint32_t unit     = 0x2000u; /* 8KB per slot */
    uint8_t  *r       = s->mmc5_prg_regs; /* r[0]=$5113 … r[4]=$5117 */

    for (int i = 0; i < 4; i++) s->mmc5_prg_is_ram[i] = false;

    switch (s->mmc5_prg_mode) {
    case 0: { /* 32KB at $8000-$FFFF from $5117 (always ROM) */
        uint32_t base = ((uint32_t)(r[4] & 0x7Cu) * unit) % prg_size;
        for (int i = 0; i < 4; i++) s->mmc5_prg_offsets[i] = base + (uint32_t)i * unit;
        break;
    }
    case 1: { /* 16KB from $5115 at $8000, 16KB from $5117 at $C000 */
        if (!(r[2] & 0x80u)) { s->mmc5_prg_is_ram[0] = s->mmc5_prg_is_ram[1] = true; }
        uint32_t b0 = ((uint32_t)(r[2] & 0x7Eu) * unit) % prg_size;
        uint32_t b1 = ((uint32_t)(r[4] & 0x7Eu) * unit) % prg_size;
        s->mmc5_prg_offsets[0] = b0;  s->mmc5_prg_offsets[1] = b0 + unit;
        s->mmc5_prg_offsets[2] = b1;  s->mmc5_prg_offsets[3] = b1 + unit;
        break;
    }
    case 2: { /* 16KB from $5115, 8KB from $5116, 8KB from $5117 */
        if (!(r[2] & 0x80u)) { s->mmc5_prg_is_ram[0] = s->mmc5_prg_is_ram[1] = true; }
        if (!(r[3] & 0x80u)) { s->mmc5_prg_is_ram[2] = true; }
        uint32_t b0 = ((uint32_t)(r[2] & 0x7Eu) * unit) % prg_size;
        s->mmc5_prg_offsets[0] = b0;  s->mmc5_prg_offsets[1] = b0 + unit;
        s->mmc5_prg_offsets[2] = ((uint32_t)(r[3] & 0x7Fu) * unit) % prg_size;
        s->mmc5_prg_offsets[3] = ((uint32_t)(r[4] & 0x7Fu) * unit) % prg_size;
        break;
    }
    case 3: /* Four 8KB banks from $5114-$5117 */
        for (int i = 0; i < 4; i++) {
            uint8_t reg = r[i + 1];
            if (i < 3 && !(reg & 0x80u)) s->mmc5_prg_is_ram[i] = true;
            s->mmc5_prg_offsets[i] = ((uint32_t)(reg & 0x7Fu) * unit) % prg_size;
        }
        break;
    }
}

/* Return 1KB CHR byte offset for a given register index + sub-slot offset. */
static uint32_t mmc5_chr1kb(const NesState *s, uint8_t reg_idx, uint8_t sub) {
    uint32_t chr_size = (uint32_t)s->cart.chr_banks * 0x2000u;
    uint16_t bank = (uint16_t)(((uint16_t)(s->mmc5_chr_hi & 3u) << 8) |
                                s->mmc5_chr_regs[reg_idx]);
    return ((uint32_t)(bank + sub) * 0x400u) % chr_size;
}

static uint8_t mmc5_chr_read(NesState *s, uint16_t addr) {
    if (s->chr_is_ram) return s->chr[addr & 0x1FFFu];

    uint32_t chr_size = (uint32_t)s->cart.chr_banks * 0x2000u;
    bool rendering = (s->ppu.ppumask & (PPUMASK_SHOW_BG | PPUMASK_SHOW_SPR)) &&
                     (s->ppu.scanline >= 0 && s->ppu.scanline < 240);

    /* Extended attribute mode: BG uses ExRAM bank bits instead of normal CHR banking */
    bool is_spr_fetch = rendering && (s->ppu.dot == 257) &&
                        (s->ppu.ppuctrl & PPUCTRL_SPR_8x16);
    if (!is_spr_fetch && rendering && s->mmc5_exram_mode == 1) {
        uint8_t bank8 = (uint8_t)(((s->mmc5_chr_hi & 3u) << 6) |
                                   (s->mmc5_exattr_latch & 0x3Fu));
        return s->chr[((uint32_t)bank8 * 0x1000u + (addr & 0x0FFFu)) % chr_size];
    }

    /* Determine whether this is a sprite or BG fetch */
    bool use_spr;
    if (s->ppu.ppuctrl & PPUCTRL_SPR_8x16)
        use_spr = is_spr_fetch;   /* 8x16: sprites at dot 257, BG elsewhere */
    else
        use_spr = !s->mmc5_last_chr_bg; /* 8x8: whichever set was last written */

    uint8_t mode = s->mmc5_chr_mode;

    if (use_spr) {
        /* Sprite banks: $5120-$5127 (indices 0-7) → 8 × 1KB windows */
        uint8_t s8 = (addr >> 10) & 7u;
        switch (mode) {
        case 0: return s->chr[(mmc5_chr1kb(s, 7, s8))         + (addr & 0x3FFu)];
        case 1: return s->chr[(mmc5_chr1kb(s, s8 >= 4 ? 7 : 3, s8 & 3u)) + (addr & 0x3FFu)];
        case 2: return s->chr[(mmc5_chr1kb(s, (s8 >> 1) * 2u + 1u, s8 & 1u)) + (addr & 0x3FFu)];
        case 3: return s->chr[(mmc5_chr1kb(s, s8, 0))          + (addr & 0x3FFu)];
        }
    } else {
        /* BG banks: $5128-$512B (indices 8-11) → 4 × 1KB, mirrored to $0000-$1FFF */
        uint8_t s4 = (addr >> 10) & 3u;
        switch (mode) {
        case 0: return s->chr[(mmc5_chr1kb(s, 11, s4))          + (addr & 0x3FFu)];
        case 1: return s->chr[(mmc5_chr1kb(s, 11, s4))          + (addr & 0x3FFu)];
        case 2: return s->chr[(mmc5_chr1kb(s, (s4 >> 1) * 2u + 9u, s4 & 1u)) + (addr & 0x3FFu)];
        case 3: return s->chr[(mmc5_chr1kb(s, 8u + s4, 0))      + (addr & 0x3FFu)];
        }
    }
    return 0;
}

static uint8_t mmc5_nt_read(uint16_t addr, void *ud) {
    NesState *s = ud;
    uint8_t  nt  = (addr >> 10) & 3u;
    uint8_t  mode = (s->mmc5_nt_map >> (nt * 2u)) & 3u;
    uint16_t off  = addr & 0x3FFu;

    /* Save ExRAM latch on tile reads (used for extended attribute CHR banking) */
    if (off < 0x3C0u && s->mmc5_exram_mode == 1)
        s->mmc5_exattr_latch = s->mmc5_exram[off];

    /* Extended attribute: override attribute reads with ExRAM palette bits */
    if (off >= 0x3C0u && s->mmc5_exram_mode == 1) {
        uint8_t pal   = (s->mmc5_exattr_latch >> 6) & 3u;
        uint8_t shift = (uint8_t)(((s->ppu.v >> 4) & 4u) | (s->ppu.v & 2u));
        return (uint8_t)(pal << shift);
    }

    switch (mode) {
    case 0: return s->ppu.vram[off];
    case 1: return s->ppu.vram[0x400u + off];
    case 2: return (s->mmc5_exram_mode >= 2) ? s->mmc5_exram[off & 0x3FFu] : 0;
    case 3: /* fill mode */
        return (off >= 0x3C0u) ? (uint8_t)(s->mmc5_fill_attr & 3u) * 0x55u
                                : s->mmc5_fill_tile;
    }
    return 0;
}

static void mmc5_nt_write(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;
    uint8_t  nt  = (addr >> 10) & 3u;
    uint8_t  mode = (s->mmc5_nt_map >> (nt * 2u)) & 3u;
    uint16_t off  = addr & 0x3FFu;
    switch (mode) {
    case 0: s->ppu.vram[off]          = val; break;
    case 1: s->ppu.vram[0x400u + off] = val; break;
    case 2: s->mmc5_exram[off & 0x3FFu] = val; break;
    case 3: break; /* fill-mode: writes ignored */
    }
}

static void mmc5_irq_scanline(void *ud) {
    NesState *s = ud;
    int sl = s->ppu.scanline;
    if (sl >= 240) {
        s->mmc5_in_frame = false;
        return;
    }
    if (!s->mmc5_in_frame) {
        s->mmc5_in_frame    = true;
        s->mmc5_irq_counter = 0;
    } else {
        s->mmc5_irq_counter++;
    }
    if (s->mmc5_irq_target > 0 && s->mmc5_irq_counter == s->mmc5_irq_target) {
        s->mmc5_irq_pending = true;
        if (s->mmc5_irq_enabled)
            s->cpu.irq = true;
    }
}

/* ── Mapper 24/26 (Konami VRC6a/VRC6b) ──────────────────────────────────── */

/* Advance all VRC6 state by one CPU cycle: handles IRQ and audio timers.
 * Returns current expansion audio output level (for apu.fds_in). */
static float vrc6_tick(NesState *s) {
    /* IRQ: prescaler counts CPU cycles; fires at 341 in scanline mode, each cycle in CPU mode */
    if (s->vrc6_irq_enabled) {
        bool fire = false;
        if (!s->vrc6_irq_mode) {
            if (++s->vrc6_prescaler >= 341) { s->vrc6_prescaler = 0; fire = true; }
        } else { fire = true; }
        if (fire) {
            if (s->vrc6_irq_counter == 0xFF) {
                s->vrc6_irq_counter = s->vrc6_irq_latch;
                s->cpu.irq = true;
            } else { s->vrc6_irq_counter++; }
        }
    }

    if (s->vrc6_halt) return 0.0f;

    /* Pulse 1 */
    float p1 = 0.0f;
    if (s->vrc6_p1_en) {
        if (s->vrc6_p1_timer == 0) {
            s->vrc6_p1_timer = s->vrc6_p1_period;
            s->vrc6_p1_seq   = (s->vrc6_p1_seq + 1u) & 15u;
        } else { s->vrc6_p1_timer--; }
        uint8_t vol = s->vrc6_p1_ctrl & 0x0Fu;
        if ((s->vrc6_p1_ctrl & 0x80u) || s->vrc6_p1_seq <= ((s->vrc6_p1_ctrl >> 4) & 7u))
            p1 = (float)vol;
    }

    /* Pulse 2 */
    float p2 = 0.0f;
    if (s->vrc6_p2_en) {
        if (s->vrc6_p2_timer == 0) {
            s->vrc6_p2_timer = s->vrc6_p2_period;
            s->vrc6_p2_seq   = (s->vrc6_p2_seq + 1u) & 15u;
        } else { s->vrc6_p2_timer--; }
        uint8_t vol = s->vrc6_p2_ctrl & 0x0Fu;
        if ((s->vrc6_p2_ctrl & 0x80u) || s->vrc6_p2_seq <= ((s->vrc6_p2_ctrl >> 4) & 7u))
            p2 = (float)vol;
    }

    /* Sawtooth: 6 accumulator additions per 14-clock cycle (even steps 2,4,6,8,10,12) */
    float saw = 0.0f;
    if (s->vrc6_saw_en) {
        if (s->vrc6_saw_timer == 0) {
            s->vrc6_saw_timer = s->vrc6_saw_period;
            s->vrc6_saw_step++;
            if (s->vrc6_saw_step >= 14u) {
                s->vrc6_saw_step  = 0;
                s->vrc6_saw_accum = 0;
            } else if ((s->vrc6_saw_step & 1u) == 0u) {
                s->vrc6_saw_accum += s->vrc6_saw_rate;
            }
        } else { s->vrc6_saw_timer--; }
        saw = (float)(s->vrc6_saw_accum >> 3);
    }

    /* Mix: VRC6 pulses use same nonlinear formula as 2A03 (vol 0-15 = same units).
     * Sawtooth scaled to roughly match one 2A03 pulse at full volume. */
    float pulse = (p1 + p2 > 0.0f) ? 95.88f / (8128.0f / (p1 + p2) + 100.0f) : 0.0f;
    return pulse + saw * (0.15f / 31.0f);
}

static void vrc6_cpu_write(NesState *s, uint16_t addr, uint8_t val) {
    /* Mapper 26 (VRC6b) swaps address pins A0 and A1 */
    if (s->cart.mapper == 26) {
        uint8_t a01 = addr & 3u;
        addr = (uint16_t)((addr & ~3u) | ((a01 & 1u) << 1) | ((a01 >> 1) & 1u));
    }
    uint32_t prg_8k = (uint32_t)s->cart.prg_banks * 2u;
    uint32_t chr_1k = (uint32_t)s->cart.chr_banks * 8u;

    if (addr < 0x9000) {
        /* $8000: 16KB PRG bank at $8000-$BFFF (bits 3:0) */
        uint32_t bank = (uint32_t)(val & 0x0Fu) % s->cart.prg_banks;
        s->mmc3_prg_offsets[0] = bank * 0x4000u;
        s->mmc3_prg_offsets[1] = bank * 0x4000u + 0x2000u;
    } else if (addr < 0xA000) {
        switch (addr & 3u) {
        case 0: s->vrc6_p1_ctrl   = val; break;
        case 1: s->vrc6_p1_period = (s->vrc6_p1_period & 0xFF00u) | val; break;
        case 2:
            s->vrc6_p1_period = (s->vrc6_p1_period & 0x00FFu) | ((uint16_t)(val & 0x0Fu) << 8);
            s->vrc6_p1_en     = (val >> 7) & 1;
            break;
        case 3: s->vrc6_halt = val & 1; break;
        }
    } else if (addr < 0xB000) {
        switch (addr & 3u) {
        case 0: s->vrc6_p2_ctrl   = val; break;
        case 1: s->vrc6_p2_period = (s->vrc6_p2_period & 0xFF00u) | val; break;
        case 2:
            s->vrc6_p2_period = (s->vrc6_p2_period & 0x00FFu) | ((uint16_t)(val & 0x0Fu) << 8);
            s->vrc6_p2_en     = (val >> 7) & 1;
            break;
        }
    } else if (addr < 0xC000) {
        switch (addr & 3u) {
        case 0: s->vrc6_saw_rate   = val & 0x3Fu; break;
        case 1: s->vrc6_saw_period = (s->vrc6_saw_period & 0xFF00u) | val; break;
        case 2:
            s->vrc6_saw_period = (s->vrc6_saw_period & 0x00FFu) | ((uint16_t)(val & 0x0Fu) << 8);
            s->vrc6_saw_en     = (val >> 7) & 1;
            break;
        case 3:
            switch ((val >> 2) & 3u) {
            case 0: s->ppu.mirror = RP2C02_MIRROR_VERTICAL;   break;
            case 1: s->ppu.mirror = RP2C02_MIRROR_HORIZONTAL; break;
            case 2: s->ppu.mirror = RP2C02_MIRROR_SINGLE_A;   break;
            case 3: s->ppu.mirror = RP2C02_MIRROR_SINGLE_B;   break;
            }
            break;
        }
    } else if (addr < 0xD000) {
        /* $C000: 8KB PRG bank at $C000-$DFFF (bits 4:0) */
        if ((addr & 3u) == 0)
            s->mmc3_prg_offsets[2] = ((uint32_t)(val & 0x1Fu) % prg_8k) * 0x2000u;
    } else if (addr < 0xE000) {
        /* $D000-$D003: CHR 1KB banks for PPU $0000-$0FFF */
        if (chr_1k) s->mmc3_chr_offsets[addr & 3u] = ((uint32_t)val % chr_1k) * 0x400u;
    } else if (addr < 0xF000) {
        /* $E000-$E003: CHR 1KB banks for PPU $1000-$1FFF */
        if (chr_1k) s->mmc3_chr_offsets[4u + (addr & 3u)] = ((uint32_t)val % chr_1k) * 0x400u;
    } else {
        /* $F000-$F002: IRQ */
        switch (addr & 3u) {
        case 0: s->vrc6_irq_latch = val; break;
        case 1:
            s->vrc6_irq_after   = val & 1u;
            s->vrc6_irq_enabled = (val >> 1) & 1u;
            s->vrc6_irq_mode    = (val >> 2) & 1u;
            if (s->vrc6_irq_enabled) {
                s->vrc6_irq_counter = s->vrc6_irq_latch;
                s->vrc6_prescaler   = 0;
            } else { s->cpu.irq = false; }
            break;
        case 2:
            s->cpu.irq          = false;
            s->vrc6_irq_counter = s->vrc6_irq_latch;
            s->vrc6_irq_enabled = s->vrc6_irq_after;
            s->vrc6_prescaler   = 0;
            break;
        }
    }
}

/* ── CHR bus callbacks (PPU address space 0x0000–0x1FFF) ─────────────────── */

static uint8_t nes_chr_read(uint16_t addr, void *ud) {
    NesState *s = ud;
    if (!s->chr) return 0;
    if (s->cart.mapper == 5) return mmc5_chr_read(s, addr);
    if (s->cart.mapper == 4) {
        if (s->chr_is_ram) return s->chr[addr & 0x1FFF];
        return s->chr[s->mmc3_chr_offsets[addr >> 10] + (addr & 0x3FF)];
    }
    if (s->cart.mapper == 24 || s->cart.mapper == 26)
        return s->chr[s->mmc3_chr_offsets[addr >> 10] + (addr & 0x3FFu)];
    if (s->cart.mapper == 9) {
        /* MMC2: 4 KB CHR windows with latch-based bank selection.
         * Latch updates AFTER the fetch — the triggering tile ($FD/$FE)
         * renders with the old bank; the new bank takes effect next fetch. */
        if (s->chr_is_ram) return s->chr[addr & 0x1FFF];
        bool     is_right = (addr & 0x1000u) != 0;
        uint16_t tile     = addr & 0x0FF8u;
        uint8_t  bank;
        if (is_right)
            bank = s->m9_latch_r ? s->m9_chr_rfe : s->m9_chr_rfd;
        else
            bank = s->m9_latch_l ? s->m9_chr_lfe : s->m9_chr_lfd;
        uint32_t chr_size = (uint32_t)s->cart.chr_banks * 0x2000u;
        uint8_t  data = s->chr[((uint32_t)bank * 0x1000u + (addr & 0x0FFFu)) % chr_size];
        /* Latch fires only on the high bitplane fetch ($xFD8-$xFDF / $xFE8-$xFEF) */
        if (tile == 0x0FD8u) {
            if (is_right) s->m9_latch_r = false; else s->m9_latch_l = false;
        } else if (tile == 0x0FE8u) {
            if (is_right) s->m9_latch_r = true;  else s->m9_latch_l = true;
        }
        return data;
    }
    if ((s->cart.mapper >= 1 && s->cart.mapper != 7) ||
        (s->cfg->is_arcade && !s->chr_is_ram)) {
        uint8_t slot = addr >= 0x1000 ? 1 : 0;
        return s->chr[s->chr_offsets[slot] + (addr & 0x0FFF)];
    }
    return s->chr[addr & 0x1FFF];
}

static void nes_chr_write(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;
    if (!s->chr_is_ram) return;
    if (s->cart.mapper == 4) { s->chr[addr & 0x1FFF] = val; return; }
    if (s->cart.mapper == 24 || s->cart.mapper == 26) {
        s->chr[s->mmc3_chr_offsets[addr >> 10] + (addr & 0x3FFu)] = val; return;
    }
    if (s->cart.mapper >= 1 && s->cart.mapper != 7) {
        uint8_t slot = addr >= 0x1000 ? 1 : 0;
        s->chr[s->chr_offsets[slot] + (addr & 0x0FFF)] = val;
    } else {
        s->chr[addr & 0x1FFF] = val;
    }
}

/* ── CPU memory map ──────────────────────────────────────────────────────── */

/* ── Game Genie ──────────────────────────────────────────────────────────── */

static const char gg_alpha[] = "APZLGITYEOXUKSVN";

static bool gg_decode(const char *code, uint16_t *addr, uint8_t *val,
                      uint8_t *cmp, bool *has_cmp) {
    int len = (int)strlen(code);
    if (len != 6 && len != 8) return false;
    uint8_t n[8] = {0};
    for (int i = 0; i < len; i++) {
        const char *p = strchr(gg_alpha, toupper((unsigned char)code[i]));
        if (!p) return false;
        n[i] = (uint8_t)(p - gg_alpha);
    }
    /* NES Game Genie bit layout per nesgg.txt:
     *   val[7]=n0[3]  val[6:4]=n1[2:0]  val[3]=n5[3](6ch)/n7[3](8ch)  val[2:0]=n0[2:0]
     *   addr[14:12]=n3[2:0]  addr[11]=n4[3]   addr[10:8]=n5[2:0]
     *   addr[7]=n1[3]  addr[6:4]=n2[2:0]  addr[3]=n3[3]  addr[2:0]=n4[2:0]
     *   cmp[7]=n6[3]  cmp[6:4]=n7[2:0]  cmp[3]=n5[3]  cmp[2:0]=n6[2:0]  (8-char) */
    *addr = 0x8000u
        | ((uint16_t)(n[3] & 7u) << 12)
        | ((uint16_t)(n[4] & 8u) <<  8)
        | ((uint16_t)(n[5] & 7u) <<  8)
        | ((uint16_t)(n[1] & 8u) <<  4)
        | ((uint16_t)(n[2] & 7u) <<  4)
        | ((uint16_t)(n[3] & 8u))
        | ((uint16_t)(n[4] & 7u));
    *has_cmp = (len == 8);
    if (*has_cmp) {
        *val = (uint8_t)(((n[0] & 8u) << 4) | ((n[1] & 7u) << 4) | (n[7] & 8u) | (n[0] & 7u));
        *cmp = (uint8_t)(((n[6] & 8u) << 4) | ((n[7] & 7u) << 4) | (n[5] & 8u) | (n[6] & 7u));
    } else {
        *val = (uint8_t)(((n[0] & 8u) << 4) | ((n[1] & 7u) << 4) | (n[5] & 8u) | (n[0] & 7u));
        *cmp = 0;
    }
    return true;
}

static uint8_t nes_prg_direct(const NesState *s, uint16_t addr) {
    if (addr < 0x8000 || !s->prg) return 0;
    if (s->cart.mapper == 9) {
        uint8_t slot   = (uint8_t)((addr - 0x8000u) >> 13);
        uint32_t prg8k = (uint32_t)s->cart.prg_banks * 2u;
        uint32_t bank8;
        if      (slot == 0) bank8 = s->m9_prg_reg & 0x0Fu;
        else if (slot == 1) bank8 = prg8k - 3u;
        else if (slot == 2) bank8 = prg8k - 2u;
        else                bank8 = prg8k - 1u;
        return s->prg[bank8 * 0x2000u + (addr & 0x1FFFu)];
    }
    if (s->cart.mapper == 4 || s->cart.mapper == 24 || s->cart.mapper == 26) {
        uint8_t slot = (uint8_t)((addr - 0x8000u) >> 13);
        return s->prg[s->mmc3_prg_offsets[slot] + (addr & 0x1FFFu)];
    }
    if (s->cart.mapper >= 1) {
        uint8_t slot = addr >= 0xC000 ? 1 : 0;
        return s->prg[s->prg_offsets[slot] + (addr & 0x3FFF)];
    }
    uint32_t off = addr - 0x8000u;
    if (s->cart.prg_banks == 1) off &= 0x3FFF;
    return s->prg[off];
}

static void nes_sync_ppu_to_cpu(NesState *s, uint64_t cpu_cycle) {
    while (s->ppu_synced_cpu_cycle < cpu_cycle) {
        s->ppu_synced_cpu_cycle++;
        if (s->fds_enabled)
            s->cpu.irq = fds_tick(&s->fds);
        for (int i = 0; i < 3; i++) {
            rp2c02_tick(&s->ppu);
            if (s->ppu.nmi_pending) {
                s->cpu.nmi = true;
                s->ppu.nmi_pending = false;
            }
            if (s->ppu.dirty)
                return;
        }
    }
}

static void nes_gamegenie_cmd(NesState *s, const char *line) {
    /* skip "gamegenie" */
    while (*line && !isspace((unsigned char)*line)) line++;
    while (*line && isspace((unsigned char)*line)) line++;

    char sub[32] = {0};
    int  si = 0;
    while (*line && !isspace((unsigned char)*line) && si < 31)
        sub[si++] = (char)tolower((unsigned char)*line++);
    while (*line && isspace((unsigned char)*line)) line++;
    const char *arg = line;
    /* trim trailing whitespace from arg */
    char argbuf[32] = {0};
    snprintf(argbuf, sizeof(argbuf), "%s", arg);
    for (int i = (int)strlen(argbuf) - 1; i >= 0 && isspace((unsigned char)argbuf[i]); i--)
        argbuf[i] = '\0';
    arg = argbuf;

    if (strcmp(sub, "list") == 0) {
        if (!*arg) {
            if (s->gg_count == 0) { printf("gamegenie: no patches active\n"); return; }
            printf("Active Game Genie patches:\n");
            for (int i = 0; i < s->gg_count; i++) {
                NesGgPatch *g = &s->gg_patches[i];
                uint8_t actual = nes_prg_direct(s, g->addr);
                if (g->has_cmp) {
                    const char *status = (actual == g->cmp) ? "active" : "mismatch";
                    printf("  %-8s  $%04X = $%02X  (if $%02X)  ROM=$%02X [%s]\n",
                           g->code, g->addr, g->val, g->cmp, actual, status);
                } else {
                    printf("  %-8s  $%04X = $%02X  ROM=$%02X [active]\n",
                           g->code, g->addr, g->val, actual);
                }
            }
        } else {
            bool found = false;
            for (int i = 0; i < s->gg_count; i++) {
                if (strcasecmp(s->gg_patches[i].code, arg) == 0) {
                    NesGgPatch *g = &s->gg_patches[i];
                    uint8_t actual = nes_prg_direct(s, g->addr);
                    if (g->has_cmp) {
                        const char *status = (actual == g->cmp) ? "active" : "mismatch";
                        printf("  %-8s  $%04X = $%02X  (if $%02X)  ROM=$%02X [%s]\n",
                               g->code, g->addr, g->val, g->cmp, actual, status);
                    } else {
                        printf("  %-8s  $%04X = $%02X  ROM=$%02X [active]\n",
                               g->code, g->addr, g->val, actual);
                    }
                    found = true; break;
                }
            }
            if (!found) printf("gamegenie: code '%s' not found\n", arg);
        }
        return;
    }

    if (strcmp(sub, "add") == 0) {
        if (!*arg) { printf("usage: gamegenie add <code>\n"); return; }
        if (s->gg_count >= NES_GG_MAX) {
            printf("gamegenie: patch limit reached (%d max)\n", NES_GG_MAX); return;
        }
        for (int i = 0; i < s->gg_count; i++) {
            if (strcasecmp(s->gg_patches[i].code, arg) == 0) {
                printf("gamegenie: %s is already active\n", arg); return;
            }
        }
        uint16_t paddr; uint8_t pval, pcmp; bool has_cmp;
        if (!gg_decode(arg, &paddr, &pval, &pcmp, &has_cmp)) {
            printf("gamegenie: invalid code '%s'\n", arg); return;
        }
        NesGgPatch *g = &s->gg_patches[s->gg_count++];
        g->addr = paddr; g->val = pval; g->cmp = pcmp; g->has_cmp = has_cmp;
        snprintf(g->code, sizeof(g->code), "%s", arg);
        if (has_cmp)
            printf("gamegenie: added %s → $%04X = $%02X (if $%02X)\n", arg, paddr, pval, pcmp);
        else
            printf("gamegenie: added %s → $%04X = $%02X\n", arg, paddr, pval);
        return;
    }

    if (strcmp(sub, "delete") == 0 || strcmp(sub, "remove") == 0) {
        if (!*arg) { printf("usage: gamegenie delete <code>\n"); return; }
        for (int i = 0; i < s->gg_count; i++) {
            if (strcasecmp(s->gg_patches[i].code, arg) == 0) {
                memmove(&s->gg_patches[i], &s->gg_patches[i + 1],
                        (size_t)(s->gg_count - i - 1) * sizeof(NesGgPatch));
                s->gg_count--;
                printf("gamegenie: removed %s\n", arg);
                return;
            }
        }
        printf("gamegenie: code '%s' not found\n", arg);
        return;
    }

    printf("gamegenie add <code> | gamegenie list [code] | gamegenie delete <code>\n");
}

static uint8_t nes_cpu_read(uint16_t addr, void *ud) {
    NesState *s = ud;
    gemu_monitor_check_read(s->monitor, addr);

    if (addr < 0x2000) return s->ram[addr & 0x07FF];

    if (addr < 0x4000) {
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        return rp2c02_read(&s->ppu, (uint8_t)(addr & 7));
    }

    /* FDS I/O registers ($4020-$408F covers disk regs + wavetable + sound) */
    if (s->fds_enabled && addr >= 0x4020 && addr <= 0x408Fu) {
        uint8_t v = fds_reg_read(&s->fds, addr);
        s->cpu.irq = fds_tick(&s->fds);  /* recalculate IRQ after flag clear */
        return v;
    }

    if (addr == 0x4015) {
        uint8_t v = apu2a03_read(&s->apu, 0x4015);
        if (!s->apu.fc_irq && !s->apu.dmc.irq_flag)
            s->cpu.irq = false;
        return v;
    }

    /* VS. System: DIP switches and coin signals layered onto $4016/$4017.
     * Hardware wiring: left stick → $4017, right stick → $4016.
     * VS. SMB P1 uses left stick ($4017) and P2 uses right stick ($4016),
     * so ctrl_state[0] (P1 keys) feeds $4017 and ctrl_state[1] (P2 keys) feeds $4016.
     * $4016 D0 = P2 shift reg | D2 = coin1 | D3 = coin2 | D4-D5 = DIP[0:1] | D6 = service
     * $4017 D0 = P1 shift reg | D2-D7 = DIP[2:7] */
    if (s->cfg->is_arcade && addr == 0x4016) {
        uint8_t bit;
        if (s->ctrl_strobe)
            s->ctrl_shift[1] = s->ctrl_state[1];
        bit = s->ctrl_shift[1] & 1;
        s->ctrl_shift[1] >>= 1;
        uint8_t v = bit
             | (s->vs_coin_latch[0] > 0 ? 0x04u : 0u)         /* D2: coin 1 */
             | (s->vs_coin_latch[1] > 0 ? 0x08u : 0u)         /* D3: coin 2 */
             | (uint8_t)((s->vs_dip & 0x03u) << 4)             /* D4-D5: DIP bits 0-1 */
             | (s->vs_service           ? 0x40u : 0u);         /* D6: service */
        if (s->vs_service || s->vs_coin_latch[0] > 0 || s->vs_coin_latch[1] > 0)
            VS_DEBUG(s, "read 4016 -> %02X pc=%04X strobe=%d dip=%02X coin=%d/%d\n",
                     v, s->cpu.PC, s->ctrl_strobe ? 1 : 0, s->vs_dip,
                     s->vs_coin_latch[0], s->vs_coin_latch[1]);
        return v;
    }
    if (s->cfg->is_arcade && addr == 0x4017) {
        uint8_t bit;
        if (s->ctrl_strobe)
            s->ctrl_shift[0] = s->ctrl_state[0];
        bit = s->ctrl_shift[0] & 1;
        s->ctrl_shift[0] >>= 1;
        uint8_t v = bit
             | (uint8_t)(s->vs_dip & ~0x03u);                 /* D2-D7: DIP bits 2-7 */
        return v;
    }

    if (s->cfg->is_arcade && addr >= 0x4020 && addr < 0x6000) {
        VS_DEBUG(s, "read %04X -> %02X pc=%04X coin_counter\n",
                 addr, s->vs_coin_counter, s->cpu.PC);
        return s->vs_coin_counter;
    }

    if (addr == 0x4016) {
        if (s->cfg->ports[0] != NES_DEVICE_CONTROLLER) return 0;
        if (s->ctrl_strobe) return (s->ctrl_state[0] & NES_BTN_A) ? 1u : 0u;
        uint8_t bit = s->ctrl_shift[0] & 1;
        s->ctrl_shift[0] = (s->ctrl_shift[0] >> 1) | 0x80u;
        /* D2: Famicom hardwired controller 2 microphone */
        uint8_t mic = (s->cfg->ports[1] == NES_DEVICE_FC2_MIC && s->mic_noise) ? 0x04u : 0u;
        return bit | mic;
    }
    if (addr == 0x4017) {
        if (s->cfg->ports[1] == NES_DEVICE_ZAPPER) {
            /* Bit 4: trigger (1 = half-pulled); bit 3: light sense (0 = detected) */
            uint8_t trigger = (s->zapper_trigger_ttl > 0) ? 0x10u : 0u;
            uint8_t light   = 0x08u; /* default: not detected */
            int sl = s->ppu.scanline;
            if (sl >= 0 && sl < 240 &&
                s->zapper_x >= 0 && s->zapper_x < 256 &&
                s->zapper_y >= 0 && s->zapper_y < 240 &&
                abs(sl - s->zapper_y) <= 8) {
                uint32_t argb = s->ppu.pixels_argb[s->zapper_y * RP2C02_WIDTH + s->zapper_x];
                uint8_t r = (uint8_t)(argb >> 16);
                uint8_t g = (uint8_t)(argb >>  8);
                uint8_t b = (uint8_t)(argb);
                uint8_t luma = (uint8_t)((r * 77u + g * 150u + b * 29u) >> 8);
                if (luma >= 85) light = 0; /* bright pixel → light detected */
            }
            return trigger | light;
        }
        if (s->cfg->ports[1] != NES_DEVICE_CONTROLLER &&
            s->cfg->ports[1] != NES_DEVICE_FC2_MIC &&
            !nes_device_is_rob(s->cfg->ports[1])) return 0;
        if (s->ctrl_strobe) return (s->ctrl_state[1] & NES_BTN_A) ? 1u : 0u;
        uint8_t bit = s->ctrl_shift[1] & 1;
        s->ctrl_shift[1] = (s->ctrl_shift[1] >> 1) | 0x80u;
        return bit;
    }

    /* MMC5: readable registers at $5100-$5BFF and ExRAM at $5C00-$5FFF */
    if (s->cart.mapper == 5 && addr >= 0x5000 && addr < 0x6000) {
        if (addr == 0x5204) {
            uint8_t v = (s->mmc5_irq_pending ? 0x80u : 0u) |
                        (s->mmc5_in_frame    ? 0x40u : 0u);
            s->mmc5_irq_pending = false;
            s->cpu.irq = false;
            return v;
        }
        if (addr == 0x5205) return (uint8_t)((uint16_t)s->mmc5_mul[0] * s->mmc5_mul[1]);
        if (addr == 0x5206) return (uint8_t)(((uint16_t)s->mmc5_mul[0] * s->mmc5_mul[1]) >> 8);
        if (addr >= 0x5C00)
            return (s->mmc5_exram_mode >= 2) ? s->mmc5_exram[addr - 0x5C00u] : 0;
        return 0;
    }

    if (addr >= 0x6000 && addr < 0x8000) {
        if (s->fds_enabled) return s->fds.ram[addr - 0x6000u];
        if (s->cart.mapper == 5)
            return s->prg_ram[addr & 0x1FFFu]; /* $5113 always maps RAM here */
        if ((s->cart.mapper == 1 && !(s->mmc1_prg & 0x10)) ||
            s->cart.mapper == 4 || s->cart.mapper == 24 || s->cart.mapper == 26 ||
            s->cart.mapper == 178 || s->cfg->is_arcade)
            return s->prg_ram[addr & 0x1FFF];
        return 0;
    }

    if (addr >= 0x8000) {
        if (s->fds_enabled) {
            if (addr >= 0xE000)
                return s->fds.bios[addr - 0xE000u];          /* BIOS ROM */
            return s->fds.ram[addr - 0x6000u];               /* $8000–$DFFF: upper FDS RAM */
        }
        if (!s->prg) return 0;
        uint8_t rom;
        if (s->cart.mapper == 9) {
            uint8_t slot   = (uint8_t)((addr - 0x8000u) >> 13);  /* 0..3 */
            uint32_t prg8k = (uint32_t)s->cart.prg_banks * 2u;
            uint32_t bank8;
            if      (slot == 0) bank8 = s->m9_prg_reg & 0x0Fu;
            else if (slot == 1) bank8 = prg8k - 3u;
            else if (slot == 2) bank8 = prg8k - 2u;
            else                bank8 = prg8k - 1u;  /* slot 3 */
            rom = s->prg[bank8 * 0x2000u + (addr & 0x1FFFu)];
        } else if (s->cart.mapper == 5) {
            uint8_t slot = (uint8_t)((addr - 0x8000u) >> 13);
            if (s->mmc5_prg_is_ram[slot])
                rom = s->prg_ram[(s->mmc5_prg_offsets[slot] + (addr & 0x1FFFu)) & 0x1FFFu];
            else
                rom = s->prg[s->mmc5_prg_offsets[slot] + (addr & 0x1FFFu)];
        } else if (s->cart.mapper == 4 || s->cart.mapper == 24 || s->cart.mapper == 26) {
            uint8_t slot = (uint8_t)((addr - 0x8000u) >> 13);
            rom = s->prg[s->mmc3_prg_offsets[slot] + (addr & 0x1FFFu)];
        } else if (s->cart.mapper >= 1) {
            uint8_t slot = addr >= 0xC000 ? 1 : 0;
            rom = s->prg[s->prg_offsets[slot] + (addr & 0x3FFF)];
        } else {
            /* NROM: 1 bank → mirrored; 2 banks → direct 32 KB */
            uint32_t off = addr - 0x8000u;
            if (s->cart.prg_banks == 1) off &= 0x3FFF;
            rom = s->prg[off];
        }
        for (int i = 0; i < s->gg_count; i++) {
            if (s->gg_patches[i].addr == addr &&
                (!s->gg_patches[i].has_cmp || rom == s->gg_patches[i].cmp))
                return s->gg_patches[i].val;
        }
        return rom;
    }

    return 0;  /* open bus */
}

static void nes_cpu_write(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;
    gemu_monitor_check_write(s->monitor, addr);

    if (addr < 0x2000) { s->ram[addr & 0x07FF] = val; return; }

    if (addr < 0x4000) {
        if (s->cfg->is_arcade) {
            uint8_t r = (uint8_t)(addr & 7);
            if (r == 0 || r == 1 || r == 5 || r == 6)
                VS_DEBUG(s, "write 200%u <- %02X pc=%04X frame=%llu sl=%d dot=%d\n",
                         r, val, s->cpu.PC,
                         (unsigned long long)s->ppu.frame,
                         s->ppu.scanline, s->ppu.dot);
        }
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        rp2c02_write(&s->ppu, (uint8_t)(addr & 7), val);
        return;
    }

    /* FDS I/O registers ($4020-$408A covers disk regs, wavetable and sound) */
    if (s->fds_enabled && addr >= 0x4020 && addr <= 0x408Au) {
        fds_reg_write(&s->fds, addr, val);
        /* $4025 bit 3 controls mirroring (0=vertical, 1=horizontal) */
        if (addr == 0x4025)
            s->ppu.mirror = (val & 0x08) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
        s->cpu.irq = fds_tick(&s->fds);
        return;
    }

    if (addr == 0x4014) {
        VS_DEBUG(s, "write 4014 <- %02X pc=%04X dma\n", val, s->cpu.PC);
        /* OAM DMA: stall CPU 513 cycles, copy 256 bytes to OAM */
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        uint8_t page[256];
        uint16_t base = (uint16_t)val << 8;
        for (int i = 0; i < 256; i++)
            page[i] = nes_cpu_read((uint16_t)(base + i), s);
        rp2c02_oam_dma(&s->ppu, page);
        s->cpu.cycle_count += 513;
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        return;
    }

    if (addr == 0x4016) {
        bool new_strobe = (val & 1) != 0;
        if (s->cfg->is_arcade &&
            (new_strobe != s->ctrl_strobe ||
             ((val & 0x04u) ? 0x2000u : 0u) != s->chr_offsets[0]))
            VS_DEBUG(s, "write 4016 <- %02X pc=%04X strobe %d->%d chr=%05X\n",
                     val, s->cpu.PC, s->ctrl_strobe ? 1 : 0,
                     new_strobe ? 1 : 0, (val & 0x04u) ? 0x2000u : 0u);
        if (s->cfg->is_arcade) {
            if (new_strobe) {
                s->ctrl_shift[0] = s->ctrl_state[0];
                s->ctrl_shift[1] = s->ctrl_state[1];
            }
        } else if (s->ctrl_strobe && !new_strobe) {
            s->ctrl_shift[0] = s->ctrl_state[0];
            s->ctrl_shift[1] = s->ctrl_state[1];
        }
        s->ctrl_strobe = new_strobe;
        /* VS. System: bit 2 selects 8KB VROM bank (0 = bank 2b at offset 0, 1 = bank 2a at 0x2000) */
        if (s->cfg->is_arcade && !s->chr_is_ram) {
            uint32_t bank_off = (val & 0x04u) ? 0x2000u : 0u;
            s->chr_offsets[0] = bank_off;
            s->chr_offsets[1] = bank_off + 0x1000u;
        }
        return;
    }

    if (s->cfg->is_arcade && addr >= 0x4020 && addr < 0x6000) {
        s->vs_coin_counter = val;
        VS_DEBUG(s, "write %04X <- %02X pc=%04X coin_counter\n", addr, val, s->cpu.PC);
        return;
    }

    /* APU registers */
    if ((addr >= 0x4000 && addr <= 0x4013) || addr == 0x4015 || addr == 0x4017) {
        apu2a03_write(&s->apu, addr, val);
        /* $4010/$4017 can clear APU IRQ flags; drop cpu.irq if both are now clear */
        if (!s->apu.fc_irq && !s->apu.dmc.irq_flag)
            s->cpu.irq = false;
        return;
    }

    if (s->cart.mapper == 178 && addr >= 0x4800 && addr <= 0x4803) {
        switch (addr) {
        case 0x4800: s->m178_mode   = val; break;
        case 0x4801: s->m178_prg_lo = val; break;
        case 0x4802: s->m178_prg_hi = val; break;
        default: break; /* $4803: PRG-RAM bank — stored implicitly, single 8KB bank only */
        }
        m178_update_banks(s);
        return;
    }

    /* MMC5 registers at $5000-$5BFF and ExRAM at $5C00-$5FFF */
    if (s->cart.mapper == 5 && addr >= 0x5000 && addr < 0x6000) {
        if (addr == 0x5100) { s->mmc5_prg_mode = val & 3u; mmc5_update_prg_banks(s); return; }
        if (addr == 0x5101) { s->mmc5_chr_mode = val & 3u; return; }
        /* $5102/$5103: PRG-RAM protect — always allow writes (not enforced) */
        if (addr == 0x5104) { s->mmc5_exram_mode = val & 3u; return; }
        if (addr == 0x5105) { s->mmc5_nt_map = val; return; }
        if (addr == 0x5106) { s->mmc5_fill_tile = val; return; }
        if (addr == 0x5107) { s->mmc5_fill_attr = val & 3u; return; }
        if (addr >= 0x5113 && addr <= 0x5117) {
            s->mmc5_prg_regs[addr - 0x5113u] = val;
            mmc5_update_prg_banks(s);
            return;
        }
        if (addr >= 0x5120 && addr <= 0x5127) {
            s->mmc5_chr_regs[addr - 0x5120u] = val;
            s->mmc5_last_chr_bg = false;
            return;
        }
        if (addr >= 0x5128 && addr <= 0x512B) {
            s->mmc5_chr_regs[8u + (addr - 0x5128u)] = val;
            s->mmc5_last_chr_bg = true;
            return;
        }
        if (addr == 0x5130) { s->mmc5_chr_hi = val & 3u; return; }
        if (addr == 0x5203) { s->mmc5_irq_target = val; return; }
        if (addr == 0x5204) {
            s->mmc5_irq_enabled = (val & 0x80u) != 0;
            if (!s->mmc5_irq_enabled) { s->mmc5_irq_pending = false; s->cpu.irq = false; }
            return;
        }
        if (addr == 0x5205) { s->mmc5_mul[0] = val; return; }
        if (addr == 0x5206) { s->mmc5_mul[1] = val; return; }
        if (addr >= 0x5C00 && s->mmc5_exram_mode <= 2) {
            s->mmc5_exram[addr - 0x5C00u] = val;
            return;
        }
        return; /* ignore other MMC5 writes */
    }

    if (addr >= 0x6000 && addr < 0x8000) {
        if (s->fds_enabled) { s->fds.ram[addr - 0x6000u] = val; return; }
        if (s->cart.mapper == 5) { s->prg_ram[addr & 0x1FFFu] = val; return; }
        if ((s->cart.mapper == 1 && !(s->mmc1_prg & 0x10)) ||
            s->cart.mapper == 4 || s->cart.mapper == 24 || s->cart.mapper == 26 ||
            s->cart.mapper == 178 || s->cfg->is_arcade)
            s->prg_ram[addr & 0x1FFF] = val;
        return;
    }

    /* FDS: $8000–$DFFF is writable RAM */
    if (s->fds_enabled && addr >= 0x8000 && addr < 0xE000) {
        s->fds.ram[addr - 0x6000u] = val;
        return;
    }

    if (addr >= 0x8000) {
        if      (s->cart.mapper == 1) mmc1_serial_write(s, addr, val);
        else if (s->cart.mapper == 2) s->prg_offsets[0] = ((uint32_t)(val % s->cart.prg_banks)) * 0x4000u;
        else if (s->cart.mapper == 66) {
            /* GxROM has bus conflicts: the CPU write value is ANDed with
             * the ROM byte currently driving the data bus. */
            val &= nes_prg_direct(s, addr);
            uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
            uint32_t chr_size = (uint32_t)s->cart.chr_banks * 0x2000u;
            s->prg_offsets[0] = (((uint32_t)(val >> 4) & 3u) * 0x8000u) % prg_size;
            s->prg_offsets[1] = s->prg_offsets[0] + 0x4000u;
            if (chr_size > 0) {
                s->chr_offsets[0] = (((uint32_t)val & 3u) * 0x2000u) % chr_size;
                s->chr_offsets[1] = s->chr_offsets[0] + 0x1000u;
            }
        }
        else if (s->cart.mapper == 4) mmc3_cpu_write(s, addr, val);
        else if (s->cart.mapper == 9) {
            /* MMC2 register map (decoded from CPU A14..A12):
             *   $A000-$AFFF  PRG bank (bits 3:0)
             *   $B000-$BFFF  left CHR  FD-latch bank (bits 4:0)
             *   $C000-$CFFF  left CHR  FE-latch bank
             *   $D000-$DFFF  right CHR FD-latch bank
             *   $E000-$EFFF  right CHR FE-latch bank
             *   $F000-$FFFF  mirroring (bit 0: 0=vertical, 1=horizontal) */
            if (addr < 0xB000)
                s->m9_prg_reg = val & 0x0Fu;
            else if (addr < 0xC000)
                s->m9_chr_lfd = val & 0x1Fu;
            else if (addr < 0xD000)
                s->m9_chr_lfe = val & 0x1Fu;
            else if (addr < 0xE000)
                s->m9_chr_rfd = val & 0x1Fu;
            else if (addr < 0xF000)
                s->m9_chr_rfe = val & 0x1Fu;
            else
                s->ppu.mirror = (val & 1u) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
        }
        else if (s->cart.mapper == 7) {
            /* AxROM: bits 2:0 = 32KB PRG bank, bit 4 = mirror (0=single-A, 1=single-B) */
            uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
            uint8_t  bank     = val & 0x07u;
            s->prg_offsets[0] = ((uint32_t)bank * 0x8000u) % prg_size;
            s->prg_offsets[1] = s->prg_offsets[0] + 0x4000u;
            s->ppu.mirror     = (val & 0x10u) ? RP2C02_MIRROR_SINGLE_B
                                               : RP2C02_MIRROR_SINGLE_A;
        }
        else if (s->cart.mapper == 228) {
            /* Action 52 — NESdev register layout (address + data bus):
             *   FEDCBA98 76543210  (full 16-bit address)
             *   1.MHHPPP PPS.CCCC
             * D(13)=mirror  C:B(12:11)=chip-sel  A→6(10:6)=page(5-bit)
             * 5=PRG-mode  3:0=chr_hi   data[1:0]=chr_lo
             * Three 512KB chips at chip-sel 0,1,3; chip-sel 2 = open bus. */
            uint8_t chip     = (uint8_t)((addr >> 11) & 3u);
            uint8_t page     = (uint8_t)((addr >> 6) & 0x1Fu);
            uint8_t prg_mode = (uint8_t)((addr >> 5) & 1u);
            uint8_t chr_hi   = (uint8_t)(addr & 0xFu);
            uint8_t mirror   = (uint8_t)((addr >> 13) & 1u);
            uint8_t chr_bank = (uint8_t)((chr_hi << 2) | (val & 3u));

            if (chip == 2u) return; /* open bus — no PRG chip, ignore */

            uint32_t global_bank = (chip == 3u) ? (64u + page)
                                                 : ((uint32_t)chip * 32u + page);
            uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
            if (!prg_mode) {
                /* 32KB split: round to even/odd pair */
                uint32_t base = (global_bank & ~1u) * 0x4000u;
                s->prg_offsets[0] = base % prg_size;
                s->prg_offsets[1] = (base + 0x4000u) % prg_size;
            } else {
                /* 16KB: same bank mirrored in both $8000 and $C000 windows */
                uint32_t base = (global_bank * 0x4000u) % prg_size;
                s->prg_offsets[0] = base;
                s->prg_offsets[1] = base;
            }

            uint32_t chr_size = (uint32_t)s->cart.chr_banks * 0x2000u;
            if (chr_size > 0) {
                s->chr_offsets[0] = ((uint32_t)chr_bank * 0x2000u) % chr_size;
                s->chr_offsets[1] = s->chr_offsets[0] + 0x1000u;
            }

            s->ppu.mirror = mirror ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
        }
        else if (s->cart.mapper == 24 || s->cart.mapper == 26)
            vrc6_cpu_write(s, addr, val);
    }
}

/* ── FDS media device ────────────────────────────────────────────────────── */

static GemuMediaResult fds_media_change(void *ud, const char *arg,
                                         char *err, size_t err_len) {
    NesState *s = ud;
    if (!arg || !arg[0]) { snprintf(err, err_len, "missing disk path"); return GEMU_MEDIA_ERR; }
    nes_fds_save(s);
    s->sav_path[0] = '\0';
    fds_disk_eject(&s->fds);
    if (!fds_disk_load(&s->fds, arg)) {
        snprintf(err, err_len, "failed to load '%s'", arg);
        return GEMU_MEDIA_ERR;
    }
    nes_fds_save_setup(s, arg);
    if (s->fds.hle_mode)
        fds_hle_boot(&s->fds, s->chr);
    return GEMU_MEDIA_OK;
}

static GemuMediaResult fds_media_eject(void *ud, char *err, size_t err_len) {
    (void)err; (void)err_len;
    NesState *s = ud;
    nes_fds_save(s);
    s->sav_path[0] = '\0';
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

/* ── Cartridge media device ──────────────────────────────────────────────── */

static GemuMediaResult nes_media_change(void *ud, const char *arg,
                                         char *err, size_t err_len) {
    NesState *s = ud;
    if (!arg || !arg[0]) {
        snprintf(err, err_len, "missing cartridge path");
        return GEMU_MEDIA_ERR;
    }
    nes_sav_save(s);
    s->battery_autosave = false;
    s->sav_path[0] = '\0';
    free(s->prg); s->prg = NULL;
    free(s->chr); s->chr = NULL;
    if (!ines_load(s, arg)) {
        snprintf(err, err_len, "failed to load '%s'", arg);
        return GEMU_MEDIA_ERR;
    }
    nes_battery_setup(s);
    return GEMU_MEDIA_OK_RESET;
}

static GemuMediaResult nes_media_eject(void *ud, char *err, size_t err_len) {
    (void)err; (void)err_len;
    NesState *s = ud;
    nes_sav_save(s);
    s->battery_autosave = false;
    s->sav_path[0] = '\0';
    free(s->prg); s->prg = NULL;
    free(s->chr); s->chr = NULL;
    s->cart_path_buf[0] = '\0';
    printf("cartridge: ejected\n");
    return GEMU_MEDIA_OK;
}

static void nes_media_status(void *ud, char *buf, size_t buf_len) {
    const NesState *s = ud;
    snprintf(buf, buf_len, "%s",
             s->cart_path_buf[0] ? s->cart_path_buf : "no cartridge");
}

/* ── VNC key → controller ─────────────────────────────────────────────────── */

/* X11 keysym constants used by VNC */
#define XK_Left   0xFF51u
#define XK_Up     0xFF52u
#define XK_Right  0xFF53u
#define XK_Down   0xFF54u
#define XK_Return 0xFF0Du
#define XK_ShiftR 0xFFE2u
#define XK_F2     0xFFBFu

/* held: bitmask returned by gemu_display_poll() this frame (0 if headless) */
static void nes_handle_keys(NesState *s, uint32_t held) {
    if (s->cfg->is_arcade) {
        /* VS. arcade: bits 0-7 = P1, 8-15 = P2, 16 = Coin1, 17 = Coin2, 18 = Service */
        if (s->display) {
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
        if (s->vnc) {
            GemuVncKeyEvent ev;
            while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
                uint8_t btn = 0;
                int page = 0;
                switch (ev.keysym) {
                case 'z': case 'Z': btn = NES_BTN_A;     page = 1; break;
                case 'x': case 'X': btn = NES_BTN_B;     page = 1; break;
                case XK_Return:     btn = VS_BTN_START;  page = 1; break;
                case XK_Up:         btn = NES_BTN_UP;    page = 1; break;
                case XK_Down:       btn = NES_BTN_DOWN;  page = 1; break;
                case XK_Left:       btn = NES_BTN_LEFT;  page = 1; break;
                case XK_Right:      btn = NES_BTN_RIGHT; page = 1; break;
                case 'd': case 'D': btn = NES_BTN_A;     break;
                case 'f': case 'F': btn = NES_BTN_B;     break;
                case 'g': case 'G': btn = VS_BTN_START;  break;
                case 'r': case 'R': btn = NES_BTN_UP;    break;
                case 'v': case 'V': btn = NES_BTN_DOWN;  break;
                case 'c': case 'C': btn = NES_BTN_LEFT;  break;
                case 'b': case 'B': btn = NES_BTN_RIGHT; break;
                case '5':
                    if (ev.down) s->vs_coin_latch[0] = 15;
                    break;
                case '6':
                    if (ev.down) s->vs_coin_latch[1] = 15;
                    break;
                case XK_F2:
                    s->vs_service = ev.down;
                    break;
                default:
                    break;
                }
                if (btn) {
                    if (ev.down) s->ctrl_state[page] |=  btn;
                    else         s->ctrl_state[page] &= (uint8_t)~btn;
                }
            }
        }
        if (s->ctrl_inject_frames > 0) {
            s->ctrl_state[0] |= s->ctrl_inject_mask;
            s->ctrl_inject_frames--;
        }
        return;
    }

    if (!s->display) {
        /* Headless: clear state unless VNC is feeding us events */
        if (!s->vnc) { s->ctrl_state[0] = 0; s->ctrl_state[1] = 0; }
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
        if (s->cfg->ports[1] == NES_DEVICE_ZAPPER) {
            GemuPointerState ptr = gemu_display_get_pointer(s->display);
            if (ptr.button && s->zapper_trigger_ttl == 0)
                s->zapper_trigger_ttl = 10; /* 10-frame trigger pulse */
            if (s->zapper_trigger_ttl > 0)
                s->zapper_trigger_ttl--;
            s->zapper_x = ptr.x;
            s->zapper_y = ptr.y;
        }
    }

    /* Monitor key injection */
    if (s->ctrl_inject_frames > 0) {
        s->ctrl_state[0] |= s->ctrl_inject_mask;
        s->ctrl_inject_frames--;
    }

    /* VNC: drain key queue; translate to controller buttons */
    if (s->vnc) {
        GemuVncKeyEvent ev;
        while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
            if (s->cfg->ports[0] != NES_DEVICE_CONTROLLER) continue;
            uint8_t btn = 0;
            switch (ev.keysym) {
            case 'z': case 'Z':   btn = NES_BTN_A;      break;
            case 'x': case 'X':   btn = NES_BTN_B;      break;
            case XK_Return:        btn = NES_BTN_START;  break;
            case XK_ShiftR:        btn = NES_BTN_SELECT; break;
            case XK_Up:            btn = NES_BTN_UP;     break;
            case XK_Down:          btn = NES_BTN_DOWN;   break;
            case XK_Left:          btn = NES_BTN_LEFT;   break;
            case XK_Right:         btn = NES_BTN_RIGHT;  break;
            default: break;
            }
            if (btn) {
                if (ev.down) s->ctrl_state[0] |=  btn;
                else         s->ctrl_state[0] &= ~btn;
            }
        }
    }
}

/* ── Screendump ──────────────────────────────────────────────────────────── */

static bool nes_screendump(void *ud, const char *path) {
    NesState *s = ud;
    int w = RP2C02_WIDTH, h = RP2C02_HEIGHT;
    uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return false;
    for (int i = 0; i < w * h; i++) {
        uint32_t c    = rp2c02_palette_rgb[s->ppu.pixels[i] & 0x3F];
        rgb[i*3+0]    = (uint8_t)(c >> 16);
        rgb[i*3+1]    = (uint8_t)(c >>  8);
        rgb[i*3+2]    = (uint8_t)(c      );
    }
    bool ok = gemu_screendump(path, rgb, w, h);
    free(rgb);
    return ok;
}

/* ── Machine lifecycle ───────────────────────────────────────────────────── */

static void nes_reset(NesState *s) {
    rp2c02_reset(&s->ppu);
    if (s->fds_enabled) {
        /* FDS: horizontal mirroring until $4025 says otherwise */
        s->ppu.mirror = RP2C02_MIRROR_HORIZONTAL;
        s->ppu_synced_cpu_cycle = s->cpu.cycle_count;
        mos6502_reset(&s->cpu);
        apu2a03_reset(&s->apu);
        return;
    }
    s->ppu.mirror = s->cart.mirror;
    s->ppu_synced_cpu_cycle = s->cpu.cycle_count;

    /* Mapper bank state must be set before mos6502_reset reads the reset
       vector, otherwise prg_offsets[1] is 0 and the vector is fetched from
       the wrong physical bank. */
    if (s->cart.mapper == 1) {
        s->mmc1_shift       = 0;
        s->mmc1_shift_count = 0;
        s->mmc1_ctrl        = 0x0C;   /* PRG mode 3: fix last bank at $C000 */
        s->mmc1_chr0        = 0;
        s->mmc1_chr1        = 0;
        s->mmc1_prg         = 0;
        mmc1_update_banks(s);
    } else if (s->cart.mapper == 2) {
        s->prg_offsets[0] = 0;
        s->prg_offsets[1] = (uint32_t)(s->cart.prg_banks - 1) * 0x4000u;
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 66) {
        s->prg_offsets[0] = 0;
        s->prg_offsets[1] = 0x4000u;
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 228) {
        /* Reset maps the last two 16KB banks so the reset vector is readable */
        uint32_t last = (uint32_t)(s->cart.prg_banks - 1) * 0x4000u;
        s->prg_offsets[0] = last >= 0x4000u ? last - 0x4000u : 0;
        s->prg_offsets[1] = last;
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 5) {
        s->mmc5_prg_mode    = 3;    /* four 8KB banks */
        s->mmc5_chr_mode    = 3;    /* 1KB CHR banks */
        s->mmc5_exram_mode  = 3;    /* read-only at reset */
        s->mmc5_nt_map      = 0;    /* all CIRAM page 0 */
        s->mmc5_irq_enabled = false;
        s->mmc5_irq_pending = false;
        s->mmc5_in_frame    = false;
        s->mmc5_irq_counter = 0;
        s->mmc5_last_chr_bg = false;
        s->mmc5_exattr_latch = 0;
        memset(s->mmc5_prg_regs,  0xFF, sizeof(s->mmc5_prg_regs));
        memset(s->mmc5_chr_regs,  0,    sizeof(s->mmc5_chr_regs));
        s->mmc5_chr_hi = 0;
        /* $5117 = 0xFF → last 8KB ROM bank at $E000-$FFFF */
        mmc5_update_prg_banks(s);
    } else if (s->cart.mapper == 178) {
        s->m178_mode   = 0;
        s->m178_prg_lo = 0;
        s->m178_prg_hi = 0;
        m178_update_banks(s);
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 9) {
        s->m9_prg_reg  = 0;
        s->m9_chr_lfd  = 0;
        s->m9_chr_lfe  = 0;
        s->m9_chr_rfd  = 0;
        s->m9_chr_rfe  = 0;
        s->m9_latch_l  = false;
        s->m9_latch_r  = false;
    } else if (s->cart.mapper == 7) {
        s->prg_offsets[0] = 0;
        s->prg_offsets[1] = 0x4000u;
        s->ppu.mirror     = RP2C02_MIRROR_SINGLE_A;
    } else if (s->cart.mapper == 4) {
        memset(s->mmc3_regs, 0, sizeof(s->mmc3_regs));
        s->mmc3_bank_sel    = 0;
        s->mmc3_irq_latch   = 0;
        s->mmc3_irq_counter = 0;
        s->mmc3_irq_reload  = false;
        s->mmc3_irq_enabled = false;
        s->cpu.irq          = false;
        mmc3_update_banks(s);
    } else if (s->cart.mapper == 24 || s->cart.mapper == 26) {
        uint32_t n8k = (uint32_t)s->cart.prg_banks * 2u;
        s->mmc3_prg_offsets[0] = 0;
        s->mmc3_prg_offsets[1] = 0x2000u;
        s->mmc3_prg_offsets[2] = 0;
        s->mmc3_prg_offsets[3] = (n8k - 1u) * 0x2000u;  /* $E000: fixed last 8KB */
        memset(s->mmc3_chr_offsets, 0, sizeof(s->mmc3_chr_offsets));
        s->vrc6_irq_latch   = 0;
        s->vrc6_irq_counter = 0;
        s->vrc6_irq_enabled = false;
        s->vrc6_irq_after   = false;
        s->vrc6_irq_mode    = false;
        s->vrc6_prescaler   = 0;
        s->vrc6_p1_ctrl  = 0; s->vrc6_p1_period = 0; s->vrc6_p1_en = false;
        s->vrc6_p1_timer = 0; s->vrc6_p1_seq    = 0;
        s->vrc6_p2_ctrl  = 0; s->vrc6_p2_period = 0; s->vrc6_p2_en = false;
        s->vrc6_p2_timer = 0; s->vrc6_p2_seq    = 0;
        s->vrc6_saw_rate = 0; s->vrc6_saw_period = 0; s->vrc6_saw_en = false;
        s->vrc6_saw_timer = 0; s->vrc6_saw_accum = 0; s->vrc6_saw_step = 0;
        s->vrc6_halt    = false;
        s->cpu.irq      = false;
    }

    mos6502_reset(&s->cpu);
    VS_DEBUG(s, "reset pc=%04X nmi=%04X reset=%04X irq=%04X prgfffc=%02X%02X dip=%02X\n",
             s->cpu.PC,
             (uint16_t)(s->prg[0x7FFAu] | ((uint16_t)s->prg[0x7FFBu] << 8)),
             (uint16_t)(s->prg[0x7FFCu] | ((uint16_t)s->prg[0x7FFDu] << 8)),
             (uint16_t)(s->prg[0x7FFEu] | ((uint16_t)s->prg[0x7FFFu] << 8)),
             s->prg[0x7FFDu], s->prg[0x7FFCu], s->vs_dip);
    apu2a03_reset(&s->apu);
}

/* ── Arcade ROM chip assembler ───────────────────────────────────────────── */
/* Builds a NROM-256 cartridge in memory from individual chip files:
 *   region "prg"  = PRG chips at MAME region offsets 0x0000-0x7FFF
 *   region "gfx1" = CHR chips at MAME region offsets 0x0000-0x3FFF
 *   region "ppu1:palette" = 2C04 palette LUT.
 * Legacy manual loads without a region still accept PRG CPU addresses
 * 0x8000-0xFFFF, CHR at 0x10000-0x1FFFF, and metadata >= 0x20000. */
static bool nes_arcade_assemble(NesState *s, const MosConfig *cfg) {
    uint8_t *prg = calloc(1, 32768);
    uint8_t *chr = calloc(1, 16384); /* 2 × 8 KB VROM banks */
    if (!prg || !chr) { free(prg); free(chr); return false; }

    bool has_prg = false, has_chr = false;

    /* Load PRG chips directly at their declared CPU offset.  VS. sockets are
     * 8 KB; keeping that boundary exact catches missing/swapped chips early. */
    for (int i = 0; i < cfg->n_roms; i++) {
        uint32_t a = cfg->roms[i].addr;
        bool is_prg = cfg->roms[i].region
                   ? strcmp(cfg->roms[i].region, "prg") == 0
                   : (a >= 0x8000 && a < 0x10000);
        if (!is_prg) continue;
        size_t off = cfg->roms[i].region ? a : a - 0x8000u;
        if (off > 32768u - 0x2000u) {
            fprintf(stderr, "arcade: PRG chip '%s' offset 0x%zX is out of range\n",
                    cfg->roms[i].path, off);
            free(prg); free(chr); return false;
        }
        FILE *f = fopen(cfg->roms[i].path, "rb");
        if (!f) continue;
        size_t n = fread(prg + off, 1, 0x2000u, f);
        fclose(f);
        if (n != 0x2000u) {
            fprintf(stderr, "arcade: PRG chip '%s' is %zu bytes, expected 8192\n",
                    cfg->roms[i].path, n);
            free(prg); free(chr); return false;
        }
        has_prg = true;
        VS_DEBUG(s, "loaded PRG %s -> prg:%04zX\n", cfg->roms[i].path, off);
    }

    /* Collect CHR chips, sort by addr, concatenate into the two 8KB VROM banks. */
    struct ChrChip { uint32_t addr; const char *path; } cc[16];
    int ncc = 0;
    for (int i = 0; i < cfg->n_roms && ncc < 16; i++) {
        uint32_t a = cfg->roms[i].addr;
        bool is_chr = cfg->roms[i].region
                   ? strcmp(cfg->roms[i].region, "gfx1") == 0
                   : (a >= 0x10000 && a < 0x20000);
        if (is_chr) {
            cc[ncc].addr = cfg->roms[i].region ? a : a - 0x10000u;
            cc[ncc].path = cfg->roms[i].path;
            ncc++;
        }
    }
    /* insertion sort by addr (tiny n) */
    for (int i = 1; i < ncc; i++) {
        struct ChrChip t = cc[i]; int j = i - 1;
        for (; j >= 0 && cc[j].addr > t.addr; j--) cc[j+1] = cc[j];
        cc[j+1] = t;
    }
    size_t chr_fill = 0;
    for (int i = 0; i < ncc && chr_fill < 16384; i++) {
        FILE *f = fopen(cc[i].path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
        size_t to = (size_t)sz < 16384 - chr_fill ? (size_t)sz : 16384 - chr_fill;
        chr_fill += fread(chr + chr_fill, 1, to, f);
        fclose(f);
        if (to != 0x2000u) {
            fprintf(stderr, "arcade: CHR chip '%s' is %zu bytes, expected 8192\n",
                    cc[i].path, to);
            free(prg); free(chr); return false;
        }
        has_chr = true;
        VS_DEBUG(s, "loaded CHR %s -> gfx1:%04zX\n", cc[i].path, chr_fill - to);
    }

    if (!has_prg) {
        fprintf(stderr, "arcade: no PRG ROM chips found in ROM set\n");
        romdb_print_needed("vssmb");
        free(prg); free(chr); return false;
    }

    s->cart.prg_banks   = 2;   /* 2 × 16KB = 32KB */
    s->cart.chr_banks   = has_chr ? 2 : 0; /* 2 × 8KB VROM banks */
    s->cart.mapper      = 0;
    s->cart.mirror      = RP2C02_MIRROR_4SCREEN;
    s->cart.has_battery = false;

    s->prg = prg;
    if (has_chr) {
        s->chr = chr;
        s->chr_is_ram = false;
    } else {
        free(chr);
        s->chr = calloc(1, 8192);
        s->chr_is_ram = true;
    }

    /* Mapper 0 fixed windows: $8000–$BFFF → prg[0], $C000–$FFFF → prg[16KB] */
    s->prg_offsets[0] = 0;
    s->prg_offsets[1] = 0x4000;
    s->chr_offsets[0] = 0;
    s->chr_offsets[1] = 0x1000;

    /* Load 2C04 palette file if present.  MAME .pal files are 64 palette
     * indexes stored as RGB-looking triples; each first byte selects one of
     * the normal NES RGB entries. */
    for (int i = 0; i < cfg->n_roms; i++) {
        bool is_palette = cfg->roms[i].region
                       ? strcmp(cfg->roms[i].region, "ppu1:palette") == 0
                       : cfg->roms[i].addr >= 0x20000;
        if (!is_palette) continue;
        FILE *pf = fopen(cfg->roms[i].path, "rb");
        if (!pf) continue;
        uint8_t rgb[192];
        size_t n = fread(rgb, 1, sizeof(rgb), pf);
        fclose(pf);
        if (n == 192) {
            for (int j = 0; j < 64; j++)
                s->vs_palette_rgb[j] = 0xFF000000u
                    | ((uint32_t)nes_pal3bit(rgb[j * 3 + 0]) << 16)
                    | ((uint32_t)nes_pal3bit(rgb[j * 3 + 1]) <<  8)
                    |  (uint32_t)nes_pal3bit(rgb[j * 3 + 2]);
            /* ppu not initialised yet; pointer set after rp2c02_init in nes_create */
            VS_DEBUG(s, "loaded 2C04 palette %s\n", cfg->roms[i].path);
        }
        break;
    }

    snprintf(s->cart_path_buf, sizeof(s->cart_path_buf), "vssmb");
    return true;
}

NesState *nes_create(const MosConfig *cfg) {
    NesState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg         = cfg;
    s->fds_enabled = cfg->fds_enabled;

    if (s->fds_enabled) {
        /* 8 KB CHR RAM for PPU (needed before HLE boot so CHR files can load) */
        s->chr = calloc(1, FDS_CHR_SIZE);
        if (!s->chr) { free(s); return NULL; }
        s->chr_is_ram     = true;
        s->cart.chr_banks = 1;
        s->cart.mapper    = 0;

        /* BIOS: use real ROM if provided, otherwise install HLE stub */
        const char *bios_path = NULL;
        for (int i = 0; i < cfg->n_roms; i++) {
            if (cfg->roms[i].addr == 0xE000u) { bios_path = cfg->roms[i].path; break; }
        }
        if (!bios_path && cfg->n_roms > 0)
            bios_path = cfg->roms[0].path;

        if (bios_path) {
            if (!fds_bios_load(&s->fds, bios_path)) { free(s->chr); free(s); return NULL; }
        } else {
            fds_hle_build_rom(&s->fds);
            s->fds.hle_mode = true;
        }

        if (cfg->fda_path) {
            if (!fds_disk_load(&s->fds, cfg->fda_path)) { free(s->chr); free(s); return NULL; }
            nes_fds_save_setup(s, cfg->fda_path);
            if (s->fds.hle_mode)
                fds_hle_boot(&s->fds, s->chr);
        }
    } else {
        if (!cfg->cart_path) {
            if (cfg->is_arcade && cfg->n_roms > 0) {
                if (!nes_arcade_assemble(s, cfg)) { free(s); return NULL; }
            } else {
                fprintf(stderr, "nes: no cartridge specified — use -cartridge FILE.nes\n");
                free(s); return NULL;
            }
        } else {
            if (!ines_load(s, cfg->cart_path)) { free(s); return NULL; }
        }
    }

    /* Wire up PPU CHR bus */
    rp2c02_init(&s->ppu, cfg->is_pal);
    if (cfg->is_dendy) s->ppu.vblank_line = 291;  /* Dendy: 50 extra post-render lines */
    if (cfg->vga == MOS_VGA_RP2C04_0004)
        s->ppu.no_odd_skip = true;
    if (cfg->is_arcade && s->vs_palette_rgb[0])    /* 2C04 palette present */
        s->ppu.alt_palette_rgb = s->vs_palette_rgb;
    s->ppu.chr_read  = nes_chr_read;
    s->ppu.chr_write = nes_chr_write;
    s->ppu.chr_ud    = s;
    s->ppu.mirror    = s->cart.mirror;

    mos6502_init(&s->cpu);
    s->cpu.mem_read        = nes_cpu_read;
    s->cpu.mem_write       = nes_cpu_write;
    s->cpu.mem_ud          = s;
    s->cpu.decimal_disable = (cfg->cpu == MOS_CPU_2A03 ||
                              cfg->cpu == MOS_CPU_2A07);

    /* APU — only initialise when sound is enabled */
    if (cfg->sound == MOS_SOUND_2A03) {
        uint32_t apu_clock = cfg->cpu == MOS_CPU_2A07 ? 1662607u : 1789773u;
        if (!apu2a03_init(&s->apu, apu_clock))
            fprintf(stderr, "nes: APU audio init failed (continuing silently)\n");
        s->apu.mem_read = nes_cpu_read;
        s->apu.mem_ud   = s;
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
    } else if (cfg->sound == MOS_SOUND_2A03_MIDI) {
        s->apu.mem_read    = nes_cpu_read;
        s->apu.mem_ud      = s;
        apu_midi_open(&s->apu_midi);
        s->apu.write_tap    = apu_midi_tap;
        s->apu.write_tap_ud = &s->apu_midi;
#endif
    }

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_screendump_cb(s->monitor, nes_screendump, s);
#ifdef GEMU_GTK
    if (cfg->display_type == GEMU_DISPLAY_GTK) {
        HexRegion nes_regions[] = {
            { "CPU RAM ($0000-$07FF)", s->ram,
              sizeof(s->ram), false, 0x0000 },
            { "PRG ROM ($8000+)", s->prg,
              s->prg ? (size_t)s->cart.prg_banks * 0x4000u : 0,
              true, 0x8000 },
            { "CHR ROM/RAM", s->chr,
              s->chr ? (size_t)s->cart.chr_banks * 0x2000u : 0,
              !s->chr_is_ram, 0x0000 },
        };
        s->hex_editor = hex_editor_create(nes_regions, 3);
    }
#endif
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

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
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
            acts   = nes_2p_actions;
            n_acts = NES_2P_N_ACTIONS;
        } else if (has_fc2_mic) {
            acts      = nes_fc2mic_actions; n_acts  = NES_FC2MIC_N_ACTIONS;
            act_pages = nes_fc2mic_pages;   n_pages = 2;
        }
        s->display = gemu_display_create(cfg->display_type,
            &(GemuDisplayConfig){
                .title       = "GEMU",
                .fb_width    = RP2C02_WIDTH,
                .fb_height   = RP2C02_HEIGHT,
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
        if (!s->display)
            fprintf(stderr, "nes: failed to create display window\n");
        else
            gemu_monitor_set_input_reset_cb(s->monitor,
                                            gemu_display_reset_input_bindings_ud,
                                            s->display);
    }

    if (!s->fds_enabled)
        nes_battery_setup(s);

    if (s->cart.mapper == 4) {
        s->ppu.irq_scanline = mmc3_irq_scanline;
        s->ppu.irq_ud       = s;
    }
    if (s->cart.mapper == 5) {
        s->ppu.irq_scanline = mmc5_irq_scanline;
        s->ppu.irq_ud       = s;
        s->ppu.nt_read      = mmc5_nt_read;
        s->ppu.nt_write     = mmc5_nt_write;
        s->ppu.nt_ud        = s;
    }

    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr, RP2C02_WIDTH, RP2C02_HEIGHT);
        if (s->vnc)
            gemu_vnc_set_palette(s->vnc,
                                 s->ppu.alt_palette_rgb ? s->ppu.alt_palette_rgb
                                                        : rp2c02_palette_rgb,
                                 64);
        else
            fprintf(stderr, "nes: failed to start VNC at %s\n", cfg->vnc_addr);
    }
    gemu_monitor_set_vnc(s->monitor, s->vnc);

#ifdef HAVE_ROB
    for (int p = 0; p < cfg->n_ports; p++) {
        if (nes_device_is_rob(cfg->ports[p])) {
            rob_init(&s->rob);
            s->rob_window = rob_window_create("mdl/rob",
                                              cfg->ports[p] == NES_DEVICE_ROB_FAMICOM);
            break;
        }
    }
#endif

    if (cfg->is_arcade)
        s->vs_dip = VS_DIP_DEFAULT;

    nes_reset(s);

    return s;
}

void nes_destroy(NesState *s) {
    nes_save_persistent(s);
    if (s->fds_enabled) { free(s->fds.raw_disk); }
#ifdef GEMU_GTK
    hex_editor_destroy(s->hex_editor);
#endif
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
    apu_midi_close(&s->apu_midi);
#endif
    apu2a03_destroy(&s->apu);
    gemu_monitor_destroy(s->monitor);
#ifdef HAVE_ROB
    rob_window_destroy(s->rob_window);  /* must happen before display destroy */
#endif
    gemu_display_destroy(s->display);
    gemu_vnc_destroy(s->vnc);
    free(s->prg);
    free(s->chr);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

/* Fallback frame duration when audio is off (headless / -soundhw none). */
#define NES_FRAME_MS       17u   /* NTSC: 1000/60 ≈ 16.67 ms */
#define NES_FRAME_PAL_MS   20u   /* PAL:  1000/50 = 20.00 ms */

/* Audio queue threshold for sync: allow up to 3 frames of latency (~50 ms).
 * ~735 samples/frame * 3 frames * 4 bytes/float = 8820 bytes */
#define AUDIO_QUEUE_MAX  (3u * 735u * (unsigned)sizeof(float))

void nes_run(NesState *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();  /* used for non-audio frame sync below */

        /* Poll display: pump events, update held bitmask, check quit/reset */
        uint32_t held = 0;
        if (s->display) {
            held = gemu_display_poll(s->display);
            if (gemu_display_should_quit(s->display)) break;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                nes_save_persistent(s);
                nes_reset(s);
            }
        }
        /* Monitor commands */
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)   { if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true); else { quit = true; break; } }
            else if (cmd == GEMU_MON_RESET)  { nes_save_persistent(s); nes_reset(s); }
            else if (cmd == GEMU_MON_CUSTOM) {
                const char *text = gemu_monitor_command_text(s->monitor);
                while (*text == ' ' || *text == '\t') text++;
                if (s->cfg->is_arcade &&
                    strncasecmp(text, "dipswitch", 9) == 0 &&
                    (text[9] == '\0' || text[9] == ' ' || text[9] == '\t'))
                    vs_dip_cmd(s, text + 9);
                else if (s->cfg->is_arcade &&
                    strncasecmp(text, "coin", 4) == 0 &&
                    (text[4] == '\0' || text[4] == ' ' || text[4] == '\t'))
                    vs_coin_cmd(s, text + 4);
                else if (s->cfg->is_arcade &&
                    strncasecmp(text, "service", 7) == 0 &&
                    (text[7] == '\0' || text[7] == ' ' || text[7] == '\t'))
                    vs_service_cmd(s);
                else if (strncasecmp(text, "gamegenie", 9) == 0 &&
                    (text[9] == '\0' || text[9] == ' ' || text[9] == '\t'))
                    nes_gamegenie_cmd(s, text);
                else if (strncasecmp(text, "sendkey", 7) == 0 &&
                         (text[7] == '\0' || text[7] == ' ' || text[7] == '\t')) {
                    const char *p = text + 7;
                    while (*p == ' ' || *p == '\t') p++;
                    uint8_t btn = 0;
                    if      (strncasecmp(p, "a",      1) == 0 && (p[1] < 'a' || p[1] > 'z')) btn = NES_BTN_A;
                    else if (strncasecmp(p, "b",      1) == 0 && (p[1] < 'a' || p[1] > 'z')) btn = NES_BTN_B;
                    else if (strncasecmp(p, "start",  5) == 0) btn = s->cfg->is_arcade ? VS_BTN_START : NES_BTN_START;
                    else if (strncasecmp(p, "select", 6) == 0) btn = NES_BTN_SELECT;
                    else if (strncasecmp(p, "up",     2) == 0) btn = NES_BTN_UP;
                    else if (strncasecmp(p, "down",   4) == 0) btn = NES_BTN_DOWN;
                    else if (strncasecmp(p, "left",   4) == 0) btn = NES_BTN_LEFT;
                    else if (strncasecmp(p, "right",  5) == 0) btn = NES_BTN_RIGHT;
                    if (btn) {
                        while (*p && *p != ' ' && *p != '\t') p++;
                        while (*p == ' ' || *p == '\t') p++;
                        int frames = (*p >= '1' && *p <= '9') ? (int)strtol(p, NULL, 10) : 5;
                        s->ctrl_inject_mask   = btn;
                        s->ctrl_inject_frames = frames;
                        printf("sendkey: holding 0x%02X for %d frame(s)\n", btn, frames);
                    } else {
                        printf("sendkey: unknown button (a b start select up down left right)\n");
                    }
                } else if (strncasecmp(text, "ppudump", 7) == 0 &&
                           (text[7] == '\0' || text[7] == ' ' || text[7] == '\t')) {
                    Rp2c02 *ppu = &s->ppu;
                    printf("PPU: ctrl=%02X mask=%02X status=%02X v=%04X t=%04X x=%d w=%d\n",
                           ppu->ppuctrl, ppu->ppumask, ppu->ppustatus,
                           ppu->v, ppu->t, ppu->x, ppu->w);
                    printf("Palette BG:  ");
                    for (int i = 0; i < 16; i++) printf("%02X ", ppu->palette[i]);
                    printf("\nPalette SPR: ");
                    for (int i = 0; i < 16; i++) printf("%02X ", ppu->palette[i + 16]);
                    printf("\nSL0 pixels[0..31]: ");
                    for (int i = 0; i < 32; i++) printf("%02X ", ppu->pixels[i]);
                    printf("\nSL8 pixels[0..31]: ");
                    for (int i = 0; i < 32; i++) printf("%02X ", ppu->pixels[256*8 + i]);
                    printf("\nNT row0 tiles: ");
                    for (int i = 0; i < 32; i++) printf("%02X ", ppu->vram[i]);
                    printf("\nNT row1 tiles: ");
                    for (int i = 0; i < 32; i++) printf("%02X ", ppu->vram[32 + i]);
                    printf("\nNT attr[0..7]: ");
                    for (int i = 0; i < 8; i++) printf("%02X ", ppu->vram[0x3C0 + i]);
                    printf("\nOAM[0..3]:     ");
                    for (int i = 0; i < 16; i++) printf("%02X ", ppu->oam[i]);
                    printf("\nOAM visible:   ");
                    int shown = 0;
                    for (int i = 0; i < 64; i++) {
                        uint8_t y = ppu->oam[i * 4 + 0];
                        if (y >= 0xEF) continue;
                        printf("#%02d:%02X %02X %02X %02X ",
                               i, y, ppu->oam[i * 4 + 1],
                               ppu->oam[i * 4 + 2], ppu->oam[i * 4 + 3]);
                        if (++shown == 12) break;
                    }
                    if (!shown) printf("(none)");
                    printf("\n");
#ifdef GEMU_GTK
                } else if (strncasecmp(text, "hexeditor", 9) == 0 &&
                           (text[9] == '\0' || text[9] == ' ' || text[9] == '\t')) {
                    if (!s->hex_editor) {
                        printf("hexeditor: GTK not available\n");
                    } else if (hex_editor_is_visible(s->hex_editor)) {
                        hex_editor_hide(s->hex_editor);
                        printf("hexeditor: closed\n");
                    } else {
                        hex_editor_show(s->hex_editor);
                        printf("hexeditor: opened\n");
                    }
#endif
                } else
                    gemu_monitor_unknown_command(s->monitor);
            }
        }
        if (quit) break;
        if (s->display)
            gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));

        /* Input: display action bits + VNC events */
        nes_handle_keys(s, held);
        if (s->cfg->is_arcade) {
            if (s->vs_coin_latch[0] > 0) s->vs_coin_latch[0]--;
            if (s->vs_coin_latch[1] > 0) s->vs_coin_latch[1]--;
        }

        if (!gemu_monitor_is_paused(s->monitor)) {
            /* Run one full frame (until PPU marks frame complete) */
            s->ppu.dirty = false;
            while (!s->ppu.dirty) {
                if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) break;
                uint64_t prev = s->cpu.cycle_count;
                mos6502_step(&s->cpu);
                uint64_t delta = s->cpu.cycle_count - prev;

                /* APU + VRC6: one tick per CPU cycle */
                bool is_vrc6 = s->cart.mapper == 24 || s->cart.mapper == 26;
                for (uint64_t i = 0; i < delta; i++) {
                    if (is_vrc6)
                        s->apu.fds_in = vrc6_tick(s);
                    else if (s->apu.audio_dev && s->fds_enabled)
                        s->apu.fds_in = fds_audio_tick(&s->fds);
                    if (s->apu.audio_dev) apu2a03_tick(&s->apu);
                }
                if (s->apu.fc_irq || s->apu.dmc.irq_flag)
                    s->cpu.irq = true;

                nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
                if (gemu_monitor_is_paused(s->monitor)) break;
            }
            VS_DEBUG(s, "frame=%llu pc=%04X a=%02X x=%02X y=%02X p=%02X sp=%02X "
                        "ctrl=%02X mask=%02X status=%02X chr=%05X/%05X nmi=%d irq=%d\n",
                     (unsigned long long)s->ppu.frame, s->cpu.PC,
                     s->cpu.A, s->cpu.X, s->cpu.Y, s->cpu.P, s->cpu.SP,
                     s->ppu.ppuctrl, s->ppu.ppumask, s->ppu.ppustatus,
                     s->chr_offsets[0], s->chr_offsets[1],
                     s->cpu.nmi ? 1 : 0, s->cpu.irq ? 1 : 0);

            /* Flush APU samples to SDL audio */
            apu2a03_flush(&s->apu);

            /* Render completed frame (PPU already builds pixels_argb) */
            if (s->display)
                gemu_display_render(s->display, s->ppu.pixels_argb,
                                    RP2C02_WIDTH, RP2C02_HEIGHT);
            if (s->vnc)
                gemu_vnc_update(s->vnc, s->ppu.pixels,
                                RP2C02_WIDTH, RP2C02_HEIGHT);
#ifdef HAVE_ROB
            if (s->cfg->n_ports > 1 && nes_device_is_rob(s->cfg->ports[1])) {
                rob_frame(&s->rob, s->ppu.pixels_argb, RP2C02_WIDTH, RP2C02_HEIGHT);
                s->ctrl_state[1] = 0;
                if (s->rob.btn_a) s->ctrl_state[1] |= NES_BTN_A;
                if (s->rob.btn_b) s->ctrl_state[1] |= NES_BTN_B;
            }
            if (s->rob_window) {
                if (rob_window_close_requested(s->rob_window)) {
                    rob_window_destroy(s->rob_window);
                    s->rob_window = NULL;
                } else {
                    rob_window_render(s->rob_window, &s->rob);
                }
            }
#endif
        }

        /* Frame sync:
         * - Audio enabled: block until SDL has consumed enough samples so the
         *   queue stays at ~3 frames. This locks emulation to the audio clock
         *   (exactly 60.099 fps) and eliminates drift entirely.
         * - Audio off / headless: fall back to a software timer. */
        if (s->apu.audio_dev) {
            while (SDL_GetQueuedAudioSize(s->apu.audio_dev) > AUDIO_QUEUE_MAX)
                SDL_Delay(1);
        } else {
            Uint32 elapsed = SDL_GetTicks() - t0;
            Uint32 frame_ms = cfg->is_pal ? NES_FRAME_PAL_MS : NES_FRAME_MS;
            if (elapsed < frame_ms)
                SDL_Delay(frame_ms - elapsed);
        }

#ifdef GEMU_GTK
        hex_editor_refresh(s->hex_editor);
        /* GTK event pump is done inside gemu_display_poll() via the backend */
#endif
    }

    printf("nes: %llu frames, %llu cpu cycles\n",
           (unsigned long long)s->ppu.frame,
           (unsigned long long)s->cpu.cycle_count);
    gemu_monitor_stop(s->monitor);
}
