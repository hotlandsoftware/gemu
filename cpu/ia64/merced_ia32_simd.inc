/*
 * MMX and SSE (Pentium III level) for Merced's IA-32 engine.  #included by
 * merced_ia32.c, after its helpers.
 *
 * Itanium state mapping (SDM Vol.1 6.2.2; the same as Linux's ia64 IA-32
 * support): MM0-MM7 are the significands of FR8-FR15 - an MMX write sets the
 * exponent to 0x1FFFF and the sign, as the x87 view of the register expects -
 * and XMM0-XMM7 are FR16-FR31 in pairs (low half first), with the integer
 * exponent 0x1003E.  MXCSR lives at +32 in FCR (control, bits 15:7) and FSR
 * (flags, bits 5:0).
 *
 * Single-precision arithmetic runs on the host's IEEE float with the guest's
 * rounding mode; the x86-specific parts (NaN propagation, the default NaN,
 * denormal-operand detection, flush-to-zero, exception masking and #XM) are
 * done here, so the results do not depend on the host's own rules.
 * RCPPS/RSQRTPS return correctly rounded values, more precise than the 12-bit
 * approximations of real hardware.
 */
#include <fenv.h>

enum { SIMD_DONE, SIMD_FAULT, SIMD_RETURN, SIMD_NONE };

typedef struct { uint64_t lo, hi; } X128;

/* ---- register state ------------------------------------------------------ */

static uint64_t mm_get(Merced *m, unsigned i) { return m->fr[8 + (i & 7)].sig; }
static void mm_set(Merced *m, unsigned i, uint64_t v) {
    m->fr[8 + (i & 7)] = (MercedFpReg){ .sig = v, .exp = 0x1ffff, .sign = 1 };
}
/* Every MMX instruction but EMMS: TOS = 0 and all eight tags valid. */
static void mmx_state(Merced *m) {
    x87_set_top(m, 0);
    x87_set_empty_tags(m, 0);
}
static X128 xmm_get(Merced *m, unsigned i) {
    return (X128){ m->fr[16 + 2 * (i & 7)].sig, m->fr[17 + 2 * (i & 7)].sig };
}
static void xmm_set(Merced *m, unsigned i, X128 v) {
    m->fr[16 + 2 * (i & 7)] = (MercedFpReg){ .sig = v.lo, .exp = 0x1003e };
    m->fr[17 + 2 * (i & 7)] = (MercedFpReg){ .sig = v.hi, .exp = 0x1003e };
}
static uint32_t x128_f(X128 v, unsigned i) {
    return (uint32_t)((i < 2 ? v.lo : v.hi) >> (32 * (i & 1)));
}
static void x128_setf(X128 *v, unsigned i, uint32_t f) {
    uint64_t *h = i < 2 ? &v->lo : &v->hi;
    unsigned s = 32 * (i & 1);
    *h = (*h & ~(UINT64_C(0xffffffff) << s)) | ((uint64_t)f << s);
}

enum { MX_IE = 1, MX_DE = 2, MX_ZE = 4, MX_OE = 8, MX_UE = 16, MX_PE = 32,
       MX_FZ = 0x8000 };
static uint32_t mxcsr(Merced *m) {
    return (uint32_t)((m->ar[21] >> 32) & 0xff80) | (uint32_t)((m->ar[28] >> 32) & 0x3f);
}
static void set_mxcsr(Merced *m, uint32_t v) {
    m->ar[21] = (m->ar[21] & ~(UINT64_C(0xffff) << 32)) | ((uint64_t)(v & 0xff80) << 32);
    m->ar[28] = (m->ar[28] & ~(UINT64_C(0x3f) << 32)) | ((uint64_t)(v & 0x3f) << 32);
}

/* ---- availability -------------------------------------------------------- */

/* MMX: #UD with CR0.EM, #NM with CR0.TS, #MF for a pending x87 exception. */
static bool mmx_ready(X86 *x, MercedStatus *st) {
    if (cr0(x) & 4) { *st = ia32_ud(x); return false; }
    return x87_ready(x, true, false, st);
}
/* SSE: #UD with CR0.EM or without CR4.OSFXSR, #NM with CR0.TS. */
static bool sse_ready(X86 *x, MercedStatus *st) {
    if (x->m->psr & (UINT64_C(1) << 18)) {      /* PSR.dfl: FR8-FR31 disabled */
        *st = merced_ia32_disabled_fp(x->m);
        return false;
    }
    if ((cr0(x) & 4) || !(cr4(x) & 0x200)) { *st = ia32_ud(x); return false; }
    if (cr0(x) & 8) { *st = merced_ia32_fp_exception(x->m, 7); return false; }
    return true;
}

/* ---- memory operands ----------------------------------------------------- */

static bool mem_read(X86 *x, uint32_t addr, unsigned n, void *out) {
    uint8_t buf[16];
    if (!x87_read_bytes(x->m, addr, n, buf)) return false;
    memcpy(out, buf, n);
    return true;
}
static bool mem_write(X86 *x, uint32_t addr, unsigned n, const void *in) {
    /* Probe the whole operand first: a page-split store must not land half. */
    if (!x87_probe_bytes(x->m, addr, n)) return false;
    return x87_write_bytes(x->m, addr, n, in);
}

/* ---- single precision ---------------------------------------------------- */

#define F32_QNAN_BIT  0x00400000u
#define F32_DEFAULT_NAN 0xffc00000u    /* the "real indefinite" */
static bool f32_nan(uint32_t a) { return (a & 0x7f800000) == 0x7f800000 && (a & 0x7fffff); }
static bool f32_snan(uint32_t a) { return f32_nan(a) && !(a & F32_QNAN_BIT); }
static bool f32_denormal(uint32_t a) { return !(a & 0x7f800000) && (a & 0x7fffff); }
static bool f32_zero(uint32_t a) { return !(a & 0x7fffffff); }
static float f32_v(uint32_t a) { float f; memcpy(&f, &a, 4); return f; }
static uint32_t f32_b(float f) { uint32_t a; memcpy(&a, &f, 4); return a; }

