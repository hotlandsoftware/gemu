/*
 * Atari 400 machine - see hardware/mos/atari400.h for the memory map and the
 * modelling scope of each chip.
 */
#include "atari400.h"
#include "gemu/screendump.h"
#include "antic_state.h"
#include "pokey_state.h"
#include "controller/pia6821_state.h"
#include "mos6502_state.h"
#include "gemu/dev/device.h"
#include "gemu/dev/option.h"
#include "gemu/dev/state.h"
#include "gemu/dev/line.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/screen.h"
#include "gemu/dev/space.h"
#include <inttypes.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Input action table ─────────────────────────────────────────────────── */

static const GemuActionDef a400_actions[A400_NUM_ACTIONS] = {
    { "p1_up",    GEMU_ACTION(A400_ACT_UP),     "Up" },
    { "p1_down",  GEMU_ACTION(A400_ACT_DOWN),   "Down" },
    { "p1_left",  GEMU_ACTION(A400_ACT_LEFT),   "Left" },
    { "p1_right", GEMU_ACTION(A400_ACT_RIGHT),  "Right" },
    { "p1_fire",  GEMU_ACTION(A400_ACT_FIRE),   "Left Alt" },
    { "start",    GEMU_ACTION(A400_ACT_START),  "F2" },
    { "select",   GEMU_ACTION(A400_ACT_SELECT), "F3" },
    { "option",   GEMU_ACTION(A400_ACT_OPTION), "F4" },
    { "break",    GEMU_ACTION(A400_ACT_BREAK),  "F7" },
};

/* ── Keyboard: host character → POKEY scan code ─────────────────────────── */

/* 6-bit scan code | 0x40 (shift) | 0x80 (ctrl); 0xFF = no mapping.
 * Layout per the Atari 400/800 keyboard matrix (OS listings, appendix). */
static uint8_t a400_char_to_code(uint32_t cp) {
    static const struct { uint8_t ch; uint8_t code; } tab[] = {
        {'l',0x00},{'j',0x01},{';',0x02},{'k',0x05},{'+',0x06},{'*',0x07},
        {'o',0x08},{'p',0x0A},{'u',0x0B},{'\r',0x0C},{'i',0x0D},{'-',0x0E},
        {'=',0x0F},{'v',0x10},{'c',0x12},{'b',0x15},{'x',0x16},{'z',0x17},
        {'4',0x18},{'3',0x1A},{'6',0x1B},{0x1B,0x1C},{'5',0x1D},{'2',0x1E},
        {'1',0x1F},{',',0x20},{' ',0x21},{'.',0x22},{'n',0x23},{'m',0x25},
        {'/',0x26},{'r',0x28},{'e',0x2A},{'y',0x2B},{'\t',0x2C},{'t',0x2D},
        {'w',0x2E},{'q',0x2F},{'9',0x30},{'0',0x32},{'7',0x33},{'\b',0x34},
        {'8',0x35},{'<',0x36},{'>',0x37},{'f',0x38},{'h',0x39},{'d',0x3A},
        {'g',0x3D},{'s',0x3E},{'a',0x3F},
        /* shifted symbols, Atari layout */
        {'!',0x5F},{'"',0x5E},{'#',0x5A},{'$',0x58},{'%',0x5D},{'&',0x5B},
        {'\'',0x73},{'@',0x75},{'(',0x70},{')',0x72},{':',0x42},{'\\',0x46},
        {'^',0x47},{'_',0x4E},{'|',0x4F},{'?',0x66},{'[',0x60},{']',0x62},
    };
    if (cp == 0x7F) cp = '\b';
    if (cp >= 'A' && cp <= 'Z')          /* uppercase = shift + letter */
        return (uint8_t)(a400_char_to_code(cp - 'A' + 'a') | 0x40u);
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (tab[i].ch == cp) return tab[i].code;
    return 0xFF;
}

static void a400_queue_key(Atari400State *s, uint8_t code) {
    int next = (s->keyq_tail + 1) % A400_KEYQ_LEN;
    if (next == s->keyq_head) return;    /* queue full - drop */
    s->keyq[s->keyq_tail] = code;
    s->keyq_tail = next;
}

static void a400_queue_char(Atari400State *s, uint32_t cp) {
    uint8_t code = a400_char_to_code(cp);
    if (code != 0xFF) a400_queue_key(s, code);
}

/* Pace queued keys: press for 3 frames, release, 2 frames gap.  The OS
 * keyboard IRQ handler has its own debounce (KEYDEL) that eats keys
 * arriving faster than this. */
static void a400_key_pump(Atari400State *s) {
    if (s->key_hold_frames > 0) {
        if (--s->key_hold_frames == 0) {
            pokey_key_up(&s->pokey);
            s->key_gap_frames = 2;
        }
        return;
    }
    if (s->key_gap_frames > 0) { s->key_gap_frames--; return; }
    if (s->keyq_head == s->keyq_tail) return;
    pokey_key_down(&s->pokey, s->keyq[s->keyq_head]);
    s->keyq_head = (s->keyq_head + 1) % A400_KEYQ_LEN;
    s->key_hold_frames = 3;
}

/* Ctrl+arrow = Atari cursor movement (ctrl + -, =, +, *). */
static void a400_poll_cursor_keys(Atari400State *s) {
    static const char *names[4] = { "Up", "Down", "Left", "Right" };
    static const uint8_t codes[4] = { 0x8E, 0x8F, 0x86, 0x87 };
    if (!s->display) return;
    bool ctrl = gemu_display_is_key_held(s->display, "Left Ctrl") ||
                gemu_display_is_key_held(s->display, "Right Ctrl");
    for (int i = 0; i < 4; i++) {
        bool held = gemu_display_is_key_held(s->display, names[i]);
        if (held && !s->prev_arrow[i] && ctrl)
            a400_queue_key(s, codes[i]);
        s->prev_arrow[i] = held;
    }
}

/* ── PIA / GTIA input callbacks ─────────────────────────────────────────── */

static uint8_t a400_pia_read_pa(void *ud) {
    Atari400State *s = ud;
    uint8_t v = 0xFF;                     /* directions active low */
    if (s->held_actions & GEMU_ACTION(A400_ACT_UP))    v &= (uint8_t)~0x01;
    if (s->held_actions & GEMU_ACTION(A400_ACT_DOWN))  v &= (uint8_t)~0x02;
    if (s->held_actions & GEMU_ACTION(A400_ACT_LEFT))  v &= (uint8_t)~0x04;
    if (s->held_actions & GEMU_ACTION(A400_ACT_RIGHT)) v &= (uint8_t)~0x08;
    return v;
}

static uint8_t a400_pia_read_pb(void *ud) {
    (void)ud;
    return 0xFF;                          /* sticks 3/4 absent */
}

static uint8_t a400_consol(const Atari400State *s) {
    uint8_t v = 0x07;                     /* active low, none pressed */
    if (s->held_actions & GEMU_ACTION(A400_ACT_START))  v &= (uint8_t)~0x01;
    if (s->held_actions & GEMU_ACTION(A400_ACT_SELECT)) v &= (uint8_t)~0x02;
    if (s->held_actions & GEMU_ACTION(A400_ACT_OPTION)) v &= (uint8_t)~0x04;
    return v;
}

/* ── Memory map ─────────────────────────────────────────────────────────── */

static bool a400_maxflash_control(Atari400State *s, uint16_t addr) {
    if (addr < 0xD500u || addr > 0xD5FFu) return false;
    if (s->cart_type == 41u) {
        /* The 1Mbit cartridge decodes only D500-D51F.  The rest of CCTL
         * must not alias it: SDX probes other cartridge implementations. */
        if (addr > 0xD51Fu) return false;
        s->cart_bank = (uint8_t)(addr & 0x0Fu);
        s->cart_enabled = (addr & 0x10u) == 0;
        return true;
    }
    if (s->cart_type == 42u) {
        s->cart_bank = (uint8_t)(addr & 0x7Fu);
        s->cart_enabled = (addr & 0x80u) == 0;
        return true;
    }
    return false;
}

