/*
 * Zilog Z80 - see z80.h.
 *
 * Decoding follows the x/y/z/p/q field split of "Decoding Z80 Opcodes"
 * (Cristian Dinu).  Undocumented behaviour follows "The Undocumented Z80
 * Documented" (Sean Young), the MEMPTR notes (boo_boo, Vladimir Kladov),
 * Patrik Rak's SCF/CCF findings and David Banks' block-instruction flag
 * research, as checked by the SingleStepTests suite.
 *
 * Each instruction is built from its bus cycles: an opcode fetch takes 4
 * T-states (and bumps R), a memory read or write 3, an I/O cycle 4, and
 * tick() adds the internal cycles in between.
 */
#include "z80.h"
#include <stdio.h>
#include <string.h>

/* ── Bus cycles ──────────────────────────────────────────────────────────── */

static inline void tick(Z80 *c, int n) { c->cycles += (uint64_t)n; }

static inline uint8_t rd(Z80 *c, uint16_t a) {
    c->cycles += 3;
    return c->read(c->ud, a);
}

static inline void wr(Z80 *c, uint16_t a, uint8_t v) {
    c->cycles += 3;
    c->write(c->ud, a, v);
}

static inline uint8_t io_in(Z80 *c, uint16_t port) {
    c->cycles += 4;
    return c->in ? c->in(c->ud, port) : 0xFF;
}

static inline void io_out(Z80 *c, uint16_t port, uint8_t v) {
    c->cycles += 4;
    if (c->out) c->out(c->ud, port, v);
}

static inline void inc_r(Z80 *c) {
    c->r = (uint8_t)((c->r & 0x80u) | ((c->r + 1u) & 0x7Fu));
}

/* M1: opcode fetch. */
static inline uint8_t fetch_op(Z80 *c) {
    c->cycles += 4;
    uint8_t v = c->read(c->ud, c->pc++);
    inc_r(c);
    return v;
}

static inline uint8_t fetch(Z80 *c) { return rd(c, c->pc++); }

static inline uint16_t fetch16(Z80 *c) {
    uint8_t lo = fetch(c);
    return (uint16_t)(lo | (fetch(c) << 8));
}

static inline void push(Z80 *c, uint16_t v) {
    wr(c, --c->sp, (uint8_t)(v >> 8));
    wr(c, --c->sp, (uint8_t)v);
}

static inline uint16_t pop(Z80 *c) {
    uint8_t lo = rd(c, c->sp++);
    return (uint16_t)(lo | (rd(c, c->sp++) << 8));
}

/* ── Flags ───────────────────────────────────────────────────────────────── */

static uint8_t sz53p[256];   /* S, Z, Y, X and parity of a byte */

static void build_tables(void) {
    for (int i = 0; i < 256; i++) {
        int bits = 0;
        for (int b = 0; b < 8; b++) bits += (i >> b) & 1;
        sz53p[i] = (uint8_t)((i & (Z80_SF | Z80_YF | Z80_XF)) | (i == 0 ? Z80_ZF : 0) |
                             ((bits & 1) ? 0 : Z80_PF));
    }
}

#define SZ53(v)  (sz53p[(uint8_t)(v)] & (uint8_t)~Z80_PF)
#define SZ53P(v) (sz53p[(uint8_t)(v)])
#define XY(v)    ((uint8_t)(v) & (Z80_XF | Z80_YF))

/* Flag writes go through here so Q (the flags the last instruction set)
 * can be tracked for SCF/CCF. */
static inline void set_f(Z80 *c, uint8_t f) {
    c->f = f;
    c->q_set = true;
}

/* ── Register access ─────────────────────────────────────────────────────── *
 * xm selects what "HL" means: 0 = HL, 1 = IX, 2 = IY (DD / FD prefixes). */

static inline uint16_t bc(const Z80 *c) { return (uint16_t)(c->b << 8 | c->c); }
static inline uint16_t de(const Z80 *c) { return (uint16_t)(c->d << 8 | c->e); }
static inline uint16_t hl(const Z80 *c) { return (uint16_t)(c->h << 8 | c->l); }
static inline void set_bc(Z80 *c, uint16_t v) { c->b = (uint8_t)(v >> 8); c->c = (uint8_t)v; }
static inline void set_de(Z80 *c, uint16_t v) { c->d = (uint8_t)(v >> 8); c->e = (uint8_t)v; }
static inline void set_hl(Z80 *c, uint16_t v) { c->h = (uint8_t)(v >> 8); c->l = (uint8_t)v; }

static inline uint16_t get_xhl(const Z80 *c, int xm) {
    return xm == 0 ? hl(c) : xm == 1 ? c->ix : c->iy;
}

static inline void set_xhl(Z80 *c, int xm, uint16_t v) {
    if (xm == 0) set_hl(c, v);
    else if (xm == 1) c->ix = v;
    else c->iy = v;
}

