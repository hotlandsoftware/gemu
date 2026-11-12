/*
 * Merced IA-32 compatibility execution.
 *
 * The architectural state mapping and decoder behavior follow HP Ski's
 * ia_state.h, ia_decoder.tmpl.c, ia_read.c, ia_write.c and ia_exec.c.  This
 * implementation is intentionally integrated with GEMU's Merced MMU/bus
 * instead of importing Ski's process-global simulator state.
 */
#include "merced.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { X_ES, X_CS, X_SS, X_DS, X_FS, X_GS };
enum { FL_CF = 1u << 0, FL_PF = 1u << 2, FL_AF = 1u << 4,
       FL_ZF = 1u << 6, FL_SF = 1u << 7, FL_IF = 1u << 9,
       FL_DF = 1u << 10, FL_OF = 1u << 11, FL_VM = 1u << 17 };

typedef struct {
    Merced *m;
    uint32_t start, pc;
    unsigned op32, addr32;
    int seg_override;
    unsigned rep;
    bool locked;
} X86;

typedef struct {
    bool is_reg;
    unsigned reg;
    unsigned seg;
    uint32_t addr;
    uint32_t off;
} RM;

static MercedStatus xhalt(X86 *x, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->m->halt_msg, sizeof(x->m->halt_msg), fmt, ap);
    va_end(ap);
    x->m->halt_ip = x->start;
    return MERCED_HALT_UNIMPL;
}

static uint64_t gr(X86 *x, unsigned r) {
    return merced_ia32_gr_read(x->m, r);
}
static void setgr(X86 *x, unsigned r, uint64_t v) {
    merced_ia32_gr_write(x->m, r, v);
}
static uint32_t xr(X86 *x, unsigned r, unsigned size) {
    uint64_t v = gr(x, 8 + (r & 7));
    if (size == 1 && r >= 4) return (uint8_t)(gr(x, 8 + r - 4) >> 8);
    if (size == 1) return (uint8_t)v;
    if (size == 2) return (uint16_t)v;
    return (uint32_t)v;
}
static void setxr(X86 *x, unsigned r, unsigned size, uint32_t v) {
    unsigned ar = r;
    unsigned shift = 0;
    if (size == 1 && r >= 4) { ar = r - 4; shift = 8; }
    uint64_t old = gr(x, 8 + ar), mask;
    if (size == 4) mask = UINT32_MAX;
    else mask = ((1ull << (size * 8)) - 1) << shift;
    setgr(x, 8 + ar, (old & ~mask) | (((uint64_t)v << shift) & mask));
}
static uint16_t sel(X86 *x, unsigned s) { return (uint16_t)gr(x, 16 + s); }
static uint32_t sbase(X86 *x, unsigned s) { return (uint32_t)gr(x, 24 + s); }
static void setseg_real(X86 *x, unsigned s, uint16_t v) {
    setgr(x, 16 + s, (gr(x, 16 + s) & ~UINT64_C(0xffff)) | v);
    uint64_t d = gr(x, 24 + s);
    d = (d & ~UINT64_C(0x000fffffffffffff)) | ((uint32_t)v << 4) |
        (UINT64_C(0xffff) << 32);
    setgr(x, 24 + s, d);
}
static uint32_t eflags(X86 *x) { return (uint32_t)x->m->ar[24]; }
static void setflags(X86 *x, uint32_t f) {
    x->m->ar[24] = (x->m->ar[24] & ~UINT64_C(0xffffffff)) | f | 2;
}

static bool ia32_active(const X86 *x) {
    return (x->m->psr & (UINT64_C(1) << 34)) != 0; /* PSR.is */
}

static bool x87_ready(X86 *x, bool waiting, bool fwait, MercedStatus *status) {
    Merced *m = x->m;
    /* SDM Vol.4 section 2.1: DFL applies even to x87 instructions that
     * only access control state. DFH is not a per-instruction x87 check. */
    if (m->psr & (UINT64_C(1) << 18)) {
        *status = merced_ia32_disabled_fp(m);
        return false;
    }
    uint32_t cr0 = (uint32_t)m->ar[27]; /* CFLG */
    bool unavailable = fwait ? (cr0 & 0xa) == 0xa : (cr0 & 0xc) != 0;
    if (unavailable) {
        *status = merced_ia32_fp_exception(m, 7);
        return false;
    }
    uint64_t fsr = m->ar[28], fcr = m->ar[21];
    /* Native software can explicitly set FSR.ES independently of the
     * exception flags; Vol.1 section 6.2.2.5.4 requires honoring it. */
    if (waiting && ((fsr & 0x80) || (fsr & ~fcr & 0x3f))) {
        *status = merced_ia32_fp_exception(m, 16);
        return false;
    }
    return true;
}

static bool rb(X86 *x, uint32_t a, unsigned size, bool fetch, uint32_t *v) {
    uint64_t q;
    if (!ia32_active(x)) return false;
    if (!merced_ia32_read(x->m, a, size, fetch, &q)) return false;
    *v = (uint32_t)q;
    return true;
}
static bool wb(X86 *x, uint32_t a, unsigned size, uint32_t v) {
    if (!ia32_active(x)) return false;
    return merced_ia32_write(x->m, a, size, v);
}
static uint64_t ioaddr(X86 *x, uint16_t port) {
    return x->m->ar[0] | ((uint64_t)(port & 0xfffc) << 10) |
           (port & 0x0fff);
}
static unsigned ia32_ata_trace_left;
static bool ioread(X86 *x, uint16_t port, unsigned size, uint32_t *v) {
    uint64_t q;
    if (!ia32_active(x)) return false;
    if (!merced_ia32_read(x->m, ioaddr(x, port), size, false, &q)) return false;
    *v = (uint32_t)q;
    if (getenv("IA32_ATA_TRACE") &&
        ((port >= 0x170 && port <= 0x177) || port == 0x376))
        fprintf(stderr, "merced: IA32-ATA read ip=%08X port=%#x size=%u val=%#x ninsts=%" PRIu64 "\n",
                x->start, port, size, *v, x->m->ninsts);
    return true;
}
static bool iowrite(X86 *x, uint16_t port, unsigned size, uint32_t v) {
    if (!ia32_active(x)) return false;
    if (getenv("IA32_ATA_TRACE") &&
        ((port >= 0x170 && port <= 0x177) || port == 0x376)) {
        fprintf(stderr, "merced: IA32-ATA write ip=%08X port=%#x size=%u val=%#x ninsts=%" PRIu64 "\n",
                x->start, port, size, v, x->m->ninsts);
        if (port == 0x177 && size == 1 && v == 0xa1)
            ia32_ata_trace_left = 500;
    }
    return merced_ia32_write(x->m, ioaddr(x, port), size, v);
}
static bool fetch(X86 *x, unsigned size, uint32_t *v) {
    if (!rb(x, x->pc, size, true, v)) return false;
    x->pc += size;
    return true;
}

static uint32_t stack_off(X86 *x) {
    return (gr(x, 26) >> 62) & 1 ? xr(x, 4, 4) : xr(x, 4, 2);
}
static void set_stack_off(X86 *x, uint32_t v) {
    setxr(x, 4, ((gr(x, 26) >> 62) & 1) ? 4 : 2, v);
}
static bool push(X86 *x, unsigned size, uint32_t v) {
    uint32_t old_sp = stack_off(x);
    uint32_t sp = old_sp - size;
    set_stack_off(x, sp);
    if (!wb(x, sbase(x, X_SS) + sp, size, v)) {
        /* IA-32 faults are precise.  The instruction will be restarted
         * after the native IA-64 TLB handler returns, so do not leak the
         * predecrement performed by a faulting PUSH into that retry. */
        set_stack_off(x, old_sp);
        return false;
    }
    return true;
}
static bool pop(X86 *x, unsigned size, uint32_t *v) {
    uint32_t sp = stack_off(x);
    if (!rb(x, sbase(x, X_SS) + sp, size, false, v)) return false;
    set_stack_off(x, sp + size);
    return true;
}

static MercedStatus jmpe(X86 *x, uint32_t offset) {
    Merced *m = x->m;
    uint32_t target = sbase(x, X_CS) + offset;
    /* Both JMPE forms use an unsigned absolute offset from CS, then align
     * and zero-extend the 32-bit target. x.pc is already the next linear
     * IA-32 instruction address, which native code receives in r1. */
    m->gr_static[1] = x->pc;
    m->nat_static[1] = 0;
    /* Publish the mapped descriptor caches before returning to native
     * execution; br.ia/rfi subsequently reload them from these ARs. */
    m->ar[25] = gr(x, 25);
    m->ar[26] = gr(x, 26);
    /* IA-32 only updates the low halves; native consumers see the
     * architectural sign extension of EAX through EDI on mode exit. */
    for (unsigned r = 8; r < 16; r++)
        setgr(x, r, (uint64_t)(int64_t)(int32_t)gr(x, r));
    setflags(x, eflags(x) & ~(1u << 16)); /* EFLAGS.rf */
    m->ip = target & UINT32_C(0xfffffff0);
    /* Vol.2 10.4.2: enabled IA-32 low FP state is dirty on native entry.
     * MFL is sticky; MFH is unchanged by IA-32 execution. */
    if (!(m->psr & (UINT64_C(1) << 18))) m->psr |= UINT64_C(1) << 4;
    m->psr &= ~((UINT64_C(1) << 34) | (UINT64_C(1) << 37) |
                (UINT64_C(3) << 41)); /* IS, ID, RI */
    m->taken = 1;
    m->ninsts++;
    return MERCED_OK;
}

static bool intercept_code(X86 *x, unsigned prefix_len, uint16_t *code) {
    *code = (x->op32 ? 1u << 1 : 0) | (x->addr32 ? 1u << 2 : 0) |
            ((prefix_len & 0xfu) << 12);
    for (unsigned i = 0; i < prefix_len; i++) {
        uint32_t p;
        if (!rb(x, x->start + i, 1, true, &p)) return false;
        switch (p) {
        case 0xf0: *code |= 1u << 3; break;
        case 0xf3: *code |= 1u << 4; break;
        case 0xf2: *code |= 1u << 5; break;
        case 0x26: *code |= (1u << 6) | (0u << 7); break;
        case 0x2e: *code |= (1u << 6) | (1u << 7); break;
        case 0x36: *code |= (1u << 6) | (2u << 7); break;
        case 0x3e: *code |= (1u << 6) | (3u << 7); break;
        case 0x64: *code |= (1u << 6) | (4u << 7); break;
        case 0x65: *code |= (1u << 6) | (5u << 7); break;
        default: break; /* 66/67 are represented by os/as. */
        }
    }
    return true;
}

static bool decode_rm(X86 *x, uint8_t modrm, RM *rm) {
    unsigned mod = modrm >> 6, r = modrm & 7;
    if (mod == 3) { rm->is_reg = true; rm->reg = r; return true; }
    rm->is_reg = false;
    unsigned seg = X_DS;
    int32_t disp = 0;
    if (!x->addr32) {
        static const int8_t base[8] = {3,3,5,5,6,7,5,3};
        static const int8_t index[8] = {6,7,6,7,-1,-1,-1,-1};
        if ((r >= 2 && r <= 3) || (r == 6 && mod != 0)) seg = X_SS;
        if (x->seg_override >= 0) seg = (unsigned)x->seg_override;
        uint32_t off = base[r] < 0 ? 0 : xr(x, base[r], 2);
        if (index[r] >= 0) off += xr(x, index[r], 2);
        uint32_t q;
        if (mod == 0 && r == 6) { if (!fetch(x, 2, &q)) return false; off = q; }
        else if (mod == 1) { if (!fetch(x, 1, &q)) return false; disp = (int8_t)q; }
        else if (mod == 2) { if (!fetch(x, 2, &q)) return false; disp = (int16_t)q; }
        rm->off = (uint16_t)(off + disp);
        rm->seg = seg;
        rm->addr = sbase(x, seg) + rm->off;
        return true;
    }
    uint32_t off = 0, q;
    if (r == 4) {
        if (!fetch(x, 1, &q)) return false;
        unsigned ss = q >> 6, idx = (q >> 3) & 7, b = q & 7;
        if (idx != 4) off += xr(x, idx, 4) << ss;
        if (b == 5 && mod == 0) {
            if (!fetch(x, 4, &q)) return false;
            off += q; /* No base: disp32 is added to the scaled index. */
        }
        else { off += xr(x, b, 4); if (b == 4 || b == 5) seg = X_SS; }
    } else if (r == 5 && mod == 0) { if (!fetch(x, 4, &off)) return false; }
    else { off = xr(x, r, 4); if (r == 4 || r == 5) seg = X_SS; }
    if (x->seg_override >= 0) seg = (unsigned)x->seg_override;
    if (mod == 1) { if (!fetch(x, 1, &q)) return false; disp = (int8_t)q; }
    else if (mod == 2) { if (!fetch(x, 4, &q)) return false; disp = (int32_t)q; }
    rm->off = off + disp;
    rm->seg = seg;
    rm->addr = sbase(x, seg) + rm->off;
    return true;
}
static bool rmread(X86 *x, RM rm, unsigned size, uint32_t *v) {
    return rm.is_reg ? (*v = xr(x, rm.reg, size), true)
                     : rb(x, rm.addr, size, false, v);
}
static bool rmwrite(X86 *x, RM rm, unsigned size, uint32_t v) {
    if (rm.is_reg) { setxr(x, rm.reg, size, v); return true; }
    return wb(x, rm.addr, size, v);
}

/* The x87 stack aliases FR8..FR15. Keep its arithmetic in integers: native
 * IA-64 FP helpers intentionally approximate exceptional values, and the
 * host's long double format/rounding mode is not part of the guest state. */
enum { X87_ZERO, X87_FINITE, X87_INF, X87_NAN, X87_UNSUPPORTED };
enum { X87_IE = 1, X87_DE = 2, X87_OE = 8, X87_UE = 16, X87_PE = 32,
       X87_SF = 64, X87_C0 = 0x100, X87_C1 = 0x200, X87_C2 = 0x400,
       X87_C3 = 0x4000 };