/* ── The bus as a core space (stage T2) ──────────────────────────────────────
 * "program", 16 bits:
 *   RAM from $0000 (ram_size); the OS ROM at $D800 (read-only);
 *   GTIA $D000, POKEY $D200, PIA $D300, ANTIC $D400 (WSYNC), CCTL $D500
 *   (the Maxflash banking), one page each; the rest of $D000-$D7FF is
 *   unmapped and reads $FF;
 *   at priority 1, Axlon's window on $4000-$7FFF (bank 1-63) and its bank
 *   selects at $0FC0-$0FFF (a write also lands in RAM: the handler has no
 *   read and passes it on) and $CFC0-$CFFF;
 *   at priority 2, the cartridge: its ROM read-only, and a handler that
 *   drops writes, so RAM under it isn't written.
 * a400_maps_sync() points the cartridge and Axlon windows after anything
 * that moves them.  The NMI (ANTIC's VBI) and /IRQ (POKEY) are lines. */
static void a400_maps_sync(Atari400State *s);

static uint8_t bus_gtia_r(void *ud, uint64_t off) {
    Atari400State *s = ud;
    uint8_t trig[4] = { 1, 1, 1, 1 };
    if (s->held_actions & GEMU_ACTION(A400_ACT_FIRE)) trig[0] = 0;
    return gtia_reg_read(&s->antic, (uint8_t)off, a400_consol(s), trig);
}
static void bus_gtia_w(void *ud, uint64_t off, uint8_t v) {
    gtia_reg_write(&((Atari400State *)ud)->antic, (uint8_t)off, v);
}
static uint8_t bus_pokey_r(void *ud, uint64_t off) {
    return pokey_read(&((Atari400State *)ud)->pokey, (uint8_t)off);
}
static void bus_pokey_w(void *ud, uint64_t off, uint8_t v) {
    pokey_write(&((Atari400State *)ud)->pokey, (uint8_t)off, v);
}
static uint8_t bus_pia_r(void *ud, uint64_t off) {
    return pia6821_read(&((Atari400State *)ud)->pia, (unsigned)off & 3u);
}
static void bus_pia_w(void *ud, uint64_t off, uint8_t v) {
    pia6821_write(&((Atari400State *)ud)->pia, (unsigned)off & 3u, v);
}
static uint8_t bus_antic_r(void *ud, uint64_t off) {
    return antic_reg_read(&((Atari400State *)ud)->antic, (uint8_t)off);
}
static void bus_antic_w(void *ud, uint64_t off, uint8_t v) {
    Atari400State *s = ud;
    if ((off & 0x0Fu) == 0x0A) s->wsync = true;      /* WSYNC */
    else antic_reg_write(&s->antic, (uint8_t)off, v);
}
static uint8_t bus_cctl_r(void *ud, uint64_t off) {
    Atari400State *s = ud;
    if (a400_maxflash_control(s, (uint16_t)(0xD500u + off))) a400_maps_sync(s);
    return 0xFF;
}
static void bus_cctl_w(void *ud, uint64_t off, uint8_t v) {
    (void)v;
    bus_cctl_r(ud, off);
}
static void bus_axlon_lo_w(void *ud, uint64_t off, uint8_t v) {
    Atari400State *s = ud;
    s->ram[0x0FC0u + off] = v;           /* the low selectors are ordinary RAM too */
    s->axlon_bank = v & 0x3Fu;
    a400_maps_sync(s);
}
static void bus_axlon_hi_w(void *ud, uint64_t off, uint8_t v) {
    (void)off;
    Atari400State *s = ud;
    s->axlon_bank = v & 0x3Fu;
    a400_maps_sync(s);
}
static void bus_drop_w(void *ud, uint64_t off, uint8_t v) { (void)ud; (void)off; (void)v; }

static const GemuHandlers bus_gtia   = { .r8 = bus_gtia_r,  .w8 = bus_gtia_w };
static const GemuHandlers bus_pokey  = { .r8 = bus_pokey_r, .w8 = bus_pokey_w };
static const GemuHandlers bus_pia    = { .r8 = bus_pia_r,   .w8 = bus_pia_w };
static const GemuHandlers bus_antic  = { .r8 = bus_antic_r, .w8 = bus_antic_w };
static const GemuHandlers bus_cctl   = { .r8 = bus_cctl_r,  .w8 = bus_cctl_w };
static const GemuHandlers bus_axlon_lo = { .w8 = bus_axlon_lo_w };
static const GemuHandlers bus_axlon_hi = { .w8 = bus_axlon_hi_w };
static const GemuHandlers bus_drop   = { .w8 = bus_drop_w };

typedef struct A400Slot {
    uint8_t     *ram;
    GemuMapping *map;
    uint32_t     off;
    uint8_t      kind;     /* 0 = not looked up, 1 = ram, 2 = map, 3 = the space */
} A400Slot;

/* A400Core: see "The core machine" below. */
static inline A400Slot *a400_slot(Atari400State *s, uint16_t addr, bool write);
static GemuSpace *a400_space(const Atari400State *s);

static uint8_t a400_read(uint16_t addr, void *ud) {
    Atari400State *s = ud;
    A400Slot *sl = a400_slot(s, addr, false);
    if (sl->kind == 1) return sl->ram[addr & 0xFFu];
    if (sl->kind == 2) return sl->map->h.r8(sl->map->ud, sl->off + (addr & 0xFFu));
    return gemu_space_read8(a400_space(s), addr);
}

static void a400_write(uint16_t addr, uint8_t val, void *ud) {
    Atari400State *s = ud;
    A400Slot *sl = a400_slot(s, addr, true);
    if (sl->kind == 1)      sl->ram[addr & 0xFFu] = val;
    else if (sl->kind == 2) sl->map->h.w8(sl->map->ud, sl->off + (addr & 0xFFu), val);
    else                    gemu_space_write8(a400_space(s), addr, val);
}

/* ── Reset / screendump / monitor commands ──────────────────────────────── */

static void a400_reset(Atari400State *s) {
    antic_reset(&s->antic);
    pokey_init(&s->pokey);
    pia6821_init(&s->pia);
    s->pia.read_pa = a400_pia_read_pa;
    s->pia.read_pb = a400_pia_read_pb;
    s->pia.ud      = s;
    s->wsync = false;
    s->axlon_bank = 0;
    s->cart_bank = 0;
    s->cart_enabled = s->cart != NULL;
    a400_maps_sync(s);
    s->keyq_head = s->keyq_tail = 0;
    s->key_hold_frames = s->key_gap_frames = 0;
    mos6502_reset(&s->cpu);
    if (s->cfg->has_start_addr) s->cpu.PC = s->cfg->start_addr;
}

static bool a400_screendump(void *ud, const char *path) {
    Atari400State *s = ud;
    return gemu_screendump_argb(path, s->antic.pixels_argb,
                                ANTIC_FB_W, ANTIC_FB_H);
}

static char a400_screen_to_ascii(uint8_t ch) {
    ch &= 0x7Fu;
    if (ch < 0x40) {
        static const char *tab =
            " !\"#$%&'()*+,-./0123456789:;<=>?"
            "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_";
        return tab[ch];
    }
    return ' ';
}

static void a400_render_curses_text(Atari400State *s) {
    char out[24 * 40];
    memset(out, ' ', sizeof(out));

    uint16_t dl = s->antic.dlist;
    uint16_t msc = 0;
    int row = 0;
    for (int safety = 0; safety < 1024 && row < 24; safety++) {
        uint8_t op = a400_read(dl, s);
        dl = (uint16_t)((dl & 0xFC00u) | ((dl + 1u) & 0x03FFu));
        int mode = op & 0x0F;
        if (mode == 0) continue;
        if (mode == 1) {
            uint8_t lo = a400_read(dl, s);
            dl = (uint16_t)((dl & 0xFC00u) | ((dl + 1u) & 0x03FFu));
            uint8_t hi = a400_read(dl, s);
            if (op & 0x40) break;
            dl = (uint16_t)(lo | ((uint16_t)hi << 8));
            continue;
        }
        if (op & 0x40) {
            uint8_t lo = a400_read(dl, s);
            dl = (uint16_t)((dl & 0xFC00u) | ((dl + 1u) & 0x03FFu));
            uint8_t hi = a400_read(dl, s);
            dl = (uint16_t)((dl & 0xFC00u) | ((dl + 1u) & 0x03FFu));
            msc = (uint16_t)(lo | ((uint16_t)hi << 8));
        }

        int cols = (mode == 6 || mode == 7) ? 20 : 40;
        if ((mode >= 2 && mode <= 7) && row < 24) {
            for (int col = 0; col < cols; col++) {
                char c = a400_screen_to_ascii(a400_read(msc, s));
                msc = (uint16_t)((msc & 0xF000u) | ((msc + 1u) & 0x0FFFu));
                if (cols == 20) {
                    out[row * 40 + col * 2] = c;
                    out[row * 40 + col * 2 + 1] = ' ';
                } else {
                    out[row * 40 + col] = c;
                }
            }
            row++;
        }
    }
    gemu_display_render_text(s->display, out, 24, 40);
}

