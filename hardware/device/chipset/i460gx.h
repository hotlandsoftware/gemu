#pragma once

#include "i2c/i2c.h"
#include "storage/fwh82802.h"

#include <stdbool.h>
#include <stdint.h>

/* The IFB's 4 MB firmware window below 4 GiB, and the register space of its
 * four firmware hubs 4 MB lower (SSDM 11.1.27). */
#define I460GX_FLASH_BASE        UINT64_C(0xFFC00000)
#define I460GX_FLASH_SIZE        UINT64_C(0x00400000)
#define I460GX_FWH_REG_BASE      UINT64_C(0xFF800000)
#define I460GX_FWH_PARTS         4u

#define I460GX_SAC_CBNR          UINT64_C(0xFEB00CB0)
#define I460GX_SAC_CCSR          UINT64_C(0xFEB00CC0)
#define I460GX_IOSAPIC_BASE      UINT64_C(0xFEC00000)
#define I460GX_IOSAPIC_SIZE      0x50u
#define I460GX_IOSAPIC_RTE_MASK  (UINT64_C(1) << 16)

typedef enum I460GXBridge {
    I460GX_BRIDGE_NONE,
    I460GX_BRIDGE_PXB,
    I460GX_BRIDGE_WXB,
    I460GX_BRIDGE_GXB,
} I460GXBridge;

/* Wiring and firmware policy belong to the board. The clock callback returns
 * monotonically increasing ticks at clock_hz (at least 1000 Hz when supplied).
 * The board owns the I2C segments and the devices on them: `smbus` is the IFB
 * SMBus segment, and memcard_i2c[card][row] is the segment behind each row
 * select of a memory card, reached through the SAC's I2C translation (NULL
 * where nothing is wired). The buses must outlive the chipset.
 *
 * The IFB's power-management outputs are board wiring, all optional:
 *   smi    SMI# changed level (true = asserted). On a 460GX board this is the
 *          processors' PMI# pin.
 *   sci    SCI changed level on legacy interrupt line `irq` (0-15), the line
 *          the IFB's interrupt routing selects. The board raises it on its
 *          8259 and I/O SAPIC input of that number.
 *   sleep  SLP_EN with a supported SLP_TYP, or a power-button override: the
 *          IFB is now in `state` and waits for a wake event. S3 and S4/S5
 *          remove main power; S1 stops the processors.
 *   wake   a wake event returned the IFB to ON from `from`; WAK_STS is set.
 * Callbacks may re-enter the chipset but must not free it. */
typedef enum I460GXPmState {
    I460GX_PM_ON,           /* S0 */
    I460GX_PM_S1_STPCLK,    /* S1: STPCLK# active, processors in Stop-Grant */
    I460GX_PM_S1_SLP,       /* S1: STPCLK# and SLP# active */
    I460GX_PM_S3,           /* suspend to RAM */
    I460GX_PM_S4S5,         /* suspend to disk / soft off */
} I460GXPmState;

typedef struct I460GXConfig {
    unsigned ifb_bus, ifb_dev;
    unsigned pid_bus, pid_dev;
    unsigned cbn_bus, cbn_dev;
    unsigned link_count;
    I460GXBridge expander[8];
    uint64_t clock_hz;
    uint64_t (*clock)(void *opaque);
    I2cBus *smbus;
    I2cBus *memcard_i2c[2][4];
    /* The board's firmware image: I460GX_FLASH_SIZE bytes, top-aligned. It is
     * the four firmware hubs' arrays, so their programs and erases land in
     * it. NULL leaves the window undecoded. */
    uint8_t *flash;
    void (*smi)(void *opaque, bool asserted);
    void (*sci)(void *opaque, unsigned irq, bool level);
    void (*sleep)(void *opaque, I460GXPmState state, bool power_button_override);
    void (*wake)(void *opaque, I460GXPmState from);
    void *opaque;
    /* Cached by init; not guest state. */
    bool iosapic_debug, pm_debug, fwh_debug;
} I460GXConfig;

/* IFB power management (SSDM 11.2.6-11.2.9, chapter 16): the APM ports, the
 * ACPI and SMI register block and the GPIO block. Only the 3 power wells'
 * survival rules differ between resets; see i460gx_pm.c. */
typedef struct I460GXPm {
    uint16_t pm1_sts, pm1_en, pm1_cnt;
    uint16_t gpe0_sts, gpe0_en, gpe1_sts, gpe1_en;
    uint16_t glb_ctl, glb_sts;
    uint64_t tmr;               /* 3.579545 MHz counts since PCI reset */
    uint8_t apmc, apms;

    /* GPIO registers in GP bit layout. */
    uint32_t gp_out, gp_data, gp_ttl, gp_blink, gp_lock, gp_inv;
    uint32_t gp_smi, gp_pulse, gp_core, gp_pullup;
    uint32_t pulse_latch, in_prev;
    uint32_t ext_mask, ext_level;       /* pins the board drives, and levels */

    /* Time: the chipset clock at the last advance, and the sub-tick
     * remainder in clock_hz units. */
    uint64_t clock_last, clock_rem;
    uint64_t swsmi_left;                /* 8254 ticks to SW SMI, 0 = stopped */
    uint64_t min_acc;                   /* 1MIN timer ticks */
    uint64_t pwrbtn_held;

    bool pwrbtn_down, pwrbtn_overridden;
    bool thrm_connected, thrm_level, thrm_prev;
    bool legacy_usb, bioswen_ble;
    uint8_t state;                      /* I460GXPmState */
    bool sci, smi;                      /* the IFB's SCI and SMI# outputs */
    int8_t sci_irq;                     /* line SCI is driven on; -1 = none */
    bool sci_line;                      /* level driven on sci_irq */
} I460GXPm;

