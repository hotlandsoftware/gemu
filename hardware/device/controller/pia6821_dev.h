#pragma once
/*
 * The 6821 PIA as a core device (stage M1): it owns its Pia6821 and
 * registers its state.  Lines: outputs "irqa", "irqb" (active high, as the
 * chip reports them; the board inverts or wire-ORs them onto a CPU's irq);
 * inputs "ca1", "cb1", "ca2", "cb2".  The machine wires the ports
 * (gemu_pia6821(d)->read_pa ... and ud) after adding it, and maps
 * gemu_pia6821_bus (four registers, A1-A0 = the offset's low bits; ud = the
 * device).  A reset is the chip's (registers cleared, IRQs low).
 */
#include "controller/pia6821.h"
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"

extern const GemuDeviceType gemu_pia6821_type;
extern const GemuHandlers   gemu_pia6821_bus;

Pia6821 *gemu_pia6821(GemuDevice *d);
