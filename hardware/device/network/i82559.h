#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define I82559_PCI_VENDOR_ID 0x8086u
#define I82559_PCI_DEVICE_ID 0x1229u
#define I82559_MMIO_SIZE     0x1000u
#define I82559_IO_SIZE       0x40u

typedef uint64_t (*I82559GuestRead)(void *opaque, uint64_t addr,
                                   unsigned size);
typedef void (*I82559GuestWrite)(void *opaque, uint64_t addr, uint64_t value,
                                unsigned size);

/* Minimal Intel 82559 10/100 Base-T controller. Packet transport
 * is intentionally absent; this implements the PCI, SCB, PHY and EEPROM
 * surfaces required by firmware's E100B UNDI driver. */
typedef struct I82559 {
    bool enabled;
    uint8_t cfg[256];
    uint8_t regs[I82559_MMIO_SIZE];
    uint16_t eeprom[64];
    uint16_t phy[32];
    uint32_t scb_pointer;
    uint32_t cu_base;
    uint32_t ru_base;
    uint32_t mdi;
    uint16_t ee_shift;
    uint8_t ee_bits;
    uint8_t ee_out_bits;
    bool ee_loaded;
    uint16_t ee_out;
    uint8_t ee_control;
    I82559GuestRead guest_read;
    I82559GuestWrite guest_write;
    void *guest_opaque;
    uint8_t *option_rom;
    size_t option_rom_size;
} I82559;

void i82559_init(I82559 *n, bool enabled, uint32_t mmio_base,
                 uint32_t io_base, uint8_t irq, I82559GuestRead read_cb,
                 I82559GuestWrite write_cb, void *opaque);
uint64_t i82559_pci_read(const I82559 *n, unsigned reg, unsigned size);
void i82559_pci_write(I82559 *n, unsigned reg, uint64_t value, unsigned size);
uint32_t i82559_bar(const I82559 *n, unsigned reg, uint32_t fallback,
                    uint32_t mask);
uint64_t i82559_reg_read(I82559 *n, unsigned reg, unsigned size);
void i82559_reg_write(I82559 *n, unsigned reg, uint64_t value, unsigned size);
bool i82559_load_option_rom(I82559 *n, const char *path);
void i82559_destroy(I82559 *n);