static const int rc_mode[4] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };

/* Host exception flags of an operation, as MXCSR flags. */
static unsigned host_flags(int e) {
    return (e & FE_INVALID ? MX_IE : 0) | (e & FE_DIVBYZERO ? MX_ZE : 0) |
           (e & FE_OVERFLOW ? MX_OE : 0) | (e & FE_UNDERFLOW ? MX_UE : 0) |
           (e & FE_INEXACT ? MX_PE : 0);
}

enum { F_ADD, F_SUB, F_MUL, F_DIV, F_SQRT, F_MIN, F_MAX };

/* One element of ADD/SUB/MUL/DIV/SQRT/MIN/MAX (a is the destination, first
 * operand; SQRT uses b only). */
static uint32_t f32_op(unsigned op, uint32_t a, uint32_t b, uint32_t csr, unsigned *flags) {
    if (op == F_MIN || op == F_MAX) {
        /* Any NaN (quiet too) is invalid and returns the second operand,
         * as does an equal pair (so -0/+0 order matters). */
        if (f32_nan(a) || f32_nan(b)) { *flags |= MX_IE; return b; }
        if (f32_denormal(a) || f32_denormal(b)) *flags |= MX_DE;
        float fa = f32_v(a), fb = f32_v(b);
        return (op == F_MIN ? fa < fb : fa > fb) ? a : b;
    }
    if (op == F_SQRT) {
        if (f32_nan(b)) {
            if (f32_snan(b)) *flags |= MX_IE;
            return b | F32_QNAN_BIT;
        }
        if (f32_denormal(b)) *flags |= MX_DE;
    } else if (f32_nan(a) || f32_nan(b)) {
        /* The first operand's NaN wins, quieted. */
        if (f32_snan(a) || f32_snan(b)) *flags |= MX_IE;
        return (f32_nan(a) ? a : b) | F32_QNAN_BIT;
    } else if (f32_denormal(a) || f32_denormal(b)) *flags |= MX_DE;

    fenv_t saved;
    fegetenv(&saved);
    fesetround(rc_mode[(csr >> 13) & 3]);
    feclearexcept(FE_ALL_EXCEPT);
    volatile float va = f32_v(a), vb = f32_v(b), r;
    switch (op) {
    case F_ADD: r = va + vb; break;
    case F_SUB: r = va - vb; break;
    case F_MUL: r = va * vb; break;
    case F_DIV: r = va / vb; break;
    default:    r = sqrtf(vb); break;
    }
    unsigned f = host_flags(fetestexcept(FE_ALL_EXCEPT));
    fesetenv(&saved);
    uint32_t res = f32_b(r);
    if (f & MX_IE) res = F32_DEFAULT_NAN;
    if ((csr & MX_FZ) && (csr & (MX_UE << 7)) && f32_denormal(res)) {
        res &= 0x80000000;                      /* flush to zero */
        f |= MX_UE | MX_PE;
    }
    *flags |= f;
    return res;
}

/* RCP/RSQRT approximations: no flags; denormal inputs count as zero and
 * results too small to be normal are flushed, like the hardware tables. */
static uint32_t f32_approx(bool rsqrt, uint32_t a) {
    uint32_t sign = a & 0x80000000;
    if (f32_nan(a)) return a | F32_QNAN_BIT;
    if (f32_zero(a) || f32_denormal(a)) return sign | 0x7f800000;
    if ((a & 0x7fffffff) == 0x7f800000) return rsqrt && sign ? F32_DEFAULT_NAN : sign;
    if (rsqrt && sign) return F32_DEFAULT_NAN;
    double v = (double)f32_v(a);
    float r = (float)(rsqrt ? 1.0 / sqrt(v) : 1.0 / v);
    uint32_t res = f32_b(r);
    if (f32_denormal(res)) res &= 0x80000000;
    return res;
}

/* CMPPS predicates 0-7: EQ LT LE UNORD NEQ NLT NLE ORD. */
static uint32_t f32_cmp(unsigned pred, uint32_t a, uint32_t b, unsigned *flags) {
    bool unordered = f32_nan(a) || f32_nan(b);
    bool signaling = pred == 1 || pred == 2 || pred == 5 || pred == 6;
    if (unordered ? (signaling || f32_snan(a) || f32_snan(b)) : false) *flags |= MX_IE;
    if (f32_denormal(a) || f32_denormal(b)) *flags |= MX_DE;
    float fa = f32_v(a), fb = f32_v(b);
    bool r;
    switch (pred & 7) {
    case 0: r = !unordered && fa == fb; break;
    case 1: r = !unordered && fa < fb; break;
    case 2: r = !unordered && fa <= fb; break;
    case 3: r = unordered; break;
    case 4: r = unordered || fa != fb; break;
    case 5: r = unordered || !(fa < fb); break;
    case 6: r = unordered || !(fa <= fb); break;
    default: r = !unordered; break;
    }
    return r ? UINT32_MAX : 0;
}

/* float -> int32: RC rounding, or truncation; out of range is invalid and
 * gives the integer indefinite 0x80000000. */
