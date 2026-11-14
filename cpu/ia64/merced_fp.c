#include "merced_fp.h"

#include <string.h>

#define FP_EXP_MAX  0x1FFFFu
#define FP_EXP_BIAS 0xFFFF

/* A 384-bit accumulator, w[0] the low word, for operands whose scales lie
 * too far apart for the 256-bit fast path. */
#define ACC_WORDS 6
typedef struct { uint64_t w[ACC_WORDS]; } Acc;

static void acc_shl(Acc *a, unsigned n) {
    unsigned wsh = n / 64u, bsh = n % 64u;
    for (int i = ACC_WORDS - 1; i >= 0; i--) {
        uint64_t v = 0;
        int src = i - (int)wsh;
        if (src >= 0) {
            v = a->w[src] << bsh;
            if (bsh && src > 0)
                v |= a->w[src - 1] >> (64u - bsh);
        }
        a->w[i] = v;
    }
}

/* Right shift; true when a 1 bit was shifted out. */
static bool acc_shr_sticky(Acc *a, unsigned n) {
    if (n >= 64u * ACC_WORDS) {
        bool any = false;
        for (unsigned i = 0; i < ACC_WORDS; i++) {
            any |= a->w[i] != 0;
            a->w[i] = 0;
        }
        return any;
    }
    unsigned wsh = n / 64u, bsh = n % 64u;
    bool sticky = false;
    for (unsigned i = 0; i < wsh; i++)
        sticky |= a->w[i] != 0;
    if (bsh)
        sticky |= (a->w[wsh] & ((UINT64_C(1) << bsh) - 1u)) != 0;
    for (unsigned i = 0; i < ACC_WORDS; i++) {
        unsigned src = i + wsh;
        uint64_t v = 0;
        if (src < ACC_WORDS) {
            v = a->w[src] >> bsh;
            if (bsh && src + 1 < ACC_WORDS)
                v |= a->w[src + 1] << (64u - bsh);
        }
        a->w[i] = v;
    }
    return sticky;
}

static void acc_add(Acc *a, const Acc *b) {
    unsigned __int128 carry = 0;
    for (unsigned i = 0; i < ACC_WORDS; i++) {
        unsigned __int128 t = (unsigned __int128)a->w[i] + b->w[i] + carry;
        a->w[i] = (uint64_t)t;
        carry = t >> 64;
    }
}

/* a -= b, a >= b. */
static void acc_sub(Acc *a, const Acc *b) {
    unsigned __int128 borrow = 0;
    for (unsigned i = 0; i < ACC_WORDS; i++) {
        unsigned __int128 t = (unsigned __int128)a->w[i] - b->w[i] - borrow;
        a->w[i] = (uint64_t)t;
        borrow = (t >> 64) ? 1u : 0u;
    }
}

static int acc_cmp(const Acc *a, const Acc *b) {
    for (int i = ACC_WORDS - 1; i >= 0; i--)
        if (a->w[i] != b->w[i])
            return a->w[i] > b->w[i] ? 1 : -1;
    return 0;
}

/* Index of the most significant set bit, or -1 for zero. */
static int acc_msb(const Acc *a) {
    for (int i = ACC_WORDS - 1; i >= 0; i--)
        if (a->w[i])
            return i * 64 + 63 - __builtin_clzll(a->w[i]);
    return -1;
}

static bool acc_bit(const Acc *a, int n) {
    if (n < 0 || n >= 64 * ACC_WORDS)
        return false;
    return ((a->w[n / 64] >> (n % 64)) & 1u) != 0;
}

/* True when a bit strictly below n is set. */
static bool acc_any_below(const Acc *a, int n) {
    if (n <= 0)
        return false;
    for (int i = 0; i < n && i < 64 * ACC_WORDS;) {
        int word = i / 64;
        int lo = i % 64;
        int hi = (n - word * 64 > 64) ? 64 : (n - word * 64);
        uint64_t mask = (hi >= 64) ? ~UINT64_C(0) : ((UINT64_C(1) << hi) - 1u);
        mask &= ~((UINT64_C(1) << lo) - 1u);
        if (a->w[word] & mask)
            return true;
        i = (word + 1) * 64;
    }
    return false;
}

static bool fr_is_nan(const MercedFpReg *f) {
    return f->exp == FP_EXP_MAX && (f->sig << 1) != 0;
}
static bool fr_is_inf(const MercedFpReg *f) {
    return f->exp == FP_EXP_MAX && f->sig == (UINT64_C(1) << 63);
}
static bool fr_is_zero(const MercedFpReg *f) {
    return f->exp == 0 && f->sig == 0;
}

static MercedFpReg fp_qnan(void) {
    MercedFpReg f = { UINT64_C(0xC000000000000000), FP_EXP_MAX, 0, 0 };
    return f;
}

