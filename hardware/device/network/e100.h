#pragma once

/* Intel 82559 Fast Ethernet controller (8255x family, 8086:1229), the onboard
 * LAN of the Intel IA-64 Workstation SDV (i2000, revision 08) and the D101M
 * add-in card.
 *
 * "SDM" in the comments is the Intel 8255x 10/100 Mbps Ethernet Controller
 * Open Source Software Developer's Manual rev 1.3, "DS" the 82559 datasheet
 * rev 2.5 and "AP" AP-394, the 82559 EEPROM map. Where the 8255x family
 * members differ the 82559's behaviour is the one modelled.
 *
 * The device owns its PCI configuration space and its address decode: the host
 * feeds it configuration cycles, memory and I/O accesses and asks which memory
 * ranges the BARs currently cover. It is a PCI bus master: every Command
 * Block, Receive Frame Descriptor and buffer is read and written in guest
 * memory through the dma_read/dma_write callbacks. Frames go to and come from
 * the optional tx/rx callbacks; with neither the controller sees a link that
 * carries nothing.
 *
 * E100 is plain data so a machine can snapshot it as raw bytes. The host
 * pointers inside it are `wiring` (callbacks and an opaque) and `rom`; after
 * loading a raw dump call e100_rebind() and e100_set_rom() to point them at
 * the live objects again. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define E100_VENDOR_ID 0x8086u
#define E100_DEVICE_ID 0x1229u

/* BARs (DS 8.1.9): 0 is the 4 KiB CSR memory window, 1 the 64-byte CSR I/O
 * window, 2 the 1 MiB flash window, and the expansion ROM base maps the same
 * flash. The flash part is 128 KiB, aliased through the 1 MiB windows (DS
 * 4.6, SDM 4.1.10). */
#define E100_CSR_MEM_SIZE 0x1000u
#define E100_CSR_IO_SIZE  0x40u
#define E100_FLASH_BAR_SIZE 0x100000u
#define E100_FLASH_SIZE   0x20000u

/* The receive FIFO (DS 2.2) holds at most this many bytes of frames; each
 * queued frame is accounted with its 4-byte FCS. */
#define E100_RX_FIFO_BYTES 3072u
#define E100_RXQ_MAX 64u

/* Statistical counters (SDM Table 17, DS Table 23). */
#define E100_NSTAT 19

/* Callbacks. clock, irq, dma_read and dma_write are required; tx and rx are
 * optional (NULL: nothing leaves the wire, nothing arrives from it). */
typedef struct {
    /* Monotonic ticks at clock_hz: drives the PHY link and auto-negotiation
     * timers, the CNA interrupt delay, flow-control pauses and the pace at
     * which received frames enter the receive FIFO. */
    uint64_t (*clock)(void *opaque);
    uint64_t clock_hz;
    void (*irq)(void *opaque, bool level);    /* INTA# level changes */
    /* Bus-master access to guest memory; false is a master abort (reads then
     * return all ones, writes are lost, PCI Status bit 13 is set). */
    bool (*dma_read)(void *opaque, uint64_t addr, void *buf, unsigned len);
    bool (*dma_write)(void *opaque, uint64_t addr, const void *buf, unsigned len);
    /* One frame, without FCS, onto the wire. */
    bool (*tx)(void *opaque, const uint8_t *frame, size_t len);
    /* Poll one received frame (without FCS) into `frame`, at most `cap` bytes;
     * returns its length, 0 for none. */
    size_t (*rx)(void *opaque, uint8_t *frame, size_t cap);
    void *opaque;
} E100Wiring;