/* GP register layout (SSDM 11.2.9): bits 8:0 are the dedicated GPIO[8:0],
 * bits 19:16 the muxed GPIO[13:10], bits 28:24 the muxed GPIO[22:18]. */
#define I460GX_GPIO_DEDICATED UINT32_C(0x000001FF)
#define I460GX_GPIO_MUXED     UINT32_C(0x1F0F0000)
#define I460GX_GPIO_ALL       UINT32_C(0x1F0F01FF)

/* IFB SMBus host and slave interface registers (SSDM chapter 14). */
typedef struct I460GXSmbus {
    uint8_t hst_sts, slv_sts;
    uint8_t hst_cnt, hst_cmd, hst_add, hst_dat0, hst_dat1;
    uint8_t block[32];
    uint8_t block_index;
    uint8_t slv_cnt, shdw_cmd;
    uint16_t slv_evt, slv_dat;
} I460GXSmbus;

/* Intel 460GX register model: SAC/SDC and expander configuration, memory-card
 * I2C access, IFB PCI/SMBus/PM/UHCI, and the I/O SAPIC with its PID
 * configuration function. RAM, attached devices, legacy interrupt wiring, and
 * firmware storage are provided by the board. */
typedef struct I460GX {
    uint32_t sac_cbnr, sac_ccsr;
    uint32_t iosapic_regsel;
    uint64_t iosapic_rte[64];
    uint8_t pid_cfg[256];
    uint8_t ifb_cfg[256], ifb_usb_cfg[256], ifb_smbus_cfg[256];
    I460GXSmbus smbus;
    uint8_t chipset_bus;
    uint8_t chipset_cfg[32][8][256];
    uint8_t memcard_cfg[2][8][256];
    I460GXPm pm;
    Fwh82802 fwh[I460GX_FWH_PARTS];
    /* Image range programmed or erased since the board last collected it. */
    uint32_t flash_changed_off, flash_changed_len;
    uint64_t uhci_regs[4];
    uint64_t uhci_frame_itc;
    /* Host configuration is deliberately separate from serialized registers. */
    I460GXConfig config;
} I460GX;

void i460gx_init(I460GX *s, const I460GXConfig *config);
/* Both reset forms retain host configuration. CF9 system reset additionally
 * retains the SAC bootstrap scratch and configuration latches. */
void i460gx_reset(I460GX *s);
void i460gx_warm_reset(I460GX *s);
void i460gx_uhci_reset(I460GX *s);

/* Return false when the access belongs to another device. */
bool i460gx_pci_read(I460GX *s, unsigned bus, unsigned dev, unsigned fun,
                     unsigned reg, unsigned size, uint64_t *value);
bool i460gx_pci_write(I460GX *s, unsigned bus, unsigned dev, unsigned fun,
                      unsigned reg, uint64_t value, unsigned size);
bool i460gx_mmio_read(I460GX *s, uint64_t addr, unsigned size, uint64_t *value);
bool i460gx_mmio_write(I460GX *s, uint64_t addr, uint64_t value, unsigned size);

/* Separate IFB functions preserve board I/O decode ordering: function 0 is
 * PM/APM, function 2 is UHCI, and function 3 is SMBus. */
bool i460gx_ifb_io_read(I460GX *s, unsigned fun, uint64_t port,
                        unsigned size, uint64_t *value);
bool i460gx_ifb_io_write(I460GX *s, unsigned fun, uint64_t port,
                         uint64_t value, unsigned size);

/* Decode an unmasked GSI. ExtINT requires the board's legacy PIC vector;
 * otherwise vector is the programmed fixed SAPIC vector. */
bool i460gx_interrupt(I460GX *s, unsigned gsi, uint8_t *vector, bool *extint);

/* Power management inputs and time. i460gx_pm_poll() advances the PM timers
 * to the board clock and re-evaluates SMI#, SCI and wake events; call it
 * regularly. Register accesses advance time themselves. */
void i460gx_pm_poll(I460GX *s);
/* PWRBTN#: a press sets PWRBTN_STS and wakes from any sleep state; held for
 * four seconds it is a power-button override (unconditional S5). */
void i460gx_pm_power_button(I460GX *s, bool pressed);
/* The RTC asserted IRQ8# (an alarm): RTC_STS, and a wake event with RTC_EN. */
void i460gx_pm_rtc_alarm(I460GX *s);
/* Drive the GPIO input pins in `mask` (GP register layout) to `levels`. */
void i460gx_pm_set_gpio_pins(I460GX *s, uint32_t mask, uint32_t levels);

/* Firmware window cycles (FF800000-FFFFFFFF), decoded as the IFB's FWHDE,
 * FWHS and BIOSEN select. Returns false when `addr` is outside the window.
 * An undecoded cycle is master-aborted: reads return all ones. */
bool i460gx_flash_access(I460GX *s, bool write, uint64_t addr, unsigned size,
                         uint64_t *value);
/* The image range the firmware programmed or erased since the last call, as
 * offsets into the board's flash; false when nothing changed. */
bool i460gx_flash_take_changes(I460GX *s, uint32_t *offset, uint32_t *length);
