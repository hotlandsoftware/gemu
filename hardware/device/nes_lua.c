#include "nes_lua.h"
#include "input_menu.h"   /* gemu_font_glyph() - shared 6x8 bitmap font */
#include "gemu/sha256.h"  /* rom.gethash() - SHA-256, not real FCEUX MD5 (see below) */
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#  define strcasecmp _stricmp
#else
#  include <strings.h>
#endif

#define NES_LUA_MEM_HOOK_MAX 64

/* NES controller shift-register bit order (hardware fact, not a GEMU
 * internal detail - kept in sync with NES_BTN_* in hardware/mos/nes.h). */
static const char   *joy_btn_names[8] = {
    "A", "B", "select", "start", "up", "down", "left", "right",
};
static const uint8_t joy_btn_bits[8] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
};

typedef struct {
    uint16_t addr;
    int      ref;   /* LUA_REGISTRYINDEX ref to the Lua callback */
} NesLuaMemHook;

struct NesLua {
    lua_State *L;       /* main state - owns globals, runs registerbefore/after */
    lua_State *co;      /* coroutine running the script's top-level body */
    int        co_ref;  /* registry ref keeping the coroutine alive */
    bool       finished; /* script body returned/errored - stop resuming it */

    NesLuaBus  bus;

    int before_ref;  /* emu.registerbefore() callback, or LUA_NOREF */
    int after_ref;   /* emu.registerafter()  callback, or LUA_NOREF */

    NesLuaMemHook read_hooks[NES_LUA_MEM_HOOK_MAX];
    int n_read_hooks;
    NesLuaMemHook write_hooks[NES_LUA_MEM_HOOK_MAX];
    int n_write_hooks;
    NesLuaMemHook exec_hooks[NES_LUA_MEM_HOOK_MAX];
    int n_exec_hooks;

    int gui_ref;  /* gui.register() callback, or LUA_NOREF */
    int exit_ref; /* emu.registerexit() callback, or LUA_NOREF */

    bool    joy_override[2]; /* joypad.set() queued for the next pre_frame() */
    uint8_t joy_value[2];
    uint8_t last_ctrl_state[2]; /* what joypad.get() reports */

    /* Valid only during nes_lua_run_frame()'s resume - gui.* draws here. */
    uint32_t *draw_px;
    int       draw_w, draw_h;

    /* rom.readbyte/readbytesigned/gethash - the raw cartridge file, header
     * included, loaded independently of NesState's parsed PRG/CHR banks. */
    uint8_t *rom_data;
    long     rom_size;
};

#define CTX(L) ((NesLua *)lua_touserdata((L), lua_upvalueindex(1)))

/* ── emu ─────────────────────────────────────────────────────────────────── */

static int l_emu_frameadvance(lua_State *L) {
    return lua_yield(L, 0);
}

