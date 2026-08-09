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
 *  - The stacked file is a 96-register circular buffer; there's no
 *    automatic background spill on physical-register-file overflow (real
 *    hardware would spill once >96 registers are outstanding across the
 *    active call chain - firmware that goes that deep between explicit
 *    flushes will read back corrupted frames, same as a real overflow the
 *    RSE never got a chance to service). flushrs/loadrs themselves DO
 *    perform real backing-store I/O (see rse_flush()/rse_load() in
 *    merced.c), but assume the ar.bspstore anchor established by firmware's
 *    last `mov ar.bspstore=r` sits at a fresh NaT-collection-group boundary,
 *    and don't model ar.rnat or per-slot NaT collection words at all.
 *  - Memory attributes (UC/WB) are ignored beyond stripping bit 63 in
 *    physical addressing mode.
 */

#define MERCED_N_GR       128
#define MERCED_N_STACKED  96
#define MERCED_RSE_CAPACITY 4096
#define MERCED_N_FR       128
#define MERCED_N_BR       8
#define MERCED_N_AR       128
#define MERCED_N_CR       128
#define MERCED_N_RR       8
/* Merced's translation-register files are asymmetric: 8 instruction TRs and
 * 48 data TRs (SDM 245318; the reference model's merced entry uses the same
 * counts).  PAL_VM_SUMMARY reports these, and an OS installs pinned
 * translations at the slot numbers it reads back from there - so a symmetric
 * guess here makes a guest's DTR 20 land on some other slot entirely. */
#define MERCED_N_ITR      64
#define MERCED_N_DTR      64
#define MERCED_N_TR       MERCED_N_DTR   /* legacy alias: the larger file */
#define MERCED_N_TC       512  /* per side; amortize software VHPT refills */
#define MERCED_TRACE_HISTORY 512
#define MERCED_CALL_HISTORY 65536

#define MERCED_PHYS_MASK  0x000FFFFFFFFFFFFFull  /* strip UC bit + unimpl */

typedef struct {
    void *ud;
    /* Backed low RAM, used to bound the firmware/early-kernel region-7
     * physical alias. Zero disables that platform fallback. */
    uint64_t ram_size;
    /* size in {1,2,4,8}; addr is a physical address (attribute bit already
     * stripped). Unmapped reads should return all-ones and log. */
    uint64_t (*read)(void *ud, uint64_t addr, unsigned size);
    /* Optional instruction-fetch path for machines whose chipset gives code
     * fetches a different physical decode from ordinary data accesses. */
    uint64_t (*fetch)(void *ud, uint64_t addr, unsigned size);
    void     (*write)(void *ud, uint64_t addr, uint64_t val, unsigned size);
    /* Optional acceleration for architecturally ordinary repeated byte
     * stores. Return false if the range is not directly fillable. */
    bool     (*fill)(void *ud, uint64_t addr, uint8_t val, uint64_t len);
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
    uint8_t  pending_purge;     /* visible until the matching srlz completes */
    uint64_t itir, pte;         /* raw insert values, for debugging */
} MercedTlbEntry;

