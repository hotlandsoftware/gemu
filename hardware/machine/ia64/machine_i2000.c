#include "i2000.h"
#include "merced.h"
#include "input_menu.h"
#include "vga_ibm.h"
#include "vgafont16.h"
#include "vgabios_rom.h"
#include "rage128.h"
#include "network/i82559.h"
#include "ps2_keyboard.h"
#include "gemu/gemu_display.h"
#include "gemu/vnc.h"
#include "gemu/monitor.h"
#include "gemu/screendump.h"
#include "gemu/util.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#endif

/* Device debug flags are immutable startup configuration.  Several checks
 * live in port/MMIO handlers, so consulting MinGW's CRT environment on every
 * guest access can dominate execution.  String literals have stable
 * addresses, making a pointer-keyed cache both sufficient and very cheap. */
typedef struct I2000EnvCacheEntry {
    const char *name;
    const char *value;
} I2000EnvCacheEntry;

static const char *i2000_cached_getenv(const char *name) {
    enum { ENV_CACHE_SIZE = 128 };
    static I2000EnvCacheEntry cache[ENV_CACHE_SIZE];
    uintptr_t h = ((uintptr_t)name >> 4) ^ ((uintptr_t)name >> 13);
    unsigned slot = (unsigned)h & (ENV_CACHE_SIZE - 1);

    for (unsigned probe = 0; probe < ENV_CACHE_SIZE; probe++) {
        I2000EnvCacheEntry *entry =
            &cache[(slot + probe) & (ENV_CACHE_SIZE - 1)];
        if (entry->name == name)
            return entry->value;
        if (!entry->name) {
            entry->value = getenv(name);
            entry->name = name;
            return entry->value;
        }
    }
    return getenv(name);
}

#define getenv(name) i2000_cached_getenv(name)

/*
 * HP i2000 system model. See i2000.h for the memory map.
 *
 * The legacy I/O port window: on IA-64 the ISA/PCI port space is a
 * memory-mapped window whose base the firmware learns from the chipset.
 * 0x00000FFFFC000000 matches what HP's SAL reports on 460GX systems; every
 * unknown physical access is logged to the front panel, so if the firmware
 * uses a different window it will show up there and we can adjust.
 */
#define I2000_IO_BASE   0x00000FFFFC000000ull
#define I2000_IO_SIZE   0x0000000004000000ull   /* 64 MiB window */
#define COM1_PORT       0x3F8
#define POST_PORT       0x80
#define PCI_CFG_ADDR     0xCF8
#define PCI_CFG_DATA     0xCFC
#define RESET_CTRL_PORT  0xCF9
#define I2000_FW_SHADOW_BASE 0x03C00000ull
#define CMD649_PCI_DEV   3
#define CMD649_PCI_FUN   1

/* Early 460GX SAC scratch/control registers used by the SAL bootstrap. */
#define I2000_SAC_CBNR  0x0000FEB00CB0ull
#define I2000_SAC_CCSR  0x0000FEB00CC0ull

/* I/O SAPIC #0 (SSDM 248704-001 sec. 2.6.2/4.1.3, Figure 4-2): fixed
 * memory-mapped indirect-register interface, the real platform interrupt-
 * delivery path for this chipset. */
#define IOSAPIC_BASE        0xFEC00000ull
#define IOSAPIC_REGSEL_OFF  0x00u
#define IOSAPIC_WINDOW_OFF  0x10u
#define IOSAPIC_EOI_OFF     0x40u
#define IOSAPIC_SIZE        0x50u
#define IOSAPIC_RTE_MASK    (UINT64_C(1) << 16)

/* SAL's ExtINT vector-readback byte (see the bus_read comment at its use
 * site). Confirmed live against the SDV 0.99 debug BIOS: the address it
 * actually reads (0xFEFE0000, within the SSDM's nominal "I/O reserved"
 * 0xFEF0_0000-0xFEFF_FFFF band adjoining the documented Interrupt Delivery
 * region at 0xFEE0_0000) rather than a datasheet-confirmed offset. */
#define I2000_LEGACY_ACK_ADDR 0xFEFE0000ull

/* SMBus host controller I/O registers (IFB function 3, SSDM 248704-001
 * sec. 14.3), offsets from the base programmed into SMBBA. */
#define SMB_HSTSTS   0x00
#define SMB_HSTCNT   0x02
#define SMB_HSTCMD   0x03
#define SMB_HSTADD   0x04
#define SMB_HSTDAT0  0x05
#define SMB_HSTDAT1  0x06
#define SMB_REGSPAN  0x07  /* highest register offset we implement */
#define SMB_HSTSTS_INTER     (1u << 1)  /* command completed */
#define SMB_HSTSTS_DEV_ERR   (1u << 2)  /* no such slave / bad command */
#define SMB_HSTCNT_START     (1u << 6)
#define SMB_PROT_BYTE_DATA   2          /* smbhstcnt[4:2]: byte data r/w */
#define SMB_PROT_QUICK       0
/* Up to 8 simulated DIMM slots (SMBus slave addresses 0x50-0x57), matching
 * one 460GX Memory Card's row count (SSDM sec. 5.1/5.5.1) - see spd_init(). */
#define I2000_SPD_SLOTS 8
#define I2000_SPD_BASE_ADDR 0x50

/* front panel framebuffer */
#define FB_W 640
#define FB_H 480
#define CELL_W 6
#define CELL_H 8
#define COLS (FB_W / CELL_W)     /* 106 */
#define ROWS (FB_H / CELL_H)     /* 50  */
#define CON_ROWS 30              /* serial console area at the bottom */

#define INSTR_PER_FRAME 50000
#define IRQ_POLL_QUANTUM 16

/* Minimum instruction gap between a mouse byte being serviced and the next
 * one being offered - see the ack handler's mouse branch for why this can't
 * be zero (synchronous re-raise corrupts the guest ISR's return frame).
 * Matches the PIT's own tick granularity, which is already known safe. */
#define MOUSE_RETRY_DELAY_INSTS 1000u
#define KEYBOARD_RETRY_DELAY_INSTS 10000u

enum {
    I2000_KEY_UP,
    I2000_KEY_DOWN,
    I2000_KEY_LEFT,
    I2000_KEY_RIGHT,
    I2000_KEY_HOME,
    I2000_KEY_END,
    I2000_KEY_PAGEUP,
    I2000_KEY_PAGEDOWN,
    I2000_KEY_INSERT,
    I2000_KEY_DELETE,
};

static const GemuActionDef i2000_keyboard_actions[] = {
    { "key_up",       GEMU_ACTION(I2000_KEY_UP),       "Up" },
    { "key_down",     GEMU_ACTION(I2000_KEY_DOWN),     "Down" },
    { "key_left",     GEMU_ACTION(I2000_KEY_LEFT),     "Left" },
    { "key_right",    GEMU_ACTION(I2000_KEY_RIGHT),    "Right" },
    { "key_home",     GEMU_ACTION(I2000_KEY_HOME),     "Home" },
    { "key_end",      GEMU_ACTION(I2000_KEY_END),      "End" },
    { "key_page_up",  GEMU_ACTION(I2000_KEY_PAGEUP),   "PageUp" },
    { "key_page_down",GEMU_ACTION(I2000_KEY_PAGEDOWN), "PageDown" },
    { "key_insert",   GEMU_ACTION(I2000_KEY_INSERT),   "Insert" },
    { "key_delete",   GEMU_ACTION(I2000_KEY_DELETE),   "Delete" },
};

static const uint32_t i2000_keyboard_keysyms[] = {
    0xff52, 0xff54, 0xff51, 0xff53, 0xff50,
    0xff57, 0xff55, 0xff56, 0xff63, 0xffff,
};
#define MOUSE_BACKLOG_MAX       254
#define I2000_ITC_HZ UINT64_C(200000000)
#define I2000_ITC_TICK_NS 5u
#define I8254_INPUT_HZ UINT64_C(1193182)
/* Command completion must not be visible in the same CPU instant as the
 * command-register write - AtaPim arms its waiter after submission, so an
 * instantaneous interrupt would fire before anything is listening. But the
 * delay must also stay well inside AtaPim's own polling patience: observed
 * behavior is a tight status-poll loop advancing ~41 ninsts/iteration for
 * 100+ iterations before falling back to a Stall()-spaced slow poll, and an
 * interrupt arriving only after that patience is exhausted (100000 ninsts
 * was tried and is far past it) gets acknowledged by unrelated generic code
 * without ever reaching back to drain the data register. Comfortably above
 * one instruction, comfortably below the observed give-up point. */
#define ATAPI_IRQ_LATENCY_NINSTS UINT64_C(2000)
#define MMIO_LOG_N 768
#define HALT_TRACE_LINES 32
#define HALT_CALL_LINES  32

#define RAGE128_PCI_DEV       5u
#define RAGE128_FB_BASE       0xC4000000ull
#define RAGE128_IO_BASE       0xC300u
#define RAGE128_MMIO_BASE     0xC8000000ull
#define I82559_PCI_DEV          6u
#define I82559_MMIO_BASE        0xC8100000u
#define I82559_IO_BASE          0xC400u
#define I2000_HIGH_DRAM_TAG   UINT64_C(0x00000e0000000000)
#define I2000_HIGH_DRAM_SIZE  (UINT64_C(64) * 1024 * 1024)
#define I2000_HIGH_DRAM_BASE  (I2000_RAM_MAX - I2000_HIGH_DRAM_SIZE)
#define I2000_FLASH_CHIP_SIZE UINT64_C(0x100000)
#define I2000_FLASH_CHIPS     (I2000_FLASH_SIZE / I2000_FLASH_CHIP_SIZE)

typedef struct {
    uint64_t addr;
    uint64_t val;
    uint32_t count;
    uint8_t  is_write;
    uint8_t  size;
} MmioLogEnt;

typedef struct {
    bool read_status;
    bool read_id;
    uint8_t status;
    uint8_t cmd;
    uint32_t cmd_addr;
} I2000FlashChip;

struct Ia64I2000State {
    GemuMonitor *monitor;
    GemuDisplay *display;
    Merced      *cpu;

    uint8_t  *ram;
    uint64_t  ram_size;
    /* Backs [I2000_CHIPSET_SCRATCH_BASE, I2000_RAM_MAX) independent of
     * ram_size - see the comment on I2000_CHIPSET_SCRATCH_SIZE in i2000.h. */
    uint8_t  *chipset_scratch;
    uint8_t  *flash;
    char      flash_file[512];
    uint32_t  flash_image_size;
    bool      flash_loaded;
    bool      flash_read_status;
    bool      flash_read_id;
    uint8_t   flash_status;
    uint8_t   flash_cmd;
    /* Legacy scalar flash command address retained for old snapshots.  The
     * per-chip command state superseded it; at runtime it now tracks the
     * active top-of-aperture SAL scratch alias. */
    uint64_t  flash_cmd_addr;

    bool      halted;
    bool      reset_requested;
    bool      fw_shadow_enabled;
    MercedStatus halt_status;

    /* front panel */
    uint32_t  fb[FB_W * FB_H];
    char      console[CON_ROWS][COLS + 1];
    int       con_row, con_col;
    bool      con_dirty;

    /* devices */
    uint8_t   post_code;
    bool      legacy_irq_routed;
    uint32_t  sac_cbnr, sac_ccsr;
    uint8_t   port61;
    uint8_t   pit2_polls;
    uint8_t   pic_master_mask, pic_slave_mask;
    uint8_t   pic_master_base, pic_slave_base;
    uint8_t   pic_master_icw, pic_slave_icw;
    uint16_t  pit0_reload, pit0_latch;
    uint8_t   pit0_write_phase;
    uint64_t  pit0_next_irq;
    /* PIT channel 1 (port 0x41): unlike channel 0, this has no standard
     * ISA IRQ wiring on real hardware (classically DRAM refresh, not an
     * interrupt source) and firmware never touches any I/O SAPIC RTE to
     * configure it either - confirmed live (IOSAPIC_DEBUG) that every RTE
     * stays at its default masked reset value the whole time this SDV
     * BIOS spins waiting on a channel-1-armed count. Delivered directly
     * at the vector firmware itself put in cr.itv (0x80) despite masking
     * cr.itv's own local-timer path - the working theory being that 0x80
     * is just firmware's conventional timer-ISR vector, fed here by
     * whichever physical timer is actually active. */
    uint16_t  pit1_reload, pit1_latch;
    uint8_t   pit1_write_phase;
    uint64_t  pit1_next_irq;
    /* I/O SAPIC #0 (SSDM 248704-001 sec. 2.6.2/2.6.3): the real platform
     * interrupt-delivery path once past legacy 8259 compatibility mode -
     * legacy_irq_routed above is permanently false (nothing ever sets it),
     * so this is the only functional interrupt-delivery mechanism. Fixed
     * at FEC00000h; regsel/window give indirect access to id/version/arb
     * (00h-02h) and 64 redirection-table entries (10h-8Fh, two dwords
     * each). GSI N is assumed == legacy ISA IRQ N (no ACPI interrupt
     * source overrides modeled). */
    uint32_t  iosapic_regsel;
    uint64_t  iosapic_rte[64];
    uint8_t   kbc_command_byte;
    uint8_t   kbc_pending_write; /* 0=keyboard data, 1=command byte, 2=output port, 3=aux */
    uint8_t   kbc_out[8];
    uint8_t   kbc_out_pos, kbc_out_len;
    uint8_t   cmos_index;
    uint8_t   cmos[128];
    uint32_t  pci_cfg_addr;
    bool      ifb_smbus_cmd_read_once;
    uint8_t   ifb_cfg[256];
    uint8_t   ifb_usb_cfg[256];
    uint8_t   ifb_smbus_cfg[256];
    /* SMBus host-controller interface (IFB function 3, I/O regs at the
     * base programmed into ifb_smbus_cfg[0x20..0x23] - SSDM 248704-001
     * sec. 14.3). Firmware uses this to read Serial Presence Detect (SPD)
     * data from each DIMM row to compute total installed memory (sec.
     * 5.5.1) rather than probing addresses; without real SPD data behind
     * it, firmware fell back to an assumed max-memory layout regardless
     * of -m, faulting on whatever wasn't actually backed. */
    uint8_t   smb_hststs, smb_hstcnt, smb_hstcmd, smb_hstadd;
    uint8_t   smb_hstdat0, smb_hstdat1;
    bool      spd_present[I2000_SPD_SLOTS];
    uint8_t   spd[I2000_SPD_SLOTS][256];
    uint8_t   cmd649_cfg[256];
    uint8_t   acpi_io[64];
    FILE     *cdrom;
    char      cdrom_file[512];
    uint64_t  cdrom_size;
    uint8_t   atapi_error, atapi_features, atapi_count;
    uint8_t   atapi_lba_low, atapi_lba_mid, atapi_lba_high, atapi_device;
    uint8_t   atapi_status, atapi_packet[12];
    unsigned  atapi_packet_pos;
    uint8_t  *atapi_data;
    size_t    atapi_data_len, atapi_data_pos;
    /* CMD-649 primary channel: a real ATA (not ATAPI) hard disk, backed by
     * -hda's raw 512-byte-sector image. Single device (master) only, always
     * responding regardless of the drive-select bit - same simplification
     * the ATAPI/CD-ROM secondary channel above already makes. */
    FILE     *hda;
    char      hda_file[512];
    uint64_t  hda_size;
    uint8_t   ata_error, ata_features, ata_count;
    uint8_t   ata_lba_low, ata_lba_mid, ata_lba_high, ata_device;
    uint8_t   ata_status;
    uint8_t  *ata_data;
    size_t    ata_data_len, ata_data_pos;
    bool      ata_data_is_write;
    uint32_t  ata_write_lba;
    uint8_t   chipset_bus;
    /* Real 460GX device map (SSDM 248704-001 Table 2-1, bus CBN):
     *   00h/01h SAC, 04h SDC, 05h/06h Memory Card A/B, 10h-17h Expander
     *   0-3 buses a/b (WXB/PXB/GXB - not yet modeled). Devices are
     *   multi-function; fn dimension added since SAC dev 1 fn 1 is
     *   accessed during boot (extended/diagnostic function, purpose
     *   still being traced). */
    uint8_t   chipset_cfg[32][8][256];
    uint8_t   memcard_cfg[2][8][256];
    uint8_t   uart_ier, uart_lcr, uart_mcr, uart_scr, uart_dll, uart_dlm;
    uint8_t   uart_rx[256], uart_rx_head, uart_rx_tail;

    VgaIbm    vga;
    /* The video BIOS option ROM shadow, kept separate from plain system RAM
     * (like `flash`) so a generic "clear all of RAM" firmware loop - which
     * legitimately treats the whole reported memory range as ordinary RAM -
     * can't wipe it out before IA-32 mode ever gets to use it. Real chipsets
     * exclude this range from the reported memory map for the same reason. */
    uint8_t   vga_rom_shadow[0x10000];

    MmioLogEnt mmio_log[MMIO_LOG_N];
    int        mmio_log_n;

    /* Wall-clock (not instruction-count) periodic autosave, so a long,
     * mostly-idle-waiting debug session always has a recent rollback point
     * without needing a human to remember to snapshot. Two rotating slots
     * rather than one: if the process is killed mid-write of the current
     * slot, the other one is still intact. */
    time_t     last_autosave;
    unsigned   autosave_slot;

    /* Keep newly-added host-only pointers at the end: v3 snapshots contain
     * every preceding byte and can therefore still be restored exactly. */
    GemuVncServer *vnc;

    /* v5 snapshot tail.  Rage128 needs only the display engine and linear
     * framebuffer for EFI/Windows setup; AGP and 3D are intentionally out
     * of scope.  The 64 MiB PCI aperture mirrors the installed 8 MiB VRAM. */
    /* Keep this in the former enabled/cfg/mmio/vram position: Rage128 has
     * the same byte layout, preserving legacy i2000 snapshot compatibility. */
    Rage128 rage128;

    /* v6 snapshot tail: IA64-VPC-compatible INT 10h/VBE bridge. */
    struct { uint16_t ax, bx, cx, dx, di, es; } int10_req, int10_res;
    uint32_t int10_signature;
    uint8_t int10_response[512];
    uint16_t int10_response_len, int10_response_pos;
    uint8_t int10_signature_words;
    uint16_t int10_mode;

    /* v7 snapshot tail: PS/2 mouse (-device mouse), routed through the
     * existing 8042 aux-device path (kbc_pending_write==3, port 0xD4/0x60)
     * that was previously always a no-op. Kept in its own queue rather than
     * sharing kbc_out[] because the 0x60/0x64 write handlers unconditionally
     * clear kbc_out on every keyboard-channel command, which would wipe an
     * in-flight mouse ACK/packet. */
    bool     mouse_enabled;
    bool     mouse_streaming;
    uint8_t  aux_out[16];
    uint8_t  aux_out_pos, aux_out_len;
    bool     mouse_prev_left, mouse_prev_right;
    uint8_t  mouse_param_cmd;   /* 0xE8 or 0xF3 awaiting its 1 param byte, 0=none */
    uint8_t  mouse_resolution, mouse_sample_rate;
    bool     mouse_scaling_2to1;
    /* Legacy-ExtINT ack disambiguation (see the comment on
     * I2000_LEGACY_ACK_ADDR's read handler): this firmware never touches
     * an I/O SAPIC RTE for the mouse either (confirmed live), so IRQ12
     * must be delivered the same legacy-8259/ExtINT way as the PIT's
     * IRQ0 - which means the single shared ack address needs to know
     * which of the two actually caused the pending ExtINT. */
    bool     pit_irq_pending;
    bool     mouse_irq_pending;
    /* v8 tail: deferred multi-byte mouse-packet retry (see the ack handler's
     * mouse branch). Re-raising vector 0 synchronously, from within the same
     * legacy-ack dispatch that's still servicing the CURRENT byte, risks
     * corrupting the guest ISR: cr.ipsr/cr.iip/cr.isr are single non-stacked
     * registers, and per merced.c's deliver_fault, a second delivery landing
     * before this ISR invocation truly completes would overwrite its not-
     * yet-consumed return frame - the same class of "IRQ0 handler's STI
     * immediately re-enter[ing]" risk the pre-existing ack-arbitration
     * comment already warned about for PIT. Deferring the retry by a real
     * instruction gap (mirrors the PIT's own known-safe 100000-instruction
     * tick granularity) closes off that risk for free.
     *
     * Update: a rapid synthetic packet burst could still reproducibly park
     * the CPU at psr.i=0 in a stable, non-corrupted spin loop even with
     * this field's deferral in place, as long as *new* packets kept being
     * queued while the guest was only partway through draining an earlier
     * one. It wasn't a one-shot POST self-test artifact after all - it
     * also reproduced at the real BIOS Configuration Manager GUI. The real
     * fix turned out to be at the call site: aux_out must be completely
     * empty (not just "has room") and this deadline must have passed
     * before a brand new packet is queued at all, not merely before it's
     * raised - see the per-frame mouse-polling block in ia64_i2000_run().
     * v9 replaces that arbitrary cooldown with completion of the actual
     * interrupt return, and accumulates host motion while a packet drains. */
    uint64_t mouse_retry_ninsts;
    /* v9: use the real PIC EOI handshake as mouse-flow control instead of
     * an arbitrary multi-million-instruction cooldown. Host motion is
     * coalesced while a report is in flight. Direction reversals discard
     * stale motion in the old direction, and each axis is capped at two
     * packets so a slow guest cannot build a long cursor backlog. */
    uint64_t mouse_wait_rfi_generation;
    int32_t mouse_accum_dx, mouse_accum_dy;
    bool mouse_host_left, mouse_host_right;
    uint64_t mouse_next_report_itc;
    /* v10 tail: separately-backed top of the 0xe00 memory-card aperture. */
    uint8_t *high_dram;
    /* v11: persistent writable flash overlay (the ROM itself stays clean). */
    char nvram_path[512];
    bool nvram_dirty;
    /* v12: legacy IRQ1 latch for queued 8042 keyboard/controller output. */
    bool keyboard_irq_pending;
    /* v13: channel-0 read sequencing and counter epoch. */
    uint8_t pit0_read_phase;
    uint64_t pit0_epoch_ninsts;
    /* v14: optional onboard Intel 82559-compatible 10/100 Base-T NIC. */
    I82559 i82559;
    /* v15: reusable PS/2 keyboard device behind this machine's 8042. */
    Ps2Keyboard ps2_keyboard;
    uint64_t keyboard_retry_ninsts;
    /* v16: secondary IDE interrupt latch. ATAPI is on slave-PIC IRQ15. */
    bool atapi_irq_pending;
    /* v17: in-service state established by the legacy acknowledge cycle. */
    uint8_t pic_master_isr, pic_slave_isr;
    /* v18: command completion is not visible in the same CPU instant as
     * the command-register write. AtaPim arms its waiter after submission. */
    uint64_t atapi_irq_ready_ninsts;
    /* v19: the 4 MiB firmware array is four independently-commanded Intel
     * 1 MiB devices. A read-array command sent to one device must not reset
     * identifier/status mode in another. The legacy scalar fields near the
     * start of this structure remain solely for old snapshot compatibility. */
    I2000FlashChip flash_chip[I2000_FLASH_CHIPS];
    /* v21: ATAPI PIO transfers are split at the host-programmed byte limit
     * and interrupt once per data phase. */
    uint32_t atapi_byte_limit;
    size_t atapi_phase_end;
    /* v22: ATA command execution has a real BSY interval.  AtaPim first
     * waits for BSY to assert, then waits for it to clear before using DRQ. */
    uint64_t atapi_status_ready_ninsts;
    uint8_t atapi_ready_status;
    bool atapi_status_pending;
    bool atapi_ready_irq;
    /* v23: OCW3 read-register selection for each legacy 8259. False is
     * IRR (the reset/default selection), true is ISR. */
    bool pic_master_read_isr, pic_slave_read_isr;
};

static void mmio_log(Ia64I2000State *s, uint64_t addr, uint64_t val,
                     unsigned size, bool is_write);
static void i2000_reset(Ia64I2000State *s);
static void iosapic_raise_gsi(Ia64I2000State *s, unsigned gsi);

static int32_t mouse_accumulate_axis(int32_t pending, int delta) {
    if (!delta)
        return pending;
    if ((pending > 0 && delta < 0) || (pending < 0 && delta > 0))
        pending = 0;
    int64_t next = (int64_t)pending + delta;
    if (next > MOUSE_BACKLOG_MAX)
        return MOUSE_BACKLOG_MAX;
    if (next < -MOUSE_BACKLOG_MAX)
        return -MOUSE_BACKLOG_MAX;
    return (int32_t)next;
}

static void aux_queue_byte(Ia64I2000State *s, uint8_t byte) {
    /* Mirrors kbc_queue_byte(), but for the separate PS/2 aux (mouse)
     * output buffer - see the field comment on aux_out[] for why this
     * can't just share kbc_out[]. Unlike the keyboard (which this
     * firmware polls OBF for, so no interrupt is needed), its mouse
     * driver is interrupt-driven - confirmed empirically: without this,
     * queued packets just piled up in aux_out[] forever, never read out,
     * because the guest was sitting in an interrupt wait that never
     * fired. This firmware never configures an I/O SAPIC RTE for the
     * mouse either (same as the PIT/IRQ0 case documented in
     * i2000_poll_interrupts()) - it stays in legacy-8259-compatible mode
     * the whole time, so IRQ12 has to be delivered the same way: ExtINT
     * (vector 0), gated by the slave PIC's IRQ12 mask bit (0x10) and the
     * master's cascade line (IRQ2, 0x04). */
    if (s->aux_out_pos) {
        memmove(s->aux_out, s->aux_out + s->aux_out_pos,
                s->aux_out_len - s->aux_out_pos);
        s->aux_out_len -= s->aux_out_pos;
        s->aux_out_pos = 0;
    }
    if (s->aux_out_len < sizeof(s->aux_out))
        s->aux_out[s->aux_out_len++] = byte;
    /* Deliberately does NOT raise here. This used to raise unconditionally
     * on every push, reasoning that merced_raise_external() just OR's a
     * single shared per-vector bit and is therefore idempotent/harmless to
     * call redundantly. That's true at the bit level, but it misses a real
     * race one level up: if a NEW packet arrives (via a fresh host mouse
     * event) while the guest is still mid-ISR servicing an EARLIER byte -
     * interrupts briefly re-enabled before that ISR's own cleanup/rfi has
     * finished, the same "IRQ0 handler's STI immediately re-enter[ing]"
     * class of risk the ack-arbitration comment below already describes for
     * PIT - this raise could hand the CPU a second, unwanted ExtINT
     * delivery right then, clobbering the first invocation's not-yet-
     * consumed cr.ipsr/cr.iip. Confirmed live: a realistic mouse-movement
     * test (8 packets, 0.3s apart, at the actual BIOS Configuration Manager
     * GUI, not just a synthetic burst) corrupted cr.iip to a near-null
     * 0x180 and halted the guest in a dead loop trying to execute there -
     * matching the user's own original report of the emulator freezing
     * entirely on real mouse movement early in boot.
     *
     * i2000_poll_interrupts() is the single place that actually raises
     * vector 0 on mouse's behalf now (both for this fresh-packet case and
     * for the ack handler's same-packet retry case below), gated on
     * mouse_retry_ninsts - which this function does NOT touch, so an
     * in-progress cooldown from the previous packet's drain naturally
     * carries over and delays this new byte's delivery too. Nothing
     * "unraises" here even when a cooldown is active: the byte is queued
     * now, and poll_interrupts will pick it up as soon as it's safe to. */
}

/* PS/2 mouse command handler, reached from the 8042's aux-write routing
 * (0x64<-0xD4 then 0x60<-cmd) once a real mouse is attached via
 * -device mouse. Modeled on the standard PS/2 mouse command set - see
 * hardware/machine/mos/machine_um6578.c's um6578_mouse_device_command()
 * for the sibling implementation this was cross-checked against. */
static void mouse_device_command(Ia64I2000State *s, uint8_t cmd) {
    if (s->mouse_param_cmd) {
        /* Second byte of a two-byte command: the value itself. */
        if (s->mouse_param_cmd == 0xE8)
            s->mouse_resolution = cmd;
        else if (s->mouse_param_cmd == 0xF3)
            s->mouse_sample_rate = cmd;
        s->mouse_param_cmd = 0;
        aux_queue_byte(s, 0xFA);
        return;
    }
    switch (cmd) {
    case 0xFF: /* reset */
        aux_queue_byte(s, 0xFA);
        aux_queue_byte(s, 0xAA);
        aux_queue_byte(s, 0x00);
        s->mouse_streaming = false;
        s->mouse_resolution = 2;
        s->mouse_sample_rate = 100;
        s->mouse_scaling_2to1 = false;
        s->mouse_prev_left = s->mouse_prev_right = false;
        s->mouse_host_left = s->mouse_host_right = false;
        s->mouse_accum_dx = s->mouse_accum_dy = 0;
        break;
    case 0xF6: /* set defaults */
        aux_queue_byte(s, 0xFA);
        s->mouse_streaming = false;
        s->mouse_resolution = 2;
        s->mouse_sample_rate = 100;
        s->mouse_scaling_2to1 = false;
        break;
    case 0xF4: /* enable data reporting */
        aux_queue_byte(s, 0xFA);
        s->mouse_streaming = true;
        break;
    case 0xF5: /* disable data reporting */
        aux_queue_byte(s, 0xFA);
        s->mouse_streaming = false;
        break;
    case 0xE6: /* set scaling 1:1 */
        aux_queue_byte(s, 0xFA);
        s->mouse_scaling_2to1 = false;
        break;
    case 0xE7: /* set scaling 2:1 */
        aux_queue_byte(s, 0xFA);
        s->mouse_scaling_2to1 = true;
        break;
    case 0xE8: /* set resolution: one param byte follows */
        aux_queue_byte(s, 0xFA);
        s->mouse_param_cmd = 0xE8;
        break;
    case 0xF3: /* set sample rate: one param byte follows */
        aux_queue_byte(s, 0xFA);
        s->mouse_param_cmd = 0xF3;
        break;
    case 0xF2: /* get device ID */
        aux_queue_byte(s, 0xFA);
        aux_queue_byte(s, 0x00); /* standard PS/2 mouse, no wheel */
        break;
    case 0xE9: /* status request */
        aux_queue_byte(s, 0xFA);
        aux_queue_byte(s, (uint8_t)((s->mouse_scaling_2to1 ? 0x10 : 0) |
                                     (s->mouse_streaming ? 0x20 : 0) |
                                     (s->mouse_prev_left ? 0x01 : 0) |
                                     (s->mouse_prev_right ? 0x04 : 0)));
        aux_queue_byte(s, s->mouse_resolution);
        aux_queue_byte(s, s->mouse_sample_rate);
        break;
    case 0xEB: /* read data (remote/poll mode): ACK + one packet, all-zero
                * delta since this stub doesn't track a separate polled
                * sample - streaming mode is what actually drives the
                * cursor and is what every BIOS/OS driver uses in practice. */
        aux_queue_byte(s, 0xFA);
        aux_queue_byte(s, (uint8_t)(0x08 | (s->mouse_prev_left ? 0x01 : 0) |
                                     (s->mouse_prev_right ? 0x02 : 0)));
        aux_queue_byte(s, 0x00);
        aux_queue_byte(s, 0x00);
        break;
    default: /* forgiving default: ACK anything unrecognized rather than
              * wedge the guest's init state machine on a vendor probe. */
        aux_queue_byte(s, 0xFA);
        break;
    }
}

static uint64_t pit_interval_itc(uint16_t reload) {
    uint64_t count = reload ? reload : UINT64_C(0x10000);
    return (count * I2000_ITC_HZ + I8254_INPUT_HZ - 1) / I8254_INPUT_HZ;
}

static uint64_t size_mask(unsigned size) {
    return size >= 8 ? ~0ull : (1ull << (size * 8)) - 1;
}

static bool atapi_debug_enabled(void) {
    const char *value = getenv("ATA_DEBUG");
    return value && value[0] && strcmp(value, "0") != 0;
}

static const char *atapi_packet_name(uint8_t opcode) {
    switch (opcode) {
    case 0x00: return "TEST UNIT READY";
    case 0x03: return "REQUEST SENSE";
    case 0x12: return "INQUIRY";
    case 0x1B: return "START STOP UNIT";
    case 0x1E: return "PREVENT/ALLOW MEDIUM REMOVAL";
    case 0x25: return "READ CAPACITY";
    case 0x28: return "READ(10)";
    case 0x43: return "READ TOC";
    case 0xA8: return "READ(12)";
    default:   return "unknown";
    }
}