/* monitor: sendkey <text>  - queue each character (\n = Return) */
static GemuMonCmd a400_cmd_sendkey(GemuMonitor *mon, const char *args, void *ud) {
    Atari400State *s = ud;
    if (!s->cfg->generic_keyboard) {
        gemu_monitor_printf(mon, "sendkey: keyboard device is not attached\n");
        return GEMU_MON_NONE;
    }
    if (!*args) {
        gemu_monitor_printf(mon, "usage: sendkey <text>\n");
        return GEMU_MON_NONE;
    }
    for (const char *p = args; *p; p++)
        a400_queue_char(s, *p == '\n' ? (uint32_t)'\r' : (uint32_t)*p);
    return GEMU_MON_NONE;
}

static void a400_cpu_state(void *ud, char *buf, size_t buf_len) {
    const Atari400State *s = ud;
    snprintf(buf, buf_len,
             "atari400: pc=%04X a=%02X x=%02X y=%02X p=%02X sp=%02X "
             "cycles=%llu insns=%llu frame=%llu line=%d "
             "dmactl=%02X dlist=%04X nmien=%02X nmist=%02X "
             "colpf=%02X,%02X,%02X,%02X colbk=%02X "
             "irqen=%02X irqst=%02X skstat=%02X audf=%02X,%02X,%02X,%02X",
             s->cpu.PC, s->cpu.A, s->cpu.X, s->cpu.Y, s->cpu.P, s->cpu.SP,
             (unsigned long long)s->cpu.cycle_count,
             (unsigned long long)s->cpu.insn_count,
             (unsigned long long)s->frame, s->antic.scanline,
             s->antic.dmactl, s->antic.dlist, s->antic.nmien, s->antic.nmist,
             s->antic.colpf[0], s->antic.colpf[1], s->antic.colpf[2],
             s->antic.colpf[3], s->antic.colbk, s->pokey.irqen,
             (uint8_t)~s->pokey.irq_pending, s->pokey.skstat,
             s->pokey.audf[0], s->pokey.audf[1], s->pokey.audf[2],
             s->pokey.audf[3]);
}

/* Monitor memory view: RAM (including the selected Axlon bank), the active
 * cartridge bank and OS ROM, decoded like a400_read() but without touching
 * the hardware.  $D000-$D7FF (GTIA, POKEY, PIA, ANTIC and the CCTL page,
 * whose accesses switch MaxFlash banks) and open bus return false. */
static bool a400_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    const Atari400State *s = ud;
    if (addr > 0xFFFF) return false;
    if (addr >= 0xD000u && addr < 0xD800u) return false;
    if (s->cart_enabled && s->cart_base && addr >= s->cart_base &&
        addr < (s->cart_type ? 0xC000u : (uint32_t)s->cart_base + s->cart_size)) {
        size_t off;
        if (s->cart_type == 41u || s->cart_type == 42u)
            off = (size_t)s->cart_bank * 0x2000u + (addr - 0xA000u);
        else if (addr < (uint32_t)s->cart_base + s->cart_size)
            off = addr - s->cart_base;
        else
            off = SIZE_MAX;
        if (off != SIZE_MAX) {
            if (!s->cart || off >= s->cart_size) return false;
            *val = s->cart[off];
            return true;
        }
    }
    if (s->axlon_ram && s->axlon_bank && addr >= 0x4000u && addr < 0x8000u) {
        *val = s->axlon_ram[(size_t)(s->axlon_bank - 1u) * A400_AXLON_BANK_SIZE +
                            (addr - 0x4000u)];
        return true;
    }
    if (addr < s->ram_size) { *val = s->ram[addr]; return true; }
    if (addr >= 0xD800u) { *val = s->os_rom[addr - 0xD800u]; return true; }
    return false;
}

/* Writes go to RAM only; cartridge-overlaid, ROM and I/O addresses return
 * false.  Axlon bank-select side effects are not triggered. */
static bool a400_mon_write(void *ud, uint64_t addr, uint8_t val) {
    Atari400State *s = ud;
    if (addr > 0xFFFF) return false;
    if (s->cart_enabled && s->cart_base && addr >= s->cart_base &&
        addr < (s->cart_type ? 0xC000u : (uint32_t)s->cart_base + s->cart_size))
        return false;
    if (s->axlon_ram && s->axlon_bank && addr >= 0x4000u && addr < 0x8000u) {
        s->axlon_ram[(size_t)(s->axlon_bank - 1u) * A400_AXLON_BANK_SIZE +
                     (addr - 0x4000u)] = val;
        return true;
    }
    if (addr < s->ram_size) { s->ram[addr] = val; return true; }
    return false;
}

/* ── ROM / cartridge loading ────────────────────────────────────────────── */

static bool a400_load_file_at(Atari400State *s, const char *path, uint32_t addr) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "atari400: cannot open '%s'\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    bool ok = false;
    if (addr >= 0xD800u && addr + sz <= 0x10000u) {
        ok = fread(s->os_rom + (addr - 0xD800u), 1, (size_t)sz, f) == (size_t)sz;
    } else if (addr + sz <= ATARI400_RAM_MAX) {
        ok = fread(s->ram + addr, 1, (size_t)sz, f) == (size_t)sz;
    } else {
        fprintf(stderr, "atari400: '%s' does not fit at 0x%04X\n", path, addr);
    }
    fclose(f);
    if (!ok) fprintf(stderr, "atari400: failed to load '%s'\n", path);
    return ok;
}

static uint32_t a400_installed_ram_size(const Atari400State *s) {
    if (s->cfg->axlon1056) return ATARI400_RAM_MAX;
    uint32_t size = s->cfg->mem_size ? s->cfg->mem_size : 0x4000u;
    return size > ATARI400_RAM_MAX ? ATARI400_RAM_MAX : size;
}

static bool a400_load_cart(Atari400State *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "atari400: cannot open cartridge '%s'\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint8_t header[16];
    uint32_t type = 0;
    long payload_size = sz;
    if (sz >= 16 && fread(header, 1, sizeof(header), f) == sizeof(header) &&
        memcmp(header, "CART", 4) == 0) {
        type = ((uint32_t)header[4] << 24) | ((uint32_t)header[5] << 16) |
               ((uint32_t)header[6] << 8) | header[7];
        payload_size -= 16;
    } else {
        rewind(f);
    }
    bool raw = type == 0 &&
               (payload_size == 0x1000 || payload_size == 0x2000 ||
                payload_size == 0x4000);
    bool maxflash = (type == 41u && payload_size == 0x20000) ||
                    (type == 42u && payload_size == 0x100000);
    if (!raw && !maxflash) {
        fprintf(stderr, "atari400: unsupported cartridge format/type %u "
                        "(%ld-byte payload; supported: raw 4/8/16K, "
                        "CAR type 41/42)\n", type, payload_size);
        fclose(f);
        return false;
    }
    uint8_t *image = malloc((size_t)payload_size);
    if (!image || fread(image, 1, (size_t)payload_size, f) !=
                  (size_t)payload_size) {
        fprintf(stderr, "atari400: read error on '%s'\n", path);
        free(image);
        fclose(f);
        return false;
    }
    fclose(f);
    free(s->cart);
    s->cart = image;
    s->cart_size = (uint32_t)payload_size;
    s->cart_type = type;
    s->cart_bank = 0;
    s->cart_enabled = true;
    s->cart_base = maxflash ? 0xA000u :
                   (uint16_t)(0xC000u - payload_size);
    /* Cartridge overlays RAM; disabling a bank-switched cart reveals it. */
    s->ram_size = a400_installed_ram_size(s);
    printf("atari400: cartridge '%s' (%ldK%s at $%04X)\n", path,
           payload_size / 1024, maxflash ? " Maxflash" : "", s->cart_base);
    return true;
}

