/*
 * Save state.  (.work/modular-devices, 2.7; question 7: required for every
 * device)
 *
 * Every device type registers its complete state in its init hook - items
 * of memory (gemu_state_add, like MAME's save_item) or custom save/load
 * functions for data whose size varies - or is marked `stateless`.  A
 * device that does neither stops the machine from starting.
 *
 * The core saves its own objects too: the machine time, every timer, every
 * executor's place in time and every line output's level (inputs are worked
 * out from them).  Mappings in spaces are not saved - they hold host
 * pointers; a device whose mappings depend on its registers rebuilds them
 * in post_load (bank switching, moved BARs), as it does other derived state.
 *
 * Save: pre_save hooks (pre-order), then everything.  Load: the whole state
 * is checked against the machine first - every device, item, timer,
 * executor and line present on both sides, with the same sizes - and the
 * machine is left untouched if anything doesn't match; then it is applied
 * and the post_load hooks run (pre-order); pacing (gemu_machine_pace) starts
 * over from the loaded time.  The machine must be started and
 * not inside gemu_machine_run_until().
 *
 * Items of 2, 4 and 8 bytes are stored little-endian (the hosts GEMU builds
 * for are), others byte for byte.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gemu/dev/device.h"

typedef struct GemuStateWriter GemuStateWriter;
typedef struct GemuStateReader GemuStateReader;

/* count elements of elem bytes at ptr.  Names are unique per device;
 * false (and the machine won't start) on a duplicate or a bad argument. */
bool gemu_state_add(GemuDevice *d, const char *name, void *ptr, size_t elem, size_t count);
#define GEMU_STATE_VAR(d, name, var)   gemu_state_add((d), (name), &(var), sizeof (var), 1)
/* Names are at most 63 characters.  prefix + name, for the parts of a
 * device (a chip pair's "master."): */
bool gemu_state_add_named(GemuDevice *d, const char *prefix, const char *name, void *ptr,
                          size_t elem, size_t count);
/* A member of a struct, under its own name: a scalar, an array (one item,
 * the whole array) or a nested plain struct. */
#define GEMU_STATE_MEMBER(d, prefix, obj, member) \
    gemu_state_add_named((d), (prefix), #member, &(obj)->member, sizeof (obj)->member, 1)
#define GEMU_STATE_ARRAY(d, name, arr) \
    gemu_state_add((d), (name), (arr), sizeof (arr)[0], sizeof (arr) / sizeof (arr)[0])

/* An array of count structs, stride bytes apart from base, saved as the
 * listed fields of each, packed (no padding, no pointers).  The field sizes
 * are saved too: a struct whose fields change is refused, not misread. */
typedef struct {
    size_t offset, size;
} GemuStateField;
#define GEMU_STATE_FIELD(type, member) \
    { offsetof(type, member), sizeof(((type *)0)->member) }
bool gemu_state_add_table(GemuDevice *d, const char *name, void *base, size_t stride,
                          size_t count, const GemuStateField *fields, size_t nfields);
#define GEMU_STATE_TABLE(d, name, arr, fields) \
    gemu_state_add_table((d), (name), (arr), sizeof (arr)[0], sizeof (arr) / sizeof (arr)[0], \
                         (fields), sizeof (fields) / sizeof (fields)[0])

/* Custom: save writes what it likes; load reads it back and returns false
 * if it can't use it (checked before anything is applied: see `check`).
 * check (may be NULL) gets the same bytes during the checking pass. */
bool gemu_state_add_custom(GemuDevice *d, const char *name,
                           void (*save)(void *ud, GemuStateWriter *w),
                           bool (*check)(void *ud, GemuStateReader *r),
                           bool (*load)(void *ud, GemuStateReader *r), void *ud);
void   gemu_state_write(GemuStateWriter *w, const void *p, size_t n);
void   gemu_state_write_u32(GemuStateWriter *w, uint32_t v);
bool   gemu_state_read(GemuStateReader *r, void *p, size_t n);
bool   gemu_state_read_u32(GemuStateReader *r, uint32_t *v);
size_t gemu_state_left(const GemuStateReader *r);
/* From a check: why it says no (the load's error message names it). */
void   gemu_state_refuse(GemuStateReader *r, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* The whole machine to/from memory (*buf is malloc'd) or a file. */
bool gemu_machine_save_state(GemuMachine *m, uint8_t **buf, size_t *len);
bool gemu_machine_load_state(GemuMachine *m, const uint8_t *buf, size_t len);
bool gemu_machine_save_file(GemuMachine *m, const char *path);
bool gemu_machine_load_file(GemuMachine *m, const char *path);
