#pragma once
/*
 * A gated square-wave tone as a core device (stage R1): the RCA machines'
 * beeper, Q turning a fixed-pitch tone on and off.  Its stream ("sound",
 * mono, 44.1 kHz) is a square wave at the set frequency while the gate is
 * on, silence otherwise.  A change of gate or frequency first brings the
 * stream up to the machine's time, so it lands on the sample it happened at
 * (it replaces audio/pcspk.c, an SDL callback that played whatever the
 * gate was when it ran).  The level is pcspk's, +-6000 of 32767.
 * Option "freq" (Hz) is the pitch at power-on.
 */
#include "gemu/dev/device.h"
#include <stdbool.h>
#include <stdint.h>

extern const GemuDeviceType gemu_tone_type;

void gemu_tone_set_gate(GemuDevice *d, bool on);
void gemu_tone_set_freq(GemuDevice *d, unsigned hz);