/* ATR is a sector dump with a 16-byte header.  In enhanced-density images
 * sectors 1-3 remain 128 bytes; later sectors use the size in the header. */
static bool a400_load_atr(Atari400State *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "atari400: cannot open floppy '%s'\n", path);
        return false;
    }
    uint8_t h[16];
    if (fread(h, 1, sizeof(h), f) != sizeof(h) || h[0] != 0x96 || h[1] != 0x02) {
        fprintf(stderr, "atari400: '%s' is not an ATR floppy image\n", path);
        fclose(f);
        return false;
    }
    uint32_t paragraphs = (uint32_t)h[2] | ((uint32_t)h[3] << 8) |
                          ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
    size_t declared_bytes = (size_t)paragraphs * 16u;
    uint16_t sector_size = (uint16_t)(h[4] | ((uint16_t)h[5] << 8));
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long file_size = ftell(f);
    if (file_size < 16 || fseek(f, 16, SEEK_SET) != 0) { fclose(f); return false; }
    size_t bytes = (size_t)file_size - 16u;
    if ((sector_size != 128 && sector_size != 256) || bytes < 3u * 128u ||
        (sector_size == 128 && bytes % 128u != 0) ||
        (sector_size == 256 && (bytes - 3u * 128u) % 256u != 0)) {
        fprintf(stderr, "atari400: unsupported ATR geometry in '%s'\n", path);
        fclose(f);
        return false;
    }
    if (bytes != declared_bytes)
        fprintf(stderr, "atari400: warning: ATR header declares %zu bytes, "
                        "using %zu-byte file payload\n", declared_bytes, bytes);
    uint8_t *data = malloc(bytes);
    if (!data || fread(data, 1, bytes, f) != bytes) {
        fprintf(stderr, "atari400: truncated ATR image '%s'\n", path);
        free(data);
        fclose(f);
        return false;
    }
    fclose(f);
    free(s->disk_data);
    s->disk_data = data;
    s->disk_size = bytes;
    s->disk_sector_size = sector_size;
    s->disk_sectors = sector_size == 128 ? (uint32_t)(bytes / 128u) :
                      3u + (uint32_t)((bytes - 3u * 128u) / sector_size);
    printf("atari400: floppy '%s' (%u sectors, %u-byte density)\n",
           path, s->disk_sectors, sector_size);
    return true;
}

/* ── Removable media slots ──────────────────────────────────────────────── */

static GemuMediaResult a400_floppy_change(void *ud, const char *arg,
                                          char *err, size_t err_len) {
    Atari400State *s = ud;
    if (!arg || !arg[0]) {
        snprintf(err, err_len, "missing floppy path");
        return GEMU_MEDIA_ERR;
    }
    if (!a400_load_atr(s, arg)) {
        snprintf(err, err_len, "failed to load '%s'", arg);
        return GEMU_MEDIA_ERR;
    }
    return GEMU_MEDIA_OK;
}

static GemuMediaResult a400_floppy_eject(void *ud, char *err, size_t err_len) {
    (void)err; (void)err_len;
    Atari400State *s = ud;
    free(s->disk_data);
    s->disk_data = NULL;
    s->disk_size = 0;
    s->disk_sector_size = 0;
    s->disk_sectors = 0;
    return GEMU_MEDIA_OK;
}

static void a400_floppy_status(void *ud, char *buf, size_t buf_len) {
    const Atari400State *s = ud;
    if (!s->disk_data)
        snprintf(buf, buf_len, "no disk");
    else
        snprintf(buf, buf_len, "%u sectors, %u bytes/sector",
                 s->disk_sectors, s->disk_sector_size);
}

static GemuMediaResult a400_cart_change(void *ud, const char *arg,
                                        char *err, size_t err_len) {
    Atari400State *s = ud;
    if (!arg || !arg[0]) {
        snprintf(err, err_len, "missing cartridge path");
        return GEMU_MEDIA_ERR;
    }
    if (!a400_load_cart(s, arg)) {
        snprintf(err, err_len, "failed to load '%s'", arg);
        return GEMU_MEDIA_ERR;
    }
    a400_maps_sync(s);
    return GEMU_MEDIA_OK_RESET;
}

static GemuMediaResult a400_cart_eject(void *ud, char *err, size_t err_len) {
    (void)err; (void)err_len;
    Atari400State *s = ud;
    free(s->cart);
    s->cart = NULL;
    s->cart_base = 0;
    s->cart_size = 0;
    s->cart_type = 0;
    s->cart_enabled = false;
    s->ram_size = a400_installed_ram_size(s);
    a400_maps_sync(s);
    return GEMU_MEDIA_OK_RESET;
}

static void a400_cart_status(void *ud, char *buf, size_t buf_len) {
    const Atari400State *s = ud;
    if (!s->cart_base)
        snprintf(buf, buf_len, "no cartridge");
    else
        snprintf(buf, buf_len, "%uK%s cartridge at $%04X%s",
                 (unsigned)(s->cart_size / 1024u),
                 s->cart_type == 41u || s->cart_type == 42u ? " Maxflash" : "",
                 s->cart_base, s->cart_enabled ? "" : " (disabled)");
}

static void a400_sio_return(Atari400State *s, uint8_t status) {
    s->ram[0x0303] = status;               /* DSTATS */
    s->cpu.Y = status;
    s->cpu.P = (uint8_t)((s->cpu.P & ~(MOS6502_P_N | MOS6502_P_Z)) |
                         (status & MOS6502_P_N) |
                         (status == 0 ? MOS6502_P_Z : 0));
    s->cpu.SP++;
    uint8_t lo = a400_read((uint16_t)(0x0100u | s->cpu.SP), s);
    s->cpu.SP++;
    uint8_t hi = a400_read((uint16_t)(0x0100u | s->cpu.SP), s);
    s->cpu.PC = (uint16_t)(((uint16_t)hi << 8) | lo);
    s->cpu.PC++;
    s->cpu.cycle_count += 200;             /* keep the trap from being free */
    s->cpu.insn_count++;
}

/* High-level emulation of drive 1 at the Atari OS SIO vector ($E459).
 * This avoids pretending that a bare Atari 400 contains the electronics of
 * an Atari 810 while retaining the normal OS/device-control-block contract. */
static bool a400_sio_trap(Atari400State *s) {
    if (!s->disk_data || s->cpu.PC != 0xE459u) return false;

    uint8_t device = s->ram[0x0300], unit = s->ram[0x0301];
    uint8_t command = s->ram[0x0302];
    uint16_t buffer = (uint16_t)(s->ram[0x0304] | ((uint16_t)s->ram[0x0305] << 8));
    uint16_t count = (uint16_t)(s->ram[0x0308] | ((uint16_t)s->ram[0x0309] << 8));
    uint16_t aux = (uint16_t)(s->ram[0x030A] | ((uint16_t)s->ram[0x030B] << 8));
    uint8_t status = 0x01;                 /* complete without error */

    if (device != 0x31 || unit != 1) {
        status = 0x8A;                     /* device timeout */
    } else if (command == 0x52) {          /* 'R' - read sector */
        size_t len = aux <= 3 ? 128u : s->disk_sector_size;
        size_t off = aux <= 3 ? (size_t)(aux - 1u) * 128u :
                     3u * 128u + (size_t)(aux - 4u) * s->disk_sector_size;
        if (aux == 0 || aux > s->disk_sectors || count < len ||
            off + len > s->disk_size || (uint32_t)buffer + len > s->ram_size) {
            status = 0x90;                 /* invalid sector / buffer */
        } else {
            for (size_t i = 0; i < len; i++)
                a400_write((uint16_t)(buffer + i), s->disk_data[off + i], s);
        }
    } else if (command == 0x53) {          /* 'S' - drive status */
        if (count < 4 || (uint32_t)buffer + 4u > s->ram_size) {
            status = 0x90;
        } else {
            a400_write(buffer + 0u, 0x10, s); /* motor/drive ready */
            a400_write(buffer + 1u, 0xFF, s);
            a400_write(buffer + 2u,
                       s->disk_sector_size == 128 ? 0xE0 : 0xA0, s);
            a400_write(buffer + 3u, 0x00, s);
        }
    } else {
        status = 0x8B;                     /* command not implemented */
    }
    a400_sio_return(s, status);
    return true;
}