typedef struct {
    uint64_t sig;                 /* normalized explicit integer bit */
    int exp;                     /* unbiased exponent of that bit */
    unsigned sign, kind;
    bool denormal;
} X87Number;

static X87Number x87_indefinite(void) {
    return (X87Number){ .sig = UINT64_C(0xc000000000000000),
                        .sign = 1, .kind = X87_NAN };
}
static X87Number x87_normalize(uint64_t sig, int exp, unsigned sign) {
    X87Number n = {.sig = sig, .exp = exp, .sign = sign,
                   .kind = sig ? X87_FINITE : X87_ZERO};
    if (sig) {
        unsigned shift = __builtin_clzll(sig);
        n.sig <<= shift;
        n.exp -= shift;
    }
    return n;
}
static X87Number x87_from_fr(MercedFpReg f) {
    X87Number n = {.sig = f.sig, .sign = f.sign};
    if (f.nat) n.kind = X87_UNSUPPORTED;
    else if (f.exp == 0x1ffff) {
        n.kind = !(f.sig >> 63) ? X87_UNSUPPORTED :
                 (f.sig << 1) ? X87_NAN : X87_INF;
    } else if (!f.exp) {
        n = x87_normalize(f.sig, -16382, f.sign);
        n.denormal = f.sig != 0 && !(f.sig >> 63);
    } else if (!(f.sig >> 63) || f.exp < 0xc001 || f.exp > 0x13ffe) {
        n.kind = X87_UNSUPPORTED;
    } else {
        n.kind = X87_FINITE;
        n.exp = (int)f.exp - 0xffff;
    }
    return n;
}
static MercedFpReg x87_to_fr(X87Number n) {
    MercedFpReg f = {.sig = n.sig, .sign = n.sign};
    if (n.kind == X87_ZERO) f.sig = 0;
    else if (n.kind == X87_INF || n.kind == X87_NAN) f.exp = 0x1ffff;
    else if (n.exp < -16382) {
        unsigned shift = -16382 - n.exp;
        f.sig = shift < 64 ? n.sig >> shift : 0;
    } else f.exp = n.exp + 0xffff;
    return f;
}
static X87Number x87_from_double(uint64_t bits) {
    unsigned exp = (bits >> 52) & 0x7ff, sign = bits >> 63;
    uint64_t frac = bits & UINT64_C(0xfffffffffffff);
    if (exp == 0x7ff)
        return (X87Number){.sig = UINT64_C(1) << 63 | (frac << 11),
                           .sign = sign, .kind = frac ? X87_NAN : X87_INF};
    X87Number n = x87_normalize((frac | (exp ? UINT64_C(1) << 52 : 0)) << 11,
                                exp ? (int)exp - 1023 : -1022, sign);
    n.denormal = exp == 0 && frac != 0;
    return n;
}
static unsigned x87_top(const Merced *m) { return (m->ar[28] >> 11) & 7; }
static bool x87_empty(const Merced *m, unsigned physical) {
    return (m->ar[28] >> (16 + 2 * physical)) & 1;
}
static void x87_set_top(Merced *m, unsigned top) {
    m->ar[28] = (m->ar[28] & ~UINT64_C(0x3800)) | ((uint64_t)(top & 7) << 11);
}
static void x87_put(Merced *m, unsigned physical, X87Number n) {
    m->fr[8 + physical] = x87_to_fr(n);
    m->ar[28] &= ~(UINT64_C(1) << (16 + 2 * physical));
}
static void x87_pop(Merced *m) {
    unsigned top = x87_top(m);
    m->ar[28] |= UINT64_C(1) << (16 + 2 * top);
    x87_set_top(m, top + 1);
}
static void x87_status(Merced *m, unsigned exceptions, bool c1) {
    m->ar[28] = (m->ar[28] & ~UINT64_C(0x200)) | (c1 ? X87_C1 : 0) | exceptions;
    if (m->ar[28] & ~m->ar[21] & 0x3f) m->ar[28] |= 0x8080;
}
static bool x87_signaling(X87Number n) {
    return n.kind == X87_NAN && !(n.sig & (UINT64_C(1) << 62));
}
static X87Number x87_nan(X87Number a, X87Number b, unsigned *exceptions) {
    if (a.kind == X87_UNSUPPORTED || b.kind == X87_UNSUPPORTED) {
        *exceptions |= X87_IE;
        return x87_indefinite();
    }
    if (x87_signaling(a) || x87_signaling(b)) *exceptions |= X87_IE;
    X87Number n;
    if (a.kind != X87_NAN) n = b;
    else if (b.kind != X87_NAN) n = a;
    else if (x87_signaling(a) != x87_signaling(b))
        n = x87_signaling(a) ? b : a;
    else if ((a.sig << 1) != (b.sig << 1)) n = (a.sig << 1) > (b.sig << 1) ? a : b;
    else n = a.sign ? b : a;
    n.sig |= UINT64_C(0xc000000000000000);
    return n;
}

/* Unsigned 128-bit values for the x87 rounding path: an exact 64x64-bit
 * product plus the shifts, masks and compares rounding needs.  Spelled out
 * with two 64-bit halves because 32-bit targets (i686 MinGW) have no
 * __int128; every target runs this same code. */
typedef struct { uint64_t lo, hi; } X87U128;

static X87U128 u128_from64(uint64_t v) { return (X87U128){v, 0}; }
static bool u128_is_zero(X87U128 v) { return !(v.lo | v.hi); }
static int u128_cmp(X87U128 a, X87U128 b) {
    if (a.hi != b.hi) return a.hi > b.hi ? 1 : -1;
    if (a.lo != b.lo) return a.lo > b.lo ? 1 : -1;
    return 0;
}
static X87U128 u128_bit(unsigned n) {                     /* 1 << n, n < 128 */
    return n < 64 ? (X87U128){UINT64_C(1) << n, 0}
                  : (X87U128){0, UINT64_C(1) << (n - 64)};
}
static X87U128 u128_shl(X87U128 v, unsigned n) {
    if (n >= 128) return (X87U128){0, 0};
    if (n >= 64) return (X87U128){0, v.lo << (n - 64)};
    if (!n) return v;
    return (X87U128){v.lo << n, (v.hi << n) | (v.lo >> (64 - n))};
}
static X87U128 u128_shr(X87U128 v, unsigned n) {
    if (n >= 128) return (X87U128){0, 0};
    if (n >= 64) return (X87U128){v.hi >> (n - 64), 0};
    if (!n) return v;
    return (X87U128){(v.lo >> n) | (v.hi << (64 - n)), v.hi >> n};
}
static X87U128 u128_low_bits(X87U128 v, unsigned n) {    /* v mod 2^n */
    if (n >= 128) return v;
    if (n >= 64) return (X87U128){v.lo, n == 64 ? 0 : v.hi & ((UINT64_C(1) << (n - 64)) - 1)};
    return (X87U128){n ? v.lo & ((UINT64_C(1) << n) - 1) : 0, 0};
}
static X87U128 u128_inc(X87U128 v) {
    v.lo++;
    if (!v.lo) v.hi++;
    return v;
}
static X87U128 u128_mul64(uint64_t a, uint64_t b) {
#ifdef __SIZEOF_INT128__
    /* One native widening multiply where the compiler has 128-bit
     * integers (all 64-bit hosts); the same result as the version below. */
    unsigned __int128 p = (unsigned __int128)a * b;
    return (X87U128){(uint64_t)p, (uint64_t)(p >> 64)};
#else
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    return (X87U128){(mid << 32) | (uint32_t)p00,
                     p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32)};
#endif
}

static unsigned x87_bit_length(X87U128 v) {
    return v.hi ? 128 - (unsigned)__builtin_clzll(v.hi)
                : 64 - (unsigned)__builtin_clzll(v.lo);
}
/* Round an exact positive integer times 2^scale. When shift >= 128 the
 * result can still round away from zero; do not perform an invalid shift. */
static X87Number x87_round_integer(X87U128 value, int scale, int shift,
                                    unsigned sign, unsigned rc,
                                    bool *inexact, bool *up) {
    X87U128 q, rem = {0, 0};
    int half = -1;
    if (shift <= 0) {
        q = u128_shl(value, (unsigned)-shift);
    } else if (shift < 128) {
        q = u128_shr(value, (unsigned)shift);
        rem = u128_low_bits(value, (unsigned)shift);
        half = u128_cmp(rem, u128_bit((unsigned)shift - 1));
    } else {
        q = (X87U128){0, 0};
        rem = value;
        if (shift == 128)
            half = u128_cmp(value, u128_bit(127));
    }
    *inexact = !u128_is_zero(rem);
    *up = *inexact && (rc == 0 ? half > 0 || (half == 0 && (q.lo & 1)) :
                       rc == 1 ? sign != 0 : rc == 2 ? sign == 0 : false);
    if (*up) q = u128_inc(q);
    if (u128_is_zero(q)) return (X87Number){.sign = sign, .kind = X87_ZERO};
    unsigned bits = x87_bit_length(q);
    /* At most 65 bits survive (the extra one is a rounding carry). */
    uint64_t sig = bits > 64 ? u128_shr(q, bits - 64).lo : q.lo << (64 - bits);
    return (X87Number){.sig = sig, .exp = scale + shift + (int)bits - 1,
                       .sign = sign, .kind = X87_FINITE};
}
static X87Number x87_round(X87U128 value, int scale, unsigned sign,
                            unsigned precision, int emin, int emax,
                            uint16_t fcw, bool memory, unsigned *exceptions,
                            bool *c1) {
    if (u128_is_zero(value)) return (X87Number){.sign = sign, .kind = X87_ZERO};
    unsigned rc = (fcw >> 10) & 3;
    int bits = x87_bit_length(value), shift = bits - (int)precision;
    bool inexact;
    X87Number n = x87_round_integer(value, scale, shift, sign, rc, &inexact, c1);
    if (n.exp > emax) {
        *exceptions |= X87_OE;
        if (!(fcw & X87_OE)) {
            if (memory) { *c1 = false; return n; }
            n.exp -= 24576;
        } else {
            inexact = true;
            bool infinity = rc == 0 || (rc == 1 && sign) || (rc == 2 && !sign);
            if (infinity) n = (X87Number){.sig = UINT64_C(1) << 63,
                                          .sign = sign, .kind = X87_INF};
            else n = (X87Number){.sig = UINT64_MAX << (64 - precision),
                                 .exp = emax, .sign = sign, .kind = X87_FINITE};
            *c1 = infinity;
        }
    } else if (n.kind != X87_ZERO && n.exp < emin) {
        if (!(fcw & X87_UE)) {
            *exceptions |= X87_UE;
            if (memory) { *c1 = false; return n; }
            n.exp += 24576;
        } else {
            shift = emin - ((int)precision - 1) - scale;
            n = x87_round_integer(value, scale, shift, sign, rc, &inexact, c1);
            if (inexact) *exceptions |= X87_UE;
        }
    }
    if (inexact) *exceptions |= X87_PE;
    return n;
}
static X87Number x87_multiply(X87Number a, X87Number b, uint16_t fcw,
                               unsigned *exceptions, bool *c1) {
    unsigned sign = a.sign ^ b.sign;
    if (a.kind >= X87_NAN || b.kind >= X87_NAN) return x87_nan(a, b, exceptions);
    if (a.denormal || b.denormal) *exceptions |= X87_DE;
    if ((a.kind == X87_ZERO && b.kind == X87_INF) ||
        (a.kind == X87_INF && b.kind == X87_ZERO)) {
        *exceptions |= X87_IE;
        return x87_indefinite();
    }
    if ((*exceptions & X87_DE) && !(fcw & X87_DE)) return a;
    if (a.kind == X87_INF || b.kind == X87_INF)
        return (X87Number){.sig = UINT64_C(1) << 63, .sign = sign, .kind = X87_INF};
    if (a.kind == X87_ZERO || b.kind == X87_ZERO)
        return (X87Number){.sign = sign, .kind = X87_ZERO};
    unsigned pc = (fcw >> 8) & 3;
    unsigned precision = pc == 0 ? 24 : pc == 2 ? 53 : 64;
    return x87_round(u128_mul64(a.sig, b.sig), a.exp + b.exp - 126,
                      sign, precision, -16382, 16383, fcw, false, exceptions, c1);
}
static uint64_t x87_store_double(X87Number n, uint16_t fcw,
                                  unsigned *exceptions, bool *c1) {
    if (n.kind == X87_UNSUPPORTED) {
        *exceptions |= X87_IE;
        n = x87_indefinite();
    } else if (x87_signaling(n)) {
        *exceptions |= X87_IE;
        n.sig |= UINT64_C(1) << 62;
    }
    if (n.kind == X87_FINITE)
        n = x87_round(u128_from64(n.sig), n.exp - 63, n.sign, 53, -1022, 1023,
                       fcw, true, exceptions, c1);
    uint64_t bits = (uint64_t)n.sign << 63;
    if (n.kind == X87_INF) return bits | UINT64_C(0x7ff0000000000000);
    if (n.kind == X87_NAN)
        return bits | UINT64_C(0x7ff8000000000000) | ((n.sig >> 11) & UINT64_C(0xfffffffffffff));
    if (n.kind == X87_ZERO) return bits;
    if (n.exp < -1022) {
        unsigned shift = -1022 - n.exp + 11;
        return bits | (shift < 64 ? n.sig >> shift : 0);
    }
    return bits | ((uint64_t)(n.exp + 1023) << 52) |
           ((n.sig >> 11) & UINT64_C(0xfffffffffffff));
}
static unsigned x87_compare(X87Number a, X87Number b, unsigned *exceptions) {
    if (a.kind >= X87_NAN || b.kind >= X87_NAN) {
        *exceptions |= X87_IE;
        return X87_C0 | X87_C2 | X87_C3;
    }
    if (a.denormal || b.denormal) *exceptions |= X87_DE;
    if (a.kind == X87_ZERO && b.kind == X87_ZERO) return X87_C3;
    int cmp;
    if (a.sign != b.sign) cmp = a.sign ? -1 : 1;
    else {
        if (a.kind != b.kind) cmp = a.kind > b.kind ? 1 : -1;
        else if (a.kind == X87_INF) cmp = 0;
        else if (a.exp != b.exp) cmp = a.exp > b.exp ? 1 : -1;
        else cmp = a.sig == b.sig ? 0 : a.sig > b.sig ? 1 : -1;
        if (a.sign) cmp = -cmp;
    }
    return cmp == 0 ? X87_C3 : cmp < 0 ? X87_C0 : 0;
}
static uint16_t x87_selector(X86 *x, unsigned seg) {
    /* Segment selectors are packed into GR16 (DS,ES,FS,GS) and GR17
     * (CS,SS,LDT,TSS), independently of their descriptor cache GRs. */
    unsigned index = seg == X_DS ? 0 : seg == X_ES ? 1 : seg == X_FS ? 2 : 3;
    if (seg == X_CS || seg == X_SS) return gr(x, 17) >> (seg == X_CS ? 0 : 16);
    return gr(x, 16) >> (16 * index);
}
static void x87_pointers(X86 *x, uint8_t op, uint8_t mr, const RM *rm) {
    unsigned fop = ((op & 7) << 8) | mr;
    x->m->ar[29] = ((uint64_t)fop << 48) | ((uint64_t)x87_selector(x, X_CS) << 32) |
                    (uint32_t)(x->start - sbase(x, X_CS));
    if (!rm->is_reg)
        x->m->ar[30] = ((uint64_t)x87_selector(x, rm->seg) << 32) | rm->off;
}

