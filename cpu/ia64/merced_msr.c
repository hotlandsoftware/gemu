/* merced_msr.c -- Itanium (Merced) model-specific registers.
 *
 * Merced MSRs are undocumented; this model is derived from what PAL does
 * with them. Sites below are in the Compaq DL590/64 M01 ROM (PAL_A 6.6.23,
 * PAL_B 7.7.28). Every register is storage that reads back what was written
 * (0 if never written) unless listed here; there is no poll-toggle. A read
 * repeated many times with no intervening write is reported, since that is
 * what a hardware-driven bit this model does not drive yet looks like.
 *
 * The command port, 600h / 615h / 60Dh / 60Eh. PAL issues a command by
 * writing its operand word to 615h and the control word to 600h, waiting
 * for 60Eh to read 0, then writing 60Dh (the kick) and waiting for 60Eh to
 * read 0 again (PAL_A 0xFFFF7C10-0xFFFF7CBC, 0xFFFF7F00-0xFFFF803C; PAL_B
 * 0xFFF48E90-0xFFF48FDC). 60Eh is the busy flag (loops "while != 0" and
 * "while bit 0 set"); commands complete at once here, so it reads 0.
 * 600h bits 46:45 are a status field PAL never writes (none of its 600h
 * writes sets them) and polls after a kick:
 *   operand bits 27:26 = 01 (0x04000240) -> PAL_B waits for status 1
 *                                          (0xFFF48F10);
 *   operand bits 27:26 = 11 (0x0C000240) -> PAL_B waits UNTIL status == 2,
 *                                          then until 3 (0xFFF48F70,
 *                                          0xFFF48F90); PAL_A only waits
 *                                          while it is 2 (0xFFFF7FE0).
 * So the status follows bits 27:26 of the last kicked operand, and 11 passes
 * through a transitional 2 that PAL_B must see before 3. INFERRED: how long
 * hardware stays in 2 is not known; here it holds until one read of 600h
 * has returned it, the shortest time both PALs accept. What the operand
 * selects is not known either. A kicked operand
 * is otherwise left in 615h: PAL reads 615h back after the 0x80 command
 * (0xFFFF7CB0) and keys on bits 23:22; what hardware returns there is not
 * known, so it reads the operand.
 *
 * A second command family uses bits 23:22 the same way. B1+ PAL (fw130
 * 0xFFDCA800, DL590 0xFFF52800 -- the DL590 never runs it) kicks 0x00400240
 * and waits for status 1, then 0x00C00240 and waits past 2 for 3. Every
 * operand PAL waits on carries select 0x240 in bits 11:0; the one other
 * operand seen with bits 23:22 set, 0x00808100 (PAL_A 0xFFFF7C10, both
 * machines), is never followed by a status wait. So bits 23:22 give the
 * status only for select 0x240 with bits 27:26 clear. INFERRED from PAL.
 * That routine then switches the control word to mode 2 (600h bits 4:0),
 * kicks 0x240 | index << 12, and reads 615h: bit 24 clear spins forever
 * (0xFFDCAAA0), so hardware must report completion there; bit 25 is a data
 * bit shifted into a result PAL does not test. Bit 24 is set on such a
 * kick; bit 25 stays the operand's (what hardware returns is not known).
 *
 * Line transfers (B1+ test 0xFFE050A0 via 0xFFDC8C30 / 0xFFDC89E0): mode
 * 1Ch writes the 64-byte line in 400h-407h (byte mask in 60Ch) with the
 * state in control bits 6:5 to address 615h; mode 0Eh reads it back into
 * 400h-407h / 60Ch and returns the state in control bits 14:13. The test
 * writes state 2 and requires 2 back, 615h = 0 after the read, and the
 * eight words unchanged. INFERRED: a read of an address never written
 * returns the registers as they stand and state 0.
 * The line is the L3's, in the set of address 615h and the way in control
 * 10:9 (0xFFDC8C30 reads it, merges the fields it was asked to change and
 * writes it back); merced_cache.c holds it (merced_l3_line_write/read).
 * A line is a cache line at physical address 615h: test 0xFFE05240
 * writes its own code page's line (state 1, control 10:9 = the cache way
 * under test) and requires plain ld8s to return it. Such a line stays
 * resident until the sequence terminator operand 8140h (both
 * command sequences end with it; that test's teardown 0xFFDC0CE0 runs
 * command A then 8140h) -- INFERRED: that is the flush.
 *
 * 612h bit 6 acknowledges bit 9. PAL clears bit 9 and spins until bit 6
 * reads 1 (PAL_A 0xFFFF7E00-0xFFFF7E5C; PAL_B 0xFFF48D60-0xFFF48DCC), runs
 * the command sequence above, then sets bit 9 again (0xFFFF8080,
 * 0xFFF49090). Bit 6 reads as the complement of bit 9; PAL never writes it.
 */
