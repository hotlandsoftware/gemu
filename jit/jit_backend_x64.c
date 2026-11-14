/*
 * x86-64 backend for gemu's JIT translators (see jit_backend.h).
 *
 * Register assignment (both System V and Win64):
 *   env  RBX  (callee-saved, loaded from the first argument)
 *   T0 RAX  T1 RCX  T2 RDX  T3 R8  T4 R9  T5 R10  (caller-saved)
 *   S0 R13  S1 R14                                 (callee-saved)
 *   R11 is the backend's own scratch register.
 *
 * Frame: the prologue pushes RBX, R12, R13 and R14 and reserves 40 bytes,
 * which keeps RSP 16-byte aligned at calls and provides the Win64 shadow
 * space.  Every jit_ret()/jit_reti() jumps to a shared epilogue.
 */
#include "jit_backend.h"
#include "jit_x64.h"
#include "jit_mem.h"

#include <stdlib.h>

#ifdef GEMU_JIT_X64

#define SCR X64_R11
#define ENV X64_RBX

static const int host_reg[JIT_NREGS] = {
    [JIT_T0] = X64_RAX, [JIT_T1] = X64_RCX, [JIT_T2] = X64_RDX,
    [JIT_T3] = X64_R8,  [JIT_T4] = X64_R9,  [JIT_T5] = X64_R10,
    [JIT_S0] = X64_R13, [JIT_S1] = X64_R14,
};
#define R(r) host_reg[r]

typedef struct { JitLabel label; size_t at; } Fixup;

struct JitBackend {
    uint8_t *mem;
    size_t size;
    X64Buf buf;
    uint8_t *entry;
    JitLabel epilogue;
    size_t *label_pos;              /* SIZE_MAX until bound */
    unsigned n_labels, cap_labels;
    Fixup *fixups;
    unsigned n_fixups, cap_fixups;
    bool oom;
};

const char *jit_backend_name(void) { return "x86-64"; }

JitBackend *jit_backend_new(size_t bytes) {
    JitBackend *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->mem = gemu_jit_alloc_exec(bytes);
    if (!b->mem) { free(b); return NULL; }
    b->size = bytes;
    x64_init(&b->buf, b->mem, bytes);
    return b;
}

void jit_backend_free(JitBackend *b) {
    if (!b) return;
    gemu_jit_free_exec(b->mem, b->size);
    free(b->label_pos);
    free(b->fixups);
    free(b);
}

void jit_backend_reset(JitBackend *b) {
    x64_init(&b->buf, b->mem, b->size);
}

size_t jit_backend_used(const JitBackend *b) {
    return x64_pos(&b->buf);
}

/* ── labels ───────────────────────────────────────────────────────────── */

JitLabel jit_label(JitBackend *b) {
    if (b->n_labels == b->cap_labels) {
        unsigned cap = b->cap_labels ? b->cap_labels * 2 : 64;
        size_t *p = realloc(b->label_pos, cap * sizeof(*p));
        if (!p) { b->oom = true; return 0; }
        b->label_pos = p;
        b->cap_labels = cap;
    }
    b->label_pos[b->n_labels] = SIZE_MAX;
    return (JitLabel)b->n_labels++;
}

void jit_bind(JitBackend *b, JitLabel l) {
    if (!b->oom) b->label_pos[l] = x64_pos(&b->buf);
}

static void fixup(JitBackend *b, JitLabel l, size_t at) {
    if (b->n_fixups == b->cap_fixups) {
        unsigned cap = b->cap_fixups ? b->cap_fixups * 2 : 128;
        Fixup *p = realloc(b->fixups, cap * sizeof(*p));
        if (!p) { b->oom = true; return; }
        b->fixups = p;
        b->cap_fixups = cap;
    }
    b->fixups[b->n_fixups++] = (Fixup){ l, at };
}

void jit_jmp(JitBackend *b, JitLabel l) {
    fixup(b, l, x64_jmp(&b->buf));
}

static int cc_of(JitCond c) {
    switch (c) {
    case JIT_EQ:  return X64_CC_E;
    case JIT_NE:  return X64_CC_NE;
    case JIT_LT:  return X64_CC_L;
    case JIT_GE:  return X64_CC_GE;
    case JIT_LE:  return X64_CC_LE;
    case JIT_GT:  return X64_CC_G;
    case JIT_LTU: return X64_CC_B;
    case JIT_GEU: return X64_CC_AE;
    case JIT_LEU: return X64_CC_BE;
    default:      return X64_CC_A;     /* JIT_GTU */
    }
}