static int l_emu_registerbefore(lua_State *L) {
    NesLua *lua = CTX(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (lua->before_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, lua->before_ref);
    lua_pushvalue(L, 1);
    lua->before_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_emu_registerafter(lua_State *L) {
    NesLua *lua = CTX(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (lua->after_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, lua->after_ref);
    lua_pushvalue(L, 1);
    lua->after_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_emu_exit(lua_State *L) {
    NesLua *lua = CTX(L);
    lua->finished = true;
    return lua_yield(L, 0);
}

static int l_emu_registerexit(lua_State *L) {
    NesLua *lua = CTX(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (lua->exit_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, lua->exit_ref);
    lua_pushvalue(L, 1);
    lua->exit_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_emu_framecount(lua_State *L) {
    NesLua *lua = CTX(L);
    lua_pushinteger(L, lua->bus.get_framecount ? (lua_Integer)lua->bus.get_framecount(lua->bus.ud) : 0);
    return 1;
}

/* Stubs for the parts of emu/FCEU real scripts sometimes call but that need
 * host plumbing (pause/lifecycle/speed) we don't have hooked up yet - a
 * harmless no-op beats an "attempt to call nil" crash. */
static int l_emu_noop(lua_State *L) { (void)L; return 0; }
static int l_emu_false(lua_State *L) { lua_pushboolean(L, 0); return 1; }
static int l_emu_zero(lua_State *L)  { lua_pushinteger(L, 0); return 1; }
static int l_emu_emptystr(lua_State *L) { lua_pushstring(L, ""); return 1; }

static int l_print(lua_State *L) {
    int n = lua_gettop(L);
    fputs("[lua] ", stdout);
    /* Route through the real Lua tostring() - lua_tostring() only converts
     * numbers/strings, but print() must also handle booleans, nil, tables
     * (and any __tostring metamethod) the way the language actually does. */
    lua_getglobal(L, "tostring");
    for (int i = 1; i <= n; i++) {
        if (i > 1) fputc('\t', stdout);
        lua_pushvalue(L, -1);
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        const char *s = lua_tostring(L, -1);
        fputs(s ? s : "?", stdout);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    fputc('\n', stdout);
    return 0;
}

/* ── memory ──────────────────────────────────────────────────────────────── */

static int l_memory_readbyte(lua_State *L) {
    NesLua *lua = CTX(L);
    uint16_t addr = (uint16_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, lua->bus.mem_read(addr, lua->bus.ud));
    return 1;
}

static int l_memory_writebyte(lua_State *L) {
    NesLua *lua = CTX(L);
    uint16_t addr = (uint16_t)luaL_checkinteger(L, 1);
    uint8_t  val  = (uint8_t)luaL_checkinteger(L, 2);
    lua->bus.mem_write(addr, val, lua->bus.ud);
    return 0;
}

static int l_memory_readbytesigned(lua_State *L) {
    NesLua *lua = CTX(L);
    uint16_t addr = (uint16_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (int8_t)lua->bus.mem_read(addr, lua->bus.ud));
    return 1;
}

static int l_memory_readbyterange(lua_State *L) {
    NesLua *lua = CTX(L);
    uint16_t addr = (uint16_t)luaL_checkinteger(L, 1);
    lua_Integer len = luaL_checkinteger(L, 2);
    if (len < 0) len = 0;
    if (len > 0x10000) len = 0x10000;
    char *buf = len ? malloc((size_t)len) : NULL;
    for (lua_Integer i = 0; i < len; i++)
        buf[i] = (char)lua->bus.mem_read((uint16_t)(addr + i), lua->bus.ud);
    lua_pushlstring(L, buf, (size_t)len);
    free(buf);
    return 1;
}

/* Shared by memory.registerwrite/registerread: passing a function (re)binds
 * the hook at `addr`; passing nil removes it. */
static int reg_mem_hook(lua_State *L, NesLuaMemHook *hooks, int *count) {
    uint16_t addr   = (uint16_t)luaL_checkinteger(L, 1);
    bool     has_fn = !lua_isnoneornil(L, 2);

    for (int i = 0; i < *count; i++) {
        if (hooks[i].addr != addr) continue;
        luaL_unref(L, LUA_REGISTRYINDEX, hooks[i].ref);
        hooks[i] = hooks[*count - 1];
        (*count)--;
        break;
    }
    if (has_fn) {
        luaL_checktype(L, 2, LUA_TFUNCTION);
        if (*count >= NES_LUA_MEM_HOOK_MAX)
            return luaL_error(L, "too many memory hooks (max %d)", NES_LUA_MEM_HOOK_MAX);
        lua_pushvalue(L, 2);
        hooks[*count].addr = addr;
        hooks[*count].ref  = luaL_ref(L, LUA_REGISTRYINDEX);
        (*count)++;
    }
    return 0;
}

static int l_memory_registerwrite(lua_State *L) {
    NesLua *lua = CTX(L);
    return reg_mem_hook(L, lua->write_hooks, &lua->n_write_hooks);
}

static int l_memory_registerread(lua_State *L) {
    NesLua *lua = CTX(L);
    return reg_mem_hook(L, lua->read_hooks, &lua->n_read_hooks);
}

static int l_memory_registerexec(lua_State *L) {
    NesLua *lua = CTX(L);
    return reg_mem_hook(L, lua->exec_hooks, &lua->n_exec_hooks);
}

/* ── rom ─────────────────────────────────────────────────────────────────── */

static int l_rom_readbyte(lua_State *L) {
    NesLua *lua = CTX(L);
    lua_Integer addr = luaL_checkinteger(L, 1);
    if (!lua->rom_data || addr < 0 || addr >= lua->rom_size)
        return luaL_error(L, "rom.readbyte: address 0x%X out of range", (unsigned)addr);
    lua_pushinteger(L, lua->rom_data[addr]);
    return 1;
}

static int l_rom_readbytesigned(lua_State *L) {
    NesLua *lua = CTX(L);
    lua_Integer addr = luaL_checkinteger(L, 1);
    if (!lua->rom_data || addr < 0 || addr >= lua->rom_size)
        return luaL_error(L, "rom.readbytesigned: address 0x%X out of range", (unsigned)addr);
    lua_pushinteger(L, (int8_t)lua->rom_data[addr]);
    return 1;
}

/* Real FCEUX returns an MD5 hex digest; we don't carry an MD5 implementation,
 * so this returns a SHA-256 hex digest instead. Same purpose (a stable
 * per-ROM fingerprint script logic can compare against), different bytes -
 * scripts hardcoding a real FCEUX MD5 string won't match this. */
static int l_rom_gethash(lua_State *L) {
    NesLua *lua = CTX(L);
    if (!lua->rom_data) return luaL_error(L, "rom.gethash: no ROM loaded");
    uint8_t digest[GEMU_SHA256_DIGEST_LEN];
    GemuSha256Ctx ctx;
    gemu_sha256_init(&ctx);
    gemu_sha256_update(&ctx, lua->rom_data, (size_t)lua->rom_size);
    gemu_sha256_final(&ctx, digest);
    char hex[65];
    gemu_sha256_hex(digest, hex);
    lua_pushstring(L, hex);
    return 1;
}

/* ── joypad ──────────────────────────────────────────────────────────────── */

static int l_joypad_get(lua_State *L) {
    NesLua *lua = CTX(L);
    int idx = (luaL_optinteger(L, 1, 1) == 2) ? 1 : 0;
    uint8_t state = lua->last_ctrl_state[idx];
    lua_newtable(L);
    for (int i = 0; i < 8; i++) {
        lua_pushboolean(L, (state & joy_btn_bits[i]) != 0);
        lua_setfield(L, -2, joy_btn_names[i]);
    }
    return 1;
}

/* BizHawk-style getdown(): table contains ONLY the buttons currently held
 * (as key=true); unheld buttons are absent, not present as key=false. Real
 * scripts rely on this to distinguish "not pressed" from "not applicable"
 * when iterating with pairs(). */
static int l_joypad_getdown(lua_State *L) {
    NesLua *lua = CTX(L);
    int idx = (luaL_optinteger(L, 1, 1) == 2) ? 1 : 0;
    uint8_t state = lua->last_ctrl_state[idx];
    lua_newtable(L);
    for (int i = 0; i < 8; i++) {
        if (!(state & joy_btn_bits[i])) continue;
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, joy_btn_names[i]);
    }
    return 1;
}

static int l_joypad_set(lua_State *L) {
    NesLua *lua = CTX(L);
    int idx = (luaL_optinteger(L, 1, 1) == 2) ? 1 : 0;
    luaL_checktype(L, 2, LUA_TTABLE);
    uint8_t val = 0;
    for (int i = 0; i < 8; i++) {
        lua_getfield(L, 2, joy_btn_names[i]);
        if (lua_toboolean(L, -1)) val |= joy_btn_bits[i];
        lua_pop(L, 1);
    }
    lua->joy_override[idx] = true;
    lua->joy_value[idx]    = val;
    return 0;
}

/* ── zapper ──────────────────────────────────────────────────────────────── */

static int l_zapper_read(lua_State *L) {
    NesLua *lua = CTX(L);
    int x = 0, y = 0, click = 0;
    if (lua->bus.get_zapper) lua->bus.get_zapper(lua->bus.ud, &x, &y, &click);
    lua_newtable(L);
    lua_pushinteger(L, x); lua_setfield(L, -2, "xmouse");
    lua_pushinteger(L, y); lua_setfield(L, -2, "ymouse");
    lua_pushinteger(L, click); lua_setfield(L, -2, "click");
    return 1;
}

/* ── input (FCEUX mouse/keyboard state, distinct from the zapper table -
 * used by GUI-tool scripts like x_interface.lua for drag/click widgets
 * drawn with gui.*). We only have host mouse position + one click button
 * wired up (the same source as zapper.read()); right/middle click and
 * keyboard key state are not tracked, so they always read false. ── */

static int l_input_get(lua_State *L) {
    NesLua *lua = CTX(L);
    int x = 0, y = 0, click = 0;
    if (lua->bus.get_zapper) lua->bus.get_zapper(lua->bus.ud, &x, &y, &click);
    lua_newtable(L);
    lua_pushinteger(L, x); lua_setfield(L, -2, "xmouse");
    lua_pushinteger(L, y); lua_setfield(L, -2, "ymouse");
    lua_pushboolean(L, click != 0); lua_setfield(L, -2, "leftclick");
    lua_pushboolean(L, 0); lua_setfield(L, -2, "rightclick");
    lua_pushboolean(L, 0); lua_setfield(L, -2, "middleclick");
    return 1;
}

/* ── gui ─────────────────────────────────────────────────────────────────── */

/* Numeric colors are 0xRRGGBB (opaque) or 0xRRGGBBAA (alpha-blended). */
static void decode_color_num(lua_Integer c, uint32_t *rgb, int *alpha) {
    if (c > 0xFFFFFF) { *alpha = (int)(c & 0xFF); c >>= 8; }
    else                *alpha = 255;
    *rgb = (uint32_t)(c & 0xFFFFFF);
}

typedef struct { const char *name; uint32_t rgb; } NesLuaNamedColor;
static const NesLuaNamedColor named_colors[] = {
    { "red",     0xFF0000 }, { "green",  0x00FF00 }, { "blue",    0x0000FF },
    { "white",   0xFFFFFF }, { "black",  0x000000 },
    { "gray",    0x808080 }, { "grey",   0x808080 },
    { "orange",  0xFFA500 }, { "yellow", 0xFFFF00 },
    { "teal",    0x008080 }, { "cyan",   0x00FFFF },
    { "purple",  0x800080 }, { "magenta",0xFF00FF },
};

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* FCEUX color arg: a number (0xRRGGBB/0xRRGGBBAA), an "#rrggbb"/"#rrggbbaa"
 * HTML string, "clear" (fully transparent), or a handful of named colors. */
static void decode_color_str(const char *s, uint32_t *rgb, int *alpha) {
    *rgb = 0xFFFFFF;
    *alpha = 255;
    if (!s) return;
    if (strcasecmp(s, "clear") == 0) { *alpha = 0; return; }
    if (s[0] == '#') {
        size_t len = strlen(s + 1);
        if (len == 6 || len == 8) {
            uint32_t v = 0;
            bool ok = true;
            for (size_t i = 0; i < len; i++) {
                int n = hex_nibble(s[1 + i]);
                if (n < 0) { ok = false; break; }
                v = (v << 4) | (uint32_t)n;
            }
            if (ok) {
                if (len == 8) { *alpha = (int)(v & 0xFF); v >>= 8; }
                *rgb = v & 0xFFFFFF;
            }
        }
        return;
    }
    for (size_t i = 0; i < sizeof(named_colors) / sizeof(named_colors[0]); i++) {
        if (strcasecmp(s, named_colors[i].name) == 0) { *rgb = named_colors[i].rgb; return; }
    }
}

static void decode_color(lua_State *L, int idx, uint32_t *rgb, int *alpha) {
    if (lua_type(L, idx) == LUA_TSTRING) {
        decode_color_str(lua_tostring(L, idx), rgb, alpha);
    } else {
        decode_color_num(luaL_optinteger(L, idx, 0xFFFFFF), rgb, alpha);
    }
}

static void blend_pixel(NesLua *lua, int x, int y, uint32_t rgb, int alpha) {
    if (!lua->draw_px) return;
    if ((unsigned)x >= (unsigned)lua->draw_w || (unsigned)y >= (unsigned)lua->draw_h) return;
    if (alpha <= 0) return;
    uint32_t *p = &lua->draw_px[y * lua->draw_w + x];
    if (alpha >= 255) { *p = rgb; return; }
    uint32_t bg = *p;
    int br = (int)((bg >> 16) & 0xFF), bg_ = (int)((bg >> 8) & 0xFF), bb = (int)(bg & 0xFF);
    int fr = (int)((rgb >> 16) & 0xFF), fg = (int)((rgb >> 8) & 0xFF), fb = (int)(rgb & 0xFF);
    int r = (fr * alpha + br  * (255 - alpha)) / 255;
    int g = (fg * alpha + bg_ * (255 - alpha)) / 255;
    int b = (fb * alpha + bb  * (255 - alpha)) / 255;
    *p = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static int l_gui_pixel(lua_State *L) {
    NesLua *lua = CTX(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    uint32_t rgb; int alpha;
    decode_color(L, 3, &rgb, &alpha);
    blend_pixel(lua, x, y, rgb, alpha);
    return 0;
}

static int l_gui_line(lua_State *L) {
    NesLua *lua = CTX(L);
    int x1 = (int)luaL_checkinteger(L, 1), y1 = (int)luaL_checkinteger(L, 2);
    int x2 = (int)luaL_checkinteger(L, 3), y2 = (int)luaL_checkinteger(L, 4);
    uint32_t rgb; int alpha;
    decode_color(L, 5, &rgb, &alpha);

    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        blend_pixel(lua, x1, y1, rgb, alpha);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
    return 0;
}

static int l_gui_register(lua_State *L) {
    NesLua *lua = CTX(L);
    bool has_fn = !lua_isnoneornil(L, 1);
    if (lua->gui_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, lua->gui_ref); lua->gui_ref = LUA_NOREF; }
    if (has_fn) {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        lua->gui_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

static int l_gui_text(lua_State *L) {
    NesLua *lua = CTX(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    const char *str = luaL_checkstring(L, 3);

    uint32_t frgb; int falpha;
    decode_color(L, 4, &frgb, &falpha);
    bool has_bg = !lua_isnoneornil(L, 5);
    uint32_t brgb = 0; int balpha = 0;
    if (has_bg) decode_color(L, 5, &brgb, &balpha);

    int cx = x;
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        const uint8_t *glyph = gemu_font_glyph((char)*p);
        for (int col = 0; col < GEMU_FONT_W; col++) {
            uint8_t bits = glyph[col];
            for (int row = 0; row < GEMU_FONT_H; row++) {
                bool on = (bits >> row) & 1u;
                if (on)         blend_pixel(lua, cx + col, y + row, frgb, falpha);
                else if (has_bg) blend_pixel(lua, cx + col, y + row, brgb, balpha);
            }
        }
        cx += GEMU_FONT_W;
    }
    return 0;
}

static int l_gui_box(lua_State *L) {
    NesLua *lua = CTX(L);
    int x1 = (int)luaL_checkinteger(L, 1), y1 = (int)luaL_checkinteger(L, 2);
    int x2 = (int)luaL_checkinteger(L, 3), y2 = (int)luaL_checkinteger(L, 4);
    if (x2 < x1) { int t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { int t = y1; y1 = y2; y2 = t; }

    bool has_fill    = !lua_isnoneornil(L, 5);
    bool has_outline = !lua_isnoneornil(L, 6);
    uint32_t frgb = 0, orgb = 0; int falpha = 0, oalpha = 0;
    if (has_fill)    decode_color(L, 5, &frgb, &falpha);
    if (has_outline) decode_color(L, 6, &orgb, &oalpha);
    if (!has_fill && !has_outline) { orgb = 0xFFFFFF; oalpha = 255; has_outline = true; }

    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            bool edge = (x == x1 || x == x2 || y == y1 || y == y2);
            /* With no separate outline color, fill covers the edges too -
             * e.g. gui.drawbox(x1,y1,x2,y2,"red") from real scripts expects
             * a solid box, not a filled interior with a transparent frame. */
            if (edge && has_outline) blend_pixel(lua, x, y, orgb, oalpha);
            else if (has_fill)       blend_pixel(lua, x, y, frgb, falpha);
        }
    }
    return 0;
}

/* ── bit (Lua 5.1 has no bitwise operators - scripts rely on this) ─────────── */

static int l_bit_band(lua_State *L) {
    int n = lua_gettop(L);
    uint32_t r = 0xFFFFFFFFu;
    for (int i = 1; i <= n; i++) r &= (uint32_t)luaL_checkinteger(L, i);
    lua_pushinteger(L, (lua_Integer)r);
    return 1;
}
static int l_bit_bor(lua_State *L) {
    int n = lua_gettop(L);
    uint32_t r = 0;
    for (int i = 1; i <= n; i++) r |= (uint32_t)luaL_checkinteger(L, i);
    lua_pushinteger(L, (lua_Integer)r);
    return 1;
}
static int l_bit_bxor(lua_State *L) {
    int n = lua_gettop(L);
    uint32_t r = 0;
    for (int i = 1; i <= n; i++) r ^= (uint32_t)luaL_checkinteger(L, i);
    lua_pushinteger(L, (lua_Integer)r);
    return 1;
}
static int l_bit_bnot(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(~(uint32_t)luaL_checkinteger(L, 1)));
    return 1;
}
static int l_bit_lshift(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)((uint32_t)luaL_checkinteger(L, 1) << (uint32_t)luaL_checkinteger(L, 2)));
    return 1;
}
static int l_bit_rshift(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)((uint32_t)luaL_checkinteger(L, 1) >> (uint32_t)luaL_checkinteger(L, 2)));
    return 1;
}
static int l_bit_arshift(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)((int32_t)luaL_checkinteger(L, 1) >> (uint32_t)luaL_checkinteger(L, 2)));
    return 1;
}

/* ── Registration ────────────────────────────────────────────────────────── */

static void reg_fn(lua_State *L, int table_idx, NesLua *lua, const char *name, lua_CFunction fn) {
    lua_pushlightuserdata(L, lua);
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, table_idx, name);
}

