/* Intel 82559 Fast Ethernet controller. See e100.h for scope and for the SDM /
 * DS / AP citations. Where those three are silent the model follows Intel's
 * own drivers, cited by name at each use: the Linux e100 driver (Intel,
 * 1999-2006) and the EFI UNDI driver (E100B, Intel, 1999-2002).
 *
 * Execution model. The device is fast compared to anything a driver can
 * observe, so a control command is accepted and executed in the CSR write that
 * issues it: the SCB command byte reads back 0 at once (SDM 6.5.1), and the
 * Command Unit runs the Command Block List until it suspends, goes idle or
 * must wait (a dynamic-TBD pointer not yet valid, a received PAUSE, bus
 * mastering disabled, or the per-call budget that keeps a cyclic list without
 * EL/S from hanging the emulator); waiting work resumes in e100_poll. Received
 * frames enter a 3 KiB receive FIFO (DS 2.2) at wire rate and are moved to
 * Receive Frame Descriptors as soon as the RU is Ready -- the 82559 holds
 * frames there while the RU is Suspended or out of resources (SDM 6.4.3.4).
 * Behaviour that is not modelled is named once on stderr (unimpl), never
 * silently accepted. */
#include "e100.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Device time base: the wiring clock is converted to this rate. */
enum { TIME_HZ = 1193182 };

/* ---- unimplemented behaviour ----------------------------------------------- */

/* An unmodelled behaviour, named on stderr once per key (capped): the list of
 * what the model still lacks. */
static void unimpl(const char *key, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void unimpl(const char *key, const char *fmt, ...)
{
    enum { NKEYS = 64 };
    static char seen[NKEYS][32];
    static unsigned nseen;
    for (unsigned i = 0; i < nseen; i++) {
        if (!strcmp(seen[i], key)) { return; }
    }
    if (nseen >= NKEYS) { return; }
    snprintf(seen[nseen++], sizeof seen[0], "%s", key);
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "e100: unimplemented: %s\n", msg);
}

/* ---- register and structure layouts ---------------------------------------- */

/* SCB STAT/ACK byte = SCB status word bits 15:8 (SDM Table 13, DS 9.1.1). */
enum {
    ST_CX = 0x80, ST_FR = 0x40, ST_CNA = 0x20, ST_RNR = 0x10,
    ST_MDI = 0x08, ST_SWI = 0x04, ST_ER = 0x02, ST_FCP = 0x01,
};
/* SCB interrupt control byte = command word bits 31:24 (SDM Table 14). */
enum {
    IC_M = 0x01, IC_SI = 0x02, IC_FCP_MASK = 0x04, IC_ER_MASK = 0x08,
    IC_RNR_MASK = 0x10, IC_CNA_MASK = 0x20, IC_FR_MASK = 0x40, IC_CX_MASK = 0x80,
};
/* CUS (status bits 7:6) and RUS (bits 5:2), SDM Table 13. */
enum { CU_IDLE = 0, CU_SUSP = 1, CU_ACTIVE = 2 };        /* 2 = LPQ active */
enum { RU_IDLE = 0, RU_SUSP = 1, RU_NORES = 2, RU_READY = 4 };

/* Action command block: command word bits (SDM Figure 14) and status bits. */
#define CB_EL   (1u << 31)
#define CB_S    (1u << 30)
#define CB_I    (1u << 29)
#define CB_NC   (1u << 20)               /* transmit: no CRC/SA insertion */
#define CB_SF   (1u << 19)               /* transmit: flexible mode */
enum { CBS_C = 0x8000, CBS_OK = 0x2000, CBS_U = 0x1000 };

/* Receive Frame Descriptor (SDM Figure 25). */
#define RFD_EL  (1u << 31)
#define RFD_S   (1u << 30)
#define RFD_H   (1u << 20)
#define RFD_SF  (1u << 19)
/* RFD status bits (SDM Table 52). */
enum {
    RS_CRC = 0x0800, RS_ALIGN = 0x0400, RS_NOSPACE = 0x0200, RS_OVERRUN = 0x0100,
    RS_SHORT = 0x0080, RS_TYPE = 0x0020, RS_RXERR = 0x0010, RS_NOMATCH = 0x0004,
    RS_NOTIA = 0x0002,
};

/* MDI control register (SDM Table 26). */
#define MDI_IE    (1u << 29)
#define MDI_READY (1u << 28)

/* PHY: the integrated 82555-compatible unit at MDI address 1 (SDM 7.1, DS 10.3.3). */
enum { PHY_ADDR = 1 };
enum {
    BMCR_RESET = 0x8000, BMCR_LOOP = 0x4000, BMCR_SPEED100 = 0x2000,
    BMCR_ANEN = 0x1000, BMCR_PDOWN = 0x0800, BMCR_ISOLATE = 0x0400,
    BMCR_ANRESTART = 0x0200, BMCR_FDX = 0x0100,
};
/* DS 10.1.1-10.1.5 reset values: 100 Mb/s + auto-negotiation enabled; the
 * advertisement's technology field 00101111b = 10/10FD/100/100FD + Pause. */
enum { BMCR_DEFAULT = 0x3000, ANAR_DEFAULT = 0x05E1 };
/* BMSR constant part (DS 10.1.2): 100FD, 100HD, 10FD, 10HD capable,
 * auto-negotiation able, extended registers. */
enum { BMSR_FIXED = 0x7809, BMSR_ANDONE = 0x0020, BMSR_LINK = 0x0004 };
/* PHY identifier (DS 10.1.3-10.1.4; SDM Table 62, 82555 revision 4). */
enum { PHY_ID1 = 0x02A8, PHY_ID2 = 0x0154 };
/* The link partner is a model choice, not a document fact: an
 * auto-negotiating 10/100 switch port advertising 100FD, 100HD, 10FD and
 * 10HD, no pause, Acknowledge set -- what register 5 holds once the
 * exchange has completed (SDM 7.2.5). */
enum { LP_WORD = 0x41E1 };

/* Link timing, from IEEE 802.3 clause 28 (not in the Intel documents): an
 * FLP burst every ~16 ms, about 14 bursts for the 3 matching + 3
 * acknowledged + 6-8 trailing code words of SDM 7.4.1, then
 * link_fail_inhibit_timer (750 ms) before link_status is OK; a renegotiation
 * from an up link first holds the link down for break_link_timer (1.2 s). A
 * forced-mode link only waits out the inhibit timer. Intel's UNDI notes a
 * PHY reset costs "3-4 seconds" of renegotiation on its hardware
 * (E100B.C, E100bShutdown); the 802.3 minimum is used here. */
#define AN_TICKS      ((uint64_t)TIME_HZ * 974 / 1000)
#define FORCED_TICKS  ((uint64_t)TIME_HZ * 750 / 1000)
#define BREAK_TICKS   ((uint64_t)TIME_HZ * 1200 / 1000)

/* Configure command bytes (SDM 6.4.2.3, Table 40). Reset values are the
 * per-field "Default" of 6.4.2.3.1. Byte 8 bit 0 (MII mode) has no default
 * and must be set before the 82559 transmits or receives (SDM 6.4.2.3); it
 * resets to 0, so a device that was never configured stays off the wire
 * (SDM 6.2). */
static const uint8_t k_cfg_default[22] = {
    0x16, 0x08, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x2E,
    0x00, 0x60, 0x00, 0xF2, 0xC8, 0x00, 0x40, 0xF2, 0x80, 0x3F, 0x05,
};

/* Statistical counters in dump order (SDM Table 17, DS Table 23). */
enum {
    SC_TX_GOOD, SC_TX_MAXCOL, SC_TX_LATECOL, SC_TX_UNDERRUN, SC_TX_LOSTCRS,
    SC_TX_DEFER, SC_TX_SINGLE, SC_TX_MULTI, SC_TX_TOTALCOL,
    SC_RX_GOOD, SC_RX_CRC, SC_RX_ALIGN, SC_RX_RESOURCE, SC_RX_OVERRUN,
    SC_RX_CDT, SC_RX_SHORT, SC_FC_TX_PAUSE, SC_FC_RX_PAUSE, SC_FC_RX_UNSUP,
    SC_COUNT
};
_Static_assert(SC_COUNT == E100_NSTAT, "statistics counter table");

enum {
    FRAME_MAX     = 16384,
    TX_MAX        = 65536,
    CU_BUDGET     = 256,                 /* CBs per call; see the header comment */
    DUMP_BYTES    = 596,                 /* SDM 6.4.2.7 */
};

enum EePhase { EE_START, EE_OP, EE_ADDR, EE_RDATA, EE_WDATA, EE_DONE };
enum EePend  { EP_NONE, EP_WRITE, EP_ERASE, EP_ERAL, EP_WRAL };

/* PCI configuration header offsets, command bits and the power management
 * capability block (SDM 4.1). */
enum {
    PCI_COMMAND = 0x04, PCI_STATUS = 0x06, PCI_CLS = 0x0C, PCI_LATENCY = 0x0D,
    PCI_BAR0 = 0x10, PCI_SUBVENDOR = 0x2C, PCI_SUBDEVICE = 0x2E, PCI_ROM = 0x30,
    PCI_CAP_PTR = 0x34, PCI_INTLINE = 0x3C, PCI_INTPIN = 0x3D,
    PCI_MIN_GNT = 0x3E, PCI_MAX_LAT = 0x3F,
    PM_CAP = 0xDC,
};
enum { CMD_IO = 0x01, CMD_MEM = 0x02, CMD_BME = 0x04 };
enum { STATUS_MASTER_ABORT = 0x20 };     /* Status bit 13, in byte 7 */
enum { NBAR = 6 };

/* Transient work buffers: assembled frames and the received frame in flight.
 * Never hold state across a call, so they are shared by every instance. */
static struct {
    uint8_t txdata[TX_MAX + 64];         /* what the transmit DMA fetched */
    uint8_t txwire[TX_MAX + 64];         /* the frame as it leaves the MAC */
    uint8_t rxwire[FRAME_MAX];           /* a frame polled from the wire */
    uint8_t rxpost[FRAME_MAX + 8];       /* a frame as posted to the RFD */
} scratch;

/* ---- helpers -------------------------------------------------------------- */

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool dma_ok(const E100 *n)
{
    /* PCI COMMAND bit 2: no bus master cycles without it (DS 8.1.2). */
    return n->pci[PCI_COMMAND] & CMD_BME;
}

/* SCB CUS (SDM Table 13): 11b while the HPQ runs, else the low priority
 * queue's state. */
static unsigned cu_status(const E100 *n)
{
    return n->hpqs == CU_ACTIVE ? 3u : n->cus;
}

static bool dma_check(E100 *n, const char *what)
{
    if (dma_ok(n)) { return true; }
    unimpl("bme", "%s needs bus mastering but PCI COMMAND bit 2 is clear; "
           "the work waits until it is set", what);
    return false;
}

/* A bus-master cycle nobody claims ends in a master abort: the master reads
 * all ones, loses its writes and sets Status bit 13 (PCI 2.2 3.3.3.1). */
static void master_abort(E100 *n, const char *what, uint32_t a)
{
    n->pci[PCI_STATUS + 1] |= STATUS_MASTER_ABORT;
    unimpl("dma-abort", "bus-master %s at 0x%08x was master-aborted", what, a);
}

static void dma_rd(E100 *n, uint32_t a, void *dst, uint32_t len)
{
    if (!n->wiring.dma_read(n->wiring.opaque, a, dst, len)) {
        memset(dst, 0xFF, len);
        master_abort(n, "read", a);
    }
}

static void dma_wr(E100 *n, uint32_t a, const void *src, uint32_t len)
{
    if (!n->wiring.dma_write(n->wiring.opaque, a, src, len)) { master_abort(n, "write", a); }
}

static uint8_t rd8(E100 *n, uint32_t a)
{
    uint8_t b;
    dma_rd(n, a, &b, 1);
    return b;
}

static uint16_t rd16(E100 *n, uint32_t a)
{
    uint8_t b[2];
    dma_rd(n, a, b, 2);
    return get_le16(b);
}

static uint32_t rd32(E100 *n, uint32_t a)
{
    uint8_t b[4];
    dma_rd(n, a, b, 4);
    return get_le32(b);
}

