/* GEMU's 86Box layer: what its files share.  See gemu_86box.h. */
#pragma once
#include "gemu_86box.h"
#include "gemu/dev/scheduler.h"
#include <stdint.h>

/* The device whose driver code is running (NULL outside), and the clock all
 * 86Box devices count in (tsc). */
extern Gemu86BoxDev *gemu_86box_cur;
extern uint64_t      gemu_86box_hz;

/* Around every call into a driver: makes b current and brings tsc up to
 * gemu_now().  Returns what was current, for leave. */
Gemu86BoxDev *gemu_86box_enter(Gemu86BoxDev *b);
void          gemu_86box_leave(Gemu86BoxDev *prev);
/* The current device, for a call that needs one (fatal if none). */
Gemu86BoxDev *gemu_86box_need(const char *what);

void gemu_86box_io_free(Gemu86BoxDev *b);       /* io.c: its handlers off */
void gemu_86box_timers_free(Gemu86BoxDev *b);   /* timer.c: its records */