static void cmp_rr(JitBackend *b, JitReg a, JitReg c, unsigned bits) {
    if (bits == 32) x64_alu_rr32(&b->buf, X64_CMP, R(a), R(c));
    else            x64_alu_rr(&b->buf, X64_CMP, R(a), R(c));
}

static void cmp_ri(JitBackend *b, JitReg a, int64_t imm, unsigned bits) {
    if (bits == 32) {
        x64_alu_ri32(&b->buf, X64_CMP, R(a), (int32_t)imm);
    } else if (imm == (int32_t)imm) {
        x64_alu_ri(&b->buf, X64_CMP, R(a), (int32_t)imm);
    } else {
        x64_mov_imm(&b->buf, SCR, (uint64_t)imm);
        x64_alu_rr(&b->buf, X64_CMP, R(a), SCR);
    }
}

void jit_br(JitBackend *b, JitCond cond, JitReg a, JitReg c, unsigned bits,
            JitLabel l) {
    cmp_rr(b, a, c, bits);
    fixup(b, l, x64_jcc(&b->buf, cc_of(cond)));
}

void jit_bri(JitBackend *b, JitCond cond, JitReg a, int64_t imm,
             unsigned bits, JitLabel l) {
    cmp_ri(b, a, imm, bits);
    fixup(b, l, x64_jcc(&b->buf, cc_of(cond)));
}

void jit_brbit(JitBackend *b, JitReg a, unsigned bit, bool set, JitLabel l) {
    x64_bt_ri(&b->buf, R(a), (uint8_t)bit);
    fixup(b, l, x64_jcc(&b->buf, set ? X64_CC_B : X64_CC_AE));
}

/* bt / bts / btr r64, r64: 0F A3 / 0F AB / 0F B3, the index in reg */
void jit_brbitr(JitBackend *b, JitReg a, JitReg bit, bool set, JitLabel l) {
    x64_op_rr(&b->buf, 1, 0x0F, 0xA3, R(bit), R(a), false);
    fixup(b, l, x64_jcc(&b->buf, set ? X64_CC_B : X64_CC_AE));
}

void jit_bitset(JitBackend *b, JitReg d, JitReg bit) {
    x64_op_rr(&b->buf, 1, 0x0F, 0xAB, R(bit), R(d), false);
}

void jit_bitclr(JitBackend *b, JitReg d, JitReg bit) {
    x64_op_rr(&b->buf, 1, 0x0F, 0xB3, R(bit), R(d), false);
}

/* ── functions ────────────────────────────────────────────────────────── */

void jit_begin(JitBackend *b) {
    X64Buf *x = &b->buf;
    b->n_labels = 0;
    b->n_fixups = 0;
    b->oom = false;
    while (x64_pos(x) & 15) x64_b(x, 0x90);
    b->entry = x->p;
    x64_push(x, X64_RBX);
    x64_push(x, X64_R12);
    x64_push(x, X64_R13);
    x64_push(x, X64_R14);
    x64_alu_ri(x, X64_SUB, X64_RSP, 40);
    x64_mov_rr(x, ENV, X64_ARG0);
    b->epilogue = jit_label(b);
}

JitFn jit_end(JitBackend *b) {
    X64Buf *x = &b->buf;
    jit_bind(b, b->epilogue);
    x64_alu_ri(x, X64_ADD, X64_RSP, 40);
    x64_pop(x, X64_R14);
    x64_pop(x, X64_R13);
    x64_pop(x, X64_R12);
    x64_pop(x, X64_RBX);
    x64_ret(x);
    if (x->overflow || b->oom)
        return NULL;
    for (unsigned i = 0; i < b->n_fixups; i++) {
        size_t target = b->label_pos[b->fixups[i].label];
        if (target == SIZE_MAX) return NULL;          /* unbound label */
        x64_patch(x, b->fixups[i].at, target);
    }
    return (JitFn)(void *)b->entry;
}

void jit_ret(JitBackend *b, JitReg v) {
    if (R(v) != X64_RAX) x64_mov_rr(&b->buf, X64_RAX, R(v));
    jit_jmp(b, b->epilogue);
}

void jit_reti(JitBackend *b, uintptr_t v) {
    x64_mov_imm(&b->buf, X64_RAX, v);
    jit_jmp(b, b->epilogue);
}