static void atapi_defer_ready(Ia64I2000State *s, bool raise_irq) {
    s->atapi_ready_status = s->atapi_status;
    s->atapi_status = 0x80; /* BSY */
    s->atapi_status_pending = true;
    s->atapi_ready_irq = raise_irq;
    s->atapi_status_ready_ninsts =
        s->cpu->ninsts + ATAPI_IRQ_LATENCY_NINSTS;
    /* The completion interrupt belongs to the ready transition, not to the
     * command-register write while the device is still busy. */
    s->atapi_irq_pending = false;
}

static void atapi_begin_data_phase(Ia64I2000State *s) {
    size_t remaining = s->atapi_data_len - s->atapi_data_pos;
    size_t phase = remaining;
    uint32_t limit = s->atapi_byte_limit ? s->atapi_byte_limit : 65536u;
    if (phase > limit)
        phase = limit;
    /* ATAPI PIO byte counts are even except for a final odd byte. */
    if (phase > 1 && (phase & 1) && phase < remaining)
        phase--;
    s->atapi_phase_end = s->atapi_data_pos + phase;
    s->atapi_count = 0x02; /* data phase: CoD=0, I/O=device-to-host */
    s->atapi_lba_mid = (uint8_t)phase;
    s->atapi_lba_high = (uint8_t)(phase >> 8);
    s->atapi_status = 0x48; /* DRDY|DRQ */
    s->atapi_irq_pending = true;
    s->atapi_irq_ready_ninsts = s->cpu->ninsts + ATAPI_IRQ_LATENCY_NINSTS;
}

static void atapi_set_data(Ia64I2000State *s, const void *data, size_t len) {
    free(s->atapi_data);
    s->atapi_data = NULL;
    s->atapi_data_len = s->atapi_data_pos = 0;
    s->atapi_phase_end = 0;
    if (len) {
        s->atapi_data = malloc(len);
        if (!s->atapi_data) {
            s->atapi_error = 0x04;
            s->atapi_status = 0x41;
            return;
        }
        memcpy(s->atapi_data, data, len);
        s->atapi_data_len = len;
    }
    if (len) {
        atapi_begin_data_phase(s);
    } else {
        s->atapi_phase_end = 0;
        s->atapi_count = 0x03; /* command complete: CoD=1, I/O=1 */
        s->atapi_lba_mid = s->atapi_lba_high = 0;
        s->atapi_status = 0x40;
        s->atapi_irq_pending = true;
        s->atapi_irq_ready_ninsts =
            s->cpu->ninsts + ATAPI_IRQ_LATENCY_NINSTS;
    }
    if (atapi_debug_enabled())
        fprintf(stderr, "i2000: ATAPI response bytes=%zu status=%#04x "
                "reason=%#04x\n", len, s->atapi_status, s->atapi_count);
}

static void atapi_no_medium(Ia64I2000State *s) {
    free(s->atapi_data);
    s->atapi_data = NULL;
    s->atapi_data_len = s->atapi_data_pos = 0;
    s->atapi_phase_end = 0;
    s->atapi_count = 0x03; /* command completion, I/O to host */
    s->atapi_error = 0x20; /* sense key: NOT READY */
    s->atapi_status = 0x41; /* DRDY|ERR */
    s->atapi_irq_pending = true;
    s->atapi_irq_ready_ninsts = s->cpu->ninsts + ATAPI_IRQ_LATENCY_NINSTS;
    if (atapi_debug_enabled())
        fprintf(stderr, "i2000: ATAPI no medium status=%#04x error=%#04x\n",
                s->atapi_status, s->atapi_error);
}

static void atapi_reply(Ia64I2000State *s) {
    const uint8_t *p = s->atapi_packet;
    uint8_t reply[64] = {0};
    uint32_t blocks = (uint32_t)(s->cdrom_size / 2048);
    if (atapi_debug_enabled()) {
        unsigned byte_limit = (unsigned)s->atapi_lba_mid |
                              ((unsigned)s->atapi_lba_high << 8);
        fprintf(stderr, "i2000: ATAPI packet %-28s opcode=%#04x "
                "host-byte-limit=%u bytes=", atapi_packet_name(p[0]), p[0],
                byte_limit ? byte_limit : 65536u);
        for (unsigned i = 0; i < sizeof(s->atapi_packet); i++)
            fprintf(stderr, "%s%02x", i ? " " : "", p[i]);
        fputc('\n', stderr);
    }
    switch (p[0]) {
    case 0x00: /* TEST UNIT READY */
        if (!s->cdrom) {
            atapi_no_medium(s);
            break;
        }
        atapi_set_data(s, NULL, 0);
        break;
    case 0x1B: /* START STOP UNIT */
    case 0x1E: /* PREVENT/ALLOW MEDIUM REMOVAL */
        atapi_set_data(s, NULL, 0);
        break;
    case 0x03: /* REQUEST SENSE */
        reply[0] = 0x70;
        if (!s->cdrom) {
            reply[2] = 0x02; /* NOT READY */
            reply[12] = 0x3a; /* medium not present */
        }
        reply[7] = 10;
        atapi_set_data(s, reply, p[4] < 18 ? p[4] : 18);
        break;
    case 0x12: { /* INQUIRY */
        reply[0] = 0x05; reply[1] = 0x80; reply[2] = 0x00;
        reply[3] = 0x21; reply[4] = 31;
        memcpy(reply + 8, "GEMU    ", 8);
        memcpy(reply + 16, "ATAPI CD-ROM    ", 16);
        memcpy(reply + 32, "1.0 ", 4);
        size_t n = p[4] < 36 ? p[4] : 36;
        atapi_set_data(s, reply, n);
        break;
    }
    case 0x25: /* READ CAPACITY */
        if (!s->cdrom) {
            atapi_no_medium(s);
            break;
        }
        if (blocks) blocks--;
        reply[0] = blocks >> 24; reply[1] = blocks >> 16;
        reply[2] = blocks >> 8;  reply[3] = blocks;
        reply[6] = 0x08;
        atapi_set_data(s, reply, 8);
        break;
    case 0x43: { /* READ TOC: one data track, lead-out */
        if (!s->cdrom) {
            atapi_no_medium(s);
            break;
        }
        reply[1] = 0x12; reply[2] = 1; reply[3] = 1;
        reply[5] = 0x14; reply[6] = 1;
        reply[13] = 0x14; reply[14] = 0xAA;
        reply[16] = blocks >> 24; reply[17] = blocks >> 16;
        reply[18] = blocks >> 8; reply[19] = blocks;
        size_t alloc = ((size_t)p[7] << 8) | p[8];
        atapi_set_data(s, reply, alloc < 20 ? alloc : 20);
        break;
    }
    case 0x28: /* READ(10) */
    case 0xA8: { /* READ(12) */
        uint32_t lba = ((uint32_t)p[2] << 24) | ((uint32_t)p[3] << 16) |
                       ((uint32_t)p[4] << 8) | p[5];
        uint32_t count = p[0] == 0x28 ? ((uint32_t)p[7] << 8) | p[8] :
                         ((uint32_t)p[6] << 24) | ((uint32_t)p[7] << 16) |
                         ((uint32_t)p[8] << 8) | p[9];
        size_t len = (size_t)count * 2048;
        if (atapi_debug_enabled())
            fprintf(stderr, "i2000: ATAPI %s lba=%" PRIu32
                    " sectors=%" PRIu32 " bytes=%zu image-blocks=%" PRIu32
                    "\n", atapi_packet_name(p[0]), lba, count, len, blocks);
        uint8_t *buf = len ? malloc(len) : NULL;
        if ((len && !buf) || lba >= blocks || count > blocks - lba ||
            fseek(s->cdrom, (long)((uint64_t)lba * 2048), SEEK_SET) != 0 ||
            (len && fread(buf, 1, len, s->cdrom) != len)) {
            free(buf); s->atapi_error = 0x50; s->atapi_status = 0x41;
            s->atapi_count = 0x03;
            s->atapi_irq_pending = true;
            s->atapi_irq_ready_ninsts =
                s->cpu->ninsts + ATAPI_IRQ_LATENCY_NINSTS;
            if (atapi_debug_enabled())
                fprintf(stderr, "i2000: ATAPI read failed lba=%" PRIu32
                        " sectors=%" PRIu32 " status=%#04x error=%#04x\n",
                        lba, count, s->atapi_status, s->atapi_error);
        } else {
            atapi_set_data(s, buf, len);
            free(buf);
        }
        break;
    }
    default:
        fprintf(stderr, "i2000: ATAPI unsupported packet command %02X\n", p[0]);
        s->atapi_error = 0x50;
        s->atapi_status = 0x41;
        s->atapi_count = 0x03;
        s->atapi_irq_pending = true;
        s->atapi_irq_ready_ninsts =
            s->cpu->ninsts + ATAPI_IRQ_LATENCY_NINSTS;
        break;
    }
}

static void ata_set_data(Ia64I2000State *s, const void *data, size_t len) {
    free(s->ata_data);
    s->ata_data = NULL;
    s->ata_data_len = s->ata_data_pos = 0;
    if (len) {
        s->ata_data = malloc(len);
        if (!s->ata_data) {
            s->ata_error = 0x04; /* ABRT */
            s->ata_status = 0x41;
            return;
        }
        if (data) memcpy(s->ata_data, data, len);
        s->ata_data_len = len;
    }
    s->ata_status = len ? 0x48 : 0x40; /* DRDY|DRQ or DRDY */
}

/* 28-bit LBA from the task-file registers, including the low nibble of the
 * device/head register (only meaningful when that register's LBA bit, 0x40,
 * is set - CHS addressing isn't modeled since every caller so far has used
 * LBA). */
static uint32_t ata_lba(const Ia64I2000State *s) {
    return (uint32_t)s->ata_lba_low | ((uint32_t)s->ata_lba_mid << 8) |
           ((uint32_t)s->ata_lba_high << 16) |
           ((uint32_t)(s->ata_device & 0x0F) << 24);
}

/* Fills a byte-swapped ASCII field the way ATA IDENTIFY DEVICE strings are
 * conventionally packed: each pair of bytes holds one 16-bit word with the
 * two characters swapped relative to normal reading order. */
static void ata_id_string(uint8_t *id, unsigned byte_off, unsigned len,
                          const char *text) {
    char buf[40];
    size_t tlen = strlen(text);
    memset(buf, ' ', sizeof(buf));
    memcpy(buf, text, tlen < sizeof(buf) ? tlen : sizeof(buf));
    for (unsigned i = 0; i < len; i += 2) {
        id[byte_off + i]     = (uint8_t)buf[i + 1];
        id[byte_off + i + 1] = (uint8_t)buf[i];
    }
}

static void ata_identify(Ia64I2000State *s) {
    uint8_t id[512] = {0};
    uint32_t sectors = s->hda ? (uint32_t)(s->hda_size / 512) : 0;
    const uint32_t heads = 16, spt = 63;
    uint32_t cyls = sectors / (heads * spt);
    if (cyls > 16383) cyls = 16383;
    uint32_t chs_sectors = cyls * heads * spt;

    id[0] = 0x40;                                    /* word0: fixed device */
    id[2] = (uint8_t)cyls; id[3] = (uint8_t)(cyls >> 8);       /* word1 */
    id[6] = (uint8_t)heads;                                     /* word3 */
    id[12] = (uint8_t)spt;                                      /* word6 */
    ata_id_string(id, 20, 20, "GEMU-HDA0001");                  /* words 10-19: serial */
    ata_id_string(id, 46, 8, "1.0");                            /* words 23-26: firmware rev */
    ata_id_string(id, 54, 40, "GEMU VIRTUAL HDD");               /* words 27-46: model */
    id[98] = 0x00; id[99] = 0x02;                     /* word49: LBA supported */
    id[106] = 0x01;                                   /* word53: words 54-58 valid */
    id[108] = (uint8_t)cyls; id[109] = (uint8_t)(cyls >> 8);    /* word54 */
    id[110] = (uint8_t)heads;                                    /* word55 */
    id[112] = (uint8_t)spt;                                      /* word56 */
    id[114] = (uint8_t)chs_sectors; id[115] = (uint8_t)(chs_sectors >> 8);
    id[116] = (uint8_t)(chs_sectors >> 16);            /* word57-58: current capacity */
    id[117] = (uint8_t)(chs_sectors >> 24);
    id[120] = (uint8_t)sectors; id[121] = (uint8_t)(sectors >> 8);
    id[122] = (uint8_t)(sectors >> 16);                /* word60-61: LBA28 total sectors */
    id[123] = (uint8_t)(sectors >> 24);
    ata_set_data(s, id, sizeof(id));
}

/* Executes an ATA task-file command (the value written to the primary
 * channel's command register, port 0x1F7). Only the handful of commands a
 * legacy BIOS/INT13h driver actually issues against a plain fixed disk are
 * implemented; anything else aborts like a real drive would for an
 * unsupported command. */
static void ata_command(Ia64I2000State *s, uint8_t cmd) {
    if (getenv("ATA_DEBUG"))
        fprintf(stderr, "i2000: ATA command %#04x device=%#04x lba=%u "
                "count=%u ninsts=%" PRIu64 "\n", cmd, s->ata_device,
                (unsigned)ata_lba(s), s->ata_count, s->cpu->ninsts);
    s->ata_error = 0;
    switch (cmd) {
    case 0xEC: /* IDENTIFY DEVICE */
        if (!s->hda) { s->ata_error = 0x04; s->ata_status = 0x41; break; }
        ata_identify(s);
        break;
    case 0x20: case 0x21: { /* READ SECTORS, with/without retry */
        if (!s->hda) { s->ata_error = 0x04; s->ata_status = 0x41; break; }
        uint32_t lba = ata_lba(s);
        uint32_t count = s->ata_count ? s->ata_count : 256;
        size_t len = (size_t)count * 512;
        uint8_t *buf = malloc(len);
        if (!buf || fseek(s->hda, (long)((uint64_t)lba * 512), SEEK_SET) != 0 ||
            fread(buf, 1, len, s->hda) != len) {
            free(buf);
            s->ata_error = 0x40; /* UNC: uncorrectable, e.g. past end of disk */
            s->ata_status = 0x41;
        } else {
            ata_set_data(s, buf, len);
            free(buf);
        }
        break;
    }
    case 0x30: case 0x31: /* WRITE SECTORS, with/without retry */
        if (!s->hda) { s->ata_error = 0x04; s->ata_status = 0x41; break; }
        s->ata_write_lba = ata_lba(s);
        s->ata_data_is_write = true;
        ata_set_data(s, NULL, (size_t)(s->ata_count ? s->ata_count : 256) * 512);
        break;
    case 0x90: /* EXECUTE DEVICE DIAGNOSTIC */
        s->ata_error = 0x01; /* device 0 passed, no device 1 present */
        s->ata_status = 0x40;
        break;
    case 0x91: /* INITIALIZE DEVICE PARAMETERS (legacy CHS setup) */
    case 0xE0: case 0xE1: case 0xE6: case 0xE7: case 0xEA: /* standby/idle/
                                                             * flush cache */
        s->ata_status = s->hda ? 0x40 : 0x00;
        break;
    default:
        fprintf(stderr, "i2000: ATA unsupported command %#04x\n", cmd);
        s->ata_error = 0x04; /* ABRT */
        s->ata_status = 0x41;
        break;
    }
}

/* Chipset-internal devices per Table 2-1: SAC (00h/01h), SDC (04h), and
 * Expander 0-3 buses a/b (10h-17h, where WXB/PXB/GXB actually live).
 * Memory Card A/B (05h/06h) are deliberately excluded - they're handled
 * by the separate memcard_cfg array below, and must be checked first by
 * callers or the (also dev-present) memcard reads/writes never fire.
 * Function 0 is always present for SAC/SDC/Expanders; other functions are
 * probed live (SAC dev 1 responds on fn 1 too) and default to an all-zero
 * backing store until we learn what firmware expects there. Making the
 * Expanders present (routed to a zeroed chipset_cfg entry instead of the
 * unhandled-access all-1s default) matters: firmware's memory-size setup
 * probes reg 0x98 on each of them and folds the result into a top-of-memory
 * computation, and an all-1s "no device" response there was read as a huge
 * bogus size, turning a bounded memclr loop into a multi-billion-iteration
 * one. */
static bool chipset_device_present(unsigned dev) {
    return dev == 0 || dev == 1 || dev == 4 || (dev >= 0x10 && dev <= 0x17);
}

enum {
    INT10_AX, INT10_BX, INT10_CX, INT10_DX,
    INT10_DI, INT10_ES, INT10_EXEC, INT10_DATA
};
#define INT10_IO_BASE 0x1e0u
#define INT10_TRIGGER 0x4941u

typedef struct I2000VbeMode {
    uint16_t number, width, height;
    uint8_t bpp;
} I2000VbeMode;

static const I2000VbeMode i2000_vbe_modes[] = {
    { 0x111, 640, 480, 16 }, { 0x112, 640, 480, 24 },
    { 0x114, 800, 600, 16 }, { 0x115, 800, 600, 24 },
    { 0x117, 1024, 768, 16 }, { 0x118, 1024, 768, 24 },
    { 0x11a, 1280, 1024, 16 }, { 0x11b, 1280, 1024, 24 },
    { 0x141, 640, 400, 32 }, { 0x142, 640, 480, 32 },
    { 0x143, 800, 600, 32 }, { 0x144, 1024, 768, 32 },
    { 0x145, 1280, 1024, 32 },
};

static const uint8_t int10_handler[] = {
    0x55,0x89,0xe5,0x50,0x52,0xba,0xe0,0x01,0xef,0x83,0xc2,0x02,
    0x89,0xd8,0xef,0x83,0xc2,0x02,0x89,0xc8,0xef,0x83,0xc2,0x02,
    0x8b,0x46,0xfc,0xef,0x83,0xc2,0x02,0x89,0xf8,0xef,0x83,0xc2,
    0x02,0x8c,0xc0,0xef,0x83,0xc2,0x02,0x81,0x7e,0xfe,0x00,0x4f,
    0x75,0x0f,0x83,0xc2,0x02,0x26,0x8b,0x05,0xef,0x26,0x8b,0x45,
    0x02,0xef,0x83,0xea,0x02,0xb8,0x41,0x49,0xef,0xed,0x89,0xc1,
    0xe3,0x0a,0x57,0x83,0xc2,0x02,0xfc,0xed,0xab,0xe2,0xfc,0x5f,
    0xba,0xe0,0x01,0xed,0x89,0x46,0xfe,0x83,0xc2,0x02,0xed,0x89,
    0xc3,0x83,0xc2,0x02,0xed,0x89,0xc1,0x83,0xc2,0x02,0xed,0x89,
    0xc2,0x8b,0x46,0xfe,0x89,0xec,0x5d,0xcf
};
static const uint8_t int10_rom_init[] = {
    0x50,0x1e,0x31,0xc0,0x8e,0xd8,0xc7,0x06,0x40,0x00,0x00,0x01,
    0xc7,0x06,0x42,0x00,0x00,0xc0,0x1f,0x58,0xcb
};

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void i2000_make_int10_rom(uint8_t rom[512]) {
    memset(rom, 0, 512);
    rom[0] = 0x55; rom[1] = 0xaa; rom[2] = 1;
    memcpy(rom + 3, int10_rom_init, sizeof(int10_rom_init));
    put16(rom + 0x18, 0x20);
    memcpy(rom + 0x20, "PCIR", 4);
    put16(rom + 0x24, RAGE128_VENDOR_ID);
    put16(rom + 0x26, RAGE128_DEVICE_ID);
    put16(rom + 0x2a, 0x18);
    rom[0x2f] = 0x03; put16(rom + 0x30, 1); put16(rom + 0x32, 0x0100);
    rom[0x35] = 0x80;
    put16(rom + 0x48, 0x80); put16(rom + 0xb0, 0xc0);
    put16(rom + 0xc8, 23000); put16(rom + 0xce, 2700);
    put16(rom + 0xd0, 4); put32(rom + 0xd2, 12000); put32(rom + 0xd6, 35000);
    memcpy(rom + 0x100, int10_handler, sizeof(int10_handler));
    memcpy(rom + 0x180, "GEMU IA64 VBE", 14);
    memcpy(rom + 0x190, "GEMU", 5);
    memcpy(rom + 0x1a0, "IA64 Rage128 VBE", 17);
    memcpy(rom + 0x1c0, "1.0", 4);
    for (size_t i = 0; i < sizeof(i2000_vbe_modes)/sizeof(i2000_vbe_modes[0]); i++)
        put16(rom + 0x1d0 + i * 2, i2000_vbe_modes[i].number);
    put16(rom + 0x1d0 + sizeof(i2000_vbe_modes)/sizeof(i2000_vbe_modes[0]) * 2,
          0xffff);
    uint8_t sum = 0;
    for (size_t i = 0; i < 511; i++) sum = (uint8_t)(sum + rom[i]);
    rom[511] = (uint8_t)-sum;
}

static void i2000_load_vga_option_rom(Ia64I2000State *s) {
    memset(s->vga_rom_shadow, 0xff, sizeof(s->vga_rom_shadow));
    memcpy(s->vga_rom_shadow, vgabios_rom, vgabios_rom_len);
    if (s->rage128.enabled) {
        /* Keep the proven SeaVGABIOS execution path, but make its PCI data
         * agree with the Rage128 function firmware discovered at 00:05.0. */
        for (size_t i = 0; i + 8 <= vgabios_rom_len; i++) {
            if (!memcmp(s->vga_rom_shadow + i, "PCIR", 4)) {
                put16(s->vga_rom_shadow + i + 4, RAGE128_VENDOR_ID);
                put16(s->vga_rom_shadow + i + 6, RAGE128_DEVICE_ID);
                break;
            }
        }
        uint8_t sum = 0;
        size_t len = (size_t)s->vga_rom_shadow[2] * 512;
        for (size_t i = 0; i < len; i++)
            sum = (uint8_t)(sum + s->vga_rom_shadow[i]);
        s->vga_rom_shadow[len - 1] -= sum;
    }
}

static const I2000VbeMode *int10_find_mode(uint16_t number) {
    number &= 0x01ff;
    for (size_t i = 0; i < sizeof(i2000_vbe_modes)/sizeof(i2000_vbe_modes[0]); i++)
        if (i2000_vbe_modes[i].number == number) return &i2000_vbe_modes[i];
    return NULL;
}

static uint32_t int10_rom_ptr(uint16_t off) { return 0xc0000000u | off; }

static void int10_response(Ia64I2000State *s, size_t len) {
    memset(s->int10_response, 0, sizeof(s->int10_response));
    s->int10_response_len = (uint16_t)len;
    s->int10_response_pos = 0;
}

static void int10_program_mode(Ia64I2000State *s, const I2000VbeMode *m,
                               bool no_clear) {
    rage128_program_mode(&s->rage128, m->width, m->height, m->bpp,
                         no_clear);
    s->int10_mode = m->number;
    if (getenv("RAGE128_DEBUG"))
        fprintf(stderr, "i2000: INT10 set VBE mode %#x %ux%ux%u\n",
                m->number, m->width, m->height, m->bpp);
}

static void int10_execute(Ia64I2000State *s) {
    s->int10_res = s->int10_req;
    int10_response(s, 0);
    uint16_t ax = s->int10_req.ax;
    if (getenv("RAGE128_DEBUG"))
        fprintf(stderr, "i2000: INT10 ax=%04x bx=%04x cx=%04x dx=%04x\n",
                ax, s->int10_req.bx, s->int10_req.cx, s->int10_req.dx);
    if ((ax & 0xff00) == 0x4f00) {
        switch (ax & 0xff) {
        case 0x00: {
            size_t len = s->int10_signature == 0x32454256u ? 512 : 256;
            int10_response(s, len);
            uint8_t *p = s->int10_response;
            memcpy(p, "VESA", 4); put16(p + 4, 0x0300);
            put32(p + 6, int10_rom_ptr(0x180));
            put32(p + 14, int10_rom_ptr(0x1d0));
            put16(p + 18, RAGE128_VRAM_SIZE >> 16);
            put16(p + 20, 0x0100); put32(p + 22, int10_rom_ptr(0x190));
            put32(p + 26, int10_rom_ptr(0x1a0));
            put32(p + 30, int10_rom_ptr(0x1c0));
            s->int10_res.ax = 0x004f;
            break;
        }
        case 0x01: {
            const I2000VbeMode *m = int10_find_mode(s->int10_req.cx);
            if (!m) { s->int10_res.ax = 0x014f; break; }
            int10_response(s, 256);
            uint8_t *p = s->int10_response;
            unsigned bytes = (m->bpp + 7) / 8;
            uint32_t image = (uint32_t)m->width * m->height * bytes;
            unsigned pages = RAGE128_VRAM_SIZE / ((image + 0xffff) & ~0xffffu);
            put16(p, 0x00bb); p[2] = 7; put16(p + 4, 64); put16(p + 6, 64);
            put16(p + 8, 0xa000); put16(p + 16, m->width * bytes);
            put16(p + 18, m->width); put16(p + 20, m->height);
            p[22]=8; p[23]=16; p[24]=1; p[25]=m->bpp; p[26]=1; p[27]=6;
            p[28]=64; p[29]=(uint8_t)(pages ? pages-1 : 0); p[30]=1;
            p[31]=m->bpp==16?5:8; p[32]=m->bpp==16?11:16;
            p[33]=m->bpp==16?6:8; p[34]=m->bpp==16?5:8;
            p[35]=m->bpp==16?5:8; p[37]=m->bpp==32?8:0;
            p[38]=m->bpp==32?24:0; p[39]=m->bpp==32?2:0;
            put32(p + 40, RAGE128_FB_BASE); put16(p + 50, m->width * bytes);
            p[52]=p[53]=p[29]; memcpy(p + 54, p + 31, 8);
            s->int10_res.ax = 0x004f;
            break;
        }
        case 0x02: {
            const I2000VbeMode *m = int10_find_mode(s->int10_req.bx);
            if (!m) s->int10_res.ax = 0x014f;
            else { int10_program_mode(s, m, s->int10_req.bx & 0x8000);
                   s->int10_res.ax = 0x004f; }
            break;
        }
        case 0x03:
            s->int10_res.bx = s->int10_mode ? s->int10_mode | 0x4000 : 3;
            s->int10_res.ax = 0x004f; break;
        case 0x05: case 0x06: case 0x07:
            s->int10_res.ax = 0x004f; break;
        case 0x10:
            if ((s->int10_req.bx & 0xff) == 0) s->int10_res.bx = 0x0f30;
            s->int10_res.ax = 0x004f; break;
        default: s->int10_res.ax = 0x024f; break;
        }
    } else if ((ax >> 8) == 0x00) {
        s->int10_mode = 0;
    } else if ((ax >> 8) == 0x0f) {
        s->int10_res.ax = s->int10_mode ? (80u << 8 | 3) : (80u << 8 | 3);
        s->int10_res.bx &= 0xff;
    } else if ((ax >> 8) == 0x1a && !(ax & 0xff)) {
        s->int10_res.ax = 0x001a; s->int10_res.bx = 8;
    }
}

static uint64_t int10_io_read(Ia64I2000State *s, unsigned reg) {
    switch (reg) {
    case INT10_AX: return s->int10_res.ax; case INT10_BX: return s->int10_res.bx;
    case INT10_CX: return s->int10_res.cx; case INT10_DX: return s->int10_res.dx;
    case INT10_DI: return s->int10_res.di; case INT10_ES: return s->int10_res.es;
    case INT10_EXEC: return s->int10_response_len / 2;
    case INT10_DATA:
        if (s->int10_response_pos < s->int10_response_len) {
            uint16_t v; memcpy(&v, s->int10_response + s->int10_response_pos, 2);
            s->int10_response_pos += 2; return v;
        }
        return 0;
    default: return 0xffff;
    }
}

static void int10_io_write(Ia64I2000State *s, unsigned reg, uint16_t v) {
    switch (reg) {
    case INT10_AX: s->int10_req.ax=v; s->int10_signature=0;
                   s->int10_signature_words=0; break;
    case INT10_BX: s->int10_req.bx=v; break; case INT10_CX: s->int10_req.cx=v; break;
    case INT10_DX: s->int10_req.dx=v; break; case INT10_DI: s->int10_req.di=v; break;
    case INT10_ES: s->int10_req.es=v; break;
    case INT10_EXEC: if (v == INT10_TRIGGER) int10_execute(s); break;
    case INT10_DATA:
        if (s->int10_signature_words < 2) {
            s->int10_signature |= (uint32_t)v << (s->int10_signature_words * 16);
            s->int10_signature_words++;
        }
        break;
    }
}

static uint64_t pci_cfg_read(Ia64I2000State *s, unsigned lane, unsigned size) {
    uint32_t a = s->pci_cfg_addr;
    if (!(a & 0x80000000u) || lane + size > 4)
        return size_mask(size);
    unsigned bus = (a >> 16) & 0xFF;
    unsigned dev = (a >> 11) & 0x1F;
    unsigned fun = (a >> 8) & 7;
    unsigned reg = (a & 0xFC) + lane;
    if (s->rage128.enabled && bus == 0 && dev == RAGE128_PCI_DEV &&
        fun == 0 && reg + size <= sizeof(s->rage128.cfg))
        return rage128_pci_read(&s->rage128, reg, size);
    if (s->i82559.enabled && bus == 0 && dev == I82559_PCI_DEV &&
        fun == 0 && reg + size <= sizeof(s->i82559.cfg)) {
        if (getenv("I82559_DEBUG"))
            fprintf(stderr, "i82559: pci read reg=%02x size=%u\n", reg, size);
        return i82559_pci_read(&s->i82559, reg, size);
    }
    if (bus == 0 && dev == CMD649_PCI_DEV && fun == 0 &&
        reg + size <= sizeof(s->ifb_cfg)) {
        uint64_t v = 0;
        memcpy(&v, &s->ifb_cfg[reg], size);
        return v;
    }
    if (bus == 0 && dev == CMD649_PCI_DEV && (fun == 2 || fun == 3)) {
        uint8_t *cfg = fun == 2 ? s->ifb_usb_cfg : s->ifb_smbus_cfg;
        uint64_t v = 0;
        memcpy(&v, &cfg[reg], size);
        if (fun == 3 && reg <= 4 && reg + size > 4) {
            /* Command register bit 0 (I/O space enable) doubles as a
             * software-driven busy pulse for the SMBus host controller's
             * retry loop: it writes 1, then polls this same bit forever
             * waiting for hardware to clear it, with no further writes in
             * the steady state - so a fixed "clear the Nth read" model
             * can't work; nothing ever arms a later read. The very first
             * ever read (confirming the enable write stuck) needs to see
             * the true set value; every read after that reports bit 0
             * clear unconditionally, so the poll succeeds immediately. */
            if (s->ifb_smbus_cmd_read_once)
                v &= ~(UINT64_C(1) << ((4 - reg) * 8));
            s->ifb_smbus_cmd_read_once = true;
        }
        return v;
    }
    if (bus == 0 && dev == CMD649_PCI_DEV && fun == CMD649_PCI_FUN &&
        reg + size <= sizeof(s->cmd649_cfg)) {
        uint64_t v = 0;
        memcpy(&v, &s->cmd649_cfg[reg], size);
        if (atapi_debug_enabled() && reg < 4) {
            static unsigned identity_reads;
            if (identity_reads++ < 16)
                fprintf(stderr, "i2000: ATAPI controller PCI identity read "
                        "reg=%#x size=%u -> %#" PRIx64 "\n", reg, size, v);
        }
        return v;
    }

    /* On bus zero, device 10h is the SAC's special CBN programming
     * endpoint.  Firmware uses byte register 40h to select the bus on
     * which the rest of the 460GX components appear. */
    if (bus == 0 && dev == 0x10 && fun == 0 && reg == 0x40 && size == 1)
        return s->chipset_bus;
    if (bus == s->chipset_bus && chipset_device_present(dev) &&
        reg + size <= 256) {
        uint64_t v = 0;
        memcpy(&v, &s->chipset_cfg[dev][fun][reg], size);
        return v;
    }
    if (bus == s->chipset_bus && (dev == 5 || dev == 6) &&
        reg + size <= 256) {
        uint64_t v = 0;
        memcpy(&v, &s->memcard_cfg[dev - 5][fun][reg], size);
        return v;
    }

    uint64_t key = 0xC000000000000000ull |
                   ((uint64_t)bus << 24) | ((uint64_t)dev << 19) |
                   ((uint64_t)fun << 16) | reg;
    mmio_log(s, key, 0, size, false);
    return size_mask(size);
}