#include "merced_msr.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    MSR_CTL     = 0x600,    /* control word; bits 46:45 command status */
    MSR_KICK    = 0x60D,
    MSR_BUSY    = 0x60E,
    MSR_ACK     = 0x612,    /* bit 9 request, bit 6 its acknowledge */
    MSR_OPERAND = 0x615,
    MSR_RATIO   = 0x617,    /* bits 25:22 the clock-ratio strap */
};
#define RATIO_SHIFT 22
#define RATIO_MASK  (UINT64_C(0xF) << RATIO_SHIFT)
#define CTL_STATUS_SHIFT 45
#define CTL_STATUS_MASK  (UINT64_C(3) << CTL_STATUS_SHIFT)
#define CTL_MODE         UINT64_C(0x1F)
#define CTL_MODE_READ    UINT64_C(2)
#define OP_SELECT        UINT64_C(0xFFF)
#define OP_SEL_SYNC      UINT64_C(0x240)
#define OP_DONE          (UINT64_C(1) << 24)
#define ACK_REQ          (UINT64_C(1) << 9)
#define ACK_ACK          (UINT64_C(1) << 6)

#define MSR_LINE0        0x400u
#define MSR_LINE_MASK    0x60Cu
#define MODE_LINE_WRITE  UINT64_C(0x1C)
#define MODE_LINE_READ   UINT64_C(0x0E)
#define OP_SEQ_END       UINT64_C(0x8140)

#define CTL_WAY_SHIFT    9      /* control 10:9: the way (0xFFDC8C30) */

/* Reads of one register with no write in between before it is reported. */
#define POLL_REPORT 4096u

static uint64_t stored(const Merced *m, unsigned mi)
{
    return m->msr_written[mi] ? m->msr[mi] : 0;
}

static void store(Merced *m, unsigned mi, uint64_t v)
{
    m->msr[mi] = v;
    m->msr_written[mi] = true;
    m->msr_polls[mi] = 0;
}

