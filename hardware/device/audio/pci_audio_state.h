#pragma once

/* Save-state registration for the PCI audio controllers (CS4281 and ICH
 * AC'97) and their shared AC'97 codec (state.h). Every item name is `prefix`
 * followed by the member's name ("" is fine). True only if every
 * registration succeeded. */

#include "gemu/dev/state.h"
#include "pci_audio.h"

#include <stdbool.h>

bool pci_audio_state_add(GemuDevice *d, const char *prefix, PciAudio *a);