static void pci_cfg_write(Ia64I2000State *s, unsigned lane,
                          uint64_t val, unsigned size) {
    uint32_t a = s->pci_cfg_addr;
    if (!(a & 0x80000000u) || lane + size > 4)
        return;
    unsigned bus = (a >> 16) & 0xFF;
    unsigned dev = (a >> 11) & 0x1F;
    unsigned fun = (a >> 8) & 7;
    unsigned reg = (a & 0xFC) + lane;
    if (s->rage128.enabled && bus == 0 && dev == RAGE128_PCI_DEV &&
        fun == 0 && reg + size <= sizeof(s->rage128.cfg)) {
        rage128_pci_write(&s->rage128, reg, val, size);
        return;
    }
    if (s->i82559.enabled && bus == 0 && dev == I82559_PCI_DEV &&
        fun == 0 && reg + size <= sizeof(s->i82559.cfg)) {
        i82559_pci_write(&s->i82559, reg, val, size);
        return;
    }
    if (bus == 0 && dev == CMD649_PCI_DEV && fun == 0 &&
        reg + size <= sizeof(s->ifb_cfg)) {
        /* Identity, class code and header type are read-only.  The rest of
         * the IFB LPC/FWH configuration space is mostly retained control
         * latches; firmware programs ACPI/GPIO bases and decode enables
         * here before entering EFI. */
        for (unsigned i = 0; i < size; i++) {
            unsigned off = reg + i;
            if (off >= 4 && !(off >= 8 && off < 16))
                s->ifb_cfg[off] = (uint8_t)(val >> (i * 8));
        }
        return;
    }
    if (bus == 0 && dev == CMD649_PCI_DEV && (fun == 2 || fun == 3)) {
        uint8_t *cfg = fun == 2 ? s->ifb_usb_cfg : s->ifb_smbus_cfg;
        for (unsigned i = 0; i < size; i++) {
            unsigned off = reg + i;
            if (off >= 4 && !(off >= 8 && off < 16))
                cfg[off] = (uint8_t)(val >> (i * 8));
        }
        return;
    }
    if (bus == 0 && dev == CMD649_PCI_DEV && fun == CMD649_PCI_FUN &&
        reg + size <= sizeof(s->cmd649_cfg)) {
        /* Identity, revision/class and header type are read-only.  Command,
         * BAR and CMD timing registers are ordinary retained latches for the
         * empty-controller model. */
        for (unsigned i = 0; i < size; i++) {
            unsigned off = reg + i;
            if (off >= 4 && !(off >= 8 && off < 16))
                s->cmd649_cfg[off] = (uint8_t)(val >> (i * 8));
        }
        return;
    }
    if (bus == 0 && dev == 0x10 && fun == 0 && reg == 0x40 && size == 1) {
        s->chipset_bus = (uint8_t)val;
        return;
    }
    if (bus == s->chipset_bus && chipset_device_present(dev) &&
        reg + size <= 256) {
        /* Device/vendor ID and class/revision are read-only on function 0
         * (real identity); other functions have no such fixed header yet,
         * so leave them fully writable until we know their layout. */
        for (unsigned i = 0; i < size; i++) {
            unsigned off = reg + i;
            if (fun != 0 || (off >= 4 && !(off >= 8 && off < 12)))
                s->chipset_cfg[dev][fun][off] = (uint8_t)(val >> (i * 8));
        }
        return;
    }
    if (bus == s->chipset_bus && (dev == 5 || dev == 6) &&
        reg + size <= 256) {
        memcpy(&s->memcard_cfg[dev - 5][fun][reg], &val, size);
        return;
    }
    uint64_t key = 0xC000000000000000ull |
                   ((uint64_t)bus << 24) | ((uint64_t)dev << 19) |
                   ((uint64_t)fun << 16) | reg;
    mmio_log(s, key, val & size_mask(size), size, true);
}

/* ── Serial console ──────────────────────────────────────────────────────── */

static void con_newline(Ia64I2000State *s) {
    if (getenv("CON_TIME_DEBUG") && s->console[s->con_row][0])
        fprintf(stderr, "i2000: CON [%s] itc=%" PRIu64 " ninsts=%" PRIu64 "\n",
                s->console[s->con_row], merced_get_itc(s->cpu), s->cpu->ninsts);
    s->con_col = 0;
    if (++s->con_row >= CON_ROWS) {
        memmove(s->console[0], s->console[1], sizeof(s->console) - sizeof(s->console[0]));
        memset(s->console[CON_ROWS - 1], 0, sizeof(s->console[0]));
        s->con_row = CON_ROWS - 1;
    }
}

static void con_putc(Ia64I2000State *s, char c) {
    s->con_dirty = true;
    if (c == '\r') { s->con_col = 0; return; }
    if (c == '\n') { con_newline(s); return; }
    if (c < 32 || c > 126) c = '.';
    if (s->con_col >= COLS) con_newline(s);
    s->console[s->con_row][s->con_col++] = c;
    s->console[s->con_row][s->con_col] = 0;
    if (getenv("CDB_FAIL_TRACE") &&
        strstr(s->console[s->con_row], "failed to update CDB")) {
        static bool traced;
        if (!traced) {
            traced = true;
            fprintf(stderr, "i2000: CDB failure text emitted, ip=%016" PRIx64
                    " ninsts=%" PRIu64 " call history:\n", s->cpu->ip,
                    s->cpu->ninsts);
            merced_dump_calls(s->cpu, 600, stderr);
        }
    }
    if (getenv("CDB_FAIL_TRACE") &&
        strstr(s->console[s->con_row], "CDBFMM.c")) {
        static bool assertion_traced;
        if (!assertion_traced) {
            assertion_traced = true;
            fprintf(stderr, "i2000: CDBFMM assertion emitted, ip=%016" PRIx64
                    " ninsts=%" PRIu64 " call history:\n", s->cpu->ip,
                    s->cpu->ninsts);
            merced_dump_calls(s->cpu, 600, stderr);
        }
    }
    if (getenv("MFG_TRACE") &&
        strstr(s->console[s->con_row], "MFG Mode detected")) {
        static bool traced;
        if (!traced) {
            traced = true;
            fprintf(stderr, "i2000: MFG Mode text emitted, ip=%016" PRIx64
                    " ninsts=%" PRIu64 " call history:\n", s->cpu->ip,
                    s->cpu->ninsts);
            merced_dump_calls(s->cpu, 600, stderr);
        }
    }
    if (getenv("MFG_TRACE") &&
        strstr(s->console[s->con_row], "Entering CDB Save")) {
        static bool traced;
        if (!traced) {
            traced = true;
            fprintf(stderr, "i2000: Entering-CDB-Save text emitted, ip=%016"
                    PRIx64 " ninsts=%" PRIu64 " call history:\n", s->cpu->ip,
                    s->cpu->ninsts);
            merced_dump_calls(s->cpu, 600, stderr);
        }
    }
}

/* ── Physical bus ────────────────────────────────────────────────────────── */

static void mmio_log(Ia64I2000State *s, uint64_t addr, uint64_t val,
                     unsigned size, bool is_write) {
    for (int i = 0; i < s->mmio_log_n; i++) {
        if (s->mmio_log[i].addr == addr && s->mmio_log[i].is_write == is_write) {
            s->mmio_log[i].count++;
            s->mmio_log[i].val = val;
            return;
        }
    }
    if (s->mmio_log_n < MMIO_LOG_N) {
        MmioLogEnt *e = &s->mmio_log[s->mmio_log_n++];
        e->addr = addr; e->val = val; e->count = 1;
        e->is_write = is_write; e->size = (uint8_t)size;
        fprintf(stderr, "i2000: unhandled %s pa=0x%012" PRIX64 " size=%u"
                        "%s0x%" PRIX64 "\n",
                is_write ? "write" : "read ", addr, size,
                is_write ? " val=" : " -> ", is_write ? val : ~0ull);
    } else {
        /* keep counting in the last slot so the panel shows activity */
        s->mmio_log[MMIO_LOG_N - 1].count++;
    }
}

static bool vga_port(uint64_t port) {
    return port == 0x3B4 || port == 0x3B5 || port == 0x3BA ||
           (port >= 0x3C0 && port <= 0x3CF) ||
           port == 0x3D4 || port == 0x3D5 || port == 0x3DA;
}

