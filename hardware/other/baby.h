#pragma once
/*
 * Manchester Baby (SSEM, 1948) - skeleton.
 *
 * The machine state the emulation will grow into: the 32-word store, the
 * accumulator and the control (CI/PI) registers, plus the display and
 * monitor every GEMU machine has.  Nothing executes yet.
 */
#include "gemu/display.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    OTHER_MACHINE_BABY,
} OtherMachineType;

typedef struct {
    OtherMachineType machine;
    GemuDisplayType  display_type;
    int              display_scale;
    bool             no_shutdown;
} OtherConfig;

#define BABY_STORE_WORDS 32

typedef struct BabyState BabyState;

BabyState *other_baby_create(const OtherConfig *cfg);
void       other_baby_run(BabyState *s, const OtherConfig *cfg);
void       other_baby_destroy(BabyState *s);
