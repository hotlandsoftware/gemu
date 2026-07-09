/*
 * Atari 400 machine — see hardware/atari400.h for the memory map and the
 * modelling scope of each chip.
 */
#include "atari400.h"
#include "gemu/screendump.h"
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
    if (next == s->keyq_head) return;    /* queue full — drop */
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
    v &= (uint8_t)~s->vnc_console;
    return v;
}

/* ── Memory map ─────────────────────────────────────────────────────────── */

static uint8_t a400_read(uint16_t addr, void *ud) {
    Atari400State *s = ud;
    if (addr < s->ram_size) return s->ram[addr];
    if (s->cart_base && addr >= s->cart_base &&
        addr < s->cart_base + s->cart_size)
        return s->cart[addr - s->cart_base];
    if (addr >= 0xD800u) return s->os_rom[addr - 0xD800u];
    switch (addr & 0xFF00u) {
    case 0xD000u: {
        uint8_t trig[4] = { 1, 1, 1, 1 };
        if (s->held_actions & GEMU_ACTION(A400_ACT_FIRE)) trig[0] = 0;
        return gtia_reg_read(&s->antic, (uint8_t)addr, a400_consol(s), trig);
    }
    case 0xD200u: return pokey_read(&s->pokey, (uint8_t)addr);
    case 0xD300u: return pia6821_read(&s->pia, addr & 3u);
    case 0xD400u: return antic_reg_read(&s->antic, (uint8_t)addr);
    default:      return 0xFF;            /* open bus */
    }
}

static void a400_write(uint16_t addr, uint8_t val, void *ud) {
    Atari400State *s = ud;
    if (addr < s->ram_size) { s->ram[addr] = val; return; }
    switch (addr & 0xFF00u) {
    case 0xD000u: gtia_reg_write(&s->antic, (uint8_t)addr, val); break;
    case 0xD200u: pokey_write(&s->pokey, (uint8_t)addr, val); break;
    case 0xD300u: pia6821_write(&s->pia, addr & 3u, val); break;
    case 0xD400u:
        if ((addr & 0x0Fu) == 0x0A) s->wsync = true;      /* WSYNC */
        else antic_reg_write(&s->antic, (uint8_t)addr, val);
        break;
    default: break;                       /* ROM / open bus */
    }
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

/* monitor: sendkey <text>  — queue each character (\n = Return) */
static bool a400_sendkey_command(Atari400State *s, const char *text) {
    if (!text || strncmp(text, "sendkey ", 8) != 0) return false;
    if (!s->cfg->generic_keyboard) {
        printf("sendkey: keyboard device is not attached\n");
        return true;
    }
    for (const char *p = text + 8; *p; p++)
        a400_queue_char(s, *p == '\n' ? (uint32_t)'\r' : (uint32_t)*p);
    return true;
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

static bool a400_peek_command(Atari400State *s, const char *text) {
    unsigned addr, len = 16;
    if (!text || sscanf(text, "peek %x %x", &addr, &len) < 1) return false;
    if (len > 128) len = 128;
    for (unsigned i = 0; i < len; i++) {
        if ((i & 15u) == 0) printf("%04X:", (addr + i) & 0xFFFFu);
        printf(" %02X", a400_read((uint16_t)(addr + i), s));
        if ((i & 15u) == 15u || i + 1 == len) printf("\n");
    }
    return true;
}

/* ── VNC keyboard ───────────────────────────────────────────────────────── */

static void a400_poll_vnc(Atari400State *s) {
    if (!s->vnc) return;
    GemuVncKeyEvent ev;
    while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
        uint32_t k = ev.keysym;
        /* Console keys follow key level; everything else queues on press. */
        if (k >= 0xFFBFu && k <= 0xFFC1u) {          /* F2..F4 */
            uint8_t bit = (uint8_t)(1u << (k - 0xFFBFu));
            if (ev.down) s->vnc_console |= bit;
            else         s->vnc_console &= (uint8_t)~bit;
            continue;
        }
        if (!s->cfg->generic_keyboard) continue;
        if (!ev.down) continue;
        switch (k) {
        case 0xFF0Du: case 0xFF8Du: a400_queue_char(s, '\r');  break;
        case 0xFF08u:               a400_queue_char(s, '\b');  break;
        case 0xFF1Bu:               a400_queue_char(s, 0x1B);  break;
        case 0xFF09u:               a400_queue_char(s, '\t');  break;
        case 0xFF52u: a400_queue_key(s, 0x8E); break;  /* Up    */
        case 0xFF54u: a400_queue_key(s, 0x8F); break;  /* Down  */
        case 0xFF51u: a400_queue_key(s, 0x86); break;  /* Left  */
        case 0xFF53u: a400_queue_key(s, 0x87); break;  /* Right */
        default:
            if (k >= 0x20u && k <= 0x7Eu) a400_queue_char(s, k);
            break;
        }
    }
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

static bool a400_load_cart(Atari400State *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "atari400: cannot open cartridge '%s'\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz != 0x1000 && sz != 0x2000 && sz != 0x4000) {
        fprintf(stderr, "atari400: unsupported cartridge size %ld "
                        "(need 4, 8 or 16 KB raw image)\n", sz);
        fclose(f);
        return false;
    }
    if (fread(s->cart, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "atari400: read error on '%s'\n", path);
        fclose(f);
        return false;
    }
    fclose(f);
    s->cart_size = (uint32_t)sz;
    s->cart_base = (uint16_t)(0xC000u - sz);   /* 4K→$B000, 8K→$A000, 16K→$8000 */
    /* Cartridge shadows RAM: cap RAM below the cart window. */
    if (s->ram_size > s->cart_base) s->ram_size = s->cart_base;
    printf("atari400: cartridge '%s' (%ldK at $%04X)\n",
           path, sz / 1024, s->cart_base);
    return true;
}

/* ── Create / destroy ───────────────────────────────────────────────────── */

Atari400State *atari400_create(const MosConfig *cfg) {
    Atari400State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;

    s->ram_size = cfg->mem_size ? cfg->mem_size : 0x4000u;
    if (s->ram_size > ATARI400_RAM_MAX) s->ram_size = ATARI400_RAM_MAX;

    bool have_os = false;
    for (int i = 0; i < cfg->n_roms; i++) {
        uint32_t addr = cfg->roms[i].addr;
        if (!a400_load_file_at(s, cfg->roms[i].path, addr)) {
            free(s);
            return NULL;
        }
        if (addr >= 0xD800u) have_os = true;
    }
    if (!have_os) {
        fprintf(stderr, "atari400: missing OS ROMs — use -rom roms/a400.zip\n");
        free(s);
        return NULL;
    }
    if (cfg->cart_path && !a400_load_cart(s, cfg->cart_path)) {
        free(s);
        return NULL;
    }

    antic_init(&s->antic, a400_read, s, cfg->is_pal);
    pokey_init(&s->pokey);
    pia6821_init(&s->pia);

    mos6502_init(&s->cpu);
    s->cpu.mem_read  = a400_read;
    s->cpu.mem_write = a400_write;
    s->cpu.mem_ud    = s;

    s->monitor = gemu_monitor_create();
    if (!s->monitor) { free(s); return NULL; }
    gemu_monitor_set_cpu_state_cb(s->monitor, a400_cpu_state, s);
    gemu_monitor_set_screendump_cb(s->monitor, a400_screendump, s);

    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr, ANTIC_FB_W, ANTIC_FB_H);
        if (s->vnc)
            gemu_vnc_set_palette(s->vnc, antic_palette_rgb, 256);
        else
            fprintf(stderr, "atari400: failed to start VNC at %s\n", cfg->vnc_addr);
    }
    gemu_monitor_set_vnc(s->monitor, s->vnc);

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        s->display = gemu_display_create(cfg->display_type, &(GemuDisplayConfig){
            .title       = "GEMU",
            .fb_width    = ANTIC_FB_W,
            .fb_height   = ANTIC_FB_H,
            .scale       = cfg->display_scale,
            .renderer    = cfg->display_renderer,
            .actions     = a400_actions,
            .n_actions   = A400_NUM_ACTIONS,
            .ini_section = "atari400",
            .terminal_text = cfg->display_type == GEMU_DISPLAY_CURSES,
        });
    }

    a400_reset(s);
    return s;
}

