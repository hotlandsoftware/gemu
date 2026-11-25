/* Shared between the files of core/src/dev. */
#pragma once
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"
#include "gemu/dev/scheduler.h"

/* scheduler.c's part of the machine */
typedef struct {
    GemuTime      now;            /* the machine time */
    GemuTime      cur;            /* gemu_now() outside a run: now, or a timer's deadline */
    GemuTimer   **heap;           /* running timers, by (deadline, seq) */
    size_t        nheap, cap;
    uint64_t      seq;
    GemuTimer    *timers;         /* all of them */
    GemuExecutor *execs, *execs_tail;
    unsigned      nexecs;
    GemuExecutor *running;        /* the executor inside its run, or NULL */
    GemuTime      slice_end;
    GemuTime      quantum;
    bool          stop;
} GemuSched;

struct GemuMachine {
    char       *name;
    GemuDevice  root;
    bool        started;
    char        error[256];
    GemuSpace  *spaces;           /* space.c: newest first */
    GemuSched   sched;
};

/* space.c: free the machine's spaces (after its devices are gone). */
void gemu_space_free_all(GemuMachine *m);
/* scheduler.c: free the machine's timers and executors. */
void gemu_sched_free_all(GemuMachine *m);
