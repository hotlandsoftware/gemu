#pragma once

/*
 * KIM-1 attachable devices.
 *
 * Each device has a unique name, a short description, and a device type.
 * Use -device ? to list, -device NAME to attach.
 */

typedef enum {
    KIM_DEVICE_NONE = 0,
    KIM_DEVICE_KEYBOARD,
} KimDeviceType;

typedef struct {
    const char   *name;
    const char   *desc;
    KimDeviceType type;
} KimDeviceDesc;

/* Returns the static device registry and its count. */
const KimDeviceDesc *kim_device_list(int *count);

/* Find a device by name (case-sensitive). Returns NULL if not found. */
const KimDeviceDesc *kim_device_find(const char *name);