void jit_call(JitBackend *b, const void *fn, uintptr_t a1, uintptr_t a2) {
    X64Buf *x = &b->buf;
    x64_mov_rr(x, X64_ARG0, ENV);
    x64_mov_imm(x, X64_ARG1, a1);
    x64_mov_imm(x, X64_ARG2, a2);
    x64_call_abs(x, fn);
}

/* ── values ───────────────────────────────────────────────────────────── */

void jit_movi(JitBackend *b, JitReg d, uint64_t imm) {
    x64_mov_imm(&b->buf, R(d), imm);
}

void jit_mov(JitBackend *b, JitReg d, JitReg s) {
    if (d != s) x64_mov_rr(&b->buf, R(d), R(s));
}

static int alu_of(JitOp op) {
    switch (op) {
    case JIT_ADD: return X64_ADD;
    case JIT_SUB: return X64_SUB;
    case JIT_OR:  return X64_OR;
    case JIT_XOR: return X64_XOR;
    default:      return X64_AND;     /* AND, ANDN */
    }
}

void jit_op(JitBackend *b, JitOp op, JitReg d, JitReg a, JitReg c) {
    X64Buf *x = &b->buf;
    int hd = R(d), ha = R(a), hc = R(c);
    if (op == JIT_ANDN) {
        x64_mov_rr(x, SCR, hc);
        x64_not(x, SCR);
        hc = SCR;
    } else if (d == c && d != a) {
        if (op != JIT_SUB) {            /* commutative: d = d op a */
            x64_alu_rr(x, alu_of(op), hd, ha);
            return;
        }
        x64_mov_rr(x, SCR, hc);
        hc = SCR;
    }
    if (hd != ha) x64_mov_rr(x, hd, ha);
    x64_alu_rr(x, alu_of(op), hd, hc);
}

void jit_opi(JitBackend *b, JitOp op, JitReg d, JitReg a, int64_t imm) {
    X64Buf *x = &b->buf;
    if (op == JIT_ANDN) imm = ~imm;
    if (d != a) x64_mov_rr(x, R(d), R(a));
    if (imm == (int32_t)imm) {
        x64_alu_ri(x, alu_of(op), R(d), (int32_t)imm);
    } else {
        x64_mov_imm(x, SCR, (uint64_t)imm);
        x64_alu_rr(x, alu_of(op), R(d), SCR);
    }
}

void jit_shli(JitBackend *b, JitReg d, JitReg a, unsigned n) {
    jit_mov(b, d, a);
    if (n) x64_shl_ri(&b->buf, R(d), (uint8_t)n);
}

void jit_shri(JitBackend *b, JitReg d, JitReg a, unsigned n) {
    jit_mov(b, d, a);
    if (n) x64_shr_ri(&b->buf, R(d), (uint8_t)n);
}

void jit_muli(JitBackend *b, JitReg d, JitReg a, int32_t imm) {
    x64_imul_rri(&b->buf, R(d), R(a), imm);
}

void jit_not(JitBackend *b, JitReg d, JitReg a) {
    jit_mov(b, d, a);
    x64_not(&b->buf, R(d));
}

void jit_neg(JitBackend *b, JitReg d, JitReg a) {
    jit_mov(b, d, a);
    x64_neg(&b->buf, R(d));
}

void jit_ext(JitBackend *b, JitExt ext, JitReg d, JitReg s) {
    X64Buf *x = &b->buf;
    switch (ext) {
    case JIT_ZX8:  x64_movzx8(x, R(d), R(s)); break;
    case JIT_ZX16: x64_movzx16(x, R(d), R(s)); break;
    case JIT_ZX32: x64_mov_rr32(x, R(d), R(s)); break;
    case JIT_SX8:  x64_movsx8(x, R(d), R(s)); break;
    case JIT_SX16: x64_movsx16(x, R(d), R(s)); break;
    default:       x64_movsxd(x, R(d), R(s)); break;
    }
}

void jit_setcond(JitBackend *b, JitCond cond, JitReg d, JitReg a, JitReg c,
                 unsigned bits) {
    cmp_rr(b, a, c, bits);
    x64_setcc(&b->buf, cc_of(cond), SCR);
    x64_movzx8(&b->buf, R(d), SCR);
}

/* ── memory ───────────────────────────────────────────────────────────── */