static void logic_flags(X86 *x, uint32_t v, unsigned size) {
    uint32_t f = eflags(x) & ~(FL_CF|FL_PF|FL_AF|FL_ZF|FL_SF|FL_OF);
    uint32_t mask = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1;
    v &= mask;
    if (!v) f |= FL_ZF;
    if (v & (1u << (size * 8 - 1))) f |= FL_SF;
    if (!(__builtin_parity(v & 0xff))) f |= FL_PF;
    setflags(x, f);
}

static void sub_flags(X86 *x, uint32_t a, uint32_t b, uint32_t v,
                      unsigned size) {
    uint64_t mask = size == 4 ? UINT32_MAX : (UINT64_C(1) << (size * 8)) - 1;
    uint32_t sign = 1u << (size * 8 - 1);
    uint32_t f = eflags(x) & ~(FL_CF|FL_PF|FL_AF|FL_ZF|FL_SF|FL_OF);
    a &= mask; b &= mask; v &= mask;
    if (a < b) f |= FL_CF;
    if ((a ^ b) & (a ^ v) & sign) f |= FL_OF;
    if ((a ^ b ^ v) & 0x10) f |= FL_AF;
    if (!v) f |= FL_ZF;
    if (v & sign) f |= FL_SF;
    if (!__builtin_parity(v & 0xff)) f |= FL_PF;
    setflags(x, f);
}

static void add_flags(X86 *x, uint32_t a, uint32_t b, uint32_t v,
                      unsigned size) {
    uint64_t mask = size == 4 ? UINT32_MAX : (UINT64_C(1) << (size * 8)) - 1;
    uint32_t sign = 1u << (size * 8 - 1);
    uint32_t f = eflags(x) & ~(FL_CF|FL_PF|FL_AF|FL_ZF|FL_SF|FL_OF);
    a &= mask; b &= mask; v &= mask;
    if ((uint64_t)a + b > mask) f |= FL_CF;
    if (~(a ^ b) & (a ^ v) & sign) f |= FL_OF;
    if ((a ^ b ^ v) & 0x10) f |= FL_AF;
    if (!v) f |= FL_ZF;
    if (v & sign) f |= FL_SF;
    if (!__builtin_parity(v & 0xff)) f |= FL_PF;
    setflags(x, f);
}

/* ADC/SBB fold the incoming carry into the wide sum/difference before
 * comparing against the truncated result, unlike plain ADD/SUB - CF must
 * reflect a+b+cf (or a-b-cf) overflowing, not a+b alone, so this can't
 * reuse add_flags/sub_flags by pre-adding cf into b: that would let the
 * carry get silently masked away for inputs already at the size limit
 * (e.g. 0xFF + 0x00 + CF=1 must set CF, but a+b alone doesn't). */
static void adc_flags(X86 *x, uint32_t a, uint32_t b, uint32_t cf_in,
                      uint32_t v, unsigned size) {
    uint64_t mask = size == 4 ? UINT32_MAX : (UINT64_C(1) << (size * 8)) - 1;
    uint32_t sign = 1u << (size * 8 - 1);
    uint32_t f = eflags(x) & ~(FL_CF|FL_PF|FL_AF|FL_ZF|FL_SF|FL_OF);
    a &= mask; b &= mask; v &= mask;
    if ((uint64_t)a + b + cf_in > mask) f |= FL_CF;
    if (~(a ^ b) & (a ^ v) & sign) f |= FL_OF;
    if ((a ^ b ^ v) & 0x10) f |= FL_AF;
    if (!v) f |= FL_ZF;
    if (v & sign) f |= FL_SF;
    if (!__builtin_parity(v & 0xff)) f |= FL_PF;
    setflags(x, f);
}
static void sbb_flags(X86 *x, uint32_t a, uint32_t b, uint32_t cf_in,
                      uint32_t v, unsigned size) {
    uint64_t mask = size == 4 ? UINT32_MAX : (UINT64_C(1) << (size * 8)) - 1;
    uint32_t sign = 1u << (size * 8 - 1);
    uint32_t f = eflags(x) & ~(FL_CF|FL_PF|FL_AF|FL_ZF|FL_SF|FL_OF);
    a &= mask; b &= mask; v &= mask;
    if ((uint64_t)a < (uint64_t)b + cf_in) f |= FL_CF;
    if ((a ^ b) & (a ^ v) & sign) f |= FL_OF;
    if ((a ^ b ^ v) & 0x10) f |= FL_AF;
    if (!v) f |= FL_ZF;
    if (v & sign) f |= FL_SF;
    if (!__builtin_parity(v & 0xff)) f |= FL_PF;
    setflags(x, f);
}

static void incdec_flags(X86 *x, uint32_t a, uint32_t b, uint32_t v,
                         unsigned size, bool sub) {
    uint32_t cf = eflags(x) & FL_CF;
    if (sub) sub_flags(x,a,b,v,size); else add_flags(x,a,b,v,size);
    setflags(x,(eflags(x) & ~FL_CF) | cf);
}

static bool condition(X86 *x, unsigned c) {
    uint32_t f = eflags(x);
    bool cf=f&FL_CF, pf=f&FL_PF, zf=f&FL_ZF, sf=f&FL_SF, of=f&FL_OF;
    switch (c & 15) {
    case 0: return of;       case 1: return !of;
    case 2: return cf;       case 3: return !cf;
    case 4: return zf;       case 5: return !zf;
    case 6: return cf||zf;   case 7: return !cf&&!zf;
    case 8: return sf;       case 9: return !sf;
    case 10:return pf;       case 11:return !pf;
    case 12:return sf!=of;   case 13:return sf==of;
    case 14:return zf||sf!=of; default:return !zf&&sf==of;
    }
}

/* Near control transfers update EIP, not the already-segmented linear IP.
 * In a 16-bit code segment the result wraps at 16 bits before CSD.base is
 * added.  Treating x.pc as a flat address made backward calls crossing the
 * segment base jump below CS (for example F000:F50FE -> physical EF019
 * instead of the intended F000:0019). */
static uint32_t near_target(X86 *x, int32_t rel) {
    uint32_t off = x->pc - sbase(x, X_CS);
    off += (uint32_t)rel;
    if (!x->op32) off &= UINT16_MAX;
    return sbase(x, X_CS) + off;
}

/* ---- Minimal INT 10h video-services shim ----
 *
 * Real Itanium firmware runs a legacy x86 video BIOS option ROM through
 * the IA-32 Execution Layer specifically so NT's boot-time text output
 * (including bugcheck/blue-screen text) can call int 10h - a real,
 * documented convention on actual Itanium hardware, not an x86-only
 * quirk. GEMU has no such ROM to run, so int 10h is intercepted directly
 * here (the same "shim the call, don't emulate the real firmware"
 * pattern already used for SAL/PAL) instead of dispatching through the
 * real-mode IVT into handler code that doesn't exist, implementing the
 * handful of functions boot-time text output actually needs against the
 * real VGA registers/memory the rest of this file already talks to.
 *
 * VGA ports live behind the generic machine's own MMIO window
 * (GENERIC_VGA_IO_BASE in hardware/ia64/generic.h) rather than the standard
 * IA-64 sparse I/O port space x86 in/out normally routes through - this
 * shim reads/writes them (and the BDA/text VRAM, both addressed exactly
 * like the existing int/iret code already addresses the real-mode IVT:
 * flat, unsegmented) directly for that reason. The field layout used to
 * compute the active screen size (VGA_CRTC_H_DISP=1, VGA_CRTC_OVERFLOW=7,
 * VGA_CRTC_MAX_SCAN=9, VGA_CRTC_V_DISP_END=0x12) matches
 * reference/qemu-system-ia64's hw/display/vga.c vga_get_resolution(),
 * not a hardcoded 80x25. */
#define INT10_VGA_IO_BASE 0xC0000000ull  /* GENERIC_VGA_IO_BASE */
#define INT10_VRAM_BASE   0xB8000ull
#define INT10_BDA_CURSOR  0x450ull       /* word per page: (row<<8)|col */
#define INT10_BDA_MODE    0x449ull

static uint32_t int10_crtc_read(X86 *x, uint8_t index) {
    uint32_t v = 0;
    wb(x, INT10_VGA_IO_BASE + (0x3D4 - 0x3B0), 1, index);
    rb(x, INT10_VGA_IO_BASE + (0x3D5 - 0x3B0), 1, false, &v);
    return v;
}

static void int10_screen_dims(X86 *x, unsigned *cols, unsigned *rows) {
    unsigned h_disp = int10_crtc_read(x, 0x01);
    unsigned max_scan = (int10_crtc_read(x, 0x09) & 0x1F) + 1;
    unsigned ov = int10_crtc_read(x, 0x07);
    unsigned vdisp = int10_crtc_read(x, 0x12) |
                     ((ov & 0x02) << 7) | ((ov & 0x40) << 3);
    *cols = h_disp + 1;
    if (!*cols || *cols > 132) *cols = 80;
    if (!max_scan) max_scan = 16;
    *rows = (vdisp + 1) / max_scan;
    if (!*rows || *rows > 60) *rows = 25;
}

static void int10_get_cursor(X86 *x, unsigned page, unsigned *row, unsigned *col) {
    uint32_t v = 0;
    rb(x, INT10_BDA_CURSOR + page * 2, 2, false, &v);
    *col = v & 0xFF; *row = (v >> 8) & 0xFF;
}
static void int10_set_cursor(X86 *x, unsigned page, unsigned row, unsigned col) {
    wb(x, INT10_BDA_CURSOR + page * 2, 2, ((row & 0xFF) << 8) | (col & 0xFF));
}

static void int10_scroll(X86 *x, uint32_t base, unsigned cols, unsigned rows,
                         uint8_t attr) {
    for (unsigned r = 1; r < rows; r++)
        for (unsigned c = 0; c < cols; c++) {
            uint32_t v = 0;
            rb(x, base + (r * cols + c) * 2, 2, false, &v);
            wb(x, base + ((r - 1) * cols + c) * 2, 2, v);
        }
    for (unsigned c = 0; c < cols; c++)
        wb(x, base + ((rows - 1) * cols + c) * 2, 2,
           ((uint32_t)attr << 8) | ' ');
}

/* Writes one character at the current cursor position (teletype-style:
 * handles CR/LF/BS/bell and end-of-line/end-of-screen wraparound),
 * advances the cursor, and scrolls if it ran off the bottom. with_attr
 * false leaves the existing attribute byte alone (matches real int 10h
 * AH=0Eh, which never touches attributes). */
static void int10_putc(X86 *x, unsigned page, uint8_t ch, uint8_t attr,
                       bool with_attr) {
    unsigned cols, rows, row, col;
    int10_screen_dims(x, &cols, &rows);
    int10_get_cursor(x, page, &row, &col);
    uint32_t base = INT10_VRAM_BASE + page * 0x1000;
    if (ch == '\r') {
        col = 0;
    } else if (ch == '\n') {
        row++;
    } else if (ch == 8) {
        if (col) col--;
    } else if (ch == 7) {
        /* bell: nothing to do without an audio device */
    } else {
        uint32_t off = base + (uint32_t)(row * cols + col) * 2;
        wb(x, off, 1, ch);
        if (with_attr) wb(x, off + 1, 1, attr);
        col++;
        if (col >= cols) { col = 0; row++; }
    }
    if (row >= rows) {
        int10_scroll(x, base, cols, rows, attr);
        row = rows - 1;
    }
    int10_set_cursor(x, page, row, col);
}

