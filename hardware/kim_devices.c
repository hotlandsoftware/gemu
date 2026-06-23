#include "kim_devices.h"
#include <string.h>

static const KimDeviceDesc devices[] = {
    {
        .name = "kim-keypad",
        .desc = "KIM-1 hex keypad (23 keys)",
        .type = KIM_DEVICE_KEYBOARD,
    },
    {
        .name = "keypad",
        .desc = "Alias for kim-keypad",
        .type = KIM_DEVICE_KEYBOARD,
    },
};

const KimDeviceDesc *kim_device_list(int *count) {
    if (count) *count = (int)(sizeof(devices) / sizeof(devices[0]));
    return devices;
}

const KimDeviceDesc *kim_device_find(const char *name) {
    int n = 0;
    const KimDeviceDesc *devs = kim_device_list(&n);
    for (int i = 0; i < n; i++)
        if (strcmp(name, devs[i].name) == 0)
            return &devs[i];
    return NULL;
}
