#pragma once
/* The 5150's 8255 PPI as a core device (type "i8255-pc"): see i8255.h for
 * how the motherboard wires it.
 *
 * Options: sw1, sw2 - the DIP switch blocks.  Lines: outputs pb0-pb7 (port
 * B: timer 2 gate, speaker data, ... keyboard clock and clear), inputs pc4
 * (cassette data) and pc5 (timer 2's output).  Ports: gemu_i8255_io, four
 * bytes, ud = the device.  The keyboard interface puts a scan code in with
 * gemu_i8255_set_kbd(). */
#include "controller/i8255.h"
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"

extern const GemuDeviceType gemu_i8255_type;
extern const GemuHandlers   gemu_i8255_io;

I8255 *gemu_i8255(GemuDevice *d);
void   gemu_i8255_set_kbd(GemuDevice *d, uint8_t scan);