/* r: 0 B, 1 C, 2 D, 3 E, 4 H, 5 L, 7 A (6 is memory - handled by callers). */
static uint8_t get_r(const Z80 *c, int r, int xm) {
    switch (r) {
    case 0: return c->b;
    case 1: return c->c;
    case 2: return c->d;
    case 3: return c->e;
    case 4: return xm == 0 ? c->h : (uint8_t)(get_xhl(c, xm) >> 8);
    case 5: return xm == 0 ? c->l : (uint8_t)get_xhl(c, xm);
    default: return c->a;
    }
}

static void set_r(Z80 *c, int r, int xm, uint8_t v) {
    switch (r) {
    case 0: c->b = v; break;
    case 1: c->c = v; break;
    case 2: c->d = v; break;
    case 3: c->e = v; break;
    case 4:
        if (xm == 0) c->h = v;
        else set_xhl(c, xm, (uint16_t)((get_xhl(c, xm) & 0x00FFu) | (v << 8)));
        break;
    case 5:
        if (xm == 0) c->l = v;
        else set_xhl(c, xm, (uint16_t)((get_xhl(c, xm) & 0xFF00u) | v));
        break;
    default: c->a = v; break;
    }
}

/* rp: BC DE HL SP;  rp2: BC DE HL AF */
static uint16_t get_rp(const Z80 *c, int p, int xm) {
    switch (p) {
    case 0: return bc(c);
    case 1: return de(c);
    case 2: return get_xhl(c, xm);
    default: return c->sp;
    }
}

static void set_rp(Z80 *c, int p, int xm, uint16_t v) {
    switch (p) {
    case 0: set_bc(c, v); break;
    case 1: set_de(c, v); break;
    case 2: set_xhl(c, xm, v); break;
    default: c->sp = v; break;
    }
}

/* The address of a (HL) operand, or (IX+d) / (IY+d): the displacement and
 * the internal cycles that compute the sum (5, except where overlapped). */
static uint16_t mem_operand(Z80 *c, int xm, int internal) {
    if (xm == 0) return hl(c);
    int8_t d = (int8_t)fetch(c);
    tick(c, internal);
    uint16_t a = (uint16_t)(get_xhl(c, xm) + d);
    c->wz = a;
    return a;
}

static bool cond(const Z80 *c, int cc) {
    switch (cc) {
    case 0: return !(c->f & Z80_ZF);
    case 1: return  (c->f & Z80_ZF);
    case 2: return !(c->f & Z80_CF);
    case 3: return  (c->f & Z80_CF);
    case 4: return !(c->f & Z80_PF);
    case 5: return  (c->f & Z80_PF);
    case 6: return !(c->f & Z80_SF);
    default: return (c->f & Z80_SF);
    }
}

/* ── ALU ─────────────────────────────────────────────────────────────────── */

static void alu_add(Z80 *c, uint8_t v, int carry) {
    unsigned r = c->a + v + (unsigned)carry;
    set_f(c, (uint8_t)(SZ53(r) | ((c->a ^ v ^ r) & Z80_HF) |
                       (((c->a ^ ~v) & (c->a ^ r) & 0x80u) ? Z80_PF : 0) |
                       (r > 0xFFu ? Z80_CF : 0)));
    c->a = (uint8_t)r;
}

static uint8_t alu_sub_flags(Z80 *c, uint8_t v, int carry, uint8_t *res) {
    unsigned r = (unsigned)c->a - v - (unsigned)carry;
    *res = (uint8_t)r;
    return (uint8_t)((sz53p[(uint8_t)r] & (Z80_SF | Z80_ZF)) | ((c->a ^ v ^ r) & Z80_HF) |
                     (((c->a ^ v) & (c->a ^ r) & 0x80u) ? Z80_PF : 0) |
                     Z80_NF | ((r >> 8) & 1u));
}

static void alu(Z80 *c, int op, uint8_t v) {
    uint8_t r;
    switch (op) {
    case 0: alu_add(c, v, 0); break;
    case 1: alu_add(c, v, c->f & Z80_CF); break;
    case 2: set_f(c, (uint8_t)(alu_sub_flags(c, v, 0, &r) | XY(r))); c->a = r; break;
    case 3: set_f(c, (uint8_t)(alu_sub_flags(c, v, c->f & Z80_CF, &r) | XY(r))); c->a = r; break;
    case 4: c->a &= v; set_f(c, (uint8_t)(SZ53P(c->a) | Z80_HF)); break;
    case 5: c->a ^= v; set_f(c, SZ53P(c->a)); break;
    case 6: c->a |= v; set_f(c, SZ53P(c->a)); break;
    default: set_f(c, (uint8_t)(alu_sub_flags(c, v, 0, &r) | XY(v))); break;   /* CP: X/Y from operand */
    }
}

static uint8_t inc8(Z80 *c, uint8_t v) {
    uint8_t r = (uint8_t)(v + 1);
    set_f(c, (uint8_t)((c->f & Z80_CF) | SZ53(r) | ((v & 0x0Fu) == 0x0F ? Z80_HF : 0) |
                       (v == 0x7F ? Z80_PF : 0)));
    return r;
}