/* ── The core machine (stage T) ──────────────────────────────────────────────
 * As the NES's (stage N1): the parts stay where Atari400State keeps them;
 * each device describes its part and registers its state.  board: RAM, the
 * WSYNC hold, the keyboard queue and the frame count; cpu; antic (ANTIC and
 * GTIA, modelled as one); pokey; pia; cart: the slot and its bank; axlon
 * (-device axlon1056): the banked RAM; fdd (the 810, a high-level trap on
 * the OS's SIO vector, no state of its own; the disk is read-only). */
typedef struct A400Core {
    GemuMachine *machine;
    GemuDevice  *board, *cpu, *antic, *pokey, *pia, *cart, *axlon, *fdd;
    /* T2: the bus, the windows on it, the CPU's cache of its decode */
    GemuSpace   *bus;
    GemuMapping *cart_rom, *cart_drop, *axlon_win;
    uint16_t     cart_lo;              /* the cartridge mappings' range, [lo, hi) */
    uint32_t     cart_hi;
    A400Slot     slot[2][256];         /* reads, writes: per 256-byte page */
    uint64_t     slot_gen;
    GemuLine    *nmi, *irq;            /* ANTIC's VBI, POKEY's /IRQ */
    GemuLineIn  *cpu_nmi, *cpu_irq;
    /* T3: the 6502 runs the machine (a400_exec_run()) */
    GemuExecutor *exec;
    uint64_t     run_start;            /* cpu.cycle_count when this run began */
    bool         stopped;              /* the last run ended at a breakpoint or a pause */
    bool         frame_done;           /* ... or it finished a frame */
    /* T4: ANTIC's picture */
    GemuScreen  *screen;
} A400Core;

static GemuSpace *a400_space(const Atari400State *s) { return s->core->bus; }

static __attribute__((noinline)) A400Slot *a400_slot_fill(A400Core *c, uint16_t addr, bool write) {
    uint64_t gen = gemu_space_generation(c->bus);
    if (c->slot_gen != gen) {
        memset(c->slot, 0, sizeof c->slot);
        c->slot_gen = gen;
    }
    A400Slot *sl = &c->slot[write][addr >> 8];
    if (!sl->kind) {
        GemuSpaceRun r;
        sl->kind = 3;
        if (gemu_space_run(c->bus, addr & 0xFF00u, write, &r) && r.avail >= 256) {
            if (r.ram) { sl->ram = r.ram; sl->kind = 1; }
            else       { sl->map = r.map; sl->off = (uint32_t)r.off; sl->kind = 2; }
        }
    }
    return sl;
}

/* How the page at addr is served: plain memory, one handler, or neither
 * (the space), looked up (gemu_space_run()) and kept while the space's
 * generation holds. */
static inline A400Slot *a400_slot(Atari400State *s, uint16_t addr, bool write) {
    A400Core *c = s->core;
    A400Slot *sl = &c->slot[write][addr >> 8];
    if (__builtin_expect(!sl->kind || c->slot_gen != gemu_space_generation(c->bus), 0))
        sl = a400_slot_fill(c, addr, write);
    return sl;
}

static bool a400_bus_build(Atari400State *s) {
    A400Core *c = s->core;
    if (!(c->bus = gemu_space_new(c->machine, "program", 16, 8, GEMU_LITTLE_ENDIAN)))
        return false;
    GemuMapping *ram, *os;
    bool ok = (ram = gemu_space_map_ram(c->bus, 0x0000, s->ram_size, s->ram, 0, 0)) &&
              (os = gemu_space_map_ram(c->bus, 0xD800, ATARI400_OS_SIZE, s->os_rom,
                                       GEMU_MAP_READONLY, 0)) &&
              gemu_space_map(c->bus, 0xD000, 0x100, &bus_gtia, s, 0, 0) &&
              gemu_space_map(c->bus, 0xD200, 0x100, &bus_pokey, s, 0, 0) &&
              gemu_space_map(c->bus, 0xD300, 0x100, &bus_pia, s, 0, 0) &&
              gemu_space_map(c->bus, 0xD400, 0x100, &bus_antic, s, 0, 0) &&
              gemu_space_map(c->bus, 0xD500, 0x100, &bus_cctl, s, 0, 0);
    if (ok) {
        gemu_mapping_set_name(ram, "RAM");
        gemu_mapping_set_name(os, "OS ROM");
    }
    if (ok && s->axlon_ram)
        ok = gemu_space_map(c->bus, 0x0FC0, 0x40, &bus_axlon_lo, s, 1, 0) &&
             gemu_space_map(c->bus, 0xCFC0, 0x40, &bus_axlon_hi, s, 1, 0) &&
             (c->axlon_win = gemu_space_map_ram(c->bus, 0x4000, A400_AXLON_BANK_SIZE, s->axlon_ram, 0, 1));
    if (ok && c->axlon_win)
        gemu_mapping_set_name(c->axlon_win, "Axlon bank");
    if (ok) a400_maps_sync(s);
    return ok;
}

/* The cartridge's window (where it is, which bank, whether it's switched
 * on) and Axlon's (which bank, if any): after a cartridge change, a reset,
 * CCTL, an Axlon select and a load. */
static void a400_maps_sync(Atari400State *s) {
    A400Core *c = s->core;
    if (!c || !c->bus) return;
    bool on = s->cart && s->cart_base && s->cart_enabled;
    uint16_t lo = s->cart ? s->cart_base : 0;
    uint32_t hi = !s->cart ? 0 : s->cart_type ? 0xC000u : (uint32_t)s->cart_base + s->cart_size;
    bool maxflash = s->cart_type == 41u || s->cart_type == 42u;
    uint8_t *ptr = !s->cart ? NULL : maxflash ? s->cart + (size_t)s->cart_bank * 0x2000u : s->cart;
    if (c->cart_rom && (c->cart_lo != lo || c->cart_hi != hi || !ptr)) {
        gemu_mapping_remove(c->cart_rom);
        gemu_mapping_remove(c->cart_drop);
        c->cart_rom = c->cart_drop = NULL;
    }
    if (!c->cart_rom && ptr && hi > lo) {
        c->cart_lo = lo;
        c->cart_hi = hi;
        c->cart_rom = gemu_space_map_ram(c->bus, lo, hi - lo, ptr, GEMU_MAP_READONLY, 2);
        c->cart_drop = gemu_space_map(c->bus, lo, hi - lo, &bus_drop, s, 2, 0);
        if (c->cart_rom) gemu_mapping_set_name(c->cart_rom, "cartridge");
    }
    if (c->cart_rom) {
        gemu_mapping_set_ptr(c->cart_rom, ptr);
        if (c->cart_rom->enabled != on) {
            gemu_mapping_enable(c->cart_rom, on);
            gemu_mapping_enable(c->cart_drop, on);
        }
    }
    if (c->axlon_win) {
        if (s->axlon_bank)
            gemu_mapping_set_ptr(c->axlon_win, s->axlon_ram +
                                 (size_t)(s->axlon_bank - 1u) * A400_AXLON_BANK_SIZE);
        if (c->axlon_win->enabled != (s->axlon_bank != 0))
            gemu_mapping_enable(c->axlon_win, s->axlon_bank != 0);
    }
}

