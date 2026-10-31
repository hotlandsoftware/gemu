/*
 * Atari 2600 machine - see hardware/mos/atari2600.h for the memory map.
 */
#include "atari2600.h"
#include "gemu/screendump.h"
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

/* ── Memory map ──────────────────────────────────────────────────────────── */

static inline void bus_access(Atari2600State *s, uint16_t addr) {
    if (addr != s->last_addr) { s->cart.distinct++; s->last_addr = addr; }
    tia_run(&s->tia, s->cpu.cycle_count * 3u);
}

static uint8_t a26_read(uint16_t addr, void *ud) {
    Atari2600State *s = ud;
    bus_access(s, addr);
    uint8_t v;
    if (addr & 0x1000u)        v = a26_cart_read(&s->cart, addr, s->bus, s->cpu.cycle_count);
    else if (!(addr & 0x80u))  v = tia_read(&s->tia, addr, s->bus);
    else if (!(addr & 0x200u)) v = s->riot.ram[addr & 0x7Fu];
    else                       v = m6532_read(&s->riot, addr, s->cpu.cycle_count);
    a26_cart_snoop(&s->cart, addr, v, false);
    s->bus = v;
    gemu_monitor_check_read(s->monitor, addr);
    return v;
}

static void a26_write(uint16_t addr, uint8_t val, void *ud) {
    Atari2600State *s = ud;
    bus_access(s, addr);
    if (addr & 0x1000u)        a26_cart_write(&s->cart, addr, val, s->cpu.cycle_count);
    else if (!(addr & 0x80u))  s->wsync |= tia_write(&s->tia, addr, val);
    else if (!(addr & 0x200u)) s->riot.ram[addr & 0x7Fu] = val;
    else                       m6532_write(&s->riot, addr, val, s->cpu.cycle_count);
    a26_cart_snoop(&s->cart, addr, val, true);
    s->bus = val;
    gemu_monitor_check_write(s->monitor, addr);
}

/* Supercharger BIOS hotspot access to the console's RAM. */
static uint8_t ram_peek(void *ud, uint8_t addr) {
    return ((Atari2600State *)ud)->riot.ram[addr & 0x7Fu];
}
static void ram_poke(void *ud, uint8_t addr, uint8_t val) {
    ((Atari2600State *)ud)->riot.ram[addr & 0x7Fu] = val;
}

/* One instruction, then the WSYNC hold: RDY stays low until the TIA
 * reaches the start of the next scanline. */
static void a26_step(Atari2600State *s) {
    mos6502_step(&s->cpu);
    if (s->wsync) {
        s->wsync = false;
        int left = (TIA_LINE_CLOCKS - s->tia.hctr) % TIA_LINE_CLOCKS;
        s->cpu.cycle_count += (uint64_t)(left + 2) / 3u;
    }
}

/* ── Reset / frame output ────────────────────────────────────────────────── */