static uint8_t dec8(Z80 *c, uint8_t v) {
    uint8_t r = (uint8_t)(v - 1);
    set_f(c, (uint8_t)((c->f & Z80_CF) | SZ53(r) | ((v & 0x0Fu) == 0 ? Z80_HF : 0) |
                       (v == 0x80 ? Z80_PF : 0) | Z80_NF));
    return r;
}

/* CB rotates and shifts: RLC RRC RL RR SLA SRA SLL SRL */
static uint8_t rot(Z80 *c, int op, uint8_t v) {
    uint8_t r, cy;
    switch (op) {
    case 0: cy = v >> 7; r = (uint8_t)(v << 1 | cy); break;
    case 1: cy = v & 1u; r = (uint8_t)(v >> 1 | cy << 7); break;
    case 2: cy = v >> 7; r = (uint8_t)(v << 1 | (c->f & Z80_CF)); break;
    case 3: cy = v & 1u; r = (uint8_t)(v >> 1 | (c->f & Z80_CF) << 7); break;
    case 4: cy = v >> 7; r = (uint8_t)(v << 1); break;
    case 5: cy = v & 1u; r = (uint8_t)((v >> 1) | (v & 0x80u)); break;
    case 6: cy = v >> 7; r = (uint8_t)(v << 1 | 1u); break;
    default: cy = v & 1u; r = (uint8_t)(v >> 1); break;
    }
    set_f(c, (uint8_t)(SZ53P(r) | cy));
    return r;
}

static void bit_op(Z80 *c, int n, uint8_t v, uint8_t xy_src) {
    uint8_t f = (uint8_t)((c->f & Z80_CF) | Z80_HF | XY(xy_src));
    if (!(v & (1u << n))) f |= Z80_ZF | Z80_PF;
    else if (n == 7)      f |= Z80_SF;
    set_f(c, f);
}

static void add16(Z80 *c, int xm, uint16_t v) {
    uint16_t a = get_xhl(c, xm);
    unsigned r = (unsigned)a + v;
    c->wz = (uint16_t)(a + 1);
    set_f(c, (uint8_t)((c->f & (Z80_SF | Z80_ZF | Z80_PF)) | XY(r >> 8) |
                       (((a ^ v ^ r) >> 8) & Z80_HF) | (r > 0xFFFFu ? Z80_CF : 0)));
    set_xhl(c, xm, (uint16_t)r);
}

static void adc16(Z80 *c, uint16_t v) {
    uint16_t a = hl(c);
    unsigned r = (unsigned)a + v + (c->f & Z80_CF);
    c->wz = (uint16_t)(a + 1);
    set_f(c, (uint8_t)(((r >> 8) & (Z80_SF | Z80_YF | Z80_XF)) | ((r & 0xFFFFu) ? 0 : Z80_ZF) |
                       (((a ^ v ^ r) >> 8) & Z80_HF) |
                       (((a ^ ~v) & (a ^ r) & 0x8000u) ? Z80_PF : 0) | (r > 0xFFFFu ? Z80_CF : 0)));
    set_hl(c, (uint16_t)r);
}

static void sbc16(Z80 *c, uint16_t v) {
    uint16_t a = hl(c);
    unsigned r = (unsigned)a - v - (c->f & Z80_CF);
    c->wz = (uint16_t)(a + 1);
    set_f(c, (uint8_t)(((r >> 8) & (Z80_SF | Z80_YF | Z80_XF)) | ((r & 0xFFFFu) ? 0 : Z80_ZF) |
                       (((a ^ v ^ r) >> 8) & Z80_HF) |
                       (((a ^ v) & (a ^ r) & 0x8000u) ? Z80_PF : 0) | Z80_NF |
                       ((r >> 16) & 1u)));
    set_hl(c, (uint16_t)r);
}

static void daa(Z80 *c) {
    uint8_t a = c->a, diff = 0, f = c->f;
    bool cy = f & Z80_CF;
    if ((f & Z80_HF) || (a & 0x0Fu) > 9) diff = 0x06;
    if (cy || a > 0x99) { diff |= 0x60; cy = true; }
    uint8_t r = (f & Z80_NF) ? (uint8_t)(a - diff) : (uint8_t)(a + diff);
    bool h = (f & Z80_NF) ? ((f & Z80_HF) && (a & 0x0Fu) < 6) : ((a & 0x0Fu) > 9);
    c->a = r;
    set_f(c, (uint8_t)(SZ53P(r) | (f & Z80_NF) | (h ? Z80_HF : 0) | (cy ? Z80_CF : 0)));
}

/* ── Block instructions ──────────────────────────────────────────────────── */

/* A repeating block instruction runs again from its own address; while it
 * does, X and Y come from that address's high byte. */
static void block_repeat(Z80 *c) {
    tick(c, 5);
    c->pc = (uint16_t)(c->pc - 2);
    c->wz = (uint16_t)(c->pc + 1);
    c->f = (uint8_t)((c->f & ~(Z80_XF | Z80_YF)) | XY(c->pc >> 8));
}

