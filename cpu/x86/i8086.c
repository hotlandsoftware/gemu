#include "i8086.h"
#include <string.h>

/* Decode-transient prefix state for the instruction currently being
 * executed. File-static rather than X86Cpu fields since it's only ever
 * valid for the duration of one x86_step() call and threading it through
 * every helper's parameter list would obscure the actual opcode logic
 * below - same tradeoff cdp1802.c and mos6502.c make with their own
 * per-instruction scratch state. */
static bool     g_seg_override;
static uint16_t g_override_seg;
static bool     g_rep;   /* REP/REPE/REPZ (0xF3) */
static bool     g_repne; /* REPNE/REPNZ (0xF2) */

uint32_t x86_linear(uint16_t seg, uint16_t off) {
    return (((uint32_t)seg << 4) + off) & 0xFFFFFu;
}

static void set_flag(X86Cpu *c, uint16_t bit, bool v) {
    if (v) c->flags |= bit; else c->flags = (uint16_t)(c->flags & ~bit);
}
static bool get_flag(const X86Cpu *c, uint16_t bit) { return (c->flags & bit) != 0; }

static bool parity8(uint8_t v) {
    v ^= (uint8_t)(v >> 4);
    v ^= (uint8_t)(v >> 2);
    v ^= (uint8_t)(v >> 1);
    return ((~v) & 1) != 0;
}

/* ── Bus access ──────────────────────────────────────────────────────────── */

static uint8_t mrd8(X86Cpu *c, uint16_t seg, uint16_t off) {
    return c->mem_read8(x86_linear(seg, off), c->bus_ud);
}
static void mwr8(X86Cpu *c, uint16_t seg, uint16_t off, uint8_t v) {
    c->mem_write8(x86_linear(seg, off), v, c->bus_ud);
}
static uint16_t mrd16(X86Cpu *c, uint16_t seg, uint16_t off) {
    uint8_t lo = mrd8(c, seg, off);
    uint8_t hi = mrd8(c, seg, (uint16_t)(off + 1));
    return (uint16_t)(lo | (hi << 8));
}
static void mwr16(X86Cpu *c, uint16_t seg, uint16_t off, uint16_t v) {
    mwr8(c, seg, off, (uint8_t)v);
    mwr8(c, seg, (uint16_t)(off + 1), (uint8_t)(v >> 8));
}
static uint8_t fetch8(X86Cpu *c) {
    uint8_t v = mrd8(c, c->cs, c->ip);
    c->ip = (uint16_t)(c->ip + 1);
    return v;
}
static uint16_t fetch16(X86Cpu *c) {
    uint8_t lo = fetch8(c);
    uint8_t hi = fetch8(c);
    return (uint16_t)(lo | (hi << 8));
}
static void push16(X86Cpu *c, uint16_t v) {
    c->sp = (uint16_t)(c->sp - 2);
    mwr16(c, c->ss, c->sp, v);
}
static uint16_t pop16(X86Cpu *c) {
    uint16_t v = mrd16(c, c->ss, c->sp);
    c->sp = (uint16_t)(c->sp + 2);
    return v;
}

/* ── Register file access by ModRM-style index ──────────────────────────── */

static uint8_t *reg8_ptr(X86Cpu *c, unsigned idx) {
    switch (idx & 7) {
    case 0: return &c->ax.b.lo; case 1: return &c->cx.b.lo;
    case 2: return &c->dx.b.lo; case 3: return &c->bx.b.lo;
    case 4: return &c->ax.b.hi; case 5: return &c->cx.b.hi;
    case 6: return &c->dx.b.hi; default: return &c->bx.b.hi;
    }
}
static uint16_t *reg16_ptr(X86Cpu *c, unsigned idx) {
    switch (idx & 7) {
    case 0: return &c->ax.x; case 1: return &c->cx.x;
    case 2: return &c->dx.x; case 3: return &c->bx.x;
    case 4: return &c->sp;   case 5: return &c->bp;
    case 6: return &c->si;   default: return &c->di;
    }
}
static uint16_t *seg_ptr(X86Cpu *c, unsigned idx) {
    switch (idx & 3) {
    case 0: return &c->es; case 1: return &c->cs;
    case 2: return &c->ss; default: return &c->ds;
    }
}

typedef struct {
    bool     mem;
    uint16_t seg, off; /* valid if mem */
    unsigned rm_reg;   /* valid if !mem: register index for the r/m operand */
    unsigned reg;      /* "reg" field: a register operand, or a GRP opcode extension */
} ModRM;

static ModRM decode_modrm(X86Cpu *c) {
    uint8_t byte = fetch8(c);
    unsigned mod = byte >> 6, reg = (byte >> 3) & 7, rm = byte & 7;
    ModRM m; memset(&m, 0, sizeof m);
    m.reg = reg;
    if (mod == 3) { m.rm_reg = rm; return m; }
    m.mem = true;
    uint16_t base_seg = g_seg_override ? g_override_seg : c->ds;
    uint16_t addr;
    switch (rm) {
    case 0: addr = (uint16_t)(c->bx.x + c->si); break;
    case 1: addr = (uint16_t)(c->bx.x + c->di); break;
    case 2: addr = (uint16_t)(c->bp + c->si); if (!g_seg_override) base_seg = c->ss; break;
    case 3: addr = (uint16_t)(c->bp + c->di); if (!g_seg_override) base_seg = c->ss; break;
    case 4: addr = c->si; break;
    case 5: addr = c->di; break;
    case 6:
        if (mod == 0) { m.off = fetch16(c); m.seg = base_seg; return m; } /* direct addr */
        addr = c->bp; if (!g_seg_override) base_seg = c->ss;
        break;
    default: addr = c->bx.x; break; /* rm==7 */
    }
    int16_t disp = 0;
    if (mod == 1) disp = (int8_t)fetch8(c);
    else if (mod == 2) disp = (int16_t)fetch16(c);
    m.off = (uint16_t)(addr + disp);
    m.seg = base_seg;
    return m;
}

