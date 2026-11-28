#pragma once
/*
 * The Manchester Baby's processor (SSEM) as a core device (stage O1): it
 * owns its Ssem (the store, the accumulator, CI and PI, the stop lamp,
 * the count) and registers its state.
 *
 * gemu_ssem_exec() makes it the machine's executor at hz instructions a
 * second (the Baby: about 1,100).  It runs while the panel's Stop/Run
 * switch is at Run (gemu_ssem_set_running, saved) and the stop lamp is
 * out; otherwise a run idles out its budget, as does the rest of one in
 * which STP is obeyed.  With a stop_before hook set (breakpoints), the
 * hook is asked before each instruction; true stops the machine there.
 */
#include "ssem.h"
#include "gemu/dev/device.h"
#include "gemu/dev/scheduler.h"
#include <stdbool.h>
#include <stdint.h>

extern const GemuDeviceType gemu_ssem_type;

Ssem *gemu_ssem(GemuDevice *d);

typedef enum {
    GEMU_SSEM_BUDGET,    /* ran (or idled) its budget */
    GEMU_SSEM_STOPPED,   /* stop_before */
} GemuSsemEnd;

GemuExecutor *gemu_ssem_exec(GemuDevice *d, uint64_t hz);
void          gemu_ssem_set_running(GemuDevice *d, bool run);
bool          gemu_ssem_running(GemuDevice *d);
void          gemu_ssem_set_stop_before(GemuDevice *d, bool (*fn)(void *ud), void *ud);
GemuSsemEnd   gemu_ssem_ended(GemuDevice *d);