static void load(X64Buf *x, int d, int base, int index, unsigned scale,
                 int32_t disp, unsigned size) {
    switch (size) {
    case 1:  x64_load8zx(x, d, base, index, (int)scale, disp); break;
    case 2:  x64_load16zx(x, d, base, index, (int)scale, disp); break;
    case 4:  x64_load32(x, d, base, index, (int)scale, disp); break;
    default: x64_load64(x, d, base, index, (int)scale, disp); break;
    }
}

static void store(X64Buf *x, int base, int index, unsigned scale,
                  int32_t disp, int s, unsigned size) {
    switch (size) {
    case 1:  x64_store8(x, s, base, index, (int)scale, disp); break;
    case 2:  x64_store16(x, s, base, index, (int)scale, disp); break;
    case 4:  x64_store32(x, s, base, index, (int)scale, disp); break;
    default: x64_store64(x, s, base, index, (int)scale, disp); break;
    }
}

static void store_imm(X64Buf *x, int base, int index, unsigned scale,
                      int32_t disp, int64_t imm, unsigned size) {
    if (size == 1) {
        x64_store8_imm(x, base, index, (int)scale, disp, (uint8_t)imm);
    } else if (size == 4) {
        x64_store32_imm(x, base, index, (int)scale, disp, (int32_t)imm);
    } else if (size == 8 && imm == (int32_t)imm) {
        x64_store64_imm32(x, base, index, (int)scale, disp, (int32_t)imm);
    } else {
        x64_mov_imm(x, SCR, (uint64_t)imm);
        store(x, base, index, scale, disp, SCR, size);
    }
}

void jit_ld_env(JitBackend *b, JitReg d, int32_t off, unsigned size) {
    load(&b->buf, R(d), ENV, X64_NOREG, 1, off, size);
}

void jit_st_env(JitBackend *b, int32_t off, JitReg s, unsigned size) {
    store(&b->buf, ENV, X64_NOREG, 1, off, R(s), size);
}

void jit_st_envi(JitBackend *b, int32_t off, int64_t imm, unsigned size) {
    store_imm(&b->buf, ENV, X64_NOREG, 1, off, imm, size);
}

void jit_ld_envx(JitBackend *b, JitReg d, int32_t off, JitReg idx,
                 unsigned scale, unsigned size) {
    load(&b->buf, R(d), ENV, R(idx), scale, off, size);
}

void jit_st_envx(JitBackend *b, int32_t off, JitReg idx, unsigned scale,
                 JitReg s, unsigned size) {
    store(&b->buf, ENV, R(idx), scale, off, R(s), size);
}

void jit_st_envxi(JitBackend *b, int32_t off, JitReg idx, unsigned scale,
                  int64_t imm, unsigned size) {
    store_imm(&b->buf, ENV, R(idx), scale, off, imm, size);
}

void jit_add_env(JitBackend *b, int32_t off, JitReg s) {
    x64_alu_mr(&b->buf, X64_ADD, ENV, off, R(s));
}

void jit_addi_env(JitBackend *b, int32_t off, int32_t imm) {
    x64_alu_mi(&b->buf, X64_ADD, ENV, off, imm);
}

void jit_ld(JitBackend *b, JitReg d, JitReg base, int32_t disp, unsigned size) {
    load(&b->buf, R(d), R(base), X64_NOREG, 1, disp, size);
}

void jit_st(JitBackend *b, JitReg base, int32_t disp, JitReg s, unsigned size) {
    store(&b->buf, R(base), X64_NOREG, 1, disp, R(s), size);
}

#else /* !GEMU_JIT_X64: no backend on this host; jit_backend_new() fails */

#include <stdio.h>

const char *jit_backend_name(void) { return "none"; }
JitBackend *jit_backend_new(size_t bytes) { (void)bytes; return NULL; }
void jit_backend_free(JitBackend *b) { (void)b; }
void jit_backend_reset(JitBackend *b) { (void)b; }
size_t jit_backend_used(const JitBackend *b) { (void)b; return 0; }

/* Unreachable without a backend. */
#define NOHOST(...) { (void)b; abort(); }
void jit_begin(JitBackend *b) NOHOST()
JitFn jit_end(JitBackend *b) NOHOST()
JitLabel jit_label(JitBackend *b) NOHOST()
void jit_bind(JitBackend *b, JitLabel l) { (void)l; NOHOST() }
void jit_jmp(JitBackend *b, JitLabel l) { (void)l; NOHOST() }
void jit_br(JitBackend *b, JitCond c, JitReg a, JitReg r, unsigned n, JitLabel l)
    { (void)c; (void)a; (void)r; (void)n; (void)l; NOHOST() }
