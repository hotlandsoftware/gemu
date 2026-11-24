#pragma once
#include <stdint.h>
#include <stdbool.h>

/* FCEUX-Lua-subset scripting for the NES machine.
 *
 * This does not aim for full FCEUX API parity - no IUP GUI toolkit, no real
 * savestates, no movie recording/playback, no host keyboard/mouse polling
 * (input.*) - it covers the subset real-world scripts use most: memory
 * peek/poke + read/write/exec watchpoints, joypad read/override, a small
 * per-frame drawing overlay (with FCEUX's string/hex color syntax), and the
 * emu.frameadvance()/registerbefore/registerafter callback styles scripts
 * are typically written against. `FCEU.*` is kept as an alias of `emu.*`
 * for older scripts. `movie`/`savestate` exist as inert tables so scripts
 * that defensively check `if movie.active() then ... end` or
 * `if savestate.registerload then ... end` degrade gracefully instead of
 * erroring on a nil global.
 */

typedef struct NesLua NesLua;

/* Host-supplied CPU bus access, used by memory.readbyte/writebyte. Must see
 * the same mapped view the CPU does (PPU/APU/mapper registers included).
 * get_framecount/get_zapper are optional (NULL is fine - callers get 0s). */
typedef struct {
    uint8_t (*mem_read)(uint16_t addr, void *ud);
    void    (*mem_write)(uint16_t addr, uint8_t val, void *ud);
    uint64_t (*get_framecount)(void *ud);
    void    (*get_zapper)(void *ud, int *x, int *y, int *click);
    void    *ud;
    const char *rom_path; /* for rom.readbyte/readbytesigned/gethash (raw file, header included) */
} NesLuaBus;

/* Loads and starts running `path`. The script's top-level chunk executes
 * immediately, up to its first emu.frameadvance() call (or to completion,
 * for pure registerbefore/registerafter-style scripts). Returns NULL and
 * prints a message to stderr on load/syntax error. */
NesLua *nes_lua_create(const char *path, const NesLuaBus *bus);
void    nes_lua_destroy(NesLua *lua);

/* Call once per emulated frame, right after real input is latched into
 * ctrl_state[2] (P1/P2 button bitmasks, NES shift-register bit order) and
 * before the CPU executes that frame. Applies any joypad.set() override
 * queued by the previous nes_lua_run_frame() call. */
void nes_lua_pre_frame(NesLua *lua, uint8_t ctrl_state[2]);

/* Call once per emulated frame, after the PPU has finished rendering it.
 * Resumes the script through registerbefore -> one frameadvance() step ->
 * registerafter/gui.register. gui.* calls made during this resume draw
 * directly into pixels_argb (0x00RRGGBB per pixel, row-major, w*h). */
void nes_lua_run_frame(NesLua *lua, uint32_t *pixels_argb, int w, int h);

/* Bus-access notifications for memory.register{read,write,exec}() hooks.
 * Cheap no-op (one branch) when nothing is registered at addr. Call
 * notify_exec once per instruction, right before it executes, with the PC
 * about to run (not routed through mem_read - that would also fire on
 * plain data reads of the same address). */
void nes_lua_notify_read (NesLua *lua, uint16_t addr, uint8_t val);
void nes_lua_notify_write(NesLua *lua, uint16_t addr, uint8_t val);
void nes_lua_notify_exec (NesLua *lua, uint16_t pc);