static uint8_t  rm_read8(X86Cpu *c, const ModRM *m)  { return m->mem ? mrd8(c, m->seg, m->off)  : *reg8_ptr(c, m->rm_reg); }
static void     rm_write8(X86Cpu *c, const ModRM *m, uint8_t v)  { if (m->mem) mwr8(c, m->seg, m->off, v);  else *reg8_ptr(c, m->rm_reg) = v; }
static uint16_t rm_read16(X86Cpu *c, const ModRM *m) { return m->mem ? mrd16(c, m->seg, m->off) : *reg16_ptr(c, m->rm_reg); }
static void     rm_write16(X86Cpu *c, const ModRM *m, uint16_t v) { if (m->mem) mwr16(c, m->seg, m->off, v); else *reg16_ptr(c, m->rm_reg) = v; }

/* ── Flags-computing ALU core ───────────────────────────────────────────── */

enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };

static void add_flags(X86Cpu *c, uint32_t a, uint32_t b, uint32_t cin, bool w) {
    uint32_t mask = w ? 0xFFFFu : 0xFFu, sign = w ? 0x8000u : 0x80u;
    uint32_t sum = a + b + cin, res = sum & mask;
    set_flag(c, X86_FLAG_CF, sum > mask);
    set_flag(c, X86_FLAG_AF, ((a ^ b ^ res) & 0x10) != 0);
    set_flag(c, X86_FLAG_OF, (((a ^ res) & (b ^ res)) & sign) != 0);
    set_flag(c, X86_FLAG_ZF, res == 0);
    set_flag(c, X86_FLAG_SF, (res & sign) != 0);
    set_flag(c, X86_FLAG_PF, parity8((uint8_t)res));
}
static void sub_flags(X86Cpu *c, uint32_t a, uint32_t b, uint32_t bin, bool w) {
    uint32_t mask = w ? 0xFFFFu : 0xFFu, sign = w ? 0x8000u : 0x80u;
    uint32_t diff = a - b - bin, res = diff & mask;
    set_flag(c, X86_FLAG_CF, a < b + bin);
    set_flag(c, X86_FLAG_AF, ((a ^ b ^ res) & 0x10) != 0);
    set_flag(c, X86_FLAG_OF, (((a ^ b) & (a ^ res)) & sign) != 0);
    set_flag(c, X86_FLAG_ZF, res == 0);
    set_flag(c, X86_FLAG_SF, (res & sign) != 0);
    set_flag(c, X86_FLAG_PF, parity8((uint8_t)res));
}
static void logic_flags(X86Cpu *c, uint32_t res, bool w) {
    uint32_t sign = w ? 0x8000u : 0x80u;
    set_flag(c, X86_FLAG_CF, false);
    set_flag(c, X86_FLAG_OF, false);
    set_flag(c, X86_FLAG_ZF, res == 0);
    set_flag(c, X86_FLAG_SF, (res & sign) != 0);
    set_flag(c, X86_FLAG_PF, parity8((uint8_t)res));
}

/* Applies ALU op `idx` (ALU_ADD..ALU_CMP) to a,b and returns the result
 * (CMP's result should be discarded by the caller - only its flags matter). */
static uint32_t alu_op(X86Cpu *c, unsigned idx, uint32_t a, uint32_t b, bool w) {
    uint32_t mask = w ? 0xFFFFu : 0xFFu;
    uint32_t cf = get_flag(c, X86_FLAG_CF) ? 1u : 0u;
    switch (idx & 7) {
    case ALU_ADD: add_flags(c, a, b, 0, w);  return (a + b) & mask;
    case ALU_OR:  { uint32_t r = (a | b) & mask; logic_flags(c, r, w); return r; }
    case ALU_ADC: add_flags(c, a, b, cf, w); return (a + b + cf) & mask;
    case ALU_SBB: sub_flags(c, a, b, cf, w); return (a - b - cf) & mask;
    case ALU_AND: { uint32_t r = (a & b) & mask; logic_flags(c, r, w); return r; }
    case ALU_SUB: sub_flags(c, a, b, 0, w);  return (a - b) & mask;
    case ALU_XOR: { uint32_t r = (a ^ b) & mask; logic_flags(c, r, w); return r; }
    default:      sub_flags(c, a, b, 0, w);  return a; /* ALU_CMP */
    }
}

static uint32_t do_inc(X86Cpu *c, uint32_t v, bool w) {
    bool cf = get_flag(c, X86_FLAG_CF);
    uint32_t r = alu_op(c, ALU_ADD, v, 1, w);
    set_flag(c, X86_FLAG_CF, cf); /* INC/DEC never touch CF */
    return r;
}
static uint32_t do_dec(X86Cpu *c, uint32_t v, bool w) {
    bool cf = get_flag(c, X86_FLAG_CF);
    uint32_t r = alu_op(c, ALU_SUB, v, 1, w);
    set_flag(c, X86_FLAG_CF, cf);
    return r;
}

