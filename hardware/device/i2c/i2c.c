#include "i2c/i2c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool i2c_debug_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("I2C_DEBUG") != NULL;
    return enabled;
}

void i2c_bus_init(I2cBus *bus, const char *name) {
    memset(bus, 0, sizeof(*bus));
    bus->name = name;
}

bool i2c_attach(I2cBus *bus, const I2cDevice *device) {
    if (bus->ndev >= I2C_MAX_DEVICES)
        return false;
    bus->dev[bus->ndev++] = *device;
    return true;
}

static I2cDevice *find_device(I2cBus *bus, uint8_t addr7) {
    for (unsigned i = 0; i < bus->ndev; i++)
        if (bus->dev[i].addr == addr7)
            return &bus->dev[i];
    return NULL;
}

bool i2c_start(I2cBus *bus, uint8_t addr7, bool read) {
    I2cDevice *device = find_device(bus, addr7);
    bus->cur = device && (!device->start || device->start(device->ctx, read))
                   ? device : NULL;
    if (i2c_debug_enabled()) {
        fprintf(stderr, "i2c[%s]: START %#04x %s -> %s\n", bus->name, addr7,
                read ? "read" : "write", bus->cur ? "ACK" : "NACK");
        uint8_t *seen = &bus->reported[addr7 >> 3];
        if (!device && !(*seen & (1u << (addr7 & 7)))) {
            *seen |= (uint8_t)(1u << (addr7 & 7));
            fprintf(stderr, "i2c[%s]: nothing answers at %#04x\n",
                    bus->name, addr7);
        }
    }
    return bus->cur != NULL;
}

bool i2c_write(I2cBus *bus, uint8_t byte) {
    bool ack = bus->cur && bus->cur->write &&
               bus->cur->write(bus->cur->ctx, byte);
    if (i2c_debug_enabled())
        fprintf(stderr, "i2c[%s]:   write %#04x -> %s\n", bus->name, byte,
                ack ? "ACK" : "NACK");
    return ack;
}

uint8_t i2c_read(I2cBus *bus, bool ack) {
    uint8_t value = bus->cur && bus->cur->read
                        ? bus->cur->read(bus->cur->ctx, ack) : 0xff;
    if (i2c_debug_enabled())
        fprintf(stderr, "i2c[%s]:   read %#04x %s\n", bus->name, value,
                ack ? "ACK" : "NACK");
    return value;
}

void i2c_stop(I2cBus *bus) {
    if (bus->cur && bus->cur->stop)
        bus->cur->stop(bus->cur->ctx);
    if (i2c_debug_enabled())
        fprintf(stderr, "i2c[%s]: STOP\n", bus->name);
    bus->cur = NULL;
}