static MercedFpReg fp_inf(bool sign) {
    MercedFpReg f = { UINT64_C(1) << 63, FP_EXP_MAX, (uint8_t)sign, 0 };
    return f;
}

/* Whether rounding increments the kept significand (vol. 1, table 5-5): lost
 * is "some bit was lost", half / above place the lost part against half an
 * ulp, odd is the kept least significant bit. */
static bool round_up(unsigned rc, bool sign, bool lost, bool half, bool above,
                     bool odd) {
    if (!lost)
        return false;
    switch (rc) {
    case 1:  return sign;               /* toward -inf */
    case 2:  return !sign;              /* toward +inf */
    case 3:  return false;              /* toward zero */
    default: return above || (half && odd);
    }
}

/* Round the 64-bit significand to bits of precision (24 for .s, 53 for .d). */
static bool round_precision(uint64_t *sig, int64_t *exp, unsigned bits,
                            unsigned rc, bool sign, bool *lost) {
    if (bits >= 64u || *sig == 0)
        return false;
    unsigned drop = 64u - bits;
    uint64_t rest = *sig & ((UINT64_C(1) << drop) - 1u);
    uint64_t half = UINT64_C(1) << (drop - 1u);
    uint64_t kept = *sig >> drop;
    if (rest)
        *lost = true;
    bool up = round_up(rc, sign, rest != 0, rest == half, rest > half,
                       (kept & 1u) != 0);
    if (up)
        kept++;
    if (kept >> bits) {
        kept >>= 1;
        (*exp)++;
    }
    *sig = kept << drop;
    return up;
}

/* The destination exponent range (vol. 1, table 5-6): an E-bit range spans
 * the register exponents 0xFFFF +/- (2^(E-1) - 1); 17 bits is the register
 * format itself.  Returns true when the result became infinity or zero. */
static bool apply_range(int64_t exp, bool sign, unsigned exp_bits,
                        MercedFpReg *out, unsigned *status) {
    int64_t emax = (int64_t)FP_EXP_MAX - 1, emin = 0;
    if (exp_bits < 17) {
        int64_t span = ((int64_t)1 << (exp_bits - 1)) - 1;
        emax = FP_EXP_BIAS + span;
        emin = FP_EXP_BIAS - span + 1;
    }
    /* Overflow and underflow do not imply inexact: with the trap enabled a
     * scaled result is delivered, exact whenever the significand fitted. */
    if (exp > emax) {
        *status |= MERCED_FP_O;
        *out = fp_inf(sign);
        return true;
    }
    if (exp < emin) {
        *status |= MERCED_FP_U;
        MercedFpReg z = { 0, 0, (uint8_t)sign, 0 };
        *out = z;
        return true;
    }
    return false;
}

/* 256-bit unsigned, hi:lo. */
typedef struct { unsigned __int128 hi, lo; } U256;

static U256 u256_shl128(unsigned __int128 x, unsigned k) {     /* k < 256 */
    U256 r;
    if (k == 0)       { r.lo = x; r.hi = 0; }
    else if (k < 128) { r.lo = x << k; r.hi = x >> (128 - k); }
    else              { r.lo = 0; r.hi = x << (k - 128); }
    return r;
}

static int u128_msb(unsigned __int128 x) {                      /* x != 0 */
    uint64_t h = (uint64_t)(x >> 64);
    return h ? 127 - __builtin_clzll(h) : 63 - __builtin_clzll((uint64_t)x);
}

/* (+/-)a*b (+/-)d exactly, when d's scale is within [-127, +191] of the
 * product's: the significand (64 bits at the top), round and sticky bits and
 * register exponent.  False on exact cancellation. */
