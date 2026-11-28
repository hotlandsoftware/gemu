/*
 * Atari 2600 machine - see hardware/mos/a2600/atari2600.h for the memory map.
 */
#include "atari2600.h"
#include "a2600_bus.h"
#include "a2600_core.h"
#include "gemu/screendump.h"
#include "a2600_cart_state.h"
#include "controller/m6532_state.h"
#include "mos6502_state.h"
#include "tia_state.h"
#include "gemu/dev/device.h"
#include "gemu/dev/option.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/state.h"
#include "gemu/dev/stream.h"
#include <SDL2/SDL.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ── Palettes (Stella's standard NTSC / PAL tables) ──────────────────────── */

static const uint32_t ntsc_palette[128] = {
    0x000000, 0x4a4a4a, 0x6f6f6f, 0x8e8e8e, 0xaaaaaa, 0xc0c0c0, 0xd6d6d6, 0xececec,
    0x484800, 0x69690f, 0x86861d, 0xa2a22a, 0xbbbb35, 0xd2d240, 0xe8e84a, 0xfcfc54,
    0x7c2c00, 0x904811, 0xa26221, 0xb47a30, 0xc3903d, 0xd2a44a, 0xdfb755, 0xecc860,
    0x901c00, 0xa33915, 0xb55328, 0xc66c3a, 0xd5824a, 0xe39759, 0xf0aa67, 0xfcbc74,
    0x940000, 0xa71a1a, 0xb83232, 0xc84848, 0xd65c5c, 0xe46f6f, 0xf08080, 0xfc9090,
    0x840064, 0x97197a, 0xa8308f, 0xb846a2, 0xc659b3, 0xd46cc3, 0xe07cd2, 0xec8ce0,
    0x500084, 0x68199a, 0x7d30ad, 0x9246c0, 0xa459d0, 0xb56ce0, 0xc57cee, 0xd48cfc,
    0x140090, 0x331aa3, 0x4e32b5, 0x6848c6, 0x7f5cd5, 0x956fe3, 0xa980f0, 0xbc90fc,
    0x000094, 0x181aa7, 0x2d32b8, 0x4248c8, 0x545cd6, 0x656fe4, 0x7580f0, 0x8490fc,
    0x001c88, 0x183b9d, 0x2d57b0, 0x4272c2, 0x548ad2, 0x65a0e1, 0x75b5ef, 0x84c8fc,
    0x003064, 0x185080, 0x2d6d98, 0x4288b0, 0x54a0c5, 0x65b7d9, 0x75cceb, 0x84e0fc,
    0x004030, 0x18624e, 0x2d8169, 0x429e82, 0x54b899, 0x65d1ae, 0x75e7c2, 0x84fcd4,
    0x004400, 0x1a661a, 0x328432, 0x48a048, 0x5cba5c, 0x6fd26f, 0x80e880, 0x90fc90,
    0x143c00, 0x355f18, 0x527e2d, 0x6e9c42, 0x87b754, 0x9ed065, 0xb4e775, 0xc8fc84,
    0x303800, 0x505916, 0x6d762b, 0x88923e, 0xa0ab4f, 0xb7c25f, 0xccd86e, 0xe0ec7c,
    0x482c00, 0x694d14, 0x866a26, 0xa28638, 0xbb9f47, 0xd2b656, 0xe8cc63, 0xfce070,
};

static const uint32_t pal_palette[128] = {
    0x000000, 0x2b2b2b, 0x525252, 0x767676, 0x979797, 0xb6b6b6, 0xd2d2d2, 0xececec,
    0x000000, 0x2b2b2b, 0x525252, 0x767676, 0x979797, 0xb6b6b6, 0xd2d2d2, 0xececec,
    0x805800, 0x96711a, 0xab8732, 0xbe9c48, 0xcfaf5c, 0xdfc06f, 0xeed180, 0xfce090,
    0x445c00, 0x5e791a, 0x769332, 0x8cac48, 0xa0c25c, 0xb3d76f, 0xc4ea80, 0xd4fc90,
    0x703400, 0x89511a, 0xa06b32, 0xb68448, 0xc99a5c, 0xdcaf6f, 0xecc280, 0xfcd490,
    0x006414, 0x1a8035, 0x329852, 0x48b06e, 0x5cc587, 0x6fd99e, 0x80ebb4, 0x90fcc8,
    0x700014, 0x891a35, 0xa03252, 0xb6486e, 0xc95c87, 0xdc6f9e, 0xec80b4, 0xfc90c8,
    0x005c5c, 0x1a7676, 0x328e8e, 0x48a4a4, 0x5cb8b8, 0x6fcbcb, 0x80dcdc, 0x90ecec,
    0x70005c, 0x841a74, 0x963289, 0xa8489e, 0xb75cb0, 0xc66fc1, 0xd380d1, 0xe090e0,
    0x003c70, 0x195a89, 0x2f75a0, 0x448eb6, 0x57a5c9, 0x68badc, 0x79ceec, 0x88e0fc,
    0x580070, 0x6e1a89, 0x8332a0, 0x9648b6, 0xa75cc9, 0xb76fdc, 0xc680ec, 0xd490fc,
    0x002070, 0x193f89, 0x2f5aa0, 0x4474b6, 0x578bc9, 0x68a1dc, 0x79b5ec, 0x88c8fc,
    0x340080, 0x4a1a96, 0x5f32ab, 0x7248be, 0x835ccf, 0x936fdf, 0xa280ee, 0xb090fc,
    0x000088, 0x1a1a9d, 0x3232b0, 0x4848c2, 0x5c5cd2, 0x6f6fe1, 0x8080ef, 0x9090fc,
    0x000000, 0x2b2b2b, 0x525252, 0x767676, 0x979797, 0xb6b6b6, 0xd2d2d2, 0xececec,
    0x000000, 0x2b2b2b, 0x525252, 0x767676, 0x979797, 0xb6b6b6, 0xd2d2d2, 0xececec,
};

/* SECAM sets the colour from the luminance bits alone. */
static const uint32_t secam_colors[8] = {
    0x000000, 0x2121ff, 0xf03c79, 0xff50ff, 0x7fff00, 0x7fffff, 0xffff3f, 0xffffff,
};

static uint32_t palette_color(const Atari2600State *s, uint8_t colu) {
    if (s->cfg->is_secam) return 0xFF000000u | secam_colors[(colu >> 1) & 7u];
    return 0xFF000000u | (s->pal ? pal_palette : ntsc_palette)[colu >> 1];
}

/* ── Input actions ───────────────────────────────────────────────────────── */

