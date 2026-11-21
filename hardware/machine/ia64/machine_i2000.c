#include "i2000.h"
#include "merced.h"
#include "chipset/i460gx.h"
#include "controller/i8259.h"
#include "i2c/at24.h"
#include "i2c/i2c.h"
#include "i2c/itanium_cart.h"
#include "i2c/lm87.h"
#include "serial/ins8250.h"
#include "storage/ata_disk.h"
#include "storage/atapi_cdrom.h"
#include "storage/ide.h"
#include "timer/i8253.h"
#include "timer/mc146818.h"
#include "input_menu.h"
#include "rage128.h"
#include "network/e100.h"
#include "network/pci_rom.h"
#include "input/ps2_keyboard.h"
#include "gemu/gemu_display.h"
#include "gemu/drive.h"
#include "gemu/net.h"
#include "gemu/monitor.h"
#include "gemu/screendump.h"
#include "gemu/util.h"
#include "gemu/audio_out.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* SAL's ExtINT vector-readback byte (see the bus_read comment at its use
 * site). Confirmed live against the SDV 0.99 debug BIOS: the address it
 * actually reads (0xFEFE0000, within the SSDM's nominal "I/O reserved"
 * 0xFEF0_0000-0xFEFF_FFFF band adjoining the documented Interrupt Delivery
 * region at 0xFEE0_0000) rather than a datasheet-confirmed offset. */
#define I2000_LEGACY_ACK_ADDR 0xFEFE0000ull

/* The i2000 memory card has eight DIMM sockets, addressed at I2C slaves
 * 0x50-0x57 and populated in matched groups of four. */
#define I2000_SPD_SLOTS 8
#define I2000_DIMMS_PER_ROW 4
#define I2000_SPD_BASE_ADDR 0x50

/* front panel framebuffer */
#define FB_W 800
#define FB_H 600
#define CELL_W 6
#define CELL_H 8
#define COLS (FB_W / CELL_W)     /* 106 */
#define ROWS (FB_H / CELL_H)     /* 50  */
#define CON_ROWS 30              /* serial console area at the bottom */

#define INSTR_PER_FRAME 50000
#define IRQ_POLL_QUANTUM 16
/* Translated code amortizes its block dispatch over far more slots than 16;
 * at JIT speed this quantum is still well under a microsecond of guest time.
 * GEMU_IRQ_QUANTUM=<power of two> overrides either default (running both
 * engines with the same quantum makes their GEMU_CPI runs identical). */
#define JIT_IRQ_POLL_QUANTUM 512
/* File-static rather than in Ia64I2000State: that struct is snapshotted raw,
 * so a new field would invalidate existing snapshots. */
static unsigned irq_poll_quantum = IRQ_POLL_QUANTUM;

/* Minimum instruction gap between a mouse byte being serviced and the next
 * one being offered - see the ack handler's mouse branch for why this can't
 * be zero (synchronous re-raise corrupts the guest ISR's return frame).
 * Matches the PIT's own tick granularity, which is already known safe. */
#define MOUSE_RETRY_DELAY_INSTS 1000u
#define KEYBOARD_RETRY_DELAY_INSTS 10000u
/* Instructions between presentations of a still-asserted level interrupt. */
#define SCI_REPRESENT_INSTS 20000u

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
    I2000_KEY_F1,       /* F1-F12 follow in order: Windows Setup uses F3,
                         * F6, F8 ... and they have no ASCII code for the
                         * window's typed-text path. */
    I2000_KEY_LSHIFT = I2000_KEY_F1 + 12,
    I2000_KEY_RSHIFT,
    I2000_KEY_LCTRL,
    I2000_KEY_RCTRL,
    I2000_KEY_LALT,
    I2000_KEY_RALT,
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
    { "key_f1",         GEMU_ACTION(I2000_KEY_F1 + 0), "F1" },
    { "key_f2",         GEMU_ACTION(I2000_KEY_F1 + 1), "F2" },
    { "key_f3",         GEMU_ACTION(I2000_KEY_F1 + 2), "F3" },
    { "key_f4",         GEMU_ACTION(I2000_KEY_F1 + 3), "F4" },
    { "key_f5",         GEMU_ACTION(I2000_KEY_F1 + 4), "F5" },
    { "key_f6",         GEMU_ACTION(I2000_KEY_F1 + 5), "F6" },
    { "key_f7",         GEMU_ACTION(I2000_KEY_F1 + 6), "F7" },
    { "key_f8",         GEMU_ACTION(I2000_KEY_F1 + 7), "F8" },
    { "key_f9",         GEMU_ACTION(I2000_KEY_F1 + 8), "F9" },
    { "key_f10",        GEMU_ACTION(I2000_KEY_F1 + 9), "F10" },
    { "key_f11",        GEMU_ACTION(I2000_KEY_F1 + 10), "F11" },
    { "key_f12",        GEMU_ACTION(I2000_KEY_F1 + 11), "F12" },
    /* Modifiers are held keys too: Setup's Shift+F10 console, Ctrl/Alt
     * combinations.  Typed text only ever carried its own synthetic Shift. */
    { "key_lshift",     GEMU_ACTION(I2000_KEY_LSHIFT), "Left Shift" },
    { "key_rshift",     GEMU_ACTION(I2000_KEY_RSHIFT), "Right Shift" },
    { "key_lctrl",      GEMU_ACTION(I2000_KEY_LCTRL),  "Left Ctrl" },
    { "key_rctrl",      GEMU_ACTION(I2000_KEY_RCTRL),  "Right Ctrl" },
    { "key_lalt",       GEMU_ACTION(I2000_KEY_LALT),   "Left Alt" },
    { "key_ralt",       GEMU_ACTION(I2000_KEY_RALT),   "Right Alt" },
};

static const uint32_t i2000_keyboard_keysyms[] = {
    0xff52, 0xff54, 0xff51, 0xff53, 0xff50,
    0xff57, 0xff55, 0xff56, 0xff63, 0xffff,
    0xffbe, 0xffbf, 0xffc0, 0xffc1, 0xffc2, 0xffc3,     /* F1-F6 */
    0xffc4, 0xffc5, 0xffc6, 0xffc7, 0xffc8, 0xffc9,     /* F7-F12 */
    0xffe1, 0xffe2, 0xffe3, 0xffe4, 0xffe9, 0xffea,     /* Shift/Ctrl/Alt L,R */
};
_Static_assert(sizeof(i2000_keyboard_keysyms) / sizeof(i2000_keyboard_keysyms[0]) ==
               sizeof(i2000_keyboard_actions) / sizeof(i2000_keyboard_actions[0]),
               "one keysym per keyboard action");
#define MOUSE_BACKLOG_MAX       254
/* The processor runs at 11/2 of the 133.288 MHz front-side bus (ratio strap
 * code 3), and the ITC counts at the processor clock: 733.084 MHz. */
#define I2000_ITC_HZ UINT64_C(733084000)
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

#define RAGE128_PCI_DEV       2u
#define NIC_PCI_DEV             5u
#define AUDIO_PCI_DEV           4u

typedef struct {
    uint64_t addr;
    uint64_t val;
    uint32_t count;
    uint8_t  is_write;
    uint8_t  size;
} MmioLogEnt;

struct Ia64I2000State {
    GemuMonitor *monitor;
    GemuDisplay *display;
    Merced      *cpu;

    uint8_t  *ram;
    uint64_t  ram_size;
    /* Plain-RAM fast path: [I2000_FAST_RAM_BASE, fast_ram_end) holds no
     * device window, so a load or store there is a copy. See
     * i2000_update_fast_ram(). mem_watch mirrors the monitor's memory
     * breakpoints, which every access must still honour. */
    uint64_t  fast_ram_end;
    bool      mem_watch;
    /* The IFB's real-time clock. Battery-backed: it is not reset with the
     * devices below. rtc_irq is IRQ8's level, presented to the CPU through the
     * I/O SAPIC until the guest reads register C. */
    Mc146818  rtc;
    bool      rtc_irq;
    uint64_t  rtc_retry_ninsts;
    uint8_t  *flash;
    char      flash_file[512];
    uint32_t  flash_image_size;
    bool      flash_loaded;

    bool      halted;
    /* Keep the former bool's one-byte snapshot slot. Legacy true requests
     * were CF9 hard resets; 2 additionally represents a CPU-only reset. */
    uint8_t   reset_requested; /* 0 = none, 1 = system, 2 = processor */
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
    /* Interval timer, clocked at 1.193182 MHz from the ITC: its state is
     * brought up to date from the ITC whenever it is accessed or polled.
     * port61 is the system-control port B latch (bit 0 gates channel 2). */
    I8253     pit;
    uint64_t  pit_itc_mark;      /* ITC value the timer was last advanced to */
    uint64_t  pit_remainder;     /* leftover ITC ticks x 1193182 */
    uint64_t  pit_clock;         /* PIT clocks since reset */
    uint8_t   port61;
    /* The IFB's 8259 pair.  Its lines come from the devices' legacy
     * interrupt state (i2000_pic_sync()); its INT reaches the processor as
     * an ExtINT, and the INTA cycles are the read of I2000_LEGACY_ACK_ADDR. */
    I8259Pair pic;
    uint8_t   kbc_command_byte;
    uint8_t   kbc_pending_write; /* 0=keyboard data, 1=command byte, 2=output port, 3=aux */
    uint8_t   kbc_out[8];
    uint8_t   kbc_out_pos, kbc_out_len;
    uint32_t  pci_cfg_addr;
    /* Memory card A's row-0 I2C segment holds one SPD EEPROM per populated
     * socket. The IFB SMBus segment is separate: it carries the processor
     * cartridge's PIROM and Scratch EEPROM and the board's clock-ratio latch. */
    I2cBus    memcard_i2c;
    I2cBus    smbus;
    At24      spd[I2000_SPD_SLOTS];
    ItaniumCartRom cart_pirom, cart_scratch;
    Lm87      hardware_monitor;
    uint8_t   ratio_latch;
    uint8_t   cmd649_cfg[256];
    /* ide1-cd0 from the drive table (NULL = no drive configured; present
     * but !gemu_drive_inserted() = empty tray).  A pointer replacing the
     * old FILE *. The
     * file/size fields below are informational only (kept in sync by
     * i2000_cdrom_changed); I/O always asks the drive. */
    GemuDrive *cdrom;
    char      cdrom_file[512];
    uint64_t  cdrom_size;
    /* The secondary IDE channel: the CD-ROM drive as device 0, present
     * whether or not -cdrom gave it a medium. */
    IdeChannel ide_secondary;
    AtapiCdrom cd;
    /* CMD-649 primary channel: a real ATA (not ATAPI) hard disk, backed by
     * -hda's raw 512-byte-sector image. Single device (master) only, always
     * responding regardless of the drive-select bit - same simplification
     * the ATAPI/CD-ROM secondary channel above already makes. */
    GemuDrive *hda;      /* ide0-hd0; file/size below are informational */
    char      hda_file[512];
    uint64_t  hda_size;
    /* The primary IDE channel (the CMD-649's): the -hda disk as device 0. */
    IdeChannel ide_primary;
    AtaDisk   hd;
    /* COM1, the firmware's serial console: a 16450 whose terminal is the
     * host (output to the console window and stdout, input from con_rx). */
    Ins8250   uart;
    uint8_t   con_rx[256], con_rx_head, con_rx_tail;

    /* The SDV's display card: a Rage 128 with the VGA core behind it. The
     * frame buffer and the video BIOS image are host-owned buffers, saved
     * and restored with the machine's snapshots separately from the structs. */
    bool      video_enabled;
    Vga       vga;
    Rage128   rage128;
    uint8_t  *vram;                 /* RAGE128_VRAM_SIZE bytes */
    uint8_t  *video_rom;
    uint32_t  video_rom_len;
    /* The onboard Intel 82559 10/100 Base-T LAN (-net nic): the card is
     * plain state; its EFI UNDI ROM is a host-owned image. */
    bool      nic_enabled;
    E100      nic;
    uint8_t  *nic_rom;
    uint32_t  nic_rom_len;
    PciAudio  audio;
    GemuAudioOut *audio_out;

    MmioLogEnt mmio_log[MMIO_LOG_N];
    int        mmio_log_n;

    /* PS/2 mouse (-device mouse), routed through the
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
    /* deferred multi-byte mouse-packet retry (see the ack handler's
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
    /* use the real PIC EOI handshake as mouse-flow control instead of
     * an arbitrary multi-million-instruction cooldown. Host motion is
     * coalesced while a report is in flight. Direction reversals discard
     * stale motion in the old direction, and each axis is capped at two
     * packets so a slow guest cannot build a long cursor backlog. */
    uint64_t mouse_wait_rfi_generation;
    int32_t mouse_accum_dx, mouse_accum_dy;
    bool mouse_host_left, mouse_host_right;
    uint64_t mouse_next_report_itc;
    /* persistent writable flash overlay (the ROM itself stays clean). */
    char nvram_path[512];
    bool nvram_dirty;
    /* legacy IRQ1 latch for queued 8042 keyboard/controller output. */
    bool keyboard_irq_pending;
    /* reusable PS/2 keyboard device behind this machine's 8042. */
    Ps2Keyboard ps2_keyboard;
    uint64_t keyboard_retry_ninsts;
    /* The CD's interrupt epoch last delivered through GSI15. */
    uint32_t atapi_irq_delivered;
    bool atapi_legacy_irq_latched;
    /* keyboard IRQ1 output edge for an OS-programmed fixed GSI1
     * vector; cleared by each 8042 data-port read. */
    bool keyboard_gsi_asserted;
    /* the primary-channel ATA disk as Windows' IDE driver drives it:
     * an INTRQ latch (normal Status read acknowledges, Alternate Status
     * doesn't, nIEN masks the output) delivered through GSI14, and PIO
     * transfers in DRQ blocks of one sector, or SET MULTIPLE sectors. */
    /* The disk's interrupt epoch last delivered (ata_deliver_irq()). */
    uint32_t ata_irq_delivered;
    /* the firmware's legacy IRQ14 (slave PIC input 6) for the disk,
     * latched until its ExtINT acknowledge like the CD's IRQ15. */
    bool ata_legacy_irq_latched;
    /* aux IRQ12 output edge for an OS-programmed fixed GSI12 vector;
     * cleared by each 8042 data-port read. Occupies what was padding
     * before `chipset`, so the snapshot layout is unchanged. */
    bool mouse_gsi_asserted;

    /* IFB power-management wiring. SCI is a level on the legacy line the IFB
     * routes it to; it reaches the CPU through that line's I/O SAPIC entry
     * and is re-presented until the handler clears the source. */
    int       sci_irq;
    bool      sci_level;
    uint64_t  sci_retry_ninsts;
    bool      cpu_stopped;          /* S1: processors held in Stop-Grant */
    bool      power_off_requested;  /* S3/S4/S5: main power removed */
    bool      wake_reset_requested; /* woken from S3/S4/S5 */

    I460GX chipset;
};

static void i2000_update_fast_ram(Ia64I2000State *s);


static void mmio_log(Ia64I2000State *s, uint64_t addr, uint64_t val,
                     unsigned size, bool is_write);
static void i2000_reset(Ia64I2000State *s, bool warm);
static void iosapic_raise_gsi(Ia64I2000State *s, unsigned gsi);
static void i2000_pic_raise(Ia64I2000State *s);

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

/* The system timer's output is IRQ0: each rising edge of channel 0 latches
 * the request in the 8259 and, if it is unmasked, offers it to the processor
 * as an ExtINT (see i2000_poll_interrupts). */
static void i2000_pit_out(void *ud, unsigned channel, bool level) {
    Ia64I2000State *s = ud;
    if (channel != 0 || !level)
        return;
    iosapic_raise_gsi(s, 0);
    s->pit_irq_pending = true;
    i2000_pic_raise(s);
}

/* Advance the timer to the present ITC. */
static void i2000_pit_run(Ia64I2000State *s) {
    uint64_t now = merced_get_time_base(s->cpu);
    if (now < s->pit_itc_mark) {            /* the ITC was set backwards */
        s->pit_itc_mark = now;
        return;
    }
    uint64_t scaled = (now - s->pit_itc_mark) * I8254_INPUT_HZ +
                      s->pit_remainder;
    s->pit_itc_mark = now;
    s->pit_remainder = scaled % I2000_ITC_HZ;
    uint64_t clocks = scaled / I2000_ITC_HZ;
    s->pit_clock += clocks;
    while (clocks) {
        uint32_t step = clocks > UINT32_MAX ? UINT32_MAX : (uint32_t)clocks;
        i8253_tick(&s->pit, step);
        clocks -= step;
    }
}

static void i2000_pit_reset(Ia64I2000State *s) {
    s->pit.out_changed = i2000_pit_out;
    s->pit.ud = s;
    i8253_reset(&s->pit);
    i8253_set_gate(&s->pit, 2, false);      /* port 61h bit 0 is clear */
    s->pit_itc_mark = s->cpu ? merced_get_time_base(s->cpu) : 0;
    s->pit_remainder = 0;
    s->pit_clock = 0;
    s->port61 = 0;
}

/* System control port B (61h): bit 0 gates channel 2, bit 1 is the speaker
 * enable, bit 4 the refresh toggle (about every 15 us) and bit 5 channel
 * 2's output. */
static uint8_t i2000_port61_read(Ia64I2000State *s) {
    i2000_pit_run(s);
    return (uint8_t)((s->port61 & 0x0F) | (((s->pit_clock / 18) & 1) << 4) |
                     (i8253_out(&s->pit, 2) ? 0x20 : 0));
}

static void i2000_port61_write(Ia64I2000State *s, uint8_t value) {
    i2000_pit_run(s);
    s->port61 = value & 0x0F;
    i8253_set_gate(&s->pit, 2, value & 1);
}

static uint64_t size_mask(unsigned size) {
    return size >= 8 ? ~0ull : (1ull << (size * 8)) - 1;
}

static bool atapi_debug_enabled(void) {
    const char *value = getenv("ATA_DEBUG");
    return value && value[0] && strcmp(value, "0") != 0;
}

static uint64_t i2000_now_ninsts(void *ctx) {
    return ((Ia64I2000State *)ctx)->cpu->ninsts;
}

/* The IDE devices' wiring: the -hda disk and the -cdrom drive, each busy
 * for ATAPI_IRQ_LATENCY_NINSTS instructions after a command.  Also after a state load, which brings
 * the device's state but not its host pointers. */
static void i2000_ide_wire(Ia64I2000State *s) {
    AtaDiskConfig cfg = {
        .drive = s->hda, .now = i2000_now_ninsts, .ctx = s,
        .latency = ATAPI_IRQ_LATENCY_NINSTS, .debug = getenv("ATA_DEBUG") != NULL,
    };
    ata_disk_rewire(&s->hd, &cfg);
    s->ide_primary.dev[0] = s->hda ? &s->hd.ide : NULL;
    s->ide_primary.dev[1] = NULL;
    AtapiCdromConfig cd_cfg = {
        .drive = s->cdrom, .now = i2000_now_ninsts, .ctx = s,
        .latency = ATAPI_IRQ_LATENCY_NINSTS, .debug = atapi_debug_enabled(),
    };
    atapi_cdrom_rewire(&s->cd, &cd_cfg);
    s->ide_secondary.dev[0] = &s->cd.ide;
    s->ide_secondary.dev[1] = NULL;
}

/* Whether the 8259 pair would pass legacy ISA IRQ n to the CPU: its own
 * mask bit clear and, for the slave's IRQs 8-15, the IRQ2 cascade too.  A
 * masked request isn't delivered (EFI masks IRQ15 and polls the CD; an
 * IRQ15 it never enabled left its CD driver timing out every read). */
static bool i2000_pic_irq_open(const Ia64I2000State *s, unsigned irq) {
    if (irq < 8)
        return !(s->pic.master.imr & (1u << irq));
    return !(s->pic.slave.imr & (1u << (irq - 8))) &&
           !(s->pic.master.imr & 0x04);
}

/* Drive INTRQ out once the disk is ready: the OS's fixed GSI14 vector, or -
 * while that entry is in its reset state or ExtINT - the firmware's legacy
 * IRQ14, whose PIC edge then survives Status reads until the ExtINT
 * acknowledge.  (The firmware's AtaPim waits for that interrupt after
 * IDENTIFY; without it, it gave up on the disk and the Boot Manager never
 * saw it.)  nIEN, SRST and selecting the absent slave lower INTRQ. */
