/*
 * 128-bit unsigned arithmetic for exact time conversions: GCC's
 * unsigned __int128 where the target has it (64-bit hosts), and the same
 * operations on two 64-bit halves where it doesn't (32-bit Windows, the
 * i686 build).  Only what the core needs: products of two 64-bit numbers,
 * a 128-bit value times a 64-bit one (the low 128 bits), sums, shifts,
 * comparisons and the quotient of a 128-bit value by a 64-bit one when it
 * fits in 64 bits.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#if defined(__SIZEOF_INT128__)

typedef unsigned __int128 GemuU128;
static inline GemuU128 gemu_u128(uint64_t v) { return v; }
static inline GemuU128 gemu_u128_mul64(uint64_t a, uint64_t b) { return (GemuU128)a * b; }
static inline GemuU128 gemu_u128_mul(GemuU128 x, uint64_t b) { return x * b; }
static inline GemuU128 gemu_u128_add(GemuU128 x, GemuU128 y) { return x + y; }
static inline GemuU128 gemu_u128_sub(GemuU128 x, GemuU128 y) { return x - y; }
static inline GemuU128 gemu_u128_shl(GemuU128 x, unsigned n) { return x << n; }
static inline bool     gemu_u128_lt(GemuU128 x, GemuU128 y) { return x < y; }
/* x / d (d > 0, the quotient below 2^64); *rem = x % d unless NULL */
static inline uint64_t gemu_u128_div64(GemuU128 x, uint64_t d, uint64_t *rem) {
    if (rem) *rem = (uint64_t)(x % d);
    return (uint64_t)(x / d);
}

#else

typedef struct { uint64_t hi, lo; } GemuU128;
static inline GemuU128 gemu_u128(uint64_t v) { GemuU128 r = { 0, v }; return r; }
static inline GemuU128 gemu_u128_mul64(uint64_t a, uint64_t b) {
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    GemuU128 r = { p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (uint32_t)p00 };
    return r;
}
static inline GemuU128 gemu_u128_mul(GemuU128 x, uint64_t b) {
    GemuU128 r = gemu_u128_mul64(x.lo, b);
    r.hi += x.hi * b;
    return r;
}
static inline GemuU128 gemu_u128_add(GemuU128 x, GemuU128 y) {
    GemuU128 r = { x.hi + y.hi, x.lo + y.lo };
    r.hi += r.lo < x.lo;
    return r;
}
static inline GemuU128 gemu_u128_sub(GemuU128 x, GemuU128 y) {
    GemuU128 r = { x.hi - y.hi - (x.lo < y.lo), x.lo - y.lo };
    return r;
}
static inline GemuU128 gemu_u128_shl(GemuU128 x, unsigned n) {
    GemuU128 r;
    if (n == 0) return x;
    if (n >= 64) { r.hi = x.lo << (n - 64); r.lo = 0; return r; }
    r.hi = (x.hi << n) | (x.lo >> (64 - n));
    r.lo = x.lo << n;
    return r;
}
static inline bool gemu_u128_lt(GemuU128 x, GemuU128 y) {
    return x.hi < y.hi || (x.hi == y.hi && x.lo < y.lo);
}
static inline uint64_t gemu_u128_div64(GemuU128 x, uint64_t d, uint64_t *rem) {
    uint64_t q = 0, r = x.hi % d;          /* the quotient fits: x.hi / d is 0 */
    for (int i = 63; i >= 0; i--) {        /* restoring division, a bit at a time */
        bool carry = r >> 63;
        r = (r << 1) | ((x.lo >> i) & 1);
        q <<= 1;
        if (carry || r >= d) {
            r -= d;
            q |= 1;
        }
    }
    if (rem) *rem = r;
    return q;
}

#endif

/* x / d, or UINT64_MAX when the quotient doesn't fit; *rem as above */
static inline uint64_t gemu_u128_div64_sat(GemuU128 x, uint64_t d, uint64_t *rem) {
    if (!gemu_u128_lt(x, gemu_u128_shl(gemu_u128(d), 64))) {   /* x >= d * 2^64 */
        if (rem) *rem = 0;
        return UINT64_MAX;
    }
    return gemu_u128_div64(x, d, rem);
}

/* a * b / c and its ceiling, exactly; UINT64_MAX when it doesn't fit */
static inline uint64_t gemu_muldiv_sat(uint64_t a, uint64_t b, uint64_t c) {
    return gemu_u128_div64_sat(gemu_u128_mul64(a, b), c, 0);
}
static inline uint64_t gemu_muldiv_ceil_sat(uint64_t a, uint64_t b, uint64_t c) {
    uint64_t rem, q = gemu_u128_div64_sat(gemu_u128_mul64(a, b), c, &rem);
    return q == UINT64_MAX ? q : q + (rem != 0);
}

/* a * b / c and its ceiling, exactly (the result below 2^64) */
static inline uint64_t gemu_muldiv(uint64_t a, uint64_t b, uint64_t c) {
    return gemu_u128_div64(gemu_u128_mul64(a, b), c, 0);
}
static inline uint64_t gemu_muldiv_ceil(uint64_t a, uint64_t b, uint64_t c) {
    uint64_t rem, q = gemu_u128_div64(gemu_u128_mul64(a, b), c, &rem);
    return q + (rem != 0);
}