void jit_bri(JitBackend *b, JitCond c, JitReg a, int64_t i, unsigned n, JitLabel l)
    { (void)c; (void)a; (void)i; (void)n; (void)l; NOHOST() }
void jit_brbit(JitBackend *b, JitReg a, unsigned i, bool s, JitLabel l)
    { (void)a; (void)i; (void)s; (void)l; NOHOST() }
void jit_ret(JitBackend *b, JitReg v) { (void)v; NOHOST() }
void jit_brbitr(JitBackend *b, JitReg a, JitReg i, bool s, JitLabel l)
    { (void)a; (void)i; (void)s; (void)l; NOHOST() }
void jit_bitset(JitBackend *b, JitReg d, JitReg i) { (void)d; (void)i; NOHOST() }
void jit_bitclr(JitBackend *b, JitReg d, JitReg i) { (void)d; (void)i; NOHOST() }
void jit_reti(JitBackend *b, uintptr_t v) { (void)v; NOHOST() }
void jit_call(JitBackend *b, const void *f, uintptr_t x, uintptr_t y)
    { (void)f; (void)x; (void)y; NOHOST() }
void jit_movi(JitBackend *b, JitReg d, uint64_t i) { (void)d; (void)i; NOHOST() }
void jit_mov(JitBackend *b, JitReg d, JitReg s) { (void)d; (void)s; NOHOST() }
void jit_op(JitBackend *b, JitOp o, JitReg d, JitReg a, JitReg c)
    { (void)o; (void)d; (void)a; (void)c; NOHOST() }
void jit_opi(JitBackend *b, JitOp o, JitReg d, JitReg a, int64_t i)
    { (void)o; (void)d; (void)a; (void)i; NOHOST() }
void jit_shli(JitBackend *b, JitReg d, JitReg a, unsigned n) { (void)d; (void)a; (void)n; NOHOST() }
void jit_shri(JitBackend *b, JitReg d, JitReg a, unsigned n) { (void)d; (void)a; (void)n; NOHOST() }
void jit_muli(JitBackend *b, JitReg d, JitReg a, int32_t i) { (void)d; (void)a; (void)i; NOHOST() }
void jit_not(JitBackend *b, JitReg d, JitReg a) { (void)d; (void)a; NOHOST() }
void jit_neg(JitBackend *b, JitReg d, JitReg a) { (void)d; (void)a; NOHOST() }
void jit_ext(JitBackend *b, JitExt e, JitReg d, JitReg s) { (void)e; (void)d; (void)s; NOHOST() }
void jit_setcond(JitBackend *b, JitCond c, JitReg d, JitReg a, JitReg r, unsigned n)
    { (void)c; (void)d; (void)a; (void)r; (void)n; NOHOST() }
void jit_ld_env(JitBackend *b, JitReg d, int32_t o, unsigned s) { (void)d; (void)o; (void)s; NOHOST() }
void jit_st_env(JitBackend *b, int32_t o, JitReg r, unsigned s) { (void)o; (void)r; (void)s; NOHOST() }
void jit_st_envi(JitBackend *b, int32_t o, int64_t i, unsigned s) { (void)o; (void)i; (void)s; NOHOST() }
void jit_ld_envx(JitBackend *b, JitReg d, int32_t o, JitReg x, unsigned k, unsigned s)
    { (void)d; (void)o; (void)x; (void)k; (void)s; NOHOST() }
void jit_st_envx(JitBackend *b, int32_t o, JitReg x, unsigned k, JitReg r, unsigned s)
    { (void)o; (void)x; (void)k; (void)r; (void)s; NOHOST() }
void jit_st_envxi(JitBackend *b, int32_t o, JitReg x, unsigned k, int64_t i, unsigned s)
    { (void)o; (void)x; (void)k; (void)i; (void)s; NOHOST() }
void jit_add_env(JitBackend *b, int32_t o, JitReg r) { (void)o; (void)r; NOHOST() }
void jit_addi_env(JitBackend *b, int32_t o, int32_t i) { (void)o; (void)i; NOHOST() }
void jit_ld(JitBackend *b, JitReg d, JitReg r, int32_t o, unsigned s)
    { (void)d; (void)r; (void)o; (void)s; NOHOST() }
void jit_st(JitBackend *b, JitReg r, int32_t o, JitReg v, unsigned s)
    { (void)r; (void)o; (void)v; (void)s; NOHOST() }

#endif
