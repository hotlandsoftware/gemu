#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Host-neutral code generation for gemu's JIT translators.
 *
 * A guest translator (e.g. cpu/ia64/jit/merced_jit.c) describes each
 * translated block with the operations below; a backend
 * (jit/jit_backend_<host>.c) turns them into host machine code.  The
 * translator never sees a host instruction or register, and the backend
 * never sees guest semantics - so a new host is one new backend, and a new
 * guest JIT reuses the existing backends.
 *
 * Model.  Each generated function is
 *
 *     uintptr_t fn(void *env)
 *
 * where env is the guest CPU state, which the jit_*_env() operations
 * address by byte offset.  Values live in a few virtual registers:
 * JIT_T0..JIT_T5 are scratch and do not survive jit_call(); JIT_S0 and
 * JIT_S1 do.  Every register is 64 bits; loads zero-extend, and an
 * operation's `bits` argument (32 or 64) says how much of its operands it
 * reads.  Branch targets are labels, bound once each.
 *
 * Code is emitted into a buffer the backend owns.  When it fills, jit_end()
 * returns NULL and the translator calls jit_backend_reset(), which discards
 * every function emitted so far.
 */

typedef struct JitBackend JitBackend;
typedef uintptr_t (*JitFn)(void *env);
typedef int JitLabel;

typedef enum {
    JIT_T0, JIT_T1, JIT_T2, JIT_T3, JIT_T4, JIT_T5,  /* clobbered by calls */
    JIT_S0, JIT_S1,                                  /* preserved */
    JIT_NREGS
} JitReg;

typedef enum {
    JIT_EQ, JIT_NE,
    JIT_LT, JIT_GE, JIT_LE, JIT_GT,          /* signed */
    JIT_LTU, JIT_GEU, JIT_LEU, JIT_GTU,      /* unsigned */
} JitCond;

typedef enum {
    JIT_ADD, JIT_SUB, JIT_AND, JIT_OR, JIT_XOR,
    JIT_ANDN,                                /* a & ~b */
} JitOp;

typedef enum {
    JIT_ZX8, JIT_ZX16, JIT_ZX32,             /* zero-extend the low bits */
    JIT_SX8, JIT_SX16, JIT_SX32,             /* sign-extend */
} JitExt;

/* ── backend lifetime ─────────────────────────────────────────────────── */

/* A code buffer of `bytes`, or NULL when this host has no backend (or the
 * host refuses executable memory).  jit_backend_name() names the host. */
JitBackend *jit_backend_new(size_t bytes);
void        jit_backend_free(JitBackend *b);
void        jit_backend_reset(JitBackend *b);
size_t      jit_backend_used(const JitBackend *b);   /* bytes emitted */
const char *jit_backend_name(void);

/* ── functions and control flow ───────────────────────────────────────── */

void     jit_begin(JitBackend *b);
JitFn    jit_end(JitBackend *b);           /* NULL: the buffer is full */

JitLabel jit_label(JitBackend *b);
void     jit_bind(JitBackend *b, JitLabel l);
void     jit_jmp(JitBackend *b, JitLabel l);
/* if (a <cond> c) goto l */
void     jit_br(JitBackend *b, JitCond cond, JitReg a, JitReg c,
                unsigned bits, JitLabel l);
void     jit_bri(JitBackend *b, JitCond cond, JitReg a, int64_t imm,
                 unsigned bits, JitLabel l);
/* if (bit `bit` of a is `set`) goto l */
void     jit_brbit(JitBackend *b, JitReg a, unsigned bit, bool set,
                   JitLabel l);
/* The same with the bit number (0-63) in a register. */
void     jit_brbitr(JitBackend *b, JitReg a, JitReg bit, bool set,
                    JitLabel l);

/* Return v from the generated function. */
void     jit_ret(JitBackend *b, JitReg v);
void     jit_reti(JitBackend *b, uintptr_t v);

/* T0 = fn(env, a1, a2) */
void     jit_call(JitBackend *b, const void *fn, uintptr_t a1, uintptr_t a2);

/* ── values ───────────────────────────────────────────────────────────── */

void jit_movi(JitBackend *b, JitReg d, uint64_t imm);
void jit_mov(JitBackend *b, JitReg d, JitReg s);
void jit_op(JitBackend *b, JitOp op, JitReg d, JitReg a, JitReg c);
void jit_opi(JitBackend *b, JitOp op, JitReg d, JitReg a, int64_t imm);
void jit_shli(JitBackend *b, JitReg d, JitReg a, unsigned n);
void jit_shri(JitBackend *b, JitReg d, JitReg a, unsigned n);
void jit_muli(JitBackend *b, JitReg d, JitReg a, int32_t imm);
void jit_not(JitBackend *b, JitReg d, JitReg a);
void jit_neg(JitBackend *b, JitReg d, JitReg a);
void jit_ext(JitBackend *b, JitExt ext, JitReg d, JitReg s);
/* d |= 1 << bit, d &= ~(1 << bit): bit number (0-63) in a register */
void jit_bitset(JitBackend *b, JitReg d, JitReg bit);
void jit_bitclr(JitBackend *b, JitReg d, JitReg bit);
/* d = (a <cond> c) ? 1 : 0 */
void jit_setcond(JitBackend *b, JitCond cond, JitReg d, JitReg a, JitReg c,
                 unsigned bits);

/* ── memory ───────────────────────────────────────────────────────────── */

/* env + off (+ idx * scale); sizes 1, 2, 4, 8 */
void jit_ld_env(JitBackend *b, JitReg d, int32_t off, unsigned size);
void jit_st_env(JitBackend *b, int32_t off, JitReg s, unsigned size);
void jit_st_envi(JitBackend *b, int32_t off, int64_t imm, unsigned size);
void jit_ld_envx(JitBackend *b, JitReg d, int32_t off, JitReg idx,
                 unsigned scale, unsigned size);
void jit_st_envx(JitBackend *b, int32_t off, JitReg idx, unsigned scale,
                 JitReg s, unsigned size);
void jit_st_envxi(JitBackend *b, int32_t off, JitReg idx, unsigned scale,
                  int64_t imm, unsigned size);
/* the 64-bit field at env + off += s / += imm */
void jit_add_env(JitBackend *b, int32_t off, JitReg s);
void jit_addi_env(JitBackend *b, int32_t off, int32_t imm);

/* base + disp, any host address */
void jit_ld(JitBackend *b, JitReg d, JitReg base, int32_t disp, unsigned size);
void jit_st(JitBackend *b, JitReg base, int32_t disp, JitReg s, unsigned size);