static bool sum_exact256(const MercedFpReg *a, const MercedFpReg *b,
                         const MercedFpReg *d, bool dzero, bool psign,
                         bool dsign, int64_t ep, int64_t ea, bool *rsign,
                         uint64_t *sig, bool *round_bit, bool *sticky,
                         int64_t *exp) {
    unsigned __int128 prod = (unsigned __int128)a->sig * b->sig;
    int64_t e0 = (dzero || ep < ea) ? ep : ea;          /* scale of bit 0 */
    U256 p = u256_shl128(prod, (unsigned)(ep - e0));
    U256 s;
    if (dzero) {
        s = p;
        *rsign = psign;
    } else {
        U256 q = u256_shl128(d->sig, (unsigned)(ea - e0));
        if (psign == dsign) {
            s.lo = p.lo + q.lo;
            s.hi = p.hi + q.hi + (s.lo < p.lo);
            *rsign = psign;
        } else {
            bool p_ge = p.hi != q.hi ? p.hi > q.hi : p.lo >= q.lo;
            U256 x = p_ge ? p : q, y = p_ge ? q : p;
            s.lo = x.lo - y.lo;
            s.hi = x.hi - y.hi - (x.lo < y.lo);
            *rsign = p_ge ? psign : dsign;
        }
    }
    if (s.hi == 0 && s.lo == 0)
        return false;
    int m = s.hi ? 128 + u128_msb(s.hi) : u128_msb(s.lo);
    int drop = m - 63;
    if (drop > 0) {
        /* Bits [drop, drop+63] are the significand; drop-1 rounds. */
        unsigned __int128 sh;
        if (drop < 128) sh = (s.lo >> drop) | (s.hi << (128 - drop));
        else            sh = s.hi >> (drop - 128);
        *sig = (uint64_t)sh;
        int rb = drop - 1;
        *round_bit = rb < 128 ? (bool)((s.lo >> rb) & 1u)
                              : (bool)((s.hi >> (rb - 128)) & 1u);
        if (rb == 0)
            *sticky = false;
        else if (rb <= 128)
            *sticky = (s.lo & (((unsigned __int128)1 << (rb - 1) << 1) - 1u)) != 0;
        else
            *sticky = s.lo != 0 ||
                      (s.hi & (((unsigned __int128)1 << (rb - 128)) - 1u)) != 0;
    } else {
        *sig = (uint64_t)s.lo << (-drop);
        *round_bit = false;
        *sticky = false;
    }
    *exp = (int64_t)FP_EXP_BIAS + e0 + m;
    return true;
}

MercedFpReg merced_fp_fma(MercedFpReg a, MercedFpReg b, MercedFpReg d,
                          bool neg_prod, bool neg_add, unsigned prec,
                          unsigned exp_bits, unsigned rc, unsigned *status) {
    *status = 0;
    if (fr_is_nan(&a) || fr_is_nan(&b) || fr_is_nan(&d))
        return fp_qnan();

    bool psign = (a.sign ^ b.sign) ^ neg_prod;
    bool dsign = d.sign ^ neg_add;

    /* inf * 0 is invalid; so is inf - inf. */
    if ((fr_is_inf(&a) && fr_is_zero(&b)) || (fr_is_zero(&a) && fr_is_inf(&b)))
        return fp_qnan();
    if (fr_is_inf(&a) || fr_is_inf(&b)) {
        if (fr_is_inf(&d) && dsign != psign)
            return fp_qnan();
        return fp_inf(psign);
    }
    if (fr_is_inf(&d))
        return fp_inf(dsign);

    bool pzero = a.sig == 0 || b.sig == 0;
    bool dzero = d.sig == 0;
    if (pzero && dzero) {
        /* Both zero: negative only when both addends are. */
        MercedFpReg z = { 0, 0, (uint8_t)(psign && dsign), 0 };
        return z;
    }
    if (pzero) {
        /* The result is the addend, still rounded to the destination
         * precision and still subject to its exponent range. */
        MercedFpReg r = d;
        r.sign = (uint8_t)dsign;
        int64_t e = (int64_t)r.exp;
        bool lost = false;
        if (round_precision(&r.sig, &e, prec, rc, r.sign, &lost))
            *status |= MERCED_FP_FPA;
        if (lost)
            *status |= MERCED_FP_I;
        MercedFpReg out;
        if (apply_range(e, r.sign, exp_bits, &out, status))
            return out;
        r.exp = (uint32_t)e;
        return r;
    }

    /* Scale of each operand's significand LSB, as a power of two. */
    int64_t ep = ((int64_t)a.exp - FP_EXP_BIAS - 63) +
                 ((int64_t)b.exp - FP_EXP_BIAS - 63);
    int64_t ea = (int64_t)d.exp - FP_EXP_BIAS - 63;

    bool rsign = false, round_bit = false, sticky_bits = false;
    uint64_t sig = 0;
    int64_t exp = 0;
    if (dzero || (ea - ep <= 191 && ep - ea <= 127)) {
        if (!sum_exact256(&a, &b, &d, dzero, psign, dsign, ep, ea, &rsign,
                          &sig, &round_bit, &sticky_bits, &exp)) {
            MercedFpReg z = { 0, 0, 0, 0 };     /* exact cancellation: +0 */
            return z;
        }
    } else {
        /* The base scale lies 128 bits below the higher operand, so
         * whichever operand is the larger fits with its low bits intact. */
        int64_t base = (dzero ? ep : (ep > ea ? ep : ea)) - 128;
        Acc pa = { { 0, 0, 0, 0, 0, 0 } };
        bool sticky = false;
        {
            unsigned __int128 prod = (unsigned __int128)a.sig * b.sig;
            pa.w[0] = (uint64_t)prod;
            pa.w[1] = (uint64_t)(prod >> 64);
            int64_t sh = ep - base;
            if (sh >= 0) acc_shl(&pa, (unsigned)sh);
            else         sticky |= acc_shr_sticky(&pa, (unsigned)(-sh));
        }
        Acc da = { { 0, 0, 0, 0, 0, 0 } };
        if (!dzero) {
            da.w[0] = d.sig;
            int64_t sh = ea - base;
            if (sh >= 0) acc_shl(&da, (unsigned)sh);
            else         sticky |= acc_shr_sticky(&da, (unsigned)(-sh));
        }
        Acc acc;
        if (dzero) {
            acc = pa;
            rsign = psign;
        } else if (psign == dsign) {
            acc = pa;
            acc_add(&acc, &da);
            rsign = psign;
        } else {
            int cmp = acc_cmp(&pa, &da);
            if (cmp >= 0) { acc = pa; acc_sub(&acc, &da); rsign = psign; }
            else          { acc = da; acc_sub(&acc, &pa); rsign = dsign; }
            /* Bits shifted out of the smaller operand were dropped, so the
             * true difference is one unit of the buffer smaller than what
             * was computed: borrow it and leave the remainder sticky. */
            if (sticky) {
                Acc one = { { 1, 0, 0, 0, 0, 0 } };
                if (acc_cmp(&acc, &one) >= 0)
                    acc_sub(&acc, &one);
            }
        }
        int m = acc_msb(&acc);
        if (m < 0) {
            MercedFpReg z = { 0, 0, 0, 0 };     /* exact cancellation: +0 */
            return z;
        }
        /* The 64 significand bits ending at the msb, with round and sticky. */
        int drop = m - 63;
        sticky_bits = sticky;
        if (drop > 0) {
            Acc t = acc;
            sticky_bits |= acc_any_below(&acc, drop - 1);
            round_bit = acc_bit(&acc, drop - 1);
            (void)acc_shr_sticky(&t, (unsigned)drop);
            sig = t.w[0];
        } else {
            Acc t = acc;
            if (drop < 0)
                acc_shl(&t, (unsigned)(-drop));
            sig = t.w[0];
            round_bit = false;
        }
        exp = (int64_t)FP_EXP_BIAS + base + m;
    }

    bool rounded_up = false;
    bool inexact = round_bit || sticky_bits;
    if (round_up(rc, rsign, round_bit || sticky_bits, round_bit,
                 round_bit && sticky_bits, (sig & 1u) != 0)) {
        sig++;
        rounded_up = true;
        if (sig == 0) {                         /* carried out */
            sig = UINT64_C(1) << 63;
            exp++;
        }
    }
    bool lost = false;
    if (round_precision(&sig, &exp, prec, rc, rsign, &lost))
        rounded_up = true;
    if (lost)
        inexact = true;

    *status = 0;
    if (inexact)    *status |= MERCED_FP_I;
    if (rounded_up) *status |= MERCED_FP_FPA;

    MercedFpReg out;
    if (apply_range(exp, rsign, exp_bits, &out, status))
        return out;
    MercedFpReg r = { sig, (uint32_t)exp, (uint8_t)rsign, 0 };
    return r;
}