static uint8_t cmos_bcd(unsigned v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* The static cmos[] array (fixed at reset, never touched again) satisfies
 * anything that just wants a valid-looking byte back, but firmware
 * commonly calibrates or synchronizes against the RTC by reading a time
 * register, then re-reading it later and waiting for the VALUE to change -
 * a permanently frozen clock makes that an infinite wait no matter what
 * byte comes back. Refresh the standard time/date registers (0-9) from
 * real host time on every access so any such wait resolves within a real
 * second or so. Status register A's UIP (bit 7, update-in-progress) is
 * left at 0 always rather than pulsed, which is a fine approximation:
 * real hardware only asserts it for ~244us out of every second, so a
 * poll for "not updating" would see it clear almost immediately anyway. */
static void cmos_sync_live_clock(Ia64I2000State *s) {
    time_t t;
    const char *fixed = getenv("GEMU_FIXED_TIME");
    if (fixed) {
        /* Real wall-clock time makes cold boots non-reproducible run to
         * run - firmware that reads the RTC into a computation (not just
         * displaying it) can diverge between two otherwise-identical
         * boots. Advance a fixed base by simulated elapsed seconds
         * (approximated from instruction count) instead, so calibration
         * loops waiting for the clock to change still terminate, but
         * deterministically. */
        t = (time_t)strtoll(fixed, NULL, 10) +
            (time_t)(s->cpu->ninsts / UINT64_C(2000000));
    } else {
        t = time(NULL);
    }
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    s->cmos[0x00] = cmos_bcd((unsigned)tmv.tm_sec);
    s->cmos[0x02] = cmos_bcd((unsigned)tmv.tm_min);
    s->cmos[0x04] = cmos_bcd((unsigned)tmv.tm_hour);
    s->cmos[0x06] = (uint8_t)(tmv.tm_wday + 1);
    s->cmos[0x07] = cmos_bcd((unsigned)tmv.tm_mday);
    s->cmos[0x08] = cmos_bcd((unsigned)(tmv.tm_mon + 1));
    s->cmos[0x09] = cmos_bcd((unsigned)(tmv.tm_year % 100));
}

/* SMBus host controller (IFB function 3) I/O-register access - see the
 * SMB_* offsets/bits defined near I2000_IO_BASE. Every transaction
 * completes synchronously (there's no real bus latency to model), so
 * HOST_BUSY is always seen clear and status is ready the instant a comand
 * finishes. */
static uint64_t smbus_reg_read(Ia64I2000State *s, uint64_t off, unsigned size) {
    if (size != 1)
        return size_mask(size);
    switch (off) {
    case SMB_HSTSTS:  return s->smb_hststs;
    case SMB_HSTCNT:  return 0;  /* START always reads back 0 */
    case SMB_HSTCMD:  return s->smb_hstcmd;
    case SMB_HSTADD:  return s->smb_hstadd;
    case SMB_HSTDAT0: return s->smb_hstdat0;
    case SMB_HSTDAT1: return s->smb_hstdat1;
    default: return size_mask(size);
    }
}

static void smbus_reg_write(Ia64I2000State *s, uint64_t off, uint64_t val,
                            unsigned size) {
    if (size != 1)
        return;
    switch (off) {
    case SMB_HSTSTS:
        s->smb_hststs &= (uint8_t)~val;  /* write-1-to-clear */
        return;
    case SMB_HSTCMD:  s->smb_hstcmd  = (uint8_t)val; return;
    case SMB_HSTADD:  s->smb_hstadd  = (uint8_t)val; return;
    case SMB_HSTDAT0: s->smb_hstdat0 = (uint8_t)val; return;
    case SMB_HSTDAT1: s->smb_hstdat1 = (uint8_t)val; return;
    case SMB_HSTCNT: {
        if (!(val & SMB_HSTCNT_START))
            return;
        unsigned protocol = (unsigned)((val >> 2) & 7);
        unsigned slave = (unsigned)(s->smb_hstadd >> 1);
        bool is_read = (s->smb_hstadd & 1) != 0;
        unsigned dimm = slave - I2000_SPD_BASE_ADDR;
        bool present = dimm < I2000_SPD_SLOTS && s->spd_present[dimm];
        if (getenv("SMBUS_DEBUG"))
            fprintf(stderr, "i2000: SMBUS start slave=%#x rw=%s proto=%u "
                    "cmd=%#x dimm=%u present=%d\n", slave,
                    is_read ? "read" : "write", protocol, s->smb_hstcmd,
                    dimm, present);
        if (!present) {
            s->smb_hststs |= SMB_HSTSTS_DEV_ERR | SMB_HSTSTS_INTER;
            return;
        }
        if (protocol == SMB_PROT_QUICK) {
            s->smb_hststs |= SMB_HSTSTS_INTER;
            return;
        }
        if (protocol == SMB_PROT_BYTE_DATA) {
            if (is_read)
                s->smb_hstdat0 = s->spd[dimm][s->smb_hstcmd];
            /* SPD EEPROMs are effectively read-only here (no firmware
             * write-back path needs modeling); accept writes silently. */
            s->smb_hststs |= SMB_HSTSTS_INTER;
            return;
        }
        s->smb_hststs |= SMB_HSTSTS_DEV_ERR | SMB_HSTSTS_INTER;
        return;
    }
    default:
        return;
    }
}

/* The SMBus base is whatever firmware programs into SMBBA (ifb_smbus_cfg
 * offset 0x20-0x23, bits[15:4]) - see smbus_reg_read/write's callers. */
static bool smbus_port_offset(Ia64I2000State *s, uint64_t port, uint64_t *off) {
    uint32_t bar;
    memcpy(&bar, &s->ifb_smbus_cfg[0x20], sizeof(bar));
    if (!(bar & 1))
        return false;  /* SMBBA not yet programmed as an I/O BAR */
    uint32_t base = bar & 0xFFF0u;
    if (base == 0 || port < base || port - base > SMB_REGSPAN)
        return false;
    *off = port - base;
    return true;
}

static uint64_t io_port_read(Ia64I2000State *s, uint64_t port, unsigned size) {
    static unsigned debug_reads;
    if (s->rage128.enabled && size == 2 && port >= INT10_IO_BASE &&
        port < INT10_IO_BASE + 16 && !(port & 1))
        return int10_io_read(s, (unsigned)(port - INT10_IO_BASE) / 2);
    if (s->rage128.enabled) {
        uint32_t base = rage128_bar(&s->rage128, 0x14, RAGE128_IO_BASE,
                                   ~UINT32_C(0xff));
        if (port >= base && port + size <= base + 0x100)
            return rage128_mmio_read(&s->rage128,
                                     (unsigned)(port - base), size);
    }
    if (s->i82559.enabled) {
        uint32_t base = i82559_bar(&s->i82559, 0x14, I82559_IO_BASE,
                                  ~UINT32_C(0x3f));
        if (port >= base && port + size <= base + I82559_IO_SIZE)
            return i82559_reg_read(&s->i82559, (unsigned)(port - base), size);
    }
    if (size == 1 && vga_port(port))
        return vga_ibm_io_read(&s->vga, (uint16_t)port);
    {
        uint64_t off;
        if (smbus_port_offset(s, port, &off))
            return smbus_reg_read(s, off, size);
    }
    if (port >= 0x1000 && port + size <= 0x1008) {
        /* Firmware busy-polls a status byte here waiting for bit 0 to
         * clear before SMBBA gets programmed (see smbus_port_offset()
         * above, which takes over this same port range once it has) - or
         * for whatever else turns out to share this range; what real
         * hardware is mapped at this port outside of SMBus isn't
         * established. No real device backs it otherwise, so read as
         * idle/complete unconditionally rather than falling through to
         * the generic all-ones "unmapped" response, which left bit 0
         * permanently set and the poll spinning forever. */
        return 0;
    }
    /* Post-IBV firmware polls this byte before entering the presentation
     * application.  PCI resource allocation places it at 0x1110 without
     * the optional NIC and at 0x1160 when the i82559 consumes the preceding
     * I/O aperture.  No asynchronous device is currently modeled behind
     * this legacy-I/O status register, so its quiescent value is zero.
     * Returning floating-bus 0xff leaves its busy/error bits asserted and
     * traps firmware in a tight retry loop. */
    if ((port == 0x1110 || port == 0x1160) && size == 1)
        return 0;
    if (port == 0x60 && size == 1) {               /* 8042 data */
        /* A real 8042 has one ordered output buffer.  Our keyboard and aux
         * queues are separate, so preserve a mouse packet already exposed
         * through AUX/IRQ12: letting a later keyboard/controller byte jump
         * ahead here substitutes it for a packet byte and destroys PS/2
         * framing (random X/Y sign reversals are the visible result). */
        if (s->aux_out_pos >= s->aux_out_len &&
            s->kbc_out_pos < s->kbc_out_len) {
            uint8_t v = s->kbc_out[s->kbc_out_pos++];
            if (s->kbc_out_pos == s->kbc_out_len)
                s->kbc_out_pos = s->kbc_out_len = 0;
            return v;
        }
        if (s->aux_out_pos >= s->aux_out_len &&
            ps2_keyboard_has_data(&s->ps2_keyboard))
            return ps2_keyboard_read(&s->ps2_keyboard);
        if (s->aux_out_pos < s->aux_out_len) {
            uint8_t v = s->aux_out[s->aux_out_pos++];
            if (getenv("MOUSE_DEBUG"))
                fprintf(stderr, "MOUSE_DEBUG: port60 read popped %02x "
                        "pos=%u len=%u ninsts=%" PRIu64 "\n", v,
                        s->aux_out_pos, s->aux_out_len, s->cpu->ninsts);
            if (s->aux_out_pos == s->aux_out_len)
                s->aux_out_pos = s->aux_out_len = 0;
            /* Do NOT re-raise here. This firmware's ISR reads port 0x60
             * BEFORE the legacy-ack address, all within the same dispatch
             * cycle - a re-raise issued right here gets immediately
             * consumed by that same cycle's own ack read (which
             * unconditionally clears mouse_irq_pending), never surviving
             * long enough to trigger a fresh ExtINT for the next byte.
             * Confirmed live: aux_out kept growing to its 16-byte cap with
             * aux_out_pos stuck at 0/1 forever under a rapid packet burst,
             * because each "re-raise" here was cancelled microseconds
             * later by the same ISR entry's own ack read. The level-
             * triggered re-fire instead happens in the ack handler below,
             * which runs *after* this read and can see whether data still
             * remains. */
            return v;
        }
        return 0;
    }
    if (port == 0x64 && size == 1) {               /* 8042 status */
        /* Commands are consumed synchronously, so IBF (bit 1) is clear.
         * OBF reflects queued controller/keyboard response bytes; bit 5
         * (0x20) tells the firmware the byte port 0x60 will return next
         * came from the aux/mouse device rather than the keyboard - only
         * meaningful once a byte is actually queued there, so this is
         * automatically inert when no mouse is attached (aux_out_len can
         * never become nonzero without -device mouse). Keyboard bytes win
         * priority when both queues happen to be nonempty. */
        bool aux_next = s->aux_out_pos < s->aux_out_len;
        uint8_t st64 = ((s->kbc_out_len || s->aux_out_len ||
                         ps2_keyboard_has_data(&s->ps2_keyboard)) ? 0x01 : 0) |
                       (aux_next ? 0x20 : 0) | 0x04; /* system flag */
        if (getenv("MOUSE_DEBUG"))
            fprintf(stderr, "MOUSE_DEBUG: port64 status read -> %02x "
                    "kbc_len=%u aux_len=%u aux_pos=%u ninsts=%" PRIu64 "\n",
                    st64, s->kbc_out_len, s->aux_out_len, s->aux_out_pos,
                    s->cpu->ninsts);
        return st64;
    }
    if ((port == 0x71 || port == 0x73) && size == 1)
    {
        /* 72h/73h is the i2000 IFB's own RTC/CFGRAM alias; 70h/71h is the
         * universal, standard AT-compatible CMOS index/data pair that
         * every x86-compatible BIOS (including the real-mode setup code
         * this firmware runs before entering native SAL/EFI) expects to
         * work regardless of platform, so both need to reach the same
         * underlying state. */
        if ((s->cmos_index & 0x7F) < 0x0A)
            cmos_sync_live_clock(s);
        uint8_t v = s->cmos[s->cmos_index & 0x7F];
        if (getenv("CMOS_DEBUG"))
            fprintf(stderr, "i2000: CMOS read reg=%#04x -> %#04x "
                    "ninsts=%" PRIu64 " itc=%" PRIu64 " ip=%016" PRIx64 "\n",
                    s->cmos_index & 0x7F, v, s->cpu->ninsts,
                    merced_get_itc(s->cpu), s->cpu->ip);
        if ((s->cmos_index & 0x7F) == 0x0A)
            /* Status Register A bit 7 is UIP (update in progress). We don't
             * emulate the real ~244us-per-second update window, and nothing
             * else ever clears whatever got stored here (firmware itself
             * can write this register, and did - a stale 1 bit is exactly
             * what left an earlier boot spinning forever waiting for "not
             * busy"). Force it clear on every read rather than trust
             * whatever's retained; the other status-A bits (divider/rate
             * select) are harmless to read back as stored. */
            v &= ~0x80u;
        if (getenv("MERCED_DEBUG") && debug_reads++ < 128)
            fprintf(stderr, "i2000: CMOS[%02X] -> %02X\n",
                    s->cmos_index & 0x7F, v);
        return v;
    }
    if (port == PCI_CFG_ADDR && size == 4) return s->pci_cfg_addr;
    if (port >= PCI_CFG_DATA && port < PCI_CFG_DATA + 4)
        return pci_cfg_read(s, (unsigned)(port - PCI_CFG_DATA), size);
    if (port >= COM1_PORT && port < COM1_PORT + 8) {
        if (getenv("MERCED_DEBUG") && debug_reads++ < 128)
            fprintf(stderr, "i2000: COM1 read reg %u\n",
                    (unsigned)(port - COM1_PORT));
        switch (port - COM1_PORT) {
        case 0:
            if (s->uart_lcr & 0x80) return s->uart_dll;
            if (s->uart_rx_head != s->uart_rx_tail) {
                uint8_t v = s->uart_rx[s->uart_rx_head++];
                return v;
            }
            return 0;
        case 1: return (s->uart_lcr & 0x80) ? s->uart_dlm : s->uart_ier;
        case 2: return ((s->uart_ier & 1) && s->uart_rx_head != s->uart_rx_tail)
                     ? 0x04 : 0x01;                /* RX data / no interrupt */
        case 3: return s->uart_lcr;
        case 4: return s->uart_mcr;
        case 5: return 0x60 | (s->uart_rx_head != s->uart_rx_tail ? 1 : 0);
        case 6: return 0xB0;                        /* MSR: CTS|DSR|DCD */
        case 7: return s->uart_scr;
        }
    }
    if (port == 0x20 && size == 1) {
        uint8_t irr = 0;
        if (s->pit_irq_pending) irr |= 0x01;
        if (s->keyboard_irq_pending) irr |= 0x02;
        if (s->mouse_irq_pending || s->atapi_irq_pending) irr |= 0x04;
        if (getenv("ATA_IRQ_TRACE"))
            fprintf(stderr, "i2000: PIC master %s read -> %#04x ninsts=%" PRIu64 "\n",
                    s->pic_master_read_isr ? "ISR" : "IRR",
                    s->pic_master_read_isr ? s->pic_master_isr : irr,
                    s->cpu->ninsts);
        return s->pic_master_read_isr ? s->pic_master_isr : irr;
    }
    if (port == 0xA0 && size == 1) {
        uint8_t irr = 0;
        if (s->mouse_irq_pending) irr |= 0x10;
        if (s->atapi_irq_pending) irr |= 0x80;
        if (getenv("ATA_IRQ_TRACE"))
            fprintf(stderr, "i2000: PIC slave %s read -> %#04x ninsts=%" PRIu64 "\n",
                    s->pic_slave_read_isr ? "ISR" : "IRR",
                    s->pic_slave_read_isr ? s->pic_slave_isr : irr,
                    s->cpu->ninsts);
        return s->pic_slave_read_isr ? s->pic_slave_isr : irr;
    }
    if (port == 0x21) return s->pic_master_mask;
    if (port == 0xA1) return s->pic_slave_mask;
    if (port == 0x40 && size == 1) {
        /* EFI reads the 8254 channel-0 count as low byte followed by high
         * byte and uses the difference to calibrate its stall routine.  A
         * constant 0xff (the old unimplemented-port result) makes that
         * difference zero and trips var.c's NumberOfTicks assertion.
         *
         * Tie the counter to executed slots. Exact wall-clock frequency is
         * not important here, but it must count down and both bytes of one
         * read must describe essentially the same sample. */
        uint32_t reload = s->pit0_reload ? s->pit0_reload : 0x10000u;
        uint64_t elapsed = merced_get_itc(s->cpu) - s->pit0_epoch_ninsts;
        uint64_t ticks = (elapsed * I8254_INPUT_HZ) / I2000_ITC_HZ;
        uint16_t count = (uint16_t)(reload - (ticks % reload));
        uint8_t v = s->pit0_read_phase ? (uint8_t)(count >> 8)
                                       : (uint8_t)count;
        s->pit0_read_phase ^= 1;
        return v;
    }
    if (port == 0x404)
        /* System status/GPIO word. Bit 24 reports J29 in its normal running
         * position. Returning zero reports OVRD/CLRCMOS instead, making the
         * SDV firmware stop with "Hardware Override / Power off and reset
         * hardware jumper!". Other strap bits remain deasserted. */
        return UINT32_C(0x01000000);
    if (port >= 0x400 && port + size <= 0x440) {
        uint64_t v = 0;
        memcpy(&v, &s->acpi_io[port - 0x400], size);
        return v;
    }
    if (port == 0x61) {                            /* PIT channel-2 gate/output */
        if (s->pit2_polls < 2) s->pit2_polls++;
        /* Bit 4 is the PIT channel-1 refresh-request toggle. Legacy BIOS
         * delay code waits for both its rising and falling edges; holding
         * it constant traps the IRQ1 handler in a permanent spin. */
        uint8_t refresh = (s->cpu->ninsts >> 8) & 1 ? 0x10 : 0;
        return s->port61 | refresh | (s->pit2_polls >= 2 ? 0x20 : 0);
    }
    if ((port >= 0x170 && port <= 0x177) || port == 0x376) {
        unsigned reg = port == 0x376 ? 7 : (unsigned)(port - 0x170);
        /* Only the secondary-channel master exists.  The firmware probes
         * the slave with IDENTIFY DEVICE; an absent ATA target floats the
         * task file instead of borrowing the master's ATAPI state/IRQ. */
        if ((s->atapi_device & 0x10) != 0)
            return reg == 6 ? s->atapi_device : 0;
        if (reg == 0) {
            uint64_t v = 0;
            size_t pos_before = s->atapi_data_pos;
            for (unsigned i = 0; i < size; i++) {
                if (s->atapi_data_pos < s->atapi_data_len &&
                    s->atapi_data_pos < s->atapi_phase_end)
                    v |= (uint64_t)s->atapi_data[s->atapi_data_pos++] << (i * 8);
            }
            if (getenv("ATA_DATA_TRACE"))
                fprintf(stderr, "i2000: ATAPI data read size=%u pos=%zu/%zu "
                        "-> %#04" PRIx64 " ip=%016" PRIx64 " ninsts=%"
                        PRIu64 "\n", size, pos_before, s->atapi_data_len, v,
                        s->cpu->ip, s->cpu->ninsts);
            if (s->atapi_data_pos >= s->atapi_phase_end &&
                s->atapi_status & 0x08) {
                if (s->atapi_data_pos < s->atapi_data_len) {
                    atapi_begin_data_phase(s);
                    if (atapi_debug_enabled())
                        fprintf(stderr, "i2000: ATAPI next PIO phase pos=%zu "
                                "end=%zu total=%zu\n", s->atapi_data_pos,
                                s->atapi_phase_end, s->atapi_data_len);
                } else {
                    s->atapi_status = 0x40;
                    s->atapi_count = 0x03; /* command complete */
                    s->atapi_lba_mid = s->atapi_lba_high = 0;
                    /* Packet PIO completion is a separate interrupt from
                     * the one that announced the final data phase.  AtaPim
                     * drains DRQ and then waits for this command-complete
                     * phase (CoD=1, I/O=1). */
                    s->atapi_irq_pending = true;
                    s->atapi_irq_ready_ninsts =
                        s->cpu->ninsts + ATAPI_IRQ_LATENCY_NINSTS;
                    if (atapi_debug_enabled())
                        fprintf(stderr, "i2000: ATAPI PIO complete bytes=%zu "
                                "status=%#04x reason=%#04x\n",
                                s->atapi_data_len, s->atapi_status,
                                s->atapi_count);
                }
            }
            return v;
        }
        uint64_t result;
        switch (reg) {
        case 1: result = s->atapi_error; break;
        case 2: result = s->atapi_count; break;
        case 3: result = s->atapi_lba_low; break;
        case 4: result = s->atapi_lba_mid; break;
        case 5: result = s->atapi_lba_high; break;
        case 6: result = s->atapi_device; break;
        case 7: result = s->atapi_status; break;
        default: result = 0; break;
        }
        if (getenv("ATA_TASKFILE_TRACE") || getenv("ATA_LOOP_TRACE")) {
            static unsigned taskfile_reads;
            static uint64_t prev_result = UINT64_MAX, prev_ip = UINT64_MAX;
            static bool loop_dumped;
            unsigned n = taskfile_reads++;
            bool changed = result != prev_result || s->cpu->ip != prev_ip;
            if (reg != 7 || n < 128 || changed || (n % 500) == 0) {
                fprintf(stderr, "i2000: ATAPI taskfile read port=%#x reg=%u -> %#04" PRIx64
                        " ip=%016" PRIx64 " ninsts=%" PRIu64 " n=%u%s\n",
                        (unsigned)port, reg, result, s->cpu->ip,
                        s->cpu->ninsts, n, changed ? " CHANGED" : "");
            }
            if (reg == 7 && n == 50 && !loop_dumped && getenv("ATA_LOOP_TRACE")) {
                loop_dumped = true;
                fprintf(stderr, "i2000: ATAPI status-poll loop, dumping call "
                        "history + next bundles from ip=%016" PRIx64 ":\n",
                        s->cpu->ip);
                merced_dump_calls(s->cpu, 16, stderr);
                s->cpu->trace_n = 60;
            }
            prev_result = result;
            prev_ip = s->cpu->ip;
        }
        return result;
    }
    if (s->hda && ((port >= 0x1F0 && port <= 0x1F7) || port == 0x3F6)) {
        unsigned reg = port == 0x3F6 ? 7 : (unsigned)(port - 0x1F0);
        if (reg == 0) {
            uint64_t v = 0;
            for (unsigned i = 0; i < size; i++) {
                if (s->ata_data_pos < s->ata_data_len)
                    v |= (uint64_t)s->ata_data[s->ata_data_pos++] << (i * 8);
            }
            if (!s->ata_data_is_write && s->ata_data_pos >= s->ata_data_len &&
                (s->ata_status & 0x08))
                s->ata_status = 0x40; /* transfer complete */
            return v;
        }
        switch (reg) {
        case 1: return s->ata_error;
        case 2: return s->ata_count;
        case 3: return s->ata_lba_low;
        case 4: return s->ata_lba_mid;
        case 5: return s->ata_lba_high;
        case 6: return s->ata_device;
        case 7:
            if (getenv("ATA_DEBUG")) {
                static unsigned n;
                if (n++ < 64)
                    fprintf(stderr, "i2000: ATA status read (port=%#x) -> "
                            "%#04x ninsts=%" PRIu64 "\n", (unsigned)port,
                            s->ata_status, s->cpu->ninsts);
            }
            return s->ata_status;
        }
    }
    /* CMD-649 primary/secondary channels, with no ATA devices attached.
     * A floating ATA bus reports status zero; returning the generic unmapped
     * value 0xFF makes firmware believe BSY is asserted forever. */
    if (size == 1 && (port == 0x1F7 || port == 0x177 ||
                      port == 0x3F6 || port == 0x376)) {
        if (getenv("ATA_DEBUG"))
            fprintf(stderr, "i2000: ATA status read port=%#x -> 0x00 "
                    "ninsts=%" PRIu64 "\n", (unsigned)port, s->cpu->ninsts);
        return 0x00;
    }
    if (size == 1 && ((port >= 0x1F0 && port <= 0x1F6) ||
                      (port >= 0x170 && port <= 0x176))) {
        if (getenv("ATA_DEBUG"))
            fprintf(stderr, "i2000: ATA read port=%#x -> 0x00 ninsts=%"
                    PRIu64 "\n", (unsigned)port, s->cpu->ninsts);
        return 0x00;
    }
    mmio_log(s, I2000_IO_BASE + port, 0, size, false);
    return size_mask(size);
}

static void io_port_write(Ia64I2000State *s, uint64_t port, uint64_t val,
                          unsigned size) {
    static unsigned debug_writes;
    if (s->rage128.enabled && size == 2 && port >= INT10_IO_BASE &&
        port < INT10_IO_BASE + 16 && !(port & 1)) {
        int10_io_write(s, (unsigned)(port - INT10_IO_BASE) / 2,
                       (uint16_t)val);
        return;
    }
    if (s->rage128.enabled) {
        uint32_t base = rage128_bar(&s->rage128, 0x14, RAGE128_IO_BASE,
                                   ~UINT32_C(0xff));
        if (port >= base && port + size <= base + 0x100) {
            rage128_mmio_write(&s->rage128,
                               (unsigned)(port - base), val, size);
            return;
        }
    }
    if (s->i82559.enabled) {
        uint32_t base = i82559_bar(&s->i82559, 0x14, I82559_IO_BASE,
                                  ~UINT32_C(0x3f));
        if (port >= base && port + size <= base + I82559_IO_SIZE) {
            i82559_reg_write(&s->i82559, (unsigned)(port - base), val, size);
            return;
        }
    }
    if (size == 1 && vga_port(port)) {
        vga_ibm_io_write(&s->vga, (uint16_t)port, (uint8_t)val);
        return;
    }
    {
        uint64_t off;
        if (smbus_port_offset(s, port, &off)) {
            smbus_reg_write(s, off, val, size);
            return;
        }
    }
    if (port >= 0x1000 && port + size <= 0x1008)
        return; /* no real device behind it - see io_port_read */
    if (port == 0x1110 && size == 1)
        return;
    if (port >= 0x400 && port + size <= 0x440) {
        memcpy(&s->acpi_io[port - 0x400], &val, size);
        return;
    }
    if (port == 0x21 && size == 1) {
        if (s->pic_master_icw) {
            if (s->pic_master_icw == 1)
                s->pic_master_base = (uint8_t)val & 0xF8;
            if (++s->pic_master_icw > 3)
                s->pic_master_icw = 0;
            return;
        }
        if (getenv("PIC_DEBUG") && val != s->pic_master_mask)
            fprintf(stderr, "i2000: PIC master mask %#04x -> %#04x "
                    "(base=%#04x) ninsts=%" PRIu64 "\n",
                    s->pic_master_mask, (unsigned)val, s->pic_master_base,
                    s->cpu->ninsts);
        s->pic_master_mask = (uint8_t)val;
        if (!(s->pic_master_mask & 1) && s->pit0_reload &&
            !s->pit0_next_irq)
            s->pit0_next_irq = merced_get_itc(s->cpu) +
                               pit_interval_itc(s->pit0_reload);
        return;
    }
    if (port == 0xA1 && size == 1) {
        if (s->pic_slave_icw) {
            if (s->pic_slave_icw == 1)
                s->pic_slave_base = (uint8_t)val & 0xF8;
            if (++s->pic_slave_icw > 3)
                s->pic_slave_icw = 0;
            return;
        }
        if (getenv("PIC_DEBUG") && val != s->pic_slave_mask)
            fprintf(stderr, "i2000: PIC slave mask %#04x -> %#04x "
                    "(base=%#04x) ninsts=%" PRIu64 "\n",
                    s->pic_slave_mask, (unsigned)val, s->pic_slave_base,
                    s->cpu->ninsts);
        s->pic_slave_mask = (uint8_t)val;
        return;
    }
    if (port == 0x20 || port == 0xA0) {
        if (getenv("MOUSE_DEBUG"))
            fprintf(stderr, "MOUSE_DEBUG: PIC cmd port=%#02x val=%#02x "
                    "ninsts=%" PRIu64 "\n", (unsigned)port, (unsigned)val,
                    s->cpu->ninsts);
        if ((val & 0x10) != 0) {
            if (port == 0x20)
                s->pic_master_icw = 1;
            else
                s->pic_slave_icw = 1;
        }
        if ((val & 0x20) != 0) { /* OCW2 non-specific EOI */
            uint8_t *isr = port == 0x20 ? &s->pic_master_isr
                                        : &s->pic_slave_isr;
            for (int bit = 7; bit >= 0; bit--)
                if (*isr & (1u << bit)) { *isr &= ~(1u << bit); break; }
        }
        /* OCW3 RR=1 selects which register a subsequent command-port read
         * returns; RIS=0 chooses IRR (0x0A), RIS=1 chooses ISR (0x0B). */
        if ((val & 0x18) == 0x08 && (val & 0x02)) {
            bool *read_isr = port == 0x20 ? &s->pic_master_read_isr
                                          : &s->pic_slave_read_isr;
            *read_isr = (val & 0x01) != 0;
        }
        /* Other commands are OCWs. */
        return;
    }
    if (port == 0x40 && size == 1) {
        if (!s->pit0_write_phase) {
            s->pit0_latch = (uint8_t)val;
            s->pit0_write_phase = 1;
        } else {
            s->pit0_reload = s->pit0_latch | ((uint16_t)(uint8_t)val << 8);
            s->pit0_write_phase = 0;
            s->pit0_read_phase = 0;
            s->pit0_epoch_ninsts = merced_get_itc(s->cpu);
            s->pit0_next_irq = s->pit0_epoch_ninsts +
                               pit_interval_itc(s->pit0_reload);
            if (getenv("PIT_DEBUG"))
                fprintf(stderr, "i2000: PIT0 reload=%u epoch_itc=%" PRIu64
                        " next_irq_itc=%" PRIu64 " ninsts=%" PRIu64 "\n",
                        s->pit0_reload, s->pit0_epoch_ninsts,
                        s->pit0_next_irq, s->cpu->ninsts);
        }
        return;
    }
    if (port == 0x41 && size == 1) {
        if (!s->pit1_write_phase) {
            s->pit1_latch = (uint8_t)val;
            s->pit1_write_phase = 1;
        } else {
            s->pit1_reload = s->pit1_latch | ((uint16_t)(uint8_t)val << 8);
            s->pit1_write_phase = 0;
            s->pit1_next_irq = merced_get_itc(s->cpu) +
                               pit_interval_itc(s->pit1_reload);
            if (getenv("PIT_DEBUG"))
                fprintf(stderr, "i2000: PIT1 reload=%u next_irq_itc=%" PRIu64
                        " ninsts=%" PRIu64 "\n",
                        s->pit1_reload, s->pit1_next_irq, s->cpu->ninsts);
        }
        return;
    }
    if (port == 0x64 && size == 1) {               /* 8042 command */
        uint8_t cmd = (uint8_t)val;
        s->kbc_out_pos = s->kbc_out_len = 0;
        switch (cmd) {
        case 0x20: /* read command byte */
            s->kbc_out[0] = s->kbc_command_byte;
            s->kbc_out_len = 1;
            break;
        case 0x60: s->kbc_pending_write = 1; break;
        case 0xAA: /* controller self-test */
            s->kbc_out[0] = 0x55; s->kbc_out_len = 1;
            break;
        case 0xAB: /* keyboard interface test */
            s->kbc_out[0] = 0x00; s->kbc_out_len = 1;
            break;
        case 0xA9: /* auxiliary interface test */
            s->kbc_out[0] = s->mouse_enabled ? 0x00 : 0x01;
            s->kbc_out_len = 1;
            break;
        case 0xAD: s->kbc_command_byte |= 0x10; break;
        case 0xAE: s->kbc_command_byte &= (uint8_t)~0x10; break;
        case 0xA7: s->kbc_command_byte |= 0x20; break;
        case 0xA8: s->kbc_command_byte &= (uint8_t)~0x20; break;
        case 0xD0: /* read output port: reset deasserted, A20 enabled */
            s->kbc_out[0] = 0x03; s->kbc_out_len = 1;
            break;
        case 0xD1: s->kbc_pending_write = 2; break;
        case 0xD4: s->kbc_pending_write = 3; break;
        default: break;
        }
        return;
    }
    if (port == 0x60 && size == 1) {               /* 8042/keyboard data */
        uint8_t data = (uint8_t)val;
        s->kbc_out_pos = s->kbc_out_len = 0;
        if (s->kbc_pending_write == 1) {
            s->kbc_command_byte = data;
        } else if (s->kbc_pending_write == 3) {
            /* 0xD4-routed byte: forward to the real PS/2 mouse command
             * handler if -device mouse attached one; otherwise consume the
             * byte but leave OBF clear so the firmware's mouse probe times
             * out and does not instantiate a phantom mouse driver. */
            if (s->mouse_enabled)
                mouse_device_command(s, data);
        } else if (s->kbc_pending_write == 0) {
            ps2_keyboard_command(&s->ps2_keyboard, data);
        }
        s->kbc_pending_write = 0;
        return;
    }
    if ((port == 0x70 || port == 0x72) && size == 1) {
        /* The i2000 IFB exposes its RTC/configuration RAM at 72h/73h, but
         * 70h/71h (standard AT CMOS index/data) must alias the same state -
         * see the read-side comment. Bit 7 of 70h is conventionally the
         * NMI-mask bit on real hardware; we don't model NMI, so it's
         * harmlessly folded into the index like the rest of the byte. */
        s->cmos_index = (uint8_t)val & 0x7F;
        return;
    }
    if ((port == 0x71 || port == 0x73) && size == 1) {
        if (getenv("MERCED_DEBUG") && debug_writes++ < 128)
            fprintf(stderr, "i2000: CMOS[%02X] <- %02X\n",
                    s->cmos_index & 0x7F, (unsigned)(uint8_t)val);
        s->cmos[s->cmos_index & 0x7F] = (uint8_t)val;
        return;
    }
    if (port == RESET_CTRL_PORT && size == 1) {
        /* Intel reset-control convention: bit 1 selects a hard reset and
         * bit 2 triggers it.  SAL writes 02h followed by 06h, then waits in
         * a dead loop for the platform reset to arrive. */
        if (val & 0x04)
            s->reset_requested = true;
        return;
    }
    if (port == PCI_CFG_ADDR && size == 4) {
        s->pci_cfg_addr = (uint32_t)val;
        return;
    }
    if (port >= PCI_CFG_DATA && port < PCI_CFG_DATA + 4) {
        pci_cfg_write(s, (unsigned)(port - PCI_CFG_DATA), val, size);
        return;
    }
    if (port == POST_PORT) {
        s->post_code = (uint8_t)val;
        return;
    }
    if (port == 0x42) {                            /* PIT channel-2 count */
        s->pit2_polls = 0;
        return;
    }
    if (port == 0x43) {
        if (getenv("PIT_DEBUG"))
            fprintf(stderr, "i2000: PIT command byte %#04x (channel=%u "
                    "mode=%u) ninsts=%" PRIu64 "\n", (unsigned)(uint8_t)val,
                    (unsigned)((uint8_t)val >> 6), (unsigned)(((uint8_t)val >> 1) & 7),
                    s->cpu->ninsts);
        if (((uint8_t)val >> 6) == 0) {
            s->pit0_write_phase = 0;
            s->pit0_read_phase = 0;
        } else if (((uint8_t)val >> 6) == 1)
            s->pit1_write_phase = 0;
        return;
    }
    if (port == 0x61) {
        s->port61 = (uint8_t)val & 0x0F;
        return;
    }
    if ((port >= 0x170 && port <= 0x177) || port == 0x376) {
        unsigned reg = port == 0x376 ? 8 : (unsigned)(port - 0x170);
        if (reg == 0) {
            unsigned old_pos = s->atapi_packet_pos;
            for (unsigned i = 0; i < size && s->atapi_packet_pos < 12; i++)
                s->atapi_packet[s->atapi_packet_pos++] = (uint8_t)(val >> (i * 8));
            if (old_pos < 12 && s->atapi_packet_pos == 12) {
                atapi_reply(s);
                /* The SDV's legacy EFI thunk checks the data phase
                 * synchronously after writing the final PACKET word.  A0
                 * itself must assert BSY (AtaPim explicitly waits for that
                 * edge), but inserting another artificial BSY interval
                 * here makes the thunk abandon PIO before its data read.
                 * atapi_reply()/atapi_set_data() already expose DRQ and
                 * schedule the normal data-ready IRQ. */
            }
            return;
        }
        switch (reg) {
        case 1: s->atapi_features = (uint8_t)val; return;
        case 2: s->atapi_count = (uint8_t)val; return;
        case 3: s->atapi_lba_low = (uint8_t)val; return;
        case 4: s->atapi_lba_mid = (uint8_t)val; return;
        case 5: s->atapi_lba_high = (uint8_t)val; return;
        case 6: {
            bool was_slave = (s->atapi_device & 0x10) != 0;
            s->atapi_device = (uint8_t)val;
            bool is_slave = (s->atapi_device & 0x10) != 0;
            if (is_slave != was_slave) {
                /* This model has no secondary slave.  Do not let a failed
                 * slave probe leave an interrupt that can complete the
                 * master's next PACKET request prematurely. */
                s->atapi_irq_pending = false;
                s->atapi_status_pending = false;
                if (!is_slave) {
                    s->atapi_error = 0;
                    s->atapi_status = 0x40;
                    s->atapi_count = 1;
                    s->atapi_lba_low = 1;
                    s->atapi_lba_mid = 0x14;
                    s->atapi_lba_high = 0xeb;
                }
            }
            return;
        }
        case 7:
            if (s->atapi_device & 0x10) {
                if (atapi_debug_enabled())
                    fprintf(stderr, "i2000: ignored ATA command=%#04x for "
                            "absent secondary slave\n",
                            (unsigned)(uint8_t)val);
                return;
            }
            /* A new command supersedes any unacknowledged completion from
             * the previous one. The command below will latch IRQ15 again
             * when its data or completion phase becomes available. */
            s->atapi_irq_pending = false;
            s->atapi_status_pending = false;
            s->atapi_error = 0;
            if (atapi_debug_enabled())
                fprintf(stderr, "i2000: ATAPI ATA command=%#04x device=%#04x "
                        "features=%#04x byte-limit=%u itc=%" PRIu64
                        " ninsts=%" PRIu64 "\n", (unsigned)(uint8_t)val,
                        s->atapi_device, s->atapi_features,
                        (unsigned)s->atapi_lba_mid |
                        ((unsigned)s->atapi_lba_high << 8),
                        merced_get_itc(s->cpu), s->cpu->ninsts);
            if ((uint8_t)val == 0xA0) { /* PACKET */
                unsigned limit = (unsigned)s->atapi_lba_mid |
                                 ((unsigned)s->atapi_lba_high << 8);
                s->atapi_byte_limit = limit ? limit : 65536u;
                s->atapi_packet_pos = 0;
                memset(s->atapi_packet, 0, sizeof(s->atapi_packet));
                s->atapi_count = 0x01;
                s->atapi_status = 0x48;
                atapi_defer_ready(s, false);
            } else if ((uint8_t)val == 0xA1) { /* IDENTIFY PACKET DEVICE */
                uint8_t id[512] = {0};
                uint8_t saved_count = s->atapi_count;
                uint8_t saved_lba_low = s->atapi_lba_low;
                uint8_t saved_lba_mid = s->atapi_lba_mid;
                uint8_t saved_lba_high = s->atapi_lba_high;
                id[0] = 0xC0; id[1] = 0x85;       /* removable ATAPI CD-ROM */
                id[98] = 0x00; id[99] = 0x02;     /* LBA supported */
                char model[40];
                memset(model, ' ', sizeof(model));
                memcpy(model, "GEMU ATAPI CD-ROM", 17);
                for (unsigned i = 0; i < 40; i += 2) {
                    id[54 + i] = model[i + 1]; id[55 + i] = model[i];
                }
                s->atapi_byte_limit = sizeof(id);
                atapi_set_data(s, id, sizeof(id));
                /* IDENTIFY PACKET is an ATA PIO-data command, not an ATAPI
                 * PACKET data phase.  Sector Count is therefore not an
                 * interrupt-reason register here, and Cylinder Low/High
                 * are not a transfer byte count. Preserve the task-file
                 * signature/inputs that were present when A1 was issued. */
                s->atapi_count = saved_count;
                s->atapi_lba_low = saved_lba_low;
                s->atapi_lba_mid = saved_lba_mid;
                s->atapi_lba_high = saved_lba_high;
                atapi_defer_ready(s, true);
            } else if ((uint8_t)val == 0x08) { /* DEVICE RESET */
                s->atapi_status = 0x40;
                s->atapi_count = 1; s->atapi_lba_mid = 0x14; s->atapi_lba_high = 0xEB;
            } else {
                s->atapi_error = 0x04; s->atapi_status = 0x41;
                s->atapi_irq_pending = true;
                s->atapi_irq_ready_ninsts = s->cpu->ninsts + ATAPI_IRQ_LATENCY_NINSTS;
                if (atapi_debug_enabled())
                    fprintf(stderr, "i2000: ATAPI unsupported ATA command=%#04x "
                            "status=%#04x error=%#04x\n",
                            (unsigned)(uint8_t)val, s->atapi_status,
                            s->atapi_error);
            }
            return;
        case 8: /* device control / software reset */
            if (atapi_debug_enabled())
                fprintf(stderr, "i2000: ATAPI device-control=%#04x (%s reset)\n",
                        (unsigned)(uint8_t)val,
                        (val & 4) ? "assert" : "deassert");
            s->atapi_irq_pending = false;
            s->atapi_status_pending = false;
            if (val & 4) s->atapi_status = 0x80;
            else { s->atapi_status = 0x40; s->atapi_count = 1;
                   s->atapi_lba_mid = 0x14; s->atapi_lba_high = 0xEB; }
            return;
        }
    }
    if (s->hda && ((port >= 0x1F0 && port <= 0x1F7) || port == 0x3F6)) {
        unsigned reg = port == 0x3F6 ? 8 : (unsigned)(port - 0x1F0);
        if (reg == 0) {
            for (unsigned i = 0; i < size; i++) {
                if (s->ata_data_pos < s->ata_data_len)
                    s->ata_data[s->ata_data_pos++] = (uint8_t)(val >> (i * 8));
            }
            if (s->ata_data_is_write && s->ata_data_pos >= s->ata_data_len) {
                if (fseek(s->hda, (long)((uint64_t)s->ata_write_lba * 512),
                          SEEK_SET) != 0 ||
                    fwrite(s->ata_data, 1, s->ata_data_len, s->hda) !=
                    s->ata_data_len) {
                    s->ata_error = 0x04;
                    s->ata_status = 0x41;
                } else {
                    fflush(s->hda);
                    s->ata_status = 0x40;
                }
                s->ata_data_is_write = false;
            }
            return;
        }
        switch (reg) {
        case 1: s->ata_features = (uint8_t)val; return;
        case 2: s->ata_count = (uint8_t)val; return;
        case 3: s->ata_lba_low = (uint8_t)val; return;
        case 4: s->ata_lba_mid = (uint8_t)val; return;
        case 5: s->ata_lba_high = (uint8_t)val; return;
        case 6: s->ata_device = (uint8_t)val; return;
        case 7: ata_command(s, (uint8_t)val); return;
        case 8: /* device control / software reset */
            if (getenv("ATA_DEBUG"))
                fprintf(stderr, "i2000: ATA device control <- %#04x "
                        "ninsts=%" PRIu64 "\n", (unsigned)val,
                        s->cpu->ninsts);
            if (val & 4) {
                s->ata_status = 0x80;
            } else {
                s->ata_status = 0x40;
                s->ata_error = 0x01;
                s->ata_count = 1;
                s->ata_lba_low = 1;
                s->ata_lba_mid = s->ata_lba_high = 0;
            }
            return;
        }
    }
    if (port >= COM1_PORT && port < COM1_PORT + 8) {
        if (getenv("MERCED_DEBUG") && debug_writes++ < 128)
            fprintf(stderr, "i2000: COM1 write reg %u <- %02X\n",
                    (unsigned)(port - COM1_PORT), (unsigned)(uint8_t)val);
        switch (port - COM1_PORT) {
        case 0:
            if (s->uart_lcr & 0x80) { s->uart_dll = (uint8_t)val; return; }
            con_putc(s, (char)val);
            fputc((int)val, stdout);
            fflush(stdout);
            return;
        case 1:
            if (s->uart_lcr & 0x80) s->uart_dlm = (uint8_t)val;
            else s->uart_ier = (uint8_t)val;
            return;
        case 3: s->uart_lcr = (uint8_t)val; return;
        case 4: s->uart_mcr = (uint8_t)val; return;
        case 7: s->uart_scr = (uint8_t)val; return;
        default: return;                            /* FCR etc. */
        }
    }
    if (size == 1 && ((port >= 0x1F0 && port <= 0x1F7) || port == 0x3F6 ||
                      (port >= 0x170 && port <= 0x177) || port == 0x376)) {
        if (getenv("ATA_DEBUG"))
            fprintf(stderr, "i2000: ATA write port=%#x <- %#x ninsts=%"
                    PRIu64 "\n", (unsigned)port, (unsigned)val, s->cpu->ninsts);
        return;                                     /* empty CMD-649 channels */
    }
    mmio_log(s, I2000_IO_BASE + port, val, size, true);
}

/* Merced uses the architected sparse legacy-I/O encoding:
 *   offset = ((port & ~3) << 10) | (port & 0xFFF)
 * a bitfield insert, not an addition - the low two address bits select the
 * byte lane, and bits 2-11 of port are carried in offset's low 12 bits
 * unchanged while offset's bits 12 and up are just (port & ~3) shifted.
 * Treating this as addition (offset = port + ((port & ~3) << 10)) happens
 * to agree with the true insert for every port below 0x1000, since there's
 * no bit overlap to lose to the OR there - which is exactly why ports like
 * 0xCFC (PCI CONFIG_DATA) and 0x3F8 (COM1) always worked. It silently
 * breaks for port >= 0x1000: the insert clips the deposited port value to
 * its low 12 bits, discarding bit 12 and up entirely, while the shifted
 * term alone already carries those bits - addition instead double-counts
 * them and produces a different offset than hardware does. A real device
 * BAR'd above 0x1000 (seen: the CMD649 SMBus function's I/O BAR at 0x1000)
 * would decode to "not a valid port" under the old formula, so its host
 * controller's status register always fell through to the generic
 * unmapped-I/O response (all ones), and firmware's "wait for not busy"
 * poll on it spun forever. */
static bool sparse_io_port(uint64_t offset, uint64_t *port) {
    uint64_t p = (offset & 0xFFF) | ((offset >> 10) & ~0xFFFull);
    if (p > 0xFFFF)
        return false;
    *port = p;
    return true;
}

static bool vga_mem_window(Ia64I2000State *s, uint64_t addr, unsigned size,
                          uint32_t *voff) {
    uint32_t base, wsize;
    vga_ibm_aperture(&s->vga, &base, &wsize);
    if (addr < base || addr + size > (uint64_t)base + wsize)
        return false;
    *voff = (uint32_t)(addr - base);
    return true;
}

#define VGA_ROM_BASE 0xC0000ull

static bool vga_rom_window(uint64_t addr, unsigned size, uint32_t *voff) {
    if (addr < VGA_ROM_BASE || addr + size > VGA_ROM_BASE + 0x10000ull)
        return false;
    *voff = (uint32_t)(addr - VGA_ROM_BASE);
    return true;
}

/* I/O SAPIC indirect register access (SSDM sec. 2.6.3): iosapic_regsel
 * selects id(00h)/version(01h)/arbitration(02h)/RTE-low-or-high(10h-8Fh),
 * manipulated through the window register. */
static uint32_t iosapic_indirect_read(Ia64I2000State *s) {
    unsigned reg = s->iosapic_regsel & 0xFF;
    if (reg == 0x00)
        return 1u << 15;             /* DT=1 (SAPIC delivery), ID=0 */
    if (reg == 0x01)
        return 0x003F0021u;          /* MAX_REDIR=63 (64 entries), VERSION=0x21 */
    if (reg >= 0x10 && reg <= 0x8F) {
        unsigned entry = (reg - 0x10) / 2;
        bool high = ((reg - 0x10) & 1) != 0;
        uint64_t rte = s->iosapic_rte[entry];
        return high ? (uint32_t)(rte >> 32) : (uint32_t)rte;
    }
    return 0;  /* arbitration ID and reserved registers */
}

static void iosapic_indirect_write(Ia64I2000State *s, uint32_t val) {
    unsigned reg = s->iosapic_regsel & 0xFF;
    if (reg < 0x10 || reg > 0x8F)
        return;  /* id/version/arbitration: not writable in this model */
    unsigned entry = (reg - 0x10) / 2;
    bool high = ((reg - 0x10) & 1) != 0;
    uint64_t rte = s->iosapic_rte[entry];
    if (high)
        rte = (rte & 0x00000000FFFFFFFFull) | ((uint64_t)val << 32);
    else
        rte = (rte & 0xFFFFFFFF00000000ull) | val;
    s->iosapic_rte[entry] = rte;
    if (getenv("IOSAPIC_DEBUG"))
        fprintf(stderr, "i2000: IOSAPIC RTE[%u] <- %016" PRIX64
                " (mask=%d vector=%#x dmode=%u) ninsts=%" PRIu64 "\n",
                entry, rte, !!(rte & IOSAPIC_RTE_MASK), (unsigned)(rte & 0xFF),
                (unsigned)((rte >> 8) & 7), s->cpu->ninsts);
}

/* Deliver global system interrupt `gsi` if its redirection-table entry
 * isn't masked (SSDM Table 2-10). GSI N == legacy ISA IRQ N (no ACPI
 * interrupt source overrides modeled). Every entry defaults masked at
 * reset, so nothing fires until firmware programs it. */
static void iosapic_raise_gsi(Ia64I2000State *s, unsigned gsi) {
    if (gsi >= 64)
        return;
    uint64_t rte = s->iosapic_rte[gsi];
    if (getenv("IOSAPIC_DEBUG")) {
        static uint64_t n;
        if (n++ < 20 || (n % 5000) == 0)
            fprintf(stderr, "i2000: iosapic_raise_gsi(%u) rte=%016" PRIX64
                    " masked=%d picmask=%02x tpr=%02" PRIX64
                    " ninsts=%" PRIu64 "\n", gsi, rte,
                    !!(rte & IOSAPIC_RTE_MASK), s->pic_master_mask,
                    s->cpu->cr[66] & 0xff, s->cpu->ninsts);
    }
    if (rte & IOSAPIC_RTE_MASK)
        return;
    unsigned delivery_mode = (unsigned)((rte >> 8) & 7);
    if (delivery_mode == 7) {
        /* ExtINT: routed to the external 8259A-compatible controller,
         * which supplies the vector via the legacy PIC's own base. */
        if (getenv("IOSAPIC_DEBUG"))
            fprintf(stderr, "i2000: iosapic delivering ExtINT vector=%#x "
                    "ninsts=%" PRIu64 "\n", s->pic_master_base, s->cpu->ninsts);
        merced_raise_external(s->cpu, s->pic_master_base);
        return;
    }
    /* Fixed SAPIC mode (000/001, the LSB is a redirection hint this model
     * doesn't act on): deliver the RTE's own vector directly. */
    if (getenv("IOSAPIC_DEBUG"))
        fprintf(stderr, "i2000: iosapic delivering fixed vector=%#x "
                "ninsts=%" PRIu64 "\n", (unsigned)(uint8_t)rte, s->cpu->ninsts);
    merced_raise_external(s->cpu, (uint8_t)rte);
}

/* SDV's 460GX memory map carries the memory-card/port selector in physical
 * address bits 43:32.  Aperture 0xe00 is distinct from the low firmware
 * mapping for data: EFI uses both at once, so folding tagged stores onto low
 * RAM corrupts its executable image and stacks. Back the top 64 MiB as a
 * complete private code-and-data aperture. */
static inline bool i2000_high_dram_addr(uint64_t addr, uint64_t *off) {
    if ((addr & UINT64_C(0x00000fff00000000)) != I2000_HIGH_DRAM_TAG)
        return false;
    uint64_t low = (uint32_t)addr;
    if (low < I2000_HIGH_DRAM_BASE || low >= I2000_RAM_MAX)
        return false;
    *off = low - I2000_HIGH_DRAM_BASE;
    return true;
}

/* The 460GX aliases sign-extended SAL scratch addresses into its 2-GiB DRAM
 * aperture.  The production AMI SAL saves state at 0x7fffa030 and restores
 * it through ffffffffffffa030, so these addresses must meet in RAM. */
static inline uint64_t i2000_normalize_pa(Ia64I2000State *s, uint64_t addr) {
    addr &= MERCED_PHYS_MASK;
    if ((addr & UINT64_C(0x000fffff00000000)) ==
        UINT64_C(0x000fffff00000000)) {
        uint64_t base = s->flash_cmd_addr;
        if (!base)
            base = UINT64_C(0x7fff0000);
        addr = base | (addr & UINT64_C(0xffff));
    } else if (s->flash_cmd_addr &&
               ((addr & UINT64_C(0xffff)) == UINT64_C(0x6030) ||
                (addr & UINT64_C(0xffff)) == UINT64_C(0xa030))) {
        uint64_t aperture_end = (addr & ~UINT64_C(0xffff)) + 0x10000;
        if (aperture_end && !(aperture_end & (aperture_end - 1)))
            addr = s->flash_cmd_addr | (addr & UINT64_C(0xffff));
    }
    return addr;
}

static uint64_t bus_read(void *ud, uint64_t addr, unsigned size) {
    Ia64I2000State *s = ud;
    for (unsigned i = 0; i < size; i++)
        gemu_monitor_check_read(s->monitor, (uint32_t)(addr + i));
    /* IA-64 region-6 uncached aliases carry 0xC in the top nibble.  The
     * chipset sees the physical address after those region bits are
     * stripped; retaining them made SDV's MMIO PCI configuration cycles
     * miss every device. */
    bool region6 = (addr >> 60) == 0xC;
    addr = i2000_normalize_pa(s, addr);
    uint64_t high_off;
    if (i2000_high_dram_addr(addr, &high_off) &&
        high_off + size <= I2000_HIGH_DRAM_SIZE) {
        uint64_t v = 0;
        memcpy(&v, s->high_dram + high_off, size);
        return v;
    }
    uint32_t voff;
    if (s->rage128.enabled) {
        uint64_t fb = rage128_bar(&s->rage128, 0x10, RAGE128_FB_BASE,
                                 ~UINT32_C(0x03ffffff));
        uint64_t mmio = rage128_bar(&s->rage128, 0x18, RAGE128_MMIO_BASE,
                                   ~UINT32_C(0x3fff));
        if (addr >= fb && addr + size <= fb + RAGE128_FB_APER_SIZE) {
            uint32_t off = (uint32_t)(addr - fb) & (RAGE128_VRAM_SIZE - 1);
            return rage128_vram_read(&s->rage128, off, size);
        }
        if (addr >= mmio && addr + size <= mmio + RAGE128_MMIO_SIZE)
            return rage128_mmio_read(&s->rage128,
                                     (unsigned)(addr - mmio), size);
    }
    if (s->i82559.enabled) {
        uint64_t mmio = i82559_bar(&s->i82559, 0x10, I82559_MMIO_BASE,
                                  ~UINT32_C(0xfff));
        if (addr >= mmio && addr + size <= mmio + I82559_MMIO_SIZE)
            return i82559_reg_read(&s->i82559, (unsigned)(addr - mmio), size);
        uint32_t rombar = 0;
        memcpy(&rombar, s->i82559.cfg + 0x30, sizeof(rombar));
        uint64_t rombase = rombar & ~UINT32_C(0x7ff);
        if ((rombar & 1) && s->i82559.option_rom && addr >= rombase &&
            addr + size <= rombase + s->i82559.option_rom_size) {
            uint64_t v = 0;
            memcpy(&v, s->i82559.option_rom + (addr - rombase), size);
            return v;
        }
    }
    /* vga_mem_window()/vga_rom_window() can only ever match addr < 0xD0000
     * (the legacy VGA aperture + option-ROM shadow window) - skip both
     * calls for the overwhelming majority of accesses, which land well
     * above that legacy hole. */
    if (addr < 0xD0000) {
        if (vga_mem_window(s, addr, size, &voff)) {
            uint64_t v = 0;
            for (unsigned i = 0; i < size; i++)
                v |= (uint64_t)vga_ibm_mem_read(&s->vga, voff + i) << (i * 8);
            return v;
        }
        if (vga_rom_window(addr, size, &voff)) {
            uint64_t v = 0;
            memcpy(&v, s->vga_rom_shadow + voff, size);
            return v;
        }
    }
    if (region6 && addr + size <= I2000_FLASH_BASE) {
        uint32_t saved = s->pci_cfg_addr;
        s->pci_cfg_addr = 0x80000000u | ((uint32_t)addr & 0xfffffffcu);
        uint64_t v = pci_cfg_read(s, (unsigned)(addr & 3), size);
        s->pci_cfg_addr = saved;
        return v;
    }
    if (addr + size <= s->ram_size) {
        uint64_t v = 0;
        memcpy(&v, s->ram + addr, size);
        return v;
    }
    if (addr >= I2000_CHIPSET_SCRATCH_BASE && addr + size <= I2000_RAM_MAX) {
        uint64_t v = 0;
        memcpy(&v, s->chipset_scratch + (addr - I2000_CHIPSET_SCRATCH_BASE), size);
        return v;
    }
    if (addr >= IOSAPIC_BASE && addr < IOSAPIC_BASE + IOSAPIC_SIZE && size == 4) {
        uint32_t off = (uint32_t)(addr - IOSAPIC_BASE);
        if (off == IOSAPIC_REGSEL_OFF) return s->iosapic_regsel & 0xFF;
        if (off == IOSAPIC_WINDOW_OFF) return iosapic_indirect_read(s);
        return 0;
    }
    /* Legacy ExtINT acknowledge: IA-64 has no INTA pin, so SAL substitutes
     * a memory-mapped byte read to learn which 8259-relative IRQ won
     * arbitration - the same information a real INTA cycle would return
     * on the data bus (ICW2 base + IRQ number). IRQ0 (the PIT) and, once
     * -device mouse attaches one, IRQ12 (the aux/mouse device), and IRQ15
     * (secondary IDE) all share this single ack address on real
     * hardware's single physical INTA-replacement cycle, so whichever one
     * is actually pending decides what gets returned - PIT wins if both
     * happen to be pending at once, matching real 8259 priority (IRQ0
     * outranks any slave-cascaded IRQ). A slave-originated vector is
     * reported relative to the slave's own programmed base (ICW2), not
     * the master's - real hardware's second INTA cycle addresses the
     * slave chip directly. */
    if (addr == I2000_LEGACY_ACK_ADDR && size == 1) {
        /* This MMIO read replaces the pair of INTA bus cycles on IA-64.
         * It therefore acknowledges the local-SAPIC ExtINT request as well
         * as returning the 8259's vector.  Leaving vector zero pending made
         * the IRQ0 handler's STI immediately re-enter the same interrupt,
         * corrupting its real-mode IRET frame before the eventual PIC EOI. */
        merced_ack_external(s->cpu, 0);
        if (getenv("MOUSE_DEBUG"))
            fprintf(stderr, "MOUSE_DEBUG: ack read pit_pending=%d "
                    "mouse_pending=%d ninsts=%" PRIu64 "\n",
                    s->pit_irq_pending, s->mouse_irq_pending, s->cpu->ninsts);
        if (s->pit_irq_pending) {
            s->pit_irq_pending = false;
            /* merced_ack_external() just cleared the ONE shared CPU-level
             * pending bit for vector 0, which both IRQ0 and IRQ12 share.
             * If IRQ12 is also still latched, don't re-raise synchronously
             * here - see the mouse branch below for why that corrupts the
             * guest ISR. Just leave mouse_irq_pending set with its retry
             * deadline (already scheduled by whichever aux_queue_byte()/ack
             * call set it) and let i2000_poll_interrupts() fire it once
             * that deadline passes. */
            return s->pic_master_base;
        }
        if (s->keyboard_irq_pending) {
            s->keyboard_irq_pending = false;
            /* The SDV thunk performs several native/IA-32 rfi transitions
             * while servicing one byte, so the global rfi counter cannot
             * identify completion of this particular IRQ1.  Keep the next
             * byte quiet long enough for the thunk and BIOS handler to
             * unwind before another shared ExtINT can overwrite cr.iip and
             * cr.ipsr. */
            s->keyboard_retry_ninsts =
                s->cpu->ninsts + KEYBOARD_RETRY_DELAY_INSTS;
            return (uint64_t)(s->pic_master_base + 1); /* IRQ1 */
        }
        if (s->mouse_irq_pending) {
            /* Real 8042 aux OBF is level-triggered: IRQ12 stays asserted as
             * long as unread aux data remains, re-firing for each
             * subsequent byte of a multi-byte packet rather than once per
             * packet. This firmware's ISR reads port 0x60 (consuming one
             * byte) *before* reaching this ack read, so aux_out already
             * reflects the post-read state - if bytes remain, more service
             * is needed.
             *
             * That next raise should NOT happen synchronously, right here:
             * cr.ipsr/cr.iip/cr.isr are single, non-stacked registers, and
             * a second delivery landing before this ISR invocation is fully
             * done servicing the current byte would clobber its not-yet-
             * consumed return frame - the same class of corruption the
             * pre-existing PIT-arbitration comment above already warned
             * about ("the IRQ0 handler's STI immediately re-enter[ing] the
             * same interrupt"). Deferring by a real instruction gap and
             * letting i2000_poll_interrupts() (which runs outside any
             * interrupt-handling context) do the actual raise once it's due
             * costs nothing and closes off that entire class of risk, even
             * though the specific rapid-burst wedge this was written to
             * chase (CPU stuck at psr.i=0, aux_out permanently stuck full)
             * turned out on closer inspection to be a stable spin loop
             * polling port 0x60 directly from what looks like this
             * firmware's own one-shot "Mouse 8042 Initialization: Waiting"
             * POST self-test, not something this delay alone resolves -
             * see mouse_retry_ninsts's struct comment. Mirrors the PIT's
             * own known-safe 100000-instruction tick granularity. */
            s->mouse_wait_rfi_generation = merced_rfi_generation() + 1;
            s->mouse_retry_ninsts = UINT64_MAX;
            if (s->aux_out_pos >= s->aux_out_len) {
                s->mouse_irq_pending = false;
            }
            return (uint64_t)(s->pic_slave_base + 4); /* IRQ12 = slave IRQ4 */
        }
        if (s->atapi_irq_pending) {
            s->atapi_irq_pending = false;
            s->pic_master_isr |= 0x04; /* IRQ2 cascade */
            s->pic_slave_isr |= 0x80;  /* IRQ15 */
            if (atapi_debug_enabled()) {
                fprintf(stderr, "i2000: ATAPI IRQ15 acknowledged vector=%#04x "
                        "ip=%016" PRIx64 " itc=%" PRIu64 " ninsts=%" PRIu64 "\n",
                        (unsigned)(s->pic_slave_base + 7), s->cpu->ip,
                        merced_get_itc(s->cpu), s->cpu->ninsts);
                if (getenv("ATA_IRQ_TRACE")) {
                    fprintf(stderr, "i2000: ATAPI IRQ15 ack call history:\n");
                    merced_dump_calls(s->cpu, 24, stderr);
                    s->cpu->trace_n = 4000;
                }
            }
            return (uint64_t)(s->pic_slave_base + 7); /* IRQ15 = slave IRQ7 */
        }
        return s->pic_master_base; /* spurious/unattributed: old behavior */
    }
    if (addr - I2000_FLASH_BASE < I2000_FLASH_SIZE) {
        uint64_t off = addr - I2000_FLASH_BASE;
        unsigned chip_index = (unsigned)(off / I2000_FLASH_CHIP_SIZE);
        uint64_t chip_off = off % I2000_FLASH_CHIP_SIZE;
        I2000FlashChip *chip = &s->flash_chip[chip_index];
        if (chip->read_status) {
            uint64_t v = 0;
            for (unsigned i = 0; i < size; i++)
                v |= (uint64_t)chip->status << (i * 8);
            return v;
        }
        if (chip->read_id) {
            uint64_t v = 0;
            for (unsigned i = 0; i < size; i++) {
                /* 82802AC identifier space is decoded within every 64 KiB
                 * block: manufacturer and device codes are at +0/+1, while
                 * +2 is that block's lock-status register.  Returning the
                 * manufacturer ID for every even byte made +2 read as 0x89,
                 * whose bit 0 tells FMM that every block is locked; CDB then
                 * reported "not enough space" without issuing a write. */
                uint32_t id_off = (uint32_t)(chip_off + i) & 0xFFFFu;
                uint8_t id = id_off == 0 ? 0x89 :
                             id_off == 1 ? 0xAC :
                             id_off == 2 ? 0x00 : 0xFF;
                v |= (uint64_t)id << (i * 8);
            }
            return v;
        }
        /* Reads through the top-of-4GiB window always see the ROM (the
         * recovery-image scan depends on it); only writes divert into the
         * RAM shadow, where the firmware reads them back through the low
         * alias. */
        uint64_t v = 0;
        if (off + size <= I2000_FLASH_SIZE)
            memcpy(&v, s->flash + off, size);
        return v;
    }
    if (addr - I2000_IO_BASE < I2000_IO_SIZE) {
        uint64_t port;
        if (sparse_io_port(addr - I2000_IO_BASE, &port)) {
            if (getenv("MOUSE_DEBUG") && (port == 0x60 || port == 0x64))
                fprintf(stderr, "MOUSE_DEBUG: sparse MMIO read port=%#x "
                        "size=%u ninsts=%" PRIu64 "\n", (unsigned)port,
                        size, s->cpu->ninsts);
            return io_port_read(s, port, size);
        }
    }
    if (addr == I2000_SAC_CBNR && size == 4)
        return s->sac_cbnr;
    if (addr == I2000_SAC_CCSR && size == 4)
        return s->sac_ccsr;
    mmio_log(s, addr, 0, size, false);
    return size_mask(size);
}

static uint64_t bus_fetch(void *ud, uint64_t addr, unsigned size) {
    Ia64I2000State *s = ud;
    addr = i2000_normalize_pa(s, addr);
    /* Code fetches in the shadow window come from the flash ROM: on real
     * hardware the firmware executes through the top-of-4GiB ROM alias
     * while its data lives in the RAM shadow at the same offsets. Serving
     * fetches from the (mutable) RAM copy lets the SAL data-area clear
     * wipe the very code performing it. */
    /* Top-of-4GiB fetches execute the ROM itself even once the window is
     * RAM-shadowed for data; low shadow fetches read RAM (the firmware
     * patches handler code there at runtime). */
    uint64_t off = addr - I2000_FLASH_BASE;
    if (off < I2000_FLASH_SIZE) {
        uint64_t v = 0;
        if (off + size <= I2000_FLASH_SIZE)
            memcpy(&v, s->flash + off, size);
        return v;
    }
    return bus_read(ud, addr, size);
}

static void bus_write(void *ud, uint64_t addr, uint64_t val, unsigned size) {
    Ia64I2000State *s = ud;
    for (unsigned i = 0; i < size; i++) {
        if (gemu_monitor_check_write(s->monitor, (uint32_t)(addr + i)))
            fprintf(stderr, "i2000: watched write pa=%016" PRIX64
                    " size=%u val=%016" PRIX64 " ip=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", addr, size, val,
                    s->cpu->ip, s->cpu->ninsts);
    }
    bool region6 = (addr >> 60) == 0xC;
    addr = i2000_normalize_pa(s, addr);
    /* SAL first runs in a small bootstrap DRAM aperture and later in the
     * fully-sized one.  It saves its two restore words at the top of the
     * currently active aperture, then reloads them through sign-extended
     * addresses.  Remember the aperture selected by the real save rather
     * than assuming that it is always the final 2-GiB top. */
    if (size == 8 && ((addr & UINT64_C(0xffff)) == UINT64_C(0x6030) ||
                      (addr & UINT64_C(0xffff)) == UINT64_C(0xa030)) &&
        addr < s->ram_size) {
        uint64_t aperture_end = (addr & ~UINT64_C(0xffff)) + 0x10000;
        if (aperture_end && !(aperture_end & (aperture_end - 1)))
            s->flash_cmd_addr = addr & ~UINT64_C(0xffff);
    }
    uint64_t high_off;
    if (i2000_high_dram_addr(addr, &high_off) &&
        high_off + size <= I2000_HIGH_DRAM_SIZE) {
        memcpy(s->high_dram + high_off, &val, size);
        return;
    }
    uint32_t voff;
    if (s->rage128.enabled) {
        uint64_t fb = rage128_bar(&s->rage128, 0x10, RAGE128_FB_BASE,
                                 ~UINT32_C(0x03ffffff));
        uint64_t mmio = rage128_bar(&s->rage128, 0x18, RAGE128_MMIO_BASE,
                                   ~UINT32_C(0x3fff));
        if (addr >= fb && addr + size <= fb + RAGE128_FB_APER_SIZE) {
            uint32_t off = (uint32_t)(addr - fb) & (RAGE128_VRAM_SIZE - 1);
            rage128_vram_write(&s->rage128, off, val, size);
            return;
        }
        if (addr >= mmio && addr + size <= mmio + RAGE128_MMIO_SIZE) {
            rage128_mmio_write(&s->rage128,
                               (unsigned)(addr - mmio), val, size);
            return;
        }
    }
    if (s->i82559.enabled) {
        uint64_t mmio = i82559_bar(&s->i82559, 0x10, I82559_MMIO_BASE,
                                  ~UINT32_C(0xfff));
        if (addr >= mmio && addr + size <= mmio + I82559_MMIO_SIZE) {
            i82559_reg_write(&s->i82559, (unsigned)(addr - mmio), val, size);
            return;
        }
    }
    /* See the matching comment in bus_read(): both window checks can only
     * ever match addr < 0xD0000. */
    if (addr < 0xD0000) {
        if (vga_mem_window(s, addr, size, &voff)) {
            for (unsigned i = 0; i < size; i++)
                vga_ibm_mem_write(&s->vga, voff + i, (uint8_t)(val >> (i * 8)));
            return;
        }
        /* Read-only, like a real (locked) option ROM shadow: this is what
         * keeps a generic "clear all of system RAM" firmware loop from
         * wiping the video BIOS out before it's ever used, since real
         * hardware wouldn't report this range as regular RAM in the first
         * place. */
        if (vga_rom_window(addr, size, &voff))
            return;
    }
    if (region6 && addr + size <= I2000_FLASH_BASE) {
        uint32_t saved = s->pci_cfg_addr;
        s->pci_cfg_addr = 0x80000000u | ((uint32_t)addr & 0xfffffffcu);
        pci_cfg_write(s, (unsigned)(addr & 3), val, size);
        s->pci_cfg_addr = saved;
        return;
    }
    if (addr + size <= s->ram_size) {
        if (addr <= UINT64_C(0xE100) && addr + size > UINT64_C(0xE000) &&
            getenv("INT13_LOAD_DEBUG")) {
            fprintf(stderr, "merced: INT13-REGION-WRITE addr=%016" PRIX64
                    " size=%u val=%016" PRIX64 " ip=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", addr, size, val,
                    s->cpu->ip, s->cpu->ninsts);
            fflush(stderr);
        }
        memcpy(s->ram + addr, &val, size);
        return;
    }
    if (addr >= I2000_CHIPSET_SCRATCH_BASE && addr + size <= I2000_RAM_MAX) {
        if (addr <= UINT64_C(0x7FFB0FEC) && addr + size > UINT64_C(0x7FFB0FEC) &&
            getenv("SCRATCH_DEBUG")) {
            fprintf(stderr, "merced: SCRATCH-WRITE addr=%016" PRIX64
                    " size=%u val=%016" PRIX64 " ip=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", addr, size, val,
                    s->cpu->ip, s->cpu->ninsts);
            fflush(stderr);
        }
        if (addr <= UINT64_C(0x7FFA0068) && addr + size > UINT64_C(0x7FFA0068) &&
            getenv("SAPIC_PTR_DEBUG")) {
            fprintf(stderr, "merced: SAPICPTR-WRITE addr=%016" PRIX64
                    " size=%u val=%016" PRIX64 " ip=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", addr, size, val,
                    s->cpu->ip, s->cpu->ninsts);
            fflush(stderr);
        }
        memcpy(s->chipset_scratch + (addr - I2000_CHIPSET_SCRATCH_BASE), &val, size);
        return;
    }
    if (addr >= IOSAPIC_BASE && addr < IOSAPIC_BASE + IOSAPIC_SIZE && size == 4) {
        uint32_t off = (uint32_t)(addr - IOSAPIC_BASE);
        if (off == IOSAPIC_REGSEL_OFF) { s->iosapic_regsel = (uint32_t)val & 0xFF; return; }
        if (off == IOSAPIC_WINDOW_OFF) { iosapic_indirect_write(s, (uint32_t)val); return; }
        /* EOI (off == IOSAPIC_EOI_OFF): we don't model level-triggered RIRR
         * resampling, so there's nothing to do here beyond accepting it. */
        return;
    }
    if (addr - I2000_FLASH_BASE < I2000_FLASH_SIZE) {
        uint64_t off = addr - I2000_FLASH_BASE;
        unsigned chip_index = (unsigned)(off / I2000_FLASH_CHIP_SIZE);
        uint64_t chip_off = off % I2000_FLASH_CHIP_SIZE;
        I2000FlashChip *chip = &s->flash_chip[chip_index];
        /* BIOS 1.30 probes the Intel flash device with the standard
         * clear-status/read-status/read-array command sequence.  Without
        * command-state handling the status read returns an array byte and
        * platform initialization reports EFI_OUT_OF_RESOURCES. */
        if (size == 1) {
            if (chip->cmd == 0x40 || chip->cmd == 0x10) {
                /* Intel byte-program operation.  Keep NVRAM updates in the
                 * in-memory image; the user's ROM file remains untouched. */
                uint8_t old = s->flash[off];
                /* Intel NOR programming can only clear bits.  FMM relies on
                 * that property to commit a record header in stages: writes
                 * such as 3f -> 2f -> 23 -> f5 -> f1 must leave 0x21, not
                 * replace it with 0xf1.  Treating program data as an ordinary
                 * RAM store changes the record class and CDBFMM returns 0x2d.
                 * A preceding block erase is what permits 0 -> 1 changes. */
                s->flash[off] = old & (uint8_t)val;
                /* The 460GX firmware window is shadowed into RAM after
                 * relocation.  FMM programs through the top-of-4GiB FWH
                 * alias but may verify through the low RAM shadow.  Keep
                 * both views coherent; otherwise a successful program is
                 * immediately read back as the stale pre-program byte and
                 * CDBFMM rejects the completed transaction. */
                if (s->fw_shadow_enabled &&
                    I2000_FW_SHADOW_BASE + off < s->ram_size)
                    s->ram[I2000_FW_SHADOW_BASE + off] = s->flash[off];
                s->nvram_dirty |= s->flash[off] != old;
                chip->status = 0x80;
                chip->read_status = true;
                chip->read_id = false;
                chip->cmd = 0;
                return;
            }
            if (chip->cmd == 0x20) {
                if ((uint8_t)val == 0xD0) {
                    uint64_t block = (uint64_t)chip_index *
                                     I2000_FLASH_CHIP_SIZE +
                                     (chip->cmd_addr & ~0xFFFFu);
                    if (block + 0x10000 <= I2000_FLASH_SIZE) {
                        memset(s->flash + block, 0xFF, 0x10000);
                        if (s->fw_shadow_enabled &&
                            I2000_FW_SHADOW_BASE + block + 0x10000 <=
                                s->ram_size)
                            memset(s->ram + I2000_FW_SHADOW_BASE + block,
                                   0xFF, 0x10000);
                        s->nvram_dirty = true;
                    }
                    chip->status = 0x80;
                } else {
                    chip->status = 0xB0; /* ready + erase error */
                }
                if (getenv("FLASH_DEBUG"))
                    fprintf(stderr, "i2000: FLASH erase confirm off=%#" PRIx64
                            " val=%#04x -> status=%#04x ninsts=%" PRIu64 "\n",
                            off, (unsigned)(uint8_t)val,
                            chip->status, s->cpu->ninsts);
                chip->read_status = true;
                chip->read_id = false;
                chip->cmd = 0;
                return;
            }
            if (getenv("FLASH_DEBUG") && (uint8_t)val != 0x70 &&
                (uint8_t)val >= 0x10)
                fprintf(stderr, "i2000: FLASH cmd=%#04x off=%#" PRIx64
                        " ninsts=%" PRIu64 "\n", (unsigned)(uint8_t)val, off,
                        s->cpu->ninsts);
            if (((uint8_t)val == 0x90 || (uint8_t)val == 0x91) &&
                off == 0xc0000 && s->cpu->ninsts > 5000000000ull &&
                getenv("FLASH_ID_TRACE")) {
                static bool traced;
                if (!traced) {
                    traced = true;
                    fprintf(stderr, "i2000: FLASH ID probe off=0xc0000 ip=%016"
                            PRIx64 " ninsts=%" PRIu64 " call history:\n",
                            s->cpu->ip, s->cpu->ninsts);
                    merced_dump_calls(s->cpu, 24, stderr);
                    s->cpu->trace_n = 150;
                }
            }
            switch ((uint8_t)val) {
            case 0x50:                         /* clear status register */
                chip->status = 0x80;           /* ready, no errors */
                return;
            case 0x70:                         /* read status register */
                chip->read_status = true;
                chip->read_id = false;
                return;
            case 0x90: case 0x91:              /* read identifier codes */
                chip->read_status = false;
                chip->read_id = true;
                return;
            case 0x10: case 0x40:              /* byte program setup */
                chip->cmd = (uint8_t)val;
                chip->cmd_addr = (uint32_t)chip_off;
                return;
            case 0x20:                         /* block erase setup */
                chip->cmd = 0x20;
                chip->cmd_addr = (uint32_t)chip_off;
                return;
            case 0xFF:                         /* read array */
                chip->read_status = false;
                chip->read_id = false;
                chip->cmd = 0;
                return;
            }
        }
        /* Shadowed: writes through the alias land in the RAM copy.
         * Unshadowed: flash programming cycles are ignored for now. */
        if (s->fw_shadow_enabled && off + size <= I2000_FLASH_SIZE &&
            I2000_FW_SHADOW_BASE + I2000_FLASH_SIZE <= s->ram_size)
            memcpy(s->ram + I2000_FW_SHADOW_BASE + off, &val, size);
        return;
    }
    if (addr - I2000_IO_BASE < I2000_IO_SIZE) {
        uint64_t port;
        if (sparse_io_port(addr - I2000_IO_BASE, &port)) {
            if (getenv("MOUSE_DEBUG") && (port == 0x60 || port == 0x64))
                fprintf(stderr, "MOUSE_DEBUG: sparse MMIO write port=%#x "
                        "size=%u val=%#" PRIx64 " ninsts=%" PRIu64 "\n",
                        (unsigned)port, size, val, s->cpu->ninsts);
            io_port_write(s, port, val, size);
            return;
        }
    }
    if (addr == I2000_SAC_CBNR && size == 4) {
        s->sac_cbnr = (uint32_t)val;
        return;
    }
    if (addr == I2000_SAC_CCSR && size == 4) {
        s->sac_ccsr = (uint32_t)val;
        return;
    }
    mmio_log(s, addr, val, size, true);
}

uint8_t ia64_i2000_phys_read8(Ia64I2000State *s, uint64_t addr) {
    return (uint8_t)bus_read(s, addr & MERCED_PHYS_MASK, 1);
}
void ia64_i2000_phys_write8(Ia64I2000State *s, uint64_t addr, uint8_t val) {
    bus_write(s, addr & MERCED_PHYS_MASK, val, 1);
}

static bool i2000_bus_fill(void *ud, uint64_t addr, uint8_t val, uint64_t len) {
    Ia64I2000State *s = ud;
    if (addr > s->ram_size || len > s->ram_size - addr)
        return false;
    memset(s->ram + addr, val, (size_t)len);
    return true;
}

/* ── Firmware ────────────────────────────────────────────────────────────── */

static bool i2000_repair_sdv_boot_timeout(Ia64I2000State *s) {
    static const uint8_t name[] = {
        'T',0,'i',0,'m',0,'e',0,'o',0,'u',0,'t',0,0,0
    };
    /* A former menu workaround changed this CDB-backed value directly.  That
     * bypassed the firmware's CDB bookkeeping, so EFI detected an altered CDB
     * and eventually failed trying to save it.  Undo only values that GEMU's
     * workaround used; leave genuine guest configuration untouched.  This is
     * also needed for snapshots and NVRAM files created by that version. */
    for (size_t off = 0; off + sizeof(name) + 2 <= I2000_FLASH_SIZE; off++) {
        if (memcmp(s->flash + off, name, sizeof(name)) != 0)
            continue;
        uint8_t *value = s->flash + off + sizeof(name);
        uint16_t timeout = (uint16_t)value[0] | (uint16_t)value[1] << 8;
        if (timeout != 60 && timeout != 3600 && timeout != 30000 &&
            timeout != 65534)
            continue;
        value[0] = 7;
        value[1] = 0;
        fprintf(stderr, "i2000: repaired obsolete GEMU SDV boot timeout "
                "(%u -> 7)\n", timeout);
        return true;
    }
    return false;
}

static bool i2000_nvram_repair_bad_prefix(Ia64I2000State *s,
                                          const uint8_t *rom_prefix) {
    static const uint32_t factory[2] = { 0x390000, 0x3A0000 };
    /* An earlier emulator version copied these ROM-resident factory record
     * lists verbatim into the writable banks. They are input records, not
     * formatted runtime CDB banks: firmware reports "No CDB found" and
     * parks with status 5. Recognize only that exact emulator-created image
     * so genuine guest NVRAM is never discarded. */
    bool copied_factory =
        !memcmp(s->flash, s->flash + factory[0], 0x10000) &&
        !memcmp(s->flash + 0x10000, s->flash + factory[1], 0x10000);
    bool erased = true;
    for (uint32_t i = 0; i < 0x20000 && erased; i++)
        erased = s->flash[i] == 0xff;
    if (!copied_factory && !erased)
        return false;
    /* This prefix is not wholly NVRAM: the firmware image contains required
     * CDB data around 0x17d20. Restore it byte-for-byte from the pristine
     * ROM rather than erasing the whole range. */
    memcpy(s->flash, rom_prefix, 0x20000);
    return true;
}

static void i2000_nvram_load(Ia64I2000State *s) {
    if (!s->nvram_path[0])
        gemu_config_path(s->nvram_path, sizeof(s->nvram_path), "i2000.nvram");
    if (getenv("I2000_IGNORE_NVRAM")) {
        printf("i2000: ignoring saved NVRAM by request\n");
        return;
    }
    FILE *f = fopen(s->nvram_path, "rb");
    if (!f)
        return;
    uint8_t *image = malloc(I2000_FLASH_SIZE);
    size_t got = image ? fread(image, 1, I2000_FLASH_SIZE, f) : 0;
    int extra = fgetc(f);
    fclose(f);
    if (got != I2000_FLASH_SIZE || extra != EOF) {
        fprintf(stderr, "i2000: ignoring malformed NVRAM '%s'\n",
                s->nvram_path);
        free(image);
        return;
    }
    uint8_t *rom_prefix = malloc(0x20000);
    if (!rom_prefix) {
        free(image);
        return;
    }
    memcpy(rom_prefix, s->flash, 0x20000);
    memcpy(s->flash, image, I2000_FLASH_SIZE);
    free(image);
    if (i2000_nvram_repair_bad_prefix(s, rom_prefix)) {
        s->nvram_dirty = true;
        printf("i2000: repaired corrupted NVRAM/ROM prefix\n");
    }
    free(rom_prefix);
    printf("i2000: loaded NVRAM '%s'\n", s->nvram_path);
}

static void i2000_nvram_save(Ia64I2000State *s) {
    /* A diagnostic pristine-ROM boot must not overwrite the user's saved
     * database with a transaction interrupted by a firmware assertion. */
    if (getenv("I2000_IGNORE_NVRAM"))
        return;
    if (!s->nvram_dirty || !s->nvram_path[0] || !s->flash)
        return;
    gemu_ensure_parent_dir(s->nvram_path);
    char tmp[sizeof(s->nvram_path) + 5];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->nvram_path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(s->flash, 1, I2000_FLASH_SIZE, f) ==
                   I2000_FLASH_SIZE && fflush(f) == 0;
    if (f && fclose(f) != 0)
        ok = false;
    if (!ok || rename(tmp, s->nvram_path) != 0) {
        remove(tmp);
        fprintf(stderr, "i2000: cannot save NVRAM '%s'\n", s->nvram_path);
        return;
    }
    s->nvram_dirty = false;
    printf("i2000: saved NVRAM '%s'\n", s->nvram_path);
}

bool ia64_i2000_load_firmware(Ia64I2000State *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gemu: cannot open firmware '%s'\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || (uint64_t)len > I2000_FLASH_SIZE) {
        fprintf(stderr, "gemu: firmware '%s' is %ld bytes (expected at most %u)\n",
                path, len, I2000_FLASH_SIZE);
        fclose(f);
        return false;
    }
    uint32_t off = I2000_FLASH_SIZE - (uint32_t)len;
    memset(s->flash, 0xFF, off);
    size_t rd = fread(s->flash + off, 1, (size_t)len, f);
    fclose(f);
    if (rd != (size_t)len) {
        fprintf(stderr, "gemu: short read on firmware '%s'\n", path);
        return false;
    }
    snprintf(s->flash_file, sizeof(s->flash_file), "%s", path);
    s->flash_image_size = (uint32_t)len;
    s->flash_loaded = true;

    /* A CDB is meaningful only with the firmware image which formatted it.
     * Sharing the old i2000.nvram between the SDV debug image and an HP/AMI
     * production image lets one BIOS replace the other's code and database,
     * eventually tripping CDBFMM's format assertion.  Key persistence by the
     * pristine, fully-positioned flash image so each firmware has an
     * independent configuration database. */
    uint64_t firmware_id = UINT64_C(1469598103934665603);
    for (uint32_t i = 0; i < I2000_FLASH_SIZE; i++) {
        firmware_id ^= s->flash[i];
        firmware_id *= UINT64_C(1099511628211);
    }
    char nvram_name[64];
    snprintf(nvram_name, sizeof(nvram_name), "i2000-%016" PRIx64 ".nvram",
             firmware_id);
    gemu_config_path(s->nvram_path, sizeof(s->nvram_path), nvram_name);
    i2000_nvram_load(s);
    if (i2000_repair_sdv_boot_timeout(s))
        s->nvram_dirty = true;
    gemu_monitor_register_rom(s->monitor, (uint32_t)(I2000_FLASH_BASE + off),
                              (uint32_t)len, path);
    return true;
}

