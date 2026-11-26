/* Shared between the files of core/src/dev. */
#pragma once
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/line.h"

/* line.c's part of the machine */
typedef struct {
    GemuLine   *outs;
    GemuLineIn *ins;
} GemuLines;

#include "gemu/dev/state.h"
#include "gemu/dev/option.h"

/* ── Per device (GemuDevice.core) ── */

typedef struct GemuStateItem {
    char      name[32];
    void     *ptr;                    /* memory items */
    size_t    elem, count;
    void    (*save)(void *ud, GemuStateWriter *w);       /* custom items */
    bool    (*check)(void *ud, GemuStateReader *r);
    bool    (*load)(void *ud, GemuStateReader *r);
    void     *ud;
    unsigned  load_mark;
    struct GemuStateItem *next;
} GemuStateItem;

typedef union {
    int64_t  i;
    bool     b;
    uint64_t size;
    int      choice;
    char    *str;
} GemuOptVal;

struct GemuDeviceCore {
    GemuStateItem *items, *items_tail;
    unsigned       nitems;
    char           state_error[128];  /* a bad gemu_state_add, reported at start */
    GemuOptVal    *opt;               /* one per type->options entry */
    bool          *given;
    unsigned       nopt;
    unsigned       ntimers, nexecs;   /* to number the device's timers and executors */
    unsigned       load_mark;         /* state.c: matched in the current check */
};

/* option.c: parse d->opts against d->type->options into d->core (false:
 * gemu_machine_error says why); free the values. */
bool gemu_options_parse(GemuDevice *d, const char *opts);
void gemu_options_free(GemuDevice *d);
/* state.c: free a device's state items. */
void gemu_state_free_items(GemuDevice *d);

/* ── Timers (scheduler.c; state.c saves them) ── */

struct GemuTimer {
    GemuMachine *m;
    GemuDevice  *owner;
    unsigned     index;              /* the owner's nth timer: its name in states */
    void       (*cb)(void *ud, int param);
    void        *ud;
    int          param;
    bool         on;
    GemuTime     deadline;
    uint64_t     seq;
    size_t       hidx;               /* in the heap, while on */
    /* periodic: in picoseconds, or in cycles of hz from anchor */
    GemuTime     period;
    uint64_t     hz, k, period_cycles;
    GemuTime     anchor;
    GemuTimer   *next, *prev;        /* all timers */
    unsigned     load_mark;
};

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
    bool          in_run;         /* inside gemu_machine_run_until */
    bool          paced;          /* gemu_machine_pace: anchored */
    uint64_t      pace_host_ns;   /* host time at the anchor */
    GemuTime      pace_machine;   /* machine time at the anchor */
} GemuSched;

struct GemuMachine {
    char       *name;
    GemuDevice  root;
    bool        inited;           /* gemu_machine_init has run every init */
    bool        started;          /* ... and gemu_machine_start the power-on reset */
    char        error[256];
    GemuSpace  *spaces;           /* space.c: newest first */
    GemuSched   sched;
    GemuLines   lines;
    struct GemuScreen *screens;   /* screen.c: in the order made */
    unsigned    load_epoch;       /* state.c: marks what a load has matched */
};

/* device.c: the machine's error message (gemu_machine_error). */
void gemu_machine_set_error(GemuMachine *m, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* space.c: free the machine's spaces (after its devices are gone). */
void gemu_space_free_all(GemuMachine *m);
/* scheduler.c: free the machine's timers and executors; a device's (on
 * destroy); and after a load, rebuild the heap from the timers' fields. */
void gemu_sched_free_all(GemuMachine *m);
void gemu_sched_remove_device(GemuDevice *d);
void gemu_sched_rebuild_heap(GemuMachine *m);
/* line.c: drop a device's lines (on destroy), and all of them. */
void gemu_lines_remove_device(GemuDevice *d);
void gemu_lines_free_all(GemuMachine *m);

/* ── State streams (state.c) ── */

struct GemuStateWriter {
    uint8_t *buf;
    size_t   len, cap;
    bool     failed;                  /* out of memory */
};

struct GemuStateReader {
    const uint8_t *p;
    size_t         left;
};

void gemu_sw_u8(GemuStateWriter *w, uint8_t v);
void gemu_sw_u64(GemuStateWriter *w, uint64_t v);
void gemu_sw_str(GemuStateWriter *w, const char *s);
bool gemu_sr_u8(GemuStateReader *r, uint8_t *v);
bool gemu_sr_u64(GemuStateReader *r, uint64_t *v);
bool gemu_sr_str(GemuStateReader *r, char *buf, size_t cap);   /* false if too long */

/* Each module's section: save, and load in two passes - check (apply =
 * false: match everything, change nothing, set gemu_machine_error on a
 * mismatch) then apply.  `epoch` marks what the check matched. */
void gemu_sched_state_save(GemuMachine *m, GemuStateWriter *w);
bool gemu_sched_state_load(GemuMachine *m, GemuStateReader *r, bool apply, unsigned epoch);
void gemu_lines_state_save(GemuMachine *m, GemuStateWriter *w);
bool gemu_lines_state_load(GemuMachine *m, GemuStateReader *r, bool apply, unsigned epoch);
/* screen.c: drop a device's screens (on destroy), and all of them. */
void gemu_screens_remove_device(GemuDevice *d);
void gemu_screens_free_all(GemuMachine *m);