static void wr16(E100 *n, uint32_t a, uint16_t v)
{
    uint8_t b[2];
    put_le16(b, v);
    dma_wr(n, a, b, 2);
}

static void wr32(E100 *n, uint32_t a, uint32_t v)
{
    uint8_t b[4];
    put_le32(b, v);
    dma_wr(n, a, b, 4);
}

/* Ethernet FCS: CRC-32 over the frame, transmitted complemented, least
 * significant byte first. */
static uint32_t fcs32(const uint8_t *p, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) { crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); }
    }
    return ~crc;
}

/* Multicast hash (SDM 6.4.2.4): the address is run through the transmit CRC
 * register -- bits enter least significant first, as on the wire, into the
 * x^31-first shift register with the 802.3 polynomial -- and bits 2..7 of the
 * register select one of 64 hash bits. Hash bit k is bit k%8 of hash
 * register k/8 (the order the Dump command reports them, SDM Table 50). */
static unsigned mc_hash(const uint8_t *a)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < 6; i++) {
        uint8_t b = a[i];
        for (int k = 0; k < 8; k++) {
            const uint32_t carry = (crc >> 31) ^ (b & 1u);
            crc <<= 1;
            b >>= 1;
            if (carry) { crc ^= 0x04C11DB7u; }
        }
    }
    return (crc >> 2) & 0x3Fu;
}

static bool hash_hit(const E100 *n, const uint8_t *a)
{
    const unsigned h = mc_hash(a);
    return (n->hash[h >> 3] >> (h & 7)) & 1u;
}

static unsigned rate_mbps(const E100 *n) { return n->spd100 ? 100 : 10; }

/* The wiring clock as device time: ticks of TIME_HZ since the last call. The
 * remainder is carried, so no precision is lost whatever clock_hz is. A clock
 * that went backwards (a restored machine) just resynchronises. */
static uint64_t time_advance(E100 *n)
{
    const uint64_t now = n->wiring.clock(n->wiring.opaque);
    const uint64_t hz = n->wiring.clock_hz;
    if (now <= n->clk_last || !hz) {
        n->clk_last = now;
        return 0;
    }
    const uint64_t d = now - n->clk_last;
    n->clk_last = now;
    const uint64_t acc = (d % hz) * TIME_HZ + n->clk_rem;
    n->clk_rem = acc % hz;
    return d / hz * TIME_HZ + acc / hz;
}

/* ---- interrupts (SDM 6.3.2.1-6.3.2.2, 6.5.2, 8.4) ------------------------- */

static void update_irq(E100 *n)
{
    uint8_t masked = 0;
    if (n->ictl & IC_CX_MASK)  { masked |= ST_CX; }
    if (n->ictl & IC_FR_MASK)  { masked |= ST_FR; }
    if (n->ictl & IC_CNA_MASK) { masked |= ST_CNA; }
    if (n->ictl & IC_RNR_MASK) { masked |= ST_RNR; }
    if (n->ictl & IC_ER_MASK)  { masked |= ST_ER; }
    if (n->ictl & IC_FCP_MASK) { masked |= ST_FCP; }
    /* M overrides everything, including SWI and MDI, which have no specific
     * mask. Events stay latched in STAT/ACK either way. */
    const bool lvl = !(n->ictl & IC_M) && (n->stat & (uint8_t)~masked) != 0;
    if (lvl != n->irq) {
        n->irq = lvl;
        n->wiring.irq(n->wiring.opaque, lvl);
    }
}

/* The CU left the active state (SDM Table 55, 6.4.2.5.4). CI mode only
 * interrupts on entering idle; a non-zero CNA interrupt delay from the last
 * TxCB defers the bit (CID x 8 us, SDM 6.4.2.5.4 item 5) while the CU status
 * itself is posted at once. */
static void cna_event(E100 *n, bool to_idle)
{
    const bool ci = n->cfgb[6] & 0x08;
    if (ci && !to_idle) { return; }
    if (n->cid) {
        n->cna_pending = true;
        n->cna_left = (uint64_t)n->cid * 8u * TIME_HZ / 1000000u;
        if (n->cna_left == 0) { n->cna_left = 1; }
    } else {
        n->stat |= ST_CNA;
    }
}

static void cna_cancel(E100 *n) { n->cna_pending = false; }

/* ---- power management events (SDM appendix A, DS 9.1.11) ---------------- */

static void wake_event(E100 *n, bool enabled, const char *what)
{
    if (!enabled) { return; }
    n->pme_st = true;
    n->pmdr |= 0x01;                     /* PMDR bit 24 mirrors PMCSR PME status */
    if (n->pme_en) {
        unimpl("pme-wire", "PME# asserted (%s) but the PME# pin is not wired to "
               "the platform", what);
    }
}

/* ---- PHY (SDM ch. 7, DS ch. 10) ------------------------------------------ */

static void phy_link_drop(E100 *n)
{
    if (n->link) {
        n->link = false;
        n->link_ll = true;               /* BMSR link status latches low */
        n->pmdr |= 0x80;                 /* PMDR bit 31: link status change */
        wake_event(n, n->cfgb[9] & 0x20, "link status change");
    }
    n->an_done = false;
    n->aner_pr = false;
    n->phy_wait = false;
}

static void phy_start_link(E100 *n, bool from_up)
{
    n->phy_wait = false;
    if (n->phy_lp || (n->bmcr & BMCR_PDOWN)) { return; }
    n->phy_wait = true;
    n->phy_left = ((n->bmcr & BMCR_ANEN) ? AN_TICKS : FORCED_TICKS) +
                  (from_up ? BREAK_TICKS : 0);
}

static void phy_restart(E100 *n)
{
    const bool was_up = n->link;
    phy_link_drop(n);
    phy_start_link(n, was_up);
}

static void phy_link_establish(E100 *n)
{
    if (n->bmcr & BMCR_ANEN) {
        /* Highest common denominator, SDM 7.4.1 Table 65. */
        const uint16_t common = n->anar & LP_WORD & 0x03E0u;
        if (common & 0x0100)      { n->spd100 = true;  n->fdx = true;  }
        else if (common & 0x0200) { n->spd100 = true;  n->fdx = false; }
        else if (common & 0x0080) { n->spd100 = true;  n->fdx = false; }
        else if (common & 0x0040) { n->spd100 = false; n->fdx = true;  }
        else if (common & 0x0020) { n->spd100 = false; n->fdx = false; }
        else {
            /* No common technology: negotiate again. */
            phy_start_link(n, false);
            return;
        }
        n->an_done = true;
        n->aner_pr = true;
    } else {
        /* Forced mode: the partner parallel-detects (SDM 7.4.2). */
        n->spd100 = (n->bmcr & BMCR_SPEED100) != 0;
        n->fdx = (n->bmcr & BMCR_FDX) != 0;
    }
    n->link = true;
    n->pmdr |= 0x80;
    wake_event(n, n->cfgb[9] & 0x20, "link status change");
}

static void phy_reset(E100 *n)
{
    n->bmcr = BMCR_DEFAULT;
    n->anar = ANAR_DEFAULT;
    n->r16w = 0;
    n->r17 = 0;
    n->r26 = 0;
    n->r27 = 0;
    phy_restart(n);                      /* "Resetting the PHY also re-starts
                                          * Auto-Negotiation", SDM 7.2.1 */
}

/* Deep power-down: entering D2/D3 with PME disabled turns the PHY off unless
 * EEPROM word 0Ah bit 6 (DDPD) disables it (SDM A.2, DS Table 1). */
static void phy_power_eval(E100 *n)
{
    const bool off = n->pstate >= 2 && !n->pme_en && !(n->ee_id & 0x0040);
    if (off && !n->phy_lp) {
        n->phy_lp = true;
        phy_link_drop(n);
    } else if (!off && n->phy_lp) {
        n->phy_lp = false;
        phy_start_link(n, false);
    }
}

static void phy_timer(E100 *n, uint64_t t)
{
    if (n->phy_wait) {
        if (t >= n->phy_left) { n->phy_wait = false; phy_link_establish(n); }
        else                  { n->phy_left -= t; }
    }
}

static uint16_t phy_read(E100 *n, unsigned reg)
{
    switch (reg) {
    case 0:
        return n->bmcr;                  /* reset completes at once: bit 15 reads 0 */
    case 1: {
        uint16_t v = BMSR_FIXED;
        if (n->an_done && (n->bmcr & BMCR_ANEN)) { v |= BMSR_ANDONE; }
        if (n->link && !n->link_ll) { v |= BMSR_LINK; }
        n->link_ll = false;              /* latch-low clears on read (DS 10.1.2 "LL") */
        return v;
    }
    case 2: return PHY_ID1;
    case 3: return PHY_ID2;
    case 4: return n->anar;
    case 5: return n->an_done ? LP_WORD : 0;
    case 6: {
        uint16_t v = (n->an_done ? 0x0001 : 0) | (n->aner_pr ? 0x0002 : 0);
        n->aner_pr = false;              /* Page Received: LH, self-clears on read */
        return v;
    }
    case 16: {
        /* DS 10.3.1. Bits 10/9 are the 100BASE-TX and 10BASE-T units' power
         * state (default 1); the unit carrying the link is the powered one. */
        uint16_t v = n->r16w & 0xF000u;
        const bool up100 = n->link && n->spd100, up10 = n->link && !n->spd100;
        if (up100)  { v |= 0x0800; }     /* descrambler in sync */
        if (!up100) { v |= 0x0400; }
        if (!up10)  { v |= 0x0200; }
        if (n->spd100) { v |= 0x0002; }
        if (n->fdx)    { v |= 0x0001; }
        return v;
    }
    case 17: return n->r17;
    case 18: return PHY_ADDR;            /* DS 10.3.3 */
    case 19: case 20: case 21: case 22: case 23: case 24: case 25:
        return 0;                        /* error counters: the model's line has no errors */
    case 26: return n->r26;
    case 27: return n->r27;
    default:
        unimpl("phy-rsvd", "MDI read of reserved PHY register %u returns 0", reg);
        return 0;
    }
}

static void phy_write(E100 *n, unsigned reg, uint16_t v)
{
    switch (reg) {
    case 0: {
        if (v & BMCR_RESET) {
            /* Settings written with Reset are not accepted (SDM 7.2.1). */
            phy_reset(n);
            return;
        }
        const uint16_t old = n->bmcr;
        /* Bit 10 is reserved on the 82559 (DS 10.1.1); SDM 7.2.1 calls it
         * Isolate. Stored, no effect. */
        n->bmcr = v & (uint16_t)~(BMCR_RESET | BMCR_ANRESTART);
        const bool pd_on = (v & BMCR_PDOWN) && !(old & BMCR_PDOWN);
        const bool pd_off = !(v & BMCR_PDOWN) && (old & BMCR_PDOWN);
        const bool an_change = (v ^ old) & BMCR_ANEN;
        const bool forced_change = !(v & BMCR_ANEN) &&
                                   ((v ^ old) & (BMCR_SPEED100 | BMCR_FDX));
        if (pd_on) {
            phy_link_drop(n);
        } else if (pd_off || an_change || forced_change ||
                   ((v & BMCR_ANRESTART) && (v & BMCR_ANEN))) {
            phy_restart(n);
        }
        return;
    }
    case 4:
        /* Bits 13:5 RW, selector read-only 00001b (DS 10.1.5). Takes effect at
         * the next negotiation (SDM 7.2.4). */
        n->anar = (uint16_t)((v & 0x3FE0u) | 0x0001u);
        return;
    case 16:
        n->r16w = v & 0xF000u;
        return;
    case 17:
        n->r17 = v;
        if (v) {
            unimpl("phy-test", "PHY register 17 (special control) = 0x%04x: scrambler/"
                   "4B5B bypass, forced patterns and link-integrity overrides have no "
                   "effect on the modelled line", v);
        }
        return;
    case 26:
        n->r26 = v;
        if (v) {
            unimpl("phy-test", "PHY register 26 (equalizer control) command 0x%04x "
                   "not modelled", v);
        }
        return;
    case 27:
        n->r27 = v;                      /* LED switch control: no LEDs */
        return;
    case 1: case 2: case 3: case 5: case 6: case 18:
    case 19: case 20: case 21: case 22: case 23: case 24: case 25:
        return;                          /* read-only: the write is ignored */
    default:
        unimpl("phy-rsvd", "MDI write of 0x%04x to reserved PHY register %u ignored", v, reg);
        return;
    }
}

