#pragma once
/*
 * The MSM5205 ADPCM speech synthesizer as a core device (stage Z1): it
 * owns its Msm5205 and registers its state (the signal, the step, the
 * RESET and 4B/3B pins).  The board clocks it (msm5205_vck, one VCK
 * period with its code) and reads the DAC (msm5205_output); the sound
 * itself is the board's stream, since what follows the DAC (filters, the
 * amplifier) is the board's.
 */
#include "msm5205.h"
#include "gemu/dev/device.h"

extern const GemuDeviceType gemu_msm5205_type;

Msm5205 *gemu_msm5205(GemuDevice *d);