static void ata_deliver_irq(Ia64I2000State *s) {
    uint32_t epoch = ata_disk_irq_epoch(&s->hd);
    if (!ide_intrq(&s->ide_primary) || s->ata_irq_delivered == epoch)
        return;
    uint64_t rte = s->chipset.iosapic_rte[14];
    if (!(rte & I460GX_IOSAPIC_RTE_MASK) && ((rte >> 8) & 7) <= 1) {
        iosapic_raise_gsi(s, 14);
        s->ata_irq_delivered = epoch;
    } else if (rte == I460GX_IOSAPIC_RTE_MASK ||
               (!(rte & I460GX_IOSAPIC_RTE_MASK) && ((rte >> 8) & 7) == 7)) {
        if ((s->cpu->cr[2] == 0 || (s->cpu->psr & (UINT64_C(1) << 34)) ||
             !(s->cpu->psr & (UINT64_C(1) << 14))) &&
            s->ram_size > 0x48E) {
            /* Also whenever the interrupt can't be delivered now: inside an
             * IA-32 BIOS callback (PSR.is), or in the native intercept code
             * around one (PSR.i clear) - EFI runs those with interrupts off,
             * so IRQ14 can't reach the BIOS's own handler before its 40:8E
             * wait times out.  Lost that way: the first GPT read after boot
             * (no partitions, so the Windows boot entry was "Not Found"),
             * and later a READ MULTIPLE block inside FPSWA.EFI ("Device
             * Error"). */
            s->ata_irq_delivered = epoch;
            /* The SDV's "BIOS CallBacks" disk path runs the IA-32 BIOS's
             * INT 13h with cr.iva zero, where GEMU delivers no external
             * interrupts (see i2000_efi_bios_key()).  That code polls the
             * BIOS data area's hard-disk interrupt flag, 40:8E, which its
             * IRQ14 handler would set; complete that handler's effect here
             * instead, or every sector waits out the BIOS timeout (seconds
             * each, a multi-minute firmware boot). */
            s->ram[0x48E] = 0xFF;
        } else if (i2000_pic_irq_open(s, 14)) {
            s->ata_irq_delivered = epoch;
            s->ata_legacy_irq_latched = true;
            i2000_pic_raise(s);
        }   /* masked: stays pending until unmasked or acknowledged */
    }
}

/* PCI IDE has four task-file/control I/O windows and one 16-byte bus-master
 * window. Preserve their hardwired type/alignment bits on byte and dword
 * accesses, including the all-ones sizing probe used by EFI and the OS. */
static uint8_t cmd649_cfg_read8(Ia64I2000State *s, unsigned reg) {
    if (reg >= 0x24 && reg < 0x28)
        return 0; /* BAR5 is unimplemented. */
    if (reg >= 0x10 && reg < 0x24) {
        static const uint32_t masks[5] = {
            UINT32_C(0xfffffff8), UINT32_C(0xfffffffc),
            UINT32_C(0xfffffff8), UINT32_C(0xfffffffc),
            UINT32_C(0xfffffff0),
        };
        unsigned bar = (reg - 0x10) / 4;
        uint32_t value;
        memcpy(&value, &s->cmd649_cfg[0x10 + bar * 4], sizeof(value));
        value = (value & masks[bar]) | 1;
        return (uint8_t)(value >> ((reg & 3) * 8));
    }
    return s->cmd649_cfg[reg];
}

static uint64_t pci_cfg_read(Ia64I2000State *s, unsigned lane, unsigned size) {
    uint32_t a = s->pci_cfg_addr;
    if (!(a & 0x80000000u) || lane + size > 4)
        return size_mask(size);
    unsigned bus = (a >> 16) & 0xFF;
    unsigned dev = (a >> 11) & 0x1F;
    unsigned fun = (a >> 8) & 7;
    unsigned reg = (a & 0xFC) + lane;
    if (s->audio.model && bus == 0 && dev == AUDIO_PCI_DEV && fun == 0)
        return pci_audio_cfg_read(&s->audio, reg, size);
    if (s->video_enabled && bus == 0 && dev == RAGE128_PCI_DEV &&
        fun == 0 && reg + size <= sizeof(s->rage128.cfg))
        return rage128_cfg_read(&s->rage128, reg, size);
    if (s->nic_enabled && bus == 0 && dev == NIC_PCI_DEV && fun == 0 &&
        reg + size <= 256) {
        return e100_cfg_read(&s->nic, reg, size);
    }
    if (bus == 0 && dev == CMD649_PCI_DEV && fun == CMD649_PCI_FUN &&
        reg + size <= sizeof(s->cmd649_cfg)) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v |= (uint64_t)cmd649_cfg_read8(s, reg + i) << (i * 8);
        if (atapi_debug_enabled() && reg < 4) {
            static unsigned identity_reads;
            if (identity_reads++ < 16)
                fprintf(stderr, "i2000: ATAPI controller PCI identity read "
                        "reg=%#x size=%u -> %#" PRIx64 "\n", reg, size, v);
        }
        return v;
    }

    uint64_t value;
    if (i460gx_pci_read(&s->chipset, bus, dev, fun, reg, size, &value))
        return value;

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
    if (s->audio.model && bus == 0 && dev == AUDIO_PCI_DEV && fun == 0) {
        pci_audio_cfg_write(&s->audio, reg, size, val);
        return;
    }
    if (s->video_enabled && bus == 0 && dev == RAGE128_PCI_DEV &&
        fun == 0 && reg + size <= sizeof(s->rage128.cfg)) {
        rage128_cfg_write(&s->rage128, reg, size, (uint32_t)val);
        i2000_update_fast_ram(s);
        return;
    }
    if (s->nic_enabled && bus == 0 && dev == NIC_PCI_DEV && fun == 0 &&
        reg + size <= 256) {
        e100_cfg_write(&s->nic, reg, size, (uint32_t)val);
        i2000_update_fast_ram(s);
        return;
    }
    if (bus == 0 && dev == CMD649_PCI_DEV && fun == CMD649_PCI_FUN &&
        reg + size <= sizeof(s->cmd649_cfg)) {
        /* Identity, revision/class and header type are read-only.  BAR
         * address bits are writable, but their type/alignment bits are not. */
        for (unsigned i = 0; i < size; i++) {
            unsigned off = reg + i;
            if (off >= 0x24 && off < 0x28)
                continue;
            if (off >= 4 && !(off >= 8 && off < 16)) {
                s->cmd649_cfg[off] = (uint8_t)(val >> (i * 8));
                if (off >= 0x10 && off < 0x24)
                    s->cmd649_cfg[off] = cmd649_cfg_read8(s, off);
            }
        }
        return;
    }
    if (i460gx_pci_write(&s->chipset, bus, dev, fun, reg, val, size))
        return;
    uint64_t key = 0xC000000000000000ull |
                   ((uint64_t)bus << 24) | ((uint64_t)dev << 19) |
                   ((uint64_t)fun << 16) | reg;
    mmio_log(s, key, val & size_mask(size), size, true);
}

/* ── Serial console ──────────────────────────────────────────────────────── */

static void con_newline(Ia64I2000State *s) {
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
}

/* COM1's terminal.  Characters the firmware sends go to the console; host
 * keystrokes queue in con_rx and are offered once the receive register is
 * empty, so typing faster than the firmware polls never overruns.  With
 * clock_hz 0 a character takes one tick, and the UART is ticked at every
 * register access and interrupt poll: bytes move as fast as they're polled,
 * as on the old inline port.  No IRQ4 yet (A8). */
