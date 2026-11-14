#pragma once

/*
 * Merced cache arrays behind PAL's array-access MSRs (see merced_cache.c).
 *
 * This header is included by merced.h (after it has typedef'd Merced), which
 * embeds MercedCache in the Merced struct: everything here is plain data, so a
 * raw copy of the Merced struct (a machine snapshot) carries the whole model,
 * and merced_reset()'s memset is the processor reset.
 */

#include <stdbool.h>
#include <stdint.h>

enum { MC_L2_SETS = 256, MC_L2_WAYS = 6, MC_L2_CHUNKS = 32 };

typedef struct {
    uint16_t chunk[MC_L2_CHUNKS];
    uint8_t  ecc[8];           /* per qword: the four chunks' 2-bit fields */
    uint32_t tag;
    uint64_t tag_seq;          /* when the tag was last written */
    uint16_t stword;           /* 5E2h payload 9:0; state = 8:6 */
    bool     tag_lo, tag_hi;   /* both halves written */
    bool     parity;           /* the parity bit as written */
    bool     modified;         /* state 2 */
    bool     owned;            /* a fill's, not the ports' (see l2_untrack) */
} MercedL2Way;

/* The L3 behind the 600h port: 4 ways of 64-byte lines. The port reads and
 * writes a (set, way) -- the set from the address in 615h, the way in
 * control 10:9 -- with no tag compare: phase-2 test 0xFFE0E100 retags the
 * four ways of one set to addresses 1 MB apart (bits 28-31 flipped), loads
 * four other lines of the set and reads ways 0-3 back at the first load's
 * address, expecting each to hold one of the loaded values. Loads match
 * (set, tag). INFERRED: 16K sets (a 4 MB L3); the tests only alias at 1 MB
 * and above, which any size up to that keeps in one set. Only lines in use
 * are kept, here by line address (set and tag together); when full the
 * oldest is replaced.
 * state: the port's 2-bit line state (control 6:5); 3 is the modified one
 * fc writes back (0xFFE0D520), 0 invalid. A planted line (written through
 * the port) is what a load of it gets; a filled one is memory's. */
enum { MC_INJ_LINES = 256, MC_L3_SETS = 16384, MC_L3_HASH = 1024 };

typedef struct {
    uint64_t pa;                /* line address */
    uint64_t addr;              /* 615h as written (a fill: the line address) */
    uint64_t data[8];
    uint64_t seq;
    uint64_t mask;              /* 60Ch as written with the line */
    uint8_t  ecc[8];
    unsigned state, way;
    bool     planted, used;
    bool     owned;             /* came from a fill (see l3_untrack) */
} MercedL3Line;

/* L1 instruction cache: 16 KB, 4 ways, 32-byte lines (two bundles), 128
 * sets. See the L1I section of merced_cache.c. */
enum { MC_L1I_SETS = 128, MC_L1I_WAYS = 4, MC_L1I_WORDS = 4 };

typedef struct {
    uint64_t word[MC_L1I_WORDS];  /* the line in the array's bit layout */
    uint8_t  wpar;             /* per-word parity bits as written */
    uint32_t tag;              /* PA >> 12 */
    bool     tpar;             /* tag parity as written */
    bool     valid;
    bool     planted;          /* written through the ports, not filled:
                                  word[] is what a fetch executes */
    uint8_t  dec_ok;           /* bit b: dec[b] holds bundle b decoded */
    uint64_t dec[2][2];
    bool     tag_bad;          /* parity checks, kept up to date by */
    uint8_t  word_bad;         /*   icache_set_state on every change */
} MercedL1IWay;

/* L1 data cache: 16 KB, 4 ways, 32-byte lines, 128 sets. See the L1D
 * section of merced_cache.c. */
enum { MC_L1D_SETS = 128, MC_L1D_WAYS = 4, MC_L1D_CHUNKS = 16 };

typedef struct {
    uint16_t chunk[MC_L1D_CHUNKS];  /* qword w's bits 16k+15:16k are chunk 4w+k */
    uint8_t  par[4];             /* per qword: even parity of each byte */
    uint32_t tag;                /* PA >> 12 */
    uint8_t  tpar;               /* even parity of tag 15:0 (bit 0), 31:16 (1) */
    bool     planted;            /* written through the ports: chunk[] is what
                                    a load gets; a filled line is memory's */
} MercedL1DWay;