/* The 6502's /NMI (an edge: ANTIC's VBI) and /IRQ (level: POKEY). */
static void cpu_nmi_in(void *ud, int idx, bool level) {
    (void)idx;
    if (level) ((Atari400State *)ud)->cpu.nmi = true;
}
static void cpu_irq_in(void *ud, int idx, bool level) {
    (void)idx;
    ((Atari400State *)ud)->cpu.irq = level;
}
static bool a400_lines_build(Atari400State *s) {
    A400Core *c = s->core;
    return (c->cpu_nmi = gemu_line_in(c->cpu, "nmi", cpu_nmi_in, s, 0)) &&
           (c->cpu_irq = gemu_line_in(c->cpu, "irq", cpu_irq_in, s, 0)) &&
           (c->nmi = gemu_line_out(c->antic, "nmi")) &&
           (c->irq = gemu_line_out(c->pokey, "irq")) &&
           gemu_line_connect_in(c->nmi, c->cpu_nmi, 0) &&
           gemu_line_connect_in(c->irq, c->cpu_irq, 0);
}
static inline void a400_irq_update(Atari400State *s) {
    gemu_line_set(s->core->irq, pokey_irq_asserted(&s->pokey));
}
static void a400_vbi(Atari400State *s) {
    s->antic.nmist |= ANTIC_NMI_VBI;
    if (s->antic.nmien & ANTIC_NMI_VBI) gemu_line_pulse(s->core->nmi);
}

static Atari400State *a400_of(GemuDevice *d) { return *(Atari400State **)d->priv; }

static void a400_board_describe(GemuDevice *d, char *buf, size_t len) {
    const Atari400State *s = a400_of(d);
    snprintf(buf, len, "%u KB RAM, frame %" PRIu64 "%s\n"
             "keyboard queue %d, key %s",
             (unsigned)(s->ram_size / 1024), s->frame, s->wsync ? ", WSYNC held" : "",
             (s->keyq_tail - s->keyq_head + A400_KEYQ_LEN) % A400_KEYQ_LEN,
             s->key_hold_frames ? "down" : "up");
}

static void a400_cpu_describe(GemuDevice *d, char *buf, size_t len) {
    const Mos6502 *c = &a400_of(d)->cpu;
    snprintf(buf, len, "PC %04X  A %02X  X %02X  Y %02X  SP %02X  P %02X\n"
             "cycle %" PRIu64 ", instruction %" PRIu64 "%s%s",
             c->PC, c->A, c->X, c->Y, c->SP, c->P, c->cycle_count, c->insn_count,
             c->nmi ? ", NMI pending" : "", c->irq ? ", /IRQ low" : "");
}

static void a400_antic_describe(GemuDevice *d, char *buf, size_t len) {
    const Antic *a = &a400_of(d)->antic;
    snprintf(buf, len, "line %d of %d, DMACTL %02X, display list %04X, CHBASE %02X\n"
             "NMIEN %02X  NMIST %02X  COLPF %02X %02X %02X %02X  COLBK %02X  PRIOR %02X",
             a->scanline, a->lines_total, a->dmactl, a->dlist, a->chbase, a->nmien, a->nmist,
             a->colpf[0], a->colpf[1], a->colpf[2], a->colpf[3], a->colbk, a->prior);
}

static void a400_pokey_describe(GemuDevice *d, char *buf, size_t len) {
    const Pokey *p = &a400_of(d)->pokey;
    snprintf(buf, len, "IRQEN %02X, pending %02X, SKCTL %02X, SKSTAT %02X, KBCODE %02X\n"
             "AUDF %02X %02X %02X %02X  AUDC %02X %02X %02X %02X  AUDCTL %02X (not played)",
             p->irqen, p->irq_pending, p->skctl, p->skstat, p->kbcode,
             p->audf[0], p->audf[1], p->audf[2], p->audf[3],
             p->audc[0], p->audc[1], p->audc[2], p->audc[3], p->audctl);
}

static void a400_pia_describe(GemuDevice *d, char *buf, size_t len) {
    const Pia6821 *p = &a400_of(d)->pia;
    snprintf(buf, len, "port A %02X (DDR %02X, CR %02X), port B %02X (DDR %02X, CR %02X)",
             p->ora, p->ddra, p->cra, p->orb, p->ddrb, p->crb);
}

static void a400_cart_status(void *ud, char *buf, size_t buf_len);
static void a400_cart_slot_describe(GemuDevice *d, char *buf, size_t len) {
    const Atari400State *s = a400_of(d);
    char st[96];
    a400_cart_status((void *)s, st, sizeof st);
    if (s->cart_type == 41u || s->cart_type == 42u)
        snprintf(buf, len, "%s, bank %u", st, s->cart_bank);
    else
        snprintf(buf, len, "%s", st);
}

static void a400_axlon_describe(GemuDevice *d, char *buf, size_t len) {
    const Atari400State *s = a400_of(d);
    snprintf(buf, len, "1008 KB in 63 banks; bank %u at $4000%s", s->axlon_bank,
             s->axlon_bank ? "" : " (base RAM)");
}

static void a400_floppy_status(void *ud, char *buf, size_t buf_len);
static void a400_fdd_describe(GemuDevice *d, char *buf, size_t len) {
    a400_floppy_status(a400_of(d), buf, len);
}

/* After a load: the ARGB frame from the colour codes; the cartridge's and
 * Axlon's windows. */
static void a400_board_post_load(GemuDevice *d) {
    Atari400State *s = a400_of(d);
    antic_refresh_argb(&s->antic);
    a400_maps_sync(s);
}

/* ── "info": which cartridge is in the slot ── its size, CAR type and base
 * (0 for none).  A state for another is refused. */
static void a400_cart_info_save(void *ud, GemuStateWriter *w) {
    const Atari400State *s = ud;
    gemu_state_write_u32(w, s->cart ? s->cart_size : 0);
    gemu_state_write_u32(w, s->cart ? s->cart_type : 0);
    gemu_state_write_u32(w, s->cart ? s->cart_base : 0);
}
static bool a400_cart_info_check(void *ud, GemuStateReader *r) {
    const Atari400State *s = ud;
    uint32_t size, type, base;
    if (gemu_state_left(r) != 12 || !gemu_state_read_u32(r, &size) ||
        !gemu_state_read_u32(r, &type) || !gemu_state_read_u32(r, &base)) {
        gemu_state_refuse(r, "atari400 cartridge: bad info item");
        return false;
    }
    uint32_t hs = s->cart ? s->cart_size : 0, ht = s->cart ? s->cart_type : 0;
    if (size != hs || type != ht) {
        gemu_state_refuse(r, "atari400 cartridge: the state is for %s%uK (type %u); the slot "
                          "holds %s%uK (type %u)", size ? "" : "no cartridge, ", size / 1024, type,
                          hs ? "" : "no cartridge, ", hs / 1024, ht);
        return false;
    }
    return true;
}
static bool a400_cart_info_load(void *ud, GemuStateReader *r) { return a400_cart_info_check(ud, r); }

#define A400_TYPE(var, tname, tdesc, describe_fn) \
    static const GemuDeviceType var = { .name = tname, .desc = tdesc, \
        .size = sizeof(Atari400State *), .describe = describe_fn }
static const GemuDeviceType a400_board_type = {
    .name = "atari400-board", .desc = "Atari 400/800 main board", .size = sizeof(Atari400State *),
    .describe = a400_board_describe, .post_load = a400_board_post_load,
};
A400_TYPE(a400_cpu_type, "mos6502", "MOS 6502 CPU", a400_cpu_describe);
A400_TYPE(a400_antic_type, "antic-gtia", "Atari ANTIC and GTIA (display)", a400_antic_describe);
A400_TYPE(a400_pokey_type, "pokey", "Atari POKEY (keyboard, IRQs, timers, serial)", a400_pokey_describe);
A400_TYPE(a400_pia_type, "pia6821", "Motorola 6820/6821 PIA (joystick ports)", a400_pia_describe);
A400_TYPE(a400_cart_type, "atari400-cart-slot", "Atari 400/800 cartridge slot", a400_cart_slot_describe);
A400_TYPE(a400_axlon_type, "axlon1056", "Axlon RAMPower 1056K banked RAM", a400_axlon_describe);
static const GemuDeviceType a400_fdd_type = {
    .name = "atari810", .desc = "Atari 810 disk drive (SIO, high-level)", .size = sizeof(Atari400State *),
    .describe = a400_fdd_describe, .stateless = true,
};

#define M(dev, field) ok &= GEMU_STATE_MEMBER(dev, "", s, field)
/* The parts' own state.  Not saved: the configuration (cfg, ram_size), the
 * OS ROM, the cartridge and disk images (media: "info" checks the
 * cartridge), the host's input (held_actions, prev_arrow) and the front
 * end. */