/* The seed tables of the reciprocal and reciprocal square root
 * approximations, as the architecture's pseudo-code defines them (vol. 3,
 * frcpa and frsqrta).  Firmware self-tests compare the results against fixed
 * values, so the seeds must be these, bit for bit. */
static const uint16_t recip_table[256] = {
    0x3fc, 0x3f4, 0x3ec, 0x3e4, 0x3dd, 0x3d5, 0x3cd, 0x3c6,
    0x3be, 0x3b7, 0x3af, 0x3a8, 0x3a1, 0x399, 0x392, 0x38b,
    0x384, 0x37d, 0x376, 0x36f, 0x368, 0x361, 0x35b, 0x354,
    0x34d, 0x346, 0x340, 0x339, 0x333, 0x32c, 0x326, 0x320,
    0x319, 0x313, 0x30d, 0x307, 0x300, 0x2fa, 0x2f4, 0x2ee,
    0x2e8, 0x2e2, 0x2dc, 0x2d7, 0x2d1, 0x2cb, 0x2c5, 0x2bf,
    0x2ba, 0x2b4, 0x2af, 0x2a9, 0x2a3, 0x29e, 0x299, 0x293,
    0x28e, 0x288, 0x283, 0x27e, 0x279, 0x273, 0x26e, 0x269,
    0x264, 0x25f, 0x25a, 0x255, 0x250, 0x24b, 0x246, 0x241,
    0x23c, 0x237, 0x232, 0x22e, 0x229, 0x224, 0x21f, 0x21b,
    0x216, 0x211, 0x20d, 0x208, 0x204, 0x1ff, 0x1fb, 0x1f6,
    0x1f2, 0x1ed, 0x1e9, 0x1e5, 0x1e0, 0x1dc, 0x1d8, 0x1d4,
    0x1cf, 0x1cb, 0x1c7, 0x1c3, 0x1bf, 0x1bb, 0x1b6, 0x1b2,
    0x1ae, 0x1aa, 0x1a6, 0x1a2, 0x19e, 0x19a, 0x197, 0x193,
    0x18f, 0x18b, 0x187, 0x183, 0x17f, 0x17c, 0x178, 0x174,
    0x171, 0x16d, 0x169, 0x166, 0x162, 0x15e, 0x15b, 0x157,
    0x154, 0x150, 0x14d, 0x149, 0x146, 0x142, 0x13f, 0x13b,
    0x138, 0x134, 0x131, 0x12e, 0x12a, 0x127, 0x124, 0x120,
    0x11d, 0x11a, 0x117, 0x113, 0x110, 0x10d, 0x10a, 0x107,
    0x103, 0x100, 0x0fd, 0x0fa, 0x0f7, 0x0f4, 0x0f1, 0x0ee,
    0x0eb, 0x0e8, 0x0e5, 0x0e2, 0x0df, 0x0dc, 0x0d9, 0x0d6,
    0x0d3, 0x0d0, 0x0cd, 0x0ca, 0x0c8, 0x0c5, 0x0c2, 0x0bf,
    0x0bc, 0x0b9, 0x0b7, 0x0b4, 0x0b1, 0x0ae, 0x0ac, 0x0a9,
    0x0a6, 0x0a4, 0x0a1, 0x09e, 0x09c, 0x099, 0x096, 0x094,
    0x091, 0x08e, 0x08c, 0x089, 0x087, 0x084, 0x082, 0x07f,
    0x07c, 0x07a, 0x077, 0x075, 0x073, 0x070, 0x06e, 0x06b,
    0x069, 0x066, 0x064, 0x061, 0x05f, 0x05d, 0x05a, 0x058,
    0x056, 0x053, 0x051, 0x04f, 0x04c, 0x04a, 0x048, 0x045,
    0x043, 0x041, 0x03f, 0x03c, 0x03a, 0x038, 0x036, 0x033,
    0x031, 0x02f, 0x02d, 0x02b, 0x029, 0x026, 0x024, 0x022,
    0x020, 0x01e, 0x01c, 0x01a, 0x018, 0x015, 0x013, 0x011,
    0x00f, 0x00d, 0x00b, 0x009, 0x007, 0x005, 0x003, 0x001,
};