/* MDI control register cycle (SDM 6.3.5). The frame takes ~26 us on the real
 * MDC; it completes in the access that issues it. A PHY address with no PHY
 * leaves MDIO floating high: reads return FFFFh (the probe both Intel drivers
 * rely on, Linux e100_phy_init and UNDI PhyDetect). */
static void mdi_cmd(E100 *n, uint32_t v)
{
    const unsigned op = (v >> 26) & 3u, phy = (v >> 21) & 31u, reg = (v >> 16) & 31u;
    uint32_t r = v & 0x3FFFFFFFu;
    if (op == 1) {
        if (phy == PHY_ADDR) { phy_write(n, reg, (uint16_t)v); }
    } else if (op == 2) {
        const uint16_t d = (phy == PHY_ADDR) ? phy_read(n, reg) : 0xFFFF;
        r = (r & 0xFFFF0000u) | d;
    } else {
        unimpl("mdi-opcode", "MDI cycle with reserved opcode %u (SDM Table 26)", op);
    }
    n->mdi = r | MDI_READY;
    if (v & MDI_IE) { n->stat |= ST_MDI; }
}

/* ---- serial EEPROM (SDM 6.3.4, AP 1.1) -----------------------------------
 * A Microwire part: with CS high, a start bit, two opcode bits and 6 or 8
 * address bits are clocked in on SK rising edges; for a read the part drives
 * the dummy 0 on the edge that takes the last address bit, then one data bit
 * per edge, MSB first. Zeros before the start bit are ignored (Linux
 * e100_eeprom_read shifts 32 bits with leading zeros). EEDO floats high
 * whenever the part is not driving it -- the size probe of SDM 6.3.4.2
 * depends on it reading 1 until the dummy zero. A read that keeps clocking
 * past 16 bits continues into the next word (the 93C46/93C66 sequential
 * read; the SDM does not name the part). Writes and erases program on the
 * falling edge of CS and finish at once, so the part reads ready. */
static void ee_addr_done(E100 *n)
{
    const unsigned a = n->ee_addr % n->ee_words;
    switch (n->ee_op) {
    case 2:
        n->ee_phase = EE_RDATA;
        n->ee_out = n->ee[a];
        n->ee_addr = a;
        n->ee_n = 0;
        n->ee_do = false;                /* dummy zero */
        break;
    case 1:
        n->ee_phase = EE_WDATA;
        n->ee_pend = EP_WRITE;
        n->ee_addr = a;
        n->ee_n = 0;
        n->ee_sh = 0;
        break;
    case 3:
        n->ee_phase = EE_DONE;
        n->ee_pend = EP_ERASE;
        n->ee_addr = a;
        break;
    default: {
        const unsigned sel = (n->ee_addr >> (n->ee_abits - 2)) & 3u;
        if (sel == 3)      { n->ee_wen = true;  n->ee_phase = EE_DONE; }
        else if (sel == 0) { n->ee_wen = false; n->ee_phase = EE_DONE; }
        else if (sel == 2) { n->ee_pend = EP_ERAL; n->ee_phase = EE_DONE; }
        else { n->ee_pend = EP_WRAL; n->ee_phase = EE_WDATA; n->ee_n = 0; n->ee_sh = 0; }
        break;
    }
    }
}

static void ee_clock(E100 *n, bool di)
{
    switch (n->ee_phase) {
    case EE_START:
        if (di) { n->ee_phase = EE_OP; n->ee_n = 0; n->ee_sh = 0; }
        break;
    case EE_OP:
        n->ee_sh = (n->ee_sh << 1) | di;
        if (++n->ee_n == 2) {
            n->ee_op = n->ee_sh & 3u;
            n->ee_phase = EE_ADDR; n->ee_n = 0; n->ee_sh = 0;
        }
        break;
    case EE_ADDR:
        n->ee_sh = (n->ee_sh << 1) | di;
        if (++n->ee_n == n->ee_abits) { n->ee_addr = n->ee_sh; ee_addr_done(n); }
        break;
    case EE_RDATA:
        n->ee_do = (n->ee_out >> 15) & 1u;
        n->ee_out = (uint16_t)(n->ee_out << 1);
        if (++n->ee_n == 16) {
            n->ee_addr = (n->ee_addr + 1) % n->ee_words;
            n->ee_out = n->ee[n->ee_addr];
            n->ee_n = 0;
        }
        break;
    case EE_WDATA:
        n->ee_sh = (n->ee_sh << 1) | di;
        if (++n->ee_n == 16) { n->ee_wdata = (uint16_t)n->ee_sh; n->ee_phase = EE_DONE; }
        break;
    default:
        break;
    }
}

static void ee_deselect(E100 *n)
{
    /* A program cycle only takes effect when it was fully clocked in and the
     * part is write-enabled (EWEN); otherwise it is ignored. */
    if (n->ee_phase == EE_DONE && n->ee_pend != EP_NONE && n->ee_wen) {
        switch (n->ee_pend) {
        case EP_WRITE: n->ee[n->ee_addr] = n->ee_wdata; break;
        case EP_ERASE: n->ee[n->ee_addr] = 0xFFFF; break;
        case EP_ERAL:
            for (unsigned i = 0; i < n->ee_words; i++) { n->ee[i] = 0xFFFF; }
            break;
        case EP_WRAL:
            for (unsigned i = 0; i < n->ee_words; i++) { n->ee[i] = n->ee_wdata; }
            break;
        default: break;
        }
    }
    n->ee_phase = EE_START;
    n->ee_pend = EP_NONE;
    n->ee_do = true;
}

static void ee_write(E100 *n, uint8_t b)
{
    const bool sk = b & 1u, cs = b & 2u, di = b & 4u;
    const bool old_sk = n->ee_reg & 1u, old_cs = n->ee_reg & 2u;
    n->ee_reg = b & 7u;
    if (!cs) {
        if (old_cs) { ee_deselect(n); }
        return;
    }
    if (!old_cs) { n->ee_phase = EE_START; n->ee_pend = EP_NONE; n->ee_do = true; }
    if (sk && !old_sk) { ee_clock(n, di); }
}

/* ---- statistics and dumps ------------------------------------------------- */

/* Dump (and reset) statistical counters, SDM 6.3.2.4. The counter set follows
 * byte 6 bits 5 and 2 (SDM Table 45). In 82559 mode the 19 counters are
 * followed by the two 16-bit TCO counters (bytes 76/78, SDM Table 17) and the
 * completion dword at byte 80 -- where Intel's Linux driver looks for it
 * (e100.c struct stats / e100_update_stats); Table 45's "24 Dwords" counts
 * the three reserved dwords after it, which the device does not write. A Dump
 * before Load Dump Counters Address (SDM 6.3.2.2 requires one) is ignored. */
static void dump_stats(E100 *n, bool reset)
{
    if (!n->dump_set) { return; }
    if (!dma_check(n, "statistics dump")) { return; }
    const bool tco = n->cfgb[6] & 0x04, std = n->cfgb[6] & 0x20;
    unsigned cnt = 19;
    uint32_t done = 76;
    if (!tco && std)       { cnt = 16; done = 64; }
    else if (!tco && !std) { cnt = 19; done = 76; }
    else {
        if (!std) {
            unimpl("stat-mode", "TCO statistics with extended counters is Reserved "
                   "(SDM Table 45); dumping the 82559 layout");
        }
        done = 80;
    }
    const uint32_t a = n->dump_addr;
    for (unsigned i = 0; i < cnt; i++) { wr32(n, a + 4 * i, n->sc[i]); }
    if (done == 80) {
        wr16(n, a + 76, n->sc_tco_tx);
        wr16(n, a + 78, n->sc_tco_rx);
    }
    wr32(n, a + done, reset ? 0xA007u : 0xA005u);
    if (reset) {
        memset(n->sc, 0, sizeof n->sc);
        n->sc_tco_tx = n->sc_tco_rx = 0;
    }
}

/* The 596-byte register dump of the Dump command and PORT Dump (SDM Tables
 * 50/51). The configuration bytes, individual address, hash registers,
 * statistical counters and SCB registers are the real ones; the FEXT, DMA
 * and micromachine internals have no counterpart in the model and are
 * written as zero. */
static void write_dump(E100 *n, uint32_t a)
{
    uint8_t b[DUMP_BYTES];
    memset(b, 0, sizeof b);
    memcpy(b + 24, n->cfgb + 8, 14);     /* configure bytes 8..21 */
    memcpy(b + 39, n->ia, 6);
    put_le16(b + 45, n->last_tx_status);
    put_le32(b + 47, n->last_tx_fcs);    /* FCS bytes in wire order */
    put_le32(b + 51, n->last_rx_fcs);
    put_le16(b + 61, n->last_rx_status);
    memcpy(b + 63, n->hash, 8);
    b[74] = 0xFC;                        /* "1 1 1 1 1 1 X X" */
    b[75] = (uint8_t)(n->last_rx_len >> 8);
    b[76] = (uint8_t)n->last_rx_len;
    for (unsigned k = 0; k < 16; k++) { put_le32(b + 4 * (38 + k), n->sc[15 - k]); }
    put_le32(b + 4 * 66, n->port_last);
    put_le32(b + 4 * 67, n->gp);
    const uint32_t scb = (uint32_t)(cu_status(n) << 6 | n->rus << 2) | (uint32_t)n->stat << 8 |
                         (uint32_t)n->ictl << 24;
    put_le32(b + 4 * 68, scb);
    put_le32(b + 4 * 69, scb);
    dma_wr(n, a, b, sizeof b);
    unimpl("dump-internals", "register dump: FEXT, DMA and micromachine registers "
           "(SDM Tables 50/51) are written as zero");
}

/* ---- resets (SDM Table 8, DS 4.1.1) --------------------------------------- */

static void rxq_flush(E100 *n)
{
    n->rxq_n = 0;
    n->rxq_bytes = 0;
    n->rx_overrun_counted = false;
}

/* Drop the oldest frame. The FIFO's bytes are packed from offset 0; what the
 * queued frames occupy is their accounted bytes less the 4-byte FCS each. */
static void rxq_pop(E100 *n)
{
    const uint32_t len = n->rxq[0].len;
    n->rxq_bytes -= len + 4u;
    n->rxq_n--;
    memmove(n->rxq, n->rxq + 1, n->rxq_n * sizeof n->rxq[0]);
    memmove(n->rxfifo, n->rxfifo + len, n->rxq_bytes - 4u * n->rxq_n);
}

/* Selective reset: CU and RU idle, pointers and micromachine state reset;
 * bases, HDS, counters, configuration and addresses kept (SDM 6.3.3.3). */
static void selective_reset(E100 *n)
{
    n->cus = CU_IDLE;
    n->hpqs = CU_IDLE;
    n->rus = RU_IDLE;
    n->cu_resume_req = false;
    cna_cancel(n);
    rxq_flush(n);
}

/* Software reset: everything but PCI configuration, the PHY and the
 * ALTRST#-only PMDR (SDM 6.3.3.1, DS 4.1.1, DS 9.1.11). The interrupt mask
 * comes out clear (SDM 8.1.1), and the MDI register reads Ready -- Linux
 * e100 mdio_ctrl_hw waits for Ready before its first MDI cycle after a
 * reset. The individual address returns to the one autoloaded from the
 * EEPROM, which the 82559 uses until an IA Setup (AP 2.2, DS 4.7). */