typedef struct {
    /* Silicon identity: what the PCI header reports when the EEPROM does not
     * override it (DS 8.1.1/8.1.4). 0 selects 1229h / 08h. */
    uint16_t device_id;
    uint8_t revision;
    /* Station address, EEPROM words 0-2 (AP 2.2). All zero selects the
     * locally administered default 02:A0:C9:00:00:01. */
    uint8_t mac[6];
    /* EEPROM words 0Ch / 0Bh (AP 2.5.3-2.5.4), loaded into PCI 2Ch/2Eh. */
    uint16_t subsys_vendor, subsys_id;
    /* EEPROM word 0Ah bit 11 (Boot Disable) clear: the Expansion ROM BAR is
     * implemented and maps the flash (DS 8.1.9.4). Off by default. */
    bool boot_rom;
    /* 64 (93C46) or 256 (93C66) words (AP 1.1); 0 selects 64. */
    uint16_t eeprom_words;
    /* Optional verbatim EEPROM image of eeprom_words words (a dump of a real
     * board's part), borrowed only during e100_init. When NULL the image is
     * built from the fields above. */
    const uint16_t *eeprom_image;
} E100Config;

/* Receive FIFO entry: frame length and the RFD status bits decided at the MAC.
 * The frames' bytes are packed in E100.rxfifo, oldest first. */
typedef struct {
    uint16_t len;
    uint16_t st;
} E100RxEntry;

typedef struct E100 {
    E100Wiring wiring;               /* HOST POINTERS: callbacks, opaque */
    const uint8_t *rom;              /* HOST POINTER, borrowed: flash image */
    uint32_t rom_len;
    uint16_t silicon_id;             /* power-on DEVICE_ID before the EEPROM's say */
    uint8_t silicon_rev;
    uint8_t pci[0x40];               /* PCI configuration header */
    bool rom_bar;                    /* expansion ROM BAR implemented (EEPROM autoload) */

    /* Time: wiring.clock converted to a fixed 1.193182 MHz device time base
     * with the remainder carried (clk_rem is in clock ticks x device Hz). */
    uint64_t clk_last, clk_rem;

    /* Serial EEPROM (SDM 6.3.4). */
    uint16_t ee[256];
    uint16_t ee_words, ee_abits;
    uint8_t ee_reg;                  /* SK/CS/DI as last written */
    bool ee_do;
    uint8_t ee_phase, ee_pend;
    uint16_t ee_n, ee_op, ee_addr;
    uint32_t ee_sh;
    uint16_t ee_out, ee_wdata;
    bool ee_wen;
    uint8_t ee_ia[6];                /* latched by the autoload */
    uint16_t ee_id;                  /* word 0Ah as autoloaded */

    /* System Control Block and CSR (SDM 6.3). */
    uint8_t stat;                    /* STAT/ACK */
    uint8_t ictl;                    /* interrupt control, SI never stored */
    uint32_t gp;
    uint32_t port_sh, port_last;
    uint32_t mdi_sh, mdi;
    uint16_t flash_ctl;
    uint8_t eri, fc_thr, pmdr, gctl;
    bool xoff_req;
    uint32_t rxbc;
    bool irq;                        /* INTA# as last reported */

    /* Command Unit (SDM 6.5.3). */
    uint8_t cus;
    uint32_t cu_base, cu_cb, cu_prev, cu_next;
    /* The High Priority Queue (82558 and later): a second CB list, run ahead
     * of the low priority one. hpqs is a CU_* state. */
    uint32_t hpq_cb, hpq_next;
    uint8_t hpqs;
    bool in_hpq;                     /* exec_cb is running an HPQ CB */
    bool cu_resume_req;
    uint8_t cid;                     /* CNA interrupt delay of the last TxCB */
    bool cna_pending;
    uint64_t cna_left;
    uint32_t dump_addr;
    bool dump_set;

    /* Receive Unit (SDM 6.5.4). */
    uint8_t rus;
    uint32_t ru_base, ru_rfd;
    uint16_t hds;
    E100RxEntry rxq[E100_RXQ_MAX];
    uint16_t rxq_n;
    uint32_t rxq_bytes;
    uint8_t rxfifo[E100_RX_FIFO_BYTES];
    bool rx_overrun_counted;
    int64_t rx_credit;               /* bits x device Hz of wire time available */

    /* MAC configuration and statistics. */
    uint8_t cfgb[22];
    uint8_t ia[6];
    uint8_t hash[8];
    uint32_t sc[E100_NSTAT];
    uint16_t sc_tco_tx, sc_tco_rx;
    uint32_t ucode[64];
    uint32_t lpf[16];

    /* Flow control (SDM 6.6). */
    uint64_t pause_left;
    bool paused, paused_low, fc_full, fc_restop_armed;
    uint64_t fc_restop_left;

    /* PHY (SDM ch. 7, DS ch. 10). */
    uint16_t bmcr, anar, r16w, r17, r26, r27;
    bool link, link_ll, an_done, aner_pr, spd100, fdx;
    bool phy_wait, phy_lp;
    uint64_t phy_left;

    /* PCI power management (SDM 4.1.18). */
    uint8_t pstate, dsel;
    bool pme_en, pme_st;

    /* Last-frame bookkeeping for the dump (SDM Table 50). */
    uint16_t last_tx_status, last_rx_status, last_rx_len;
    uint32_t last_tx_fcs, last_rx_fcs;
} E100;