static const uint16_t recip_sqrt_table[256] = {
    0x1a5, 0x1a0, 0x19a, 0x195, 0x18f, 0x18a, 0x185, 0x180,
    0x17a, 0x175, 0x170, 0x16b, 0x166, 0x161, 0x15d, 0x158,
    0x153, 0x14e, 0x14a, 0x145, 0x140, 0x13c, 0x138, 0x133,
    0x12f, 0x12a, 0x126, 0x122, 0x11e, 0x11a, 0x115, 0x111,
    0x10d, 0x109, 0x105, 0x101, 0x0fd, 0x0fa, 0x0f6, 0x0f2,
    0x0ee, 0x0ea, 0x0e7, 0x0e3, 0x0df, 0x0dc, 0x0d8, 0x0d5,
    0x0d1, 0x0ce, 0x0ca, 0x0c7, 0x0c3, 0x0c0, 0x0bd, 0x0b9,
    0x0b6, 0x0b3, 0x0b0, 0x0ad, 0x0a9, 0x0a6, 0x0a3, 0x0a0,
    0x09d, 0x09a, 0x097, 0x094, 0x091, 0x08e, 0x08b, 0x088,
    0x085, 0x082, 0x07f, 0x07d, 0x07a, 0x077, 0x074, 0x071,
    0x06f, 0x06c, 0x069, 0x067, 0x064, 0x061, 0x05f, 0x05c,
    0x05a, 0x057, 0x054, 0x052, 0x04f, 0x04d, 0x04a, 0x048,
    0x045, 0x043, 0x041, 0x03e, 0x03c, 0x03a, 0x037, 0x035,
    0x033, 0x030, 0x02e, 0x02c, 0x029, 0x027, 0x025, 0x023,
    0x020, 0x01e, 0x01c, 0x01a, 0x018, 0x016, 0x014, 0x011,
    0x00f, 0x00d, 0x00b, 0x009, 0x007, 0x005, 0x003, 0x001,
    0x3fc, 0x3f4, 0x3ec, 0x3e5, 0x3dd, 0x3d5, 0x3ce, 0x3c7,
    0x3bf, 0x3b8, 0x3b1, 0x3aa, 0x3a3, 0x39c, 0x395, 0x38e,
    0x388, 0x381, 0x37a, 0x374, 0x36d, 0x367, 0x361, 0x35a,
    0x354, 0x34e, 0x348, 0x342, 0x33c, 0x336, 0x330, 0x32b,
    0x325, 0x31f, 0x31a, 0x314, 0x30f, 0x309, 0x304, 0x2fe,
    0x2f9, 0x2f4, 0x2ee, 0x2e9, 0x2e4, 0x2df, 0x2da, 0x2d5,
    0x2d0, 0x2cb, 0x2c6, 0x2c1, 0x2bd, 0x2b8, 0x2b3, 0x2ae,
    0x2aa, 0x2a5, 0x2a1, 0x29c, 0x298, 0x293, 0x28f, 0x28a,
    0x286, 0x282, 0x27d, 0x279, 0x275, 0x271, 0x26d, 0x268,
    0x264, 0x260, 0x25c, 0x258, 0x254, 0x250, 0x24c, 0x249,
    0x245, 0x241, 0x23d, 0x239, 0x235, 0x232, 0x22e, 0x22a,
    0x227, 0x223, 0x220, 0x21c, 0x218, 0x215, 0x211, 0x20e,
    0x20a, 0x207, 0x204, 0x200, 0x1fd, 0x1f9, 0x1f6, 0x1f3,
    0x1f0, 0x1ec, 0x1e9, 0x1e6, 0x1e3, 0x1df, 0x1dc, 0x1d9,
    0x1d6, 0x1d3, 0x1d0, 0x1cd, 0x1ca, 0x1c7, 0x1c4, 0x1c1,
    0x1be, 0x1bb, 0x1b8, 0x1b5, 0x1b2, 0x1af, 0x1ac, 0x1aa,
};