static const GemuActionDef a26_actions[A26_NUM_ACTIONS] = {
    { "Up",               GEMU_ACTION(A26_ACT_P1_UP),      "Up"         },
    { "Down",             GEMU_ACTION(A26_ACT_P1_DOWN),    "Down"       },
    { "Left",             GEMU_ACTION(A26_ACT_P1_LEFT),    "Left"       },
    { "Right",            GEMU_ACTION(A26_ACT_P1_RIGHT),   "Right"      },
    { "Fire",             GEMU_ACTION(A26_ACT_P1_FIRE),    "Space"      },
    { "P2 Up",            GEMU_ACTION(A26_ACT_P2_UP),      "w"          },
    { "P2 Down",          GEMU_ACTION(A26_ACT_P2_DOWN),    "s"          },
    { "P2 Left",          GEMU_ACTION(A26_ACT_P2_LEFT),    "a"          },
    { "P2 Right",         GEMU_ACTION(A26_ACT_P2_RIGHT),   "d"          },
    { "P2 Fire",          GEMU_ACTION(A26_ACT_P2_FIRE),    "Left Shift" },
    { "Game Reset",       GEMU_ACTION(A26_ACT_RESET),      "F2"         },
    { "Game Select",      GEMU_ACTION(A26_ACT_SELECT),     "F1"         },
    { "Color/B&W",        GEMU_ACTION(A26_ACT_COLOR),      "F3"         },
    { "Left Difficulty",  GEMU_ACTION(A26_ACT_LEFT_DIFF),  "F5"         },
    { "Right Difficulty", GEMU_ACTION(A26_ACT_RIGHT_DIFF), "F6"         },
};

/* One controller: P1 actions + console switches. */
static const GemuActionDef a26_actions_1p[10] = {
    { "Up",               GEMU_ACTION(A26_ACT_P1_UP),      "Up"    },
    { "Down",             GEMU_ACTION(A26_ACT_P1_DOWN),    "Down"  },
    { "Left",             GEMU_ACTION(A26_ACT_P1_LEFT),    "Left"  },
    { "Right",            GEMU_ACTION(A26_ACT_P1_RIGHT),   "Right" },
    { "Fire",             GEMU_ACTION(A26_ACT_P1_FIRE),    "Space" },
    { "Game Reset",       GEMU_ACTION(A26_ACT_RESET),      "F2"    },
    { "Game Select",      GEMU_ACTION(A26_ACT_SELECT),     "F1"    },
    { "Color/B&W",        GEMU_ACTION(A26_ACT_COLOR),      "F3"    },
    { "Left Difficulty",  GEMU_ACTION(A26_ACT_LEFT_DIFF),  "F5"    },
    { "Right Difficulty", GEMU_ACTION(A26_ACT_RIGHT_DIFF), "F6"    },
};

static const char *ctrl_name(A2600Controller c) {
    switch (c) {
    case A2600_CTRL_CX10: return "cx10";
    case A2600_CTRL_CX30: return "cx30";
    case A2600_CTRL_NONE: return "none";
    default:              return "cx40";
    }
}

static bool is_joystick(A2600Controller c) {
    return c == A2600_CTRL_CX40 || c == A2600_CTRL_CX10;
}

/* Which paddle (0-3) player p (0/1) turns, or -1. */
static int player_paddle(const Atari2600State *s, int p) {
    if (s->cfg->a2600_ports[0] == A2600_CTRL_CX30) return p;
    if (s->cfg->a2600_ports[1] == A2600_CTRL_CX30) return 2 + p;
    return -1;
}

/* ── RIOT ports ──────────────────────────────────────────────────────────── */

static uint8_t a26_read_pa(void *ud) {
    const Atari2600State *s = ud;
    uint32_t h = s->held;
    uint8_t v = 0xFF;
    for (int port = 0; port < 2; port++) {
        A2600Controller c = s->cfg->a2600_ports[port];
        int shift = port ? 0 : 4;
        if (is_joystick(c)) {
            int base = port ? A26_ACT_P2_UP : A26_ACT_P1_UP;
            if (h & GEMU_ACTION(base + 0)) v &= (uint8_t)~(0x1u << shift);   /* up */
            if (h & GEMU_ACTION(base + 1)) v &= (uint8_t)~(0x2u << shift);   /* down */
            if (h & GEMU_ACTION(base + 2)) v &= (uint8_t)~(0x4u << shift);   /* left */
            if (h & GEMU_ACTION(base + 3)) v &= (uint8_t)~(0x8u << shift);   /* right */
        } else if (c == A2600_CTRL_CX30) {   /* paddle buttons on the right/left lines */
            for (int p = 0; p < 2; p++)
                if (player_paddle(s, p) == port * 2 + p &&
                    (h & GEMU_ACTION(p ? A26_ACT_P2_FIRE : A26_ACT_P1_FIRE)))
                    v &= (uint8_t)~((p ? 0x4u : 0x8u) << shift);
        }
    }
    return v;
}

static uint8_t a26_read_pb(void *ud) {
    const Atari2600State *s = ud;
    uint8_t v = 0x3F;   /* Color; buttons released; bits 2, 4, 5 float high */
    if (s->held & GEMU_ACTION(A26_ACT_RESET))  v &= (uint8_t)~0x01u;
    if (s->held & GEMU_ACTION(A26_ACT_SELECT)) v &= (uint8_t)~0x02u;
    if (!s->color)    v &= (uint8_t)~0x08u;
    if (s->diff_a[0]) v |= 0x40u;
    if (s->diff_a[1]) v |= 0x80u;
    return v;
}

/* Joystick fire buttons go to INPT4/5; paddles to the pot inputs. */
static void update_tia_inputs(Atari2600State *s) {
    for (int port = 0; port < 2; port++) {
        bool fire = is_joystick(s->cfg->a2600_ports[port]) &&
                    (s->held & GEMU_ACTION(port ? A26_ACT_P2_FIRE : A26_ACT_P1_FIRE));
        s->tia.fire[port] = fire;
    }
    for (int i = 0; i < 4; i++) {
        bool connected = s->cfg->a2600_ports[i / 2] == A2600_CTRL_CX30;
        /* Charge time grows with the pot's resistance (counter-clockwise). */
        s->tia.pot_charge[i] = connected
            ? (uint32_t)(TIA_LINE_CLOCKS * (4.0 + s->paddle[i] * 230.0)) : UINT32_MAX;
    }
}

/* ── Reset / frame output ────────────────────────────────────────────────── */

/* Power cycle (the console's own Reset is a switch the program reads). */
static void a26_power_on(Atari2600State *s) {
    a26_bus_power_on(s);
    if (s->cfg->has_start_addr) s->cpu.PC = s->cfg->start_addr;
    s->frame_start_cycle = s->cpu.cycle_count;
}

/* Pick the 240 lines to show: centred on the lines the program un-blanked
 * in the last frame, moved only when that shifts by more than a few lines
 * so the picture doesn't jitter. */
