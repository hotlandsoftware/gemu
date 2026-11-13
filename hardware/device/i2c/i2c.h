#pragma once

/* An I2C bus segment: the devices attached to it and the byte-level
 * transactions a bus master runs (START + address, data bytes, STOP).
 * Timing is not modelled; every byte completes at once. Set I2C_DEBUG to
 * trace transactions and to report each address that nothing answers. */

#include <stdbool.h>
#include <stdint.h>

typedef struct I2cDevice {
    uint8_t addr;                                   /* 7-bit address */
    void *ctx;
    /* Addressed after a START; `read` is the R/W bit. True = ACK. */
    bool (*start)(void *ctx, bool read);
    /* A byte from the master. True = ACK. */
    bool (*write)(void *ctx, uint8_t byte);
    /* A byte to the master, who ACKs it or ends the transfer with a NACK. */
    uint8_t (*read)(void *ctx, bool ack);
    void (*stop)(void *ctx);
} I2cDevice;

enum { I2C_MAX_DEVICES = 24 };

typedef struct I2cBus {
    const char *name;
    I2cDevice dev[I2C_MAX_DEVICES];
    unsigned ndev;
    I2cDevice *cur;         /* the device addressed; NULL if none ACKed */
    uint8_t reported[16];   /* bitmap of absent addresses already reported */
} I2cBus;

void i2c_bus_init(I2cBus *bus, const char *name);
/* False when the bus is full. */
bool i2c_attach(I2cBus *bus, const I2cDevice *device);

/* START (or repeated START) and the address byte. True if a device ACKed. */
bool i2c_start(I2cBus *bus, uint8_t addr7, bool read);
bool i2c_write(I2cBus *bus, uint8_t byte);
/* With nothing driving SDA the pull-up reads all ones. */
uint8_t i2c_read(I2cBus *bus, bool ack);
void i2c_stop(I2cBus *bus);

/* Whether I2C_DEBUG is set; shared by the devices that report unmodelled
 * accesses. */
bool i2c_debug_enabled(void);