/* ── Front panel ─────────────────────────────────────────────────────────── */

#define C_BG     0xFF101418
#define C_TITLE  0xFFFFB000
#define C_TEXT   0xFFB0B8C0
#define C_DIM    0xFF586068
#define C_GOOD   0xFF40C040
#define C_BAD    0xFFE05050
#define C_CON    0xFF30D030

static void panel_text(Ia64I2000State *s, int col, int row,
                       uint32_t color, const char *str) {
    for (; *str && col < COLS; str++, col++) {
        char c = *str;
        if (c < 32 || c > 122) c = '?';
        const uint8_t *g = gemu_font_glyph(c);
        int px = col * CELL_W, py = row * CELL_H;
        for (int x = 0; x < CELL_W; x++)
            for (int y = 0; y < 8; y++)
                if (g[x] & (1 << y))
                    s->fb[(py + y) * FB_W + (px + x)] = color;
    }
}

static void panel_render(Ia64I2000State *s) {
    char line[COLS + 1];
    Merced *m = s->cpu;

    for (int i = 0; i < FB_W * FB_H; i++)
        s->fb[i] = C_BG;

    panel_text(s, 0, 0, C_TITLE,
               "HP i2000 - Intel Itanium (Merced) - GEMU front panel");
    snprintf(line, sizeof(line), "POST %02X", s->post_code);
    panel_text(s, COLS - 8, 0, C_TITLE, line);

    snprintf(line, sizeof(line),
             "IP %016" PRIX64 ".%u  insts %-12" PRIu64 " faults %" PRIu64,
             m->ip & ~0xFull, (unsigned)(m->ip & 0xF), m->ninsts, m->nfaults);
    panel_text(s, 0, 2, C_TEXT, line);

    snprintf(line, sizeof(line),
             "PSR %016" PRIX64 "  ic=%u i=%u it=%u dt=%u bn=%u  CFM sof=%u sol=%u",
             m->psr,
             (unsigned)((m->psr >> 13) & 1), (unsigned)((m->psr >> 14) & 1),
             (unsigned)((m->psr >> 36) & 1), (unsigned)((m->psr >> 17) & 1),
             (unsigned)((m->psr >> 44) & 1),
             (unsigned)(m->cfm & 0x7F), (unsigned)((m->cfm >> 7) & 0x7F));
    panel_text(s, 0, 3, C_TEXT, line);

    if (s->halted) {
        panel_text(s, 0, 5, C_BAD, "HALTED:");
        panel_text(s, 8, 5, C_BAD, m->halt_msg);
    } else {
        panel_text(s, 0, 5, C_GOOD, "RUNNING");
    }

    panel_text(s, 0, 7, C_DIM, "unhandled MMIO (addr / last value / count):");
    for (int i = 0; i < s->mmio_log_n; i++) {
        MmioLogEnt *e = &s->mmio_log[i];
        snprintf(line, sizeof(line), "%c 0x%012" PRIX64 " %u  0x%-16" PRIX64 " x%u",
                 e->is_write ? 'W' : 'R', e->addr, e->size, e->val, e->count);
        panel_text(s, 2, 8 + i, C_DIM, line);
    }

    int con_top = ROWS - CON_ROWS - 1;
    panel_text(s, 0, con_top, C_DIM,
               "--- COM1 ------------------------------------------------------------------------------------------------");
    for (int r = 0; r < CON_ROWS; r++)
        panel_text(s, 0, con_top + 1 + r, C_CON, s->console[r]);
}