static void update_window(Atari2600State *s) {
    const Tia *t = &s->tia;
    int start;
    if (t->done_first_lit >= 0 && t->done_last_lit - t->done_first_lit >= 100)
        start = (t->done_first_lit + t->done_last_lit + 1) / 2 - A2600_FB_H / 2;
    else
        start = (t->frame_lines - A2600_FB_H) / 2;
    if (start < 0) start = 0;
    if (start > TIA_MAX_LINES - A2600_FB_H) start = TIA_MAX_LINES - A2600_FB_H;
    if (abs(start - s->window_start) > 4) s->window_start = start;

    /* Auto-detect a PAL program from its line count. */
    if (!s->cfg->is_pal && !s->cfg->is_secam) s->pal = t->frame_lines >= 290;
}

static void build_frame(Atari2600State *s) {
    const uint8_t (*frame)[TIA_WIDTH] = tia_frame(&s->tia);
    for (int i = 0; i < 128; i++) s->lut[i] = palette_color(s, (uint8_t)(i << 1));
    for (int y = 0; y < A2600_FB_H; y++) {
        const uint8_t *src = frame[s->window_start + y];
        uint32_t *dst = s->fb + y * A2600_FB_W;
        for (int x = 0; x < TIA_WIDTH; x++)
            dst[2 * x] = dst[2 * x + 1] = s->lut[src[x] >> 1];
    }
}

const uint32_t *a26_palette(const Atari2600State *s) {
    return s->lut;
}

/* Saves what the window shows: the picture, or the full-frame view. */
static bool a26_screendump(void *ud, const char *path) {
    Atari2600State *s = ud;
    if (s->display && gemu_display_head(s->display) == 1 && s->dbg.full_fb)
        return gemu_screendump_argb(path, s->dbg.full_fb, A26_FULL_W, A26_FULL_H);
    return gemu_screendump_argb(path, s->fb, A2600_FB_W, A2600_FB_H);
}

/* ── Monitor ─────────────────────────────────────────────────────────────── */

static void a26_cpu_state(void *ud, char *buf, size_t len) {
    const Atari2600State *s = ud;
    char cpu[160];
    mos6502_format_state(&s->cpu, cpu, sizeof(cpu));
    snprintf(buf, len, "%s frame=%" PRIu64 " line=%d hctr=%d intim=%02X",
             cpu, s->tia.frame_count, s->tia.scanline, s->tia.hctr,
             m6532_timer(&s->riot, s->cpu.cycle_count));
}

static bool a26_mon_read(void *ud, uint64_t addr64, uint8_t *val) {
    const Atari2600State *s = ud;
    uint16_t addr = (uint16_t)(addr64 & 0x1FFFu);
    if (addr64 > 0xFFFF) return false;
    if (addr & 0x1000u)        *val = a26_cart_peek(&s->cart, addr);
    else if (!(addr & 0x80u))  *val = tia_peek(&s->tia, addr);
    else if (!(addr & 0x200u)) *val = s->riot.ram[addr & 0x7Fu];
    else                       *val = m6532_peek(&s->riot, addr, s->cpu.cycle_count);
    return true;
}

/* Side-effect-free bus read (debugger, disassembler). */
uint8_t a26_peek(const Atari2600State *s, uint16_t addr) {
    uint8_t v = 0;
    a26_mon_read((void *)s, addr, &v);
    return v;
}

static bool a26_mon_write(void *ud, uint64_t addr64, uint8_t val) {
    Atari2600State *s = ud;
    uint16_t addr = (uint16_t)(addr64 & 0x1FFFu);
    if (addr64 > 0xFFFF || (addr & 0x1000u) || !(addr & 0x80u) || (addr & 0x200u)) return false;
    s->riot.ram[addr & 0x7Fu] = val;
    return true;
}

/* sendkey <button>[-<button>...] [frames]: up down left right fire reset
 * select, p2up... p2fire for the right controller. */
static GemuMonCmd a26_cmd_sendkey(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    static const struct { const char *name; int act; } names[] = {
        { "up", A26_ACT_P1_UP }, { "down", A26_ACT_P1_DOWN }, { "left", A26_ACT_P1_LEFT },
        { "right", A26_ACT_P1_RIGHT }, { "fire", A26_ACT_P1_FIRE }, { "button", A26_ACT_P1_FIRE },
        { "p2up", A26_ACT_P2_UP }, { "p2down", A26_ACT_P2_DOWN }, { "p2left", A26_ACT_P2_LEFT },
        { "p2right", A26_ACT_P2_RIGHT }, { "p2fire", A26_ACT_P2_FIRE },
        { "reset", A26_ACT_RESET }, { "select", A26_ACT_SELECT },
    };
    uint32_t mask = 0;
    const char *p = args;
    while (*p && *p != ' ' && *p != '\t') {
        size_t len = strcspn(p, "-+ \t");
        bool found = false;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (strlen(names[i].name) == len && strncasecmp(p, names[i].name, len) == 0) {
                mask |= GEMU_ACTION(names[i].act);
                found = true;
            }
        if (!found) {
            gemu_monitor_printf(mon, "sendkey: unknown button '%.*s' (up down left right fire "
                                     "reset select, p2up... p2fire; combine with -)\n", (int)len, p);
            return GEMU_MON_NONE;
        }
        p += len;
        if (*p == '-' || *p == '+') p++;
    }
    if (!mask) {
        gemu_monitor_printf(mon, "usage: sendkey <button>[-<button>...] [frames]\n");
        return GEMU_MON_NONE;
    }
    while (*p == ' ' || *p == '\t') p++;
    int frames = (*p >= '1' && *p <= '9') ? (int)strtol(p, NULL, 10) : 5;
    s->inject_mask = mask;
    s->inject_frames = frames;
    gemu_monitor_printf(mon, "sendkey: holding for %d frame(s)\n", frames);
    return GEMU_MON_NONE;
}

/* dipswitch [list | color color|bw | left a|b | right a|b]: the console's
 * toggle switches. */