static uint32_t f32_to_i32(uint32_t a, bool truncate, uint32_t csr, unsigned *flags) {
    if (f32_nan(a)) { *flags |= MX_IE; return 0x80000000u; }
    double v = (double)f32_v(a), r;
    if (truncate) r = trunc(v);
    else {
        fenv_t saved;
        fegetenv(&saved);
        fesetround(rc_mode[(csr >> 13) & 3]);
        r = nearbyint(v);
        fesetenv(&saved);
    }
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) { *flags |= MX_IE; return 0x80000000u; }
    if (r != v) *flags |= MX_PE;
    return (uint32_t)(int32_t)r;
}
static uint32_t i32_to_f32(int32_t v, uint32_t csr, unsigned *flags) {
    fenv_t saved;
    fegetenv(&saved);
    fesetround(rc_mode[(csr >> 13) & 3]);
    feclearexcept(FE_ALL_EXCEPT);
    volatile int32_t vv = v;
    volatile float r = (float)vv;
    *flags |= host_flags(fetestexcept(FE_INEXACT));
    fesetenv(&saved);
    return f32_b(r);
}

/* After all elements: record the flags, or raise #XM (#UD without
 * CR4.OSXMMEXCPT) for an unmasked one - the destination is then unchanged. */
static bool sse_commit(X86 *x, unsigned flags, MercedStatus *st) {
    uint32_t csr = mxcsr(x->m);
    set_mxcsr(x->m, csr | flags);
    if (flags & ~(csr >> 7) & 0x3f) {
        *st = (cr4(x) & 0x400) ? ia32_fault(x, 19, 0) : ia32_ud(x);
        return false;
    }
    return true;
}

/* ---- MMX lanes ----------------------------------------------------------- */

static uint64_t lane(uint64_t v, unsigned i, unsigned w) {
    return w == 64 ? v : (v >> (i * w)) & ((UINT64_C(1) << w) - 1);
}
static int64_t slane(uint64_t v, unsigned i, unsigned w) {
    uint64_t u = lane(v, i, w);
    return w == 64 ? (int64_t)u : (int64_t)(u << (64 - w)) >> (64 - w);
}
static uint64_t put_lane(uint64_t v, unsigned i, unsigned w, uint64_t x) {
    if (w == 64) return x;
    uint64_t mask = ((UINT64_C(1) << w) - 1) << (i * w);
    return (v & ~mask) | ((x << (i * w)) & mask);
}
static int64_t sat_s(int64_t v, unsigned w) {
    int64_t hi = (INT64_C(1) << (w - 1)) - 1, lo = -hi - 1;
    return v > hi ? hi : v < lo ? lo : v;
}
static int64_t sat_u(int64_t v, unsigned w) {
    int64_t hi = (INT64_C(1) << w) - 1;
    return v > hi ? hi : v < 0 ? 0 : v;
}

/* Element-wise ops on two MMX values: element width w. */
enum { L_ADD, L_ADDS, L_ADDUS, L_SUB, L_SUBS, L_SUBUS, L_CMPEQ, L_CMPGT,
       L_MULLO, L_MULHI, L_MULHIU, L_MINU, L_MAXU, L_MINS, L_MAXS, L_AVG };
static uint64_t lanes(unsigned kind, uint64_t a, uint64_t b, unsigned w) {
    uint64_t r = 0;
    for (unsigned i = 0; i < 64 / w; i++) {
        int64_t sa = slane(a, i, w), sb = slane(b, i, w), v;
        int64_t ua = (int64_t)lane(a, i, w), ub = (int64_t)lane(b, i, w);
        switch (kind) {
        case L_ADD:    v = ua + ub; break;
        case L_ADDS:   v = sat_s(sa + sb, w); break;
        case L_ADDUS:  v = sat_u(ua + ub, w); break;
        case L_SUB:    v = ua - ub; break;
        case L_SUBS:   v = sat_s(sa - sb, w); break;
        case L_SUBUS:  v = sat_u(ua - ub, w); break;
        case L_CMPEQ:  v = ua == ub ? -1 : 0; break;
        case L_CMPGT:  v = sa > sb ? -1 : 0; break;
        case L_MULLO:  v = sa * sb; break;
        case L_MULHI:  v = (sa * sb) >> 16; break;
        case L_MULHIU: v = (ua * ub) >> 16; break;
        case L_MINU:   v = ua < ub ? ua : ub; break;
        case L_MAXU:   v = ua > ub ? ua : ub; break;
        case L_MINS:   v = sa < sb ? sa : sb; break;
        case L_MAXS:   v = sa > sb ? sa : sb; break;
        default:       v = (ua + ub + 1) >> 1; break;   /* L_AVG */
        }
        r = put_lane(r, i, w, (uint64_t)v);
    }
    return r;
}
/* Shifts by a count; logical counts >= width clear, arithmetic ones fill. */
static uint64_t lane_shift(unsigned kind, uint64_t a, uint64_t count, unsigned w) {
    uint64_t r = 0;
    for (unsigned i = 0; i < 64 / w; i++) {
        uint64_t v;
        if (kind == 2) v = count >= w ? (uint64_t)(slane(a, i, w) >> (w - 1))
                                      : (uint64_t)(slane(a, i, w) >> count);    /* sra */
        else if (count >= w) v = 0;
        else v = kind == 0 ? lane(a, i, w) >> count : lane(a, i, w) << count;  /* srl, sll */
        r = put_lane(r, i, w, v);
    }
    return r;
}
/* PACKSSWB/PACKSSDW/PACKUSWB: w is the source element width. */
static uint64_t pack(uint64_t a, uint64_t b, unsigned w, bool unsigned_sat) {
    unsigned n = 64 / w, h = w / 2;
    uint64_t r = 0;
    for (unsigned i = 0; i < 2 * n; i++) {
        int64_t v = slane(i < n ? a : b, i % n, w);
        r = put_lane(r, i, h, (uint64_t)(unsigned_sat ? sat_u(v, h) : sat_s(v, h)));
    }
    return r;
}
/* PUNPCKL/H: interleave the low or high halves, a's elements first. */
static uint64_t unpack(uint64_t a, uint64_t b, unsigned w, bool high) {
    unsigned n = 32 / w, off = high ? n : 0;
    uint64_t r = 0;
    for (unsigned i = 0; i < n; i++) {
        r = put_lane(r, 2 * i, w, lane(a, off + i, w));
        r = put_lane(r, 2 * i + 1, w, lane(b, off + i, w));
    }
    return r;
}