/* ── Monitor callbacks ───────────────────────────────────────────────────── */

static void i2000_cpu_state(void *ud, char *buf, size_t buf_len) {
    Ia64I2000State *s = ud;
    merced_dump_state(s->cpu, buf, buf_len);
}

static void i2000_repair_saved_mode12(Ia64I2000State *s) {
    if (s->ram[0x449] != 0x12 || s->vga.crtc[1] || s->vga.seq[2])
        return;
    static const uint8_t seq[5] = { 0x03,0x01,0x0f,0x00,0x06 };
    static const uint8_t crtc[25] = {
        0x5f,0x4f,0x50,0x82,0x54,0x80,0x0b,0x3e,0x00,0x40,0x00,0x00,0x00,
        0x00,0x00,0x00,0xea,0x0c,0xdf,0x28,0x00,0xe7,0x04,0xe3,0xff
    };
    static const uint8_t gc[9] = { 0,0,0,0,0,0,0x05,0x0f,0xff };
    static const uint8_t attr[21] = {
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x01,0,0x0f,0,0
    };
    static const uint8_t ega[16][3] = {
        {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}
    };
    s->vga.misc_output = 0xe3;
    memcpy(s->vga.seq, seq, sizeof(seq));
    memcpy(s->vga.crtc, crtc, sizeof(crtc));
    memcpy(s->vga.gc, gc, sizeof(gc));
    memcpy(s->vga.attr, attr, sizeof(attr));
    memcpy(s->vga.dac, ega, sizeof(ega));
    s->vga.dac_mask = 0xff;
    s->vga.attr_index = 0x20;
    s->vga.attr_flipflop = false;
    fprintf(stderr, "i2000: repaired legacy snapshot VGA mode 12 state\n");
}

static void i2000_render_frame(Ia64I2000State *s) {
    if (s->rage128.enabled &&
        rage128_render(&s->rage128, s->fb, FB_W, FB_H))
        return;
    if (getenv("VGA_TEXT_DEBUG")) {
        static unsigned printed;
        if (printed < 20) {
            printed++;
            fprintf(stderr, "i2000: VGA-TEXT #%u ninsts=%" PRIu64 " crtc[1]=%02x crtc[7]=%02x "
                    "crtc[9]=%02x crtc[0x12]=%02x crtc[0x13]=%02x "
                    "crtc[0x14]=%02x crtc[0x17]=%02x seq[4]=%02x gc[6]=%02x\n",
                    printed, s->cpu->ninsts, s->vga.crtc[1], s->vga.crtc[7],
                    s->vga.crtc[9], s->vga.crtc[0x12], s->vga.crtc[0x13],
                    s->vga.crtc[0x14], s->vga.crtc[0x17], s->vga.seq[4],
                    s->vga.gc[6]);
            fprintf(stderr, "i2000: VGA-WRITEREGS seq[2]=%02x gc[0]=%02x "
                    "gc[1]=%02x gc[3]=%02x gc[4]=%02x gc[5]=%02x gc[8]=%02x "
                    "attr[0x11]=%02x attr[0x12]=%02x\n",
                    s->vga.seq[2], s->vga.gc[0], s->vga.gc[1], s->vga.gc[3],
                    s->vga.gc[4], s->vga.gc[5], s->vga.gc[8],
                    s->vga.attr[0x11], s->vga.attr[0x12]);
            fprintf(stderr, "i2000: VGA-ATTR ");
            for (int i = 0; i < 16; i++)
                fprintf(stderr, "%02x ", s->vga.attr[i]);
            fprintf(stderr, "\ni2000: VGA-ATTR-MODE attr[0x10]=%02x attr[0x13]=%02x "
                    "attr[0x14]=%02x\n", s->vga.attr[0x10], s->vga.attr[0x13],
                    s->vga.attr[0x14]);
            fprintf(stderr, "i2000: VGA-DAC(idx0-15) ");
            for (int i = 0; i < 16; i++)
                fprintf(stderr, "[%u]=%02x,%02x,%02x ", i,
                        s->vga.dac[i][0], s->vga.dac[i][1], s->vga.dac[i][2]);
            fprintf(stderr, "\ni2000: VGA-DAC(idx16-63) ");
            for (int i = 16; i < 64; i++)
                fprintf(stderr, "[%u]=%02x,%02x,%02x ", i,
                        s->vga.dac[i][0], s->vga.dac[i][1], s->vga.dac[i][2]);
            fprintf(stderr, "\n");
            {
                /* Verified against exact pixel samples of a screendump:
                 * y=10 = solid blue outer margin/frame (no text),
                 * y=30 = teal band with "BIOS Configuration Manager" text,
                 * y=65 = black tab-row band, y=200 = content pane. */
                static const struct { const char *name; int row; } regions[] = {
                    { "MARGIN", 10 }, { "REALTITLE", 30 },
                    { "TABROW", 65 }, { "CONTENT", 200 },
                    { "BOTTOMBAND", 440 }, { "CONTENTEDGE", 425 },
                };
                for (size_t r = 0; r < sizeof(regions) / sizeof(regions[0]); r++) {
                    for (int plane = 0; plane < 4; plane++) {
                        fprintf(stderr, "i2000: VGA-%s plane%d row%d[20..45]=",
                                regions[r].name, plane, regions[r].row);
                        for (int i = 20; i < 45; i++)
                            fprintf(stderr, "%02x ",
                                    s->vga.vram[plane][regions[r].row * 80 + i]);
                        fprintf(stderr, "\n");
                    }
                }
            }
            for (int plane = 0; plane < 4; plane++) {
                for (int row = 200; row < 204; row++) {
                    fprintf(stderr, "i2000: VGA-BODY plane%d row%d[0..15]=",
                            plane, row);
                    for (int i = 20; i < 30; i++)
                        fprintf(stderr, "%02x ",
                                s->vga.vram[plane][row * 80 + i]);
                    fprintf(stderr, "\n");
                }
            }
            for (int plane = 0; plane < 4; plane++) {
                for (int row = 16; row < 22; row++) {
                    fprintf(stderr, "i2000: VGA-TEXT plane%d row%d[0..47]=",
                            plane, row);
                    for (int i = 0; i < 48; i++)
                        fprintf(stderr, "%02x ",
                                s->vga.vram[plane][row * 80 + i]);
                    fprintf(stderr, "\n");
                }
            }
            fflush(stderr);
        }
    }
    vga_ibm_render(&s->vga, s->fb, FB_W, FB_H, vgafont16);
}

static bool i2000_screendump(void *ud, const char *path) {
    Ia64I2000State *s = ud;
    if (s->rage128.enabled) {
        size_t nonzero = rage128_nonzero_vram(&s->rage128);
        fprintf(stderr,
                "i2000: Rage128 screen gen=%08X hdisp=%08X vdisp=%08X "
                "offset=%08X pitch=%08X nonzero_vram=%zu int10=%02X%02X:%02X%02X\n",
                rage128_reg32(&s->rage128, 0x0050),
                rage128_reg32(&s->rage128, 0x0200),
                rage128_reg32(&s->rage128, 0x0208),
                rage128_reg32(&s->rage128, 0x0224),
                rage128_reg32(&s->rage128, 0x022c), nonzero,
                s->ram[0x43], s->ram[0x42],
                s->ram[0x41], s->ram[0x40]);
    }
    i2000_render_frame(s);
    return gemu_screendump_argb(path, s->fb, FB_W, FB_H);
}

/* The SDV EFI presentation environment leaves cr.iva at zero and obtains
 * console keys through its CSM/INT16-compatible BIOS data-area ring.  On
 * real hardware the legacy IRQ1 thunk transfers 8042 scan codes into this
 * buffer.  GEMU deliberately suppresses interrupts while IVA is zero (the
 * native vector at 0x3000 is empty), so complete that transfer here for host
 * input while execution is in the 0x7fxxxxxx firmware presentation code.
 * Outside that narrowly identified environment the real PS/2 path remains
 * authoritative, avoiding duplicate keys for an OS with a live IRQ1 stack. */
static uint8_t i2000_bios_scan(uint8_t ascii) {
    static const uint8_t letters[26] = {
        0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
        0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c
    };
    if (ascii >= 'a' && ascii <= 'z') return letters[ascii - 'a'];
    if (ascii >= 'A' && ascii <= 'Z') return letters[ascii - 'A'];
    if (ascii >= '1' && ascii <= '9') return (uint8_t)(ascii - '1' + 2);
    if (ascii == '0') return 0x0b;
    switch (ascii) {
    case '\r': case '\n': return 0x1c;
    case '\b': return 0x0e;
    case '\t': return 0x0f;
    case 0x1b: return 0x01;
    case ' ': return 0x39;
    case '-': return 0x0c; case '=': return 0x0d;
    case '[': return 0x1a; case ']': return 0x1b;
    case ';': return 0x27; case '\'': return 0x28;
    case '`': return 0x29; case '\\': return 0x2b;
    case ',': return 0x33; case '.': return 0x34; case '/': return 0x35;
    default: return 0;
    }
}

static void i2000_efi_bios_key(Ia64I2000State *s, uint8_t ascii) {
    if (!ascii || s->ram_size <= 0x43d || s->cpu->cr[2] != 0 ||
        s->cpu->ip < UINT64_C(0x7f000000) ||
        s->cpu->ip >= UINT64_C(0x80000000))
        return;
    uint16_t head = (uint16_t)(s->ram[0x41a] | (s->ram[0x41b] << 8));
    uint16_t tail = (uint16_t)(s->ram[0x41c] | (s->ram[0x41d] << 8));
    if (head < 0x1e || head >= 0x3e || (head & 1) ||
        tail < 0x1e || tail >= 0x3e || (tail & 1))
        return;
    uint16_t next = (uint16_t)(tail + 2);
    if (next >= 0x3e) next = 0x1e;
    if (next == head) return; /* BIOS ring full */
    s->ram[0x400 + tail] = ascii;
    s->ram[0x401 + tail] = i2000_bios_scan(ascii);
    s->ram[0x41c] = (uint8_t)next;
    s->ram[0x41d] = (uint8_t)(next >> 8);
}

/* Snapshot format: full-machine save/restore so a slow, deterministic boot
 * only ever has to run once. Everything in Ia64I2000State and Merced is
 * plain data (fixed-size arrays and scalars) except a handful of pointers
 * that are only meaningful within one process's lifetime - live handles
 * (monitor/display), the CPU's back-reference and bus hookup, and buffers
 * whose CONTENTS need saving but whose addresses obviously can't be
 * (ram, flash, atapi_data). The save/restore strategy is: snapshot those
 * buffers' contents separately, then bulk-copy the rest of each struct in
 * one shot with the pointer fields blanked out (save) or preserved from the
 * live, already-correctly-allocated instance (load) - far less fragile
 * than hand-listing every scalar field, and it stays correct automatically
 * as fields get added. */
#define I2000_SNAPSHOT_MAGIC 0x32304B32554D4547ull /* "GEMU2K02" */
#define I2000_SNAPSHOT_VERSION 23u /* v23 adds PIC OCW3 IRR/ISR selection */

static bool i2000_save_snapshot(Ia64I2000State *s, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = true;
    uint64_t magic = I2000_SNAPSHOT_MAGIC;
    uint32_t version = I2000_SNAPSHOT_VERSION;
    ok &= fwrite(&magic, sizeof(magic), 1, f) == 1;
    ok &= fwrite(&version, sizeof(version), 1, f) == 1;
    ok &= fwrite(&s->ram_size, sizeof(s->ram_size), 1, f) == 1;
    ok &= fwrite(s->ram, 1, s->ram_size, f) == s->ram_size;
    ok &= fwrite(s->flash, 1, I2000_FLASH_SIZE, f) == I2000_FLASH_SIZE;
    ok &= fwrite(s->chipset_scratch, 1, I2000_CHIPSET_SCRATCH_SIZE, f) ==
          I2000_CHIPSET_SCRATCH_SIZE;
    ok &= fwrite(s->high_dram, 1, I2000_HIGH_DRAM_SIZE, f) ==
          I2000_HIGH_DRAM_SIZE;
    uint64_t atapi_len = (uint64_t)s->atapi_data_len;
    ok &= fwrite(&atapi_len, sizeof(atapi_len), 1, f) == 1;
    if (atapi_len)
        ok &= fwrite(s->atapi_data, 1, atapi_len, f) == atapi_len;
    uint64_t ata_len = (uint64_t)s->ata_data_len;
    ok &= fwrite(&ata_len, sizeof(ata_len), 1, f) == 1;
    if (ata_len)
        ok &= fwrite(s->ata_data, 1, ata_len, f) == ata_len;

    Ia64I2000State *snap = malloc(sizeof(*snap));
    if (!snap) { fclose(f); return false; }
    *snap = *s;
    snap->monitor = NULL;
    snap->display = NULL;
    snap->vnc = NULL;
    snap->cpu = NULL;
    snap->ram = NULL;
    snap->flash = NULL;
    snap->chipset_scratch = NULL;
    snap->high_dram = NULL;
    snap->cdrom = NULL;
    snap->atapi_data = NULL;
    snap->hda = NULL;
    snap->ata_data = NULL;
    snap->i82559.guest_read = NULL;
    snap->i82559.guest_write = NULL;
    snap->i82559.guest_opaque = NULL;
    snap->i82559.option_rom = NULL;
    ok &= fwrite(snap, sizeof(*snap), 1, f) == 1;
    free(snap);

    Merced cpu_snap = *s->cpu;
    memset(&cpu_snap.bus, 0, sizeof(cpu_snap.bus));
    cpu_snap.translation_cache = NULL;
    ok &= fwrite(&cpu_snap, sizeof(cpu_snap), 1, f) == 1;

    fclose(f);
    return ok;
}

static bool i2000_load_snapshot(Ia64I2000State *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = true;
    uint64_t magic = 0;
    uint32_t version = 0;
    uint64_t ram_size = 0;
    ok &= fread(&magic, sizeof(magic), 1, f) == 1;
    ok &= fread(&version, sizeof(version), 1, f) == 1;
    ok &= fread(&ram_size, sizeof(ram_size), 1, f) == 1;
    if (!ok || magic != I2000_SNAPSHOT_MAGIC ||
        (version < 9u || version > I2000_SNAPSHOT_VERSION) ||
        ram_size != s->ram_size) {
        fclose(f);
        return false;
    }
    uint8_t rom_prefix[0x20000];
    memcpy(rom_prefix, s->flash, sizeof(rom_prefix));
    ok &= fread(s->ram, 1, s->ram_size, f) == s->ram_size;
    ok &= fread(s->flash, 1, I2000_FLASH_SIZE, f) == I2000_FLASH_SIZE;
    if (ok)
        i2000_repair_sdv_boot_timeout(s);
    ok &= fread(s->chipset_scratch, 1, I2000_CHIPSET_SCRATCH_SIZE, f) ==
          I2000_CHIPSET_SCRATCH_SIZE;
    if (version >= 10u)
        ok &= fread(s->high_dram, 1, I2000_HIGH_DRAM_SIZE, f) ==
              I2000_HIGH_DRAM_SIZE;
    else
        memset(s->high_dram, 0, I2000_HIGH_DRAM_SIZE);
    if (ok && i2000_nvram_repair_bad_prefix(s, rom_prefix))
        printf("i2000: repaired corrupted snapshot NVRAM/ROM prefix\n");

    uint64_t atapi_len = 0;
    ok &= fread(&atapi_len, sizeof(atapi_len), 1, f) == 1;
    free(s->atapi_data);
    s->atapi_data = atapi_len ? malloc((size_t)atapi_len) : NULL;
    if (atapi_len) {
        if (!s->atapi_data) { fclose(f); return false; }
        ok &= fread(s->atapi_data, 1, atapi_len, f) == atapi_len;
    }
    uint64_t ata_len = 0;
    ok &= fread(&ata_len, sizeof(ata_len), 1, f) == 1;
    free(s->ata_data);
    s->ata_data = ata_len ? malloc((size_t)ata_len) : NULL;
    if (ata_len) {
        if (!s->ata_data) { fclose(f); return false; }
        ok &= fread(s->ata_data, 1, ata_len, f) == ata_len;
    }

    GemuMonitor *monitor = s->monitor;
    GemuDisplay *display = s->display;
    GemuVncServer *vnc = s->vnc;
    bool configured_rage128 = s->rage128.enabled;
    bool configured_mouse_enabled = s->mouse_enabled;
    bool configured_i82559 = s->i82559.enabled;
    char configured_cdrom_file[sizeof(s->cdrom_file)];
    snprintf(configured_cdrom_file, sizeof(configured_cdrom_file), "%s",
             s->cdrom ? s->cdrom_file : "");
    uint64_t configured_cdrom_size = s->cdrom ? s->cdrom_size : 0;
    uint8_t *i82559_option_rom = s->i82559.option_rom;
    size_t i82559_option_rom_size = s->i82559.option_rom_size;
    Merced *cpu = s->cpu;
    uint8_t *ram = s->ram;
    uint8_t *flash = s->flash;
    uint8_t *chipset_scratch = s->chipset_scratch;
    uint8_t *high_dram = s->high_dram;
    uint8_t *atapi_data = s->atapi_data;
    uint8_t *ata_data = s->ata_data;
    if (s->cdrom) fclose(s->cdrom);
    if (s->hda) fclose(s->hda);
    Ia64I2000State *loaded = calloc(1, sizeof(*loaded));
    if (!loaded) { fclose(f); return false; }
    size_t state_size;
    if (version == 3u)
        state_size = offsetof(Ia64I2000State, vnc);
    else if (version == 4u)
        state_size = offsetof(Ia64I2000State, rage128);
    else if (version == 5u)
        state_size = offsetof(Ia64I2000State, int10_req);
    else if (version == 6u)
        state_size = offsetof(Ia64I2000State, mouse_enabled);
    else if (version == 7u)
        state_size = offsetof(Ia64I2000State, mouse_retry_ninsts);
    else if (version == 8u)
        state_size = offsetof(Ia64I2000State, mouse_wait_rfi_generation);
    else if (version == 9u)
        state_size = offsetof(Ia64I2000State, high_dram);
    else if (version == 10u)
        state_size = offsetof(Ia64I2000State, nvram_path);
    else if (version == 11u)
        /* v11 ended after a bool, but sizeof(struct) included tail padding
         * to its 8-byte alignment. Consume that padding or every following
         * snapshot section becomes byte-shifted. */
        state_size = (offsetof(Ia64I2000State, keyboard_irq_pending) + 7u) & ~7u;
    else if (version == 12u)
        state_size = (offsetof(Ia64I2000State, pit0_read_phase) + 7u) & ~7u;
    else if (version == 13u)
        state_size = offsetof(Ia64I2000State, i82559);
    else if (version == 14u)
        state_size = offsetof(Ia64I2000State, ps2_keyboard);
    else if (version == 15u)
        state_size = offsetof(Ia64I2000State, atapi_irq_pending);
    else if (version == 16u)
        /* v16 ended in a bool and sizeof(struct) included tail padding. */
        state_size = (offsetof(Ia64I2000State, pic_master_isr) + 7u) & ~7u;
    else if (version == 17u)
        state_size = offsetof(Ia64I2000State, atapi_irq_ready_ninsts);
    else if (version == 18u)
        state_size = offsetof(Ia64I2000State, flash_chip);
    else if (version == 19u || version == 20u)
        state_size = offsetof(Ia64I2000State, atapi_byte_limit);
    else if (version == 21u)
        state_size = offsetof(Ia64I2000State, atapi_status_ready_ninsts);
    else if (version == 22u)
        /* v22 ended in bool fields and sizeof(struct) included tail padding
         * to the structure's 8-byte alignment. */
        state_size = (offsetof(Ia64I2000State, pic_master_read_isr) + 7u) & ~7u;
    else
        state_size = sizeof(*loaded);
    ok &= fread(loaded, state_size, 1, f) == 1;
    if (ok) {
        char nvram_path[sizeof(s->nvram_path)];
        snprintf(nvram_path, sizeof(nvram_path), "%s", s->nvram_path);
        *s = *loaded;
        s->monitor = monitor;
        s->display = display;
        s->vnc = vnc;
        s->cpu = cpu;
        s->ram = ram;
        s->flash = flash;
        s->chipset_scratch = chipset_scratch;
        s->high_dram = high_dram;
        snprintf(s->nvram_path, sizeof(s->nvram_path), "%s", nvram_path);
        /* Loading a VM state must not implicitly overwrite persistent
         * machine NVRAM. A subsequent guest flash program/erase will set
         * this again and be persisted normally. */
        s->nvram_dirty = false;
        s->atapi_data = atapi_data;
        s->ata_data = ata_data;
        /* Explicit -cdrom media belongs to this run, not to the historical
         * snapshot.  This makes a firmware checkpoint reusable with any ISO. */
        if (configured_cdrom_file[0]) {
            snprintf(s->cdrom_file, sizeof(s->cdrom_file), "%s",
                     configured_cdrom_file);
            s->cdrom_size = configured_cdrom_size;
        }
        s->cdrom = s->cdrom_file[0] ? fopen(s->cdrom_file, "rb") : NULL;
        s->hda = s->hda_file[0] ? fopen(s->hda_file, "r+b") : NULL;
        /* Restart the autosave clock from now, not from whatever wall time
         * the snapshot itself was taken at. */
        s->last_autosave = time(NULL);
        /* A snapshot taken right at (or after) a halt is exactly the point
         * of loading it back in - to keep going, typically to retest a
         * fix for whatever caused that halt. Don't leave it stuck. */
        s->halted = false;
        if (version < 19u) {
            for (unsigned i = 0; i < I2000_FLASH_CHIPS; i++) {
                s->flash_chip[i].read_status = s->flash_read_status;
                s->flash_chip[i].read_id = s->flash_read_id;
                s->flash_chip[i].status = s->flash_status;
                s->flash_chip[i].cmd = s->flash_cmd;
                s->flash_chip[i].cmd_addr =
                    (uint32_t)(s->flash_cmd_addr % I2000_FLASH_CHIP_SIZE);
            }
        }
        if (version < 21u) {
            s->atapi_byte_limit = 65536u;
            s->atapi_phase_end = (s->atapi_status & 0x08)
                               ? s->atapi_data_len : s->atapi_data_pos;
        }
        if (version < 22u) {
            s->atapi_status_pending = false;
            s->atapi_ready_irq = false;
        }
        if (version < 5u) {
            rage128_init(&s->rage128, configured_rage128,
                         RAGE128_FB_BASE, RAGE128_IO_BASE,
                         RAGE128_MMIO_BASE, 17);
        }
        if (version < 6u) {
            memset(&s->int10_req, 0,
                   sizeof(*s) - offsetof(Ia64I2000State, int10_req));
            i2000_load_vga_option_rom(s);
        }
        if (version < 7u) {
            s->mouse_enabled = configured_mouse_enabled;
        }
        if (version < 9u) {
            s->mouse_accum_dx = s->mouse_accum_dy = 0;
            s->mouse_host_left = s->mouse_prev_left;
            s->mouse_host_right = s->mouse_prev_right;
            /* Old snapshots may contain the former 3M-instruction
             * cooldown. It is no longer meaningful under EOI flow control. */
            s->mouse_retry_ninsts = s->cpu->ninsts;
        }
        if (version < 14u) {
            i82559_init(&s->i82559, configured_i82559, I82559_MMIO_BASE,
                        I82559_IO_BASE, 18, bus_read, bus_write, s);
        } else {
            /* Snapshot callback pointers refer to the saving process. */
            s->i82559.guest_read = bus_read;
            s->i82559.guest_write = bus_write;
            s->i82559.guest_opaque = s;
        }
        s->i82559.option_rom = i82559_option_rom;
        s->i82559.option_rom_size = i82559_option_rom_size;
        if (version < 15u)
            ps2_keyboard_init(&s->ps2_keyboard);
        if (version < 16u)
            s->cmd649_cfg[0x3C] = 15; /* PCI Interrupt Line: legacy IRQ15 */
        /* Snapshots made by the short-lived DSC experiment can contain
         * 0x58/0x50. This SDV's AtaPim waits for the ATAPI form without
         * DSC (0x48/0x40), so normalize those states when replaying them. */
        s->atapi_status &= ~0x10u;
        if (s->atapi_data_len == 512 && s->atapi_data_pos == 0 &&
            (s->atapi_status & 0x08)) {
            s->atapi_count = 1;
            s->atapi_lba_low = 1;
            s->atapi_lba_mid = 0x14;
            s->atapi_lba_high = 0xEB;
        }
    }
    free(loaded);

    MercedBus bus = s->cpu->bus;
    void *translation_cache = s->cpu->translation_cache;
    Merced loaded_cpu;
    memset(&loaded_cpu, 0, sizeof(loaded_cpu));
    size_t cpu_state_size = version < 20u
                          ? offsetof(Merced, translation_cache)
                          : sizeof(loaded_cpu);
    ok &= fread(&loaded_cpu, cpu_state_size, 1, f) == 1;
    if (ok) {
        *s->cpu = loaded_cpu;
        s->cpu->bus = bus;
        s->cpu->translation_cache = translation_cache;
        merced_flush_translation_cache(s->cpu);
        if (version < 18u && s->atapi_irq_pending)
            s->atapi_irq_ready_ninsts = s->cpu->ninsts + ATAPI_IRQ_LATENCY_NINSTS;
        /* rfi_generation is process-local rather than snapshot state, so an
         * absolute saved target cannot be meaningful after load. */
        s->mouse_wait_rfi_generation = 0;
        s->keyboard_retry_ninsts = s->cpu->ninsts;
        s->mouse_next_report_itc = merced_get_itc(s->cpu);
        /* A snapshot can land between making an ATAPI PIO phase visible
         * (DRQ plus unread bytes) and delivering/acknowledging its legacy
         * IRQ15.  Older runs could consequently save neither ownership
         * latch: irq_pending was clear and the slave PIC ISR did not yet
         * contain IRQ15.  AtaPim then waits in its 8254-backed IRQ timeout
         * loop forever instead of consuming the already-ready phase.
         * Re-latch only that otherwise ownerless DRQ state; an acknowledged
         * interrupt remains represented by pic_slave_isr bit 7 and is not
         * duplicated. */
        if ((s->atapi_status & 0x08) &&
            s->atapi_data_pos < s->atapi_phase_end &&
            !s->atapi_irq_pending && !(s->pic_slave_isr & 0x80)) {
            s->atapi_irq_pending = true;
            s->atapi_irq_ready_ninsts = s->cpu->ninsts;
        }
        /* PIT deadlines describe host-time progression and must never be
         * resumed as absolute snapshot values.  This also upgrades older
         * snapshots whose fields used the former instruction-count clock. */
        bool pit0_armed = s->pit0_next_irq != 0;
        bool pit1_armed = s->pit1_next_irq != 0;
        s->pit0_epoch_ninsts = merced_get_itc(s->cpu);
        s->pit0_next_irq = pit0_armed
                         ? s->pit0_epoch_ninsts +
                           pit_interval_itc(s->pit0_reload) : 0;
        s->pit1_next_irq = pit1_armed
                         ? s->pit0_epoch_ninsts +
                           pit_interval_itc(s->pit1_reload) : 0;
        i2000_repair_saved_mode12(s);
    }

    fclose(f);
    return ok;
}

#define I2000_AUTOSAVE_DIR "snapshots"
#define I2000_AUTOSAVE_PERIOD_SEC 300

/* Called once per run_slice from the main loop. Wall-clock gated (not
 * instruction-count gated) since the whole point is a rollback point every
 * few minutes of real debugging time, regardless of how fast or slow any
 * particular stretch of firmware executes. Silent no-op until the first
 * period has elapsed, so a quick one-off run doesn't pay for a snapshot it
 * will never use. */
static bool i2000_autosave_tick(Ia64I2000State *s) {
    time_t now = time(NULL);
    if (s->last_autosave == 0) {
        s->last_autosave = now;
        return false;
    }
    if (now - s->last_autosave < I2000_AUTOSAVE_PERIOD_SEC)
        return false;
    s->last_autosave = now;
    char path[256];
    snprintf(path, sizeof(path), "%s/autosave_%u.vmstate",
             I2000_AUTOSAVE_DIR, s->autosave_slot);
    s->autosave_slot ^= 1u;
    if (i2000_save_snapshot(s, path))
        printf("i2000: autosaved to %s (ninsts=%" PRIu64 ")\n",
               path, s->cpu->ninsts);
    else
        fprintf(stderr, "i2000: autosave to %s FAILED\n", path);
    return true;
}

/* monitor "x 0xADDR [count]": physical memory hexdump
 * monitor "trace N": stderr-trace the next N executed slots
 * monitor "history [N]": dump the most recently executed slots
 * monitor "panel FILE": dump the CPU/MMIO debug front panel (the live
 * display and default screendump now show real VGA output instead)
 * monitor "savevm FILE": snapshot the full machine state to FILE
 * monitor "loadvm FILE": restore full machine state from FILE (run this
 * right after startup to skip re-running a slow, already-verified boot
 * prefix) */
