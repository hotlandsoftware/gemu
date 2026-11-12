#pragma once

#include <stdbool.h>
#include <stdint.h>

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
 * SPD reads return false when no EEPROM acknowledges the selected card, row
 * and slave. APM commands are delivered after the command latch is updated,
 * allowing board firmware handshakes. */
typedef struct I460GXConfig {
    unsigned ifb_bus, ifb_dev;
    unsigned cbn_bus, cbn_dev;
    unsigned link_count;
    I460GXBridge expander[8];
    uint64_t clock_hz;
    uint64_t (*clock)(void *opaque);
    bool (*spd_read)(void *opaque, unsigned card, unsigned row,
                     unsigned slave, unsigned reg, uint8_t *value);
    unsigned smbus_card, smbus_row;
    void (*apm_write)(void *opaque, uint8_t value);
    void *opaque;
    /* Cached by init; not guest state. */
    bool smbus_debug, iosapic_debug;
} I460GXConfig;

/* Intel 460GX register model: SAC/SDC and expander configuration, memory-card
 * I2C access, IFB PCI/SMBus/PM/UHCI, and the I/O SAPIC. RAM, attached devices,
 * legacy interrupt wiring, and firmware storage are provided by the board. */
typedef struct I460GX {
    uint32_t sac_cbnr, sac_ccsr;
    uint32_t iosapic_regsel;
    uint64_t iosapic_rte[64];
    uint8_t ifb_cfg[256], ifb_usb_cfg[256], ifb_smbus_cfg[256];
    uint8_t smb_hststs, smb_hstcnt, smb_hstcmd, smb_hstadd;
    uint8_t smb_hstdat0, smb_hstdat1;
    uint8_t chipset_bus;
    uint8_t chipset_cfg[32][8][256];
    uint8_t memcard_cfg[2][8][256];
    uint64_t acpi_pm[8];
    uint8_t apm_control, apm_status;
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