typedef struct {
    /* The interpreter consults these two on every bundle and access. */
    bool     observe;           /* every data access and fetch must reach the
                                   model; the JIT must not run */
    bool     ic_track;          /* the I-cache model is following fills, so
                                   every fetch must reach it too */
    bool     served;            /* the bundle cache holds a bundle the model
                                   served, which memory would not give */
    /* The access xlate just translated is not write-back (a UC mapping or
     * physical address with bit 63 set). Only an observing model reads it,
     * and the access that consumes it clears it again: an uncacheable
     * access bypasses the caches -- PAL's own code and data live at
     * 0x8000... physical, and lines its L2 self-tests leave tagged with
     * PAL's flash page must not answer them. Inverted, so that zero is the
     * ordinary write-back access. */
    bool     access_uc;

    uint64_t seq;
    bool     mca_pending;
    uint64_t l2mode;            /* msr[0x580] as last written */
    unsigned nl1d;              /* valid L1D ways */
    unsigned nl1d_planted;      /* of those, planted */
    bool     l1d_known;         /* every fill since the last invalidate seen */
    uint32_t l1d_loads;         /* cacheable loads since the invalidate */
    uint8_t  l1d_plru[MC_L1D_SETS];     /* 5D2h */
    uint8_t  l1d_valid[64];     /* 5D1h: set & 3Fh, bit way + 4 * set bit 6 */
    uint16_t l1d_wp[MC_L1D_SETS][8];    /* 5D3h, stored only */
    MercedL1DWay l1d[MC_L1D_SETS][MC_L1D_WAYS];
    uint64_t icctl;             /* msr[0x420] as last written */
    unsigned nplanted;          /* valid planted L1I ways */
    bool     ic_known;          /* every fill since the last invalidate seen */
    bool     ic_unknown_logged;
    uint64_t ic_lost_ip, ic_lost_psr;   /* where ic_known was lost */
    uint32_t ic_fetches;        /* cacheable fetches since the invalidate */
    uint8_t  plru[MC_L1I_SETS]; /* 423h, per set */
    MercedL1IWay l1i[MC_L1I_SETS][MC_L1I_WAYS];
    bool     testmode;          /* msr[0x580] bit 40 or 43 */
    MercedL3Line inj[MC_INJ_LINES];
    unsigned ninj;              /* L3 lines held (any state) */
    unsigned nl3_planted;       /* planted lines in a non-zero state */
    bool     l3_dirty;          /* l3_head/l3_next need rebuilding (l3_find) */
    int16_t  l3_head[MC_L3_HASH];
    int16_t  l3_next[MC_INJ_LINES];
    bool     l3_known;          /* every fill since the last 8140h flush seen */
    uint32_t l3_loads;          /* cacheable loads since then */
    uint8_t  l3_plru[MC_L3_SETS];
    unsigned nvalid;            /* valid L2 ways */
    unsigned nstate;            /* L2 ways with a non-zero state, tagged or not
                                   (fc acts on them, 0xFFE0D520) */
    uint64_t l1d_t0, l2_t0, l3_t0, ic_t0;   /* bundle each window opened */
    bool     l2_known;          /* every fill since the last L2 invalidate seen */
    uint32_t l2_accesses;       /* cacheable accesses since then */
    uint64_t l2ctl;             /* msr[0x5E4]: 6 and 2 disable the L2 */
    uint64_t l2inv;             /* msr[0x5E5]: a 0 -> 1 of 17 invalidates */
    uint8_t  l2_order[256][3];  /* pair LRU: pairs (0,1)(2,3)(4,5), [0] oldest */
    uint8_t  l2_inpair[256];    /* bit p: the victim in pair p is the odd way */
    uint64_t port_ip;           /* memory ops of the current bundle: */
    unsigned port_ri, port;     /* 0 logs in 5EAh, 1 in 5EBh */
    MercedL2Way l2[MC_L2_SETS][MC_L2_WAYS];
} MercedCache;

/* Observes a write to one of the array / mode MSRs (others are ignored). */
void merced_cache_msr_write(Merced *m, unsigned mi, uint64_t v);
/* While m->cache.observe: a load served from a resident array line, in
 * memory byte order; false = not resident, use the bus. */
bool merced_cache_load(Merced *m, uint64_t pa, unsigned size, uint64_t *raw);
/* The same for a store (raw in memory byte order): true = the L2 took it
 * (a resident line was updated, or an error was raised), so it must not
 * reach memory. */
bool merced_cache_store(Merced *m, uint64_t pa, unsigned size, uint64_t raw);
/* The 600h port's line write and read: the L3 line in (set of `addr`,
 * `way`). A written line holds `data` (memory byte order) in `state` (0
 * invalid) until fc or merced_cache_drop_injected; `mask` is 60Ch, which
 * supplies the bus check bytes when `ecc_given`. A read returns false for a
 * (set, way) holding nothing, else the line and the address it holds (615h
 * as written, or the line address of a fill). */
void merced_l3_line_write(Merced *m, uint64_t addr, unsigned way, const uint64_t data[8],
                          uint64_t mask, bool ecc_given, unsigned state);
bool merced_l3_line_read(Merced *m, uint64_t addr, unsigned way, uint64_t data[8],
                         uint64_t *mask, unsigned *state, uint64_t *line_addr);
void merced_cache_drop_injected(Merced *m);
/* fc on a (physical, write-back) address: a resident L2 line is checked,
 * written back if modified, and invalidated. */
void merced_cache_fc(Merced *m, uint64_t pa);
/* A machine check the arrays raised during the last instruction, consumed. */
bool merced_cache_take_mca(Merced *m);

/* While m->cache.observe or ic_track: the fetch of the bundle at `pa` (`wb`:
 * a cacheable fetch). HIT fills *lo and *hi (the bundle's two words); MCA means
 * an array error was raised, take it with merced_cache_take_mca. */
typedef enum { MERCED_IC_MISS, MERCED_IC_HIT, MERCED_IC_MCA } MercedIcResult;
MercedIcResult merced_icache_fetch(Merced *m, uint64_t pa, bool wb,
                                   uint64_t *lo, uint64_t *hi);
/* A fetch the I-cache did not serve: the bundle at `pa` from the L2 or an
 * L3 line holding it (they can hold stores memory has not seen); false =
 * memory's. */
bool merced_cache_fetch(Merced *m, uint64_t pa, uint64_t *lo, uint64_t *hi);
/* Called on every bundle the interpreter fetches while the model observes:
 * ends fill windows that have been open too long (merced_cache.c). */
void merced_cache_tick(Merced *m);