static void ldi_ldd(Z80 *c, int dir, bool repeat) {
    uint8_t v = rd(c, hl(c));
    wr(c, de(c), v);
    tick(c, 2);
    set_hl(c, (uint16_t)(hl(c) + dir));
    set_de(c, (uint16_t)(de(c) + dir));
    set_bc(c, (uint16_t)(bc(c) - 1));
    uint8_t n = (uint8_t)(c->a + v);
    set_f(c, (uint8_t)((c->f & (Z80_SF | Z80_ZF | Z80_CF)) | (n & Z80_XF) | ((n << 4) & Z80_YF) |
                       (bc(c) ? Z80_PF : 0)));
    if (repeat && bc(c)) block_repeat(c);
}

static void cpi_cpd(Z80 *c, int dir, bool repeat) {
    uint8_t v = rd(c, hl(c));
    tick(c, 5);
    set_hl(c, (uint16_t)(hl(c) + dir));
    set_bc(c, (uint16_t)(bc(c) - 1));
    c->wz = (uint16_t)(c->wz + dir);
    uint8_t r = (uint8_t)(c->a - v);
    bool h = ((c->a ^ v ^ r) & Z80_HF) != 0;
    uint8_t n = (uint8_t)(r - (h ? 1 : 0));
    set_f(c, (uint8_t)((c->f & Z80_CF) | (sz53p[r] & (Z80_SF | Z80_ZF)) | (h ? Z80_HF : 0) |
                       (n & Z80_XF) | ((n << 4) & Z80_YF) | (bc(c) ? Z80_PF : 0) | Z80_NF));
    if (repeat && bc(c) && !(c->f & Z80_ZF)) block_repeat(c);
}

/* Flags shared by INI/IND/OUTI/OUTD; k is the byte plus C+-1 (in) or L (out). */
static void io_block_flags(Z80 *c, uint8_t v, unsigned k, bool repeat) {
    uint8_t b = c->b;
    uint8_t f = (uint8_t)(SZ53(b) | ((v & 0x80u) ? Z80_NF : 0));
    if (k > 0xFF) f |= Z80_HF | Z80_CF;
    f |= sz53p[(uint8_t)((k & 7u) ^ b)] & Z80_PF;
    if (repeat && b) {
        /* Repeating: the interrupted cycle leaves H and P depending on
         * the next B value as well (Banks' research). */
        f = (uint8_t)((f & ~(Z80_XF | Z80_YF)) | XY((c->pc - 2) >> 8));
        uint8_t p = f & Z80_PF;
        if (f & Z80_CF) {
            if (v & 0x80u) {
                p ^= sz53p[(uint8_t)((b - 1) & 7)] & Z80_PF;
                p ^= Z80_PF;
                f = (uint8_t)((f & ~Z80_HF) | ((b & 0x0Fu) == 0x00 ? Z80_HF : 0));
            } else {
                p ^= sz53p[(uint8_t)((b + 1) & 7)] & Z80_PF;
                p ^= Z80_PF;
                f = (uint8_t)((f & ~Z80_HF) | ((b & 0x0Fu) == 0x0F ? Z80_HF : 0));
            }
        } else {
            p ^= sz53p[(uint8_t)(b & 7)] & Z80_PF;
            p ^= Z80_PF;
        }
        f = (uint8_t)((f & ~Z80_PF) | p);
    }
    set_f(c, f);
}

static void ini_ind(Z80 *c, int dir, bool repeat) {
    tick(c, 1);
    uint8_t v = io_in(c, bc(c));
    c->wz = (uint16_t)(bc(c) + dir);
    wr(c, hl(c), v);
    c->b--;
    set_hl(c, (uint16_t)(hl(c) + dir));
    io_block_flags(c, v, (unsigned)v + (uint8_t)(c->c + dir), repeat);
    if (repeat && c->b) {
        tick(c, 5);
        c->pc = (uint16_t)(c->pc - 2);
        c->wz = (uint16_t)(c->pc + 1);
    }
}

static void outi_outd(Z80 *c, int dir, bool repeat) {
    tick(c, 1);
    uint8_t v = rd(c, hl(c));
    c->b--;
    io_out(c, bc(c), v);
    c->wz = (uint16_t)(bc(c) + dir);
    set_hl(c, (uint16_t)(hl(c) + dir));
    io_block_flags(c, v, (unsigned)v + c->l, repeat);
    if (repeat && c->b) {
        tick(c, 5);
        c->pc = (uint16_t)(c->pc - 2);
        c->wz = (uint16_t)(c->pc + 1);
    }
}

/* ── CB prefix ───────────────────────────────────────────────────────────── */