static void i2000_uart_tx(void *dev, uint8_t b) {
    Ia64I2000State *s = dev;
    con_putc(s, (char)b);
    fputc(b, stdout);
    fflush(stdout);
}
static bool i2000_uart_rx(void *dev, uint8_t *b) {
    Ia64I2000State *s = dev;
    if ((s->uart.lsr & 0x01) || s->con_rx_head == s->con_rx_tail)
        return false;
    *b = s->con_rx[s->con_rx_head++];
    return true;
}
static void i2000_uart_wire(Ia64I2000State *s) {
    s->uart.model = INS8250_16450;
    s->uart.clock_hz = 0;                    /* one tick per character */
    s->uart.set_irq = NULL;
    s->uart.ud = s;
    s->uart.dev_tx = i2000_uart_tx;
    s->uart.dev_lines = NULL;
    s->uart.dev_rx = i2000_uart_rx;
    s->uart.dev = s;
    s->uart.modem_in = 0xB0;                 /* CTS, DSR, DCD */
}
static void i2000_uart_reset(Ia64I2000State *s) {
    i2000_uart_wire(s);
    ins8250_reset(&s->uart);
    s->con_rx_head = s->con_rx_tail = 0;
}
/* A host character for the serial console (dropped if 255 are waiting). */
static void i2000_con_rx(Ia64I2000State *s, uint8_t c) {
    uint8_t next = (uint8_t)(s->con_rx_tail + 1);
    if (next != s->con_rx_head) {
        s->con_rx[s->con_rx_tail] = c;
        s->con_rx_tail = next;
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

/* The RTC counts on the machine's own time base, so it advances exactly as
 * fast as the guest does, whether the ITC follows the host clock or the
 * instruction count. */
static uint64_t i2000_rtc_clock(void *opaque) {
    Ia64I2000State *s = opaque;
    return s->cpu ? merced_get_time_base(s->cpu) : 0;
}

/* IRQ8 changed level. An assertion is also the IFB's RTC_STS event. */
/* An edge-triggered IRQ8 entry gets one interrupt per rising edge; this is
 * set by the edge and cleared once it has been sent.  File-static, as
 * Ia64I2000State is snapshotted raw. */
static bool i2000_rtc_edge_pending;

static void i2000_rtc_irq(void *opaque, bool level) {
    Ia64I2000State *s = opaque;
    if (level && !s->rtc_irq)
        i2000_rtc_edge_pending = true;
    s->rtc_irq = level;
    if (level)
        i460gx_pm_rtc_alarm(&s->chipset);
}

/* Days since 1970-01-01 of a proleptic Gregorian date, and the inverse. These
 * stand in for timegm()/gmtime_r(), which not every host C library has. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int64_t tm_to_seconds(const struct tm *tm) {
    int64_t days = days_from_civil(tm->tm_year + 1900, (unsigned)tm->tm_mon + 1,
                                   (unsigned)tm->tm_mday);
    return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

static void seconds_to_tm(int64_t seconds, struct tm *tm) {
    int64_t days = seconds / 86400;
    int64_t rem = seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1;
    unsigned month = mp < 10 ? mp + 3 : mp - 9;
    int64_t year = (int64_t)yoe + era * 400 + (month <= 2);
    memset(tm, 0, sizeof(*tm));
    tm->tm_year = (int)(year - 1900);
    tm->tm_mon = (int)month - 1;
    tm->tm_mday = (int)day;
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)(((days % 7) + 11) % 7);   /* 1970-01-01 was a Thursday */
    tm->tm_yday = (int)(days - days_from_civil(year, 1, 1));
}

/* The host's local calendar time as a seconds count that treats it as UTC,
 * so differences are exact whatever the zone. GEMU_FIXED_TIME pins it for
 * reproducible boots. */
static int64_t host_naive_seconds(void) {
    const char *fixed = getenv("GEMU_FIXED_TIME");
    if (fixed)
        return (int64_t)strtoll(fixed, NULL, 10);
    time_t now = time(NULL);
    const struct tm *local = localtime(&now);
    return local ? tm_to_seconds(local) : (int64_t)now;
}

static void rtc_set_naive_seconds(Ia64I2000State *s, int64_t seconds) {
    struct tm tm;
    seconds_to_tm(seconds, &tm);
    mc146818_set_time(&s->rtc, &tm);
}

static int64_t rtc_naive_seconds(Ia64I2000State *s) {
    struct tm tm;
    mc146818_get_time(&s->rtc, &tm);
    return tm_to_seconds(&tm);
}

/* A cell with no stored state: the clock starts at the host's time, in BCD
 * with 24-hour mode. Extended byte 3 bit 3 is the firmware's "previous boot
 * completed" flag; the bootstrap takes its recovery path whenever it is
 * clear, which is what a real board's cleared CMOS would do once and then
 * set. */
static void i2000_rtc_initialise(Ia64I2000State *s) {
    mc146818_set_defaults(&s->rtc);
    rtc_set_naive_seconds(s, host_naive_seconds());
    s->rtc.nv[128 + 3] = 0x08;
}

/* The two IDE channels' INTRQ lines as the 8259 sees them: asserted while
 * a completion is pending and visible (not BSY, nIEN/SRST clear, the
 * present drive selected) until a normal Status read lowers them.  IRR
 * reports a raised line whether or not IMR masks it - AMI's IA-32 BIOS
 * waits for IRQ14/15 by polling IRR with interrupts off, and EFI keeps
 * IRQ15 masked while it does. */
/* The 8259 pair's input lines, from the devices' legacy interrupt state.
 * The PIT, keyboard and mouse flags are set only on their legacy (ExtINT)
 * paths and cleared by the acknowledge; IRQ12 also drops while the mouse
 * waits out its post-acknowledge delay, so its retry is a new edge.  The
 * IDE lines are INTRQ, or a request the PIC has taken (the latch) that
 * outlives a Status read until acknowledged.  The RTC's IRQ8 reaches the
 * processor only through the I/O SAPIC. */
static void i2000_pic_sync(Ia64I2000State *s) {
    I8259Pair *p = &s->pic;
    i8259_pair_set_irq(p, 0, s->pit_irq_pending);
    i8259_pair_set_irq(p, 1, s->keyboard_irq_pending);
    i8259_pair_set_irq(p, 12, s->mouse_irq_pending &&
                              s->mouse_wait_rfi_generation == 0 &&
                              s->cpu->ninsts >= s->mouse_retry_ninsts);
    i8259_pair_set_irq(p, 14, s->ata_legacy_irq_latched || ide_intrq(&s->ide_primary));
    i8259_pair_set_irq(p, 15, s->atapi_legacy_irq_latched || ide_intrq(&s->ide_secondary));
}
/* A device's legacy path asks for the processor: offer the pair's INT as an
 * ExtINT (vector 0) if it is asserting one. */
static void i2000_pic_raise(Ia64I2000State *s) {
    i2000_pic_sync(s);
    if (i8259_pair_intr(&s->pic))
        merced_raise_external(s->cpu, 0);
}
/* The pair as the board resets it: everything masked, and the vector bases
 * the firmware's legacy thunk uses until it programs ICW2 itself. */
static void i2000_pic_reset(Ia64I2000State *s) {
    i8259_pair_reset(&s->pic);
    s->pic.master.vector_base = 0x08;
    s->pic.slave.vector_base = 0x70;
}

static uint64_t io_port_read(Ia64I2000State *s, uint64_t port, unsigned size) {
    uint64_t value;
    if (port <= 0xffff && pci_audio_io_access(&s->audio, false, port, size, &value)) return value;
    if (i460gx_ifb_io_read(&s->chipset, 0, port, size, &value) ||
        i460gx_ifb_io_read(&s->chipset, 2, port, size, &value))
        return value;
    if (s->video_enabled && port <= 0xFFFF &&
        rage128_io_access(&s->rage128, false, (uint16_t)port, size, &value))
        return value;
    if (s->nic_enabled && port <= 0xFFFF &&
        e100_io_access(&s->nic, false, (uint16_t)port, size, &value))
        return value;
    if (i460gx_ifb_io_read(&s->chipset, 3, port, size, &value))
        return value;
    if (port == 0x60 && size == 1) {               /* 8042 data */
        /* Consuming a byte ends the current IRQ1 edge; the next poll raises
         * a new one if another byte is waiting. */
        s->keyboard_gsi_asserted = false;
        s->mouse_gsi_asserted = false;
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
    if (port >= 0x70 && port <= 0x77 && size == 1) {
        uint8_t v;
        if (mc146818_io(&s->rtc, false, (uint16_t)port, &v,
                        s->chipset.ifb_cfg[0xc8], s->chipset.ifb_cfg[0xfc] & 1))
            return v;
    }
    if (port == PCI_CFG_ADDR && size == 4) return s->pci_cfg_addr;
    if (port >= PCI_CFG_DATA && port < PCI_CFG_DATA + 4)
        return pci_cfg_read(s, (unsigned)(port - PCI_CFG_DATA), size);
    if (port >= COM1_PORT && port < COM1_PORT + 8) {
        ins8250_tick(&s->uart, 1);
        return ins8250_read(&s->uart, (unsigned)(port - COM1_PORT));
    }
    if ((port == 0x20 || port == 0xA0) && size == 1) {
        uint8_t v;
        i2000_pic_sync(s);
        i8259_pair_io_read(&s->pic, (uint16_t)port, &v);
        if (getenv("ATA_IRQ_TRACE"))
            fprintf(stderr, "i2000: PIC %s read -> %#04x ninsts=%" PRIu64 "\n",
                    port == 0x20 ? "master" : "slave", v, s->cpu->ninsts);
        return v;
    }
    if (port == 0x21) return s->pic.master.imr;
    if (port == 0xA1) return s->pic.slave.imr;
    if (port >= 0x40 && port <= 0x43 && size == 1) {
        i2000_pit_run(s);
        return i8253_io_read(&s->pit, port);
    }
    if (port == 0x61 && size == 1)
        return i2000_port61_read(s);
    if ((port >= 0x170 && port <= 0x177) || port == 0x376)
        return ide_read(&s->ide_secondary,
                        port == 0x376 ? 8 : (unsigned)(port - 0x170), size);
    if (s->hda && ((port >= 0x1F0 && port <= 0x1F7) || port == 0x3F6)) {
        unsigned reg = port == 0x3F6 ? 8 : (unsigned)(port - 0x1F0);
        /* A ready interrupt reaches the PIC/SAPIC before the Status read
         * that acknowledges it can lower INTRQ. */
        if (reg >= 7)
            ata_deliver_irq(s);
        return ide_read(&s->ide_primary, reg, size);
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
    if (port <= 0xffff && pci_audio_io_access(&s->audio, true, port, size, &val)) return;
    if (i460gx_ifb_io_write(&s->chipset, 0, port, val, size) ||
        i460gx_ifb_io_write(&s->chipset, 2, port, val, size))
        return;
    if (s->video_enabled && port <= 0xFFFF &&
        rage128_io_access(&s->rage128, true, (uint16_t)port, size, &val))
        return;
    if (s->nic_enabled && port <= 0xFFFF &&
        e100_io_access(&s->nic, true, (uint16_t)port, size, &val))
        return;
    if (i460gx_ifb_io_write(&s->chipset, 3, port, val, size))
        return;
    if (port == 0x20 || port == 0x21 || port == 0xA0 || port == 0xA1) {
        I8259 *chip = port < 0xA0 ? &s->pic.master : &s->pic.slave;
        bool mask_write = (port & 1) && chip->icw_step == 0;
        if (getenv("PIC_DEBUG") && mask_write && val != chip->imr)
            fprintf(stderr, "i2000: PIC %s mask %#04x -> %#04x "
                    "(base=%#04x) ninsts=%" PRIu64 "\n",
                    port < 0xA0 ? "master" : "slave", chip->imr,
                    (unsigned)(uint8_t)val, chip->vector_base, s->cpu->ninsts);
        if (getenv("MOUSE_DEBUG") && !(port & 1))
            fprintf(stderr, "MOUSE_DEBUG: PIC cmd port=%#02x val=%#02x "
                    "ninsts=%" PRIu64 "\n", (unsigned)port, (unsigned)val,
                    s->cpu->ninsts);
        i2000_pic_sync(s);
        i8259_pair_io_write(&s->pic, (uint16_t)port, (uint8_t)val);
        /* Unmasking the master can expose a waiting request (the timer's). */
        if (mask_write && port == 0x21)
            i2000_pic_raise(s);
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
    if (port >= 0x70 && port <= 0x77 && size == 1) {
        uint8_t v = (uint8_t)val;
        if (mc146818_io(&s->rtc, true, (uint16_t)port, &v,
                        s->chipset.ifb_cfg[0xc8], s->chipset.ifb_cfg[0xfc] & 1))
            return;
    }
    if (port == RESET_CTRL_PORT && size == 1) {
        /* Intel reset-control convention: bit 1 selects a hard reset and
         * bit 2 triggers it.  SAL writes 02h followed by 06h, then waits in
         * a dead loop for the platform reset to arrive. */
        if (val & 0x04) {
            s->reset_requested = (val & 0x02) ? 1 : 2;
            merced_request_exit(s->cpu);   /* handled after this slot */
        }
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
    if (port >= 0x40 && port <= 0x43 && size == 1) {
        i2000_pit_run(s);
        i8253_io_write(&s->pit, port, (uint8_t)val);
        return;
    }
    if (port == 0x61 && size == 1) {
        i2000_port61_write(s, (uint8_t)val);
        return;
    }
    if ((port >= 0x170 && port <= 0x177) || port == 0x376) {
        ide_write(&s->ide_secondary, port == 0x376 ? 8 : (unsigned)(port - 0x170),
                  val, size);
        return;
    }
    if (s->hda && ((port >= 0x1F0 && port <= 0x1F7) || port == 0x3F6)) {
        ide_write(&s->ide_primary, port == 0x3F6 ? 8 : (unsigned)(port - 0x1F0),
                  val, size);
        return;
    }
    if (port >= COM1_PORT && port < COM1_PORT + 8) {
        ins8250_tick(&s->uart, 1);
        ins8250_write(&s->uart, (unsigned)(port - COM1_PORT), (uint8_t)val);
        return;
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

/* Deliver global system interrupt `gsi` if its redirection-table entry
 * isn't masked (SSDM Table 2-10). GSI N == legacy ISA IRQ N (no ACPI
 * interrupt source overrides modeled). Every entry defaults masked at
 * reset, so nothing fires until firmware programs it. */
static void iosapic_raise_gsi(Ia64I2000State *s, unsigned gsi) {
    if (gsi >= 64)
        return;
    uint64_t rte = s->chipset.iosapic_rte[gsi];
    if (getenv("IOSAPIC_DEBUG")) {
        static uint64_t n;
        if (n++ < 20 || (n % 5000) == 0)
            fprintf(stderr, "i2000: iosapic_raise_gsi(%u) rte=%016" PRIX64
                    " masked=%d picmask=%02x tpr=%02" PRIX64
                    " ninsts=%" PRIu64 "\n", gsi, rte,
                    !!(rte & I460GX_IOSAPIC_RTE_MASK), s->pic.master.imr,
                    s->cpu->cr[66] & 0xff, s->cpu->ninsts);
    }
    uint8_t vector;
    bool extint;
    if (!i460gx_interrupt(&s->chipset, gsi, &vector, &extint))
        return;
    if (extint) {
        /* ExtINT: routed to the external 8259A-compatible controller,
         * which supplies the vector via the legacy PIC's own base. */
        if (getenv("IOSAPIC_DEBUG"))
            fprintf(stderr, "i2000: iosapic delivering ExtINT vector=%#x "
                    "ninsts=%" PRIu64 "\n", s->pic.master.vector_base, s->cpu->ninsts);
        merced_raise_external(s->cpu, s->pic.master.vector_base);
        return;
    }
    /* Fixed SAPIC mode (000/001, the LSB is a redirection hint this model
     * doesn't act on): deliver the RTE's own vector directly. */
    if (getenv("IOSAPIC_DEBUG"))
        fprintf(stderr, "i2000: iosapic delivering fixed vector=%#x "
                "ninsts=%" PRIu64 "\n", (unsigned)vector, s->cpu->ninsts);
    merced_raise_external(s->cpu, vector);
}

/* SAL restores its top-of-RAM scratch words through sign-extended
 * addresses; map them onto the top of installed RAM. */
static inline uint64_t i2000_normalize_pa(Ia64I2000State *s, uint64_t addr) {
    addr &= MERCED_PHYS_MASK;
    if ((addr & UINT64_C(0x000fffff00000000)) ==
        UINT64_C(0x000fffff00000000)) {
        addr = ((s->ram_size - UINT64_C(0x10000)) & ~UINT64_C(0xffff)) |
               (addr & UINT64_C(0xffff));
    }
    return addr;
}

static uint64_t i2000_canonical_pa(void *ud, uint64_t addr) {
    return i2000_normalize_pa(ud, addr);
}

/* memcpy() with the access size spelled out, so the common widths compile
 * to a single move instead of a library call (this is every RAM access). */
static inline void i2000_ram_copy(const void *src, void *dst, unsigned size) {
    switch (size) {
    case 1: memcpy(dst, src, 1); break;
    case 2: memcpy(dst, src, 2); break;
    case 4: memcpy(dst, src, 4); break;
    case 8: memcpy(dst, src, 8); break;
    default: memcpy(dst, src, size); break;
    }
}

/* Whether a PCI card decodes `addr` as memory: the display's legacy VGA
 * window, and the frame buffer, register, flash and ROM BARs of the display
 * and the LAN. DRAM has priority over a BAR: until firmware places one above
 * memory (a BAR left at its power-on zero, or placed over RAM), it decodes
 * nothing. The chipset's own space (I/O and local SAPICs, flash) at the top
 * of the 4 GiB map has priority too. */
static bool i2000_card_claims(const Ia64I2000State *s, uint64_t addr) {
    uint64_t a, a_size, b, b_size, rom, rom_size;
    if (s->video_enabled && addr >= 0xA0000 && addr < 0xC0000)
        return rage128_mem_ranges(&s->rage128, &a, &a_size, &b, &b_size);
    if (addr < s->ram_size || addr >= I460GX_IOSAPIC_BASE)
        return false;
    if (pci_audio_mem_claims(&s->audio, addr)) return true;
    if (s->video_enabled) {
        if (rage128_mem_ranges(&s->rage128, &a, &a_size, &b, &b_size) &&
            (addr - a < a_size || addr - b < b_size))
            return true;
        if (rage128_rom_range(&s->rage128, &rom, &rom_size) &&
            addr - rom < rom_size)
            return true;
    }
    if (s->nic_enabled) {
        if (e100_mem_ranges(&s->nic, &a, &a_size, &b, &b_size) &&
            (addr - a < a_size || addr - b < b_size))
            return true;
        if (e100_rom_range(&s->nic, &rom, &rom_size) &&
            addr - rom < rom_size)
            return true;
    }
    return false;
}

/* Recompute the span of RAM that no device decodes. Devices take priority
 * over RAM in the bus decode, so the span ends at the lowest base address of
 * a PCI window that could fall in RAM. Call whenever such a BAR may have
 * changed. */
#define I2000_FAST_RAM_BASE UINT64_C(0x100000)

static void i2000_update_fast_ram(Ia64I2000State *s) {
    uint64_t end = s->ram_size;
    uint64_t a, a_size, b, b_size, rom, rom_size;
    if (s->video_enabled) {
        if (rage128_mem_ranges(&s->rage128, &a, &a_size, &b, &b_size)) {
            if (a_size && a >= s->ram_size && a < end) end = a;
            if (b_size && b >= s->ram_size && b < end) end = b;
        }
        if (rage128_rom_range(&s->rage128, &rom, &rom_size) &&
            rom >= s->ram_size && rom < end)
            end = rom;
    }
    if (s->nic_enabled) {
        if (e100_mem_ranges(&s->nic, &a, &a_size, &b, &b_size)) {
            if (a_size && a >= s->ram_size && a < end) end = a;
            if (b_size && b >= s->ram_size && b < end) end = b;
        }
        if (e100_rom_range(&s->nic, &rom, &rom_size) &&
            rom >= s->ram_size && rom < end)
            end = rom;
    }
    s->fast_ram_end = end;
    if (s->cpu)
        merced_direct_ram_changed(s->cpu);
}

/* An access wholly inside the plain-RAM span. The two scratch offsets are
 * excluded because SAL's top-of-aperture words there are tracked and
 * re-addressed by i2000_normalize_pa(). */
/* Below I2000_FAST_RAM_BASE only the legacy VGA window decodes to a device
 * (i2000_card_claims()); the rest of the first megabyte is RAM, which NT
 * uses heavily. */
static inline bool i2000_low_vga(const Ia64I2000State *s, uint64_t addr,
                                 uint64_t end) {
    return s->video_enabled && addr < 0xC0000 && end > 0xA0000;
}

static inline bool i2000_plain_ram(const Ia64I2000State *s, uint64_t addr,
                                   unsigned size) {
    if (addr < I2000_FAST_RAM_BASE)
        return addr + size <= I2000_FAST_RAM_BASE &&
               !i2000_low_vga(s, addr, addr + size) && !s->mem_watch &&
               (addr & UINT64_C(0xffff)) != UINT64_C(0x6030) &&
               (addr & UINT64_C(0xffff)) != UINT64_C(0xa030);
    return addr < s->fast_ram_end &&
           size <= s->fast_ram_end - addr && !s->mem_watch &&
           (addr & UINT64_C(0xffff)) != UINT64_C(0x6030) &&
           (addr & UINT64_C(0xffff)) != UINT64_C(0xa030);
}

/* The JIT's direct-RAM pages: those wholly inside the plain-RAM span, with
 * the SAL scratch offset i2000_plain_ram() excludes left to the bus. */
static uint8_t *i2000_direct_page(void *ud, uint64_t pa_page,
                                  uint16_t *slow_off) {
    Ia64I2000State *s = ud;
    if (s->mem_watch || pa_page + 0x1000 > s->fast_ram_end ||
        (pa_page < I2000_FAST_RAM_BASE &&
         i2000_low_vga(s, pa_page, pa_page + 0x1000)))
        return NULL;
    unsigned off16 = (unsigned)(pa_page & 0xF000);
    *slow_off = (off16 == 0x6000 || off16 == 0xA000) ? 0x030 : 0xFFFF;
    return s->ram + pa_page;
}

static uint64_t bus_read(void *ud, uint64_t addr, unsigned size) {
    Ia64I2000State *s = ud;
    if (i2000_plain_ram(s, addr, size)) {
        uint64_t v = 0;
        i2000_ram_copy(s->ram + addr, &v, size);
        return v;
    }
    if (gemu_monitor_has_mem_breakpoints(s->monitor))
        for (unsigned i = 0; i < size; i++)
            gemu_monitor_check_read(s->monitor, (uint32_t)(addr + i));
    /* IA-64 region-6 uncached aliases carry 0xC in the top nibble.  The
     * chipset sees the physical address after those region bits are
     * stripped; retaining them made SDV's MMIO PCI configuration cycles
     * miss every device. */
    bool region6 = (addr >> 60) == 0xC;
    addr = i2000_normalize_pa(s, addr);
    if (i2000_card_claims(s, addr)) {
        uint64_t v;
        if ((s->video_enabled &&
             rage128_mem_access(&s->rage128, false, addr, size, &v)) ||
            (s->nic_enabled &&
             e100_mem_access(&s->nic, false, addr, size, &v)) ||
            pci_audio_mem_access(&s->audio, false, addr, size, &v))
            return v;
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
        i2000_ram_copy(s->ram + addr, &v, size);
        return v;
    }
    uint64_t chipset_value;
    if (i460gx_mmio_read(&s->chipset, addr, size, &chipset_value))
        return chipset_value;
    /* Legacy ExtINT acknowledge: IA-64 has no INTA pin, so SAL substitutes
     * a memory-mapped byte read to learn which 8259-relative IRQ won
     * arbitration - the same information a real INTA cycle would return
     * on the data bus (ICW2 base + IRQ number).  The 8259 pair arbitrates
     * (IRQ0 outranks the slave's IRQs on IRQ2) and marks the winner in
     * service; the device behind it then does what its acknowledge
     * implies. */
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
        i2000_pic_sync(s);
        int irq;
        uint8_t vector = i8259_pair_ack(&s->pic, &irq);
        switch (irq) {
        case 0:
            s->pit_irq_pending = false;
            /* merced_ack_external() just cleared the ONE shared CPU-level
             * pending bit for vector 0, which both IRQ0 and IRQ12 share.
             * If IRQ12 is also still latched, don't re-raise synchronously
             * here - see the mouse branch below for why that corrupts the
             * guest ISR. Just leave mouse_irq_pending set with its retry
             * deadline (already scheduled by whichever aux_queue_byte()/ack
             * call set it) and let i2000_poll_interrupts() fire it once
             * that deadline passes. */
            break;
        case 1:
            s->keyboard_irq_pending = false;
            /* The SDV thunk performs several native/IA-32 rfi transitions
             * while servicing one byte, so the global rfi counter cannot
             * identify completion of this particular IRQ1.  Keep the next
             * byte quiet long enough for the thunk and BIOS handler to
             * unwind before another shared ExtINT can overwrite cr.iip and
             * cr.ipsr. */
            s->keyboard_retry_ninsts =
                s->cpu->ninsts + KEYBOARD_RETRY_DELAY_INSTS;
            break;
        case 12:
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
            break;
        case 14:
            if (!s->ata_legacy_irq_latched)
                break;
            s->ata_legacy_irq_latched = false;
            ata_disk_clear_irq(&s->hd);
            if (getenv("ATA_DEBUG"))
                fprintf(stderr, "i2000: ATA IRQ14 acknowledged vector=%#04x "
                        "ninsts=%" PRIu64 "\n",
                        (unsigned)vector, s->cpu->ninsts);
            break;
        case 15:
            if (!s->atapi_legacy_irq_latched)
                break;
            s->atapi_legacy_irq_latched = false;
            atapi_cdrom_clear_irq(&s->cd);
            if (atapi_debug_enabled()) {
                fprintf(stderr, "i2000: ATAPI IRQ15 acknowledged vector=%#04x "
                        "ip=%016" PRIx64 " itc=%" PRIu64 " ninsts=%" PRIu64 "\n",
                        (unsigned)vector, s->cpu->ip,
                        merced_get_itc(s->cpu), s->cpu->ninsts);
                if (getenv("ATA_IRQ_TRACE")) {
                    fprintf(stderr, "i2000: ATAPI IRQ15 ack call history:\n");
                    merced_dump_calls(s->cpu, 24, stderr);
                    s->cpu->trace_n = 4000;
                }
            }
            break;
        default:   /* -1: spurious, IRQ7's vector */
            break;
        }
        i2000_pic_sync(s);
        return vector;
    }
    {
        /* The firmware window and the hubs' register space are the IFB's
         * firmware hub interface: reads see the array, status, identifier or
         * lock registers as each hub's command state says. */
        uint64_t v;
        if (i460gx_flash_access(&s->chipset, false, addr, size, &v))
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
    if (i2000_plain_ram(s, addr, size)) {
        i2000_ram_copy(&val, s->ram + addr, size);
        return;
    }
    for (unsigned i = 0; gemu_monitor_has_mem_breakpoints(s->monitor) && i < size; i++) {
        if (gemu_monitor_check_write(s->monitor, (uint32_t)(addr + i)))
            fprintf(stderr, "i2000: watched write pa=%016" PRIX64
                    " size=%u val=%016" PRIX64 " ip=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", addr, size, val,
                    s->cpu->ip, s->cpu->ninsts);
    }
    bool region6 = (addr >> 60) == 0xC;
    addr = i2000_normalize_pa(s, addr);
    if (i2000_card_claims(s, addr) &&
        ((s->video_enabled &&
          rage128_mem_access(&s->rage128, true, addr, size, &val)) ||
         (s->nic_enabled &&
          e100_mem_access(&s->nic, true, addr, size, &val)) ||
         pci_audio_mem_access(&s->audio, true, addr, size, &val)))
        return;
    if (region6 && addr + size <= I2000_FLASH_BASE) {
        uint32_t saved = s->pci_cfg_addr;
        s->pci_cfg_addr = 0x80000000u | ((uint32_t)addr & 0xfffffffcu);
        pci_cfg_write(s, (unsigned)(addr & 3), val, size);
        s->pci_cfg_addr = saved;
        return;
    }
    if (addr + size <= s->ram_size) {
        i2000_ram_copy(&val, s->ram + addr, size);
        return;
    }
    if (i460gx_mmio_write(&s->chipset, addr, val, size))
        return;
    if (addr - I2000_FLASH_BASE < I2000_FLASH_SIZE) {
        uint64_t off = addr - I2000_FLASH_BASE;
        if (size == 1) {
            /* Byte cycles are firmware hub command and data cycles. What the
             * firmware programs or erases is mirrored into the RAM shadow so
             * its read-back through the low alias stays coherent. */
            i460gx_flash_access(&s->chipset, true, addr, size, &val);
            uint32_t lo, n;
            while (i460gx_flash_take_changes(&s->chipset, &lo, &n)) {
                if (s->fw_shadow_enabled &&
                    I2000_FW_SHADOW_BASE + (uint64_t)lo + n <= s->ram_size)
                    memcpy(s->ram + I2000_FW_SHADOW_BASE + lo, s->flash + lo, n);
                s->nvram_dirty = true;
            }
            return;
        }
        /* Wider stores are not flash cycles: they are the firmware's cacheable
         * data stores into the window, which a processor keeps in its cache.
         * Until the caches are modelled they land in the RAM shadow. */
        if (s->fw_shadow_enabled && off + size <= I2000_FLASH_SIZE &&
            I2000_FW_SHADOW_BASE + I2000_FLASH_SIZE <= s->ram_size)
            memcpy(s->ram + I2000_FW_SHADOW_BASE + off, &val, size);
        return;
    }
    /* The hubs' register space, 4 MiB below the window. */
    if (i460gx_flash_access(&s->chipset, true, addr, size, &val))
        return;
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
    mmio_log(s, addr, val, size, true);
}

uint8_t ia64_i2000_phys_read8(Ia64I2000State *s, uint64_t addr) {
    return (uint8_t)bus_read(s, addr & MERCED_PHYS_MASK, 1);
}
void ia64_i2000_phys_write8(Ia64I2000State *s, uint64_t addr, uint8_t val) {
    bus_write(s, addr & MERCED_PHYS_MASK, val, 1);
}
uint32_t ia64_i2000_phys_read32(Ia64I2000State *s, uint64_t addr) {
    return (uint32_t)bus_read(s, addr & MERCED_PHYS_MASK, 4);
}
void ia64_i2000_phys_write32(Ia64I2000State *s, uint64_t addr, uint32_t val) {
    bus_write(s, addr & MERCED_PHYS_MASK, val, 4);
}

static bool i2000_bus_fill(void *ud, uint64_t addr, uint8_t val, uint64_t len) {
    Ia64I2000State *s = ud;
    if (addr > s->ram_size || len > s->ram_size - addr)
        return false;
    memset(s->ram + addr, val, (size_t)len);
    return true;
}

/* ── Firmware ────────────────────────────────────────────────────────────── */

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
    /* The file is the flash image, then the RTC's battery-backed RAM and the
     * offset of its time from the host's. */
    uint8_t *image = malloc(I2000_FLASH_SIZE);
    size_t got = image ? fread(image, 1, I2000_FLASH_SIZE, f) : 0;
    uint8_t rtc_ram[MC146818_NV_SIZE];
    uint8_t rtc_offset[8];
    bool has_rtc = got == I2000_FLASH_SIZE &&
                   fread(rtc_ram, 1, sizeof(rtc_ram), f) == sizeof(rtc_ram) &&
                   fread(rtc_offset, 1, sizeof(rtc_offset), f) == sizeof(rtc_offset);
    int extra = fgetc(f);
    fclose(f);
    if (got != I2000_FLASH_SIZE || extra != EOF) {
        fprintf(stderr, "i2000: ignoring malformed NVRAM '%s'\n",
                s->nvram_path);
        free(image);
        return;
    }
    memcpy(s->flash, image, I2000_FLASH_SIZE);
    free(image);
    if (has_rtc) {
        /* The clock kept its offset from the host while the machine was off. */
        int64_t offset = 0;
        for (unsigned i = 0; i < 8; i++)
            offset |= (int64_t)rtc_offset[i] << (8 * i);
        memcpy(s->rtc.nv, rtc_ram, sizeof(rtc_ram));
        s->rtc.battery = true;
        mc146818_power_up(&s->rtc);
        rtc_set_naive_seconds(s, host_naive_seconds() + offset);
        s->rtc.dirty = false;
    }
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
                   I2000_FLASH_SIZE;
    if (ok) {
        int64_t offset = rtc_naive_seconds(s) - host_naive_seconds();
        uint8_t trailer[8];
        for (unsigned i = 0; i < 8; i++)
            trailer[i] = (uint8_t)((uint64_t)offset >> (8 * i));
        ok = fwrite(s->rtc.nv, 1, sizeof(s->rtc.nv), f) == sizeof(s->rtc.nv) &&
             fwrite(trailer, 1, sizeof(trailer), f) == sizeof(trailer) &&
             fflush(f) == 0;
    }
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

/* The display card runs on the machine's own time base, so its raster
 * advances exactly as fast as the guest does. */
static uint64_t i2000_rage_clock(void *opaque) {
    Ia64I2000State *s = opaque;
    return s->cpu ? merced_get_time_base(s->cpu) : 0;
}

/* The Rage 128 (00:02.0) INTA# is GSI 39: the firmware's PCI0 _PRT routes
 * device 2 pins A-D to GSIs 39-36 (and device 5's INTA#, the LAN, to 44 -
 * see i2000_nic_irq()).  GSI 17 was a slot on another bus; Windows connects
 * the display driver's interrupt where _PRT says. */
static void i2000_rage_irq(void *opaque, bool level) {
    Ia64I2000State *s = opaque;
    if (level)
        iosapic_raise_gsi(s, 39);
}

/* The onboard LAN is a bus master: its descriptors and buffers are read and
 * written through the same physical decode as the processor's accesses. */
static bool i2000_nic_dma_read(void *opaque, uint64_t addr, void *buf,
                               unsigned len) {
    uint8_t *p = buf;
    while (len) {
        unsigned n = len >= 8 ? 8 : len >= 4 ? 4 : len >= 2 ? 2 : 1;
        uint64_t v = bus_read(opaque, addr, n);
        memcpy(p, &v, n);
        p += n;
        addr += n;
        len -= n;
    }
    return true;
}

static bool i2000_nic_dma_write(void *opaque, uint64_t addr, const void *buf,
                                unsigned len) {
    const uint8_t *p = buf;
    while (len) {
        unsigned n = len >= 8 ? 8 : len >= 4 ? 4 : len >= 2 ? 2 : 1;
        uint64_t v = 0;
        memcpy(&v, p, n);
        bus_write(opaque, addr, v, n);
        p += n;
        addr += n;
        len -= n;
    }
    return true;
}

static void i2000_nic_irq(void *opaque, bool level) {
    if (level)
        iosapic_raise_gsi(opaque, 44);
}

/* The host side of the LAN (gemu/net.h).  File-static: Ia64I2000State is
 * snapshotted raw. */
static GemuNic *i2000_net;

static bool i2000_nic_tx(void *opaque, const uint8_t *frame, size_t len) {
    (void)opaque;
    return i2000_net ? gemu_nic_send(i2000_net, frame, len) : true;
}

static size_t i2000_nic_rx(void *opaque, uint8_t *frame, size_t cap) {
    (void)opaque;
    return i2000_net ? gemu_nic_recv(i2000_net, frame, cap) : 0;
}

/* Monitor "set_link": plug or unplug the cable. */
static void i2000_nic_link(void *opaque, bool up) {
    Ia64I2000State *s = opaque;
    if (s->nic_enabled)
        e100_set_link(&s->nic, up);
}

static E100Wiring i2000_nic_wiring(Ia64I2000State *s) {
    return (E100Wiring){ .clock = i2000_rage_clock, .clock_hz = I2000_ITC_HZ,
                         .irq = i2000_nic_irq,
                         .dma_read = i2000_nic_dma_read,
                         .dma_write = i2000_nic_dma_write,
                         .tx = i2000_nic_tx, .rx = i2000_nic_rx, .opaque = s };
}

/* PCI0._PRT's workstation branch routes 00:04 INTA# to GSI 45.
 * The CS4281 is a separate PCI function, not an IFB audio function. */
static void i2000_audio_irq(void *opaque, bool level) {
    if (level) iosapic_raise_gsi(opaque, 45);
}
static bool i2000_audio_read(void *opaque, uint64_t addr, void *buf, unsigned len) {
    Ia64I2000State *s = opaque;
    return addr <= s->ram_size && len <= s->ram_size - addr &&
           i2000_nic_dma_read(s, addr, buf, len);
}
static bool i2000_audio_write(void *opaque, uint64_t addr, const void *buf, unsigned len) {
    Ia64I2000State *s = opaque;
    return addr <= s->ram_size && len <= s->ram_size - addr &&
           i2000_nic_dma_write(s, addr, buf, len);
}
static void i2000_audio_output(void *opaque, const float *samples, size_t frames) {
    Ia64I2000State *s = opaque;
    gemu_audio_queue(s->audio_out, samples, frames * 2);
}
static PciAudioWiring i2000_audio_wiring(Ia64I2000State *s) {
    return (PciAudioWiring){.opaque = s, .clock = i2000_rage_clock,
        .clock_hz = I2000_ITC_HZ, .read = i2000_audio_read,
        .write = i2000_audio_write, .irq = i2000_audio_irq,
        .output = i2000_audio_output};
}

static Rage128Wiring i2000_rage_wiring(Ia64I2000State *s) {
    return (Rage128Wiring){ .clock = i2000_rage_clock,
                            .clock_hz = I2000_ITC_HZ,
                            .irq = i2000_rage_irq, .opaque = s };
}

/* Graphics DMA addresses are PCI physical addresses, not CPU region/SAL
 * aliases. Restrict the bus master to RAM; an invalid GART entry must not
 * recursively access the card's own MMIO registers. */
static bool i2000_rage_dma_read(void *opaque, uint64_t addr, void *buf, unsigned len) {
    Ia64I2000State *s = opaque;
    if (addr >= s->ram_size || len > s->ram_size - addr ||
        i2000_card_claims(s, addr)) return false;
    memcpy(buf, s->ram + addr, len);
    return true;
}

static bool i2000_rage_dma_write(void *opaque, uint64_t addr, const void *buf,
                                  unsigned len) {
    Ia64I2000State *s = opaque;
    if (addr >= s->ram_size || len > s->ram_size - addr ||
        i2000_card_claims(s, addr)) return false;
    const uint8_t *p = buf;
    for (unsigned i = 0; i < len; i++)
        merced_bus_write(s->cpu, addr + i, p[i], 1);
    return true;
}

static void i2000_rage_dma_poll(Ia64I2000State *s) {
    if (!s->video_enabled) return;
    const Rage128Dma dma = {i2000_rage_dma_read, i2000_rage_dma_write, s};
    rage128_cce_tick(&s->rage128, &dma);
}

/* Install the card's video BIOS image (a PCI expansion ROM, at most 128 KiB). */
bool ia64_i2000_load_video_rom(Ia64I2000State *s, const char *path) {
    if (!s->video_enabled)
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t *rom = malloc(RAGE128_ROM_BAR_SIZE);
    size_t len = rom ? fread(rom, 1, RAGE128_ROM_BAR_SIZE, f) : 0;
    bool ok = rom && len >= 0x1c && rom[0] == 0x55 && rom[1] == 0xaa &&
              fgetc(f) == EOF;
    fclose(f);
    if (!ok) {
        fprintf(stderr, "gemu: '%s' is not a video BIOS image\n", path);
        free(rom);
        return false;
    }
    free(s->video_rom);
    s->video_rom = rom;
    s->video_rom_len = (uint32_t)len;
    rage128_set_rom(&s->rage128, rom, (uint32_t)len);
    return true;
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


/* The size of the picture the monitor shows: the card's current mode (kept
 * out of Ia64I2000State, which snapshots copy raw). */
static uint32_t i2000_pic_w = 640, i2000_pic_h = 480;

/* The card's native scan-out when its CRTC is programmed for one, otherwise
 * the VGA core's picture.  Leaves it in s->fb packed at its own size,
 * i2000_pic_w x i2000_pic_h, which follows the mode the way a monitor
 * would; with no signal the last size stays, showing black. */
static void i2000_render_frame(Ia64I2000State *s) {
    uint32_t w = 0, h = 0, y0, y1;
    bool shown = false;
    if (s->video_enabled) {
        rage128_tick(&s->rage128);
        shown = rage128_scanout(&s->rage128, s->fb, FB_W, FB_H, &w, &h,
                                true, &y0, &y1) ||
                vga_blit_ex(&s->vga, s->fb, FB_W, FB_H, 640, 480, 1, &w, &h,
                            true, &y0, &y1);
    }
    if (!shown || !w || !h) {
        memset(s->fb, 0, sizeof(s->fb));
        return;
    }
    if (w > FB_W) w = FB_W;
    if (h > FB_H) h = FB_H;
    i2000_pic_w = w;
    i2000_pic_h = h;
    /* rows were drawn FB_W apart; pack them (each moves to a lower address) */
    if (w != FB_W)
        for (uint32_t y = 1; y < h; y++)
            memmove(&s->fb[y * w], &s->fb[y * FB_W], w * sizeof(s->fb[0]));
}

static bool i2000_screendump(void *ud, const char *path) {
    Ia64I2000State *s = ud;
    i2000_render_frame(s);
    return gemu_screendump_argb(path, s->fb, (int)i2000_pic_w, (int)i2000_pic_h);
}

/* The SDV EFI presentation environment leaves cr.iva at zero and obtains
 * console keys through its CSM/INT16-compatible BIOS data-area ring.  On
 * real hardware the legacy IRQ1 thunk transfers 8042 scan codes into this
 * buffer.  GEMU deliberately suppresses interrupts while IVA is zero (the
 * native vector at 0x3000 is empty), so complete that transfer here for host
 * input while execution is in the firmware presentation code in the top
 * 16 MiB of installed RAM.
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

/* INT16 scan code for a non-ASCII X11 keysym (arrows, editing and
 * function keys); these enter the BIOS ring with ASCII 0. */
static uint8_t i2000_bios_ext_scan(uint32_t keysym) {
    switch (keysym) {
    case 0xff52: return 0x48;   /* up */
    case 0xff54: return 0x50;   /* down */
    case 0xff51: return 0x4b;   /* left */
    case 0xff53: return 0x4d;   /* right */
    case 0xff50: return 0x47;   /* home */
    case 0xff57: return 0x4f;   /* end */
    case 0xff55: return 0x49;   /* page up */
    case 0xff56: return 0x51;   /* page down */
    case 0xff63: return 0x52;   /* insert */
    case 0xffff: return 0x53;   /* delete */
    case 0xffc8: return 0x85;   /* F11 */
    case 0xffc9: return 0x86;   /* F12 */
    default:
        if (keysym >= 0xffbe && keysym <= 0xffc7)   /* F1-F10 */
            return (uint8_t)(0x3b + (keysym - 0xffbe));
        return 0;
    }
}

/* Append one (ascii, scan) entry to the BIOS keyboard ring, while the
 * firmware presentation environment is the one reading it. */
static void i2000_efi_bios_ring_put(Ia64I2000State *s, uint8_t ascii,
                                    uint8_t scan) {
    const uint64_t firmware_window = UINT64_C(16) * 1024 * 1024;
    if ((!ascii && !scan) || s->ram_size < firmware_window ||
        s->cpu->cr[2] != 0 ||
        s->cpu->ip < s->ram_size - firmware_window ||
        s->cpu->ip >= s->ram_size)
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
    s->ram[0x401 + tail] = scan;
    s->ram[0x41c] = (uint8_t)next;
    s->ram[0x41d] = (uint8_t)(next >> 8);
}

static void i2000_efi_bios_key(Ia64I2000State *s, uint8_t ascii) {
    if (ascii)
        i2000_efi_bios_ring_put(s, ascii, i2000_bios_scan(ascii));
}

/* Non-ASCII keys (arrows, F-keys...) for the same environment. */
static void i2000_efi_bios_ext_key(Ia64I2000State *s, uint32_t keysym) {
    i2000_efi_bios_ring_put(s, 0, i2000_bios_ext_scan(keysym));
}

/* ── Drives ────────────────────────────────────────────────────────────────
 *
 * Fixed CMD-649 wiring, matching QEMU's PC naming: primary channel master
 * = ATA hard disk (ide0-hd0, -hda, index=0), secondary channel master =
 * ATAPI CD-ROM (ide1-cd0, -cdrom, index=2).  Slaves and any other
 * interface stay unclaimed, so gemu_drive_check_unclaimed() rejects them. */

/* Mirror the drives' current medium into the informational file/size
 * fields (savevm still carries them, and 'atapistate'-style debugging
 * reads them); device I/O never uses them. */
static void i2000_drive_info_refresh(Ia64I2000State *s) {
    snprintf(s->cdrom_file, sizeof(s->cdrom_file), "%s",
             gemu_drive_filename(s->cdrom));
    s->cdrom_size = gemu_drive_size(s->cdrom);
    snprintf(s->hda_file, sizeof(s->hda_file), "%s",
             gemu_drive_filename(s->hda));
    s->hda_size = gemu_drive_size(s->hda);
}

/* Monitor change/eject on ide1-cd0.  The ATAPI model has no unit-attention
 * state; it reads the medium's presence and size on every packet, so a
 * swap is picked up by the next TEST UNIT READY / READ CAPACITY. */
static void i2000_cdrom_changed(void *ud, bool inserted) {
    Ia64I2000State *s = ud;
    i2000_drive_info_refresh(s);
    if (inserted && (s->cdrom_size == 0 || s->cdrom_size % 2048 != 0))
        fprintf(stderr, "i2000: warning: CD-ROM image '%s' is not a "
                "sector-aligned ISO; the partial last sector is unreadable\n",
                s->cdrom_file);
    if (atapi_debug_enabled())
        fprintf(stderr, "i2000: ATAPI media %s '%s' bytes=%" PRIu64 "\n",
                inserted ? "inserted" : "ejected", s->cdrom_file,
                s->cdrom_size);
}

static bool i2000_claim_drives(Ia64I2000State *s) {
    s->hda = gemu_drive_get(GEMU_DRIVE_IF_IDE, 0, 0);
    s->cdrom = gemu_drive_get(GEMU_DRIVE_IF_IDE, 1, 0);
    if (s->hda && gemu_drive_media(s->hda) != GEMU_DRIVE_MEDIA_DISK) {
        fprintf(stderr, "gemu: i2000: %s: the primary IDE channel has a hard "
                "disk, not a CD-ROM (use -cdrom or index=2)\n",
                gemu_drive_name(s->hda));
        return false;
    }
    if (s->cdrom && gemu_drive_media(s->cdrom) != GEMU_DRIVE_MEDIA_CDROM) {
        fprintf(stderr, "gemu: i2000: %s: the secondary IDE channel has a "
                "CD-ROM, not a hard disk (use -hda or index=0)\n",
                gemu_drive_name(s->cdrom));
        return false;
    }
    if (s->hda) {
        uint64_t size = gemu_drive_size(s->hda);
        if (size == 0 || size % 512 != 0) {
            fprintf(stderr, "gemu: HDD image '%s' is not a sector-aligned "
                    "(512-byte) raw disk image\n", gemu_drive_filename(s->hda));
            return false;
        }
    }
    if (gemu_drive_inserted(s->cdrom)) {
        uint64_t size = gemu_drive_size(s->cdrom);
        if (size == 0 || size % 2048 != 0) {
            fprintf(stderr, "gemu: CD-ROM image '%s' is not a sector-aligned ISO\n",
                    gemu_drive_filename(s->cdrom));
            return false;
        }
    }
    gemu_drive_set_change_cb(s->cdrom, i2000_cdrom_changed, s);
    ata_disk_init(&s->hd, &(AtaDiskConfig){0});
    atapi_cdrom_init(&s->cd, &(AtapiCdromConfig){0});
    i2000_ide_wire(s);
    i2000_drive_info_refresh(s);
    if (atapi_debug_enabled() && gemu_drive_inserted(s->cdrom))
        fprintf(stderr, "i2000: ATAPI media attached '%s' bytes=%" PRIu64
                " sectors=%" PRIu64 "\n", s->cdrom_file, s->cdrom_size,
                s->cdrom_size / 2048);
    return true;
}

/* Snapshot format: buffer contents followed by the current machine and CPU
 * structures. Host pointers and chipset wiring are cleared on save and
 * restored from the live instance on load. The raw structure format is
 * host-ABI-dependent; changes to its layout require a new version.
 * Version 31 separates the 460GX state and intentionally rejects old saves. */
#define I2000_SNAPSHOT_MAGIC 0x32304B32554D4547ull /* "GEMU2K02" */
#define I2000_SNAPSHOT_VERSION 45u   /* the CD-ROM as an AtapiCdrom */

/* Version 40 ends Rage128 just before cce. Its following machine fields
 * keep their relative layout because the inserted state is 8-byte aligned. */
_Static_assert(sizeof(Rage128Cce) % 8 == 0, "CCE snapshot insertion alignment");
_Static_assert(offsetof(Rage128, cce) + sizeof(Rage128Cce) == sizeof(Rage128),
               "CCE must remain the final Rage128 field for v40 imports");

static bool i2000_save_snapshot(Ia64I2000State *s, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = true;
    uint64_t magic = I2000_SNAPSHOT_MAGIC;
    uint32_t version = I2000_SNAPSHOT_VERSION;
    ok &= fwrite(&magic, sizeof(magic), 1, f) == 1;
    ok &= fwrite(&version, sizeof(version), 1, f) == 1;
    ok &= fwrite(&s->ram_size, sizeof(s->ram_size), 1, f) == 1;
    uint32_t audio_model = s->audio.model;
    ok &= fwrite(&audio_model, sizeof(audio_model), 1, f) == 1;
    ok &= fwrite(s->ram, 1, s->ram_size, f) == s->ram_size;
    ok &= fwrite(s->flash, 1, I2000_FLASH_SIZE, f) == I2000_FLASH_SIZE;
    if (s->video_enabled)
        ok &= fwrite(s->vram, 1, RAGE128_VRAM_SIZE, f) == RAGE128_VRAM_SIZE;
    uint64_t atapi_len = (uint64_t)s->cd.data_len;
    ok &= fwrite(&atapi_len, sizeof(atapi_len), 1, f) == 1;
    if (atapi_len)
        ok &= fwrite(s->cd.data, 1, atapi_len, f) == atapi_len;

    Ia64I2000State *snap = malloc(sizeof(*snap));
    if (!snap) { fclose(f); return false; }
    *snap = *s;
    snap->monitor = NULL;
    snap->display = NULL;
    snap->cpu = NULL;
    snap->ram = NULL;
    snap->flash = NULL;
    snap->vram = NULL;
    snap->video_rom = NULL;
    snap->cdrom = NULL;
    snap->cd.data = NULL;
    memset(&snap->cd.cfg, 0, sizeof(snap->cd.cfg));
    memset(&snap->cd.ide, 0, sizeof(snap->cd.ide));
    memset(snap->ide_secondary.dev, 0, sizeof(snap->ide_secondary.dev));
    snap->hda = NULL;
    memset(&snap->hd.cfg, 0, sizeof(snap->hd.cfg));
    memset(&snap->hd.ide, 0, sizeof(snap->hd.ide));
    memset(snap->ide_primary.dev, 0, sizeof(snap->ide_primary.dev));
    snap->nic_rom = NULL;
    snap->audio_out = NULL;
    memset(&snap->audio.wiring, 0, sizeof(snap->audio.wiring));
    memset(&snap->nic.wiring, 0, sizeof(snap->nic.wiring));
    memset(&snap->chipset.config, 0, sizeof(snap->chipset.config));
    memset(&snap->rtc.config, 0, sizeof(snap->rtc.config));
    /* The I2C segments hold pointers into this run's device state. */
    memset(&snap->memcard_i2c, 0, sizeof(snap->memcard_i2c));
    memset(&snap->smbus, 0, sizeof(snap->smbus));
    ok &= fwrite(snap, sizeof(*snap), 1, f) == 1;
    free(snap);

    /* Merced is ~2 MB: on the heap, as Windows threads have a 1 MiB stack. */
    Merced *cpu_snap = malloc(sizeof(*cpu_snap));
    if (!cpu_snap) { fclose(f); return false; }
    *cpu_snap = *s->cpu;
    memset(&cpu_snap->bus, 0, sizeof(cpu_snap->bus));
    cpu_snap->translation_cache = NULL;
    ok &= fwrite(cpu_snap, sizeof(*cpu_snap), 1, f) == 1;
    free(cpu_snap);
    /* The disks as of this moment: -snapshot writes live only in memory, so
     * without them a restored guest would lose its own disk changes. */
    ok &= gemu_drive_save_state(s->hda, f);
    ok &= gemu_drive_save_state(s->cdrom, f);

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
        version != I2000_SNAPSHOT_VERSION || ram_size != s->ram_size) {
        fclose(f);
        return false;
    }
    uint32_t audio_model = 0;
    if (fread(&audio_model, sizeof(audio_model), 1, f) != 1 || audio_model != s->audio.model) {
        fclose(f);
        fprintf(stderr, "gemu: snapshot sound card differs from -soundhw selection\n");
        return false;
    }
    ok &= fread(s->ram, 1, s->ram_size, f) == s->ram_size;
    ok &= fread(s->flash, 1, I2000_FLASH_SIZE, f) == I2000_FLASH_SIZE;
    if (s->video_enabled)
        ok &= fread(s->vram, 1, RAGE128_VRAM_SIZE, f) == RAGE128_VRAM_SIZE;

    uint64_t atapi_len = 0;
    ok &= fread(&atapi_len, sizeof(atapi_len), 1, f) == 1;
    atapi_cdrom_free(&s->cd);
    s->cd.data = atapi_len ? malloc((size_t)atapi_len) : NULL;
    if (atapi_len) {
        if (!s->cd.data) { fclose(f); return false; }
        ok &= fread(s->cd.data, 1, atapi_len, f) == atapi_len;
    }

    GemuMonitor *monitor = s->monitor;
    GemuDisplay *display = s->display;
    GemuAudioOut *audio_out = s->audio_out;
    bool audio_debug = s->audio.debug;
    /* Drives and host device connections belong to this run, not the save. */
    GemuDrive *cdrom = s->cdrom;
    GemuDrive *hda = s->hda;
    I460GXConfig chipset_config = s->chipset.config;
    Mc146818Config rtc_config = s->rtc.config;
    I2cBus memcard_i2c = s->memcard_i2c;
    I2cBus smbus = s->smbus;
    uint8_t *nic_rom = s->nic_rom;
    uint32_t nic_rom_len = s->nic_rom_len;
    Merced *cpu = s->cpu;
    uint8_t *ram = s->ram;
    uint8_t *flash = s->flash;
    uint8_t *vram = s->vram;
    uint8_t *video_rom = s->video_rom;
    uint32_t video_rom_len = s->video_rom_len;
    uint8_t *atapi_data = s->cd.data;
    Ia64I2000State *loaded = calloc(1, sizeof(*loaded));
    /* Merced is ~2 MB: on the heap, as Windows threads have a 1 MiB stack. */
    Merced *loaded_cpu = malloc(sizeof(*loaded_cpu));
    if (!loaded || !loaded_cpu) {
        free(loaded);
        free(loaded_cpu);
        fclose(f);
        return false;
    }
    ok &= fread(loaded, sizeof(*loaded), 1, f) == 1;
    if (ok) {
        char nvram_path[sizeof(s->nvram_path)];
        snprintf(nvram_path, sizeof(nvram_path), "%s", s->nvram_path);
        *s = *loaded;
        s->monitor = monitor;
        s->display = display;
        s->audio_out = audio_out;
        s->audio.wiring = i2000_audio_wiring(s);
        s->audio.debug = audio_debug;
        gemu_audio_flush(audio_out);
        s->cpu = cpu;
        s->ram = ram;
        s->flash = flash;
        s->vram = vram;
        s->video_rom = video_rom;
        s->video_rom_len = video_rom_len;
        s->chipset.config = chipset_config;
        s->rtc.config = rtc_config;
        s->memcard_i2c = memcard_i2c;
        s->smbus = smbus;
        snprintf(s->nvram_path, sizeof(s->nvram_path), "%s", nvram_path);
        /* Restore must not implicitly overwrite persistent machine NVRAM.
         * Subsequent guest flash writes mark it dirty normally. */
        s->nvram_dirty = false;
        s->cd.data = atapi_data;
        s->cdrom = cdrom;
        s->hda = hda;
        i2000_ide_wire(s);
        i2000_drive_info_refresh(s);
        i2000_update_fast_ram(s);
        /* A halt checkpoint can be resumed to test the faulting operation. */
        s->halted = false;
        s->nic_rom = nic_rom;
        s->nic_rom_len = nic_rom_len;
        if (s->nic_enabled) {
            E100Wiring wiring = i2000_nic_wiring(s);
            e100_rebind(&s->nic, &wiring);
            e100_set_rom(&s->nic, s->nic_rom, s->nic_rom_len);
            /* The cable is this run's (set_link), not the snapshot's. */
            e100_set_link(&s->nic, !i2000_net || gemu_nic_link_up(i2000_net));
            if (!i2000_net)
                fprintf(stderr, "gemu: note: the snapshot has the onboard LAN but "
                        "this run has -nic none; it sees no network\n");
        }
        if (s->video_enabled) {
            Rage128Wiring wiring = i2000_rage_wiring(s);
            vga_attach_vram(&s->vga, s->vram, RAGE128_VRAM_SIZE);
            rage128_rebind(&s->rage128, &s->vga, &wiring);
            rage128_set_rom(&s->rage128, s->video_rom, s->video_rom_len);
        }
    }
    free(loaded);

    MercedBus bus = s->cpu->bus;
    void *translation_cache = s->cpu->translation_cache;
    ok &= fread(loaded_cpu, sizeof(*loaded_cpu), 1, f) == 1;
    if (ok) {
        *s->cpu = *loaded_cpu;
        s->cpu->bus = bus;
        s->cpu->translation_cache = translation_cache;
        merced_flush_translation_cache(s->cpu);
        /* Interrupt-return generations and host input deadlines are local
         * to the current run rather than portable snapshot values. */
        s->mouse_wait_rfi_generation = 0;
        s->keyboard_retry_ninsts = s->cpu->ninsts;
        s->mouse_next_report_itc = merced_get_time_base(s->cpu);
        s->pit.out_changed = i2000_pit_out;
        s->pit.ud = s;
        i2000_uart_wire(s);
        s->pit_itc_mark = merced_get_time_base(s->cpu);
    }
    free(loaded_cpu);
    if (ok && version >= 39) {
        ok &= gemu_drive_load_state(s->hda, f);
        ok &= gemu_drive_load_state(s->cdrom, f);
        i2000_drive_info_refresh(s);
    } else if (ok)
        fprintf(stderr, "gemu: loadvm: this older state has no disk information; it only"
                        " works with the disk exactly as it was when it was saved\n");

    fclose(f);
    return ok;
}

/* ── Monitor memory view ─────────────────────────────────────────────────── */

/* Side-effect-free physical memory access for the monitor's x/xp/peek/poke/
 * memsave and for "dump".  Mirrors bus_read()'s decode order (so the same
 * address resolves to the same backing store) but only ever touches plain
 * memory arrays: device apertures, PCI config cycles, the legacy INTA ack
 * byte, I/O SAPIC and sparse I/O ports all return false, and the monitor's
 * own read watchpoints are never consulted. */
static bool i2000_mon_read(void *ud, uint64_t addr, uint8_t *val) {
    Ia64I2000State *s = ud;
    bool region6 = (addr >> 60) == 0xC;
    addr = i2000_normalize_pa(s, addr);
    uint64_t off;
    if (i2000_card_claims(s, addr))
        return false;       /* device apertures have side effects */
    if (region6 && addr < I2000_FLASH_BASE)
        return false;       /* PCI configuration cycle */
    if (addr < s->ram_size) {
        *val = s->ram[addr];
        return true;
    }
    if (addr >= I460GX_IOSAPIC_BASE && addr < I460GX_IOSAPIC_BASE + I460GX_IOSAPIC_SIZE)
        return false;
    if (addr == I2000_LEGACY_ACK_ADDR)
        return false;       /* acknowledges the pending ExtINT */
    if (addr - I2000_FLASH_BASE < I2000_FLASH_SIZE) {
        /* The array contents; a guest read sees status or identifier codes
         * when the firmware hub is in that mode. */
        off = addr - I2000_FLASH_BASE;
        *val = s->flash[off];
        return true;
    }
    return false;           /* sparse I/O ports, SAC registers, unmapped */
}

/* Writes RAM and the device windows a monitor write can reach. */
static bool i2000_mon_write(void *ud, uint64_t addr, uint8_t val) {
    Ia64I2000State *s = ud;
    bool region6 = (addr >> 60) == 0xC;
    addr = i2000_normalize_pa(s, addr);
    if (i2000_card_claims(s, addr))
        return false;
    if (region6 && addr < I2000_FLASH_BASE)
        return false;
    if (addr < s->ram_size) {
        s->ram[addr] = val;
        return true;
    }
    return false;
}

/* ── Monitor commands ────────────────────────────────────────────────────── */

/* monitor "input": keyboard controller and PIT state */
static GemuMonCmd i2000_cmd_input(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    (void)args;
    gemu_monitor_printf(mon, "8042: command=%02X out=%u/%u aux=%u/%u "
           "kbd=%u/%u irq1=%u retry=%" PRIu64 " ninsts=%" PRIu64 "\n",
           s->kbc_command_byte,
           s->kbc_out_pos, s->kbc_out_len,
           s->aux_out_pos, s->aux_out_len,
           s->ps2_keyboard.queue_pos, s->ps2_keyboard.queue_len,
           s->keyboard_irq_pending, s->keyboard_retry_ninsts,
           s->cpu->ninsts);
    i2000_pit_run(s);
    for (unsigned c = 0; c < 3; c++)
        gemu_monitor_printf(mon, "PIT%u: mode=%u reload=%u count=%u out=%u "
               "gate=%u\n", c, s->pit.ch[c].mode,
               s->pit.ch[c].reload ? (unsigned)s->pit.ch[c].reload : 65536u,
               (unsigned)s->pit.ch[c].count, s->pit.ch[c].out,
               s->pit.ch[c].gate);
    gemu_monitor_printf(mon, "IRQ0: pending=%u mask=%02X\n",
           s->pit_irq_pending, s->pic.master.imr);
    if (ps2_keyboard_has_data(&s->ps2_keyboard)) {
        gemu_monitor_printf(mon, "keyboard bytes:");
        for (unsigned i = s->ps2_keyboard.queue_pos;
             i < s->ps2_keyboard.queue_len; i++)
            gemu_monitor_printf(mon, " %02X", s->ps2_keyboard.queue[i]);
        gemu_monitor_printf(mon, "\n");
    }
    return GEMU_MON_NONE;
}

/* One host key press: the PS/2 keyboard, plus the serial console and the
 * EFI BIOS key buffer for ASCII (the same paths window input takes). */
static void i2000_type_key(Ia64I2000State *s, uint32_t keysym) {
    if (keysym <= 0x7F) {
        i2000_con_rx(s, (uint8_t)keysym);
        i2000_efi_bios_key(s, (uint8_t)keysym);
    } else {
        i2000_efi_bios_ext_key(s, keysym);
    }
    ps2_keyboard_tap(&s->ps2_keyboard, keysym);
}

/* QEMU key names (sendkey), as X11 keysyms or ASCII. */
static const struct { const char *name; uint32_t keysym; } i2000_key_names[] = {
    { "ret", '\r' }, { "spc", ' ' }, { "tab", '\t' }, { "esc", 0x1B },
    { "backspace", '\b' }, { "delete", 0xffff }, { "insert", 0xff63 },
    { "up", 0xff52 }, { "down", 0xff54 }, { "left", 0xff51 }, { "right", 0xff53 },
    { "home", 0xff50 }, { "end", 0xff57 }, { "pgup", 0xff55 }, { "pgdn", 0xff56 },
    { "f1", 0xffbe }, { "f2", 0xffbf }, { "f3", 0xffc0 }, { "f4", 0xffc1 },
    { "f5", 0xffc2 }, { "f6", 0xffc3 }, { "f7", 0xffc4 }, { "f8", 0xffc5 },
    { "f9", 0xffc6 }, { "f10", 0xffc7 }, { "f11", 0xffc8 }, { "f12", 0xffc9 },
    { "menu", 0xff67 },
};

/* Modifier names for QEMU-style combos ("ctrl-alt-delete", "shift-f10"). */
static const struct { const char *name; uint32_t keysym; } i2000_mod_names[] = {
    { "shift", 0xffe1 }, { "shift_r", 0xffe2 }, { "ctrl", 0xffe3 },
    { "ctrl_r", 0xffe4 }, { "alt", 0xffe9 }, { "alt_r", 0xffea },
    { "altgr", 0xffea }, { "meta_l", 0xffeb }, { "meta_r", 0xffec },
    { "win", 0xffeb },
};

static uint32_t i2000_lookup_key(const char *name, size_t len, bool modifier) {
    if (modifier) {
        for (size_t i = 0; i < sizeof(i2000_mod_names) / sizeof(i2000_mod_names[0]); i++)
            if (strlen(i2000_mod_names[i].name) == len &&
                strncmp(name, i2000_mod_names[i].name, len) == 0)
                return i2000_mod_names[i].keysym;
        return 0;
    }
    for (size_t i = 0; i < sizeof(i2000_key_names) / sizeof(i2000_key_names[0]); i++)
        if (strlen(i2000_key_names[i].name) == len &&
            strncmp(name, i2000_key_names[i].name, len) == 0)
            return i2000_key_names[i].keysym;
    for (size_t i = 0; i < sizeof(i2000_mod_names) / sizeof(i2000_mod_names[0]); i++)
        if (strlen(i2000_mod_names[i].name) == len &&
            strncmp(name, i2000_mod_names[i].name, len) == 0)
            return i2000_mod_names[i].keysym;      /* a lone modifier tap */
    return len == 1 ? (unsigned char)name[0] : 0;
}

/* "mod-mod-key": hold the modifiers down on the PS/2 keyboard around a tap
 * of the key. False (nothing sent) unless every part before the last '-' is
 * a modifier name and the last part a key name or single character, so text
 * containing dashes still types literally. */
static bool i2000_send_combo(Ia64I2000State *s, const char *tok) {
    uint32_t mods[8];
    unsigned nmods = 0;
    const char *p = tok;
    for (const char *dash; (dash = strchr(p, '-')) && dash[1]; p = dash + 1) {
        uint32_t m = i2000_lookup_key(p, (size_t)(dash - p), true);
        if (!m || nmods == sizeof(mods) / sizeof(mods[0]))
            return false;
        mods[nmods++] = m;
    }
    if (!nmods)
        return false;
    uint32_t key = i2000_lookup_key(p, strlen(p), false);
    if (!key)
        return false;
    for (unsigned i = 0; i < nmods; i++)
        ps2_keyboard_key(&s->ps2_keyboard, mods[i], true);
    ps2_keyboard_tap(&s->ps2_keyboard, key);
    for (unsigned i = nmods; i-- > 0; )
        ps2_keyboard_key(&s->ps2_keyboard, mods[i], false);
    return true;
}

/* monitor "sendkey KEY|TEXT ...": named keys (ret, spc, f8, ...) or text */
static GemuMonCmd i2000_cmd_sendkey(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    if (!*args) {
        gemu_monitor_printf(mon, "usage: sendkey <keys|text>...  e.g. sendkey ret, "
                                 "sendkey fs0: ret, sendkey ctrl-alt-delete, "
                                 "sendkey shift-f10\n");
        return GEMU_MON_NONE;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", args);
    for (char *tok = strtok(buf, " \t"); tok; tok = strtok(NULL, " \t")) {
        if (i2000_send_combo(s, tok))
            continue;
        bool named = false;
        for (size_t i = 0; i < sizeof(i2000_key_names) / sizeof(i2000_key_names[0]); i++)
            if (strcmp(tok, i2000_key_names[i].name) == 0) {
                i2000_type_key(s, i2000_key_names[i].keysym);
                named = true;
                break;
            }
        if (!named)
            for (const char *p = tok; *p; p++)
                i2000_type_key(s, (unsigned char)*p);
    }
    return GEMU_MON_NONE;
}

/* monitor "savevm FILE": snapshot the full machine state to FILE */
static GemuMonCmd i2000_cmd_savevm(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    char vm_path[256];
    if (sscanf(args, "%255s", vm_path) != 1) {
        gemu_monitor_printf(mon, "usage: savevm <file>\n");
        return GEMU_MON_NONE;
    }
    if (i2000_save_snapshot(s, vm_path))
        gemu_monitor_printf(mon, "saved snapshot to %s (ninsts=%" PRIu64 ")\n",
                            vm_path, s->cpu->ninsts);
    else
        gemu_monitor_printf(mon, "failed to save snapshot to %s\n", vm_path);
    return GEMU_MON_NONE;
}

/* monitor "loadvm FILE": restore full machine state from FILE (run this
 * right after startup to skip re-running a slow, already-verified boot
 * prefix) */
static GemuMonCmd i2000_cmd_loadvm(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    char vm_path[256];
    if (sscanf(args, "%255s", vm_path) != 1) {
        gemu_monitor_printf(mon, "usage: loadvm <file>\n");
        return GEMU_MON_NONE;
    }
    if (i2000_load_snapshot(s, vm_path))
        gemu_monitor_printf(mon, "loaded snapshot from %s (ninsts=%" PRIu64 ")\n",
                            vm_path, s->cpu->ninsts);
    else
        gemu_monitor_printf(mon, "failed to load snapshot from %s\n", vm_path);
    return GEMU_MON_NONE;
}

/* monitor "panel FILE": dump the CPU/MMIO debug front panel (the live
 * display and default screendump now show real VGA output instead) */
static GemuMonCmd i2000_cmd_panel(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    char panel_path[256];
    if (sscanf(args, "%255s", panel_path) != 1) {
        gemu_monitor_printf(mon, "usage: panel <file>\n");
        return GEMU_MON_NONE;
    }
    panel_render(s);
    if (gemu_screendump_argb(panel_path, s->fb, FB_W, FB_H))
        gemu_monitor_printf(mon, "wrote debug panel to %s\n", panel_path);
    else
        gemu_monitor_printf(mon, "cannot write %s\n", panel_path);
    return GEMU_MON_NONE;
}

/* monitor "trace N": stderr-trace the next N executed slots */
static GemuMonCmd i2000_cmd_trace(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    uint64_t n;
    if (sscanf(args, "%" SCNu64, &n) != 1) {
        gemu_monitor_printf(mon, "usage: trace <count>\n");
        return GEMU_MON_NONE;
    }
    s->cpu->trace_n = n;
    gemu_monitor_printf(mon, "tracing next %" PRIu64 " slots to stderr\n", n);
    return GEMU_MON_NONE;
}

/* monitor "history [N]": dump the most recently executed slots to stderr */
static GemuMonCmd i2000_cmd_history(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    (void)mon;
    unsigned count = 128;
    (void)sscanf(args, "%u", &count);
    if (count > MERCED_TRACE_HISTORY) count = MERCED_TRACE_HISTORY;
    merced_dump_trace(s->cpu, count, stderr);
    return GEMU_MON_NONE;
}

/* monitor "calls [N]": dump the recent call history to stderr */
static GemuMonCmd i2000_cmd_calls(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    (void)mon;
    unsigned count = MERCED_CALL_HISTORY;
    if (sscanf(args, "%u", &count) != 1)
        count = MERCED_CALL_HISTORY;
    if (count > MERCED_CALL_HISTORY) count = MERCED_CALL_HISTORY;
    merced_dump_calls(s->cpu, count, stderr);
    return GEMU_MON_NONE;
}

/* monitor "mousestate": deliberately dumps to stderr next to the trace */
static GemuMonCmd i2000_cmd_mousestate(GemuMonitor *mon, const char *args,
                                       void *ud) {
    Ia64I2000State *s = ud;
    (void)mon;
    (void)args;
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
            s->chipset.iosapic_rte[12], !!(s->chipset.iosapic_rte[12] & I460GX_IOSAPIC_RTE_MASK),
            s->pic.master.imr, s->pic.slave.imr);
    fflush(stderr);
    return GEMU_MON_NONE;
}

/* monitor "mousemove DX DY [left|right]" */
static GemuMonCmd i2000_cmd_mousemove(GemuMonitor *mon, const char *args,
                                      void *ud) {
    Ia64I2000State *s = ud;
    int mdx = 0, mdy = 0;
    char mbtn[8] = "";
    if (sscanf(args, "%d %d %7s", &mdx, &mdy, mbtn) < 2) {
        gemu_monitor_printf(mon, "usage: mousemove <dx> <dy> [left] [right]\n");
        return GEMU_MON_NONE;
    }
    /* Synthetic PS/2 packet injection for headless testing - see
     * um6578's own "mousemove"/"mouse" monitor commands for the
     * sibling feature. Goes through the exact same aux_queue_byte()/
     * ExtINT path a real host mouse event would, so this exercises
     * the full pipeline (packet build, IRQ delivery, ack) without
     * needing a live display. */
    if (!s->mouse_enabled) {
        gemu_monitor_printf(mon, "mousemove: no mouse attached (started with -nodefaults)\n");
    } else if (!s->mouse_streaming) {
        gemu_monitor_printf(mon, "mousemove: mouse attached but guest hasn't "
                            "enabled streaming yet (mouse_streaming=0)\n");
    } else if (s->aux_out_len != 0 ||
               s->cpu->ninsts < s->mouse_retry_ninsts) {
        /* Matches the real per-frame polling path's backpressure
         * guard (ia64_i2000_run()) - real host mouse motion never
         * queues a new packet while the previous one is still even
         * partially undrained or its cooldown hasn't elapsed,
         * letting the guest fully finish with one report before
         * being offered the next. */
        gemu_monitor_printf(mon, "mousemove: previous packet not yet "
                            "drained/cooled down (aux_out_len=%u), dropped\n",
                            s->aux_out_len);
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
        gemu_monitor_printf(mon, "mousemove: dx=%d dy=%d left=%d right=%d queued\n",
                            mdx, mdy, left, right);
    }
    return GEMU_MON_NONE;
}

/* monitor "mousekick" */
static GemuMonCmd i2000_cmd_mousekick(GemuMonitor *mon, const char *args,
                                      void *ud) {
    Ia64I2000State *s = ud;
    (void)args;
    /* Debug-only: unconditionally re-raise IRQ12 if aux_out has any
     * unread bytes, regardless of the empty->nonempty transition
     * aux_queue_byte()/the 0x60 read handler normally require. For
     * recovering a stuck backlog from an old snapshot saved before
     * mouse_irq_pending existed, not part of the real device model. */
    if (s->aux_out_pos < s->aux_out_len &&
        !(s->pic.slave.imr & 0x10) && !(s->pic.master.imr & 0x04)) {
        s->mouse_irq_pending = true;
        i2000_pic_raise(s);
        gemu_monitor_printf(mon, "mousekick: re-raised (aux_out_pos=%u "
                            "aux_out_len=%u)\n",
                            s->aux_out_pos, s->aux_out_len);
    } else {
        gemu_monitor_printf(mon, "mousekick: nothing to do (aux_out_pos=%u "
                            "aux_out_len=%u pic_slave_mask=%02x "
                            "pic_master_mask=%02x)\n",
                            s->aux_out_pos, s->aux_out_len, s->pic.slave.imr,
                            s->pic.master.imr);
    }
    return GEMU_MON_NONE;
}

/* monitor "key KEY|TEXT" */
static GemuMonCmd i2000_cmd_key(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    const char *keyarg = args;
    if (!keyarg[0]) {
        gemu_monitor_printf(mon, "usage: key <right|left|up|down|enter|tab|esc|mod-key|text>  (mod: shift ctrl alt win)\n");
        return GEMU_MON_NONE;
    }
    /* Monitor-scripted key injection for headless UI testing - the
     * display input path (SDL or VNC) only ever feeds kbc_queue_ascii()
     * printable/control ASCII, so arrows (needed for the graphical
     * setup's tab bar per its own on-screen instructions) have no
     * other way in. A non-special argument is typed verbatim, which
     * permits commands such as "key map -r" at the EFI shell. */
    if (!strpbrk(keyarg, " \t") && i2000_send_combo(s, keyarg)) {
        gemu_monitor_printf(mon, "key %s queued\n", keyarg);
        return GEMU_MON_NONE;
    }
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
        if (special > 0x7f)
            i2000_efi_bios_ext_key(s, special);
        /* EFI's built-in shell uses the polled serial console in
         * this firmware revision (cr.iva remains zero, so it does
         * not consume PS/2 IRQ1 scan codes).  Real display ASCII
         * input is mirrored into UART RX; monitor injection must do
         * the same or `key` appears to succeed while the shell sees
         * nothing. */
        if (special <= 0x7f) {
            i2000_con_rx(s, (uint8_t)special);
            i2000_efi_bios_key(s, (uint8_t)special);
        }
        gemu_monitor_printf(mon, "key %s queued\n", keyarg);
    } else {
        size_t queued = 0;
        for (const unsigned char *p = (const unsigned char *)keyarg;
             *p; p++) {
            if (*p < 0x20 || *p > 0x7e)
                continue;
            ps2_keyboard_tap(&s->ps2_keyboard, *p);
            i2000_con_rx(s, *p);
            i2000_efi_bios_key(s, *p);
            queued++;
        }
        gemu_monitor_printf(mon, "key: queued %" PRIu64 " character%s; use "
                            "'key enter' to submit\n", (uint64_t)queued,
                            queued == 1 ? "" : "s");
    }
    return GEMU_MON_NONE;
}

/* monitor "dump ADDR LEN FILE" (both hex): physical memory to a file.
 * Uses the side-effect-free memory view; bytes it can't read safely
 * (MMIO, I/O ports) are written as 0xFF, like an unmapped bus read. */
static GemuMonCmd i2000_cmd_dump(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    uint64_t daddr, dlen;
    char path[256];
    if (sscanf(args, "%" SCNx64 " %" SCNx64 " %255s", &daddr, &dlen, path) != 3) {
        gemu_monitor_printf(mon, "usage: dump <addr> <len> <file>\n");
        return GEMU_MON_NONE;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        gemu_monitor_printf(mon, "cannot open %s\n", path);
        return GEMU_MON_NONE;
    }
    for (uint64_t i = 0; i < dlen; i++) {
        uint8_t b;
        if (!i2000_mon_read(s, daddr + i, &b))
            b = 0xFF;
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    gemu_monitor_printf(mon, "dumped 0x%" PRIX64 " bytes from 0x%" PRIX64 " to %s\n",
                        dlen, daddr, path);
    return GEMU_MON_NONE;
}

static GemuMonCmd i2000_cmd_vhptstats(GemuMonitor *mon, const char *args,
                                      void *ud) {
    (void)args;
    (void)ud;
    uint64_t calls, disabled, hit, unmapped, tagfail, np;
    merced_vhpt_stats(&calls, &disabled, &hit, &unmapped, &tagfail, &np);
    gemu_monitor_printf(mon, "vhpt: calls=%" PRIu64 " disabled=%" PRIu64
                        " hit=%" PRIu64 " unmapped=%" PRIu64 " tagfail=%" PRIu64
                        " np=%" PRIu64 "\n",
                        calls, disabled, hit, unmapped, tagfail, np);
    return GEMU_MON_NONE;
}

static GemuMonCmd i2000_cmd_faultstats(GemuMonitor *mon, const char *args,
                                       void *ud) {
    (void)args;
    (void)ud;
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
    gemu_monitor_printf(mon, "faults:");
    for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
        if (counts[vectors[i].slot])
            gemu_monitor_printf(mon, " %s=%" PRIu64, vectors[i].name,
                                counts[vectors[i].slot]);
    gemu_monitor_printf(mon, "\n");
    return GEMU_MON_NONE;
}

static GemuMonCmd i2000_cmd_atapistate(GemuMonitor *mon, const char *args,
                                       void *ud) {
    Ia64I2000State *s = ud;
    (void)args;
    const AtapiCdrom *d = &s->cd;
    gemu_monitor_printf(mon, "atapi: status=%#04x error=%#04x count=%#04x "
           "device=%#04x control=%#04x irq_pending=%u irq_delivered=%u "
           "irq_ready=%" PRIu64 " status_pending=%u "
           "status_ready=%" PRIu64 " ready_status=%#04x ready_irq=%u "
           "data=%" PRIu64 "/%" PRIu64 " phase_end=%" PRIu64
           " ninsts=%" PRIu64 "\n",
           d->status, d->error, d->count, d->device, d->control,
           d->irq_pending, s->atapi_irq_delivered == d->irq_epoch,
           d->irq_ready_at, d->status_pending,
           d->status_ready_at, d->ready_status,
           d->ready_irq, (uint64_t)d->data_pos,
           (uint64_t)d->data_len, (uint64_t)d->phase_end,
           s->cpu->ninsts);
    return GEMU_MON_NONE;
}


static GemuMonCmd i2000_cmd_ntsym(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    uint64_t va;
    if (sscanf(args, "%" SCNx64, &va) != 1) {
        gemu_monitor_printf(mon, "usage: ntsym <hex va>\n");
        return GEMU_MON_NONE;
    }
    merced_nt_identify(s->cpu, va);
    return GEMU_MON_NONE;
}

static GemuMonCmd i2000_cmd_audio(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    PciAudio *a = &s->audio;
    (void)args;
    gemu_monitor_printf(mon, "audio: %s command=%04x BAR0=%08x BAR1=%08x irq=%u\n",
        pci_audio_name(a->model), pci_audio_cfg_read(a, 4, 2),
        pci_audio_cfg_read(a, 0x10, 4), pci_audio_cfg_read(a, 0x14, 4), a->irq_level);
    gemu_monitor_printf(mon, "  frames=%" PRIu64 " nonzero=%" PRIu64
        " dma_bytes=%" PRIu64 " interrupts=%" PRIu64
        " master=%04x pcm=%04x host=%s queued=%u underruns=%u\n",
        a->frames, a->nonzero_frames, a->dma_bytes, a->interrupts,
        a->codec[1], a->codec[0x18 / 2], s->audio_out ? "open" : "closed",
        gemu_audio_queued(s->audio_out), gemu_audio_underruns(s->audio_out));
    if (a->model == PCI_AUDIO_CS4281) {
        for (unsigned c = 0; c < 4; c++)
            gemu_monitor_printf(mon, "  dma%u mode=%08x control=%08x address=%08x count=%08x fifo=%08x\n",
                c, a->u.cs.reg[(0x150 + c * 8) / 4], a->u.cs.reg[(0x154 + c * 8) / 4],
                a->u.cs.reg[(0x110 + c * 16) / 4], a->u.cs.reg[(0x114 + c * 16) / 4],
                a->u.cs.reg[(0x180 + c * 4) / 4]);
    } else if (a->model == PCI_AUDIO_AC97) {
        for (unsigned c = 0; c < 3; c++) {
            IchAudioDma *d = &a->u.ich.dma[c];
            gemu_monitor_printf(mon, "  dma%u bdl=%08x civ=%u lvi=%u remaining=%u status=%04x control=%02x\n",
                c, d->bdbar, d->civ, d->lvi, d->remaining, d->status, d->command);
        }
    }
    return GEMU_MON_NONE;
}

static GemuMonCmd i2000_cmd_iosapic(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    (void)args;
    for (unsigned i = 0; i < 64; i++) {
        uint64_t rte = s->chipset.iosapic_rte[i];
        if (rte == I460GX_IOSAPIC_RTE_MASK || rte == 0)
            continue;
        gemu_monitor_printf(mon, "gsi%-2u rte=%016" PRIX64 " vector=%02x mode=%u%s%s%s\n",
                            i, rte, (unsigned)(uint8_t)rte, (unsigned)((rte >> 8) & 7),
                            (rte & I460GX_IOSAPIC_RTE_MASK) ? " masked" : "",
                            (rte & (UINT64_C(1) << 15)) ? " level" : " edge",
                            (rte & (UINT64_C(1) << 13)) ? " low" : " high");
    }
    gemu_monitor_printf(mon, "pic masks master=%02x slave=%02x kbd_data=%d gsi1_asserted=%d\n",
                        s->pic.master.imr, s->pic.slave.imr,
                        s->kbc_out_pos < s->kbc_out_len ||
                        ps2_keyboard_has_data(&s->ps2_keyboard),
                        s->keyboard_gsi_asserted);
    gemu_monitor_printf(mon, "cpu psr.i=%u psr.ic=%u psr.is=%u eflags.if=%u cflg=%" PRIX64
                        " iva=%016" PRIX64 " tpr=%016" PRIX64
                        " lrr0=%016" PRIX64 " pending=%u in-service:",
                        (unsigned)((s->cpu->psr >> 14) & 1),
                        (unsigned)((s->cpu->psr >> 13) & 1),
                        (unsigned)((s->cpu->psr >> 34) & 1),
                        (unsigned)((s->cpu->ar[24] >> 9) & 1),
                        s->cpu->ar[27], s->cpu->cr[2], s->cpu->cr[66], s->cpu->cr[80],
                        (unsigned)s->cpu->ext_pending_count);
    for (unsigned v = 0; v < 256; v++)
        if (s->cpu->ext_in_service[v >> 3] & (1u << (v & 7)))
            gemu_monitor_printf(mon, " %02x", v);
    gemu_monitor_printf(mon, " pending:");
    for (unsigned v = 0; v < 256; v++)
        if (s->cpu->external_pending[v >> 3] & (1u << (v & 7)))
            gemu_monitor_printf(mon, " %02x", v);
    gemu_monitor_printf(mon, "\n");
    return GEMU_MON_NONE;
}

/* A short press of the power button: the IFB sets PWRBTN_STS and the guest's
 * ACPI handler decides what to do about it. */
static GemuMonCmd i2000_cmd_powerdown(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    (void)mon; (void)args;
    i460gx_pm_power_button(&s->chipset, true);
    i460gx_pm_power_button(&s->chipset, false);
    return GEMU_MON_NONE;
}

static GemuMonCmd i2000_cmd_vgareg(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    (void)args;
    gemu_monitor_printf(mon, "vga: misc=%02x text_mode=%d blanked=%d\n",
                        s->vga.misc, vga_text_mode(&s->vga),
                        vga_blanked(&s->vga));
    gemu_monitor_printf(mon, "seq[0-7]:");
    for (int i = 0; i < 8; i++) gemu_monitor_printf(mon, " %02x", s->vga.seq[i]);
    gemu_monitor_printf(mon, "\ngc[0-15]:");
    for (int i = 0; i < 16; i++) gemu_monitor_printf(mon, " %02x", s->vga.gc[i]);
    gemu_monitor_printf(mon, "\ncrtc[0-31]:");
    for (int i = 0; i < 32; i++) gemu_monitor_printf(mon, " %02x", s->vga.crtc[i]);
    gemu_monitor_printf(mon, "\nattr[0-31]:");
    for (int i = 0; i < 32; i++) gemu_monitor_printf(mon, " %02x", s->vga.attr[i]);
    gemu_monitor_printf(mon, "\n");
    gemu_monitor_printf(mon, "dac[0-15] (r,g,b):");
    for (int i = 0; i < 16; i++)
        gemu_monitor_printf(mon, " %02x%02x%02x", s->vga.dac[i * 3],
                            s->vga.dac[i * 3 + 1], s->vga.dac[i * 3 + 2]);
    gemu_monitor_printf(mon, "\ndac write index=%u sub=%u mask=%02x\n",
                        s->vga.dac_idx, s->vga.dac_sub, s->vga.pixel_mask);
    return GEMU_MON_NONE;
}

static GemuMonCmd i2000_cmd_vgapeek(GemuMonitor *mon, const char *args, void *ud) {
    Ia64I2000State *s = ud;
    unsigned plane, off, count;
    if (sscanf(args, "%u %x %u", &plane, &off, &count) == 3 && plane < 4 &&
        count <= 256 && off + count <= 0x10000) {
        gemu_monitor_printf(mon, "vgapeek plane=%u off=0x%x:", plane, off);
        for (unsigned i = 0; i < count; i++)
            gemu_monitor_printf(mon, " %02x", s->vga.plane[plane][off + i]);
        gemu_monitor_printf(mon, "\n");
    } else {
        gemu_monitor_printf(mon, "usage: vgapeek <plane 0-3> <hex offset> <count>\n");
    }
    return GEMU_MON_NONE;
}

static void i2000_monitor_setup(Ia64I2000State *s) {
    GemuMonitor *mon = s->monitor;
    gemu_monitor_bind(mon, "input",      i2000_cmd_input, s);
    gemu_monitor_bind(mon, "sendkey",    i2000_cmd_sendkey, s);
    gemu_monitor_bind(mon, "savevm",     i2000_cmd_savevm, s);
    gemu_monitor_bind(mon, "loadvm",     i2000_cmd_loadvm, s);
    gemu_monitor_bind(mon, "panel",      i2000_cmd_panel, s);
    gemu_monitor_bind(mon, "trace",      i2000_cmd_trace, s);
    gemu_monitor_bind(mon, "history",    i2000_cmd_history, s);
    gemu_monitor_bind(mon, "calls",      i2000_cmd_calls, s);
    gemu_monitor_bind(mon, "mousestate", i2000_cmd_mousestate, s);
    gemu_monitor_bind(mon, "mousemove",  i2000_cmd_mousemove, s);
    gemu_monitor_bind(mon, "mousekick",  i2000_cmd_mousekick, s);
    gemu_monitor_bind(mon, "key",        i2000_cmd_key, s);
    gemu_monitor_bind(mon, "dump",       i2000_cmd_dump, s);
    gemu_monitor_bind(mon, "vhptstats",  i2000_cmd_vhptstats, s);
    gemu_monitor_bind(mon, "faultstats", i2000_cmd_faultstats, s);
    gemu_monitor_bind(mon, "atapistate", i2000_cmd_atapistate, s);
    gemu_monitor_bind(mon, "vgareg",     i2000_cmd_vgareg, s);
    gemu_monitor_bind(mon, "iosapic",    i2000_cmd_iosapic, s);
    gemu_monitor_bind(mon, "audio",      i2000_cmd_audio, s);
    gemu_monitor_bind(mon, "system_powerdown", i2000_cmd_powerdown, s);
    gemu_monitor_bind(mon, "ntsym",      i2000_cmd_ntsym, s);
    gemu_monitor_bind(mon, "vgapeek",    i2000_cmd_vgapeek, s);
    gemu_monitor_set_memory(mon, &(GemuMonMemory){
        .read = i2000_mon_read, .write = i2000_mon_write, .ud = s,
        .addr_bits = 64, .big_endian = false });
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/* Encode one DIMM's Serial Presence Detect content for `bytes` of
 * capacity (must be a power of two) per the Intel/JEDEC "PC SDRAM Serial
 * Presence Detect" spec (JESD21-C sec. 4.1.2.5) that SSDM 248704-001
 * sec. 5.5.1 cites. Only the fields firmware's memory-sizing path actually
 * needs are filled in with confidence (type, row/column address counts,
 * physical banks, data width, banks-per-device); everything else is left
 * at conservative, spec-legal defaults since nothing here validates DIMM
 * timing against real electrical constraints anyway. Size formula used:
 * bytes = 2^(rows+cols) * (width_bits/8) * banks_per_device * phys_banks.
 * With one physical rank and a 64-bit module width, two internal banks
 * give rows+cols = log2(bytes) - 4; four banks give log2(bytes) - 5.
 * Each EEPROM describes one of the four matched DIMMs in a memory row. */
static void spd_encode(uint8_t out[256], uint64_t bytes) {
    memset(out, 0, 256);
    unsigned total_bits = 0;
    for (uint64_t b = bytes; b > 1; b >>= 1)
        total_bits++;
    unsigned banks = bytes < (UINT64_C(64) << 20) ? 2 : 4;
    unsigned addr_bits = total_bits - (banks == 2 ? 4 : 5);
    /* Match the SDRAM device organizations supported by the 460GX:
     * 16/32 MiB modules use 11 row bits and two banks, 64/128 MiB use
     * 12 row bits and four banks, and 256 MiB uses 13 row bits. */
    unsigned rows = bytes >= (UINT64_C(256) << 20) ? 13 :
                    bytes >= (UINT64_C(64) << 20) ? 12 : 11;
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
    out[17] = (uint8_t)banks; /* internal banks per SDRAM device */
    out[18] = 0x06;  /* CAS latencies supported: CL2, CL3 */
    out[62] = 0x11;  /* SPD revision 1.1 */
    unsigned sum = 0;
    for (unsigned i = 0; i < 63; i++)
        sum += out[i];
    out[63] = (uint8_t)sum;  /* checksum over bytes 0-62 */
}

/* Two rows of four DIMMs can represent only sums of one or two supported
 * row capacities.  Reject other sizes before allocating RAM rather than
 * silently reporting a smaller population through SPD. */
static bool i2000_ram_size_valid(uint64_t bytes) {
    const uint64_t min_row = UINT64_C(64) << 20;
    const uint64_t max_row = UINT64_C(1024) << 20;
    if (bytes < min_row || bytes > I2000_RAM_MAX || bytes % min_row)
        return false;
    for (uint64_t row = min_row; row <= max_row; row <<= 1) {
        if (bytes == row)
            return true;
        if (bytes > row) {
            uint64_t other = bytes - row;
            if (other >= min_row && other <= max_row &&
                !(other & (other - 1)))
                return true;
        }
    }
    return false;
}

/* A 460GX memory row requires four identical DIMMs.  Populate the i2000's
 * eight sockets in matched groups, with at most 256 MiB per module. */
static void spd_init(Ia64I2000State *s) {
    i2c_bus_init(&s->memcard_i2c, "memory card A");
    unsigned slot = 0;
    uint64_t remaining = s->ram_size;
    while (remaining && slot < I2000_SPD_SLOTS) {
        uint64_t chunk = remaining / I2000_DIMMS_PER_ROW;
        if (chunk > UINT64_C(256) * 1024 * 1024)
            chunk = UINT64_C(256) * 1024 * 1024;
        /* SPD geometry describes powers of two.  Use the largest module
         * that fits; ordinary supported -m sizes divide exactly. */
        while (chunk & (chunk - 1))
            chunk &= chunk - 1;
        for (unsigned i = 0; i < I2000_DIMMS_PER_ROW; i++, slot++) {
            at24_init(&s->spd[slot]);
            spd_encode(s->spd[slot].mem, chunk);
            s->spd[slot].write_protect = true;
            I2cDevice eeprom = at24_device(&s->spd[slot],
                                           I2000_SPD_BASE_ADDR + slot);
            i2c_attach(&s->memcard_i2c, &eeprom);
        }
        remaining -= chunk * I2000_DIMMS_PER_ROW;
    }
}

/* The board's processor clock-ratio latch on the IFB SMBus. A write sets the
 * ratio strap code the processor samples at the next reset; a receive-byte
 * returns it. The emulated clock does not follow it. */
#define I2000_RATIO_LATCH_ADDR 0x4e
#define I2000_RATIO_CODE       3        /* (8 + 3) / 2 of the 133 MHz bus */
#define I2000_HARDWARE_MONITOR_ADDR 0x2c

/* The J29 boot jumper's levels on the IFB's GPIO inputs, in GP register
 * layout. Bit 24 (GPIO[18]) selects normal running rather than OVRD/CLRCMOS;
 * bit 18 (GPIO[12]) deasserts manufacturing mode, which would otherwise make
 * the boot manager rebuild BootOrder on every boot and discard an installer's
 * boot entry; bit 19 (GPIO[13]) is the third pin the positions differ in.
 * NORM is the documented default position. */
#define I2000_J29_PINS UINT32_C(0x010c0000)
#define I2000_J29_NORM UINT32_C(0x01040000)

static bool ratio_latch_start(void *ctx, bool read) {
    (void)ctx; (void)read;
    return true;
}

static bool ratio_latch_write(void *ctx, uint8_t code) {
    *(uint8_t *)ctx = code;
    return true;
}

static uint8_t ratio_latch_read(void *ctx, bool ack) {
    (void)ack;
    return *(uint8_t *)ctx;
}

/* RESET# samples the ratio latch onto the processor's ratio strap, which
 * PAL_FREQ_RATIOS reads from MSR 0x617. */
static void i2000_sample_ratio_latch(Ia64I2000State *s) {
    merced_set_clock_ratio_code(s->cpu, s->ratio_latch);
}

/* One processor cartridge: its Scratch EEPROM at 50h and its PIROM at 51h
 * (the board's SMA[1:0] straps give the first slot 00b). The PIROM states
 * the 733 MHz, 2 MB part. */
static void i2000_smbus_init(Ia64I2000State *s) {
    i2c_bus_init(&s->smbus, "IFB SMBus");
    ItaniumPirom pirom = {
        .sspec = "SL4LT", .production = true,
        .cpuid3 = (s->cpu->cpuid[3] & ~UINT64_C(0xff00)) | UINT64_C(6) << 8,
        .max_core_mhz = 733, .max_bus_mhz = 133, .l3_kb = 2048,
        .tap_devices = 3, .scratch_present = true,
    };
    uint8_t image[128];
    itanium_pirom_build(image, &pirom);
    itanium_cart_rom_init(&s->cart_pirom, image, false);
    itanium_cart_rom_init(&s->cart_scratch, NULL, true);
    I2cDevice device = itanium_cart_rom_device(&s->cart_scratch, 0x50);
    i2c_attach(&s->smbus, &device);
    device = itanium_cart_rom_device(&s->cart_pirom, 0x51);
    i2c_attach(&s->smbus, &device);
    s->ratio_latch = I2000_RATIO_CODE;
    device = (I2cDevice){ .addr = I2000_RATIO_LATCH_ADDR, .ctx = &s->ratio_latch,
                          .start = ratio_latch_start, .write = ratio_latch_write,
                          .read = ratio_latch_read };
    i2c_attach(&s->smbus, &device);
    i2000_sample_ratio_latch(s);

    /* The baseboard's hardware monitor, ADD pin open. */
    lm87_reset(&s->hardware_monitor);
    device = lm87_device(&s->hardware_monitor, I2000_HARDWARE_MONITOR_ADDR);
    i2c_attach(&s->smbus, &device);
}

/* Board wiring for the reusable 460GX register model. */
static uint64_t i2000_chipset_clock(void *opaque) {
    Ia64I2000State *s = opaque;
    return s->cpu ? merced_get_time_base(s->cpu) : 0;
}

/* The IFB's SMI# is the processor's PMI# pin. The firmware's own PMI handler
 * services it, including the ACPI enable and disable commands written to the
 * APM port. */
static void i2000_pm_smi(void *opaque, bool asserted) {
    Ia64I2000State *s = opaque;
    if (asserted && s->cpu)
        merced_pmi_raise(s->cpu, 0);
}

static void i2000_pm_sci(void *opaque, unsigned irq, bool level) {
    Ia64I2000State *s = opaque;
    s->sci_irq = (int)irq;
    s->sci_level = level;
}

static void i2000_pm_sleep(void *opaque, I460GXPmState state,
                           bool power_button_override) {
    Ia64I2000State *s = opaque;
    (void)power_button_override;
    if (state == I460GX_PM_S1_STPCLK || state == I460GX_PM_S1_SLP)
        s->cpu_stopped = true;
    else
        s->power_off_requested = true;
}

static void i2000_pm_wake(void *opaque, I460GXPmState from) {
    Ia64I2000State *s = opaque;
    if (from == I460GX_PM_S1_STPCLK || from == I460GX_PM_S1_SLP)
        s->cpu_stopped = false;
    else
        s->wake_reset_requested = true;
}

static void i2000_chipset_init(Ia64I2000State *s) {
    const I460GXConfig config = {
        .ifb_bus = 0, .ifb_dev = 3,
        .pid_bus = 0, .pid_dev = 0,
        .cbn_bus = 0, .cbn_dev = 0x10,
        .link_count = 4,
        /* PXB on link 0, dual-bus WXB on link 1, GXB on links 2/3. */
        .expander = { I460GX_BRIDGE_PXB, I460GX_BRIDGE_NONE,
                      I460GX_BRIDGE_WXB, I460GX_BRIDGE_WXB,
                      I460GX_BRIDGE_GXB },
        .clock_hz = I2000_ITC_HZ, .clock = i2000_chipset_clock,
        .smbus = &s->smbus,
        .flash = s->flash,
        /* Eight sockets on memory card A, selected by function 4's row mux. */
        .memcard_i2c = { [0][0] = &s->memcard_i2c },
        .smi = i2000_pm_smi, .sci = i2000_pm_sci,
        .sleep = i2000_pm_sleep, .wake = i2000_pm_wake, .opaque = s,
    };
    i460gx_init(&s->chipset, &config);
    /* J29, the boot jumper, drives three of the IFB's GPIO input pins; the
     * firmware reads them through GP Data once it enables the GPIO block. */
    i460gx_pm_set_gpio_pins(&s->chipset, I2000_J29_PINS, I2000_J29_NORM);
}

static void i2000_devices_reset(Ia64I2000State *s, bool warm) {
    s->pci_cfg_addr = 0;
    memset(s->cmd649_cfg, 0, sizeof(s->cmd649_cfg));
    i2000_uart_reset(s);
    mc146818_core_reset(&s->rtc);
    i2000_pit_reset(s);
    atapi_cdrom_reset(&s->cd);
    ide_reset(&s->ide_secondary);
    s->atapi_legacy_irq_latched = false;
    ata_disk_reset(&s->hd);
    ide_reset(&s->ide_primary);
    s->ata_legacy_irq_latched = false;

    /* The IDE model advertises a CMD Technology PCI-649 at 00:03.1. */
    s->cmd649_cfg[0x00] = 0x95; s->cmd649_cfg[0x01] = 0x10; /* 1095 */
    s->cmd649_cfg[0x02] = 0x49; s->cmd649_cfg[0x03] = 0x06; /* 0649 */
    s->cmd649_cfg[0x08] = 0x02;                    /* revision */
    s->cmd649_cfg[0x09] = 0x8A;                    /* native/legacy IDE */
    s->cmd649_cfg[0x0A] = 0x01;                    /* IDE subclass */
    s->cmd649_cfg[0x0B] = 0x01;                    /* mass storage */
    s->cmd649_cfg[0x0E] = 0x00;                    /* normal header */
    for (unsigned bar = 0; bar < 5; bar++)
        s->cmd649_cfg[0x10 + bar * 4] = 1;         /* I/O BAR, unassigned */
    s->cmd649_cfg[0x3C] = 15;                      /* secondary IDE IRQ15 */
    s->cmd649_cfg[0x3D] = 0x01;                    /* INTA */
    /* CNTRL's primary/secondary enable bits describe the two channels
     * exposed at the legacy taskfile ports. CmdIde checks these bits
     * before creating channel devices; zero hid the boot CD entirely. */
    s->cmd649_cfg[0x51] = 0x0c;
    spd_init(s);
    if (warm)
        i460gx_warm_reset(&s->chipset);
    else
        i460gx_reset(&s->chipset);
}

Ia64I2000State *ia64_i2000_create(const Ia64Config *cfg) {
    if (!i2000_ram_size_valid(cfg->ram_size)) {
        fprintf(stderr, "gemu: i2000: unsupported RAM size %" PRIu64
                " bytes; use one or two rows of 64, 128, 256, 512 or"
                " 1024 MiB (up to 2 GiB total)\n", cfg->ram_size);
        return NULL;
    }
    Ia64I2000State *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    i2000_pic_reset(s);
    s->mouse_enabled = cfg->mouse_enabled;

    s->ram_size = cfg->ram_size;
    s->ram = calloc(1, (size_t)s->ram_size);
    s->flash = malloc(I2000_FLASH_SIZE);
    if (!s->ram || !s->flash) {
        fprintf(stderr, "gemu: cannot allocate %" PRIu64 " MiB guest RAM\n",
                cfg->ram_size >> 20);
        ia64_i2000_destroy(s);
        return NULL;
    }
    memset(s->flash, 0xFF, I2000_FLASH_SIZE);
    if (cfg->vga && strcmp(cfg->vga, "none") != 0) {
        s->vram = calloc(1, RAGE128_VRAM_SIZE);
        if (!s->vram) {
            fprintf(stderr, "gemu: cannot allocate video memory\n");
            ia64_i2000_destroy(s);
            return NULL;
        }
        s->video_enabled = true;
        vga_init(&s->vga, s->vram, RAGE128_VRAM_SIZE);
        Rage128Wiring wiring = i2000_rage_wiring(s);
        rage128_init(&s->rage128, &s->vga, &wiring);
        /* The card's own BIOS, found by -rom DIR's SHA-256 scan. */
        if (!cfg->video_rom || !ia64_i2000_load_video_rom(s, cfg->video_rom))
            fprintf(stderr, "Rage 128 video BIOS not found - point -rom at"
                    "folder holding it\n");
    }
    if (cfg->nic) {
        /* The SDV EFI expects the integrated 82559 to supply an EFI UNDI
         * driver through its PCI expansion ROM.  Merely exposing the PCI
         * function leaves Boot Manager's network-device cleanup on a broken
         * firmware error path ("Could not load UNDI" followed by a pool
         * assertion).  The ROM is an IA-64 build of QEMU's EEPRO100 EFI
         * image (the stock one carries only x86 drivers), found by -rom
         * DIR's SHA-256 scan as region "nic"; its PCI ID is retagged to the
         * integrated controller's 8086:1229. */
        size_t len;
        uint8_t *rom = cfg->nic_rom ? pci_rom_load(cfg->nic_rom, &len) : NULL;
        if (rom) {
            pci_rom_retag(rom, len, E100_VENDOR_ID, E100_DEVICE_ID);
            len = pci_rom_keep_efi(rom, len, 0x0200);
            if (len) {
                s->nic_rom = rom;
                s->nic_rom_len = (uint32_t)len;
            } else {
                free(rom);
            }
        }
        /* Without one, the production firmware (bios117c/bios130) still
         * drives the 82559 with its own built-in UNDI; src/ia64_setup.c
         * warns for the SDV build, which has none. */
        s->nic_enabled = true;
        i2000_net = cfg->nic;
        E100Config nic_cfg = { .boot_rom = s->nic_rom != NULL };
        gemu_nic_mac(cfg->nic, nic_cfg.mac);
        E100Wiring wiring = i2000_nic_wiring(s);
        e100_init(&s->nic, &nic_cfg, &wiring);
        e100_set_link(&s->nic, gemu_nic_link_up(cfg->nic));
        gemu_nic_set_link_cb(cfg->nic, i2000_nic_link, s);
        if (s->nic_rom)
            e100_set_rom(&s->nic, s->nic_rom, s->nic_rom_len);
    }
    ps2_keyboard_init(&s->ps2_keyboard);
    if (!i2000_claim_drives(s)) {
        ia64_i2000_destroy(s);
        return NULL;
    }
    i2000_chipset_init(s);
    i2000_devices_reset(s, false);

    MercedBus bus = {
        .ud = s, .ram_size = s->ram_size,
        .read = bus_read, .fetch = bus_fetch, .write = bus_write,
        .canonical = i2000_canonical_pa,
        .fill = i2000_bus_fill
    };
    s->cpu = merced_create(&bus);
    if (!s->cpu) {
        ia64_i2000_destroy(s);
        return NULL;
    }
    PciAudioWiring audio_wiring = i2000_audio_wiring(s);
    pci_audio_init(&s->audio, cfg->sound, &audio_wiring);
    if (cfg->sound)
        s->audio_out = gemu_audio_open_channels(pci_audio_name(cfg->sound), 48000, 2);
    /* PAL's MSR command port and cache arrays (merced_msr.c, merced_cache.c). */
    merced_set_msr_model(s->cpu, true);
    merced_set_cpu_revision(s->cpu, 6);             /* Merced C0 */
    bool jit = cfg->jit &&
               merced_set_accel(s->cpu, MERCED_ACCEL_JIT) == MERCED_ACCEL_JIT;
    irq_poll_quantum = jit ? JIT_IRQ_POLL_QUANTUM : IRQ_POLL_QUANTUM;
    if (jit)
        merced_set_direct_ram(s->cpu, i2000_direct_page);
    {
        const char *q = getenv("GEMU_IRQ_QUANTUM");
        unsigned long v = q ? strtoul(q, NULL, 0) : 0;
        if (v && !(v & (v - 1)) && v <= INSTR_PER_FRAME)
            irq_poll_quantum = (unsigned)v;
        else if (q)
            fprintf(stderr, "i2000: GEMU_IRQ_QUANTUM must be a power of two "
                    "up to %d; ignoring '%s'\n", INSTR_PER_FRAME, q);
    }
    i2000_smbus_init(s);
    s->monitor = gemu_monitor_create();
    i2000_monitor_setup(s);
    gemu_monitor_set_cpu_state_cb(s->monitor, i2000_cpu_state, s);
    gemu_monitor_set_screendump_cb(s->monitor, i2000_screendump, s);
    gemu_drive_attach_monitor(s->hda, s->monitor);
    gemu_drive_attach_monitor(s->cdrom, s->monitor);

    if (gemu_display_wanted(cfg->display_type)) {
        GemuDisplayConfig dc = {
            .fb_width = FB_W,
            .monitor = s->monitor,
            .fb_height = FB_H,
            .scale = cfg->display_scale,
            .actions = i2000_keyboard_actions,
            .n_actions = (int)(sizeof(i2000_keyboard_actions) /
                               sizeof(i2000_keyboard_actions[0])),
            /* No ini_section: the navigation keys above are a fixed PC
             * keyboard mapping, not user-rebindable controls, so there is
             * no Input menu page and no gemu.ini override for them. */
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
    mc146818_init(&s->rtc, &(Mc146818Config){
        .clock = i2000_rtc_clock, .clock_hz = I2000_ITC_HZ,
        .irq = i2000_rtc_irq, .opaque = s,
        .debug = getenv("RTC_DEBUG") != NULL });
    if (mc146818_power_up(&s->rtc))
        i2000_rtc_initialise(s);
    i2000_update_fast_ram(s);
    return s;
}

void ia64_i2000_destroy(Ia64I2000State *s) {
    if (s) gemu_audio_close(s->audio_out);
    if (!s)
        return;
    i2000_nvram_save(s);
    gemu_display_destroy(s->display);
    if (s->monitor) gemu_monitor_destroy(s->monitor);
    merced_destroy(s->cpu);
    if (i2000_net) {
        gemu_nic_set_link_cb(i2000_net, NULL, NULL);
        i2000_net = NULL;
    }
    gemu_net_shutdown();
    free(s->nic_rom);
    atapi_cdrom_free(&s->cd);
    /* The drive table owns the drives; just stop them calling back here. */
    gemu_drive_set_change_cb(s->cdrom, NULL, NULL);
    free(s->video_rom);
    free(s->vram);
    free(s->flash);
    free(s->ram);
    free(s);
}

/* ── Execution ───────────────────────────────────────────────────────────── */

static void i2000_poll_interrupts(Ia64I2000State *s) {
    ins8250_tick(&s->uart, 1);
    if (s->audio.model) {
        pci_audio_poll(&s->audio);
        if (s->audio.irq_level) iosapic_raise_gsi(s, 45);
    }
    if (s->nic_enabled)
        e100_poll(&s->nic);
    if (s->mouse_wait_rfi_generation &&
        merced_rfi_generation() >= s->mouse_wait_rfi_generation) {
        s->mouse_wait_rfi_generation = 0;
        s->mouse_retry_ninsts = s->cpu->ninsts + MOUSE_RETRY_DELAY_INSTS;
        if (s->aux_out_pos >= s->aux_out_len) {
            unsigned rate = s->mouse_sample_rate ? s->mouse_sample_rate : 100;
            uint64_t interval = I2000_ITC_HZ / rate;
            if (interval < 5000) interval = 5000;
            s->mouse_next_report_itc = merced_get_time_base(s->cpu) + interval;
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
    i2000_pit_run(s);
    /* Controller replies and host keystrokes assert legacy IRQ1 whenever
     * the keyboard output buffer becomes non-empty. Previously we queued
     * the byte but never signalled the CPU, leaving firmware event waits in
     * CdbPim blocked with an unread 8042 byte. Keep the latch asserted until
     * the firmware performs its ExtINT acknowledge; if more bytes remain,
     * the next poll naturally reasserts it. */
    bool kbd_data = s->kbc_out_pos < s->kbc_out_len ||
                    ps2_keyboard_has_data(&s->ps2_keyboard);
    uint64_t kbd_rte = s->chipset.iosapic_rte[1];
    bool kbd_fixed = !(kbd_rte & I460GX_IOSAPIC_RTE_MASK) &&
                     ((kbd_rte >> 8) & 7) <= 1;
    if (kbd_fixed) {
        /* Windows' i8042 driver takes IRQ1 through the SAPIC vector it
         * programs into GSI1, not the firmware's legacy ExtINT thunk (the
         * PIC is masked by then).  Without this, keystrokes queued in the
         * 8042 were never signalled and text-mode Setup ignored Enter. */
        if (kbd_data && !s->keyboard_gsi_asserted) {
            iosapic_raise_gsi(s, 1);
            s->keyboard_gsi_asserted = true;
        }
    } else if (kbd_data &&
               (kbd_rte == I460GX_IOSAPIC_RTE_MASK ||
                (!(kbd_rte & I460GX_IOSAPIC_RTE_MASK) && ((kbd_rte >> 8) & 7) == 7)) &&
               s->cpu->ninsts >= s->keyboard_retry_ninsts &&
               !(s->pic.master.imr & 0x02)) {
        /* The firmware's reset-RTE (or explicit ExtINT) legacy path.  An OS
         * that masks its own GSI1 vector must not get IRQ1 via the PIC. */
        s->keyboard_irq_pending = true;
        i2000_pic_raise(s);
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
    uint64_t aux_rte = s->chipset.iosapic_rte[12];
    bool aux_fixed = !(aux_rte & I460GX_IOSAPIC_RTE_MASK) &&
                     ((aux_rte >> 8) & 7) <= 1;
    if (aux_fixed) {
        /* Windows' i8042 driver takes IRQ12 through its own GSI12 vector,
         * like IRQ1 above; the PIC is masked by then.  Without this, the
         * first aux byte (e.g. the mouse's reset reply) was never signalled
         * and, sitting first in the shared output buffer, also starved the
         * keyboard.  The 8042 only drives IRQ12 with command-byte INT2 set. */
        if (s->aux_out_pos < s->aux_out_len && (s->kbc_command_byte & 0x02) &&
            !s->mouse_gsi_asserted) {
            iosapic_raise_gsi(s, 12);
            s->mouse_gsi_asserted = true;
        }
    } else if (s->aux_out_pos < s->aux_out_len &&
        s->cpu->ninsts >= s->mouse_retry_ninsts &&
        s->mouse_wait_rfi_generation == 0 &&
        !(s->pic.slave.imr & 0x10) && !(s->pic.master.imr & 0x04)) {
        s->mouse_irq_pending = true;
        i2000_pic_raise(s);
    }
    /* SCI is level-triggered: present it through its I/O SAPIC entry again
     * after each acknowledgement until the handler clears the source. */
    if (s->sci_level && s->cpu->ninsts >= s->sci_retry_ninsts) {
        unsigned gsi = (unsigned)s->sci_irq;
        uint8_t vector;
        bool extint;
        if (i460gx_interrupt(&s->chipset, gsi, &vector, &extint) && !extint &&
            !merced_external_pending(s->cpu, vector)) {
            iosapic_raise_gsi(s, gsi);
            s->sci_retry_ninsts = s->cpu->ninsts + SCI_REPRESENT_INSTS;
        }
    }
    /* IRQ8 stays asserted until the guest reads register C.  Through a
     * level-triggered I/O SAPIC entry (Windows) it is presented again after
     * each acknowledgement while it does; an edge-triggered entry (HelenOS
     * programs the legacy lines that way) sees one interrupt per rising
     * edge, so a guest that never reads register C gets exactly one. */
    if (s->rtc_irq && s->cpu->ninsts >= s->rtc_retry_ninsts) {
        uint8_t vector;
        bool extint;
        if (i460gx_interrupt(&s->chipset, 8, &vector, &extint) && !extint) {
            if (i460gx_gsi_level(&s->chipset, 8)) {
                if (!merced_external_pending(s->cpu, vector)) {
                    iosapic_raise_gsi(s, 8);
                    s->rtc_retry_ninsts = s->cpu->ninsts + SCI_REPRESENT_INSTS;
                }
            } else if (i2000_rtc_edge_pending) {
                iosapic_raise_gsi(s, 8);
                i2000_rtc_edge_pending = false;
            }
        }
    }
    /* Primary IDE (the -hda disk) uses GSI14; see ata_deliver_irq(). */
    if (s->ata_legacy_irq_latched)
        i2000_pic_raise(s);
    ata_deliver_irq(s);
    /* Secondary IDE uses GSI15. Windows installs a fixed SAPIC vector;
     * firmware leaves that entry masked and uses its legacy ExtINT thunk.
     * Device-control nIEN masks the output without discarding completion. */
    atapi_cdrom_poll(&s->cd);
    /* A PIC edge survives a device Status read until the legacy INTA
     * replacement cycle acknowledges it. Keep that ownership separately
     * from the device's INTRQ latch. */
    if (s->atapi_legacy_irq_latched)
        i2000_pic_raise(s);
    if (ide_intrq(&s->ide_secondary)) {
        uint64_t rte = s->chipset.iosapic_rte[15];
        uint32_t epoch = atapi_cdrom_irq_epoch(&s->cd);
        if (!(rte & I460GX_IOSAPIC_RTE_MASK) && ((rte >> 8) & 7) <= 1) {
            if (s->atapi_irq_delivered != epoch) {
                iosapic_raise_gsi(s, 15);
                s->atapi_irq_delivered = epoch;
            }
        } else if (rte == I460GX_IOSAPIC_RTE_MASK ||
                   (!(rte & I460GX_IOSAPIC_RTE_MASK) && ((rte >> 8) & 7) == 7)) {
            /* Preserve the firmware's reset-RTE ExtINT path. Masking a
             * configured OS vector must not fall back to a legacy IRQ, and
             * a PIC-masked IRQ15 isn't delivered either: EFI masks it and
             * polls the CD. */
            if (i2000_pic_irq_open(s, 15)) {
                s->atapi_legacy_irq_latched = true;
                i2000_pic_raise(s);
            }
        }
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
    /* A fingerprint of the machine at the target, so a speed-up can be
     * checked to leave emulation exactly as it was: same IP, same RAM.
     * (Registers aren't hashed: guest code copies AR.ITC, which follows
     * host time, into them.) */
    const Merced *m = s->cpu;
    uint64_t ram_h = UINT64_C(0xcbf29ce484222325);
    for (uint64_t i = 0; i + 8 <= s->ram_size; i += 8) {
        uint64_t w;
        memcpy(&w, s->ram + i, 8);
        ram_h = (ram_h ^ w) * UINT64_C(0x100000001b3);
    }
    printf("gemu-bench: window=[%" PRIu64 ",%" PRIu64 "] ninsts_delta=%"
           PRIu64 " elapsed=%.3fs slots/sec=%.0f bundle-equiv/sec=%.0f"
           " ip=%016" PRIX64 " ram=%016" PRIX64 "\n",
           window_start_ninsts, s->cpu->ninsts, ninsts_delta, elapsed_s,
           slots_per_sec, slots_per_sec / 3.0, m->ip, ram_h);
    fflush(stdout);
    exit(0);
}

/* After a slot (or a run of them): carry out a platform reset the slot
 * requested, or stop on a halt.  Returns false when the slice must end. */
/* Idle skipping.  Windows idles in a busy loop (KiIdleLoop polls its DPC
 * queue and next thread; HalProcessorIdle only enables interrupts), so a
 * waiting guest burns host time at wall-clock speed.  A run slice that wrote
 * no memory and ended in an architectural state already seen at the end of
 * another such slice is in a loop that only an interrupt can end; time then
 * moves straight on to the guest's next timer deadline.  Guest time runs
 * ahead of the host while it idles, so this is opt-in: GEMU_IDLE_SKIP=1. */
/* GEMU_PROFILE=1: once per host second, the slot rate and how many poll
 * quanta were spent in an idle loop (as i2000_idle_check() detects one). */
static int i2000_profile_on = -1;
static uint64_t prof_quanta, prof_idle;

/* Guest IP histogram, 64 KiB buckets, sampled once per poll quantum. */
#define PROF_HIST 4096
static struct { uint64_t key, n; } prof_hist[PROF_HIST];

static void i2000_profile_sample(uint64_t ip) {
    uint64_t key = (ip >> 16) | 1;
    unsigned h = (unsigned)((key * UINT64_C(0x9E3779B97F4A7C15)) >> 52);
    for (unsigned k = 0; k < PROF_HIST; k++, h = (h + 1) & (PROF_HIST - 1)) {
        if (prof_hist[h].key == key) { prof_hist[h].n++; return; }
        if (!prof_hist[h].key) { prof_hist[h].key = key; prof_hist[h].n = 1; return; }
    }
}

static void i2000_profile_dump(double t) {
    uint64_t total = 0;
    for (unsigned i = 0; i < PROF_HIST; i++) total += prof_hist[i].n;
    if (!total) return;
    fprintf(stderr, "profile-ip: t=%6.1f", t);
    for (int k = 0; k < 6; k++) {
        unsigned best = 0;
        for (unsigned i = 1; i < PROF_HIST; i++)
            if (prof_hist[i].n > prof_hist[best].n) best = i;
        if (!prof_hist[best].n) break;
        fprintf(stderr, " %012" PRIX64 "xxxx:%.0f%%", prof_hist[best].key >> 0 & ~UINT64_C(1),
                100.0 * (double)prof_hist[best].n / (double)total);
        prof_hist[best].n = 0;
    }
    fprintf(stderr, "\n");
    memset(prof_hist, 0, sizeof(prof_hist));
}

static void i2000_profile_tick(Ia64I2000State *s) {
    static uint64_t last_ns, last_insts;
    static unsigned ticks;
    uint64_t now = i2000_monotonic_ns();
    if (!last_ns) { last_ns = now; last_insts = s->cpu->ninsts; return; }
    if (now - last_ns < UINT64_C(1000000000)) return;
    static uint64_t start_ns;
    if (!start_ns) start_ns = last_ns;
    double dt = (double)(now - last_ns) / 1e9;
    fprintf(stderr, "profile: t=%6.1f %7.2f Mslots/s idle %5.1f%% ip=%016" PRIX64 "\n",
            (double)(now - start_ns) / 1e9,
            (double)(s->cpu->ninsts - last_insts) / dt / 1e6,
            prof_quanta ? 100.0 * (double)prof_idle / (double)prof_quanta : 0.0,
            s->cpu->ip);
    if (++ticks % 10 == 0)
        i2000_profile_dump((double)(now - start_ns) / 1e9);
    last_ns = now;
    last_insts = s->cpu->ninsts;
    prof_quanta = prof_idle = 0;
}

static void i2000_idle_check(Ia64I2000State *s) {
    static int enabled = -1;
    static uint64_t dirty_mark, seen[16];
    static unsigned n_seen;
    static uint64_t skips;
    static bool last_idle;
    if (enabled < 0) {
        const char *e = getenv("GEMU_IDLE_SKIP");
        enabled = e && strcmp(e, "0") != 0;
        e = getenv("GEMU_PROFILE");
        i2000_profile_on = e && strcmp(e, "0") != 0;
    }
    if (!enabled && !i2000_profile_on) return;
    Merced *cpu = s->cpu;
    if (i2000_profile_on) {
        /* Windows Server 2003 IA-64's KiIdleLoop (ntoskrnl 5.2.3790). */
        prof_quanta++;
        i2000_profile_sample(cpu->ip);
        prof_idle += cpu->ip >= UINT64_C(0xE000000083009500) &&
                     cpu->ip <  UINT64_C(0xE000000083009800);
        i2000_profile_tick(s);
    }
    uint64_t dirty = merced_idle_dirty(cpu);
    if (dirty != dirty_mark || cpu->ext_pending_count || cpu->timer_pending) {
        dirty_mark = dirty;
        n_seen = 0;
        last_idle = false;
        return;
    }
    uint64_t h = merced_state_hash(cpu);
    bool repeat = false;
    for (unsigned i = 0; i < n_seen && i < 16; i++) repeat |= seen[i] == h;
    if (!repeat) {
        seen[n_seen++ % 16] = h;
        return;
    }
    n_seen = 0;
    last_idle = true;
    if (!enabled) return;
    /* The interval timer must be armed, unmasked and still ahead. */
    uint64_t itm = cpu->cr[1], itv = cpu->cr[72], itc = merced_get_itc(cpu);
    if (!itm || (itv & (UINT64_C(1) << 16)) || (int64_t)(itm - itc) <= 0) return;
    uint64_t delta = itm - itc;
    if (delta > I2000_ITC_HZ) delta = I2000_ITC_HZ;   /* never more than 1 s at once */
    merced_advance_itc(cpu, delta);
    if (getenv("GEMU_IDLE_DEBUG") && ++skips % 1000 == 0)
        fprintf(stderr, "i2000: idle skips %" PRIu64 " (last %" PRIu64 " ticks at %016" PRIX64 ")\n",
                skips, delta, cpu->ip);
}

static bool i2000_after_step(Ia64I2000State *s, MercedStatus st) {
    if (s->reset_requested) {
        fprintf(stderr, "i2000: firmware requested a platform reset\n");
        if (s->reset_requested == 1) {
            /* Reset the platform while retaining the chipset's bootstrap
             * latches, as a CF9 system reset does on this board. */
            i2000_reset(s, true);
        } else {
            mc146818_clock_restart(&s->rtc);
            i2000_sample_ratio_latch(s);
            merced_reset(s->cpu);
        }
        /* SAC CBNR bit 0 selects the second bootstrap phase, with the
         * firmware range shadowed in RAM: the flash image is copied to
         * 0x03C00000 and the top-of-4GiB window becomes RAM-backed for
         * reads AND writes (the firmware's data segment lives up there
         * - its physical-mode stores via the 0xFFFDxxxx alias must
         * stick, or the SAL descriptors stay empty). */
        s->fw_shadow_enabled = true;
        if (I2000_FW_SHADOW_BASE + I2000_FLASH_SIZE <= s->ram_size)
            memcpy(s->ram + I2000_FW_SHADOW_BASE, s->flash,
                   I2000_FLASH_SIZE);
        s->halted = false;
        s->reset_requested = false;
        s->mmio_log_n = 0;
        memset(s->mmio_log, 0, sizeof(s->mmio_log));
        printf("i2000: processor reset, IP=0x%016" PRIX64 "\n",
               s->cpu->ip);
        return false;
    }
    if (st != MERCED_OK) {
        s->halted = true;
        s->halt_status = st;
        i2000_report_halt(s);
        return false;
    }
    return true;
}

static void i2000_run_slice(Ia64I2000State *s) {
    if (s->halted || s->cpu_stopped)
        return;
    bool check_exec = gemu_monitor_has_exec_breakpoints(s->monitor);
    bool check_mem = gemu_monitor_has_mem_breakpoints(s->monitor);
    s->mem_watch = check_mem;
    for (int i = 0; i < INSTR_PER_FRAME; ) {
        /* Ordinary execution: one interrupt-poll quantum per call into the
         * CPU's fast path.  It stops after a slot that requests a reset
         * (merced_request_exit()).  The slot-by-slot loop below still runs
         * whenever a breakpoint or watchpoint is set, and finishes a quantum
         * the fast path left. */
        if (!check_exec && !check_mem &&
            (i & (irq_poll_quantum - 1)) == 0 &&
            i + (int)irq_poll_quantum <= INSTR_PER_FRAME) {
            i2000_poll_interrupts(s);
            unsigned n = 0;
            MercedStatus st = merced_run_fast_range(
                s->cpu, irq_poll_quantum, &n, 0, 0, 0, 0);
            i += (int)n;
            if (st == MERCED_OK)
                i2000_idle_check(s);
            if (!i2000_after_step(s, st))
                return;
            continue;
        }
        if (check_exec &&
            gemu_monitor_check_exec(s->monitor, (uint32_t)s->cpu->ip))
            return;
        /* Device callbacks raise already-ready interrupts directly.  Only
         * deadline-driven sources (PIT and retry timers) need this platform
         * poll, and checking those on every IA-64 slot was a large fraction
         * of execution time.  A 16-slot quantum is far below the latency of
         * the modeled hardware while bounding delivery skew to 15 slots. */
        if ((i & (irq_poll_quantum - 1)) == 0)
            i2000_poll_interrupts(s);
        MercedStatus st = merced_step(s->cpu);
        i++;
        /* A memory watchpoint can be raised from inside the bus callback.
         * Stop this execution slice immediately so the reported IP remains
         * the instruction that performed the access. */
        if (check_mem && gemu_monitor_is_paused(s->monitor))
            return;
        if (!i2000_after_step(s, st))
            return;
    }
}

static void i2000_reset(Ia64I2000State *s, bool warm) {
    bool mouse_enabled = s->mouse_enabled;
    /* The device tail mixes resettable state with persistent flash
     * bookkeeping. Retain it across the bulk reset below: losing it discards
     * pending NVRAM updates. */
    char nvram_path[sizeof(s->nvram_path)];
    memcpy(nvram_path, s->nvram_path, sizeof(nvram_path));
    bool nvram_dirty = s->nvram_dirty;
    mc146818_clock_restart(&s->rtc);        /* the RTC keeps running */
    i2000_sample_ratio_latch(s);
    merced_reset(s->cpu);
    if (s->video_enabled)
        rage128_reset(&s->rage128);
    if (s->nic_enabled)
        e100_reset(&s->nic);
    pci_audio_reset(&s->audio);
    gemu_audio_flush(s->audio_out);
    memset(&s->mouse_enabled, 0,
           offsetof(Ia64I2000State, chipset) -
           offsetof(Ia64I2000State, mouse_enabled));
    memcpy(s->nvram_path, nvram_path, sizeof(s->nvram_path));
    s->nvram_dirty = nvram_dirty;
    i2000_update_fast_ram(s);
    ps2_keyboard_init(&s->ps2_keyboard);
    /* mouse_enabled is a CLI-configured setting (-device mouse), not
     * protocol state - a reset should clear the aux device's transient
     * state (queue, streaming flag, etc, all zeroed by the memset above)
     * without un-attaching the mouse itself. */
    s->mouse_enabled = mouse_enabled;
    s->halted = false;
    s->reset_requested = false;
    s->fw_shadow_enabled = false;
    s->post_code = 0;
    s->legacy_irq_routed = false;
    i2000_pic_reset(s);
    i2000_devices_reset(s, warm);
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
    if (s->cdrom) {
        if (gemu_drive_inserted(s->cdrom))
            printf("  CD-ROM: %s: %s (%" PRIu64 " MiB)\n",
                   gemu_drive_name(s->cdrom), gemu_drive_filename(s->cdrom),
                   gemu_drive_size(s->cdrom) >> 20);
        else
            printf("  CD-ROM: %s: (empty)\n", gemu_drive_name(s->cdrom));
    }
    if (s->hda)
        printf("  HDD   : %s: %s (%" PRIu64 " MiB)\n",
               gemu_drive_name(s->hda), gemu_drive_filename(s->hda),
               gemu_drive_size(s->hda) >> 20);

    gemu_monitor_start(s->monitor);
    i2000_update_fast_ram(s);

    /* The ITC follows elapsed host time, so a firmware 10 ms input timer
     * takes 10 ms however slowly the guest runs.  It also counts within a
     * frame, as real hardware's does every cycle: the CPU charges the
     * per-slot cycles (merced_set_cycles_per_slot()), and each frame tops
     * the platform time base up to host time when the guest is behind it.
     * PAL's AR self-test writes ar.itc and expects a later read to have
     * moved on; an ITC that only advanced between frames failed it and
     * sent C0 firmware into its dead loop. */
    merced_set_external_itc(s->cpu, false);
    uint64_t itc_last_ns = i2000_monotonic_ns();
    uint64_t itc_fraction = 0;
    uint64_t itc_target = merced_get_time_base(s->cpu);
    uint64_t itc_seen = itc_target;
    uint32_t display_keys_held = 0;

    /* A benchmark (GEMU_BENCH_NINSTS) runs the ITC off the instruction
     * count instead, so the guest takes the same path however fast the
     * host is and its end-of-run fingerprint can be compared across
     * builds (with GEMU_FIXED_TIME pinning the RTC as well). */
    uint64_t bench_warmup, bench_target;
    i2000_bench_targets(&bench_warmup, &bench_target);
    /* GEMU_CPI=<n> paces the ITC by the instruction count instead of the
     * host clock, charging n ticks per executed slot. The guest then takes
     * the same path however fast the host is, and firmware that polls the ITC
     * for a delay no longer burns real time (and a great many instructions)
     * waiting for it. */
    const char *cpi_text = getenv("GEMU_CPI");
    unsigned cpi = cpi_text ? (unsigned)strtoul(cpi_text, NULL, 0) : 0;
    merced_set_cycles_per_slot(s->cpu, cpi ? cpi : 1);
    bool host_itc = bench_target == 0 && !cpi;

    bool running = true;
    while (running) {
        /* loadvm restores this CPU flag too, so reassert machine policy.
         * Paused time must not become one enormous timer jump on resume. */
        merced_set_external_itc(s->cpu, false);
        uint64_t now_ns = i2000_monotonic_ns();
        uint64_t time_base = merced_get_time_base(s->cpu);
        if (time_base < itc_seen)           /* loadvm or reset: re-anchor */
            itc_target = time_base;
        itc_seen = time_base;
        if (!host_itc) {
            itc_last_ns = now_ns;
        } else if (gemu_monitor_is_paused(s->monitor) || s->halted) {
            itc_last_ns = now_ns;
            itc_target = time_base;
        } else {
            /* Exact ticks for the elapsed host time, carrying the remainder
             * (in units of 1/10^9 tick). The span is bounded so the product
             * cannot overflow. */
            uint64_t elapsed_ns = now_ns - itc_last_ns;
            if (elapsed_ns > UINT64_C(10000000000))
                elapsed_ns = UINT64_C(10000000000);
            itc_fraction += elapsed_ns * I2000_ITC_HZ;
            uint64_t ticks = itc_fraction / UINT64_C(1000000000);
            itc_fraction %= UINT64_C(1000000000);
            itc_last_ns = now_ns;
            /* Host time owed beyond what the slots already charged; a guest
             * running ahead of real time owes nothing and carries no debt. */
            itc_target += ticks;
            if (time_base < itc_target)
                merced_advance_itc(s->cpu, itc_target - time_base);
            else
                itc_target = time_base;
            itc_seen = merced_get_time_base(s->cpu);
        }
        if (s->display) {
            uint32_t keys_held = gemu_display_poll(s->display);
            uint32_t keys_changed = keys_held ^ display_keys_held;
            for (unsigned i = 0;
                 i < sizeof(i2000_keyboard_keysyms) /
                     sizeof(i2000_keyboard_keysyms[0]); i++) {
                uint32_t bit = GEMU_ACTION(i);
                if (keys_changed & bit) {
                    ps2_keyboard_key(&s->ps2_keyboard,
                                     i2000_keyboard_keysyms[i],
                                     (keys_held & bit) != 0);
                    if (keys_held & bit)
                        i2000_efi_bios_ext_key(s, i2000_keyboard_keysyms[i]);
                }
            }
            display_keys_held = keys_held;
            uint32_t cp;
            while ((cp = gemu_display_pop_raw_key(s->display)) != 0) {
                if (cp <= 0x7F) {
                    i2000_type_key(s, cp);
                    /* An uppercase character carries its own Shift make/
                     * break; re-assert a Shift the user is still holding. */
                    if (keys_held & GEMU_ACTION(I2000_KEY_LSHIFT))
                        ps2_keyboard_key(&s->ps2_keyboard, 0xffe1, true);
                    if (keys_held & GEMU_ACTION(I2000_KEY_RSHIFT))
                        ps2_keyboard_key(&s->ps2_keyboard, 0xffe2, true);
                }
            }
            if (gemu_display_should_quit(s->display)) {
                if (cfg->no_shutdown) gemu_display_clear_flags(s->display);
                else running = false;
            }
            if (gemu_display_reset_requested(s->display)) {
                i2000_reset(s, false);
                gemu_display_clear_flags(s->display);
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
                (int64_t)(merced_get_time_base(s->cpu) -
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

        /* Monitor commands run inside gemu_monitor_poll(); snapshot I/O
         * (savevm/loadvm) can block for several seconds with 2 GiB of guest
         * RAM.  That host maintenance time is not guest time: counting it
         * on the next loop can expire an EFI menu timer immediately after
         * loadvm/savevm returns, so exclude it from the ITC below. */
        uint64_t mon_start_ns = i2000_monotonic_ns();
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            switch (cmd) {
            case GEMU_MON_QUIT:
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { running = false; }
                break;
            case GEMU_MON_RESET:
                i2000_reset(s, false);
                break;
            case GEMU_MON_STEP: {
                uint32_t n = gemu_monitor_step_count(s->monitor);
                if (n == 0) n = 1;
                s->halted = false;
                s->mem_watch = gemu_monitor_has_mem_breakpoints(s->monitor);
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
            default:
                break;
            }
            if (!running)
                break;
        }
        itc_last_ns += i2000_monotonic_ns() - mon_start_ns;
        if (!running)
            break;

        if (!gemu_monitor_is_paused(s->monitor)) {
            i2000_run_slice(s);
            i2000_rage_dma_poll(s);
            i2000_bench_check(s);
            i460gx_pm_poll(&s->chipset);
            mc146818_poll(&s->rtc);
            if (mc146818_take_dirty(&s->rtc))
                s->nvram_dirty = true;
        }
        if (s->wake_reset_requested) {
            /* A wake event from S3/S4/S5 powers the machine up and resets it. */
            s->wake_reset_requested = false;
            s->power_off_requested = false;
            fprintf(stderr, "i2000: waking from a power-off state\n");
            i2000_reset(s, false);
        }
        if (s->power_off_requested) {
            s->power_off_requested = false;
            fprintf(stderr, "i2000: the guest powered the machine off\n");
            if (cfg->no_shutdown)
                gemu_monitor_shutdown_or_pause(s->monitor, true);
            else
                running = false;
        }

        /* A slice is ~1-2 ms of guest time: draw at 60 Hz, not per slice,
         * and only sleep when the CPU isn't running. */
        bool idle = s->halted || s->cpu_stopped ||
                    gemu_monitor_is_paused(s->monitor);
        if (s->display) {
            static uint64_t last_frame_ns;
            uint64_t frame_ns = i2000_monotonic_ns();
            if (idle || frame_ns - last_frame_ns >= UINT64_C(16666667)) {
                last_frame_ns = frame_ns;
                gemu_display_set_paused(s->display, idle);
                i2000_render_frame(s);
                gemu_display_set_size(s->display, (int)i2000_pic_w,
                                      (int)i2000_pic_h, 0, 0);
                gemu_display_render(s->display, s->fb, (int)i2000_pic_w,
                                    (int)i2000_pic_h);
            }
        }
        if (idle)
            gemu_sleep_ms(30);
    }
    gemu_monitor_stop(s->monitor);
}
