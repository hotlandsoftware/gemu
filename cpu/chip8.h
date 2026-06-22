#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gemu/tcg.h"
#include "gemu/monitor.h"
#include "gemu/vnc.h"
#include "gemu/display.h"

/* ── Display ──────────────────────────────────────────────────────────────── */
#define CHIP8_DISPLAY_W  64
#define CHIP8_DISPLAY_H  32

/* ── Memory ───────────────────────────────────────────────────────────────── */
#define CHIP8_MEM_SIZE   4096
#define CHIP8_ROM_BASE   0x200
#define CHIP8_FONT_BASE  0x050
#define CHIP8_FONT_BYTES 80

/* ── CPU ──────────────────────────────────────────────────────────────────── */
#define CHIP8_NUM_REGS    16
#define CHIP8_STACK_DEPTH 16
#define CHIP8_NUM_KEYS    16
#define CHIP8_TIMER_HZ        60
#define CHIP8_DEFAULT_CPF     10   /* classic .ch8 ROMs: ~600 Hz at 60 fps */
#define CHIP8_DEFAULT_CPF_8O  30   /* Octo .8o ROMs need more cycles/frame */

/* Legacy alias */
#define CHIP8_DEFAULT_HZ  CHIP8_DEFAULT_CPF

/* ── Decoded instruction types ────────────────────────────────────────────── */
typedef enum {
    C8_CLS, C8_RET,
    C8_JP_NNN, C8_CALL_NNN, C8_JP_V0_NNN,
    C8_SE_VX_KK, C8_SNE_VX_KK,
    C8_SE_VX_VY, C8_SNE_VX_VY,
    C8_SKP_VX,   C8_SKNP_VX,
    C8_LD_VX_KK, C8_ADD_VX_KK,
    C8_LD_VX_VY,
    C8_OR_VX_VY, C8_AND_VX_VY, C8_XOR_VX_VY,
    C8_ADD_VX_VY, C8_SUB_VX_VY, C8_SUBN_VX_VY,
    C8_SHR_VX, C8_SHL_VX,
    C8_LD_I_NNN, C8_ADD_I_VX,
    C8_LD_F_VX,  C8_LD_B_VX,
    C8_LD_I_VX,  C8_LD_VX_I,
    C8_DRW_VX_VY_N,
    C8_LD_VX_DT, C8_LD_DT_VX, C8_LD_ST_VX,
    C8_RND_VX_KK, C8_LD_VX_K,
    C8_INVALID,
} Chip8InsnType;

typedef struct {
    Chip8InsnType type;
    uint8_t  x, y, n;
    uint8_t  kk;
    uint16_t nnn;
} Chip8Insn;

static inline bool chip8_insn_is_terminal(Chip8InsnType t) {
    switch (t) {
    case C8_RET:       case C8_JP_NNN:    case C8_CALL_NNN:
    case C8_JP_V0_NNN: case C8_SE_VX_KK:  case C8_SNE_VX_KK:
    case C8_SE_VX_VY:  case C8_SNE_VX_VY: case C8_SKP_VX:
    case C8_SKNP_VX:   case C8_LD_VX_K:
        return true;
    default:
        return false;
    }
}

/* ── Machine type ─────────────────────────────────────────────────────────── */
typedef enum {
    CHIP8_MACHINE_GENERIC,
} Chip8MachineType;

/* ── Machine state ────────────────────────────────────────────────────────── */
typedef struct Chip8State {
    uint8_t  V[CHIP8_NUM_REGS];
    uint16_t I;
    uint16_t PC;
    uint8_t  SP;
    uint16_t stack[CHIP8_STACK_DEPTH];
    uint8_t  delay;
    uint8_t  sound;
    uint8_t  mem[CHIP8_MEM_SIZE];
    uint8_t  vram[CHIP8_DISPLAY_W * CHIP8_DISPLAY_H];
    bool     draw_flag;
    uint8_t  keys[CHIP8_NUM_KEYS];
    uint8_t  keys_prev[CHIP8_NUM_KEYS];
    bool     wait_key;
    uint8_t  wait_reg;
    GemuTbCache  tb_cache;
    GemuMonitor *monitor;
    GemuVncServer *vnc;
    uint64_t     insn_count;
    uint64_t     tb_hits;
    uint64_t     tb_misses;
} Chip8State;

typedef struct Chip8Config {
    const char      *rom_path;
    uint32_t         mem_size;
    int              cpu_hz;       /* cycles per frame (not Hz) */
    GemuDisplayType  display_type;
    int              display_scale;
    bool             quirk_shift;
    bool             quirk_jump;
    const char      *vnc_addr;
    Chip8MachineType machine;
    uint32_t         fg_argb;     /* on-pixel color  (default: Octo yellow) */
    uint32_t         bg_argb;     /* off-pixel color (default: black) */
} Chip8Config;


/* ── CPU/machine API ──────────────────────────────────────────────────────── */
/* cpu.c */
Chip8Insn chip8_decode(uint16_t opcode);
GemuTb   *chip8_translate_block(Chip8State *s, uint16_t pc);
void      chip8_execute_tb(Chip8State *s, GemuTb *tb);
void      chip8_exec_single(Chip8State *s); /* monitor step */

/* machine.c */
Chip8State *chip8_machine_create(const Chip8Config *cfg);
void        chip8_machine_reset(Chip8State *s, const Chip8Config *cfg);
void        chip8_machine_run(Chip8State *s, const Chip8Config *cfg);
void        chip8_machine_destroy(Chip8State *s);