static const MercedFpReg fp_qnan_indef = {
    UINT64_C(0xC000000000000000), FP_EXP_MAX, 1, 0
};

/* Shift the significand up so its top bit is bit 63, adjusting the exponent.
 * False for a true zero. */
static bool fp_normalize(MercedFpReg *f) {
    if (f->sig == 0)
        return false;
    while (!(f->sig >> 63)) {
        f->sig <<= 1;
        if (f->exp)
            f->exp--;
    }
    return true;
}

bool merced_fp_frcpa(MercedFpReg num, MercedFpReg den, MercedFpReg *out) {
    if (fr_is_nan(&num)) {
        *out = num;
        out->sig |= UINT64_C(1) << 62;
        return false;
    }
    if (fr_is_nan(&den)) {
        *out = den;
        out->sig |= UINT64_C(1) << 62;
        return false;
    }

    bool sign = num.sign ^ den.sign;
    bool nz = fr_is_zero(&num), dz = fr_is_zero(&den);
    bool ni = fr_is_inf(&num), di = fr_is_inf(&den);

    if ((nz && dz) || (ni && di)) { *out = fp_qnan_indef; return false; }
    if (ni) { *out = fp_inf(sign); return false; }        /* inf / finite */
    if (di || nz) {                                       /* x / inf, 0 / x */
        MercedFpReg z = { 0, 0, (uint8_t)sign, 0 };
        *out = z;
        return false;
    }
    if (dz) { *out = fp_inf(sign); return false; }        /* finite / 0 */

    /* fp_ieee_recip(den), vol. 3, frcpa. */
    MercedFpReg d = den;
    (void)fp_normalize(&d);
    unsigned idx = (unsigned)((d.sig >> 55) & 0xFFu);
    int64_t exp = (int64_t)FP_EXP_MAX - 2 - (int64_t)d.exp;
    MercedFpReg r = { (UINT64_C(1) << 63) | ((uint64_t)recip_table[idx] << 53),
                      (uint32_t)exp, d.sign, 0 };
    *out = r;
    return true;
}

bool merced_fp_frsqrta(MercedFpReg a, MercedFpReg *out) {
    if (fr_is_nan(&a)) {
        *out = a;
        out->sig |= UINT64_C(1) << 62;
        return false;
    }
    if (fr_is_zero(&a))              { *out = a; return false; }   /* +-0 */
    if (fr_is_inf(&a) && !a.sign)    { *out = a; return false; }   /* +inf */
    if (a.sign)                      { *out = fp_qnan_indef; return false; }

    /* fp_ieee_recip_sqrt(root), vol. 3, frsqrta. */
    MercedFpReg v = a;
    (void)fp_normalize(&v);
    unsigned idx = (unsigned)(((v.exp & 1u) << 7) | ((v.sig >> 56) & 0x7Fu));
    int64_t exp = (int64_t)FP_EXP_BIAS - 1 -
                  (((int64_t)v.exp - (int64_t)FP_EXP_BIAS) >> 1);
    MercedFpReg r = { (UINT64_C(1) << 63) | ((uint64_t)recip_sqrt_table[idx] << 53),
                      (uint32_t)exp, 0, 0 };
    *out = r;
    return true;
}

/* Register format to single-precision memory format, rounding to nearest
 * even on what is dropped. */