static void int10_mode12(X86 *x) {
    static const uint8_t seq[5] = { 0x03,0x01,0x0f,0x00,0x06 };
    static const uint8_t crtc[25] = {
        0x5f,0x4f,0x50,0x82,0x54,0x80,0x0b,0x3e,0x00,0x40,0x00,0x00,0x00,
        0x00,0x00,0x00,0xea,0x0c,0xdf,0x28,0x00,0xe7,0x04,0xe3,0xff
    };
    static const uint8_t gc[9] = { 0,0,0,0,0,0,0x05,0x0f,0xff };
    static const uint8_t attr[21] = {
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x01,0,0x0f,0,0
    };
    static const uint8_t ega[16][3] = {
        {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}
    };
    iowrite(x, 0x3c2, 1, 0xe3);
    for (unsigned i = 0; i < 5; i++) {
        iowrite(x, 0x3c4, 1, i); iowrite(x, 0x3c5, 1, seq[i]);
    }
    iowrite(x, 0x3d4, 1, 0x11); iowrite(x, 0x3d5, 1, crtc[0x11] & 0x7f);
    for (unsigned i = 0; i < 25; i++) {
        iowrite(x, 0x3d4, 1, i); iowrite(x, 0x3d5, 1, crtc[i]);
    }
    for (unsigned i = 0; i < 9; i++) {
        iowrite(x, 0x3ce, 1, i); iowrite(x, 0x3cf, 1, gc[i]);
    }
    for (unsigned i = 0; i < 21; i++) {
        uint32_t dummy; ioread(x, 0x3da, 1, &dummy);
        iowrite(x, 0x3c0, 1, i); iowrite(x, 0x3c0, 1, attr[i]);
    }
    iowrite(x, 0x3c6, 1, 0xff); iowrite(x, 0x3c8, 1, 0);
    for (unsigned i = 0; i < 16; i++)
        for (unsigned c = 0; c < 3; c++) iowrite(x, 0x3c9, 1, ega[i][c]);
    uint32_t dummy; ioread(x, 0x3da, 1, &dummy); iowrite(x, 0x3c0, 1, 0x20);
}

static void int10_mode3(X86 *x, bool no_clear) {
    /* Standard 80x25 color text mode.  The SDV firmware requests AX=0083h
     * during the handoff to its text console: bit 7 means preserve display
     * memory, while the actual BIOS mode number is still 03h. */
    static const uint8_t seq[5] = { 0x03,0x00,0x03,0x00,0x02 };
    static const uint8_t crtc[25] = {
        0x5f,0x4f,0x50,0x82,0x55,0x81,0xbf,0x1f,0x00,0x4f,0x0d,0x0e,0x00,
        0x00,0x00,0x50,0x9c,0x0e,0x8f,0x28,0x1f,0x96,0xb9,0xa3,0xff
    };
    static const uint8_t gc[9] = { 0,0,0,0,0,0x10,0x0e,0,0xff };
    static const uint8_t attr[21] = {
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x0c,0,0x0f,0x08,0
    };
    static const uint8_t ega[16][3] = {
        {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}
    };
    iowrite(x, 0x3c2, 1, 0x67);
    for (unsigned i = 0; i < 5; i++) {
        iowrite(x, 0x3c4, 1, i); iowrite(x, 0x3c5, 1, seq[i]);
    }
    iowrite(x, 0x3d4, 1, 0x11); iowrite(x, 0x3d5, 1, crtc[0x11] & 0x7f);
    for (unsigned i = 0; i < 25; i++) {
        iowrite(x, 0x3d4, 1, i); iowrite(x, 0x3d5, 1, crtc[i]);
    }
    for (unsigned i = 0; i < 9; i++) {
        iowrite(x, 0x3ce, 1, i); iowrite(x, 0x3cf, 1, gc[i]);
    }
    for (unsigned i = 0; i < 21; i++) {
        uint32_t dummy; ioread(x, 0x3da, 1, &dummy);
        iowrite(x, 0x3c0, 1, i); iowrite(x, 0x3c0, 1, attr[i]);
    }
    iowrite(x, 0x3c6, 1, 0xff); iowrite(x, 0x3c8, 1, 0);
    for (unsigned i = 0; i < 16; i++)
        for (unsigned c = 0; c < 3; c++) iowrite(x, 0x3c9, 1, ega[i][c]);
    uint32_t dummy; ioread(x, 0x3da, 1, &dummy); iowrite(x, 0x3c0, 1, 0x20);
    if (!no_clear) {
        for (unsigned i = 0; i < 80 * 25; i++) {
            wb(x, INT10_VRAM_BASE + i * 2, 1, ' ');
            wb(x, INT10_VRAM_BASE + i * 2 + 1, 1, 0x07);
        }
    }
}

static void int10_handler(X86 *x) {
    unsigned ah = xr(x, 4, 1), al = xr(x, 0, 1) & 0xFF;
    static unsigned int10_debug;
    if (int10_debug++ < 40)
        fprintf(stderr, "merced: INT10 ah=%02x al=%02x ip=%08x ninsts=%"
                PRIu64 "\n", ah, al, x->start, x->m->ninsts);
    switch (ah) {
    case 0x0E:                                      /* teletype output */
        int10_putc(x, 0, (uint8_t)al, 0x07, false);
        break;
    case 0x13: {                                    /* write string */
        unsigned mode = al & 3;
        unsigned page = xr(x, 7, 1) & 7;             /* BH */
        uint8_t attr = (uint8_t)xr(x, 3, 1);         /* BL */
        unsigned cnt = xr(x, 1, 2);                  /* CX */
        unsigned row = xr(x, 6, 1), col = xr(x, 2, 1); /* DH, DL */
        uint32_t str_addr = sbase(x, X_ES) + xr(x, 5, 2); /* ES:BP */
        int10_set_cursor(x, page, row, col);
        for (unsigned i = 0; i < cnt; i++) {
            uint32_t ch = 0, at = attr;
            if (!rb(x, str_addr++, 1, false, &ch)) break;
            if (mode & 2) {
                uint32_t a = 0;
                rb(x, str_addr++, 1, false, &a);
                at = a;
            }
            int10_putc(x, page, (uint8_t)ch, (uint8_t)at, true);
        }
        break;
    }
    case 0x02: {                                    /* set cursor position */
        unsigned page = xr(x, 7, 1) & 7;             /* BH */
        int10_set_cursor(x, page, xr(x, 6, 1), xr(x, 2, 1)); /* DH, DL */
        break;
    }
    case 0x03: {                                    /* get cursor position */
        unsigned page = xr(x, 7, 1) & 7, row, col;   /* BH */
        int10_get_cursor(x, page, &row, &col);
        setxr(x, 6, 1, row);                         /* DH */
        setxr(x, 2, 1, col);                          /* DL */
        setxr(x, 1, 2, 0x0607);                       /* CX: cursor shape */
        break;
    }
    case 0x0F: {                                    /* get video mode */
        unsigned cols, rows;
        int10_screen_dims(x, &cols, &rows);
        uint32_t mode = 0;
        rb(x, INT10_BDA_MODE, 1, false, &mode);
        setxr(x, 0, 1, mode ? mode : 3);              /* AL */
        setxr(x, 4, 1, cols);                         /* AH = columns */
        setxr(x, 7, 1, 0);                            /* BH = page 0 */
        break;
    }
    case 0x00:                                       /* set video mode */
        if ((al & 0x7f) == 0x03)
            int10_mode3(x, (al & 0x80) != 0);
        else if ((al & 0x7f) == 0x12)
            int10_mode12(x);
        wb(x, INT10_BDA_MODE, 1, al & 0x7f);
        break;
    default:
        /* Unimplemented function: no-op rather than halting, so a rare
         * call here doesn't take down an otherwise-working boot. */
        break;
    }
}

