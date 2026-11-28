#pragma once
/*
 * The MM58167 real-time clock as a core device (stage Z1): it owns its
 * Mm58167 and registers its state (the counters, the RAM, the interrupt
 * registers, the crystal's phase and where it was last brought up to).
 * The board drives it as before (gemu_mm58167(d): mm58167_sync with the
 * bus cycle count, then read/write).
 */
#include "timer/mm58167.h"
#include "gemu/dev/device.h"

extern const GemuDeviceType gemu_mm58167_type;

Mm58167 *gemu_mm58167(GemuDevice *d);
