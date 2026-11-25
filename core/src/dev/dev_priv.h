/* Shared between the files of core/src/dev. */
#pragma once
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"

struct GemuMachine {
    char       *name;
    GemuDevice  root;
    bool        started;
    char        error[256];
    GemuSpace  *spaces;           /* space.c: newest first */
};

/* space.c: free the machine's spaces (after its devices are gone). */
void gemu_space_free_all(GemuMachine *m);