static GemuMonCmd a26_cmd_dipswitch(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    char sw[32] = "", val[32] = "";
    int n = sscanf(args, "%31s %31s", sw, val);
    if (n <= 0 || strcasecmp(sw, "list") == 0) {
        gemu_monitor_printf(mon, "color  %s   (color | bw)\n", s->color ? "color" : "bw");
        gemu_monitor_printf(mon, "left   %s       (a | b: left difficulty)\n", s->diff_a[0] ? "a" : "b");
        gemu_monitor_printf(mon, "right  %s       (a | b: right difficulty)\n", s->diff_a[1] ? "a" : "b");
        return GEMU_MON_NONE;
    }
    if (n < 2) {
        gemu_monitor_printf(mon, "usage: dipswitch <color|left|right> <value>\n");
        return GEMU_MON_NONE;
    }
    if (strcasecmp(sw, "color") == 0 && (!strcasecmp(val, "color") || !strcasecmp(val, "bw"))) {
        s->color = strcasecmp(val, "color") == 0;
    } else if ((!strcasecmp(sw, "left") || !strcasecmp(sw, "right")) &&
               (!strcasecmp(val, "a") || !strcasecmp(val, "b"))) {
        s->diff_a[strcasecmp(sw, "right") == 0] = strcasecmp(val, "a") == 0;
    } else {
        gemu_monitor_printf(mon, "dipswitch: unknown setting '%s %s' (try 'dipswitch list')\n", sw, val);
        return GEMU_MON_NONE;
    }
    gemu_monitor_printf(mon, "%s = %s\n", sw, val);
    return GEMU_MON_NONE;
}

static GemuMonCmd a26_cmd_mapperdump(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    (void)args;
    char buf[160];
    a26_cart_describe(&s->cart, buf, sizeof(buf));
    gemu_monitor_printf(mon, "%s\n", buf);
    return GEMU_MON_NONE;
}

static GemuMonCmd a26_cmd_hexeditor(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    (void)args;
    if (!s->hex_editor) {
        gemu_monitor_printf(mon, "hexeditor: not available with this display\n");
    } else if (hex_editor_is_visible(s->hex_editor)) {
        hex_editor_hide(s->hex_editor);
        gemu_monitor_printf(mon, "hexeditor: closed\n");
    } else {
        hex_editor_show(s->hex_editor);
        gemu_monitor_printf(mon, "hexeditor: opened\n");
    }
    return GEMU_MON_NONE;
}

static void a26_hex_toggle(void *ud) {
    Atari2600State *s = ud;
    if (!s->hex_editor) return;
    if (hex_editor_is_visible(s->hex_editor)) hex_editor_hide(s->hex_editor);
    else                                       hex_editor_show(s->hex_editor);
}

/* ── Save states (stage T6) ──────────────────────────────────────────────────
 * savevm/loadvm are the core machine's state (registered in stage T1): the
 * board's post_load re-points the ROM windows and builds the picture.  A
 * file in the old format (the raw struct, "2GATAI26") is refused by name:
 * no converter, as decided for the i2000 and the NES. */

#define A26_OLD_SNAPSHOT_MAGIC 0x3632494154414732ULL   /* "2GATAI26" */

static GemuMonCmd a26_cmd_savevm(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    char path[256];
    if (sscanf(args, "%255s", path) != 1) {
        gemu_monitor_printf(mon, "usage: savevm <file>\n");
        return GEMU_MON_NONE;
    }
    a26_bus_sync(s);
    if (gemu_machine_save_file(s->core->machine, path))
        gemu_monitor_printf(mon, "saved snapshot to %s (frame=%" PRIu64 ")\n", path, s->tia.frame_count);
    else
        gemu_monitor_printf(mon, "savevm: %s\n", gemu_machine_error(s->core->machine));
    return GEMU_MON_NONE;
}

static GemuMonCmd a26_cmd_loadvm(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    char path[256];
    if (sscanf(args, "%255s", path) != 1) {
        gemu_monitor_printf(mon, "usage: loadvm <file>\n");
        return GEMU_MON_NONE;
    }
    FILE *f = fopen(path, "rb");
    uint64_t magic = 0;
    uint32_t version = 0;
    if (f) {
        if (fread(&magic, sizeof magic, 1, f) == 1 && magic == A26_OLD_SNAPSHOT_MAGIC &&
            fread(&version, sizeof version, 1, f) != 1)
            version = 0;
        fclose(f);
    }
    if (magic == A26_OLD_SNAPSHOT_MAGIC) {
        gemu_monitor_printf(mon, "loadvm: %s is a save state in the old format (version %u), "
                            "which this GEMU no longer reads; save a new one\n", path, version);
        return GEMU_MON_NONE;
    }
    if (gemu_machine_load_file(s->core->machine, path))
        gemu_monitor_printf(mon, "loaded snapshot from %s (frame=%" PRIu64 ")\n", path, s->tia.frame_count);
    else
        gemu_monitor_printf(mon, "loadvm: %s\n", gemu_machine_error(s->core->machine));
    return GEMU_MON_NONE;
}

/* ── Cartridge slot ──────────────────────────────────────────────────────── */

static void make_hex_editor(Atari2600State *s);

static GemuMediaResult a26_cart_change(void *ud, const char *arg, char *err, size_t err_len) {
    Atari2600State *s = ud;
    if (!arg || !arg[0]) { snprintf(err, err_len, "missing cartridge path"); return GEMU_MEDIA_ERR; }
    if (!a26_cart_load(&s->cart, arg)) {
        snprintf(err, err_len, "failed to load '%s'", arg);
        return GEMU_MEDIA_ERR;
    }
    snprintf(s->cart_path, sizeof(s->cart_path), "%s", arg);
    make_hex_editor(s);
    a26_power_on(s);
    return GEMU_MEDIA_OK;
}

static GemuMediaResult a26_cart_eject(void *ud, char *err, size_t err_len) {
    (void)err; (void)err_len;
    Atari2600State *s = ud;
    a26_cart_free(&s->cart);
    s->cart_path[0] = '\0';
    make_hex_editor(s);
    a26_power_on(s);
    return GEMU_MEDIA_OK;
}

bool a26_reload_cart(Atari2600State *s, char *err, size_t err_len) {
    if (!s->cart_path[0]) { snprintf(err, err_len, "no cartridge inserted"); return false; }
    if (!a26_cart_load(&s->cart, s->cart_path)) {
        snprintf(err, err_len, "failed to load '%s'", s->cart_path);
        return false;
    }
    make_hex_editor(s);
    a26_power_on(s);
    return true;
}

static void a26_cart_status(void *ud, char *buf, size_t len) {
    const Atari2600State *s = ud;
    if (!s->cart.rom) { snprintf(buf, len, "[not inserted]"); return; }
    snprintf(buf, len, "%s (%s%s, %uK)", s->cart_path, a26_cart_type_name(s->cart.type),
             s->cart.sc ? "SC" : "", (unsigned)((s->cart.rom_size + 1023) / 1024));
}

static void make_hex_editor(Atari2600State *s) {
    if (s->hex_editor) { hex_editor_destroy(s->hex_editor); s->hex_editor = NULL; }
    HexRegion regions[3] = {
        { "RIOT RAM ($80-$FF)", s->riot.ram, sizeof(s->riot.ram), false, 0x80 },
        { "Cartridge ROM", s->cart.rom, s->cart.rom_size, true, 0 },
        { "Cartridge RAM", s->cart.ram, s->cart.ram_size, false, 0 },
    };
    s->hex_editor = hex_editor_create(s->cfg->display_type, regions, s->cart.ram_size ? 3 : 2);
}