MercedStatus merced_ia32_step(Merced *m) {
    uint64_t entry_flags = m->ar[24];
    X86 x = {.m=m, .start=(uint32_t)m->ip, .pc=(uint32_t)m->ip,
             .seg_override=-1};
    unsigned d = (unsigned)((gr(&x, 25) >> 62) & 1);
    x.op32 = x.addr32 = d;
    uint32_t q;
    uint8_t op;
    for (;;) {
        if (!fetch(&x, 1, &q)) {
            /* The shared Merced MMU has already delivered a successful
             * IA-32 translation fault into the IA-64 IVT. */
            if (!(m->psr & (UINT64_C(1) << 34))) return MERCED_OK;
            return xhalt(&x, "undeliverable IA-32 fetch fault at %08X", x.pc);
        }
        op = (uint8_t)q;
        if (getenv("IA32_ZERO_DEBUG")) {
            static bool fired;
            static uint32_t run_start;
            static unsigned run_len;
            if (op == 0x00) {
                if (run_len == 0) run_start = x.start;
                run_len++;
                if (run_len == 20 && !fired) {
                    fired = true;
                    fprintf(stderr, "merced: IA32-ZERO-RUN-START at=%08X "
                            "ninsts=%" PRIu64 "\n", run_start,
                            m->ninsts - 19);
                    fflush(stderr);
                }
            } else {
                run_len = 0;
            }
        }
        if (op == 0x66) { x.op32 ^= 1; continue; }
        if (op == 0x67) { x.addr32 ^= 1; continue; }
        if (op == 0x26) { x.seg_override=X_ES; continue; }
        if (op == 0x2e) { x.seg_override=X_CS; continue; }
        if (op == 0x36) { x.seg_override=X_SS; continue; }
        if (op == 0x3e) { x.seg_override=X_DS; continue; }
        if (op == 0x64) { x.seg_override=X_FS; continue; }
        if (op == 0x65) { x.seg_override=X_GS; continue; }
        if (op == 0xf0) { x.locked = true; continue; }
        if (op == 0xf2 || op == 0xf3) { x.rep=op; continue; }
        break;
    }
    unsigned size = x.op32 ? 4 : 2;
    bool branch = false;

    unsigned hi = m->trace_history_next % MERCED_TRACE_HISTORY;
    unsigned hi_ext = m->trace_history_next % MERCED_TRACE_EXT_HISTORY;
    m->trace_history_next++;
    m->trace_history[hi].ip = x.start;
    m->trace_history[hi].raw = op;
    m->trace_history[hi].b0 = m->br[0];
    m->trace_history[hi].unit = 'x';
    m->trace_history[hi].qp = 0;
    m->trace_history_ext[hi_ext].src2 = gr(&x, 8);
    m->trace_history_ext[hi_ext].src3 = eflags(&x);
    m->trace_history_ext[hi_ext].r25 = gr(&x, 25);

    if (op == 0xf4) {
        /* HLT is an IA-32 instruction intercept.  Keep m->ip at x.start:
         * QEMU's helper restores the translated instruction's fault state
         * before raising this intercept, despite its temporary next-EIP
         * update in the translator. */
        unsigned prefix_len = (unsigned)(x.pc - x.start - 1);
        uint16_t code;
        if (!intercept_code(&x, prefix_len, &code)) goto fault;
        return merced_ia32_instruction_intercept(m, 0xf4, code); /* HLT */
    }
    if (op == 0x90) { /* nop */ }
    else if (op == 0x9b) {                     /* fwait */
        if (x.locked) {
            uint16_t code;
            if (!intercept_code(&x, x.pc - x.start - 1, &code)) goto fault;
            return merced_ia32_instruction_intercept(m, op, code);
        }
        MercedStatus status;
        if (!x87_ready(&x, true, true, &status)) return status;
    }
    else if (op >= 0x91 && op <= 0x97) {
        unsigned r=op&7; uint32_t a=xr(&x,0,size), b=xr(&x,r,size);
        setxr(&x,0,size,b); setxr(&x,r,size,a);
    }
    else if ((op & 6) == 4 && (op <= 0x0d || (op >= 0x10 && op <= 0x1d) ||
             (op >= 0x20 && op <= 0x3d))) {
        unsigned n=(op&1)?size:1; uint32_t imm,a=xr(&x,0,n),v;
        if(!fetch(&x,n,&imm))goto fault;
        unsigned family=op&0x38;
        uint32_t cf_in=eflags(&x)&FL_CF?1:0;
        if(family==0x00){v=a+imm;add_flags(&x,a,imm,v,n);}
        else if(family==0x08){v=a|imm;logic_flags(&x,v,n);}
        else if(family==0x10){v=a+imm+cf_in;adc_flags(&x,a,imm,cf_in,v,n);}
        else if(family==0x18){v=a-imm-cf_in;sbb_flags(&x,a,imm,cf_in,v,n);}
        else if(family==0x20){v=a&imm;logic_flags(&x,v,n);}
        else if(family==0x28||family==0x38){v=a-imm;sub_flags(&x,a,imm,v,n);}
        else {v=a^imm;logic_flags(&x,v,n);}
        if(family!=0x38)setxr(&x,0,n,v);
    }
    else if ((op <= 0x03) || (op >= 0x08 && op <= 0x0b) ||
             (op >= 0x10 && op <= 0x13) || (op >= 0x18 && op <= 0x1b) ||
             (op >= 0x20 && op <= 0x23) || (op >= 0x28 && op <= 0x2b) ||
             (op >= 0x30 && op <= 0x33) || (op >= 0x38 && op <= 0x3b)) {
        unsigned n = (op & 1) ? size : 1;
        bool reg_dest = (op & 2) != 0;
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if (!decode_rm(&x,mr,&rm)) goto fault;
        unsigned r=(mr>>3)&7; uint32_t rv=xr(&x,r,n), mv;
        if (!rmread(&x,rm,n,&mv)) goto fault;
        uint32_t a=reg_dest?rv:mv, b=reg_dest?mv:rv, v;
        unsigned family=op&0xf8;
        uint32_t cf_in=eflags(&x)&FL_CF?1:0;
        if (family==0x00) { v=a+b; add_flags(&x,a,b,v,n); }
        else if (family==0x08) { v=a|b; logic_flags(&x,v,n); }
        else if (family==0x10) { v=a+b+cf_in; adc_flags(&x,a,b,cf_in,v,n); }
        else if (family==0x18) { v=a-b-cf_in; sbb_flags(&x,a,b,cf_in,v,n); }
        else if (family==0x20) { v=a&b; logic_flags(&x,v,n); }
        else if (family==0x28 || family==0x38) { v=a-b; sub_flags(&x,a,b,v,n); }
        else { v=a^b; logic_flags(&x,v,n); }
        if (family!=0x38) {
            if (reg_dest) setxr(&x,r,n,v);
            else if (!rmwrite(&x,rm,n,v)) goto fault;
        }
    }
    else if (op >= 0xb0 && op <= 0xb7) {
        if (!fetch(&x, 1, &q)) goto fault;
        setxr(&x, op - 0xb0, 1, q);
    } else if (op >= 0xb8 && op <= 0xbf) {
        if (!fetch(&x, size, &q)) goto fault;
        setxr(&x, op - 0xb8, size, q);
    } else if (op >= 0x50 && op <= 0x57) {
        if (!push(&x, size, xr(&x, op - 0x50, size))) goto fault;
    } else if (op == 0x68 || op == 0x6a) {
        if (!fetch(&x,op==0x68?size:1,&q)) goto fault;
        if (op==0x6a) q=(uint32_t)(int32_t)(int8_t)q;
        if (!push(&x,size,q)) goto fault;
    } else if (op >= 0x58 && op <= 0x5f) {
        if (!pop(&x, size, &q)) goto fault;
        setxr(&x, op - 0x58, size, q);
    } else if (op >= 0x40 && op <= 0x4f) {
        unsigned r=op&7; uint32_t a=xr(&x,r,size);
        bool sub=op>=0x48; uint32_t v=sub?a-1:a+1;
        setxr(&x,r,size,v); incdec_flags(&x,a,1,v,size,sub);
    } else if (op == 0x06 || op == 0x0e || op == 0x16 || op == 0x1e) {
        unsigned s = op == 0x06 ? X_ES : op == 0x0e ? X_CS : op == 0x16 ? X_SS : X_DS;
        if (!push(&x, 2, sel(&x, s))) goto fault;
    } else if (op == 0x07 || op == 0x17 || op == 0x1f) {
        unsigned s = op == 0x07 ? X_ES : op == 0x17 ? X_SS : X_DS;
        if (!pop(&x, 2, &q)) goto fault;
        setseg_real(&x, s, q);
    } else if (op == 0x60 || op == 0x61) {
        uint32_t oldsp = stack_off(&x), vals[8];
        if (op == 0x60) {
            for (unsigned i=0;i<8;i++) vals[i]=xr(&x,i,size);
            for (int i=0;i<8;i++) if (!push(&x,size,i==4?oldsp:vals[i])) goto fault;
        } else {
            for (int i=7;i>=0;i--) if (!pop(&x,size,&vals[i])) goto fault;
            for (unsigned i=0;i<8;i++) if (i!=4) setxr(&x,i,size,vals[i]);
        }
    } else if (op == 0x9c) {
        if (!push(&x, size, eflags(&x))) goto fault;
    } else if (op == 0x9d) {
        if (!pop(&x, size, &q)) goto fault;
        setflags(&x, q);
    } else if (op == 0x9e) {                       /* sahf */
        uint32_t mask = FL_SF|FL_ZF|FL_AF|FL_PF|FL_CF;
        setflags(&x, (eflags(&x) & ~mask) | (xr(&x, 4, 1) & mask));
    } else if (op == 0x9f) {                       /* lahf */
        uint32_t mask = FL_SF|FL_ZF|FL_AF|FL_PF|FL_CF;
        setxr(&x, 4, 1, (eflags(&x) & mask) | 0x02);
    } else if (op == 0x98) {                       /* cbw / cwde */
        if (size == 2)
            setxr(&x, 0, 2, (uint16_t)(int16_t)(int8_t)xr(&x, 0, 1));
        else
            setxr(&x, 0, 4, (uint32_t)(int32_t)(int16_t)xr(&x, 0, 2));
    } else if (op == 0x99) {                       /* cwd / cdq */
        uint32_t a = xr(&x, 0, size);
        setxr(&x, 2, size,
              size == 2 ? ((a & 0x8000) ? 0xffff : 0)
                        : ((a & 0x80000000u) ? UINT32_MAX : 0));
    } else if (op == 0xfc) setflags(&x, eflags(&x) & ~FL_DF);
    else if (op == 0xfd) setflags(&x, eflags(&x) | FL_DF);
    else if (op >= 0x70 && op <= 0x7f) {
        if (!fetch(&x,1,&q)) goto fault;
        if (condition(&x,op&15)) { x.pc=near_target(&x,(int8_t)q); branch=true; }
    } else if (op >= 0xe0 && op <= 0xe3) {
        if (!fetch(&x,1,&q)) goto fault;
        unsigned n=x.addr32?4:2; uint32_t count=xr(&x,1,n);
        bool take;
        if(op==0xe3) take=count==0;
        else {
            count--; setxr(&x,1,n,count);
            take=count!=0 && (op==0xe2 || (op==0xe1)==!!(eflags(&x)&FL_ZF));
        }
        if (take) {
            x.pc=near_target(&x,(int8_t)q); branch=true;
        }
    } else if (op == 0xeb || op == 0xe9) {
        if (!fetch(&x, op==0xeb?1:size, &q)) goto fault;
        int32_t rel = op==0xeb ? (int8_t)q : size==2 ? (int16_t)q : (int32_t)q;
        x.pc=near_target(&x,rel); branch=true;
    } else if (op == 0xe8) {
        if (!fetch(&x,size,&q) || !push(&x,size,x.pc-sbase(&x,X_CS))) goto fault;
        x.pc=near_target(&x,size==2?(int16_t)q:(int32_t)q); branch=true;
    } else if (op == 0x9a) {                       /* call ptr16:16/32 */
        uint32_t off, cs;
        if (!fetch(&x,size,&off) || !fetch(&x,2,&cs)) goto fault;
        /* A far return pops the offset first and CS second, so CALL pushes
         * the old CS first, followed by the next instruction's offset.
         * The offset width follows the operand-size prefix; CS is always a
         * 16-bit selector. This matches the already-supported FF /3 form. */
        if (!push(&x,2,sel(&x,X_CS)) ||
            !push(&x,size,x.pc-sbase(&x,X_CS))) goto fault;
        setseg_real(&x,X_CS,(uint16_t)cs);
        x.pc=sbase(&x,X_CS)+(off&(size==2?UINT16_MAX:UINT32_MAX));
        branch=true;
    } else if (op == 0xea) {
        uint32_t off, cs;
        if (!fetch(&x,size,&off) || !fetch(&x,2,&cs)) goto fault;
        setseg_real(&x,X_CS,cs); x.pc=sbase(&x,X_CS)+(off&(size==2?0xffff:UINT32_MAX)); branch=true;
    } else if (op == 0xc2 || op == 0xc3 || op == 0xca || op == 0xcb) {
        uint32_t off, cs, cleanup = 0;
        /* Complete instruction fetch before consuming the return address:
         * a split imm16 fetch fault must retry with the original stack. */
        if ((op == 0xc2 || op == 0xca) && !fetch(&x, 2, &cleanup)) goto fault;
        if (!pop(&x,size,&off)) goto fault;
        if (op==0xca || op==0xcb) { if (!pop(&x,2,&cs)) goto fault; setseg_real(&x,X_CS,cs); }
        if (op==0xc2 || op==0xca) {
            set_stack_off(&x, stack_off(&x) + (uint16_t)cleanup);
        }
        x.pc=sbase(&x,X_CS)+(off&(size==2?0xffff:UINT32_MAX)); branch=true;
    } else if (op == 0xc9) {                       /* leave */
        unsigned stack_size = ((gr(&x, 26) >> 62) & 1) ? 4 : 2;
        uint32_t frame = xr(&x, 5, stack_size), saved;
        if (!merced_ia32_read_operand(m, sbase(&x, X_SS) + frame, size, &saved))
            goto fault;
        /* SS.B controls the implicit stack address independently of 67;
         * operand size controls the pop width and BP/EBP destination. */
        set_stack_off(&x, frame + size);
        setxr(&x, 5, size, saved);
    } else if (op == 0xc4 || op == 0xc5) {          /* les / lds */
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if(!decode_rm(&x,mr,&rm)||rm.is_reg)goto fault;
        uint32_t off,seg;
        if(!rb(&x,rm.addr,size,false,&off)||!rb(&x,rm.addr+size,2,false,&seg))goto fault;
        setxr(&x,(mr>>3)&7,size,off);
        setseg_real(&x,op==0xc4?X_ES:X_DS,(uint16_t)seg);
    } else if (op == 0xcd || op == 0xcc) {           /* int imm8 / int3 */
        uint32_t vec = 3;
        if (op == 0xcd && !fetch(&x,1,&vec)) goto fault;
        if (op == 0xcd && vec == 0x10) {
            /* No real video BIOS ROM behind the IVT to jump to - service
             * the call directly instead (see int10_handler()'s comment). */
            int10_handler(&x);
            /* The HLE handler uses the normal IA-32 MMU for BDA, VRAM and
             * VGA-I/O accesses. A miss transfers control to the native IVT
             * and clears PSR.is. Do not retire INT or overwrite that IVT IP
             * with x.pc; rfi will restore IA-32 mode and retry precisely. */
            if (!ia32_active(&x))
                return MERCED_OK;
        } else {
            uint32_t ip_lo, cs_lo;
            if (!push(&x,2,(uint16_t)eflags(&x)) ||
                !push(&x,2,sel(&x,X_CS)) ||
                !push(&x,2,(uint16_t)(x.pc-sbase(&x,X_CS))) ||
                !rb(&x,vec*4,2,false,&ip_lo) || !rb(&x,vec*4+2,2,false,&cs_lo))
                goto fault;
            setseg_real(&x,X_CS,(uint16_t)cs_lo);
            x.pc = sbase(&x,X_CS) + ip_lo;
            setflags(&x, eflags(&x) & ~(FL_IF));
            branch = true;
        }
    } else if (op == 0xcf) {                        /* iret */
        uint32_t ip_r, cs_r, fl_r;
        if (!pop(&x,2,&ip_r) || !pop(&x,2,&cs_r) || !pop(&x,2,&fl_r)) goto fault;
        setseg_real(&x,X_CS,(uint16_t)cs_r);
        x.pc = sbase(&x,X_CS) + ip_r;
        setflags(&x, fl_r);
        branch = true;
    } else if (op == 0xfa) setflags(&x, eflags(&x) & ~FL_IF);   /* cli */
    else if (op == 0xfb) setflags(&x, eflags(&x) | FL_IF);      /* sti */
    else if (op == 0xf8) setflags(&x, eflags(&x) & ~FL_CF);      /* clc */
    else if (op == 0xf9) setflags(&x, eflags(&x) | FL_CF);       /* stc */
    else if (op == 0xf5) setflags(&x, eflags(&x) ^ FL_CF);       /* cmc */
    else if (op == 0xe4 || op == 0xe5 || op == 0xe6 || op == 0xe7 ||
             op == 0xec || op == 0xed || op == 0xee || op == 0xef) {
        bool out = op == 0xe6 || op == 0xe7 || op == 0xee || op == 0xef;
        unsigned n = (op & 1) ? size : 1;
        uint16_t port;
        if (op >= 0xec) port = (uint16_t)xr(&x,2,2);
        else { if (!fetch(&x,1,&q)) goto fault; port = (uint8_t)q; }
        if (out) {
            if (!iowrite(&x,port,n,xr(&x,0,n))) goto fault;
        } else {
            if (!ioread(&x,port,n,&q)) goto fault;
            setxr(&x,0,n,q);
        }
    } else if (op == 0x84 || op == 0x85) {          /* test r/m, r */
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if (!decode_rm(&x,mr,&rm)) goto fault;
        unsigned n=op==0x84?1:size, reg=(mr>>3)&7; uint32_t a;
        if (!rmread(&x,rm,n,&a)) goto fault;
        logic_flags(&x, a & xr(&x,reg,n), n);
    } else if (op == 0x86 || op == 0x87) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if (!decode_rm(&x,mr,&rm)) goto fault;
        unsigned n=op==0x86?1:size, reg=(mr>>3)&7; uint32_t a,b;
        if(!rmread(&x,rm,n,&a))goto fault;
        b=xr(&x,reg,n);
        if(!rmwrite(&x,rm,n,b))goto fault;
        setxr(&x,reg,n,a);
    } else if (op == 0x8d) {                       /* lea */
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm;
        if (!decode_rm(&x,mr,&rm) || rm.is_reg) goto fault;
        setxr(&x,(mr>>3)&7,size,rm.off);
    } else if (op == 0x8f) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm;
        if (((mr>>3)&7)!=0 || !decode_rm(&x,mr,&rm)) goto fault;
        if (!pop(&x,size,&q) || !rmwrite(&x,rm,size,q)) goto fault;
    } else if (op == 0x88 || op == 0x8a || op == 0x8b || op == 0x89 ||
               op == 0x8c || op == 0x8e) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if (!decode_rm(&x,mr,&rm)) goto fault;
        unsigned reg=(mr>>3)&7;
        if (op==0x8a || op==0x8b) { unsigned n=op==0x8a?1:size; if(!rmread(&x,rm,n,&q))goto fault; setxr(&x,reg,n,q); }
        else if(op==0x88 || op==0x89) { unsigned n=op==0x88?1:size; if(!rmwrite(&x,rm,n,xr(&x,reg,n)))goto fault; }
        else if(op==0x8c) { if(reg>5||!rmwrite(&x,rm,2,sel(&x,reg)))goto fault; }
        else { if(reg>5||!rmread(&x,rm,2,&q))goto fault; setseg_real(&x,reg,q); }
    } else if (op == 0x69 || op == 0x6b) {           /* imul r, r/m, imm */
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if(!decode_rm(&x,mr,&rm)) goto fault;
        unsigned reg=(mr>>3)&7;
        uint32_t a, imm;
        if(!rmread(&x,rm,size,&a)) goto fault;
        if (op==0x69) { if(!fetch(&x,size,&imm)) goto fault; }
        else { if(!fetch(&x,1,&imm)) goto fault; imm=(uint32_t)(int32_t)(int8_t)imm; }
        int64_t sa = size==2 ? (int16_t)a : (int32_t)a;
        int64_t si = size==2 ? (int16_t)imm : (int32_t)imm;
        int64_t full = sa*si;
        uint32_t v = size==2 ? (uint32_t)(uint16_t)full : (uint32_t)full;
        int64_t sv = size==2 ? (int16_t)v : (int32_t)v;
        uint32_t f = eflags(&x) & ~(FL_CF|FL_OF);
        if (full != sv) f |= FL_CF|FL_OF;
        setflags(&x, f);
        setxr(&x, reg, size, v);
    } else if (op >= 0xa0 && op <= 0xa3) {
        uint32_t off; if(!fetch(&x,x.addr32?4:2,&off))goto fault;
        unsigned seg=x.seg_override>=0?(unsigned)x.seg_override:X_DS;
        uint32_t addr=sbase(&x,seg)+(x.addr32?off:(uint16_t)off);
        unsigned n=(op&1)?size:1;
        if(op<0xa2){if(!rb(&x,addr,n,false,&q))goto fault;setxr(&x,0,n,q);}
        else if(!wb(&x,addr,n,xr(&x,0,n)))goto fault;
    } else if (op == 0x2f) {                        /* das */
        uint32_t f = eflags(&x);
        uint32_t al = xr(&x, 0, 1);
        uint32_t old_al = al;
        bool old_cf = f & FL_CF, af = f & FL_AF, cf;
        if ((al & 0xf) > 9 || af) { al = (al - 6) & 0xff; af = true; }
        else af = false;
        if (old_al > 0x99 || old_cf) { al = (al - 0x60) & 0xff; cf = true; }
        else cf = false;
        f &= ~(FL_CF|FL_AF|FL_ZF|FL_SF|FL_PF);
        if (cf) f |= FL_CF;
        if (af) f |= FL_AF;
        if (!al) f |= FL_ZF;
        if (al & 0x80) f |= FL_SF;
        if (!__builtin_parity(al)) f |= FL_PF;
        setflags(&x, f);
        setxr(&x, 0, 1, al);
    } else if (op == 0x3c) {
        if (!fetch(&x,1,&q)) goto fault;
        uint32_t a=xr(&x,0,1), v=a-(uint8_t)q;
        sub_flags(&x,a,(uint8_t)q,v,1);
    } else if (op == 0xd7) {                       /* xlatb */
        /* AL <- byte ptr DS:[(E)BX + unsigned AL].  Address-size selects
         * BX versus EBX; an explicit segment override replaces DS. */
        unsigned seg = x.seg_override >= 0 ? (unsigned)x.seg_override : X_DS;
        uint32_t base = xr(&x, 3, x.addr32 ? 4 : 2);
        uint32_t off = base + xr(&x, 0, 1);
        if (!x.addr32) off = (uint16_t)off;
        if (!rb(&x, sbase(&x, seg) + off, 1, false, &q)) goto fault;
        setxr(&x, 0, 1, q);
    } else if (op == 0xc0 || op == 0xc1 ||
               op == 0xd0 || op == 0xd1 || op == 0xd2 || op == 0xd3) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm; if(!decode_rm(&x,mr,&rm))goto fault;
        unsigned sub=(mr>>3)&7, n=(op&1)?size:1, count;
        uint32_t v, old;
        if(!rmread(&x,rm,n,&v))goto fault;
        old=v;
        if(op==0xc0||op==0xc1){if(!fetch(&x,1,&q))goto fault;count=q&31;}
        else if(op==0xd0||op==0xd1)count=1;
        else count=xr(&x,1,1)&31;
        if(count){
            uint32_t cf=0, carry=!!(eflags(&x)&FL_CF), sign=1u<<(n*8-1);
            if(sub==6)sub=4; /* historical SAL alias */
            if(sub>7)
                return xhalt(&x,"IA-32 shift /%u unimplemented at %08X",sub,x.start);
            bool rotate=sub<=3;
            for(unsigned i=0;i<count;i++){
                if(sub==0){cf=!!(v&sign);v=(v<<1)|cf;}
                else if(sub==1){cf=v&1;v=(v>>1)|(cf?sign:0);}
                else if(sub==2){uint32_t oldcf=carry;cf=!!(v&sign);v=(v<<1)|oldcf;}
                else if(sub==3){uint32_t oldcf=carry;cf=v&1;v=(v>>1)|(oldcf?sign:0);}
                else if(sub==4){cf=!!(v&sign);v<<=1;}
                else {cf=v&1;
                    if(sub==5)v>>=1;
                    else if(n==1)v=(uint8_t)((int8_t)v>>1);
                    else if(n==2)v=(uint16_t)((int16_t)v>>1);
                    else v=(uint32_t)((int32_t)v>>1);
                }
                carry=cf;
            }
            if(n<4)v&=(1u<<(n*8))-1;
            if(!rotate)logic_flags(&x,v,n);
            uint32_t f=eflags(&x)&~(FL_CF|FL_OF);
            if(cf)f|=FL_CF;
            if(count==1){
                if((sub==0||sub==2||sub==4)&&((!!(v&sign))!=cf))f|=FL_OF;
                else if((sub==1||sub==3)&&((!!(v&sign))!=!!(v&(sign>>1))))f|=FL_OF;
                else if(sub==5&&(old&sign))f|=FL_OF;
            }
            setflags(&x,f);
            if(!rmwrite(&x,rm,n,v))goto fault;
        }
    } else if (op == 0xc6 || op == 0xc7) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm;
        if (((mr>>3)&7)!=0 || !decode_rm(&x,mr,&rm)) goto fault;
        unsigned n=op==0xc6?1:size;
        if (!fetch(&x,n,&q) || !rmwrite(&x,rm,n,q)) goto fault;
    } else if (op == 0x80 || op == 0x81 || op == 0x83) {
        if (!fetch(&x,1,&q)) goto fault;
        uint8_t mr=q; RM rm;
        if(!decode_rm(&x,mr,&rm))goto fault;
        unsigned sub=(mr>>3)&7;
        unsigned n=op==0x80?1:size;
        uint32_t a,imm; if(!rmread(&x,rm,n,&a)||!fetch(&x,op==0x83?1:n,&imm))goto fault;
        if(op==0x83) imm=(uint32_t)(int32_t)(int8_t)imm;
        uint32_t cf_in = !!(eflags(&x) & FL_CF);
        uint32_t v; if(sub==0)v=a+imm;
        else if(sub==1)v=a|imm;
        else if(sub==2)v=a+imm+cf_in;
        else if(sub==3)v=a-imm-cf_in;
        else if(sub==4)v=a&imm;
        else if(sub==6)v=a^imm;
        else if(sub==5||sub==7)v=a-imm;
        else return xhalt(&x,"IA-32 group1 /%u unimplemented at %08X",sub,x.start);
        if(sub!=7&&!rmwrite(&x,rm,n,v))goto fault;
        if(sub==3)sbb_flags(&x,a,imm,cf_in,v,n);
        else if(sub==2)adc_flags(&x,a,imm,cf_in,v,n);
        else if(sub==5||sub==7)sub_flags(&x,a,imm,v,n);
        else if(sub==0)add_flags(&x,a,imm,v,n);
        else logic_flags(&x,v,n);
    } else if (op == 0xa8 || op == 0xa9) {
        /* TEST AL/AX/EAX, imm: update arithmetic flags without storing the
         * result. Legacy IRQ handlers use A8 to test controller status. */
        unsigned n = op == 0xa8 ? 1 : size;
        uint32_t imm;
        if (!fetch(&x, n, &imm)) goto fault;
        logic_flags(&x, xr(&x, 0, n) & imm, n);
    } else if (op >= 0x6c && op <= 0x6f) {
        /* String port I/O. ATA BIOS code uses REP INSW to consume the
         * 256-word IDENTIFY response; implementing only scalar IN/OUT left
         * DRQ asserted forever even though the device data was ready. */
        bool out = op >= 0x6e;
        unsigned n = (op & 1) ? size : 1;
        unsigned asize = x.addr32 ? 4 : 2;
        uint32_t count = x.rep ? xr(&x, 1, asize) : 1;
        uint16_t port = (uint16_t)xr(&x, 2, 2);
        int step = (eflags(&x) & FL_DF) ? -(int)n : (int)n;
        unsigned source_seg = x.seg_override >= 0
                            ? (unsigned)x.seg_override : X_DS;
        while (count) {
            uint32_t v;
            if (out) {
                uint32_t si = xr(&x, 6, asize);
                if (!rb(&x, sbase(&x, source_seg) + si, n, false, &v) ||
                    !iowrite(&x, port, n, v))
                    goto fault;
                setxr(&x, 6, asize, si + step);
            } else {
                uint32_t di = xr(&x, 7, asize);
                /* INS destination is always ES:(E)DI; segment overrides do
                 * not apply to it.  Probe the destination before consuming
                 * the port value: a page fault must leave the I/O device
                 * untouched so the restarted instruction reads the same
                 * word.  Without this probe REP INSW dropped one ATAPI word
                 * at every new destination page.  The following write uses
                 * the translation established by the probe; these firmware
                 * buffers are ordinary read/write RAM. */
                uint32_t ignored;
                if (!rb(&x, sbase(&x, X_ES) + di, n, false, &ignored) ||
                    !ioread(&x, port, n, &v) ||
                    !wb(&x, sbase(&x, X_ES) + di, n, v))
                    goto fault;
                setxr(&x, 7, asize, di + step);
            }
            count--;
            if (x.rep)
                setxr(&x, 1, asize, count);
        }
    } else if (op == 0xa4 || op == 0xa5) {
        unsigned n = op == 0xa4 ? 1 : size, asize = x.addr32 ? 4 : 2;
        uint32_t count = x.rep ? xr(&x, 1, asize) : 1;
        int step = (eflags(&x) & FL_DF) ? -(int)n : (int)n;
        unsigned sg = x.seg_override >= 0 ? (unsigned)x.seg_override : X_DS;
        while (count) {
            uint32_t value, si = xr(&x, 6, asize), di = xr(&x, 7, asize);
            if (!merced_ia32_read_operand(m, sbase(&x, sg) + si, n, &value) ||
                !merced_ia32_write_operand(m, sbase(&x, X_ES) + di, n, value))
                goto fault;
            setxr(&x, 6, asize, si + step);
            setxr(&x, 7, asize, di + step);
            if (!x.rep) break;
            setxr(&x, 1, asize, --count);
        }
    } else if (op == 0xa6 || op == 0xa7) {        /* cmpsb / cmpsw / cmpsd */
        if (x.locked) {
            uint16_t code;
            if (!intercept_code(&x, x.pc - x.start - 1, &code)) goto fault;
            return merced_ia32_instruction_intercept(m, op, code);
        }
        unsigned n = op == 0xa6 ? 1 : size, asize = x.addr32 ? 4 : 2;
        uint32_t count = x.rep ? xr(&x, 1, asize) : 1;
        int step = (eflags(&x) & FL_DF) ? -(int)n : (int)n;
        unsigned sg = x.seg_override >= 0 ? (unsigned)x.seg_override : X_DS;
        uint64_t initial_flags = m->ar[24];
        while (count) {
            uint32_t si = xr(&x, 6, asize), di = xr(&x, 7, asize), src, dst;
            /* Only the source accepts a segment override. Neither index
             * advances until both complete operands have been read. */
            if (!merced_ia32_read_operand(m, sbase(&x, sg) + si, n, &src) ||
                !merced_ia32_read_operand(m, sbase(&x, X_ES) + di, n, &dst)) {
                if (x.rep) m->ar[24] = initial_flags;
                goto fault;
            }
            sub_flags(&x, src, dst, src - dst, n);
            setxr(&x, 6, asize, si + step);
            setxr(&x, 7, asize, di + step);
            if (!x.rep) break;
            setxr(&x, 1, asize, --count);
            if ((x.rep == 0xf3) != !!(eflags(&x) & FL_ZF)) break;
        }
    } else if (op == 0xac || op == 0xad) {
        unsigned n = op == 0xac ? 1 : size, asize = x.addr32 ? 4 : 2;
        uint32_t count = x.rep ? xr(&x, 1, asize) : 1;
        int step = (eflags(&x) & FL_DF) ? -(int)n : (int)n;
        unsigned sg = x.seg_override >= 0 ? (unsigned)x.seg_override : X_DS;
        while (count) {
            uint32_t value, si = xr(&x, 6, asize);
            if (!merced_ia32_read_operand(m, sbase(&x, sg) + si, n, &value)) goto fault;
            setxr(&x, 0, n, value);
            setxr(&x, 6, asize, si + step);
            if (!x.rep) break;
            setxr(&x, 1, asize, --count);
        }
    } else if (op == 0xae || op == 0xaf) {        /* scasb / scasw / scasd */
        unsigned n = op == 0xae ? 1 : size, asize = x.addr32 ? 4 : 2;
        uint32_t count = x.rep ? xr(&x, 1, asize) : 1;
        uint32_t acc = xr(&x, 0, n);
        int step = (eflags(&x) & FL_DF) ? -(int)n : (int)n;
        uint64_t initial_flags = m->ar[24];
        while (count) {
            uint32_t di = xr(&x, 7, asize), value;
            /* ES is implicit and cannot be replaced by a segment prefix. */
            if (!merced_ia32_read_operand(m, sbase(&x, X_ES) + di, n, &value)) {
                /* Completed iterations retain DI/CX. Restore entry flags
                 * on a repeated comparison fault so the unread suffix can
                 * restart independently of earlier comparisons. */
                if (x.rep) m->ar[24] = initial_flags;
                goto fault;
            }
            sub_flags(&x, acc, value, acc - value, n);
            setxr(&x, 7, asize, di + step);
            if (!x.rep) break;
            setxr(&x, 1, asize, --count);
            if ((x.rep == 0xf3) != !!(eflags(&x) & FL_ZF)) break;
        }
    } else if (op == 0xaa || op == 0xab) {
        unsigned n = op == 0xaa ? 1 : size, asize = x.addr32 ? 4 : 2;
        uint32_t count = x.rep ? xr(&x, 1, asize) : 1;
        int step = (eflags(&x) & FL_DF) ? -(int)n : (int)n;
        uint32_t value = xr(&x, 0, n);
        while (count) {
            uint32_t di = xr(&x, 7, asize);
            if (!merced_ia32_write_operand(m, sbase(&x, X_ES) + di, n, value)) goto fault;
            setxr(&x, 7, asize, di + step);
            if (!x.rep) break;
            setxr(&x, 1, asize, --count);
        }
    } else if (op >= 0xd8 && op <= 0xdf) {
        unsigned prefix_len = x.pc - x.start - 1;
        if (!fetch(&x, 1, &q)) goto fault;
        uint8_t mr = q;
        unsigned sub = (mr >> 3) & 7;
        bool clear = op == 0xdb && mr == 0xe2; /* fnclex */
        bool load = op == 0xd9 && sub == 5 && mr < 0xc0; /* fldcw m16 */
        bool store = op == 0xd9 && sub == 7 && mr < 0xc0; /* fnstcw m16 */
        bool status_ax = op == 0xdf && mr == 0xe0; /* fnstsw ax */
        bool fild = op == 0xdb && sub == 0 && mr < 0xc0;
        bool fld = op == 0xdd && sub == 0 && mr < 0xc0;
        bool fmul = op == 0xdc && sub == 1 && mr < 0xc0;
        bool fcomp = op == 0xdc && sub == 3 && mr < 0xc0;
        bool fst = op == 0xdd && (sub == 2 || sub == 3) && mr < 0xc0;
        bool fstp_reg = op == 0xdd && (mr & 0xf8) == 0xd8;
        bool numeric = fild || fld || fmul || fcomp || fst || fstp_reg;
        if (!clear && !load && !store && !status_ax && !numeric)
            return xhalt(&x, "IA-32 x87 %02X %02X unimplemented at %08X", op, mr, x.start);
        RM rm = {.is_reg = true, .reg = mr & 7};
        if (!clear && !status_ax && !decode_rm(&x, mr, &rm)) goto fault;
        if (x.locked) {
            uint16_t code;
            if (!intercept_code(&x, prefix_len, &code)) goto fault;
            return merced_ia32_instruction_intercept(m, op | ((uint64_t)mr << 8), code);
        }
        MercedStatus status;
        /* FNSTCW/FNCLEX are the no-wait forms. An explicit 9B is its own
         * waiting instruction; FLDCW checks existing pending exceptions. */
        if (!x87_ready(&x, load || numeric, false, &status)) return status;
        if (clear) {
            m->ar[28] &= ~UINT64_C(0x80ff); /* exception flags, SF, ES, B */
        } else if (status_ax) {
            setxr(&x, 0, 2, m->ar[28]);
        } else if (store) {
            if (!wb(&x, rm.addr, 2, (uint16_t)m->ar[21])) goto fault;
        } else if (load) {
            uint32_t word;
            if (!rb(&x, rm.addr, 2, false, &word)) goto fault;
            uint16_t fcw = (word & 0x1f3f) | 0x40;
            m->ar[21] = (m->ar[21] & ~UINT64_C(0xffff)) | fcw;
            bool pending = (m->ar[28] & ~fcw & 0x3f) != 0;
            m->ar[28] = (m->ar[28] & ~UINT64_C(0x8080)) | (pending ? 0x8080 : 0);
        } else {
            /* Resolve all memory faults before changing FP status, pointers,
             * tags or TOP. Stores need W permission even if an unmasked
             * numeric exception subsequently suppresses their write cycle. */
            uint64_t data = 0;
            if (fst) {
                if (!merced_ia32_probe_write(m, rm.addr, 8)) goto fault;
            } else if (!rm.is_reg &&
                       !merced_ia32_read(m, rm.addr, fild ? 4 : 8, false, &data)) goto fault;
            unsigned top = x87_top(m), exceptions = 0;
            uint16_t fcw = m->ar[21];
            bool c1 = false;
            X87Number a = x87_from_fr(m->fr[8 + top]), result = a;
            x87_pointers(&x, op, mr, &rm);
            if (fild || fld) {
                unsigned next = (top + 7) & 7;
                if (!x87_empty(m, next)) {
                    exceptions = X87_IE | X87_SF;
                    c1 = true;
                    result = x87_indefinite();
                } else if (fild) {
                    int64_t integer = (int32_t)data;
                    result = x87_normalize(integer < 0 ? -integer : integer, 63, integer < 0);
                } else {
                    result = x87_from_double(data);
                    if (x87_signaling(result)) {
                        exceptions = X87_IE;
                        result.sig |= UINT64_C(1) << 62;
                    } else if (result.denormal) exceptions = X87_DE;
                }
                /* FLD has a specific exception rule: even an unmasked
                 * denormal-operand exception pushes the normalized value. */
                if (!(exceptions & ~fcw & X87_IE)) {
                    x87_set_top(m, next);
                    x87_put(m, next, result);
                }
            } else {
                bool empty = x87_empty(m, top);
                if (empty) {
                    exceptions = X87_IE | X87_SF;
                    a = result = x87_indefinite();
                }
                if (fmul) {
                    if (!empty) result = x87_multiply(a, x87_from_double(data), fcw,
                                                       &exceptions, &c1);
                    if (!(exceptions & ~fcw & (X87_IE | X87_DE))) x87_put(m, top, result);
                } else if (fcomp) {
                    unsigned cc = empty ? X87_C0 | X87_C2 | X87_C3 :
                                  x87_compare(a, x87_from_double(data), &exceptions);
                    if (!(exceptions & ~fcw & (X87_IE | X87_DE))) {
                        m->ar[28] = (m->ar[28] & ~UINT64_C(0x4500)) | cc;
                        x87_pop(m);
                    }
                } else if (fst) {
                    uint64_t bits = x87_store_double(a, fcw, &exceptions, &c1);
                    if (!(exceptions & ~fcw & (X87_IE | X87_OE | X87_UE))) {
                        if (!merced_ia32_write(m, rm.addr, 8, bits)) goto fault;
                        if (sub == 3) x87_pop(m);
                    }
                } else { /* FSTP ST(i), including the common discard ST(0). */
                    if (!(exceptions & ~fcw & X87_IE)) {
                        unsigned target = (top + rm.reg) & 7;
                        if (empty) x87_put(m, target, a);
                        else {
                            /* Register stores transfer the extended encoding
                             * unchanged, even SNaN or unsupported patterns. */
                            m->fr[8 + target] = m->fr[8 + top];
                            m->ar[28] &= ~(UINT64_C(1) << (16 + 2 * target));
                        }
                        x87_pop(m);
                    }
                }
            }
            x87_status(m, exceptions, c1);
        }
    } else if (op == 0xf6 || op == 0xf7) {
        if(!fetch(&x,1,&q))goto fault;
        uint8_t mr=q;RM rm;if(!decode_rm(&x,mr,&rm))goto fault;
        unsigned sub=(mr>>3)&7,n=op==0xf6?1:size;uint32_t src;
        if(!rmread(&x,rm,n,&src))goto fault;
        if(sub==0){uint32_t imm;if(!fetch(&x,n,&imm))goto fault;
            logic_flags(&x,src&imm,n);
        } else if(sub==4){
            if(n==1){uint16_t v=(uint16_t)xr(&x,0,1)*(uint8_t)src;
                setxr(&x,0,2,v);setflags(&x,(eflags(&x)&~(FL_CF|FL_OF))|((v>>8)?FL_CF|FL_OF:0));}
            else {uint64_t v=(uint64_t)xr(&x,0,n)*src;
                setxr(&x,0,n,(uint32_t)v);setxr(&x,2,n,(uint32_t)(v>>(n*8)));
                setflags(&x,(eflags(&x)&~(FL_CF|FL_OF))|((v>>(n*8))?FL_CF|FL_OF:0));}
        } else if(sub==2){uint32_t v=~src;if(!rmwrite(&x,rm,n,v))goto fault;}
        else if(sub==3){uint32_t v=(uint32_t)-(int32_t)src;
            if(!rmwrite(&x,rm,n,v))goto fault;
            sub_flags(&x,0,src,v,n);}
        else if(sub==5){
            if(n==1){int16_t v=(int8_t)xr(&x,0,1)*(int8_t)src;
                setxr(&x,0,2,(uint16_t)v);
                int16_t ext=(int16_t)(int8_t)(uint8_t)v;
                setflags(&x,(eflags(&x)&~(FL_CF|FL_OF))|(v!=ext?FL_CF|FL_OF:0));}
            else {int64_t v=(int64_t)(int32_t)xr(&x,0,n)*(int64_t)(int32_t)src;
                setxr(&x,0,n,(uint32_t)v);setxr(&x,2,n,(uint32_t)((uint64_t)v>>(n*8)));
                int64_t ext=n==2?(int64_t)(int16_t)(uint16_t)v:(int64_t)(int32_t)(uint32_t)v;
                setflags(&x,(eflags(&x)&~(FL_CF|FL_OF))|(v!=ext?FL_CF|FL_OF:0));}
        } else if(sub==6 || sub==7){
            bool sgn=sub==7;
            uint64_t dividend; uint32_t divisor=src;
            if(n==1) dividend=xr(&x,0,2);
            else dividend=((uint64_t)xr(&x,2,n)<<(n*8))|xr(&x,0,n);
            uint64_t q,r;bool overflow=false;
            if(divisor==0) overflow=true;
            else if(sgn){
                int64_t sd=n==1?(int16_t)dividend:n==2?(int32_t)(int16_t)dividend:(int64_t)(int32_t)dividend;
                int64_t sv=n==1?(int8_t)divisor:n==2?(int16_t)divisor:(int32_t)divisor;
                int64_t sq=sd/sv,sr=sd%sv;
                int64_t qmax=n==1?127:n==2?32767:INT32_MAX,qmin=n==1?-128:n==2?-32768:INT32_MIN;
                if(sq>qmax||sq<qmin) overflow=true;
                q=(uint64_t)sq;r=(uint64_t)sr;
            } else {
                uint64_t uv=divisor;
                q=dividend/uv;r=dividend%uv;
                uint64_t qmax=n==1?255:n==2?65535:UINT32_MAX;
                if(q>qmax) overflow=true;
            }
            if(overflow){
                /* #DE (divide error), vector 0 - same delivery as int N,
                 * but processor-generated: no immediate byte, and the
                 * pushed return address is this instruction (not the
                 * next one), so a handler can retry after fixing things
                 * up. real hardware behavior; DOS/BIOS-era real-mode code
                 * occasionally relies on it for divide-by-zero traps. */
                uint32_t ip_lo,cs_lo;
                if(!push(&x,2,(uint16_t)eflags(&x))||!push(&x,2,sel(&x,X_CS))||
                   !push(&x,2,(uint16_t)(x.start-sbase(&x,X_CS)))||
                   !rb(&x,0,2,false,&ip_lo)||!rb(&x,2,2,false,&cs_lo))goto fault;
                setseg_real(&x,X_CS,(uint16_t)cs_lo);
                x.pc=sbase(&x,X_CS)+ip_lo;
                setflags(&x,eflags(&x)&~FL_IF);
                branch=true;
            } else {
                setxr(&x,0,n==1?1:n,(uint32_t)q);
                setxr(&x,n==1?4:2,n==1?1:n,(uint32_t)r);
            }
        } else return xhalt(&x,"IA-32 group3 /%u unimplemented at %08X",sub,x.start);
    } else if (op == 0xfe || op == 0xff) {
        if(!fetch(&x,1,&q))goto fault;
        uint8_t mr=q;RM rm;
        if(!decode_rm(&x,mr,&rm))goto fault;
        unsigned sub=(mr>>3)&7;
        unsigned n=op==0xfe?1:size;
        if(sub<=1){uint32_t a,v;if(!rmread(&x,rm,n,&a))goto fault;v=sub?a-1:a+1;
            if(!rmwrite(&x,rm,n,v))goto fault;
            incdec_flags(&x,a,1,v,n,sub!=0);}
        else if(op==0xff && sub==2){uint32_t off;if(!rmread(&x,rm,n,&off)||
            !push(&x,n,x.pc-sbase(&x,X_CS)))goto fault;
            x.pc=sbase(&x,X_CS)+(off&(n==2?UINT16_MAX:UINT32_MAX));branch=true;}
        else if(op==0xff && sub==6){uint32_t v;if(!rmread(&x,rm,n,&v)||!push(&x,n,v))goto fault;}
        else if(op==0xff && sub==4){uint32_t off;if(!rmread(&x,rm,n,&off))goto fault;
            x.pc=sbase(&x,X_CS)+(off&(n==2?UINT16_MAX:UINT32_MAX));branch=true;}
        else if(op==0xff && sub==3 && !rm.is_reg){uint32_t off,cs;if(!rb(&x,rm.addr,size,false,&off)||!rb(&x,rm.addr+size,2,false,&cs))goto fault;
            if(!push(&x,2,sel(&x,X_CS))||!push(&x,size,x.pc-sbase(&x,X_CS)))goto fault;
            setseg_real(&x,X_CS,cs);x.pc=sbase(&x,X_CS)+(off&(size==2?0xffff:UINT32_MAX));branch=true;}
        else if(op==0xff && sub==5 && !rm.is_reg){uint32_t off,cs;if(!rb(&x,rm.addr,size,false,&off)||!rb(&x,rm.addr+size,2,false,&cs))goto fault;
            setseg_real(&x,X_CS,cs);x.pc=sbase(&x,X_CS)+(off&(size==2?0xffff:UINT32_MAX));branch=true;}
        else return xhalt(&x,"IA-32 %02X /%u unimplemented at %08X",op,sub,x.start);
    } else if (op == 0x0f) {
        uint32_t op2;if(!fetch(&x,1,&op2))goto fault;
        if(op2>=0x80 && op2<=0x8f){
            if(!fetch(&x,size,&q))goto fault;
            int32_t rel=size==2?(int16_t)q:(int32_t)q;
            if(condition(&x,op2&15)){x.pc=near_target(&x,rel);branch=true;}
        } else if(op2>=0x90 && op2<=0x9f){
            if(!fetch(&x,1,&q))goto fault;
            RM rm;if(!decode_rm(&x,(uint8_t)q,&rm)||
                !rmwrite(&x,rm,1,condition(&x,op2&15)?1:0))goto fault;
        } else if (op2 >= 0xc8 && op2 <= 0xcf) { /* bswap r32 */
            if (x.locked) {
                uint16_t code;
                if (!intercept_code(&x, x.pc - x.start - 2, &code)) goto fault;
                return merced_ia32_instruction_intercept(m, 0x0f | ((uint64_t)op2 << 8), code);
            }
            /* Intel defines BSWAP only for a 32-bit operand. For the
             * undefined 16-bit form, use deterministic zero in the word;
             * upper register bits and all EFLAGS remain unchanged. */
            setxr(&x, op2 & 7, size,
                  size == 4 ? __builtin_bswap32(xr(&x, op2 & 7, 4)) : 0);
        } else if (op2 == 0xbc || op2 == 0xbd) { /* bsf/bsr r, r/m */
            unsigned prefix_len = x.pc - x.start - 2;
            uint32_t mr, value;
            if (!fetch(&x, 1, &mr)) goto fault;
            RM rm;
            if (!decode_rm(&x, mr, &rm)) goto fault;
            if (x.locked) {
                uint16_t code;
                if (!intercept_code(&x, prefix_len, &code)) goto fault;
                return merced_ia32_instruction_intercept(m,
                           0x0f | ((uint64_t)op2 << 8) | ((uint64_t)mr << 16), code);
            }
            if (!rmread(&x, rm, size, &value)) goto fault;
            /* ZF is the only defined flag. Retain other flags and, for
             * a zero source, the architecturally undefined destination. */
            if (value) {
                unsigned index = op2 == 0xbd ? 31 - __builtin_clz(value) :
                                               __builtin_ctz(value);
                setxr(&x, (mr >> 3) & 7, size, index);
                setflags(&x, eflags(&x) & ~FL_ZF);
            } else setflags(&x, eflags(&x) | FL_ZF);
        } else if (op2 == 0xa4 || op2 == 0xa5 ||
                   op2 == 0xac || op2 == 0xad) { /* shld/shrd r/m, r, imm8/CL */
            unsigned prefix_len = x.pc - x.start - 2;
            uint32_t mr, count;
            if (!fetch(&x, 1, &mr)) goto fault;
            RM rm;
            if (!decode_rm(&x, mr, &rm)) goto fault;
            if (!(op2 & 1)) {
                if (!fetch(&x, 1, &count)) goto fault;
            } else {
                count = xr(&x, 1, 1);
            }
            if (x.locked) {
                uint16_t code;
                if (!intercept_code(&x, prefix_len, &code)) goto fault;
                return merced_ia32_instruction_intercept(m,
                           0x0f | ((uint64_t)op2 << 8) | ((uint64_t)mr << 16), code);
            }
            unsigned width = size * 8;
            uint32_t src = xr(&x, (mr >> 3) & 7, size), dest;
            if (rm.is_reg) {
                dest = xr(&x, rm.reg, size);
            } else {
                uint64_t value;
                /* Even a zero count checks destination write permission.
                 * Validate every byte before touching a memory-mapped device. */
                if (!merced_ia32_read_for_write(m, rm.addr, size, &value)) goto fault;
                dest = value;
            }
            count &= 31;
            if (count) {
                bool right = (op2 & 8) != 0;
                uint64_t pair = right ? ((uint64_t)src << width) | dest :
                                        ((uint64_t)dest << width) | src;
                uint32_t result = right ? pair >> count : (pair << count) >> width;
                if (size == 2) result &= 0xffff;
                if (!rmwrite(&x, rm, size, result)) goto fault;
                /* Intel leaves the entire result/flags undefined for a
                 * 16-bit operand shifted by more than 16. The concatenation
                 * above gives a deterministic result without invalid shifts;
                 * preserve flags for that case and undefined AF/OF otherwise. */
                if (count <= width) {
                    unsigned cf = right ? (dest >> (count - 1)) & 1 :
                                          (dest >> (width - count)) & 1;
                    uint32_t f = eflags(&x) & ~(FL_CF | FL_SF | FL_ZF | FL_PF);
                    if (cf) f |= FL_CF;
                    if (result & (UINT32_C(1) << (width - 1))) f |= FL_SF;
                    if (!result) f |= FL_ZF;
                    if (!__builtin_parity(result & 0xff)) f |= FL_PF;
                    if (count == 1) {
                        f &= ~FL_OF;
                        if ((dest ^ result) & (UINT32_C(1) << (width - 1))) f |= FL_OF;
                    }
                    setflags(&x, f);
                }
            }
        } else if(op2==0xaf){                         /* imul r, r/m */
            if(!fetch(&x,1,&q))goto fault;
            uint8_t mr=(uint8_t)q;RM rm;if(!decode_rm(&x,mr,&rm))goto fault;
            uint32_t a;if(!rmread(&x,rm,size,&a))goto fault;
            unsigned reg=(mr>>3)&7;
            int64_t lhs=size==2?(int16_t)xr(&x,reg,size):(int32_t)xr(&x,reg,size);
            int64_t rhs=size==2?(int16_t)a:(int32_t)a;
            int64_t full=lhs*rhs;
            uint32_t v=size==2?(uint16_t)full:(uint32_t)full;
            int64_t truncated=size==2?(int16_t)v:(int32_t)v;
            uint32_t f=eflags(&x)&~(FL_CF|FL_OF);
            if(full!=truncated)f|=FL_CF|FL_OF;
            setflags(&x,f);setxr(&x,reg,size,v);
        } else if (op2 == 0xb0 || op2 == 0xb1) {     /* cmpxchg r/m, r */
            uint32_t mr;
            if (!fetch(&x, 1, &mr)) goto fault;
            RM rm;
            if (!decode_rm(&x, (uint8_t)mr, &rm)) goto fault;
            unsigned n = op2 == 0xb0 ? 1 : size;
            uint64_t dest;
            uint32_t acc = xr(&x, 0, n), src = xr(&x, (mr >> 3) & 7, n);
            /* Both outcomes issue a destination write cycle. In particular,
             * mismatch must still fault on a read-only page. Commit flags
             * and the accumulator only after that write succeeds, so a
             * translation/permission fault can restart this instruction. */
            if (rm.is_reg) {
                dest = xr(&x, rm.reg, n);
                setxr(&x, rm.reg, n, acc == dest ? src : dest);
            } else if (!merced_ia32_cmpxchg(m, rm.addr, n, acc, src, &dest)) {
                goto fault;
            }
            if (acc != dest) setxr(&x, 0, n, dest);
            sub_flags(&x, acc, dest, acc - dest, n);
        } else if (op2 == 0xc0 || op2 == 0xc1) { /* xadd r/m, r */
            uint32_t mr;
            if (!fetch(&x, 1, &mr)) goto fault;
            RM rm;
            if (!decode_rm(&x, mr, &rm)) goto fault;
            unsigned n = op2 == 0xc0 ? 1 : size, reg = (mr >> 3) & 7;
            uint32_t src = xr(&x, reg, n), dest;
            if (rm.is_reg) {
                if (x.locked) {
                    uint16_t code;
                    if (!intercept_code(&x, x.pc - x.start - 3, &code)) goto fault;
                    return merced_ia32_instruction_intercept(m,
                               0x0f | ((uint64_t)op2 << 8) | ((uint64_t)mr << 16), code);
                }
                dest = xr(&x, rm.reg, n);
                /* Capture both originals; the destination write wins when
                 * both operands name the same register or byte lane. */
                setxr(&x, reg, n, dest);
                setxr(&x, rm.reg, n, dest + src);
            } else {
                if (!merced_ia32_xadd(m, rm.addr, n, src, &dest)) goto fault;
                setxr(&x, reg, n, dest);
            }
            add_flags(&x, dest, src, dest + src, n);
        } else if (op2 == 0xc7) {                 /* cmpxchg8b m64 */
            uint32_t mr;
            if (!fetch(&x, 1, &mr)) goto fault;
            if (((mr >> 3) & 7) != 1)
                return xhalt(&x, "IA-32 0F C7 /%u unimplemented at %08X",
                              (mr >> 3) & 7, x.start);
            RM rm;
            if (!decode_rm(&x, mr, &rm)) goto fault;
            if (rm.is_reg) {
                /* In the Itanium system environment #UD is redirected to
                 * the IA-32 instruction-intercept handler. */
                uint16_t code;
                if (!intercept_code(&x, x.pc - x.start - 3, &code)) goto fault;
                return merced_ia32_instruction_intercept(m,
                           UINT64_C(0xc70f) | ((uint64_t)mr << 16), code);
            }
            uint64_t acc = ((uint64_t)xr(&x, 2, 4) << 32) | xr(&x, 0, 4);
            uint64_t src = ((uint64_t)xr(&x, 1, 4) << 32) | xr(&x, 3, 4), dest;
            /* This form remains 64 bits even with a 66 prefix. */
            if (!merced_ia32_cmpxchg(m, rm.addr, 8, acc, src, &dest)) goto fault;
            if (acc != dest) {
                setxr(&x, 0, 4, (uint32_t)dest);
                setxr(&x, 2, 4, (uint32_t)(dest >> 32));
            }
            setflags(&x, (eflags(&x) & ~FL_ZF) | (acc == dest ? FL_ZF : 0));
        } else if(op2==0xb6 || op2==0xb7 || op2==0xbe || op2==0xbf){
            uint32_t mr;if(!fetch(&x,1,&mr))goto fault;RM rm;
            if(!decode_rm(&x,mr,&rm))goto fault;
            unsigned n=(op2&1)?2:1;uint32_t v;if(!rmread(&x,rm,n,&v))goto fault;
            if(op2>=0xbe)v=n==1?(uint32_t)(int32_t)(int8_t)v:(uint32_t)(int32_t)(int16_t)v;
            setxr(&x,(mr>>3)&7,size,v);
        } else if (op2 == 0xb8) {                  /* JMPE disp16/disp32 */
            uint32_t target = 0;
            /* Immediate bytes may span instruction pages. */
            for (unsigned i = 0; i < size; i++) {
                if (!fetch(&x, 1, &q)) goto fault;
                target |= q << (8 * i);
            }
            if (m->psr & (UINT64_C(1) << 22))
                return merced_ia32_disabled_transition(m);
            return jmpe(&x, target);
        } else if(op2==0x00){uint32_t mr;if(!fetch(&x,1,&mr))goto fault;
            if (((mr >> 3) & 7) == 6) {
                RM rm;
                if (!decode_rm(&x, mr, &rm)) goto fault;
                /* Instruction fetch faults precede DI, which in turn
                 * precedes faults on an indirect data operand. */
                if (m->psr & (UINT64_C(1) << 22))
                    return merced_ia32_disabled_transition(m);
                if (!rmread(&x, rm, size, &q)) goto fault;
                return jmpe(&x, q);
            }
            return xhalt(&x,"IA-32 0F %02X unimplemented at %08X",op2,x.start);
        } else if(op2==0xa2){
            /* CPUID. Only leaf 0 (vendor string + max leaf) and leaf 1
             * (family/model/stepping + feature flags) are modeled; any
             * other leaf falls back to the leaf-1 shape rather than
             * halting. Feature flags advertise FPU only. RDTSC and
             * other optional instructions remain incomplete, so keep the
             * existing conservative feature mask until validated together. */
            uint32_t a,b,c,d;
            if(xr(&x,0,4)==0){a=1;b=0x756e6547;d=0x49656e69;c=0x6c65746e;}
            else{a=0x00000601;b=0;c=0;d=0x00000001;}
            setxr(&x,0,4,a);setxr(&x,3,4,b);setxr(&x,1,4,c);setxr(&x,2,4,d);
        } else return xhalt(&x,"IA-32 0F %02X unimplemented at %08X",op2,x.start);
    } else return xhalt(&x,"IA-32 opcode %02X unimplemented at %08X",op,x.start);

    if (ia32_ata_trace_left) {
        fprintf(stderr, "merced: IA32-ATA-TRACE from=%08X op=%02X to=%08X"
                " ax=%08X bx=%08X cx=%08X dx=%08X si=%08X di=%08X"
                " flags=%08X branch=%u ninsts=%" PRIu64 "\n",
                x.start, op, x.pc, xr(&x, 0, 4), xr(&x, 3, 4),
                xr(&x, 1, 4), xr(&x, 2, 4), xr(&x, 6, 4), xr(&x, 7, 4),
                eflags(&x), branch, m->ninsts);
        fflush(stderr);
        ia32_ata_trace_left--;
    }
    if (getenv("IA32_TRACE_AFTER")) {
        static unsigned trace_count;
        uint64_t after = strtoull(getenv("IA32_TRACE_AFTER"), NULL, 0);
        if (m->ninsts >= after && trace_count++ < 500) {
            fprintf(stderr, "merced: IA32-TRACE from=%08X op=%02X to=%08X"
                    " cs=%04X csd=%016" PRIX64 " ss=%04X ssd=%016" PRIX64
                    " sp=%08X branch=%u ninsts=%" PRIu64 "\n",
                    x.start, op, x.pc, sel(&x, X_CS), gr(&x, 25),
                    sel(&x, X_SS), gr(&x, 26), stack_off(&x), branch,
                    m->ninsts);
            fflush(stderr);
        }
    }
    if (getenv("IA32_JUMP_DEBUG") &&
        x.pc >= UINT64_C(0x9F000) && x.pc < UINT64_C(0xA0000) &&
        (x.start < UINT64_C(0x9F000) || x.start >= UINT64_C(0xA0000))) {
        fprintf(stderr, "merced: IA32-ENTER-EBDA from=%08X op=%02X to=%08X "
                "ninsts=%" PRIu64 "\n", x.start, op, x.pc, m->ninsts);
        fflush(stderr);
    }
    m->ip=x.pc; m->ninsts++; return MERCED_OK;
fault:
    /* A memory destination may fault after the arithmetic result was
     * calculated. Retrying ADC/SBB must see the original carry, and an
     * incomplete comparison must not expose its temporary flags. */
    m->ar[24] = entry_flags;
    if (!(m->psr & (UINT64_C(1) << 34))) return MERCED_OK;
    return xhalt(&x,"IA-32 memory fault at %08X (instruction %08X)",x.pc,x.start);
}
