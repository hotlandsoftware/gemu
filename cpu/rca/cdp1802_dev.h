#pragma once
/*
 * The CDP1802 as a core device (stage R1): it owns its Cdp1802 and
 * registers its state.  The machine sets the memory (cdp1802's mem and
 * mem_mask, or mem_read/mem_write) and the callbacks after adding it, and
 * names the one place DMA-out bytes go (gemu_cdp1802_set_dma_target: the
 * video chip's callback), so the DMA queue can be refilled after a load.
 * The EF inputs stay fields the machine sets (it does so every machine
 * cycle); interrupt and DMA requests stay cdp1802_request_irq/_dma_out,
 * the chip's own handshake.
 *
 * gemu_cdp1802_exec() makes it the machine's executor at hz machine cycles
 * a second.  A run goes a machine cycle at a time until its budget is
 * spent or a hook stops it:
 *   stop_before  at each instruction's fetch (a breakpoint at the PC):
 *                true stops the machine before it
 *   cycle        one machine cycle with the machine's extras (the EF
 *                inputs, the beam); NULL = cdp1802_step
 *   stop_after   after a cycle that ended at an instruction's fetch (the
 *                monitor paused): true stops the machine
 *   done         after each cycle: true ends the run (a frame finished)
 */
#include "cdp1802.h"
#include "gemu/dev/device.h"
#include "gemu/dev/scheduler.h"
#include <stdbool.h>
#include <stdint.h>

extern const GemuDeviceType gemu_cdp1802_type;

Cdp1802 *gemu_cdp1802(GemuDevice *d);
void     gemu_cdp1802_set_dma_target(GemuDevice *d, void (*cb)(uint8_t *byte, void *ud), void *ud);

typedef struct {
    bool (*stop_before)(void *ud);
    void (*cycle)(void *ud);
    bool (*stop_after)(void *ud);
    bool (*done)(void *ud);
    void *ud;
} GemuCdp1802Hooks;

typedef enum {
    GEMU_CDP1802_BUDGET,
    GEMU_CDP1802_STOPPED,
    GEMU_CDP1802_DONE,
} GemuCdp1802End;

GemuExecutor  *gemu_cdp1802_exec(GemuDevice *d, uint64_t hz, const GemuCdp1802Hooks *h);
GemuCdp1802End gemu_cdp1802_ended(GemuDevice *d);