static void exec_cb(Z80 *c, int xm) {
    if (xm) {   /* DDCB d op / FDCB d op: always (IX+d); results also land in r */
        int8_t d = (int8_t)fetch(c);
        uint8_t op = fetch(c);
        tick(c, 2);
        uint16_t a = (uint16_t)(get_xhl(c, xm) + d);
        c->wz = a;
        int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
        uint8_t v = rd(c, a);
        tick(c, 1);
        if (x == 1) { bit_op(c, y, v, (uint8_t)(a >> 8)); return; }
        uint8_t r = x == 0 ? rot(c, y, v) : x == 2 ? (uint8_t)(v & ~(1u << y)) : (uint8_t)(v | (1u << y));
        wr(c, a, r);
        if (z != 6) set_r(c, z, 0, r);
        return;
    }
    uint8_t op = fetch_op(c);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    if (z == 6) {
        uint16_t a = hl(c);
        uint8_t v = rd(c, a);
        tick(c, 1);
        if (x == 1) { bit_op(c, y, v, (uint8_t)(c->wz >> 8)); return; }
        uint8_t r = x == 0 ? rot(c, y, v) : x == 2 ? (uint8_t)(v & ~(1u << y)) : (uint8_t)(v | (1u << y));
        wr(c, a, r);
        return;
    }
    uint8_t v = get_r(c, z, 0);
    switch (x) {
    case 0: set_r(c, z, 0, rot(c, y, v)); break;
    case 1: bit_op(c, y, v, v); break;
    case 2: set_r(c, z, 0, (uint8_t)(v & ~(1u << y))); break;
    default: set_r(c, z, 0, (uint8_t)(v | (1u << y))); break;
    }
}

/* ── ED prefix ───────────────────────────────────────────────────────────── */

static void exec_ed(Z80 *c) {
    uint8_t op = fetch_op(c);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;

    if (x == 2 && z <= 3 && y >= 4) {
        int dir = (y & 1) ? -1 : 1;
        bool repeat = y >= 6;
        switch (z) {
        case 0: ldi_ldd(c, dir, repeat); break;
        case 1: cpi_cpd(c, dir, repeat); break;
        case 2: ini_ind(c, dir, repeat); break;
        default: outi_outd(c, dir, repeat); break;
        }
        return;
    }
    if (x != 1) return;   /* the rest of ED is NOP (8 T-states) */

    switch (z) {
    case 0: {   /* IN r,(C) - y=6: IN (C), flags only */
        uint8_t v = io_in(c, bc(c));
        c->wz = (uint16_t)(bc(c) + 1);
        set_f(c, (uint8_t)((c->f & Z80_CF) | SZ53P(v)));
        if (y != 6) set_r(c, y, 0, v);
        break;
    }
    case 1:     /* OUT (C),r - y=6: OUT (C),0 */
        io_out(c, bc(c), y == 6 ? 0 : get_r(c, y, 0));
        c->wz = (uint16_t)(bc(c) + 1);
        break;
    case 2:
        tick(c, 7);
        if (q == 0) sbc16(c, get_rp(c, p, 0));
        else        adc16(c, get_rp(c, p, 0));
        break;
    case 3: {
        uint16_t nn = fetch16(c);
        if (q == 0) {
            uint16_t v = get_rp(c, p, 0);
            wr(c, nn, (uint8_t)v);
            wr(c, (uint16_t)(nn + 1), (uint8_t)(v >> 8));
        } else {
            uint8_t lo = rd(c, nn);
            set_rp(c, p, 0, (uint16_t)(lo | (rd(c, (uint16_t)(nn + 1)) << 8)));
        }
        c->wz = (uint16_t)(nn + 1);
        break;
    }
    case 4: {   /* NEG */
        uint8_t v = c->a;
        c->a = 0;
        alu(c, 2, v);
        break;
    }
    case 5:     /* RETN / RETI */
        c->pc = pop(c);
        c->wz = c->pc;
        c->iff1 = c->iff2;
        break;
    case 6: {
        static const uint8_t modes[8] = { 0, 0, 1, 2, 0, 0, 1, 2 };
        c->im = modes[y];
        break;
    }
    default:
        switch (y) {
        case 0: tick(c, 1); c->i = c->a; break;
        case 1: tick(c, 1); c->r = c->a; break;
        case 2: case 3:   /* LD A,I / LD A,R: P/V shows IFF2 */
            tick(c, 1);
            c->a = y == 2 ? c->i : c->r;
            set_f(c, (uint8_t)((c->f & Z80_CF) | SZ53(c->a) | (c->iff2 ? Z80_PF : 0)));
            c->p = true;
            break;
        case 4: case 5: {   /* RRD / RLD */
            uint16_t a = hl(c);
            uint8_t v = rd(c, a), n;
            tick(c, 4);
            if (y == 4) {
                n = (uint8_t)((c->a << 4) | (v >> 4));
                c->a = (uint8_t)((c->a & 0xF0u) | (v & 0x0Fu));
            } else {
                n = (uint8_t)((v << 4) | (c->a & 0x0Fu));
                c->a = (uint8_t)((c->a & 0xF0u) | (v >> 4));
            }
            wr(c, a, n);
            c->wz = (uint16_t)(a + 1);
            set_f(c, (uint8_t)((c->f & Z80_CF) | SZ53P(c->a)));
            break;
        }
        default: break;   /* ED 77 / ED 7F: NOP */
        }
        break;
    }
}

