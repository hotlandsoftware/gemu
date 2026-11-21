#pragma once
/*
 * CMD Technology PCI-649 IDE controller: the PCI function and its two IDE
 * channels (ide.h) at the legacy addresses.
 *
 * Config space: vendor 1095h, device 0649h, revision 2, class 01 01 8A
 * (mass storage, IDE, both channels in legacy mode with bus mastering
 * advertised).  BARs 0-4 are I/O BARs (task file, control, task file,
 * control, bus master) whose type and alignment bits are fixed, so a
 * sizing probe reads the size mask back; BAR5 is unimplemented.  Identity,
 * revision, class and header type are read-only.  CNTRL (51h) resets with
 * both channels enabled, which drivers check before creating them.
 *
 * I/O: the primary channel at 1F0h-1F7h and 3F6h, the secondary at
 * 170h-177h and 376h, always decoded (the BARs aren't used for decoding,
 * and the bus-master engine isn't modelled).  A channel wired as NULL isn't
 * decoded at all, leaving its ports to the machine.
 */
#include "storage/ide.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    IdeChannel *channel[2];     /* wiring: primary, secondary (NULL: none) */
    uint8_t     cfg[256];
} Cmd649;

void     cmd649_init(Cmd649 *c, IdeChannel *primary, IdeChannel *secondary);
void     cmd649_reset(Cmd649 *c);
uint32_t cmd649_cfg_read(const Cmd649 *c, unsigned reg, unsigned size);
void     cmd649_cfg_write(Cmd649 *c, unsigned reg, unsigned size, uint32_t val);

/* Which channel and register (ide.h numbering) a legacy port is, or false
 * if the controller doesn't decode it. */
bool     cmd649_decode(const Cmd649 *c, uint16_t port, unsigned *channel, unsigned *reg);
bool     cmd649_io_read(Cmd649 *c, uint16_t port, unsigned size, uint64_t *val);
bool     cmd649_io_write(Cmd649 *c, uint16_t port, unsigned size, uint64_t val);