uint32_t merced_fp_to_single_bits(MercedFpReg fr) {
    const MercedFpReg *f = &fr;
    const uint32_t sgn = (uint32_t)f->sign << 31;

    if (f->exp == FP_EXP_MAX) {
        if ((f->sig << 1) == 0) { return sgn | 0x7F800000u; }  /* Inf */
        /* NaN: keep the quiet bit and the top of the payload. */
        return sgn | 0x7F800000u
             | (uint32_t)((f->sig >> 40) & 0x7FFFFFu) | 0x400000u;
    }
    if (f->sig == 0) { return sgn; }                            /* +-0 */

    MercedFpReg v = *f;
    (void)fp_normalize(&v);
    int64_t be = (int64_t)v.exp - FP_EXP_BIAS + 127;   /* single bias */

    if (be >= 255) { return sgn | 0x7F7FFFFFu; }            /* saturate */

    uint64_t sig = v.sig;                 /* 1.xxx, msb = integer bit */
    if (be <= 0) {
        /* Subnormal: shift the significand down by the deficit. */
        const int64_t sh = 1 - be;
        if (sh >= 64) { return sgn; }
        sig >>= sh;
        be = 0;
        if (sig == 0) { return sgn; }
    }
    /* Mantissa = bits 62:40, rounded to nearest even on what is dropped. */
    uint32_t mant = (uint32_t)((sig >> 40) & 0x7FFFFFu);
    const uint64_t lost = sig & ((UINT64_C(1) << 40) - 1u);
    const uint64_t half = UINT64_C(1) << 39;
    if (lost > half || (lost == half && (mant & 1u))) {
        mant++;
        if (mant >> 23) { mant = 0; be++; if (be >= 255) { return sgn | 0x7F7FFFFFu; } }
    }
    return sgn | ((uint32_t)be << 23) | mant;
}


/* A register to a 64-bit integer, exactly.  is_unsigned selects the unsigned
 * range; a NaN, an infinity or an out-of-range value gives the integer
 * indefinite, 0x8000000000000000, with *invalid set (vol. 1, 5.4.4). */
uint64_t merced_fp_to_int(MercedFpReg fr, bool use_trunc, bool is_unsigned,
                          bool *invalid) {
    const MercedFpReg *f = &fr;
    const uint64_t indef = UINT64_C(0x8000000000000000);
    bool inv = false;

    if (f->exp == FP_EXP_MAX) {
        inv = true; goto out;   /* NaN or infinity */
    }
    if (f->sig == 0) { if (invalid) { *invalid = false; } return 0; }
    /* fcvt.fxu has no negative range at all. */
    if (is_unsigned && f->sign) { inv = true; goto out; }

    MercedFpReg v = *f;
    (void)fp_normalize(&v);
    const int64_t e = (int64_t)v.exp - FP_EXP_BIAS - 63;

    uint64_t mag;
    if (e >= 0) {
        if (e >= 64) { inv = true; goto out; }             /* out of range */
        /* Would shifting drop a significant bit? Then it does not fit. */
        if (v.sig > (~UINT64_C(0) >> e)) { inv = true; goto out; }
        mag = v.sig << e;
    } else {
        const unsigned sh = (unsigned)(-e);
        if (sh >= 64) {
            /* Magnitude below 1: truncation gives 0; round-to-nearest can
             * still give 1 when the value is exactly at or above a half. */
            if (invalid) { *invalid = false; }
            if (use_trunc || sh > 64) { return 0; }
            mag = (v.sig >> 63) ? 1u : 0u;
            return f->sign ? (uint64_t)(-(int64_t)mag) : mag;
        }
        mag = v.sig >> sh;
        if (!use_trunc) {
            /* Round to nearest even on the discarded bits. */
            const uint64_t rest = v.sig & ((UINT64_C(1) << sh) - 1u);
            const uint64_t half = UINT64_C(1) << (sh - 1u);
            if (rest > half || (rest == half && (mag & 1u))) { mag++; }
        }
    }

    if (is_unsigned) {                      /* full 64-bit range */
        if (invalid) { *invalid = false; }
        return mag;
    }
    if (f->sign) {
        if (mag > (uint64_t)INT64_MAX + 1u) { inv = true; goto out; }
        if (invalid) { *invalid = false; }
        return (uint64_t)(-(int64_t)mag);
    }
    if (mag > (uint64_t)INT64_MAX) { inv = true; goto out; }
    if (invalid) { *invalid = false; }
    return mag;

out:
    /* SDM vol. 1, 5.4.4: with the trap disabled the result is the
     * largest-magnitude negative integer, even for unsigned. */
    if (invalid) { *invalid = inv; }
    return indef;
}