static void i2000_custom_cmd(Ia64I2000State *s) {
    const char *txt = gemu_monitor_command_text(s->monitor);
    uint64_t addr;
    int count = 64;
    uint64_t n;
    char panel_path[256];
    char vm_path[256];
    if (txt && (strcmp(txt, "input") == 0 || strcmp(txt, "input-state") == 0)) {
        printf("8042: command=%02X out=%u/%u aux=%u/%u "
               "kbd=%u/%u irq1=%u retry=%" PRIu64 " ninsts=%" PRIu64 "\n",
               s->kbc_command_byte,
               s->kbc_out_pos, s->kbc_out_len,
               s->aux_out_pos, s->aux_out_len,
               s->ps2_keyboard.queue_pos, s->ps2_keyboard.queue_len,
               s->keyboard_irq_pending, s->keyboard_retry_ninsts,
               s->cpu->ninsts);
        printf("PIT0: reload=%u next=%" PRIu64 " now=%" PRIu64
               " pending=%u mask=%02X\n",
               s->pit0_reload ? (unsigned)s->pit0_reload : 65536u,
               s->pit0_next_irq, merced_get_itc(s->cpu),
               s->pit_irq_pending, s->pic_master_mask);
        if (ps2_keyboard_has_data(&s->ps2_keyboard)) {
            printf("keyboard bytes:");
            for (unsigned i = s->ps2_keyboard.queue_pos;
                 i < s->ps2_keyboard.queue_len; i++)
                printf(" %02X", s->ps2_keyboard.queue[i]);
            putchar('\n');
        }
        return;
    }
    if (txt && sscanf(txt, "savevm %255s", vm_path) == 1) {
        if (i2000_save_snapshot(s, vm_path))
            printf("saved snapshot to %s (ninsts=%" PRIu64 ")\n",
                   vm_path, s->cpu->ninsts);
        else
            printf("failed to save snapshot to %s\n", vm_path);
        return;
    }
    if (txt && sscanf(txt, "loadvm %255s", vm_path) == 1) {
        if (i2000_load_snapshot(s, vm_path))
            printf("loaded snapshot from %s (ninsts=%" PRIu64 ")\n",
                   vm_path, s->cpu->ninsts);
        else
            printf("failed to load snapshot from %s\n", vm_path);
        return;
    }
    if (txt && sscanf(txt, "panel %255s", panel_path) == 1) {
        panel_render(s);
        if (gemu_screendump_argb(panel_path, s->fb, FB_W, FB_H))
            printf("wrote debug panel to %s\n", panel_path);
        else
            printf("cannot write %s\n", panel_path);
        return;
    }
    if (txt && sscanf(txt, "trace %" SCNu64, &n) == 1) {
        s->cpu->trace_n = n;
        printf("tracing next %" PRIu64 " slots to stderr\n", n);
        return;
    }
    if (txt && strncmp(txt, "history", 7) == 0) {
        unsigned count = 128;
        (void)sscanf(txt + 7, "%u", &count);
        if (count > MERCED_TRACE_HISTORY) count = MERCED_TRACE_HISTORY;
        merced_dump_trace(s->cpu, count, stderr);
        return;
    }
    if (txt && strncmp(txt, "calls", 5) == 0) {
        unsigned count = MERCED_CALL_HISTORY;
        if (sscanf(txt + 5, "%u", &count) != 1)
            count = MERCED_CALL_HISTORY;
        if (count > MERCED_CALL_HISTORY) count = MERCED_CALL_HISTORY;
        merced_dump_calls(s->cpu, count, stderr);
        return;
    }
    if (txt && strncmp(txt, "mousestate", 10) == 0) {
        fprintf(stderr, "i2000: ninsts=%" PRIu64 " ip=%016" PRIx64
                " psr=%016" PRIx64
                " psr.i=%d tpr=%016" PRIx64 " tpr.mmi=%d\n",
                s->cpu->ninsts, s->cpu->ip, s->cpu->psr,
                !!(s->cpu->psr & (UINT64_C(1) << 14)),
                s->cpu->cr[66] /* CR_TPR */,
                !!(s->cpu->cr[66] & (UINT64_C(1) << 16)));
        merced_dump_trace(s->cpu, 12, stderr);
        fprintf(stderr, "i2000: mouse_enabled=%d mouse_streaming=%d "
                "kbc_command_byte=%02x aux_out_len=%u aux_out_pos=%u "
                "mouse_param_cmd=%02x mouse_prev_left=%d mouse_prev_right=%d "
                "mouse_accum=(%d,%d) mouse_resolution=%u "
                "mouse_sample_rate=%u kbc_out_len=%u "
                "kbc_pending_write=%u\n",
                s->mouse_enabled, s->mouse_streaming, s->kbc_command_byte,
                s->aux_out_len, s->aux_out_pos, s->mouse_param_cmd,
                s->mouse_prev_left, s->mouse_prev_right,
                s->mouse_accum_dx, s->mouse_accum_dy, s->mouse_resolution,
                s->mouse_sample_rate, s->kbc_out_len, s->kbc_pending_write);
        fprintf(stderr, "i2000: iosapic_rte[12]=%016" PRIX64 " masked=%d "
                "pic_master_mask=%02x pic_slave_mask=%02x\n",
                s->iosapic_rte[12], !!(s->iosapic_rte[12] & IOSAPIC_RTE_MASK),
                s->pic_master_mask, s->pic_slave_mask);
        fflush(stderr);
        return;
    }
    {
        int mdx = 0, mdy = 0;
        char mbtn[8] = "";
        int nmatch = txt ? sscanf(txt, "mousemove %d %d %7s", &mdx, &mdy, mbtn) : 0;
        if (nmatch >= 2) {
            /* Synthetic PS/2 packet injection for headless testing - see
             * um6578's own "mousemove"/"mouse" monitor commands for the
             * sibling feature. Goes through the exact same aux_queue_byte()/
             * ExtINT path a real host mouse event would, so this exercises
             * the full pipeline (packet build, IRQ delivery, ack) without
             * needing a live display. */
            if (!s->mouse_enabled) {
                printf("mousemove: no mouse attached (-device mouse)\n");
            } else if (!s->mouse_streaming) {
                printf("mousemove: mouse attached but guest hasn't enabled "
                       "streaming yet (mouse_streaming=0)\n");
            } else if (s->aux_out_len != 0 ||
                       s->cpu->ninsts < s->mouse_retry_ninsts) {
                /* Matches the real per-frame polling path's backpressure
                 * guard (ia64_i2000_run()) - real host mouse motion never
                 * queues a new packet while the previous one is still even
                 * partially undrained or its cooldown hasn't elapsed,
                 * letting the guest fully finish with one report before
                 * being offered the next. */
                printf("mousemove: previous packet not yet drained/cooled "
                       "down (aux_out_len=%u), dropped\n", s->aux_out_len);
            } else {
                bool left = strcmp(mbtn, "left") == 0;
                bool right = strcmp(mbtn, "right") == 0;
                int dy = -mdy;
                bool x_overflow = mdx < -127 || mdx > 127;
                bool y_overflow = dy < -127 || dy > 127;
                int8_t cdx = (int8_t)(mdx < -127 ? -127 : mdx > 127 ? 127 : mdx);
                int8_t cdy = (int8_t)(dy < -127 ? -127 : dy > 127 ? 127 : dy);
                uint8_t flags = (uint8_t)((left ? 0x01 : 0) | (right ? 0x02 : 0) |
                                          0x08 |
                                          (cdx < 0 ? 0x10 : 0) | (cdy < 0 ? 0x20 : 0) |
                                          (x_overflow ? 0x40 : 0) | (y_overflow ? 0x80 : 0));
                aux_queue_byte(s, flags);
                aux_queue_byte(s, (uint8_t)cdx);
                aux_queue_byte(s, (uint8_t)cdy);
                s->mouse_prev_left = left;
                s->mouse_prev_right = right;
                printf("mousemove: dx=%d dy=%d left=%d right=%d queued\n",
                       mdx, mdy, left, right);
            }
            return;
        }
    }
    if (txt && strncmp(txt, "mousekick", 9) == 0) {
        /* Debug-only: unconditionally re-raise IRQ12 if aux_out has any
         * unread bytes, regardless of the empty->nonempty transition
         * aux_queue_byte()/the 0x60 read handler normally require. For
         * recovering a stuck backlog from an old snapshot saved before
         * mouse_irq_pending existed, not part of the real device model. */
        if (s->aux_out_pos < s->aux_out_len &&
            !(s->pic_slave_mask & 0x10) && !(s->pic_master_mask & 0x04)) {
            s->mouse_irq_pending = true;
            merced_raise_external(s->cpu, 0);
            printf("mousekick: re-raised (aux_out_pos=%u aux_out_len=%u)\n",
                   s->aux_out_pos, s->aux_out_len);
        } else {
            printf("mousekick: nothing to do (aux_out_pos=%u aux_out_len=%u "
                   "pic_slave_mask=%02x pic_master_mask=%02x)\n",
                   s->aux_out_pos, s->aux_out_len, s->pic_slave_mask,
                   s->pic_master_mask);
        }
        return;
    }
    {
        if (txt && strncmp(txt, "key ", 4) == 0 && txt[4]) {
            const char *keyarg = txt + 4;
            /* Monitor-scripted key injection for headless UI testing - the
             * -display/-vnc input paths only ever feed kbc_queue_ascii()
             * printable/control ASCII, so arrows (needed for the graphical
             * setup's tab bar per its own on-screen instructions) have no
             * other way in. A non-special argument is typed verbatim, which
             * permits commands such as "key map -r" at the EFI shell. */
            uint32_t special = 0;
            if (!strcmp(keyarg, "right"))     special = 0xff53;
            else if (!strcmp(keyarg, "left")) special = 0xff51;
            else if (!strcmp(keyarg, "up"))   special = 0xff52;
            else if (!strcmp(keyarg, "down")) special = 0xff54;
            else if (!strcmp(keyarg, "enter")) special = '\r';
            else if (!strcmp(keyarg, "tab"))   special = '\t';
            else if (!strcmp(keyarg, "esc"))   special = 0x1b;
            if (special) {
                ps2_keyboard_tap(&s->ps2_keyboard, special);
                /* EFI's built-in shell uses the polled serial console in
                 * this firmware revision (cr.iva remains zero, so it does
                 * not consume PS/2 IRQ1 scan codes).  Real SDL/VNC ASCII
                 * input is mirrored into UART RX; monitor injection must do
                 * the same or `key` appears to succeed while the shell sees
                 * nothing. */
                if (special <= 0x7f) {
                    uint8_t next = (uint8_t)(s->uart_rx_tail + 1);
                    if (next != s->uart_rx_head) {
                        s->uart_rx[s->uart_rx_tail] = (uint8_t)special;
                        s->uart_rx_tail = next;
                    }
                    i2000_efi_bios_key(s, (uint8_t)special);
                }
                printf("key %s queued\n", keyarg);
            } else {
                size_t queued = 0;
                for (const unsigned char *p = (const unsigned char *)keyarg;
                     *p; p++) {
                    if (*p < 0x20 || *p > 0x7e)
                        continue;
                    ps2_keyboard_tap(&s->ps2_keyboard, *p);
                    uint8_t next = (uint8_t)(s->uart_rx_tail + 1);
                    if (next != s->uart_rx_head) {
                        s->uart_rx[s->uart_rx_tail] = *p;
                        s->uart_rx_tail = next;
                    }
                    i2000_efi_bios_key(s, *p);
                    queued++;
                }
                printf("key: queued %zu character%s; use 'key enter' to "
                       "submit\n", queued, queued == 1 ? "" : "s");
            }
            return;
        }
    }
    {
        uint64_t daddr, dlen;
        char path[256];
        if (txt && sscanf(txt, "dump %" SCNx64 " %" SCNx64 " %255s",
                          &daddr, &dlen, path) == 3) {
            FILE *f = fopen(path, "wb");
            if (!f) { printf("cannot open %s\n", path); return; }
            for (uint64_t i = 0; i < dlen; i++) {
                uint8_t b = (uint8_t)bus_read(s, daddr + i, 1);
                fwrite(&b, 1, 1, f);
            }
            fclose(f);
            printf("dumped 0x%" PRIX64 " bytes from 0x%" PRIX64 " to %s\n",
                   dlen, daddr, path);
            return;
        }
    }
    if (txt && strncmp(txt, "vhptstats", 9) == 0) {
        uint64_t calls, disabled, hit, unmapped, tagfail, np;
        merced_vhpt_stats(&calls, &disabled, &hit, &unmapped, &tagfail, &np);
        printf("vhpt: calls=%" PRIu64 " disabled=%" PRIu64 " hit=%" PRIu64
               " unmapped=%" PRIu64 " tagfail=%" PRIu64 " np=%" PRIu64 "\n",
               calls, disabled, hit, unmapped, tagfail, np);
        return;
    }
    if (txt && strncmp(txt, "faultstats", 10) == 0) {
        uint64_t counts[0x5B] = {0};
        merced_fault_stats(counts, sizeof(counts) / sizeof(counts[0]));
        static const struct { unsigned slot; const char *name; } vectors[] = {
            { 0x00, "vhpt" }, { 0x04, "itlb" }, { 0x08, "dtlb" },
            { 0x0c, "alt-itlb" }, { 0x10, "alt-dtlb" },
            { 0x14, "nested-dtlb" }, { 0x20, "dirty" },
            { 0x24, "iaccess" }, { 0x28, "daccess" },
            { 0x2c, "break" }, { 0x30, "extint" },
            { 0x50, "page-not-present" }, { 0x54, "general" },
            { 0x56, "nat" }, { 0x57, "spec" }, { 0x5a, "unaligned" },
        };
        printf("faults:");
        for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
            if (counts[vectors[i].slot])
                printf(" %s=%" PRIu64, vectors[i].name,
                       counts[vectors[i].slot]);
        printf("\n");
        return;
    }
    if (txt && strncmp(txt, "atapistate", 10) == 0) {
        printf("atapi: status=%#04x error=%#04x count=%#04x device=%#04x "
               "irq_pending=%u irq_ready=%" PRIu64 " status_pending=%u "
               "status_ready=%" PRIu64 " ready_status=%#04x ready_irq=%u "
               "data=%zu/%zu phase_end=%zu ninsts=%" PRIu64 "\n",
               s->atapi_status, s->atapi_error, s->atapi_count,
               s->atapi_device, s->atapi_irq_pending,
               s->atapi_irq_ready_ninsts, s->atapi_status_pending,
               s->atapi_status_ready_ninsts, s->atapi_ready_status,
               s->atapi_ready_irq, s->atapi_data_pos, s->atapi_data_len,
               s->atapi_phase_end, s->cpu->ninsts);
        return;
    }
    if (txt && sscanf(txt, "x %" SCNx64 " %d", &addr, &count) >= 1) {
        if (count > 1024) count = 1024;
        for (int i = 0; i < count; i += 16) {
            printf("%012" PRIX64 ": ", addr + (uint64_t)i);
            for (int j = 0; j < 16 && i + j < count; j++)
                printf("%02X ", (unsigned)bus_read(s, addr + (uint64_t)(i + j), 1));
            printf("\n");
        }
        return;
    }
    gemu_monitor_unknown_command(s->monitor);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/* Encode one DIMM row's Serial Presence Detect content for `bytes` of
 * capacity (must be a power of two) per the Intel/JEDEC "PC SDRAM Serial
 * Presence Detect" spec (JESD21-C sec. 4.1.2.5) that SSDM 248704-001
 * sec. 5.5.1 cites. Only the fields firmware's memory-sizing path actually
 * needs are filled in with confidence (type, row/column address counts,
 * physical banks, data width, banks-per-device); everything else is left
 * at conservative, spec-legal defaults since nothing here validates DIMM
 * timing against real electrical constraints anyway. Size formula used:
 * bytes = 2^(rows+cols) * (width_bits/8) * banks_per_device * phys_banks.
 * PC100/PC133 SDRAM devices have four internal banks; advertising one made
 * the SDV firmware derive a nonsensical top-of-memory value whose upper
 * half contained SPD byte 16 (0x0e), eventually producing addresses such as
 * 0x00000e007ffff000.  Keep one physical rank and a 64-bit module width, so
 * rows+cols = log2(bytes) - 5. */
static void spd_encode(uint8_t out[256], uint64_t bytes) {
    memset(out, 0, 256);
    unsigned total_bits = 0;
    for (uint64_t b = bytes; b > 1; b >>= 1)
        total_bits++;
    unsigned addr_bits = total_bits - 5;
    /* Period SDRAM DIMMs normally use 12-13 row bits and 9-11 column bits.
     * Bias the split toward rows instead of generating impossible 14-column
     * devices for large modules. */
    unsigned rows = addr_bits > 10 ? 13 : (addr_bits + 1) / 2;
    unsigned cols = addr_bits - rows;
    out[0]  = 128;   /* bytes used by module manufacturer */
    out[1]  = 8;     /* total SPD EEPROM size = 2^8 = 256 bytes */
    out[2]  = 4;     /* fundamental memory type: SDRAM */
    out[3]  = (uint8_t)rows;
    out[4]  = (uint8_t)cols;
    out[5]  = 1;     /* physical banks (ranks) on this DIMM */
    out[6]  = 64;    /* module data width, LSB (bits) */
    out[7]  = 0;     /* module data width, MSB */
    out[8]  = 4;     /* voltage interface: LVTTL */
    out[9]  = 0x75;  /* SDRAM cycle time @ max CAS latency: 7.5 ns */
    out[10] = 0x60;  /* access time from clock: 6.0 ns */
    out[11] = 0;     /* config type: non-parity, non-ECC */
    out[12] = 0x82;  /* refresh: normal rate, self-refresh supported */
    out[13] = 8;     /* primary SDRAM width (bits per chip) */
    out[16] = 0x0E;  /* burst lengths supported: 2, 4, 8 */
    out[17] = 4;     /* internal banks per SDRAM device */
    out[18] = 0x06;  /* CAS latencies supported: CL2, CL3 */
    out[62] = 0x11;  /* SPD revision 1.1 */
    unsigned sum = 0;
    for (unsigned i = 0; i < 63; i++)
        sum += out[i];
    out[63] = (uint8_t)sum;  /* checksum over bytes 0-62 */
}

/* Populate simulated DIMM SPD content matching the actually-configured
 * ram_size, so firmware's real memory-detection path (SSDM sec. 5.5.1)
 * reports the true size instead of assuming a fixed maximum.  The i2000's
 * eight slots predate multi-gigabyte SDRAM DIMMs, so populate them with at
 * most 256 MiB each.  Representing 2 GiB as one fictional 2 GiB PC100 DIMM
 * overflowed fields in the firmware's SPD geometry calculation. */
static void spd_init(Ia64I2000State *s) {
    memset(s->spd_present, 0, sizeof(s->spd_present));
    memset(s->spd, 0, sizeof(s->spd));
    unsigned slot = 0;
    uint64_t remaining = s->ram_size;
    while (remaining && slot < I2000_SPD_SLOTS) {
        uint64_t chunk = remaining > UINT64_C(256) * 1024 * 1024
                       ? UINT64_C(256) * 1024 * 1024 : remaining;
        /* SPD geometry describes powers of two.  Use the largest module
         * that fits; ordinary supported -m sizes divide exactly. */
        while (chunk & (chunk - 1))
            chunk &= chunk - 1;
        spd_encode(s->spd[slot], chunk);
        s->spd_present[slot] = true;
        remaining -= chunk;
        slot++;
    }
}

static void chipset_cfg_reset(Ia64I2000State *s) {
    s->flash_read_status = false;
    s->flash_read_id = false;
    s->flash_status = 0x80;
    s->flash_cmd = 0;
    s->flash_cmd_addr = 0;
    memset(s->flash_chip, 0, sizeof(s->flash_chip));
    for (unsigned i = 0; i < I2000_FLASH_CHIPS; i++)
        s->flash_chip[i].status = 0x80;
    s->pci_cfg_addr = 0;
    s->ifb_smbus_cmd_read_once = false;
    s->chipset_bus = 0xFF;   /* 460GX power-on CBN default: top bus number */
    memset(s->chipset_cfg, 0, sizeof(s->chipset_cfg));
    memset(s->memcard_cfg, 0, sizeof(s->memcard_cfg));
    memset(s->ifb_cfg, 0, sizeof(s->ifb_cfg));
    memset(s->ifb_usb_cfg, 0, sizeof(s->ifb_usb_cfg));
    memset(s->ifb_smbus_cfg, 0, sizeof(s->ifb_smbus_cfg));
    memset(s->cmd649_cfg, 0, sizeof(s->cmd649_cfg));
    memset(s->acpi_io, 0, sizeof(s->acpi_io));
    s->uart_rx_head = s->uart_rx_tail = 0;
    memset(s->cmos, 0, sizeof(s->cmos));
    s->cmos_index = 0;
    s->cmos[0x0A] = 0x26;                         /* divider, 32.768 kHz */
    s->cmos[0x0B] = 0x02;                         /* 24-hour BCD mode */
    s->cmos[0x0D] = 0x80;                         /* CMOS power valid */
    /* Extended byte 3 bit 3: "previous boot completed" flag. The bootstrap
     * takes the PspRecover path (and wants wpgbios.bin from recovery
     * media) whenever it is clear, i.e. on CMOS loss. */
    s->cmos[0x03] = 0x08;
    s->atapi_error = s->atapi_features = 0;
    s->atapi_count = 1;
    s->atapi_lba_low = 1;
    s->atapi_lba_mid = 0x14;
    s->atapi_lba_high = 0xEB;
    s->atapi_device = 0xA0;
    /* The secondary-master ATAPI drive exists even with its tray empty.
     * Media presence affects SCSI packet results, not ATA device presence. */
    s->atapi_status = 0x40;
    s->atapi_packet_pos = 0;
    s->atapi_byte_limit = 65536u;
    s->atapi_phase_end = 0;
    s->atapi_irq_pending = false;
    s->atapi_status_pending = false;
    s->atapi_ready_irq = false;
    free(s->atapi_data);
    s->atapi_data = NULL;
    s->atapi_data_len = s->atapi_data_pos = 0;
    s->ata_error = s->ata_features = 0;
    s->ata_count = 1;
    s->ata_lba_low = 1;
    s->ata_lba_mid = s->ata_lba_high = 0;
    s->ata_device = 0;
    s->ata_status = s->hda ? 0x40 : 0;
    s->ata_data_is_write = false;
    free(s->ata_data);
    s->ata_data = NULL;
    s->ata_data_len = s->ata_data_pos = 0;

    /* Intel 460GX IFB PCI-to-LPC/FWH bridge at 00:03.0.  BIOS 1.30 writes
     * its device-specific configuration registers very early; treating
     * the function as absent made every read return all ones.  Reset values
     * are from SSDM 248704-001, chapter 11. */
    s->ifb_cfg[0x00] = 0x86; s->ifb_cfg[0x01] = 0x80; /* Intel */
    s->ifb_cfg[0x02] = 0x00; s->ifb_cfg[0x03] = 0x76; /* IFB 7600 */
    s->ifb_cfg[0x04] = 0x07; s->ifb_cfg[0x05] = 0x00;
    s->ifb_cfg[0x06] = 0x80; s->ifb_cfg[0x07] = 0x02;
    s->ifb_cfg[0x09] = 0x00; s->ifb_cfg[0x0A] = 0x01;
    s->ifb_cfg[0x0B] = 0x06; s->ifb_cfg[0x0E] = 0x80;
    s->ifb_cfg[0x4E] = 0xC1; s->ifb_cfg[0x4F] = 0x07;
    s->ifb_cfg[0x60] = s->ifb_cfg[0x61] = 0x80;
    s->ifb_cfg[0x62] = s->ifb_cfg[0x63] = 0x80;
    s->ifb_cfg[0x64] = 0x10;
    s->ifb_cfg[0x69] = 0x02;
    s->ifb_cfg[0x84] = 0x00; s->ifb_cfg[0x85] = 0x05;

    /* The remaining documented IFB functions are present even though the
     * UHCI and SMBus engines themselves are currently empty.  Advertising
     * their real identities lets EFI enumerate and then cleanly disable
     * them instead of treating configuration reads as master aborts. */
    s->ifb_usb_cfg[0x00] = 0x86; s->ifb_usb_cfg[0x01] = 0x80;
    s->ifb_usb_cfg[0x02] = 0x02; s->ifb_usb_cfg[0x03] = 0x76;
    s->ifb_usb_cfg[0x06] = 0x80; s->ifb_usb_cfg[0x07] = 0x02;
    s->ifb_usb_cfg[0x09] = 0x00; s->ifb_usb_cfg[0x0A] = 0x03;
    s->ifb_usb_cfg[0x0B] = 0x0C; s->ifb_usb_cfg[0x0E] = 0x00;
    s->ifb_usb_cfg[0x3D] = 0x04; s->ifb_usb_cfg[0x60] = 0x10;

    s->ifb_smbus_cfg[0x00] = 0x86; s->ifb_smbus_cfg[0x01] = 0x80;
    s->ifb_smbus_cfg[0x02] = 0x03; s->ifb_smbus_cfg[0x03] = 0x76;
    s->ifb_smbus_cfg[0x06] = 0x80; s->ifb_smbus_cfg[0x07] = 0x02;
    s->ifb_smbus_cfg[0x09] = 0x00; s->ifb_smbus_cfg[0x0A] = 0x05;
    s->ifb_smbus_cfg[0x0B] = 0x0C; s->ifb_smbus_cfg[0x0E] = 0x00;
    s->ifb_smbus_cfg[0x3D] = 0x02;

    /* Integrated CMD Technology PCI-649 Ultra ATA/100 controller at the
     * i2000 IFB's fixed 00:03.1 function. */
    s->cmd649_cfg[0x00] = 0x95; s->cmd649_cfg[0x01] = 0x10; /* 1095 */
    s->cmd649_cfg[0x02] = 0x49; s->cmd649_cfg[0x03] = 0x06; /* 0649 */
    s->cmd649_cfg[0x08] = 0x02;                    /* revision */
    s->cmd649_cfg[0x09] = 0x8A;                    /* native/legacy IDE */
    s->cmd649_cfg[0x0A] = 0x01;                    /* IDE subclass */
    s->cmd649_cfg[0x0B] = 0x01;                    /* mass storage */
    s->cmd649_cfg[0x0E] = 0x00;                    /* normal header */
    s->cmd649_cfg[0x3C] = 15;                      /* secondary IDE IRQ15 */
    s->cmd649_cfg[0x3D] = 0x01;                    /* INTA */
    /* Device numbers and roles per SSDM 248704-001 Table 2-1 (Device
     * Mapping on Bus CBN): 00h/01h = SAC (82461GX), 04h = SDC (82462GX,
     * not GXB as an earlier version of this table assumed - GXB has no
     * fixed device number, it lives on an Expander bus per WXB/PXB/GXB
     * board population), 05h/06h = Memory Card A/B (handled via
     * memcard_cfg). Real vendor/device IDs aren't published in the
     * datasheet or SSDM; these placeholders just need to be internally
     * consistent and non-zero (0xFFFF reads as "no device"). */
    static const struct { uint8_t dev; uint16_t did; } ids[] = {
        { 0, 0x84E0 }, { 1, 0x84E0 },   /* SAC */
        { 4, 0x84E2 },                  /* SDC */
        { 0x10, 0x84E4 }, { 0x11, 0x84E4 }, { 0x12, 0x84E4 }, { 0x13, 0x84E4 },
        { 0x14, 0x84E4 }, { 0x15, 0x84E4 }, { 0x16, 0x84E4 }, { 0x17, 0x84E4 },
                                         /* Expander 0-3 buses a/b */
    };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        uint8_t *c = s->chipset_cfg[ids[i].dev][0];
        c[0] = 0x86; c[1] = 0x80;
        c[2] = (uint8_t)ids[i].did; c[3] = (uint8_t)(ids[i].did >> 8);
        c[0x0B] = 0x06;
    }
    /* SAC dev0/fn0 byte 60h: a capability-bit count that gates a firmware
     * loop walking the 32-bit feature mask at reg 70h and registering a
     * handler for each clear bit. Left at zero (the memset default), that
     * loop never runs even once, so a later, unconditional dispatch call
     * (hardcoded to request feature type 6) finds an empty handler list,
     * falls through to a NULL-derived default pointer, and eventually
     * crashes dereferencing stale low memory. Real hardware's value isn't
     * published anywhere we have; 20h matches the mask's full width so the
     * loop at least examines every bit reg 70h can report. */
    s->chipset_cfg[0][0][0x60] = 0x20;
    /* Memory Card A/B (dev 5/6) live in memcard_cfg, not chipset_cfg - give
     * their function 0 a real PCI identity too, or firmware sees an all-zero
     * vendor ID at CBN:05.0/06.0 and concludes the card is absent before it
     * ever reads the processor descriptor at CBN:05.2 below. */
    static const struct { uint8_t card; uint16_t did; } mcids[] = {
        { 0, 0x84E3 }, { 1, 0x84E3 },   /* Memory Card A/B */
    };
    for (unsigned i = 0; i < sizeof(mcids) / sizeof(mcids[0]); i++) {
        uint8_t *c = s->memcard_cfg[mcids[i].card][0];
        c[0] = 0x86; c[1] = 0x80;
        c[2] = (uint8_t)mcids[i].did; c[3] = (uint8_t)(mcids[i].did >> 8);
        c[0x0B] = 0x06;
    }

    /* The SDV firmware enumerates processors through the 460GX system-bus
     * configuration mechanism.  Its address format is PCI-like: processor
     * zero is CBN:05.2, and byte 02h contains the presence/type code 4.
     * Bytes 03h-05h form the family/model/revision signature used to group
     * compatible processors.  Advertise the one Merced CPU implemented by
     * this machine; leaving this function all zero makes SAL conclude that
     * no processor exists and deliberately enter its timer-calibrated park
     * loop at FFFE2020.
     *
     * This must stay consistent with cpuid[3]'s family/model/revision in
     * merced_reset() (cpu/ia64/merced.c) - firmware cross-checks the two, and a
     * mismatch also lands in the FFFE2020 park loop as if no (recognized)
     * processor were present. */
    s->memcard_cfg[0][2][0x02] = 4;  /* processor present */
    s->memcard_cfg[0][2][0x03] = 7;  /* Itanium family */
    s->memcard_cfg[0][2][0x04] = 0;  /* Merced model */
    s->memcard_cfg[0][2][0x05] = 0;  /* SDV prototype revision; see run loop */

    s->smb_hststs = s->smb_hstcnt = s->smb_hstcmd = s->smb_hstadd = 0;
    s->smb_hstdat0 = s->smb_hstdat1 = 0;
    spd_init(s);

    s->iosapic_regsel = 0;
    for (unsigned i = 0; i < 64; i++)
        s->iosapic_rte[i] = 0x10000;  /* mask bit set, per SSDM Table 2-10 */
}

Ia64I2000State *ia64_i2000_create(const Ia64Config *cfg) {
    Ia64I2000State *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

#ifdef _WIN32
    _mkdir(I2000_AUTOSAVE_DIR); /* ignore EEXIST/already-there */
#else
    mkdir(I2000_AUTOSAVE_DIR, 0755); /* ignore EEXIST/already-there */
#endif

    s->pic_master_mask = 0xFF;
    s->pic_slave_mask = 0xFF;
    s->mouse_enabled = cfg->mouse_enabled;

    s->ram_size = cfg->ram_size;
    s->ram = calloc(1, (size_t)s->ram_size);
    s->flash = malloc(I2000_FLASH_SIZE);
    s->chipset_scratch = calloc(1, I2000_CHIPSET_SCRATCH_SIZE);
    s->high_dram = calloc(1, I2000_HIGH_DRAM_SIZE);
    if (!s->ram || !s->flash || !s->chipset_scratch || !s->high_dram) {
        fprintf(stderr, "gemu: cannot allocate %" PRIu64 " MiB guest RAM\n",
                cfg->ram_size >> 20);
        ia64_i2000_destroy(s);
        return NULL;
    }
    memset(s->flash, 0xFF, I2000_FLASH_SIZE);
    vga_ibm_reset(&s->vga);
    rage128_init(&s->rage128,
                 cfg->vga && strcmp(cfg->vga, "rage128") == 0,
                 RAGE128_FB_BASE, RAGE128_IO_BASE, RAGE128_MMIO_BASE, 17);
    i82559_init(&s->i82559, cfg->i82559_enabled, I82559_MMIO_BASE,
                I82559_IO_BASE, 18, bus_read, bus_write, s);
    if (cfg->i82559_enabled) {
        /* The SDV EFI expects the integrated 82559 to supply an EFI UNDI
         * driver through its PCI expansion ROM.  Merely exposing the PCI
         * function leaves Boot Manager's network-device cleanup on a broken
         * firmware error path ("Could not load UNDI" followed by a pool
         * assertion).  Prefer an installed ROM, with the in-tree QEMU ROM as
         * the development-tree fallback.  i82559_load_option_rom() retags
         * its EEPRO100 PCI ID to the integrated controller's 8086:1229 ID. */
        static const char *const undi_rom_paths[] = {
            "roms/efi-eepro100.rom",
            "reference/qemu-system-ia64/pc-bios/efi-eepro100.rom",
            "reference/qemu-system-ia64-merced/pc-bios/efi-eepro100.rom",
        };
        bool loaded = false;
        for (unsigned i = 0;
             i < sizeof(undi_rom_paths) / sizeof(undi_rom_paths[0]); i++) {
            if (i82559_load_option_rom(&s->i82559, undi_rom_paths[i])) {
                loaded = i82559_keep_efi_option_rom(&s->i82559, 0x0200);
                break;
            }
        }
        if (!loaded)
            fprintf(stderr, "i2000: warning: i82559 EFI UNDI ROM not found\n");
    }
    ps2_keyboard_init(&s->ps2_keyboard);
    i2000_load_vga_option_rom(s);
    if (cfg->cdrom_path) {
        s->cdrom = fopen(cfg->cdrom_path, "rb");
        if (!s->cdrom || fseek(s->cdrom, 0, SEEK_END) != 0) {
            fprintf(stderr, "gemu: cannot open CD-ROM image '%s'\n", cfg->cdrom_path);
            ia64_i2000_destroy(s);
            return NULL;
        }
        long end = ftell(s->cdrom);
        if (end <= 0 || (end % 2048) != 0 || fseek(s->cdrom, 0, SEEK_SET) != 0) {
            fprintf(stderr, "gemu: CD-ROM image '%s' is not a sector-aligned ISO\n",
                    cfg->cdrom_path);
            ia64_i2000_destroy(s);
            return NULL;
        }
        s->cdrom_size = (uint64_t)end;
        snprintf(s->cdrom_file, sizeof(s->cdrom_file), "%s", cfg->cdrom_path);
        if (atapi_debug_enabled())
            fprintf(stderr, "i2000: ATAPI media attached '%s' bytes=%" PRIu64
                    " sectors=%" PRIu64 "\n", s->cdrom_file, s->cdrom_size,
                    s->cdrom_size / 2048);
    }
    if (cfg->hda_path) {
        s->hda = fopen(cfg->hda_path, "r+b");
        if (!s->hda || fseek(s->hda, 0, SEEK_END) != 0) {
            fprintf(stderr, "gemu: cannot open HDD image '%s'\n", cfg->hda_path);
            ia64_i2000_destroy(s);
            return NULL;
        }
        long end = ftell(s->hda);
        if (end <= 0 || (end % 512) != 0 || fseek(s->hda, 0, SEEK_SET) != 0) {
            fprintf(stderr, "gemu: HDD image '%s' is not a sector-aligned "
                    "(512-byte) raw disk image\n", cfg->hda_path);
            ia64_i2000_destroy(s);
            return NULL;
        }
        s->hda_size = (uint64_t)end;
        snprintf(s->hda_file, sizeof(s->hda_file), "%s", cfg->hda_path);
    }
    chipset_cfg_reset(s);

    MercedBus bus = {
        .ud = s, .ram_size = s->ram_size,
        .read = bus_read, .fetch = bus_fetch, .write = bus_write,
        .fill = i2000_bus_fill
    };
    s->cpu = merced_create(&bus);
    if (!s->cpu) {
        ia64_i2000_destroy(s);
        return NULL;
    }
    s->monitor = gemu_monitor_create();
    gemu_monitor_set_cpu_state_cb(s->monitor, i2000_cpu_state, s);
    gemu_monitor_set_screendump_cb(s->monitor, i2000_screendump, s);

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        GemuDisplayConfig dc = {
            .title = "GEMU",
            .fb_width = FB_W,
            .fb_height = FB_H,
            .scale = cfg->display_scale,
            .actions = i2000_keyboard_actions,
            .n_actions = (int)(sizeof(i2000_keyboard_actions) /
                               sizeof(i2000_keyboard_actions[0])),
            .ini_section = "i2000",
            .no_menu = cfg->menu_disabled,
            /* VM-style click-to-capture, same as um6578: the guest firmware
             * draws its own cursor sprite, so the host cursor should be
             * hidden/captured on click rather than tracked in parallel. */
            .capture_pointer = cfg->mouse_enabled,
        };
        s->display = gemu_display_create(cfg->display_type, &dc);
        if (!s->display) {
            ia64_i2000_destroy(s);
            return NULL;
        }
    }
    if (cfg->vnc_addr) {
        /* The i2000 front-panel framebuffer is 32-bit, while the current
         * VNC renderer accepts 8-bit surfaces.  Still expose an RFB server
         * for keyboard input; monitor screendump remains the video path. */
        s->vnc = gemu_vnc_create(cfg->vnc_addr, FB_W, FB_H);
        if (!s->vnc) {
            fprintf(stderr, "gemu: cannot start VNC server on '%s'\n",
                    cfg->vnc_addr);
            ia64_i2000_destroy(s);
            return NULL;
        }
        gemu_monitor_set_vnc(s->monitor, s->vnc);
    }
    return s;
}