/* ── The core machine (stage T) ──────────────────────────────────────────────
 * As the NES's (stage N1): the parts stay where Atari2600State keeps them;
 * each device describes its part and registers its state.  board: the
 * console's own state (the data bus, the WSYNC hold, the switches, the
 * paddles, the picture window); cpu: the 6507; tia; riot; cart: the slot.
 * The bus is a core space (a2600_bus.c). */
/* A26Core: a2600_core.h */

static Atari2600State *a26_of(GemuDevice *d) { return *(Atari2600State **)d->priv; }

static void a26_board_describe(GemuDevice *d, char *buf, size_t len) {
    const Atari2600State *s = a26_of(d);
    snprintf(buf, len, "data bus %02X, last address %04X%s\n"
             "switches: %s, left %c, right %c; paddles %.2f %.2f %.2f %.2f\n"
             "%s palette, picture from line %d",
             s->bus, s->last_addr, s->wsync ? ", WSYNC held" : "",
             s->color ? "colour" : "B&W", s->diff_a[0] ? 'A' : 'B', s->diff_a[1] ? 'A' : 'B',
             s->paddle[0], s->paddle[1], s->paddle[2], s->paddle[3],
             s->cfg->is_secam ? "SECAM" : s->pal ? "PAL" : "NTSC", s->window_start);
}

static void a26_cpu_describe(GemuDevice *d, char *buf, size_t len) {
    const Mos6502 *c = &a26_of(d)->cpu;
    snprintf(buf, len, "PC %04X  A %02X  X %02X  Y %02X  SP %02X  P %02X\n"
             "cycle %" PRIu64 ", instruction %" PRIu64,
             c->PC, c->A, c->X, c->Y, c->SP, c->P, c->cycle_count, c->insn_count);
}

static void a26_tia_describe(GemuDevice *d, char *buf, size_t len) {
    const Tia *t = &a26_of(d)->tia;
    snprintf(buf, len, "frame %" PRIu64 " (%d lines), line %d, colour clock %d\n"
             "VSYNC %02X  VBLANK %02X  COLUBK %02X  COLUPF %02X  CTRLPF %02X\n"
             "sound: AUDC %X/%X  AUDF %02X/%02X  AUDV %X/%X",
             t->frame_count, t->frame_lines, t->scanline, t->hctr,
             t->vsync, t->vblank, t->colubk, t->colupf, t->ctrlpf,
             t->sound.ch[0].audc, t->sound.ch[1].audc, t->sound.ch[0].audf,
             t->sound.ch[1].audf, t->sound.ch[0].audv, t->sound.ch[1].audv);
}

static void a26_riot_describe(GemuDevice *d, char *buf, size_t len) {
    const Atari2600State *s = a26_of(d);
    const M6532 *r = &s->riot;
    snprintf(buf, len, "port A %02X (DDR %02X), port B %02X (DDR %02X)\n"
             "timer %02X (interval %u)",
             r->ora, r->ddra, r->orb, r->ddrb, m6532_timer(r, s->cpu.cycle_count),
             1u << r->timer_shift);
}

static void a26_cart_slot_describe(GemuDevice *d, char *buf, size_t len) {
    const A2600Cart *c = &a26_of(d)->cart;
    if (!c->rom) { snprintf(buf, len, "empty"); return; }
    a26_cart_describe(c, buf, (int)len);
}

static void build_frame(Atari2600State *s);
/* After a load: the cartridge's ROM windows (its banks), the picture from
 * the TIA's last frame; the TIA's unsent samples dropped. */
static void a26_board_post_load(GemuDevice *d) {
    Atari2600State *s = a26_of(d);
    a26_maps_sync(s);
    build_frame(s);
    s->tia.sound.buf_n = 0;   /* samples made since: another timeline's */
}

#define A26_TYPE(var, tname, tdesc, describe_fn) \
    static const GemuDeviceType var = { .name = tname, .desc = tdesc, \
        .size = sizeof(Atari2600State *), .describe = describe_fn }
/* The machine's settings (stage T7): the console's switches at power-on,
 * -M atari2600,-color,left_difficulty=a and the like (gemu_opts_route()).
 * The keys and the monitor's dipswitch still flip them while it runs. */
static const char *const a26_diff_choices[] = { "b", "a", NULL };
static const GemuOptionDef a26_board_opts[] = {
    { "color", "Color/B&W switch: colour (off: B&W)", GEMU_OPT_BOOL, "on", 0, 0, NULL },
    { "left_difficulty", "Left difficulty switch (b: novice, a: expert)", GEMU_OPT_CHOICE, "b", 0, 0,
      a26_diff_choices },
    { "right_difficulty", "Right difficulty switch (b: novice, a: expert)", GEMU_OPT_CHOICE, "b", 0, 0,
      a26_diff_choices },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};
static const GemuDeviceType a26_board_type = {
    .name = "atari2600-board", .desc = "Atari 2600 console board", .size = sizeof(Atari2600State *),
    .describe = a26_board_describe, .post_load = a26_board_post_load, .options = a26_board_opts,
};
A26_TYPE(a26_cpu_type, "mos6507", "MOS 6507 CPU (6502, 13 address lines)", a26_cpu_describe);
A26_TYPE(a26_tia_type, "tia", "Atari TIA (video, sound, inputs)", a26_tia_describe);
A26_TYPE(a26_riot_type, "mos6532", "MOS 6532 RIOT (RAM, I/O, timer)", a26_riot_describe);
A26_TYPE(a26_cart_type, "atari2600-cart-slot", "Atari 2600 cartridge slot", a26_cart_slot_describe);

#define BOARD(field) ok &= GEMU_STATE_MEMBER(d, "", s, field)
/* The board's own state.  Not saved: the configuration (cfg, color_hz),
 * the host's input this frame (held, prev_held, inject_*), what follows
 * from the cartridge (bus_fast, bus_fast_mask) or the frame (fb, lut: the
 * post_load builds them), the cartridge's path, the debugger and the
 * front end. */
static bool a26_board_state(Atari2600State *s, GemuDevice *d) {
    bool ok = true;
    BOARD(bus);
    BOARD(last_addr);
    BOARD(wsync);
    BOARD(color);
    BOARD(diff_a);
    BOARD(paddle);
    BOARD(pal);
    BOARD(window_start);
    BOARD(frame_start_cycle);
    return ok;
}
#undef BOARD