static void soft_reset(E100 *n)
{
    selective_reset(n);
    n->stat = 0;
    n->ictl = 0;
    n->gp = 0;
    n->port_sh = 0;
    n->mdi_sh = 0;
    n->mdi = MDI_READY;
    n->flash_ctl = 0;
    n->eri = 0;
    n->fc_thr = 0;
    n->xoff_req = false;
    n->rxbc = 0;
    n->cu_base = n->ru_base = 0;
    n->cu_cb = n->cu_prev = n->cu_next = 0;
    n->cid = 0;
    n->dump_addr = 0;
    n->dump_set = false;
    n->hds = 0;
    memcpy(n->cfgb, k_cfg_default, sizeof n->cfgb);
    memcpy(n->ia, n->ee_ia, 6);
    memset(n->hash, 0, sizeof n->hash);
    memset(n->sc, 0, sizeof n->sc);
    n->sc_tco_tx = n->sc_tco_rx = 0;
    memset(n->ucode, 0, sizeof n->ucode);
    memset(n->lpf, 0, sizeof n->lpf);
    n->pause_left = 0;
    n->paused = n->paused_low = n->fc_full = n->fc_restop_armed = false;
    n->fc_restop_left = 0;
    n->ee_reg = 0;
    n->ee_phase = EE_START;
    n->ee_pend = EP_NONE;
    n->ee_do = true;
    update_irq(n);
}

/* ---- transmit (SDM 6.4.2.5) ----------------------------------------------- */

static bool link_path_open(const E100 *n)
{
    /* SDM 6.4.2.3: byte 8 bit 0 must be 1 and CSMA enabled (bit 7 clear) for
     * the 82559 to put anything on, or take anything off, the link. */
    if (!(n->cfgb[8] & 0x01) || (n->cfgb[8] & 0x80)) { return false; }
    return n->link && !n->phy_lp && !(n->bmcr & BMCR_PDOWN);
}

static unsigned loopback_mode(const E100 *n)
{
    const unsigned lb = (n->cfgb[10] >> 6) & 3u;
    if (lb == 2) {
        unimpl("loopback-rsvd", "configure byte 10 loopback = 10b is Reserved (SDM "
               "6.4.2.3.1); treated as no loopback");
        return 0;
    }
    return lb;
}

static bool in_loopback(const E100 *n)
{
    return loopback_mode(n) != 0 || (n->bmcr & BMCR_LOOP);
}

static void rx_wire(E100 *n, const uint8_t *f, uint32_t len);

/* One frame leaves the MAC. `f` is the frame without FCS. */
static void mac_send(E100 *n, const uint8_t *f, uint32_t len, uint32_t fcs)
{
    n->last_tx_fcs = fcs;
    if (in_loopback(n)) {
        /* Internal (MAC) loopback, external loopback and PHY loopback all
         * return the frame to the receive side instead of the cable; the
         * configuration bit takes priority over the MDI one (DS 10.1.1). */
        n->sc[SC_TX_GOOD]++;
        rx_wire(n, f, len);
        return;
    }
    /* Link interface not configured: the frame is dropped. */
    if (!(n->cfgb[8] & 0x01) || (n->cfgb[8] & 0x80)) { return; }
    if (!link_path_open(n)) {
        /* No carrier: the frame goes out unheard and the MAC sees no CRS. */
        n->sc[SC_TX_LOSTCRS]++;
        return;
    }
    if (n->wiring.tx) { n->wiring.tx(n->wiring.opaque, f, len); }
    n->sc[SC_TX_GOOD]++;
}

/* Frame assembly by the transmit byte machine (SDM 6.4.2.5.3). `d` holds what
 * the transmit DMA fetched. Returns false for a DMA underrun. */
static bool tx_frame(E100 *n, const uint8_t *d, uint32_t len, bool nc)
{
    uint8_t *f = scratch.txwire;
    const bool nsai = n->cfgb[10] & 0x08;
    uint32_t fl;
    if (!nc && !nsai) {
        /* SA inserted from the IA after the destination address. */
        if (len < 6) { return false; }
        memcpy(f, d, 6);
        memcpy(f + 6, n->ia, 6);
        memcpy(f + 12, d + 6, len - 6);
        fl = len + 6;
    } else {
        /* SA from memory: fewer than the two addresses forces an underrun. */
        if (len < 12) { return false; }
        memcpy(f, d, len);
        fl = len;
    }
    uint32_t fcs;
    if (nc) {
        /* CRC supplied by software as the last four bytes (SDM Figure 19, NC). */
        if (fl < 16) { return false; }
        fl -= 4;
        fcs = get_le32(f + fl);
    } else {
        if ((n->cfgb[18] & 0x02) && fl < 60) {
            /* Pad to 64 bytes with the CRC. SDM 6.4.2.5.3 gives 7Eh, the
             * 82557's flag byte; Intel's Linux driver documents that "82557
             * pads with 7Eh, while the later controllers pad with 00h"
             * (e100.c, Theory of Operation). */
            memset(f + fl, 0x00, 60 - fl);
            fl = 60;
        }
        fcs = fcs32(f, fl);
    }
    mac_send(n, f, fl, fcs);
    return true;
}

enum { CB_DONE = 0, CB_STALL = 1 };

static bool tx_paused(const E100 *n)
{
    /* SDM 6.6.2.3.1: a non-zero pause count inhibits further frames when
     * transmit flow control is enabled (byte 19 bit 2 clear). A pause low
     * frame stops only the LPQ; the HPQ keeps transmitting (6.6.3.1). */
    if ((n->cfgb[19] & 0x04) || n->pause_left == 0) { return false; }
    return !(n->in_hpq && n->paused_low);
}

static int cb_transmit(E100 *n, uint32_t a, uint32_t w0, uint16_t *st)
{
    if (tx_paused(n)) { return CB_STALL; }
    const bool ext = !(n->cfgb[6] & 0x10);        /* byte 6 bit 4 = 0: extended TxCB */
    const bool flex = (w0 & CB_SF) || ext;         /* an extended TxCB is flexible */
    const bool nc = w0 & CB_NC;
    const bool dyn = n->cfgb[7] & 0x80;
    const uint32_t tbd_arr = rd32(n, a + 8);
    const uint32_t w3 = rd32(n, a + 12);
    const uint32_t tcb_cnt = w3 & 0x3FFFu;
    const unsigned ntbd = w3 >> 24;
    uint32_t len = 0;
    bool trunc = false;

    if (tcb_cnt) {
        /* Data immediately after the TCB, from byte 10h (SDM 6.4.2.5). The
         * extended TCB is 8 Dwords, its two TBDs at 10h-1Fh, read in one
         * burst so that "both the TCB and its immediate data field" are
         * cache line aligned: the data follows the 8 Dwords, at 20h. (The
         * Whistler e100 driver sends whole frames this way, TBD number 0.) */
        const uint32_t src = a + (ext ? 0x20u : 0x10u);
        dma_rd(n, src, scratch.txdata, tcb_cnt);
        len = tcb_cnt;
    }
    /* A simplified TxCB's TBD array address should be FFFFFFFFh; it is unused. */
    if (flex && ntbd != 0) {
        /* TBD k: the extended TCB holds the first two at 10h/18h and the array
         * pointer addresses the third (SDM 6.4.2.5). EL ends the frame in
         * dynamic TBD mode, and on the extended TCB's own two TBDs; otherwise
         * the TBD number does (SDM 6.4.2.3.1 byte 7, 6.4.2.5.1). */
        const uint32_t arr = n->cu_base + tbd_arr;
        for (unsigned k = 0;; k++) {
            if (!(dyn && ntbd == 0xFF) && k >= ntbd) { break; }
            if (k >= 256) {
                unimpl("dyn-tbd-limit", "dynamic TBD list longer than 256 entries "
                       "without EL: frame ended there");
                break;
            }
            const uint32_t ta = (ext && k < 2) ? a + 0x10 + 8 * k
                                               : arr + 8 * (ext ? k - 2 : k);
            uint8_t tbd[8];
            dma_rd(n, ta, tbd, sizeof tbd);
            const uint32_t ba = get_le32(tbd);
            const uint32_t sz = get_le32(tbd + 4);
            if (dyn && ba == 0) {
                /* Not-valid pointer: re-read the TBD later (SDM 6.4.2.5.1). */
                return CB_STALL;
            }
            uint32_t bl = sz & 0x3FFFu;
            if (len + bl > TX_MAX) { bl = TX_MAX - len; trunc = true; }
            dma_rd(n, ba, scratch.txdata + len, bl);
            len += bl;
            if ((sz & 0x10000u) && (dyn || (ext && k < 2))) { break; }
        }
    }
    if (trunc) {
        unimpl("tx-big", "transmit frame longer than %u bytes truncated", (unsigned)TX_MAX);
    }
    n->cid = (uint8_t)((w0 >> 24) & 0x1Fu);
    const bool ok = tx_frame(n, scratch.txdata, len, nc);
    *st = CBS_OK;
    if (!ok) {
        /* Underrun forced by a frame too short for its addresses (SDM
         * 6.4.2.5.3 step 5); the C/OK of a TxCB still reflect the DMA. */
        n->sc[SC_TX_UNDERRUN]++;
        *st |= CBS_U;
    }
    n->last_tx_status = (uint16_t)(*st | CBS_C);
    return CB_DONE;
}

/* ---- action commands (SDM 6.4.2) ------------------------------------------ */

static int exec_cb(E100 *n, uint32_t a, uint32_t w0, uint16_t *st)
{
    const unsigned cmd = (w0 >> 16) & 7u;
    *st = CBS_OK;
    switch (cmd) {
    case 0:
        if (w0 & (1u << 19)) {
            /* 82559 Load Programmable Filter, CB opcode 0008 0000h (SDM A.7.1). */
            for (unsigned i = 0; i < 16; i++) { n->lpf[i] = rd32(n, a + 8 + 4 * i); }
            unimpl("lpf", "Load Programmable Filter stored; wake-up packet filters "
                   "(interesting packets, PMDR bit 29) are not evaluated");
        }
        return CB_DONE;                  /* else NOP */
    case 1:
        dma_rd(n, a + 8, n->ia, 6);
        return CB_DONE;
    case 2: {
        unsigned cnt = rd8(n, a + 8) & 0x3Fu;
        if (cnt < 8)  { cnt = 8; }       /* the minimum (SDM 6.4.2.3) */
        if (cnt > 22) { cnt = 22; }
        dma_rd(n, a + 8, n->cfgb, cnt);
        if (n->cfgb[15] & 0x20) {
            unimpl("crc16", "configure byte 15 bit 5: 16-bit CRC engine (SDM says must be "
                   "0); frames still carry the 32-bit FCS");
        }
        (void)loopback_mode(n);          /* reports the reserved loopback code */
        return CB_DONE;
    }
    case 3: {
        uint32_t cnt = rd16(n, a + 8) & 0x3FFFu;
        cnt -= cnt % 6;                  /* "reduces ... to the nearest multiple of 6" */
        memset(n->hash, 0, sizeof n->hash);
        for (uint32_t o = 0; o < cnt; o += 6) {
            uint8_t m[6];
            dma_rd(n, a + 10 + o, m, 6);
            const unsigned h = mc_hash(m);
            n->hash[h >> 3] |= (uint8_t)(1u << (h & 7));
        }
        return CB_DONE;
    }
    case 4:
        return cb_transmit(n, a, w0, st);
    case 5:
        for (unsigned i = 0; i < 64; i++) { n->ucode[i] = rd32(n, a + 8 + 4 * i); }
        unimpl("ucode", "Load Microcode accepted but the patch is not executed (its "
               "format is not documented, SDM 6.4.2.6); CPUSaver interrupt bundling "
               "does not happen");
        return CB_DONE;
    case 6:
        write_dump(n, n->cu_base + rd32(n, a + 8));
        return CB_DONE;
    default:
        /* Diagnose: the backoff self-test passes, F = 0 (SDM 6.4.2.8). */
        return CB_DONE;
    }
}

/* Run the Command Block List (SDM 6.4.1.1, 6.5.3, Table 55). */
static void cu_run(E100 *n, unsigned budget)
{
    /* The LPQ waits while the HPQ has CBs pending. */
    while (n->cus == CU_ACTIVE && n->hpqs != CU_ACTIVE && budget--) {
        if (!dma_check(n, "the Command Unit")) { return; }
        const uint32_t a = n->cu_cb;
        uint8_t hdr[8];
        dma_rd(n, a, hdr, sizeof hdr);
        const uint32_t w0 = get_le32(hdr), link = get_le32(hdr + 4);
        uint16_t st = 0;
        if (exec_cb(n, a, w0, &st) == CB_STALL) { return; }
        wr16(n, a, (uint16_t)(st | CBS_C));
        if (w0 & CB_I) { n->stat |= ST_CX; }
        n->cu_prev = a;
        n->cu_next = n->cu_base + link;
        if (w0 & CB_EL) {
            n->cus = CU_IDLE;
            n->cu_resume_req = false;
            cna_event(n, true);
            return;
        }
        if (w0 & CB_S) {
            /* A resume that arrived while this CB ran re-checks its S bit
             * (SDM 6.5.3.2 item 2). */
            if (n->cu_resume_req && !(rd32(n, a) & CB_S)) {
                n->cu_resume_req = false;
                n->cu_cb = n->cu_next;
                continue;
            }
            n->cu_resume_req = false;
            n->cus = CU_SUSP;
            cna_event(n, false);
            return;
        }
        n->cu_resume_req = false;
        n->cu_cb = n->cu_next;
    }
}

