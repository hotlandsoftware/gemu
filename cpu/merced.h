#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Intel Itanium (Merced) - IA-64 interpreter core.
 *
 * Instruction encodings follow the Itanium SDM (cross-checked against HP
 * Ski's machine-generated encoding tables, reference/ski/src/encodings/).
 * The subset implemented is driven by what the HP i2000 firmware actually
 * executes; anything undecoded halts with MERCED_HALT_UNIMPL and a precise
 * diagnostic so the gap can be filled.
 *
 * Simplifications vs. real hardware (deliberate, revisit as needed):
 *  - Instructions execute sequentially; instruction-group parallelism is
 *    not modeled (a RAW hazard inside one group reads the NEW value here,
 *    old value on silicon - compilers never emit that, handcode rarely).
 *  - The RSE never spills/fills to backing store on its own; the stacked
 *    file is a 96-register circular buffer (flushrs/loadrs are no-ops).
 *  - Memory attributes (UC/WB) are ignored beyond stripping bit 63 in
 *    physical addressing mode.
 */

#define MERCED_N_GR       128
#define MERCED_N_STACKED  96
#define MERCED_N_FR       128
#define MERCED_N_BR       8
#define MERCED_N_AR       128
#define MERCED_N_CR       128
#define MERCED_N_RR       8
#define MERCED_N_TR       16   /* per side (code/data) */
#define MERCED_N_TC       64   /* per side, round-robin */
#define MERCED_TRACE_HISTORY 512
#define MERCED_CALL_HISTORY 128

#define MERCED_PHYS_MASK  0x000FFFFFFFFFFFFFull  /* strip UC bit + unimpl */

typedef struct {
    void *ud;
    /* size in {1,2,4,8}; addr is a physical address (attribute bit already
     * stripped). Unmapped reads should return all-ones and log. */
    uint64_t (*read)(void *ud, uint64_t addr, unsigned size);
    /* Optional instruction-fetch path for machines whose chipset gives code
     * fetches a different physical decode from ordinary data accesses. */
    uint64_t (*fetch)(void *ud, uint64_t addr, unsigned size);
    void     (*write)(void *ud, uint64_t addr, uint64_t val, unsigned size);
} MercedBus;

typedef enum {
    MERCED_OK = 0,
    MERCED_HALT_UNIMPL,   /* undecoded/unimplemented instruction */
    MERCED_HALT_BREAK,    /* break instruction executed */
    MERCED_HALT_FAULT,    /* fault that could not be delivered */
    MERCED_HALT_BAD_IP,   /* branch to obviously bad address */
    MERCED_HALT_DEADLOOP, /* firmware entered an empty unconditional loop */
} MercedStatus;

/* 82-bit FP register: significand, 17-bit exponent, sign. */
typedef struct {
    uint64_t sig;
    uint32_t exp;      /* 17 bits */
    uint8_t  sign;
    uint8_t  nat;      /* NaTVal marker (simplified) */
} MercedFpReg;

typedef struct {
    uint64_t va_start, va_end;  /* rid-qualified range */
    uint64_t pfn_base;          /* phys base of mapping */
    uint32_t rid;
    uint8_t  ps;                /* page size (log2) */
    uint8_t  valid;
    uint64_t itir, pte;         /* raw insert values, for debugging */
} MercedTlbEntry;

typedef struct Merced {
    MercedBus bus;

    /* --- architectural state --- */
    uint64_t ip;                        /* bundle address | slot (0-2) */
    uint64_t psr;
    uint64_t cfm;
    uint64_t pr;                        /* 64 predicate bits, pr0 forced 1 */

    uint64_t gr_static[32];             /* r0-r31, bank 1 view of 16-31 */
    uint64_t gr_bank0[16];              /* r16-r31 bank 0 */
    uint8_t  nat_static[32];
    uint8_t  nat_bank0[16];

    uint64_t gr_stack[MERCED_N_STACKED];
    uint8_t  nat_stack[MERCED_N_STACKED];
    uint32_t bof;                       /* bottom-of-frame index into gr_stack */

    MercedFpReg fr[MERCED_N_FR];
    uint64_t br[MERCED_N_BR];
    uint64_t ar[MERCED_N_AR];
    uint64_t cr[MERCED_N_CR];
    uint64_t rr[MERCED_N_RR];
    uint64_t pkr[16];
    uint64_t dbr[16], ibr[16];
    uint64_t pmc[32], pmd[32];
    uint64_t cpuid[8];
    /* Merced model-specific registers (opaque, no public documentation).
     * PAL polls hardware-handshake status bits in these, with mixed
     * polarities (wait-for-set and wait-for-clear). Reads of never-written
     * MSRs alternate 0 / all-ones so either poll flavor completes within
     * two iterations; written MSRs read back their stored value. */
    uint64_t msr[4096];
    uint8_t  msr_written[4096];
    uint8_t  msr_toggle[4096];
    uint16_t msr_polls[4096];   /* reads since last write (poll detector) */

    MercedTlbEntry itr[MERCED_N_TR], dtr[MERCED_N_TR];
    MercedTlbEntry itc[MERCED_N_TC], dtc[MERCED_N_TC];
    uint32_t itc_next, dtc_next;

    /* --- bookkeeping --- */
    uint8_t  taken;       /* set when the executed slot redirected IP */
    uint64_t trace_n;     /* stderr-trace this many slots (debug) */
    struct {
        uint64_t ip, raw, src2, src3, r25, b0;
        uint8_t unit, qp;
    } trace_history[MERCED_TRACE_HISTORY];
    uint32_t trace_history_next;
    struct {
        uint64_t from, to;
        uint8_t is_return;
    } call_history[MERCED_CALL_HISTORY];
    uint32_t call_history_next;
    uint64_t ninsts;
    uint64_t nfaults;
    char     halt_msg[256];
    uint64_t halt_ip;

    /* one-shot warnings for approximated ops */
    uint32_t warned;
} Merced;

Merced *merced_create(const MercedBus *bus);
void    merced_destroy(Merced *m);
void    merced_reset(Merced *m);

/* Execute one instruction slot. On anything but MERCED_OK, halt_msg
 * describes why and the CPU is stopped at the offending IP. */
MercedStatus merced_step(Merced *m);

/* Full register dump for the monitor. */
void merced_dump_state(const Merced *m, char *buf, size_t len);
void merced_dump_trace(const Merced *m, unsigned count, FILE *out);
void merced_dump_calls(const Merced *m, unsigned count, FILE *out);

/* Read a general register by architectural number (for debugging). */
uint64_t merced_gr(const Merced *m, unsigned r);