void atari400_destroy(Atari400State *s) {
    if (!s) return;
    gemu_monitor_destroy(s->monitor);
    gemu_display_destroy(s->display);
    gemu_vnc_destroy(s->vnc);
    free(s);
}

/* ── Run loop ───────────────────────────────────────────────────────────── */

void atari400_run(Atari400State *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);

    const int lines = s->antic.lines_total;
    const Uint32 frame_ms = cfg->is_pal ? 20u : 16u;
    uint64_t frame_start = s->cpu.cycle_count;

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

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
        a400_poll_vnc(s);

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { quit = true; break; }
            } else if (cmd == GEMU_MON_RESET) {
                a400_reset(s);
            } else if (cmd == GEMU_MON_CUSTOM) {
                const char *text = gemu_monitor_command_text(s->monitor);
                if (!a400_sendkey_command(s, text) &&
                    !a400_peek_command(s, text))
                    gemu_monitor_unknown_command(s->monitor);
            }
        }
        if (quit) break;
        if (s->display)
            gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));

        a400_key_pump(s);

        if (!gemu_monitor_is_paused(s->monitor)) {
            bool stop = false;
            for (int line = 0; line < lines && !stop; line++) {
                s->antic.scanline = line;
                if (line == ANTIC_VBI_LINE) {
                    s->antic.nmist |= ANTIC_NMI_VBI;
                    if (s->antic.nmien & ANTIC_NMI_VBI) s->cpu.nmi = true;
                }
                pokey_tick(&s->pokey);
                uint64_t target = frame_start +
                    (uint64_t)(line + 1) * ANTIC_CYCLES_PER_LINE;
                while (s->cpu.cycle_count < target) {
                    if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) { stop = true; break; }
                    s->cpu.irq = pokey_irq_asserted(&s->pokey);
                    mos6502_step(&s->cpu);
                    if (s->wsync) {
                        s->wsync = false;
                        if (s->cpu.cycle_count < target)
                            s->cpu.cycle_count = target;
                    }
                    if (gemu_monitor_is_paused(s->monitor)) { stop = true; break; }
                }
            }
            frame_start = s->cpu.cycle_count;
            antic_render_frame(&s->antic);
            s->frame++;
        }

        if (s->display) {
            if (cfg->display_type == GEMU_DISPLAY_CURSES)
                a400_render_curses_text(s);
            else
                gemu_display_render(s->display, s->antic.pixels_argb,
                                    ANTIC_FB_W, ANTIC_FB_H);
        }
        if (s->vnc)
            gemu_vnc_update(s->vnc, s->antic.pixels, ANTIC_FB_W, ANTIC_FB_H);

        Uint32 dt = SDL_GetTicks() - t0;
        if (dt < frame_ms) SDL_Delay(frame_ms - dt);
    }

    gemu_monitor_stop(s->monitor);
    printf("atari400: %llu frames, %llu cpu cycles\n",
           (unsigned long long)s->frame, (unsigned long long)s->cpu.cycle_count);
}