/* ---- decoding ------------------------------------------------------------ */

static bool simd_modrm(X86 *x, unsigned *reg, RM *rm, uint8_t *mr) {
    uint32_t q;
    if (!fetch(x, 1, &q)) return false;
    *mr = (uint8_t)q;
    *reg = (q >> 3) & 7;
    return decode_rm(x, (uint8_t)q, rm);
}
/* mm/m64 (or m32 for the PUNPCKL forms) as a 64-bit value. */
static bool mm_src(X86 *x, RM *rm, unsigned bytes, uint64_t *v) {
    if (rm->is_reg) { *v = mm_get(x->m, rm->reg); return true; }
    *v = 0;
    return mem_read(x, rm->addr, bytes, v);
}
/* xmm/m128 (aligned when required) or xmm/m32. */
static bool xmm_src(X86 *x, RM *rm, unsigned bytes, bool aligned, X128 *v, MercedStatus *st, bool *gp) {
    *gp = false;
    if (rm->is_reg) { *v = xmm_get(x->m, rm->reg); return true; }
    if (aligned && (rm->addr & 15)) { *gp = true; *st = ia32_fault(x, 13, 0); return false; }
    uint8_t buf[16] = {0};
    if (!mem_read(x, rm->addr, bytes, buf)) return false;
    memcpy(&v->lo, buf, 8);
    memcpy(&v->hi, buf + 8, 8);
    return true;
}

static bool simd_opcode(uint8_t op2) {
    return (op2 >= 0x10 && op2 <= 0x17) || (op2 >= 0x28 && op2 <= 0x2f) ||
           (op2 >= 0x50 && op2 <= 0x7f) || op2 == 0xae || op2 == 0xc2 ||
           (op2 >= 0xc4 && op2 <= 0xc6) || op2 >= 0xd0;
}

/* Packed/scalar FP arithmetic opcodes 51-5F (no 5A/5B on a P3). */
static int sse_arith_op(uint8_t op2) {
    switch (op2) {
    case 0x51: return F_SQRT; case 0x58: return F_ADD; case 0x59: return F_MUL;
    case 0x5c: return F_SUB; case 0x5d: return F_MIN; case 0x5e: return F_DIV;
    case 0x5f: return F_MAX; default: return -1;
    }
}

/* ---- FXSAVE / FXRSTOR ---------------------------------------------------- */

static int fxsave(X86 *x, RM *rm, bool restore, MercedStatus *st) {
    Merced *m = x->m;
    bool xmm = (cr4(x) & 0x200) != 0;           /* CR4.OSFXSR: SSE state too */
    unsigned len = xmm ? 288 : 160;
    uint8_t buf[288] = {0};
    if (rm->addr & 15) { *st = ia32_fault(x, 13, 0); return SIMD_RETURN; }
    if (!restore) {
        unsigned ftw = 0;
        for (unsigned p = 0; p < 8; p++)
            if (!x87_empty(m, p)) ftw |= 1u << p;
        uint64_t fir = m->ar[29], fdr = m->ar[30];
        x87_put_le(buf + 0, 2, (uint16_t)m->ar[21]);
        x87_put_le(buf + 2, 2, (uint16_t)m->ar[28]);
        buf[4] = (uint8_t)ftw;
        x87_put_le(buf + 6, 2, (fir >> 48) & 0x7ff);
        x87_put_le(buf + 8, 4, (uint32_t)fir);
        x87_put_le(buf + 12, 2, (uint16_t)(fir >> 32));
        x87_put_le(buf + 16, 4, (uint32_t)fdr);
        x87_put_le(buf + 20, 2, (uint16_t)(fdr >> 32));
        if (xmm) x87_put_le(buf + 24, 4, mxcsr(m));
        for (unsigned j = 0; j < 8; j++) {
            MercedFpReg f = m->fr[8 + x87_phys(m, j)];
            x87_put_le(buf + 32 + 16 * j, 8, f.sig);
            x87_put_le(buf + 40 + 16 * j, 2, x87_m80_sign_exp(f));
        }
        if (xmm)
            for (unsigned j = 0; j < 8; j++) {
                X128 v = xmm_get(m, j);
                x87_put_le(buf + 160 + 16 * j, 8, v.lo);
                x87_put_le(buf + 168 + 16 * j, 8, v.hi);
            }
        return mem_write(x, rm->addr, len, buf) ? SIMD_DONE : SIMD_FAULT;
    }
    if (!x87_read_bytes(m, rm->addr, len, buf)) return SIMD_FAULT;
    uint32_t csr = (uint32_t)x87_le(buf + 24, 4);
    if (xmm && (csr & ~0xffbfu)) { *st = ia32_fault(x, 13, 0); return SIMD_RETURN; }
    uint16_t fcw = (uint16_t)((x87_le(buf, 2) & 0x1f3f) | 0x40);
    uint16_t fsw = (uint16_t)x87_le(buf + 2, 2);
    m->ar[21] = (m->ar[21] & ~UINT64_C(0xffff)) | fcw;
    m->ar[28] = (m->ar[28] & ~UINT64_C(0xffff)) | (fsw & 0x7f7f);
    x87_set_empty_tags(m, ~(unsigned)buf[4] & 0xff);
    if (fsw & ~fcw & 0x3f) m->ar[28] |= 0x8080;
    m->ar[29] = ((x87_le(buf + 6, 2) & 0x7ff) << 48) | (x87_le(buf + 12, 2) << 32) |
                x87_le(buf + 8, 4);
    m->ar[30] = (x87_le(buf + 20, 2) << 32) | x87_le(buf + 16, 4);
    for (unsigned j = 0; j < 8; j++)
        m->fr[8 + x87_phys(m, j)] = x87_fr_from_m80(x87_le(buf + 32 + 16 * j, 8),
                                                    (uint16_t)x87_le(buf + 40 + 16 * j, 2));
    if (xmm) {
        set_mxcsr(m, csr);
        for (unsigned j = 0; j < 8; j++)
            xmm_set(m, j, (X128){ x87_le(buf + 160 + 16 * j, 8), x87_le(buf + 168 + 16 * j, 8) });
    }
    return SIMD_DONE;
}