/* The High Priority Queue: CU HPQ Start puts the first CB of a second CB
 * list in the SCB General Pointer, relative to the CU base, and HPQ Resume
 * continues it (SDM Table 15); the CUS field reads 11b, "HQP Active", while
 * it runs (Table 13). Its CBs execute exactly as the low priority list's,
 * ahead of it. The list ends at a CB with S set: the 82559 does not take EL
 * as the end of an HPQ ("Software should only use the Start (S) bit to
 * identify the end of a Command Block List (CBL) in a HPQ", 82559 stepping
 * information, erratum "Unsupported End of List Bit in High Priority
 * Queues"), so an EL alone is passed over to the link. */
static void hpq_run(E100 *n, unsigned budget)
{
    while (n->hpqs == CU_ACTIVE && budget--) {
        if (!dma_check(n, "the Command Unit (HPQ)")) { return; }
        const uint32_t a = n->hpq_cb;
        uint8_t hdr[8];
        dma_rd(n, a, hdr, sizeof hdr);
        const uint32_t w0 = get_le32(hdr), link = get_le32(hdr + 4);
        uint16_t st = 0;
        n->in_hpq = true;
        const int r = exec_cb(n, a, w0, &st);
        n->in_hpq = false;
        if (r == CB_STALL) { return; }
        wr16(n, a, (uint16_t)(st | CBS_C));
        if (w0 & CB_I) { n->stat |= ST_CX; }
        n->hpq_next = n->cu_base + link;
        if (w0 & CB_S) {
            n->hpqs = CU_SUSP;
            cna_event(n, false);
            return;
        }
        n->hpq_cb = n->hpq_next;
    }
}

/* ---- receive (SDM 6.4.3) -------------------------------------------------- */

static void ru_setup(E100 *n)
{
    /* RX DMA byte count starts at the next buffer's size, bits 10:3 (SDM 6.3.6). */
    if (n->rus == RU_READY && dma_ok(n)) {
        n->rxbc = (rd32(n, n->ru_rfd + 12) >> 16) & 0x7F8u;
    }
}

enum { RX_TAKEN, RX_HOLD };

/* Close the oldest queued frame into the current RFD (SDM 6.4.3.3). */
static int rx_deliver(E100 *n)
{
    const uint8_t *frame = n->rxfifo;
    const uint32_t flen = n->rxq[0].len;
    uint16_t st = n->rxq[0].st;
    const uint32_t a = n->ru_rfd;
    uint8_t hdr[16];
    dma_rd(n, a, hdr, sizeof hdr);
    const uint32_t w0 = get_le32(hdr);
    const uint32_t link = get_le32(hdr + 4);
    const uint32_t size = (get_le32(hdr + 12) >> 16) & 0x3FFFu;
    if (w0 & RFD_SF) {
        unimpl("rfd-flex", "flexible-mode RFD (SF=1): only the simplified memory "
               "model is supported (SDM 6.0 note); treated as simplified");
    }
    if ((w0 & RFD_H) && n->hds) {
        unimpl("rfd-header", "header RFD (H=1 after Load HDS) treated as a plain RFD");
    }
    if (size == 0) {
        /* No data space at all: the RU runs out of resources without touching
         * the descriptor -- Intel's Linux driver parks the RU on a size-0 RFD
         * with EL because "hardware will not write to a size 0 descriptor"
         * (e100.c, Theory of Operation IV). The frame stays in the FIFO. */
        n->rus = RU_NORES;
        n->stat |= ST_RNR;
        return RX_HOLD;
    }

    uint32_t plen = flen;
    const bool crc_xfer = n->cfgb[18] & 0x04;
    if ((n->cfgb[18] & 0x01) && !crc_xfer && plen >= 14) {
        /* Stripping: drop what follows a Length field's payload (byte 18 bit 0). */
        const unsigned tl = (unsigned)frame[12] << 8 | frame[13];
        if (tl > 0 && tl <= 1500 && 14 + tl < plen) { plen = 14 + tl; }
    }
    uint8_t *post = scratch.rxpost;
    memcpy(post, frame, plen);
    uint32_t total = plen;
    const uint32_t fcs = fcs32(frame, flen);
    if (crc_xfer) { put_le32(post + total, fcs); total += 4; }
    if (n->cfgb[9] & 0x01) {
        /* 82559 TCP/UDP checksum word (SDM 6.4.3.4.1): 16-bit little-endian
         * words after the 14-byte MAC header, end-around carry, odd byte
         * padded with a zero high byte; appended LSB first after any CRC. */
        uint32_t sum = 0;
        for (uint32_t i = 14; i < plen; i += 2) {
            sum += (uint32_t)post[i] | (i + 1 < plen ? (uint32_t)post[i + 1] << 8 : 0);
            sum = (sum & 0xFFFFu) + (sum >> 16);
        }
        put_le16(post + total, (uint16_t)sum);
        total += 2;
    }
    uint32_t count = total;
    if (total > size) { st |= RS_NOSPACE; count = size; }
    const bool ok = !(st & (RS_NOSPACE | RS_SHORT | RS_OVERRUN | RS_CRC | RS_ALIGN | RS_RXERR));
    if (!ok && !(n->cfgb[6] & 0x80)) {
        /* Bad frame, Save Bad Frames off: the RFD is reclaimed (SDM 6.4.3.3.2 step 3). */
        return RX_TAKEN;
    }
    dma_wr(n, a + 16, post, count);
    wr16(n, a + 12, (uint16_t)(count | 0xC000u));   /* EOF | F | actual count */
    wr16(n, a, (uint16_t)(CBS_C | (ok ? CBS_OK : 0) | st));
    if (ok) { n->sc[SC_RX_GOOD]++; }
    n->last_rx_status = (uint16_t)(CBS_C | (ok ? CBS_OK : 0) | st);
    n->last_rx_len = (uint16_t)flen;
    n->last_rx_fcs = fcs;
    n->stat |= ST_FR;
    if (n->cna_pending) {
        /* End of receive raises a delayed CNA with FR (SDM 6.4.2.5.4 item 2). */
        n->cna_pending = false;
        n->stat |= ST_CNA;
    }
    n->ru_rfd = n->ru_base + link;
    if (st & RS_NOSPACE) {
        /* Ran out of buffer space in this frame (SDM 6.4.3.3.3 step 1). */
        n->rus = RU_NORES;
        n->stat |= ST_RNR;
    } else if (w0 & RFD_EL) {
        n->rus = RU_NORES;
        n->stat |= ST_RNR;
    } else if (w0 & RFD_S) {
        n->rus = RU_SUSP;
        n->stat |= ST_RNR;
    }
    ru_setup(n);
    return RX_TAKEN;
}

static void fc_eval(E100 *n);

static void rx_drain(E100 *n)
{
    bool moved = false;
    while (n->rxq_n && n->rus == RU_READY) {
        if (!dma_check(n, "the Receive Unit")) { break; }
        if (rx_deliver(n) == RX_HOLD) { break; }
        rxq_pop(n);
        moved = true;
    }
    if (moved) { fc_eval(n); }
}

/* ---- flow control (SDM 6.6) ----------------------------------------------- */

static uint64_t slots_to_ticks(const E100 *n, uint64_t slots)
{
    /* A slot is 512 bit times. */
    return slots * 512u * TIME_HZ / ((uint64_t)rate_mbps(n) * 1000000u);
}

static void fc_send_pause(E100 *n, uint16_t quanta)
{
    uint8_t f[60];
    memset(f, 0, sizeof f);
    const uint8_t da[6] = { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x01 };
    memcpy(f, da, 6);
    memcpy(f + 6, n->ia, 6);
    f[12] = 0x88; f[13] = 0x08;          /* MAC control */
    f[14] = 0x00; f[15] = 0x01;          /* PAUSE */
    f[16] = (uint8_t)(quanta >> 8);
    f[17] = (uint8_t)quanta;
    n->sc[SC_FC_TX_PAUSE]++;
    mac_send(n, f, sizeof f, fcs32(f, sizeof f));
}

/* Receive flow control (SDM 6.6.2.3.2): with ReStop or ReStart configured
 * (byte 19 bits 3/4), a receive FIFO with fewer free bytes than the threshold
 * register selects (SDM Table 30), or the Xoff request, sends PAUSE(FC
 * delay); the state lasts until the FIFO is empty. ReStart then sends
 * PAUSE(0); ReStop repeats the PAUSE each time a counter of the FC delay with
 * its low five bits ignored runs out (continuously when that is zero). */
static void fc_eval(E100 *n)
{
    static const uint16_t thr[8] = { 512, 1024, 1280, 1536, 1792, 2048, 2304, 2560 };
    const bool restop = n->cfgb[19] & 0x08, restart = n->cfgb[19] & 0x10;
    const uint16_t delay = (uint16_t)(n->cfgb[17] << 8 | n->cfgb[16]);
    const bool congested = n->xoff_req ||
                           (E100_RX_FIFO_BYTES - n->rxq_bytes) < thr[n->fc_thr & 7];
    if (!(restop || restart)) { n->fc_full = false; return; }
    if (!n->fc_full) {
        if (congested) {
            n->fc_full = true;
            fc_send_pause(n, delay);
            n->fc_restop_armed = restop;
            n->fc_restop_left = slots_to_ticks(n, delay & ~0x1Fu);
        }
        return;
    }
    if (!congested && n->rxq_bytes == 0) {
        n->fc_full = false;
        n->fc_restop_armed = false;
        if (restart) { fc_send_pause(n, 0); }
    }
}

static void fc_timer(E100 *n, uint64_t t)
{
    if (n->pause_left) {
        if (t >= n->pause_left) {
            n->pause_left = 0;
            n->paused = n->paused_low = false;
        } else {
            n->pause_left -= t;
        }
    }
    if (n->fc_full && n->fc_restop_armed) {
        if (t >= n->fc_restop_left) {
            const uint16_t delay = (uint16_t)(n->cfgb[17] << 8 | n->cfgb[16]);
            fc_send_pause(n, delay);
            n->fc_restop_left = slots_to_ticks(n, delay & ~0x1Fu);
        } else {
            n->fc_restop_left -= t;
        }
    }
}

/* A received MAC control frame (SDM 6.6.2.1-6.6.3). */
static void fc_rx(E100 *n, const uint8_t *f, uint32_t len)
{
    const unsigned op = (unsigned)f[14] << 8 | f[15];
    if (op != 1) { n->sc[SC_FC_RX_UNSUP]++; return; }
    n->sc[SC_FC_RX_PAUSE]++;
    if (n->cfgb[19] & 0x04) { return; }  /* transmit flow control disabled */
    const unsigned quanta = (unsigned)f[16] << 8 | f[17];
    bool low = false;
    const unsigned pthr = (n->cfgb[18] >> 4) & 7u;
    if (pthr != 7) {
        /* Priority byte #19 or #31 (1-based, byte 20 bit 5), SDM 6.6.3.2. */
        const uint32_t pos = (n->cfgb[20] & 0x20) ? 30 : 18;
        if (pos < len) { low = (f[pos] & 7u) > pthr; }
    }
    if (quanta == 0) {
        n->pause_left = 0;
        n->paused = n->paused_low = false;
    } else {
        n->pause_left = slots_to_ticks(n, quanta);
        if (!n->pause_left) { n->pause_left = 1; }
        n->paused = !low;
        n->paused_low = low;
    }
    n->stat |= ST_FCP;
}