typedef struct Merced {
    MercedBus bus;

    /* --- architectural state --- */
    uint64_t ip;                        /* bundle address | slot (0-2) */
    uint64_t psr;
    /* An IC transition is not visible to nested-TLB classification until
     * the following data serialization operation completes. */
    uint8_t psr_ic_inflight;
    uint64_t cfm;
    uint64_t pr;                        /* 64 predicate bits, pr0 forced 1 */

    uint64_t gr_static[32];             /* r0-r31, bank 1 view of 16-31 */
    uint64_t gr_bank0[16];              /* r16-r31 bank 0 */
    uint8_t  nat_static[32];
    uint8_t  nat_bank0[16];

    uint64_t gr_stack[MERCED_RSE_CAPACITY];
    uint8_t  nat_stack[MERCED_RSE_CAPACITY];
    uint32_t bof;                       /* bottom-of-frame index into gr_stack */

    /* RSE backing-store bookkeeping. bof_total mirrors bof (updated at the
     * same four sites: call, return, cover, rfi-restore) but never wraps, so
     * it's a stable register-units axis for translating to/from real
     * addresses. rse_anchor_{addr,regs} record the (address, bof_total+sof)
     * pair captured the last time firmware executed `mov ar.bspstore=r`
     * (architecturally the only way an address<->register-count
     * correspondence gets established); rse_flushed_regs is that same axis's
     * mirror of ar.bspstore, advanced by flushrs and rewound by loadrs. */
    uint64_t bof_total;
    uint64_t rse_anchor_addr;
    int64_t  rse_anchor_regs;
    int64_t  rse_flushed_regs;
    /*
     * The implementation models the RSE's dirty/clean/invalid partitions
     * with a compact circular register file.  A real interruption protects
     * the interrupted partition while its handler executes cover/flushrs
     * and may switch BSPSTORE.  Preserve that implementation-private state
     * until the matching rfi; architectural CR/AR state remains guest-owned.
     */
    struct {
        uint64_t iip, ipsr;
        uint64_t gr_stack[MERCED_RSE_CAPACITY];
        uint8_t  nat_stack[MERCED_RSE_CAPACITY];
        uint32_t bof;
        uint64_t bof_total;
        uint64_t anchor_addr;
        int64_t  anchor_regs;
        int64_t  flushed_regs;
    } interruption_rse[4];
    uint8_t interruption_rse_depth;
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

    MercedTlbEntry itr[MERCED_N_ITR], dtr[MERCED_N_DTR];
    MercedTlbEntry itc[MERCED_N_TC], dtc[MERCED_N_TC];
    uint32_t itc_next, dtc_next;

    /* Integer Advanced Load Address Table.  Merced exposes 32 ALAT entries;
     * an entry associates an advanced-load destination register with the
     * physical byte range that supplied it. */
    struct {
        uint64_t phys_addr;
        uint8_t  size;
        uint8_t  reg;
        uint8_t  valid;
    } alat[32];
    /* Interval-timer external interrupt: edge-latched when ar.itc crosses
     * cr.itm, delivered at the next instruction boundary if psr.i is set,
     * acknowledged (cleared) by a cr.ivr read. Real firmware relies on this
     * for cooperative timeouts during driver/protocol enumeration; without
     * it, any code that waits on a timer tick spins forever. */
    uint8_t  timer_pending;
    /* External (platform) interrupts: a 256-bit per-vector pending bitmap,
     * mirroring a real I/O SAPIC's independent per-vector latches rather
     * than a single-slot "one interrupt at a time" simplification. That
     * matters because cr.tpr masks delivery by priority *class*: a vector
     * raised while its class is masked must stay latched without blocking
     * a later, higher-class vector from also becoming pending and getting
     * delivered first - confirmed live on the i2000 SDV BIOS, which raises
     * tpr around ATA identify/enumeration and stalls forever once a
     * lower-class periodic tick occupies the old single pending slot. */
    uint8_t  external_pending[32];
    uint8_t  external_itc;

    /* --- bookkeeping --- */
    uint8_t  taken;       /* set when the executed slot redirected IP */
    uint8_t  group_start; /* true if the next slot begins a new instruction
                            * group (stop bit, or a taken branch, precedes
                            * it) - alloc/cover validate against this */
    uint8_t  slot_stop_after; /* scratch: does the slot about to execute have
                                * a stop bit immediately after it (per its
                                * bundle template) - cover validates this */
    uint64_t trace_n;     /* stderr-trace this many slots (debug) */
    struct {
        uint64_t ip, raw, src2, src3, r8, r25, r32, r33, r34, r35, r36;
        uint64_t b0, pr;
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
    uint64_t last_successful_bundle;
    char     halt_msg[256];
    uint64_t halt_ip;

    /* one-shot warnings for approximated ops */
    uint32_t warned;

    /* cpuid[3] revision field (see merced_reset()) - defaults to 0
     * (matches i2000's own firmware cross-check); machines that need a
     * different reported stepping call merced_set_cpu_revision()
     * instead of poking cpuid[3] directly, so it survives merced_reset()
     * (e.g. the monitor's "reset" command, or a machine's own reset
     * handling) rather than reverting to the default every time. */
    uint8_t  cpu_revision;

    /* cpuid[3] model field (bits 23:16) - 0 = Merced (Itanium), 1 =
     * McKinley (Itanium 2). Set via merced_set_cpu_model(); survives
     * merced_reset() the same way cpu_revision does. */
    uint8_t  cpu_model;

    /* Firmware-visible PAL state.  These outlive a single PAL call because
     * the procedures that set them are "register this with the processor"
     * calls whose effect a later call (or a machine check) reads back. */
    uint64_t pal_mc_save_addr;         /* PAL_MC_REGISTER_MEM */
    uint64_t pal_pmi_entry;            /* PAL_PMI_ENTRYPOINT */
    uint64_t pal_interrupt_block_addr; /* PAL_PLATFORM_ADDR type 0 */
    uint64_t pal_io_block_addr;        /* PAL_PLATFORM_ADDR type 1 */
    bool     pal_mc_expected;          /* PAL_MC_EXPECTED latch */

    /* Upper bound of the persistent region-7 KSEG physical alias. */
    uint64_t region7_directmap_limit;
} Merced;

Merced *merced_create(const MercedBus *bus);
void    merced_destroy(Merced *m);
void    merced_reset(Merced *m);
/* Select a machine-supplied ITC clock.  The default remains one tick per
 * interpreted slot for deterministic tests and standalone CPU users. */
void    merced_set_external_itc(Merced *m, bool enabled);
void    merced_advance_itc(Merced *m, uint64_t ticks);

/* Overrides the stepping Windows/firmware sees in cpuid[3]'s revision
 * field (bits 15:8). Takes effect immediately and persists across
 * merced_reset(). See the comment on cpuid[3] in merced_reset() for
 * what's known about which values work where. */
void    merced_set_cpu_revision(Merced *m, uint8_t revision);

/* Overrides the family model Windows/firmware sees in cpuid[3]'s model
 * field (bits 23:16): 0 = Merced (Itanium), 1 = McKinley (Itanium 2).
 * Takes effect immediately and persists across merced_reset(). */
void    merced_set_cpu_model(Merced *m, uint8_t model);

/* Execute one instruction slot. On anything but MERCED_OK, halt_msg
 * describes why and the CPU is stopped at the offending IP. */
MercedStatus merced_step(Merced *m);

/* IA-32 compatibility engine.  These hooks deliberately use the IA-64
 * translation machinery: IA-32 code and data references share the Merced
 * TR/TC/RR resources architecturally. */
MercedStatus merced_ia32_step(Merced *m);
bool merced_ia32_read(Merced *m, uint64_t va, unsigned size,
                      bool ifetch, uint64_t *value);
bool merced_ia32_write(Merced *m, uint64_t va, unsigned size,
                       uint64_t value);
MercedStatus merced_ia32_instruction_intercept(Merced *m, uint64_t iim,
                                                uint16_t code);
uint64_t merced_ia32_gr_read(Merced *m, unsigned reg);
void merced_ia32_gr_write(Merced *m, unsigned reg, uint64_t value);

/* Latch a platform interrupt for delivery through cr.ivr. */
void merced_raise_external(Merced *m, uint8_t vector);
void merced_ack_external(Merced *m, uint8_t vector);

/* Force cr.tpr directly - EXPERIMENTAL hook for the i2000 ATA controller,
 * see the call site in machine_i2000.c for why. */
void merced_set_tpr(Merced *m, uint64_t tpr);

/* Full register dump for the monitor. */
void merced_dump_state(const Merced *m, char *buf, size_t len);
void merced_dump_trace(const Merced *m, unsigned count, FILE *out);
void merced_dump_calls(const Merced *m, unsigned count, FILE *out);

/* Read a general register by architectural number (for debugging). */
uint64_t merced_gr(const Merced *m, unsigned r);

/* Temporary VHPT-walker diagnostic counters - see the definition in
 * merced.c for what each field means. Remove once the walker's hit rate
 * on real workloads is understood. */
void merced_vhpt_stats(uint64_t *calls, uint64_t *disabled, uint64_t *hit,
                       uint64_t *unmapped, uint64_t *tagfail, uint64_t *np);
void merced_fault_stats(uint64_t *counts, size_t count);
