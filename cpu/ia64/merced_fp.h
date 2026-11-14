#pragma once

/*
 * Exact floating-point arithmetic for the Itanium 82-bit register format: a
 * 64-bit significand with an explicit integer bit, a 17-bit biased exponent
 * (bias 0xFFFF) and a sign, value = sig * 2^(exp - 0xFFFF - 63).  Results are
 * computed without loss and rounded once, as the hardware does.
 */

#include <stdbool.h>
#include <stdint.h>

#include "merced.h"

/* IEEE status of an operation (vol. 1, table 5-4). */
enum {
    MERCED_FP_I   = 1u << 0,    /* inexact */
    MERCED_FP_U   = 1u << 1,    /* tiny: underflow */
    MERCED_FP_O   = 1u << 2,    /* overflow */
    MERCED_FP_FPA = 1u << 3,    /* rounding added one to the significand */
};

/* (neg_prod ? -(a*b) : a*b) + (neg_add ? -d : d), rounded to prec significand
 * bits (64, 53 or 24) and an exp_bits exponent range (17, 15, 11 or 8) under
 * rounding control rc (0 nearest-even, 1 -inf, 2 +inf, 3 zero).  *status
 * receives the MERCED_FP_* conditions raised. */
MercedFpReg merced_fp_fma(MercedFpReg a, MercedFpReg b, MercedFpReg d,
                          bool neg_prod, bool neg_add, unsigned prec,
                          unsigned exp_bits, unsigned rc, unsigned *status);

/* frcpa: the approximation of 1/den, or the special-case result for NaN,
 * zero and infinite operands.  Returns the value for the second predicate
 * (true: the approximation is valid and needs refining). */
bool merced_fp_frcpa(MercedFpReg num, MercedFpReg den, MercedFpReg *out);

/* frsqrta: the approximation of 1/sqrt(a); same convention. */
bool merced_fp_frsqrta(MercedFpReg a, MercedFpReg *out);

/* Register format to single-precision memory format. */
uint32_t merced_fp_to_single_bits(MercedFpReg f);

/* fcvt.fx / fcvt.fxu: the register as a 64-bit integer, exactly. */
uint64_t merced_fp_to_int(MercedFpReg f, bool use_trunc, bool is_unsigned,
                          bool *invalid);

/* fcvt.xf: a signed integer as a register value, exactly. */
MercedFpReg merced_fp_from_int(int64_t v);

/* Exact comparison: -1 less, 0 equal, 1 greater, 2 unordered. */
int merced_fp_compare(MercedFpReg a, MercedFpReg b);

/* Memory formats. */
MercedFpReg merced_fp_from_single_mem(uint32_t bits);
MercedFpReg merced_fp_from_double_mem(uint64_t bits);
uint32_t merced_fp_to_single_mem(MercedFpReg f);
uint64_t merced_fp_to_double_mem(MercedFpReg f);