/* ---- the MAC receive path (SDM 6.4.3.3, 6.4.2.3.1, DS 4.8) ---------------- */

static bool is_magic(const E100 *n, const uint8_t *f, uint32_t len)
{
    /* Magic Packet: six FFh then the station address sixteen times, anywhere
     * in the frame (the AMD Magic Packet definition the SDM refers to). */
    for (uint32_t i = 0; i + 6 + 96 <= len; i++) {
        uint32_t k = 0;
        while (k < 6 && f[i + k] == 0xFF) { k++; }
        if (k < 6) { continue; }
        bool all = true;
        for (unsigned r = 0; r < 16 && all; r++) {
            if (memcmp(f + i + 6 + 6 * r, n->ia, 6) != 0) { all = false; }
        }
        if (all) { return true; }
    }
    return false;
}

static void rx_wire(E100 *n, const uint8_t *f, uint32_t len)
{
    if (len < 6) { return; }             /* "at least 6 bytes long", SDM 6.4.3.3 */
    if (len > FRAME_MAX) { len = FRAME_MAX; }
    const bool promisc = n->cfgb[15] & 0x01;
    uint8_t ia_m[6];
    memcpy(ia_m, f, 6);
    if (n->cfgb[15] & 0x10) { ia_m[0] = (uint8_t)((ia_m[0] & ~0x02u) | (n->ia[0] & 0x02u)); }
    const bool ia = memcmp(ia_m, n->ia, 6) == 0;
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t fcmc[6] = { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x01 };
    bool hit = false;
    if (!(f[0] & 1u)) {
        hit = ia || ((n->cfgb[20] & 0x40) && hash_hit(n, f));   /* multiple IA */
    } else if (!memcmp(f, bcast, 6)) {
        hit = !(n->cfgb[15] & 0x02);
    } else {
        hit = (n->cfgb[21] & 0x08) || hash_hit(n, f);
    }
    uint16_t st = 0;
    if (!ia)  { st |= RS_NOTIA; }
    if (!hit) { st |= RS_NOMATCH; }
    bool accept = hit || promisc;

    if (len >= 14) {
        const unsigned tl = (unsigned)f[12] << 8 | f[13];
        if (tl == 0 || tl > 1500) { st |= RS_TYPE; }
        if (tl == 0x8808 && len >= 18) {
            const bool special = !memcmp(f, fcmc, 6);
            if (ia || special) { fc_rx(n, f, len); }
            if (special && !ia) {
                /* Reject FC keeps frames to the special address out of
                 * memory; frames to the IA pass regardless (SDM 6.6.2.3.1). */
                accept = !(n->cfgb[19] & 0x20);
            }
        }
    }
    if (!accept) { return; }
    if (is_magic(n, f, len)) {
        n->pmdr |= 0x40;                 /* PMDR bit 30, regardless of enables */
        wake_event(n, !(n->cfgb[19] & 0x02), "Magic Packet");
    }
    if (len < 60) {
        /* Shorter than 64 bytes with the CRC: counted even when discarded. */
        st |= RS_SHORT;
        n->sc[SC_RX_SHORT]++;
        if (n->cfgb[7] & 0x01) { return; }
    }
    /* Long Receive OK clear: frames over 1514 bytes are discarded (DS 4.8.4). */
    if (len > 1514 && !(n->cfgb[18] & 0x08)) { return; }
    if (n->rus == RU_IDLE) { return; }
    const uint32_t cost = len + 4;
    if (n->rxq_n == E100_RXQ_MAX || n->rxq_bytes + cost > E100_RX_FIFO_BYTES) {
        /* FIFO full: the next frame is an overrun, the ones after are simply
         * lost (SDM 6.4.3.4, Table 17 byte 52). */
        if (!n->rx_overrun_counted) {
            n->sc[SC_RX_OVERRUN]++;
            n->rx_overrun_counted = true;
        }
        return;
    }
    memcpy(n->rxfifo + (n->rxq_bytes - 4u * n->rxq_n), f, len);
    n->rxq[n->rxq_n].len = (uint16_t)len;
    n->rxq[n->rxq_n].st = st;
    n->rxq_n++;
    n->rxq_bytes += cost;
    n->rx_overrun_counted = false;
    rx_drain(n);
    fc_eval(n);
}

/* Frames from the cable, at the rate the link carries them: preamble/SFD (8),
 * FCS (4) and inter-frame gap (12) add to each frame's wire time. */
static void rx_poll(E100 *n, uint64_t t)
{
    if (!n->wiring.rx) { return; }
    const bool open = link_path_open(n) && !in_loopback(n);
    if (open) {
        const int64_t rate = (int64_t)rate_mbps(n) * 1000000;
        const int64_t cap = (int64_t)(TIME_HZ / 100) * rate;     /* 10 ms of wire time */
        const uint64_t tt = t > (uint64_t)TIME_HZ ? (uint64_t)TIME_HZ : t;
        n->rx_credit += (int64_t)tt * rate;
        if (n->rx_credit > cap) { n->rx_credit = cap; }
    } else {
        n->rx_credit = 0;
    }
    for (;;) {
        if (open && n->rx_credit <= 0) { break; }
        size_t len = n->wiring.rx(n->wiring.opaque, scratch.rxwire, FRAME_MAX);
        if (!len) { break; }
        if (len > FRAME_MAX) { len = FRAME_MAX; }
        if (!open) { continue; }         /* link or MAC not receiving: dropped */
        n->rx_credit -= (int64_t)(len + 24) * 8 * TIME_HZ;
        rx_wire(n, scratch.rxwire, (uint32_t)len);
    }
}

/* ---- SCB commands (SDM 6.3.2.2, 6.5) -------------------------------------- */

static void cu_command(E100 *n, unsigned cuc)
{
    switch (cuc) {
    case 1:                              /* CU Start */
        if (n->cus == CU_ACTIVE) { return; }   /* prohibited, SDM 6.5.3.1 */
        cna_cancel(n);
        n->cu_cb = n->cu_base + n->gp;
        n->cus = CU_ACTIVE;
        n->cu_resume_req = false;
        cu_run(n, CU_BUDGET);
        return;
    case 2:                              /* CU Resume */
        if (n->cus == CU_IDLE) { return; }
        cna_cancel(n);
        if (n->cus == CU_ACTIVE) { n->cu_resume_req = true; return; }
        if (rd32(n, n->cu_prev) & CB_S) {
            /* The S bit of the CB that suspended is re-read (SDM 6.5.3.2). */
            return;
        }
        n->cus = CU_ACTIVE;
        n->cu_cb = n->cu_next;
        cu_run(n, CU_BUDGET);
        return;
    case 0xA:                            /* CU Static Resume: no S re-check */
        if (n->cus == CU_IDLE) { return; }
        cna_cancel(n);
        if (n->cus == CU_ACTIVE) { n->cu_resume_req = true; return; }
        n->cus = CU_ACTIVE;
        n->cu_cb = n->cu_next;
        cu_run(n, CU_BUDGET);
        return;
    case 4:
        n->dump_addr = n->gp;            /* absolute (SDM Table 9) */
        n->dump_set = true;
        return;
    case 5:
    case 7:
        dump_stats(n, cuc == 7);
        return;
    case 6:                              /* Load CU Base; prohibited unless idle, SDM 6.2.1 */
        n->cu_base = n->gp;
        return;
    case 3:                              /* CU HPQ Start */
        n->hpq_cb = n->cu_base + n->gp;
        n->hpqs = CU_ACTIVE;
        hpq_run(n, CU_BUDGET);
        return;
    case 0xB:                            /* CU HPQ Resume */
        if (n->hpqs != CU_SUSP) { return; }
        n->hpq_cb = n->hpq_next;
        n->hpqs = CU_ACTIVE;
        hpq_run(n, CU_BUDGET);
        return;
    default:
        unimpl("cuc", "CU command %u is not defined (SDM Table 14); ignored", cuc);
        return;
    }
}

static void ru_command(E100 *n, unsigned ruc)
{
    switch (ruc) {
    case 1:                              /* RU Start; prohibited while Ready, SDM Table 56 */
        n->ru_rfd = n->ru_base + n->gp;
        n->rus = RU_READY;
        ru_setup(n);
        rx_drain(n);
        return;
    case 2:                              /* RU Resume: only from Suspended (SDM Table 56) */
        if (n->rus == RU_SUSP) {
            n->rus = RU_READY;
            ru_setup(n);
            rx_drain(n);
        }
        return;
    case 4:                              /* RU Abort */
        if (n->rus == RU_READY) { n->stat |= ST_RNR; }
        n->rus = RU_IDLE;
        rxq_flush(n);
        return;
    case 5:
        n->hds = (uint16_t)(n->gp & 0x3FFFu);
        if (n->hds) {
            unimpl("hds", "Load HDS %u: header RFDs are not supported in the "
                   "simplified memory model (SDM 6.0 note); RFDs stay plain", n->hds);
        }
        return;
    case 6:                              /* Load RU Base; prohibited unless idle, SDM 6.2.1 */
        n->ru_base = n->gp;
        return;
    case 3:
        unimpl("ruc-redirect", "RU command 3 (Receive DMA Redirect) not supported; ignored");
        return;
    default:
        unimpl("ruc-rbd", "RU command 7 (RBD Resume) not supported; ignored");
        return;
    }
}

static void scb_command(E100 *n, uint8_t c)
{
    const unsigned cuc = c >> 4, ruc = c & 7u;
    if (c & 0x08) { unimpl("scb-bit19", "SCB command bit 19 (reserved) written as 1"); }
    /* A CU and an RU command in one write is forbidden (SDM 6.5.1); the CU
     * command goes first. */
    if (cuc) { cu_command(n, cuc); }
    if (ruc) { ru_command(n, ruc); }
}

/* ---- PORT (SDM 6.3.3) ----------------------------------------------------- */

/* Not the silicon's ROM signature -- that is an LFSR over internal ROM no
 * document gives. Any non-zero value passes Intel's drivers (Linux
 * e100_self_test only checks it is non-zero). */
enum { SELFTEST_SIGNATURE = 0x55AA6559u };

static void port_cmd(E100 *n, uint32_t v)
{
    n->port_last = v;
    const uint32_t p = v & ~0xFu;
    switch (v & 0xFu) {
    case 0:
        soft_reset(n);
        return;
    case 1:
        selective_reset(n);
        if (dma_check(n, "PORT self-test")) {
            wr32(n, p, SELFTEST_SIGNATURE);
            wr32(n, p + 4, 0);           /* all pass, SDM Figure 11 */
            unimpl("selftest-sig", "PORT self-test reports a synthetic ROM signature "
                   "0x%08x (the silicon's value is undocumented)", SELFTEST_SIGNATURE);
        }
        soft_reset(n);                   /* "executes a full internal reset" */
        return;
    case 2:
        selective_reset(n);
        return;
    case 3:
        if (dma_check(n, "PORT dump")) {
            write_dump(n, p);
            wr32(n, p + DUMP_BYTES, 0xA006u);   /* dword 149 */
        }
        return;
    case 7:
        /* No wake-up packet is stored (flexible filters are not evaluated):
         * byte count 0, then the complete status (SDM 6.3.3.5, A.7.3). */
        if (dma_check(n, "PORT dump wake-up")) {
            wr32(n, p + 4, 0);
            wr32(n, p, 0xA000u);
        }
        selective_reset(n);
        return;
    default:
        unimpl("port-op", "PORT function %u is not defined (SDM Table 19); ignored", v & 0xFu);
        return;
    }
}

/* ---- CSR space (SDM 6.3.1 Table 11, DS 9.1) ------------------------------- */

static uint8_t gsts(const E100 *n)
{
    return (uint8_t)((n->link ? 1 : 0) | (n->spd100 ? 2 : 0) | (n->fdx ? 4 : 0));
}