/* ---- dispatch ------------------------------------------------------------ */

/* MMX/SSE opcode 0F op2.  SIMD_NONE for an encoding a Pentium III does not
 * have (the caller raises #UD); SIMD_FAULT for a memory fault. */
static int ia32_simd(X86 *x, uint8_t op2, MercedStatus *st) {
    Merced *m = x->m;
    if (!simd_opcode(op2)) return SIMD_NONE;
    bool f3 = x->rep == 0xf3;
    /* 66 and F2 select SSE2 forms, which a Pentium III lacks; F3 selects a
     * scalar form only where SSE defines one.  LOCK is never valid. */
    static const uint8_t scalar_ops[] = { 0x10, 0x11, 0x2a, 0x2c, 0x2d, 0x51, 0x52,
                                          0x53, 0x58, 0x59, 0x5c, 0x5d, 0x5e, 0x5f, 0xc2 };
    bool has_scalar = memchr(scalar_ops, op2, sizeof scalar_ops) != NULL;
    if (x->locked || x->rep == 0xf2 || (x->p66 && op2 != 0xae) || (f3 && !has_scalar))
        return SIMD_NONE;

    unsigned reg;
    uint8_t mr;
    RM rm;
    uint64_t a, b;
    bool gp;

    /* ---- 0F AE: FXSAVE FXRSTOR LDMXCSR STMXCSR SFENCE ---- */
    if (op2 == 0xae) {
        if (!simd_modrm(x, &reg, &rm, &mr)) return SIMD_FAULT;
        if (rm.is_reg) {
            if (reg != 7) return SIMD_NONE;         /* SFENCE: F8-FF */
            if (!sse_ready(x, st)) return SIMD_RETURN;
            return SIMD_DONE;
        }
        if (reg <= 1) {
            if (cr0(x) & 4) { *st = ia32_ud(x); return SIMD_RETURN; }
            if (cr0(x) & 8) { *st = merced_ia32_fp_exception(m, 7); return SIMD_RETURN; }
            if (m->psr & (UINT64_C(1) << 18)) { *st = merced_ia32_disabled_fp(m); return SIMD_RETURN; }
            return fxsave(x, &rm, reg == 1, st);
        }
        if (reg > 3) return SIMD_NONE;
        if (!sse_ready(x, st)) return SIMD_RETURN;
        if (reg == 3) {
            uint32_t v = mxcsr(m);
            return mem_write(x, rm.addr, 4, &v) ? SIMD_DONE : SIMD_FAULT;
        }
        uint32_t v;
        if (!rb(x, rm.addr, 4, false, &v)) return SIMD_FAULT;
        if (v & ~0xffbfu) { *st = ia32_fault(x, 13, 0); return SIMD_RETURN; }
        set_mxcsr(m, v);
        return SIMD_DONE;
    }

    /* ---- SSE floating point and moves ---- */
    if (op2 <= 0x2f || (op2 >= 0x50 && op2 <= 0x5f) || op2 == 0xc2 || op2 == 0xc6) {
        if (op2 == 0x5a || op2 == 0x5b) return SIMD_NONE;   /* SSE2 */
        if (!simd_modrm(x, &reg, &rm, &mr)) return SIMD_FAULT;
        /* CVTPI2PS/CVTPS2PI/CVTTPS2PI touch an MMX register. */
        bool mmx_side = !f3 && ((op2 == 0x2a && rm.is_reg) || op2 == 0x2c || op2 == 0x2d);
        if (!sse_ready(x, st)) return SIMD_RETURN;
        if (mmx_side && !mmx_ready(x, st)) return SIMD_RETURN;
        uint32_t csr = mxcsr(m);
        unsigned flags = 0;
        X128 d = xmm_get(m, reg), s;
        uint32_t imm = 0;

        switch (op2) {
        case 0x10: case 0x11: case 0x28: case 0x29: case 0x2b: {  /* moves */
            bool store = op2 == 0x11 || op2 == 0x29 || op2 == 0x2b;
            bool aligned = op2 >= 0x28;
            if (op2 == 0x2b && rm.is_reg) return SIMD_NONE;      /* MOVNTPS m128 */
            unsigned bytes = f3 ? 4 : 16;
            if (!store) {
                if (!xmm_src(x, &rm, bytes, aligned, &s, st, &gp)) return gp ? SIMD_RETURN : SIMD_FAULT;
                if (f3 && rm.is_reg) { x128_setf(&d, 0, x128_f(s, 0)); s = d; }
                else if (f3) s = (X128){ s.lo & 0xffffffff, 0 };  /* load clears the rest */
                xmm_set(m, reg, s);
                return SIMD_DONE;
            }
            if (rm.is_reg) {
                X128 t = xmm_get(m, rm.reg);
                if (f3) x128_setf(&t, 0, x128_f(d, 0)); else t = d;
                xmm_set(m, rm.reg, t);
                return SIMD_DONE;
            }
            if (aligned && (rm.addr & 15)) { *st = ia32_fault(x, 13, 0); return SIMD_RETURN; }
            uint8_t buf[16];
            memcpy(buf, &d.lo, 8);
            memcpy(buf + 8, &d.hi, 8);
            return mem_write(x, rm.addr, bytes, buf) ? SIMD_DONE : SIMD_FAULT;
        }
        case 0x12: case 0x13: case 0x16: case 0x17: {  /* movlps/movhps, movhlps/movlhps */
            bool high = op2 >= 0x16, store = op2 & 1;
            if (store) {
                if (rm.is_reg) return SIMD_NONE;
                uint64_t v = high ? d.hi : d.lo;
                return mem_write(x, rm.addr, 8, &v) ? SIMD_DONE : SIMD_FAULT;
            }
            uint64_t v;
            if (rm.is_reg) {
                X128 t = xmm_get(m, rm.reg);
                v = high ? t.lo : t.hi;                     /* MOVLHPS / MOVHLPS */
            } else if (!mem_read(x, rm.addr, 8, &v)) return SIMD_FAULT;
            if (high) d.hi = v; else d.lo = v;
            xmm_set(m, reg, d);
            return SIMD_DONE;
        }
        case 0x14: case 0x15: {                         /* unpcklps / unpckhps */
            if (!xmm_src(x, &rm, 16, true, &s, st, &gp)) return gp ? SIMD_RETURN : SIMD_FAULT;
            unsigned o = op2 == 0x15 ? 2 : 0;
            X128 r;
            x128_setf(&r, 0, x128_f(d, o));
            x128_setf(&r, 1, x128_f(s, o));
            x128_setf(&r, 2, x128_f(d, o + 1));
            x128_setf(&r, 3, x128_f(s, o + 1));
            xmm_set(m, reg, r);
            return SIMD_DONE;
        }
        case 0x2a: {                                    /* cvtpi2ps / cvtsi2ss */
            if (f3) {
                uint32_t v;
                if (!rmread(x, rm, 4, &v)) return SIMD_FAULT;
                uint32_t r = i32_to_f32((int32_t)v, csr, &flags);
                if (!sse_commit(x, flags, st)) return SIMD_RETURN;
                x128_setf(&d, 0, r);
            } else {
                uint64_t v;
                if (!mm_src(x, &rm, 8, &v)) return SIMD_FAULT;
                uint32_t r0 = i32_to_f32((int32_t)v, csr, &flags);
                uint32_t r1 = i32_to_f32((int32_t)(v >> 32), csr, &flags);
                if (!sse_commit(x, flags, st)) return SIMD_RETURN;
                if (rm.is_reg) mmx_state(m);
                d.lo = r0 | ((uint64_t)r1 << 32);
            }
            xmm_set(m, reg, d);
            return SIMD_DONE;
        }
        case 0x2c: case 0x2d: {                         /* cvt(t)ps2pi / cvt(t)ss2si */
            bool truncate = op2 == 0x2c;
            if (!xmm_src(x, &rm, f3 ? 4 : 8, false, &s, st, &gp)) return gp ? SIMD_RETURN : SIMD_FAULT;
            if (f3) {
                uint32_t r = f32_to_i32(x128_f(s, 0), truncate, csr, &flags);
                if (!sse_commit(x, flags, st)) return SIMD_RETURN;
                setxr(x, reg, 4, r);
            } else {
                uint32_t r0 = f32_to_i32(x128_f(s, 0), truncate, csr, &flags);
                uint32_t r1 = f32_to_i32(x128_f(s, 1), truncate, csr, &flags);
                if (!sse_commit(x, flags, st)) return SIMD_RETURN;
                mmx_state(m);
                mm_set(m, reg, r0 | ((uint64_t)r1 << 32));
            }
            return SIMD_DONE;
        }
        case 0x2e: case 0x2f: {                         /* ucomiss / comiss */
            if (!xmm_src(x, &rm, 4, false, &s, st, &gp)) return gp ? SIMD_RETURN : SIMD_FAULT;
            uint32_t fa = x128_f(d, 0), fb = x128_f(s, 0);
            bool unordered = f32_nan(fa) || f32_nan(fb);
            if (unordered && (op2 == 0x2f || f32_snan(fa) || f32_snan(fb))) flags |= MX_IE;
            if (f32_denormal(fa) || f32_denormal(fb)) flags |= MX_DE;
            if (!sse_commit(x, flags, st)) return SIMD_RETURN;
            uint32_t f = eflags(x) & ~(FL_ZF | FL_PF | FL_CF | FL_OF | FL_SF | FL_AF);
            if (unordered) f |= FL_ZF | FL_PF | FL_CF;
            else if (f32_v(fa) < f32_v(fb)) f |= FL_CF;
            else if (f32_v(fa) == f32_v(fb)) f |= FL_ZF;
            setflags(x, f);
            return SIMD_DONE;
        }
        case 0x50: {                                    /* movmskps r32, xmm */
            if (!rm.is_reg) return SIMD_NONE;
            s = xmm_get(m, rm.reg);
            uint32_t r = 0;
            for (unsigned i = 0; i < 4; i++) r |= (x128_f(s, i) >> 31) << i;
            setxr(x, reg, 4, r);
            return SIMD_DONE;
        }
        case 0x54: case 0x55: case 0x56: case 0x57: {   /* andps andnps orps xorps */
            if (!xmm_src(x, &rm, 16, true, &s, st, &gp)) return gp ? SIMD_RETURN : SIMD_FAULT;
            X128 r = op2 == 0x54 ? (X128){ d.lo & s.lo, d.hi & s.hi } :
                     op2 == 0x55 ? (X128){ ~d.lo & s.lo, ~d.hi & s.hi } :
                     op2 == 0x56 ? (X128){ d.lo | s.lo, d.hi | s.hi } :
                                   (X128){ d.lo ^ s.lo, d.hi ^ s.hi };
            xmm_set(m, reg, r);
            return SIMD_DONE;
        }
        case 0xc6: {                                    /* shufps */
            if (!xmm_src(x, &rm, 16, true, &s, st, &gp)) return gp ? SIMD_RETURN : SIMD_FAULT;
            if (!fetch(x, 1, &imm)) return SIMD_FAULT;
            X128 r;
            x128_setf(&r, 0, x128_f(d, imm & 3));
            x128_setf(&r, 1, x128_f(d, (imm >> 2) & 3));
            x128_setf(&r, 2, x128_f(s, (imm >> 4) & 3));
            x128_setf(&r, 3, x128_f(s, (imm >> 6) & 3));
            xmm_set(m, reg, r);
            return SIMD_DONE;
        }
        default: break;
        }
        /* Element-wise: 51-53, 58-5F, C2 (packed, or scalar with F3). */
        if (op2 < 0x51 || op2 == 0x5a || op2 == 0x5b) return SIMD_NONE;
        if (!xmm_src(x, &rm, f3 ? 4 : 16, !f3, &s, st, &gp)) return gp ? SIMD_RETURN : SIMD_FAULT;
        if (op2 == 0xc2 && !fetch(x, 1, &imm)) return SIMD_FAULT;
        X128 r = d;
        for (unsigned i = 0; i < (f3 ? 1u : 4u); i++) {
            uint32_t fa = x128_f(d, i), fb = x128_f(s, i), v;
            if (op2 == 0xc2) v = f32_cmp(imm & 7, fa, fb, &flags);
            else if (op2 == 0x52 || op2 == 0x53) v = f32_approx(op2 == 0x52, fb);
            else v = f32_op((unsigned)sse_arith_op(op2), fa, fb, csr, &flags);
            x128_setf(&r, i, v);
        }
        if (!sse_commit(x, flags, st)) return SIMD_RETURN;
        xmm_set(m, reg, r);
        return SIMD_DONE;
    }

    /* ---- MMX, and the SSE additions on MMX registers ---- */
    static const uint8_t no_mmx[] = { 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x6c, 0x6d,
                                      0xd0, 0xd4, 0xd6, 0xe6, 0xf0, 0xf4, 0xfb, 0xff };
    if (memchr(no_mmx, op2, sizeof no_mmx)) return SIMD_NONE;
    if (op2 == 0x77) {                                  /* emms */
        if (!mmx_ready(x, st)) return SIMD_RETURN;
        x87_set_empty_tags(m, 0xff);
        return SIMD_DONE;
    }
    if (!simd_modrm(x, &reg, &rm, &mr)) return SIMD_FAULT;
    /* Register-only and memory-only forms. */
    if (rm.is_reg ? (op2 == 0xe7) :
        (op2 == 0x71 || op2 == 0x72 || op2 == 0x73 || op2 == 0xc5 || op2 == 0xd7 || op2 == 0xf7))
        return SIMD_NONE;
    if (op2 >= 0x71 && op2 <= 0x73 && (reg & 1 || reg == 0 || (op2 == 0x73 ? reg == 4 : false)))
        return SIMD_NONE;                               /* only /2 /4 /6 (73: /2 /6) */
    if (!mmx_ready(x, st)) return SIMD_RETURN;
    uint32_t imm = 0;
    uint64_t r = 0;
    a = mm_get(m, reg);

    switch (op2) {
    case 0x6e: {                                        /* movd mm, r/m32 */
        uint32_t v;
        if (!rmread(x, rm, 4, &v)) return SIMD_FAULT;
        mmx_state(m);
        mm_set(m, reg, v);
        return SIMD_DONE;
    }
    case 0x7e:                                          /* movd r/m32, mm */
        if (!rmwrite(x, rm, 4, (uint32_t)a)) return SIMD_FAULT;
        mmx_state(m);
        return SIMD_DONE;
    case 0x7f: case 0xe7:                               /* movq mm/m64, mm; movntq */
        if (rm.is_reg) { mmx_state(m); mm_set(m, rm.reg, a); return SIMD_DONE; }
        if (!mem_write(x, rm.addr, 8, &a)) return SIMD_FAULT;
        mmx_state(m);
        return SIMD_DONE;
    case 0xc5:                                          /* pextrw r32, mm, imm8 */
        if (!fetch(x, 1, &imm)) return SIMD_FAULT;
        mmx_state(m);
        setxr(x, reg, 4, (uint32_t)lane(mm_get(m, rm.reg), imm & 3, 16));
        return SIMD_DONE;
    case 0xd7: {                                        /* pmovmskb r32, mm */
        b = mm_get(m, rm.reg);
        uint32_t v = 0;
        for (unsigned i = 0; i < 8; i++) v |= (uint32_t)((b >> (8 * i + 7)) & 1) << i;
        mmx_state(m);
        setxr(x, reg, 4, v);
        return SIMD_DONE;
    }
    case 0xf7: {                                        /* maskmovq mm, mm */
        b = mm_get(m, rm.reg);
        unsigned seg = x->seg_override >= 0 ? (unsigned)x->seg_override : X_DS;
        uint32_t di = xr(x, 7, x->addr32 ? 4 : 2);
        uint32_t base = sbase(x, seg) + di;
        for (unsigned i = 0; i < 8; i++)
            if (((b >> (8 * i + 7)) & 1) && !merced_ia32_probe_write(m, base + i, 1))
                return SIMD_FAULT;
        for (unsigned i = 0; i < 8; i++)
            if (((b >> (8 * i + 7)) & 1) && !wb(x, base + i, 1, (uint8_t)(a >> (8 * i))))
                return SIMD_FAULT;
        mmx_state(m);
        return SIMD_DONE;
    }
    case 0x71: case 0x72: case 0x73: {                  /* shift by imm8 */
        if (!fetch(x, 1, &imm)) return SIMD_FAULT;
        unsigned w = op2 == 0x71 ? 16 : op2 == 0x72 ? 32 : 64;
        unsigned kind = reg == 2 ? 0 : reg == 4 ? 2 : 1;
        mmx_state(m);
        mm_set(m, rm.reg, lane_shift(kind, mm_get(m, rm.reg), imm & 0xff, w));
        return SIMD_DONE;
    }
    default: break;
    }

    /* Two-operand forms: mm, mm/m64 (m32 for PUNPCKL*). */
    unsigned bytes = (op2 >= 0x60 && op2 <= 0x62) ? 4 : op2 == 0xc4 ? 2 : 8;
    if (op2 == 0xc4) {                                  /* pinsrw mm, r32/m16, imm8 */
        uint32_t v;
        if (!rmread(x, rm, 2, &v) || !fetch(x, 1, &imm)) return SIMD_FAULT;
        mmx_state(m);
        mm_set(m, reg, put_lane(mm_get(m, reg), imm & 3, 16, v & 0xffff));
        return SIMD_DONE;
    }
    if (!mm_src(x, &rm, bytes, &b)) return SIMD_FAULT;
    if (op2 == 0x70 && !fetch(x, 1, &imm)) return SIMD_FAULT;
    a = mm_get(m, reg);
    switch (op2) {
    case 0x60: r = unpack(a, b, 8, false); break;
    case 0x61: r = unpack(a, b, 16, false); break;
    case 0x62: r = unpack(a, b, 32, false); break;
    case 0x63: r = pack(a, b, 16, false); break;
    case 0x64: r = lanes(L_CMPGT, a, b, 8); break;
    case 0x65: r = lanes(L_CMPGT, a, b, 16); break;
    case 0x66: r = lanes(L_CMPGT, a, b, 32); break;
    case 0x67: r = pack(a, b, 16, true); break;
    case 0x68: r = unpack(a, b, 8, true); break;
    case 0x69: r = unpack(a, b, 16, true); break;
    case 0x6a: r = unpack(a, b, 32, true); break;
    case 0x6b: r = pack(a, b, 32, false); break;
    case 0x6f: r = b; break;                            /* movq mm, mm/m64 */
    case 0x70:                                          /* pshufw */
        for (unsigned i = 0; i < 4; i++) r = put_lane(r, i, 16, lane(b, (imm >> (2 * i)) & 3, 16));
        break;
    case 0x74: r = lanes(L_CMPEQ, a, b, 8); break;
    case 0x75: r = lanes(L_CMPEQ, a, b, 16); break;
    case 0x76: r = lanes(L_CMPEQ, a, b, 32); break;
    case 0xd1: r = lane_shift(0, a, b, 16); break;      /* psrlw/d/q */
    case 0xd2: r = lane_shift(0, a, b, 32); break;
    case 0xd3: r = lane_shift(0, a, b, 64); break;
    case 0xd5: r = lanes(L_MULLO, a, b, 16); break;
    case 0xd8: r = lanes(L_SUBUS, a, b, 8); break;
    case 0xd9: r = lanes(L_SUBUS, a, b, 16); break;
    case 0xda: r = lanes(L_MINU, a, b, 8); break;
    case 0xdb: r = a & b; break;
    case 0xdc: r = lanes(L_ADDUS, a, b, 8); break;
    case 0xdd: r = lanes(L_ADDUS, a, b, 16); break;
    case 0xde: r = lanes(L_MAXU, a, b, 8); break;
    case 0xdf: r = ~a & b; break;
    case 0xe0: r = lanes(L_AVG, a, b, 8); break;
    case 0xe1: r = lane_shift(2, a, b, 16); break;      /* psraw/d */
    case 0xe2: r = lane_shift(2, a, b, 32); break;
    case 0xe3: r = lanes(L_AVG, a, b, 16); break;
    case 0xe4: r = lanes(L_MULHIU, a, b, 16); break;
    case 0xe5: r = lanes(L_MULHI, a, b, 16); break;
    case 0xe8: r = lanes(L_SUBS, a, b, 8); break;
    case 0xe9: r = lanes(L_SUBS, a, b, 16); break;
    case 0xea: r = lanes(L_MINS, a, b, 16); break;
    case 0xeb: r = a | b; break;
    case 0xec: r = lanes(L_ADDS, a, b, 8); break;
    case 0xed: r = lanes(L_ADDS, a, b, 16); break;
    case 0xee: r = lanes(L_MAXS, a, b, 16); break;
    case 0xef: r = a ^ b; break;
    case 0xf1: r = lane_shift(1, a, b, 16); break;      /* psllw/d/q */
    case 0xf2: r = lane_shift(1, a, b, 32); break;
    case 0xf3: r = lane_shift(1, a, b, 64); break;
    case 0xf5:                                          /* pmaddwd */
        for (unsigned i = 0; i < 2; i++)
            r = put_lane(r, i, 32, (uint64_t)(slane(a, 2 * i, 16) * slane(b, 2 * i, 16) +
                                              slane(a, 2 * i + 1, 16) * slane(b, 2 * i + 1, 16)));
        break;
    case 0xf6: {                                        /* psadbw */
        uint64_t sum = 0;
        for (unsigned i = 0; i < 8; i++) {
            int64_t dlt = (int64_t)lane(a, i, 8) - (int64_t)lane(b, i, 8);
            sum += (uint64_t)(dlt < 0 ? -dlt : dlt);
        }
        r = sum;
        break;
    }
    case 0xf8: r = lanes(L_SUB, a, b, 8); break;
    case 0xf9: r = lanes(L_SUB, a, b, 16); break;
    case 0xfa: r = lanes(L_SUB, a, b, 32); break;
    case 0xfc: r = lanes(L_ADD, a, b, 8); break;
    case 0xfd: r = lanes(L_ADD, a, b, 16); break;
    case 0xfe: r = lanes(L_ADD, a, b, 32); break;
    default: return SIMD_NONE;
    }
    mmx_state(m);
    mm_set(m, reg, r);
    return SIMD_DONE;
}