/* ── Unprefixed (and DD / FD) ────────────────────────────────────────────── */

static void exec_main(Z80 *c, uint8_t op, int xm) {
    for (;;) {   /* DD/FD runs: only the last prefix counts */
        int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;

        switch (x) {
        case 0:
            switch (z) {
            case 0:
                switch (y) {
                case 0: break;   /* NOP */
                case 1: {        /* EX AF,AF' */
                    uint16_t t = (uint16_t)(c->a << 8 | c->f);
                    c->a = (uint8_t)(c->af_ >> 8);
                    c->f = (uint8_t)c->af_;
                    c->af_ = t;
                    break;
                }
                case 2: {        /* DJNZ */
                    tick(c, 1);
                    int8_t d = (int8_t)fetch(c);
                    if (--c->b) { tick(c, 5); c->pc = (uint16_t)(c->pc + d); c->wz = c->pc; }
                    break;
                }
                default: {       /* JR / JR cc */
                    int8_t d = (int8_t)fetch(c);
                    if (y == 3 || cond(c, y - 4)) {
                        tick(c, 5);
                        c->pc = (uint16_t)(c->pc + d);
                        c->wz = c->pc;
                    }
                    break;
                }
                }
                return;
            case 1:
                if (q == 0) set_rp(c, p, xm, fetch16(c));
                else { tick(c, 7); add16(c, xm, get_rp(c, p, xm)); }
                return;
            case 2: {
                uint16_t a;
                switch (y) {
                case 0: case 2: {   /* LD (BC),A / LD (DE),A */
                    a = y == 0 ? bc(c) : de(c);
                    wr(c, a, c->a);
                    c->wz = (uint16_t)((c->a << 8) | ((a + 1) & 0xFFu));
                    break;
                }
                case 1: case 3:     /* LD A,(BC) / LD A,(DE) */
                    a = y == 1 ? bc(c) : de(c);
                    c->a = rd(c, a);
                    c->wz = (uint16_t)(a + 1);
                    break;
                case 4: {           /* LD (nn),HL */
                    a = fetch16(c);
                    uint16_t v = get_xhl(c, xm);
                    wr(c, a, (uint8_t)v);
                    wr(c, (uint16_t)(a + 1), (uint8_t)(v >> 8));
                    c->wz = (uint16_t)(a + 1);
                    break;
                }
                case 5: {           /* LD HL,(nn) */
                    a = fetch16(c);
                    uint8_t lo = rd(c, a);
                    set_xhl(c, xm, (uint16_t)(lo | (rd(c, (uint16_t)(a + 1)) << 8)));
                    c->wz = (uint16_t)(a + 1);
                    break;
                }
                case 6:             /* LD (nn),A */
                    a = fetch16(c);
                    wr(c, a, c->a);
                    c->wz = (uint16_t)((c->a << 8) | ((a + 1) & 0xFFu));
                    break;
                default:            /* LD A,(nn) */
                    a = fetch16(c);
                    c->a = rd(c, a);
                    c->wz = (uint16_t)(a + 1);
                    break;
                }
                return;
            }
            case 3:
                tick(c, 2);
                set_rp(c, p, xm, (uint16_t)(get_rp(c, p, xm) + (q ? -1 : 1)));
                return;
            case 4: case 5:   /* INC r / DEC r */
                if (y == 6) {
                    uint16_t a = mem_operand(c, xm, 5);
                    uint8_t v = rd(c, a);
                    tick(c, 1);
                    wr(c, a, z == 4 ? inc8(c, v) : dec8(c, v));
                } else {
                    uint8_t v = get_r(c, y, xm);
                    set_r(c, y, xm, z == 4 ? inc8(c, v) : dec8(c, v));
                }
                return;
            case 6:           /* LD r,n */
                if (y == 6) {
                    if (xm) {
                        int8_t d = (int8_t)fetch(c);
                        uint8_t n = fetch(c);
                        tick(c, 2);
                        uint16_t a = (uint16_t)(get_xhl(c, xm) + d);
                        c->wz = a;
                        wr(c, a, n);
                    } else {
                        uint8_t n = fetch(c);
                        wr(c, hl(c), n);
                    }
                } else {
                    set_r(c, y, xm, fetch(c));
                }
                return;
            default: {        /* accumulator / flag ops */
                uint8_t a = c->a, f = c->f;
                switch (y) {
                case 0: c->a = (uint8_t)(a << 1 | a >> 7);
                        set_f(c, (uint8_t)((f & (Z80_SF | Z80_ZF | Z80_PF)) | XY(c->a) | (a >> 7))); break;
                case 1: c->a = (uint8_t)(a >> 1 | a << 7);
                        set_f(c, (uint8_t)((f & (Z80_SF | Z80_ZF | Z80_PF)) | XY(c->a) | (a & 1u))); break;
                case 2: c->a = (uint8_t)(a << 1 | (f & Z80_CF));
                        set_f(c, (uint8_t)((f & (Z80_SF | Z80_ZF | Z80_PF)) | XY(c->a) | (a >> 7))); break;
                case 3: c->a = (uint8_t)(a >> 1 | (f & Z80_CF) << 7);
                        set_f(c, (uint8_t)((f & (Z80_SF | Z80_ZF | Z80_PF)) | XY(c->a) | (a & 1u))); break;
                case 4: daa(c); break;
                case 5: c->a = (uint8_t)~a;
                        set_f(c, (uint8_t)((f & (Z80_SF | Z80_ZF | Z80_PF | Z80_CF)) | XY(c->a) |
                                           Z80_HF | Z80_NF)); break;
                case 6: {   /* SCF: X/Y = (Q ^ F) | A */
                    set_f(c, (uint8_t)((f & (Z80_SF | Z80_ZF | Z80_PF)) | XY((c->q ^ f) | a) | Z80_CF));
                    break;
                }
                default: {  /* CCF */
                    set_f(c, (uint8_t)((f & (Z80_SF | Z80_ZF | Z80_PF)) | XY((c->q ^ f) | a) |
                                       ((f & Z80_CF) ? Z80_HF : Z80_CF)));
                    break;
                }
                }
                return;
            }
            }
            return;

        case 1:
            if (op == 0x76) {   /* HALT */
                c->halted = true;
                return;
            }
            if (y == 6)      wr(c, mem_operand(c, xm, 5), get_r(c, z, 0));
            else if (z == 6) set_r(c, y, 0, rd(c, mem_operand(c, xm, 5)));
            else             set_r(c, y, xm, get_r(c, z, xm));
            return;

        case 2:
            alu(c, y, z == 6 ? rd(c, mem_operand(c, xm, 5)) : get_r(c, z, xm));
            return;

        default:
            switch (z) {
            case 0:   /* RET cc */
                tick(c, 1);
                if (cond(c, y)) { c->pc = pop(c); c->wz = c->pc; }
                return;
            case 1:
                if (q == 0) {   /* POP rp2 */
                    uint16_t v = pop(c);
                    if (p == 3) { c->a = (uint8_t)(v >> 8); c->f = (uint8_t)v; }
                    else set_rp(c, p, xm, v);
                    return;
                }
                switch (p) {
                case 0: c->pc = pop(c); c->wz = c->pc; break;   /* RET */
                case 1: {   /* EXX */
                    uint16_t t;
                    t = bc(c); set_bc(c, c->bc_); c->bc_ = t;
                    t = de(c); set_de(c, c->de_); c->de_ = t;
                    t = hl(c); set_hl(c, c->hl_); c->hl_ = t;
                    break;
                }
                case 2: c->pc = get_xhl(c, xm); break;           /* JP (HL) */
                default: tick(c, 2); c->sp = get_xhl(c, xm); break;   /* LD SP,HL */
                }
                return;
            case 2: {   /* JP cc,nn */
                uint16_t nn = fetch16(c);
                c->wz = nn;
                if (cond(c, y)) c->pc = nn;
                return;
            }
            case 3:
                switch (y) {
                case 0: c->pc = fetch16(c); c->wz = c->pc; break;   /* JP nn */
                case 1: exec_cb(c, xm); break;
                case 2: {   /* OUT (n),A */
                    uint8_t n = fetch(c);
                    io_out(c, (uint16_t)(c->a << 8 | n), c->a);
                    c->wz = (uint16_t)((c->a << 8) | ((n + 1) & 0xFFu));
                    break;
                }
                case 3: {   /* IN A,(n) */
                    uint8_t n = fetch(c);
                    uint16_t port = (uint16_t)(c->a << 8 | n);
                    c->a = io_in(c, port);
                    c->wz = (uint16_t)(port + 1);
                    break;
                }
                case 4: {   /* EX (SP),HL */
                    uint16_t v = get_xhl(c, xm);
                    uint8_t lo = rd(c, c->sp), hi = rd(c, (uint16_t)(c->sp + 1));
                    tick(c, 1);
                    wr(c, (uint16_t)(c->sp + 1), (uint8_t)(v >> 8));
                    wr(c, c->sp, (uint8_t)v);
                    tick(c, 2);
                    set_xhl(c, xm, (uint16_t)(lo | hi << 8));
                    c->wz = (uint16_t)(lo | hi << 8);
                    break;
                }
                case 5: {   /* EX DE,HL (never IX/IY) */
                    uint16_t t = de(c);
                    set_de(c, hl(c));
                    set_hl(c, t);
                    break;
                }
                case 6: c->iff1 = c->iff2 = false; break;            /* DI */
                default: c->iff1 = c->iff2 = true; c->ei = true; break;   /* EI */
                }
                return;
            case 4: {   /* CALL cc,nn */
                uint16_t nn = fetch16(c);
                c->wz = nn;
                if (cond(c, y)) { tick(c, 1); push(c, c->pc); c->pc = nn; }
                return;
            }
            case 5:
                if (q == 0) {   /* PUSH rp2 */
                    tick(c, 1);
                    push(c, p == 3 ? (uint16_t)(c->a << 8 | c->f) : get_rp(c, p, xm));
                    return;
                }
                switch (p) {
                case 0: {   /* CALL nn */
                    uint16_t nn = fetch16(c);
                    c->wz = nn;
                    tick(c, 1);
                    push(c, c->pc);
                    c->pc = nn;
                    return;
                }
                case 2: exec_ed(c); return;   /* any DD/FD before ED is ignored */
                default:                      /* DD / FD: sets no flags, so Q = 0 */
                    xm = p == 1 ? 1 : 2;
                    c->q = 0;
                    op = fetch_op(c);
                    continue;
                }
            case 6:
                alu(c, y, fetch(c));
                return;
            default:    /* RST */
                tick(c, 1);
                push(c, c->pc);
                c->pc = (uint16_t)(y * 8);
                c->wz = c->pc;
                return;
            }
        }
    }
}