/* ── The 6507 as the machine's executor (stage T3) ──────────────────────────
 * Clocked at the colour clock, three to a CPU cycle, so the machine's time
 * is exact.  A run executes instructions (with the WSYNC hold) until its
 * budget is spent, the TIA finishes a frame, or the monitor or the debugger
 * stops it; the last two stop the machine.  The TIA still catches up with
 * the CPU lazily (a2600_bus.c).  The monitor's step runs instructions
 * directly, as before. */
static void a26_end_frame(Atari2600State *s);
static uint64_t a26_exec_run(void *ud, uint64_t budget) {
    Atari2600State *s = ud;
    A26Core *c = s->core;
    (void)budget;
    uint64_t start = c->run_start = s->cpu.cycle_count;
    while ((s->cpu.cycle_count - start) * 3u < gemu_executor_budget(c->exec)) {
        if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) { c->stopped = true; break; }
        if (a26_debug_before_step(s)) { c->stopped = true; break; }
        a26_bus_step(s);
        if (a26_debug_after_step(s)) { c->stopped = true; break; }
        if (gemu_monitor_is_paused(s->monitor)) { c->stopped = true; break; }
        if (s->tia.frame_ready) break;
    }
    if (c->stopped || s->tia.frame_ready)
        gemu_machine_stop(c->machine);
    return (s->cpu.cycle_count - start) * 3u;
}

static uint64_t a26_exec_elapsed(void *ud) {
    Atari2600State *s = ud;
    return (s->cpu.cycle_count - s->core->run_start) * 3u;
}

/* Run until the TIA finishes a frame or something stops the machine, a
 * frame's worth of colour clocks at a time. */
static void a26_run_frame(Atari2600State *s) {
    A26Core *c = s->core;
    c->stopped = false;
    uint64_t hz = (uint64_t)s->color_hz;
    while (!s->tia.frame_ready && !c->stopped)
        gemu_machine_run_until(c->machine, gemu_machine_time(c->machine) +
                               gemu_clocks(hz, (uint64_t)TIA_LINE_CLOCKS * 262u + 1u));
}

/* ── The TIA's screen (stage T4) ─────────────────────────────────────────────
 * 320×240: the 240 lines the window shows, each colour clock two pixels
 * wide (build_frame()).  Device-timed: a frame begins when the TIA finishes
 * one (a26_end_frame()).  The raw timing describes the beam - 228 colour
 * clocks a line, 68 of them blank, the last frame's lines, the window's
 * first line - for vpos/hpos, and follows the program's line count.  The
 * debugger's full-frame view stays the display's second head. */
static void a26_screen_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    const Atari2600State *s = ud;
    for (int y = clip->y0; y < clip->y1; y++)
        memcpy(bmp->pix + (size_t)y * (size_t)bmp->stride, s->fb + (size_t)y * A2600_FB_W,
               A2600_FB_W * sizeof(uint32_t));
}

static void a26_screen_timing(Atari2600State *s) {
    A26Core *c = s->core;
    int lines = s->tia.frame_lines > A2600_FB_H ? s->tia.frame_lines : 262;
    int top = s->window_start;
    if (top + A2600_FB_H > lines) top = lines - A2600_FB_H;
    if (!c->screen || (c->screen_lines == lines && c->screen_top == top)) return;
    c->screen_lines = lines;
    c->screen_top = top;
    gemu_screen_set_raw(c->screen, (uint64_t)s->color_hz * 2u, TIA_LINE_CLOCKS * 2, TIA_HBLANK * 2,
                        TIA_LINE_CLOCKS * 2, lines, top, top + A2600_FB_H, false);
}

static void a26_screen_frame(Atari2600State *s) {
    A26Core *c = s->core;
    if (!c || !c->screen) return;
    a26_screen_timing(s);
    gemu_screen_frame_begin(c->screen);
}

static bool a26_screen_build(Atari2600State *s) {
    A26Core *c = s->core;
    if (!(c->screen = gemu_screen_new(c->tia, "screen")) ||
        !gemu_screen_set_size(c->screen, A2600_FB_W, A2600_FB_H))
        return false;
    gemu_screen_set_aspect(c->screen, 4, 3);
    gemu_screen_set_update(c->screen, a26_screen_update, s);
    a26_screen_timing(s);
    return true;
}

GemuScreen *a26_screen(Atari2600State *s) { return s->core ? s->core->screen : NULL; }

/* ── The TIA's stream (stage T5) ─────────────────────────────────────────────
 * The TIA makes its samples as it catches up (two a line, box-filtered to
 * 44.1 kHz, the DC taken out) into sound.buf.  Its stream hands them on as
 * the mixer asks, in order, none lost: a request for more than there are
 * repeats the last; what's left over waits for the next.  The mixer runs at
 * the end of each frame (a26_frame()), when the TIA has caught up, and
 * sends the result to the host audio device. */
static void a26_sound_update(void *ud, float *const *out, unsigned n) {
    Atari2600State *s = ud;
    TiaSound *ts = &s->tia.sound;
    unsigned have = (unsigned)ts->buf_n, take = n < have ? n : have;
    memcpy(out[0], ts->buf, take * sizeof(float));
    float last = take ? out[0][take - 1] : (have ? ts->buf[0] : 0.0f);
    for (unsigned i = take; i < n; i++) out[0][i] = last;
    if (take < have)
        memmove(ts->buf, ts->buf + take, (have - take) * sizeof(float));
    ts->buf_n = (int)(have - take);
}

static void a26_sound_out(void *ud, const float *samples, size_t frames) {
    gemu_audio_queue(ud, samples, frames);
}

static bool a26_sound_build(Atari2600State *s) {
    A26Core *c = s->core;
    if (!(c->sound = gemu_stream_new(c->tia, "sound", 1, TIA_SOUND_RATE, a26_sound_update, s)))
        return false;
    gemu_mixer_set_output(c->machine, TIA_SOUND_RATE, 1,
                          s->tia.sound.dev ? a26_sound_out : NULL, s->tia.sound.dev);
    return true;
}

static GemuDevice *a26_part_opts(Atari2600State *s, const GemuDeviceType *t, const char *name,
                                 const char *opts) {
    GemuDevice *d = gemu_device_add(s->core->machine, t, name, NULL, opts && *opts ? opts : NULL);
    if (d)
        *(Atari2600State **)d->priv = s;
    return d;
}

static GemuDevice *a26_part(Atari2600State *s, const GemuDeviceType *t, const char *name) {
    GemuDevice *d = gemu_device_add(s->core->machine, t, name, NULL, NULL);
    if (d)
        *(Atari2600State **)d->priv = s;
    return d;
}