static bool a400_parts_state(Atari400State *s) {
    A400Core *c = s->core;
    bool ok = true;
    M(c->board, ram);
    M(c->board, wsync);
    M(c->board, keyq);
    M(c->board, keyq_head);
    M(c->board, keyq_tail);
    M(c->board, key_hold_frames);
    M(c->board, key_gap_frames);
    M(c->board, frame);
    M(c->board, line);
    M(c->board, line_begun);
    M(c->board, frame_start);
    ok &= gemu_state_add_custom(c->cart, "info", a400_cart_info_save, a400_cart_info_check,
                                a400_cart_info_load, s);
    M(c->cart, cart_bank);
    M(c->cart, cart_enabled);
    if (c->axlon) {
        M(c->axlon, axlon_bank);
        ok &= gemu_state_add(c->axlon, "ram", s->axlon_ram, A400_AXLON_BANK_SIZE, A400_AXLON_EXT_BANKS);
    }
    return ok;
}
#undef M

static GemuDevice *a400_part(Atari400State *s, const GemuDeviceType *t, const char *name) {
    GemuDevice *d = gemu_device_add(s->core->machine, t, name, NULL, NULL);
    if (d)
        *(Atari400State **)d->priv = s;
    return d;
}

static uint64_t a400_exec_run(void *ud, uint64_t budget);
static uint64_t a400_exec_elapsed(void *ud);
static uint64_t a400_cpu_hz(const Atari400State *s);

/* ── ANTIC's screen (stage T4) ───────────────────────────────────────────────
 * 320×240, ANTIC's ARGB frame (lines 8-247).  Device-timed: a frame begins
 * when ANTIC paints one (a400_insn()).  The raw timing describes the beam:
 * 114 CPU cycles a line, four hi-res pixels to a cycle, 320 of the 456
 * shown, the region's lines. */
static void a400_screen_update(void *ud, GemuBitmap *bmp, const GemuRect *clip) {
    const Atari400State *s = ud;
    for (int y = clip->y0; y < clip->y1; y++)
        memcpy(bmp->pix + (size_t)y * (size_t)bmp->stride, s->antic.pixels_argb + (size_t)y * ANTIC_FB_W,
               ANTIC_FB_W * sizeof(uint32_t));
}

static bool a400_screen_build(Atari400State *s) {
    A400Core *c = s->core;
    if (!(c->screen = gemu_screen_new(c->antic, "screen")) ||
        !gemu_screen_set_size(c->screen, ANTIC_FB_W, ANTIC_FB_H))
        return false;
    gemu_screen_set_aspect(c->screen, 4, 3);
    gemu_screen_set_update(c->screen, a400_screen_update, s);
    int htotal = ANTIC_CYCLES_PER_LINE * 4, hbend = (htotal - ANTIC_FB_W) / 2;
    return gemu_screen_set_raw(c->screen, a400_cpu_hz(s) * 4u, htotal, hbend, hbend + ANTIC_FB_W,
                               s->antic.lines_total, ANTIC_FIRST_VISIBLE,
                               ANTIC_FIRST_VISIBLE + ANTIC_FB_H, false);
}

GemuScreen *a400_screen(Atari400State *s) { return s->core ? s->core->screen : NULL; }

