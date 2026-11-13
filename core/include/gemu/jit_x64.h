#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Minimal x86-64 machine-code emitter for gemu's JIT backends.
 *
 * Only what the translators need: 64-bit integer moves and ALU operations
 * against registers, immediates and [base + index*scale + disp32] memory
 * operands, bit tests, conditional jumps with patchable rel32 targets, and
 * calls into C helpers through the host ABI (System V or Win64).
 *
 * Every memory operand is encoded with a 32-bit displacement, which keeps
 * the encoder free of the RBP/R13 and short-displacement special cases.
 *
 * GEMU_JIT_X64 is defined when the host can run the generated code.
 */

#if defined(__x86_64__) || defined(_M_X64)
#define GEMU_JIT_X64 1
#endif

enum {
    X64_RAX = 0, X64_RCX, X64_RDX, X64_RBX, X64_RSP, X64_RBP, X64_RSI, X64_RDI,
    X64_R8, X64_R9, X64_R10, X64_R11, X64_R12, X64_R13, X64_R14, X64_R15,
    X64_NOREG = -1,
};

/* Host ABI integer argument registers. */
#ifdef _WIN32
#define X64_ARG0 X64_RCX
#define X64_ARG1 X64_RDX
#define X64_ARG2 X64_R8
#define X64_ARG3 X64_R9
#else
#define X64_ARG0 X64_RDI
#define X64_ARG1 X64_RSI
#define X64_ARG2 X64_RDX
#define X64_ARG3 X64_RCX
#endif

/* Condition codes (the low nibble of Jcc / SETcc). */
enum {
    X64_CC_O = 0, X64_CC_NO, X64_CC_B, X64_CC_AE, X64_CC_E, X64_CC_NE,
    X64_CC_BE, X64_CC_A, X64_CC_S, X64_CC_NS, X64_CC_P, X64_CC_NP,
    X64_CC_L, X64_CC_GE, X64_CC_LE, X64_CC_G,
};

/* /digit of the 0x81 group and the matching reg,r/m opcodes */
enum { X64_ADD = 0, X64_OR = 1, X64_AND = 4, X64_SUB = 5, X64_XOR = 6, X64_CMP = 7 };

typedef struct {
    uint8_t *base, *p, *end;
    bool overflow;          /* ran out of room: discard and retry after flush */
} X64Buf;

/* Executable memory (RWX).  Returns NULL when the host refuses. */
void *gemu_jit_alloc_exec(size_t size);
void  gemu_jit_free_exec(void *p, size_t size);

static inline void x64_init(X64Buf *b, void *mem, size_t size) {
    b->base = b->p = (uint8_t *)mem;
    b->end = b->base + size;
    b->overflow = false;
}

static inline size_t x64_pos(const X64Buf *b) { return (size_t)(b->p - b->base); }

static inline void x64_b(X64Buf *b, uint8_t v) {
    if (b->p < b->end) *b->p++ = v;
    else b->overflow = true;
}

static inline void x64_d(X64Buf *b, uint32_t v) {
    x64_b(b, (uint8_t)v); x64_b(b, (uint8_t)(v >> 8));
    x64_b(b, (uint8_t)(v >> 16)); x64_b(b, (uint8_t)(v >> 24));
}

static inline void x64_q(X64Buf *b, uint64_t v) {
    x64_d(b, (uint32_t)v); x64_d(b, (uint32_t)(v >> 32));
}

/* REX prefix; emitted when any bit is needed, or when `force` is set (byte
 * operations on SPL/BPL/SIL/DIL). */
static inline void x64_rex(X64Buf *b, int w, int reg, int index, int base,
                           bool force) {
    uint8_t rex = (uint8_t)(0x40 | (w ? 8 : 0) |
                            (reg >= 0 && (reg & 8) ? 4 : 0) |
                            (index >= 0 && (index & 8) ? 2 : 0) |
                            (base >= 0 && (base & 8) ? 1 : 0));
    if (rex != 0x40 || force) x64_b(b, rex);
}