static void alu_rm_rm8(X86Cpu *c, unsigned idx, bool reg_is_dest) {
    ModRM m = decode_modrm(c);
    uint8_t *regp = reg8_ptr(c, m.reg);
    if (reg_is_dest) {
        uint8_t r = (uint8_t)alu_op(c, idx, *regp, rm_read8(c, &m), false);
        if (idx != ALU_CMP) *regp = r;
    } else {
        uint8_t r = (uint8_t)alu_op(c, idx, rm_read8(c, &m), *regp, false);
        if (idx != ALU_CMP) rm_write8(c, &m, r);
    }
}
static void alu_rm_rm16(X86Cpu *c, unsigned idx, bool reg_is_dest) {
    ModRM m = decode_modrm(c);
    uint16_t *regp = reg16_ptr(c, m.reg);
    if (reg_is_dest) {
        uint16_t r = (uint16_t)alu_op(c, idx, *regp, rm_read16(c, &m), true);
        if (idx != ALU_CMP) *regp = r;
    } else {
        uint16_t r = (uint16_t)alu_op(c, idx, rm_read16(c, &m), *regp, true);
        if (idx != ALU_CMP) rm_write16(c, &m, r);
    }
}
static void alu_al_imm8(X86Cpu *c, unsigned idx) {
    uint8_t imm = fetch8(c);
    uint8_t r = (uint8_t)alu_op(c, idx, c->ax.b.lo, imm, false);
    if (idx != ALU_CMP) c->ax.b.lo = r;
}
static void alu_ax_imm16(X86Cpu *c, unsigned idx) {
    uint16_t imm = fetch16(c);
    uint16_t r = (uint16_t)alu_op(c, idx, c->ax.x, imm, true);
    if (idx != ALU_CMP) c->ax.x = r;
}

/* ── Shift/rotate group (0xD0-0xD3) ─────────────────────────────────────── */

static uint32_t shift_op(X86Cpu *c, unsigned op, uint32_t val, unsigned count, bool w) {
    uint32_t mask = w ? 0xFFFFu : 0xFFu, sign = w ? 0x8000u : 0x80u;
    if (count == 0) return val & mask; /* count=0: flags untouched (real 8086 behavior) */
    bool cf = get_flag(c, X86_FLAG_CF);
    uint32_t res = val & mask, before = res;
    for (unsigned i = 0; i < count; i++) {
        switch (op & 7) {
        case 0: cf = (res & sign) != 0; res = ((res << 1) | (cf ? 1u : 0u)) & mask; break;                 /* ROL */
        case 1: cf = (res & 1) != 0;    res = ((res >> 1) | (cf ? sign : 0u)) & mask; break;               /* ROR */
        case 2: { bool n = (res & sign) != 0; res = ((res << 1) | (cf ? 1u : 0u)) & mask; cf = n; } break; /* RCL */
        case 3: { bool n = (res & 1) != 0; res = ((res >> 1) | (cf ? sign : 0u)) & mask; cf = n; } break;  /* RCR */
        case 5: cf = (res & 1) != 0; res = (res >> 1) & mask; break;                                       /* SHR */
        case 7: { bool msb = (res & sign) != 0; cf = (res & 1) != 0; res = ((res >> 1) | (msb ? sign : 0u)) & mask; } break; /* SAR */
        default: cf = (res & sign) != 0; res = (res << 1) & mask; break;                                   /* SHL/SAL (4,6) */
        }
    }
    set_flag(c, X86_FLAG_CF, cf);
    /* OF is architecturally defined only for count==1; we compute a
     * reasonable value for larger counts too but don't promise spec
     * compliance there - matches common simple-interpreter practice. */
    bool of;
    switch (op & 7) {
    case 5:  of = (before & sign) != 0; break;             /* SHR: original MSB */
    case 7:  of = false; break;                             /* SAR: always 0 */
    default: of = ((res & sign) != 0) ^ cf; break;           /* ROL/RCL/SHL/SAL/ROR/RCR approx */
    }
    set_flag(c, X86_FLAG_OF, of);
    if ((op & 7) >= 4) { /* SHL/SHR/SAL/SAR update ZF/SF/PF; pure rotates don't */
        set_flag(c, X86_FLAG_ZF, res == 0);
        set_flag(c, X86_FLAG_SF, (res & sign) != 0);
        set_flag(c, X86_FLAG_PF, parity8((uint8_t)res));
    }
    return res;
}

/* ── String instructions ────────────────────────────────────────────────── */

static void do_movs(X86Cpu *c, bool w) {
    int delta = (get_flag(c, X86_FLAG_DF) ? -1 : 1) * (w ? 2 : 1);
    uint16_t srcseg = g_seg_override ? g_override_seg : c->ds;
    bool repeat = g_rep || g_repne;
    do {
        if (repeat && c->cx.x == 0) break;
        if (w) mwr16(c, c->es, c->di, mrd16(c, srcseg, c->si));
        else   mwr8(c, c->es, c->di, mrd8(c, srcseg, c->si));
        c->si = (uint16_t)(c->si + delta);
        c->di = (uint16_t)(c->di + delta);
        if (repeat) c->cx.x--;
    } while (repeat && c->cx.x != 0);
}
static void do_stos(X86Cpu *c, bool w) {
    int delta = (get_flag(c, X86_FLAG_DF) ? -1 : 1) * (w ? 2 : 1);
    bool repeat = g_rep || g_repne;
    do {
        if (repeat && c->cx.x == 0) break;
        if (w) mwr16(c, c->es, c->di, c->ax.x); else mwr8(c, c->es, c->di, c->ax.b.lo);
        c->di = (uint16_t)(c->di + delta);
        if (repeat) c->cx.x--;
    } while (repeat && c->cx.x != 0);
}
static void do_lods(X86Cpu *c, bool w) {
    int delta = (get_flag(c, X86_FLAG_DF) ? -1 : 1) * (w ? 2 : 1);
    uint16_t srcseg = g_seg_override ? g_override_seg : c->ds;
    bool repeat = g_rep || g_repne;
    do {
        if (repeat && c->cx.x == 0) break;
        if (w) c->ax.x = mrd16(c, srcseg, c->si); else c->ax.b.lo = mrd8(c, srcseg, c->si);
        c->si = (uint16_t)(c->si + delta);
        if (repeat) c->cx.x--;
    } while (repeat && c->cx.x != 0);
}
static void do_cmps(X86Cpu *c, bool w) {
    int delta = (get_flag(c, X86_FLAG_DF) ? -1 : 1) * (w ? 2 : 1);
    uint16_t srcseg = g_seg_override ? g_override_seg : c->ds;
    bool repeat = g_rep || g_repne;
    for (;;) {
        if (repeat && c->cx.x == 0) break;
        uint32_t a = w ? mrd16(c, srcseg, c->si) : mrd8(c, srcseg, c->si);
        uint32_t b = w ? mrd16(c, c->es, c->di)  : mrd8(c, c->es, c->di);
        alu_op(c, ALU_CMP, a, b, w);
        c->si = (uint16_t)(c->si + delta);
        c->di = (uint16_t)(c->di + delta);
        if (!repeat) break;
        c->cx.x--;
        bool zf = get_flag(c, X86_FLAG_ZF);
        if ((g_rep && !zf) || (g_repne && zf)) break;
    }
}
static void do_scas(X86Cpu *c, bool w) {
    int delta = (get_flag(c, X86_FLAG_DF) ? -1 : 1) * (w ? 2 : 1);
    bool repeat = g_rep || g_repne;
    for (;;) {
        if (repeat && c->cx.x == 0) break;
        uint32_t a = w ? c->ax.x : c->ax.b.lo;
        uint32_t b = w ? mrd16(c, c->es, c->di) : mrd8(c, c->es, c->di);
        alu_op(c, ALU_CMP, a, b, w);
        c->di = (uint16_t)(c->di + delta);
        if (!repeat) break;
        c->cx.x--;
        bool zf = get_flag(c, X86_FLAG_ZF);
        if ((g_rep && !zf) || (g_repne && zf)) break;
    }
}