static void reg_plain(lua_State *L, int table_idx, const char *name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    lua_setfield(L, table_idx, name);
}

/* Global AND/OR/XOR/BIT - the pre-"bit table" bitwise API older FCEUX
 * scripts use directly (variadic, per the official function list). */
static int l_global_and(lua_State *L) { return l_bit_band(L); }
static int l_global_or(lua_State *L)  { return l_bit_bor(L); }
static int l_global_xor(lua_State *L) { return l_bit_bxor(L); }
static int l_global_bit(lua_State *L) {
    int n = lua_gettop(L);
    uint32_t r = 0;
    for (int i = 1; i <= n; i++) r |= 1u << (uint32_t)luaL_checkinteger(L, i);
    lua_pushinteger(L, (lua_Integer)r);
    return 1;
}

static void register_api(lua_State *L, NesLua *lua) {
    lua_pushlightuserdata(L, lua);
    lua_pushcclosure(L, l_print, 1);
    lua_setglobal(L, "print");

    reg_plain(L, LUA_GLOBALSINDEX, "AND", l_global_and);
    reg_plain(L, LUA_GLOBALSINDEX, "OR",  l_global_or);
    reg_plain(L, LUA_GLOBALSINDEX, "XOR", l_global_xor);
    reg_plain(L, LUA_GLOBALSINDEX, "BIT", l_global_bit);

    lua_newtable(L);
    int emu = lua_gettop(L);
    reg_fn(L, emu, lua, "frameadvance",   l_emu_frameadvance);
    reg_fn(L, emu, lua, "registerbefore", l_emu_registerbefore);
    reg_fn(L, emu, lua, "registerafter",  l_emu_registerafter);
    reg_fn(L, emu, lua, "registerexit",   l_emu_registerexit);
    reg_fn(L, emu, lua, "exit",           l_emu_exit);
    reg_fn(L, emu, lua, "print",          l_print);
    reg_fn(L, emu, lua, "framecount",     l_emu_framecount);
    reg_fn(L, emu, lua, "lagcount",       l_emu_zero);
    reg_fn(L, emu, lua, "exec_count",     l_emu_zero);
    reg_fn(L, emu, lua, "exec_time",      l_emu_zero);
    reg_fn(L, emu, lua, "lagged",         l_emu_false);
    reg_fn(L, emu, lua, "getreadonly",    l_emu_false);
    /* poweron/softreset/speedmode/pause/message/setreadonly/setrenderplanes/
     * setlagflag need host lifecycle plumbing we don't have yet - no-op
     * rather than crash. */
    reg_fn(L, emu, lua, "poweron",         l_emu_noop);
    reg_fn(L, emu, lua, "softreset",       l_emu_noop);
    reg_fn(L, emu, lua, "speedmode",       l_emu_noop);
    reg_fn(L, emu, lua, "pause",           l_emu_noop);
    reg_fn(L, emu, lua, "message",         l_emu_noop);
    reg_fn(L, emu, lua, "setreadonly",     l_emu_noop);
    reg_fn(L, emu, lua, "setrenderplanes", l_emu_noop);
    reg_fn(L, emu, lua, "setlagflag",      l_emu_noop);
    lua_setglobal(L, "emu");

    /* FCEU.* is the pre-rebrand name for the same table - old scripts use
     * it interchangeably with emu.*. */
    lua_getglobal(L, "emu");
    lua_setglobal(L, "FCEU");

    lua_newtable(L);
    int mem = lua_gettop(L);
    reg_fn(L, mem, lua, "readbyte",       l_memory_readbyte);
    reg_fn(L, mem, lua, "readbytesigned", l_memory_readbytesigned);
    reg_fn(L, mem, lua, "writebyte",      l_memory_writebyte);
    reg_fn(L, mem, lua, "readbyterange",  l_memory_readbyterange);
    reg_fn(L, mem, lua, "registerwrite",  l_memory_registerwrite);
    reg_fn(L, mem, lua, "register",       l_memory_registerwrite); /* legacy alias */
    reg_fn(L, mem, lua, "registerread",   l_memory_registerread);
    reg_fn(L, mem, lua, "registerexec",   l_memory_registerexec);
    reg_fn(L, mem, lua, "registerexecute",l_memory_registerexec); /* legacy alias */
    lua_setglobal(L, "memory");

    lua_newtable(L);
    int rom = lua_gettop(L);
    reg_fn(L, rom, lua, "readbyte",       l_rom_readbyte);
    reg_fn(L, rom, lua, "readbytesigned", l_rom_readbytesigned);
    reg_fn(L, rom, lua, "gethash",        l_rom_gethash);
    lua_setglobal(L, "rom");

    lua_newtable(L);
    int joy = lua_gettop(L);
    reg_fn(L, joy, lua, "get",     l_joypad_get);
    reg_fn(L, joy, lua, "read",    l_joypad_get); /* FCEUX alias */
    reg_fn(L, joy, lua, "getdown", l_joypad_getdown);
    reg_fn(L, joy, lua, "set",     l_joypad_set);
    reg_fn(L, joy, lua, "write",   l_joypad_set); /* FCEUX alias */
    lua_setglobal(L, "joypad");

    lua_newtable(L);
    int zap = lua_gettop(L);
    reg_fn(L, zap, lua, "read", l_zapper_read);
    lua_setglobal(L, "zapper");

    lua_newtable(L);
    int inp = lua_gettop(L);
    reg_fn(L, inp, lua, "get", l_input_get);
    lua_setglobal(L, "input");

    lua_newtable(L);
    int gui = lua_gettop(L);
    reg_fn(L, gui, lua, "pixel",     l_gui_pixel);
    reg_fn(L, gui, lua, "drawpixel", l_gui_pixel);
    reg_fn(L, gui, lua, "text",      l_gui_text);
    reg_fn(L, gui, lua, "box",       l_gui_box);
    reg_fn(L, gui, lua, "drawbox",   l_gui_box);
    reg_fn(L, gui, lua, "line",      l_gui_line);
    reg_fn(L, gui, lua, "drawline",  l_gui_line);
    reg_fn(L, gui, lua, "register",  l_gui_register);
    lua_setglobal(L, "gui");

    lua_newtable(L);
    int bit = lua_gettop(L);
    reg_plain(L, bit, "band",   l_bit_band);
    reg_plain(L, bit, "bor",    l_bit_bor);
    reg_plain(L, bit, "bxor",   l_bit_bxor);
    reg_plain(L, bit, "bnot",   l_bit_bnot);
    reg_plain(L, bit, "lshift", l_bit_lshift);
    reg_plain(L, bit, "rshift", l_bit_rshift);
    reg_plain(L, bit, "arshift", l_bit_arshift);
    lua_setglobal(L, "bit");

    /* movie/savestate: no real recording/state support, but scripts that
     * defensively check `if movie.active() then ...` or
     * `if savestate.registerload then ...` should degrade gracefully
     * instead of erroring on a nil global. */
    lua_newtable(L);
    int movie = lua_gettop(L);
    reg_fn(L, movie, lua, "active",           l_emu_false);
    reg_fn(L, movie, lua, "framecount",       l_emu_framecount);
    reg_fn(L, movie, lua, "mode",             l_emu_noop); /* 0 results -> nil */
    reg_fn(L, movie, lua, "name",             l_emu_emptystr);
    reg_fn(L, movie, lua, "getname",          l_emu_emptystr);
    reg_fn(L, movie, lua, "rerecordcounting", l_emu_noop);
    reg_fn(L, movie, lua, "rerecordcount",    l_emu_zero);
    reg_fn(L, movie, lua, "length",           l_emu_zero);
    reg_fn(L, movie, lua, "stop",             l_emu_noop);
    reg_fn(L, movie, lua, "close",            l_emu_noop);
    reg_fn(L, movie, lua, "playbeginning",    l_emu_noop);
    reg_fn(L, movie, lua, "ispoweron",        l_emu_false);
    lua_setglobal(L, "movie");

    lua_newtable(L); /* intentionally empty: no savestate.* implementations */
    lua_setglobal(L, "savestate");
}