/* ModRM (+SIB) + disp32 for [base + index*scale + disp]. */
static inline void x64_modrm_mem(X64Buf *b, int reg, int base, int index,
                                 int scale, int32_t disp) {
    if (index < 0 && (base & 7) != X64_RSP) {
        x64_b(b, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
    } else {
        unsigned ss = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
        int idx = index < 0 ? X64_RSP : index;   /* RSP = no index */
        x64_b(b, (uint8_t)(0x80 | ((reg & 7) << 3) | 4));
        x64_b(b, (uint8_t)((ss << 6) | ((idx & 7) << 3) | (base & 7)));
    }
    x64_d(b, (uint32_t)disp);
}

/* Generic "opcode reg, [mem]" with a 1- or 2-byte opcode (op2 < 0: none). */
static inline void x64_op_mem(X64Buf *b, int w, int op1, int op2, int reg,
                              int base, int index, int scale, int32_t disp,
                              bool byte_reg) {
    x64_rex(b, w, reg, index, base, byte_reg && reg >= 4);
    x64_b(b, (uint8_t)op1);
    if (op2 >= 0) x64_b(b, (uint8_t)op2);
    x64_modrm_mem(b, reg, base, index, scale, disp);
}

/* Generic "opcode reg, rm" register form. */
static inline void x64_op_rr(X64Buf *b, int w, int op1, int op2, int reg,
                             int rm, bool byte_regs) {
    x64_rex(b, w, reg, -1, rm, byte_regs && (reg >= 4 || rm >= 4));
    x64_b(b, (uint8_t)op1);
    if (op2 >= 0) x64_b(b, (uint8_t)op2);
    x64_b(b, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

/* ── moves ──────────────────────────────────────────────────────────────── */

/* mov r64, [base + index*scale + disp] */
static inline void x64_load64(X64Buf *b, int dst, int base, int index,
                              int scale, int32_t disp) {
    x64_op_mem(b, 1, 0x8B, -1, dst, base, index, scale, disp, false);
}
/* mov r32, [mem] (zero-extends) */
static inline void x64_load32(X64Buf *b, int dst, int base, int index,
                              int scale, int32_t disp) {
    x64_op_mem(b, 0, 0x8B, -1, dst, base, index, scale, disp, false);
}
/* movzx r32, byte [mem] */
static inline void x64_load8zx(X64Buf *b, int dst, int base, int index,
                               int scale, int32_t disp) {
    x64_op_mem(b, 0, 0x0F, 0xB6, dst, base, index, scale, disp, false);
}
/* mov [mem], r64 */
static inline void x64_store64(X64Buf *b, int src, int base, int index,
                               int scale, int32_t disp) {
    x64_op_mem(b, 1, 0x89, -1, src, base, index, scale, disp, false);
}
/* mov [mem], r8 */
static inline void x64_store8(X64Buf *b, int src, int base, int index,
                              int scale, int32_t disp) {
    x64_op_mem(b, 0, 0x88, -1, src, base, index, scale, disp, true);
}
/* mov byte [mem], imm8 */
static inline void x64_store8_imm(X64Buf *b, int base, int index, int scale,
                                  int32_t disp, uint8_t imm) {
    x64_op_mem(b, 0, 0xC6, -1, 0, base, index, scale, disp, false);
    x64_b(b, imm);
}
/* mov qword [mem], simm32 */
static inline void x64_store64_imm32(X64Buf *b, int base, int index, int scale,
                                     int32_t disp, int32_t imm) {
    x64_op_mem(b, 1, 0xC7, -1, 0, base, index, scale, disp, false);
    x64_d(b, (uint32_t)imm);
}

/* mov r64, imm (shortest encoding) */
static inline void x64_mov_imm(X64Buf *b, int dst, uint64_t imm) {
    if (imm <= UINT32_MAX) {                         /* mov r32, imm32 */
        x64_rex(b, 0, -1, -1, dst, false);
        x64_b(b, (uint8_t)(0xB8 + (dst & 7)));
        x64_d(b, (uint32_t)imm);
    } else if ((int64_t)imm == (int32_t)imm) {       /* mov r64, simm32 */
        x64_rex(b, 1, -1, -1, dst, false);
        x64_b(b, 0xC7);
        x64_b(b, (uint8_t)(0xC0 | (dst & 7)));
        x64_d(b, (uint32_t)imm);
    } else {                                         /* movabs */
        x64_rex(b, 1, -1, -1, dst, false);
        x64_b(b, (uint8_t)(0xB8 + (dst & 7)));
        x64_q(b, imm);
    }
}

/* mov dst, src (64-bit) */
static inline void x64_mov_rr(X64Buf *b, int dst, int src) {
    x64_op_rr(b, 1, 0x89, -1, src, dst, false);
}
/* mov dst32, src32 (zero-extends) */
static inline void x64_mov_rr32(X64Buf *b, int dst, int src) {
    x64_op_rr(b, 0, 0x89, -1, src, dst, false);
}

/* lea dst, [base + disp] */
static inline void x64_lea(X64Buf *b, int dst, int base, int32_t disp) {
    x64_op_mem(b, 1, 0x8D, -1, dst, base, -1, 1, disp, false);
}

/* ── ALU ────────────────────────────────────────────────────────────────── */

/* dst = dst <op> src, op one of X64_ADD..X64_CMP (64-bit) */
static inline void x64_alu_rr(X64Buf *b, int op, int dst, int src) {
    x64_op_rr(b, 1, (op << 3) | 1, -1, src, dst, false);
}
/* 32-bit form */
static inline void x64_alu_rr32(X64Buf *b, int op, int dst, int src) {
    x64_op_rr(b, 0, (op << 3) | 1, -1, src, dst, false);
}
/* dst = dst <op> simm32 (64-bit) */
static inline void x64_alu_ri(X64Buf *b, int op, int dst, int32_t imm) {
    x64_rex(b, 1, -1, -1, dst, false);
    if (imm >= -128 && imm <= 127) {
        x64_b(b, 0x83); x64_b(b, (uint8_t)(0xC0 | (op << 3) | (dst & 7)));
        x64_b(b, (uint8_t)imm);
    } else {
        x64_b(b, 0x81); x64_b(b, (uint8_t)(0xC0 | (op << 3) | (dst & 7)));
        x64_d(b, (uint32_t)imm);
    }
}
/* 32-bit form */
static inline void x64_alu_ri32(X64Buf *b, int op, int dst, int32_t imm) {
    x64_rex(b, 0, -1, -1, dst, false);
    x64_b(b, 0x81); x64_b(b, (uint8_t)(0xC0 | (op << 3) | (dst & 7)));
    x64_d(b, (uint32_t)imm);
}
/* qword [mem] <op>= simm32 */
static inline void x64_alu_mi(X64Buf *b, int op, int base, int32_t disp,
                              int32_t imm) {
    x64_op_mem(b, 1, 0x81, -1, op, base, -1, 1, disp, false);
    x64_d(b, (uint32_t)imm);
}
/* qword [mem] <op>= reg */
static inline void x64_alu_mr(X64Buf *b, int op, int base, int32_t disp,
                              int src) {
    x64_op_mem(b, 1, (op << 3) | 1, -1, src, base, -1, 1, disp, false);
}
/* reg <op>= qword [mem] */
static inline void x64_alu_rm(X64Buf *b, int op, int dst, int base,
                              int32_t disp) {
    x64_op_mem(b, 1, (op << 3) | 3, -1, dst, base, -1, 1, disp, false);
}
/* byte [mem] cmp imm8 */
static inline void x64_cmp8_mi(X64Buf *b, int base, int32_t disp, uint8_t imm) {
    x64_op_mem(b, 0, 0x80, -1, 7, base, -1, 1, disp, false);
    x64_b(b, imm);
}

static inline void x64_not(X64Buf *b, int r) {
    x64_op_rr(b, 1, 0xF7, -1, 2, r, false);
}
static inline void x64_neg(X64Buf *b, int r) {
    x64_op_rr(b, 1, 0xF7, -1, 3, r, false);
}
/* shl r64, imm8 */
static inline void x64_shl_ri(X64Buf *b, int r, uint8_t n) {
    x64_op_rr(b, 1, 0xC1, -1, 4, r, false);
    x64_b(b, n);
}
/* imul dst, src, simm32 */
static inline void x64_imul_rri(X64Buf *b, int dst, int src, int32_t imm) {
    x64_op_rr(b, 1, 0x69, -1, dst, src, false);
    x64_d(b, (uint32_t)imm);
}
/* test r32, r32 */
static inline void x64_test_rr32(X64Buf *b, int a, int c) {
    x64_op_rr(b, 0, 0x85, -1, c, a, false);
}
/* shr r64, imm8 */
static inline void x64_shr_ri(X64Buf *b, int r, uint8_t n) {
    x64_op_rr(b, 1, 0xC1, -1, 5, r, false);
    x64_b(b, n);
}
/* reg32 <op>= dword [base + index*scale + disp] */
static inline void x64_alu_rm32(X64Buf *b, int op, int dst, int base,
                                int index, int scale, int32_t disp) {
    x64_op_mem(b, 0, (op << 3) | 3, -1, dst, base, index, scale, disp, false);
}
/* reg64 <op>= qword [base + index*scale + disp] */
static inline void x64_alu_rmx(X64Buf *b, int op, int dst, int base,
                               int index, int scale, int32_t disp) {
    x64_op_mem(b, 1, (op << 3) | 3, -1, dst, base, index, scale, disp, false);
}
/* movzx r32, word [mem] */
static inline void x64_load16zx(X64Buf *b, int dst, int base, int index,
                                int scale, int32_t disp) {
    x64_op_mem(b, 0, 0x0F, 0xB7, dst, base, index, scale, disp, false);
}
/* mov word [mem], r16 */
static inline void x64_store16(X64Buf *b, int src, int base, int index,
                               int scale, int32_t disp) {
    x64_b(b, 0x66);
    x64_op_mem(b, 0, 0x89, -1, src, base, index, scale, disp, false);
}
/* mov dword [mem], r32 */
static inline void x64_store32(X64Buf *b, int src, int base, int index,
                               int scale, int32_t disp) {
    x64_op_mem(b, 0, 0x89, -1, src, base, index, scale, disp, false);
}
/* inc dword [mem] */
static inline void x64_inc32_m(X64Buf *b, int base, int index, int scale,
                               int32_t disp) {
    x64_op_mem(b, 0, 0xFF, -1, 0, base, index, scale, disp, false);
}
/* lea dst, [base + index*scale + disp] */
static inline void x64_lea_x(X64Buf *b, int dst, int base, int index,
                             int scale, int32_t disp) {
    x64_op_mem(b, 1, 0x8D, -1, dst, base, index, scale, disp, false);
}

/* test r8, r8 */
static inline void x64_test_rr8(X64Buf *b, int a, int c) {
    x64_op_rr(b, 0, 0x84, -1, c, a, true);
}
/* dec qword [mem] */
static inline void x64_dec_m(X64Buf *b, int base, int32_t disp) {
    x64_op_mem(b, 1, 0xFF, -1, 1, base, -1, 1, disp, false);
}

/* bt / bts / btr qword [mem], imm8: 4 = bt, 5 = bts, 6 = btr */
static inline void x64_bt_mi(X64Buf *b, int which, int base, int32_t disp,
                             uint8_t bit) {
    x64_op_mem(b, 1, 0x0F, 0xBA, which, base, -1, 1, disp, false);
    x64_b(b, bit);
}

/* setcc r8 */
static inline void x64_setcc(X64Buf *b, int cc, int r) {
    x64_rex(b, 0, -1, -1, r, r >= 4);
    x64_b(b, 0x0F); x64_b(b, (uint8_t)(0x90 | cc));
    x64_b(b, (uint8_t)(0xC0 | (r & 7)));
}

/* Extensions (register to register). */
static inline void x64_movzx8(X64Buf *b, int dst, int src) {
    x64_op_rr(b, 0, 0x0F, 0xB6, dst, src, true);
}
static inline void x64_movzx16(X64Buf *b, int dst, int src) {
    x64_op_rr(b, 0, 0x0F, 0xB7, dst, src, false);
}
static inline void x64_movsx8(X64Buf *b, int dst, int src) {
    x64_op_rr(b, 1, 0x0F, 0xBE, dst, src, true);
}
static inline void x64_movsx16(X64Buf *b, int dst, int src) {
    x64_op_rr(b, 1, 0x0F, 0xBF, dst, src, false);
}
static inline void x64_movsxd(X64Buf *b, int dst, int src) {
    x64_op_rr(b, 1, 0x63, -1, dst, src, false);
}

/* ── control flow ───────────────────────────────────────────────────────── */

/* jcc rel32; returns the rel32 field's offset for x64_patch() */
static inline size_t x64_jcc(X64Buf *b, int cc) {
    x64_b(b, 0x0F); x64_b(b, (uint8_t)(0x80 | cc));
    size_t at = x64_pos(b);
    x64_d(b, 0);
    return at;
}
static inline size_t x64_jmp(X64Buf *b) {
    x64_b(b, 0xE9);
    size_t at = x64_pos(b);
    x64_d(b, 0);
    return at;
}
/* Point the rel32 at `at` to `target` (both buffer offsets). */
static inline void x64_patch(X64Buf *b, size_t at, size_t target) {
    if (at + 4 > (size_t)(b->end - b->base)) return;   /* overflowed */
    int32_t rel = (int32_t)((int64_t)target - (int64_t)(at + 4));
    memcpy(b->base + at, &rel, 4);
}
/* Patch to the current position. */
static inline void x64_here(X64Buf *b, size_t at) { x64_patch(b, at, x64_pos(b)); }

/* call an absolute address through RAX */
static inline void x64_call_abs(X64Buf *b, const void *fn) {
    x64_mov_imm(b, X64_RAX, (uint64_t)(uintptr_t)fn);
    x64_b(b, 0xFF); x64_b(b, 0xD0);
}

static inline void x64_push(X64Buf *b, int r) {
    x64_rex(b, 0, -1, -1, r, false);
    x64_b(b, (uint8_t)(0x50 + (r & 7)));
}
static inline void x64_pop(X64Buf *b, int r) {
    x64_rex(b, 0, -1, -1, r, false);
    x64_b(b, (uint8_t)(0x58 + (r & 7)));
}
static inline void x64_ret(X64Buf *b) { x64_b(b, 0xC3); }
