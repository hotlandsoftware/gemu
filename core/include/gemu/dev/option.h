/*
 * Device options: one schema type for -device name,key=val, the Machine
 * Manager's settings pages and the ported devices' configuration.
 * (.work/modular-devices, 2.7)
 *
 * A type lists its options (GemuDeviceType.options, ended by an entry with
 * a NULL name).  gemu_device_add() parses the option string against it:
 * "key=val,key=val"; ",," is a literal comma inside a value; a bare "key"
 * sets a bool option.  Options not given take their default.  Unknown keys,
 * bad values, numbers out of range and unknown choices make the add fail
 * with a message in gemu_machine_error().
 *
 * Values: int - decimal, 0x hex, 0 octal, optionally negative; bool -
 * on/off, yes/no, true/false, 1/0; size - an int with an optional K, M or G
 * (powers of 1024); choice - one of the listed words; string and file -
 * any text (file is a path, for the front end's benefit).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct GemuDevice GemuDevice;

typedef enum {
    GEMU_OPT_INT,
    GEMU_OPT_BOOL,
    GEMU_OPT_CHOICE,
    GEMU_OPT_STRING,
    GEMU_OPT_FILE,
    GEMU_OPT_SIZE,
} GemuOptKind;

typedef struct GemuOptionDef {
    const char         *name;
    const char         *desc;
    GemuOptKind         kind;
    const char         *def;        /* the default, as text; NULL = 0 / off / "" / first choice */
    int64_t             min, max;   /* int and size: inclusive; both 0 = no limit */
    const char *const  *choices;    /* choice: NULL-terminated */
} GemuOptionDef;

/* The device's value of an option (its type must have it, of that kind:
 * asking for one it doesn't have is a bug, and aborts). */
int64_t     gemu_opt_int(const GemuDevice *d, const char *name);
bool        gemu_opt_bool(const GemuDevice *d, const char *name);
uint64_t    gemu_opt_size(const GemuDevice *d, const char *name);
const char *gemu_opt_str(const GemuDevice *d, const char *name);   /* string, file, choice */
int         gemu_opt_choice(const GemuDevice *d, const char *name); /* index into choices */
/* Whether the option string gave it (rather than the default). */
bool        gemu_opt_given(const GemuDevice *d, const char *name);

/* A -device argument, "type,key=val,...": the type name and the rest.
 * Returns false if there's no type name. */
bool gemu_device_spec_split(const char *spec, char *type, unsigned type_len, const char **opts);