/* A signed integer to the register format, exactly. */
MercedFpReg merced_fp_from_int(int64_t v) {
    MercedFpReg r = { 0, 0, 0, 0 };
    if (v == 0) { return r; }

    r.sign = (uint8_t)(v < 0);
    /* INT64_MIN has no positive counterpart, so negate in unsigned. */
    uint64_t mag = r.sign ? (~(uint64_t)v + 1u) : (uint64_t)v;

    const int m = 63 - __builtin_clzll(mag);              /* mag != 0 */
    r.sig = mag << (63 - m);
    r.exp = (uint32_t)(FP_EXP_BIAS + m);
    return r;
}

/* Exact comparison of two register values: -1 less, 0 equal, 1 greater,
 * 2 unordered.  A zero equals a zero of either sign. */
int merced_fp_compare(MercedFpReg a, MercedFpReg b) {
    if (fr_is_nan(&a) || fr_is_nan(&b))
        return 2;
    bool az = a.sig == 0, bz = b.sig == 0;
    if (az && bz)
        return 0;
    /* Magnitude order, then the sign: -1 when a is below b. */
    int mag;
    if (az)       mag = -1;
    else if (bz)  mag = 1;
    else {
        MercedFpReg x = a, y = b;
        (void)fp_normalize(&x);
        (void)fp_normalize(&y);
        if (x.exp != y.exp)        mag = x.exp < y.exp ? -1 : 1;
        else if (x.sig != y.sig)   mag = x.sig < y.sig ? -1 : 1;
        else                       mag = 0;
    }
    if (az)       return b.sign ? 1 : -1;
    if (bz)       return a.sign ? -1 : 1;
    if (a.sign != b.sign)
        return a.sign ? -1 : 1;
    if (mag == 0)
        return 0;
    return a.sign ? -mag : mag;
}

/* Single and double memory formats to and from the register format (vol. 1,
 * 5.1.3): the exponent is re-biased and the significand's integer bit
 * is implicit in memory. */
MercedFpReg merced_fp_from_single_mem(uint32_t bits)
{
    const bool     sgn  = (bits >> 31) & 1u;
    const uint32_t mexp = (bits >> 23) & 0xFFu;
    const uint64_t mant = (uint64_t)(bits & 0x7FFFFFu) << 40;

    if (mexp == 0xFFu) {                       /* infinities and NaNs */
        return (MercedFpReg){ (UINT64_C(1) << 63) | mant, 0x1FFFFu, sgn, 0 };
    }
    if (mexp == 0u) {                          /* zeros and denormals */
        if (mant == 0) { return (MercedFpReg){ 0, 0u, sgn, 0 }; }
        return (MercedFpReg){ mant, 0x0FF81u, sgn, 0 };
    }
    return (MercedFpReg){ (UINT64_C(1) << 63) | mant, mexp + 0xFF80u, sgn, 0 };
}

MercedFpReg merced_fp_from_double_mem(uint64_t bits)
{
    const bool     sgn  = (bits >> 63) & 1u;
    const uint32_t mexp = (uint32_t)((bits >> 52) & 0x7FFu);
    const uint64_t mant = (bits & UINT64_C(0x000FFFFFFFFFFFFF)) << 11;

    if (mexp == 0x7FFu) {
        return (MercedFpReg){ (UINT64_C(1) << 63) | mant, 0x1FFFFu, sgn, 0 };
    }
    if (mexp == 0u) {
        if (mant == 0) { return (MercedFpReg){ 0, 0u, sgn, 0 }; }
        return (MercedFpReg){ mant, 0x0FC01u, sgn, 0 };
    }
    return (MercedFpReg){ (UINT64_C(1) << 63) | mant, mexp + 0xFC00u, sgn, 0 };
}

/* Register format -> single/double MEMORY format for stfs/stfd and */
uint32_t merced_fp_to_single_mem(MercedFpReg fr) {
    const MercedFpReg *f = &fr;
    const uint32_t ibit = (uint32_t)((f->sig >> 63) & 1u);
    /* Rebiased when the result FITS -- FR bias 0xFFFF minus single bias 0x7F */
    uint32_t exp = 0u;
    if (ibit) {
        const uint32_t reb = f->exp - 0xFF80u;
        exp = (reb <= 0xFFu) ? reb : (f->exp & 0xFFu);
    }
    return ((uint32_t)f->sign << 31) | (exp << 23)
         | (uint32_t)((f->sig >> 40) & 0x7FFFFFu);
}

uint64_t merced_fp_to_double_mem(MercedFpReg fr) {
    const MercedFpReg *f = &fr;
    /* The double field is rebiased before truncation -- FR bias 0xFFFF minus */
    const uint64_t ibit = (f->sig >> 63) & 1u;
    const uint64_t exp  = ibit
        ? (uint64_t)(((uint64_t)f->exp - UINT64_C(0xFC00)) & 0x7FFu) : 0u;
    return ((uint64_t)f->sign << 63) | (exp << 52)
         | ((f->sig >> 11) & UINT64_C(0x000FFFFFFFFFFFFF));
}