/* Real FCEUX scripts routinely require() sibling helper modules that live
 * next to the main script, not next to wherever gemu happens to be run
 * from - so prepend the script's own directory to package.path. */
static void add_script_dir_to_package_path(lua_State *L, const char *script_path) {
    const char *slash = strrchr(script_path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(script_path, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    char dir[900];
    if (slash) {
        size_t len = (size_t)(slash - script_path);
        if (len >= sizeof(dir)) len = sizeof(dir) - 1;
        memcpy(dir, script_path, len);
        dir[len] = '\0';
    } else {
        dir[0] = '.';
        dir[1] = '\0';
    }

    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const char *old_path = lua_tostring(L, -1);
    char new_path[2048];
    snprintf(new_path, sizeof(new_path), "%s/?.lua;%s/?/init.lua;%s",
             dir, dir, old_path ? old_path : "");
    lua_pop(L, 1); /* old path string */
    lua_pushstring(L, new_path);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1); /* package table */
}

/* ── Public API ──────────────────────────────────────────────────────────── */

NesLua *nes_lua_create(const char *path, const NesLuaBus *bus) {
    NesLua *lua = calloc(1, sizeof(*lua));
    if (!lua) return NULL;
    lua->bus        = *bus;
    lua->before_ref = LUA_NOREF;
    lua->after_ref  = LUA_NOREF;
    lua->gui_ref    = LUA_NOREF;
    lua->exit_ref   = LUA_NOREF;

    if (bus->rom_path) {
        FILE *rf = fopen(bus->rom_path, "rb");
        if (rf) {
            fseek(rf, 0, SEEK_END);
            lua->rom_size = ftell(rf);
            rewind(rf);
            if (lua->rom_size > 0) {
                lua->rom_data = malloc((size_t)lua->rom_size);
                if (lua->rom_data && fread(lua->rom_data, 1, (size_t)lua->rom_size, rf) != (size_t)lua->rom_size) {
                    free(lua->rom_data);
                    lua->rom_data = NULL;
                }
            }
            fclose(rf);
        }
    }

    lua->L = luaL_newstate();
    if (!lua->L) { free(lua); return NULL; }
    luaL_openlibs(lua->L);
    add_script_dir_to_package_path(lua->L, path);
    register_api(lua->L, lua);

    /* Run the script body on its own coroutine so emu.frameadvance() can
     * yield control back to the host once per frame. */
    lua->co = lua_newthread(lua->L);
    lua->co_ref = luaL_ref(lua->L, LUA_REGISTRYINDEX); /* keep alive vs. GC */

    if (luaL_loadfile(lua->co, path) != 0) {
        fprintf(stderr, "nes-lua: %s\n", lua_tostring(lua->co, -1));
        lua_close(lua->L);
        free(lua->rom_data);
        free(lua);
        return NULL;
    }

    int status = lua_resume(lua->co, 0);
    if (status != LUA_YIELD) {
        if (status != 0)
            fprintf(stderr, "nes-lua: %s\n", lua_tostring(lua->co, -1));
        lua->finished = true; /* either errored, or a pure callback-style script */
    }

    printf("nes-lua: loaded '%s'\n", path);
    return lua;
}

static void call_ref(NesLua *lua, int ref); /* defined below */

void nes_lua_destroy(NesLua *lua) {
    if (!lua) return;
    if (lua->L) {
        call_ref(lua, lua->exit_ref); /* emu.registerexit() */
        lua_close(lua->L);           /* also frees the coroutine */
    }
    free(lua->rom_data);
    free(lua);
}

void nes_lua_pre_frame(NesLua *lua, uint8_t ctrl_state[2]) {
    if (!lua) return;
    for (int i = 0; i < 2; i++) {
        if (lua->joy_override[i]) {
            ctrl_state[i] = lua->joy_value[i];
            lua->joy_override[i] = false;
        }
    }
    lua->last_ctrl_state[0] = ctrl_state[0];
    lua->last_ctrl_state[1] = ctrl_state[1];
}

static void call_ref(NesLua *lua, int ref) {
    if (ref == LUA_NOREF) return;
    lua_rawgeti(lua->L, LUA_REGISTRYINDEX, ref);
    if (lua_pcall(lua->L, 0, 0, 0) != 0) {
        fprintf(stderr, "nes-lua: callback error: %s\n", lua_tostring(lua->L, -1));
        lua_pop(lua->L, 1);
    }
}

void nes_lua_run_frame(NesLua *lua, uint32_t *pixels_argb, int w, int h) {
    if (!lua) return;
    lua->draw_px = pixels_argb;
    lua->draw_w  = w;
    lua->draw_h  = h;

    call_ref(lua, lua->before_ref);

    if (!lua->finished) {
        int status = lua_resume(lua->co, 0);
        if (status != LUA_YIELD) {
            if (status != 0)
                fprintf(stderr, "nes-lua: %s\n", lua_tostring(lua->co, -1));
            lua->finished = true;
        }
    }

    call_ref(lua, lua->after_ref);
    call_ref(lua, lua->gui_ref); /* gui.register(): draw after this frame's logic ran */

    lua->draw_px = NULL;
}

static void notify(lua_State *L, NesLuaMemHook *hooks, int count, uint16_t addr, uint8_t val) {
    for (int i = 0; i < count; i++) {
        if (hooks[i].addr != addr) continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, hooks[i].ref);
        lua_pushinteger(L, addr);
        lua_pushinteger(L, val);
        if (lua_pcall(L, 2, 0, 0) != 0) {
            fprintf(stderr, "nes-lua: memory hook error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

void nes_lua_notify_read(NesLua *lua, uint16_t addr, uint8_t val) {
    if (!lua || lua->n_read_hooks == 0) return;
    notify(lua->L, lua->read_hooks, lua->n_read_hooks, addr, val);
}

void nes_lua_notify_write(NesLua *lua, uint16_t addr, uint8_t val) {
    if (!lua || lua->n_write_hooks == 0) return;
    notify(lua->L, lua->write_hooks, lua->n_write_hooks, addr, val);
}

void nes_lua_notify_exec(NesLua *lua, uint16_t pc) {
    if (!lua || lua->n_exec_hooks == 0) return;
    notify(lua->L, lua->exec_hooks, lua->n_exec_hooks, pc, 0);
}