/* ── Interrupts ──────────────────────────────────────────────────────────── */

static void deliver_interrupt(X86Cpu *c, uint8_t vec) {
    push16(c, c->flags);
    push16(c, c->cs);
    push16(c, c->ip);
    set_flag(c, X86_FLAG_IF, false);
    set_flag(c, X86_FLAG_TF, false);
    uint16_t new_ip = mrd16(c, 0, (uint16_t)(vec * 4));
    uint16_t new_cs = mrd16(c, 0, (uint16_t)(vec * 4 + 2));
    c->ip = new_ip;
    c->cs = new_cs;
}

void x86_request_irq(X86Cpu *c, uint8_t vector) {
    c->irq_pending = true;
    c->irq_vector = vector;
}

/* ── Jcc condition table (0x70-0x7F order: O,NO,B,AE,E,NE,BE,A,S,NS,P,NP,L,GE,LE,G) */

static bool cond_true(const X86Cpu *c, unsigned cc) {
    bool cf = get_flag(c, X86_FLAG_CF), zf = get_flag(c, X86_FLAG_ZF);
    bool sf = get_flag(c, X86_FLAG_SF), of = get_flag(c, X86_FLAG_OF), pf = get_flag(c, X86_FLAG_PF);
    switch (cc & 0xF) {
    case 0x0: return of;
    case 0x1: return !of;
    case 0x2: return cf;
    case 0x3: return !cf;
    case 0x4: return zf;
    case 0x5: return !zf;
    case 0x6: return cf || zf;
    case 0x7: return !cf && !zf;
    case 0x8: return sf;
    case 0x9: return !sf;
    case 0xA: return pf;
    case 0xB: return !pf;
    case 0xC: return sf != of;
    case 0xD: return sf == of;
    case 0xE: return zf || (sf != of);
    default:  return !zf && (sf == of);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void x86_reset(X86Cpu *c) {
    memset(&c->ax, 0, (size_t)((char*)&c->flags - (char*)&c->ax) + sizeof c->flags);
    c->cs = 0xFFFF;
    c->ip = 0x0000;
    c->ds = c->es = c->ss = 0x0000;
    c->sp = 0x0000;
    c->flags = X86_FLAGS_FIXED_ON;
    c->halted = false;
    c->irq_pending = false;
    c->insn_count = 0;
}

unsigned x86_step(X86Cpu *c) {
    if (c->irq_pending && (c->flags & X86_FLAG_IF)) {
        c->irq_pending = false;
        c->halted = false;
        deliver_interrupt(c, c->irq_vector);
        return 61;
    }
    if (c->halted) return 2;

    g_seg_override = false; g_override_seg = 0; g_rep = false; g_repne = false;
    uint8_t op;
    for (;;) {
        op = fetch8(c);
        switch (op) {
        case 0x26: g_seg_override = true; g_override_seg = c->es; continue;
        case 0x2E: g_seg_override = true; g_override_seg = c->cs; continue;
        case 0x36: g_seg_override = true; g_override_seg = c->ss; continue;
        case 0x3E: g_seg_override = true; g_override_seg = c->ds; continue;
        case 0xF0: continue; /* LOCK - no-op, no bus arbitration to model */
        case 0xF2: g_repne = true; continue;
        case 0xF3: g_rep = true; continue;
        default: break;
        }
        break;
    }
    c->insn_count++;

    switch (op) {
    /* ── ALU groups: ADD/OR/ADC/SBB/AND/SUB/XOR/CMP, 6 forms each ──────── */
#define ALU_GROUP(base, idx) \
        case (base) + 0: alu_rm_rm8(c, (idx), false); break; \
        case (base) + 1: alu_rm_rm16(c, (idx), false); break; \
        case (base) + 2: alu_rm_rm8(c, (idx), true); break; \
        case (base) + 3: alu_rm_rm16(c, (idx), true); break; \
        case (base) + 4: alu_al_imm8(c, (idx)); break; \
        case (base) + 5: alu_ax_imm16(c, (idx)); break;
    ALU_GROUP(0x00, ALU_ADD)
    ALU_GROUP(0x08, ALU_OR)
    ALU_GROUP(0x10, ALU_ADC)
    ALU_GROUP(0x18, ALU_SBB)
    ALU_GROUP(0x20, ALU_AND)
    ALU_GROUP(0x28, ALU_SUB)
    ALU_GROUP(0x30, ALU_XOR)
    ALU_GROUP(0x38, ALU_CMP)
#undef ALU_GROUP

    case 0x06: push16(c, c->es); break;
    case 0x07: c->es = pop16(c); break;
    case 0x0E: push16(c, c->cs); break;
    case 0x0F: c->cs = pop16(c); break; /* POP CS: real but unusual 8086 opcode */
    case 0x16: push16(c, c->ss); break;
    case 0x17: c->ss = pop16(c); break;
    case 0x1E: push16(c, c->ds); break;
    case 0x1F: c->ds = pop16(c); break;

    case 0x27: { /* DAA */
        uint8_t old_al = c->ax.b.lo; bool old_cf = get_flag(c, X86_FLAG_CF);
        bool af = get_flag(c, X86_FLAG_AF), cf;
        uint8_t al = old_al;
        if (((al & 0x0F) > 9) || af) { al = (uint8_t)(al + 6); af = true; } else af = false;
        if ((old_al > 0x99) || old_cf) { al = (uint8_t)(al + 0x60); cf = true; } else cf = false;
        c->ax.b.lo = al;
        set_flag(c, X86_FLAG_CF, cf); set_flag(c, X86_FLAG_AF, af);
        set_flag(c, X86_FLAG_ZF, al == 0); set_flag(c, X86_FLAG_SF, (al & 0x80) != 0);
        set_flag(c, X86_FLAG_PF, parity8(al));
        break;
    }
    case 0x2F: { /* DAS */
        uint8_t old_al = c->ax.b.lo; bool old_cf = get_flag(c, X86_FLAG_CF);
        bool af = get_flag(c, X86_FLAG_AF), cf;
        uint8_t al = old_al;
        if (((al & 0x0F) > 9) || af) { al = (uint8_t)(al - 6); af = true; } else af = false;
        if ((old_al > 0x99) || old_cf) { al = (uint8_t)(al - 0x60); cf = true; } else cf = false;
        c->ax.b.lo = al;
        set_flag(c, X86_FLAG_CF, cf); set_flag(c, X86_FLAG_AF, af);
        set_flag(c, X86_FLAG_ZF, al == 0); set_flag(c, X86_FLAG_SF, (al & 0x80) != 0);
        set_flag(c, X86_FLAG_PF, parity8(al));
        break;
    }
    case 0x37: { /* AAA */
        uint8_t al = c->ax.b.lo, ah = c->ax.b.hi; bool af;
        if (((al & 0x0F) > 9) || get_flag(c, X86_FLAG_AF)) { al = (uint8_t)(al + 6); ah = (uint8_t)(ah + 1); af = true; }
        else af = false;
        c->ax.b.lo = (uint8_t)(al & 0x0F); c->ax.b.hi = ah;
        set_flag(c, X86_FLAG_AF, af); set_flag(c, X86_FLAG_CF, af);
        break;
    }
    case 0x3F: { /* AAS */
        uint8_t al = c->ax.b.lo, ah = c->ax.b.hi; bool af;
        if (((al & 0x0F) > 9) || get_flag(c, X86_FLAG_AF)) { al = (uint8_t)(al - 6); ah = (uint8_t)(ah - 1); af = true; }
        else af = false;
        c->ax.b.lo = (uint8_t)(al & 0x0F); c->ax.b.hi = ah;
        set_flag(c, X86_FLAG_AF, af); set_flag(c, X86_FLAG_CF, af);
        break;
    }

    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47: {
        uint16_t *r = reg16_ptr(c, op - 0x40);
        *r = (uint16_t)do_inc(c, *r, true);
        break;
    }
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        uint16_t *r = reg16_ptr(c, op - 0x48);
        *r = (uint16_t)do_dec(c, *r, true);
        break;
    }
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        push16(c, *reg16_ptr(c, op - 0x50));
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        *reg16_ptr(c, op - 0x58) = pop16(c);
        break;

    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t rel = (int8_t)fetch8(c);
        if (cond_true(c, (unsigned)(op - 0x70))) c->ip = (uint16_t)(c->ip + rel);
        break;
    }

    case 0x80: case 0x82: { ModRM m = decode_modrm(c); uint8_t imm = fetch8(c);
        uint8_t r = (uint8_t)alu_op(c, m.reg, rm_read8(c, &m), imm, false);
        if (m.reg != ALU_CMP) { rm_write8(c, &m, r); }
        break; }
    case 0x81: { ModRM m = decode_modrm(c); uint16_t imm = fetch16(c);
        uint16_t r = (uint16_t)alu_op(c, m.reg, rm_read16(c, &m), imm, true);
        if (m.reg != ALU_CMP) { rm_write16(c, &m, r); }
        break; }
    case 0x83: { ModRM m = decode_modrm(c); uint16_t imm = (uint16_t)(int16_t)(int8_t)fetch8(c);
        uint16_t r = (uint16_t)alu_op(c, m.reg, rm_read16(c, &m), imm, true);
        if (m.reg != ALU_CMP) { rm_write16(c, &m, r); }
        break; }

    case 0x84: { ModRM m = decode_modrm(c); uint8_t r = (uint8_t)(rm_read8(c,&m) & *reg8_ptr(c,m.reg)); logic_flags(c,r,false); break; }
    case 0x85: { ModRM m = decode_modrm(c); uint16_t r = (uint16_t)(rm_read16(c,&m) & *reg16_ptr(c,m.reg)); logic_flags(c,r,true); break; }
    case 0x86: { ModRM m = decode_modrm(c); uint8_t *r = reg8_ptr(c,m.reg); uint8_t t = rm_read8(c,&m); rm_write8(c,&m,*r); *r = t; break; }
    case 0x87: { ModRM m = decode_modrm(c); uint16_t *r = reg16_ptr(c,m.reg); uint16_t t = rm_read16(c,&m); rm_write16(c,&m,*r); *r = t; break; }

    case 0x88: { ModRM m = decode_modrm(c); rm_write8(c, &m, *reg8_ptr(c, m.reg)); break; }
    case 0x89: { ModRM m = decode_modrm(c); rm_write16(c, &m, *reg16_ptr(c, m.reg)); break; }
    case 0x8A: { ModRM m = decode_modrm(c); *reg8_ptr(c, m.reg) = rm_read8(c, &m); break; }
    case 0x8B: { ModRM m = decode_modrm(c); *reg16_ptr(c, m.reg) = rm_read16(c, &m); break; }
    case 0x8C: { ModRM m = decode_modrm(c); rm_write16(c, &m, *seg_ptr(c, m.reg)); break; }
    case 0x8D: { ModRM m = decode_modrm(c); *reg16_ptr(c, m.reg) = m.mem ? m.off : 0; break; } /* LEA */
    case 0x8E: { ModRM m = decode_modrm(c); *seg_ptr(c, m.reg) = rm_read16(c, &m); break; }
    case 0x8F: { ModRM m = decode_modrm(c); rm_write16(c, &m, pop16(c)); break; }

    case 0x90: break; /* NOP */
    case 0x91: case 0x92: case 0x93: case 0x94:
    case 0x95: case 0x96: case 0x97: {
        uint16_t *r = reg16_ptr(c, op - 0x90); uint16_t t = c->ax.x; c->ax.x = *r; *r = t; break;
    }
    case 0x98: c->ax.b.hi = (c->ax.b.lo & 0x80) ? 0xFF : 0x00; break; /* CBW */
    case 0x99: c->dx.x = (c->ax.x & 0x8000) ? 0xFFFF : 0x0000; break; /* CWD */
    case 0x9A: { uint16_t new_ip = fetch16(c); uint16_t new_cs = fetch16(c);
        push16(c, c->cs); push16(c, c->ip); c->cs = new_cs; c->ip = new_ip; break; } /* CALL far */
    case 0x9B: break; /* WAIT: no coprocessor to synchronize with */
    case 0x9C: push16(c, c->flags); break;
    case 0x9D: c->flags = (uint16_t)((pop16(c) | X86_FLAGS_FIXED_ON) & ~X86_FLAGS_FIXED_OFF); break;
    case 0x9E: c->flags = (uint16_t)((c->flags & 0xFF00) | c->ax.b.hi | X86_FLAGS_FIXED_ON); break; /* SAHF */
    case 0x9F: c->ax.b.hi = (uint8_t)(c->flags & 0xFF); break; /* LAHF */

    case 0xA0: c->ax.b.lo = mrd8(c, g_seg_override ? g_override_seg : c->ds, fetch16(c)); break;
    case 0xA1: c->ax.x = mrd16(c, g_seg_override ? g_override_seg : c->ds, fetch16(c)); break;
    case 0xA2: mwr8(c, g_seg_override ? g_override_seg : c->ds, fetch16(c), c->ax.b.lo); break;
    case 0xA3: mwr16(c, g_seg_override ? g_override_seg : c->ds, fetch16(c), c->ax.x); break;
    case 0xA4: do_movs(c, false); break;
    case 0xA5: do_movs(c, true); break;
    case 0xA6: do_cmps(c, false); break;
    case 0xA7: do_cmps(c, true); break;
    case 0xA8: { uint8_t imm = fetch8(c); logic_flags(c, (uint8_t)(c->ax.b.lo & imm), false); break; }
    case 0xA9: { uint16_t imm = fetch16(c); logic_flags(c, (uint16_t)(c->ax.x & imm), true); break; }
    case 0xAA: do_stos(c, false); break;
    case 0xAB: do_stos(c, true); break;
    case 0xAC: do_lods(c, false); break;
    case 0xAD: do_lods(c, true); break;
    case 0xAE: do_scas(c, false); break;
    case 0xAF: do_scas(c, true); break;

    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        *reg8_ptr(c, op - 0xB0) = fetch8(c); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        *reg16_ptr(c, op - 0xB8) = fetch16(c); break;

    case 0xC2: { uint16_t n = fetch16(c); c->ip = pop16(c); c->sp = (uint16_t)(c->sp + n); break; }
    case 0xC3: c->ip = pop16(c); break;
    case 0xC4: { ModRM m = decode_modrm(c); uint16_t off = rm_read16(c, &m); uint16_t seg = mrd16(c, m.seg, (uint16_t)(m.off + 2));
        *reg16_ptr(c, m.reg) = off; c->es = seg; break; }
    case 0xC5: { ModRM m = decode_modrm(c); uint16_t off = rm_read16(c, &m); uint16_t seg = mrd16(c, m.seg, (uint16_t)(m.off + 2));
        *reg16_ptr(c, m.reg) = off; c->ds = seg; break; }
    case 0xC6: { ModRM m = decode_modrm(c); rm_write8(c, &m, fetch8(c)); break; }
    case 0xC7: { ModRM m = decode_modrm(c); rm_write16(c, &m, fetch16(c)); break; }
    case 0xCA: { uint16_t n = fetch16(c); c->ip = pop16(c); c->cs = pop16(c); c->sp = (uint16_t)(c->sp + n); break; }
    case 0xCB: c->ip = pop16(c); c->cs = pop16(c); break;
    case 0xCC: deliver_interrupt(c, 3); break;
    case 0xCD: deliver_interrupt(c, fetch8(c)); break;
    case 0xCE: if (get_flag(c, X86_FLAG_OF)) deliver_interrupt(c, 4); break;
    case 0xCF: c->ip = pop16(c); c->cs = pop16(c);
        c->flags = (uint16_t)((pop16(c) | X86_FLAGS_FIXED_ON) & ~X86_FLAGS_FIXED_OFF); break;

    case 0xD0: { ModRM m = decode_modrm(c); rm_write8(c, &m, (uint8_t)shift_op(c, m.reg, rm_read8(c, &m), 1, false)); break; }
    case 0xD1: { ModRM m = decode_modrm(c); rm_write16(c, &m, (uint16_t)shift_op(c, m.reg, rm_read16(c, &m), 1, true)); break; }
    case 0xD2: { ModRM m = decode_modrm(c); rm_write8(c, &m, (uint8_t)shift_op(c, m.reg, rm_read8(c, &m), c->cx.b.lo, false)); break; }
    case 0xD3: { ModRM m = decode_modrm(c); rm_write16(c, &m, (uint16_t)shift_op(c, m.reg, rm_read16(c, &m), c->cx.b.lo, true)); break; }
    case 0xD4: { uint8_t base = fetch8(c); if (base == 0) { deliver_interrupt(c, 0); break; }
        uint8_t al = c->ax.b.lo, ah = (uint8_t)(al / base); al = (uint8_t)(al % base);
        c->ax.b.hi = ah; c->ax.b.lo = al;
        set_flag(c, X86_FLAG_ZF, al == 0); set_flag(c, X86_FLAG_SF, (al & 0x80) != 0); set_flag(c, X86_FLAG_PF, parity8(al));
        break; }
    case 0xD5: { uint8_t base = fetch8(c); uint8_t res = (uint8_t)(c->ax.b.lo + c->ax.b.hi * base);
        c->ax.b.lo = res; c->ax.b.hi = 0;
        set_flag(c, X86_FLAG_ZF, res == 0); set_flag(c, X86_FLAG_SF, (res & 0x80) != 0); set_flag(c, X86_FLAG_PF, parity8(res));
        break; }
    case 0xD7: c->ax.b.lo = mrd8(c, g_seg_override ? g_override_seg : c->ds, (uint16_t)(c->bx.x + c->ax.b.lo)); break; /* XLAT */
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        decode_modrm(c); /* ESC: no 8087 present; consume the ModRM/disp bytes like real unpopulated hardware */
        break;

    case 0xE0: { int8_t rel = (int8_t)fetch8(c); c->cx.x--; if (c->cx.x != 0 && !get_flag(c, X86_FLAG_ZF)) c->ip = (uint16_t)(c->ip + rel); break; }
    case 0xE1: { int8_t rel = (int8_t)fetch8(c); c->cx.x--; if (c->cx.x != 0 && get_flag(c, X86_FLAG_ZF)) c->ip = (uint16_t)(c->ip + rel); break; }
    case 0xE2: { int8_t rel = (int8_t)fetch8(c); c->cx.x--; if (c->cx.x != 0) c->ip = (uint16_t)(c->ip + rel); break; }
    case 0xE3: { int8_t rel = (int8_t)fetch8(c); if (c->cx.x == 0) c->ip = (uint16_t)(c->ip + rel); break; }
    case 0xE4: c->ax.b.lo = c->io_read8(fetch8(c), c->bus_ud); break;
    case 0xE5: { uint8_t p = fetch8(c); c->ax.b.lo = c->io_read8(p, c->bus_ud); c->ax.b.hi = c->io_read8((uint8_t)(p + 1), c->bus_ud); break; }
    case 0xE6: c->io_write8(fetch8(c), c->ax.b.lo, c->bus_ud); break;
    case 0xE7: { uint8_t p = fetch8(c); c->io_write8(p, c->ax.b.lo, c->bus_ud); c->io_write8((uint8_t)(p + 1), c->ax.b.hi, c->bus_ud); break; }
    case 0xE8: { int16_t rel = (int16_t)fetch16(c); push16(c, c->ip); c->ip = (uint16_t)(c->ip + rel); break; }
    case 0xE9: { int16_t rel = (int16_t)fetch16(c); c->ip = (uint16_t)(c->ip + rel); break; }
    case 0xEA: { uint16_t new_ip = fetch16(c); uint16_t new_cs = fetch16(c); c->ip = new_ip; c->cs = new_cs; break; }
    case 0xEB: { int8_t rel = (int8_t)fetch8(c); c->ip = (uint16_t)(c->ip + rel); break; }
    case 0xEC: c->ax.b.lo = c->io_read8(c->dx.x, c->bus_ud); break;
    case 0xED: c->ax.b.lo = c->io_read8(c->dx.x, c->bus_ud); c->ax.b.hi = c->io_read8((uint16_t)(c->dx.x + 1), c->bus_ud); break;
    case 0xEE: c->io_write8(c->dx.x, c->ax.b.lo, c->bus_ud); break;
    case 0xEF: c->io_write8(c->dx.x, c->ax.b.lo, c->bus_ud); c->io_write8((uint16_t)(c->dx.x + 1), c->ax.b.hi, c->bus_ud); break;

    case 0xF4: c->halted = true; break;
    case 0xF5: set_flag(c, X86_FLAG_CF, !get_flag(c, X86_FLAG_CF)); break;
    case 0xF6: { ModRM m = decode_modrm(c); uint8_t v = rm_read8(c, &m);
        switch (m.reg) {
        case 0: case 1: { uint8_t imm = fetch8(c); logic_flags(c, (uint8_t)(v & imm), false); break; }
        case 2: rm_write8(c, &m, (uint8_t)~v); break;
        case 3: rm_write8(c, &m, (uint8_t)alu_op(c, ALU_SUB, 0, v, false)); break;
        case 4: { uint16_t r = (uint16_t)(c->ax.b.lo * (unsigned)v); c->ax.x = r;
            bool cf = r > 0xFF; set_flag(c, X86_FLAG_CF, cf); set_flag(c, X86_FLAG_OF, cf); break; }
        case 5: { int16_t r = (int16_t)((int8_t)c->ax.b.lo * (int)(int8_t)v); c->ax.x = (uint16_t)r;
            bool ov = r < -128 || r > 127; set_flag(c, X86_FLAG_CF, ov); set_flag(c, X86_FLAG_OF, ov); break; }
        case 6: { if (v == 0) { deliver_interrupt(c, 0); break; } unsigned q = c->ax.x / v, r = c->ax.x % v;
            if (q > 0xFF) { deliver_interrupt(c, 0); break; } c->ax.b.lo = (uint8_t)q; c->ax.b.hi = (uint8_t)r; break; }
        default: { if (v == 0) { deliver_interrupt(c, 0); break; } int16_t dd = (int16_t)c->ax.x;
            int q = dd / (int8_t)v, r = dd % (int8_t)v;
            if (q > 127 || q < -128) { deliver_interrupt(c, 0); break; }
            c->ax.b.lo = (uint8_t)(int8_t)q; c->ax.b.hi = (uint8_t)(int8_t)r; break; }
        }
        break; }
    case 0xF7: { ModRM m = decode_modrm(c); uint16_t v = rm_read16(c, &m);
        switch (m.reg) {
        case 0: case 1: { uint16_t imm = fetch16(c); logic_flags(c, (uint16_t)(v & imm), true); break; }
        case 2: rm_write16(c, &m, (uint16_t)~v); break;
        case 3: rm_write16(c, &m, (uint16_t)alu_op(c, ALU_SUB, 0, v, true)); break;
        case 4: { uint32_t r = (uint32_t)c->ax.x * v; c->ax.x = (uint16_t)r; c->dx.x = (uint16_t)(r >> 16);
            bool cf = r > 0xFFFF; set_flag(c, X86_FLAG_CF, cf); set_flag(c, X86_FLAG_OF, cf); break; }
        case 5: { int32_t r = (int32_t)(int16_t)c->ax.x * (int32_t)(int16_t)v;
            c->ax.x = (uint16_t)r; c->dx.x = (uint16_t)((uint32_t)r >> 16);
            bool ov = r < -32768 || r > 32767; set_flag(c, X86_FLAG_CF, ov); set_flag(c, X86_FLAG_OF, ov); break; }
        case 6: { if (v == 0) { deliver_interrupt(c, 0); break; } uint32_t n = ((uint32_t)c->dx.x << 16) | c->ax.x;
            uint32_t q = n / v, r = n % v; if (q > 0xFFFF) { deliver_interrupt(c, 0); break; }
            c->ax.x = (uint16_t)q; c->dx.x = (uint16_t)r; break; }
        default: { if (v == 0) { deliver_interrupt(c, 0); break; } int32_t n = (int32_t)(((uint32_t)c->dx.x << 16) | c->ax.x);
            int32_t q = n / (int16_t)v, r = n % (int16_t)v;
            if (q > 32767 || q < -32768) { deliver_interrupt(c, 0); break; }
            c->ax.x = (uint16_t)(int16_t)q; c->dx.x = (uint16_t)(int16_t)r; break; }
        }
        break; }
    case 0xF8: set_flag(c, X86_FLAG_CF, false); break;
    case 0xF9: set_flag(c, X86_FLAG_CF, true); break;
    case 0xFA: set_flag(c, X86_FLAG_IF, false); break;
    case 0xFB: set_flag(c, X86_FLAG_IF, true); break;
    case 0xFC: set_flag(c, X86_FLAG_DF, false); break;
    case 0xFD: set_flag(c, X86_FLAG_DF, true); break;
    case 0xFE: { ModRM m = decode_modrm(c);
        if (m.reg == 0) rm_write8(c, &m, (uint8_t)do_inc(c, rm_read8(c, &m), false));
        else if (m.reg == 1) rm_write8(c, &m, (uint8_t)do_dec(c, rm_read8(c, &m), false));
        else if (c->on_unknown_opcode) c->on_unknown_opcode(c, op, c->bus_ud);
        break; }
    case 0xFF: { ModRM m = decode_modrm(c);
        switch (m.reg) {
        case 0: rm_write16(c, &m, (uint16_t)do_inc(c, rm_read16(c, &m), true)); break;
        case 1: rm_write16(c, &m, (uint16_t)do_dec(c, rm_read16(c, &m), true)); break;
        case 2: { uint16_t t = rm_read16(c, &m); push16(c, c->ip); c->ip = t; break; }             /* CALL near indirect */
        case 3: { uint16_t off = rm_read16(c, &m); uint16_t seg = mrd16(c, m.seg, (uint16_t)(m.off + 2));
            push16(c, c->cs); push16(c, c->ip); c->cs = seg; c->ip = off; break; }                  /* CALL far indirect */
        case 4: c->ip = rm_read16(c, &m); break;                                                    /* JMP near indirect */
        case 5: { uint16_t off = rm_read16(c, &m); uint16_t seg = mrd16(c, m.seg, (uint16_t)(m.off + 2));
            c->cs = seg; c->ip = off; break; }                                                      /* JMP far indirect */
        case 6: push16(c, rm_read16(c, &m)); break;                                                 /* PUSH r/m16 */
        default: if (c->on_unknown_opcode) c->on_unknown_opcode(c, op, c->bus_ud); break;
        }
        break; }

    default:
        if (c->on_unknown_opcode) c->on_unknown_opcode(c, op, c->bus_ud);
        break;
    }

    return 4; /* placeholder average - not cycle-exact this pass */
}
