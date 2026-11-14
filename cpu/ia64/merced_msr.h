#pragma once

/*
 * Itanium (Merced) model-specific registers, as PAL drives them.  Enabled per
 * processor by merced_set_msr_model().  See merced_msr.c for what each
 * modelled register does and the PAL code it was derived from.
 */

#include "merced.h"

/* mov msr[mi]=v (to) or mov r=msr[mi]; returns the value read (a write
 * returns v). */
uint64_t merced_msr_access(Merced *m, bool to, unsigned mi, uint64_t v);