static bool a26_core_build(Atari2600State *s) {
    A26Core *c = s->core = calloc(1, sizeof *c);
    if (!c || !(c->machine = gemu_machine_new("atari2600")))
        return false;
    static const GemuDeviceType *const set_types[] = { &a26_board_type, &a26_cpu_type, &a26_tia_type,
                                                       &a26_riot_type, &a26_cart_type };
    char set[5][GEMU_OPTS_ROUTE_LEN], why[512];
    if (!gemu_opts_route(s->cfg->machine_opts, set_types, 5, set, why, sizeof why)) {
        fprintf(stderr, "gemu: %s\n", why);
        return false;
    }
    bool ok = (c->board = a26_part_opts(s, &a26_board_type, "board", set[0])) &&
              a26_board_state(s, c->board) &&
              (c->cpu = a26_part(s, &a26_cpu_type, "cpu")) && mos6502_state_add(c->cpu, "", &s->cpu) &&
              (c->tia = a26_part(s, &a26_tia_type, "tia")) && tia_state_add(c->tia, "", &s->tia) &&
              (c->riot = a26_part(s, &a26_riot_type, "riot")) && m6532_state_add(c->riot, "", &s->riot) &&
              (c->cart = a26_part(s, &a26_cart_type, "cart")) && a26_cart_state_add(c->cart, "", &s->cart);
    if (ok) {   /* the switches' power-on positions */
        s->color = gemu_opt_bool(c->board, "color");
        s->diff_a[0] = gemu_opt_choice(c->board, "left_difficulty") == 1;
        s->diff_a[1] = gemu_opt_choice(c->board, "right_difficulty") == 1;
    }
    if (!ok) {   /* before init, which clears the reason (a bad setting, say) */
        fprintf(stderr, "atari2600: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    bool inited = gemu_machine_init(c->machine);
    static const GemuExecutorOps exec_ops = { .run = a26_exec_run, .elapsed = a26_exec_elapsed };
    ok = ok && inited && a26_bus_build(s) &&
         (c->exec = gemu_executor_new(c->cpu, (uint64_t)s->color_hz, &exec_ops, s)) &&
         a26_screen_build(s) && a26_sound_build(s);
    if (!ok || !inited || !gemu_machine_start(c->machine)) {
        fprintf(stderr, "atari2600: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    gemu_monitor_attach_machine(s->monitor, c->machine, "program");
    return true;
}

static void a26_core_free(Atari2600State *s) {
    if (!s->core) return;
    gemu_machine_free(s->core->machine);
    free(s->core);
    s->core = NULL;
}

/* ── Create / destroy ────────────────────────────────────────────────────── */

Atari2600State *atari2600_create(const MosConfig *cfg) {
    Atari2600State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;
    s->color = true;
    for (int i = 0; i < 4; i++) s->paddle[i] = 0.5;
    s->color_hz = cfg->is_pal || cfg->is_secam ? A2600_COLOR_HZ_PAL : A2600_COLOR_HZ_NTSC;
    s->pal = cfg->is_pal;
    s->window_start = 20;

    s->cart.cpu_hz = s->color_hz / 3.0;
    if (cfg->cart_path) {
        if (!a26_cart_load(&s->cart, cfg->cart_path)) { free(s); return NULL; }
        snprintf(s->cart_path, sizeof(s->cart_path), "%s", cfg->cart_path);
    }

    tia_init(&s->tia);
    if (cfg->sound != MOS_SOUND_NONE &&
        !tia_sound_init(&s->tia.sound, s->color_hz / 114.0, true))
        fprintf(stderr, "atari2600: no audio (continuing silently)\n");
    if (!s->tia.sound.tia_rate) s->tia.sound.tia_rate = s->color_hz / 114.0;

    m6532_init(&s->riot);
    s->riot.read_pa = a26_read_pa;
    s->riot.read_pb = a26_read_pb;
    s->riot.ud = s;

    mos6502_init(&s->cpu);
    a26_bus_attach(s);
    mos_cpu_apply(&s->cpu, cfg->cpu);
    s->cpu.dummy_cycles = true;   /* hotspots see every bus cycle */
    for (int i = 0; i < 128; i++) s->lut[i] = palette_color(s, (uint8_t)(i << 1));

    s->monitor = gemu_monitor_create();
    if (!s->monitor) { tia_sound_destroy(&s->tia.sound); a26_cart_free(&s->cart); free(s); return NULL; }
    GemuMonitor *mon = s->monitor;
    gemu_monitor_set_addr_mask(mon, 0x1FFF);   /* 13 address lines: "break F000" = $1000 */
    a26_debug_init(s);
    a26_debug_bind(s);
    if (cfg->symbols_path) {
        int n = gemu_monitor_load_symbols(mon, cfg->symbols_path);
        if (n < 0) fprintf(stderr, "atari2600: cannot read symbols '%s'\n", cfg->symbols_path);
        else       printf("atari2600: %d symbols from %s\n", n, cfg->symbols_path);
    }
    gemu_monitor_set_cpu_state_cb(mon, a26_cpu_state, s);
    gemu_monitor_set_screendump_cb(mon, a26_screendump, s);
    gemu_monitor_set_memory(mon, &(GemuMonMemory){
        .read = a26_mon_read, .write = a26_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = false });
    gemu_monitor_bind(mon, "sendkey",    a26_cmd_sendkey,    s);
    gemu_monitor_bind(mon, "dipswitch",  a26_cmd_dipswitch,  s);
    gemu_monitor_bind(mon, "mapperdump", a26_cmd_mapperdump, s);
    gemu_monitor_bind(mon, "savevm",     a26_cmd_savevm,     s);
    gemu_monitor_bind(mon, "loadvm",     a26_cmd_loadvm,     s);
    gemu_monitor_bind(mon, "hexeditor",  a26_cmd_hexeditor,  s);

    GemuMediaDevice cart_dev = {
        .name = "cartridge", .kind = "cartridge", .ud = s,
        .change = a26_cart_change, .eject = a26_cart_eject, .status = a26_cart_status,
    };
    snprintf(cart_dev.file, sizeof(cart_dev.file), "%s", s->cart_path);
    gemu_monitor_register_media(mon, &cart_dev);

    make_hex_editor(s);

    if (gemu_display_wanted(cfg->display_type)) {
        /* One input page per plugged-in controller, then the console. */
        static GemuInputPage pages[3];
        int n_pages = 0;
        bool two = cfg->a2600_ports[1] != A2600_CTRL_NONE;
        pages[n_pages++] = (GemuInputPage){ ctrl_name(cfg->a2600_ports[0]), 5 };
        if (two) pages[n_pages++] = (GemuInputPage){ ctrl_name(cfg->a2600_ports[1]), 5 };
        pages[n_pages++] = (GemuInputPage){ "console", 5 };
        s->display = gemu_display_create(cfg->display_type, &(GemuDisplayConfig){
            .fb_width    = A2600_FB_W,
            .fb_height   = A2600_FB_H,
            .scale       = cfg->display_scale,
            .renderer    = cfg->display_renderer,
            .actions     = two ? a26_actions : a26_actions_1p,
            .n_actions   = two ? A26_NUM_ACTIONS : 10,
            .ini_section = "atari2600",
            .pages       = pages,
            .n_pages     = n_pages,
            .monitor     = s->monitor,
            .gtk         = &(GemuDisplayGtkExtras){
                .monitor       = s->monitor,
                .hex_toggle_cb = a26_hex_toggle,
                .hex_toggle_ud = s,
            },
        });
        if (s->display) {
            gemu_monitor_set_input_reset_cb(mon, gemu_display_reset_input_bindings_ud, s->display);
            /* View menu: the picture, or every color clock of every line. */
            gemu_display_set_heads(s->display, (GemuDisplayHead[]){
                { "TV picture", A2600_FB_W, A2600_FB_H, 4, 3 },
                { "Full frame (debug)", A26_FULL_W, A26_FULL_H, 0, 0 },
            }, 2);
            gemu_display_add_debug_item(s->display, "Reload Cartridge", a26_debug_reload, s);
            gemu_display_add_debug_item(s->display, "Debug Colors", a26_debug_toggle_colors, s);
        }
    }

    if (!a26_core_build(s)) {   /* the bus first: power-on reads the reset vector */
        atari2600_destroy(s);
        return NULL;
    }
    a26_power_on(s);
    return s;
}

void atari2600_destroy(Atari2600State *s) {
    if (!s) return;
    a26_core_free(s);
    hex_editor_destroy(s->hex_editor);
    gemu_monitor_destroy(s->monitor);
    gemu_display_destroy(s->display);
    a26_debug_destroy(s);
    tia_sound_destroy(&s->tia.sound);
    a26_cart_free(&s->cart);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

/* Keep about three frames of audio queued. */
#define A26_AUDIO_QUEUE_MAX (3u * 735u * (unsigned)sizeof(float))

static void handle_input(Atari2600State *s) {
    uint32_t h = s->held;
    if (s->inject_frames > 0) { h |= s->inject_mask; s->inject_frames--; }
    s->held = h;
    uint32_t pressed = h & ~s->prev_held;
    s->prev_held = h;
    if (pressed & GEMU_ACTION(A26_ACT_COLOR))      s->color = !s->color;
    if (pressed & GEMU_ACTION(A26_ACT_LEFT_DIFF))  s->diff_a[0] = !s->diff_a[0];
    if (pressed & GEMU_ACTION(A26_ACT_RIGHT_DIFF)) s->diff_a[1] = !s->diff_a[1];

    /* Paddles: the mouse turns player 1's; the left/right keys turn either. */
    for (int p = 0; p < 2; p++) {
        int i = player_paddle(s, p);
        if (i < 0) continue;
        int left = p ? A26_ACT_P2_LEFT : A26_ACT_P1_LEFT, right = left + 1;
        if (h & GEMU_ACTION(left))  s->paddle[i] += 0.02;
        if (h & GEMU_ACTION(right)) s->paddle[i] -= 0.02;
        if (p == 0 && s->display) {
            GemuPointerState ptr = gemu_display_get_pointer(s->display);
            if (ptr.x >= 0) s->paddle[i] = 1.0 - (double)ptr.x / (A2600_FB_W - 1);
            if (ptr.button) s->held |= GEMU_ACTION(A26_ACT_P1_FIRE);
        }
        if (s->paddle[i] < 0) s->paddle[i] = 0;
        if (s->paddle[i] > 1) s->paddle[i] = 1;
    }
    update_tia_inputs(s);
}

/* The TIA finished a frame: hand it to the debugger's statistics. */
static void a26_end_frame(Atari2600State *s) {
    s->tia.frame_ready = false;
    a26_screen_frame(s);
    a26_debug_frame_done(s, s->cpu.cycle_count - s->frame_start_cycle);
    s->frame_start_cycle = s->cpu.cycle_count;
}

/* One frame, as the run loop runs it: to the TIA's frame (or a stop), the
 * TIA caught up, the frame handed on, the frame's sound out through the
 * mixer, the picture built. */
static void a26_frame(Atari2600State *s) {
    a26_run_frame(s);
    a26_bus_sync(s);   /* the picture and debugger see where the beam is */
    if (s->tia.frame_ready) a26_end_frame(s);
    gemu_mixer_run(s->core->machine);
    update_window(s);
    build_frame(s);
}

void atari2600_run(Atari2600State *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);
    bool quit = false;
    while (!quit) {
        s->held = 0;
        if (s->display) {
            s->held = gemu_display_poll(s->display);
            if (gemu_display_should_quit(s->display)) break;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                a26_power_on(s);
            }
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { quit = true; break; }
            } else if (cmd == GEMU_MON_RESET) {
                a26_power_on(s);
            } else if (cmd == GEMU_MON_STEP) {
                for (uint32_t n = gemu_monitor_step_count(s->monitor); n; n--) {
                    a26_bus_step(s);
                    if (s->tia.frame_ready) a26_end_frame(s);
                }
                a26_bus_sync(s);
                if (s->tia.frame_ready) a26_end_frame(s);
                /* Show the last whole frame, as a running machine would;
                 * the loop below presents fb even while paused. */
                update_window(s);
                build_frame(s);
            }
        }
        if (quit) break;
        bool paused = gemu_monitor_is_paused(s->monitor);
        if (s->display) gemu_display_set_paused(s->display, paused);

        handle_input(s);

        a26_debug_poll(s);

        if (!paused) a26_frame(s);

        if (s->display) {
            if (gemu_display_head_shown(s->display, 0)) {
                const GemuBitmap *b = gemu_screen_render(a26_screen(s));
                gemu_display_render_head(s->display, 0, b->pix, b->width, b->height);
            }
            a26_debug_present(s);
        }

        /* Pace to the audio clock, or to the machine's time (the CPU's
         * cycles at the colour clock); after a pause, start over. */
        if (s->tia.sound.dev && !paused) {
            while (tia_sound_queued(&s->tia.sound) > A26_AUDIO_QUEUE_MAX) SDL_Delay(1);
        } else if (paused) {
            gemu_machine_pace_reset(s->core->machine);
            SDL_Delay(16);
        } else {
            gemu_machine_pace(s->core->machine, 0);
        }
        hex_editor_refresh(s->hex_editor);
    }
    gemu_monitor_stop(s->monitor);
    printf("atari2600: %" PRIu64 " frames, %" PRIu64 " cpu cycles\n",
           s->tia.frame_count, s->cpu.cycle_count);
}