/* ── Interrupts and the step loop ────────────────────────────────────────── */

static void take_nmi(Z80 *c) {
    c->nmi_line = false;
    c->halted = false;
    tick(c, 5);   /* M1 with the opcode ignored */
    inc_r(c);
    c->iff1 = false;
    push(c, c->pc);
    c->pc = 0x0066;
    c->wz = c->pc;
}

static void take_int(Z80 *c) {
    /* NMOS quirk: an INT accepted right after LD A,I / LD A,R clears the
     * P/V it just copied from IFF2. */
    if (c->p) c->f &= (uint8_t)~Z80_PF;
    c->halted = false;
    c->iff1 = c->iff2 = false;
    inc_r(c);
    switch (c->im) {
    case 0:   /* the data bus supplies an opcode: an RST on real boards */
        tick(c, 6);
        if ((c->int_data & 0xC7u) == 0xC7u) {
            tick(c, 1);
            push(c, c->pc);
            c->pc = (uint16_t)(c->int_data & 0x38u);
            c->wz = c->pc;
        }
        break;
    case 1:
        tick(c, 7);
        push(c, c->pc);
        c->pc = 0x0038;
        c->wz = c->pc;
        break;
    default: {
        tick(c, 7);
        push(c, c->pc);
        uint16_t v = (uint16_t)(c->i << 8 | c->int_data);
        uint8_t lo = rd(c, v);
        c->pc = (uint16_t)(lo | rd(c, (uint16_t)(v + 1)) << 8);
        c->wz = c->pc;
        break;
    }
    }
}