static bool a400_core_build(Atari400State *s) {
    A400Core *c = s->core = calloc(1, sizeof *c);
    if (!c || !(c->machine = gemu_machine_new("atari400")))
        return false;
    /* -M settings (stage T7): its devices have none, so any is refused */
    static const GemuDeviceType *const set_types[] = { &a400_board_type, &a400_cpu_type,
        &a400_antic_type, &a400_pokey_type, &a400_pia_type, &a400_cart_type };
    char set[6][GEMU_OPTS_ROUTE_LEN], why[512];
    if (!gemu_opts_route(s->cfg->machine_opts, set_types, 6, set, why, sizeof why)) {
        fprintf(stderr, "gemu: %s\n", why);
        return false;
    }
    bool ok = (c->board = a400_part(s, &a400_board_type, "board")) &&
              (c->cpu = a400_part(s, &a400_cpu_type, "cpu")) && mos6502_state_add(c->cpu, "", &s->cpu) &&
              (c->antic = a400_part(s, &a400_antic_type, "antic")) &&
              antic_state_add(c->antic, "", &s->antic) &&
              (c->pokey = a400_part(s, &a400_pokey_type, "pokey")) &&
              pokey_state_add(c->pokey, "", &s->pokey) &&
              (c->pia = a400_part(s, &a400_pia_type, "pia")) && pia6821_state_add(c->pia, "", &s->pia) &&
              (c->cart = a400_part(s, &a400_cart_type, "cart"));
    if (ok && s->axlon_ram) ok = (c->axlon = a400_part(s, &a400_axlon_type, "axlon")) != NULL;
    if (ok && s->cfg->atari810) ok = (c->fdd = a400_part(s, &a400_fdd_type, "fdd")) != NULL;
    ok = ok && a400_parts_state(s);
    if (!ok) {   /* before init, which clears the reason (a bad setting, say) */
        fprintf(stderr, "atari400: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    bool inited = gemu_machine_init(c->machine);
    static const GemuExecutorOps exec_ops = { .run = a400_exec_run, .elapsed = a400_exec_elapsed };
    ok = ok && inited && a400_bus_build(s) && a400_lines_build(s) &&
         (c->exec = gemu_executor_new(c->cpu, a400_cpu_hz(s), &exec_ops, s)) &&
         a400_screen_build(s);
    if (!ok || !inited || !gemu_machine_start(c->machine)) {
        fprintf(stderr, "atari400: core machine: %s\n", gemu_machine_error(c->machine)[0]
                ? gemu_machine_error(c->machine) : "a device couldn't be added");
        return false;
    }
    gemu_monitor_attach_machine(s->monitor, c->machine, "program");
    return true;
}

static void a400_core_free(Atari400State *s) {
    if (!s->core) return;
    gemu_machine_free(s->core->machine);
    free(s->core);
    s->core = NULL;
}

/* ── The beam and the 6502 as the machine's executor (stage T3) ─────────────
 * 114 CPU cycles a line, lines_total lines a frame.  A line's start sets
 * VCOUNT's line, raises the VBI on line 248 and ticks POKEY; WSYNC holds
 * the CPU to the line's end; after the last line ANTIC paints the frame.
 * Before T3 this was the run loop's own loop, restarted at line 0 after
 * every stop; now it is machine state (line, line_begun, frame_start) and a
 * run carries on where the last one stopped.  The monitor's step goes
 * through the same instruction, so it clocks the beam too (before, it
 * didn't: no VBI, no POKEY ticks while stepping). */
static void a400_line_begin(Atari400State *s) {
    if (s->line_begun) return;
    s->line_begun = true;
    s->antic.scanline = s->line;
    if (s->line == ANTIC_VBI_LINE) a400_vbi(s);
    pokey_tick(&s->pokey);
}

static uint64_t a400_line_end(const Atari400State *s) {
    return s->frame_start + (uint64_t)(s->line + 1) * ANTIC_CYCLES_PER_LINE;
}

/* One instruction (or an SIO trap) on the beam; true if it finished the
 * frame. */
static bool a400_insn(Atari400State *s) {
    a400_line_begin(s);
    uint64_t end = a400_line_end(s);
    if (!a400_sio_trap(s)) {
        a400_irq_update(s);
        mos6502_step(&s->cpu);
        if (s->wsync) {
            s->wsync = false;
            if (s->cpu.cycle_count < end) s->cpu.cycle_count = end;
        }
    }
    while (s->cpu.cycle_count >= a400_line_end(s)) {
        s->line_begun = false;
        if (++s->line == s->antic.lines_total) {
            s->line = 0;
            s->frame_start = s->cpu.cycle_count;
            antic_render_frame(&s->antic);
            s->frame++;
            if (s->core->screen) gemu_screen_frame_begin(s->core->screen);
            return true;
        }
        a400_line_begin(s);
    }
    return false;
}

static uint64_t a400_exec_run(void *ud, uint64_t budget) {
    Atari400State *s = ud;
    A400Core *c = s->core;
    (void)budget;
    uint64_t start = c->run_start = s->cpu.cycle_count;
    while (s->cpu.cycle_count - start < gemu_executor_budget(c->exec)) {
        a400_line_begin(s);
        if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) { c->stopped = true; break; }
        bool done = a400_insn(s);
        if (gemu_monitor_is_paused(s->monitor)) { c->stopped = true; break; }
        if (done) { c->frame_done = true; break; }
    }
    if (c->stopped || c->frame_done)
        gemu_machine_stop(c->machine);
    return s->cpu.cycle_count - start;
}

static uint64_t a400_exec_elapsed(void *ud) {
    Atari400State *s = ud;
    return s->cpu.cycle_count - s->core->run_start;
}

static uint64_t a400_cpu_hz(const Atari400State *s) {
    return s->cfg->is_pal ? (uint64_t)ATARI400_CPU_HZ_PAL : (uint64_t)ATARI400_CPU_HZ_NTSC;
}

/* Run until the frame is painted or something stops the machine. */
static void a400_run_frame(Atari400State *s) {
    A400Core *c = s->core;
    c->stopped = c->frame_done = false;
    uint64_t cycles = (uint64_t)s->antic.lines_total * ANTIC_CYCLES_PER_LINE + 1u;
    while (!c->frame_done && !c->stopped)
        gemu_machine_run_until(c->machine, gemu_machine_time(c->machine) +
                               gemu_clocks(a400_cpu_hz(s), cycles));
}

/* ── Create / destroy ───────────────────────────────────────────────────── */

Atari400State *atari400_create(const MosConfig *cfg) {
    Atari400State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;

    s->ram_size = a400_installed_ram_size(s);
    if (cfg->axlon1056) {
        s->axlon_ram = calloc(A400_AXLON_EXT_BANKS, A400_AXLON_BANK_SIZE);
        if (!s->axlon_ram) { free(s); return NULL; }
        printf("atari400: Axlon 1056K expansion (48K base + 63 x 16K banks)\n");
    }

    bool have_os = false;
    for (int i = 0; i < cfg->n_roms; i++) {
        uint32_t addr = cfg->roms[i].addr;
        if (!a400_load_file_at(s, cfg->roms[i].path, addr)) {
            free(s->cart); free(s->axlon_ram); free(s);
            return NULL;
        }
        if (addr >= 0xD800u) have_os = true;
    }
    if (!have_os) {
        fprintf(stderr, "atari400: missing OS ROMs - use -rom roms/a400.zip\n");
        free(s->cart); free(s->axlon_ram); free(s);
        return NULL;
    }
    if (cfg->cart_path && !a400_load_cart(s, cfg->cart_path)) {
        free(s->cart); free(s->axlon_ram); free(s);
        return NULL;
    }
    if (cfg->fda_path && !a400_load_atr(s, cfg->fda_path)) {
        free(s->disk_data); free(s->cart); free(s->axlon_ram); free(s);
        return NULL;
    }

    antic_init(&s->antic, a400_read, s, cfg->is_pal);
    pokey_init(&s->pokey);
    pia6821_init(&s->pia);

    mos6502_init(&s->cpu);
    s->cpu.mem_read  = a400_read;
    s->cpu.mem_write = a400_write;
    s->cpu.mem_ud    = s;
    mos_cpu_apply(&s->cpu, cfg->cpu);

    s->monitor = gemu_monitor_create();
    if (!s->monitor) {
        free(s->disk_data); free(s->cart); free(s->axlon_ram); free(s); return NULL;
    }
    gemu_monitor_set_cpu_state_cb(s->monitor, a400_cpu_state, s);
    gemu_monitor_set_screendump_cb(s->monitor, a400_screendump, s);
    gemu_monitor_bind(s->monitor, "sendkey", a400_cmd_sendkey, s);
    gemu_monitor_set_memory(s->monitor, &(GemuMonMemory){
        .read = a400_mon_read, .write = a400_mon_write, .ud = s,
        .addr_bits = 16, .big_endian = false });

    if (cfg->atari810) {
        GemuMediaDevice floppy_dev = {
            .name = "floppy", .kind = "atari810", .ud = s,
            .change = a400_floppy_change, .eject = a400_floppy_eject,
            .status = a400_floppy_status,
        };
        if (cfg->fda_path)
            snprintf(floppy_dev.file, sizeof(floppy_dev.file), "%s", cfg->fda_path);
        gemu_monitor_register_media(s->monitor, &floppy_dev);
    }

    GemuMediaDevice cart_dev = {
        .name = "cartridge", .kind = "cartridge", .ud = s,
        .change = a400_cart_change, .eject = a400_cart_eject,
        .status = a400_cart_status,
    };
    if (cfg->cart_path)
        snprintf(cart_dev.file, sizeof(cart_dev.file), "%s", cfg->cart_path);
    gemu_monitor_register_media(s->monitor, &cart_dev);

    if (gemu_display_wanted(cfg->display_type)) {
        s->display = gemu_display_create(cfg->display_type, &(GemuDisplayConfig){
            .fb_width    = ANTIC_FB_W,
            .monitor     = s->monitor,
            .fb_height   = ANTIC_FB_H,
            .scale       = cfg->display_scale,
            .renderer    = cfg->display_renderer,
            .actions     = a400_actions,
            .n_actions   = A400_NUM_ACTIONS,
            .ini_section = "atari400",
            .terminal_text = cfg->display_type == GEMU_DISPLAY_CURSES,
        });
    }

    if (!a400_core_build(s)) {   /* the bus first: reset reads the vector */
        atari400_destroy(s);
        return NULL;
    }
    a400_reset(s);
    s->frame_start = s->cpu.cycle_count;   /* frame 0 begins after the reset */
    return s;
}

void atari400_destroy(Atari400State *s) {
    if (!s) return;
    a400_core_free(s);
    gemu_monitor_destroy(s->monitor);
    gemu_display_destroy(s->display);
    free(s->disk_data);
    free(s->cart);
    free(s->axlon_ram);
    free(s);
}

/* ── Run loop ───────────────────────────────────────────────────────────── */

void atari400_run(Atari400State *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        if (s->display) {
            s->held_actions = gemu_display_poll(s->display);
            if (gemu_display_should_quit(s->display)) break;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                a400_reset(s);
            }
            uint32_t cp;
            while ((cp = gemu_display_pop_raw_key(s->display)) != 0) {
                if (!s->cfg->generic_keyboard) continue;
                a400_queue_char(s, cp);
            }
            if (s->cfg->generic_keyboard)
                a400_poll_cursor_keys(s);
            if (s->cfg->generic_keyboard &&
                (gemu_display_last_pressed(s->display) & GEMU_ACTION(A400_ACT_BREAK)))
                pokey_break_key(&s->pokey);
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { quit = true; break; }
            } else if (cmd == GEMU_MON_RESET) {
                a400_reset(s);
            } else if (cmd == GEMU_MON_STEP) {
                for (uint32_t n = gemu_monitor_step_count(s->monitor); n; n--)
                    a400_insn(s);
                antic_render_frame(&s->antic);   /* the window follows stepping */
            }
        }
        if (quit) break;
        if (s->display)
            gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));

        a400_key_pump(s);

        bool paused = gemu_monitor_is_paused(s->monitor);
        if (!paused) {
            a400_run_frame(s);
            if (s->core->stopped)
                antic_render_frame(&s->antic);   /* the window shows where it stopped */
        }

        if (s->display) {
            if (cfg->display_type == GEMU_DISPLAY_CURSES)
                a400_render_curses_text(s);
            else {
                const GemuBitmap *b = gemu_screen_render(a400_screen(s));
                gemu_display_render(s->display, b->pix, b->width, b->height);
            }
        }

        /* Pace to the machine's time (the CPU's cycles at its clock);
         * after a pause, start over. */
        if (paused) {
            gemu_machine_pace_reset(s->core->machine);
            SDL_Delay(16);
        } else {
            gemu_machine_pace(s->core->machine, 0);
        }
    }

    gemu_monitor_stop(s->monitor);
    printf("atari400: %llu frames, %llu cpu cycles\n",
           (unsigned long long)s->frame, (unsigned long long)s->cpu.cycle_count);
}