/* Power-on (PCI RST# with a cold EEPROM): the EEPROM image is built from
 * `cfg`, then the autoload, PHY reset and auto-negotiation start happen as
 * in e100_reset(). Nothing is borrowed from `cfg` afterwards. */
void e100_init(E100 *n, const E100Config *cfg, const E100Wiring *wiring);
/* Re-point the host pointers after loading a raw dump; changes no state. */
void e100_rebind(E100 *n, const E100Wiring *wiring);
/* PCI RST# (DS 4.7): the configuration space is rebuilt from the EEPROM
 * autoload (words 0-2, 0Ah-0Ch), the controller returns to its software-reset
 * state, the PHY resets and restarts auto-negotiation, INTA# is released. The
 * EEPROM, PMDR (ALTRST# only, DS 9.1.11) and the borrowed flash image keep
 * their contents. */
void e100_reset(E100 *n);
/* The flash image (borrowed) behind BAR2 and the expansion ROM BAR; `len` is
 * clamped to the 128 KiB part, the rest of the window reads FFh. */
void e100_set_rom(E100 *n, const uint8_t *rom, uint32_t len);

/* Configuration cycles; `len` is 1, 2 or 4 within one dword. */
uint32_t e100_cfg_read(const E100 *n, unsigned reg, unsigned len);
void e100_cfg_write(E100 *n, unsigned reg, unsigned len, uint32_t val);

/* Address decode. Both return false when the device does not claim the access
 * (COMMAND.MEM / COMMAND.IO, the BARs as currently programmed, the PCI power
 * state and the ROM BAR's enable bit decide). `len` is 1, 2, 4 or 8. */
/* BAR0 CSRs, BAR2 flash and the enabled expansion ROM. */
bool e100_mem_access(E100 *n, bool write, uint64_t addr, unsigned len, uint64_t *val);
/* BAR1 CSR I/O. */
bool e100_io_access(E100 *n, bool write, uint16_t port, unsigned len, uint64_t *val);

/* Guest address ranges the memory BARs currently cover, so the host can keep
 * its RAM fast path clear of them. Returns false while memory decode is off
 * (COMMAND.MEM clear); otherwise a size of 0 means that BAR is not placed. */
bool e100_mem_ranges(const E100 *n, uint64_t *csr_base, uint64_t *csr_size,
                     uint64_t *flash_base, uint64_t *flash_size);
/* The same for the expansion ROM window; false unless it is enabled. */
bool e100_rom_range(const E100 *n, uint64_t *base, uint64_t *size);

/* Bring the device up to date with wiring.clock: PHY link and auto-negotiation
 * timing, delayed CNA interrupts, flow-control pause timers, receiving from
 * the rx callback at wire rate, and any Command Unit work left waiting
 * (dynamic-TBD polling, a pause, bus mastering disabled). Also done by every
 * CSR access. Cheap when idle. */
void e100_poll(E100 *n);

/* Level of the INTA# output. */
bool e100_irq(const E100 *n);
