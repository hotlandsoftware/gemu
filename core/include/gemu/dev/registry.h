/* The shape of the device type registry.  configure generates the table
 * (generated/device_types.c, and one per device test that links the device
 * model) from the <type> elements in data/device XML; only device.c reads
 * it - everything else uses gemu_device_type_find(). */
#pragma once
#include "gemu/dev/device.h"

typedef struct {
    const char           *name;   /* the data/device XML name */
    const GemuDeviceType *type;
} GemuDeviceTypeEntry;

/* Sorted by name (strcmp), then a { NULL, NULL } entry. */
extern const GemuDeviceTypeEntry gemu_device_type_table[];