/* Power cycle (the console's own Reset is a switch the program reads). */
static void a26_power_on(Atari2600State *s) {
    tia_reset(&s->tia);
    m6532_reset(&s->riot, s->cpu.cycle_count);
    a26_cart_reset(&s->cart);
    s->wsync = false;
    mos6502_reset(&s->cpu);
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

/* ── Snapshots ───────────────────────────────────────────────────────────── *
 *
 * The state struct is written raw with host pointers cleared, then spliced
 * back around the live pointers on load (as the NES does). */

#define A26_SNAPSHOT_MAGIC   0x3632494154414732ULL   /* "2GATAI26" */
#define A26_SNAPSHOT_VERSION 1u

static bool a26_save_snapshot(Atari2600State *s, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    Atari2600State *snap = malloc(sizeof(*snap));
    if (!snap) { fclose(f); return false; }
    *snap = *s;
    snap->cfg = NULL; snap->display = NULL; snap->monitor = NULL; snap->hex_editor = NULL;
    snap->cpu.mem_read = NULL; snap->cpu.mem_write = NULL; snap->cpu.mem_ud = NULL;
    snap->riot.read_pa = NULL; snap->riot.read_pb = NULL; snap->riot.ud = NULL;
    snap->cart.rom = NULL;
    snap->tia.sound.dev = 0;
    snap->dbg.trace = NULL;
    snap->dbg.full_fb = NULL;
    uint64_t magic = A26_SNAPSHOT_MAGIC;
    uint32_t version = A26_SNAPSHOT_VERSION, rom_size = s->cart.rom_size;
    bool ok = fwrite(&magic, sizeof(magic), 1, f) == 1 &&
              fwrite(&version, sizeof(version), 1, f) == 1 &&
              fwrite(&rom_size, sizeof(rom_size), 1, f) == 1 &&
              fwrite(snap, sizeof(*snap), 1, f) == 1;
    free(snap);
    fclose(f);
    return ok;
}

static bool a26_load_snapshot(Atari2600State *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint64_t magic = 0;
    uint32_t version = 0, rom_size = 0;
    Atari2600State *snap = malloc(sizeof(*snap));
    bool ok = snap &&
              fread(&magic, sizeof(magic), 1, f) == 1 &&
              fread(&version, sizeof(version), 1, f) == 1 &&
              fread(&rom_size, sizeof(rom_size), 1, f) == 1 &&
              magic == A26_SNAPSHOT_MAGIC && version == A26_SNAPSHOT_VERSION &&
              rom_size == s->cart.rom_size &&
              fread(snap, sizeof(*snap), 1, f) == 1 &&
              snap->cart.type == s->cart.type;
    fclose(f);
    if (!ok) { free(snap); return false; }

    const MosConfig *cfg = s->cfg;
    GemuDisplay *display = s->display;
    GemuMonitor *monitor = s->monitor;
    HexEditor *hex = s->hex_editor;
    uint8_t *rom = s->cart.rom;
    SDL_AudioDeviceID dev = s->tia.sound.dev;
    void *ud = s->cpu.mem_ud;
    A26Debug dbg = s->dbg;   /* the debugger's settings aren't machine state */
    *s = *snap;
    free(snap);
    s->dbg = dbg;
    s->cfg = cfg; s->display = display; s->monitor = monitor; s->hex_editor = hex;
    s->cart.rom = rom;
    s->tia.sound.dev = dev;
    s->tia.sound.buf_n = 0;
    s->cpu.mem_read = a26_read; s->cpu.mem_write = a26_write; s->cpu.mem_ud = ud;
    s->riot.read_pa = a26_read_pa; s->riot.read_pb = a26_read_pb; s->riot.ud = ud;
    return true;
}

static GemuMonCmd a26_cmd_savevm(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    char path[256];
    if (sscanf(args, "%255s", path) != 1) {
        gemu_monitor_printf(mon, "usage: savevm <file>\n");
        return GEMU_MON_NONE;
    }
    if (a26_save_snapshot(s, path))
        gemu_monitor_printf(mon, "saved snapshot to %s (frame=%" PRIu64 ")\n", path, s->tia.frame_count);
    else
        gemu_monitor_printf(mon, "failed to save snapshot to %s\n", path);
    return GEMU_MON_NONE;
}

static GemuMonCmd a26_cmd_loadvm(GemuMonitor *mon, const char *args, void *ud) {
    Atari2600State *s = ud;
    char path[256];
    if (sscanf(args, "%255s", path) != 1) {
        gemu_monitor_printf(mon, "usage: loadvm <file>\n");
        return GEMU_MON_NONE;
    }
    if (a26_load_snapshot(s, path))
        gemu_monitor_printf(mon, "loaded snapshot from %s (frame=%" PRIu64 ")\n", path, s->tia.frame_count);
    else
        gemu_monitor_printf(mon, "failed to load snapshot from %s (wrong file or cartridge?)\n", path);
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
    a26_cart_set_ram_access(ram_peek, ram_poke, s);

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
    s->cpu.mem_read  = a26_read;
    s->cpu.mem_write = a26_write;
    s->cpu.mem_ud    = s;
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

    a26_power_on(s);
    return s;
}

void atari2600_destroy(Atari2600State *s) {
    if (!s) return;
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

void atari2600_run(Atari2600State *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);
    double carry_ms = 0;
    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

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
                for (uint32_t n = gemu_monitor_step_count(s->monitor); n; n--) a26_step(s);
            }
        }
        if (quit) break;
        bool paused = gemu_monitor_is_paused(s->monitor);
        if (s->display) gemu_display_set_paused(s->display, paused);

        handle_input(s);

        a26_debug_poll(s);

        uint64_t frame_cycles = 0;
        if (!paused) {
            uint64_t start = s->cpu.cycle_count;
            while (!s->tia.frame_ready) {
                if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) break;
                if (a26_debug_before_step(s)) break;
                a26_step(s);
                if (a26_debug_after_step(s)) break;
                if (gemu_monitor_is_paused(s->monitor)) break;
            }
            frame_cycles = s->cpu.cycle_count - start;
            if (s->tia.frame_ready) {
                s->tia.frame_ready = false;
                a26_debug_frame_done(s, s->cpu.cycle_count - s->frame_start_cycle);
                s->frame_start_cycle = s->cpu.cycle_count;
            }
            tia_sound_flush(&s->tia.sound);
            update_window(s);
            build_frame(s);
        }

        if (s->display) {
            if (gemu_display_head_shown(s->display, 0))
                gemu_display_render_head(s->display, 0, s->fb, A2600_FB_W, A2600_FB_H);
            a26_debug_present(s);
        }

        /* Pace to the audio clock, or to the frame's real duration. */
        if (s->tia.sound.dev && !paused) {
            while (tia_sound_queued(&s->tia.sound) > A26_AUDIO_QUEUE_MAX) SDL_Delay(1);
        } else {
            double ms = frame_cycles ? frame_cycles * 3000.0 / s->color_hz : 16.7;
            carry_ms += ms - (SDL_GetTicks() - t0);
            if (carry_ms > 0) {
                Uint32 d = (Uint32)carry_ms;
                SDL_Delay(d);
                carry_ms -= d;
            } else if (carry_ms < -100) {
                carry_ms = 0;
            }
        }
        hex_editor_refresh(s->hex_editor);
    }
    gemu_monitor_stop(s->monitor);
    printf("atari2600: %" PRIu64 " frames, %" PRIu64 " cpu cycles\n",
           s->tia.frame_count, s->cpu.cycle_count);
}
