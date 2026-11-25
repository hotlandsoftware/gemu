/*
 * The core device model: device types, a machine's tree of device
 * instances, and the type registry.  (.work/modular-devices, 2.1)
 *
 * A type is static data: a name, the size of its state and its hooks.  An
 * instance is a node in the machine's tree with a path like "isa/adlib";
 * its state (`priv`, `type->size` bytes) is allocated zeroed.
 *
 * Life cycle:
 *   gemu_machine_new()           an empty tree (the root has path "")
 *   gemu_device_add() ...        build the tree; nothing runs yet
 *   gemu_machine_start()         init every device, parents before children,
 *                                in the order they were added; then a
 *                                power-on reset
 *   gemu_device_add() later      (hot-plug) inits and resets that device now
 *   gemu_machine_reset(kind)     reset hooks, pre-order, then
 *                                reset_after_children, post-order
 *   gemu_device_destroy()        children first (last added first), then the
 *                                device's own destroy hook
 *   gemu_machine_free()          destroys the whole tree
 *
 * Errors (a bad name, a duplicate, init failing) return NULL/false and leave
 * a message in gemu_machine_error().
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GemuMachine    GemuMachine;
typedef struct GemuDevice     GemuDevice;
typedef struct GemuDeviceType GemuDeviceType;

/* Power-on: everything to its initial state.  Hard: the reset line (RAM and
 * the like may survive).  Soft: a program-requested reset (a reset register,
 * a keyboard controller pulse).  Devices that don't distinguish ignore it. */
typedef enum {
    GEMU_RESET_POWER_ON,
    GEMU_RESET_HARD,
    GEMU_RESET_SOFT,
} GemuResetKind;

struct GemuDeviceType {
    const char *name;      /* "i8259": the data/device XML name */
    const char *desc;
    size_t      size;      /* bytes of instance state (priv), zeroed; may be 0 */
    /* All hooks optional. */
    bool (*init)(GemuDevice *d);       /* false = fail (set gemu_device_error) */
    void (*reset)(GemuDevice *d, GemuResetKind kind);
    void (*reset_after_children)(GemuDevice *d, GemuResetKind kind);
    void (*destroy)(GemuDevice *d);    /* free what init made; priv is freed after */
    void (*describe)(GemuDevice *d, char *buf, size_t len);   /* monitor "info" */
};

#define GEMU_DEVICE_PATH_MAX 64

struct GemuDevice {
    const GemuDeviceType *type;     /* NULL only for the root */
    GemuMachine *machine;
    GemuDevice  *parent;            /* NULL only for the root */
    GemuDevice  *first_child, *last_child, *next_sibling, *prev_sibling;
    char         path[GEMU_DEVICE_PATH_MAX];   /* "isa/adlib"; "" for the root */
    const char  *name;              /* the last path component (points into path) */
    uint64_t     clock_hz;          /* 0 = the parent's (gemu_device_clock) */
    char        *opts;              /* the option string given to add, or NULL */
    void        *priv;              /* type->size zeroed bytes, or NULL */
    bool         started;           /* init has run */
};

/* ── Machine ──────────────────────────────────────────────────────────────── */

GemuMachine *gemu_machine_new(const char *name);
void         gemu_machine_free(GemuMachine *m);
const char  *gemu_machine_name(const GemuMachine *m);
GemuDevice  *gemu_machine_root(GemuMachine *m);
bool         gemu_machine_start(GemuMachine *m);
bool         gemu_machine_started(const GemuMachine *m);
void         gemu_machine_reset(GemuMachine *m, GemuResetKind kind);
const char  *gemu_machine_error(const GemuMachine *m);   /* "" when none */

/* ── Devices ──────────────────────────────────────────────────────────────── */

/* Add a device of type t called `name` under parent (NULL = the root).  The
 * name is one path component: letters, digits, '_', '-' and '.'.  opts is
 * kept as given (parsed in stage C5); NULL for none. */
GemuDevice *gemu_device_add(GemuMachine *m, const GemuDeviceType *t,
                            const char *name, GemuDevice *parent, const char *opts);
/* The same, with the type looked up in the registry by name. */
GemuDevice *gemu_device_add_type(GemuMachine *m, const char *type_name,
                                 const char *name, GemuDevice *parent, const char *opts);

/* By full path ("isa/adlib"; "" is the root), or relative to a device:
 * "child/grandchild", ".." for the parent, "../sibling". */
GemuDevice *gemu_device_find(GemuMachine *m, const char *path);
GemuDevice *gemu_device_lookup(GemuDevice *from, const char *rel);

/* Reset d and everything under it. */
void gemu_device_reset(GemuDevice *d, GemuResetKind kind);
/* Remove d and everything under it.  Not the root. */
void gemu_device_destroy(GemuDevice *d);

/* d's clock, or the nearest ancestor's that has one (0 if none). */
uint64_t gemu_device_clock(const GemuDevice *d);
void     gemu_device_set_clock(GemuDevice *d, uint64_t hz);

/* The type's describe hook, or "<type>: <desc>". */
void gemu_device_describe(GemuDevice *d, char *buf, size_t len);

/* Pre-order walk of the tree under d (d included).  Return false from cb to
 * stop; the walk then returns false. */
bool gemu_device_walk(GemuDevice *d, bool (*cb)(GemuDevice *d, void *ud), void *ud);

/* For hooks: report why init failed ("pic: no such parent" style messages
 * get the device path prefixed). */
void gemu_device_error(GemuDevice *d, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* ── Type registry ────────────────────────────────────────────────────────── */
/* Generated by configure from the <type> elements in data/device/ XML: the
 * types whose source is linked into this binary (or test), sorted by name. */

const GemuDeviceType *gemu_device_type_find(const char *name);
size_t                gemu_device_type_count(void);
const GemuDeviceType *gemu_device_type_at(size_t i);
