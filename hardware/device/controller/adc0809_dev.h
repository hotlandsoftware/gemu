#pragma once
/*
 * The ADC0809 8-channel A/D converter as a core device (stage Z1): it owns
 * its Adc0809 and registers its state (the inputs, the latch, a conversion
 * in progress).  The board drives it as before: adc0809_start/read/eoc
 * with the bus cycle count (gemu_adc0809(d)), and sets the inputs.
 */
#include "controller/adc0809.h"
#include "gemu/dev/device.h"

extern const GemuDeviceType gemu_adc0809_type;

Adc0809 *gemu_adc0809(GemuDevice *d);