static uint8_t csr_rd8(E100 *n, uint32_t o)
{
    switch (o) {
    case 0x00: return (uint8_t)(cu_status(n) << 6 | n->rus << 2);
    case 0x01: return n->stat;
    case 0x02: return 0;                 /* accepted at once (SDM 6.5.1) */
    case 0x03: return n->ictl;           /* SI reads 0 */
    case 0x04: case 0x05: case 0x06: case 0x07: return (uint8_t)(n->gp >> (8 * (o - 4)));
    case 0x08: case 0x09: case 0x0A: case 0x0B: return 0;   /* PORT done (UNDI SelectiveReset polls for 0) */
    case 0x0C: return (uint8_t)n->flash_ctl;
    case 0x0D: return (uint8_t)(n->flash_ctl >> 8);
    case 0x0E: return (uint8_t)((n->ee_reg & 7u) | (n->ee_do ? 0x08 : 0));
    case 0x0F: return 0;
    case 0x10: case 0x11: case 0x12: case 0x13: return (uint8_t)(n->mdi >> (8 * (o - 0x10)));
    case 0x14: case 0x15: case 0x16: case 0x17: return (uint8_t)(n->rxbc >> (8 * (o - 0x14)));
    case 0x18: return n->eri;
    case 0x19: return n->fc_thr & 7u;
    case 0x1A:
        /* Flow control register bits 23:16 (SDM 6.3.8): Xon reads 0. */
        return (uint8_t)((n->xoff_req ? 0x02 : 0) | (n->fc_full ? 0x04 : 0) |
                         (n->paused ? 0x08 : 0) | (n->paused_low ? 0x10 : 0));
    case 0x1B: return (uint8_t)(n->pmdr | (n->pme_st ? 0x01 : 0));
    case 0x1C: return n->gctl;
    case 0x1D: return gsts(n);
    default:
        if (o >= 0x30 && o < 0x40) {
            unimpl("csr-cardbus", "CardBus status-change register read at CSR 0x%02x "
                   "(forbidden in PCI mode, DS 9.1.14) returns 0", o);
        } else if (o >= 0x40) {
            unimpl("csr-high", "CSR read beyond the registers at 0x%03x returns 0", o);
        } else {
            unimpl("csr-rsvd", "reserved CSR read at 0x%02x returns 0", o);
        }
        return 0;
    }
}

static void csr_write(E100 *n, uint32_t off, unsigned len, uint64_t v)
{
    bool have_cmd = false, have_ictl = false, port = false, mdi = false;
    uint8_t cmd = 0, ictl = 0;
    for (unsigned i = 0; i < len; i++) {
        const uint32_t o = off + i;
        const uint8_t b = (uint8_t)(v >> (8 * i));
        switch (o) {
        case 0x00: break;                /* CUS/RUS are read-only */
        case 0x01: n->stat &= (uint8_t)~b; break;      /* acknowledge, SDM 6.5.2 */
        case 0x02: cmd = b; have_cmd = true; break;
        case 0x03: ictl = b; have_ictl = true; break;
        case 0x04: case 0x05: case 0x06: case 0x07: {
            const unsigned s = 8 * (o - 4);
            n->gp = (n->gp & ~(0xFFu << s)) | (uint32_t)b << s;
            break;
        }
        case 0x08: case 0x09: case 0x0A: case 0x0B: {
            const unsigned s = 8 * (o - 8);
            n->port_sh = (n->port_sh & ~(0xFFu << s)) | (uint32_t)b << s;
            if (o == 0x0B) { port = true; }   /* acted on with the high byte */
            break;
        }
        case 0x0C: case 0x0D: {
            const unsigned s = 8 * (o - 0x0C);
            n->flash_ctl = (uint16_t)((n->flash_ctl & ~(0xFFu << s)) | (unsigned)b << s);
            unimpl("flash-ctl", "flash control register written (0x%04x): its bits are "
                   "not documented (DS 9.1.5); stored only", n->flash_ctl);
            break;
        }
        case 0x0E: ee_write(n, b); break;
        case 0x0F: break;
        case 0x10: case 0x11: case 0x12: case 0x13: {
            const unsigned s = 8 * (o - 0x10);
            n->mdi_sh = (n->mdi_sh & ~(0xFFu << s)) | (uint32_t)b << s;
            if (o == 0x13) { mdi = true; }    /* latched with the high byte */
            break;
        }
        case 0x14: case 0x15: case 0x16: case 0x17: case 0x1D:
            unimpl("csr-ro-write", "write to read-only CSR 0x%02x ignored", o);
            break;
        case 0x18:
            n->eri = b;
            if (b) {
                unimpl("eri", "early receive interrupt count %u: ER interrupts are not "
                       "generated (SDM 6.3.7)", b);
            }
            break;
        case 0x19: n->fc_thr = b & 7u; break;
        case 0x1A:
            if (b & 0x02) { n->xoff_req = true; }
            if (b & 0x01) { n->xoff_req = false; }
            fc_eval(n);
            break;
        case 0x1B:
            n->pmdr &= (uint8_t)~(b & 0xE0u);   /* bits 31:29 write-1-to-clear */
            if (b & 0x01) { n->pme_st = false; n->pmdr &= (uint8_t)~0x01u; }
            break;
        case 0x1C:
            n->gctl = b & 3u;
            if (b & 2u) {
                unimpl("gctl-dpd", "general control: deep power-down on link down "
                       "enabled; no effect");
            }
            break;
        default:
            if (o >= 0x30 && o < 0x40) {
                unimpl("csr-cardbus", "CardBus status-change register write at CSR 0x%02x "
                       "(forbidden in PCI mode, DS 9.1.14) ignored", o);
            } else if (o >= 0x40) {
                unimpl("csr-high", "CSR write beyond the registers at 0x%03x ignored", o);
            } else {
                unimpl("csr-rsvd", "reserved CSR write at 0x%02x ignored", o);
            }
            break;
        }
    }
    if (have_ictl) {
        n->ictl = ictl & (uint8_t)~IC_SI;
        if (ictl & IC_SI) { n->stat |= ST_SWI; }   /* latched; M gates the pin (SDM 8.4) */
    }
    if (have_cmd && cmd) { scb_command(n, cmd); }
    if (port) { port_cmd(n, n->port_sh); }
    if (mdi)  { mdi_cmd(n, n->mdi_sh); }
}

/* The CSRs behave as bytes: every access is split into byte accesses, so the
 * EEPROM Control Register (the 16-bit word at 0Eh, which both the PXE ROM and
 * the firmware's UNDI driver bit-bang with byte and word accesses) and the
 * dword registers are handled the same way whatever the access size. */
static void csr_access(E100 *n, bool write, uint32_t off, unsigned len, uint64_t *val)
{
    e100_poll(n);
    if (write) {
        csr_write(n, off, len, *val);
    } else {
        uint64_t v = 0;
        for (unsigned i = 0; i < len; i++) { v |= (uint64_t)csr_rd8(n, off + i) << (8 * i); }
        *val = v;
    }
    update_irq(n);
}

/* ---- PCI configuration space (SDM 4.1) ------------------------------------- */

static uint8_t pm_data(const E100 *n)
{
    /* SDM Table 6 / DS Table 10 (the 82559 rev 08 column): mW / 10. */
    static const uint8_t d[9] = { 58, 40, 40, 40, 58, 40, 40, 40, 0 };
    return n->dsel <= 8 ? d[n->dsel] : 0;
}

static uint8_t cfg_rd8(const E100 *n, unsigned reg)
{
    if (reg < sizeof n->pci) { return n->pci[reg]; }
    switch (reg) {
    case 0xDC: return 0x01;              /* capability ID: PCI PM (SDM 4.1.18.1) */
    case 0xDD: return 0x00;              /* last item */
    case 0xDE: return 0x21;              /* PMC 7E21h, no auxiliary power (DS 8.1.20) */
    case 0xDF: return 0x7E;
    case 0xE0: return n->pstate & 3u;
    case 0xE1: return (uint8_t)((n->pme_en ? 0x01 : 0) | (n->dsel << 1) |
                                ((n->dsel <= 8 ? 2u : 0u) << 5) | (n->pme_st ? 0x80 : 0));
    case 0xE2: return pm_data(n);        /* Data at E2h (SDM 4.1.18.5, DS Figure 21) */
    default:   return 0;
    }
}

/* The BARs (SDM 4.1.10): the size is found by writing all ones and reading
 * back the mask with the type bits restored; any other value is an address
 * with the bits below the size not settable. BAR3-5 are not implemented and
 * read as zero. */
static uint32_t bar_size(unsigned idx)
{
    switch (idx) {
    case 0: return E100_CSR_MEM_SIZE;
    case 1: return E100_CSR_IO_SIZE;
    case 2: return E100_FLASH_BAR_SIZE;
    default: return 0;
    }
}

static void bar_write(E100 *n, unsigned idx, uint32_t val)
{
    const uint32_t size = bar_size(idx);
    if (!size) { return; }
    uint8_t *p = n->pci + PCI_BAR0 + 4 * idx;
    const uint32_t type = get_le32(p) & (idx == 1 ? 0x3u : 0xFu);   /* I/O flag, or memory type */
    put_le32(p, (val & ~(size - 1u)) | type);
}

/* Expansion ROM base (DS 8.1.9.4): bit 0 enables the decoder, bits 10:1 are
 * reserved, the rest is the base address, settable only above the window. The
 * register is read-only zero when the EEPROM disables boot. */
static void rom_write(E100 *n, uint32_t val)
{
    if (!n->rom_bar) { return; }
    put_le32(n->pci + PCI_ROM,
             (val & ~(E100_FLASH_BAR_SIZE - 1u) & 0xFFFFF800u) | (val & 1u));
}

/* Header bytes (00h-3Fh) a configuration write may change. SDM 4.1.3: Command
 * bits 8, 6, 4, 2, 1, 0 are implemented, the rest hardwired 0; 4.1.4: Status
 * 15-12 and 8 are error bits (write 1 to clear), the rest read-only. SDM
 * 4.1.7: the 82559 keeps a Cache Line Size of 8 or 16 dwords only -- "If a
 * value other than 8 or 16 is written ... the device returns all zeroes when
 * the CLS register is read". The Latency Timer and Interrupt Line are plain
 * read/write; everything else is read-only. */
static void hdr_write_byte(E100 *n, unsigned reg, uint8_t b)
{
    switch (reg) {
    case PCI_COMMAND:         n->pci[reg] = b & 0x57u; break;
    case PCI_COMMAND + 1:     n->pci[reg] = b & 0x01u; break;
    case PCI_STATUS + 1:      n->pci[reg] &= (uint8_t)~(b & 0xF1u); break;
    case PCI_CLS:             n->pci[reg] = (b == 8 || b == 16) ? b : 0; break;
    case PCI_LATENCY:
    case PCI_INTLINE:         n->pci[reg] = b; break;
    default:                  break;
    }
}

/* Power state change (SDM 4.1.18.4). */
static void set_pstate(E100 *n, uint8_t ps)
{
    const uint8_t old = n->pstate;
    if (ps == old) { return; }
    n->pstate = ps;
    if (old == 3 && ps == 0) {
        /* D3 to D0: microcode, MAC configuration, pointers, statistics and
         * the PCI configuration registers reset; not the EEPROM, the PHY
         * configuration or PME context (DS 4.1.1). */
        soft_reset(n);
        put_le16(n->pci + PCI_COMMAND, 0);
        n->pci[PCI_CLS] = 0;
        n->pci[PCI_LATENCY] = 0;
        memset(n->pci + PCI_BAR0, 0, 4 * NBAR);
        n->pci[PCI_BAR0 + 4] = 1;        /* BAR1 is an I/O BAR */
        put_le32(n->pci + PCI_ROM, 0);
    }
    phy_power_eval(n);
}

static void cfg_wr8(E100 *n, unsigned reg, uint8_t b)
{
    switch (reg) {
    case 0xE0:
        set_pstate(n, b & 3u);           /* bits 7:2 read-only */
        return;
    case 0xE1:
        /* PME_En (bit 8), Data Select (12:9), PME Status write-1-to-clear (15),
         * which also clears PMDR bit 24 (DS 8.1.21). */
        n->pme_en = b & 1u;
        n->dsel = (b >> 1) & 0xFu;
        if (b & 0x80) { n->pme_st = false; n->pmdr &= (uint8_t)~0x01u; }
        phy_power_eval(n);
        return;
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: case 0xE2: case 0xE3:
        return;                          /* read-only */
    default:
        unimpl("cfg-rsvd", "write 0x%02x to reserved configuration offset 0x%02x ignored",
               b, reg);
        return;
    }
}

