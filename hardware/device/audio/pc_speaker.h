#pragma once
/* The PC's speaker as a core device (type "pc-speaker", stage D5).
 *
 * Inputs: timer (PIT channel 2's output) and data (PPI port B bit 1); the
 * cone follows timer AND data, so a PIT square wave, a program toggling
 * PB1 by hand (clicks, buzzes, PWM sound) and anything in between all
 * sound as they do on the machine.  A stream (option rate, default 192 kHz)
 * carries the cone's position through a DC-blocking filter (the speaker's
 * coupling), updated to the moment of every edge. */
#include "gemu/dev/device.h"

extern const GemuDeviceType gemu_pc_speaker_type;