void ia64_i2000_destroy(Ia64I2000State *s) {
    if (!s)
        return;
    i2000_nvram_save(s);
    gemu_display_destroy(s->display);
    gemu_vnc_destroy(s->vnc);
    if (s->monitor) gemu_monitor_destroy(s->monitor);
    merced_destroy(s->cpu);
    i82559_destroy(&s->i82559);
    free(s->atapi_data);
    if (s->cdrom) fclose(s->cdrom);
    free(s->ata_data);
    if (s->hda) fclose(s->hda);
    free(s->flash);
    free(s->chipset_scratch);
    free(s->high_dram);
    free(s->ram);
    free(s);
}

/* ── Execution ───────────────────────────────────────────────────────────── */

/* The SDV BIOS's FIT type-0x12 entry is its IA-32 compatibility runtime.
 * SAL shadows that image immediately below 1 MiB; the EFI interposer later
 * says "already Loaded by SAL" and only loads AmiRt13Tbl at 0xE3000.  The
 * shadow behaves like retained chipset/firmware memory, not ordinary DRAM.
 * Without it the IVT's IRQ0 vector, F000:FEA5, enters zero-filled RAM.
 *
 * Derive both source and length from the firmware's FIT instead of baking
 * this ROM's addresses into the machine.  Recheck the tail at PIT delivery
 * because SAL may enable the timer before a subsequent DRAM clear. */
static void i2000_ensure_legacy_runtime(Ia64I2000State *s) {
    static const uint8_t fit_magic[8] =
        {'_', 'F', 'I', 'T', '_', ' ', ' ', ' '};
    static bool checked;
    static uint32_t flash_off;
    static uint32_t runtime_size;

    if (!checked) {
        checked = true;
        for (uint32_t off = 0; off + 16 <= I2000_FLASH_SIZE; off += 16) {
            const uint8_t *hdr = s->flash + off;
            if (memcmp(hdr, fit_magic, sizeof(fit_magic)) != 0)
                continue;
            uint32_t entries = (uint32_t)hdr[8] |
                               ((uint32_t)hdr[9] << 8) |
                               ((uint32_t)hdr[10] << 16);
            if (!entries || entries > (I2000_FLASH_SIZE - off) / 16)
                break;
            for (uint32_t i = 1; i < entries; i++) {
                const uint8_t *e = hdr + i * 16;
                if ((e[14] & 0x7f) != 0x12)
                    continue;
                uint64_t addr;
                memcpy(&addr, e, sizeof(addr));
                addr &= MERCED_PHYS_MASK;
                uint32_t units = (uint32_t)e[8] |
                                 ((uint32_t)e[9] << 8) |
                                 ((uint32_t)e[10] << 16);
                uint64_t bytes = (uint64_t)units * 16;
                if (addr < I2000_FLASH_BASE ||
                    addr + bytes > I2000_FLASH_BASE + I2000_FLASH_SIZE ||
                    !bytes || bytes > 0x100000)
                    break;
                flash_off = (uint32_t)(addr - I2000_FLASH_BASE);
                runtime_size = (uint32_t)bytes;
                break;
            }
            break;
        }
    }

    uint32_t len = runtime_size;
    if (!len || s->ram_size < 0x100000)
        return;
    uint32_t dst = 0x100000 - len;
    const uint8_t *src = s->flash + flash_off;
    if (memcmp(s->ram + dst + len - 8, src + len - 8, 8) != 0) {
        memcpy(s->ram + dst, src, len);
        fprintf(stderr, "i2000: restored FIT IA-32 runtime at %#x-%#x\n",
                dst, dst + len - 1);
    }
}

static void i2000_poll_interrupts(Ia64I2000State *s) {
    if (s->mouse_wait_rfi_generation &&
        merced_rfi_generation() >= s->mouse_wait_rfi_generation) {
        s->mouse_wait_rfi_generation = 0;
        s->mouse_retry_ninsts = s->cpu->ninsts + MOUSE_RETRY_DELAY_INSTS;
        if (s->aux_out_pos >= s->aux_out_len) {
            unsigned rate = s->mouse_sample_rate ? s->mouse_sample_rate : 100;
            uint64_t interval = I2000_ITC_HZ / rate;
            if (interval < 5000) interval = 5000;
            s->mouse_next_report_itc = merced_get_itc(s->cpu) + interval;
        }
    }
    /* legacy_irq_routed is permanently false (nothing ever sets it) - this
     * firmware never configures a single I/O SAPIC RTE (confirmed live,
     * IOSAPIC_DEBUG: every RTE stays at its masked reset value for the
     * entire boot) and instead stays in legacy-8259-compatible mode the
     * whole time, unmasking IRQs through the classic ports (0x20/0x21/
     * 0xA0/0xA1) exactly as real DOS-era firmware does. On real IA-64
     * hardware that compatibility path reaches the CPU through the local
     * SAPIC's "ExtINT" delivery mode - architecturally vector 0, gated
     * only by cr.tpr.mmi and NOT by cr.tpr.mic's priority class (there is
     * no IA-64 IRQL concept for a legacy PIC interrupt) - confirmed against
     * reference/qemu-system-ia64-merced's sapic_vector_unmasked()/
     * IA64_SAPIC_DELIVERY_EXTINT. Delivering IRQ0 as a normal (>=16)
     * vector is exactly why this used to hang forever: firmware raises
     * cr.tpr to 0xC0 around ATA identify and never lowers it again, which
     * permanently masks any normal vector but must NOT mask ExtINT. */
    uint64_t now_itc = merced_get_itc(s->cpu);
    if (s->pit0_next_irq && (int64_t)(now_itc - s->pit0_next_irq) >= 0) {
        i2000_ensure_legacy_runtime(s);
        iosapic_raise_gsi(s, 0);  /* GSI 0 == legacy PIT/IRQ0, forward-compat */
        if (!(s->pic_master_mask & 1)) {
            s->pit_irq_pending = true;
            merced_raise_external(s->cpu, 0);  /* ExtINT: IRQ0 unmasked */
        }
        s->pit0_next_irq = now_itc + pit_interval_itc(s->pit0_reload);
    }
    /* Channel 1 has no standard ISA IRQ wiring on real hardware (classically
     * DRAM refresh, not an interrupt source) and firmware never configures
     * any delivery path for it either - the reload-count register (port
     * 0x41) is still modeled since real chipsets expose it, but nothing
     * here raises an interrupt for it. */
    if (s->pit1_next_irq && (int64_t)(now_itc - s->pit1_next_irq) >= 0) {
        s->pit1_next_irq = now_itc + pit_interval_itc(s->pit1_reload);
    }
    /* Controller replies and host keystrokes assert legacy IRQ1 whenever
     * the keyboard output buffer becomes non-empty. Previously we queued
     * the byte but never signalled the CPU, leaving firmware event waits in
     * CdbPim blocked with an unread 8042 byte. Keep the latch asserted until
     * the firmware performs its ExtINT acknowledge; if more bytes remain,
     * the next poll naturally reasserts it. */
    if ((s->kbc_out_pos < s->kbc_out_len ||
         ps2_keyboard_has_data(&s->ps2_keyboard)) &&
        s->cpu->ninsts >= s->keyboard_retry_ninsts &&
        !(s->pic_master_mask & 0x02)) {
        s->keyboard_irq_pending = true;
        merced_raise_external(s->cpu, 0);
    }
    /* The single place that actually raises vector 0 on mouse's behalf -
     * covers both a brand new packet (aux_queue_byte() only queues bytes,
     * it doesn't raise) and a same-packet retry for the next byte
     * (scheduled by the legacy-ack handler's mouse branch). Runs outside
     * any interrupt-handling context, unlike the ack handler itself, which
     * is exactly why it's safe to raise from here - see the long comments
     * on aux_queue_byte() and the ack handler's mouse branch for what goes
     * wrong when this is done synchronously or without a cooldown.
     * mouse_retry_ninsts defaults to 0 (calloc'd), so a fresh packet with
     * no prior cooldown scheduled raises as soon as it's queued.
     *
     * Deliberately does NOT gate on !mouse_irq_pending: the ack handler's
     * retry case leaves mouse_irq_pending TRUE (on purpose, so ack
     * arbitration can still attribute the eventual ack to mouse) while it
     * still wants this loop to keep re-raising every time the retry
     * deadline is hit. merced_raise_external() is a plain OR of an
     * already-possibly-set bit, so raising here on every eligible
     * instruction between the deadline and the guest actually taking it is
     * harmless, not a flood - mouse_retry_ninsts is the real rate limiter,
     * not this flag. */
    if (s->aux_out_pos < s->aux_out_len &&
        s->cpu->ninsts >= s->mouse_retry_ninsts &&
        s->mouse_wait_rfi_generation == 0 &&
        !(s->pic_slave_mask & 0x10) && !(s->pic_master_mask & 0x04)) {
        s->mouse_irq_pending = true;
        merced_raise_external(s->cpu, 0);
    }
    /* The CMD-649 secondary channel is legacy IRQ15 (slave IRQ7). AtaPim
     * waits for this interrupt after IDENTIFY/PACKET rather than polling
     * DRQ continuously. Keep the device latch asserted until the firmware
     * performs the shared legacy acknowledge read. */
    /* CMD649 is a PCI device.  Its INTA reaches the platform's ExtINT
     * bridge even while the firmware has the legacy slave-PIC mask set;
     * AtaPim installs the IRQ15 handler before it submits the command. */
    if (s->atapi_status_pending &&
        s->cpu->ninsts >= s->atapi_status_ready_ninsts) {
        s->atapi_status_pending = false;
        s->atapi_status = s->atapi_ready_status;
        if (s->atapi_ready_irq) {
            s->atapi_irq_pending = true;
            s->atapi_irq_ready_ninsts = s->cpu->ninsts;
        }
        if (atapi_debug_enabled())
            fprintf(stderr, "i2000: ATAPI BSY cleared status=%#04x "
                    "reason=%#04x ninsts=%" PRIu64 "\n",
                    s->atapi_status, s->atapi_count, s->cpu->ninsts);
    }
    if (s->atapi_irq_pending &&
        s->cpu->ninsts >= s->atapi_irq_ready_ninsts) {
        merced_raise_external(s->cpu, 0);
    }
}

static void i2000_report_halt(Ia64I2000State *s) {
    Merced *m = s->cpu;
    char buf[4096];
    fprintf(stderr, "\ni2000: CPU halted after %" PRIu64 " instructions\n"
                    "i2000: %s\n",
            m->ninsts, m->halt_msg);
    uint64_t bpa = m->halt_ip & ~0xFull & MERCED_PHYS_MASK;
    fprintf(stderr, "i2000: bundle @ 0x%012" PRIX64 ":", bpa);
    for (int i = 0; i < 16; i++)
        fprintf(stderr, " %02X", (unsigned)bus_read(s, bpa + (uint64_t)i, 1));
    fprintf(stderr, "\n");
    fprintf(stderr, "i2000: recent instruction slots:\n");
    merced_dump_trace(m, HALT_TRACE_LINES, stderr);
    fprintf(stderr, "i2000: recent calls/returns:\n");
    merced_dump_calls(m, HALT_CALL_LINES, stderr);
    fprintf(stderr, "i2000: translation registers:\n");
    /* The two TR files are different sizes (8 instruction, 48 data), so walk
     * them separately rather than indexing both with one counter. */
    for (unsigned i = 0; i < MERCED_N_ITR; i++) {
        const MercedTlbEntry *it = &m->itr[i];
        if (it->valid)
            fprintf(stderr, "  itr[%u] rid=%06X va=%016" PRIX64
                            "-%016" PRIX64 " pa=%016" PRIX64 " ps=%u\n",
                    i, it->rid, it->va_start, it->va_end, it->pfn_base, it->ps);
    }
    for (unsigned i = 0; i < MERCED_N_DTR; i++) {
        const MercedTlbEntry *dt = &m->dtr[i];
        if (dt->valid)
            fprintf(stderr, "  dtr[%u] rid=%06X va=%016" PRIX64
                            "-%016" PRIX64 " pa=%016" PRIX64 " ps=%u\n",
                    i, dt->rid, dt->va_start, dt->va_end, dt->pfn_base, dt->ps);
    }
    merced_dump_state(m, buf, sizeof(buf));
    fputs(buf, stderr);
}

static uint64_t i2000_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* GEMU_BENCH_NINSTS=<N> [+ optional GEMU_BENCH_WARMUP=<W>]: measures a
 * windowed slots/sec rate from ninsts==W (default 0) to ninsts==N, then
 * prints and exits. The warmup point matters because a few hand-patched
 * fast paths in merced_step() (e.g. the SAL RAM-clear idiom) advance ninsts
 * by hundreds of millions in a single call without doing per-slot
 * interpretation - a window starting at ninsts==0 would measure that
 * shortcut's speed, not the interpreter's. Set GEMU_BENCH_WARMUP past the
 * point where those one-shot jumps have already happened so the window only
 * covers genuine bundle-by-bundle execution. Resolved once - see the
 * getenv()-per-instruction cost warning near watch_init() in merced.c, same
 * reasoning applies to any check made from the per-slice loop below. */
static void i2000_bench_targets(uint64_t *warmup, uint64_t *target) {
    static uint64_t w, t;
    static bool resolved;
    if (!resolved) {
        resolved = true;
        const char *wspec = getenv("GEMU_BENCH_WARMUP");
        const char *tspec = getenv("GEMU_BENCH_NINSTS");
        if (wspec) w = strtoull(wspec, NULL, 0);
        if (tspec) t = strtoull(tspec, NULL, 0);
    }
    *warmup = w;
    *target = t;
}

static void i2000_bench_check(Ia64I2000State *s) {
    uint64_t warmup, target;
    i2000_bench_targets(&warmup, &target);
    if (!target)
        return;

    static uint64_t window_start_ninsts;
    static uint64_t window_start_ns;
    static bool window_open;
    if (!window_open) {
        if (s->cpu->ninsts < warmup)
            return;
        window_open = true;
        window_start_ninsts = s->cpu->ninsts;
        window_start_ns = i2000_monotonic_ns();
    }

    if (s->cpu->ninsts < target)
        return;
    uint64_t ninsts_delta = s->cpu->ninsts - window_start_ninsts;
    double elapsed_s = (double)(i2000_monotonic_ns() - window_start_ns) / 1e9;
    double slots_per_sec = (double)ninsts_delta / elapsed_s;
    printf("gemu-bench: window=[%" PRIu64 ",%" PRIu64 "] ninsts_delta=%"
           PRIu64 " elapsed=%.3fs slots/sec=%.0f bundle-equiv/sec=%.0f\n",
           window_start_ninsts, s->cpu->ninsts, ninsts_delta, elapsed_s,
           slots_per_sec, slots_per_sec / 3.0);
    fflush(stdout);
    exit(0);
}

static void i2000_run_slice(Ia64I2000State *s) {
    if (s->halted)
        return;
    bool check_exec = gemu_monitor_has_exec_breakpoints(s->monitor);
    bool check_mem = gemu_monitor_has_mem_breakpoints(s->monitor);
    for (int i = 0; i < INSTR_PER_FRAME; i++) {
        if (check_exec &&
            gemu_monitor_check_exec(s->monitor, (uint32_t)s->cpu->ip))
            return;
        /* Device callbacks raise already-ready interrupts directly.  Only
         * deadline-driven sources (PIT and retry timers) need this platform
         * poll, and checking those on every IA-64 slot was a large fraction
         * of execution time.  A 16-slot quantum is far below the latency of
         * the modeled hardware while bounding delivery skew to 15 slots. */
        if ((i & (IRQ_POLL_QUANTUM - 1)) == 0)
            i2000_poll_interrupts(s);
        /* The 0.99 SDV firmware only completes SAL initialization when its
         * prototype processor descriptor and CPUID both report revision 0.
         * Windows/IA-64, however, deliberately rejects pre-B3 silicon.  EFI
         * applications are loaded into the 460GX tagged high-DRAM aperture;
         * once execution enters one, processor enumeration is complete and
         * it is safe to expose the production Merced C0 revision (CPUID
         * 0007000604).  This keeps the firmware's early cross-check coherent
         * without presenting obsolete prototype silicon to the OS loader. */
        if (s->cpu->cpu_revision == 0 &&
            (s->cpu->ip & UINT64_C(0x00000fff00000000)) ==
                I2000_HIGH_DRAM_TAG)
            merced_set_cpu_revision(s->cpu, 6);
        MercedStatus st = merced_step(s->cpu);
        /* A memory watchpoint can be raised from inside the bus callback.
         * Stop this execution slice immediately so the reported IP remains
         * the instruction that performed the access. */
        if (check_mem && gemu_monitor_is_paused(s->monitor))
            return;
        if (s->reset_requested) {
            fprintf(stderr, "i2000: firmware requested a platform reset\n");
            /* CF9 resets the processor while the 460GX retains the sticky
             * configuration state SAL just programmed.  In particular,
             * SAC CBNR bit 0 selects the second bootstrap phase, with the
             * firmware range shadowed in RAM: the flash image is copied to
             * 0x03C00000 and the top-of-4GiB window becomes RAM-backed for
             * reads AND writes (the firmware's data segment lives up there
             * - its physical-mode stores via the 0xFFFDxxxx alias must
             * stick, or the SAL descriptors stay empty). */
            s->fw_shadow_enabled = true;
            if (I2000_FW_SHADOW_BASE + I2000_FLASH_SIZE <= s->ram_size)
                memcpy(s->ram + I2000_FW_SHADOW_BASE, s->flash,
                       I2000_FLASH_SIZE);
            merced_reset(s->cpu);
            s->halted = false;
            s->reset_requested = false;
            s->mmio_log_n = 0;
            memset(s->mmio_log, 0, sizeof(s->mmio_log));
            printf("i2000: processor reset, IP=0x%016" PRIX64 "\n",
                   s->cpu->ip);
            return;
        }
        if (st != MERCED_OK) {
            s->halted = true;
            s->halt_status = st;
            i2000_report_halt(s);
            break;
        }
    }
}

static void i2000_reset(Ia64I2000State *s) {
    bool rage128_enabled = s->rage128.enabled;
    bool i82559_enabled = s->i82559.enabled;
    uint8_t *i82559_option_rom = s->i82559.option_rom;
    size_t i82559_option_rom_size = s->i82559.option_rom_size;
    bool mouse_enabled = s->mouse_enabled;
    merced_reset(s->cpu);
    vga_ibm_reset(&s->vga);
    rage128_init(&s->rage128, rage128_enabled, RAGE128_FB_BASE,
                 RAGE128_IO_BASE, RAGE128_MMIO_BASE, 17);
    memset(&s->int10_req, 0,
           sizeof(*s) - offsetof(Ia64I2000State, int10_req));
    i82559_init(&s->i82559, i82559_enabled, I82559_MMIO_BASE,
                I82559_IO_BASE, 18, bus_read, bus_write, s);
    s->i82559.option_rom = i82559_option_rom;
    s->i82559.option_rom_size = i82559_option_rom_size;
    ps2_keyboard_init(&s->ps2_keyboard);
    /* mouse_enabled is a CLI-configured setting (-device mouse), not
     * protocol state - a reset should clear the aux device's transient
     * state (queue, streaming flag, etc, all zeroed by the memset above)
     * without un-attaching the mouse itself. */
    s->mouse_enabled = mouse_enabled;
    /* Selected VGA option ROM, shadowed at its conventional address so any
     * legacy option-ROM scan (0x55 0xAA signature check) finds it, the same
     * way a real add-in VGA card's ROM would appear at boot. */
    i2000_load_vga_option_rom(s);
    size_t vga_rom_len = vgabios_rom_len;
    if (0xC0000ull + vga_rom_len <= s->ram_size)
        memcpy(s->ram + 0xC0000, s->vga_rom_shadow, vga_rom_len);
    s->halted = false;
    s->reset_requested = false;
    s->fw_shadow_enabled = false;
    s->post_code = 0;
    s->legacy_irq_routed = false;
    s->sac_cbnr = s->sac_ccsr = 0;
    s->port61 = s->pit2_polls = 0;
    s->pic_master_mask = s->pic_slave_mask = 0xFF;
    s->pic_master_base = 0x08;
    s->pic_slave_base = 0x70;
    s->pic_master_icw = s->pic_slave_icw = 0;
    s->pic_master_isr = s->pic_slave_isr = 0;
    s->pit0_reload = s->pit0_latch = 0;
    s->pit0_write_phase = 0;
    s->pit0_read_phase = 0;
    s->pit0_epoch_ninsts = merced_get_itc(s->cpu);
    s->pit0_next_irq = 0;
    s->pit1_reload = s->pit1_latch = 0;
    s->pit1_write_phase = 0;
    s->pit1_next_irq = 0;
    chipset_cfg_reset(s);
    s->mmio_log_n = 0;
    memset(s->mmio_log, 0, sizeof(s->mmio_log));
    memset(s->console, 0, sizeof(s->console));
    s->con_row = s->con_col = 0;
    printf("i2000: reset, IP=0x%016" PRIX64 "\n", s->cpu->ip);
}

void ia64_i2000_run(Ia64I2000State *s, const Ia64Config *cfg) {
    printf("gemu-ia64: HP i2000, Intel Itanium (Merced), 460GX chipset\n"
           "  RAM   : %" PRIu64 " MiB\n"
           "  Flash : %s (mapped at 0x%08" PRIX64 "-0xFFFFFFFF)\n"
           "  Reset : IP=0x00000000%08" PRIX64 " (PALE_RESET)\n",
           s->ram_size >> 20,
           s->flash_loaded ? s->flash_file : "(none)",
           (uint64_t)I2000_FLASH_BASE, (uint64_t)IA64_RESET_VECTOR);
    if (s->cdrom)
        printf("  CD-ROM: %s (%" PRIu64 " MiB)\n", s->cdrom_file,
               s->cdrom_size >> 20);
    if (s->hda)
        printf("  HDD   : %s (%" PRIu64 " MiB)\n", s->hda_file,
               s->hda_size >> 20);

    gemu_monitor_start(s->monitor);

    /* PAL_FREQ_RATIOS advertises a 200 MHz ITC.  Drive it from elapsed
     * machine time, not interpreted slots: otherwise a firmware 10 ms input
     * timer takes roughly half a second on a 3.8 Mslot/s host. */
    merced_set_external_itc(s->cpu, true);
    uint64_t itc_last_ns = i2000_monotonic_ns();
    uint32_t display_keys_held = 0;

    bool running = true;
    while (running) {
        /* loadvm restores this CPU flag too, so reassert machine policy.
         * Paused time must not become one enormous timer jump on resume. */
        merced_set_external_itc(s->cpu, true);
        uint64_t now_ns = i2000_monotonic_ns();
        if (gemu_monitor_is_paused(s->monitor) || s->halted) {
            itc_last_ns = now_ns;
        } else {
            uint64_t ticks = (now_ns - itc_last_ns) / I2000_ITC_TICK_NS;
            if (ticks) {
                merced_advance_itc(s->cpu, ticks);
                itc_last_ns += ticks * I2000_ITC_TICK_NS;
            }
        }
        if (s->display) {
            uint32_t keys_held = gemu_display_poll(s->display);
            uint32_t keys_changed = keys_held ^ display_keys_held;
            for (unsigned i = 0;
                 i < sizeof(i2000_keyboard_keysyms) /
                     sizeof(i2000_keyboard_keysyms[0]); i++) {
                uint32_t bit = GEMU_ACTION(i);
                if (keys_changed & bit)
                    ps2_keyboard_key(&s->ps2_keyboard,
                                     i2000_keyboard_keysyms[i],
                                     (keys_held & bit) != 0);
            }
            display_keys_held = keys_held;
            uint32_t cp;
            while ((cp = gemu_display_pop_raw_key(s->display)) != 0) {
                uint8_t next = (uint8_t)(s->uart_rx_tail + 1);
                if (next != s->uart_rx_head && cp <= 0x7F) {
                    s->uart_rx[s->uart_rx_tail] = (uint8_t)cp;
                    s->uart_rx_tail = next;
                    ps2_keyboard_tap(&s->ps2_keyboard, cp);
                    i2000_efi_bios_key(s, (uint8_t)cp);
                }
            }
            if (gemu_display_should_quit(s->display)) {
                if (cfg->no_shutdown) gemu_display_clear_flags(s->display);
                else running = false;
            }
            if (gemu_display_reset_requested(s->display)) {
                i2000_reset(s);
                gemu_display_clear_flags(s->display);
            }
        }

        if (s->vnc) {
            GemuVncKeyEvent ev;
            while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
                uint32_t cp = 0;
                if (ev.keysym <= 0x7F)        cp = ev.keysym;
                else if (ev.keysym == 0xFF0D) cp = '\r';
                else if (ev.keysym == 0xFF08) cp = '\b';
                else if (ev.keysym == 0xFF09) cp = '\t';
                else if (ev.keysym == 0xFF1B) cp = 0x1b;
                else if (ev.keysym == 0xFFFF) cp = 0x7f;
                if (cp && ev.down) {
                    uint8_t next = (uint8_t)(s->uart_rx_tail + 1);
                    if (next != s->uart_rx_head) {
                        s->uart_rx[s->uart_rx_tail] = (uint8_t)cp;
                        s->uart_rx_tail = next;
                    }
                    i2000_efi_bios_key(s, (uint8_t)cp);
                }
                ps2_keyboard_key(&s->ps2_keyboard,
                                 cp ? cp : ev.keysym, ev.down);
            }
        }

        if (s->mouse_enabled) {
            int rel_x = 0, rel_y = 0;
            bool left = s->mouse_prev_left, right = s->mouse_prev_right;
            bool have_pointer = false;
            if (s->display) {
                GemuPointerState p = gemu_display_get_pointer(s->display);
                rel_x = p.rel_x; rel_y = p.rel_y;
                left = p.button; right = p.right_button;
                have_pointer = true;
            } else if (s->vnc) {
                GemuVncPointerState p = gemu_vnc_get_pointer(s->vnc);
                rel_x = p.rel_x; rel_y = p.rel_y;
                left = p.button; right = p.right_button;
                have_pointer = true;
            }
            if (have_pointer && s->mouse_streaming) {
                s->mouse_accum_dx =
                    mouse_accumulate_axis(s->mouse_accum_dx, rel_x);
                /* SDL/VNC positive Y is down; PS/2 positive Y is up. */
                s->mouse_accum_dy =
                    mouse_accumulate_axis(s->mouse_accum_dy, -rel_y);
                s->mouse_host_left = left;
                s->mouse_host_right = right;
            }
            if (s->mouse_streaming &&
                (s->mouse_accum_dx || s->mouse_accum_dy ||
                 s->mouse_host_left != s->mouse_prev_left ||
                 s->mouse_host_right != s->mouse_prev_right) &&
                /* Only one PS/2 report is in flight at a time. Motion that
                 * arrives meanwhile stays in the bounded accumulator above,
                 * and is drained in <=127-count chunks after the real PIC
                 * EOI handshake completes. */
                s->aux_out_len == 0 &&
                s->kbc_out_len == 0 &&
                !ps2_keyboard_has_data(&s->ps2_keyboard) &&
                s->mouse_wait_rfi_generation == 0 &&
                s->cpu->ninsts >= s->mouse_retry_ninsts &&
                (int64_t)(merced_get_itc(s->cpu) -
                          s->mouse_next_report_itc) >= 0) {
                int8_t cdx = (int8_t)(s->mouse_accum_dx < -127 ? -127 :
                                      s->mouse_accum_dx > 127 ? 127 : s->mouse_accum_dx);
                int8_t cdy = (int8_t)(s->mouse_accum_dy < -127 ? -127 :
                                      s->mouse_accum_dy > 127 ? 127 : s->mouse_accum_dy);
                uint8_t flags = (uint8_t)((s->mouse_host_left ? 0x01 : 0) |
                                          (s->mouse_host_right ? 0x02 : 0) |
                                          0x08 |
                                          (cdx < 0 ? 0x10 : 0) | (cdy < 0 ? 0x20 : 0));
                aux_queue_byte(s, flags);
                aux_queue_byte(s, (uint8_t)cdx);
                aux_queue_byte(s, (uint8_t)cdy);
                s->mouse_accum_dx -= cdx;
                s->mouse_accum_dy -= cdy;
                s->mouse_prev_left = s->mouse_host_left;
                s->mouse_prev_right = s->mouse_host_right;
            }
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            switch (cmd) {
            case GEMU_MON_QUIT:
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { running = false; }
                break;
            case GEMU_MON_RESET:
                i2000_reset(s);
                break;
            case GEMU_MON_STEP: {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                if (n == 0) n = 1;
                s->halted = false;
                for (uint32_t k = 0; k < n && !s->halted; k++) {
                    if (gemu_monitor_check_exec(s->monitor,
                                                (uint32_t)s->cpu->ip))
                        break;
                    i2000_poll_interrupts(s);
                    MercedStatus st = merced_step(s->cpu);
                    if (st != MERCED_OK) {
                        s->halted = true;
                        s->halt_status = st;
                        i2000_report_halt(s);
                    }
                }
                char buf[512];
                snprintf(buf, sizeof(buf), "IP=0x%016" PRIX64 " insts=%" PRIu64 "\n",
                         s->cpu->ip, s->cpu->ninsts);
                fputs(buf, stdout);
                break;
            }
            case GEMU_MON_CUSTOM:
                i2000_custom_cmd(s);
                /* Snapshot I/O can block for several seconds with 2 GiB of
                 * guest RAM.  That host maintenance time is not guest time:
                 * counting it on the next loop can expire an EFI menu timer
                 * immediately after loadvm/savevm returns. */
                itc_last_ns = i2000_monotonic_ns();
                break;
            default:
                break;
            }
            if (!running)
                break;
        }
        if (!running)
            break;

        if (!gemu_monitor_is_paused(s->monitor)) {
            i2000_run_slice(s);
            if (!s->halted && i2000_autosave_tick(s))
                itc_last_ns = i2000_monotonic_ns();
            i2000_bench_check(s);
        }

        if (s->display) {
            gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor) || s->halted);
            i2000_render_frame(s);
            gemu_display_render(s->display, s->fb, FB_W, FB_H);
            gemu_sleep_ms(s->halted ? 30 : 1);
        } else if (s->halted) {
            gemu_sleep_ms(30);
        }
    }
    gemu_monitor_stop(s->monitor);
}