uint32_t e100_cfg_read(const E100 *n, unsigned reg, unsigned len)
{
    const uint32_t all_ones = len >= 4 ? 0xFFFFFFFFu : (1u << (8 * len)) - 1;
    if (!len || len > 4 || reg + len > 256) { return all_ones; }
    uint32_t v = 0;
    for (unsigned i = 0; i < len; i++) { v |= (uint32_t)cfg_rd8(n, reg + i) << (8 * i); }
    return v;
}

/* BARs and the ROM base take byte enables like any configuration register
 * (PCI 2.3 3.2.2.3.2): the written bytes are merged into the register, then
 * its writable-bit rules apply to the whole dword. */
void e100_cfg_write(E100 *n, unsigned reg, unsigned len, uint32_t val)
{
    if (!len || len > 4 || reg + len > 256 || (reg & 3u) + len > 4) { return; }
    if ((reg >= PCI_BAR0 && reg < PCI_BAR0 + 4 * NBAR) || (reg >= PCI_ROM && reg < PCI_ROM + 4)) {
        const unsigned base = reg & ~3u;
        uint32_t cur = get_le32(n->pci + base);
        for (unsigned i = 0; i < len; i++) {
            const unsigned sh = 8u * (reg - base + i);
            cur = (cur & ~(0xFFu << sh)) | (((val >> (8 * i)) & 0xFFu) << sh);
        }
        if (base == PCI_ROM) { rom_write(n, cur); }
        else                 { bar_write(n, (base - PCI_BAR0) / 4u, cur); }
    } else if (reg < sizeof n->pci) {
        for (unsigned i = 0; i < len; i++) { hdr_write_byte(n, reg + i, (uint8_t)(val >> (8 * i))); }
    } else {
        for (unsigned i = 0; i < len; i++) { cfg_wr8(n, reg + i, (uint8_t)(val >> (8 * i))); }
    }
    update_irq(n);
}

/* The header after PCI RST#: identity from the autoloaded EEPROM word 0Ah (DS
 * 8.1.12 Table 7). With a valid signature, AltID (bit 7) clear makes a 1229h
 * part report 1029h, ID (bit 13) supplies revision bits 2:0 from bits 10:8,
 * and words 0Bh/0Ch become the subsystem IDs; without one, the defaults stand.
 * The expansion ROM BAR exists when the signature is valid and Boot Disable
 * (bit 11) is clear (AP 2.5.1). */
static void pci_poweron(E100 *n)
{
    const uint16_t wa = n->ee_id;
    const bool sig = (wa >> 14) == 1;
    uint16_t did = n->silicon_id;
    uint8_t rev = n->silicon_rev;
    uint16_t ssid = 0, ssvid = 0;
    if (sig) {
        if (!(wa & 0x0080) && did == 0x1229) { did = 0x1029; }
        if (wa & 0x2000) { rev = (uint8_t)((rev & ~7u) | ((wa >> 8) & 7u)); }
        ssid = n->ee[0x0B];
        ssvid = n->ee[0x0C];
        if (wa & 0x0001) {
            unimpl("modem", "EEPROM word 0Ah selects a LAN/modem combination; the "
                   "modem function is not modelled");
        }
    }
    n->rom_bar = sig && !(wa & 0x0800);
    uint8_t *c = n->pci;
    memset(c, 0, sizeof n->pci);
    put_le16(c, E100_VENDOR_ID);
    put_le16(c + 0x02, did);
    put_le16(c + PCI_STATUS, 0x0290);    /* fast back-to-back, capabilities list, DEVSEL medium (DS 8.1.3) */
    c[0x08] = rev;
    c[0x0B] = 0x02;                      /* class: network, Ethernet */
    put_le16(c + PCI_SUBVENDOR, ssvid);
    put_le16(c + PCI_SUBDEVICE, ssid);
    c[PCI_BAR0 + 4] = 1;                 /* BAR1 is an I/O BAR */
    c[PCI_CAP_PTR] = PM_CAP;             /* hard-wired on the 82559 (SDM 4.1.14) */
    c[PCI_INTPIN] = 0x01;                /* INTA# */
    c[PCI_MIN_GNT] = 0x08;               /* DS 8.1.16 */
    c[PCI_MAX_LAT] = 0x18;               /* DS 8.1.17 */
}

/* ---- address decode --------------------------------------------------------- */

static bool valid_len(unsigned len)
{
    return len == 1 || len == 2 || len == 4 || len == 8;
}

static uint32_t command(const E100 *n)
{
    return get_le16(n->pci + PCI_COMMAND);
}

static uint64_t mem_bar_base(const E100 *n, unsigned idx)
{
    return get_le32(n->pci + PCI_BAR0 + 4 * idx) & ~(uint32_t)0xF;
}

bool e100_rom_range(const E100 *n, uint64_t *base, uint64_t *size)
{
    const uint32_t raw = get_le32(n->pci + PCI_ROM);
    if (!(command(n) & CMD_MEM) || !n->rom_bar || !(raw & 1u) || !(raw & 0xFFFFF800u)) {
        return false;
    }
    *base = raw & 0xFFFFF800u;
    *size = E100_FLASH_BAR_SIZE;
    return true;
}

bool e100_mem_ranges(const E100 *n, uint64_t *csr_base, uint64_t *csr_size,
                     uint64_t *flash_base, uint64_t *flash_size)
{
    if (!(command(n) & CMD_MEM)) { return false; }
    *csr_base = mem_bar_base(n, 0);
    *csr_size = *csr_base ? E100_CSR_MEM_SIZE : 0;
    *flash_base = mem_bar_base(n, 2);
    *flash_size = *flash_base ? E100_FLASH_BAR_SIZE : 0;
    return true;
}

/* The 128 KiB flash is aliased through the 1 MiB window; past the image it
 * reads as erased (FFh), and no flash command set is modelled. */
static uint8_t flash_byte(const E100 *n, uint64_t off)
{
    const uint32_t o = (uint32_t)(off & (E100_FLASH_SIZE - 1u));
    return o < n->rom_len ? n->rom[o] : 0xFFu;
}

static void flash_access(const E100 *n, bool write, uint64_t off, unsigned len, uint64_t *val)
{
    if (write) {
        unimpl("flash-write", "flash write at 0x%05x (0x%llx): no flash part command set "
               "is modelled; ignored", (unsigned)off, (unsigned long long)*val);
        return;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < len; i++) { v |= (uint64_t)flash_byte(n, off + i) << (8 * i); }
    *val = v;
}

bool e100_mem_access(E100 *n, bool write, uint64_t addr, unsigned len, uint64_t *val)
{
    if (!valid_len(len) || !(command(n) & CMD_MEM)) { return false; }
    uint64_t rom_base, rom_size;
    if (e100_rom_range(n, &rom_base, &rom_size) && addr >= rom_base &&
        addr < rom_base + rom_size) {
        /* The expansion ROM is the flash, read-only. */
        if (!write) { flash_access(n, false, addr - rom_base, len, val); }
        return true;
    }
    /* Memory and I/O decode only in D0 (PCI PM). */
    if (n->pstate != 0) { return false; }
    const uint64_t csr = mem_bar_base(n, 0), flash = mem_bar_base(n, 2);
    if (csr && addr >= csr && addr < csr + E100_CSR_MEM_SIZE) {
        csr_access(n, write, (uint32_t)(addr - csr), len, val);
        return true;
    }
    if (flash && addr >= flash && addr < flash + E100_FLASH_BAR_SIZE) {
        flash_access(n, write, addr - flash, len, val);
        return true;
    }
    return false;
}

bool e100_io_access(E100 *n, bool write, uint16_t port, unsigned len, uint64_t *val)
{
    if (!valid_len(len) || !(command(n) & CMD_IO) || n->pstate != 0) { return false; }
    /* An I/O BAR's low two bits are type, not address (PCI 2.3 6.2.5.1). */
    const uint32_t io = get_le32(n->pci + PCI_BAR0 + 4) & ~3u;
    if (!io || port < io || port >= io + E100_CSR_IO_SIZE) { return false; }
    csr_access(n, write, port - io, len, val);
    return true;
}

/* ---- lifecycle -------------------------------------------------------------- */

/* PCI RST# (DS 4.7): the EEPROM autoload of words 0-2 and 0Ah (and 0Bh/0Ch
 * for the header), power state D0 and general control cleared, the
 * software-reset state and a PHY reset that restarts auto-negotiation. PMDR
 * is ALTRST#-only (DS 9.1.11): kept. */
void e100_reset(E100 *n)
{
    for (unsigned i = 0; i < 3; i++) { put_le16(n->ee_ia + 2 * i, n->ee[i]); }
    n->ee_id = n->ee[0x0A];
    pci_poweron(n);
    n->pstate = 0;
    n->gctl = 0;
    n->ee_do = true;
    soft_reset(n);
    phy_reset(n);
}

/* AP-394 Table 1. */
static void eeprom_build(E100 *n, const E100Config *c)
{
    n->ee_words = c->eeprom_words == 256 ? 256 : 64;
    n->ee_abits = n->ee_words == 256 ? 8 : 6;
    if (c->eeprom_words && c->eeprom_words != 64 && c->eeprom_words != 256) {
        unimpl("eeprom-size", "eeprom_words %u unsupported (64 or 256); using 64", c->eeprom_words);
    }
    if (c->eeprom_image) {
        memcpy(n->ee, c->eeprom_image, n->ee_words * 2u);
        return;
    }
    static const uint8_t default_mac[6] = { 0x02, 0xA0, 0xC9, 0x00, 0x00, 0x01 };
    uint8_t mac[6];
    memcpy(mac, c->mac, 6);
    if (!(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) { memcpy(mac, default_mac, 6); }
    for (unsigned i = 0; i < 3; i++) { n->ee[i] = get_le16(mac + 2 * i); }
    n->ee[3] = 0x0003;                   /* MC10, MC100 "set this bit to 1" (AP 2.3.1) */
    n->ee[5] = 0x0201;                   /* controller type 02 (82559), RJ-45 (AP 2.4.1-2) */
    n->ee[6] = 0x4701;                   /* PHY device record (AP 2.4.3) */
    /* Word 0Ah (AP 2.5.1, DS Table 1): signature 01b, ID 0, PM/AltID 1, Boot
     * Disable unless a boot ROM was asked for; standby, WOL, modem off. */
    n->ee[0x0A] = (uint16_t)(0x4000 | 0x0080 | (c->boot_rom ? 0 : 0x0800));
    n->ee[0x0B] = c->subsys_id;
    n->ee[0x0C] = c->subsys_vendor;
    uint16_t sum = 0;
    for (unsigned i = 0; i + 1 < n->ee_words; i++) { sum = (uint16_t)(sum + n->ee[i]); }
    n->ee[n->ee_words - 1] = (uint16_t)(0xBABA - sum);   /* AP 2.6.6 */
}

void e100_init(E100 *n, const E100Config *cfg, const E100Wiring *wiring)
{
    memset(n, 0, sizeof *n);
    n->wiring = *wiring;
    n->silicon_id = cfg->device_id ? cfg->device_id : E100_DEVICE_ID;
    n->silicon_rev = cfg->revision ? cfg->revision : 0x08;
    eeprom_build(n, cfg);
    n->clk_last = wiring->clock(wiring->opaque);
    e100_reset(n);
}

void e100_rebind(E100 *n, const E100Wiring *wiring)
{
    n->wiring = *wiring;
}

void e100_set_rom(E100 *n, const uint8_t *rom, uint32_t len)
{
    n->rom = rom;
    n->rom_len = len < E100_FLASH_SIZE ? len : E100_FLASH_SIZE;
}

bool e100_irq(const E100 *n) { return n->irq; }

void e100_poll(E100 *n)
{
    const uint64_t t = time_advance(n);
    phy_timer(n, t);
    fc_timer(n, t);
    if (n->cna_pending) {
        if (t >= n->cna_left) { n->cna_pending = false; n->stat |= ST_CNA; }
        else                  { n->cna_left -= t; }
    }
    rx_poll(n, t);
    hpq_run(n, CU_BUDGET);
    cu_run(n, CU_BUDGET);
    rx_drain(n);
    update_irq(n);
}