int z80_step(Z80 *c) {
    uint64_t start = c->cycles;
    bool after_ei = c->ei;
    c->ei = false;

    if (c->nmi_line) {
        take_nmi(c);
        c->q = 0;
        c->p = false;
        return (int)(c->cycles - start);
    }
    if (c->int_line && c->iff1 && !after_ei) {
        take_int(c);
        c->q = 0;
        c->p = false;
        return (int)(c->cycles - start);
    }
    if (c->halted) {   /* HALT: M1 cycles at the next address, PC held */
        tick(c, 4);
        (void)c->read(c->ud, c->pc);
        inc_r(c);
        c->q = 0;
        c->p = false;
        return (int)(c->cycles - start);
    }

    /* c->q still holds the previous instruction's Q for SCF/CCF. */
    c->p = false;
    c->q_set = false;
    exec_main(c, fetch_op(c), 0);
    c->q = c->q_set ? c->f : 0;
    return (int)(c->cycles - start);
}

/* ── Setup ───────────────────────────────────────────────────────────────── */

void z80_reset(Z80 *c) {
    c->pc = 0;
    c->i = c->r = 0;
    c->im = 0;
    c->iff1 = c->iff2 = false;
    c->halted = false;
    c->ei = false;
    c->p = false;
    c->q = 0;
    c->af_ = c->bc_ = c->de_ = c->hl_ = 0xFFFF;
    c->a = c->f = 0xFF;
    c->sp = 0xFFFF;
    c->wz = 0;
}

void z80_init(Z80 *c) {
    static bool built;
    if (!built) { build_tables(); built = true; }
    memset(c, 0, sizeof(*c));
    c->int_data = 0xFF;
    z80_reset(c);
}

void z80_format_state(const Z80 *c, char *buf, size_t len) {
    static const char names[] = "SZYHXPNC";
    char f[9];
    for (int i = 0; i < 8; i++) f[i] = (c->f & (0x80u >> i)) ? names[i] : '.';
    f[8] = '\0';
    snprintf(buf, len,
             "PC=%04X SP=%04X AF=%02X%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X IX=%04X IY=%04X "
             "I=%02X R=%02X [%s] IM%d %s%s cyc=%llu",
             c->pc, c->sp, c->a, c->f, c->b, c->c, c->d, c->e, c->h, c->l, c->ix, c->iy,
             c->i, c->r, f, c->im, c->iff1 ? "EI" : "DI", c->halted ? " HALT" : "",
             (unsigned long long)c->cycles);
}