uint64_t merced_msr_access(Merced *m, bool to, unsigned mi, uint64_t v)
{
    mi &= 0xFFFu;
    if (to) {
        switch (mi) {
        case MSR_CTL:
            /* The status field is the processor's, not software's. */
            store(m, mi, (v & ~CTL_STATUS_MASK) | (stored(m, mi) & CTL_STATUS_MASK));
            break;
        case MSR_KICK: {
            store(m, mi, v);
            const uint64_t op = stored(m, MSR_OPERAND);
            const bool sel240 = (op & OP_SELECT) == OP_SEL_SYNC;
            uint64_t st = (op >> 26) & 3u;
            if (st == 0u && sel240) { st = (op >> 22) & 3u; }
            if (sel240 && (stored(m, MSR_CTL) & CTL_MODE) == CTL_MODE_READ) {
                store(m, MSR_OPERAND, op | OP_DONE);
            }
            const uint64_t mode = stored(m, MSR_CTL) & CTL_MODE;
            const unsigned way = (unsigned)((stored(m, MSR_CTL) >> CTL_WAY_SHIFT) & 3u);
            if (mode == MODE_LINE_WRITE) {
                uint64_t data[8];
                for (unsigned w = 0; w < 8; w++) { data[w] = stored(m, MSR_LINE0 + w); }
                /* 600h bit 19: 60Ch was written for this transfer
                 * (0xFFDC8C30 sets the bit exactly when it writes 60Ch) and
                 * supplies the line's bus check bytes; otherwise they are
                 * generated (0xFFE15840 passes them with one bit flipped). */
                merced_l3_line_write(m, op, way, data, stored(m, MSR_LINE_MASK),
                                     ((stored(m, MSR_CTL) >> 19) & 1u) != 0,
                                     (unsigned)((stored(m, MSR_CTL) >> 5) & 3u));
            } else if (mode == MODE_LINE_READ) {
                uint64_t data[8], mask = 0, line_addr = 0;
                unsigned state = 0;
                uint64_t ctl = stored(m, MSR_CTL) & ~(UINT64_C(3) << 13);
                if (merced_l3_line_read(m, op, way, data, &mask, &state, &line_addr)) {
                    for (unsigned w = 0; w < 8; w++) { store(m, MSR_LINE0 + w, data[w]); }
                    store(m, MSR_LINE_MASK, mask);
                    ctl |= (uint64_t)state << 13;
                }
                store(m, MSR_CTL, ctl);
                /* 615h returns the address the way holds: 0xFFE0F550 finds
                 * the way holding a line it loaded by comparing it with the
                 * load's address; 0xFFE050A0 wrote address 0 and reads 0. */
                store(m, MSR_OPERAND, line_addr);
            }
            if (op == OP_SEQ_END) { merced_cache_drop_injected(m); }
            if (st == 3u) { st = 2u; }  /* transitional; see the header */
            store(m, MSR_CTL, (stored(m, MSR_CTL) & ~CTL_STATUS_MASK) |
                              (st << CTL_STATUS_SHIFT));
            break;
        }
        case MSR_BUSY:
            store(m, mi, 0);            /* a status register: never busy */
            break;
        case MSR_ACK:
            store(m, mi, v & ~ACK_ACK);
            break;
        default:
            store(m, mi, v);
            break;
        }
        /* After the store: an array read command leaves its result in the
         * register (merced_cache.c). */
        merced_cache_msr_write(m, mi, v);
        return v;
    }

    uint64_t out = stored(m, mi);
    switch (mi) {
    case MSR_RATIO:
        /* Bits 25:22 are the clock-ratio strap, not storage: fw130's
         * PAL_FREQ_RATIOS (PAL_PROC index 14, RAM copy 0x7FF58100) reads
         * them as a code and returns processor and ITC ratio (8 + code) / 2
         * over a 1/1 bus for codes 0-8, -3 (no such ratio) otherwise. A
         * board that never latched a strap leaves the register as storage. */
        if (m->clock_ratio_strapped) {
            out = (out & ~RATIO_MASK) | ((uint64_t)(m->clock_ratio_code & 0xFu) << RATIO_SHIFT);
        }
        break;
    case MSR_CTL:
        if (((out & CTL_STATUS_MASK) >> CTL_STATUS_SHIFT) == 2u) {
            /* Seen once: the transition completes. */
            m->msr[mi] = out | CTL_STATUS_MASK;
        }
        break;
    case MSR_BUSY:
        out = 0;
        break;
    case MSR_ACK:
        out = (out & ~ACK_ACK) | ((out & ACK_REQ) ? 0 : ACK_ACK);
        break;
    default:
        break;
    }
    if (m->msr_polls[mi] < UINT16_MAX && ++m->msr_polls[mi] == POLL_REPORT) {
        fprintf(stderr, "merced: msr[0x%03x] read %u times with no write, reads 0x%016" PRIX64
                " -- a hardware-driven bit? ip=0x%016" PRIX64 " (unmodelled)\n",
                mi, POLL_REPORT, out, m->ip);
    }
    return out;
}
