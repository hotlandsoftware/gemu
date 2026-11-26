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
/* sound.c: on entry, sound_pos_global/music_pos_global for the device (or
 * the one that added it) with handlers; before a save, generate up to now;
 * after a load, start again from now; free them. */
void gemu_86box_sound_enter(Gemu86BoxDev *b);
void gemu_86box_sound_pre_save(Gemu86BoxDev *b);
void gemu_86box_sound_post_load(Gemu86BoxDev *b);
void gemu_86box_sound_free(Gemu86BoxDev *b);
void gemu_86box_timers_free(Gemu86BoxDev *b);   /* timer.c: its records */
/* mem.c: the core mappings again from the structures (after a load); off,
 * with the ROM buffers (after the driver's close) */
void gemu_86box_mem_post_load(Gemu86BoxDev *b);
void gemu_86box_mem_free(Gemu86BoxDev *b);
